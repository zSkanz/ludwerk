# 0140 — A coarse level of the terrain is the fine surface, gathered

- **Status:** accepted (2026-09-30)
- **Amends:** [ADR 0082](0082-terrain-is-a-grid-of-voxels.md) -- its mip
  pyramid, its coarse meshing, its skirts and its level selection
- **Asked for by:** the terrain audit of 2026-09-29 (TA1, TA2, TA3, TA6, TA7,
  TA11 and the mesh P2); ledger `docs/briefs/terrain-audit-2026-09-29-full.md`

## Context

ADR 0082 meshed a node at level `L` from a mip pyramid: each coarse voxel the
mean of the occupancy of the `2^L` cube under it and the material of its
fullest voxel, then the same surface net as level 0 over those means. The
audit photographed what that does at a distance, and T0 measured it:

- **A shape thinner than about half a coarse cell had no surface at all.**
  The mean of a thin slab is under one half at every coarse point: the owner's
  2 m ground was gone from 500 m on, and balls went blocky, then went.
- **Flat ground moved with the level.** A mean of a linear ramp, crossed
  linearly between two coarse samples, is biased: 8 cm at L2, 1.29 m at L4 and
  1.62 m at L5 for ground at 3.75 m. Every seam was a step, and a node jumped
  when its level changed.
- **A coarse cell took the material of its lowest full voxel**, and the paint
  over it was lost from L4 on.
- **The sky term was computed against level 0 and applied to coarse
  vertices**, so ground under a feature the coarse level no longer drew stayed
  dark.
- **A skirt hung from the fine side only, and down**, so where the coarse side
  was higher -- or, on flat ground, simply further along -- the seam was open.

Three representations were weighed, as the audit named them:

1. **A signed distance per voxel, min/max-aware.** A point sample of a true
   distance is exact for a plane, but a thin feature between two samples is
   still missed, and the fine field knows a distance only within its ramp --
   two voxels -- where a coarse ramp at L3 needs sixteen. A distance
   transform over the whole field at every level costs what it saves.
2. **Max-of-occupancy on the solid side.** Nothing vanishes, but every
   surface moves out by half a cell: flat ground a metre up at L2.
3. **The surface's own position**, gathered from level 0. Chosen.

## Decision

### A coarse cell is the level-0 surface inside it

**Vertex clustering, over the surface net level 0 already builds.** Each
level-0 cell the surface passes through has a vertex (ADR 0082). A cell at
level `L` holds the `2^L` cube of level-0 cells under it, and:

- **its vertex is the point nearest their tangent planes**, inside the cell:
  each level-0 vertex adds its plane to a quadric, and the vertex solves it,
  keeping the mean along any direction the planes hardly constrain -- an
  eigenvalue under 2 per cent of the largest. So flat ground is exactly where
  it is at every level -- under 2 cm at L0 to L5, as the tests hold it -- and
  an edge or a corner the cell holds stays where it is: a slab's rim does not
  shrink in by half a cell. At 10 per cent it did, where a rim cell had many
  vertices on its top and few on its side (the terrain gallery, far away);
- **its material is the one most of them carry**, and its paint theirs,
  averaged -- the surface's, never what is under it;
- it exists wherever any of them does, so **no surface vanishes while a coarse
  cell holds any of it**.

**Where a coarse quad is** follows from what clustering does to the level-0
quads: one around a level-0 edge collapses to a coarse quad only when the edge
lies on a coarse lattice line, and to nothing otherwise. So a coarse edge
carries **how many of the level-0 edges along it the surface crosses, each
way** -- ground below and air above, or the other way:

- **a count one way is a quad wound that way**: winding comes from the edge's
  sign, never from a normal. Level 0 is wound by its edges' signs too now (the
  mesh P2);
- **counts both ways are both quads**: a slab thinner than a coarse cell
  gathers into one sheet with a face each side. It thins to a sheet; it does
  not vanish.

A feature that crosses no coarse lattice line at all -- a ball of radius two
metres between the lines of an 8 m level -- collapses. Level selection keeps
what a level gets wrong under its pixel budget (below), so what goes is under
that.

**Normals** are the mean of the level-0 normals where they agree, a surface
seen from one side; where a cell gathered two sides of a slab, each side gets
its own vertex, lit by the faces it is wound into. Nothing falls back to
straight up: a level-0 vertex with no gradient -- a feature a voxel thin --
takes its faces' normal too.

**Stored as sums, per chunk and level, in a cache the field keeps**: the
positions, the count, the normals, the quadric, the material votes, the paint,
and each coarse edge's two counts, with each cell's vertex and error worked out
once. Keyed by what they were gathered from -- the chunk's digest and its
neighbours' borders -- because a surface cell can sit in a chunk nobody
stored, beside one somebody did, and a copy of the field (an undo) shares the
cache and simply misses where it differs.

