// The hand-written half of the render module's classes (architecture.md §4).
//
// The generator emits descriptors and declares these; nothing here knows what a
// descriptor is. Each pair is the only code that touches its component, which
// is what lets a property's storage move without the API definition changing.
//
// Every getter answers `Value{}` -- nil -- for an instance whose component is
// missing, rather than asserting. A property read on an instance that is being
// torn down is ordinary, and the alternative is a crash on a race the tree
// makes legal.
//
// Every setter returns false rather than raising. `scene` has no error
// formatting (that lives above it), so the caller turns a false into the
// property's own `errKeyOnInvalidSet`.
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>

#include "../generated/class_descriptors.gen.h"
#include "engine/asset/material.h"
#include "engine/core/math.h"
#include "engine/render/lighting.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::render::native {
namespace {

using core::f32;
using core::f64;
using scene::Value;

// The read/write pairs below are one line each and exist so the null check is
// written once per component rather than once per accessor. `find` on a pool
// the instance does not belong to returns null, which is the case every getter
// has to answer nil for.
[[nodiscard]] const scene::MeshPartComponent* readMeshPart(const scene::World& world, core::InstanceId id)
{
    return world.meshParts().find(id);
}

[[nodiscard]] scene::MeshPartComponent* writeMeshPart(scene::World& world, core::InstanceId id)
{
    return world.meshParts().find(id);
}

[[nodiscard]] const scene::CameraComponent* readCamera(const scene::World& world, core::InstanceId id)
{
    return world.cameras().find(id);
}

[[nodiscard]] scene::CameraComponent* writeCamera(scene::World& world, core::InstanceId id)
{
    return world.cameras().find(id);
}

[[nodiscard]] const scene::PointLightComponent* readPointLight(const scene::World& world, core::InstanceId id)
{
    return world.pointLights().find(id);
}

[[nodiscard]] scene::PointLightComponent* writePointLight(scene::World& world, core::InstanceId id)
{
    return world.pointLights().find(id);
}

[[nodiscard]] const scene::SpotLightComponent* readSpotLight(const scene::World& world, core::InstanceId id)
{
    return world.spotLights().find(id);
}

[[nodiscard]] scene::SpotLightComponent* writeSpotLight(scene::World& world, core::InstanceId id)
{
    return world.spotLights().find(id);
}

[[nodiscard]] const scene::LightingComponent* readLighting(const scene::World& world, core::InstanceId id)
{
    return world.lighting().find(id);
}

[[nodiscard]] scene::LightingComponent* writeLighting(scene::World& world, core::InstanceId id)
{
    return world.lighting().find(id);
}

// A number property that must stay finite and positive. Rejecting rather than
// clamping: a NaN field of view produces a projection matrix of NaNs and a black
// frame with no message, and the write that caused it is long gone by then.
[[nodiscard]] bool takePositive(const Value& value, f32& out) noexcept
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !std::isfinite(*number) || *number <= 0.0)
        return false;
    out = static_cast<f32>(*number);
    return true;
}

// A number property that may be any finite value, including zero and negatives.
[[nodiscard]] bool takeFinite(const Value& value, f32& out) noexcept
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !std::isfinite(*number))
        return false;
    out = static_cast<f32>(*number);
    return true;
}

} // namespace

// --- MeshPart ---------------------------------------------------------------

// MeshPart.PoseFrom (ADR 0201): a reference, kept as written. Who it leads to
// is found by the animation system each tick, so naming a mesh that is not
// skinned, or itself, is stored and follows nobody.
Value getMeshPartPoseFrom(const scene::World& world, core::InstanceId id)
{
    const scene::MeshPartComponent* mesh = world.meshParts().find(id);
    if (mesh == nullptr || !mesh->poseFrom.valid() || !world.alive(mesh->poseFrom))
        return Value{};
    return Value{mesh->poseFrom};
}

bool setMeshPartPoseFrom(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::MeshPartComponent* mesh = world.meshParts().find(id);
    if (mesh == nullptr)
        return false;
    if (const auto* reference = std::get_if<core::InstanceId>(&value); reference != nullptr) {
        // Only a mesh can be worn.
        if (reference->valid() && world.meshParts().find(*reference) == nullptr)
            return false;
        mesh->poseFrom = *reference;
        return true;
    }
    if (scene::valueType(value) != scene::ValueType::Nil)
        return false;
    mesh->poseFrom = core::InstanceId{};
    return true;
}

Value getMeshPartMeshContent(const scene::World& world, core::InstanceId id)
{
    const scene::MeshPartComponent* mesh = readMeshPart(world, id);
    if (mesh == nullptr)
        return Value{};
    return std::string(world.atoms().text(mesh->meshContent));
}

bool setMeshPartMeshContent(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::MeshPartComponent* mesh = writeMeshPart(world, id);
    if (text == nullptr || mesh == nullptr)
        return false;
    // Interned, not resolved. Whether the file loads is the renderer's problem
    // and a later one; reading the property back must give what was written
    // even when it does not.
    mesh->meshContent = world.atoms().intern(*text);
    return true;
}

Value getMeshPartCollisionFidelity(const scene::World& world, core::InstanceId id)
{
    const scene::MeshPartComponent* mesh = readMeshPart(world, id);
    if (mesh == nullptr)
        return Value{};
    return Value{scene::EnumValue{generated::CollisionFidelityEnumId, mesh->collisionFidelity}};
}

bool setMeshPartCollisionFidelity(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::MeshPartComponent* mesh = writeMeshPart(world, id);
    if (item == nullptr || mesh == nullptr || item->enumId != generated::CollisionFidelityEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    // Stored as written, including `Precise`, which this release collides as a
    // hull. A value that silently became another value is worse than one that
    // reads back what was asked for and says what it did (the property's Doc).
    mesh->collisionFidelity = item->value;
    return true;
}

Value getMeshPartMeshSize(const scene::World& world, core::InstanceId id)
{
    const scene::MeshPartComponent* mesh = readMeshPart(world, id);
    if (mesh == nullptr)
        return Value{};
    return Value{mesh->meshSize};
}

bool setMeshPartMeshSize(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* size = std::get_if<core::Vec3>(&value);
    scene::MeshPartComponent* mesh = writeMeshPart(world, id);
    if (size == nullptr || mesh == nullptr)
        return false;
    // It is a DIVISOR, so a zero or a negative is not a small mesh -- it is a
    // scale factor that is infinite or mirrored, and both reach the renderer and
    // the hull builder as a shape neither can make. Refused here, where it
    // becomes a keyed error.
    if (!std::isfinite(size->x) || !std::isfinite(size->y) || !std::isfinite(size->z) || size->x <= 0.0f ||
        size->y <= 0.0f || size->z <= 0.0f)
        return false;
    mesh->meshSize = *size;
    return true;
}

// --- Bone ---------------------------------------------------------------------
//
// **No storage hook.** `Bone` inherits `Attachment`'s component, and declaring
// one here would attach a second -- `World::create` calls every hook down the
// ancestry (the same reason `Script` does not declare one beside `BaseScript`'s).

Value getBoneJointName(const scene::World& world, core::InstanceId id)
{
    const scene::AttachmentComponent* bone = world.attachments().find(id);
    if (bone == nullptr)
        return Value{};
    return std::string(world.atoms().text(bone->jointName));
}

bool setBoneJointName(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::AttachmentComponent* bone = world.attachments().find(id);
    if (text == nullptr || bone == nullptr)
        return false;
    bone->jointName = world.atoms().intern(*text);
    // Re-resolved by the tick rather than here: this module cannot see the
    // skeleton library, and a name written before a mesh has loaded has nothing
    // to resolve against anyway.
    bone->jointIndex = -1;
    return true;
}

Value getBoneJointIndex(const scene::World& world, core::InstanceId id)
{
    const scene::AttachmentComponent* bone = world.attachments().find(id);
    return bone == nullptr ? Value{} : Value{static_cast<f64>(bone->jointIndex)};
}

Value getBoneTransform(const scene::World& world, core::InstanceId id)
{
    const scene::AttachmentComponent* bone = world.attachments().find(id);
    return bone == nullptr ? Value{} : Value{bone->transform};
}

bool setBoneTransform(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* frame = std::get_if<core::CFrameD>(&value);
    scene::AttachmentComponent* bone = world.attachments().find(id);
    if (frame == nullptr || bone == nullptr)
        return false;
    bone->transform = *frame;
    return true;
}

// --- SpringBone, SpringCollider (ADR 0194) --------------------------------------
//
// What the instances say. Where the chain is, frame by frame, is not here: it
// is the renderer's, each machine's own, and no property reads it.

void attachSpringBoneComponents(scene::World& world, core::InstanceId id)
{
    world.springBones().add(id, scene::SpringBoneComponent{});
}

void detachSpringBoneComponents(scene::World& world, core::InstanceId id)
{
    world.springBones().remove(id);
}

void attachSpringColliderComponents(scene::World& world, core::InstanceId id)
{
    world.springColliders().add(id, scene::SpringColliderComponent{});
}

void detachSpringColliderComponents(scene::World& world, core::InstanceId id)
{
    world.springColliders().remove(id);
}

namespace {

// One number of a component, read and written within a range. `least` and
// `most` are inclusive; a write outside them, or one that is not a number, is
// refused and the property's own error says what it takes.
template <class Component>
[[nodiscard]] Value readNumber(const Component* component, f32 Component::*field)
{
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->*field)};
}

template <class Component>
[[nodiscard]] bool writeNumber(Component* component, f32 Component::*field, const Value& value, f64 least, f64 most)
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || component == nullptr || !std::isfinite(*number) || *number < least || *number > most)
        return false;
    component->*field = static_cast<f32>(*number);
    return true;
}

constexpr f64 AnyAmount = 1.0e9;

} // namespace

Value getSpringBoneRootJoint(const scene::World& world, core::InstanceId id)
{
    const scene::SpringBoneComponent* spring = world.springBones().find(id);
    return spring == nullptr ? Value{} : Value{std::string(world.atoms().text(spring->rootJoint))};
}

bool setSpringBoneRootJoint(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::SpringBoneComponent* spring = world.springBones().find(id);
    if (text == nullptr || spring == nullptr)
        return false;
    spring->rootJoint = world.atoms().intern(*text);
    return true;
}

Value getSpringBoneJointPattern(const scene::World& world, core::InstanceId id)
{
    const scene::SpringBoneComponent* spring = world.springBones().find(id);
    return spring == nullptr ? Value{} : Value{std::string(world.atoms().text(spring->jointPattern))};
}

bool setSpringBoneJointPattern(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::SpringBoneComponent* spring = world.springBones().find(id);
    if (text == nullptr || spring == nullptr)
        return false;
    spring->jointPattern = world.atoms().intern(*text);
    return true;
}

Value getSpringBoneEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::SpringBoneComponent* spring = world.springBones().find(id);
    return spring == nullptr ? Value{} : Value{spring->enabled};
}

bool setSpringBoneEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::SpringBoneComponent* spring = world.springBones().find(id);
    if (flag == nullptr || spring == nullptr)
        return false;
    spring->enabled = *flag;
    return true;
}

Value getSpringBoneStiffness(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springBones().find(id), &scene::SpringBoneComponent::stiffness);
}

bool setSpringBoneStiffness(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.springBones().find(id), &scene::SpringBoneComponent::stiffness, value, 0.0, 1.0);
}

Value getSpringBoneDamping(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springBones().find(id), &scene::SpringBoneComponent::damping);
}

bool setSpringBoneDamping(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.springBones().find(id), &scene::SpringBoneComponent::damping, value, 0.0, 1.0);
}

Value getSpringBoneGravityScale(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springBones().find(id), &scene::SpringBoneComponent::gravityScale);
}

bool setSpringBoneGravityScale(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.springBones().find(id), &scene::SpringBoneComponent::gravityScale, value, -AnyAmount,
                       AnyAmount);
}

Value getSpringBoneInertia(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springBones().find(id), &scene::SpringBoneComponent::inertia);
}

bool setSpringBoneInertia(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.springBones().find(id), &scene::SpringBoneComponent::inertia, value, 0.0, 1.0);
}

Value getSpringBoneLimitAngle(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springBones().find(id), &scene::SpringBoneComponent::limitAngle);
}

bool setSpringBoneLimitAngle(scene::World& world, core::InstanceId id, const Value& value)
{
    // Greater than nought and less than a half turn, as the error says.
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !(*number > 0.0) || !(*number < 180.0))
        return false;
    return writeNumber(world.springBones().find(id), &scene::SpringBoneComponent::limitAngle, value, 0.0, 180.0);
}

Value getSpringBoneRadius(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springBones().find(id), &scene::SpringBoneComponent::radius);
}

bool setSpringBoneRadius(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.springBones().find(id), &scene::SpringBoneComponent::radius, value, 0.0, AnyAmount);
}

