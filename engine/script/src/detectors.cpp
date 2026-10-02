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
    scene::ClassId prompt = 0;
    scene::ClassId basePart = 0;
    scene::ClassId attachment = 0;
    scene::ClassId model = 0;
    scene::ClassId promptService = 0;
    scene::ClassId dragDetector = 0;
};

[[nodiscard]] Classes classesOf(World& w)
{
    const auto id = [&w](std::string_view name) { return w.classes().findId(w.atoms().intern(name)); };
    return Classes{id("ClickDetector"), id("ProximityPrompt"),        id("BasePart"),    id("Attachment"),
                   id("Model"),         id("ProximityPromptService"), id("DragDetector")};
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
        view.cframe = w.engineState().drawnCameraValid ? w.engineState().drawnCamera : camera->cframe;
        view.fieldOfView = camera->fieldOfView;
        view.orthographic = camera->projection == 1;
        view.orthographicSize = camera->orthographicSize;
    }
    return view;
}

// Where a prompt hangs: its part's centre, its attachment, or its model's
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
[[nodiscard]] InstanceId detectorAbove(const World& w, const Classes& classes, InstanceId hit, scene::ClassId kind)
{
    for (InstanceId at = hit; at.valid(); at = w.parentOf(at)) {
        if (at != hit && !w.isA(at, classes.model))
            break;
        if (const InstanceId found = w.findFirstChildOfClass(at, kind); found.valid())
            return found;
    }
    return {};
}

