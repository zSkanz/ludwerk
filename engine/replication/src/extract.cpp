#include "engine/replication/extract.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <type_traits>

#include "engine/replication/script_templates.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"
#include "wire_schema.gen.h"

namespace engine::replication {

namespace {

// --- ADR 0090's two encodings ----------------------------------------------------------
//
// Written member by member at fixed offsets, never as a struct, so padding can
// never reach the wire and a cell that was cleared compares equal to one that
// was written with the same values.

constexpr asset::MaterialField PackedOrder[] = {asset::MaterialField::Color,      asset::MaterialField::Transparency,
                                                asset::MaterialField::Emissive,   asset::MaterialField::Metalness,
                                                asset::MaterialField::Roughness,  asset::MaterialField::NormalScale,
                                                asset::MaterialField::AlphaCutoff};

void putFloats(FieldValue& out, core::usize at, std::initializer_list<float> values) noexcept
{
    for (const float value : values) {
        std::memcpy(out.raw.data() + at, &value, sizeof(value));
        at += sizeof(value);
    }
}

[[nodiscard]] float floatAt(const FieldValue& value, core::usize at) noexcept
{
    float out = 0.0f;
    std::memcpy(&out, value.raw.data() + at, sizeof(out));
    return out;
}

void packValues(FieldValue& out, asset::MaterialFieldMask set, const asset::MaterialProperties& p) noexcept
{
    out.raw.fill(0);
    std::memcpy(out.raw.data(), &set, sizeof(set));
    putFloats(out, 4,
              {p.color.r, p.color.g, p.color.b, p.transparency, p.emissive.r, p.emissive.g, p.emissive.b, p.metalness,
               p.roughness, p.normalScale, p.alphaCutoff});
}

void unpackValues(const FieldValue& value, asset::MaterialFieldMask& set, asset::MaterialProperties& p) noexcept
{
    std::memcpy(&set, value.raw.data(), sizeof(set));
    p.color = core::Color3{floatAt(value, 4), floatAt(value, 8), floatAt(value, 12)};
    p.transparency = floatAt(value, 16);
    p.emissive = core::Color3{floatAt(value, 20), floatAt(value, 24), floatAt(value, 28)};
    p.metalness = floatAt(value, 32);
    p.roughness = floatAt(value, 36);
    p.normalScale = floatAt(value, 40);
    p.alphaCutoff = floatAt(value, 44);
}

void setOverrides(FieldValue& out, const asset::MaterialOverrides& overrides) noexcept
{
    packValues(out, overrides.set, asset::overrideValues(overrides));
}

[[nodiscard]] asset::MaterialOverrides asOverrides(const FieldValue& value) noexcept
{
    asset::MaterialFieldMask set = 0;
    asset::MaterialProperties values;
    unpackValues(value, set, values);
    asset::MaterialOverrides out;
    for (const asset::MaterialField field : PackedOrder) {
        if ((set & asset::fieldBit(field)) != 0)
            (void)asset::setOverride(out, field, values);
    }
    return out;
}

void setCloneValues(FieldValue& out, const scene::MaterialClone& clone) noexcept
{
    packValues(out, clone.set, clone.values);
    out.raw[MaterialOverridesBytes] = static_cast<core::u8>(clone.values.alphaMode);
    out.raw[MaterialOverridesBytes + 1] = clone.values.doubleSided ? 1u : 0u;
}

// What a replica's copy of a clone is numbered: the authority's number in the
// top half of the range, which a replica's own scripts count up from the bottom
// of and never reach.
constexpr core::u32 ReplicatedCloneBase = 0x80000000u;

// The map field a wire name is, or `Count`.
[[nodiscard]] asset::MaterialField cloneMapField(std::string_view name) noexcept
{
    if (name == "MaterialCloneColorMap")
        return asset::MaterialField::ColorMap;
    if (name == "MaterialCloneNormalMap")
        return asset::MaterialField::NormalMap;
    if (name == "MaterialCloneMetallicRoughnessMap")
        return asset::MaterialField::MetallicRoughnessMap;
    if (name == "MaterialCloneEmissiveMap")
        return asset::MaterialField::EmissiveMap;
    return asset::MaterialField::Count;
}

[[nodiscard]] const std::string* mapOf(const asset::MaterialProperties& p, asset::MaterialField field) noexcept
{
    switch (field) {
    case asset::MaterialField::ColorMap:
        return &p.colorMap;
    case asset::MaterialField::NormalMap:
        return &p.normalMap;
    case asset::MaterialField::MetallicRoughnessMap:
        return &p.metallicRoughnessMap;
    case asset::MaterialField::EmissiveMap:
        return &p.emissiveMap;
    default:
        return nullptr;
    }
}

void setMapOf(asset::MaterialProperties& p, asset::MaterialField field, std::string_view text)
{
    switch (field) {
    case asset::MaterialField::ColorMap:
        p.colorMap = text;
        break;
    case asset::MaterialField::NormalMap:
        p.normalMap = text;
        break;
    case asset::MaterialField::MetallicRoughnessMap:
        p.metallicRoughnessMap = text;
        break;
    case asset::MaterialField::EmissiveMap:
        p.emissiveMap = text;
        break;
    default:
        break;
    }
}

} // namespace
namespace {

using core::InstanceId;
using core::usize;
using generated::ClassDesc;
using generated::Encoding;
using generated::FieldDesc;
using generated::Source;

[[nodiscard]] const ClassDesc* schemaNamed(std::string_view name)
{
    for (const ClassDesc& desc : generated::Classes) {
        if (desc.name == name) {
            return &desc;
        }
    }
    return nullptr;
}

[[nodiscard]] bool excludedByName(std::string_view name)
{
    for (const std::string_view excluded : generated::ExcludedClasses) {
        if (excluded == name)
            return true;
    }
    return false;
}

// --- The joints' plain numbers and switches (NA34) ----------------------------------
//
// A table rather than a branch each: every one is a member read and written as
// it is, and thirty branches of the same three lines hide the one that is not.

template <class C, class T>
struct Member
{
    std::string_view name;
    T C::*at;
};

template <class C, class T, usize N>
[[nodiscard]] T C::*memberNamed(const Member<C, T> (&table)[N], std::string_view name) noexcept
{
    for (const Member<C, T>& entry : table) {
        if (entry.name == name)
            return entry.at;
    }
    return nullptr;
}

using Joint = scene::ConstraintComponent;
using Mover = scene::MoverComponent;

constexpr Member<Joint, float> JointNumbers[] = {
    {"LimitLow", &Joint::limitLow},
    {"LimitHigh", &Joint::limitHigh},
    {"SwingLimit", &Joint::swingLimit},
    {"TwistLimit", &Joint::twistLimit},
    {"MotorVelocity", &Joint::motorVelocity},
    {"MotorMaxForce", &Joint::motorMaxForce},
    {"MotorMaxAcceleration", &Joint::motorMaxAcceleration},
    {"ServoTarget", &Joint::servoTarget},
    {"ServoSpeed", &Joint::servoSpeed},
    {"ServoMaxForce", &Joint::servoMaxForce},
    {"Responsiveness", &Joint::responsiveness},
    {"Stiffness", &Joint::stiffness},
    {"Damping", &Joint::damping},
    {"Length", &Joint::length},
    {"MinLength", &Joint::minLength},
    {"MaxLength", &Joint::maxLength},
    {"WinchTarget", &Joint::winchTarget},
    {"WinchSpeed", &Joint::winchSpeed},
    {"WinchForce", &Joint::winchForce},
    {"BreakForce", &Joint::breakForce},
    {"BreakTorque", &Joint::breakTorque},
    {"Thickness", &Joint::thickness},
};

constexpr Member<Joint, bool> JointSwitches[] = {
    {"Enabled", &Joint::enabled},
    {"LimitsEnabled", &Joint::limitsEnabled},
    {"CollideConnected", &Joint::collideConnected},
    {"WinchEnabled", &Joint::winchEnabled},
    {"Visible", &Joint::visible},
};

constexpr Member<Mover, float> MoverNumbers[] = {
    {"LineVelocity", &Mover::lineVelocity},
    {"MaxForce", &Mover::maxForce},
    {"MaxTorque", &Mover::maxTorque},
    {"MaxVelocity", &Mover::maxVelocity},
    {"MaxAngularVelocity", &Mover::maxAngularVelocity},
    {"MoverResponsiveness", &Mover::responsiveness},
    {"MoverStiffness", &Mover::stiffness},
    {"MoverDamping", &Mover::damping},
};

constexpr Member<Mover, core::Vec3> MoverVectors[] = {
    {"Vector", &Mover::vector},
    {"LineDirection", &Mover::lineDirection},
    {"PrimaryTangentAxis", &Mover::primaryTangentAxis},
    {"SecondaryTangentAxis", &Mover::secondaryTangentAxis},
};

constexpr Member<Mover, bool> MoverSwitches[] = {
    {"RigidityEnabled", &Mover::rigidityEnabled},
    {"ApplyAtCenterOfMass", &Mover::applyAtCenterOfMass},
    {"ReactionEnabled", &Mover::reactionEnabled},
};

// A rotation alone, as a `CFrameD` whose position is zero: the encoding the
// wire has for nine floats.
void setRotation(FieldValue& out, const core::Mat3& rotation) noexcept
{
    core::CFrameD frame;
    frame.rotation = rotation;
    setCFrame(out, frame);
}

// Reads one component-sourced field.
//
// **A switch on the POOL and then on the FIELD NAME, and that is deliberate.**
// The generator emits a description and never a reader (ADR 0069, and the same
// rule `native_accessors.cpp` follows), so this is where the engine's own types
// are named. The alternative -- teaching the generator what a component is --
// would make every storage change a generator change.
[[nodiscard]] bool readComponent(const scene::World& world, InstanceId id, const FieldDesc& field, FieldValue& out)
{
    // **The joints** (NA34). A field that names another instance is read as
    // this machine's instance; the session sends it as the peer's network id.
    if (field.pool == "attachments") {
        const scene::AttachmentComponent* attachment = world.attachments().find(id);
        if (attachment == nullptr || field.name != "CFrame")
            return false;
        setCFrame(out, attachment->cframe);
        return true;
    }
    if (field.pool == "constraints") {
        const Joint* joint = world.constraints().find(id);
        if (joint == nullptr)
            return false;
        if (float Joint::*number = memberNamed(JointNumbers, field.name); number != nullptr) {
            setF32(out, joint->*number);
            return true;
        }
        if (bool Joint::*flag = memberNamed(JointSwitches, field.name); flag != nullptr) {
            setBool(out, joint->*flag);
            return true;
        }
        if (field.name == "Attachment0" || field.name == "Attachment1") {
            setInstance(out, field.name == "Attachment0" ? joint->attachment0 : joint->attachment1);
            return true;
        }
        if (field.name == "ActuatorType") {
            setI32(out, joint->actuatorType);
            return true;
        }
        if (field.name == "TargetOrientation") {
            setRotation(out, joint->targetOrientation);
            return true;
        }
        if (field.name == "Color") {
            setVec3(out, core::Vec3{joint->color.r, joint->color.g, joint->color.b});
            return true;
        }
        return false;
    }
    if (field.pool == "movers") {
        const Mover* mover = world.movers().find(id);
        if (mover == nullptr)
            return false;
        if (float Mover::*number = memberNamed(MoverNumbers, field.name); number != nullptr) {
            setF32(out, mover->*number);
            return true;
        }
        if (core::Vec3 Mover::*vector = memberNamed(MoverVectors, field.name); vector != nullptr) {
            setVec3(out, mover->*vector);
            return true;
        }
        if (bool Mover::*flag = memberNamed(MoverSwitches, field.name); flag != nullptr) {
            setBool(out, mover->*flag);
            return true;
        }
        if (field.name == "Mode" || field.name == "RelativeTo") {
            setI32(out, field.name == "Mode" ? mover->mode : mover->relativeTo);
            return true;
        }
        if (field.name == "PlaneVelocity") {
            setVec3(out, core::Vec3{mover->planeVelocity.x, mover->planeVelocity.y, 0.0f});
            return true;
        }
        if (field.name == "Position") {
            setPosition(out, mover->position);
            return true;
        }
        if (field.name == "Orientation") {
            setRotation(out, mover->orientation);
            return true;
        }
        return false;
    }
    if (field.pool == "welds") {
        const scene::WeldComponent* weld = world.welds().find(id);
        if (weld == nullptr)
            return false;
        if (field.name == "Part0" || field.name == "Part1") {
            setInstance(out, field.name == "Part0" ? weld->part0 : weld->part1);
            return true;
        }
        if (field.name == "C0" || field.name == "C1") {
            setCFrame(out, field.name == "C0" ? weld->c0 : weld->c1);
            return true;
        }
        if (field.name == "Enabled") {
            setBool(out, weld->enabled);
            return true;
        }
        return false;
    }
    if (field.pool == "noCollisions") {
        const scene::NoCollisionComponent* pair = world.noCollisions().find(id);
        if (pair == nullptr)
            return false;
        if (field.name == "Part0" || field.name == "Part1") {
            setInstance(out, field.name == "Part0" ? pair->part0 : pair->part1);
            return true;
        }
        if (field.name == "Enabled") {
            setBool(out, pair->enabled);
            return true;
        }
        return false;
    }

    if (field.pool == "parts") {
        const scene::PartComponent* part = world.parts().find(id);
        if (part == nullptr) {
            return false;
        }
        if (field.name == "CFrame") {
            setCFrame(out, part->cframe);
            return true;
        }
        if (field.name == "Size") {
            setVec3(out, part->size);
            return true;
        }
        if (field.name == "Shape") {
            setI32(out, part->shape);
            return true;
        }
        if (field.name == "CastShadow") {
            setBool(out, part->castShadow);
            return true;
        }
        if (field.name == "ReceivesDecals") {
            setBool(out, part->receivesDecals);
            return true;
        }
        // **What the part wears** (ADR 0090): the material by name, the part's
        // overrides, and -- when it wears a runtime copy -- the copy's number,
        // what it changed and the maps it names.
        if (field.name == "Material") {
            setU32(out, part->material.id);
            return true;
        }
        if (field.name == "MaterialParameters") {
            setOverrides(out, part->materialParameters);
            return true;
        }
        const scene::MaterialClone* clone =
            part->materialClone != 0 ? world.materialClone(part->materialClone) : nullptr;
        if (field.name == "MaterialClone") {
            setU32(out, clone != nullptr ? part->materialClone : 0u);
            return true;
        }
        if (field.name == "MaterialCloneValues") {
            // Zero when there is no copy, so putting one back on is a change.
            if (clone != nullptr)
                setCloneValues(out, *clone);
            return true;
        }
        if (const asset::MaterialField map = cloneMapField(field.name); map != asset::MaterialField::Count) {
            core::u32 atom = 0;
            if (clone != nullptr && (clone->set & asset::fieldBit(map)) != 0)
                atom = world.atoms().lookup(*mapOf(clone->values, map)).id;
            setU32(out, atom);
            return true;
        }
        return false;
    }

    // What a `MeshPart` is beyond a part (D424): its mesh, by name.
    if (field.pool == "meshParts") {
        const scene::MeshPartComponent* mesh = world.meshParts().find(id);
        if (mesh == nullptr) {
            return false;
        }
        if (field.name == "MeshContent") {
            setU32(out, mesh->meshContent.id);
            return true;
        }
        if (field.name == "CollisionFidelity") {
            setI32(out, mesh->collisionFidelity);
            return true;
        }
        if (field.name == "MeshSize") {
            setVec3(out, mesh->meshSize);
            return true;
        }
        return false;
    }

    // **A part's simulation state is a different component from its shape**, and
    // the schema said `parts` for one commit before the compiler said otherwise.
    // `scene` (L3) must not learn what a body is, so `anchored` and `canCollide`
    // live where the mirror reads them.
    if (field.pool == "rigidBodies") {
        const scene::RigidBodyComponent* body = world.rigidBodies().find(id);
        if (body == nullptr) {
            return false;
        }
        if (field.name == "Anchored") {
            setBool(out, body->anchored);
            return true;
        }
        if (field.name == "CanCollide") {
            setBool(out, body->canCollide);
            return true;
        }
        if (field.name == "LinearVelocity") {
            setVec3(out, body->linearVelocity);
            return true;
        }
        if (field.name == "AngularVelocity") {
            setVec3(out, body->angularVelocity);
            return true;
        }
        // The group by name (D545), as a material is.
        if (field.name == "CollisionGroup") {
            setU32(out, body->collisionGroup.id);
            return true;
        }
        return false;
    }

    if (field.pool == "characterBodies") {
        const scene::CharacterBodyComponent* body = world.characterBodies().find(id);
        if (body == nullptr) {
            return false;
        }
        if (field.name == "VerticalVelocity") {
            setF32(out, body->verticalVelocity);
            return true;
        }
        if (field.name == "Grounded") {
            setBool(out, body->grounded);
            return true;
        }
        if (field.name == "State") {
            setI32(out, body->state);
            return true;
        }
        if (field.name == "WalkSpeed") {
            setF32(out, body->walkSpeed);
            return true;
        }
        if (field.name == "JumpSpeed") {
            setF32(out, body->jumpSpeed);
            return true;
        }
        if (field.name == "MaxSlopeAngle") {
            setF32(out, body->maxSlopeAngle);
            return true;
        }
        if (field.name == "AutoStepHeight") {
            setF32(out, body->autoStepHeight);
            return true;
        }
        if (field.name == "PushVelocity") {
            setVec3(out, body->push);
            return true;
        }
        if (field.name == "GravityScale") {
            setF32(out, body->gravityScale);
            return true;
        }
        if (field.name == "SwimSpeed") {
            setF32(out, body->swimSpeed);
            return true;
        }
        if (field.name == "FlySpeed") {
            setF32(out, body->flySpeed);
            return true;
        }
        if (field.name == "Flying") {
            setBool(out, body->flying);
            return true;
        }
        return false;
    }

    if (field.pool == "workspaces") {
        const scene::WorkspaceComponent* workspace = world.workspaces().find(id);
        if (workspace == nullptr)
            return false;
        if (field.name == "GlobalWind")
            setVec3(out, workspace->globalWind);
        else if (field.name == "WindGusts")
            setF32(out, workspace->windGusts);
        else if (field.name == "WindTurbulence")
            setF32(out, workspace->windTurbulence);
        else if (field.name == "Gravity")
            setVec3(out, workspace->gravity);
        else
            return false;
        return true;
    }

    if (field.pool == "lighting") {
        const scene::LightingComponent* lighting = world.lighting().find(id);
        if (lighting == nullptr) {
            return false;
        }
        if (field.name == "ClockTime") {
            setF32(out, lighting->clockTime);
            return true;
        }
        if (field.name == "GeographicLatitude") {
            setF32(out, lighting->geographicLatitude);
            return true;
        }
        if (field.name == "Ambient") {
            setVec3(out, core::Vec3{lighting->ambient.r, lighting->ambient.g, lighting->ambient.b});
            return true;
        }
        if (field.name == "Brightness") {
            setF32(out, lighting->brightness);
            return true;
        }
        if (field.name == "FogColor") {
            setVec3(out, core::Vec3{lighting->fogColor.r, lighting->fogColor.g, lighting->fogColor.b});
            return true;
        }
        if (field.name == "FogStart") {
            setF32(out, lighting->fogStart);
            return true;
        }
        if (field.name == "FogEnd") {
            setF32(out, lighting->fogEnd);
            return true;
        }
        if (field.name == "ExposureCompensation") {
            setF32(out, lighting->exposureCompensation);
            return true;
        }
        if (field.name == "EnvironmentDiffuseScale") {
            setF32(out, lighting->environmentDiffuseScale);
            return true;
        }
        if (field.name == "EnvironmentSpecularScale") {
            setF32(out, lighting->environmentSpecularScale);
            return true;
        }
        if (field.name == "ShadowSoftness") {
            setF32(out, lighting->shadowSoftness);
            return true;
        }
        if (field.name == "GlobalShadows") {
            setBool(out, lighting->globalShadows);
            return true;
        }
        if (field.name == "AutoExposure") {
            setBool(out, lighting->autoExposure);
            return true;
        }
        if (field.name == "ExposureMin") {
            setF32(out, lighting->exposureMin);
            return true;
        }
        if (field.name == "ExposureMax") {
            setF32(out, lighting->exposureMax);
            return true;
        }
        return false;
    }

    // ADR 0096's look: the effects, the air and the sky.
    if (field.pool == "postEffects") {
        const scene::PostEffectComponent* component = world.postEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Enabled") {
            setBool(out, component->enabled);
            return true;
        }
        return false;
    }

