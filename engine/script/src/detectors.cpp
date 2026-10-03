#include "engine/script/detectors.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <string_view>

#include "class_descriptors.gen.h"
#include "engine/core/dmath.h"
#include "engine/core/math.h"
#include "engine/input/input.h"
#include "engine/physics/physics.h"
#include "engine/scene/physics_sync.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"
#include "engine/script/instance_binding.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

using core::DVec3;
using core::f32;
using core::f64;
using core::i32;
using core::InstanceId;
using core::u64;
using core::Vec2;
using core::Vec3;
using scene::DetectorMessage;
using scene::World;

// How far the pointer reaches into the world. Past any detector's reach, so
// the nearest thing hit is what the pointer is on.
constexpr f32 PointerReach = 1000.0f;
// What the authority forgives a client about where its character is: a tick
// of motion, and a part's hit point on its surface rather than at its centre.
constexpr f64 AuthoritySlack = 4.0;

constexpr i32 MouseButton1 = static_cast<i32>(input::UserInputType::MouseButton1);
constexpr i32 MouseButton2 = static_cast<i32>(input::UserInputType::MouseButton2);

struct Classes
{
    scene::ClassId clickDetector = 0;
    scene::ClassId basePart = 0;
    scene::ClassId attachment = 0;
    scene::ClassId model = 0;
};

[[nodiscard]] Classes classesOf(World& w)
{
    const auto id = [&w](std::string_view name) { return w.classes().findId(w.atoms().intern(name)); };
    return Classes{id("ClickDetector"), id("BasePart"), id("Attachment"), id("Model")};
}

struct Ray
{
    DVec3 origin;
    Vec3 direction;
};

// The camera the world is seen through, and what the pointer is on a function
// of the input and the world, and the same in a replay. **With a window, the
// camera as the last frame drew it is part of the input** (ADR 0134): sampled
// once a frame with the pointer, it is the picture the player pointed at --
// the tick's camera, a following one, is up to a tick ahead of that picture.
// Without a window there is no drawn camera -- a replay, a gate, a server --
// and it is the tick's, as it always was, so a replay stays exact.
struct View
{
    core::CFrameD cframe;
    f32 fieldOfView = 70.0f;
    bool orthographic = false;
    f32 orthographicSize = 10.0f;
    Vec2 viewport{};

    [[nodiscard]] bool valid() const noexcept { return viewport.x > 0.0f && viewport.y > 0.0f; }
    [[nodiscard]] f32 aspect() const noexcept { return viewport.x / viewport.y; }

    [[nodiscard]] Ray rayThrough(Vec2 pixel) const noexcept
    {
        const f32 x = 2.0f * pixel.x / viewport.x - 1.0f;
        const f32 y = 1.0f - 2.0f * pixel.y / viewport.y;
        if (orthographic) {
            const Vec3 offset{x * orthographicSize * aspect(), y * orthographicSize, 0.0f};
            return Ray{cframe.position + core::toDVec3(cframe.rotation * offset),
                       cframe.rotation * Vec3{0.0f, 0.0f, -1.0f}};
        }
        const f32 half = std::tan(fieldOfView * std::numbers::pi_v<f32> / 360.0f);
        return Ray{cframe.position, core::normalize(cframe.rotation * Vec3{x * half * aspect(), y * half, -1.0f})};
    }
};

[[nodiscard]] View viewOf(const World& w)
{
    View view;
    view.viewport = w.engineState().viewportSize;
    const scene::CameraComponent* camera = nullptr;
    w.workspaces().forEach([&](InstanceId, const scene::WorkspaceComponent& workspace) {
        if (camera == nullptr)
            camera = w.cameras().find(workspace.currentCamera);
    });
    if (camera != nullptr) {
        view.cframe = w.engineState().drawnCameraValid ? w.engineState().drawnCamera : camera->cframe;
        view.fieldOfView = camera->fieldOfView;
        view.orthographic = camera->projection == 1;
        view.orthographicSize = camera->orthographicSize;
    }
    return view;
}

