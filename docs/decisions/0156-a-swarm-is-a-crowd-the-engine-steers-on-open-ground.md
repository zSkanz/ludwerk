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

## Amendment, 2026-10-03: a horde shared by a match

The horde test game became a match: a host and friends who join by address.
The host simulates the horde and the players draw it from compact snapshots.
That needed two things of a swarm.

1. **Every player at once.** `Swarm:SetTargets({ vector })`: each agent walks
   at the nearest of the targets, the first of two as near. Its `NearDistance`
   and `FarDistance` are measured to that one. An agent near any player thinks
   every tick, and one far from all of them every fourth. The targets are
   called again as the players move. An empty table returns to `Target`
   alone, the single-target path as it was.
2. **Agents with no body.** `Swarm:AddAgentAt(position, settings)` adds a row
   the swarm steps with nothing to place, read back through `GetPositions` and
   `GetAgentPosition`. A body under `ServerStorage` would work too, since what
   is under it never replicates. But it would be a part written every tick
   for nobody to see. Unity's ECS crowds and Unreal's Mass entities are rows
   for the same reason: the simulation does not need an object to draw.

Tests: two agents each walk at their nearest of two targets; an agent ten
metres from one target and two hundred from the other thinks every tick; an
agent with no body walks as one with a body does. From a script, one swarm
chases two targets with body-less agents.

## Amendment, 2026-10-06: how high the crowd piles

A crowd round something that stands still climbs itself for as long as there
is one in the way: measured in the test this amendment adds, a hundred and
fifty agents round one point stand fourteen metres deep, and in the game it
was asked for a hero who stood still was under a tower that hid the fight.
A game could say whether an agent climbs (`Climbs`), not how high the pile
goes.

**`Swarm.PileHeight`**, in metres over the ground under an agent, 0 for no
limit (the default, and what a swarm did). Past it a top is nothing to climb
and nothing to stand on: an agent blocked by one whose top is past it is
pushed aside as by any neighbour, and one found standing on such a top is not
held up by it and comes down. An agent still hops as it crests the one it
climbed, as it always did -- four tenths of a metre over where it then
stands. The authority's step alone: a replica is told where agents are.

Test: the same crowd with no limit and with two metres -- at rest, the highest
feet are at fourteen metres and at two.

## Amendment, 2026-10-06, the second: what a step no longer does every tick

Measured on a horde of 525 agents stepping among a map's 376 obstacles
(player build, a desktop): 0.44 ms a step, and by part --

- a quarter of it **filing the obstacles into cells**, all of them, every
  tick, into a map made and thrown away;
- a quarter **finding neighbours**, nine cells an agent, each a lookup in a
  map with a node a cell, which the step also built again when it was done;
- a third **the ground's height**: an agent that has moved asks, a height is
  the tops of five columns of the terrain, and a top is a walk down a column
  of voxels -- for answers that were the same a tick ago.

1. **The obstacles' cells are kept** (`SwarmComponent::obstacleCells`) and
   made again only when the obstacles are other than the ones they were made
   from, in the order they were.
2. **The cells are a table of the swarm's own** (`SwarmCells`): open
   addressing over a power of two, emptied and filled each step with no
   allocation.
3. **The swarm keeps the column tops it has asked for** until a terrain's
   revision or place changes (`groundCells`, `asset::heightAtWith`). The
   height is the same arithmetic over the same tops.

**Nothing an agent does changes by a bit**: neighbours and obstacles are met
in the order they were, the ground is the number the terrain gives -- the
determinism replays do not move, and a test holds an agent's ground equal to
the terrain's own answer, over a mound and after a hole is dug ahead of it.
After: 0.16 ms a step.
