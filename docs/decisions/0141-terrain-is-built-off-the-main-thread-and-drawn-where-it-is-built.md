# 0141 — Terrain is built off the main thread, and drawn where it is built

- **Status:** accepted (2026-09-30)
- **Amends:** [ADR 0140](0140-a-coarse-level-of-the-terrain-is-the-fine-surface-gathered.md)
  -- when a node is rebuilt, and what is drawn meanwhile
- **Asked for by:** the terrain audit of 2026-09-29 (TA14, T5); ludwerk-08's
  check of T2 on the owner's place; ledger
  `docs/briefs/terrain-audit-2026-09-29-full.md`

## Context

ADR 0140 made a coarse node the level-0 surface gathered under it, which is
right and costs more to build than the mip it replaced. The loader built on the
main thread: a node loading within a budget, and every node whose ground or
whose neighbours' levels changed in the frame that changed them, because a node
drawn beside levels it was not stitched to opens a crack, and neighbours drawn
from two versions of one edit flicker. On the owner's place, whose streamed
cells land under the camera throughout the terrain flight, that was a p95 of
22 ms and a worst frame of 60 ms, against 7.4 and 25 before ADR 0140.

Three shapes were tried before this one:

1. **Everything off the thread, drawn only when the whole selection is built.**
   Correct, and it drew nothing for as long as the camera moved: a selection
   that changes every frame is never all built.
2. **Loads and changed ground off the thread, seams in the frame.** A frame
   that changed a level still meshed its neighbours: 3 to 18 ms of meshing, and
   a p99 of 33 ms.
3. **Stitching on the GPU.** A stitched vertex is the coarser cell's; with
   every node's level at most one from its neighbours it is the vertex slid all
   the way to its parent, and a draw could move it. Weighed and left: it needs
   the levels held one apart, a second offset per boundary vertex and a buffer
   per node, for what the fourth shape does with meshes already built.

## Decision

**Every mesh the loader wants is built off the main thread**, by a few workers
-- a quarter of them, at least one and at most four, so the frame's own jobs
never queue behind the ground -- and put up by a later `sync`, a few a frame.

**What is drawn changes only where it is built:**

- **A node keeps two meshes**, each built for some ground and some levels
  beside it; a finished build replaces the one no drawn set shows.
- **A node is drawn only with a mesh built for the seams the drawn set gives
  it.** Where a wanted node has none, the ground under it is drawn as the frame
  before drew it -- and so, in turn, are the nodes beside it whose levels its
  seams depend on -- until every node drawn is built for what is drawn beside
  it. The frame before's set was, so this ends. What is wanted is built
  meanwhile: a change of level is drawn a frame or two late, and never with a
  seam open.
- **What is drawn is judged against the ground as last put up**, not as it is
  now: a batch holds every drawn node its snapshot changed and goes up as one,
  so all that is drawn is of one revision. Ground a streamed cell brings in, or
  an edit makes, appears everywhere in the frame it does -- never on one side
  of a seam, never half an edit.
- **A node with nothing to draw is not beside anything**, as it never was: it
  is left out before any seam is worked out.
- **A batch reads a snapshot of the field.** Its chunks are shared with the
  live one, which clones a chunk before writing one a snapshot holds, so what a
  worker reads never changes under it. A chunk's lazy digests are atomic, and
  the gathered-surface cache is locked and hands out what it holds; meshing
  itself only reads. A worker also gathers the surfaces a node's seams would
  read at every coarser level, so a rebuild for new neighbours only meshes.
- **A test or a picture builds in the frame**: `TerrainLoader::setAsync(false)`,
  which a run photographing on a schedule (`--screenshot-every`) uses, so its
  pictures are of what their frame built.

**`--pace=HZ`** makes a headless run wait out each frame's share of a second,
left out of `--frame-stats`. Without it a headless flight is over in a second,
and ground built beside the frame is measured against a camera moving thirty
times too fast.

## Consequences

- The owner's place, flown at 60 Hz (`--pace=60`): a p95 of 5.0 to 6.0 ms and a
  p99 of 7.9 to 9.3 ms, against 22 and 33 built in the frame
  (`docs/perf-baselines.md`). The worst frame is 18 to 25 ms; scheduling the
  workers is sometimes milliseconds on its own, and that is T5's.
- A change of level, an edit and a streamed cell are drawn a frame or two
  after they happen, whole.
- How much ground a flight draws depends on the streamer, which runs on the
  wall clock: a paced and an unpaced run of one flight draw different ground,
  and pictures of them are not compared pixel for pixel.
- The collider is still built on the main thread (TA14's other half, T5).
