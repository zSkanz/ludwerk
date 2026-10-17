#pragma once

// **What no player is near sleeps** (ADR 0234).
//
// An online world is mostly empty of players at any moment, and its cost
// should follow them: a `CharacterBody` no player's focus is within
// `[server] dormant_radius` of does not walk, and what it was asked to do
// waits for a player to come near. A part marked `Dormancy = Never` stays
// awake wherever it is.
//
// **Decided where the physics step begins**, from the players' foci as the
// tick has them -- each player's `ReplicationFocus`, or their character --
// so it is a function of the world and the tick (R10). Only a dedicated
// server sleeps anything: a host and a single-player game keep everything
// awake, and so does a world any of whose players has no focus -- a player
// with none is sent the whole world, and sees all of it.

#include "engine/core/math.h"

namespace engine::scene {

class World;

// Takes this tick's players' foci into the world's engine state.
void takePlayerFoci(World& world);

// The distance from `at` to the nearest player's focus, as last taken;
// infinity with no player that has one.
[[nodiscard]] core::f64 nearestPlayerDistance(const World& world, const core::DVec3& at) noexcept;

// Whether anything may sleep in this world now, and how far from a focus.
[[nodiscard]] bool dormancyOn(const World& world) noexcept;
[[nodiscard]] core::f64 dormantRadiusOf(const World& world) noexcept;

// Whether something that may sleep, at `at`, does.
[[nodiscard]] bool dormantAt(const World& world, const core::DVec3& at) noexcept;

} // namespace engine::scene
