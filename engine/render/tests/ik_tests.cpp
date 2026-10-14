// The IK solvers (ADR 0198), on numbers: small rigs built by hand, and what
// each solver owes them.

#include <array>
#include <cmath>
#include <cstring>
#include <doctest/doctest.h>
#include <initializer_list>
#include <vector>

#include "engine/render/ik.h"

using namespace engine;
using core::f32;
using core::Mat3;
using core::Mat4;
using core::u32;
using core::usize;
using core::Vec3;

namespace {

constexpr u32 NoJoint = asset::Joint::NoParent;

// A skeleton and the pose it is in. Every joint is added unturned, at a place
// in model space; its rest pose is that place too, unless a test says another.
struct Rig
{
    std::vector<asset::Joint> joints;
    std::vector<Mat4> model;

    u32 add(u32 parent, Vec3 at)
    {
        asset::Joint joint;
        joint.parent = parent;
        const Vec3 from = parent == NoJoint ? Vec3{} : position(parent);
        joint.localBind.position = core::toDVec3(at - from);
        joints.push_back(joint);
        model.push_back(core::translation(at));
        return static_cast<u32>(joints.size() - 1);
    }

    [[nodiscard]] Vec3 position(u32 joint) const
    {
        return {model[joint].m[3][0], model[joint].m[3][1], model[joint].m[3][2]};
    }

    [[nodiscard]] Vec3 axis(u32 joint, int column) const
    {
        return {model[joint].m[column][0], model[joint].m[column][1], model[joint].m[column][2]};
    }

    // The joint's upper three columns: for a joint added unturned and
    // unscaled, the turn it has been given since.
    [[nodiscard]] Mat3 turn(u32 joint) const
    {
        Mat3 out;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                out.m[c][r] = model[joint].m[c][r];
        return out;
    }

    [[nodiscard]] f32 bone(u32 from, u32 to) const { return core::length(position(to) - position(from)); }

    void scale(u32 joint, Vec3 by) { model[joint] = model[joint] * core::scaling(by); }
};

[[nodiscard]] bool nearly(Vec3 a, Vec3 b, f32 within)
{
    return core::length(a - b) < within;
}

[[nodiscard]] bool nearly(f32 a, f32 b, f32 within)
{
    return std::abs(a - b) < within;
}

[[nodiscard]] bool nearly(const Mat3& a, const Mat3& b, f32 within)
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (!(std::abs(a.m[c][r] - b.m[c][r]) < within))
                return false;
    return true;
}

// Bit for bit: what "changes nothing" is held to.
[[nodiscard]] bool same(const std::vector<Mat4>& a, const std::vector<Mat4>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Mat4)) == 0;
}

[[nodiscard]] bool same(const Mat4& a, const Mat4& b)
{
    return std::memcmp(&a, &b, sizeof(Mat4)) == 0;
}

[[nodiscard]] bool allFinite(const std::vector<Mat4>& pose)
{
    for (const Mat4& m : pose)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m.m[c][r]))
                    return false;
    return true;
}

// How far a rotation turns, radians.
[[nodiscard]] f32 angleOf(const Mat3& turn)
{
    const f32 trace = turn.m[0][0] + turn.m[1][1] + turn.m[2][2];
    return std::acos(std::fmin(1.0f, std::fmax(-1.0f, (trace - 1.0f) * 0.5f)));
}

// An arm lying along +X, its elbow out towards +Z, and a finger on the hand
// so that what the hand carries can be seen.
struct Arm
{
    Rig rig;
    u32 shoulder = 0;
    u32 elbow = 0;
    u32 hand = 0;
    u32 finger = 0;
    f32 upper = 0.0f;
    f32 lower = 0.0f;
};

[[nodiscard]] Arm arm()
{
    Arm out;
    out.shoulder = out.rig.add(NoJoint, Vec3{0.0f, 0.0f, 0.0f});
    out.elbow = out.rig.add(out.shoulder, Vec3{1.0f, 0.0f, 0.3f});
    out.hand = out.rig.add(out.elbow, Vec3{2.0f, 0.0f, 0.0f});
    out.finger = out.rig.add(out.hand, Vec3{2.3f, 0.0f, 0.0f});
    out.upper = out.rig.bone(out.shoulder, out.elbow);
    out.lower = out.rig.bone(out.elbow, out.hand);
    return out;
}

// A spine, a neck and a head up +Y, on a base, with a hat on the head.
struct Torso
{
    Rig rig;
    u32 base = 0;
    u32 spine = 0;
    u32 neck = 0;
    u32 head = 0;
    u32 hat = 0;
};

[[nodiscard]] Torso torso()
{
    Torso out;
    out.base = out.rig.add(NoJoint, Vec3{0.0f, 0.0f, 0.0f});
    out.spine = out.rig.add(out.base, Vec3{0.0f, 1.0f, 0.0f});
    out.neck = out.rig.add(out.spine, Vec3{0.0f, 1.5f, 0.0f});
    out.head = out.rig.add(out.neck, Vec3{0.0f, 1.7f, 0.0f});
    out.hat = out.rig.add(out.head, Vec3{0.0f, 2.0f, 0.0f});
    return out;
}

constexpr Vec3 Ahead{0.0f, 0.0f, 1.0f};

// Hips with a spine on them and two legs: a thigh, a knee a little forward
// of straight, an ankle a tenth above the ground at nought, and a toe.
struct Legs
{
    Rig rig;
    u32 hips = 0;
    u32 spine = 0;
    u32 thigh[2]{};
    u32 knee[2]{};
    u32 ankle[2]{};
    u32 toe[2]{};
};

constexpr int Left = 0;
constexpr int Right = 1;
constexpr Vec3 ToeFromAnkle{0.0f, -0.08f, 0.15f};

// `leftLift`: how far above standing the clip has the left foot.
[[nodiscard]] Legs legs(f32 leftLift = 0.0f)
{
    Legs out;
    out.hips = out.rig.add(NoJoint, Vec3{0.0f, 1.0f, 0.0f});
    out.spine = out.rig.add(out.hips, Vec3{0.0f, 1.3f, 0.0f});
    for (int side = 0; side < 2; ++side) {
        const f32 x = side == Left ? 0.1f : -0.1f;
        const f32 lift = side == Left ? leftLift : 0.0f;
        out.thigh[side] = out.rig.add(out.hips, Vec3{x, 0.9f, 0.0f});
        out.knee[side] = out.rig.add(out.thigh[side], Vec3{x, 0.5f + lift * 0.5f, 0.08f + lift * 0.5f});
        out.ankle[side] = out.rig.add(out.knee[side], Vec3{x, 0.1f + lift, 0.0f});
        out.toe[side] = out.rig.add(out.ankle[side], Vec3{x, 0.1f + lift, 0.0f} + ToeFromAnkle);
    }
    return out;
}