**Gathered on every thread, before the nodes are meshed on every thread.** A
chunk's missing levels come from one read of its voxels; the ones asked for
are keyed on one thread (the key fills the chunks' lazy digests), gathered in
parallel, and cached on one thread again. A chunk that is one value all round
-- air, or rock under the ground, most of a view -- has no surface and is
known to have none from the chunks alone, without reading a voxel. Meshing
itself only reads, so any number of meshes are made at once.

### Seams are stitched, not skirted

**The cells a node shares with a coarser node are gathered at that node's
level.** A cell straddling the seam is drawn by both nodes; the finer one takes
the coarser one's cell there -- the same point from the same level-0 surface --
so the two meshes share every vertex on the seam, and the quads that
clustering collapses fall out as degenerate. It holds whichever side the coarse
node is on and however many levels coarser it is.

- **Sides and corners.** A node is meshed knowing the level drawn beside each
  of its four sides and four corners, where it is coarser. A corner's cell is
  shared by four nodes and takes the coarsest of them: stitched to its sides
  alone, a node left a slit at a corner whose diagonal neighbour was coarser
  than either side (the terrain gallery).
- **A node whose neighbours changed is rebuilt in the frame they did**, as one
  whose ground changed is.
- **The skirts are gone.** A skirt is a wall with normals of its own; it could
  show at a grazing angle, in the shadow map and in the sky term, and it closed
  a gap only where the gap ran the way it hung.
- **Transition cells were not chosen**: the Transvoxel tables solve the seam of
  a marching-cubes mesh, whose coarse vertices sit on edges. A gathered level's
  vertices are points of the gathered surface, and sharing them is exact where
  a table would approximate.

### The level is chosen by what it gets wrong, and changes without a pop

- **Projected error.** A node's error is the largest of its cells': how far
  the level-0 surface is from the cell's vertex (the root mean square of their
  plane distances), and half the cell where it holds more than one material or
  any paint, which a coarse cell blends across its width. A node shows its
  children where that error, projected at the viewport's height and field of
  view, passes a budget of pixels -- **4 at quality low, 3 at medium, 2 at
  high, 1.5 at ultra** -- measured from the node's own box, height included.
  Flat ground's error is nothing, and it stays coarse; a node not built yet
  counts its whole cell. A budget by the cell's size was measured first: it
  split every flat field to the finest level near the camera, and the owner's
  place built ten times the nodes.
- **Hysteresis**: a node splits under the budget and merges only past 1.25
  times it, so a camera on the line does not flip it every frame.
- **A geomorph**: every vertex carries the offset to its parent's -- the cell
  one level up that holds it -- and slides onto it between 85 per cent of the
  distance at which its node gives way to its parent and that distance, so the
  node that replaces it is the geometry already drawn. The band is narrow
  because a sliding node shows up to its parent's error -- over the budget by
  as much as the band is wide.
- **A seam's vertex slides the same in every node that draws it.** Nodes chosen
  by their own error give way at different distances, and a vertex on a seam is
  drawn by every node there: sliding each over its own node's range opened
  every seam while it slid. So a vertex carries the level it was gathered at
  and the sides it sits on, a draw carries its own range and the range and
  level of the node beside each side and corner, and a vertex slides over the
  smallest range of the nodes of its level that draw it -- which each of them
  works out alike. A stitched vertex slides as the coarser node slides it.
- **The vertex layout is 48 bytes and asserted**; the terrain frees three
  floats of it for the offset: the normal is packed octahedral into two, the
  vertex's own material (which no shader read) goes, and the triangle corner's
  index and the vertex's seams share the sky's float, as
  `sky + 2 * corner + 8 * tag`. The depth prepass draws terrain through its own
  pipeline with the same slide, or it wrote depth where the ground no longer
  was and the ground failed the depth test there.

### The sky term is the drawn geometry's

Computed per node from **its own level's surface** -- level 1's for level 0 --
never from the voxels: a coarse node's columns are its own vertices' highest
and lowest, so a feature the level does not draw shades nothing. The columns
keep every solid run they have, so air under an overhang is air; and the rays
go out on sixteen bearings, not eight, which is what drew lobes along the axes
and the diagonals.

**A column is a whole cell**, 2 m at level 1 and 8 m at level 3, so three
rules keep its coarseness out of the answer (the owner's place, checked by
ludwerk-08 at 500 and 1 000 m):

- **a run nothing closes ends at the column's own top**: under the bulge of a
  mound a column holds the underside, facing down, while the wall above it is
  too steep to count -- and the run went to the sky, a pillar every ray past
  it met, in a row of dark streaks down the flank;
- **a run round the point itself is not a roof or a floor**: a vertex on a
  ball fell inside the ball's own run, or not, by where in the cell it was;
- **a sideways ray does not meet the column it starts in**, for the same
  reason; that column's sky is the straight up and down.

## Consequences

- **A coarse node costs more to build**: it is meshed from the level-0 surface
  under it and round it, where it was meshed from a mip. On the owner's place,
  whose streamed cells land under the camera throughout the terrain flight, the
  flight's p95 went from 7.4 ms to 22 ms, its p99 from 11 ms to 33 ms and its
  worst frame from 25 ms to 60 ms (`docs/perf-baselines.md`); the median went
  from 0.85 ms to 2.7 ms, as a view drawn to its error budget draws more
  nodes. What was done
  here to bound it -- the gather in parallel, plain chunks skipped, one read
  for every level, each cell's vertex worked out once, the quads walked over
  the surface rather than the volume -- took the first measurement's p95 of
  109 ms and worst of 1.3 s to those. Rebuilding a changed coarse node a frame
  apart was tried and dropped: it drew a seam between a node rebuilt on the new
  ground and one still on the old, for a few per cent of p95. Meshing off the main thread is the audit's T5 (TA14).
- **Every determinism trace that meshes coarse terrain moves**, and is
  re-recorded once for this.
- **The mips are gone**: `TerrainChunk::mip`, `prepareMip` and
  `TerrainField::voxelAt` by level, replaced by the gathered surfaces.
- The collider stays level 0 (ADR 0082), wound now by its edges' signs.
- **The terrain gallery's check allows a silhouette to move by the budget**:
  its slack is the quality's pixel budget over 0.85, rounded up and never
  under 3 pixels, since a level is chosen to be that close to the full shape,
  a sliding node that much further, and a curved silhouette close up a pixel
  further than an error measured over a cell says. Sky enclosed by ground has
  no slack.