    if (field.pool == "waters") {
        const scene::WaterComponent* component = world.waters().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Shape") {
            setI32(out, component->shape);
            return true;
        }
        if (field.name == "SurfaceLevel") {
            setF32(out, static_cast<core::f32>(component->surfaceLevel));
            return true;
        }
        if (field.name == "Position") {
            setVec3(out, component->position);
            return true;
        }
        if (field.name == "Size") {
            setVec3(out, component->size);
            return true;
        }
        if (field.name == "Density") {
            setF32(out, static_cast<core::f32>(component->density));
            return true;
        }
        if (field.name == "Viscosity") {
            setF32(out, static_cast<core::f32>(component->viscosity));
            return true;
        }
        if (field.name == "Current") {
            setVec3(out, component->current);
            return true;
        }
        if (field.name == "FlowSpeed") {
            setF32(out, static_cast<core::f32>(component->flowSpeed));
            return true;
        }
        return false;
    }

    if (field.pool == "waterWaves") {
        const scene::WaterWaveComponent* component = world.waterWaves().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Wavelength") {
            setF32(out, static_cast<core::f32>(component->wavelength));
            return true;
        }
        if (field.name == "Amplitude") {
            setF32(out, static_cast<core::f32>(component->amplitude));
            return true;
        }
        if (field.name == "Direction") {
            setF32(out, static_cast<core::f32>(component->direction));
            return true;
        }
        if (field.name == "Steepness") {
            setF32(out, static_cast<core::f32>(component->steepness));
            return true;
        }
        if (field.name == "Phase") {
            setF32(out, static_cast<core::f32>(component->phase));
            return true;
        }
        return false;
    }

    if (field.pool == "waterPoints") {
        const scene::WaterPointComponent* component = world.waterPoints().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Position") {
            setVec3(out, component->position);
            return true;
        }
        return false;
    }

    if (field.pool == "clickDetectors") {
        const scene::ClickDetectorComponent* component = world.clickDetectors().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "MaxActivationDistance") {
            setF32(out, static_cast<core::f32>(component->maxActivationDistance));
            return true;
        }
        if (field.name == "CursorIcon") {
            setU32(out, component->cursorIcon.id);
            return true;
        }
        return false;
    }

    if (field.pool == "bloomEffects") {
        const scene::BloomEffectComponent* component = world.bloomEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Intensity") {
            setF32(out, component->intensity);
            return true;
        }
        if (field.name == "Size") {
            setF32(out, component->size);
            return true;
        }
        if (field.name == "Threshold") {
            setF32(out, component->threshold);
            return true;
        }
        return false;
    }

    if (field.pool == "colorCorrectionEffects") {
        const scene::ColorCorrectionEffectComponent* component = world.colorCorrectionEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Brightness") {
            setF32(out, component->brightness);
            return true;
        }
        if (field.name == "Contrast") {
            setF32(out, component->contrast);
            return true;
        }
        if (field.name == "Saturation") {
            setF32(out, component->saturation);
            return true;
        }
        if (field.name == "TintColor") {
            setVec3(out, core::Vec3{component->tintColor.r, component->tintColor.g, component->tintColor.b});
            return true;
        }
        return false;
    }

    if (field.pool == "blurEffects") {
        const scene::BlurEffectComponent* component = world.blurEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Size") {
            setF32(out, component->size);
            return true;
        }
        return false;
    }

    if (field.pool == "swarms") {
        const scene::SwarmComponent* component = world.swarms().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Replicates") {
            setBool(out, component->replicates);
            return true;
        }
        if (field.name == "ReplicationRadius") {
            setF32(out, component->replicationRadius);
            return true;
        }
        if (field.name == "Enabled") {
            setBool(out, component->enabled);
            return true;
        }
        return false;
    }

    if (field.pool == "depthOfFieldEffects") {
        const scene::DepthOfFieldEffectComponent* component = world.depthOfFieldEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "FocusDistance") {
            setF32(out, component->focusDistance);
            return true;
        }
        if (field.name == "InFocusRadius") {
            setF32(out, component->inFocusRadius);
            return true;
        }
        if (field.name == "NearIntensity") {
            setF32(out, component->nearIntensity);
            return true;
        }
        if (field.name == "FarIntensity") {
            setF32(out, component->farIntensity);
            return true;
        }
        return false;
    }

    if (field.pool == "sunRaysEffects") {
        const scene::SunRaysEffectComponent* component = world.sunRaysEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Intensity") {
            setF32(out, component->intensity);
            return true;
        }
        if (field.name == "Spread") {
            setF32(out, component->spread);
            return true;
        }
        return false;
    }

    if (field.pool == "atmospheres") {
        const scene::AtmosphereComponent* component = world.atmospheres().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Density") {
            setF32(out, component->density);
            return true;
        }
        if (field.name == "Offset") {
            setF32(out, component->offset);
            return true;
        }
        if (field.name == "Color") {
            setVec3(out, core::Vec3{component->color.r, component->color.g, component->color.b});
            return true;
        }
        if (field.name == "Decay") {
            setF32(out, component->decay);
            return true;
        }
        if (field.name == "Glare") {
            setF32(out, component->glare);
            return true;
        }
        if (field.name == "Haze") {
            setF32(out, component->haze);
            return true;
        }
        return false;
    }

    if (field.pool == "skies") {
        const scene::SkyComponent* component = world.skies().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "SkyboxBack") {
            setU32(out, component->skyboxBack.id);
            return true;
        }
        if (field.name == "SkyboxDown") {
            setU32(out, component->skyboxDown.id);
            return true;
        }
        if (field.name == "SkyboxFront") {
            setU32(out, component->skyboxFront.id);
            return true;
        }
        if (field.name == "SkyboxLeft") {
            setU32(out, component->skyboxLeft.id);
            return true;
        }
        if (field.name == "SkyboxRight") {
            setU32(out, component->skyboxRight.id);
            return true;
        }
        if (field.name == "SkyboxUp") {
            setU32(out, component->skyboxUp.id);
            return true;
        }
        if (field.name == "SkyboxOrientation") {
            setVec3(out, component->skyboxOrientation);
            return true;
        }
        if (field.name == "SunTexture") {
            setU32(out, component->sunTexture.id);
            return true;
        }
        if (field.name == "MoonTexture") {
            setU32(out, component->moonTexture.id);
            return true;
        }
        if (field.name == "SunAngularSize") {
            setF32(out, component->sunAngularSize);
            return true;
        }
        if (field.name == "MoonAngularSize") {
            setF32(out, component->moonAngularSize);
            return true;
        }
        if (field.name == "StarCount") {
            setF32(out, component->starCount);
            return true;
        }
        if (field.name == "CelestialBodiesShown") {
            setBool(out, component->celestialBodiesShown);
            return true;
        }
        if (field.name == "CloudCover") {
            setF32(out, component->cloudCover);
            return true;
        }
        if (field.name == "CloudDensity") {
            setF32(out, component->cloudDensity);
            return true;
        }
        if (field.name == "CloudColor") {
            setVec3(out, core::Vec3{component->cloudColor.r, component->cloudColor.g, component->cloudColor.b});
            return true;
        }
        return false;
    }

    if (field.pool == "decals") {
        const scene::DecalComponent* decal = world.decals().find(id);
        if (decal == nullptr) {
            return false;
        }
        if (field.name == "CFrame") {
            setCFrame(out, decal->cframe);
            return true;
        }
        if (field.name == "Size") {
            setVec3(out, decal->size);
            return true;
        }
        if (field.name == "Texture") {
            setU32(out, decal->texture.id);
            return true;
        }
        if (field.name == "Color") {
            setVec3(out, core::Vec3{decal->color.r, decal->color.g, decal->color.b});
            return true;
        }
        if (field.name == "Transparency") {
            setF32(out, decal->transparency);
            return true;
        }
        return false;
    }

    if (field.pool == "particleEmitters") {
        const scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
        if (emitter == nullptr) {
            return false;
        }
        if (field.name == "Enabled") {
            setBool(out, emitter->enabled);
            return true;
        }
        if (field.name == "Rate") {
            setF32(out, emitter->rate);
            return true;
        }
        if (field.name == "Lifetime") {
            setF32(out, emitter->lifetime);
            return true;
        }
        if (field.name == "Speed") {
            setF32(out, emitter->speed);
            return true;
        }
        if (field.name == "SpreadAngle") {
            setF32(out, emitter->spreadAngle);
            return true;
        }
        if (field.name == "Acceleration") {
            setVec3(out, emitter->acceleration);
            return true;
        }
        if (field.name == "Drag") {
            setF32(out, emitter->drag);
            return true;
        }
        if (field.name == "Color") {
            setVec3(out, core::Vec3{emitter->color.r, emitter->color.g, emitter->color.b});
            return true;
        }
        if (field.name == "ColorEnd") {
            setVec3(out, core::Vec3{emitter->colorEnd.r, emitter->colorEnd.g, emitter->colorEnd.b});
            return true;
        }
        if (field.name == "Size") {
            setF32(out, emitter->size);
            return true;
        }
        if (field.name == "SizeEnd") {
            setF32(out, emitter->sizeEnd);
            return true;
        }
        if (field.name == "Transparency") {
            setF32(out, emitter->transparency);
            return true;
        }
        if (field.name == "TransparencyEnd") {
            setF32(out, emitter->transparencyEnd);
            return true;
        }
        if (field.name == "LightEmission") {
            setF32(out, emitter->lightEmission);
            return true;
        }
        if (field.name == "Brightness") {
            setF32(out, emitter->brightness);
            return true;
        }
        if (field.name == "Shape") {
            setI32(out, emitter->shape);
            return true;
        }
        if (field.name == "Emitted") {
            setU32(out, static_cast<core::u32>(emitter->emitted));
            return true;
        }
        if (field.name == "WindAffectsDrift") {
            setBool(out, emitter->windAffectsDrift);
            return true;
        }
        return false;
    }

    if (field.pool == "models") {
        const scene::ModelComponent* model = world.models().find(id);
        if (model == nullptr) {
            return false;
        }
        if (field.name == "Scale") {
            setF32(out, model->scale);
            return true;
        }
        return false;
    }

    // **The 2D layer's sprites** (ADR 0088). A `Vector2` travels as a `Vector3`
    // with a zero z, so the codec needs no encoding of its own for it.
    if (field.pool == "parts2d") {
        const scene::Part2DComponent* sprite = world.parts2d().find(id);
        if (sprite == nullptr) {
            return false;
        }
        const auto flat = [](core::Vec2 value) { return core::Vec3{value.x, value.y, 0.0f}; };
        if (field.name == "Position")
            setVec3(out, flat(sprite->position));
        else if (field.name == "Rotation")
            setF32(out, sprite->rotation);
        else if (field.name == "Size")
            setVec3(out, flat(sprite->size));
        else if (field.name == "Velocity")
            setVec3(out, flat(sprite->velocity));
        else if (field.name == "AngularVelocity")
            setF32(out, sprite->angularVelocity);
        else if (field.name == "Color")
            setVec3(out, core::Vec3{sprite->color.r, sprite->color.g, sprite->color.b});
        else if (field.name == "Transparency")
            setF32(out, sprite->transparency);
        else if (field.name == "Anchored")
            setBool(out, sprite->anchored);
        else if (field.name == "CanCollide")
            setBool(out, sprite->canCollide);
        else if (field.name == "Sensor")
            setBool(out, sprite->sensor);
        else if (field.name == "Shape")
            setI32(out, sprite->shape);
        else if (field.name == "ZIndex")
            setI32(out, sprite->zIndex);
        else if (field.name == "FlipX")
            setBool(out, sprite->flipX);
        else if (field.name == "FlipY")
            setBool(out, sprite->flipY);
        else if (field.name == "Image")
            setU32(out, sprite->image.id);
        else if (field.name == "ImageRectOffset")
            setVec3(out, flat(sprite->imageRectOffset));
        else if (field.name == "ImageRectSize")
            setVec3(out, flat(sprite->imageRectSize));
        else if (field.name == "Filter")
            setI32(out, sprite->filter);
        else if (field.name == "ExactColor")
            setBool(out, sprite->exactColor);
        else
            return false;
        return true;
    }

    // A tilemap's properties (ADR 0103). Its cells are not fields: they travel
    // in `TilemapBlocks`.
    if (field.pool == "tilemaps2d") {
        const scene::Tilemap2DComponent* tilemap = world.tilemaps2d().find(id);
        if (tilemap == nullptr) {
            return false;
        }
        const auto flat = [](core::Vec2 value) { return core::Vec3{value.x, value.y, 0.0f}; };
        if (field.name == "Position")
            setVec3(out, flat(tilemap->position));
        else if (field.name == "CellSize")
            setF32(out, tilemap->cellSize);
        else if (field.name == "Tileset")
            setU32(out, tilemap->tileset.id);
        else if (field.name == "TileSize")
            setVec3(out, flat(tilemap->tileSize));
        else if (field.name == "ZIndex")
            setI32(out, tilemap->zIndex);
        else if (field.name == "Color")
            setVec3(out, core::Vec3{tilemap->color.r, tilemap->color.g, tilemap->color.b});
        else if (field.name == "Filter")
            setI32(out, tilemap->filter);
        else if (field.name == "Collides")
            setBool(out, tilemap->collides);
        else if (field.name == "Friction")
            setF32(out, tilemap->friction);
        else if (field.name == "ExactColor")
            setBool(out, tilemap->exactColor);
        else
            return false;
        return true;
    }

    return false;
}