Value getSpringBoneWindInfluence(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springBones().find(id), &scene::SpringBoneComponent::windInfluence);
}

bool setSpringBoneWindInfluence(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.springBones().find(id), &scene::SpringBoneComponent::windInfluence, value, 0.0, AnyAmount);
}

Value getSpringColliderJointName(const scene::World& world, core::InstanceId id)
{
    const scene::SpringColliderComponent* collider = world.springColliders().find(id);
    return collider == nullptr ? Value{} : Value{std::string(world.atoms().text(collider->jointName))};
}

bool setSpringColliderJointName(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::SpringColliderComponent* collider = world.springColliders().find(id);
    if (text == nullptr || collider == nullptr)
        return false;
    collider->jointName = world.atoms().intern(*text);
    return true;
}

Value getSpringColliderRadius(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springColliders().find(id), &scene::SpringColliderComponent::radius);
}

bool setSpringColliderRadius(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !(*number > 0.0))
        return false;
    return writeNumber(world.springColliders().find(id), &scene::SpringColliderComponent::radius, value, 0.0,
                       AnyAmount);
}

Value getSpringColliderLength(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.springColliders().find(id), &scene::SpringColliderComponent::length);
}

bool setSpringColliderLength(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.springColliders().find(id), &scene::SpringColliderComponent::length, value, 0.0,
                       AnyAmount);
}

Value getSpringColliderOffset(const scene::World& world, core::InstanceId id)
{
    const scene::SpringColliderComponent* collider = world.springColliders().find(id);
    return collider == nullptr ? Value{} : Value{collider->offset};
}

bool setSpringColliderOffset(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* offset = std::get_if<core::Vec3>(&value);
    scene::SpringColliderComponent* collider = world.springColliders().find(id);
    if (offset == nullptr || collider == nullptr || !std::isfinite(offset->x) || !std::isfinite(offset->y) ||
        !std::isfinite(offset->z))
        return false;
    collider->offset = *offset;
    return true;
}

void attachMeshPartComponents(scene::World& world, core::InstanceId id)
{
    world.meshParts().add(id, scene::MeshPartComponent{});
}

void detachMeshPartComponents(scene::World& world, core::InstanceId id)
{
    world.meshParts().remove(id);
}

// --- Camera -----------------------------------------------------------------

// **By phase** (ADR 0136): a render phase reads and writes the camera's
// presentation, which is drawn as written; every other phase -- and the world
// hash, which reads through here -- the simulated `cframe`.
Value getCameraCFrame(const scene::World& world, core::InstanceId id)
{
    const scene::CameraComponent* camera = readCamera(world, id);
    if (camera == nullptr)
        return Value{};
    const bool presented = world.engineState().renderPhase && camera->presenting;
    return Value{presented ? camera->presented : camera->cframe};
}

bool setCameraCFrame(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* cframe = std::get_if<core::CFrameD>(&value);
    scene::CameraComponent* camera = writeCamera(world, id);
    if (cframe == nullptr || camera == nullptr)
        return false;
    if (world.engineState().renderPhase) {
        camera->presented = *cframe;
        camera->presenting = true;
        camera->presentedSinceTick = true;
        return true;
    }
    camera->cframe = *cframe;
    camera->presenting = false;
    camera->presentedSinceTick = false;
    return true;
}

Value getCameraViewportSize(const scene::World& world, core::InstanceId id)
{
    // One size for every camera: they all draw into the same target, and the
    // one that is not current would draw into it the moment it became so.
    return readCamera(world, id) == nullptr ? Value{} : Value{world.engineState().viewportSize};
}

Value getCameraFieldOfView(const scene::World& world, core::InstanceId id)
{
    const scene::CameraComponent* camera = readCamera(world, id);
    return camera == nullptr ? Value{} : Value{static_cast<f64>(camera->fieldOfView)};
}

bool setCameraFieldOfView(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::CameraComponent* camera = writeCamera(world, id);
    if (camera == nullptr)
        return false;
    f32 degrees = 0.0f;
    // The open interval matters at both ends: zero collapses the projection and
    // 180 sends the near plane to infinity. Both render nothing, silently.
    if (!takePositive(value, degrees) || degrees >= 180.0f)
        return false;
    camera->fieldOfView = degrees;
    return true;
}

Value getCameraNearPlane(const scene::World& world, core::InstanceId id)
{
    const scene::CameraComponent* camera = readCamera(world, id);
    return camera == nullptr ? Value{} : Value{static_cast<f64>(camera->nearPlane)};
}

bool setCameraNearPlane(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::CameraComponent* camera = writeCamera(world, id);
    if (camera == nullptr)
        return false;
    f32 distance = 0.0f;
    if (!takePositive(value, distance))
        return false;
    camera->nearPlane = distance;
    return true;
}

Value getCameraFarPlane(const scene::World& world, core::InstanceId id)
{
    const scene::CameraComponent* camera = readCamera(world, id);
    return camera == nullptr ? Value{} : Value{static_cast<f64>(camera->farPlane)};
}

bool setCameraFarPlane(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::CameraComponent* camera = writeCamera(world, id);
    if (camera == nullptr)
        return false;
    f32 distance = 0.0f;
    if (!takePositive(value, distance))
        return false;
    camera->farPlane = distance;
    return true;
}

Value getCameraProjection(const scene::World& world, core::InstanceId id)
{
    const scene::CameraComponent* camera = readCamera(world, id);
    return camera == nullptr ? Value{} : Value{scene::EnumValue{generated::CameraProjectionEnumId, camera->projection}};
}

bool setCameraProjection(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::CameraComponent* camera = writeCamera(world, id);
    if (camera == nullptr || item == nullptr || item->enumId != generated::CameraProjectionEnumId || item->value < 0 ||
        item->value > 1)
        return false;
    camera->projection = item->value;
    return true;
}

Value getCameraClipPlane(const scene::World& world, core::InstanceId id)
{
    const scene::CameraComponent* camera = readCamera(world, id);
    return camera == nullptr ? Value{} : Value{camera->clipPlane};
}

bool setCameraClipPlane(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* cframe = std::get_if<core::CFrameD>(&value);
    scene::CameraComponent* camera = writeCamera(world, id);
    if (cframe == nullptr || camera == nullptr)
        return false;
    camera->clipPlane = *cframe;
    return true;
}

Value getCameraClipPlaneEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::CameraComponent* camera = readCamera(world, id);
    return camera == nullptr ? Value{} : Value{camera->clipPlaneOn};
}

bool setCameraClipPlaneEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::CameraComponent* camera = writeCamera(world, id);
    if (flag == nullptr || camera == nullptr)
        return false;
    camera->clipPlaneOn = *flag;
    return true;
}

Value getCameraOrthographicSize(const scene::World& world, core::InstanceId id)
{
    const scene::CameraComponent* camera = readCamera(world, id);
    return camera == nullptr ? Value{} : Value{static_cast<f64>(camera->orthographicSize)};
}

bool setCameraOrthographicSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::CameraComponent* camera = writeCamera(world, id);
    f32 size = 0.0f;
    if (camera == nullptr || !takePositive(value, size))
        return false;
    camera->orthographicSize = size;
    return true;
}

void attachCameraComponents(scene::World& world, core::InstanceId id)
{
    world.cameras().add(id, scene::CameraComponent{});
}

void detachCameraComponents(scene::World& world, core::InstanceId id)
{
    world.cameras().remove(id);
}

// --- CameraTexture (ADR 0107) -----------------------------------------------------

void attachCameraTextureComponents(scene::World& world, core::InstanceId id)
{
    world.cameraTextures().add(id, scene::CameraTextureComponent{});
}

void detachCameraTextureComponents(scene::World& world, core::InstanceId id)
{
    world.cameraTextures().remove(id);
}

Value getCameraTextureCamera(const scene::World& world, core::InstanceId id)
{
    const scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    if (view == nullptr || !world.alive(view->camera))
        return Value{};
    return Value{view->camera};
}

bool setCameraTextureCamera(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    if (view == nullptr)
        return false;
    if (const auto* reference = std::get_if<core::InstanceId>(&value); reference != nullptr) {
        // Typed `Camera?`; this is the runtime half of that, as on
        // `Workspace.CurrentCamera`.
        const scene::ClassId cameraClass = world.classes().findId(world.atoms().lookup("Camera"));
        if (cameraClass == scene::InvalidClass || !world.isA(*reference, cameraClass))
            return false;
        view->camera = *reference;
        return true;
    }
    if (scene::valueType(value) != scene::ValueType::Nil)
        return false;
    view->camera = core::InstanceId{};
    return true;
}

Value getCameraTextureViewName(const scene::World& world, core::InstanceId id)
{
    const scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    return view == nullptr ? Value{} : Value{std::string(world.atoms().text(view->viewName))};
}

bool setCameraTextureViewName(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    if (text == nullptr || view == nullptr)
        return false;
    view->viewName = world.atoms().intern(*text);
    return true;
}

Value getCameraTextureResolution(const scene::World& world, core::InstanceId id)
{
    const scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    return view == nullptr ? Value{} : Value{view->resolution};
}

bool setCameraTextureResolution(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* size = std::get_if<core::Vec2>(&value);
    scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    if (size == nullptr || view == nullptr || !(size->x >= 1.0f) || !(size->y >= 1.0f) || !std::isfinite(size->x) ||
        !std::isfinite(size->y))
        return false;
    view->resolution = *size;
    return true;
}

Value getCameraTextureEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    return view == nullptr ? Value{} : Value{view->enabled};
}

bool setCameraTextureEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    if (flag == nullptr || view == nullptr)
        return false;
    view->enabled = *flag;
    return true;
}

Value getCameraTextureUpdateInterval(const scene::World& world, core::InstanceId id)
{
    const scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    return view == nullptr ? Value{} : Value{static_cast<f64>(view->updateInterval)};
}

bool setCameraTextureUpdateInterval(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* number = std::get_if<f64>(&value);
    scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    // A whole number of frames, one or more; past a thousand is a picture that
    // never changes, which `Enabled = false` says better.
    if (number == nullptr || view == nullptr || !std::isfinite(*number) || *number < 1.0 || *number > 1000.0 ||
        std::floor(*number) != *number)
        return false;
    view->updateInterval = static_cast<core::u32>(*number);
    return true;
}

Value getCameraTextureQuality(const scene::World& world, core::InstanceId id)
{
    const scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    return view == nullptr ? Value{} : Value{scene::EnumValue{generated::ViewQualityEnumId, view->quality}};
}

bool setCameraTextureQuality(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::CameraTextureComponent* view = world.cameraTextures().find(id);
    if (view == nullptr || item == nullptr || item->enumId != generated::ViewQualityEnumId || item->value < 0 ||
        item->value > 1)
        return false;
    view->quality = item->value;
    return true;
}

// --- SubWorld (ADR 0107 §3) -------------------------------------------------------

void attachSubWorldComponents(scene::World& world, core::InstanceId id)
{
    world.subWorlds().add(id, scene::SubWorldComponent{});
}

void detachSubWorldComponents(scene::World& world, core::InstanceId id)
{
    world.subWorlds().remove(id);
}

Value getSubWorldScene(const scene::World& world, core::InstanceId id)
{
    const scene::SubWorldComponent* self = world.subWorlds().find(id);
    return self == nullptr ? Value{} : Value{std::string(world.atoms().text(self->scene))};
}

bool setSubWorldScene(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::SubWorldComponent* self = world.subWorlds().find(id);
    if (text == nullptr || self == nullptr)
        return false;
    self->scene = world.atoms().intern(*text);
    return true;
}

Value getSubWorldViewName(const scene::World& world, core::InstanceId id)
{
    const scene::SubWorldComponent* self = world.subWorlds().find(id);
    return self == nullptr ? Value{} : Value{std::string(world.atoms().text(self->viewName))};
}

bool setSubWorldViewName(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::SubWorldComponent* self = world.subWorlds().find(id);
    if (text == nullptr || self == nullptr)
        return false;
    self->viewName = world.atoms().intern(*text);
    return true;
}

Value getSubWorldResolution(const scene::World& world, core::InstanceId id)
{
    const scene::SubWorldComponent* self = world.subWorlds().find(id);
    return self == nullptr ? Value{} : Value{self->resolution};
}

bool setSubWorldResolution(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* size = std::get_if<core::Vec2>(&value);
    scene::SubWorldComponent* self = world.subWorlds().find(id);
    if (size == nullptr || self == nullptr || !(size->x >= 1.0f) || !(size->y >= 1.0f) || !std::isfinite(size->x) ||
        !std::isfinite(size->y))
        return false;
    self->resolution = *size;
    return true;
}

Value getSubWorldUpdateInterval(const scene::World& world, core::InstanceId id)
{
    const scene::SubWorldComponent* self = world.subWorlds().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->updateInterval)};
}