[[nodiscard]] render::ik::Foot foot(u32 joint, f32 ground, Vec3 normal = Vec3{0.0f, 1.0f, 0.0f})
{
    render::ik::Foot out;
    out.joint = joint;
    out.hit = true;
    out.ground = ground;
    out.normal = normal;
    return out;
}

// How bent a knee is: nought for a straight leg.
[[nodiscard]] f32 bendOf(const Legs& body, int side)
{
    const Vec3 thigh = core::normalize(body.rig.position(body.knee[side]) - body.rig.position(body.thigh[side]));
    const Vec3 shin = core::normalize(body.rig.position(body.ankle[side]) - body.rig.position(body.knee[side]));
    return std::acos(std::fmin(1.0f, core::dot(thigh, shin)));
}

void checkLegLengths(const Legs& now, const Legs& before)
{
    for (int side = 0; side < 2; ++side) {
        CHECK(nearly(now.rig.bone(now.thigh[side], now.knee[side]),
                     before.rig.bone(before.thigh[side], before.knee[side]), 1e-5f));
        CHECK(nearly(now.rig.bone(now.knee[side], now.ankle[side]),
                     before.rig.bone(before.knee[side], before.ankle[side]), 1e-5f));
        CHECK(nearly(now.rig.bone(now.ankle[side], now.toe[side]),
                     before.rig.bone(before.ankle[side], before.toe[side]), 1e-5f));
    }
}

} // namespace

// --- place -------------------------------------------------------------------

TEST_CASE("IK place: a joint is put where it is told and everything below it keeps its place relative to it")
{
    Arm body = arm();
    const Mat4 placed = core::translation(Vec3{0.5f, 2.0f, -1.0f}) *
                        core::toRenderMatrix(core::CFrameD{core::DVec3{}, core::rotationY(0.9f)}, core::DVec3{}) *
                        core::scaling(Vec3{2.0f, 2.0f, 2.0f});
    render::ik::place(body.rig.joints, body.rig.model, body.elbow, placed);

    CHECK(same(body.rig.model[body.elbow], placed));
    // The shoulder is above it and is not touched.
    CHECK(nearly(body.rig.position(body.shoulder), Vec3{0.0f, 0.0f, 0.0f}, 1e-6f));
    // The hand was at (1, 0, -0.3) from an unturned elbow; the finger 0.3
    // further along x.
    CHECK(nearly(body.rig.position(body.hand), core::transformPoint(placed, Vec3{1.0f, 0.0f, -0.3f}), 1e-5f));
    CHECK(nearly(body.rig.position(body.finger), core::transformPoint(placed, Vec3{1.3f, 0.0f, -0.3f}), 1e-5f));
    // And they took its turn and its scale with it.
    CHECK(nearly(body.rig.axis(body.hand, 0), core::transformDirection(placed, Vec3{1.0f, 0.0f, 0.0f}), 1e-5f));
}

TEST_CASE("IK place: a joint scaled to nothing carries its children by how far it moved, and an index out of range "
          "changes nothing")
{
    Arm body = arm();
    body.rig.scale(body.elbow, Vec3{0.0f, 0.0f, 0.0f});
    const Vec3 hand = body.rig.position(body.hand);
    render::ik::place(body.rig.joints, body.rig.model, body.elbow, core::translation(Vec3{1.0f, 4.0f, 0.3f}));
    CHECK(allFinite(body.rig.model));
    CHECK(nearly(body.rig.position(body.hand), hand + Vec3{0.0f, 4.0f, 0.0f}, 1e-5f));

    const std::vector<Mat4> before = body.rig.model;
    render::ik::place(body.rig.joints, body.rig.model, 99, Mat4{});
    CHECK(same(body.rig.model, before));
}

// --- two bones ---------------------------------------------------------------

TEST_CASE("IK two-bone: a target inside reach is reached exactly and both bones keep their length")
{
    Arm body = arm();
    const Vec3 target{1.2f, 0.8f, 0.1f};
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, nullptr, nullptr, 1.0f));

    CHECK(nearly(body.rig.position(body.hand), target, 1e-4f));
    CHECK(nearly(body.rig.position(body.shoulder), Vec3{}, 1e-6f));
    CHECK(nearly(body.rig.bone(body.shoulder, body.elbow), body.upper, 1e-5f));
    CHECK(nearly(body.rig.bone(body.elbow, body.hand), body.lower, 1e-5f));
    // The elbow turned to point down its bone: its own +X no longer lies
    // along the world's.
    CHECK(!nearly(body.rig.axis(body.elbow, 0), Vec3{1.0f, 0.0f, 0.0f}, 1e-2f));
    // The hand kept the way it faced, and the finger is where that puts it.
    CHECK(nearly(body.rig.turn(body.hand), Mat3{}, 1e-5f));
    CHECK(nearly(body.rig.position(body.finger), target + Vec3{0.3f, 0.0f, 0.0f}, 1e-4f));
}

TEST_CASE("IK two-bone: a target beyond reach leaves the limb straight, pointing at it, at full reach")
{
    Arm body = arm();
    const Vec3 target{5.0f, 5.0f, 1.0f};
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, nullptr, nullptr, 1.0f));

    const Vec3 towards = core::normalize(target);
    CHECK(nearly(body.rig.position(body.elbow), towards * body.upper, 1e-5f));
    CHECK(nearly(body.rig.position(body.hand), towards * (body.upper + body.lower), 1e-5f));
    CHECK(nearly(body.rig.bone(body.shoulder, body.elbow), body.upper, 1e-5f));
    CHECK(nearly(body.rig.bone(body.elbow, body.hand), body.lower, 1e-5f));
}

TEST_CASE("IK two-bone: the middle joint bends to the pole's side of the line from root to target")
{
    const Vec3 target{1.5f, 0.2f, 0.0f};
    const Vec3 along = core::normalize(target);
    for (const Vec3 pole : {Vec3{0.7f, 4.0f, 0.5f}, Vec3{0.7f, -4.0f, 0.5f}, Vec3{0.7f, 0.0f, -4.0f}}) {
        Arm body = arm();
        CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, nullptr, &pole, 1.0f));
        CHECK(nearly(body.rig.position(body.hand), target, 1e-4f));
        CHECK(nearly(body.rig.bone(body.shoulder, body.elbow), body.upper, 1e-5f));
        CHECK(nearly(body.rig.bone(body.elbow, body.hand), body.lower, 1e-5f));

        const Vec3 elbow = body.rig.position(body.elbow);
        const Vec3 elbowSide = core::normalize(elbow - along * core::dot(elbow, along));
        const Vec3 poleSide = core::normalize(pole - along * core::dot(pole, along));
        // Not merely the same half: the elbow is in the plane the pole names.
        CHECK(core::dot(elbowSide, poleSide) > 0.9999f);
    }
}

