// **One clip on bodies of other proportions** (ADR 0199): what each joint of a
// rig IS, and the map that carries a clip made on one humanoid rig onto
// another. The arithmetic and nothing of the world -- `AnimationSystem` builds
// a map a pair of rigs and asks it two things a channel; a test hands it
// invented rigs.
//
// A clip is carried across by roles, not by names, and as turns from rest, not
// as transforms. A joint's role is read off its name against lists of the
// names rigs really arrive with (the tables at the top of the .cpp), put right
// by a `<model>.rig.json` where a guess is wrong. For a pair of joints with
// one role, what crosses is how far the clip turns the source joint from its
// rest, measured in the model's space, so a bone that rolls differently about
// its own length or was drawn along another axis turns the same way in the
// world; the two rigs' stances are compared once and their difference taken
// out; and the hips' travel is scaled by the two rigs' legs. Every other
// translation and every scale is the target's own.
//
// **The simulation's side of the hash (R10).** Everything here is a pure
// function of the two rigs: additions, multiplications, divisions and square
// roots, none of the platform's transcendentals, and no container whose order
// is not the rig's own.
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/model.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::render::retarget {

// What a joint is in a body. The left and right of a limb are two roles, and a
// rig has at most one joint a role.
//
// The spine is up to three: `Spine` is the first joint above the hips,
// `Chest` the next, `UpperChest` the last below the neck. A rig with one has
// only `Spine`; one with more than three leaves those between `Chest` and
// `UpperChest` without a role.
//
// A finger is three joints, 1 nearest the palm. The joints inside the palm
// that some rigs have, and the tips that some exporters add, have no role.
enum class Role : core::u8
{
    None,
    Hips,
    Spine,
    Chest,
    UpperChest,
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToes,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToes,
    LeftThumb1,
    LeftThumb2,
    LeftThumb3,
    LeftIndex1,
    LeftIndex2,
    LeftIndex3,
    LeftMiddle1,
    LeftMiddle2,
    LeftMiddle3,
    LeftRing1,
    LeftRing2,
    LeftRing3,
    LeftLittle1,
    LeftLittle2,
    LeftLittle3,
    RightThumb1,
    RightThumb2,
    RightThumb3,
    RightIndex1,
    RightIndex2,
    RightIndex3,
    RightMiddle1,
    RightMiddle2,
    RightMiddle3,
    RightRing1,
    RightRing2,
    RightRing3,
    RightLittle1,
    RightLittle2,
    RightLittle3,
    Count,
};

inline constexpr core::usize RoleCount = static_cast<core::usize>(Role::Count);

// The role as it is written in a `.rig.json` and shown in the editor:
// "LeftUpperArm". Empty for `Count` and anything past it.
[[nodiscard]] std::string_view roleName(Role role) noexcept;

// The other way, exactly as `roleName` writes it. `None` for anything else --
// and for "None" itself, which is not a role a file may give a joint.
[[nodiscard]] Role roleFromName(std::string_view name) noexcept;

// One line of a rig's own say: this joint is that role. An empty `joint` says
// the rig has NO joint for the role, which is how a guess that should have
// found nothing is put right.
struct RoleOverride
{
    Role role = Role::None;
    std::string joint;
};

// Reads a `<model>.rig.json`:
//
//     { "format": "rig", "version": 1,
//       "roles": { "LeftUpperArm": "Bip01_L_UpperArm", "LeftToes": "" } }
//
// In the order the file writes them. Nullopt for a file that is not that, and
// `error` (when given) then says what was wrong, naming the role: one this
// engine has no such role as, or one whose joint is not a string. The text is
// a developer's diagnostic in the way the JSON reader's own is; a caller that
// shows it puts it inside a keyed message.
[[nodiscard]] std::optional<std::vector<RoleOverride>> readRigRoles(std::string_view json,
                                                                    std::string* error = nullptr);

// A rig's roles, both ways round.
struct RigRoles
{
    // A role, or `None`, for every joint.
    std::vector<Role> ofJoint;
    // A joint, or -1, for every role. `jointOf[None]` is -1.
    std::array<core::i32, RoleCount> jointOf{};

    RigRoles() noexcept { jointOf.fill(-1); }

    [[nodiscard]] core::i32 joint(Role role) const noexcept
    {
        return static_cast<core::usize>(role) < RoleCount ? jointOf[static_cast<core::usize>(role)] : -1;
    }

    // Enough of a body to be retargeted by roles: hips, both upper and lower
    // legs, both upper and lower arms, and a spine or a chest. A tail, a
    // shirt's few joints or a creature is not, and plays by equal names.
    [[nodiscard]] bool body() const noexcept;
};

// Gives every joint of `joints` its role, from its name and then from where
// it stands in the rig; `overrides` are applied last, each named joint taking
// its role from whoever was guessed for it. An override naming a joint the
// rig does not have is ignored.
//
// `joints` are parents first, as the loader leaves them. The answer does not
// depend on the order siblings come in.
[[nodiscard]] RigRoles assignRoles(std::span<const asset::Joint> joints, std::span<const RoleOverride> overrides = {});

// How a clip made on one rig (the SOURCE) is put on another (the TARGET).
// Every array is indexed by the SOURCE's joints, which is how a clip's
// channels name them.
struct Map
{
    // The target joint a source joint drives, or -1 for one the target has
    // nothing for.
    std::vector<core::i32> slots;
    // 1: the pair was matched by role. Its rotation goes through `rotation`,
    // and its translation and scale channels are DROPPED -- the target keeps
    // its own rest offsets, which are its own bone lengths -- but for the
    // translation of `hips`, which goes through `hipsTranslation`.
    // 0: matched by equal name, and every sample is applied as it is.
    std::vector<core::u8> byRole;
    // Four floats (x, y, z, w) a source joint: for a `byRole` joint the target's
    // rotation is `pre * sample * post`. The identity for the others.
    std::vector<core::f32> pre;
    std::vector<core::f32> post;
    // Four floats a source joint: what `rotation` answers for the source's own
    // rest, which is the target's rest turned into the source's stance. A
    // `byRole` joint a clip has NO rotation channel for is given this, or it
    // would stand in the target's stance under a parent standing in the
    // source's. The identity for the others.
    std::vector<core::f32> rest;

    // The source joint whose translation is carried (its hips), or -1.
    core::i32 hips = -1;
    // The two hips' rest translations from their parents, and the target's
    // leg length over the source's. The turn between the two parents' frames
    // is `pre` of `hips`.
    core::DVec3 hipsSourceRest;
    core::DVec3 hipsTargetRest;
    core::f64 hipsScale = 1.0;

    // The roles the source has a joint for and the target has none, each once,
    // in `Role`'s order: what a warning names.
    std::vector<Role> unmapped;
    // Whether anything here is by role: both rigs are bodies. False is the map
    // by equal names alone, as it was before there were roles.
    bool roles = false;
};

// Built once a pair of rigs. `sourceRoles` and `targetRoles` are what
// `assignRoles` answered for the same two joint lists.
//
// When either rig is not a body, every source joint maps to the target joint
// of the equal name and nothing else is filled in. When both are, a source
// joint with a role maps to the target joint with that role, and a source
// joint with none maps to the target joint of the equal name that has none
// either -- a tail, a cape, the spine joints between `Chest` and `UpperChest`.
[[nodiscard]] Map buildMap(std::span<const asset::Joint> source, const RigRoles& sourceRoles,
                           std::span<const asset::Joint> target, const RigRoles& targetRoles);

// A rotation sample of `sourceJoint` (x, y, z, w, relative to its parent, as a
// clip keys it) as the target joint's. Two quaternion products for a `byRole`
// joint and a copy for any other. `out` may be `sample`.
void rotation(const Map& map, core::u32 sourceJoint, const core::f32 sample[4], core::f32 out[4]) noexcept;

// A translation sample of the source's hips as the target hips' translation
// from its parent: the travel from rest, turned into the target parent's
// frame and scaled by the legs. `sample` itself when the map carries no hips.
[[nodiscard]] core::DVec3 hipsTranslation(const Map& map, core::DVec3 sample) noexcept;

} // namespace engine::render::retarget
