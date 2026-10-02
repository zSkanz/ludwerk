// Water (ADR 0118): one wave definition, read by the simulation here and by
// the renderer from the same parameters.
//
// **The surface is a pure function of a water's waves and the time**: the
// sum of up to eight travelling waves, each `A (sin p - s/2 cos 2p)` with
// `p = k (d . xz) - w t + phase` and `w = sqrt(g k)` -- a sine sharpened at
// its crest by its steepness `s`, and still one closed form, so the height
// under a point is exact rather than searched for. Its maths is `dmath`'s
// (R10): a boat floats the same on every machine. The shader
// (`shaders/surface/water.surface.hlsl`) evaluates the same terms, term for
// term.
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::scene {

class World;

inline constexpr core::usize MaxWaterWaves = 8;
// The gravity a wave's speed is taken from: the sea's, whatever a game sets
// `Workspace.Gravity` to, so the picture and the simulation keep one speed.
inline constexpr core::f64 WaveGravity = 9.81;

// One wave, ready to evaluate.
struct WaveTerm
{
    core::f64 k = 0.0;
    core::f64 amplitude = 0.0;
    core::f64 dirX = 1.0;
    core::f64 dirZ = 0.0;
    core::f64 omega = 0.0;
    core::f64 phase = 0.0;
    core::f64 steepness = 0.0;
};

struct WaterSample
{
    // Metres above the still surface, and the surface's two slopes.
    core::f64 height = 0.0;
    core::f64 slopeX = 0.0;
    core::f64 slopeZ = 0.0;
};

// A water's surface: its still level and its waves, in their child order.
struct WaterSurface
{
    core::f64 level = 0.0;
    std::array<WaveTerm, MaxWaterWaves> terms{};
    core::usize count = 0;

    [[nodiscard]] WaterSample sample(core::f64 x, core::f64 z, core::f64 time) const noexcept;
    // **How the water itself is moving** `depth` metres under the surface
    // above a column: each wave's orbit, up and down and to and fro, fading
    // with depth as a deep-water wave does. At the surface its vertical part is
    // exactly how fast the height is changing -- which is what lets a hull
    // rise with a wave instead of being held back by the water that lifts it.
    [[nodiscard]] core::DVec3 motion(core::f64 x, core::f64 z, core::f64 depth, core::f64 time) const noexcept;
    // The world height of the surface above a column: `level` plus
    // `sample`'s height, for less.
    [[nodiscard]] core::f64 heightAt(core::f64 x, core::f64 z, core::f64 time) const noexcept;
};

[[nodiscard]] WaterSurface surfaceOf(const World& world, core::InstanceId water);

// `Enum.WaterShape`, as `WaterComponent::shape` holds it (ADR 0146). `Box` is
// `Pool`'s older name, and `Spline` the older river: level at the water's
// `SurfaceLevel`, where a `River` takes each point's own height.
namespace water_shape {
inline constexpr core::i32 Ocean = 0;
inline constexpr core::i32 Box = 1;
inline constexpr core::i32 Spline = 2;
inline constexpr core::i32 Lake = 3;
inline constexpr core::i32 River = 4;
inline constexpr core::i32 Pool = 5;
} // namespace water_shape

[[nodiscard]] constexpr bool waterIsRiver(core::i32 shape) noexcept
{
    return shape == water_shape::Spline || shape == water_shape::River;
}
[[nodiscard]] constexpr bool waterIsPool(core::i32 shape) noexcept
{
    return shape == water_shape::Box || shape == water_shape::Pool;
}

// **A river's course, or a lake's outline, as everything reads it** (ADR 0146
// section 2): the curve through the water's points -- a centripetal
// Catmull-Rom spline, which passes through every point and makes no loop, a
// corner at a point that says `Sharp` -- cut into samples a couple of metres
// apart. The simulation, a query and the picture all walk these, so a boat
// floats on the ribbon that is drawn.
//
// Across the ground the curve is the spline; a river's HEIGHT runs straight
// from one point's to the next's, so it never climbs between two points that
// descend. Its width and depth run the same way.
struct WaterCourseSample
{
    core::DVec3 position;
    core::f64 width = 0.0;
    core::f64 depth = 0.0;
};

struct WaterCourse
{
    // A river's, first to last; a lake's outline, the first not said again.
    std::vector<WaterCourseSample> samples;
    // Which sample each of the water's points is, in child order.
    std::vector<core::u32> points;
    // A lake: the last sample joins the first.
    bool closed = false;
    // What it covers across the ground, half the widest width included.
    core::f64 minX = 0.0;
    core::f64 maxX = 0.0;
    core::f64 minZ = 0.0;
    core::f64 maxZ = 0.0;
    // The highest its still surface is.
    core::f64 top = 0.0;
};

// Empty for a water that is not along its points.
[[nodiscard]] WaterCourse courseOf(const World& world, core::InstanceId water);

// **A water at a column**: whether it is there, how high its still surface is
// -- a river's, where it descends, is its curve's -- how deep, and how it
// flows: along a river's course, by one where it is level and faster by four
// times its slope where it drops.
struct WaterHere
{
    bool covered = false;
    core::f64 level = 0.0;
    core::f64 depth = 0.0;
    core::Vec3 flow{};
};

// `course` is the water's, when the caller holds it; made here when it does
// not.
[[nodiscard]] WaterHere waterHere(const World& world, core::InstanceId water, core::f64 x, core::f64 z,
                                  const WaterCourse* course = nullptr);

// How much faster a river runs where it drops: by this times its slope.
inline constexpr core::f64 RiverSlopeSpeed = 4.0;

// Whether a column is inside a water's extent -- everywhere for an ocean, a
// pool's rectangle, a river's width along its course, a lake's outline -- and,
// for a river, the way it flows there (`WaterHere::flow`).
[[nodiscard]] bool waterCovers(const World& world, core::InstanceId water, core::f64 x, core::f64 z,
                               core::Vec3* flow = nullptr);

// **The water pushes and drags what is in it** (ADR 0118 §3): every
// unanchored, buoyant part inside a water in the workspace is sampled at 27
// points over its shape, and each point under the surface is pushed up by the
// water it displaces and dragged by the water's viscosity against the water's
// own motion -- as impulses at the point, so a hull pitches and rolls. Queued
// on the part's pending impulses, which the physics mirror applies this tick.
void applyWaterForces(World& world, core::InstanceId workspace, core::f64 dt);

} // namespace engine::scene