TEST_CASE("IK two-bone: with no pole the limb bends in the plane it was already bent in")
{
    // The arm lies in the plane y = 0 with its elbow on the +Z side of the
    // line from shoulder to hand. A target in that plane leaves it there, and
    // on the same side of the line as it swings round.
    Arm body = arm();
    const Vec3 sideBefore = core::cross(body.rig.position(body.hand), body.rig.position(body.elbow));
    const Vec3 target{1.4f, 0.0f, -0.9f};
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, nullptr, nullptr, 1.0f));

    CHECK(nearly(body.rig.position(body.hand), target, 1e-4f));
    CHECK(nearly(body.rig.position(body.elbow).y, 0.0f, 1e-5f));
    const Vec3 sideAfter = core::cross(body.rig.position(body.hand), body.rig.position(body.elbow));
    CHECK(sideBefore.y * sideAfter.y > 0.0f);

    // And a target out of the plane swings the plane with the limb rather
    // than rolling the elbow about it: the limb's plane still holds the line
    // the swing turned about.
    Arm lifted = arm();
    const Vec3 raised{1.6f, 0.9f, 0.0f};
    CHECK(render::ik::solveTwoBone(lifted.rig.joints, lifted.rig.model, lifted.hand, raised, nullptr, nullptr, 1.0f));
    CHECK(nearly(lifted.rig.position(lifted.hand), raised, 1e-4f));
    // The swing from +X to `raised` is about Z, so the elbow's +Z offset from
    // the line is still along +Z.
    const Vec3 along = core::normalize(raised);
    const Vec3 elbow = lifted.rig.position(lifted.elbow);
    CHECK(nearly(core::normalize(elbow - along * core::dot(elbow, along)), Vec3{0.0f, 0.0f, 1.0f}, 1e-4f));
}

TEST_CASE("IK two-bone: weight nought changes nothing, bit for bit, and a half puts the end halfway")
{
    Arm body = arm();
    const std::vector<Mat4> before = body.rig.model;
    const Vec3 target{1.2f, 0.8f, 0.1f};
    const Mat3 rotation = core::rotationZ(0.7f);
    const Vec3 pole{0.0f, 5.0f, 0.0f};
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, &rotation, &pole, 0.0f));
    CHECK(same(body.rig.model, before));
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, &rotation, &pole, -3.0f));
    CHECK(same(body.rig.model, before));

    const Vec3 hand = body.rig.position(body.hand);
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, &rotation, nullptr, 0.5f));
    CHECK(nearly(body.rig.position(body.hand), hand + (target - hand) * 0.5f, 1e-4f));
    CHECK(nearly(body.rig.bone(body.shoulder, body.elbow), body.upper, 1e-5f));
    CHECK(nearly(body.rig.bone(body.elbow, body.hand), body.lower, 1e-5f));
    // Half the way to the orientation too.
    CHECK(nearly(body.rig.turn(body.hand), core::rotationZ(0.35f), 1e-5f));

    // And a weight past one is one.
    Arm whole = arm();
    CHECK(render::ik::solveTwoBone(whole.rig.joints, whole.rig.model, whole.hand, target, nullptr, nullptr, 7.0f));
    CHECK(nearly(whole.rig.position(whole.hand), target, 1e-4f));
}

TEST_CASE("IK two-bone: a rotation given is taken by the end, and a child of the end is carried")
{
    Arm body = arm();
    const Vec3 target{1.2f, 0.8f, 0.1f};
    const Mat3 rotation = core::rotationZ(0.7f) * core::rotationX(0.3f);
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, &rotation, nullptr, 1.0f));

    CHECK(nearly(body.rig.position(body.hand), target, 1e-4f));
    CHECK(nearly(body.rig.turn(body.hand), rotation, 1e-5f));
    CHECK(nearly(body.rig.position(body.finger), target + rotation * Vec3{0.3f, 0.0f, 0.0f}, 1e-4f));
    CHECK(nearly(body.rig.turn(body.finger), rotation, 1e-5f));
}

TEST_CASE("IK two-bone: a scaled joint keeps its scale")
{
    Arm body = arm();
    body.rig.scale(body.shoulder, Vec3{1.5f, 1.5f, 1.5f});
    body.rig.scale(body.elbow, Vec3{2.0f, 1.0f, 0.5f});
    body.rig.scale(body.hand, Vec3{3.0f, 0.25f, 1.0f});
    const Vec3 target{0.9f, 1.1f, -0.4f};
    const Mat3 rotation = core::rotationY(1.1f) * core::rotationZ(-0.4f);
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, &rotation, nullptr, 1.0f));

    CHECK(nearly(body.rig.position(body.hand), target, 1e-4f));
    CHECK(nearly(core::length(body.rig.axis(body.shoulder, 0)), 1.5f, 1e-5f));
    CHECK(nearly(core::length(body.rig.axis(body.shoulder, 2)), 1.5f, 1e-5f));
    CHECK(nearly(core::length(body.rig.axis(body.elbow, 0)), 2.0f, 1e-5f));
    CHECK(nearly(core::length(body.rig.axis(body.elbow, 1)), 1.0f, 1e-5f));
    CHECK(nearly(core::length(body.rig.axis(body.elbow, 2)), 0.5f, 1e-5f));
    // The hand's columns are the rotation's, each as long as it was.
    CHECK(nearly(body.rig.axis(body.hand, 0), rotation * Vec3{3.0f, 0.0f, 0.0f}, 1e-5f));
    CHECK(nearly(body.rig.axis(body.hand, 1), rotation * Vec3{0.0f, 0.25f, 0.0f}, 1e-5f));
    CHECK(nearly(body.rig.axis(body.hand, 2), rotation * Vec3{0.0f, 0.0f, 1.0f}, 1e-5f));
}

