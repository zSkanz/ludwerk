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

// Whether a column is inside a water's extent -- everywhere for an ocean, a
// box's rectangle, a river's width along its points -- and, for a river, the
// way it flows there (unit length, or zero where it does not).
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
