# 0182 — A run of instances is drawn in the pieces a pass needs

- Status: accepted
- Date: 2026-10-05
- Decided by: the agent, in the performance programme the owner put first,
  under the standing rule to decide as professional engines do and record it
- Builds on: ADR 0043 (per-instance vertex stepping, and the runs it made),
  D537 (what reaches a cascade), the lot that made a skinned horde one run (H2)

## Context

An instanced run is one mesh drawn many times by one call: a forest's trees,
a horde's goblins. A call cannot be issued in part, so a run was drawn whole
or not at all, in every pass:

- **the camera's passes** drew all of a run when any member of it was in the
  frustum -- every tree of a map for one tree on the screen, twice over, in
  the depth prepass and the forward pass;
- **each shadow cascade** drew all of a run when the sphere round all of it
  reached the cascade -- and the sphere round a horde spread about the player
  reaches every cascade there is.

Counted in the benchmark's scene (a horde game, 1560 by 720, fifteen hundred
frames): of the triangles the instanced runs sent down, the camera's passes
needed four in ten, and the shadow maps one in five at Low, half at Medium
and two thirds at High. Every one of them is a vertex shaded, and a skinned
one reads four joints to be; on a phone's tile-based GPU every one is binned.
With the ground's fragment cured (ADR 0179) the horde's vertices were three
quarters of a phone's shadow pass.

## How mature engines do it

An instanced batch is culled by instance, not as a body: Unreal's instanced
and hierarchical instanced static meshes cull per instance (and per cluster)
for each view, the shadow views among them; Unity's batch renderer hands the
engine a visibility list per instance and per shadow split. What reaches the
GPU for a view is the instances that view needs.

## Decision

1. **A run's members are staged in an order a pass can take a piece of**
   (`orderInstanceRun`): those in the camera's frustum first, in the order
   the frame gave them; then those outside it, nearest the camera first.

2. **The camera's passes draw the first piece**: the members in the frustum.
   In the order they were always drawn in, so the passes are the pixels they
   were.

3. **A shadow map draws the members that reach it** (`castersReaching`, by
   the test a single caster is held to, `casterReaches`): one range of those
   the camera sees and one of those it does not, each from the first member
   that reaches the map's sphere to the last. At most two calls where there
   was one. What lies between two members that reach is drawn with them: a
   piece is a range, and the frame's order -- by depth, within a material --
   is what keeps a range tight.

4. The sphere round the whole run is still tested first, and is grown in the
   order it always was: a sphere grown in another order is another sphere.

Nothing of it is the simulation's, and nothing is on the wire.

## What it does not do

- It does not sort the members the camera sees by distance. That would
  tighten a cascade's range across a run of several materials, and would
  change the order two members that lie in one plane are drawn in.
- It does not cull by occlusion, or a member behind a hill.
- It does not shade a skinned vertex once a frame for every pass that wants
  it. A member that the camera sees and two cascades reach is still posed
  three and four times; a skinning pass of its own is the next step for a
  horde, and a larger one.
- Foliage has its own culling, on the GPU (ADR 0116), and is not touched.

## Consequences

- In the benchmark's scene the triangles of instanced runs fall by 58% in the
  camera's passes at Low and 61% at Medium and High, and in the shadow maps
  by 79% at Low (two cascades), 47% at Medium (three) and 31% at High (four).
  Together, at Low: from about 1.1 million a frame to under 0.4.
- Every picture the gates hold is the picture it was. One recorded command
  stream changed, as it should: a run of 45 drawn as the 39 that reach each
  cascade (`look-everything-3frames.jsonl`).
- A frame sorts the members of its runs that the camera does not see.