TEST_CASE("IK two-bone: a straight limb with no pole bends the way its rest pose bends")
{
    // At rest the forearm leans towards -Z, which puts the elbow on the +Z
    // side of the line; the pose has the arm dead straight along +X.
    Rig rig;
    const u32 shoulder = rig.add(NoJoint, Vec3{0.0f, 0.0f, 0.0f});
    const u32 elbow = rig.add(shoulder, Vec3{1.0f, 0.0f, 0.0f});
    const u32 hand = rig.add(elbow, Vec3{2.0f, 0.0f, 0.0f});
    rig.joints[hand].localBind.position = core::DVec3{1.0, 0.0, -0.5};

    Rig bent = rig;
    CHECK(render::ik::solveTwoBone(bent.joints, bent.model, hand, Vec3{1.5f, 0.0f, 0.0f}, nullptr, nullptr, 1.0f));
    CHECK(nearly(bent.position(hand), Vec3{1.5f, 0.0f, 0.0f}, 1e-4f));
    CHECK(bent.position(elbow).z > 0.5f);
    CHECK(nearly(bent.position(elbow).y, 0.0f, 1e-5f));
    CHECK(nearly(bent.bone(shoulder, elbow), 1.0f, 1e-5f));
    CHECK(nearly(bent.bone(elbow, hand), 1.0f, 1e-5f));

    // The other way at rest, the other way now.
    rig.joints[hand].localBind.position = core::DVec3{1.0, 0.0, 0.5};
    CHECK(render::ik::solveTwoBone(rig.joints, rig.model, hand, Vec3{1.5f, 0.0f, 0.0f}, nullptr, nullptr, 1.0f));
    CHECK(rig.position(elbow).z < -0.5f);
}

TEST_CASE("IK two-bone: a leg straight in the pose and at rest, with no pole, puts its knee forward")
{
    // Nothing says which way: the fixed answer for a limb hanging straight
    // down is +Z, which is the front of a model as the file format has it.
    Rig rig;
    const u32 thigh = rig.add(NoJoint, Vec3{0.1f, 0.9f, 0.0f});
    const u32 knee = rig.add(thigh, Vec3{0.1f, 0.5f, 0.0f});
    const u32 ankle = rig.add(knee, Vec3{0.1f, 0.1f, 0.0f});
    CHECK(render::ik::solveTwoBone(rig.joints, rig.model, ankle, Vec3{0.1f, 0.3f, 0.0f}, nullptr, nullptr, 1.0f));
    CHECK(nearly(rig.position(ankle), Vec3{0.1f, 0.3f, 0.0f}, 1e-4f));
    CHECK(rig.position(knee).z > 0.2f);
    CHECK(nearly(rig.position(knee).x, 0.1f, 1e-5f));
    CHECK(nearly(rig.bone(thigh, knee), 0.4f, 1e-5f));
    CHECK(nearly(rig.bone(knee, ankle), 0.4f, 1e-5f));
}

TEST_CASE("IK two-bone: nothing it can be handed makes a NaN")
{
    const Vec3 onRoot{0.0f, 0.0f, 0.0f};
    const Vec3 inside{1.0f, 0.5f, 0.0f};

    // A limb straight in the pose and at rest, and no pole: some fixed side.
    {
        Rig rig;
        const u32 shoulder = rig.add(NoJoint, Vec3{0.0f, 0.0f, 0.0f});
        const u32 elbow = rig.add(shoulder, Vec3{1.0f, 0.0f, 0.0f});
        const u32 hand = rig.add(elbow, Vec3{2.0f, 0.0f, 0.0f});
        CHECK(render::ik::solveTwoBone(rig.joints, rig.model, hand, Vec3{1.2f, 0.0f, 0.0f}, nullptr, nullptr, 1.0f));
        CHECK(allFinite(rig.model));
        CHECK(nearly(rig.position(hand), Vec3{1.2f, 0.0f, 0.0f}, 1e-4f));
        CHECK(nearly(rig.bone(shoulder, elbow), 1.0f, 1e-5f));
        CHECK(nearly(rig.bone(elbow, hand), 1.0f, 1e-5f));
    }
    // A target on the root joint: bones of one length fold onto it.
    {
        Rig rig;
        const u32 shoulder = rig.add(NoJoint, Vec3{0.0f, 0.0f, 0.0f});
        const u32 elbow = rig.add(shoulder, Vec3{1.0f, 0.0f, 0.0f});
        const u32 hand = rig.add(elbow, Vec3{2.0f, 0.0f, 0.0f});
        CHECK(render::ik::solveTwoBone(rig.joints, rig.model, hand, onRoot, nullptr, nullptr, 1.0f));
        CHECK(allFinite(rig.model));
        CHECK(nearly(rig.position(hand), onRoot, 1e-4f));
        CHECK(nearly(rig.bone(shoulder, elbow), 1.0f, 1e-5f));
    }
    // And bones of two lengths fold as far as they go.
    {
        Arm body = arm();
        body.rig.model[body.hand] = core::translation(Vec3{1.4f, 0.0f, 0.0f});
        const f32 lower = body.rig.bone(body.elbow, body.hand);
        CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, onRoot, nullptr, nullptr, 1.0f));
        CHECK(allFinite(body.rig.model));
        CHECK(nearly(body.rig.bone(body.shoulder, body.elbow), body.upper, 1e-5f));
        CHECK(nearly(body.rig.bone(body.elbow, body.hand), lower, 1e-5f));
        CHECK(nearly(core::length(body.rig.position(body.hand)), body.upper - lower, 1e-5f));
    }
    // A pole on the limb's line says nothing; the plane it had is kept.
    {
        Arm body = arm();
        const Vec3 pole = inside * 3.0f;
        CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, inside, nullptr, &pole, 1.0f));
        CHECK(allFinite(body.rig.model));
        CHECK(nearly(body.rig.position(body.hand), inside, 1e-4f));
    }
    // Bones of no length at all, one and then both.
    {
        Rig rig;
        const u32 shoulder = rig.add(NoJoint, Vec3{0.0f, 0.0f, 0.0f});
        const u32 elbow = rig.add(shoulder, Vec3{0.0f, 0.0f, 0.0f});
        const u32 hand = rig.add(elbow, Vec3{1.0f, 0.0f, 0.0f});
        const Vec3 pole{0.0f, 1.0f, 0.0f};
        CHECK(render::ik::solveTwoBone(rig.joints, rig.model, hand, Vec3{0.0f, 0.6f, 0.0f}, nullptr, &pole, 1.0f));
        CHECK(allFinite(rig.model));
        CHECK(nearly(rig.bone(elbow, hand), 1.0f, 1e-5f));

        Rig point;
        (void)point.add(NoJoint, Vec3{});
        (void)point.add(0, Vec3{});
        (void)point.add(1, Vec3{});
        const Mat3 rotation = core::rotationX(0.5f);
        CHECK(render::ik::solveTwoBone(point.joints, point.model, 2, inside, &rotation, &pole, 0.5f));
        CHECK(allFinite(point.model));
    }
    // A joint scaled to nothing, asked to take an orientation.
    {
        Arm body = arm();
        body.rig.scale(body.hand, Vec3{0.0f, 0.0f, 0.0f});
        const Mat3 rotation = core::rotationX(0.5f);
        CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, inside, &rotation, nullptr, 1.0f));
        CHECK(allFinite(body.rig.model));
    }
}