[[nodiscard]] bool writeComponent(scene::World& world, InstanceId id, const FieldDesc& field, const FieldValue& value)
{
    // ADR 0096's look: the effects, the air and the sky.
    const auto toColour = [](core::Vec3 v) { return core::Color3{v.x, v.y, v.z}; };

    // **The joints** (NA34). A reference arrives as this machine's instance:
    // the session resolved the network id before it called here.
    if (field.pool == "attachments") {
        scene::AttachmentComponent* attachment = world.attachments().find(id);
        if (attachment == nullptr || field.name != "CFrame")
            return false;
        attachment->cframe = asCFrame(value);
        return true;
    }
    if (field.pool == "constraints") {
        Joint* joint = world.constraints().find(id);
        if (joint == nullptr)
            return false;
        if (float Joint::*number = memberNamed(JointNumbers, field.name); number != nullptr) {
            joint->*number = asF32(value);
            return true;
        }
        if (bool Joint::*flag = memberNamed(JointSwitches, field.name); flag != nullptr) {
            joint->*flag = asBool(value);
            return true;
        }
        if (field.name == "Attachment0" || field.name == "Attachment1") {
            (field.name == "Attachment0" ? joint->attachment0 : joint->attachment1) = asInstance(value);
            return true;
        }
        if (field.name == "ActuatorType") {
            joint->actuatorType = asI32(value);
            return true;
        }
        if (field.name == "TargetOrientation") {
            joint->targetOrientation = asCFrame(value).rotation;
            return true;
        }
        if (field.name == "Color") {
            joint->color = toColour(asVec3(value));
            return true;
        }
        return false;
    }
    if (field.pool == "movers") {
        Mover* mover = world.movers().find(id);
        if (mover == nullptr)
            return false;
        if (float Mover::*number = memberNamed(MoverNumbers, field.name); number != nullptr) {
            mover->*number = asF32(value);
            return true;
        }
        if (core::Vec3 Mover::*vector = memberNamed(MoverVectors, field.name); vector != nullptr) {
            mover->*vector = asVec3(value);
            return true;
        }
        if (bool Mover::*flag = memberNamed(MoverSwitches, field.name); flag != nullptr) {
            mover->*flag = asBool(value);
            return true;
        }
        if (field.name == "Mode" || field.name == "RelativeTo") {
            (field.name == "Mode" ? mover->mode : mover->relativeTo) = asI32(value);
            return true;
        }
        if (field.name == "PlaneVelocity") {
            const core::Vec3 plane = asVec3(value);
            mover->planeVelocity = core::Vec2{plane.x, plane.y};
            return true;
        }
        if (field.name == "Position") {
            mover->position = asPosition(value);
            return true;
        }
        if (field.name == "Orientation") {
            mover->orientation = asCFrame(value).rotation;
            return true;
        }
        return false;
    }
    if (field.pool == "welds") {
        scene::WeldComponent* weld = world.welds().find(id);
        if (weld == nullptr)
            return false;
        if (field.name == "Part0" || field.name == "Part1") {
            (field.name == "Part0" ? weld->part0 : weld->part1) = asInstance(value);
            return true;
        }
        if (field.name == "C0" || field.name == "C1") {
            (field.name == "C0" ? weld->c0 : weld->c1) = asCFrame(value);
            return true;
        }
        if (field.name == "Enabled") {
            weld->enabled = asBool(value);
            return true;
        }
        return false;
    }
    if (field.pool == "noCollisions") {
        scene::NoCollisionComponent* pair = world.noCollisions().find(id);
        if (pair == nullptr)
            return false;
        if (field.name == "Part0" || field.name == "Part1") {
            (field.name == "Part0" ? pair->part0 : pair->part1) = asInstance(value);
            return true;
        }
        if (field.name == "Enabled") {
            pair->enabled = asBool(value);
            return true;
        }
        return false;
    }
    if (field.pool == "postEffects") {
        scene::PostEffectComponent* component = world.postEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Enabled") {
            component->enabled = asBool(value);
            return true;
        }
        return false;
    }

    if (field.pool == "waters") {
        scene::WaterComponent* component = world.waters().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Shape") {
            component->shape = asI32(value);
            return true;
        }
        if (field.name == "SurfaceLevel") {
            component->surfaceLevel = static_cast<core::f64>(asF32(value));
            return true;
        }
        if (field.name == "Position") {
            component->position = asVec3(value);
            return true;
        }
        if (field.name == "Size") {
            component->size = asVec3(value);
            return true;
        }
        if (field.name == "Density") {
            component->density = static_cast<core::f64>(asF32(value));
            return true;
        }
        if (field.name == "Viscosity") {
            component->viscosity = static_cast<core::f64>(asF32(value));
            return true;
        }
        if (field.name == "Current") {
            component->current = asVec3(value);
            return true;
        }
        if (field.name == "FlowSpeed") {
            component->flowSpeed = static_cast<core::f64>(asF32(value));
            return true;
        }
        return false;
    }

    if (field.pool == "waterWaves") {
        scene::WaterWaveComponent* component = world.waterWaves().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Wavelength") {
            component->wavelength = static_cast<core::f64>(asF32(value));
            return true;
        }
        if (field.name == "Amplitude") {
            component->amplitude = static_cast<core::f64>(asF32(value));
            return true;
        }
        if (field.name == "Direction") {
            component->direction = static_cast<core::f64>(asF32(value));
            return true;
        }
        if (field.name == "Steepness") {
            component->steepness = static_cast<core::f64>(asF32(value));
            return true;
        }
        if (field.name == "Phase") {
            component->phase = static_cast<core::f64>(asF32(value));
            return true;
        }
        return false;
    }

    if (field.pool == "waterPoints") {
        scene::WaterPointComponent* component = world.waterPoints().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Position") {
            component->position = asVec3(value);
            return true;
        }
        return false;
    }

    if (field.pool == "clickDetectors") {
        scene::ClickDetectorComponent* component = world.clickDetectors().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "MaxActivationDistance") {
            component->maxActivationDistance = static_cast<core::f64>(asF32(value));
            return true;
        }
        if (field.name == "CursorIcon") {
            component->cursorIcon = core::NameAtom{asU32(value)};
            return true;
        }
        return false;
    }

    if (field.pool == "bloomEffects") {
        scene::BloomEffectComponent* component = world.bloomEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Intensity") {
            component->intensity = asF32(value);
            return true;
        }
        if (field.name == "Size") {
            component->size = asF32(value);
            return true;
        }
        if (field.name == "Threshold") {
            component->threshold = asF32(value);
            return true;
        }
        return false;
    }

    if (field.pool == "colorCorrectionEffects") {
        scene::ColorCorrectionEffectComponent* component = world.colorCorrectionEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Brightness") {
            component->brightness = asF32(value);
            return true;
        }
        if (field.name == "Contrast") {
            component->contrast = asF32(value);
            return true;
        }
        if (field.name == "Saturation") {
            component->saturation = asF32(value);
            return true;
        }
        if (field.name == "TintColor") {
            component->tintColor = toColour(asVec3(value));
            return true;
        }
        return false;
    }

    if (field.pool == "blurEffects") {
        scene::BlurEffectComponent* component = world.blurEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Size") {
            component->size = asF32(value);
            return true;
        }
        return false;
    }

    if (field.pool == "swarms") {
        scene::SwarmComponent* component = world.swarms().find(id);
        if (component == nullptr) {
            return false;
        }
        // What arrives is the authority's swarm: read here, never stepped
        // (ADR 0162).
        component->mirrored = true;
        if (field.name == "Replicates") {
            component->replicates = asBool(value);
            return true;
        }
        if (field.name == "ReplicationRadius") {
            component->replicationRadius = asF32(value);
            return true;
        }
        if (field.name == "Enabled") {
            component->enabled = asBool(value);
            return true;
        }
        return false;
    }

    if (field.pool == "depthOfFieldEffects") {
        scene::DepthOfFieldEffectComponent* component = world.depthOfFieldEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "FocusDistance") {
            component->focusDistance = asF32(value);
            return true;
        }
        if (field.name == "InFocusRadius") {
            component->inFocusRadius = asF32(value);
            return true;
        }
        if (field.name == "NearIntensity") {
            component->nearIntensity = asF32(value);
            return true;
        }
        if (field.name == "FarIntensity") {
            component->farIntensity = asF32(value);
            return true;
        }
        return false;
    }

    if (field.pool == "sunRaysEffects") {
        scene::SunRaysEffectComponent* component = world.sunRaysEffects().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Intensity") {
            component->intensity = asF32(value);
            return true;
        }
        if (field.name == "Spread") {
            component->spread = asF32(value);
            return true;
        }
        return false;
    }

    if (field.pool == "atmospheres") {
        scene::AtmosphereComponent* component = world.atmospheres().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "Density") {
            component->density = asF32(value);
            return true;
        }
        if (field.name == "Offset") {
            component->offset = asF32(value);
            return true;
        }
        if (field.name == "Color") {
            component->color = toColour(asVec3(value));
            return true;
        }
        if (field.name == "Decay") {
            component->decay = asF32(value);
            return true;
        }
        if (field.name == "Glare") {
            component->glare = asF32(value);
            return true;
        }
        if (field.name == "Haze") {
            component->haze = asF32(value);
            return true;
        }
        return false;
    }

    if (field.pool == "skies") {
        scene::SkyComponent* component = world.skies().find(id);
        if (component == nullptr) {
            return false;
        }
        if (field.name == "SkyboxBack") {
            component->skyboxBack = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "SkyboxDown") {
            component->skyboxDown = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "SkyboxFront") {
            component->skyboxFront = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "SkyboxLeft") {
            component->skyboxLeft = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "SkyboxRight") {
            component->skyboxRight = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "SkyboxUp") {
            component->skyboxUp = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "SkyboxOrientation") {
            component->skyboxOrientation = asVec3(value);
            return true;
        }
        if (field.name == "SunTexture") {
            component->sunTexture = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "MoonTexture") {
            component->moonTexture = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "SunAngularSize") {
            component->sunAngularSize = asF32(value);
            return true;
        }
        if (field.name == "MoonAngularSize") {
            component->moonAngularSize = asF32(value);
            return true;
        }
        if (field.name == "StarCount") {
            component->starCount = asF32(value);
            return true;
        }
        if (field.name == "CelestialBodiesShown") {
            component->celestialBodiesShown = asBool(value);
            return true;
        }
        if (field.name == "CloudCover") {
            component->cloudCover = asF32(value);
            return true;
        }
        if (field.name == "CloudDensity") {
            component->cloudDensity = asF32(value);
            return true;
        }
        if (field.name == "CloudColor") {
            component->cloudColor = toColour(asVec3(value));
            return true;
        }
        return false;
    }

    if (field.pool == "meshParts") {
        scene::MeshPartComponent* mesh = world.meshParts().find(id);
        if (mesh == nullptr) {
            return false;
        }
        if (field.name == "MeshContent") {
            // This machine's own atom, as a material's name arrives.
            const core::NameAtom atom{asU32(value)};
            mesh->meshContent = world.atoms().text(atom).empty() ? core::NameAtom{} : atom;
            return true;
        }
        if (field.name == "CollisionFidelity") {
            mesh->collisionFidelity = asI32(value);
            return true;
        }
        if (field.name == "MeshSize") {
            mesh->meshSize = asVec3(value);
            return true;
        }
        return false;
    }

    if (field.pool == "parts") {
        scene::PartComponent* part = world.parts().find(id);
        if (part == nullptr) {
            return false;
        }
        if (field.name == "CFrame") {
            part->cframe = asCFrame(value);
            return true;
        }
        if (field.name == "Size") {
            part->size = asVec3(value);
            return true;
        }
        if (field.name == "Shape") {
            part->shape = asI32(value);
            return true;
        }
        if (field.name == "CastShadow") {
            part->castShadow = asBool(value);
            return true;
        }
        if (field.name == "ReceivesDecals") {
            part->receivesDecals = asBool(value);
            return true;
        }
        // The name arrives as this machine's own atom (the session translated
        // it); an empty one is the engine default.
        if (field.name == "Material") {
            const core::NameAtom atom{asU32(value)};
            part->material = world.atoms().text(atom).empty() ? core::NameAtom{} : atom;
            return true;
        }
        if (field.name == "MaterialParameters") {
            part->materialParameters = asOverrides(value);
            return true;
        }
        if (field.name == "MaterialClone") {
            const core::u32 authority = asU32(value);
            if (authority == 0) {
                if (part->materialClone != 0)
                    world.requestMaterialSweep();
                part->materialClone = 0;
                return true;
            }
            // Adopted under the authority's number, pointed at what the part
            // wears -- which arrived first, in field order.
            part->materialClone = ReplicatedCloneBase | authority;
            (void)world.adoptMaterialClone(part->materialClone, part->material);
            return true;
        }
        if (field.name == "MaterialCloneValues") {
            scene::MaterialClone* clone =
                part->materialClone != 0 ? world.writeMaterialClone(part->materialClone) : nullptr;
            if (clone == nullptr)
                return true;
            // The maps are set by their own fields; only the rest comes from here.
            constexpr asset::MaterialFieldMask Maps = asset::fieldBit(asset::MaterialField::ColorMap) |
                                                      asset::fieldBit(asset::MaterialField::NormalMap) |
                                                      asset::fieldBit(asset::MaterialField::MetallicRoughnessMap) |
                                                      asset::fieldBit(asset::MaterialField::EmissiveMap);
            asset::MaterialFieldMask set = 0;
            unpackValues(value, set, clone->values);
            clone->values.alphaMode = static_cast<core::i32>(value.raw[MaterialOverridesBytes]);
            clone->values.doubleSided = value.raw[MaterialOverridesBytes + 1] != 0;
            clone->set = static_cast<asset::MaterialFieldMask>((set & ~Maps) | (clone->set & Maps));
            return true;
        }
        if (const asset::MaterialField map = cloneMapField(field.name); map != asset::MaterialField::Count) {
            scene::MaterialClone* clone =
                part->materialClone != 0 ? world.writeMaterialClone(part->materialClone) : nullptr;
            if (clone == nullptr)
                return true;
            const std::string_view text = world.atoms().text(core::NameAtom{asU32(value)});
            setMapOf(clone->values, map, text);
            if (text.empty())
                clone->set = static_cast<asset::MaterialFieldMask>(clone->set & ~asset::fieldBit(map));
            else
                clone->set |= asset::fieldBit(map);
            return true;
        }
        return false;
    }

    if (field.pool == "rigidBodies") {
        scene::RigidBodyComponent* body = world.rigidBodies().find(id);
        if (body == nullptr) {
            return false;
        }
        if (field.name == "Anchored") {
            body->anchored = asBool(value);
            return true;
        }
        if (field.name == "CanCollide") {
            body->canCollide = asBool(value);
            return true;
        }
        if (field.name == "LinearVelocity") {
            body->linearVelocity = asVec3(value);
            return true;
        }
        if (field.name == "AngularVelocity") {
            body->angularVelocity = asVec3(value);
            return true;
        }
        // **This machine's own atom for the group's name** (D545). One it has
        // not registered is registered here, colliding with everything: the
        // table of groups travels in a message of its own, and a part may be
        // told of before it. No name is `Default`.
        if (field.name == "CollisionGroup") {
            const core::NameAtom atom{asU32(value)};
            scene::CollisionGroups& groups = world.collisionGroups();
            if (world.atoms().text(atom).empty()) {
                body->collisionGroup = groups.nameAt(scene::CollisionGroups::kDefault);
                return true;
            }
            if (groups.find(atom) == scene::CollisionGroups::kInvalid) {
                if (groups.add(atom) == scene::CollisionGroups::kInvalid) {
                    body->collisionGroup = groups.nameAt(scene::CollisionGroups::kDefault);
                    return true;
                }
                groups.bumpRevision();
            }
            body->collisionGroup = atom;
            return true;
        }
        return false;
    }

    if (field.pool == "characterBodies") {
        scene::CharacterBodyComponent* body = world.characterBodies().find(id);
        if (body == nullptr) {
            return false;
        }
        if (field.name == "VerticalVelocity") {
            body->verticalVelocity = asF32(value);
            return true;
        }
        if (field.name == "Grounded") {
            body->grounded = asBool(value);
            return true;
        }
        if (field.name == "State") {
            body->state = static_cast<core::i32>(asU32(value));
            return true;
        }
        if (field.name == "WalkSpeed") {
            body->walkSpeed = asF32(value);
            return true;
        }
        if (field.name == "JumpSpeed") {
            body->jumpSpeed = asF32(value);
            return true;
        }
        if (field.name == "MaxSlopeAngle") {
            body->maxSlopeAngle = asF32(value);
            return true;
        }
        if (field.name == "AutoStepHeight") {
            body->autoStepHeight = asF32(value);
            return true;
        }
        if (field.name == "PushVelocity") {
            body->push = asVec3(value);
            return true;
        }
        if (field.name == "GravityScale") {
            body->gravityScale = asF32(value);
            return true;
        }
        if (field.name == "SwimSpeed") {
            body->swimSpeed = asF32(value);
            return true;
        }
        if (field.name == "FlySpeed") {
            body->flySpeed = asF32(value);
            return true;
        }
        if (field.name == "Flying") {
            body->flying = asBool(value);
            return true;
        }
        return false;
    }

    if (field.pool == "workspaces") {
        scene::WorkspaceComponent* workspace = world.workspaces().find(id);
        if (workspace == nullptr)
            return false;
        if (field.name == "GlobalWind")
            workspace->globalWind = asVec3(value);
        else if (field.name == "WindGusts")
            workspace->windGusts = asF32(value);
        else if (field.name == "WindTurbulence")
            workspace->windTurbulence = asF32(value);
        else if (field.name == "Gravity")
            workspace->gravity = asVec3(value);
        else
            return false;
        return true;
    }

    if (field.pool == "lighting") {
        scene::LightingComponent* lighting = world.lighting().find(id);
        if (lighting == nullptr) {
            return false;
        }
        const auto colour = [&value] {
            const core::Vec3 v = asVec3(value);
            return core::Color3{v.x, v.y, v.z};
        };
        if (field.name == "ClockTime")
            lighting->clockTime = asF32(value);
        else if (field.name == "GeographicLatitude")
            lighting->geographicLatitude = asF32(value);
        else if (field.name == "Ambient")
            lighting->ambient = colour();
        else if (field.name == "Brightness")
            lighting->brightness = asF32(value);
        else if (field.name == "FogColor")
            lighting->fogColor = colour();
        else if (field.name == "FogStart")
            lighting->fogStart = asF32(value);
        else if (field.name == "FogEnd")
            lighting->fogEnd = asF32(value);
        else if (field.name == "ExposureCompensation")
            lighting->exposureCompensation = asF32(value);
        else if (field.name == "EnvironmentDiffuseScale")
            lighting->environmentDiffuseScale = asF32(value);
        else if (field.name == "EnvironmentSpecularScale")
            lighting->environmentSpecularScale = asF32(value);
        else if (field.name == "ShadowSoftness")
            lighting->shadowSoftness = asF32(value);
        else if (field.name == "GlobalShadows")
            lighting->globalShadows = asBool(value);
        else if (field.name == "AutoExposure")
            lighting->autoExposure = asBool(value);
        else if (field.name == "ExposureMin")
            lighting->exposureMin = asF32(value);
        else if (field.name == "ExposureMax")
            lighting->exposureMax = asF32(value);
        else
            return false;
        return true;
    }

    if (field.pool == "decals") {
        scene::DecalComponent* decal = world.decals().find(id);
        if (decal == nullptr) {
            return false;
        }
        if (field.name == "CFrame") {
            decal->cframe = asCFrame(value);
            return true;
        }
        if (field.name == "Size") {
            decal->size = asVec3(value);
            return true;
        }
        if (field.name == "Texture") {
            decal->texture = core::NameAtom{asU32(value)};
            return true;
        }
        if (field.name == "Color") {
            const core::Vec3 colour = asVec3(value);
            decal->color = core::Color3{colour.x, colour.y, colour.z};
            return true;
        }
        if (field.name == "Transparency") {
            decal->transparency = asF32(value);
            return true;
        }
        return false;
    }

    if (field.pool == "particleEmitters") {
        scene::ParticleEmitterComponent* emitter = world.particleEmitters().find(id);
        if (emitter == nullptr) {
            return false;
        }
        if (field.name == "Enabled") {
            emitter->enabled = asBool(value);
            return true;
        }
        if (field.name == "Rate") {
            emitter->rate = asF32(value);
            return true;
        }
        if (field.name == "Lifetime") {
            emitter->lifetime = asF32(value);
            return true;
        }
        if (field.name == "Speed") {
            emitter->speed = asF32(value);
            return true;
        }
        if (field.name == "SpreadAngle") {
            emitter->spreadAngle = asF32(value);
            return true;
        }
        if (field.name == "Acceleration") {
            emitter->acceleration = asVec3(value);
            return true;
        }
        if (field.name == "Drag") {
            emitter->drag = asF32(value);
            return true;
        }
        if (field.name == "Color") {
            const core::Vec3 colour = asVec3(value);
            emitter->color = core::Color3{colour.x, colour.y, colour.z};
            return true;
        }
        if (field.name == "ColorEnd") {
            const core::Vec3 colour = asVec3(value);
            emitter->colorEnd = core::Color3{colour.x, colour.y, colour.z};
            return true;
        }
        if (field.name == "Size") {
            emitter->size = asF32(value);
            return true;
        }
        if (field.name == "SizeEnd") {
            emitter->sizeEnd = asF32(value);
            return true;
        }
        if (field.name == "Transparency") {
            emitter->transparency = asF32(value);
            return true;
        }
        if (field.name == "TransparencyEnd") {
            emitter->transparencyEnd = asF32(value);
            return true;
        }
        if (field.name == "LightEmission") {
            emitter->lightEmission = asF32(value);
            return true;
        }
        if (field.name == "Brightness") {
            emitter->brightness = asF32(value);
            return true;
        }
        if (field.name == "Shape") {
            emitter->shape = static_cast<core::i32>(asU32(value));
            return true;
        }
        if (field.name == "Emitted") {
            // The replica keeps its own running total and moves it by the
            // difference, so a wrap of the 32 bits on the wire costs nothing.
            const core::u32 sent = asU32(value);
            emitter->emitted += static_cast<core::u32>(sent - static_cast<core::u32>(emitter->emitted));
            return true;
        }
        if (field.name == "WindAffectsDrift") {
            emitter->windAffectsDrift = asBool(value);
            return true;
        }
        return false;
    }

    if (field.pool == "models") {
        scene::ModelComponent* model = world.models().find(id);
        if (model == nullptr) {
            return false;
        }
        if (field.name == "Scale") {
            model->scale = asF32(value);
            return true;
        }
        return false;
    }

    if (field.pool == "parts2d") {
        scene::Part2DComponent* sprite = world.parts2d().find(id);
        if (sprite == nullptr) {
            return false;
        }
        const auto plane = [](const FieldValue& cell) {
            const core::Vec3 value = asVec3(cell);
            return core::Vec2{value.x, value.y};
        };
        if (field.name == "Position")
            sprite->position = plane(value);
        else if (field.name == "Rotation")
            sprite->rotation = asF32(value);
        else if (field.name == "Size")
            sprite->size = plane(value);
        else if (field.name == "Velocity")
            sprite->velocity = plane(value);
        else if (field.name == "AngularVelocity")
            sprite->angularVelocity = asF32(value);
        else if (field.name == "Color") {
            const core::Vec3 colour = asVec3(value);
            sprite->color = core::Color3{colour.x, colour.y, colour.z};
        }
        else if (field.name == "Transparency")
            sprite->transparency = asF32(value);
        else if (field.name == "Anchored")
            sprite->anchored = asBool(value);
        else if (field.name == "CanCollide")
            sprite->canCollide = asBool(value);
        else if (field.name == "Sensor")
            sprite->sensor = asBool(value);
        else if (field.name == "Shape")
            sprite->shape = asI32(value);
        else if (field.name == "ZIndex")
            sprite->zIndex = asI32(value);
        else if (field.name == "FlipX")
            sprite->flipX = asBool(value);
        else if (field.name == "FlipY")
            sprite->flipY = asBool(value);
        else if (field.name == "Image")
            sprite->image = core::NameAtom{asU32(value)};
        else if (field.name == "ImageRectOffset")
            sprite->imageRectOffset = plane(value);
        else if (field.name == "ImageRectSize")
            sprite->imageRectSize = plane(value);
        else if (field.name == "Filter")
            sprite->filter = asI32(value);
        else if (field.name == "ExactColor")
            sprite->exactColor = asBool(value);
        else
            return false;
        return true;
    }

    if (field.pool == "tilemaps2d") {
        scene::Tilemap2DComponent* tilemap = world.tilemaps2d().find(id);
        if (tilemap == nullptr) {
            return false;
        }
        const auto plane = [](const FieldValue& cell) {
            const core::Vec3 value = asVec3(cell);
            return core::Vec2{value.x, value.y};
        };
        if (field.name == "Position")
            tilemap->position = plane(value);
        else if (field.name == "CellSize")
            tilemap->cellSize = asF32(value);
        else if (field.name == "Tileset")
            tilemap->tileset = core::NameAtom{asU32(value)};
        else if (field.name == "TileSize")
            tilemap->tileSize = plane(value);
        else if (field.name == "ZIndex")
            tilemap->zIndex = asI32(value);
        else if (field.name == "Color") {
            const core::Vec3 colour = asVec3(value);
            tilemap->color = core::Color3{colour.x, colour.y, colour.z};
        }
        else if (field.name == "Filter")
            tilemap->filter = asI32(value);
        else if (field.name == "Collides")
            tilemap->collides = asBool(value);
        else if (field.name == "Friction")
            tilemap->friction = asF32(value);
        else if (field.name == "ExactColor")
            tilemap->exactColor = asBool(value);
        else
            return false;
        return true;
    }

    return false;
}

} // namespace