bool setSubWorldUpdateInterval(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* number = std::get_if<f64>(&value);
    scene::SubWorldComponent* self = world.subWorlds().find(id);
    // As `CameraTexture.UpdateInterval`.
    if (number == nullptr || self == nullptr || !std::isfinite(*number) || *number < 1.0 || *number > 1000.0 ||
        std::floor(*number) != *number)
        return false;
    self->updateInterval = static_cast<core::u32>(*number);
    return true;
}

Value getSubWorldQuality(const scene::World& world, core::InstanceId id)
{
    const scene::SubWorldComponent* self = world.subWorlds().find(id);
    return self == nullptr ? Value{} : Value{scene::EnumValue{generated::ViewQualityEnumId, self->quality}};
}

bool setSubWorldQuality(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::SubWorldComponent* self = world.subWorlds().find(id);
    if (self == nullptr || item == nullptr || item->enumId != generated::ViewQualityEnumId || item->value < 0 ||
        item->value > 1)
        return false;
    self->quality = item->value;
    return true;
}

Value getSubWorldRunning(const scene::World& world, core::InstanceId id)
{
    const scene::SubWorldComponent* self = world.subWorlds().find(id);
    return self == nullptr ? Value{} : Value{self->running};
}

bool setSubWorldRunning(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::SubWorldComponent* self = world.subWorlds().find(id);
    if (flag == nullptr || self == nullptr)
        return false;
    self->running = *flag;
    return true;
}

// --- Decal (F2) -------------------------------------------------------------------

void attachDecalComponents(scene::World& world, core::InstanceId id)
{
    world.decals().add(id, scene::DecalComponent{});
}

void detachDecalComponents(scene::World& world, core::InstanceId id)
{
    world.decals().remove(id);
}

Value getDecalCFrame(const scene::World& world, core::InstanceId id)
{
    const scene::DecalComponent* decal = world.decals().find(id);
    return decal == nullptr ? Value{} : Value{decal->cframe};
}

bool setDecalCFrame(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* cframe = std::get_if<core::CFrameD>(&value);
    scene::DecalComponent* decal = world.decals().find(id);
    if (cframe == nullptr || decal == nullptr)
        return false;
    decal->cframe = *cframe;
    return true;
}

Value getDecalSize(const scene::World& world, core::InstanceId id)
{
    const scene::DecalComponent* decal = world.decals().find(id);
    return decal == nullptr ? Value{} : Value{decal->size};
}

bool setDecalSize(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* size = std::get_if<core::Vec3>(&value);
    scene::DecalComponent* decal = world.decals().find(id);
    // A box with no extent paints nothing and divides by zero on the way.
    if (size == nullptr || decal == nullptr || !(size->x > 0.0f) || !(size->y > 0.0f) || !(size->z > 0.0f) ||
        !std::isfinite(size->x) || !std::isfinite(size->y) || !std::isfinite(size->z))
        return false;
    decal->size = *size;
    return true;
}

Value getDecalTexture(const scene::World& world, core::InstanceId id)
{
    const scene::DecalComponent* decal = world.decals().find(id);
    return decal == nullptr ? Value{} : Value{std::string(world.atoms().text(decal->texture))};
}

bool setDecalTexture(scene::World& world, core::InstanceId id, const Value& value)
{
    // Not `setMap`, which checks for a material component: this is a decal's.
    const auto* text = std::get_if<std::string>(&value);
    scene::DecalComponent* decal = world.decals().find(id);
    if (text == nullptr || decal == nullptr)
        return false;
    decal->texture = world.atoms().intern(*text);
    return true;
}

Value getDecalColor(const scene::World& world, core::InstanceId id)
{
    const scene::DecalComponent* decal = world.decals().find(id);
    return decal == nullptr ? Value{} : Value{decal->color};
}

bool setDecalColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::DecalComponent* decal = world.decals().find(id);
    if (color == nullptr || decal == nullptr)
        return false;
    decal->color = *color;
    return true;
}

Value getDecalTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::DecalComponent* decal = world.decals().find(id);
    return decal == nullptr ? Value{} : Value{static_cast<f64>(decal->transparency)};
}

bool setDecalTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::DecalComponent* decal = world.decals().find(id);
    f32 next = 0.0f;
    if (decal == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    decal->transparency = next;
    return true;
}

Value getDecalBlendMode(const scene::World& world, core::InstanceId id)
{
    const scene::DecalComponent* decal = world.decals().find(id);
    return decal == nullptr ? Value{} : Value{scene::EnumValue{generated::DecalBlendModeEnumId, decal->blendMode}};
}

bool setDecalBlendMode(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::DecalComponent* decal = world.decals().find(id);
    if (item == nullptr || decal == nullptr || item->enumId != generated::DecalBlendModeEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    decal->blendMode = item->value;
    return true;
}

Value getDecalEmissive(const scene::World& world, core::InstanceId id)
{
    const scene::DecalComponent* decal = world.decals().find(id);
    return decal == nullptr ? Value{} : Value{static_cast<f64>(decal->emissive)};
}

bool setDecalEmissive(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::DecalComponent* decal = world.decals().find(id);
    f32 next = 0.0f;
    if (decal == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    decal->emissive = next;
    return true;
}

// --- ParticleEmitter (F2) ------------------------------------------------------
//
// Every number is refused when negative, and the three that are fractions --
// the transparencies and the light emission -- above one as well: a particle
// cannot be more than invisible, and emission is a blend between two modes.

void attachParticleEmitterComponents(scene::World& world, core::InstanceId id)
{
    world.particleEmitters().add(id, scene::ParticleEmitterComponent{});
}

void detachParticleEmitterComponents(scene::World& world, core::InstanceId id)
{
    world.particleEmitters().remove(id);
}

Value getParticleEmitterEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{emitter->enabled};
}

bool setParticleEmitterEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<bool>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (next == nullptr || emitter == nullptr)
        return false;
    emitter->enabled = *next;
    return true;
}

Value getParticleEmitterAcceleration(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{emitter->acceleration};
}

bool setParticleEmitterAcceleration(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::Vec3>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (next == nullptr || emitter == nullptr || !std::isfinite(next->x) || !std::isfinite(next->y) ||
        !std::isfinite(next->z))
        return false;
    emitter->acceleration = *next;
    return true;
}

Value getParticleEmitterShape(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{scene::EnumValue{generated::ParticleShapeEnumId, emitter->shape}};
}

bool setParticleEmitterShape(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (item == nullptr || emitter == nullptr || item->enumId != generated::ParticleShapeEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    emitter->shape = item->value;
    return true;
}

// --- A particle's picture, its frames, its turn and its curves (ADR 0160) ------

Value getParticleEmitterTexture(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{std::string(world.atoms().text(emitter->texture))};
}

bool setParticleEmitterTexture(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (text == nullptr || emitter == nullptr)
        return false;
    emitter->texture = world.atoms().intern(*text);
    return true;
}

Value getParticleEmitterFlipbookColumns(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->flipbookColumns)};
}

bool setParticleEmitterFlipbookColumns(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    // One to sixteen frames a side (ADR 0160): a grid finer than that is a
    // picture nobody could draw the frames of.
    if (emitter == nullptr || !takeFinite(value, next) || next < 1.0f || next > 16.0f || next != std::floor(next))
        return false;
    emitter->flipbookColumns = static_cast<core::i32>(next);
    return true;
}

Value getParticleEmitterFlipbookRows(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->flipbookRows)};
}

bool setParticleEmitterFlipbookRows(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    // One to sixteen frames a side (ADR 0160): a grid finer than that is a
    // picture nobody could draw the frames of.
    if (emitter == nullptr || !takeFinite(value, next) || next < 1.0f || next > 16.0f || next != std::floor(next))
        return false;
    emitter->flipbookRows = static_cast<core::i32>(next);
    return true;
}

Value getParticleEmitterFlipbookFramerate(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->flipbookFramerate)};
}

bool setParticleEmitterFlipbookFramerate(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->flipbookFramerate = next;
    return true;
}

Value getParticleEmitterFlipbookMode(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{}
                              : Value{scene::EnumValue{generated::ParticleFlipbookModeEnumId, emitter->flipbookMode}};
}

bool setParticleEmitterFlipbookMode(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (item == nullptr || emitter == nullptr || item->enumId != generated::ParticleFlipbookModeEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    emitter->flipbookMode = item->value;
    return true;
}

Value getParticleEmitterRotation(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->rotation)};
}

bool setParticleEmitterRotation(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next))
        return false;
    emitter->rotation = next;
    return true;
}

Value getParticleEmitterRotationSpread(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->rotationSpread)};
}

bool setParticleEmitterRotationSpread(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->rotationSpread = next;
    return true;
}

Value getParticleEmitterRotationSpeed(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->rotationSpeed)};
}

bool setParticleEmitterRotationSpeed(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next))
        return false;
    emitter->rotationSpeed = next;
    return true;
}

Value getParticleEmitterRotationSpeedSpread(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->rotationSpeedSpread)};
}

bool setParticleEmitterRotationSpeedSpread(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->rotationSpeedSpread = next;
    return true;
}

Value getParticleEmitterColorOverLife(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{emitter->colorOverLife};
}

bool setParticleEmitterColorOverLife(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::ColorSequence>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (next == nullptr || emitter == nullptr || !core::validSequence(next->keypoints))
        return false;
    emitter->colorOverLife = *next;
    return true;
}

Value getParticleEmitterSizeOverLife(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{emitter->sizeOverLife};
}

bool setParticleEmitterSizeOverLife(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::NumberSequence>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (next == nullptr || emitter == nullptr || !core::validSequence(next->keypoints))
        return false;
    emitter->sizeOverLife = *next;
    return true;
}

Value getParticleEmitterTransparencyOverLife(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{emitter->transparencyOverLife};
}

bool setParticleEmitterTransparencyOverLife(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::NumberSequence>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (next == nullptr || emitter == nullptr || !core::validSequence(next->keypoints))
        return false;
    emitter->transparencyOverLife = *next;
    return true;
}

// --- What a particle meets, and where it is simulated (ADR 0160) --------------

Value getParticleEmitterCollision(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{}
                              : Value{scene::EnumValue{generated::ParticleCollisionEnumId, emitter->collision}};
}

bool setParticleEmitterCollision(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (item == nullptr || emitter == nullptr || item->enumId != generated::ParticleCollisionEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    emitter->collision = item->value;
    return true;
}

Value getParticleEmitterCollisionResponse(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr
               ? Value{}
               : Value{scene::EnumValue{generated::ParticleCollisionResponseEnumId, emitter->collisionResponse}};
}

bool setParticleEmitterCollisionResponse(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (item == nullptr || emitter == nullptr || item->enumId != generated::ParticleCollisionResponseEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    emitter->collisionResponse = item->value;
    return true;
}

Value getParticleEmitterBounce(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->bounce)};
}

bool setParticleEmitterBounce(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    emitter->bounce = next;
    return true;
}

Value getParticleEmitterFriction(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->friction)};
}

bool setParticleEmitterFriction(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    emitter->friction = next;
    return true;
}

Value getParticleEmitterCollisionRadius(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->collisionRadius)};
}

bool setParticleEmitterCollisionRadius(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->collisionRadius = next;
    return true;
}

Value getParticleEmitterSimulation(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{}
                              : Value{scene::EnumValue{generated::ParticleSimulationEnumId, emitter->simulation}};
}

bool setParticleEmitterSimulation(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (item == nullptr || emitter == nullptr || item->enumId != generated::ParticleSimulationEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    emitter->simulation = item->value;
    return true;
}

Value getParticleEmitterRate(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->rate)};
}

bool setParticleEmitterRate(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->rate = next;
    return true;
}

Value getParticleEmitterLifetime(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->lifetime)};
}

bool setParticleEmitterLifetime(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->lifetime = next;
    return true;
}

Value getParticleEmitterSpeed(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->speed)};
}

bool setParticleEmitterSpeed(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->speed = next;
    return true;
}

Value getParticleEmitterSpreadAngle(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->spreadAngle)};
}

bool setParticleEmitterSpreadAngle(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f || next > 180.0f)
        return false;
    emitter->spreadAngle = next;
    return true;
}

Value getParticleEmitterDrag(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->drag)};
}

bool setParticleEmitterDrag(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->drag = next;
    return true;
}

Value getParticleEmitterWindAffectsDrift(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{emitter->windAffectsDrift};
}

bool setParticleEmitterWindAffectsDrift(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<bool>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (next == nullptr || emitter == nullptr)
        return false;
    emitter->windAffectsDrift = *next;
    return true;
}

Value getParticleEmitterColor(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{emitter->color};
}

bool setParticleEmitterColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::Color3>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (next == nullptr || emitter == nullptr)
        return false;
    emitter->color = *next;
    return true;
}