TEST_CASE("IK two-bone: an end with no parent and grandparent, or out of range, is refused and nothing changes")
{
    Arm body = arm();
    const std::vector<Mat4> before = body.rig.model;
    const Vec3 target{1.2f, 0.8f, 0.1f};
    CHECK_FALSE(
        render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.shoulder, target, nullptr, nullptr, 1.0f));
    CHECK_FALSE(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.elbow, target, nullptr, nullptr, 1.0f));
    CHECK_FALSE(render::ik::solveTwoBone(body.rig.joints, body.rig.model, 4, target, nullptr, nullptr, 1.0f));
    CHECK_FALSE(render::ik::solveTwoBone(body.rig.joints, body.rig.model, NoJoint, target, nullptr, nullptr, 1.0f));
    CHECK(same(body.rig.model, before));
}

TEST_CASE("IK two-bone: a weight near nought is a pose near the clip's, pole and rotation and all")
{
    // Nothing snaps as a control fades in: not the bend, which a pole on the
    // far side of the limb would otherwise flip at once, and not the hand.
    Arm body = arm();
    const Arm clip = arm();
    const Vec3 target{1.2f, 0.8f, 0.1f};
    const Vec3 pole{1.0f, 0.0f, -6.0f};
    const Mat3 rotation = core::rotationZ(1.4f);
    CHECK(render::ik::solveTwoBone(body.rig.joints, body.rig.model, body.hand, target, &rotation, &pole, 0.001f));
    for (const u32 joint : {body.shoulder, body.elbow, body.hand, body.finger}) {
        CHECK(nearly(body.rig.position(joint), clip.rig.position(joint), 5e-3f));
        CHECK(nearly(body.rig.turn(joint), Mat3{}, 5e-3f));
    }
}

TEST_CASE("IK carry: a rig far larger than a skin is drawn with is carried as a small one is")
{
    // The same arm twice: as it is, and with three hundred joints that are
    // nothing to do with it numbered between its shoulder and its elbow -- so
    // the elbow, the hand and what hangs from them come after more joints
    // than the pass keeps its answers for.
    struct Built
    {
        Rig rig;
        std::array<u32, 5> arm{};
        u32 firstIdle = 0;
    };
    const auto build = [](int idle) {
        Built out;
        const u32 base = out.rig.add(NoJoint, Vec3{0.0f, 0.0f, 0.0f});
        out.arm[0] = out.rig.add(base, Vec3{0.0f, 0.0f, 0.0f});
        out.firstIdle = static_cast<u32>(out.rig.joints.size());
        for (int index = 0; index < idle; ++index)
            (void)out.rig.add(base, Vec3{static_cast<f32>(index), 5.0f, 0.0f});
        out.arm[1] = out.rig.add(out.arm[0], Vec3{1.0f, 0.0f, 0.3f});
        out.arm[2] = out.rig.add(out.arm[1], Vec3{2.0f, 0.0f, 0.0f});
        out.arm[3] = out.rig.add(out.arm[2], Vec3{2.3f, 0.0f, 0.0f});
        // A pad on the upper arm, and one on the forearm.
        out.arm[4] = out.rig.add(out.arm[0], Vec3{0.5f, 0.2f, 0.0f});
        (void)out.rig.add(out.arm[1], Vec3{1.5f, 0.2f, 0.0f});
        return out;
    };
    Built plain = build(0);
    Built padded = build(300);
    const std::vector<Mat4> before = padded.rig.model;
    const Vec3 target{1.2f, 0.8f, 0.1f};
    const Mat3 rotation = core::rotationZ(0.7f);
    CHECK(render::ik::solveTwoBone(plain.rig.joints, plain.rig.model, plain.arm[2], target, &rotation, nullptr, 1.0f));
    CHECK(
        render::ik::solveTwoBone(padded.rig.joints, padded.rig.model, padded.arm[2], target, &rotation, nullptr, 1.0f));

    CHECK(nearly(padded.rig.position(padded.arm[2]), target, 1e-4f));
    for (usize index = 0; index < plain.arm.size(); ++index)
        CHECK(same(padded.rig.model[padded.arm[index]], plain.rig.model[plain.arm[index]]));
    CHECK(same(padded.rig.model.back(), plain.rig.model.back()));
    // The pads moved, and the joints that are nobody's did not.
    CHECK(!nearly(padded.rig.position(padded.arm[4]), Vec3{0.5f, 0.2f, 0.0f}, 1e-2f));
    for (u32 joint = padded.firstIdle; joint < padded.firstIdle + 300; ++joint)
        CHECK(same(padded.rig.model[joint], before[joint]));
}

// --- look --------------------------------------------------------------------

TEST_CASE("IK look: a chain length of nought turns the end alone, to face the target, and carries its child")
{
    Torso body = torso();
    const std::vector<Mat4> before = body.rig.model;
    const Vec3 target{1.0f, 2.7f, 1.0f};
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 0, target, Ahead, 3.0f, 1.0f));

    const Mat3 turn = body.rig.turn(body.head);
    const Vec3 head = body.rig.position(body.head);
    CHECK(nearly(head, Vec3{0.0f, 1.7f, 0.0f}, 1e-6f));
    CHECK(nearly(turn * Ahead, core::normalize(target - head), 1e-3f));
    // Nothing above the head moved at all.
    CHECK(same(body.rig.model[body.base], before[body.base]));
    CHECK(same(body.rig.model[body.spine], before[body.spine]));
    CHECK(same(body.rig.model[body.neck], before[body.neck]));
    // The hat sat 0.3 above an unturned head.
    CHECK(nearly(body.rig.position(body.hat), head + turn * Vec3{0.0f, 0.3f, 0.0f}, 1e-5f));
    CHECK(nearly(body.rig.turn(body.hat), turn, 1e-5f));
}

