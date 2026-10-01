# 0144 — Ground is drawn from the whole terrain; streaming governs only what is resident

- Status: accepted (terrain audit T5, D398)
- Date: 2026-09-30
- Decided by: the agent, reviewed by ludwerk-08 for the owner, after T5's check
  of the owner's place from 1 km: *"The horizon must never show cell steps or
  a missing region, at any camera position."*
- Amends: [0053](0053-the-grid-decides-when-and-the-model-decides-what.md) (a
  streamed terrain's cells), [0140](0140-a-coarse-level-of-the-terrain-is-the-fine-surface-gathered.md)
  (where a coarse node's surface comes from).

## Context

A streamed terrain keeps in memory only the cells within its load radius --
1 024 m by default, measured across the ground since D397. The loader drew
only what was in memory, so a terrain wider than the radius ended where its
loaded cells did: in the corners of 64 m cells, a sawtooth, from any camera
that could see past the radius. The owner's place is 1 024 m across; from his
camera a kilometre off, its far side was a staircase.

What engines do is keep a coarse picture of the far ground resident. Two ways
were weighed:

- **A separate far field**, the terrain coarsened by averaging occupancy and
  voting materials, drawn outside a circle. Rejected: the averaging is TA1 --
  an 8x mean of a 2 m slab is a quarter full, and it vanishes -- and the vote
  is TA7, the buried material instead of the surface's. And two different
  surfaces meeting at a circle draw a ring there, whatever dither hides it.
- **The same surface, everywhere.** The coarse levels are already built from
  the fine surface gathered (ADR 0140): what a coarse node draws is a function
  of the cells' voxels. Read those voxels from disk for a node whose ground is
  not resident, and the far ground is the same function of the same data as the
  near: nothing to stitch to but what ADR 0140 stitches already.

## Decision

1. **The loader draws the whole terrain.** A streamed terrain carries the cells
   it is made of, as a source any thread can read (`asset::TerrainCellSource`,
   set by the field streamer when it adopts the terrain's index). The loader's
   selection covers the extent of the cells, not of what is resident, and a
   node is occupied when a resident chunk or a cell is under it.
2. **A node whose ground is not resident is built from its cells.** Its build
   job reads the cells under the node from disk, gathers their surfaces at the
   coarse levels into the field's content-addressed cache, and meshes the node
   with the same function as any other -- the same surfaces, the same stitching
   with its neighbours, the same geomorph. Only coarse nodes are: a level-0
   node is never drawn over ground that is not resident; its parent is.
3. **What a node is built from does not depend on what is resident.** A
   chunk's digest, read once from its cell, is kept: a node's content is the
   same before its cell comes in, while it is in, and after it is evicted
   untouched, so ground streaming in under a node rebuilds nothing (D383's
   churn). An edit changes the chunk's digest, as it always did.
4. **Memory is bounded, whatever a node spans.** A build reads its cells a row
   at a time, keeping three rows of decoded cells: each chunk's surfaces are
   gathered while its neighbours are decoded, and the chunk is then kept only
   as a summary -- its digests, whether it is one value, and which of its faces
   are solid ground, which is all a coarse mesh reads of a chunk once its
   surfaces are gathered. The peak is three rows of cells across the widest
   node, stated in `docs/perf-baselines.md` with the measured peak.
5. **Far ground never waits ahead of near ground.** Builds from disk run in a
   batch of their own -- one lane, two on a machine of more than eight threads
   -- beside the resident builds; a node near the camera is never queued
   behind one far away, nor is a streaming load.
6. **A chunk's gathered surface is kept under a few contents at once** (four).
   A near build and a far one can read one chunk with different ground round
   it -- a neighbour resident to one, summarised to the other -- and with one
   entry a key each took the other's away between gathering and meshing: the
   far node meshed a surface from a summary, which has no voxels, and drew a
   hole the size of a chunk. A far build also gathers again whatever is still
   lacking before it meshes.
7. **Render only.** Nothing but drawing reads ground that is not resident:
   physics, raycasts, edits and the navmesh see what is loaded, as before.

## Consequences

- From any camera, the ground goes to the terrain's real edge at the level of
  detail its distance asks for, and the load radius leaves no mark on it.
- **A cold first view reads the whole world once**, in the background. The
  owner's place is 256 cells, 6 MB. On the large fixture -- 3 km of ground,
  some 2 200 cells, seen from 2 km with 300 m resident -- the far ground's
  outline is drawn within 0.3 s and its last detail within 20 s, no frame
  over 33 ms, at a peak of 315 to 323 MiB against 339 MiB for the same ground
  held whole (`docs/perf-baselines.md`). The time is the meshing, the same a
  resident terrain does, not the reading: a second lane and reading only the
  cells round what a node lacks each changed it by under a second. Persisting
  the gathered coarse surfaces between sessions -- under `.engine/` in the
  editor, in the pack in a build -- is the follow-up when a world makes that
  cost matter.
- **Tested** two ways. `terrain_loader_tests.cpp` builds every node a
  terrain's cells reach -- a floating 2 m slab, ground filled from the floor,
  a ball across the cells' borders -- from cells, with half, none and three by
  three of them resident, and holds each against the node the whole ground
  gives: vertex for vertex and content for content. `terrain_far` photographs
  ground three kilometres across, held whole and then streamed at 300 m, from
  one and two kilometres: the pictures are the same to the pixel, where before
  the streamed one had no ground at all from two.
- A world edited in the editor and not yet saved draws its far ground from the
  cells as saved; the edited cells are resident, and drawn as they are.
