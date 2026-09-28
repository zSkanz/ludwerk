#include "engine/script/detectors.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <string_view>

#include "class_descriptors.gen.h"
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
    scene::ClassId prompt = 0;
    scene::ClassId basePart = 0;
    scene::ClassId attachment = 0;
    scene::ClassId model = 0;
    scene::ClassId promptService = 0;
};

[[nodiscard]] Classes classesOf(World& w)
{
    const auto id = [&w](std::string_view name) { return w.classes().findId(w.atoms().intern(name)); };
    return Classes{id("ClickDetector"), id("ProximityPrompt"), id("BasePart"),
                   id("Attachment"),    id("Model"),           id("ProximityPromptService")};
}

struct Ray
{
    DVec3 origin;
    Vec3 direction;
};

// The camera the world is seen through. Tick data only -- its frame and the
// viewport the frame last drew -- so what the pointer is on is a function of
// the input and the world, and the same in a replay.
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

    // Where a point in the world is on the screen, or nothing behind the camera.
    [[nodiscard]] std::optional<Vec2> project(DVec3 point) const noexcept
    {
        const Vec3 relative = core::toVec3(point - cframe.position);
        const Mat3Columns axes = columns();
        const f32 forward = -core::dot(relative, axes.back);
        const f32 right = core::dot(relative, axes.right);
        const f32 up = core::dot(relative, axes.up);
        f32 x = 0.0f;
        f32 y = 0.0f;
        if (orthographic) {
            x = right / (orthographicSize * aspect());
            y = up / orthographicSize;
        }
        else {
            if (forward <= 0.01f)
                return std::nullopt;
            const f32 half = std::tan(fieldOfView * std::numbers::pi_v<f32> / 360.0f);
            x = right / (forward * half * aspect());
            y = up / (forward * half);
        }
        return Vec2{(x + 1.0f) * 0.5f * viewport.x, (1.0f - y) * 0.5f * viewport.y};
    }

private:
    struct Mat3Columns
    {
        Vec3 right;
        Vec3 up;
        Vec3 back;
    };
    [[nodiscard]] Mat3Columns columns() const noexcept
    {
        const auto& m = cframe.rotation.m;
        return Mat3Columns{Vec3{m[0][0], m[1][0], m[2][0]}, Vec3{m[0][1], m[1][1], m[2][1]},
                           Vec3{m[0][2], m[1][2], m[2][2]}};
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
        view.cframe = camera->cframe;
        view.fieldOfView = camera->fieldOfView;
        view.orthographic = camera->projection == 1;
        view.orthographicSize = camera->orthographicSize;
    }
    return view;
}

