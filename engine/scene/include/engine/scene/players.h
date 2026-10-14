// Players, as the world holds them (ADR 0069).
//
// **A `Player` is somebody taking part, and it lives under `NetworkService`.**
// Solo has one, a host has one plus one per replica, a dedicated server has one
// per replica only, and a replica has its own. A game reads all of them through
// the same two calls -- `NetworkService:GetPlayers()` and
// `player:GetIntent(action)` -- whether or not anybody is networked, which is
// what makes a multiplayer game's code the same code as its solo one.
//
// These helpers are the one place players are made and unmade, so
// `PlayerAdded` and `PlayerRemoving` fire on every path that does either.
#pragma once

#include <vector>

#include "engine/core/id.h"
#include "engine/core/types.h"

namespace engine::scene {

class World;

// The world's `NetworkService`, found under the data model. Invalid when this
// world has none -- a stamp stage, a test world.
[[nodiscard]] core::InstanceId networkServiceOf(const World& world, core::InstanceId dataModel) noexcept;

// Creates a player under `networkService`, names it, and fires `PlayerAdded`.
core::InstanceId createPlayer(World& world, core::InstanceId networkService, core::u32 userId, bool local);

// Fires `PlayerRemoving`, then destroys the player.
void removePlayer(World& world, core::InstanceId networkService, core::InstanceId player);

// Destroys the players `removePlayer` took out of the list, now that the
// handlers of their `PlayerRemoving` have had them. Once a tick, at its start.
void finishLeavingPlayers(World& world);

// **A guest's number before the authority has given it one** (ADR 0205): a
// machine that seats a guest in a match it joined numbers it from here up
// until the authority answers, so a number below this is the authority's.
inline constexpr core::u32 ProvisionalUserIdBase = 0x80000000u;

// The player at this machine, or invalid.
[[nodiscard]] core::InstanceId localPlayerOf(const World& world) noexcept;

// **Every player at this machine**, in the order they joined: the first, and
// the guests of ADR 0195. Not one that is leaving.
void localPlayers(const World& world, std::vector<core::InstanceId>& out);

// The player with this number, or invalid.
[[nodiscard]] core::InstanceId playerByUserId(const World& world, core::u32 userId) noexcept;

// **Copies this machine's input into its own player's intents**, once a tick,
// right after the simulation-rate input dispatch -- so a script that reads its
// own player's intent reads this tick's input, and a replica sends this tick's.
//
// Every enabled action in a `Simulation` context, in pool order. A second
// action with a name already copied is skipped: an intent is looked up by name,
// and two answers to one name is a question the sender cannot mean.
void captureLocalIntents(World& world);

// **This machine's players, held at rest** (ADR 0204, `SequencePlayer.Controls
// = Suspend`): every intent of every local player reads as nothing pressed and
// nothing pushed -- what a script reads and what a replica sends. After
// `captureLocalIntents` and whatever code writes over it.
void restLocalIntents(World& world);
// And their characters, whatever a controller asked of them this tick by
// whatever it read: not walked and not made to jump. Before the simulation.
void restLocalCharacters(World& world);

} // namespace engine::scene