// Where a detector is: its part's centre, its attachment, or its model's
// primary part -- the first part in it when it names none.
[[nodiscard]] std::optional<DVec3> anchorOf(const World& w, const Classes& classes, InstanceId detector,
                                            InstanceId* from = nullptr)
{
    const InstanceId parent = w.parentOf(detector);
    if (!parent.valid())
        return std::nullopt;
    const auto hung = [from](InstanceId id, const DVec3& at) {
        if (from != nullptr)
            *from = id;
        return std::optional<DVec3>(at);
    };
    if (const scene::PartComponent* part = w.parts().find(parent))
        return hung(parent, part->cframe.position);
    if (const scene::AttachmentComponent* attachment = w.attachments().find(parent))
        return hung(parent, attachment->worldCFrame.position);
    if (w.isA(parent, classes.model)) {
        if (const scene::ModelComponent* model = w.models().find(parent); model != nullptr) {
            if (const scene::PartComponent* primary = w.parts().find(model->primaryPart))
                return hung(model->primaryPart, primary->cframe.position);
        }
        std::vector<InstanceId> inside;
        w.collectDescendants(parent, inside);
        for (const InstanceId id : inside) {
            if (const scene::PartComponent* part = w.parts().find(id))
                return hung(id, part->cframe.position);
        }
    }
    return std::nullopt;
}

[[nodiscard]] InstanceId characterOf(const World& w, InstanceId player) noexcept
{
    const scene::PlayerComponent* component = w.players().find(player);
    return component != nullptr ? component->character : InstanceId{};
}

// Where distance is measured from: the local character, or the camera.
[[nodiscard]] DVec3 reachOrigin(const World& w, InstanceId player, const View& view)
{
    if (const scene::PlayerComponent* component = w.players().find(player); component != nullptr) {
        if (const scene::PartComponent* body = w.parts().find(component->character))
            return body->cframe.position;
    }
    return view.cframe.position;
}

