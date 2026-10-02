# Editing terrain without lag: the kickoff and the ledger

**The owner's report of 2026-10-01**, measured by ludwerk-08 by running it:
*"muito lag durante a edição do terreno"*, *"uma malha se cruzando formando um
X ... passo um smooth e resolve"*, *"um mapa do tamanho de GTA V deveria ser
possível"*, *"o editor roda a 2000 FPS ... deveria limitar à taxa do monitor"*.
In the queue after the placeability sweep (D399) and before the editor-i18n
ledger's E4, because it is the owner's daily pain. Decided by what
professional sculpting tools do (the owner's standing rule of 2026-09-30).

| Stage | What | Decision |
|---|---|---|
| P1 | A stamp of every brush, Smooth first, in a frame's budget | this ledger, D400 |
| P2 | Paint as cheap | this ledger |
| P3 | No mesh folded over itself, at any level, after any sequence | this ledger |
| P4 | A save writes only the cells that changed | this ledger |
| P5 | A world the size of a city's: 8 km and past it | this ledger, D403; an importer for what no script can hold (P5b) |
| P5b | A world larger than memory: the session cache and the importer | ADR 0149, D405, D406; the flight over it is P5c |
| P6 | The frame rate follows the monitor, in the editor and in games | moved to [ADR 0147](../decisions/0147-graphics-and-display-settings-are-one-model-the-project-sets-the-player-chooses-a-script-reads-and-writes.md), `settings-kickoff.md` G0 |

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- **The measurement comes before the fix**: a bench that shows the cost, then
  the change, then the bench again. The numbers go in `docs/perf-baselines.md`.
- **A faster brush leaves the same ground.** `terrain_tests` pins the voxels a
  run of every brush leaves (`every brush's result is fixed`, `a smooth
  stamp's result is fixed`), serially and on the job pool.
- The full `scripts/localgate.ps1` before each push, and its test times read:
  a test that took three times what it did is a finding, not a detail (D409
  went out that way).
- **After every push, CI is read before the next item starts**
  (`gh run list`, `gh run view --log-failed`). A change to `docs/` alone is
  gated too (`scripts/localgate.ps1 -Only docs`). ADR 0149's push was red for
  two hours with nobody looking, and its fix red again for a defect's number
  (D407).

## P1 — a stamp in a frame's budget

ludwerk-08's measurements, the owner's place (`adadadw`) driven in the editor
with `--editor-drive`, 30 drags a tool:

| Tool | p95 | p99 | Frames over 33 ms |
|---|---|---|---|
| **Smooth** | **420 ms** | **561 ms** | **210** |
| Raise, Lower, Flatten, Add, Dig | 14 to 16 ms | 16 to 18 ms | 0 |

And headless, a stamp a tick on 512 m of hills: `SmoothBall` at radius 8 cost
55 ms (82.7 ms in `win-msvc-dev`), the next dearest `GrowBall` at 16 m 2.1 ms.

- [x] **The smooth read four times what it moved.** A stamp read a box padded
  by `kernel + window + RampReach + 2` voxels a side -- 93 voxels across for a
  brush of 17, 800 000 voxels -- and looked for surface crossings along every
  row of it, three times over, each voxel through a clamped three-axis lookup.
  Only the rows within `kernel` of the brush are ever asked for, and the
  margin a crossing needs is the larger of what it looks along a row and
  across it, not their sum. The rows are walked a byte at a time by their
  stride, with the fraction taken only at a crossing; each crossing carries
  its brush weight, and the kernel is a table.
- [x] **On the job pool**: the box is read a chunk column a range, the rows
  scanned in buckets merged in row order, and each row's moves and rewrite --
  which read only the crossings and write only the row -- a range of rows.
  The ground is the same whichever worker ran which (R10); the digest test
  runs serially and with four workers.
- [x] Every brush's reads go through the chunk the writer read last
  (`FieldWriter::get`), where a binary search over the field's chunks was a
  third of a paint stamp.
- [x] **The growth across strokes** (146 to 717 ms, stroke after stroke) was
  the smooth's cost rising with the ground's roughness: more crossings, each
  one the whole kernel. It is gone with the cost: the owner's place, the same
  drive, p95 **14.4 ms**, p99 **17.7 ms**, none over 33 ms, in the dev build
  with the GPU validation layer on.
- [x] **Held by the bench gate**: `tests/bench/terrain_smooth` and
  `terrain_paint`, a stamp a tick on rolling ground, in `perf_budget` at the
  16 ms catastrophe budget every bench scene has; the old smooth is five times
  over it.

| A stamp, `win-msvc-dev` | Before | After |
|---|---|---|
| `SmoothBall` r 4, 1 m voxels (unit bench) | 12.5 ms | 0.5 ms |
| `SmoothBall` r 8, 512 m of hills (`editbench`) | 82.7 ms | **2.3 ms** |
| `SmoothBall` r 16, 1 m voxels (unit bench) | 98.2 ms | **3.2 ms** |
| `SmoothBall` r 32, 0.5 m voxels (unit bench) | 853 ms | 41 ms |

## P2 — paint

- [x] A paint stamp walked its ball's whole box and found each voxel's chunk
  by a search. It walks each row's chord of the ball now (a voxel wider each
  side; the distance test still decides), through the writer's chunk:
  `PaintBall` r 24, blend, **4.8 to 2.5 ms**.