// Where a prompt hangs: its part's centre, its attachment, or its model's
// primary part -- the first part in it when it names none.
[[nodiscard]] std::optional<DVec3> anchorOf(const World& w, const Classes& classes, InstanceId detector)
{
    const InstanceId parent = w.parentOf(detector);
    if (!parent.valid())
        return std::nullopt;
    if (const scene::PartComponent* part = w.parts().find(parent))
        return part->cframe.position;
    if (const scene::AttachmentComponent* attachment = w.attachments().find(parent))
        return attachment->worldCFrame.position;
    if (w.isA(parent, classes.model)) {
        if (const scene::ModelComponent* model = w.models().find(parent); model != nullptr) {
            if (const scene::PartComponent* primary = w.parts().find(model->primaryPart))
                return primary->cframe.position;
        }
        std::vector<InstanceId> inside;
        w.collectDescendants(parent, inside);
        for (const InstanceId id : inside) {
            if (const scene::PartComponent* part = w.parts().find(id))
                return part->cframe.position;
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
    const scene::PhysicsSync* sync = services.physics;
    if (sync == nullptr)
        return std::nullopt;
    physics::RayHit hit;
    const physics::RayD query{ray.origin, ray.direction * reach};
    const u64 own = character.valid() ? sync->userDataOf(character) : 0u;
    physics::QueryFilter filter;
    if (own != 0)
        filter.userData = std::span<const u64>(&own, 1);
    if (!sync->backend().raycast(sync->worldHandle(), query, filter, hit))
        return std::nullopt;
    return Hit{sync->instanceOf(hit.userData), hit.position, hit.distance};
}

// The `ClickDetector` in the part hit or in a model around it, nearest first.
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

void fireShown(lua_State* L, InstanceId prompt, i32 inputType)
{
    World& w = *context(L).world;
    const scene::EventDesc* descriptor = w.classes().findEvent(w.classOf(prompt), w.atoms().intern("PromptShown"));
    if (descriptor == nullptr)
        return;
    pushEnumItem(L, scene::EnumValue{scene::generated::ProximityPromptInputTypeEnumId, inputType});
    fireInstanceEvent(L, prompt, descriptor->slot, lua_gettop(L), 1);
    lua_pop(L, 1);
}

void fireHidden(lua_State* L, InstanceId prompt)
{
    World& w = *context(L).world;
    const scene::EventDesc* descriptor = w.classes().findEvent(w.classOf(prompt), w.atoms().intern("PromptHidden"));
    if (descriptor != nullptr)
        fireInstanceEvent(L, prompt, descriptor->slot, lua_gettop(L) + 1, 0);
}

// One thing a detector does, with the player who did it: its own signal, and
// for a trigger the service's too.
void fireKind(lua_State* L, const Classes& classes, InstanceId detector, InstanceId player, DetectorMessage::Kind kind)
{
    World& w = *context(L).world;
    switch (kind) {
    case DetectorMessage::Kind::Click:
        fireWith(L, detector, "MouseClick", player);
        break;
    case DetectorMessage::Kind::RightClick:
        fireWith(L, detector, "RightMouseClick", player);
        break;
    case DetectorMessage::Kind::Triggered: {
        fireWith(L, detector, "Triggered", player);
        const InstanceId service = w.findFirstChildOfClass(context(L).services->dataModel, classes.promptService);
        const scene::EventDesc* descriptor =
            service.valid() ? w.classes().findEvent(classes.promptService, w.atoms().intern("PromptTriggered"))
                            : nullptr;
        if (descriptor != nullptr) {
            pushInstance(L, detector);
            pushInstance(L, player);
            fireInstanceEvent(L, service, descriptor->slot, lua_gettop(L) - 1, 2);
            lua_pop(L, 2);
        }
        break;
    }
    case DetectorMessage::Kind::TriggerEnded:
        fireWith(L, detector, "TriggerEnded", player);
        break;
    case DetectorMessage::Kind::HoldBegan:
        fireWith(L, detector, "PromptButtonHoldBegan", player);
        break;
    case DetectorMessage::Kind::HoldEnded:
        fireWith(L, detector, "PromptButtonHoldEnded", player);
        break;
    }
}

// What this machine's player did: fired here at once, and in a match sent to
// the authority, which fires it there with the player it knows.
void act(lua_State* L, const Classes& classes, InstanceId detector, InstanceId player, DetectorMessage::Kind kind)
{
    fireKind(L, classes, detector, player, kind);
    scene::EngineState& state = context(L).world->engineState();
    if (state.networkTopology == scene::NetworkTopology::Replica)
        state.detectorOutbox.push_back(DetectorMessage{detector, {}, kind, 0});
}

// --- The authority --------------------------------------------------------------

// A click or a trigger a client sent, fired only if its player could have
// done it: the detector is live and of the right kind, enabled, and within
// reach of the player's character.
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
        if (body == nullptr)
            continue;
        const bool click =
            message.kind == DetectorMessage::Kind::Click || message.kind == DetectorMessage::Kind::RightClick;
        f64 reach = 0.0;
        if (click) {
            const scene::ClickDetectorComponent* detector = w.clickDetectors().find(message.detector);
            if (detector == nullptr)
                continue;
            reach = detector->maxActivationDistance;
        }
        else {
            const scene::ProximityPromptComponent* prompt = w.proximityPrompts().find(message.detector);
            if (prompt == nullptr || !prompt->enabled || !w.engineState().promptsEnabled)
                continue;
            reach = prompt->maxActivationDistance;
        }
        // A part's size counts: a click lands on its surface, not its centre.
        f64 extent = 0.0;
        if (const scene::PartComponent* part = w.parts().find(w.parentOf(message.detector))) {
            const Vec3 half = part->size * 0.5f;
            extent = static_cast<f64>(core::length(half));
        }
        const std::optional<DVec3> anchor = anchorOf(w, classes, message.detector);
        if (!anchor.has_value() || distance(*anchor, body->cframe.position) > reach + extent + AuthoritySlack)
            continue;
        fireKind(L, classes, message.detector, message.player, message.kind);
    }
}

