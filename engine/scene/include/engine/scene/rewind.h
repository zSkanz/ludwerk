#pragma once

// **A shot is checked against the world the shooter saw** (ADR 0235).
//
// An authority keeps, tick by tick, where every player's character and every
// `LagCompensated` part was. A player's machine says which tick it is
// drawing everyone else at; a rewound query places what it drew there.

#include <optional>

#include "engine/core/types.h"
#include "engine/scene/world.h"

namespace engine::scene {

// This tick's frame, on an authority (a host or a dedicated server):
// nothing anywhere else. Called once a tick, after everything in it ran --
// the state a snapshot of this tick carries.
void recordRewind(World& world);

// **The tick `player`'s machine was drawing everyone else at**, as of the
// tick being run, never further back than the cap. Nothing where their
// machine has not said -- the authority's own player.
[[nodiscard]] std::optional<u64> rewindTickFor(const World& world, const PlayerComponent& player) noexcept;

// The frame recorded at `tick`, or the oldest kept when it is older than
// that; nothing when none is kept, or `tick` is newer than the newest.
[[nodiscard]] const EngineState::RewindFrame* rewindFrameAt(const World& world, u64 tick) noexcept;

} // namespace engine::scene