const ClassDesc* schemaFor(const scene::World& world, InstanceId id)
{
    scene::ClassId classId = world.classOf(id);
    // **Walks up**, so `Part` and `MeshPart` find `BasePart`'s schema. Bounded
    // by the hierarchy's own depth, which the registry guarantees is acyclic.
    for (int guard = 0; guard < 64 && classId != scene::InvalidClass; ++guard) {
        const scene::ClassDescriptor* descriptor = world.classes().find(classId);
        if (descriptor == nullptr) {
            return nullptr;
        }
        const std::string_view name = world.atoms().text(descriptor->name);
        if (const ClassDesc* desc = schemaNamed(name); desc != nullptr) {
            return desc;
        }
        // **An exclusion holds below a replicated ancestor** (NA34): a `Bone`
        // is an `Attachment`, and is not described by its schema.
        if (excludedByName(name))
            return nullptr;
        classId = descriptor->super;
    }
    return nullptr;
}

namespace {

// The class whose fields this one also carries, or null.
[[nodiscard]] const ClassDesc* baseOf(const ClassDesc& desc) noexcept
{
    if (desc.base < 0 || static_cast<usize>(desc.base) >= std::size(generated::Classes))
        return nullptr;
    return &generated::Classes[desc.base];
}

} // namespace