Value getParticleEmitterColorEnd(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{emitter->colorEnd};
}

bool setParticleEmitterColorEnd(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::Color3>(&value);
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    if (next == nullptr || emitter == nullptr)
        return false;
    emitter->colorEnd = *next;
    return true;
}

Value getParticleEmitterSize(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->size)};
}

bool setParticleEmitterSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->size = next;
    return true;
}

Value getParticleEmitterSizeEnd(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->sizeEnd)};
}

bool setParticleEmitterSizeEnd(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->sizeEnd = next;
    return true;
}

Value getParticleEmitterTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->transparency)};
}

bool setParticleEmitterTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    emitter->transparency = next;
    return true;
}

Value getParticleEmitterTransparencyEnd(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->transparencyEnd)};
}

bool setParticleEmitterTransparencyEnd(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    emitter->transparencyEnd = next;
    return true;
}

Value getParticleEmitterLightEmission(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->lightEmission)};
}

bool setParticleEmitterLightEmission(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    emitter->lightEmission = next;
    return true;
}

Value getParticleEmitterBrightness(const scene::World& world, core::InstanceId id)
{
    const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    return emitter == nullptr ? Value{} : Value{static_cast<f64>(emitter->brightness)};
}

bool setParticleEmitterBrightness(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
    f32 next = 0.0f;
    if (emitter == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    emitter->brightness = next;
    return true;
}

// --- PointLight -------------------------------------------------------------

Value getPointLightColor(const scene::World& world, core::InstanceId id)
{
    const scene::PointLightComponent* light = readPointLight(world, id);
    return light == nullptr ? Value{} : Value{light->color};
}

bool setPointLightColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::PointLightComponent* light = writePointLight(world, id);
    if (color == nullptr || light == nullptr)
        return false;
    light->color = *color;
    return true;
}

Value getPointLightBrightness(const scene::World& world, core::InstanceId id)
{
    const scene::PointLightComponent* light = readPointLight(world, id);
    return light == nullptr ? Value{} : Value{static_cast<f64>(light->brightness)};
}

bool setPointLightBrightness(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::PointLightComponent* light = writePointLight(world, id);
    if (light == nullptr)
        return false;
    // Zero is legal and is how a light is turned off without destroying it;
    // negative is not, because it would subtract light.
    f32 brightness = 0.0f;
    if (!takeFinite(value, brightness) || brightness < 0.0f)
        return false;
    light->brightness = brightness;
    return true;
}

Value getPointLightRange(const scene::World& world, core::InstanceId id)
{
    const scene::PointLightComponent* light = readPointLight(world, id);
    return light == nullptr ? Value{} : Value{static_cast<f64>(light->range)};
}

bool setPointLightRange(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::PointLightComponent* light = writePointLight(world, id);
    if (light == nullptr)
        return false;
    f32 range = 0.0f;
    if (!takeFinite(value, range) || range < 0.0f)
        return false;
    light->range = range;
    return true;
}

Value getPointLightEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::PointLightComponent* light = readPointLight(world, id);
    return light == nullptr ? Value{} : Value{light->enabled};
}

bool setPointLightEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::PointLightComponent* light = writePointLight(world, id);
    if (flag == nullptr || light == nullptr)
        return false;
    light->enabled = *flag;
    return true;
}

Value getPointLightShadows(const scene::World& world, core::InstanceId id)
{
    const scene::PointLightComponent* light = readPointLight(world, id);
    return light == nullptr ? Value{} : Value{light->shadows};
}

bool setPointLightShadows(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::PointLightComponent* light = writePointLight(world, id);
    if (flag == nullptr || light == nullptr)
        return false;
    // Stored and reported faithfully while this release casts shadows from the
    // sun alone. A property that round-trips is honest; one that silently reads
    // back false would be the lie the IDL's own header warns about.
    light->shadows = *flag;
    return true;
}

void attachPointLightComponents(scene::World& world, core::InstanceId id)
{
    world.pointLights().add(id, scene::PointLightComponent{});
}

void detachPointLightComponents(scene::World& world, core::InstanceId id)
{
    world.pointLights().remove(id);
}

// --- SpotLight --------------------------------------------------------------

Value getSpotLightColor(const scene::World& world, core::InstanceId id)
{
    const scene::SpotLightComponent* light = readSpotLight(world, id);
    return light == nullptr ? Value{} : Value{light->color};
}

bool setSpotLightColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::SpotLightComponent* light = writeSpotLight(world, id);
    if (color == nullptr || light == nullptr)
        return false;
    light->color = *color;
    return true;
}

Value getSpotLightBrightness(const scene::World& world, core::InstanceId id)
{
    const scene::SpotLightComponent* light = readSpotLight(world, id);
    return light == nullptr ? Value{} : Value{static_cast<f64>(light->brightness)};
}

bool setSpotLightBrightness(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SpotLightComponent* light = writeSpotLight(world, id);
    if (light == nullptr)
        return false;
    f32 brightness = 0.0f;
    if (!takeFinite(value, brightness) || brightness < 0.0f)
        return false;
    light->brightness = brightness;
    return true;
}

Value getPointLightCFrame(const scene::World& world, core::InstanceId id)
{
    const scene::PointLightComponent* light = readPointLight(world, id);
    return light == nullptr ? Value{} : Value{light->cframe};
}

bool setPointLightCFrame(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* cframe = std::get_if<core::CFrameD>(&value);
    scene::PointLightComponent* light = writePointLight(world, id);
    if (cframe == nullptr || light == nullptr)
        return false;
    light->cframe = *cframe;
    return true;
}

Value getSpotLightCFrame(const scene::World& world, core::InstanceId id)
{
    const scene::SpotLightComponent* light = readSpotLight(world, id);
    return light == nullptr ? Value{} : Value{light->cframe};
}

bool setSpotLightCFrame(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* cframe = std::get_if<core::CFrameD>(&value);
    scene::SpotLightComponent* light = writeSpotLight(world, id);
    if (cframe == nullptr || light == nullptr)
        return false;
    light->cframe = *cframe;
    return true;
}

Value getSpotLightRange(const scene::World& world, core::InstanceId id)
{
    const scene::SpotLightComponent* light = readSpotLight(world, id);
    return light == nullptr ? Value{} : Value{static_cast<f64>(light->range)};
}

bool setSpotLightRange(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SpotLightComponent* light = writeSpotLight(world, id);
    if (light == nullptr)
        return false;
    f32 range = 0.0f;
    if (!takeFinite(value, range) || range < 0.0f)
        return false;
    light->range = range;
    return true;
}

Value getSpotLightAngle(const scene::World& world, core::InstanceId id)
{
    const scene::SpotLightComponent* light = readSpotLight(world, id);
    return light == nullptr ? Value{} : Value{static_cast<f64>(light->angle)};
}

bool setSpotLightAngle(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SpotLightComponent* light = writeSpotLight(world, id);
    if (light == nullptr)
        return false;
    f32 angle = 0.0f;
    // The full cone width, so 180 is a hemisphere and anything at or beyond it
    // is no longer a cone.
    if (!takePositive(value, angle) || angle >= 180.0f)
        return false;
    light->angle = angle;
    return true;
}

Value getSpotLightEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::SpotLightComponent* light = readSpotLight(world, id);
    return light == nullptr ? Value{} : Value{light->enabled};
}

bool setSpotLightEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::SpotLightComponent* light = writeSpotLight(world, id);
    if (flag == nullptr || light == nullptr)
        return false;
    light->enabled = *flag;
    return true;
}

Value getSpotLightShadows(const scene::World& world, core::InstanceId id)
{
    const scene::SpotLightComponent* light = readSpotLight(world, id);
    return light == nullptr ? Value{} : Value{light->shadows};
}

bool setSpotLightShadows(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::SpotLightComponent* light = writeSpotLight(world, id);
    if (flag == nullptr || light == nullptr)
        return false;
    light->shadows = *flag;
    return true;
}

void attachSpotLightComponents(scene::World& world, core::InstanceId id)
{
    world.spotLights().add(id, scene::SpotLightComponent{});
}

void detachSpotLightComponents(scene::World& world, core::InstanceId id)
{
    world.spotLights().remove(id);
}

// --- Lighting ---------------------------------------------------------------

Value getLightingClockTime(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{static_cast<f64>(lighting->clockTime)};
}

bool setLightingClockTime(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (lighting == nullptr)
        return false;
    f32 hours = 0.0f;
    if (!takeFinite(value, hours))
        return false;
    // Wraps rather than clamps, so `ClockTime += dt` never has to check. A
    // clamp would make midnight a wall the sun stops against.
    hours = std::fmod(hours, 24.0f);
    lighting->clockTime = hours < 0.0f ? hours + 24.0f : hours;
    return true;
}

Value getLightingGeographicLatitude(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{static_cast<f64>(lighting->geographicLatitude)};
}

bool setLightingGeographicLatitude(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (lighting == nullptr)
        return false;
    f32 degrees = 0.0f;
    if (!takeFinite(value, degrees) || degrees < -90.0f || degrees > 90.0f)
        return false;
    lighting->geographicLatitude = degrees;
    return true;
}

Value getLightingAmbient(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{lighting->ambient};
}

bool setLightingAmbient(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (color == nullptr || lighting == nullptr)
        return false;
    lighting->ambient = *color;
    return true;
}

Value getLightingOutdoorAmbient(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{lighting->outdoorAmbient};
}

bool setLightingOutdoorAmbient(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (color == nullptr || lighting == nullptr)
        return false;
    lighting->outdoorAmbient = *color;
    return true;
}

Value getLightingBrightness(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{static_cast<f64>(lighting->brightness)};
}

bool setLightingBrightness(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (lighting == nullptr)
        return false;
    f32 brightness = 0.0f;
    if (!takeFinite(value, brightness) || brightness < 0.0f)
        return false;
    lighting->brightness = brightness;
    return true;
}

Value getLightingFogColor(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{lighting->fogColor};
}

bool setLightingFogColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (color == nullptr || lighting == nullptr)
        return false;
    lighting->fogColor = *color;
    return true;
}

Value getLightingFogStart(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{static_cast<f64>(lighting->fogStart)};
}

bool setLightingFogStart(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (lighting == nullptr)
        return false;
    f32 distance = 0.0f;
    if (!takeFinite(value, distance) || distance < 0.0f)
        return false;
    lighting->fogStart = distance;
    return true;
}

Value getLightingFogEnd(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{static_cast<f64>(lighting->fogEnd)};
}

bool setLightingFogEnd(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (lighting == nullptr)
        return false;
    f32 distance = 0.0f;
    // Not required to exceed `fogStart`: equal or below means no fog at all,
    // which is the documented way to switch it off. Rejecting the ordering here
    // would make turning fog off an error.
    if (!takeFinite(value, distance) || distance < 0.0f)
        return false;
    lighting->fogEnd = distance;
    return true;
}

Value getLightingExposureCompensation(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{static_cast<f64>(lighting->exposureCompensation)};
}

bool setLightingExposureCompensation(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LightingComponent* lighting = writeLighting(world, id);
    if (lighting == nullptr)
        return false;
    f32 stops = 0.0f;
    // Finite, and otherwise unbounded: a scene that deliberately blows out or
    // crushes its exposure is making a picture, not a mistake.
    if (!takeFinite(value, stops))
        return false;
    lighting->exposureCompensation = stops;
    return true;
}

Value getLightingExposureMin(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{static_cast<f64>(lighting->exposureMin)};
}

bool setLightingExposureMin(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LightingComponent* lighting = writeLighting(world, id);
    f32 stops = 0.0f;
    if (lighting == nullptr || !takeFinite(value, stops))
        return false;
    lighting->exposureMin = stops;
    return true;
}

Value getLightingExposureMax(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    return lighting == nullptr ? Value{} : Value{static_cast<f64>(lighting->exposureMax)};
}

bool setLightingExposureMax(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LightingComponent* lighting = writeLighting(world, id);
    f32 stops = 0.0f;
    if (lighting == nullptr || !takeFinite(value, stops))
        return false;
    lighting->exposureMax = stops;
    return true;
}

Value getLightingSunDirection(const scene::World& world, core::InstanceId id)
{
    const scene::LightingComponent* lighting = readLighting(world, id);
    if (lighting == nullptr)
        return Value{};
    return Value{sunDirection(lighting->clockTime, lighting->geographicLatitude)};
}

void attachLightingComponents(scene::World& world, core::InstanceId id)
{
    world.lighting().add(id, scene::LightingComponent{});
}

void detachLightingComponents(scene::World& world, core::InstanceId id)
{
    world.lighting().remove(id);
}

// --- The look of a world (ADR 0096) ---------------------------------------------
//
// Eight classes whose properties are all one of a handful of shapes, so the
// shapes are written once below and each accessor is one line naming its field
// and its range. Every range is CLOSED and refused rather than clamped, for the
// reason the lights' are: a value that silently became another reads back as
// something nobody wrote.