// --- This machine ---------------------------------------------------------------

void stepClicks(lua_State* L, const Classes& classes, InstanceId player, const View& view, DVec3 origin,
                std::span<const input::RawInputEvent> events, std::span<const Vec2> promptTaps)
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
            act(L, classes, state.hovered, player,
                type == MouseButton1 ? DetectorMessage::Kind::Click : DetectorMessage::Kind::RightClick);
        }
        else if (event.userInputType == input::UserInputType::Touch) {
            const Vec2 at{event.position.x, event.position.y};
            // A tap on a prompt is the prompt's.
            const bool onPrompt = std::any_of(promptTaps.begin(), promptTaps.end(), [&](Vec2 centre) {
                return std::abs(at.x - centre.x) <= scene::PromptWidth * 0.5f &&
                       std::abs(at.y - centre.y) <= scene::PromptHeight * 0.5f;
            });
            if (onPrompt)
                continue;
            if (const InstanceId detector = under(at); detector.valid())
                act(L, classes, detector, player, DetectorMessage::Kind::Click);
        }
    }
}

// Which prompts show, nearest first, by the rules of `Exclusivity`, the
// service's cap, reach and line of sight.
[[nodiscard]] std::vector<scene::ShownPrompt> choosePrompts(lua_State* L, const Classes& classes, DVec3 origin,
                                                            InstanceId character, i32 inputType)
{
    World& w = *context(L).world;
    const ServiceState& services = *context(L).services;
    scene::EngineState& state = w.engineState();
    std::vector<scene::ShownPrompt> shown;
    if (!state.promptsEnabled)
        return shown;

    struct Candidate
    {
        InstanceId prompt;
        DVec3 anchor;
        f64 distance = 0.0;
    };
    std::vector<Candidate> candidates;
    w.proximityPrompts().forEach([&](InstanceId id, const scene::ProximityPromptComponent& prompt) {
        if (!prompt.enabled || w.destroyed(id) || !w.isAncestorOf(services.dataModel, id))
            return;
        const std::optional<DVec3> anchor = anchorOf(w, classes, id);
        if (!anchor.has_value())
            return;
        const f64 away = distance(*anchor, origin);
        if (away > prompt.maxActivationDistance)
            return;
        if (prompt.requiresLineOfSight) {
            const Vec3 toward = core::toVec3(*anchor - origin);
            const f32 length = core::length(toward);
            if (length > 0.05f) {
                const std::optional<Hit> hit = cast(services, Ray{origin, toward * (1.0f / length)}, length, character);
                const InstanceId parent = w.parentOf(id);
                const bool own = hit.has_value() && hit->instance.valid() &&
                                 (hit->instance == parent || w.isAncestorOf(parent, hit->instance) ||
                                  w.isAncestorOf(hit->instance, parent));
                if (hit.has_value() && !own && hit->distance < length - 0.1f)
                    return;
            }
        }
        candidates.push_back(Candidate{id, *anchor, away});
    });
    // Nearest first, and by id at a tie: the pool's order must not decide.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.distance != b.distance)
            return a.distance < b.distance;
        return a.prompt.index < b.prompt.index;
    });

    std::vector<i32> keysTaken;
    bool globalTaken = false;
    const auto limit = static_cast<std::size_t>(state.maxPromptsVisible);
    for (const Candidate& candidate : candidates) {
        if (shown.size() >= limit)
            break;
        const scene::ProximityPromptComponent& prompt = *w.proximityPrompts().find(candidate.prompt);
        if (prompt.exclusivity == 0) {
            const i32 key = inputType == 1 ? prompt.gamepadKeyCode : prompt.keyboardKeyCode;
            if (std::find(keysTaken.begin(), keysTaken.end(), key) != keysTaken.end())
                continue;
            keysTaken.push_back(key);
        }
        else if (prompt.exclusivity == 1) {
            if (globalTaken)
                continue;
            globalTaken = true;
        }
        shown.push_back(scene::ShownPrompt{candidate.prompt, candidate.anchor, 0.0f, static_cast<core::u8>(inputType)});
    }
    return shown;
}