[[nodiscard]] InstanceId detectorAbove(const World& w, const Classes& classes, InstanceId hit)
{
    return detectorAbove(w, classes, hit, classes.clickDetector);
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
    case DetectorMessage::Kind::DragStart:
    case DetectorMessage::Kind::DragContinue:
    case DetectorMessage::Kind::DragEnd:
        // A drag's carry their rays (`fireDrag`).
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

// --- Dragging (ADR 0126 §3) -------------------------------------------------------
//
// **One function works out where a drag puts the part, from the pointer's ray
// and how the drag began** -- on the machine that drags and, in a match, on
// the authority from the rays it is sent -- so the two agree without the
// replica ever naming a position. Its maths is `dmath`'s where it turns an
// angle (R10): the part it moves is the world's.

[[nodiscard]] DVec3 scaled(Vec3 v, f64 by) noexcept
{
    return DVec3{static_cast<f64>(v.x) * by, static_cast<f64>(v.y) * by, static_cast<f64>(v.z) * by};
}

[[nodiscard]] DVec3 scaled(DVec3 v, f64 by) noexcept
{
    return DVec3{v.x * by, v.y * by, v.z * by};
}

[[nodiscard]] f64 dot(DVec3 a, Vec3 b) noexcept
{
    return a.x * static_cast<f64>(b.x) + a.y * static_cast<f64>(b.y) + a.z * static_cast<f64>(b.z);
}

[[nodiscard]] f64 dot(DVec3 a, DVec3 b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] DVec3 crossed(DVec3 a, DVec3 b) noexcept
{
    return DVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] f64 length(DVec3 v) noexcept
{
    return std::sqrt(dot(v, v));
}

// What a detector drags: the part it is in, or the model it is in with the
// part whose frame the drag is worked in.
struct Dragged
{
    InstanceId moved;
    InstanceId handle;
};

[[nodiscard]] std::optional<Dragged> draggedBy(const World& w, const Classes& classes, InstanceId detector)
{
    const InstanceId parent = w.parentOf(detector);
    if (!parent.valid())
        return std::nullopt;
    if (w.parts().find(parent) != nullptr)
        return Dragged{parent, parent};
    if (!w.isA(parent, classes.model))
        return std::nullopt;
    if (const scene::ModelComponent* model = w.models().find(parent);
        model != nullptr && w.parts().find(model->primaryPart) != nullptr)
        return Dragged{parent, model->primaryPart};
    std::vector<InstanceId> inside;
    w.collectDescendants(parent, inside);
    for (const InstanceId id : inside) {
        if (w.parts().find(id) != nullptr)
            return Dragged{parent, id};
    }
    return std::nullopt;
}

// The axis in the world: `ReferenceInstance`'s frame when it names a part.
[[nodiscard]] Vec3 axisOf(const World& w, const scene::DragDetectorComponent& detector) noexcept
{
    Vec3 axis = core::normalize(detector.axis);
    if (const scene::PartComponent* reference = w.parts().find(detector.referenceInstance))
        axis = reference->cframe.rotation * axis;
    return axis;
}

// Where the limits are measured from: `ReferenceInstance`, or where the part
// rested.
[[nodiscard]] DVec3 originOf(const World& w, const scene::DragDetectorComponent& detector) noexcept
{
    if (const scene::PartComponent* reference = w.parts().find(detector.referenceInstance))
        return reference->cframe.position;
    return detector.rest.position;
}

// Where a ray meets the plane through `point` whose normal is `normal`, ahead
// of the ray's start.
[[nodiscard]] std::optional<DVec3> meetPlane(const Ray& ray, DVec3 point, Vec3 normal) noexcept
{
    const f64 facing = static_cast<f64>(core::dot(ray.direction, normal));
    if (std::abs(facing) < 1e-6)
        return std::nullopt;
    const f64 along = dot(point - ray.origin, normal) / facing;
    if (!(along >= 0.0))
        return std::nullopt;
    return ray.origin + scaled(ray.direction, along);
}

struct Placed
{
    core::CFrameD frame;
    // A turn's angle from where the part rests, degrees.
    f64 angle = 0.0;
};

[[nodiscard]] std::optional<Placed> placeDrag(const World& w, const scene::DragDetectorComponent& detector,
                                              const DetectorState::Drag& drag, const Ray& ray)
{
    const bool limited = detector.maxDragTranslation > detector.minDragTranslation;
    switch (detector.dragStyle) {
    case 0: {
        // Along the line through the grab: the point on it nearest the ray.
        const Vec3 axis = axisOf(w, detector);
        const DVec3 between = ray.origin - drag.grab;
        const f64 facing = static_cast<f64>(core::dot(axis, ray.direction));
        const f64 across = 1.0 - facing * facing;
        if (across < 1e-6)
            return std::nullopt;
        const f64 along = (dot(between, axis) - dot(between, ray.direction) * facing) / across;
        DVec3 position = drag.start.position + scaled(axis, along);
        if (limited) {
            const f64 from = dot(position - originOf(w, detector), axis);
            const f64 held = std::clamp(from, detector.minDragTranslation, detector.maxDragTranslation);
            position = position + scaled(axis, held - from);
        }
        return Placed{core::CFrameD{position, drag.start.rotation}, 0.0};
    }
    case 1:
    case 2: {
        // Across a plane through the grab: `Axis`'s normal, or the view's.
        const Vec3 normal =
            detector.dragStyle == 1 ? axisOf(w, detector) : core::normalize(drag.startDirection * -1.0f);
        const std::optional<DVec3> met = meetPlane(ray, drag.grab, normal);
        if (!met.has_value())
            return std::nullopt;
        DVec3 position = drag.start.position + (*met - drag.grab);
        if (limited && detector.maxDragTranslation > 0.0) {
            const DVec3 origin = originOf(w, detector);
            const DVec3 away = position - origin;
            const DVec3 out = scaled(normal, dot(away, normal));
            const DVec3 flat = away - out;
            const f64 far = length(flat);
            if (far > detector.maxDragTranslation)
                position = origin + out + scaled(flat, detector.maxDragTranslation / far);
        }
        return Placed{core::CFrameD{position, drag.start.rotation}, 0.0};
    }
    case 3: {
        // Turned about the axis through the pivot, by the angle between where
        // it was grabbed and where the ray meets the grab's plane.
        const Vec3 axis = axisOf(w, detector);
        const scene::PartComponent* reference = w.parts().find(detector.referenceInstance);
        const DVec3 pivot = reference != nullptr ? reference->cframe.position : drag.start.position;
        const std::optional<DVec3> met = meetPlane(ray, drag.grab, axis);
        if (!met.has_value())
            return std::nullopt;
        const DVec3 axisD = core::toDVec3(axis);
        const auto flatten = [&](DVec3 v) { return v - scaled(axisD, dot(v, axisD)); };
        const DVec3 from = flatten(drag.grab - pivot);
        const DVec3 to = flatten(*met - pivot);
        if (length(from) < 1e-4 || length(to) < 1e-4)
            return std::nullopt;
        const f64 turned = core::dmath::atan2(dot(crossed(from, to), axisD), dot(from, to)) * 180.0 / std::numbers::pi;
        f64 angle = drag.startAngle + turned;
        if (detector.maxDragAngle > detector.minDragAngle)
            angle = std::clamp(angle, detector.minDragAngle, detector.maxDragAngle);
        const auto radians = static_cast<f32>((angle - drag.startAngle) * std::numbers::pi / 180.0);
        const core::Mat3 turn = core::fromAxisAngle(axis, radians);
        const DVec3 position = pivot + core::toDVec3(turn * core::toVec3(drag.start.position - pivot));
        return Placed{core::CFrameD{position, turn * drag.start.rotation}, angle};
    }
    case 4: {
        // Turned as a ball about its pivot: from where it was grabbed to where
        // the ray meets the plane facing the view through the grab.
        const DVec3 pivot = drag.start.position;
        const std::optional<DVec3> met = meetPlane(ray, drag.grab, core::normalize(drag.startDirection * -1.0f));
        if (!met.has_value())
            return std::nullopt;
        const DVec3 from = drag.grab - pivot;
        const DVec3 to = *met - pivot;
        const DVec3 about = crossed(from, to);
        const f64 size = length(about);
        if (size < 1e-9 || length(from) < 1e-4 || length(to) < 1e-4)
            return Placed{drag.start, drag.startAngle};
        const auto radians = static_cast<f32>(core::dmath::atan2(size, dot(from, to)));
        const core::Mat3 turn = core::fromAxisAngle(core::toVec3(scaled(about, 1.0 / size)), radians);
        return Placed{core::CFrameD{pivot, turn * drag.start.rotation}, drag.startAngle};
    }
    default:
        // `Scriptable`: the events say where the pointer is, and nothing moves.
        return std::nullopt;
    }
}

// **Moves what a drag holds to where it was dragged**: put there, or pulled
// there with a force under `MaxForce`. A turn, and an anchored part, are put
// there -- a force turns nothing yet (`MaxTorque` is stored).
void moveDrag(lua_State* L, const scene::DragDetectorComponent& detector, const DetectorState::Drag& drag,
              const Placed& placed, f64 dt)
{
    World& w = *context(L).world;
    const scene::PartComponent* handle = w.parts().find(drag.handle);
    if (handle == nullptr)
        return;
    const bool turning = detector.dragStyle == 3 || detector.dragStyle == 4;
    scene::RigidBodyComponent* body = w.rigidBodies().find(drag.handle);
    if (detector.responseStyle == 1 && !turning && body != nullptr && !body->anchored && drag.moved == drag.handle) {
        const Vec3 size = handle->size;
        const f64 mass = std::max(1e-3, static_cast<f64>(body->density) * static_cast<f64>(size.x) *
                                            static_cast<f64>(size.y) * static_cast<f64>(size.z));
        const DVec3 gap = placed.frame.position - handle->cframe.position;
        const DVec3 wanted = scaled(gap, detector.responsiveness);
        const DVec3 change = wanted - core::toDVec3(body->linearVelocity);
        DVec3 impulse = scaled(change, mass);
        const f64 most = detector.maxForce * dt;
        if (const f64 strength = length(impulse); strength > most && strength > 0.0)
            impulse = scaled(impulse, most / strength);
        body->pendingImpulse = body->pendingImpulse + core::toVec3(impulse);
        return;
    }
    // Put there: the model with it, by the same move, as `PivotTo` does.
    const core::NameAtom cframe = context(L).wellKnown.cframe;
    const core::CFrameD delta = placed.frame * core::inverse(handle->cframe);
    if (drag.moved != drag.handle) {
        std::vector<InstanceId> inside;
        w.collectDescendants(drag.moved, inside);
        for (const InstanceId id : inside) {
            if (const scene::PartComponent* part = w.parts().find(id))
                w.setProperty(id, cframe, scene::Value{delta * part->cframe});
        }
    }
    else {
        w.setProperty(drag.handle, cframe, scene::Value{placed.frame});
    }
    flushSceneChanges(L);
}

void fireDrag(lua_State* L, InstanceId detector, InstanceId player, DetectorMessage::Kind kind, const Ray& ray,
              DVec3 hit)
{
    World& w = *context(L).world;
    const std::string_view name = kind == DetectorMessage::Kind::DragStart      ? "DragStart"
                                  : kind == DetectorMessage::Kind::DragContinue ? "DragContinue"
                                                                                : "DragEnd";
    const scene::EventDesc* descriptor = w.classes().findEvent(w.classOf(detector), w.atoms().intern(name));
    if (descriptor == nullptr)
        return;
    const int first = lua_gettop(L) + 1;
    pushInstance(L, player);
    int count = 1;
    if (kind != DetectorMessage::Kind::DragEnd) {
        pushVector3(L, core::toVec3(ray.origin));
        pushVector3(L, ray.direction);
        count = 3;
        if (kind == DetectorMessage::Kind::DragStart) {
            pushVector3(L, core::toVec3(hit));
            count = 4;
        }
    }
    fireInstanceEvent(L, detector, descriptor->slot, first, count);
    lua_pop(L, count);
}

// Whether a drag may go on: the detector live, in the world and enabled, the
// part still there, and the player's character still near it -- a drag
// walked away from lets go.
[[nodiscard]] bool dragHolds(const World& w, const Classes& classes, const DetectorState::Drag& drag, f64 slack)
{
    if (!w.alive(drag.detector) || w.destroyed(drag.detector) || !w.isA(drag.detector, classes.dragDetector))
        return false;
    const scene::DragDetectorComponent* detector = w.dragDetectors().find(drag.detector);
    const scene::PartComponent* handle = w.parts().find(drag.handle);
    if (detector == nullptr || !detector->enabled || handle == nullptr || w.destroyed(drag.handle))
        return false;
    const std::optional<Dragged> now = draggedBy(w, classes, drag.detector);
    if (!now.has_value() || now->handle != drag.handle)
        return false;
    const scene::PlayerComponent* player = w.players().find(drag.player);
    const scene::PartComponent* body = player != nullptr ? w.parts().find(player->character) : nullptr;
    if (body == nullptr)
        return true;
    const f64 extent = static_cast<f64>(core::length(handle->size * 0.5f));
    return distance(handle->cframe.position, body->cframe.position) <= detector->maxActivationDistance + extent + slack;
}

// A drag begun: the detector's rest taken the first time, and the drag's
// start.
[[nodiscard]] DetectorState::Drag beginDrag(World& w, InstanceId detectorId, InstanceId player, const Dragged& dragged,
                                            const Ray& ray, DVec3 hit, i32 touchId, Vec2 pointer)
{
    scene::DragDetectorComponent& detector = *w.dragDetectors().find(detectorId);
    const scene::PartComponent& handle = *w.parts().find(dragged.handle);
    if (!detector.rested) {
        detector.rest = handle.cframe;
        detector.rested = true;
        detector.angle = 0.0;
    }
    DetectorState::Drag drag;
    drag.detector = detectorId;
    drag.player = player;
    drag.moved = dragged.moved;
    drag.handle = dragged.handle;
    drag.touchId = touchId;
    drag.pointer = pointer;
    drag.grab = hit;
    drag.start = handle.cframe;
    drag.startDirection = ray.direction;
    drag.startAngle = detector.angle;
    return drag;
}

// A tick of a drag: where it puts the part, the part moved when this machine
// is the one to move it, and the turn's angle kept.
void continueDrag(lua_State* L, const DetectorState::Drag& drag, const Ray& ray, f64 dt, bool move)
{
    World& w = *context(L).world;
    scene::DragDetectorComponent* detector = w.dragDetectors().find(drag.detector);
    if (detector == nullptr || detector->responseStyle == 2)
        return;
    const std::optional<Placed> placed = placeDrag(w, *detector, drag, ray);
    if (!placed.has_value())
        return;
    if (detector->dragStyle == 3)
        detector->angle = placed->angle;
    if (move)
        moveDrag(L, *detector, drag, *placed, dt);
}

// Whether this machine moves what it drags: alone, or the authority, always;
// a replica only a `Physical` drag of a part handed to it -- a `Geometric`
// one is the authority's to move, from the rays it is sent.
[[nodiscard]] bool movesHere(const World& w, const scene::DragDetectorComponent& detector, InstanceId handle)
{
    if (w.engineState().networkTopology != scene::NetworkTopology::Replica)
        return true;
    if (detector.responseStyle != 1)
        return false;
    const scene::RigidBodyComponent* body = w.rigidBodies().find(handle);
    return body != nullptr && !body->anchored && body->networkOwner != 0;
}

void tellAuthority(World& w, const DetectorState::Drag& drag, DetectorMessage::Kind kind, const Ray& ray, DVec3 hit)
{
    scene::EngineState& state = w.engineState();
    if (state.networkTopology != scene::NetworkTopology::Replica)
        return;
    DetectorMessage message{drag.detector, {}, kind, 0};
    message.origin = ray.origin;
    message.direction = ray.direction;
    message.hit = hit;
    state.detectorOutbox.push_back(message);
}

void endDrag(lua_State* L, const DetectorState::Drag& drag, bool tell)
{
    World& w = *context(L).world;
    if (w.alive(drag.detector))
        fireDrag(L, drag.detector, drag.player, DetectorMessage::Kind::DragEnd, Ray{}, DVec3{});
    if (tell)
        tellAuthority(w, drag, DetectorMessage::Kind::DragEnd, Ray{}, DVec3{});
}

// --- The authority --------------------------------------------------------------

// A drag a client sent: begun only where its player could have taken hold --
// the detector live, enabled and dragging something, the hit on it and within
// reach of the player's character -- and from then on worked out here from
// the client's rays, as the client did.
void receiveDrag(lua_State* L, const Classes& classes, const DetectorMessage& message)
{
    World& w = *context(L).world;
    ServiceState& services = *context(L).services;
    std::vector<DetectorState::Drag>& drags = services.detectors.remoteDrags;
    const auto held = std::find_if(drags.begin(), drags.end(), [&](const DetectorState::Drag& drag) {
        return drag.player == message.player && drag.detector == message.detector;
    });
    const Ray ray{message.origin, message.direction};

    const auto release = [&](const DetectorState::Drag& drag) {
        // A part handed to the player comes back, if it is still theirs.
        if (drag.handedOver) {
            const scene::PlayerComponent* player = w.players().find(drag.player);
            scene::RigidBodyComponent* body = w.rigidBodies().find(drag.handle);
            if (body != nullptr && player != nullptr && body->networkOwner == player->userId)
                body->networkOwner = drag.previousOwner;
        }
    };

    if (message.kind == DetectorMessage::Kind::DragStart) {
        if (held != drags.end())
            return;
        if (!w.alive(message.detector) || w.destroyed(message.detector) ||
            !w.isA(message.detector, classes.dragDetector))
            return;
        const scene::DragDetectorComponent* detector = w.dragDetectors().find(message.detector);
        const std::optional<Dragged> dragged = draggedBy(w, classes, message.detector);
        const scene::PlayerComponent* player = w.players().find(message.player);
        const scene::PartComponent* body = player != nullptr ? w.parts().find(player->character) : nullptr;
        if (detector == nullptr || !detector->enabled || !dragged.has_value() || body == nullptr)
            return;
        const scene::PartComponent& handle = *w.parts().find(dragged->handle);
        const f64 extent = static_cast<f64>(core::length(handle.size * 0.5f));
        if (distance(message.hit, handle.cframe.position) > extent + AuthoritySlack ||
            distance(message.hit, body->cframe.position) > detector->maxActivationDistance + AuthoritySlack)
            return;
        DetectorState::Drag drag = beginDrag(w, message.detector, message.player, *dragged, ray, message.hit, 0, {});
        // **A `Physical` drag of an unanchored part is the player's** for its
        // duration (ADR 0099): its machine pulls it, and simulates it.
        if (scene::RigidBodyComponent* rigid = w.rigidBodies().find(dragged->handle);
            detector->responseStyle == 1 && rigid != nullptr && !rigid->anchored && dragged->moved == dragged->handle &&
            !player->local) {
            drag.previousOwner = rigid->networkOwner;
            drag.handedOver = true;
            rigid->networkOwner = player->userId;
        }
        drags.push_back(drag);
        fireDrag(L, message.detector, message.player, DetectorMessage::Kind::DragStart, ray, message.hit);
        return;
    }
    if (held == drags.end())
        return;
    const DetectorState::Drag drag = *held;
    if (message.kind == DetectorMessage::Kind::DragEnd || !dragHolds(w, classes, drag, 2.0 * AuthoritySlack)) {
        drags.erase(held);
        release(drag);
        if (w.alive(drag.detector))
            fireDrag(L, drag.detector, drag.player, DetectorMessage::Kind::DragEnd, Ray{}, DVec3{});
        return;
    }
    // A `Physical` drag handed over is pulled by the player's machine; every
    // other is worked out and moved here.
    continueDrag(L, drag, ray, w.engineState().fixedTimestep, !drag.handedOver);
    fireDrag(L, drag.detector, drag.player, DetectorMessage::Kind::DragContinue, ray, DVec3{});
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
        if (message.kind == DetectorMessage::Kind::DragStart || message.kind == DetectorMessage::Kind::DragContinue ||
            message.kind == DetectorMessage::Kind::DragEnd) {
            if (w.alive(message.player))
                receiveDrag(L, classes, message);
            continue;
        }
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

// This machine's drag: begun by a press on a detector within reach, moved
// each tick from where the pointer -- or the finger -- is, ended when it comes
// up or the drag can no longer hold.
void stepDrags(lua_State* L, const Classes& classes, InstanceId player, const View& view, DVec3 origin, f64 dt,
               std::span<const input::RawInputEvent> events)
{
    World& w = *context(L).world;
    ServiceState& services = *context(L).services;
    DetectorState& state = services.detectors;
    const auto pointerPixel = [&]() -> Vec2 {
        // A locked pointer points from the middle of the screen.
        return w.engineState().pointerLocked ? view.viewport * 0.5f : w.engineState().pointerPosition;
    };

    const auto finish = [&] {
        if (state.drag.has_value()) {
            const DetectorState::Drag drag = *state.drag;
            state.drag.reset();
            endDrag(L, drag, true);
        }
    };

    for (const input::RawInputEvent& event : events) {
        const bool touch = event.userInputType == input::UserInputType::Touch;
        const bool mouse = static_cast<i32>(event.userInputType) == MouseButton1;
        if (!touch && !mouse)
            continue;
        if (event.phase == input::RawInputEvent::Phase::Began && !event.uiConsumed && !state.drag.has_value()) {
            if (!view.valid())
                continue;
            const Vec2 pixel = touch ? Vec2{event.position.x, event.position.y} : pointerPixel();
            const Ray ray = view.rayThrough(pixel);
            const std::optional<Hit> hit = cast(services, ray, PointerReach, characterOf(w, player));
            if (!hit.has_value() || !hit->instance.valid())
                continue;
            const InstanceId detectorId = detectorAbove(w, classes, hit->instance, classes.dragDetector);
            const scene::DragDetectorComponent* detector = w.dragDetectors().find(detectorId);
            if (detector == nullptr || !detector->enabled ||
                distance(hit->position, origin) > detector->maxActivationDistance)
                continue;
            const std::optional<Dragged> dragged = draggedBy(w, classes, detectorId);
            if (!dragged.has_value())
                continue;
            state.drag =
                beginDrag(w, detectorId, player, *dragged, ray, hit->position, touch ? event.touchId : 0, pixel);
            fireDrag(L, detectorId, player, DetectorMessage::Kind::DragStart, ray, hit->position);
            tellAuthority(w, *state.drag, DetectorMessage::Kind::DragStart, ray, hit->position);
        }
        else if (state.drag.has_value() &&
                 (touch ? state.drag->touchId == event.touchId && event.touchId != 0 : state.drag->touchId == 0)) {
            if (event.phase == input::RawInputEvent::Phase::Ended)
                finish();
            else if (touch)
                state.drag->pointer = Vec2{event.position.x, event.position.y};
        }
    }

    if (!state.drag.has_value())
        return;
    if (!view.valid() || !dragHolds(w, classes, *state.drag, 0.0)) {
        finish();
        return;
    }
    const Vec2 pixel = state.drag->touchId != 0 ? state.drag->pointer : pointerPixel();
    const Ray ray = view.rayThrough(pixel);
    const scene::DragDetectorComponent& detector = *w.dragDetectors().find(state.drag->detector);
    continueDrag(L, *state.drag, ray, dt, movesHere(w, detector, state.drag->handle));
    fireDrag(L, state.drag->detector, player, DetectorMessage::Kind::DragContinue, ray, DVec3{});
    tellAuthority(w, *state.drag, DetectorMessage::Kind::DragContinue, ray, DVec3{});
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
        InstanceId hangsFrom;
    };
    std::vector<Candidate> candidates;
    w.proximityPrompts().forEach([&](InstanceId id, const scene::ProximityPromptComponent& prompt) {
        if (!prompt.enabled || w.destroyed(id) || !w.isAncestorOf(services.dataModel, id))
            return;
        InstanceId hangsFrom;
        const std::optional<DVec3> anchor = anchorOf(w, classes, id, &hangsFrom);
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
        candidates.push_back(Candidate{id, *anchor, away, hangsFrom});
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
        scene::ShownPrompt now{.prompt = candidate.prompt,
                               .anchor = candidate.anchor,
                               .inputType = static_cast<core::u8>(inputType),
                               .hangsFrom = candidate.hangsFrom};
        // Where the frame drew it last, kept across the tick that lists it
        // again: a tap is tested on the box the player saw.
        for (const scene::ShownPrompt& before : w.engineState().shownPrompts) {
            if (before.prompt == now.prompt && before.drawn) {
                now.drawnAt = before.drawnAt;
                now.drawn = true;
            }
        }
        shown.push_back(now);
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
            if (now.drawn) {
                onScreen.emplace_back(now.prompt, now.drawnAt);
                continue;
            }
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

    if (w.engineState().networkTopology != scene::NetworkTopology::Replica) {
        receive(L, classes);
        // A player who went, or whose drag can no longer hold, lets go.
        std::vector<DetectorState::Drag>& drags = services.detectors.remoteDrags;
        for (std::size_t at = 0; at < drags.size();) {
            const DetectorState::Drag drag = drags[at];
            if (w.alive(drag.player) && dragHolds(w, classes, drag, 2.0 * AuthoritySlack)) {
                ++at;
                continue;
            }
            drags.erase(drags.begin() + static_cast<std::ptrdiff_t>(at));
            if (drag.handedOver) {
                const scene::PlayerComponent* owner = w.players().find(drag.player);
                scene::RigidBodyComponent* body = w.rigidBodies().find(drag.handle);
                if (body != nullptr && (owner == nullptr || body->networkOwner == owner->userId))
                    body->networkOwner = drag.previousOwner;
            }
            if (w.alive(drag.detector) && w.alive(drag.player))
                fireDrag(L, drag.detector, drag.player, DetectorMessage::Kind::DragEnd, Ray{}, DVec3{});
        }
    }

    // No player here -- a dedicated server -- is nobody pointing at anything.
    const InstanceId player = scene::localPlayerOf(w);
    if (!player.valid()) {
        w.engineState().shownPrompts.clear();
        std::vector<DetectorState::Drag> remote = std::move(services.detectors.remoteDrags);
        services.detectors = DetectorState{};
        services.detectors.remoteDrags = std::move(remote);
        return;
    }
    const View view = viewOf(w);
    const DVec3 origin = reachOrigin(w, player, view);

    // Prompts first, so a tap on one is not also a click behind it.
    stepPrompts(L, classes, player, view, origin, dt, events);
    std::vector<Vec2> taps;
    if (view.valid()) {
        for (const scene::ShownPrompt& now : w.engineState().shownPrompts) {
            if (now.drawn) {
                taps.push_back(now.drawnAt);
                continue;
            }
            if (const std::optional<Vec2> at = view.project(now.anchor)) {
                const scene::ProximityPromptComponent* prompt = w.proximityPrompts().find(now.prompt);
                const Vec2 offset = prompt != nullptr ? prompt->uiOffset : Vec2{};
                taps.push_back(Vec2{at->x + offset.x, at->y + offset.y - scene::PromptLift});
            }
        }
    }
    // A drag takes the press before a click does not: a part may have both.
    stepDrags(L, classes, player, view, origin, dt, events);
    stepClicks(L, classes, player, view, origin, events, taps);
}

} // namespace engine::script