TEST_CASE("IK look: a chain shares the turn equally, each joint about its own place, and the end faces the target")
{
    Torso body = torso();
    const Vec3 target{2.0f, 1.2f, 3.0f};
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 2, target, Ahead, 3.0f, 1.0f));

    // The base is above the chain and is where it was.
    CHECK(nearly(body.rig.turn(body.base), Mat3{}, 1e-7f));
    const Mat3 spine = body.rig.turn(body.spine);
    const Mat3 neck = body.rig.turn(body.neck);
    const Mat3 head = body.rig.turn(body.head);
    const f32 whole = angleOf(head);
    CHECK(whole > 0.3f);
    // One part, two parts, three: each joint turned a third and stands on
    // what the ones above it turned.
    CHECK(nearly(angleOf(spine), whole / 3.0f, 1e-4f));
    CHECK(nearly(angleOf(neck), whole * 2.0f / 3.0f, 1e-4f));
    CHECK(nearly(neck, spine * spine, 1e-5f));
    CHECK(nearly(head, spine * spine * spine, 1e-5f));

    // Each about its own place.
    const Vec3 spineAt{0.0f, 1.0f, 0.0f};
    CHECK(nearly(body.rig.position(body.spine), spineAt, 1e-6f));
    const Vec3 neckAt = spineAt + spine * Vec3{0.0f, 0.5f, 0.0f};
    CHECK(nearly(body.rig.position(body.neck), neckAt, 1e-5f));
    const Vec3 headAt = neckAt + neck * Vec3{0.0f, 0.2f, 0.0f};
    CHECK(nearly(body.rig.position(body.head), headAt, 1e-5f));
    CHECK(nearly(body.rig.position(body.hat), headAt + head * Vec3{0.0f, 0.3f, 0.0f}, 1e-5f));

    // And from where the turn left the head, it faces the target.
    CHECK(nearly(head * Ahead, core::normalize(target - headAt), 1e-3f));

    // A chain longer than the skeleton is tall is the whole of it.
    Torso tall = torso();
    CHECK(render::ik::solveLook(tall.rig.joints, tall.rig.model, tall.head, 40, target, Ahead, 3.0f, 1.0f));
    CHECK(nearly(angleOf(tall.rig.turn(tall.base)) * 4.0f, angleOf(tall.rig.turn(tall.head)), 1e-4f));
    CHECK(nearly(tall.rig.turn(tall.head) * Ahead, core::normalize(target - tall.rig.position(tall.head)), 1e-3f));
}

TEST_CASE("IK look: past its limit the whole turn is exactly the limit")
{
    const Vec3 target{3.0f, 1.7f, -1.0f};
    for (const u32 chainLength : {0u, 2u}) {
        Torso body = torso();
        CHECK(
            render::ik::solveLook(body.rig.joints, body.rig.model, body.head, chainLength, target, Ahead, 0.5f, 1.0f));
        CHECK(nearly(angleOf(body.rig.turn(body.head)), 0.5f, 1e-5f));
        // Towards the target, not away from it.
        CHECK((body.rig.turn(body.head) * Ahead).x > 0.4f);
    }

    // Straight behind: round about the way up, as far as it may go.
    Torso body = torso();
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 0, Vec3{0.0f, 1.7f, -5.0f}, Ahead, 1.2f,
                                1.0f));
    CHECK(allFinite(body.rig.model));
    CHECK(nearly(angleOf(body.rig.turn(body.head)), 1.2f, 1e-5f));
    CHECK(nearly(body.rig.axis(body.head, 1), Vec3{0.0f, 1.0f, 0.0f}, 1e-5f));
}

TEST_CASE("IK look: weight scales the angle")
{
    const Vec3 target{2.0f, 1.2f, 3.0f};
    Torso whole = torso();
    CHECK(render::ik::solveLook(whole.rig.joints, whole.rig.model, whole.head, 2, target, Ahead, 3.0f, 1.0f));
    Torso half = torso();
    CHECK(render::ik::solveLook(half.rig.joints, half.rig.model, half.head, 2, target, Ahead, 3.0f, 0.5f));
    Torso quarter = torso();
    CHECK(render::ik::solveLook(quarter.rig.joints, quarter.rig.model, quarter.head, 2, target, Ahead, 3.0f, 0.25f));

    const f32 full = angleOf(whole.rig.turn(whole.head));
    CHECK(nearly(angleOf(half.rig.turn(half.head)), full * 0.5f, 1e-4f));
    CHECK(nearly(angleOf(quarter.rig.turn(quarter.head)), full * 0.25f, 1e-4f));
    CHECK(nearly(angleOf(half.rig.turn(half.spine)), full / 6.0f, 1e-4f));
}

TEST_CASE("IK look: a target at the joint, a weight of nought and a direction of no length change nothing")
{
    Torso body = torso();
    const std::vector<Mat4> before = body.rig.model;
    const Vec3 target{2.0f, 1.2f, 3.0f};
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 2, Vec3{0.0f, 1.7f, 0.0f}, Ahead, 3.0f,
                                1.0f));
    CHECK(same(body.rig.model, before));
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 2, target, Ahead, 3.0f, 0.0f));
    CHECK(same(body.rig.model, before));
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 2, target, Vec3{}, 3.0f, 1.0f));
    CHECK(same(body.rig.model, before));
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 2, target, Ahead, 0.0f, 1.0f));
    CHECK(same(body.rig.model, before));
    // Already facing it.
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 2, Vec3{0.0f, 1.7f, 9.0f}, Ahead, 3.0f,
                                1.0f));
    CHECK(same(body.rig.model, before));
    CHECK_FALSE(render::ik::solveLook(body.rig.joints, body.rig.model, 5, 2, target, Ahead, 3.0f, 1.0f));
    CHECK(same(body.rig.model, before));
}

TEST_CASE("IK look: a scaled joint keeps its scale, and a target inside the chain makes no NaN")
{
    Torso body = torso();
    body.rig.scale(body.neck, Vec3{2.0f, 0.5f, 1.0f});
    CHECK(render::ik::solveLook(body.rig.joints, body.rig.model, body.head, 2, Vec3{2.0f, 1.2f, 3.0f}, Ahead, 3.0f,
                                1.0f));
    CHECK(nearly(core::length(body.rig.axis(body.neck, 0)), 2.0f, 1e-5f));
    CHECK(nearly(core::length(body.rig.axis(body.neck, 1)), 0.5f, 1e-5f));
    CHECK(nearly(core::length(body.rig.axis(body.neck, 2)), 1.0f, 1e-5f));

    // Nearer than the chain is long, where the aims do not settle: still an
    // answer, still every bone its length.
    Torso nearby = torso();
    CHECK(render::ik::solveLook(nearby.rig.joints, nearby.rig.model, nearby.head, 3, Vec3{0.05f, 1.6f, 0.02f}, Ahead,
                                3.0f, 1.0f));
    CHECK(allFinite(nearby.rig.model));
    CHECK(nearly(nearby.rig.bone(nearby.spine, nearby.neck), 0.5f, 1e-5f));
    CHECK(nearly(nearby.rig.bone(nearby.neck, nearby.head), 0.2f, 1e-5f));
}

// --- feet --------------------------------------------------------------------

TEST_CASE("IK feet: flat ground at the clip's height changes nothing")
{
    Legs body = legs();
    const std::vector<Mat4> before = body.rig.model;
    const render::ik::FeetResult result =
        render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.ankle[Left], 0.0f),
                              foot(body.ankle[Right], 0.0f), 0.0f, 0.3f, true, 1.0f);
    CHECK(same(body.rig.model, before));
    CHECK(result.leftPlanted);
    CHECK(result.rightPlanted);
    CHECK(result.hipsOffset == 0.0f);
}

