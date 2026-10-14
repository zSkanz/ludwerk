#pragma once

// **The wind** (ADR 0115): a pure function of the workspace's three settings,
// a position and the simulation clock -- never the wall clock -- written here
// for `Workspace:GetWindAt` and particles, and line for line in
// `shaders/include/engine/wind.hlsli` for what is drawn. Visual only: it
// pushes no body, and the world hash leaves its settings out.
//
// Gusts travel downwind at the wind's own speed, so a gust is seen crossing a
// field; turbulence turns the direction a little about the vertical, here and
// there.

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::scene {

struct WindSettings
{
    // Direction and speed, metres a second.
    core::Vec3 global{};
    // 0 to 1: how far the speed rises and falls in gusts.
    core::f32 gusts = 0.0f;
    // 0 to 1: how far the direction wanders.
    core::f32 turbulence = 0.0f;
};

// The wind at a point of the world at a time, metres a second. Zero wherever
// `global` is zero.
[[nodiscard]] core::Vec3 windAt(const WindSettings& wind, core::Vec3 position, core::f32 time) noexcept;

} // namespace engine::scene
