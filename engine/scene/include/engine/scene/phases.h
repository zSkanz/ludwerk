#pragma once

// **A player sees the world their progress has** (ADR 0231).
//
// `Instance.Phase` names the phase a thing belongs to; a thing with one is
// seen only by the players in that phase, and so is everything under it. A
// player is in a set of phases (`Player:AddPhase`). A world holds at most
// `MostPhases` names -- each is a bit of a mask, which is what the physics
// filter carries on a player's character and the parts they own.

#include <optional>

#include "engine/core/id.h"
#include "engine/core/name_atom.h"
#include "engine/core/types.h"

namespace engine::scene {

class World;
struct PlayerComponent;

inline constexpr core::u32 MostPhases = 64;

// The bit `phase` has in this world, given one if it had none; nothing past
// `MostPhases`.
[[nodiscard]] std::optional<core::u32> phaseBit(World& world, core::NameAtom phase);
// The bit it has, without giving one.
[[nodiscard]] std::optional<core::u32> phaseBitOf(const World& world, core::NameAtom phase) noexcept;

// Whether any instance of this world has a phase: nearly every world has
// none, and pays nothing for them.
[[nodiscard]] bool anyPhased(const World& world) noexcept;

// The bits of the phases `player` is in.
[[nodiscard]] core::u64 phaseMask(const World& world, const PlayerComponent& player) noexcept;

// Whether `player` sees `id`: its phase, and every phase above it, is one
// they are in.
[[nodiscard]] bool playerSees(const World& world, const PlayerComponent& player, core::InstanceId id) noexcept;

// The phase `id` collides by (ADR 0231 §4): its own or its nearest
// ancestor's, when `PhaseCollision` is on; invalid for none.
[[nodiscard]] core::NameAtom collidingPhase(const World& world, core::InstanceId id) noexcept;

} // namespace engine::scene