namespace {

constexpr f32 kUnbounded = std::numeric_limits<f32>::infinity();

template <typename Component>
[[nodiscard]] Value readNumber(const scene::ComponentPool<Component>& pool, core::InstanceId id, f32 Component::*field)
{
    const Component* component = pool.find(id);
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->*field)};
}

template <typename Component>
[[nodiscard]] bool writeNumber(scene::ComponentPool<Component>& pool, core::InstanceId id, const Value& value,
                               f32 Component::*field, f32 low, f32 high)
{
    Component* component = pool.find(id);
    f32 next = 0.0f;
    if (component == nullptr || !takeFinite(value, next) || next < low || next > high)
        return false;
    component->*field = next;
    return true;
}

template <typename Component, typename Field>
[[nodiscard]] Value readValue(const scene::ComponentPool<Component>& pool, core::InstanceId id, Field Component::*field)
{
    const Component* component = pool.find(id);
    return component == nullptr ? Value{} : Value{component->*field};
}

// A boolean, a colour or a vector: any value of the right type is legal, except
// a vector with a non-finite component -- an orientation of NaN degrees turns
// every direction the sky is sampled in into NaN.
template <typename Component, typename Field>
[[nodiscard]] bool writeValue(scene::ComponentPool<Component>& pool, core::InstanceId id, const Value& value,
                              Field Component::*field)
{
    const auto* next = std::get_if<Field>(&value);
    Component* component = pool.find(id);
    if (next == nullptr || component == nullptr)
        return false;
    if constexpr (std::is_same_v<Field, core::Vec3>) {
        if (!std::isfinite(next->x) || !std::isfinite(next->y) || !std::isfinite(next->z))
            return false;
    }
    component->*field = *next;
    return true;
}

template <typename Component>
[[nodiscard]] Value readContent(const scene::World& world, const scene::ComponentPool<Component>& pool,
                                core::InstanceId id, core::NameAtom Component::*field)
{
    const Component* component = pool.find(id);
    return component == nullptr ? Value{} : Value{std::string(world.atoms().text(component->*field))};
}

// Interned, not resolved, as `MeshPart.MeshContent` is: whether the image loads
// is the renderer's question, and a later one.
template <typename Component>
[[nodiscard]] bool writeContent(scene::World& world, scene::ComponentPool<Component>& pool, core::InstanceId id,
                                const Value& value, core::NameAtom Component::*field)
{
    const auto* text = std::get_if<std::string>(&value);
    Component* component = pool.find(id);
    if (text == nullptr || component == nullptr)
        return false;
    component->*field = world.atoms().intern(*text);
    return true;
}

} // namespace

// PostEffect

void attachPostEffectComponents(scene::World& world, core::InstanceId id)
{
    world.postEffects().add(id, scene::PostEffectComponent{});
}

void detachPostEffectComponents(scene::World& world, core::InstanceId id)
{
    world.postEffects().remove(id);
}

Value getPostEffectEnabled(const scene::World& world, core::InstanceId id)
{
    return readValue(world.postEffects(), id, &scene::PostEffectComponent::enabled);
}

bool setPostEffectEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.postEffects(), id, value, &scene::PostEffectComponent::enabled);
}

// BloomEffect

void attachBloomEffectComponents(scene::World& world, core::InstanceId id)
{
    world.bloomEffects().add(id, scene::BloomEffectComponent{});
}

void detachBloomEffectComponents(scene::World& world, core::InstanceId id)
{
    world.bloomEffects().remove(id);
}

Value getBloomEffectIntensity(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.bloomEffects(), id, &scene::BloomEffectComponent::intensity);
}

bool setBloomEffectIntensity(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.bloomEffects(), id, value, &scene::BloomEffectComponent::intensity, 0.0f, kUnbounded);
}

Value getBloomEffectSize(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.bloomEffects(), id, &scene::BloomEffectComponent::size);
}

bool setBloomEffectSize(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.bloomEffects(), id, value, &scene::BloomEffectComponent::size, 0.0f, 56.0f);
}

Value getBloomEffectThreshold(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.bloomEffects(), id, &scene::BloomEffectComponent::threshold);
}

bool setBloomEffectThreshold(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.bloomEffects(), id, value, &scene::BloomEffectComponent::threshold, 0.0f, kUnbounded);
}

// ColorCorrectionEffect

void attachColorCorrectionEffectComponents(scene::World& world, core::InstanceId id)
{
    world.colorCorrectionEffects().add(id, scene::ColorCorrectionEffectComponent{});
}

void detachColorCorrectionEffectComponents(scene::World& world, core::InstanceId id)
{
    world.colorCorrectionEffects().remove(id);
}

Value getColorCorrectionEffectBrightness(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.colorCorrectionEffects(), id, &scene::ColorCorrectionEffectComponent::brightness);
}

bool setColorCorrectionEffectBrightness(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.colorCorrectionEffects(), id, value, &scene::ColorCorrectionEffectComponent::brightness,
                       -1.0f, 1.0f);
}

Value getColorCorrectionEffectContrast(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.colorCorrectionEffects(), id, &scene::ColorCorrectionEffectComponent::contrast);
}

bool setColorCorrectionEffectContrast(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.colorCorrectionEffects(), id, value, &scene::ColorCorrectionEffectComponent::contrast,
                       -1.0f, 1.0f);
}

Value getColorCorrectionEffectSaturation(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.colorCorrectionEffects(), id, &scene::ColorCorrectionEffectComponent::saturation);
}

bool setColorCorrectionEffectSaturation(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.colorCorrectionEffects(), id, value, &scene::ColorCorrectionEffectComponent::saturation,
                       -1.0f, 1.0f);
}

Value getColorCorrectionEffectTintColor(const scene::World& world, core::InstanceId id)
{
    return readValue(world.colorCorrectionEffects(), id, &scene::ColorCorrectionEffectComponent::tintColor);
}

bool setColorCorrectionEffectTintColor(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.colorCorrectionEffects(), id, value, &scene::ColorCorrectionEffectComponent::tintColor);
}

// BlurEffect

void attachBlurEffectComponents(scene::World& world, core::InstanceId id)
{
    world.blurEffects().add(id, scene::BlurEffectComponent{});
}

void detachBlurEffectComponents(scene::World& world, core::InstanceId id)
{
    world.blurEffects().remove(id);
}

Value getBlurEffectSize(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.blurEffects(), id, &scene::BlurEffectComponent::size);
}

bool setBlurEffectSize(scene::World& world, core::InstanceId id, const Value& value)
{
    // To a hundred: past it the picture is one flat colour whatever the
    // number, and a billion was accepted (the others here have a range).
    return writeNumber(world.blurEffects(), id, value, &scene::BlurEffectComponent::size, 0.0f, 100.0f);
}

// DepthOfFieldEffect

void attachDepthOfFieldEffectComponents(scene::World& world, core::InstanceId id)
{
    world.depthOfFieldEffects().add(id, scene::DepthOfFieldEffectComponent{});
}

void detachDepthOfFieldEffectComponents(scene::World& world, core::InstanceId id)
{
    world.depthOfFieldEffects().remove(id);
}

Value getDepthOfFieldEffectFocusDistance(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.depthOfFieldEffects(), id, &scene::DepthOfFieldEffectComponent::focusDistance);
}

bool setDepthOfFieldEffectFocusDistance(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.depthOfFieldEffects(), id, value, &scene::DepthOfFieldEffectComponent::focusDistance, 0.0f,
                       kUnbounded);
}

Value getDepthOfFieldEffectInFocusRadius(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.depthOfFieldEffects(), id, &scene::DepthOfFieldEffectComponent::inFocusRadius);
}

bool setDepthOfFieldEffectInFocusRadius(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.depthOfFieldEffects(), id, value, &scene::DepthOfFieldEffectComponent::inFocusRadius, 0.0f,
                       kUnbounded);
}

Value getDepthOfFieldEffectNearIntensity(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.depthOfFieldEffects(), id, &scene::DepthOfFieldEffectComponent::nearIntensity);
}

bool setDepthOfFieldEffectNearIntensity(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.depthOfFieldEffects(), id, value, &scene::DepthOfFieldEffectComponent::nearIntensity, 0.0f,
                       1.0f);
}

Value getDepthOfFieldEffectFarIntensity(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.depthOfFieldEffects(), id, &scene::DepthOfFieldEffectComponent::farIntensity);
}

bool setDepthOfFieldEffectFarIntensity(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.depthOfFieldEffects(), id, value, &scene::DepthOfFieldEffectComponent::farIntensity, 0.0f,
                       1.0f);
}

// SunRaysEffect

void attachSunRaysEffectComponents(scene::World& world, core::InstanceId id)
{
    world.sunRaysEffects().add(id, scene::SunRaysEffectComponent{});
}

void detachSunRaysEffectComponents(scene::World& world, core::InstanceId id)
{
    world.sunRaysEffects().remove(id);
}

Value getSunRaysEffectIntensity(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.sunRaysEffects(), id, &scene::SunRaysEffectComponent::intensity);
}

bool setSunRaysEffectIntensity(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.sunRaysEffects(), id, value, &scene::SunRaysEffectComponent::intensity, 0.0f, 1.0f);
}

Value getSunRaysEffectSpread(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.sunRaysEffects(), id, &scene::SunRaysEffectComponent::spread);
}

bool setSunRaysEffectSpread(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.sunRaysEffects(), id, value, &scene::SunRaysEffectComponent::spread, 0.0f, 1.0f);
}

// Atmosphere

void attachAtmosphereComponents(scene::World& world, core::InstanceId id)
{
    world.atmospheres().add(id, scene::AtmosphereComponent{});
}

void detachAtmosphereComponents(scene::World& world, core::InstanceId id)
{
    world.atmospheres().remove(id);
}

Value getAtmosphereDensity(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.atmospheres(), id, &scene::AtmosphereComponent::density);
}

bool setAtmosphereDensity(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.atmospheres(), id, value, &scene::AtmosphereComponent::density, 0.0f, 1.0f);
}

Value getAtmosphereOffset(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.atmospheres(), id, &scene::AtmosphereComponent::offset);
}

bool setAtmosphereOffset(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.atmospheres(), id, value, &scene::AtmosphereComponent::offset, -kUnbounded, kUnbounded);
}

Value getAtmosphereColor(const scene::World& world, core::InstanceId id)
{
    return readValue(world.atmospheres(), id, &scene::AtmosphereComponent::color);
}

bool setAtmosphereColor(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.atmospheres(), id, value, &scene::AtmosphereComponent::color);
}

Value getAtmosphereDecay(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.atmospheres(), id, &scene::AtmosphereComponent::decay);
}

bool setAtmosphereDecay(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.atmospheres(), id, value, &scene::AtmosphereComponent::decay, 0.0f, 1.0f);
}

Value getAtmosphereGlare(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.atmospheres(), id, &scene::AtmosphereComponent::glare);
}

bool setAtmosphereGlare(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.atmospheres(), id, value, &scene::AtmosphereComponent::glare, 0.0f, 10.0f);
}

Value getAtmosphereHaze(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.atmospheres(), id, &scene::AtmosphereComponent::haze);
}

bool setAtmosphereHaze(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.atmospheres(), id, value, &scene::AtmosphereComponent::haze, 0.0f, 10.0f);
}

// Sky

void attachSkyComponents(scene::World& world, core::InstanceId id)
{
    world.skies().add(id, scene::SkyComponent{});
}

void detachSkyComponents(scene::World& world, core::InstanceId id)
{
    world.skies().remove(id);
}

Value getSkySkyboxBack(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.skies(), id, &scene::SkyComponent::skyboxBack);
}

bool setSkySkyboxBack(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.skies(), id, value, &scene::SkyComponent::skyboxBack);
}

Value getSkySkyboxDown(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.skies(), id, &scene::SkyComponent::skyboxDown);
}

bool setSkySkyboxDown(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.skies(), id, value, &scene::SkyComponent::skyboxDown);
}

Value getSkySkyboxFront(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.skies(), id, &scene::SkyComponent::skyboxFront);
}

bool setSkySkyboxFront(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.skies(), id, value, &scene::SkyComponent::skyboxFront);
}

Value getSkySkyboxLeft(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.skies(), id, &scene::SkyComponent::skyboxLeft);
}

bool setSkySkyboxLeft(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.skies(), id, value, &scene::SkyComponent::skyboxLeft);
}

Value getSkySkyboxRight(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.skies(), id, &scene::SkyComponent::skyboxRight);
}

bool setSkySkyboxRight(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.skies(), id, value, &scene::SkyComponent::skyboxRight);
}

Value getSkySkyboxUp(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.skies(), id, &scene::SkyComponent::skyboxUp);
}