- [ ] Past that the cost is the voxels themselves: a ball of 24 m paints
  58 000 of them, most inside the ground where nothing shows. Painting only
  what is within reach of the surface is the next halving, and a change of
  what paint means (dig into painted ground and the paint is not there) --
  ADR 0114's to amend if taken.

## P3 — a mesh that never folds

- [x] **Found by fuzz, and it was the mesher.** `terrain_mesh_validation_tests`
  sculpts a slab with 24 random brushes from each of 16 fixed seeds -- raises
  and digs at full strength, sharp boxes, smooths -- and checks every triangle
  of the result against every triangle near it. At full detail, before:

  | | Over 16 seeds | Worst seed |
  |---|---|---|
  | Edges shared by four faces | 86 | 27 |
  | Triangles lit from behind | 117 | 35 |
  | Triangles through each other, or folded over an edge | 0 | 0 |

  A lattice cell had one vertex, so every sheet of surface that passed through
  it was joined there: a wall a voxel thin, two hollows a corner apart. Their
  quads met on the edge beside that vertex, four faces to an edge -- in
  section, an X, which is what the owner drew; and a smooth, which moves the
  sheets apart, cured it.
- [x] **A vertex a sheet** (`cellSheets`, `terrain_mesher.cpp`): a cell's
  crossed edges are traced face by face into the sheets they belong to, a face
  with all four edges crossed deciding by its four corners -- which both cells
  that share it read alike -- and each sheet has its own vertex; a quad takes,
  from each of its four cells, the vertex of the sheet its edge is on. A cell
  with one sheet, which is nearly all of them, is as it was: no golden, no
  digest and no trace moved. A vertex whose gradient points against its own
  faces -- beside a second sheet a voxel away -- takes its faces' normal.

  | After | Over 16 seeds | Worst seed |
  |---|---|---|
  | Edges shared by four faces | 6 | 2 |
  | Triangles lit from behind | 21 | 12 |
  | Open or inconsistently wound edges | 0 | 0 |

- [x] **What is left, and why**: a neck thinner than a voxel -- two hollows, or
  two masses, that the field joins through the middle of a lattice face. One
  vertex a sheet a cell cannot hold a tube that thin; its four faces meet on
  one edge and do not cross. The test holds it under one in two thousand
  triangles of ground built to have them.
- [x] **The coarse levels** gather the same ground (ADR 0140): a slab thinner
  than a coarse cell is a sheet drawn both ways by design, and two sheets in
  one coarse cell share its vertex. Triangles through each other there: 47 in
  53 000 at level 1, 30 in 13 000 at level 2 -- drawn only from where the
  level's error is under the pixel budget. Held under one in fifty; unchanged
  by this stage.