TEST_CASE("IK feet: a step up under one foot plants it higher, leaves the hips where they were and bends that knee")
{
    const Legs clip = legs();
    Legs body = legs();
    const render::ik::FeetResult result =
        render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.ankle[Left], 0.15f),
                              foot(body.ankle[Right], 0.0f), 0.0f, 0.3f, false, 1.0f);
    CHECK(result.leftPlanted);
    CHECK(result.rightPlanted);
    CHECK(result.hipsOffset == 0.0f);

    // The hips, the spine and the whole right leg are the clip's, to the bit.
    for (const u32 joint :
         {body.hips, body.spine, body.thigh[Right], body.knee[Right], body.ankle[Right], body.toe[Right]})
        CHECK(same(body.rig.model[joint], clip.rig.model[joint]));

    CHECK(nearly(body.rig.position(body.ankle[Left]), Vec3{0.1f, 0.25f, 0.0f}, 1e-4f));
    CHECK(nearly(body.rig.position(body.thigh[Left]), Vec3{0.1f, 0.9f, 0.0f}, 1e-6f));
    CHECK(bendOf(body, Left) > bendOf(clip, Left) + 0.3f);
    // The knee bends the way it was bent: forward, and in the leg's own plane.
    CHECK(body.rig.position(body.knee[Left]).z > 0.08f);
    CHECK(nearly(body.rig.position(body.knee[Left]).x, 0.1f, 1e-5f));
    // The foot stays flat, and its toe with it.
    CHECK(nearly(body.rig.turn(body.ankle[Left]), Mat3{}, 1e-6f));
    CHECK(nearly(body.rig.position(body.toe[Left]), Vec3{0.1f, 0.25f, 0.0f} + ToeFromAnkle, 1e-4f));
    checkLegLengths(body, clip);
}

TEST_CASE("IK feet: a step down under one foot lowers the hips by the step, plants the low foot and bends the other "
          "knee")
{
    const Legs clip = legs();
    Legs body = legs();
    const render::ik::FeetResult result =
        render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.ankle[Left], -0.15f),
                              foot(body.ankle[Right], 0.0f), 0.0f, 0.3f, false, 1.0f);
    CHECK(result.leftPlanted);
    CHECK(result.rightPlanted);
    CHECK(nearly(result.hipsOffset, -0.15f, 1e-7f));

    const Vec3 down{0.0f, -0.15f, 0.0f};
    CHECK(nearly(body.rig.position(body.hips), clip.rig.position(clip.hips) + down, 1e-6f));
    CHECK(nearly(body.rig.position(body.spine), clip.rig.position(clip.spine) + down, 1e-6f));
    // The low leg went down whole: it did not have to stretch.
    CHECK(nearly(body.rig.position(body.thigh[Left]), clip.rig.position(clip.thigh[Left]) + down, 1e-6f));
    CHECK(nearly(body.rig.position(body.knee[Left]), clip.rig.position(clip.knee[Left]) + down, 1e-6f));
    CHECK(nearly(body.rig.position(body.ankle[Left]), Vec3{0.1f, -0.05f, 0.0f}, 1e-6f));
    CHECK(nearly(body.rig.position(body.toe[Left]), Vec3{0.1f, -0.05f, 0.0f} + ToeFromAnkle, 1e-6f));
    CHECK(nearly(bendOf(body, Left), bendOf(clip, Left), 1e-5f));
    // The high leg's thigh came down with the hips and its ankle did not.
    CHECK(nearly(body.rig.position(body.thigh[Right]), clip.rig.position(clip.thigh[Right]) + down, 1e-6f));
    CHECK(nearly(body.rig.position(body.ankle[Right]), Vec3{-0.1f, 0.1f, 0.0f}, 1e-4f));
    CHECK(nearly(body.rig.position(body.toe[Right]), Vec3{-0.1f, 0.1f, 0.0f} + ToeFromAnkle, 1e-4f));
    CHECK(bendOf(body, Right) > bendOf(clip, Right) + 0.3f);
    CHECK(body.rig.position(body.knee[Right]).z > 0.08f);
    checkLegLengths(body, clip);
}

TEST_CASE("IK feet: a foot the clip lifted stays lifted by the same amount over its ground")
{
    // The clip has the left foot 0.2 above standing; the ground under it is
    // 0.1 up. The ankle stands 0.1 off the ground, so: 0.1 + 0.2 + 0.1.
    const Legs clip = legs(0.2f);
    Legs body = legs(0.2f);
    const render::ik::FeetResult result =
        render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.ankle[Left], 0.1f),
                              foot(body.ankle[Right], 0.0f), 0.0f, 0.3f, false, 1.0f);
    CHECK(result.leftPlanted);
    CHECK(result.hipsOffset == 0.0f);
    CHECK(nearly(body.rig.position(body.ankle[Left]), Vec3{0.1f, 0.4f, 0.0f}, 1e-4f));
    checkLegLengths(body, clip);

    // Both grounds up by the same: the whole body rises and no knee bends.
    Legs raised = legs(0.2f);
    const render::ik::FeetResult both =
        render::ik::solveFeet(raised.rig.joints, raised.rig.model, raised.hips, foot(raised.ankle[Left], 0.1f),
                              foot(raised.ankle[Right], 0.1f), 0.0f, 0.3f, false, 1.0f);
    CHECK(nearly(both.hipsOffset, 0.1f, 1e-7f));
    CHECK(nearly(raised.rig.position(raised.ankle[Left]), Vec3{0.1f, 0.4f, 0.0f}, 1e-6f));
    CHECK(nearly(raised.rig.position(raised.ankle[Right]), Vec3{-0.1f, 0.2f, 0.0f}, 1e-6f));
    CHECK(nearly(bendOf(raised, Left), bendOf(clip, Left), 1e-5f));
    CHECK(nearly(bendOf(raised, Right), bendOf(clip, Right), 1e-5f));
}

