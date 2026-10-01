# 0150 — The far ground is kept on disk as it was gathered, and a node is named by its cells

- Status: accepted (built; `docs/briefs/terrain-editing-perf.md`, P5c)
- Date: 2026-10-01
- Decided by: ludwerk-08 for the owner, on the flight ADR 0149's bench could
  finally make: *"The owner's goal is a GTA-sized map he can fly and play,
  not just one that can be made."* Its six conditions are the sections below.
- Amends: [0140](0140-a-coarse-level-of-the-terrain-is-the-fine-surface-gathered.md)
  (where a gathered surface is kept),
  [0144](0144-ground-is-drawn-from-the-whole-terrain-and-streaming-governs-only-what-is-resident.md)
  (what a far node is built from, and what it is named by; its follow-up,
  "persisting the gathered coarse surfaces", is this),
  [0149](0149-changed-ground-is-kept-on-disk-for-the-session-and-a-world-larger-than-memory-is-imported-a-tile-at-a-time.md)
  (an import makes them too; where a run's cache is, and how much changed
  ground is held before any of it is written out -- section 7).

## Context

ADR 0144 draws the whole terrain: a coarse node whose ground is not resident
reads the cells under it from disk, gathers their surface (ADR 0140) and is
meshed as any node is. It was measured on 2 200 cells. ADR 0149 made a world
of 65 536 -- sixteen kilometres at a metre voxel -- and flying over it showed
what did not scale, on the build before as on that one:

- **a far node read every level-0 cell under it**: 324 for a top-level node,
  a third of a second and more, again whenever the node was needed again;
- **every drawn node's content was worked out from every chunk under it,
  every frame the ground changed** -- and ground streaming in changes it most
  frames. 260 000 such walks in thirty seconds of flight: 11.6 ms a frame;
- **the selection walked every unbuilt subtree to its leaves**, twice a frame
  -- forty thousand nodes asked after, while the far ground was behind;
- **memory rose with the distance flown**: 2.6 GiB after one crossing.

## Decision

### 1. What is kept is the gathered surface

1. **Per chunk and per level, exactly as ADR 0140 gathers it**: each coarse
   cell's vertex where the fine surface's tangent planes put it, how far that
   surface is from it, its normal, its materials' votes and its paint, and
   each coarse edge's crossings each way. Never voxels averaged to a coarser
   voxel: that is TA1 and TA7, and ADR 0144 turned it down for that. A slab
   two metres thick is in the file as the sheet it gathers to, with a face
   each side; the material on top is the one the surface carries.
2. **A file a node** (`asset::PyramidNode`, `.lnode`): the chunks in the
   node's footprint, each with what a coarse mesh reads of a chunk without its
   voxels -- one value or more, which faces are solid ground, its digest --
   and its surface at the node's level. A chunk gathered and found to have
   no surface is kept as that. The sums a cell's vertex is worked out from
   are not kept: the vertex is.
3. **Levels 3, 4 and 5** -- cells of 8, 16 and 32 voxels. Levels 1 and 2 are
   four and sixteen times the data, and a node of either reaches so few cells
   that it reads them, as before.
4. **The tests of ADR 0140 hold on the files** (`terrain_pyramid_tests`): with
   nothing resident, the gallery's slabs and balls have triangles at every
   level kept, flat ground is within 2 cm, a coarse cell takes the material on
   top and paint survives -- and a node built from the files is, vertex for
   vertex, the node the whole ground gives.

### 2. Incremental

1. **A block is a top-level node's footprint** -- 32 chunk columns a side, a
   kilometre at a metre voxel -- and its 21 files are brought up to date
   together (`TerrainPyramid::ensure`). The top-level file says what every
   cell under the block, and in the ring of columns round it, held when the
   block was gathered.
2. **A cell is known by what it holds** (`terrainCellSignature`): its chunks'
   keys and digests, one number. Whoever writes a cell -- a save, the session
   cache, the partition -- writes it into the cell's row of the index
   (`"sig"`).
3. **A cell that holds something else now has its columns, and the columns
   beside them, gathered again** from that cell and the eight round it, and
   the files those columns are in written again: one a level, when the cell is
   inside a node of each. Nothing else of the block is read; nothing of any
   other block. `terrain_pyramid_tests` holds it to nine cells read and three
   files written for a crater.
4. **Ground changed and not written out is not in the files**: a resident
   cell that holds something else than its file -- a brush's edit, a script's
   -- is meshed from its voxels as it is, with the columns beside it, gathered
   in memory. The files stay what the disk is.

### 3. A node is named by its cells

1. **A node's content is its cells' signatures** -- worked out from the field
   for a cell that is resident, as the source knows it for one that is not,
   the same number while the cell is untouched. A walk of the cells in its
   span, not of every chunk under it.
2. **And it is checked only when it could have changed**: the terrain counts
   which of its revisions were ground coming in or going out as it is on disk
   (`TerrainComponent::streamedRevisions`), and the cell source counts its own
   changes. While neither the edits nor the cells have moved, no node is
   asked.
3. **The selection does not walk what is not built**: a node keeps how many
   nodes under it are built, and an unbuilt node with none is not descended.
   **Far ground is built from the top down**: a node over ground that is not
   resident, not built yet, is asked for and not split -- unbuilt, its error
   was its whole cell, and split by that the selection asked for every fine
   node under it across the view before the coarse one that would have said
   the ground there is flat.

### 4. Bounded

- The decoded files kept: 384 (`TerrainPyramid::NodesKept`), the least lately
  asked for going first.