- [ ] **P2, to revisit**: gather the coarse levels a sheet at a time, as
  level 0 now is -- a coarse cell's vertices clustered by the sheet their
  level-0 vertices are on -- so the bound of one in fifty can become none.
- Meshing a full-detail node costs what it did: 21.6 ms before, 22.2 after
  (`what meshing a terrain node costs`).

## P4 — saving only what changed

- [x] **Not reproduced, and the save already does it** (closed so by ludwerk-08). The report was every
  one of 256 cell files with a new time after a save. A copy of the owner's
  place, raised in five places and saved: **5 cells and the index written**,
  the other 251 untouched (`saveTerrain` writes a cell only when
  `terrainCellUntouched` says its chunks changed, and `terrain_cells_tests`
  holds it). ludwerk-08's own copies show the same: 2 to 9 files newer than
  the copy, the rest the copy's time -- the 256 were the copy itself.

## P5 — a city's worth of world

- [x] **The V-shaped notches were the ground ending** (D403). ludwerk-08's
  picture -- an 8 km world at a 2 m voxel from (-3500, 900, -3500) towards
  (1500, 0, 1500) -- reproduced to the pixel. The height function's own
  silhouette from that camera, worked out apart, is one smooth dome with its
  top 417 pixels down; the picture's "silhouette" was 480 to 560 down, which
  is ground 4.8 km from the camera in a world that reaches 10.7. The loader
  drew nothing past `viewDistance`, 4 096 m, whatever the camera's
  `FarPlane`, and dropped whole nodes by their distance: a scalloped edge with
  a notch where two nodes met, against haze the colour of the sky.
- [x] **The ground is drawn as far as the camera sees**
  (`render::terrainLodFor`): the view distance is the camera's far plane. The
  same camera with `FarPlane = 20000` draws the world whole, the dome's top at
  418 pixels: 278 000 triangles for 91 000, median frame 2.2 ms for 1.5, and
  the same 15 frames over 33 ms either way -- the script writing its tiles.
  `terrain_loader_tests` holds it.