bool setSkySkyboxUp(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.skies(), id, value, &scene::SkyComponent::skyboxUp);
}

Value getSkySkyboxOrientation(const scene::World& world, core::InstanceId id)
{
    return readValue(world.skies(), id, &scene::SkyComponent::skyboxOrientation);
}

bool setSkySkyboxOrientation(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.skies(), id, value, &scene::SkyComponent::skyboxOrientation);
}

Value getSkySunTexture(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.skies(), id, &scene::SkyComponent::sunTexture);
}

bool setSkySunTexture(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.skies(), id, value, &scene::SkyComponent::sunTexture);
}

Value getSkyMoonTexture(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.skies(), id, &scene::SkyComponent::moonTexture);
}

bool setSkyMoonTexture(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.skies(), id, value, &scene::SkyComponent::moonTexture);
}

Value getSkySunAngularSize(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.skies(), id, &scene::SkyComponent::sunAngularSize);
}

bool setSkySunAngularSize(scene::World& world, core::InstanceId id, const Value& value)
{
    // Above zero -- a disc of no size is `CelestialBodiesShown` spelled wrong --
    // and at most sixty degrees, past which a disc is most of the sky.
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !(*number > 0.0))
        return false;
    return writeNumber(world.skies(), id, value, &scene::SkyComponent::sunAngularSize, 0.0f, 60.0f);
}

Value getSkyMoonAngularSize(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.skies(), id, &scene::SkyComponent::moonAngularSize);
}

bool setSkyMoonAngularSize(scene::World& world, core::InstanceId id, const Value& value)
{
    // Above zero -- a disc of no size is `CelestialBodiesShown` spelled wrong --
    // and at most sixty degrees, past which a disc is most of the sky.
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !(*number > 0.0))
        return false;
    return writeNumber(world.skies(), id, value, &scene::SkyComponent::moonAngularSize, 0.0f, 60.0f);
}

Value getSkyStarCount(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.skies(), id, &scene::SkyComponent::starCount);
}

bool setSkyStarCount(scene::World& world, core::InstanceId id, const Value& value)
{
    // A whole number: half a star is not a thing the sky can draw, and a count
    // that read back rounded would not be what was written.
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || std::floor(*number) != *number)
        return false;
    return writeNumber(world.skies(), id, value, &scene::SkyComponent::starCount, 0.0f, 10000.0f);
}

Value getSkyCelestialBodiesShown(const scene::World& world, core::InstanceId id)
{
    return readValue(world.skies(), id, &scene::SkyComponent::celestialBodiesShown);
}

bool setSkyCelestialBodiesShown(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.skies(), id, value, &scene::SkyComponent::celestialBodiesShown);
}

Value getSkyCloudCover(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.skies(), id, &scene::SkyComponent::cloudCover);
}

bool setSkyCloudCover(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.skies(), id, value, &scene::SkyComponent::cloudCover, 0.0f, 1.0f);
}

Value getSkyCloudDensity(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.skies(), id, &scene::SkyComponent::cloudDensity);
}

bool setSkyCloudDensity(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.skies(), id, value, &scene::SkyComponent::cloudDensity, 0.0f, 1.0f);
}

Value getSkyCloudColor(const scene::World& world, core::InstanceId id)
{
    return readValue(world.skies(), id, &scene::SkyComponent::cloudColor);
}

bool setSkyCloudColor(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.skies(), id, value, &scene::SkyComponent::cloudColor);
}

// Lighting: ADR 0096's five, beside the look's shapes they are written in.

Value getLightingEnvironmentDiffuseScale(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.lighting(), id, &scene::LightingComponent::environmentDiffuseScale);
}

bool setLightingEnvironmentDiffuseScale(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.lighting(), id, value, &scene::LightingComponent::environmentDiffuseScale, 0.0f, 1.0f);
}

Value getLightingEnvironmentSpecularScale(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.lighting(), id, &scene::LightingComponent::environmentSpecularScale);
}

bool setLightingEnvironmentSpecularScale(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.lighting(), id, value, &scene::LightingComponent::environmentSpecularScale, 0.0f, 1.0f);
}

Value getLightingShadowSoftness(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.lighting(), id, &scene::LightingComponent::shadowSoftness);
}

bool setLightingShadowSoftness(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.lighting(), id, value, &scene::LightingComponent::shadowSoftness, 0.0f, 1.0f);
}

Value getLightingGlobalShadows(const scene::World& world, core::InstanceId id)
{
    return readValue(world.lighting(), id, &scene::LightingComponent::globalShadows);
}

bool setLightingGlobalShadows(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.lighting(), id, value, &scene::LightingComponent::globalShadows);
}

Value getLightingAutoExposure(const scene::World& world, core::InstanceId id)
{
    return readValue(world.lighting(), id, &scene::LightingComponent::autoExposure);
}

bool setLightingAutoExposure(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.lighting(), id, value, &scene::LightingComponent::autoExposure);
}

// FoliageLayer and FoliageMesh (ADR 0116)

void attachFoliageLayerComponents(scene::World& world, core::InstanceId id)
{
    world.foliageLayers().add(id, scene::FoliageLayerComponent{});
}

void detachFoliageLayerComponents(scene::World& world, core::InstanceId id)
{
    world.foliageLayers().remove(id);
}

void attachFoliageMeshComponents(scene::World& world, core::InstanceId id)
{
    world.foliageMeshes().add(id, scene::FoliageMeshComponent{});
}

void detachFoliageMeshComponents(scene::World& world, core::InstanceId id)
{
    world.foliageMeshes().remove(id);
}

Value getFoliageLayerEnabled(const scene::World& world, core::InstanceId id)
{
    return readValue(world.foliageLayers(), id, &scene::FoliageLayerComponent::enabled);
}

bool setFoliageLayerEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::enabled);
}

Value getFoliageLayerDensity(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::density);
}

bool setFoliageLayerDensity(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::density, 0.0f, kUnbounded);
}

Value getFoliageLayerSlopeMin(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::slopeMin);
}

bool setFoliageLayerSlopeMin(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::slopeMin, 0.0f, 90.0f);
}

Value getFoliageLayerSlopeMax(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::slopeMax);
}

bool setFoliageLayerSlopeMax(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::slopeMax, 0.0f, 90.0f);
}

Value getFoliageLayerHeightMin(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::heightMin);
}

bool setFoliageLayerHeightMin(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::heightMin, -kUnbounded,
                       kUnbounded);
}

Value getFoliageLayerHeightMax(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::heightMax);
}

bool setFoliageLayerHeightMax(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::heightMax, -kUnbounded,
                       kUnbounded);
}

Value getFoliageLayerClumping(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::clumping);
}

bool setFoliageLayerClumping(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::clumping, 0.0f, 1.0f);
}

Value getFoliageLayerMinSpacing(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::minSpacing);
}

bool setFoliageLayerMinSpacing(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::minSpacing, 0.0f, kUnbounded);
}

Value getFoliageLayerDrawDistance(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::drawDistance);
}

bool setFoliageLayerDrawDistance(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::drawDistance,
                       std::numeric_limits<f32>::min(), kUnbounded);
}

Value getFoliageLayerFadeDistance(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::fadeDistance);
}

bool setFoliageLayerFadeDistance(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::fadeDistance, 0.0f, kUnbounded);
}

Value getFoliageLayerSeed(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageLayers(), id, &scene::FoliageLayerComponent::seed);
}

bool setFoliageLayerSeed(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::seed, -kUnbounded, kUnbounded);
}

Value getFoliageLayerReceivesDecals(const scene::World& world, core::InstanceId id)
{
    return readValue(world.foliageLayers(), id, &scene::FoliageLayerComponent::receivesDecals);
}

bool setFoliageLayerReceivesDecals(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.foliageLayers(), id, value, &scene::FoliageLayerComponent::receivesDecals);
}

Value getFoliageMeshMesh(const scene::World& world, core::InstanceId id)
{
    return readContent(world, world.foliageMeshes(), id, &scene::FoliageMeshComponent::mesh);
}

bool setFoliageMeshMesh(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeContent(world, world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::mesh);
}

Value getFoliageMeshWeight(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageMeshes(), id, &scene::FoliageMeshComponent::weight);
}

bool setFoliageMeshWeight(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::weight, 0.0f, kUnbounded);
}

Value getFoliageMeshScaleMin(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageMeshes(), id, &scene::FoliageMeshComponent::scaleMin);
}

bool setFoliageMeshScaleMin(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::scaleMin,
                       std::numeric_limits<f32>::min(), kUnbounded);
}

Value getFoliageMeshScaleMax(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageMeshes(), id, &scene::FoliageMeshComponent::scaleMax);
}

bool setFoliageMeshScaleMax(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::scaleMax,
                       std::numeric_limits<f32>::min(), kUnbounded);
}

Value getFoliageMeshRandomRotation(const scene::World& world, core::InstanceId id)
{
    return readValue(world.foliageMeshes(), id, &scene::FoliageMeshComponent::randomRotation);
}

bool setFoliageMeshRandomRotation(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::randomRotation);
}

Value getFoliageMeshAlignToNormal(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageMeshes(), id, &scene::FoliageMeshComponent::alignToNormal);
}

bool setFoliageMeshAlignToNormal(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::alignToNormal, 0.0f, 1.0f);
}

Value getFoliageMeshSink(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageMeshes(), id, &scene::FoliageMeshComponent::sink);
}

bool setFoliageMeshSink(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::sink, 0.0f, kUnbounded);
}

Value getFoliageMeshWindResponse(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageMeshes(), id, &scene::FoliageMeshComponent::windResponse);
}

bool setFoliageMeshWindResponse(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::windResponse, 0.0f, 1.0f);
}

Value getFoliageMeshStiffness(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.foliageMeshes(), id, &scene::FoliageMeshComponent::stiffness);
}

bool setFoliageMeshStiffness(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::stiffness,
                       std::numeric_limits<f32>::min(), kUnbounded);
}

Value getFoliageMeshCastShadow(const scene::World& world, core::InstanceId id)
{
    return readValue(world.foliageMeshes(), id, &scene::FoliageMeshComponent::castShadow);
}

bool setFoliageMeshCastShadow(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeValue(world.foliageMeshes(), id, value, &scene::FoliageMeshComponent::castShadow);
}

Value getFoliageMeshMaterial(const scene::World& world, core::InstanceId id)
{
    const scene::FoliageMeshComponent* mesh = world.foliageMeshes().find(id);
    if (mesh == nullptr || !mesh->material.valid())
        return Value{};
    return Value{scene::MaterialRef{std::string(world.atoms().text(mesh->material)), 0}};
}

// A material asset and nothing else -- never a runtime clone, since every
// instance of the mesh shares the one material.
bool setFoliageMeshMaterial(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::FoliageMeshComponent* mesh = world.foliageMeshes().find(id);
    if (mesh == nullptr)
        return false;
    if (const auto* material = std::get_if<scene::MaterialRef>(&value); material != nullptr) {
        if (material->clone != 0 || !asset::isMaterialPath(material->source))
            return false;
        mesh->material = world.atoms().intern(material->source);
        return true;
    }
    if (scene::valueType(value) != scene::ValueType::Nil)
        return false;
    mesh->material = core::NameAtom{};
    return true;
}

// --- Highlight, Beam and Trail (ADR 0129) ---------------------------------------
//
// What a script sets and nothing else: the renderer reads these on the frame.
// A fraction is refused outside 0 to 1 and a length when negative, as every
// number here is; an end must be an `Attachment`, and what is highlighted a
// part or a model.

namespace {

// An `Attachment`, or nothing.
[[nodiscard]] bool takeAttachment(const scene::World& world, const Value& value, core::InstanceId& out)
{
    if (const auto* reference = std::get_if<core::InstanceId>(&value); reference != nullptr) {
        if (!world.alive(*reference) || world.attachments().find(*reference) == nullptr)
            return false;
        out = *reference;
        return true;
    }
    if (scene::valueType(value) != scene::ValueType::Nil)
        return false;
    out = core::InstanceId{};
    return true;
}

// A shape, or nothing: a part, or a `Model` holding them. A point, a light or
// a script has no outline to draw.
[[nodiscard]] bool takeInstance(const scene::World& world, const Value& value, core::InstanceId& out)
{
    if (const auto* reference = std::get_if<core::InstanceId>(&value); reference != nullptr) {
        if (!world.alive(*reference) || world.destroyed(*reference))
            return false;
        const scene::ClassId model = world.classes().findId(world.atoms().lookup("Model"));
        if (world.parts().find(*reference) == nullptr &&
            !(model != scene::InvalidClass && world.isA(*reference, model)))
            return false;
        out = *reference;
        return true;
    }
    if (scene::valueType(value) != scene::ValueType::Nil)
        return false;
    out = core::InstanceId{};
    return true;
}

} // namespace