namespace {

// A word at a time, as `hashOf` does an entity's fields.
[[nodiscard]] core::u64 mixWord(core::u64 hash, core::u64 word) noexcept
{
    hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
    return hash ^ (hash >> 29);
}

// A component's bytes, four words abreast: each lane is a chain of its own, so
// the four multiplies of a step do not wait for each other. This is read on
// one machine and compared with what that machine read a tick ago -- it is on
// no wire, and has only to be quick and to notice a change.
[[nodiscard]] core::u64 mixBytes(core::u64 hash, const void* data, usize size) noexcept
{
    const auto* bytes = static_cast<const core::u8*>(data);
    constexpr core::u64 Prime = 0xFF51AFD7ED558CCDull;
    core::u64 lane[4]{hash, hash ^ 0x9E3779B97F4A7C15ull, hash ^ 0xC2B2AE3D27D4EB4Full, hash ^ 0x165667B19E3779F9ull};
    usize at = 0;
    for (; at + 4 * sizeof(core::u64) <= size; at += 4 * sizeof(core::u64)) {
        core::u64 word[4];
        std::memcpy(word, bytes + at, sizeof(word));
        for (int index = 0; index < 4; ++index) {
            lane[index] = (lane[index] ^ word[index]) * Prime;
            lane[index] ^= lane[index] >> 29;
        }
    }
    hash = mixWord(mixWord(mixWord(lane[0], lane[1]), lane[2]), lane[3]);
    for (; at + sizeof(core::u64) <= size; at += sizeof(core::u64)) {
        core::u64 word = 0;
        std::memcpy(&word, bytes + at, sizeof(word));
        hash = mixWord(hash, word);
    }
    if (at < size) {
        core::u64 word = 0;
        std::memcpy(&word, bytes + at, size - at);
        hash = mixWord(hash, word);
    }
    return hash;
}

// One pool's part of an instance's digest. False when the component cannot be
// read as bytes: it holds memory of its own (a list, a string), and what that
// holds can change with every byte of the component the same.
template <class T>
[[nodiscard]] bool digestIn(const scene::ComponentPool<T>& pool, InstanceId id, core::u64& hash) noexcept
{
    if constexpr (!std::is_trivially_copyable_v<T>) {
        (void)pool;
        (void)id;
        (void)hash;
        return false;
    }
    else {
        const T* found = pool.find(id);
        // None is a fact too: a component taken away changes what is read.
        hash = found != nullptr ? mixBytes(hash, found, sizeof(T)) : mixWord(hash, 0x6E6F6E65ull);
        return true;
    }
}

using DigestIn = bool (*)(const scene::World&, InstanceId, core::u64&) noexcept;
struct NamedPool
{
    std::string_view name;
    DigestIn digest;
};
#define ENG_REPLICATION_POOL_DIGEST(name)                                                                              \
    NamedPool{#name, [](const scene::World& world, InstanceId id, core::u64& hash) noexcept -> bool {                  \
                  return digestIn(world.name(), id, hash);                                                             \
              }},
