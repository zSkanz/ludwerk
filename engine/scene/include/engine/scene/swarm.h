// **A crowd on open ground that the engine steers** (ADR 0156): the agents of
// every `Swarm`, as rows, walked at the swarm's target on the simulation tick.
#pragma once

#include <optional>
#include <vector>

#include "engine/core/dmath.h"
#include "engine/core/id.h"
#include "engine/core/types.h"
#include "engine/scene/components.h"

namespace engine::scene {

class World;
class PhysicsSync;

// What an agent is, as `AddAgent` was told.
struct SwarmAgentSettings
{
    f32 radius = 0.5f;
    f32 height = 1.0f;
    f32 speed = 4.0f;
    bool floats = false;
    f32 floatHeight = 1.0f;
    bool climbs = true;
};

// Adds an agent at `position` moving `body`; its number, one more than its
// slot, or 0 when the swarm is full.
[[nodiscard]] u32 addSwarmAgent(SwarmComponent& swarm, core::InstanceId body, core::DVec3 position,
                                const SwarmAgentSettings& settings);
// Forgets an agent. False when there is no such agent. `tick` is the world's,
// for the replication that tells the replicas (ADR 0162).
bool removeSwarmAgent(SwarmComponent& swarm, u32 agent, u64 tick = 0);
// `SetAgentTag`: false when there is no such agent.
bool setSwarmAgentTag(SwarmComponent& swarm, u32 agent, u16 tag);

// **A replica's row** (ADR 0162): the authority's agent `slot`, as it was
// told. `tick` is the authority's.
struct SwarmTold
{
    core::DVec3 position;
    // How far above its ground: the terrain under it where there is one, the
    // `floor` otherwise.
    f32 lift = 0.0f;
    f64 floor = 0.0;
    f32 yaw = 0.0f;
    f32 walk = 0.0f;
};
// The terrain's top at (x, z), or nothing where no terrain covers it: the
// ground an authority and its replicas agree on without a ray.
[[nodiscard]] std::optional<f64> swarmTerrainAt(const World& world, f64 x, f64 z);
// The same answer, from the column tops `swarm` has kept and the ones it
// asks for now: what a step asks, hundreds of times a tick.
[[nodiscard]] std::optional<f64> swarmTerrainAt(const World& world, SwarmComponent& swarm, f64 x, f64 z);
// An agent this replica now has. A row that held another is that one removed
// first.
void mirrorSwarmAgentAdded(SwarmComponent& swarm, u32 slot, u16 tag, f32 radius, f32 height, const SwarmTold& told,
                           f64 tick);
// One it no longer has, and why (`Enum.SwarmAgentRemoval`).
void mirrorSwarmAgentRemoved(SwarmComponent& swarm, u32 slot, u8 reason, u16 tag, core::DVec3 position);
// Where one is now, newer than what the row holds; false for a row that is
// not alive or holds something newer. `tag` is what it carries now.
bool mirrorSwarmAgentTold(SwarmComponent& swarm, u32 slot, const SwarmTold& told, f64 tick, f64 dt,
                          std::optional<u16> tag);
// Where the authority's agent is at `tick`, carried along its walk from the
// step that last moved it.
[[nodiscard]] core::DVec3 swarmAgentAt(const SwarmAgent& agent, f64 tick, f64 dt) noexcept;

// How long a replica carries an agent forward with nothing new, in ticks:
// past the longest an agent goes untold, so one walking straight is not
// stopped and corrected for nothing. At half a second -- what it was first --
// a walker on a straight line was told every forty ticks, each time a metre
// and three quarters behind.
inline constexpr f64 SwarmCarryTicks = 300.0;
// How many events a swarm keeps for a script layer that is not firing them.
inline constexpr usize MaxSwarmEvents = 65536;
// The live agent `agent` names, or null.
[[nodiscard]] SwarmAgent* swarmAgent(SwarmComponent& swarm, u32 agent) noexcept;
[[nodiscard]] const SwarmAgent* swarmAgent(const SwarmComponent& swarm, u32 agent) noexcept;
// Every live agent whose feet are within `radius` of `centre` -- along the
// ground alone when `flat` -- appended to `out` in slot order. Through the
// swarm's grid, built again first when an agent moved since the last step.
void querySwarmRadius(SwarmComponent& swarm, core::DVec3 centre, f64 radius, bool flat, std::vector<u32>& out);

// One simulation tick of every enabled swarm in `world`, in pool order and
// each one's agents in slot order (R10). `physics` answers the ground where no
// terrain covers it; null means the ground there is at zero.
void stepSwarms(World& world, const PhysicsSync* physics, f64 dt);

} // namespace engine::scene