void attachAnimationPlayerComponents(scene::World& world, core::InstanceId id)
{
    world.animationPlayers().add(id, scene::AnimationPlayerComponent{});
}

void detachAnimationPlayerComponents(scene::World& world, core::InstanceId id)
{
    world.animationPlayers().remove(id);
}

Value getAnimationPlayerCullingMode(const scene::World& world, core::InstanceId id)
{
    const scene::AnimationPlayerComponent* self = world.animationPlayers().find(id);
    return self == nullptr ? Value{}
                           : Value{scene::EnumValue{generated::AnimationCullingModeEnumId, self->cullingMode}};
}

bool setAnimationPlayerCullingMode(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::AnimationPlayerComponent* self = world.animationPlayers().find(id);
    const auto* item = std::get_if<scene::EnumValue>(&value);
    if (self == nullptr || item == nullptr || item->enumId != generated::AnimationCullingModeEnumId ||
        world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    self->cullingMode = item->value;
    return true;
}

Value getAnimationPlayerGraph(const scene::World& world, core::InstanceId id)
{
    const scene::AnimationPlayerComponent* self = world.animationPlayers().find(id);
    return self == nullptr ? Value{} : Value{std::string(world.atoms().text(self->graph))};
}

bool setAnimationPlayerGraph(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::AnimationPlayerComponent* self = world.animationPlayers().find(id);
    const auto* text = std::get_if<std::string>(&value);
    if (self == nullptr || text == nullptr)
        return false;
    self->graph = text->empty() ? core::NameAtom{} : world.atoms().intern(*text);
    return true;
}

Value getAnimationPlayerRetargeting(const scene::World& world, core::InstanceId id)
{
    const scene::AnimationPlayerComponent* self = world.animationPlayers().find(id);
    return self == nullptr ? Value{} : Value{scene::EnumValue{generated::RetargetingEnumId, self->retargeting}};
}

bool setAnimationPlayerRetargeting(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::AnimationPlayerComponent* self = world.animationPlayers().find(id);
    const auto* item = std::get_if<scene::EnumValue>(&value);
    if (self == nullptr || item == nullptr || item->enumId != generated::RetargetingEnumId ||
        world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    self->retargeting = item->value;
    return true;
}

// --- IKControl, FootPlacement (ADR 0198) ----------------------------------------
//
// What the instances say. Where a limb ends up is the renderer's, each frame,
// each machine's own, and no property reads it.

void attachIKControlComponents(scene::World& world, core::InstanceId id)
{
    world.ikControls().add(id, scene::IKControlComponent{});
}

void detachIKControlComponents(scene::World& world, core::InstanceId id)
{
    world.ikControls().remove(id);
}

void attachFootPlacementComponents(scene::World& world, core::InstanceId id)
{
    world.footPlacements().add(id, scene::FootPlacementComponent{});
}

void detachFootPlacementComponents(scene::World& world, core::InstanceId id)
{
    world.footPlacements().remove(id);
}

namespace {

// A part or an attachment, or nothing: what a limb reaches for.
[[nodiscard]] bool takePlace(const scene::World& world, const Value& value, core::InstanceId& out)
{
    if (const auto* reference = std::get_if<core::InstanceId>(&value); reference != nullptr) {
        if (!world.alive(*reference) || world.destroyed(*reference))
            return false;
        if (world.parts().find(*reference) == nullptr && world.attachments().find(*reference) == nullptr)
            return false;
        out = *reference;
        return true;
    }
    if (scene::valueType(value) != scene::ValueType::Nil)
        return false;
    out = core::InstanceId{};
    return true;
}

[[nodiscard]] Value readPlace(const scene::World& world, core::InstanceId place)
{
    return !place.valid() || !world.alive(place) || world.destroyed(place) ? Value{} : Value{place};
}

template <class Component>
[[nodiscard]] Value readJoint(const scene::World& world, const Component* component, core::NameAtom Component::*field)
{
    return component == nullptr ? Value{} : Value{std::string(world.atoms().text(component->*field))};
}

template <class Component>
[[nodiscard]] bool writeJoint(scene::World& world, Component* component, core::NameAtom Component::*field,
                              const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr || component == nullptr)
        return false;
    component->*field = world.atoms().intern(*text);
    return true;
}

template <class Component>
[[nodiscard]] Value readFlag(const Component* component, bool Component::*field)
{
    return component == nullptr ? Value{} : Value{component->*field};
}

template <class Component>
[[nodiscard]] bool writeFlag(Component* component, bool Component::*field, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr || component == nullptr)
        return false;
    component->*field = *flag;
    return true;
}

} // namespace

Value getIKControlType(const scene::World& world, core::InstanceId id)
{
    const scene::IKControlComponent* self = world.ikControls().find(id);
    return self == nullptr ? Value{} : Value{scene::EnumValue{generated::IKControlTypeEnumId, self->type}};
}

bool setIKControlType(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::IKControlComponent* self = world.ikControls().find(id);
    const auto* item = std::get_if<scene::EnumValue>(&value);
    if (self == nullptr || item == nullptr || item->enumId != generated::IKControlTypeEnumId ||
        world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    self->type = item->value;
    return true;
}

Value getIKControlEndJoint(const scene::World& world, core::InstanceId id)
{
    return readJoint(world, world.ikControls().find(id), &scene::IKControlComponent::endJoint);
}

bool setIKControlEndJoint(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeJoint(world, world.ikControls().find(id), &scene::IKControlComponent::endJoint, value);
}

Value getIKControlTarget(const scene::World& world, core::InstanceId id)
{
    const scene::IKControlComponent* self = world.ikControls().find(id);
    return self == nullptr ? Value{} : readPlace(world, self->target);
}

bool setIKControlTarget(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::IKControlComponent* self = world.ikControls().find(id);
    return self != nullptr && takePlace(world, value, self->target);
}

Value getIKControlTargetOffset(const scene::World& world, core::InstanceId id)
{
    const scene::IKControlComponent* self = world.ikControls().find(id);
    return self == nullptr ? Value{} : Value{self->targetOffset};
}

bool setIKControlTargetOffset(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::IKControlComponent* self = world.ikControls().find(id);
    const auto* frame = std::get_if<core::CFrameD>(&value);
    if (self == nullptr || frame == nullptr)
        return false;
    self->targetOffset = *frame;
    return true;
}

Value getIKControlPole(const scene::World& world, core::InstanceId id)
{
    const scene::IKControlComponent* self = world.ikControls().find(id);
    return self == nullptr ? Value{} : readPlace(world, self->pole);
}

bool setIKControlPole(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::IKControlComponent* self = world.ikControls().find(id);
    return self != nullptr && takePlace(world, value, self->pole);
}

Value getIKControlAlignRotation(const scene::World& world, core::InstanceId id)
{
    return readFlag(world.ikControls().find(id), &scene::IKControlComponent::alignRotation);
}

bool setIKControlAlignRotation(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeFlag(world.ikControls().find(id), &scene::IKControlComponent::alignRotation, value);
}

Value getIKControlChainLength(const scene::World& world, core::InstanceId id)
{
    const scene::IKControlComponent* self = world.ikControls().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->chainLength)};
}

bool setIKControlChainLength(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::IKControlComponent* self = world.ikControls().find(id);
    const auto* number = std::get_if<f64>(&value);
    if (self == nullptr || number == nullptr || !(*number >= 0.0 && *number <= 8.0) || *number != std::floor(*number))
        return false;
    self->chainLength = static_cast<core::i32>(*number);
    return true;
}

Value getIKControlMaxAngle(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.ikControls().find(id), &scene::IKControlComponent::maxAngle);
}

bool setIKControlMaxAngle(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* number = std::get_if<f64>(&value);
    // Open at both ends: no turn at all is `Weight` 0, and half a circle has
    // no shortest way round.
    if (number == nullptr || !(*number > 0.0 && *number < 180.0))
        return false;
    return writeNumber(world.ikControls().find(id), &scene::IKControlComponent::maxAngle, value, 0.0, 180.0);
}

Value getIKControlWeight(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.ikControls().find(id), &scene::IKControlComponent::weight);
}

bool setIKControlWeight(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.ikControls().find(id), &scene::IKControlComponent::weight, value, 0.0, 1.0);
}

Value getIKControlEnabled(const scene::World& world, core::InstanceId id)
{
    return readFlag(world.ikControls().find(id), &scene::IKControlComponent::enabled);
}

bool setIKControlEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeFlag(world.ikControls().find(id), &scene::IKControlComponent::enabled, value);
}

Value getIKControlSmoothing(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.ikControls().find(id), &scene::IKControlComponent::smoothing);
}

bool setIKControlSmoothing(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.ikControls().find(id), &scene::IKControlComponent::smoothing, value, 0.0, AnyAmount);
}

Value getFootPlacementLeftFoot(const scene::World& world, core::InstanceId id)
{
    return readJoint(world, world.footPlacements().find(id), &scene::FootPlacementComponent::leftFoot);
}

bool setFootPlacementLeftFoot(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeJoint(world, world.footPlacements().find(id), &scene::FootPlacementComponent::leftFoot, value);
}

Value getFootPlacementRightFoot(const scene::World& world, core::InstanceId id)
{
    return readJoint(world, world.footPlacements().find(id), &scene::FootPlacementComponent::rightFoot);
}

bool setFootPlacementRightFoot(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeJoint(world, world.footPlacements().find(id), &scene::FootPlacementComponent::rightFoot, value);
}

Value getFootPlacementHips(const scene::World& world, core::InstanceId id)
{
    return readJoint(world, world.footPlacements().find(id), &scene::FootPlacementComponent::hips);
}

bool setFootPlacementHips(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeJoint(world, world.footPlacements().find(id), &scene::FootPlacementComponent::hips, value);
}

Value getFootPlacementFootHeight(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.footPlacements().find(id), &scene::FootPlacementComponent::footHeight);
}

bool setFootPlacementFootHeight(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.footPlacements().find(id), &scene::FootPlacementComponent::footHeight, value, 0.0,
                       AnyAmount);
}

Value getFootPlacementStepHeight(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.footPlacements().find(id), &scene::FootPlacementComponent::stepHeight);
}

bool setFootPlacementStepHeight(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.footPlacements().find(id), &scene::FootPlacementComponent::stepHeight, value, 0.0,
                       AnyAmount);
}

Value getFootPlacementAlignToSlope(const scene::World& world, core::InstanceId id)
{
    return readFlag(world.footPlacements().find(id), &scene::FootPlacementComponent::alignToSlope);
}

bool setFootPlacementAlignToSlope(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeFlag(world.footPlacements().find(id), &scene::FootPlacementComponent::alignToSlope, value);
}

Value getFootPlacementWeight(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.footPlacements().find(id), &scene::FootPlacementComponent::weight);
}

bool setFootPlacementWeight(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.footPlacements().find(id), &scene::FootPlacementComponent::weight, value, 0.0, 1.0);
}

Value getFootPlacementEnabled(const scene::World& world, core::InstanceId id)
{
    return readFlag(world.footPlacements().find(id), &scene::FootPlacementComponent::enabled);
}

bool setFootPlacementEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeFlag(world.footPlacements().find(id), &scene::FootPlacementComponent::enabled, value);
}

void attachHighlightComponents(scene::World& world, core::InstanceId id)
{
    world.highlights().add(id, scene::HighlightComponent{});
}

void detachHighlightComponents(scene::World& world, core::InstanceId id)
{
    world.highlights().remove(id);
}

Value getHighlightAdornee(const scene::World& world, core::InstanceId id)
{
    const scene::HighlightComponent* self = world.highlights().find(id);
    if (self == nullptr || !self->adornee.valid() || !world.alive(self->adornee) || world.destroyed(self->adornee))
        return Value{};
    return Value{self->adornee};
}

bool setHighlightAdornee(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::HighlightComponent* self = world.highlights().find(id);
    return self != nullptr && takeInstance(world, value, self->adornee);
}

Value getHighlightFillColor(const scene::World& world, core::InstanceId id)
{
    const scene::HighlightComponent* self = world.highlights().find(id);
    return self == nullptr ? Value{} : Value{self->fillColor};
}

bool setHighlightFillColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::Color3>(&value);
    scene::HighlightComponent* self = world.highlights().find(id);
    if (next == nullptr || self == nullptr)
        return false;
    self->fillColor = *next;
    return true;
}

Value getHighlightFillTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::HighlightComponent* self = world.highlights().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->fillTransparency)};
}

bool setHighlightFillTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::HighlightComponent* self = world.highlights().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    self->fillTransparency = next;
    return true;
}

Value getHighlightOutlineColor(const scene::World& world, core::InstanceId id)
{
    const scene::HighlightComponent* self = world.highlights().find(id);
    return self == nullptr ? Value{} : Value{self->outlineColor};
}