// Every pool `readComponent` reads, by the name the wire schema has for it. A
// field of a pool that is not here makes its class one that is always read
// -- slower, never wrong -- and a test says so.
constexpr NamedPool Pools[] = {
    ENG_REPLICATION_POOL_DIGEST(attachments) ENG_REPLICATION_POOL_DIGEST(constraints) ENG_REPLICATION_POOL_DIGEST(
        movers) ENG_REPLICATION_POOL_DIGEST(welds) ENG_REPLICATION_POOL_DIGEST(noCollisions)
        ENG_REPLICATION_POOL_DIGEST(parts) ENG_REPLICATION_POOL_DIGEST(meshParts) ENG_REPLICATION_POOL_DIGEST(
            rigidBodies) ENG_REPLICATION_POOL_DIGEST(characterBodies) ENG_REPLICATION_POOL_DIGEST(workspaces)
            ENG_REPLICATION_POOL_DIGEST(lighting) ENG_REPLICATION_POOL_DIGEST(postEffects) ENG_REPLICATION_POOL_DIGEST(
                waters) ENG_REPLICATION_POOL_DIGEST(waterWaves) ENG_REPLICATION_POOL_DIGEST(waterPoints)
                ENG_REPLICATION_POOL_DIGEST(clickDetectors) ENG_REPLICATION_POOL_DIGEST(bloomEffects)
                    ENG_REPLICATION_POOL_DIGEST(colorCorrectionEffects) ENG_REPLICATION_POOL_DIGEST(blurEffects)
                        ENG_REPLICATION_POOL_DIGEST(swarms) ENG_REPLICATION_POOL_DIGEST(depthOfFieldEffects)
                            ENG_REPLICATION_POOL_DIGEST(sunRaysEffects) ENG_REPLICATION_POOL_DIGEST(atmospheres)
                                ENG_REPLICATION_POOL_DIGEST(skies) ENG_REPLICATION_POOL_DIGEST(decals)
                                    ENG_REPLICATION_POOL_DIGEST(particleEmitters) ENG_REPLICATION_POOL_DIGEST(models)
                                        ENG_REPLICATION_POOL_DIGEST(parts2d) ENG_REPLICATION_POOL_DIGEST(tilemaps2d)};