void stepPrompts(lua_State* L, const Classes& classes, InstanceId player, const View& view, DVec3 origin, f64 dt,
                 std::span<const input::RawInputEvent> events)
{
    World& w = *context(L).world;
    ServiceState& services = *context(L).services;
    DetectorState& state = services.detectors;
    const i32 device = w.engineState().lastInputDeviceType;
    const i32 inputType = device == 1 ? 1 : device == 2 ? 2 : 0;

    std::vector<scene::ShownPrompt> shown = choosePrompts(L, classes, origin, characterOf(w, player), inputType);

    // Shown and hidden, on this machine.
    for (const InstanceId was : state.shown) {
        const bool still =
            std::any_of(shown.begin(), shown.end(), [&](const scene::ShownPrompt& p) { return p.prompt == was; });
        if (!still && w.alive(was))
            fireHidden(L, was);
    }
    for (const scene::ShownPrompt& now : shown) {
        if (std::find(state.shown.begin(), state.shown.end(), now.prompt) == state.shown.end())
            fireShown(L, now.prompt, inputType);
    }
    state.shown.clear();
    for (const scene::ShownPrompt& now : shown)
        state.shown.push_back(now.prompt);

    // Where each is on the screen, for a tap.
    std::vector<std::pair<InstanceId, Vec2>> onScreen;
    if (view.valid()) {
        for (const scene::ShownPrompt& now : shown) {
            if (const std::optional<Vec2> at = view.project(now.anchor)) {
                const scene::ProximityPromptComponent& prompt = *w.proximityPrompts().find(now.prompt);
                onScreen.emplace_back(now.prompt,
                                      Vec2{at->x + prompt.uiOffset.x, at->y + prompt.uiOffset.y - scene::PromptLift});
            }
        }
    }

    const auto begin = [&](InstanceId id, i32 keyCode, i32 touchId) {
        const scene::ProximityPromptComponent& prompt = *w.proximityPrompts().find(id);
        DetectorState::Hold hold{id, keyCode, touchId, 0.0, false};
        if (prompt.holdDuration <= 0.0) {
            hold.triggered = true;
            act(L, classes, id, player, DetectorMessage::Kind::Triggered);
        }
        else {
            act(L, classes, id, player, DetectorMessage::Kind::HoldBegan);
        }
        state.holds.push_back(hold);
    };
    const auto end = [&](const DetectorState::Hold& hold) {
        if (!w.alive(hold.prompt))
            return;
        if (hold.triggered)
            act(L, classes, hold.prompt, player, DetectorMessage::Kind::TriggerEnded);
        const scene::ProximityPromptComponent* prompt = w.proximityPrompts().find(hold.prompt);
        if (prompt != nullptr && prompt->holdDuration > 0.0)
            act(L, classes, hold.prompt, player, DetectorMessage::Kind::HoldEnded);
    };

    for (const input::RawInputEvent& event : events) {
        const bool touch = event.userInputType == input::UserInputType::Touch;
        if (event.phase == input::RawInputEvent::Phase::Began && !event.uiConsumed) {
            if (touch) {
                const Vec2 at{event.position.x, event.position.y};
                for (const auto& [id, centre] : onScreen) {
                    if (std::abs(at.x - centre.x) <= scene::PromptWidth * 0.5f &&
                        std::abs(at.y - centre.y) <= scene::PromptHeight * 0.5f) {
                        begin(id, 0, event.touchId);
                        break;
                    }
                }
                continue;
            }
            // The engine's own context, below every one a game makes: a key a
            // game's context took is not the prompt's.
            if (event.keyCode == 0 || (services.input != nullptr && services.input->consumed(event.keyCode)))
                continue;
            for (const scene::ShownPrompt& now : shown) {
                const scene::ProximityPromptComponent& prompt = *w.proximityPrompts().find(now.prompt);
                if (event.keyCode == prompt.keyboardKeyCode || event.keyCode == prompt.gamepadKeyCode)
                    begin(now.prompt, event.keyCode, 0);
            }
        }
        else if (event.phase == input::RawInputEvent::Phase::Ended) {
            std::erase_if(state.holds, [&](const DetectorState::Hold& hold) {
                const bool mine = touch ? hold.touchId != 0 && hold.touchId == event.touchId
                                        : hold.touchId == 0 && hold.keyCode == event.keyCode;
                if (mine)
                    end(hold);
                return mine;
            });
        }
    }

    // Holds under way: a hold that reaches its duration triggers; one whose
    // prompt went away ends.
    std::erase_if(state.holds, [&](DetectorState::Hold& hold) {
        const scene::ProximityPromptComponent* prompt = w.proximityPrompts().find(hold.prompt);
        const bool showing = std::find(state.shown.begin(), state.shown.end(), hold.prompt) != state.shown.end();
        if (prompt == nullptr || !showing) {
            end(hold);
            return true;
        }
        if (!hold.triggered) {
            hold.seconds += dt;
            if (hold.seconds >= prompt->holdDuration) {
                hold.triggered = true;
                act(L, classes, hold.prompt, player, DetectorMessage::Kind::Triggered);
            }
        }
        return false;
    });

    // What the frame draws: the engine's look only, with how far a hold is.
    std::vector<scene::ShownPrompt>& drawn = w.engineState().shownPrompts;
    drawn.clear();
    for (scene::ShownPrompt& now : shown) {
        const scene::ProximityPromptComponent& prompt = *w.proximityPrompts().find(now.prompt);
        if (prompt.style != 0)
            continue;
        for (const DetectorState::Hold& hold : state.holds) {
            if (hold.prompt == now.prompt && prompt.holdDuration > 0.0)
                now.holdProgress = static_cast<f32>(std::min(1.0, hold.seconds / prompt.holdDuration));
        }
        drawn.push_back(now);
    }
}

} // namespace

