// Retargeting: roles from names, and one clip on two bodies (ADR 0199).
//
// The first half feeds whole humanoid joint lists, one a family of names rigs
// arrive with, and holds every joint to the role it must get. The lists are
// written from what those rigs are known to call their joints, not read out of
// files; a family whose real export differs is one line in the tables of
// retarget.cpp and one line here.
//
// The second half is two rigs made to differ in every way that matters --
// other names, bones drawn along another axis and rolled about their own
// length, another stance, legs half as long, a root turned on its side above
// the hips -- and a pose carried from one to the other, read back in the
// model's space with the engine's own matrices.
#include <algorithm>
#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/render/retarget.h"

using namespace engine;
using engine::core::f32;
using engine::core::f64;
using engine::core::i32;
using engine::core::u32;
using engine::core::usize;
using engine::core::Vec3;
using engine::render::retarget::Role;
namespace retarget = engine::render::retarget;

namespace {

// --- Rigs by name ----------------------------------------------------------------

struct Named
{
    std::string name;
    // Empty for a root.
    std::string parent;
    Role role;
};

std::vector<asset::Joint> jointsOf(const std::vector<Named>& rig)
{
    std::vector<asset::Joint> joints;
    for (const Named& entry : rig) {
        asset::Joint joint;
        joint.name = entry.name;
        for (usize other = 0; other < joints.size(); ++other) {
            if (!entry.parent.empty() && joints[other].name == entry.parent)
                joint.parent = static_cast<u32>(other);
        }
        REQUIRE((entry.parent.empty() || joint.parent != asset::Joint::NoParent));
        joints.push_back(joint);
    }
    return joints;
}

// The same rig with every set of siblings in the opposite order: by depth,
// and within one depth back to front. Parents still come first.
std::vector<Named> siblingsReversed(const std::vector<Named>& rig)
{
    const std::vector<asset::Joint> joints = jointsOf(rig);
    std::vector<u32> depth(joints.size(), 0);
    for (usize joint = 0; joint < joints.size(); ++joint) {
        if (joints[joint].parent != asset::Joint::NoParent)
            depth[joint] = depth[joints[joint].parent] + 1;
    }
    std::vector<usize> order(joints.size());
    for (usize joint = 0; joint < order.size(); ++joint)
        order[joint] = joint;
    std::sort(order.begin(), order.end(),
              [&depth](usize a, usize b) { return depth[a] != depth[b] ? depth[a] < depth[b] : a > b; });
    std::vector<Named> out;
    for (const usize joint : order)
        out.push_back(rig[joint]);
    return out;
}

Role finger(bool left, u32 which, u32 joint)
{
    const u32 first = static_cast<u32>(left ? Role::LeftThumb1 : Role::RightThumb1);
    return static_cast<Role>(first + which * 3 + joint);
}

Role sided(bool left, Role leftRole, Role rightRole)
{
    return left ? leftRole : rightRole;
}

// Every joint has the role the list says, the two directions of the answer
// agree, and -- for a whole humanoid -- no role is left without a joint.
void checkRoles(const std::vector<Named>& rig, bool everyRole)
{
    const std::vector<asset::Joint> joints = jointsOf(rig);
    const retarget::RigRoles roles = retarget::assignRoles(joints);
    REQUIRE(roles.ofJoint.size() == joints.size());
    for (usize joint = 0; joint < joints.size(); ++joint) {
        INFO("joint " << rig[joint].name << " is " << std::string(retarget::roleName(roles.ofJoint[joint]))
                      << ", wanted " << std::string(retarget::roleName(rig[joint].role)));
        CHECK(roles.ofJoint[joint] == rig[joint].role);
        if (roles.ofJoint[joint] != Role::None)
            CHECK(roles.joint(roles.ofJoint[joint]) == static_cast<i32>(joint));
    }
    CHECK(roles.joint(Role::None) == -1);
    if (everyRole) {
        for (usize role = 1; role < retarget::RoleCount; ++role) {
            INFO("role " << std::string(retarget::roleName(static_cast<Role>(role))));
            CHECK(roles.jointOf[role] >= 0);
        }
        CHECK(roles.body());
    }

    // Whatever order siblings come in.
    const std::vector<Named> reversed = siblingsReversed(rig);
    const std::vector<asset::Joint> reversedJoints = jointsOf(reversed);
    const retarget::RigRoles again = retarget::assignRoles(reversedJoints);
    for (usize joint = 0; joint < reversed.size(); ++joint) {
        INFO("joint " << reversed[joint].name << ", siblings reversed");
        CHECK(again.ofJoint[joint] == reversed[joint].role);
    }
}

Role roleOfName(std::string name)
{
    asset::Joint joint;
    joint.name = std::move(name);
    const std::vector<asset::Joint> joints{joint};
    return retarget::assignRoles(joints).ofJoint[0];
}

// The common free motion library's rig, with whatever stands before each name.
std::vector<Named> motionLibraryRig(const std::string& prefix)
{
    const auto n = [&prefix](const std::string& name) { return prefix + name; };
    std::vector<Named> rig{
        {n("Hips"), "", Role::Hips},
        {n("Spine"), n("Hips"), Role::Spine},
        {n("Spine1"), n("Spine"), Role::Chest},
        {n("Spine2"), n("Spine1"), Role::UpperChest},
        {n("Neck"), n("Spine2"), Role::Neck},
        {n("Head"), n("Neck"), Role::Head},
        {n("HeadTop_End"), n("Head"), Role::None},
    };
    const char* const fingers[5] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
    for (const bool left : {true, false}) {
        const std::string side = left ? "Left" : "Right";
        rig.push_back({n(side + "Shoulder"), n("Spine2"), sided(left, Role::LeftShoulder, Role::RightShoulder)});
        rig.push_back({n(side + "Arm"), n(side + "Shoulder"), sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back({n(side + "ForeArm"), n(side + "Arm"), sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({n(side + "Hand"), n(side + "ForeArm"), sided(left, Role::LeftHand, Role::RightHand)});
        for (u32 which = 0; which < 5; ++which) {
            std::string parent = n(side + "Hand");
            for (u32 joint = 0; joint < 4; ++joint) {
                // The fourth is the tip the exporter closes the chain with.
                const std::string name = n(side + "Hand" + fingers[which] + std::to_string(joint + 1));
                rig.push_back({name, parent, joint < 3 ? finger(left, which, joint) : Role::None});
                parent = name;
            }
        }
        rig.push_back({n(side + "UpLeg"), n("Hips"), sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({n(side + "Leg"), n(side + "UpLeg"), sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({n(side + "Foot"), n(side + "Leg"), sided(left, Role::LeftFoot, Role::RightFoot)});
        rig.push_back({n(side + "ToeBase"), n(side + "Foot"), sided(left, Role::LeftToes, Role::RightToes)});
        rig.push_back({n(side + "Toe_End"), n(side + "ToeBase"), Role::None});
    }
    return rig;
}

// One big engine's humanoid, as its export names the joints.
std::vector<Named> humanoidExportRig()
{
    std::vector<Named> rig{
        {"Hips", "", Role::Hips},           {"Spine", "Hips", Role::Spine},
        {"Chest", "Spine", Role::Chest},    {"UpperChest", "Chest", Role::UpperChest},
        {"Neck", "UpperChest", Role::Neck}, {"Head", "Neck", Role::Head},
        {"LeftEye", "Head", Role::None},    {"RightEye", "Head", Role::None},
        {"Jaw", "Head", Role::None},
    };
    const char* const fingers[5] = {"Thumb", "Index", "Middle", "Ring", "Little"};
    const char* const joints[3] = {"Proximal", "Intermediate", "Distal"};
    for (const bool left : {true, false}) {
        const std::string side = left ? "Left" : "Right";
        rig.push_back({side + "Shoulder", "UpperChest", sided(left, Role::LeftShoulder, Role::RightShoulder)});
        rig.push_back({side + "UpperArm", side + "Shoulder", sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back({side + "LowerArm", side + "UpperArm", sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({side + "Hand", side + "LowerArm", sided(left, Role::LeftHand, Role::RightHand)});
        for (u32 which = 0; which < 5; ++which) {
            std::string parent = side + "Hand";
            for (u32 joint = 0; joint < 3; ++joint) {
                const std::string name = side + " " + fingers[which] + " " + joints[joint];
                rig.push_back({name, parent, finger(left, which, joint)});
                parent = name;
            }
        }
        rig.push_back({side + "UpperLeg", "Hips", sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({side + "LowerLeg", side + "UpperLeg", sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({side + "Foot", side + "LowerLeg", sided(left, Role::LeftFoot, Role::RightFoot)});
        rig.push_back({side + "Toes", side + "Foot", sided(left, Role::LeftToes, Role::RightToes)});
    }
    return rig;
}

// The other big engine's mannequin, helpers and all.
std::vector<Named> mannequinRig()
{
    std::vector<Named> rig{
        {"root", "", Role::None},
        {"pelvis", "root", Role::Hips},
        {"spine_01", "pelvis", Role::Spine},
        {"spine_02", "spine_01", Role::Chest},
        {"spine_03", "spine_02", Role::UpperChest},
        {"neck_01", "spine_03", Role::Neck},
        {"head", "neck_01", Role::Head},
        {"ik_foot_root", "root", Role::None},
        {"ik_foot_l", "ik_foot_root", Role::None},
        {"ik_foot_r", "ik_foot_root", Role::None},
        {"ik_hand_root", "root", Role::None},
        {"ik_hand_gun", "ik_hand_root", Role::None},
        {"ik_hand_l", "ik_hand_gun", Role::None},
        {"ik_hand_r", "ik_hand_gun", Role::None},
    };
    const char* const fingers[5] = {"thumb", "index", "middle", "ring", "pinky"};
    for (const bool left : {true, false}) {
        const std::string side = left ? "_l" : "_r";
        rig.push_back({"clavicle" + side, "spine_03", sided(left, Role::LeftShoulder, Role::RightShoulder)});
        rig.push_back({"upperarm" + side, "clavicle" + side, sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back({"upperarm_twist_01" + side, "upperarm" + side, Role::None});
        rig.push_back({"lowerarm" + side, "upperarm" + side, sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({"lowerarm_twist_01" + side, "lowerarm" + side, Role::None});
        rig.push_back({"hand" + side, "lowerarm" + side, sided(left, Role::LeftHand, Role::RightHand)});
        for (u32 which = 0; which < 5; ++which) {
            std::string parent = "hand" + side;
            if (which > 0) {
                // The joint inside the palm the newer mannequin has.
                const std::string palm = std::string(fingers[which]) + "_metacarpal" + side;
                rig.push_back({palm, parent, Role::None});
                parent = palm;
            }
            for (u32 joint = 0; joint < 3; ++joint) {
                const std::string name = std::string(fingers[which]) + "_0" + std::to_string(joint + 1) + side;
                rig.push_back({name, parent, finger(left, which, joint)});
                parent = name;
            }
        }
        rig.push_back({"thigh" + side, "pelvis", sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({"thigh_twist_01" + side, "thigh" + side, Role::None});
        rig.push_back({"calf" + side, "thigh" + side, sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({"calf_twist_01" + side, "calf" + side, Role::None});
        rig.push_back({"foot" + side, "calf" + side, sided(left, Role::LeftFoot, Role::RightFoot)});
        rig.push_back({"ball" + side, "foot" + side, sided(left, Role::LeftToes, Role::RightToes)});
    }
    return rig;
}

// The common Blender rig's deform bones, with whatever stands before each.
std::vector<Named> blenderRig(const std::string& prefix)
{
    const auto n = [&prefix](const std::string& name) { return prefix + name; };
    std::vector<Named> rig{
        {n("hips"), "", Role::Hips},
        {n("spine"), n("hips"), Role::Spine},
        {n("spine.001"), n("spine"), Role::Chest},
        {n("chest"), n("spine.001"), Role::UpperChest},
        {n("neck"), n("chest"), Role::Neck},
        {n("head"), n("neck"), Role::Head},
    };
    const char* const fingers[5] = {"thumb", "f_index", "f_middle", "f_ring", "f_pinky"};
    for (const bool left : {true, false}) {
        const std::string side = left ? ".L" : ".R";
        rig.push_back({n("shoulder" + side), n("chest"), sided(left, Role::LeftShoulder, Role::RightShoulder)});
        rig.push_back(
            {n("upper_arm" + side), n("shoulder" + side), sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        // The second half of a limb that is skinned in two pieces.
        rig.push_back({n("upper_arm" + side + ".001"), n("upper_arm" + side), Role::None});
        rig.push_back({n("forearm" + side), n("upper_arm" + side + ".001"),
                       sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({n("forearm" + side + ".001"), n("forearm" + side), Role::None});
        rig.push_back({n("hand" + side), n("forearm" + side + ".001"), sided(left, Role::LeftHand, Role::RightHand)});
        for (u32 which = 0; which < 5; ++which) {
            std::string parent = n("hand" + side);
            if (which > 0) {
                const std::string palm = n("palm.0" + std::to_string(which) + side);
                rig.push_back({palm, parent, Role::None});
                parent = palm;
            }
            for (u32 joint = 0; joint < 3; ++joint) {
                const std::string name = n(std::string(fingers[which]) + ".0" + std::to_string(joint + 1) + side);
                rig.push_back({name, parent, finger(left, which, joint)});
                parent = name;
            }
        }
        rig.push_back({n("thigh" + side), n("hips"), sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({n("thigh" + side + ".001"), n("thigh" + side), Role::None});
        rig.push_back(
            {n("shin" + side), n("thigh" + side + ".001"), sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({n("foot" + side), n("shin" + side), sided(left, Role::LeftFoot, Role::RightFoot)});
        rig.push_back({n("toe" + side), n("foot" + side), sided(left, Role::LeftToes, Role::RightToes)});
    }
    return rig;
}

// The 3ds-style biped.
std::vector<Named> bipedRig(const std::string& prefix, char separator)
{
    const auto n = [&prefix, separator](std::string name) {
        std::replace(name.begin(), name.end(), ' ', separator);
        return prefix + name;
    };
    std::vector<Named> rig{
        {prefix, "", Role::None},
        {n(" Pelvis"), prefix, Role::Hips},
        {n(" Spine"), n(" Pelvis"), Role::Spine},
        {n(" Spine1"), n(" Spine"), Role::Chest},
        {n(" Spine2"), n(" Spine1"), Role::UpperChest},
        {n(" Neck"), n(" Spine2"), Role::Neck},
        {n(" Head"), n(" Neck"), Role::Head},
        {n(" HeadNub"), n(" Head"), Role::None},
        {n(" Footsteps"), prefix, Role::None},
    };
    for (const bool left : {true, false}) {
        const std::string side = left ? " L " : " R ";
        rig.push_back({n(side + "Clavicle"), n(" Neck"), sided(left, Role::LeftShoulder, Role::RightShoulder)});
        rig.push_back(
            {n(side + "UpperArm"), n(side + "Clavicle"), sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back(
            {n(side + "Forearm"), n(side + "UpperArm"), sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({n(side + "ForeTwist"), n(side + "Forearm"), Role::None});
        rig.push_back({n(side + "Hand"), n(side + "Forearm"), sided(left, Role::LeftHand, Role::RightHand)});
        for (u32 which = 0; which < 5; ++which) {
            // `Finger0`, `Finger01`, `Finger02`, and a nub to close it.
            const std::string base = side + "Finger" + std::to_string(which);
            rig.push_back({n(base), n(side + "Hand"), finger(left, which, 0)});
            rig.push_back({n(base + "1"), n(base), finger(left, which, 1)});
            rig.push_back({n(base + "2"), n(base + "1"), finger(left, which, 2)});
            rig.push_back({n(base + "Nub"), n(base + "2"), Role::None});
        }
        // The classic biped hangs its legs from the first joint of the spine.
        rig.push_back({n(side + "Thigh"), n(" Spine"), sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({n(side + "Calf"), n(side + "Thigh"), sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({n(side + "Foot"), n(side + "Calf"), sided(left, Role::LeftFoot, Role::RightFoot)});
        rig.push_back({n(side + "Toe0"), n(side + "Foot"), sided(left, Role::LeftToes, Role::RightToes)});
        rig.push_back({n(side + "Toe0Nub"), n(side + "Toe0"), Role::None});
    }
    return rig;
}

// --- Rigs with a shape -------------------------------------------------------------

constexpr f32 Pi = 3.14159265358979f;

bool nearly(Vec3 a, Vec3 b, f32 epsilon) noexcept
{
    return std::fabs(a.x - b.x) <= epsilon && std::fabs(a.y - b.y) <= epsilon && std::fabs(a.z - b.z) <= epsilon;
}

// The shortest turn from one direction to another, the test's own.
core::Mat3 arc(Vec3 from, Vec3 to)
{
    const Vec3 a = core::normalize(from);
    const Vec3 b = core::normalize(to);
    const Vec3 axis = core::cross(a, b);
    const f32 sine = core::length(axis);
    const f32 cosine = core::dot(a, b);
    if (sine < 1.0e-6f) {
        if (cosine > 0.0f)
            return {};
        const Vec3 other = std::fabs(a.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
        return core::fromAxisAngle(core::cross(a, other), Pi);
    }
    return core::fromAxisAngle(axis, std::atan2(sine, cosine));
}

// A joint's rotation in the model's space: its own `axis` laid on `towards`,
// then rolled about that direction.
core::Mat3 aim(Vec3 axis, Vec3 towards, f32 roll)
{
    return core::fromAxisAngle(towards, roll) * arc(axis, towards);
}

struct Shaped
{
    std::string name;
    // Empty for a root.
    std::string parent;
    // Both in the MODEL's space; the rig's own rest transforms are derived.
    core::DVec3 at;
    core::Mat3 turned;
};

i32 find(const std::vector<asset::Joint>& joints, std::string_view name)
{
    for (usize joint = 0; joint < joints.size(); ++joint) {
        if (joints[joint].name == name)
            return static_cast<i32>(joint);
    }
    return -1;
}

usize at(const std::vector<asset::Joint>& joints, std::string_view name)
{
    const i32 joint = find(joints, name);
    REQUIRE(joint >= 0);
    return static_cast<usize>(joint);
}

std::vector<asset::Joint> rigOf(const std::vector<Shaped>& shape)
{
    std::vector<asset::Joint> joints;
    std::vector<core::CFrameD> model;
    for (const Shaped& entry : shape) {
        asset::Joint joint;
        joint.name = entry.name;
        const core::CFrameD frame{entry.at, entry.turned};
        if (entry.parent.empty()) {
            joint.localBind = frame;
        }
        else {
            const usize parent = at(joints, entry.parent);
            joint.parent = static_cast<u32>(parent);
            joint.localBind = core::inverse(model[parent]) * frame;
        }
        joints.push_back(joint);
        model.push_back(frame);
    }
    return joints;
}

core::DVec3 along(core::DVec3 from, Vec3 towards, f64 length)
{
    const Vec3 unit = core::normalize(towards);
    return core::DVec3{from.x + static_cast<f64>(unit.x) * length, from.y + static_cast<f64>(unit.y) * length,
                       from.z + static_cast<f64>(unit.z) * length};
}

constexpr Vec3 Up{0.0f, 1.0f, 0.0f};
constexpr Vec3 Down{0.0f, -1.0f, 0.0f};
constexpr Vec3 Left{1.0f, 0.0f, 0.0f};
constexpr Vec3 Right{-1.0f, 0.0f, 0.0f};
constexpr Vec3 Forward{0.0f, 0.0f, 1.0f};

// The SOURCE: the motion library's names, standing in a T, every bone drawn
// along its joint's own +Y with no roll, hips at a metre, legs of 0.85.
std::vector<asset::Joint> sourceRig()
{
    constexpr Vec3 Y{0.0f, 1.0f, 0.0f};
    std::vector<Shaped> shape{
        {"mixamorig:Hips", "", {0.0, 1.0, 0.0}, aim(Y, Up, 0.0f)},
        {"mixamorig:Spine", "mixamorig:Hips", {0.0, 1.1, 0.0}, aim(Y, Up, 0.0f)},
        {"mixamorig:Spine1", "mixamorig:Spine", {0.0, 1.25, 0.0}, aim(Y, Up, 0.0f)},
        {"mixamorig:Spine2", "mixamorig:Spine1", {0.0, 1.4, 0.0}, aim(Y, Up, 0.0f)},
        {"mixamorig:Neck", "mixamorig:Spine2", {0.0, 1.55, 0.0}, aim(Y, Up, 0.0f)},
        {"mixamorig:Head", "mixamorig:Neck", {0.0, 1.65, 0.0}, aim(Y, Up, 0.0f)},
        {"Tail", "mixamorig:Hips", {0.0, 1.0, -0.1}, core::Mat3{}},
    };
    for (const bool left : {true, false}) {
        const std::string side = left ? "mixamorig:Left" : "mixamorig:Right";
        const Vec3 out = left ? Left : Right;
        const f64 x = left ? 1.0 : -1.0;
        shape.push_back({side + "Shoulder", "mixamorig:Spine2", {0.05 * x, 1.5, 0.0}, aim(Y, out, 0.0f)});
        shape.push_back({side + "Arm", side + "Shoulder", {0.2 * x, 1.5, 0.0}, aim(Y, out, 0.0f)});
        shape.push_back({side + "ForeArm", side + "Arm", {0.5 * x, 1.5, 0.0}, aim(Y, out, 0.0f)});
        shape.push_back({side + "Hand", side + "ForeArm", {0.8 * x, 1.5, 0.0}, aim(Y, out, 0.0f)});
        shape.push_back({side + "UpLeg", "mixamorig:Hips", {0.1 * x, 0.95, 0.0}, aim(Y, Down, 0.0f)});
        shape.push_back({side + "Leg", side + "UpLeg", {0.1 * x, 0.5, 0.0}, aim(Y, Down, 0.0f)});
        shape.push_back({side + "Foot", side + "Leg", {0.1 * x, 0.1, 0.0}, aim(Y, Forward, 0.0f)});
        shape.push_back({side + "ToeBase", side + "Foot", {0.1 * x, 0.0, 0.15}, aim(Y, Forward, 0.0f)});
    }
    return rigOf(shape);
}

constexpr f64 TargetUpperArm = 0.25;
constexpr f64 TargetLowerArm = 0.2;
constexpr f64 TargetThigh = 0.225;
constexpr f64 TargetShin = 0.2;

// The TARGET: the mannequin's names, standing in an A with its legs a little
// apart, every bone drawn along its joint's own X (+X on the left and the
// middle, -X on the right) and rolled about its own length by another angle a
// joint; legs of 0.425, half the source's; a shorter back of two joints; no
// toes; and a root above the hips lying on its side.
std::vector<asset::Joint> targetRig()
{
    constexpr Vec3 X{1.0f, 0.0f, 0.0f};
    constexpr Vec3 MinusX{-1.0f, 0.0f, 0.0f};
    std::vector<Shaped> shape{
        {"root", "", {0.0, 0.0, 0.0}, core::rotationX(-Pi * 0.5f)},
        {"pelvis", "root", {0.0, 0.55, 0.0}, aim(X, Up, 0.3f)},
        {"spine_01", "pelvis", {0.0, 0.65, 0.0}, aim(X, Up, -0.4f)},
        {"spine_02", "spine_01", {0.0, 0.85, 0.0}, aim(X, Up, 0.2f)},
        {"neck_01", "spine_02", {0.0, 1.05, 0.0}, aim(X, Up, 0.1f)},
        {"head", "neck_01", {0.0, 1.15, 0.0}, aim(X, Up, 0.0f)},
        {"Tail", "pelvis", {0.0, 0.55, -0.1}, core::Mat3{}},
    };
    for (const bool left : {true, false}) {
        const std::string side = left ? "_l" : "_r";
        const Vec3 axis = left ? X : MinusX;
        const f32 x = left ? 1.0f : -1.0f;
        const f64 xd = left ? 1.0 : -1.0;
        const f32 roll = left ? 1.0f : -0.7f;
        const Vec3 out = left ? Left : Right;
        const Vec3 arm{0.70710678f * x, -0.70710678f, 0.0f};
        const Vec3 leg{0.15f * x, -1.0f, 0.0f};

        const core::DVec3 shoulder{0.03 * xd, 1.0, 0.0};
        const core::DVec3 upperArm{0.15 * xd, 1.0, 0.0};
        const core::DVec3 lowerArm = along(upperArm, arm, TargetUpperArm);
        const core::DVec3 hand = along(lowerArm, arm, TargetLowerArm);
        shape.push_back({"clavicle" + side, "spine_02", shoulder, aim(axis, out, 0.5f * roll)});
        shape.push_back({"upperarm" + side, "clavicle" + side, upperArm, aim(axis, arm, 0.7f * roll)});
        shape.push_back(
            {"upperarm_twist_01" + side, "upperarm" + side, along(upperArm, arm, 0.1), aim(axis, arm, 0.7f * roll)});
        shape.push_back({"lowerarm" + side, "upperarm" + side, lowerArm, aim(axis, arm, -0.6f * roll)});
        shape.push_back({"hand" + side, "lowerarm" + side, hand, aim(axis, arm, 1.1f * roll)});

        const core::DVec3 thigh{0.08 * xd, 0.5, 0.0};
        const core::DVec3 calf = along(thigh, leg, TargetThigh);
        const core::DVec3 foot = along(calf, leg, TargetShin);
        shape.push_back({"thigh" + side, "pelvis", thigh, aim(axis, leg, 0.8f * roll)});
        shape.push_back({"calf" + side, "thigh" + side, calf, aim(axis, leg, -0.2f * roll)});
        shape.push_back({"foot" + side, "calf" + side, foot, aim(axis, Forward, 0.3f * roll)});
    }
    return rigOf(shape);
}

// --- A pose, and where it puts a rig -------------------------------------------------

// What a clip says a tick: each joint's rotation and translation from its
// parent, which the pose walk composes parents first.
struct Local
{
    std::vector<std::array<f32, 4>> rotation;
    std::vector<core::DVec3> translation;
};

std::array<f32, 4> quaternionOf(const core::Mat3& rotation)
{
    std::array<f32, 4> q{0.0f, 0.0f, 0.0f, 1.0f};
    core::toQuaternion(rotation, q[0], q[1], q[2], q[3]);
    return q;
}

Local restOf(const std::vector<asset::Joint>& joints)
{
    Local local;
    for (const asset::Joint& joint : joints) {
        local.rotation.push_back(quaternionOf(joint.localBind.rotation));
        local.translation.push_back(joint.localBind.position);
    }
    return local;
}

// Each joint in the model's space, as `rebuildPose` resolves it.
std::vector<core::CFrameD> modelOf(const std::vector<asset::Joint>& joints, const Local& local)
{
    std::vector<core::CFrameD> model;
    for (usize joint = 0; joint < joints.size(); ++joint) {
        const std::array<f32, 4>& q = local.rotation[joint];
        const core::CFrameD frame{local.translation[joint], core::fromQuaternion(q[0], q[1], q[2], q[3])};
        model.push_back(joints[joint].parent == asset::Joint::NoParent ? frame : model[joints[joint].parent] * frame);
    }
    return model;
}

// A pose of the source put on the target the way a caller of the map does it:
// a rotation through `rotation`; a translation dropped for a pair matched by
// role, but the hips', which goes through `hipsTranslation`; everything as it
// is for a pair matched by name.
Local carried(const retarget::Map& map, const Local& pose, const std::vector<asset::Joint>& target)
{
    Local out = restOf(target);
    for (usize joint = 0; joint < map.slots.size(); ++joint) {
        if (map.slots[joint] < 0)
            continue;
        const usize slot = static_cast<usize>(map.slots[joint]);
        retarget::rotation(map, static_cast<u32>(joint), pose.rotation[joint].data(), out.rotation[slot].data());
        if (map.byRole[joint] == 0)
            out.translation[slot] = pose.translation[joint];
        else if (static_cast<i32>(joint) == map.hips)
            out.translation[slot] = retarget::hipsTranslation(map, pose.translation[joint]);
    }
    return out;
}

Vec3 direction(const std::vector<core::CFrameD>& model, usize from, usize to)
{
    return core::normalize(core::toVec3(model[to].position - model[from].position));
}

f64 distance(const std::vector<core::CFrameD>& model, usize from, usize to)
{
    const core::DVec3 delta = model[to].position - model[from].position;
    return std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
}

bool sameRotation(const core::Mat3& a, const core::Mat3& b, f32 epsilon) noexcept
{
    for (usize column = 0; column < 3; ++column) {
        for (usize row = 0; row < 3; ++row) {
            if (std::fabs(a.m[column][row] - b.m[column][row]) > epsilon)
                return false;
        }
    }
    return true;
}

// Turns `joint` of a pose so that it stands `turn` (a rotation in the MODEL's
// space) from where the pose had it, its parents left alone.
void turnInModel(const std::vector<asset::Joint>& joints, Local& pose, usize joint, const core::Mat3& turn)
{
    const std::vector<core::CFrameD> model = modelOf(joints, pose);
    const core::Mat3 parent =
        joints[joint].parent == asset::Joint::NoParent ? core::Mat3{} : model[joints[joint].parent].rotation;
    pose.rotation[joint] = quaternionOf(core::transpose(parent) * turn * model[joint].rotation);
}

struct Pair
{
    std::vector<asset::Joint> source;
    std::vector<asset::Joint> target;
    retarget::RigRoles sourceRoles;
    retarget::RigRoles targetRoles;
    retarget::Map map;
};

Pair twoBodies()
{
    Pair pair;
    pair.source = sourceRig();
    pair.target = targetRig();
    pair.sourceRoles = retarget::assignRoles(pair.source);
    pair.targetRoles = retarget::assignRoles(pair.target);
    pair.map = retarget::buildMap(pair.source, pair.sourceRoles, pair.target, pair.targetRoles);
    return pair;
}

// The rule before there were roles, written out again: the first target joint
// of the equal name.
std::vector<i32> slotsByName(const std::vector<asset::Joint>& source, const std::vector<asset::Joint>& target)
{
    std::vector<i32> slots(source.size(), -1);
    for (usize joint = 0; joint < source.size(); ++joint)
        slots[joint] = find(target, source[joint].name);
    return slots;
}

std::vector<asset::Joint> tailRig()
{
    return rigOf({
        {"Tail", "", {0.0, 1.0, 0.0}, core::Mat3{}},
        {"Tail1", "Tail", {0.0, 1.0, -0.2}, core::rotationX(0.3f)},
        {"Tail2", "Tail1", {0.0, 0.9, -0.4}, core::rotationX(0.6f)},
    });
}

} // namespace

// --- Roles -------------------------------------------------------------------------

TEST_CASE("retarget: a role's name reads back as the role")
{
    for (usize index = 1; index < retarget::RoleCount; ++index) {
        const Role role = static_cast<Role>(index);
        CHECK_FALSE(retarget::roleName(role).empty());
        CHECK(retarget::roleFromName(retarget::roleName(role)) == role);
    }
    // The table is in the enumeration's order, at both ends and in the middle.
    CHECK(retarget::roleName(Role::Hips) == "Hips");
    CHECK(retarget::roleName(Role::LeftUpperArm) == "LeftUpperArm");
    CHECK(retarget::roleName(Role::RightHand) == "RightHand");
    CHECK(retarget::roleName(Role::RightToes) == "RightToes");
    CHECK(retarget::roleName(Role::LeftThumb1) == "LeftThumb1");
    CHECK(retarget::roleName(Role::LeftLittle3) == "LeftLittle3");
    CHECK(retarget::roleName(Role::RightThumb1) == "RightThumb1");
    CHECK(retarget::roleName(Role::RightLittle3) == "RightLittle3");
    CHECK(retarget::roleName(Role::Count).empty());

    CHECK(retarget::roleFromName("None") == Role::None);
    CHECK(retarget::roleFromName("leftupperarm") == Role::None);
    CHECK(retarget::roleFromName("") == Role::None);
}

TEST_CASE("retarget: the free motion library's rig gets every role, with its prefix or without")
{
    checkRoles(motionLibraryRig("mixamorig:"), true);
    checkRoles(motionLibraryRig(""), true);
    // As exporters rewrite the prefix: no colon, or under an armature's name.
    checkRoles(motionLibraryRig("mixamorig_"), true);
    checkRoles(motionLibraryRig("Armature|mixamorig1:"), true);
}

TEST_CASE("retarget: a humanoid export's rig gets every role")
{
    checkRoles(humanoidExportRig(), true);
}

TEST_CASE("retarget: the mannequin gets every role and its twist and IK joints none")
{
    checkRoles(mannequinRig(), true);
}

TEST_CASE("retarget: the Blender rig's deform bones get every role, prefixed or not")
{
    checkRoles(blenderRig(""), true);
    checkRoles(blenderRig("DEF-"), true);
}

TEST_CASE("retarget: the biped gets every role and its nubs none")
{
    checkRoles(bipedRig("Bip01", ' '), true);
    checkRoles(bipedRig("Bip001", ' '), true);
    checkRoles(bipedRig("Bip01", '_'), true);
}

TEST_CASE("retarget: plain English names, whichever way round the side is said")
{
    // A side in front, lower case; `leg` beside `shin` is the upper leg.
    checkRoles(
        {
            {"hips", "", Role::Hips},
            {"spine", "hips", Role::Spine},
            {"chest", "spine", Role::Chest},
            {"neck", "chest", Role::Neck},
            {"head", "neck", Role::Head},
            {"l_shoulder", "chest", Role::LeftShoulder},
            {"l_arm", "l_shoulder", Role::LeftUpperArm},
            {"l_forearm", "l_arm", Role::LeftLowerArm},
            {"l_hand", "l_forearm", Role::LeftHand},
            {"r_shoulder", "chest", Role::RightShoulder},
            {"r_arm", "r_shoulder", Role::RightUpperArm},
            {"r_forearm", "r_arm", Role::RightLowerArm},
            {"r_hand", "r_forearm", Role::RightHand},
            {"l_leg", "hips", Role::LeftUpperLeg},
            {"l_shin", "l_leg", Role::LeftLowerLeg},
            {"l_foot", "l_shin", Role::LeftFoot},
            {"l_toe", "l_foot", Role::LeftToes},
            {"r_leg", "hips", Role::RightUpperLeg},
            {"r_shin", "r_leg", Role::RightLowerLeg},
            {"r_foot", "r_shin", Role::RightFoot},
            {"r_toe", "r_foot", Role::RightToes},
        },
        false);

    // Named joint by joint, the side a word in front: with no other joint for
    // the upper arm, `Shoulder` is it.
    checkRoles(
        {
            {"Pelvis", "", Role::Hips},
            {"Spine", "Pelvis", Role::Spine},
            {"Neck", "Spine", Role::Neck},
            {"Head", "Neck", Role::Head},
            {"LeftShoulder", "Spine", Role::LeftUpperArm},
            {"LeftElbow", "LeftShoulder", Role::LeftLowerArm},
            {"LeftWrist", "LeftElbow", Role::LeftHand},
            {"RightShoulder", "Spine", Role::RightUpperArm},
            {"RightElbow", "RightShoulder", Role::RightLowerArm},
            {"RightWrist", "RightElbow", Role::RightHand},
            {"LeftHip", "Pelvis", Role::LeftUpperLeg},
            {"LeftKnee", "LeftHip", Role::LeftLowerLeg},
            {"LeftAnkle", "LeftKnee", Role::LeftFoot},
            {"LeftBall", "LeftAnkle", Role::LeftToes},
            {"RightHip", "Pelvis", Role::RightUpperLeg},
            {"RightKnee", "RightHip", Role::RightLowerLeg},
            {"RightAnkle", "RightKnee", Role::RightFoot},
            {"RightBall", "RightAnkle", Role::RightToes},
        },
        false);

    // The side a word or a letter behind.
    checkRoles(
        {
            {"Hips", "", Role::Hips},
            {"Chest", "Hips", Role::Spine},
            {"Head", "Chest", Role::Head},
            {"UpperArm_Left", "Chest", Role::LeftUpperArm},
            {"LowerArm_Left", "UpperArm_Left", Role::LeftLowerArm},
            {"Hand_Left", "LowerArm_Left", Role::LeftHand},
            {"Arm_R", "Chest", Role::RightUpperArm},
            {"Elbow_R", "Arm_R", Role::RightLowerArm},
            {"Hand_R", "Elbow_R", Role::RightHand},
            {"UpperLeg_Left", "Hips", Role::LeftUpperLeg},
            {"LowerLeg_Left", "UpperLeg_Left", Role::LeftLowerLeg},
            {"Foot_Left", "LowerLeg_Left", Role::LeftFoot},
            {"Toes_Left", "Foot_Left", Role::LeftToes},
            {"Thigh_R", "Hips", Role::RightUpperLeg},
            {"Calf_R", "Thigh_R", Role::RightLowerLeg},
            {"Ankle_R", "Calf_R", Role::RightFoot},
            {"Ball_R", "Ankle_R", Role::RightToes},
        },
        false);
}

TEST_CASE("retarget: one name at a time -- prefixes, sides, and the words of a helper")
{
    struct Case
    {
        const char* name;
        Role role;
    };
    const Case cases[] = {
        {"Arm_L", Role::LeftUpperArm},
        {"l_arm", Role::LeftUpperArm},
        {"LeftArm", Role::LeftUpperArm},
        {"arm.left", Role::LeftUpperArm},
        {"lForeArm", Role::LeftLowerArm},
        {"HandR", Role::RightHand},
        {"thumb1L", Role::LeftThumb1},
        {"Character1_RightHand", Role::RightHand},
        {"Armature|mixamorig:LeftFoot", Role::LeftFoot},
        {"Armature_LeftFoot", Role::LeftFoot},
        {"rig_thigh.L", Role::LeftUpperLeg},
        {"ORG-shin.R", Role::RightLowerLeg},
        {"Bip001 R Calf", Role::RightLowerLeg},
        {"knee_r", Role::RightLowerLeg},
        {"ankle.L", Role::LeftFoot},
        {"RightHandIndex2", Role::RightIndex2},
        {"index_03_r", Role::RightIndex3},
        {"Right Little Distal", Role::RightLittle3},
        {"pinky_02_l", Role::LeftLittle2},
        {"Pelvis", Role::Hips},
        {"HEAD", Role::Head},
        {"neck_01", Role::Neck},

        // Helpers, however the word is set off.
        {"upperarm_twist_01_l", Role::None},
        {"LeftForeArmRoll", Role::None},
        {"ik_hand_l", Role::None},
        {"IKFootL", Role::None},
        {"hand_pole.L", Role::None},
        {"foot_ctrl_l", Role::None},
        {"Bip01 L Finger1Nub", Role::None},
        {"LeftHandIndex4", Role::None},
        {"LeftHand_end", Role::None},
        // A word that only contains one is not one.
        {"LeftHandIndex1", Role::LeftIndex1},

        // A limb with no side and a middle joint with one are nothing.
        {"Hand", Role::None},
        {"Arm", Role::None},
        {"Head_L", Role::None},
        {"pelvis.L", Role::None},
        // Nothing left once the rig's own part is off.
        {"Bip01", Role::None},
        {"mixamorig:", Role::None},
        {"Left", Role::None},
        {"L", Role::None},
        {"", Role::None},
        {"Weapon", Role::None},
    };
    for (const Case& one : cases) {
        INFO("name " << one.name);
        CHECK(roleOfName(one.name) == one.role);
    }
}

TEST_CASE("retarget: a back of one to five joints is spine, chest and upper chest by its order")
{
    const auto back = [](u32 count) {
        std::vector<Named> rig{{"pelvis", "", Role::Hips}};
        std::string parent = "pelvis";
        for (u32 index = 0; index < count; ++index) {
            Role role = Role::None;
            if (index == 0)
                role = Role::Spine;
            else if (index == 1)
                role = Role::Chest;
            else if (index + 1 == count)
                role = Role::UpperChest;
            const std::string name = "spine_0" + std::to_string(index + 1);
            rig.push_back({name, parent, role});
            parent = name;
        }
        rig.push_back({"neck_01", parent, Role::Neck});
        // A second joint of the neck: the first has the role.
        rig.push_back({"neck_02", "neck_01", Role::None});
        rig.push_back({"head", "neck_02", Role::Head});
        return rig;
    };
    for (u32 count = 1; count <= 5; ++count) {
        INFO("joints in the back: " << count);
        checkRoles(back(count), false);
    }
}

TEST_CASE("retarget: a column numbered as one spine has its hips, its neck and its head found by what hangs where")
{
    // The rig that calls nothing hips, neck or head: the legs hang from the
    // first `spine`, the arms from the fourth, and three more stand above.
    std::vector<Named> rig{
        {"DEF-spine", "", Role::Hips},
        {"DEF-spine.001", "DEF-spine", Role::Spine},
        {"DEF-spine.002", "DEF-spine.001", Role::Chest},
        {"DEF-spine.003", "DEF-spine.002", Role::UpperChest},
        {"DEF-spine.004", "DEF-spine.003", Role::Neck},
        {"DEF-spine.005", "DEF-spine.004", Role::None},
        {"DEF-spine.006", "DEF-spine.005", Role::Head},
        {"DEF-pelvis.L", "DEF-spine", Role::None},
        {"DEF-pelvis.R", "DEF-spine", Role::None},
        {"DEF-breast.L", "DEF-spine.003", Role::None},
    };
    for (const bool left : {true, false}) {
        const std::string side = left ? ".L" : ".R";
        rig.push_back({"DEF-shoulder" + side, "DEF-spine.003", sided(left, Role::LeftShoulder, Role::RightShoulder)});
        rig.push_back(
            {"DEF-upper_arm" + side, "DEF-shoulder" + side, sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back({"DEF-upper_arm" + side + ".001", "DEF-upper_arm" + side, Role::None});
        rig.push_back({"DEF-forearm" + side, "DEF-upper_arm" + side + ".001",
                       sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({"DEF-hand" + side, "DEF-forearm" + side, sided(left, Role::LeftHand, Role::RightHand)});
        rig.push_back({"DEF-thigh" + side, "DEF-spine", sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({"DEF-shin" + side, "DEF-thigh" + side, sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({"DEF-foot" + side, "DEF-shin" + side, sided(left, Role::LeftFoot, Role::RightFoot)});
        rig.push_back({"DEF-toe" + side, "DEF-foot" + side, sided(left, Role::LeftToes, Role::RightToes)});
    }
    checkRoles(rig, false);
    CHECK(retarget::assignRoles(jointsOf(rig)).body());
}

// --- Families met in real files (2026-10-10) -------------------------------------------
//
// The joint names below are those of rigs the mapper was run over: a game's
// own heroes and three packs of free characters. Names only; no file.

TEST_CASE("retarget: a rig of a dozen joints in one short word each -- Arm, Fore, Hand, Leg, Shin, Foot")
{
    std::vector<Named> rig{
        {"Root", "", Role::None},
        {"Hips", "Root", Role::Hips},
        {"Chest", "Hips", Role::Spine},
        {"Head", "Chest", Role::Head},
    };
    for (const bool left : {false, true}) {
        const std::string side = left ? "L" : "R";
        rig.push_back({"Arm" + side, "Chest", sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back({"Fore" + side, "Arm" + side, sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({"Hand" + side, "Fore" + side, sided(left, Role::LeftHand, Role::RightHand)});
        rig.push_back({"Leg" + side, "Hips", sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({"Shin" + side, "Leg" + side, sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({"Foot" + side, "Shin" + side, sided(left, Role::LeftFoot, Role::RightFoot)});
    }
    checkRoles(rig, false);
    CHECK(retarget::assignRoles(jointsOf(rig)).body());
}

TEST_CASE("retarget: a rig with a wrist and a hand, and the handles it was animated with beside it")
{
    std::vector<Named> rig{
        {"root", "", Role::None},        {"hips", "root", Role::Hips},  {"spine", "hips", Role::Spine},
        {"chest", "spine", Role::Chest}, {"head", "chest", Role::Head},
    };
    for (const bool left : {true, false}) {
        const std::string side = left ? ".l" : ".r";
        rig.push_back({"upperarm" + side, "chest", sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back({"lowerarm" + side, "upperarm" + side, sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        // The wrist is the joint the forearm ends at; the hand under it is a
        // second joint of the same part.
        rig.push_back({"wrist" + side, "lowerarm" + side, sided(left, Role::LeftHand, Role::RightHand)});
        rig.push_back({"hand" + side, "wrist" + side, Role::None});
        rig.push_back({"handslot" + side, "hand" + side, Role::None});
        rig.push_back({"upperleg" + side, "hips", sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({"lowerleg" + side, "upperleg" + side, sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({"foot" + side, "lowerleg" + side, sided(left, Role::LeftFoot, Role::RightFoot)});
        rig.push_back({"toes" + side, "foot" + side, sided(left, Role::LeftToes, Role::RightToes)});
        rig.push_back({"kneeIK" + side, "root", Role::None});
        rig.push_back({"control-toe-roll" + side, "root", Role::None});
        rig.push_back({"control-heel-roll" + side, "control-toe-roll" + side, Role::None});
        rig.push_back({"control-foot-roll" + side, "control-heel-roll" + side, Role::None});
        rig.push_back({"heelIK" + side, "control-foot-roll" + side, Role::None});
        rig.push_back({"IK-foot" + side, "control-foot-roll" + side, Role::None});
        rig.push_back({"IK-toe" + side, "control-heel-roll" + side, Role::None});
        rig.push_back({"elbowIK" + side, "root", Role::None});
        rig.push_back({"handIK" + side, "root", Role::None});
    }
    checkRoles(rig, false);
    CHECK(retarget::assignRoles(jointsOf(rig)).body());
}

TEST_CASE("retarget: handles exported as joints -- feet under the root, a body above the hips, a finger's first joint")
{
    // The legs and the back hang from `Body`, which is what a clip moves as
    // the hips; the joint CALLED hips is the first of the back. The feet lie
    // under the root, where the legs' handles were, and are no part of a leg.
    // A finger is `Thumb` and `Thumb2`; the palms are nothing.
    std::vector<Named> rig{
        {"Bone", "", Role::None},         {"Body", "Bone", Role::Hips},           {"Hips", "Body", Role::Spine},
        {"Abdomen", "Hips", Role::Chest}, {"Torso", "Abdomen", Role::UpperChest}, {"Neck", "Torso", Role::Neck},
        {"Head", "Neck", Role::Head},
    };
    for (const bool left : {true, false}) {
        const std::string side = left ? ".L" : ".R";
        rig.push_back({"Foot" + side, "Bone", Role::None});
        rig.push_back({"PoleTarget" + side, "Bone", Role::None});
        rig.push_back({"Shoulder" + side, "Torso", sided(left, Role::LeftShoulder, Role::RightShoulder)});
        rig.push_back({"UpperArm" + side, "Shoulder" + side, sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back({"LowerArm" + side, "UpperArm" + side, sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({"Palm1" + side, "LowerArm" + side, Role::None});
        rig.push_back({"Index" + side, "Palm1" + side, finger(left, 1, 0)});
        rig.push_back({"Index2" + side, "Index" + side, finger(left, 1, 1)});
        rig.push_back({"Palm2" + side, "LowerArm" + side, Role::None});
        rig.push_back({"Middle1" + side, "Palm2" + side, finger(left, 2, 0)});
        rig.push_back({"Middle2" + side, "Middle1" + side, finger(left, 2, 1)});
        rig.push_back({"Thumb" + side, "LowerArm" + side, finger(left, 0, 0)});
        rig.push_back({"Thumb2" + side, "Thumb" + side, finger(left, 0, 1)});
        rig.push_back({"UpperLeg" + side, "Body", sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({"LowerLeg" + side, "UpperLeg" + side, sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
    }
    checkRoles(rig, false);
    CHECK(retarget::assignRoles(jointsOf(rig)).body());

    // A handle that is nearer the root than the real foot does not take the
    // role from it.
    std::vector<Named> both{
        {"Root", "", Role::None},
        {"Hips", "Root", Role::Hips},
        {"Spine", "Hips", Role::Spine},
        {"Foot.L", "Root", Role::None},
        {"UpperLeg.L", "Hips", Role::LeftUpperLeg},
        {"LowerLeg.L", "UpperLeg.L", Role::LeftLowerLeg},
        {"Foot_L", "LowerLeg.L", Role::LeftFoot},
        {"Hand.L", "Root", Role::None},
        {"UpperArm.L", "Spine", Role::LeftUpperArm},
        {"LowerArm.L", "UpperArm.L", Role::LeftLowerArm},
    };
    checkRoles(both, false);

    // Legs that hang from the rig's ROOT leave the named hips the hips: a root
    // stays where the character is put, and is never what a clip moves.
    checkRoles(
        {
            {"Root", "", Role::None},
            {"Hips", "Root", Role::Hips},
            {"Spine", "Hips", Role::Spine},
            {"UpperLeg.L", "Root", Role::LeftUpperLeg},
            {"UpperLeg.R", "Root", Role::RightUpperLeg},
        },
        false);
}

TEST_CASE("retarget: the avatar format's rig, which says the middle and the sides after its prefix")
{
    std::vector<Named> rig{
        {"Root", "", Role::None},
        {"J_Bip_C_Hips", "Root", Role::Hips},
        {"J_Bip_C_Spine", "J_Bip_C_Hips", Role::Spine},
        {"J_Bip_C_Chest", "J_Bip_C_Spine", Role::Chest},
        {"J_Bip_C_UpperChest", "J_Bip_C_Chest", Role::UpperChest},
        {"J_Bip_C_Neck", "J_Bip_C_UpperChest", Role::Neck},
        {"J_Bip_C_Head", "J_Bip_C_Neck", Role::Head},
        {"J_Sec_Hair1_01", "J_Bip_C_Head", Role::None},
    };
    constexpr const char* Fingers[] = {"Thumb", "Index", "Middle", "Ring", "Little"};
    for (const bool left : {true, false}) {
        const std::string side = left ? "J_Bip_L_" : "J_Bip_R_";
        rig.push_back({side + "Shoulder", "J_Bip_C_UpperChest", sided(left, Role::LeftShoulder, Role::RightShoulder)});
        rig.push_back({side + "UpperArm", side + "Shoulder", sided(left, Role::LeftUpperArm, Role::RightUpperArm)});
        rig.push_back({side + "LowerArm", side + "UpperArm", sided(left, Role::LeftLowerArm, Role::RightLowerArm)});
        rig.push_back({side + "Hand", side + "LowerArm", sided(left, Role::LeftHand, Role::RightHand)});
        for (u32 which = 0; which < 5; ++which) {
            std::string parent = side + "Hand";
            for (u32 joint = 0; joint < 3; ++joint) {
                const std::string name = side + Fingers[which] + std::to_string(joint + 1);
                rig.push_back({name, parent, finger(left, which, joint)});
                parent = name;
            }
        }
        rig.push_back({side + "UpperLeg", "J_Bip_C_Hips", sided(left, Role::LeftUpperLeg, Role::RightUpperLeg)});
        rig.push_back({side + "LowerLeg", side + "UpperLeg", sided(left, Role::LeftLowerLeg, Role::RightLowerLeg)});
        rig.push_back({side + "Foot", side + "LowerLeg", sided(left, Role::LeftFoot, Role::RightFoot)});
        rig.push_back({side + "ToeBase", side + "Foot", sided(left, Role::LeftToes, Role::RightToes)});
    }
    checkRoles(rig, true);
}

TEST_CASE("retarget: a tail is not a body, and neither is a body with an arm missing")
{
    CHECK_FALSE(retarget::assignRoles(tailRig()).body());

    std::vector<Named> rig = mannequinRig();
    CHECK(retarget::assignRoles(jointsOf(rig)).body());
    for (Named& entry : rig) {
        if (entry.name == "lowerarm_r")
            entry.name = "sleeve_r";
        if (entry.parent == "lowerarm_r")
            entry.parent = "sleeve_r";
    }
    CHECK_FALSE(retarget::assignRoles(jointsOf(rig)).body());

    const retarget::RigRoles none;
    CHECK_FALSE(none.body());
    CHECK(none.joint(Role::Hips) == -1);
}

// --- The rig's own say ---------------------------------------------------------------

TEST_CASE("retarget: a rig file is read, and one that is wrong says where")
{
    const std::string_view good = R"({
        "format": "rig",
        "version": 1,
        "roles": { "LeftUpperArm": "Bip01_L_UpperArm", "Hips": "Root", "LeftToes": "" }
    })";
    const auto read = retarget::readRigRoles(good);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 3);
    CHECK((*read)[0].role == Role::LeftUpperArm);
    CHECK((*read)[0].joint == "Bip01_L_UpperArm");
    CHECK((*read)[1].role == Role::Hips);
    CHECK((*read)[1].joint == "Root");
    CHECK((*read)[2].role == Role::LeftToes);
    CHECK((*read)[2].joint.empty());

    std::string error;
    CHECK_FALSE(
        retarget::readRigRoles(R"({"format":"rig","version":1,"roles":{"LeftElbow":"a"}})", &error).has_value());
    CHECK(error.find("LeftElbow") != std::string::npos);

    error.clear();
    CHECK_FALSE(retarget::readRigRoles(R"({"format":"rig","version":1,"roles":{"Head":7}})", &error).has_value());
    CHECK(error.find("Head") != std::string::npos);

    error.clear();
    CHECK_FALSE(retarget::readRigRoles(R"({"format":"rig","version":1,"roles":["Head"]})", &error).has_value());
    CHECK(error.find("roles") != std::string::npos);

    error.clear();
    CHECK_FALSE(retarget::readRigRoles(R"({"format":"material","version":1,"roles":{}})", &error).has_value());
    CHECK_FALSE(error.empty());

    error.clear();
    CHECK_FALSE(retarget::readRigRoles(R"({"format":"rig","version":2,"roles":{}})", &error).has_value());
    CHECK(error.find('2') != std::string::npos);

    error.clear();
    CHECK_FALSE(retarget::readRigRoles("{ not json", &error).has_value());
    CHECK_FALSE(error.empty());

    // With nowhere to say why, still only a refusal.
    CHECK_FALSE(retarget::readRigRoles("[]").has_value());
    // No roles at all is a rig file that puts nothing right.
    const auto empty = retarget::readRigRoles(R"({"format":"rig","version":1,"roles":{}})");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("retarget: what a rig says of itself displaces what was guessed")
{
    const std::vector<Named> rig = motionLibraryRig("");
    const std::vector<asset::Joint> joints = jointsOf(rig);
    const usize arm = at(joints, "LeftArm");
    const usize foreArm = at(joints, "LeftForeArm");
    const usize tip = at(joints, "HeadTop_End");

    const retarget::RigRoles guessed = retarget::assignRoles(joints);
    CHECK(guessed.joint(Role::LeftUpperArm) == static_cast<i32>(arm));
    CHECK(guessed.joint(Role::LeftLowerArm) == static_cast<i32>(foreArm));

    const auto read = retarget::readRigRoles(R"({
        "format": "rig", "version": 1,
        "roles": { "LeftUpperArm": "LeftForeArm", "Head": "HeadTop_End", "RightToes": "", "Neck": "NoSuchJoint" }
    })");
    REQUIRE(read.has_value());
    const retarget::RigRoles said = retarget::assignRoles(joints, *read);

    // The named joint has the role; who had it has none; what the joint was
    // guessed to be has no joint.
    CHECK(said.joint(Role::LeftUpperArm) == static_cast<i32>(foreArm));
    CHECK(said.ofJoint[foreArm] == Role::LeftUpperArm);
    CHECK(said.ofJoint[arm] == Role::None);
    CHECK(said.joint(Role::LeftLowerArm) == -1);
    // A joint the names would never give a role is given one.
    CHECK(said.joint(Role::Head) == static_cast<i32>(tip));
    CHECK(said.ofJoint[at(joints, "Head")] == Role::None);
    // An empty name takes the role away.
    CHECK(said.joint(Role::RightToes) == -1);
    CHECK(said.ofJoint[at(joints, "RightToeBase")] == Role::None);
    // A joint the rig does not have changes nothing.
    CHECK(said.joint(Role::Neck) == guessed.joint(Role::Neck));
    // And nothing else moved.
    CHECK(said.joint(Role::RightUpperArm) == guessed.joint(Role::RightUpperArm));
    CHECK(said.joint(Role::Hips) == guessed.joint(Role::Hips));
    CHECK_FALSE(said.body());
}

// --- The map -------------------------------------------------------------------------

TEST_CASE("retarget: the two test rigs are the bodies the cases below take them for")
{
    const Pair pair = twoBodies();
    REQUIRE(pair.sourceRoles.body());
    REQUIRE(pair.targetRoles.body());
    REQUIRE(pair.map.roles);

    // The target rests in an A and the source in a T: at rest their arms are
    // forty-five degrees apart, which is what the stance has to take out.
    const std::vector<core::CFrameD> source = modelOf(pair.source, restOf(pair.source));
    const std::vector<core::CFrameD> target = modelOf(pair.target, restOf(pair.target));
    const Vec3 sourceArm =
        direction(source, at(pair.source, "mixamorig:LeftArm"), at(pair.source, "mixamorig:LeftForeArm"));
    const Vec3 targetArm = direction(target, at(pair.target, "upperarm_l"), at(pair.target, "lowerarm_l"));
    CHECK(nearly(sourceArm, Left, 1.0e-5f));
    CHECK(static_cast<double>(core::dot(sourceArm, targetArm)) == doctest::Approx(0.70710678).epsilon(1.0e-4));

    // The rig is built from model-space places, so its rest is those places.
    CHECK(distance(target, at(pair.target, "upperarm_l"), at(pair.target, "lowerarm_l")) ==
          doctest::Approx(TargetUpperArm).epsilon(1.0e-5));
    CHECK(target[at(pair.target, "pelvis")].position.y == doctest::Approx(0.55).epsilon(1.0e-5));
}

TEST_CASE("retarget: a clip that is the source's rest stands the target in the source's stance, at its own lengths")
{
    const Pair pair = twoBodies();
    const Local rest = restOf(pair.source);
    const std::vector<core::CFrameD> source = modelOf(pair.source, rest);
    const std::vector<core::CFrameD> target = modelOf(pair.target, carried(pair.map, rest, pair.target));

    struct BonePair
    {
        const char* sourceFrom;
        const char* sourceTo;
        const char* targetFrom;
        const char* targetTo;
    };
    const BonePair bones[] = {
        {"mixamorig:LeftArm", "mixamorig:LeftForeArm", "upperarm_l", "lowerarm_l"},
        {"mixamorig:LeftForeArm", "mixamorig:LeftHand", "lowerarm_l", "hand_l"},
        {"mixamorig:RightArm", "mixamorig:RightForeArm", "upperarm_r", "lowerarm_r"},
        {"mixamorig:RightForeArm", "mixamorig:RightHand", "lowerarm_r", "hand_r"},
        {"mixamorig:LeftShoulder", "mixamorig:LeftArm", "clavicle_l", "upperarm_l"},
        {"mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "thigh_l", "calf_l"},
        {"mixamorig:LeftLeg", "mixamorig:LeftFoot", "calf_l", "foot_l"},
        {"mixamorig:RightUpLeg", "mixamorig:RightLeg", "thigh_r", "calf_r"},
        {"mixamorig:RightLeg", "mixamorig:RightFoot", "calf_r", "foot_r"},
        {"mixamorig:Hips", "mixamorig:Spine", "pelvis", "spine_01"},
        {"mixamorig:Neck", "mixamorig:Head", "neck_01", "head"},
    };
    for (const BonePair& bone : bones) {
        INFO("bone " << bone.targetFrom << " to " << bone.targetTo);
        const Vec3 wanted = direction(source, at(pair.source, bone.sourceFrom), at(pair.source, bone.sourceTo));
        const Vec3 got = direction(target, at(pair.target, bone.targetFrom), at(pair.target, bone.targetTo));
        CHECK(nearly(got, wanted, 1.0e-3f));
    }
    // The arms straight out, the legs straight down: the T, on a rig that
    // rests in an A with its legs apart.
    CHECK(nearly(direction(target, at(pair.target, "upperarm_l"), at(pair.target, "lowerarm_l")), Left, 1.0e-3f));
    CHECK(nearly(direction(target, at(pair.target, "upperarm_r"), at(pair.target, "lowerarm_r")), Right, 1.0e-3f));
    CHECK(nearly(direction(target, at(pair.target, "thigh_l"), at(pair.target, "calf_l")), Down, 1.0e-3f));

    // Every bone is as long as the target made it.
    for (usize joint = 0; joint < pair.target.size(); ++joint) {
        if (pair.target[joint].parent == asset::Joint::NoParent)
            continue;
        const core::DVec3 offset = pair.target[joint].localBind.position;
        const f64 own = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
        INFO("joint " << pair.target[joint].name);
        CHECK(distance(target, pair.target[joint].parent, joint) == doctest::Approx(own).epsilon(1.0e-5));
    }
    CHECK(distance(target, at(pair.target, "upperarm_l"), at(pair.target, "lowerarm_l")) ==
          doctest::Approx(TargetUpperArm).epsilon(1.0e-5));
    CHECK(distance(target, at(pair.target, "thigh_r"), at(pair.target, "calf_r")) ==
          doctest::Approx(TargetThigh).epsilon(1.0e-5));
    // And the hips have not moved from the target's own rest.
    CHECK(nearly(core::toVec3(target[at(pair.target, "pelvis")].position), Vec3{0.0f, 0.55f, 0.0f}, 1.0e-5f));

    // `rest` is that same answer, kept for a joint a clip does not key.
    for (usize joint = 0; joint < pair.source.size(); ++joint) {
        if (pair.map.byRole[joint] == 0 || pair.map.slots[joint] < 0)
            continue;
        std::array<f32, 4> out{};
        retarget::rotation(pair.map, static_cast<u32>(joint), rest.rotation[joint].data(), out.data());
        f32 dot = 0.0f;
        for (usize lane = 0; lane < 4; ++lane)
            dot += out[lane] * pair.map.rest[joint * 4 + lane];
        INFO("joint " << pair.source[joint].name);
        CHECK(static_cast<double>(std::fabs(dot)) == doctest::Approx(1.0).epsilon(1.0e-5));
    }
}

TEST_CASE("retarget: an arm raised on the source is raised to the same place in the world on the target")
{
    const Pair pair = twoBodies();
    const usize sourceArm = at(pair.source, "mixamorig:LeftArm");
    const usize sourceForeArm = at(pair.source, "mixamorig:LeftForeArm");
    const usize sourceHand = at(pair.source, "mixamorig:LeftHand");
    const usize targetArm = at(pair.target, "upperarm_l");
    const usize targetForeArm = at(pair.target, "lowerarm_l");
    const usize targetHand = at(pair.target, "hand_l");

    // Sixty degrees about the model's forward axis: the left arm, out along
    // +X, goes up.
    const core::Mat3 raise = core::rotationZ(Pi / 3.0f);
    Local pose = restOf(pair.source);
    turnInModel(pair.source, pose, sourceArm, raise);

    const std::vector<core::CFrameD> source = modelOf(pair.source, pose);
    const std::vector<core::CFrameD> target = modelOf(pair.target, carried(pair.map, pose, pair.target));
    const Vec3 raised{0.5f, 0.8660254f, 0.0f};
    CHECK(nearly(direction(source, sourceArm, sourceForeArm), raised, 1.0e-4f));
    CHECK(nearly(direction(target, targetArm, targetForeArm), raised, 1.0e-3f));
    // What hangs from it goes with it, on both.
    CHECK(nearly(direction(source, sourceForeArm, sourceHand), raised, 1.0e-4f));
    CHECK(nearly(direction(target, targetForeArm, targetHand), raised, 1.0e-3f));
    // The other arm did not hear of it.
    CHECK(nearly(direction(target, at(pair.target, "upperarm_r"), at(pair.target, "lowerarm_r")), Right, 1.0e-3f));
    CHECK(distance(target, targetArm, targetForeArm) == doctest::Approx(TargetUpperArm).epsilon(1.0e-5));

    // Then the elbow bent a right angle about the model's up, from there: the
    // forearm swings forward in the plane the raised arm left it in, on both.
    const core::Mat3 bend = core::rotationY(-Pi * 0.5f);
    turnInModel(pair.source, pose, sourceForeArm, bend);
    const std::vector<core::CFrameD> sourceBent = modelOf(pair.source, pose);
    const std::vector<core::CFrameD> targetBent = modelOf(pair.target, carried(pair.map, pose, pair.target));
    const Vec3 wanted = direction(sourceBent, sourceForeArm, sourceHand);
    CHECK(nearly(wanted, bend * raised, 1.0e-4f));
    CHECK(nearly(direction(targetBent, targetForeArm, targetHand), wanted, 1.0e-3f));
}

TEST_CASE("retarget: a turn about a bone's own length is the same turn in the world, whatever the bone's roll and axis")
{
    const Pair pair = twoBodies();
    const usize sourceArm = at(pair.source, "mixamorig:LeftArm");
    const usize targetArm = at(pair.target, "upperarm_l");
    const usize targetHand = at(pair.target, "hand_l");

    const Local rest = restOf(pair.source);
    const std::vector<core::CFrameD> stance = modelOf(pair.target, carried(pair.map, rest, pair.target));

    // Forty degrees about the arm's own length, which in a T is the model's X:
    // nothing moves, and everything from the arm down is turned by exactly
    // that in the model's space -- a direction along the bone cannot show it,
    // so the joints' own rotations are compared.
    const core::Mat3 twist = core::rotationX(0.7f);
    Local pose = rest;
    turnInModel(pair.source, pose, sourceArm, twist);
    const std::vector<core::CFrameD> target = modelOf(pair.target, carried(pair.map, pose, pair.target));

    CHECK(sameRotation(target[targetArm].rotation, twist * stance[targetArm].rotation, 1.0e-4f));
    CHECK(sameRotation(target[targetHand].rotation, twist * stance[targetHand].rotation, 1.0e-4f));
    CHECK_FALSE(sameRotation(target[targetArm].rotation, stance[targetArm].rotation, 1.0e-2f));
    CHECK(nearly(core::toVec3(target[targetHand].position), core::toVec3(stance[targetHand].position), 1.0e-4f));
    // The right arm's bones run along -X of their joints; the same twist the
    // other way round the body comes out the same way in the world.
    Local other = rest;
    turnInModel(pair.source, other, at(pair.source, "mixamorig:RightArm"), twist);
    const std::vector<core::CFrameD> right = modelOf(pair.target, carried(pair.map, other, pair.target));
    const usize targetRight = at(pair.target, "upperarm_r");
    CHECK(sameRotation(right[targetRight].rotation, twist * stance[targetRight].rotation, 1.0e-4f));
}

TEST_CASE("retarget: the hips travel half as far on legs half as long, in the frame of the target's own parent")
{
    const Pair pair = twoBodies();
    const usize sourceHips = at(pair.source, "mixamorig:Hips");
    const usize targetHips = at(pair.target, "pelvis");
    REQUIRE(pair.map.hips == static_cast<i32>(sourceHips));
    CHECK(pair.map.hipsScale == doctest::Approx(0.5).epsilon(1.0e-5));

    Local pose = restOf(pair.source);
    const core::DVec3 travel{0.3, 0.2, 0.1};
    pose.translation[sourceHips] = pose.translation[sourceHips] + travel;

    const Local put = carried(pair.map, pose, pair.target);
    const std::vector<core::CFrameD> rest = modelOf(pair.target, restOf(pair.target));
    const std::vector<core::CFrameD> target = modelOf(pair.target, put);
    // In the model's space: the same way, half as far.
    const core::DVec3 moved = target[targetHips].position - rest[targetHips].position;
    CHECK(moved.x == doctest::Approx(0.15).epsilon(1.0e-5));
    CHECK(moved.y == doctest::Approx(0.1).epsilon(1.0e-5));
    CHECK(moved.z == doctest::Approx(0.05).epsilon(1.0e-5));
    // From the target's parent, which lies on its side, that is another
    // vector: the model's up is the root's +Z and its forward the root's -Y.
    const core::DVec3 local = put.translation[targetHips] - pair.target[targetHips].localBind.position;
    CHECK(local.x == doctest::Approx(0.15).epsilon(1.0e-5));
    CHECK(local.y == doctest::Approx(-0.05).epsilon(1.0e-5));
    CHECK(local.z == doctest::Approx(0.1).epsilon(1.0e-5));

    // No other joint matched by role takes a translation: a source that keyed
    // its arm a metre out leaves the target's arm its own length.
    const usize sourceForeArm = at(pair.source, "mixamorig:LeftForeArm");
    pose.translation[sourceForeArm] = pose.translation[sourceForeArm] + core::DVec3{1.0, 0.0, 0.0};
    const std::vector<core::CFrameD> stretched = modelOf(pair.target, carried(pair.map, pose, pair.target));
    CHECK(distance(stretched, at(pair.target, "upperarm_l"), at(pair.target, "lowerarm_l")) ==
          doctest::Approx(TargetUpperArm).epsilon(1.0e-5));

    // At rest the hips are where the target rests them.
    const core::DVec3 still = retarget::hipsTranslation(pair.map, pair.source[sourceHips].localBind.position);
    CHECK(still.x == doctest::Approx(pair.target[targetHips].localBind.position.x));
    CHECK(still.y == doctest::Approx(pair.target[targetHips].localBind.position.y));
    CHECK(still.z == doctest::Approx(pair.target[targetHips].localBind.position.z));
}

TEST_CASE("retarget: a role the target lacks is named once, and a joint with no role goes by its name")
{
    const Pair pair = twoBodies();

    // The source has three joints in its back and toes; the target two and none.
    const std::vector<Role> wanted{Role::UpperChest, Role::LeftToes, Role::RightToes};
    CHECK(pair.map.unmapped == wanted);
    const usize sourceToes = at(pair.source, "mixamorig:LeftToeBase");
    CHECK(pair.map.slots[sourceToes] == -1);
    CHECK(pair.map.byRole[sourceToes] == 1);

    // Role to role, whatever the two are called.
    CHECK(pair.map.slots[at(pair.source, "mixamorig:LeftArm")] == find(pair.target, "upperarm_l"));
    CHECK(pair.map.slots[at(pair.source, "mixamorig:RightLeg")] == find(pair.target, "calf_r"));
    CHECK(pair.map.slots[at(pair.source, "mixamorig:Spine1")] == find(pair.target, "spine_02"));
    CHECK(pair.map.byRole[at(pair.source, "mixamorig:LeftArm")] == 1);
    // The target's twist joint is nobody's.
    const i32 twist = find(pair.target, "upperarm_twist_01_l");
    CHECK(std::find(pair.map.slots.begin(), pair.map.slots.end(), twist) == pair.map.slots.end());

    // The tail: by name, and its samples as they are.
    const usize sourceTail = at(pair.source, "Tail");
    CHECK(pair.map.slots[sourceTail] == find(pair.target, "Tail"));
    CHECK(pair.map.byRole[sourceTail] == 0);
    const std::array<f32, 4> sample{0.1f, -0.2f, 0.3f, 0.9f};
    std::array<f32, 4> out{};
    retarget::rotation(pair.map, static_cast<u32>(sourceTail), sample.data(), out.data());
    CHECK(out == sample);
    // A joint the map has never heard of is left alone too.
    retarget::rotation(pair.map, 9999, sample.data(), out.data());
    CHECK(out == sample);

    // In place: the answer may be written over the sample.
    const usize sourceArm = at(pair.source, "mixamorig:LeftArm");
    std::array<f32, 4> apart{};
    retarget::rotation(pair.map, static_cast<u32>(sourceArm), sample.data(), apart.data());
    std::array<f32, 4> inPlace = sample;
    retarget::rotation(pair.map, static_cast<u32>(sourceArm), inPlace.data(), inPlace.data());
    CHECK(inPlace == apart);
    CHECK_FALSE(apart == sample);
}

TEST_CASE("retarget: a joint with no role does not go by name to a joint that has one")
{
    // The source's `spine_02` sits between its chest and its upper chest and
    // has no role; the target's joint of that name is its chest, which the
    // source's chest already drives.
    const auto rig = [](u32 back) {
        std::vector<Named> named{{"pelvis", "", Role::None}};
        std::string parent = "pelvis";
        for (u32 index = 0; index < back; ++index) {
            const std::string name = "spine_0" + std::to_string(index);
            named.push_back({name, parent, Role::None});
            parent = name;
        }
        for (const char* side : {"_l", "_r"}) {
            const std::string s = side;
            named.push_back({"upperarm" + s, parent, Role::None});
            named.push_back({"lowerarm" + s, "upperarm" + s, Role::None});
            named.push_back({"thigh" + s, "pelvis", Role::None});
            named.push_back({"calf" + s, "thigh" + s, Role::None});
        }
        return jointsOf(named);
    };
    const std::vector<asset::Joint> source = rig(4);
    const std::vector<asset::Joint> target = rig(3);
    const retarget::RigRoles sourceRoles = retarget::assignRoles(source);
    const retarget::RigRoles targetRoles = retarget::assignRoles(target);
    REQUIRE(sourceRoles.ofJoint[at(source, "spine_02")] == Role::None);
    REQUIRE(targetRoles.ofJoint[at(target, "spine_02")] == Role::UpperChest);

    const retarget::Map map = retarget::buildMap(source, sourceRoles, target, targetRoles);
    REQUIRE(map.roles);
    CHECK(map.slots[at(source, "spine_02")] == -1);
    CHECK(map.byRole[at(source, "spine_02")] == 0);
    CHECK(map.slots[at(source, "spine_03")] == find(target, "spine_02"));
    CHECK(map.unmapped.empty());
    // Each target joint is driven by one source joint at most.
    for (usize joint = 0; joint < target.size(); ++joint) {
        INFO("target joint " << target[joint].name);
        CHECK(std::count(map.slots.begin(), map.slots.end(), static_cast<i32>(joint)) <= 1);
    }
}

TEST_CASE("retarget: a rig that is not a body goes by equal names, as it always did")
{
    const std::vector<asset::Joint> tail = tailRig();
    const retarget::RigRoles tailRoles = retarget::assignRoles(tail);
    REQUIRE_FALSE(tailRoles.body());

    const Pair pair = twoBodies();
    const std::array<f32, 4> sample{0.5f, 0.5f, -0.5f, 0.5f};
    const core::DVec3 place{1.0, 2.0, 3.0};

    const auto same = [&](const std::vector<asset::Joint>& source, const retarget::RigRoles& sourceRoles,
                          const std::vector<asset::Joint>& target, const retarget::RigRoles& targetRoles) {
        const retarget::Map map = retarget::buildMap(source, sourceRoles, target, targetRoles);
        CHECK_FALSE(map.roles);
        CHECK(map.slots == slotsByName(source, target));
        CHECK(map.byRole == std::vector<core::u8>(source.size(), 0));
        CHECK(map.unmapped.empty());
        CHECK(map.hips == -1);
        for (usize joint = 0; joint < source.size(); ++joint) {
            std::array<f32, 4> out{};
            retarget::rotation(map, static_cast<u32>(joint), sample.data(), out.data());
            CHECK(out == sample);
        }
        CHECK(retarget::hipsTranslation(map, place) == place);
    };

    // A tail on a body that has one of that name, a body's clip on a tail, a
    // tail on itself under another file's name, and a tail on nothing.
    same(tail, tailRoles, pair.target, pair.targetRoles);
    same(pair.source, pair.sourceRoles, tail, tailRoles);
    same(tail, tailRoles, tail, tailRoles);
    same(tail, tailRoles, {}, retarget::RigRoles{});

    const retarget::Map onBody = retarget::buildMap(tail, tailRoles, pair.target, pair.targetRoles);
    CHECK(onBody.slots[0] == find(pair.target, "Tail"));
    CHECK(onBody.slots[1] == -1);

    // Two bodies whose roles were never worked out go by name as well, rather
    // than by roles that are not there.
    const retarget::Map unread = retarget::buildMap(pair.source, retarget::RigRoles{}, pair.target, pair.targetRoles);
    CHECK_FALSE(unread.roles);
    CHECK(unread.slots == slotsByName(pair.source, pair.target));
}

TEST_CASE("retarget: the same two rigs make the same map, to the bit")
{
    const Pair first = twoBodies();
    const Pair second = twoBodies();
    CHECK(first.map.slots == second.map.slots);
    CHECK(first.map.pre == second.map.pre);
    CHECK(first.map.post == second.map.post);
    CHECK(first.map.rest == second.map.rest);
    CHECK(first.map.hipsScale == second.map.hipsScale);
}

// --- Rigs that stand another way ---------------------------------------------------

namespace {

// The whole rig turned in its own space: every root's rest goes round, and
// with it everything below. A body that faces the other way, or a rig whose
// joints rest in a space with another axis up.
std::vector<asset::Joint> turnedWhole(std::vector<asset::Joint> joints, const core::Mat3& turn)
{
    for (asset::Joint& joint : joints) {
        if (joint.parent == asset::Joint::NoParent)
            joint.localBind = core::CFrameD{core::DVec3{}, turn} * joint.localBind;
    }
    return joints;
}

// A walk's worth of a pose on the source: the left arm up, the right leg
// swung forward with its knee bent back, the hips turned and moved.
Local stride(const std::vector<asset::Joint>& source)
{
    Local pose = restOf(source);
    const usize hips = at(source, "mixamorig:Hips");
    turnInModel(source, pose, hips, core::rotationY(0.3f));
    pose.translation[hips] = pose.translation[hips] + core::DVec3{0.05, -0.1, 0.2};
    turnInModel(source, pose, at(source, "mixamorig:LeftArm"), core::rotationZ(0.9f));
    turnInModel(source, pose, at(source, "mixamorig:RightUpLeg"), core::rotationX(-0.6f));
    turnInModel(source, pose, at(source, "mixamorig:RightLeg"), core::rotationX(0.8f));
    return pose;
}

} // namespace

TEST_CASE("retarget: a body that faces the other way walks the same way round")
{
    // The target as it is, and the same target drawn looking along -Z: its
    // left is at -X. A leg the clip swings FORWARD goes to the front of each,
    // which in the second's own space is the opposite way.
    const Pair pair = twoBodies();
    const core::Mat3 about = core::rotationY(Pi);
    const std::vector<asset::Joint> turned = turnedWhole(pair.target, about);
    const retarget::RigRoles turnedRoles = retarget::assignRoles(turned);
    const retarget::Map map = retarget::buildMap(pair.source, pair.sourceRoles, turned, turnedRoles);
    REQUIRE(map.roles);

    const Local pose = stride(pair.source);
    const std::vector<core::CFrameD> plain = modelOf(pair.target, carried(pair.map, pose, pair.target));
    const std::vector<core::CFrameD> faced = modelOf(turned, carried(map, pose, turned));
    for (usize joint = 0; joint < turned.size(); ++joint) {
        INFO("joint " << turned[joint].name);
        CHECK(nearly(core::toVec3(faced[joint].position), about * core::toVec3(plain[joint].position), 1.0e-4f));
        CHECK(sameRotation(faced[joint].rotation, about * plain[joint].rotation, 1.0e-4f));
    }
    // And it is a walk: the right foot is ahead of the hips, the way the body
    // looks.
    const usize foot = at(turned, "foot_r");
    const usize hips = at(turned, "pelvis");
    CHECK(plain[foot].position.z - plain[hips].position.z > 0.05);
    CHECK(faced[foot].position.z - faced[hips].position.z < -0.05);
}

TEST_CASE("retarget: a rig whose joints rest in a space with another axis up is carried as the body it is")
{
    // What a file converted from an older format looks like from inside: a
    // node that is no joint stands the rig up, so the joints themselves lie
    // along +Z. The clip is the same clip, said in that space.
    const Pair pair = twoBodies();
    const core::Mat3 lying = core::rotationX(Pi * 0.5f);
    const std::vector<asset::Joint> source = turnedWhole(pair.source, lying);
    const retarget::RigRoles sourceRoles = retarget::assignRoles(source);
    const retarget::Map map = retarget::buildMap(source, sourceRoles, pair.target, pair.targetRoles);
    REQUIRE(map.roles);
    CHECK(map.hipsScale == doctest::Approx(pair.map.hipsScale).epsilon(1.0e-6));

    const Local pose = stride(pair.source);
    Local said = pose;
    for (usize joint = 0; joint < source.size(); ++joint) {
        if (source[joint].parent != asset::Joint::NoParent)
            continue;
        const std::array<f32, 4>& q = pose.rotation[joint];
        said.rotation[joint] = quaternionOf(lying * core::fromQuaternion(q[0], q[1], q[2], q[3]));
        said.translation[joint] = core::toDVec3(lying * core::toVec3(pose.translation[joint]));
    }

    const std::vector<core::CFrameD> plain = modelOf(pair.target, carried(pair.map, pose, pair.target));
    const std::vector<core::CFrameD> stood = modelOf(pair.target, carried(map, said, pair.target));
    for (usize joint = 0; joint < pair.target.size(); ++joint) {
        INFO("joint " << pair.target[joint].name);
        CHECK(nearly(core::toVec3(stood[joint].position), core::toVec3(plain[joint].position), 1.0e-4f));
        CHECK(sameRotation(stood[joint].rotation, plain[joint].rotation, 1.0e-4f));
    }

    // Both ways at once, and the target the one that lies: nothing but the
    // space changes.
    const std::vector<asset::Joint> target = turnedWhole(pair.target, lying);
    const retarget::Map both = retarget::buildMap(source, sourceRoles, target, retarget::assignRoles(target));
    const std::vector<core::CFrameD> lain = modelOf(target, carried(both, said, target));
    for (usize joint = 0; joint < target.size(); ++joint) {
        INFO("joint " << target[joint].name);
        CHECK(nearly(core::toVec3(lain[joint].position), lying * core::toVec3(plain[joint].position), 1.0e-4f));
    }
}

namespace {

// The rig hung under one more joint, `root`, turned as said: what an exporter
// writes above the hips to stand a rig up.
std::vector<asset::Joint> underRoot(const std::vector<asset::Joint>& joints, const core::Mat3& turn)
{
    const core::CFrameD frame{core::DVec3{}, turn};
    std::vector<asset::Joint> out;
    asset::Joint root;
    root.name = "root";
    root.localBind = frame;
    out.push_back(root);
    for (asset::Joint joint : joints) {
        if (joint.parent == asset::Joint::NoParent) {
            joint.parent = 0;
            joint.localBind = core::inverse(frame) * joint.localBind;
        }
        else {
            joint.parent += 1;
        }
        out.push_back(joint);
    }
    return out;
}

} // namespace

TEST_CASE("retarget: a root above the hips is the rig's own, and is not carried by its name")
{
    // Both rigs have a joint called `root` above their hips. The source's
    // rests upright and the target's lies a quarter turn over; a clip keys the
    // source's at its rest. By its name that laid the target on its back.
    const Pair pair = twoBodies();
    const std::vector<asset::Joint> source = underRoot(pair.source, core::Mat3{});
    const retarget::RigRoles sourceRoles = retarget::assignRoles(source);
    const retarget::Map map = retarget::buildMap(source, sourceRoles, pair.target, pair.targetRoles);
    REQUIRE(map.roles);
    CHECK(map.slots[at(source, "root")] == -1);
    // A joint with no role that is not above the hips still goes by its name.
    CHECK(map.slots[at(source, "Tail")] == static_cast<i32>(at(pair.target, "Tail")));

    const Local plainPose = stride(pair.source);
    Local pose = restOf(source);
    for (usize joint = 0; joint < pair.source.size(); ++joint) {
        pose.rotation[joint + 1] = plainPose.rotation[joint];
        pose.translation[joint + 1] = plainPose.translation[joint];
    }
    const std::vector<core::CFrameD> plain = modelOf(pair.target, carried(pair.map, plainPose, pair.target));
    const std::vector<core::CFrameD> rooted = modelOf(pair.target, carried(map, pose, pair.target));
    for (usize joint = 0; joint < pair.target.size(); ++joint) {
        INFO("joint " << pair.target[joint].name);
        CHECK(nearly(core::toVec3(rooted[joint].position), core::toVec3(plain[joint].position), 1.0e-4f));
        CHECK(sameRotation(rooted[joint].rotation, plain[joint].rotation, 1.0e-4f));
    }

    // The other way about: the source's root lies over and the target's
    // stands, and the target still stands.
    const std::vector<asset::Joint> lying = underRoot(pair.source, core::rotationX(-Pi * 0.5f));
    const std::vector<asset::Joint> standing = underRoot(turnedWhole(pair.source, core::Mat3{}), core::Mat3{});
    const retarget::Map over =
        retarget::buildMap(lying, retarget::assignRoles(lying), standing, retarget::assignRoles(standing));
    REQUIRE(over.roles);
    CHECK(over.slots[at(lying, "root")] == -1);
    const std::vector<core::CFrameD> stood = modelOf(standing, carried(over, restOf(lying), standing));
    const std::vector<core::CFrameD> rest = modelOf(standing, restOf(standing));
    for (usize joint = 0; joint < standing.size(); ++joint) {
        INFO("joint " << standing[joint].name);
        CHECK(nearly(core::toVec3(stood[joint].position), core::toVec3(rest[joint].position), 1.0e-4f));
    }
}

TEST_CASE("retarget: a back that starts below the hips does not turn the other rig's pelvis over")
{
    // The source's first joint of the back lies below and behind its hips, as
    // a rig has it whose legs hang from a joint above the one it calls hips.
    // From hips to that joint is no bone to stand another rig's pelvis along.
    Pair pair = twoBodies();
    const usize spine = at(pair.source, "mixamorig:Spine");
    const usize next = at(pair.source, "mixamorig:Spine1");
    const core::DVec3 moved{0.0, -0.2, -0.1};
    pair.source[spine].localBind.position = pair.source[spine].localBind.position + moved;
    pair.source[next].localBind.position = pair.source[next].localBind.position - moved;
    const retarget::RigRoles roles = retarget::assignRoles(pair.source);
    const retarget::Map map = retarget::buildMap(pair.source, roles, pair.target, pair.targetRoles);
    REQUIRE(map.roles);

    const std::vector<core::CFrameD> rest = modelOf(pair.target, restOf(pair.target));
    const std::vector<core::CFrameD> stood = modelOf(pair.target, carried(map, restOf(pair.source), pair.target));
    const usize pelvis = at(pair.target, "pelvis");
    const usize first = at(pair.target, "spine_01");
    CHECK(sameRotation(stood[pelvis].rotation, rest[pelvis].rotation, 1.0e-4f));
    CHECK(stood[first].position.y > stood[pelvis].position.y);
    // The legs are still stood as the source stands, from the joint down.
    CHECK(nearly(direction(stood, at(pair.target, "thigh_l"), at(pair.target, "calf_l")), Down, 1.0e-3f));
}