#undef ENG_REPLICATION_POOL_DIGEST

// What each class reads from, found once: the distinct pools its fields name,
// its base's and the common set's among them, and whether a part is among
// them. `unknown` when one names a pool that is not in the list above.
struct ClassPools
{
    bool built = false;
    bool unknown = false;
    bool parts = false;
    std::vector<DigestIn> pools;
};

[[nodiscard]] const ClassPools& poolsOf(const ClassDesc& desc)
{
    static std::array<ClassPools, std::size(generated::Classes)> all;
    const auto at = static_cast<usize>(&desc - generated::Classes);
    static ClassPools none{true, true, false, {}};
    if (at >= all.size())
        return none;
    ClassPools& mine = all[at];
    if (mine.built)
        return mine;
    for (usize index = 0, count = fieldCount(desc); index < count; ++index) {
        const FieldDesc* field = fieldAt(desc, index);
        if (field == nullptr || field->source != Source::Component)
            continue;
        const auto known = std::find_if(std::begin(Pools), std::end(Pools),
                                        [&](const NamedPool& pool) { return pool.name == field->pool; });
        if (known == std::end(Pools)) {
            mine.unknown = true;
            continue;
        }
        if (field->pool == "parts")
            mine.parts = true;
        if (std::find(mine.pools.begin(), mine.pools.end(), known->digest) == mine.pools.end())
            mine.pools.push_back(known->digest);
    }
    mine.built = true;
    return mine;
}

} // namespace