void stepDetectors(lua_State* L, core::f64 dt, std::span<const input::RawInputEvent> events)
{
    World& w = *context(L).world;
    const Classes classes = classesOf(w);
    ServiceState& services = *context(L).services;

    if (w.engineState().networkTopology != scene::NetworkTopology::Replica)
        receive(L, classes);

    // No player here -- a dedicated server -- is nobody pointing at anything.
    const InstanceId player = scene::localPlayerOf(w);
    if (!player.valid()) {
        w.engineState().shownPrompts.clear();
        services.detectors = DetectorState{};
        return;
    }
    const View view = viewOf(w);
    const DVec3 origin = reachOrigin(w, player, view);

    // Prompts first, so a tap on one is not also a click behind it.
    stepPrompts(L, classes, player, view, origin, dt, events);
    std::vector<Vec2> taps;
    if (view.valid()) {
        for (const scene::ShownPrompt& now : w.engineState().shownPrompts) {
            if (const std::optional<Vec2> at = view.project(now.anchor)) {
                const scene::ProximityPromptComponent* prompt = w.proximityPrompts().find(now.prompt);
                const Vec2 offset = prompt != nullptr ? prompt->uiOffset : Vec2{};
                taps.push_back(Vec2{at->x + offset.x, at->y + offset.y - scene::PromptLift});
            }
        }
    }
    stepClicks(L, classes, player, view, origin, events, taps);
}

} // namespace engine::script
