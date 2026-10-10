// **A chain of joints that trails behind what carries it** (ADR 0194): the
// solver, and nothing of the world. `SpringBones` gathers a chain from a rig
// and a frame from the animation and hands both here; a test hands it numbers.
//
// What it is: each joint a point kept at its length from its parent, carried
// by its own motion, pulled back towards where the animation would have it,
// never bent from that by more than a limit, and kept out of a few capsules.
// The first joint of a chain is pinned where the animation puts it and turns
// to follow its child; the rest swing.
//
// **Picture, not simulation.** It is stepped at the rate frames are drawn,
// with whatever that machine's frame time was, and it is not expected to be
// the same on two machines -- nothing reads it but the renderer. What it does
// owe is to look the same at 30, 60 and 144 frames a second and never to fly
// apart, and both come from the same rule: fixed steps of `SpringStep`, no
// more of them a frame than cover `SpringFrameCover`, and a frame longer than
// those cover loses its motion rather than integrating it.
#pragma once

#include <span>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::render {

using core::f32;
using core::u32;

// The step the chain is advanced by, seconds: one hundred and twenty a second.
inline constexpr f32 SpringStep = 1.0f / 120.0f;
// The coarser one a lower quality takes: sixty a second, half the work. The
// solver is where a chain's cost is -- measured, three fifths of it -- so
// this is what halves it; stepping on every other frame did not, since the
// frame between still has to fit the chain to where the body went.
inline constexpr f32 SpringCoarseStep = 1.0f / 60.0f;
// What a frame's steps may cover at most, seconds: a frame at thirty a
// second. Four of the fine step, two of the coarse.
inline constexpr f32 SpringFrameCover = 1.0f / 30.0f;
// A frame at least this long is a hitch, not motion: the chain keeps its
// shape and starts again from rest.
inline constexpr f32 SpringHitchSeconds = 0.25f;

// What a `SpringBone` says, as the solver takes it.
struct SpringSettings
{
    // 0 to 1, as a share of the way back to the animated place in a sixtieth
    // of a second.
    f32 stiffness = 0.25f;
    // 0 to 1, as the share of its motion a joint loses in a sixtieth.
    f32 damping = 0.2f;
    // 0 to 1: how much of the carrier's own motion the chain is left behind by.
    f32 inertia = 1.0f;
    // Degrees from the animated direction.
    f32 limitAngle = 70.0f;
    // The chain's own thickness against a capsule.
    f32 radius = 0.05f;
};

// One joint. `parent` is its place in the same chain, and precedes it; the
// first joint's is -1.
struct SpringJoint
{
    core::i32 parent = -1;
    // The animated transform from its parent, this frame.
    core::Mat3 localRotation{};
    core::Vec3 localOffset{};
    // Where it is and was, in the world. The solver's own, kept between frames.
    core::DVec3 position{};
    core::DVec3 previous{};
    // Which way it faces in the world this frame: what the solver answers.
    core::Mat3 rotation{};
};

// A ball, or a capsule from `a` to `b`, in the world.
struct SpringCapsule
{
    core::DVec3 a{};
    core::DVec3 b{};
    f32 radius = 0.0f;
};

// What a chain keeps between frames besides its joints.
struct SpringState
{
    // Whether the joints have been placed at all.
    bool seeded = false;
    // Where the first joint was a frame ago, for what `inertia` takes off.
    core::DVec3 lastRoot{};
    // The part of a step the frames so far have not used.
    f32 carry = 0.0f;
};

// The chain rigid, as the animation alone would draw it from `root`, and at
// rest: where a chain starts, and where it is put back after a hitch or a
// jump.
void seedSpringChain(std::span<SpringJoint> chain, SpringState& state, const core::CFrameD& root, f32 scale) noexcept;

// A frame of `seconds`. `root` is the first joint's animated frame in the
// world; `acceleration` is gravity and wind together, metres a second squared;
// `scale` is how many world units one of the rig's is, for the lengths.
//
// A chain not yet seeded is seeded. A frame of `SpringHitchSeconds` or more,
// or a first joint that moved further in the frame than `jump` (the chain's
// own length is a sensible one; nought never snaps), puts the chain back as
// `seedSpringChain` does -- carried along, not flung.
//
// `step` is the fixed step, `SpringStep` unless a lower quality asks for the
// coarse one.
void stepSpringChain(std::span<SpringJoint> chain, SpringState& state, const core::CFrameD& root,
                     const SpringSettings& settings, std::span<const SpringCapsule> capsules, core::Vec3 acceleration,
                     f32 scale, f32 seconds, f32 jump, f32 step = SpringStep) noexcept;

} // namespace engine::render