- [x] **And it ends on a line** (ludwerk-08's review): the far plane is flat
  and a node's distance is round, so a node at the side of the picture is
  inside the plane and further from the camera than the plane is. Dropped at
  the plane's own distance, the ground was still scalloped at the sides of a
  world wider than the default 5 000 m. The view distance reaches the far
  plane's corners -- a node goes only when it is wholly past the plane, and
  the clip does the rest a pixel at a time. `terrain_far_plane` photographs a
  flat slab 12 km across under a camera that sees five: the row under the
  far plane's line is ground from one side of the picture to the other, the
  row over it sky.
- [ ] **The cut itself is never seen**: by default the far ground should be
  gone into the air before the plane clips it, as the engines' defaults do.
  A scene has no fog unless it sets `FogEnd` or wears an `Atmosphere`, and
  neither knows the camera's far plane. To decide with ADR 0147's
  `ViewDistance` (G1): a fade over the last tenth before the plane, in the
  terrain's and the parts' shaders, into what is behind.
- [x] **A 16 384 by 16 384 world at a 1 m voxel, saved as cells and streamed**
  -- could not be measured, because **nothing could make one** (P5b makes
  it, and measures it). What stood in the way, found trying:
  - A script generates ground resident: 8 km at a 2 m voxel peaks at 787 MiB,
    so 16 km at 1 m is some 12 GiB before anything is saved. **A cell
    somebody changed is never evicted**, by design, so streaming does not
    relieve a generator.
  - `--save-scene` writes what a script built in its boot, and a boot is one
    drain under the 5 s watchdog: a row of eight 1 km tiles, and then the
    scene is written -- inline, 206 MB of JSON for that row, where the
    editor's save would have cut cells.
  - The editor refuses an edit that reaches more than 4 096 cells not loaded.

  A world that size is imported, not sculpted in one go, in every engine that
  has one. What this stage needs is an importer that writes cells straight to
  disk a tile at a time -- from a heightmap, or from a script's function of
  `(x, z)` -- and never holds more than a tile: `ludwerk terrain import`, and
  the editor's Create tab over it. Its own stage, P5b, with its own bench: the
  flight over the result, memory bounded by the load radius.

  **Approved by ludwerk-08 (2026-10-01), after ADR 0147's G0, with one more
  thing**: ground made or changed at run time must be evictable too. A changed
  cell past the load radius is written to a session cache on disk and let go,
  and streamed back like any saved cell -- the editor's working copy, a game's
  edits and a script's generator alike; saving the scene commits the cache,
  and undo reaches across cells let go. Its own ADR; its bench is 16 km at a
  1 m voxel generated by a script in bounded memory.

  **Built: P5b, below.**

## P5b — a world larger than memory (ADR 0149)

- [x] **A changed cell is written out and let go** (`FieldStreamer`,
  `spillFarGround`): past the load radius and two cells, to this run's folder
  under `.engine/session/`, in the format a saved cell has; it streams back
  like any cell. Ground no file describes is the same -- a generated world
  becomes a streamed one the first time its far ground is written out.
  Bounded by count, not by a clock: past 96 cells waiting, all of them go
  before the frame goes on.
- [x] **Only where nothing could want the ground back as it was** (§1.6). The
  editor's brush keeps what it changed in memory until the save, so undo is
  what it was; Play and an import write to a layer of the cache that Stop and
  Cancel drop (`beginSessionLayer`, `dropSessionLayer`, `keepSessionLayer`).
  Found working it out: a snapshot of the world is not a snapshot of a file,
  and an undo across a cell let go would have brought back ground the edit
  had added.
- [x] **A save commits the cache** by moving its files into the scene's folder
  -- a rename on one disk -- and a part of a cell written where it was not
  loaded goes out with the rest of its file.
- [x] **Nothing falls**: a cell within a collider's reach of a loose body or a
  character is not written out, wherever the camera is.
- [x] **Cells read for an edit are let go** (`m_readForEdit`): `loadNow` read
  cells the manager never asked for, so the manager never evicted them -- a
  generator kept the cells along every tile's edge, two chunks a cell of the
  whole world.
- [x] **The importer** (`TerrainImport`, `terrain_import.h`): a tile of 256
  columns at a time, each with a column of its neighbours round it
  (`asset::HeightWindow`), so what is laid is voxel for voxel what one table
  lays -- `terrain_import_tests` compares the two over hills, and shows tiles
  laid without the apron differ at the seams. Sources: an image, a folder of
  tiles `<name>_x<i>_y<j>`, hills, and a Luau `function(x, z)`.
- [x] **`ludwerk terrain import`** and the engine's `--import-terrain=` under
  it; the editor's Create tab lays anything past 4 096 columns the same way,
  with a progress bar and Cancel, and gains From a function.
- [x] **The far ground is told, not made again**
  (`TerrainCellSource::update`): the cell source was rebuilt whenever the
  index changed and forgot every summary it had read. With ground written out
  every frame that was the whole horizon read again every frame.
- [x] **Two defects of the streaming under it**, found measuring: a cell whose
  read came back after the camera had left was held for ever (D405), and the
  far ground drew a saved cell as it was before the save (D406).

**The bench** (`docs/perf-baselines.md`, "A world larger than memory"), at a
1 m voxel, this machine, the dev build:

| | Time | Peak private memory |
|---|---|---|
| `ludwerk terrain import hills --size=8192` | 106 s | 386 MiB |
| `ludwerk terrain import hills --size=16383`, 65 536 cells, 4.0 GB of cells | **411 s** | **617 MiB** |
| A script, 8 km, `WriteHeights` a 512 tile a frame, camera seeing 1 km | 91 s | 643 MiB, level from the 24th second |
| The same, camera seeing 20 km | 94 s | 1 120 MiB and rising with the world |

Held whole, the 16 km world is some 12 GiB before a save.

- [x] **The flight over it** was not there -- and had not been before: the
  far ground read every level-0 cell a far node covers, kept a summary of
  each for good, and worked out a node's content from all of them whenever
  any cell came or went. At 2 200 cells that was 3 ms; at 65 536 it was the
  frame. P5c, below.
- [x] **An import's memory growing some 6 KiB a cell laid**: 1.9 KiB once
  D408 and the summaries were out of it, and that is the index. P5c.
- [x] **The editor's panel during an import**: looked at, with a cancel in
  the middle of sixteen kilometres. P5c.

## P5c — a world that size, flown (ADR 0150)

The order: *"The owner's goal is a GTA-sized map he can fly and play, not
just one that can be made."* Six conditions; each is named below.

- [x] **The far ground is kept on disk as it was gathered** (condition 1;
  `asset::TerrainPyramid`, `terrain_pyramid.h`): a file a node at levels 3, 4
  and 5 -- cells of 8, 16 and 32 voxels -- holding, a chunk, the surface ADR
  0140 gathers and what a coarse mesh reads of the chunk without its voxels.
  Never voxels averaged to a coarser voxel. `terrain_pyramid_tests` runs the
  T2 tests on the files with nothing resident: the gallery's two-metre slab
  and its balls have triangles at every level kept, flat ground is within
  2 cm, a coarse cell takes the material on top, paint survives -- and a node
  built from the files is, vertex for vertex, the node the whole ground gives.
  ADR 0144's follow-up, "persisting the gathered coarse surfaces", is this.
- [x] **Incremental** (condition 2): a cell is known by what it holds
  (`terrainCellSignature`, in its row of the index as `"sig"`); a block -- a
  top-level node's footprint, 32 chunk columns, 21 files -- regathers the
  columns of a cell that holds something else and the columns beside them,
  from that cell and the eight round it, and writes the files those columns
  are in: one a level. The test holds a crater to nine cells read, one block
  and three files.
- [x] **A node is named by its cells, and asked only when it could have
  changed**: its content is its cells' signatures, not every chunk under it;
  the terrain counts which of its revisions were ground streaming in or out
  as it is on disk (`TerrainComponent::streamedRevisions`), and while neither
  the edits nor the cells have moved no node is checked. That walk was
  11.6 ms of every frame of the flight.
- [x] **The selection does not walk what is not built** (`Node::builtBelow`),
  and **far ground is built from the top down** (`farUnbuilt`): forty thousand
  nodes were asked after a frame while the far ground was behind.
- [x] **Bounded** (condition 4): 384 decoded files, 1 024 cells' summaries,
  and what was gathered of a cell's chunks goes when the cell does
  (`TerrainField::dropSurfaces`). Three leaks of the streaming under it, each
  rising with the distance flown: D405, D406, and **D408** -- a cell's bytes
  were kept, at their capacity, after the cell was made of them.
- [x] **The 6 KiB a cell of an import is accounted for**: measured again it is
  1.9 KiB a cell, and it is the index -- a row, its path and the manager's
  entry. The rest was D408 and the summaries, which no longer grow.
- [x] **Old projects** (condition 5): an index whose rows say nothing of what
  a cell holds has its blocks brought up to date in the background, nearest
  the camera first, a block at a time off the main thread
  (`FieldStreamer::pumpFarGround`); the block's own file remembers each cell
  by its file's size and time, so the next run reads none of them. The
  terrain panel says how far it is. The world is never held for it.
- [x] **Held for the ground** (the fast-travel target): while the simulation
  waits for the ground under a player, the ring is read where it stands,
  nearest first, inside 8 ms a frame -- not asked of a service that answers a
  few reads a frame.
- [x] **A gate** (condition 3; `terrain_far_flight`,
  `tests/streamsoak/run_far_flight_gate.cmake`): four kilometres laid by
  `--import-terrain`, the far ground's files counted, and the world flown out
  and back twice on a real device. It fails on ground that did not stream in
  and out, on far ground not read from its files, on a cell read whole to
  make a file the import had made, on a second lap that holds 15% more than
  the first, and on a frame's worth of time inside streaming. **The first
  version of it ran `--rhi=null` and passed over nothing**: no renderer, so no
  node of the far ground was ever asked for. Found because 75 MiB was too
  good. The soak's report now counts the ground (`--soak-min-ground`) and
  compares the laps (`--soak-memory-growth`); a ceiling wide enough for a
  software rasteriser would have passed every leak this flight found.
- [x] **The editor's panel during an import, looked at** (condition 6;
  `--editor-drive`, pictures in the session's scratchpad): sixteen kilometres
  of hills from the Create tab -- the size typed, the note that it is laid a
  tile at a time, the bar, Cancel. Cancelled at 17%: "import cancelled; the
  world is as it was", the panel back to "This world has no terrain yet", no
  cell and no far-ground file left on disk, the frame rate back from 8 to
  100. Three things seen and changed: the scene was left marked as changed
  (D410); the far ground's own percentage was said a line above the import's
  bar, two numbers for what reads as one job; and nothing else. One seen and
  left: the editor draws at 8 to 10 frames a second while a tile is laid each
  frame, and the console takes a "frame took 90 ms" warning every five
  seconds of it.

- [x] **Built and flown as a game** -- the owner tests through the package.
  `ludwerk build` of the gate's four-kilometre world, and the folder it
  makes run: **an exported game with a streamed terrain had no ground**
  (D411, since ADR 0087): the cells were packed, and the engine streams them
  as files. They ship as files now, the far ground's with them. And **a
  terrain saved as cells was not waited for at start** (D412): the first
  ticks ran over nothing, and whoever stood there fell.

- [x] **And as an APK** (ludwerk-08's order after the report: "the same
  class as D411, and the owner tests on his phone"). `ludwerk build --target
  android` of the same world: the APK carries the 4 356 cells at
  `game/content/terrain/`, the index, and the far ground's 488 files under
  `game/.engine/terrain-pyramid/`, and `payload.txt` -- the list the phone
  extracts by, an APK's assets not being a file system -- names every one.
  What the APK holds, extracted as the phone extracts it and run: the ground
  is waited for and streams (5 295 cells in, 5 065 out), the far ground is
  read from the shipped files, no cell is read to make one.
  `tests/android`, "an APK of a game with a streamed terrain carries its
  cells and its far ground". **Not run on a device**: there is no emulator
  on this machine and none was installed, and nothing is put on the owner's
  phone but by him. One thing fixed by reading: a game's session cache went
  to the system's temporary folder, which an app may not write -- changed
  ground would have stayed in memory for good; it goes to the app's own
  storage where the first is refused.

**What the gate's clock found, in ADR 0149's own stage** (D409): every
changed cell past the load radius was written out, whatever it weighed, and a
file is two thirds of a millisecond on Windows however many threads write.
`terrain_far_plane` -- twelve kilometres of flat ground a script makes in one
frame, 305 MiB held, 4 MiB as cells -- went from under a minute to 142 s, and
to 716 s in the Linux container, and was pushed. Now: changed ground is held
until it does not fit (256 MiB of voxels, 64 on a phone), the furthest first
past that; a game's cache is in the machine's temporary folder; many cells at
once are encoded on every worker. `terrain_far_plane` is 33 s and 74 s.

**The Linux stage compiles what CI compiles** (D407): the Tier-2 image had
libstdc++ 13 and the hosted runner has 14, which no longer brings
`<algorithm>` in by the way. ADR 0149's push was red for it for two hours.

**The bench** (`docs/perf-baselines.md`, "A world that size, flown"), the
16 km world of 65 536 cells, the dev build, paced at 60:

| | Median | p99 | Over 33 ms | Memory |
|---|---|---|---|---|
| Flight corner to corner at 200 m/s, `FarPlane` 1 500 | 4.6 ms | 11.9 ms | 0 | level at 790 MiB |
| The same, `FarPlane` 6 000 | 5.7 ms | 11.5 ms | 0 | level at 810 MiB |
| Camera still, `FarPlane` 6 000 | 5.7 ms | 8.3 ms | 0 | 801 MiB, flat |
| Before, the flight at 1 500 | 18.0 ms | 33.5 ms | 77 | 2.6 GiB, rising |

A teleport of ten kilometres has ground under the player in 252 to 268 ms
(2.4 s before). The import with the far ground's files is 430 s and
575 MiB. A project from before makes its files in the background in
under three minutes, at a p99 of 8.2 ms while it does.

- [ ] **The session cache as one file** (ADR 0150 section 7.4). A file a cell
  costs half a millisecond to create on Windows whatever it holds: 65 536 of
  them are most of what an import's writing costs, and the reason changed
  ground is held to a budget and not written as it goes. Cells appended to a
  file of the run, found by an offset, make writing one cost what encoding it
  does. It needs reads of a part of a file from the streaming's IO, and a
  save that writes the cells out and does not rename them.
- [ ] **The editor during an import draws at 8 to 10 frames a second**: a
  tile is laid inside each frame. Enough for a bar and Cancel; not what an
  editor should feel like. The tile on a worker, the frame free.
- [ ] **The streaming manager walks every row of its index a tick**: a third
  of a millisecond at 65 536 cells. A world four times that would notice.
- [ ] **The block world has no far ground**, and a cell of it somebody built
  in is still never let go.
- [ ] **The async IO service answers 150 to 250 reads a second**, and it is
  not known why; the held ring reads where it stands instead of asking it.
- [ ] **A streamed terrain on a phone, run**: checked to the APK's contents
  and no further. The first launch extracts a file a cell.

These five and the first are a ledger of their own after R3 (ludwerk-08,
2026-10-01); the session cache as one file removes the budget.

## P6 — the frame rate follows the monitor

- [x] **Moved** (ludwerk-08, 2026-10-01): the owner asked for every graphics
  and display setting, in the editor and from a script, as Unity's and
  Unreal's are. ADR 0147 decides the whole model; its ledger,
  `settings-kickoff.md`, takes the frame rate first as G0.

## P7 — the brush is seen when it lands

The owner, 2026-10-01: "everything in the terrain editor feels delayed" -- not
a low frame rate; any small action lags.

- [x] **Measured first** (`TerrainLoader::editLatency`, shown in Stats under
  Terrain as "Edit to picture"): from the `sync` that finds ground edited to
  the one that puts up the meshes built from it. A 4 m brush stamp was **three
  frames** late: built off the main thread, then put up six meshes a frame,
  and a stamp is the node under it and the eight round it.
- [x] **A small edit is built in the frame that finds it**: at most 24 nodes
  and 16 finest nodes' worth of ground, on every worker, put up at once and
  drawn in that frame -- **no frames late**. Its build does not gather the
  surfaces a later change of level would read; that stays with the builds off
  the main thread.
- [x] **A large edit goes up faster too**: half the workers, up to eight,
  where it was a quarter up to four, and 24 meshes a frame where it was six.
- [x] A level-0 mesh asks "ground or air" of every cell with an integer
  compare where it divided: the same answer, and what is left of a walk that
  is nearly all wholly ground or wholly air.
- [x] Test: `terrain_loader_tests.cpp`, "a brush stamp is drawn in the frame
  that finds it, built off the main thread or not".
- Later: a level-0 mesh is still some 7 ms in the dev build, 5 of them in the
  walk of a node's cells from the terrain's floor to its ceiling -- a column's
  solid runs would skip the uniform ones; the stroke's start and end (undo
  capture, dirty marking), the foliage, navmesh and collider work a stamp
  sets off, and paint's uploads, are not measured yet; the starter grass
  still reads as a grid from ten to forty metres.