- The cells' summaries kept, for the two finest levels: 1 024 cells
  (`TerrainCellSource::SummariesKept`).
- A cell that leaves the field takes what was gathered of its chunks with it
  (`TerrainField::dropSurfaces`): the cache was shared and never emptied.
- **What is left that grows with the world is its index**: a row, a path and
  an entry a cell, some 2 KiB -- 130 MiB at 65 536 cells -- and a start-up
  peak from reading it. Stated in `docs/perf-baselines.md`; not with the
  camera.
- Three leaks of the streaming under it, each of which rose with distance,
  were found measuring and fixed: D405, D406 and D408.

### 5. Old projects

1. **A terrain whose rows say nothing of what a cell holds** -- an index from
   before -- is read a cell at a time as its blocks are first brought up to
   date, and the block's own file remembers each cell with its file's size and
   time: while those stand, the next run learns what the cell holds from
   there.
2. **In the background, once, with progress**: the streamer brings a block up
   to date at a time off the main thread, nearest the camera first
   (`FieldStreamer::farGroundProgress`; the editor's terrain panel says how
   far it is). The world is never held for it: until a block's files are
   made, the far ground over it appears when they are.
3. **Where they are**: `<project>/.engine/terrain-pyramid/<the index's
   folder>/` for a terrain saved as cells, beside the partition's cells for
   one cut from its scene, and the session's folder for ground no scene holds
   yet. A cache: deleted, it is made again.
4. **Made ahead where somebody would wait**: `ludwerk terrain import` makes
   them after it saves, and the partition step makes them, so a built game
   ships them -- beside the terrain's cells, which a build ships as the files
   the engine streams and not in its pack (D411: packed, the game had no
   ground at all).

### 6. Held for the ground

When the simulation waits for the ground -- the first load, a fast travel
with `PauseOutsideLoadedArea` -- the frame is the ground's: the ring under the
player is read where it stands, nearest first, inside eight milliseconds a
frame, rather than asked of a service that answers a few reads a frame.

### 7. The session cache, where it is and what goes to it

Measured on the way to the gate, and amending ADR 0149 section 1:

1. **A game's cache is in the machine's temporary folder; the editor's and an
   import's stay beside the project.** Beside the project is for the save,
   which takes the cache's files by renaming them. A game that is running
   saves nothing that way, and its project may be an installed game's folder
   -- nothing can be written there -- or a source tree mounted into a
   container, where a test that made 34 000 files took thirteen minutes.
2. **Changed ground is held until it does not fit** (D409): 256 MiB of voxels
   past the load radius, 64 MiB on a phone, and past that the furthest cells
   go until what is left fits (`FieldStreamer::LooseBudgetBytes`). ADR 0149
   wrote out every changed cell past the radius, and a file is two thirds of
   a millisecond on Windows whatever is in it and however many threads write:
   twelve kilometres of flat ground made by a script in one frame -- 305 MiB
   held, four megabytes as cells -- was thirty-eight seconds before the first
   frame. An import asks for none to be held: it is laying the world on disk.
3. **Many cells at once are encoded on every worker**, a batch at a time, and
   the field is told of a batch in one pass: an import's cells go out two and
   a half times as fast. The file's own cost does not divide.
4. **Not done: the cache as one file.** With cells appended to a file of the
   run and found by an offset, writing one out costs what encoding it does,
   and the budget has nothing left to trade. It needs reads of a part of a
   file from the streaming's IO, and a save that writes the cells out rather
   than renaming them. The ledger carries it.

### 8. The gate

`terrain_far_flight`: four kilometres laid by `--import-terrain`, the far
ground's files counted, and the world flown out and back twice **on a real
device** -- with no renderer nothing asks for a node of the far ground, and
the first version of the gate, on the null device as the soaks beside it are,
passed over a flight that built none. It fails when:

- fewer than 2 000 cells of ground streamed in, or out
  (`--soak-min-ground`): a world that stood still has the best numbers;
- the far ground was not read from its files, or a cell was read whole to
  make a file the import had made;
- the last quarter's peak memory is more than 15% over the second quarter's
  (`--soak-memory-growth`) -- the second lap over the first. A ceiling wide
  enough for a software rasteriser, which holds half as much again as a GPU
  of the same world, would have passed every leak this flight found;
- a frame's worth of time was spent inside streaming, the ground's pump
  counted with the parts'.

Frame times are not gated: the whole frame is 1.5 ms at the median on a GPU
and 24 ms on lavapipe. They are in `docs/perf-baselines.md`, measured on the
sixteen-kilometre world.

## Consequences

- On the sixteen-kilometre world (`docs/perf-baselines.md`): a flight corner
  to corner at 200 m/s an axis, `FarPlane` 1 500 and 6 000, and a still camera
  at 6 000, are under 16.6 ms at p99 with no frame over 33 ms, and memory is
  level; ten kilometres' fast travel has ground under the player in a quarter
  of a second.
- A game that changes or makes ground holds up to the budget of it past the
  load radius: some 400 MiB of the process at 256 MiB of voxels. A world made
  by a script that fits is never written out.
- The far ground's files are some 950 MB for that world's 4.0 GB of cells.
- An index gains a field a row; one written by this build is read by an older
  build, which ignores it.
- **Not done here**: the block world has no far ground at all, and a cell of
  it somebody built in is still never let go. The streaming manager still
  walks every row of its index a tick -- a third of a millisecond at 65 536
  cells -- which a world four times that would notice.