[[nodiscard]] f64 distance(DVec3 a, DVec3 b) noexcept
{
    const DVec3 d = a - b;
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

// The part a ray hits first, and where.
struct Hit
{
    InstanceId instance;
    DVec3 position;
    f32 distance = 0.0f;
};

// Through the player's own character, which is not what they point at or
// what stands between them and a prompt -- the ray starts inside it.
[[nodiscard]] std::optional<Hit> cast(const ServiceState& services, const Ray& ray, f32 reach,
                                      InstanceId character = {})
{
    scene::PhysicsSync* sync = services.physics;
    if (sync == nullptr)
        return std::nullopt;
    physics::RayHit hit;
    const physics::RayD query{ray.origin, ray.direction * reach};
    const u64 own = character.valid() ? sync->userDataOf(character) : 0u;
    physics::QueryFilter filter;
    if (own != 0)
        filter.userData = std::span<const u64>(&own, 1);
    sync->syncForQuery();
    if (!sync->backend().raycast(sync->worldHandle(), query, filter, hit))
        return std::nullopt;
    return Hit{sync->instanceOf(hit.userData), hit.position, hit.distance};
}

// The detector of a class in the part hit or in a model around it, nearest
// first.
[[nodiscard]] InstanceId detectorAbove(const World& w, const Classes& classes, InstanceId hit)
{
    for (InstanceId at = hit; at.valid(); at = w.parentOf(at)) {
        if (at != hit && !w.isA(at, classes.model))
            break;
        if (const InstanceId found = w.findFirstChildOfClass(at, classes.clickDetector); found.valid())
            return found;
    }
    return {};
}

// --- Firing ---------------------------------------------------------------------

void fireWith(lua_State* L, InstanceId owner, std::string_view event, InstanceId argument)
{
    World& w = *context(L).world;
    const scene::EventDesc* descriptor = w.classes().findEvent(w.classOf(owner), w.atoms().intern(event));
    if (descriptor == nullptr)
        return;
    pushInstance(L, argument);
    fireInstanceEvent(L, owner, descriptor->slot, lua_gettop(L), 1);
    lua_pop(L, 1);
}

// A click, with the player who made it.
void fireKind(lua_State* L, InstanceId detector, InstanceId player, DetectorMessage::Kind kind)
{
    fireWith(L, detector, kind == DetectorMessage::Kind::Click ? "MouseClick" : "RightMouseClick", player);
}

// What this machine's player did: fired here at once, and in a match sent to
// the authority, which fires it there with the player it knows.
void act(lua_State* L, InstanceId detector, InstanceId player, DetectorMessage::Kind kind)
{
    fireKind(L, detector, player, kind);
    scene::EngineState& state = context(L).world->engineState();
    if (state.networkTopology == scene::NetworkTopology::Replica)
        state.detectorOutbox.push_back(DetectorMessage{detector, {}, kind, 0});
}

// --- The authority --------------------------------------------------------------

// A click a client sent, fired only if its player could have made it: the
// detector is live, a `ClickDetector`, and within reach of the player's
// character.
void receive(lua_State* L, const Classes& classes)
{
    World& w = *context(L).world;
    std::vector<DetectorMessage> inbox;
    inbox.swap(w.engineState().detectorInbox);
    for (const DetectorMessage& message : inbox) {
        if (!w.alive(message.detector) || w.destroyed(message.detector) || !w.alive(message.player))
            continue;
        const scene::PlayerComponent* player = w.players().find(message.player);
        const scene::PartComponent* body = player != nullptr ? w.parts().find(player->character) : nullptr;
        const scene::ClickDetectorComponent* detector = w.clickDetectors().find(message.detector);
        if (body == nullptr || detector == nullptr)
            continue;
        // A part's size counts: a click lands on its surface, not its centre.
        f64 extent = 0.0;
        if (const scene::PartComponent* part = w.parts().find(w.parentOf(message.detector))) {
            const Vec3 half = part->size * 0.5f;
            extent = static_cast<f64>(core::length(half));
        }
        const std::optional<DVec3> anchor = anchorOf(w, classes, message.detector);
        if (!anchor.has_value() ||
            distance(*anchor, body->cframe.position) > detector->maxActivationDistance + extent + AuthoritySlack)
            continue;
        fireKind(L, message.detector, message.player, message.kind);
    }
}

// --- This machine ---------------------------------------------------------------

void stepClicks(lua_State* L, const Classes& classes, InstanceId player, const View& view, DVec3 origin,
                std::span<const input::RawInputEvent> events)
{
    World& w = *context(L).world;
    ServiceState& services = *context(L).services;
    DetectorState& state = services.detectors;

    // What is under a pixel, and within reach.
    const auto under = [&](Vec2 pixel) -> InstanceId {
        if (!view.valid())
            return {};
        const std::optional<Hit> hit = cast(services, view.rayThrough(pixel), PointerReach, characterOf(w, player));
        if (!hit.has_value() || !hit->instance.valid())
            return {};
        const InstanceId detector = detectorAbove(w, classes, hit->instance);
        const scene::ClickDetectorComponent* component = w.clickDetectors().find(detector);
        if (component == nullptr || distance(hit->position, origin) > component->maxActivationDistance)
            return {};
        return detector;
    };

    // Hovering is the mouse's: a finger is not over anything between taps.
    InstanceId hovered;
    if (w.engineState().lastInputDeviceType != 2 && !w.engineState().pointerLocked)
        hovered = under(w.engineState().pointerPosition);
    if (hovered != state.hovered) {
        if (state.hovered.valid() && w.alive(state.hovered))
            fireWith(L, state.hovered, "MouseHoverLeave", player);
        if (hovered.valid())
            fireWith(L, hovered, "MouseHoverEnter", player);
        state.hovered = hovered;
    }

    for (const input::RawInputEvent& event : events) {
        if (event.phase != input::RawInputEvent::Phase::Began || event.uiConsumed)
            continue;
        const auto type = static_cast<i32>(event.userInputType);
        if (type == MouseButton1 || type == MouseButton2) {
            if (!state.hovered.valid())
                continue;
            act(L, state.hovered, player,
                type == MouseButton1 ? DetectorMessage::Kind::Click : DetectorMessage::Kind::RightClick);
        }
        else if (event.userInputType == input::UserInputType::Touch) {
            if (const InstanceId detector = under(Vec2{event.position.x, event.position.y}); detector.valid())
                act(L, detector, player, DetectorMessage::Kind::Click);
        }
    }
}

} // namespace

void stepDetectors(lua_State* L, core::f64 dt, std::span<const input::RawInputEvent> events)
{
    (void)dt;
    World& w = *context(L).world;
    const Classes classes = classesOf(w);
    ServiceState& services = *context(L).services;

    if (w.engineState().networkTopology != scene::NetworkTopology::Replica)
        receive(L, classes);

    // No player here -- a dedicated server -- is nobody pointing at anything.
    const InstanceId player = scene::localPlayerOf(w);
    if (!player.valid()) {
        services.detectors = DetectorState{};
        return;
    }
    const View view = viewOf(w);
    stepClicks(L, classes, player, view, reachOrigin(w, player, view), events);
}

} // namespace engine::script
