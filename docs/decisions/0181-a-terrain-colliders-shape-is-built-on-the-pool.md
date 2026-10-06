# 0181 — A terrain collider's shape is built on the pool

- Status: accepted
- Date: 2026-10-05
- Decided by: the agent, in the performance programme the owner put first,
  under the standing rule to decide as professional engines do and record it
- Builds on: ADR 0143 (a chunk's collider and its band), ADR 0117 (what the
  ground is, triangle by triangle), the terrain audit's TA14 (a tick's
  colliders meshed side by side)

## Context

With the ground's fragment cured (ADR 0179) a phone's frame no longer waits on
its GPU, and the slowest frames of a run are the CPU's. On the phone, six of
the nine worst were one scope: `physics.apply`, at 17 to 27 ms -- more than a
whole frame -- each time the player walked onto ground that had no collider
yet.

A terrain's colliders are made a few a tick (four, and every one in a mover's
way): the count is part of the operation sequence, so it cannot depend on how
fast the machine is (R10). Their meshes were already made side by side on the
pool. What was not: the collision shape of each -- the tree Jolt builds over
the triangles -- and the pass that says what each triangle is made of. Those
ran on the main thread, one chunk after another.

## How mature engines do it

Cooking collision off the game thread is the rule: Unreal cooks a landscape
component's collision on a task and adds the body when it is done; Unity bakes
a mesh collider on a job (`Physics.BakeMesh`) and assigns it on the main
thread. What stays on the main thread is adding the body to the world.

## Decision

1. **A shape can be built apart from any body**: `IPhysics::prepareShape`
   takes a shape's description and hands back the backend's own shape, opaque
   (R17). A function of the description and nothing else, safe on any thread
   and several at once; no world is touched.

2. **A body can be made from one**: `BodyDesc::prepared`. The description
   still says what the shape is; a body handed nothing prepared is made from
   its description as before.

3. **A tick's terrain colliders are prepared where they are meshed**: the
   mesh, what each triangle is drawn as, and the shape, all in the one job a
   chunk already had. The bodies are then made on the main thread in the order
   they always were, so the ids the backend hands out, and so the simulation,
   are what they were (R10).

## What it does not do

- It does not spread a collider over several ticks. The work of one chunk is
  still inside the tick that asked for it: about four milliseconds on the
  development machine for the slowest (its mesh three of them), and a tick
  with four waits for the slowest of the four rather than the sum. A collider
  begun in one tick and added in a later one would be hidden state across a
  rollback's re-simulation, and is not attempted here.
- It does not make the mesher faster, which is now most of what a collider
  costs.

## Consequences

- On the development machine, the same run before and after: the worst
  `physics.apply` 14.4 ms, then 5.7; the worst frames it made 21 ms, then 12.
- `physics.terrain.shapes` and `physics.terrain.bodies` are scopes of
  `--frame-stats`, so a report says which half a slow tick was.