bool setHighlightOutlineColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::Color3>(&value);
    scene::HighlightComponent* self = world.highlights().find(id);
    if (next == nullptr || self == nullptr)
        return false;
    self->outlineColor = *next;
    return true;
}

Value getHighlightOutlineTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::HighlightComponent* self = world.highlights().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->outlineTransparency)};
}

bool setHighlightOutlineTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::HighlightComponent* self = world.highlights().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    self->outlineTransparency = next;
    return true;
}

Value getHighlightDepthMode(const scene::World& world, core::InstanceId id)
{
    const scene::HighlightComponent* self = world.highlights().find(id);
    return self == nullptr ? Value{} : Value{scene::EnumValue{generated::HighlightDepthModeEnumId, self->depthMode}};
}

bool setHighlightDepthMode(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::HighlightComponent* self = world.highlights().find(id);
    if (item == nullptr || self == nullptr || item->enumId != generated::HighlightDepthModeEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    self->depthMode = item->value;
    return true;
}

Value getHighlightEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::HighlightComponent* self = world.highlights().find(id);
    return self == nullptr ? Value{} : Value{self->enabled};
}

bool setHighlightEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<bool>(&value);
    scene::HighlightComponent* self = world.highlights().find(id);
    if (next == nullptr || self == nullptr)
        return false;
    self->enabled = *next;
    return true;
}

void attachBeamComponents(scene::World& world, core::InstanceId id)
{
    world.beams().add(id, scene::BeamComponent{});
}

void detachBeamComponents(scene::World& world, core::InstanceId id)
{
    world.beams().remove(id);
}

Value getBeamAttachment0(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    if (self == nullptr || !self->attachment0.valid() || !world.alive(self->attachment0) ||
        world.destroyed(self->attachment0))
        return Value{};
    return Value{self->attachment0};
}

bool setBeamAttachment0(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    return self != nullptr && takeAttachment(world, value, self->attachment0);
}

Value getBeamAttachment1(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    if (self == nullptr || !self->attachment1.valid() || !world.alive(self->attachment1) ||
        world.destroyed(self->attachment1))
        return Value{};
    return Value{self->attachment1};
}

bool setBeamAttachment1(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    return self != nullptr && takeAttachment(world, value, self->attachment1);
}

Value getBeamColor(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{self->color};
}

bool setBeamColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::ColorSequence>(&value);
    scene::BeamComponent* self = world.beams().find(id);
    if (next == nullptr || self == nullptr || !core::validSequence(next->keypoints))
        return false;
    self->color = *next;
    return true;
}

Value getBeamTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{self->transparency};
}

bool setBeamTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::NumberSequence>(&value);
    scene::BeamComponent* self = world.beams().find(id);
    if (next == nullptr || self == nullptr || !core::validSequence(next->keypoints))
        return false;
    self->transparency = *next;
    return true;
}

Value getBeamWidth0(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->width0)};
}

bool setBeamWidth0(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    self->width0 = next;
    return true;
}

Value getBeamWidth1(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->width1)};
}

bool setBeamWidth1(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    self->width1 = next;
    return true;
}

Value getBeamCurveSize0(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->curveSize0)};
}

bool setBeamCurveSize0(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next))
        return false;
    self->curveSize0 = next;
    return true;
}

Value getBeamCurveSize1(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->curveSize1)};
}

bool setBeamCurveSize1(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next))
        return false;
    self->curveSize1 = next;
    return true;
}

Value getBeamSegments(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->segments)};
}

bool setBeamSegments(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 1.0f || next > 64.0f || next != std::floor(next))
        return false;
    self->segments = static_cast<core::i32>(next);
    return true;
}

Value getBeamTexture(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{std::string(world.atoms().text(self->texture))};
}

bool setBeamTexture(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::BeamComponent* self = world.beams().find(id);
    if (text == nullptr || self == nullptr)
        return false;
    self->texture = world.atoms().intern(*text);
    return true;
}

Value getBeamTextureLength(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->textureLength)};
}

bool setBeamTextureLength(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next <= 0.0f)
        return false;
    self->textureLength = next;
    return true;
}

Value getBeamTextureMode(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{scene::EnumValue{generated::TextureModeEnumId, self->textureMode}};
}

bool setBeamTextureMode(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::BeamComponent* self = world.beams().find(id);
    if (item == nullptr || self == nullptr || item->enumId != generated::TextureModeEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    self->textureMode = item->value;
    return true;
}

Value getBeamTextureSpeed(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->textureSpeed)};
}

bool setBeamTextureSpeed(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next))
        return false;
    self->textureSpeed = next;
    return true;
}

Value getBeamFaceCamera(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{self->faceCamera};
}

bool setBeamFaceCamera(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<bool>(&value);
    scene::BeamComponent* self = world.beams().find(id);
    if (next == nullptr || self == nullptr)
        return false;
    self->faceCamera = *next;
    return true;
}

Value getBeamLightEmission(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->lightEmission)};
}

bool setBeamLightEmission(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    self->lightEmission = next;
    return true;
}

Value getBeamLightInfluence(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->lightInfluence)};
}

bool setBeamLightInfluence(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    self->lightInfluence = next;
    return true;
}

Value getBeamZOffset(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->zOffset)};
}

bool setBeamZOffset(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BeamComponent* self = world.beams().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next))
        return false;
    self->zOffset = next;
    return true;
}

Value getBeamEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::BeamComponent* self = world.beams().find(id);
    return self == nullptr ? Value{} : Value{self->enabled};
}

bool setBeamEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<bool>(&value);
    scene::BeamComponent* self = world.beams().find(id);
    if (next == nullptr || self == nullptr)
        return false;
    self->enabled = *next;
    return true;
}

void attachTrailComponents(scene::World& world, core::InstanceId id)
{
    world.trails().add(id, scene::TrailComponent{});
}

void detachTrailComponents(scene::World& world, core::InstanceId id)
{
    world.trails().remove(id);
}

Value getTrailAttachment0(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    if (self == nullptr || !self->attachment0.valid() || !world.alive(self->attachment0) ||
        world.destroyed(self->attachment0))
        return Value{};
    return Value{self->attachment0};
}

bool setTrailAttachment0(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TrailComponent* self = world.trails().find(id);
    return self != nullptr && takeAttachment(world, value, self->attachment0);
}

Value getTrailAttachment1(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    if (self == nullptr || !self->attachment1.valid() || !world.alive(self->attachment1) ||
        world.destroyed(self->attachment1))
        return Value{};
    return Value{self->attachment1};
}

bool setTrailAttachment1(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TrailComponent* self = world.trails().find(id);
    return self != nullptr && takeAttachment(world, value, self->attachment1);
}

Value getTrailLifetime(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->lifetime)};
}

bool setTrailLifetime(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TrailComponent* self = world.trails().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next <= 0.0f)
        return false;
    self->lifetime = next;
    return true;
}

Value getTrailMinLength(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->minLength)};
}

bool setTrailMinLength(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TrailComponent* self = world.trails().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    self->minLength = next;
    return true;
}

Value getTrailMaxLength(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->maxLength)};
}

bool setTrailMaxLength(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TrailComponent* self = world.trails().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f)
        return false;
    self->maxLength = next;
    return true;
}

Value getTrailColor(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{self->color};
}

bool setTrailColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::ColorSequence>(&value);
    scene::TrailComponent* self = world.trails().find(id);
    if (next == nullptr || self == nullptr || !core::validSequence(next->keypoints))
        return false;
    self->color = *next;
    return true;
}

Value getTrailTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{self->transparency};
}

bool setTrailTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::NumberSequence>(&value);
    scene::TrailComponent* self = world.trails().find(id);
    if (next == nullptr || self == nullptr || !core::validSequence(next->keypoints))
        return false;
    self->transparency = *next;
    return true;
}

Value getTrailWidthScale(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{self->widthScale};
}

bool setTrailWidthScale(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<core::NumberSequence>(&value);
    scene::TrailComponent* self = world.trails().find(id);
    if (next == nullptr || self == nullptr || !core::validSequence(next->keypoints))
        return false;
    self->widthScale = *next;
    return true;
}

Value getTrailTexture(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{std::string(world.atoms().text(self->texture))};
}

bool setTrailTexture(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::TrailComponent* self = world.trails().find(id);
    if (text == nullptr || self == nullptr)
        return false;
    self->texture = world.atoms().intern(*text);
    return true;
}

Value getTrailTextureLength(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->textureLength)};
}

bool setTrailTextureLength(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TrailComponent* self = world.trails().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next <= 0.0f)
        return false;
    self->textureLength = next;
    return true;
}

Value getTrailTextureMode(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{scene::EnumValue{generated::TextureModeEnumId, self->textureMode}};
}

bool setTrailTextureMode(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::TrailComponent* self = world.trails().find(id);
    if (item == nullptr || self == nullptr || item->enumId != generated::TextureModeEnumId)
        return false;
    if (world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    self->textureMode = item->value;
    return true;
}

Value getTrailFaceCamera(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{self->faceCamera};
}

bool setTrailFaceCamera(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<bool>(&value);
    scene::TrailComponent* self = world.trails().find(id);
    if (next == nullptr || self == nullptr)
        return false;
    self->faceCamera = *next;
    return true;
}

Value getTrailLightEmission(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->lightEmission)};
}

bool setTrailLightEmission(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TrailComponent* self = world.trails().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    self->lightEmission = next;
    return true;
}

Value getTrailLightInfluence(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->lightInfluence)};
}

bool setTrailLightInfluence(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TrailComponent* self = world.trails().find(id);
    f32 next = 0.0f;
    if (self == nullptr || !takeFinite(value, next) || next < 0.0f || next > 1.0f)
        return false;
    self->lightInfluence = next;
    return true;
}

Value getTrailEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::TrailComponent* self = world.trails().find(id);
    return self == nullptr ? Value{} : Value{self->enabled};
}

bool setTrailEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<bool>(&value);
    scene::TrailComponent* self = world.trails().find(id);
    if (next == nullptr || self == nullptr)
        return false;
    self->enabled = *next;
    return true;
}

// --- LipSync (ADR 0200) ---------------------------------------------------------

void attachLipSyncComponents(scene::World& world, core::InstanceId id)
{
    world.lipSyncs().add(id, scene::LipSyncComponent{});
}

void detachLipSyncComponents(scene::World& world, core::InstanceId id)
{
    world.lipSyncs().remove(id);
}

Value getLipSyncSource(const scene::World& world, core::InstanceId id)
{
    const scene::LipSyncComponent* self = world.lipSyncs().find(id);
    if (self == nullptr || !self->source.valid() || !world.alive(self->source) || world.destroyed(self->source))
        return Value{};
    return Value{self->source};
}

bool setLipSyncSource(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LipSyncComponent* self = world.lipSyncs().find(id);
    if (self == nullptr)
        return false;
    if (const auto* reference = std::get_if<core::InstanceId>(&value); reference != nullptr) {
        self->source = *reference;
        return true;
    }
    if (scene::valueType(value) != scene::ValueType::Nil)
        return false;
    self->source = core::InstanceId{};
    return true;
}

Value getLipSyncMode(const scene::World& world, core::InstanceId id)
{
    const scene::LipSyncComponent* self = world.lipSyncs().find(id);
    return self == nullptr ? Value{} : Value{scene::EnumValue{generated::LipSyncModeEnumId, self->mode}};
}

bool setLipSyncMode(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::LipSyncComponent* self = world.lipSyncs().find(id);
    const auto* item = std::get_if<scene::EnumValue>(&value);
    if (self == nullptr || item == nullptr || item->enumId != generated::LipSyncModeEnumId ||
        world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    self->mode = item->value;
    return true;
}

Value getLipSyncMap(const scene::World& world, core::InstanceId id)
{
    return readJoint(world, world.lipSyncs().find(id), &scene::LipSyncComponent::map);
}

bool setLipSyncMap(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeJoint(world, world.lipSyncs().find(id), &scene::LipSyncComponent::map, value);
}

Value getLipSyncSmoothing(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.lipSyncs().find(id), &scene::LipSyncComponent::smoothing);
}

bool setLipSyncSmoothing(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.lipSyncs().find(id), &scene::LipSyncComponent::smoothing, value, 0.0, 1.0);
}

Value getLipSyncWeight(const scene::World& world, core::InstanceId id)
{
    return readNumber(world.lipSyncs().find(id), &scene::LipSyncComponent::weight);
}

bool setLipSyncWeight(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeNumber(world.lipSyncs().find(id), &scene::LipSyncComponent::weight, value, 0.0, 2.0);
}

Value getLipSyncEnabled(const scene::World& world, core::InstanceId id)
{
    return readFlag(world.lipSyncs().find(id), &scene::LipSyncComponent::enabled);
}

bool setLipSyncEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    return writeFlag(world.lipSyncs().find(id), &scene::LipSyncComponent::enabled, value);
}

} // namespace engine::render::native