TEST_CASE("IK feet: a step further than the step height, and a foot with no ground, are left to the clip")
{
    Legs body = legs();
    const std::vector<Mat4> before = body.rig.model;
    render::ik::FeetResult result =
        render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.ankle[Left], 0.5f),
                              foot(body.ankle[Right], 0.0f), 0.0f, 0.3f, true, 1.0f);
    CHECK_FALSE(result.leftPlanted);
    CHECK(result.rightPlanted);
    CHECK(result.hipsOffset == 0.0f);
    CHECK(same(body.rig.model, before));

    // A drop too far counts for nothing either: the hips do not follow it.
    result = render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.ankle[Left], -0.5f),
                                   foot(body.ankle[Right], 0.0f), 0.0f, 0.3f, true, 1.0f);
    CHECK_FALSE(result.leftPlanted);
    CHECK(result.hipsOffset == 0.0f);
    CHECK(same(body.rig.model, before));

    render::ik::Foot missed = foot(body.ankle[Left], 0.1f);
    missed.hit = false;
    result = render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, missed, foot(body.ankle[Right], 0.0f),
                                   0.0f, 0.3f, true, 1.0f);
    CHECK_FALSE(result.leftPlanted);
    CHECK(result.rightPlanted);
    CHECK(same(body.rig.model, before));

    // The other foot is planted all the same.
    result = render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, missed, foot(body.ankle[Right], 0.1f),
                                   0.0f, 0.3f, false, 1.0f);
    CHECK(result.rightPlanted);
    CHECK(result.hipsOffset == 0.0f);
    CHECK(nearly(body.rig.position(body.ankle[Right]), Vec3{-0.1f, 0.2f, 0.0f}, 1e-4f));
    CHECK(same(body.rig.model[body.ankle[Left]], before[body.ankle[Left]]));
}

TEST_CASE("IK feet: a slope turns a planted foot to its normal, and only when asked")
{
    const Vec3 normal = core::normalize(Vec3{0.0f, 1.0f, 0.4f});
    const Legs clip = legs();

    Legs flat = legs();
    (void)render::ik::solveFeet(flat.rig.joints, flat.rig.model, flat.hips, foot(flat.ankle[Left], 0.1f, normal),
                                foot(flat.ankle[Right], 0.0f), 0.0f, 0.3f, false, 1.0f);
    CHECK(nearly(flat.rig.turn(flat.ankle[Left]), Mat3{}, 1e-6f));

    Legs body = legs();
    const render::ik::FeetResult result =
        render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.ankle[Left], 0.1f, normal),
                              foot(body.ankle[Right], 0.0f, normal), 0.0f, 0.3f, true, 1.0f);
    CHECK(result.leftPlanted);
    CHECK(result.rightPlanted);
    for (int side = 0; side < 2; ++side) {
        const Mat3 turn = body.rig.turn(body.ankle[side]);
        CHECK(nearly(turn * Vec3{0.0f, 1.0f, 0.0f}, normal, 1e-5f));
        // By the shortest way: what lies along the slope's own axis is not
        // rolled.
        CHECK(nearly(turn * Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 1e-5f));
        // About the ankle, and the toe goes round with it.
        const Vec3 ankle = body.rig.position(body.ankle[side]);
        CHECK(nearly(ankle.y, side == Left ? 0.2f : 0.1f, 1e-4f));
        CHECK(nearly(body.rig.position(body.toe[side]), ankle + turn * ToeFromAnkle, 1e-5f));
    }
    // The right foot's leg had nothing to solve: only the foot turned.
    CHECK(same(body.rig.model[body.knee[Right]], clip.rig.model[clip.knee[Right]]));
    checkLegLengths(body, clip);

    // A foot left to the clip is not turned.
    Legs distant = legs();
    (void)render::ik::solveFeet(distant.rig.joints, distant.rig.model, distant.hips,
                                foot(distant.ankle[Left], 0.9f, normal), foot(distant.ankle[Right], 0.0f), 0.0f, 0.3f,
                                true, 1.0f);
    CHECK(same(distant.rig.model, clip.rig.model));
}

TEST_CASE("IK feet: weight scales all of it, and nought changes nothing")
{
    const Vec3 normal = core::normalize(Vec3{0.0f, 1.0f, 0.4f});
    const Legs clip = legs();
    Legs body = legs();
    render::ik::FeetResult result =
        render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.ankle[Left], 0.2f, normal),
                              foot(body.ankle[Right], -0.1f), 0.0f, 0.3f, true, 0.5f);
    CHECK(nearly(result.hipsOffset, -0.05f, 1e-7f));
    CHECK(nearly(body.rig.position(body.hips), Vec3{0.0f, 0.95f, 0.0f}, 1e-6f));
    CHECK(nearly(body.rig.position(body.ankle[Left]), Vec3{0.1f, 0.2f, 0.0f}, 1e-4f));
    CHECK(nearly(body.rig.position(body.ankle[Right]), Vec3{-0.1f, 0.05f, 0.0f}, 1e-6f));
    // Half the way round to the slope.
    const f32 slope = std::acos(normal.y);
    CHECK(nearly(angleOf(body.rig.turn(body.ankle[Left])), slope * 0.5f, 1e-4f));
    checkLegLengths(body, clip);

    Legs still = legs();
    result = render::ik::solveFeet(still.rig.joints, still.rig.model, still.hips, foot(still.ankle[Left], 0.2f, normal),
                                   foot(still.ankle[Right], -0.1f), 0.0f, 0.3f, true, 0.0f);
    CHECK(same(still.rig.model, clip.rig.model));
    CHECK_FALSE(result.leftPlanted);
    CHECK_FALSE(result.rightPlanted);
    CHECK(result.hipsOffset == 0.0f);
}

TEST_CASE("IK feet: hips out of range, a foot that is no leg and a normal of no length are answered without harm")
{
    const Legs clip = legs();
    Legs body = legs();
    render::ik::FeetResult result =
        render::ik::solveFeet(body.rig.joints, body.rig.model, 99, foot(body.ankle[Left], 0.1f),
                              foot(body.ankle[Right], 0.1f), 0.0f, 0.3f, true, 1.0f);
    CHECK_FALSE(result.leftPlanted);
    CHECK(same(body.rig.model, clip.rig.model));

    // The spine is not the end of two bones below the hips, and a joint that
    // is not there is nobody's foot; the right foot is planted regardless.
    result = render::ik::solveFeet(body.rig.joints, body.rig.model, body.hips, foot(body.spine, 0.1f),
                                   foot(body.ankle[Right], 0.1f), 0.0f, 0.3f, true, 1.0f);
    CHECK_FALSE(result.leftPlanted);
    CHECK(result.rightPlanted);
    CHECK(result.hipsOffset == 0.0f);
    CHECK(nearly(body.rig.position(body.ankle[Right]), Vec3{-0.1f, 0.2f, 0.0f}, 1e-4f));

    Legs other = legs();
    result = render::ik::solveFeet(other.rig.joints, other.rig.model, other.hips, foot(77, 0.1f),
                                   foot(other.ankle[Right], 0.1f, Vec3{}), 0.0f, 0.3f, true, 1.0f);
    CHECK_FALSE(result.leftPlanted);
    CHECK(result.rightPlanted);
    CHECK(allFinite(other.rig.model));
    CHECK(nearly(other.rig.turn(other.ankle[Right]), Mat3{}, 1e-6f));
}
