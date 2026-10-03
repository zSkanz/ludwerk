# 0156 — A swarm is a crowd the engine steers on open ground

- Status: accepted
- Date: 2026-10-03
- Decided by: the agent, on the measured finding H6 of the horde test game
  (ludwerk-08, 2026-10-03), under the standing rule to decide as professional
  engines do and record it
- Amends: 0098 (crowds are agents the engine moves). Everything it decided
  stands; this adds a second kind of crowd beside it.

## Context

A horde game -- hundreds of enemies walking at the player across open ground,
pushing one another apart, piling into towers where the player stands above
them -- was written in Luau because nothing in the engine does it. Measured on
the horde test game: 550 enemies cost 4.5 to 5 ms of every frame in the
script on a desktop, growing as the crowd packs together, and a phone is three
times slower. What the script does each tick, for each enemy:

- walk at the target;
- push apart from the neighbours its own size, found through a grid of 1.5 m
  cells rebuilt every tick, at most fourteen of them;
- climb the one in front when it is blocked, and stand on whoever is under it,
  with gravity;
- follow the height of the ground;
- go round static obstacles -- trees, rocks;
- think less often far from the target, with the time it skipped;
- face the target.

ADR 0098's `NavigationAgent` is the other kind of crowd: agents on a navigation
mesh, avoiding each other as DetourCrowd does. It needs a mesh built over the
ground, an instance per agent, and has nothing for stacking. It is the crowd
for a town; this is the crowd for a field.

## How mature engines do it

- **Unreal**: Mass -- entities as rows of fragments in archetype chunks,
  processors stepping them in bulk, avoidance as a processor; the actor drawn
  for one is a representation chosen by distance.
- **Unity**: Entities -- components in chunks, systems over them in jobs; boids
  and crowds are user systems over the same machinery.
- **Godot**: no crowd beyond the navigation agent's avoidance; hordes are
  written over `MultiMeshInstance3D` and a script, or in a native extension.

The common shape: the agents are rows of data, not objects; one system steps
them all in order; scripts touch them in bulk and through queries, never one
call per agent per frame.

## Decision

1. **A `Swarm` is an instance that owns its agents as rows.** Under
   `Workspace` (or anything in it). An agent is not an instance: it is a slot
   with a position, a velocity, a radius, a height, a speed and two flags, and
   it moves one body -- a `BasePart` the game gives it, which the swarm places
   each tick facing the target.

2. **The script talks to it in bulk.**
   - `AddAgent(body, settings) -> number` and `RemoveAgent(agent)`; the number
     is the agent's for as long as it lives and is reused after.
   - `SetAgentSpeed(agent, speed)`, `SetAgentPosition(agent, position)` and
     `Push(agent, velocity)` -- a knock-back that dies away.
   - `GetAgentPosition(agent)`, `GetAgents()` and `GetPositions()` -- every
     live agent and where it is, in one call each.
   - `QueryRadius(centre, radius) -> { number }` -- what a weapon asks.
   - `AddObstacle(position, radius)` and `ClearObstacles()` -- the static
     things to go round, as circles.
   - Properties: `Target`, `CellSize`, `MaxNeighbours`, `Gravity`,
     `ClimbSpeed`, `NearDistance`, `FarDistance`, `Enabled`.

3. **It steps on the simulation tick, natively, in slot order** (R10), after
   `PreSimulation`'s scripts and beside ADR 0098's crowd, so the orders a script
   gives in the tick are the ones it walks with. The rules are the horde
   script's, unchanged: the grid rebuilt each tick and its neighbours visited in
   a fixed order; pushing apart side by side; standing on one whose top is at
   its feet; climbing one in its way when blocked; gravity; the ground's
   height; obstacles; every second tick past `NearDistance` and every fourth
   past `FarDistance`, staggered by slot, with the time skipped.

4. **The ground is the world's terrain**, sampled from its field where a
   `Terrain` covers the point, and a ray down where none does -- each cached
   per agent until it moves, because a ray a tick for five hundred agents is a
   millisecond.

5. **Reaching the target is the game's to decide.** `QueryRadius` round the
   target says who is there; what happens then -- a strike, a fuse -- is the
   game's, as is the animation each body plays.

## Consequences

- The horde's per-agent Luau goes: the game spawns, removes, pushes and asks,
  and the engine walks.
- An agent's body is moved by the swarm. A body that is also simulated, or
  that something else moves, is the game's choice and loses.
- Two kinds of crowd. A game picks by the ground: a mesh with routes, or a
  field.
- Agents are not replicated as agents: a networked game moves their bodies,
  which replicate as parts do, or runs the swarm where it is seen.
