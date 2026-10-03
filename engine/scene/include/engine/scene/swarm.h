// **A crowd on open ground that the engine steers** (ADR 0156): the agents of
// every `Swarm`, as rows, walked at the swarm's target on the simulation tick.
#pragma once

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
// Forgets an agent. False when there is no such agent.
bool removeSwarmAgent(SwarmComponent& swarm, u32 agent);
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