bool digestKnowsPoolsOf(const ClassDesc& desc) noexcept
{
    return !poolsOf(desc).unknown;
}

core::u64 sourceDigestOf(const scene::World& world, InstanceId id, const ClassDesc& desc) noexcept
{
    const ClassPools& pools = poolsOf(desc);
    if (pools.unknown)
        return 0;
    core::u64 hash = 0x9E3779B97F4A7C15ull;
    for (const DigestIn digest : pools.pools) {
        if (!digest(world, id, hash))
            return 0;
    }
    // A part wearing a material copy changes when the copy does, and the copy
    // is the world's, in no component.
    if (pools.parts) {
        if (const scene::PartComponent* part = world.parts().find(id); part != nullptr && part->materialClone != 0)
            hash = mixWord(hash, world.materialClonesRevision());
    }
    return hash != 0 ? hash : 1;
}

bool carriesField(const scene::World& world, InstanceId id, std::string_view name)
{
    const ClassDesc* desc = schemaFor(world, id);
    if (desc == nullptr)
        return false;
    for (usize index = 0, count = fieldCount(*desc); index < count; ++index) {
        if (const FieldDesc* field = fieldAt(*desc, index); field != nullptr && field->name == name)
            return true;
    }
    return false;
}

usize fieldCount(const ClassDesc& desc)
{
    const ClassDesc* base = baseOf(desc);
    return std::size(generated::CommonFields) + (base != nullptr ? base->fields.size() : 0) + desc.fields.size();
}

core::u16 wireIdAt(const ClassDesc& desc, usize index)
{
    // **An id is only unique within its own half of the set**, and this is the
    // whole reason this function exists rather than `field->id` at every call
    // site. `api/wire/schema.luau` numbers the common fields from 1 and each
    // class's fields from 1 independently -- so `Name` (common id 1) and
    // `CFrame` (`BasePart` id 1) are the same number, and a decoder matching on
    // the raw id would write a name into a transform.
    //
    // The top bit says which half. Ids are capped at 32767 by that, which is
    // four orders of magnitude more than any class will have, and the wire form
    // is what both ends compare -- so it is the permanent id, not the index.
    //
    // A class that extends another carries a third half, its base's fields,
    // under the next bit down -- so `CharacterBody`'s `VerticalVelocity` (own
    // id 1) and its inherited `CFrame` (base id 1) cannot collide either.
    constexpr core::u16 ClassBit = 0x8000;
    constexpr core::u16 BaseBit = 0x4000;
    const usize common = std::size(generated::CommonFields);
    const ClassDesc* base = baseOf(desc);
    const usize inherited = base != nullptr ? base->fields.size() : 0;
    const FieldDesc* field = fieldAt(desc, index);
    if (field == nullptr) {
        return 0;
    }
    if (index < common)
        return field->id;
    if (index < common + inherited)
        return static_cast<core::u16>(field->id | BaseBit);
    return static_cast<core::u16>(field->id | ClassBit);
}

const FieldDesc* fieldAt(const ClassDesc& desc, usize index)
{
    const usize common = std::size(generated::CommonFields);
    if (index < common) {
        return &generated::CommonFields[index];
    }
    usize own = index - common;
    if (const ClassDesc* base = baseOf(desc); base != nullptr) {
        if (own < base->fields.size())
            return &base->fields[own];
        own -= base->fields.size();
    }
    if (own < desc.fields.size()) {
        return &desc.fields[own];
    }
    return nullptr;
}

bool extractFieldsInto(const scene::World& world, InstanceId id, const ClassDesc& desc, FieldSet& out)
{
    const usize count = fieldCount(desc);
    // Every field starts as zeros, as a fresh set's do: a reader writes what
    // its value takes and leaves the rest.
    out.assign(count, FieldValue{});

    for (usize at = 0; at < count; ++at) {
        const FieldDesc* field = fieldAt(desc, at);
        if (field == nullptr) {
            return false;
        }

        if (field->source == Source::Component) {
            if (!readComponent(world, id, *field, out[at])) {
                return false;
            }
            continue;
        }

        // The common set: what every instance has whatever its class.
        if (field->name == "Name") {
            setU32(out[at], world.name(id).id);
            continue;
        }
        if (field->name == "Parent") {
            // **The parent's own id, resolved to a network id by the caller.**
            // This layer stores the instance id and knows nothing about peers;
            // mapping it is the sender's job, because which network id a peer
            // holds is a fact about that peer.
            setU32(out[at], world.parentOf(id).index);
            continue;
        }
        return false;
    }
    return true;
}

bool extractFields(const scene::World& world, InstanceId id, const ClassDesc& desc, FieldSet& out)
{
    // **Into a scratch and only then into `out`.** A half-filled set diffed
    // against a baseline reports its unread half as changed, every tick, for
    // ever -- so a field that cannot be read is the whole extraction failing
    // rather than a zero nobody notices.
    FieldSet scratch;
    if (!extractFieldsInto(world, id, desc, scratch))
        return false;
    out = std::move(scratch);
    return true;
}

void diffFields(const ClassDesc& desc, std::span<const FieldValue> baseline, std::span<const FieldValue> current,
                std::vector<FieldDelta>& out)
{
    out.clear();

    if (baseline.size() != current.size()) {
        // Everything changed. See the header: this is a baseline taken under a
        // different class, and sending all of it is what makes the replica
        // correct rather than subtly wrong.
        for (usize at = 0; at < current.size(); ++at) {
            out.push_back(FieldDelta{wireIdAt(desc, at), current[at]});
        }
        return;
    }

    for (usize at = 0; at < current.size(); ++at) {
        if (baseline[at] == current[at]) {
            continue;
        }
        out.push_back(FieldDelta{wireIdAt(desc, at), current[at]});
    }
}

namespace {

// The first clear of a join or a scene change that destroys anything drops the
// templates of the scene before it (`ScriptTemplates::dropSceneTemplates`).
struct TemplateTaking
{
    ScriptTemplates* templates = nullptr;
    bool dropped = false;

    void before(scene::World& world, std::span<const InstanceId> doomed)
    {
        if (templates == nullptr || doomed.empty())
            return;
        if (!dropped) {
            templates->dropSceneTemplates(world);
            dropped = true;
        }
        for (const InstanceId id : doomed)
            templates->takeFrom(world, id);
    }
};

usize clearReplicatedTaking(scene::World& world, InstanceId root, TemplateTaking& taking)
{
    std::vector<InstanceId> doomed;
    for (InstanceId child = world.firstChild(root); child.valid(); child = world.nextSibling(child)) {
        if (schemaFor(world, child) != nullptr)
            doomed.push_back(child);
    }
    // **A replica's own scripts are kept out of what goes** (ADR 0138 §6),
    // to be put back under what the authority sends.
    taking.before(world, doomed);
    for (const InstanceId id : doomed)
        (void)world.destroy(id);
    world.retireDestroyed();
    return doomed.size();
}

} // namespace

usize clearReplicated(scene::World& world, InstanceId root, ScriptTemplates* templates)
{
    TemplateTaking taking{templates};
    return clearReplicatedTaking(world, root, taking);
}

namespace {

// Destroys every child of `container`, and says how many.
usize emptyOf(scene::World& world, InstanceId container)
{
    std::vector<InstanceId> doomed;
    for (InstanceId child = world.firstChild(container); child.valid(); child = world.nextSibling(child))
        doomed.push_back(child);
    for (const InstanceId id : doomed)
        (void)world.destroy(id);
    world.retireDestroyed();
    return doomed.size();
}

} // namespace

usize clearForReplica(scene::World& world, InstanceId workspace, ScriptTemplates* templates)
{
    TemplateTaking taking{templates};
    usize cleared = clearReplicatedTaking(world, workspace, taking);
    const InstanceId dataModel = world.parentOf(workspace);
    for (InstanceId service = dataModel.valid() ? world.firstChild(dataModel) : InstanceId{}; service.valid();
         service = world.nextSibling(service)) {
        const scene::ClassDescriptor* descriptor = world.classes().find(world.classOf(service));
        if (descriptor == nullptr)
            continue;
        const std::string_view name = world.atoms().text(descriptor->name);
        // `Lighting`'s children travel too (ADR 0096): the authority's
        // effects, air and sky replace whatever the replica's scene put there.
        if (name == "ReplicatedStorage" || name == "Lighting") {
            cleared += clearReplicatedTaking(world, service, taking);
        }
        else if (name == "ServerStorage" || name == "ServerScriptService") {
            // The server's, and never a replica's (ADR 0080, ADR 0105): what
            // the replica's own package brought here goes, whole.
            cleared += emptyOf(world, service);
        }
        else if (name == "GlobalScriptService") {
            // Its fixed `Server` folder, emptied but kept: the folder is the
            // engine's, and what is inside it is the authority's code.
            if (const InstanceId server = world.findFirstChild(service, world.atoms().lookup("Server")); server.valid())
                cleared += emptyOf(world, server);
        }
    }
    return cleared;
}

bool applyField(scene::World& world, InstanceId id, const ClassDesc& desc, const FieldDelta& delta)
{
    const usize count = fieldCount(desc);
    for (usize at = 0; at < count; ++at) {
        const FieldDesc* field = fieldAt(desc, at);
        if (field == nullptr || wireIdAt(desc, at) != delta.id) {
            continue;
        }

        if (field->source == Source::Component) {
            return writeComponent(world, id, *field, delta.value);
        }

        if (field->name == "Name") {
            world.setName(id, core::NameAtom{asU32(delta.value)});
            return true;
        }
        if (field->name == "Parent") {
            // Resolved by the caller, which is the only layer that knows which
            // network id means which local instance.
            return true;
        }
        return false;
    }
    return false;
}

} // namespace engine::replication
