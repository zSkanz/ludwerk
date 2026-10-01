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
- The full `scripts/localgate.ps1` before each push.

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

- [ ] **The flight over it is not there** -- and was not before: the same
  numbers on the build before this stage. Corner to corner over the 16 km
  world at 200 m/s an axis, `FarPlane` 1 500 m, 60 Hz paced: the resident
  ground stays what the radius holds (2 000 to 2 300 chunks), and **memory
  climbs to 2.07 GiB, the median frame is 17 ms, p95 130 ms, 756 of 6 589
  frames over 33 ms**. With the camera still and `FarPlane` 6 000 m: 2.3 GiB
  and p95 300 ms while the far ground builds. The session cache is not in
  it; the far ground is (ADR 0144): it reads every level-0 cell a far node
  covers to summarise it, keeps a summary of each for good, and works out a
  node's content from all of them whenever any cell comes or goes. At 2 200
  cells that was 3 ms; at 65 536 it is the frame. **What a world this size
  needs is coarse levels on disk** -- a pyramid of cells, each level twice
  the voxel, written when a cell is saved or imported -- so a far node reads
  one cell of its own level, and what is kept is bounded by what is in view.
  Its own stage (P5c), and its own ADR.
- [ ] **An import's memory grows some 6 KiB a cell laid** (202 to 604 MiB over
  the 16 km): about 1 KiB of it is the index and the paths, and the rest is
  not in live blocks -- the allocator's, not found. It is bounded by the
  world's cells, not its voxels.
- [ ] **The editor's panel during an import has not been looked at**: the
  progress bar and Cancel are tested through `Editor`, not seen. With the
  other pictures of the editor that wait for the desktop.

## P6 — the frame rate follows the monitor

- [x] **Moved** (ludwerk-08, 2026-10-01): the owner asked for every graphics
  and display setting, in the editor and from a script, as Unity's and
  Unreal's are. ADR 0147 decides the whole model; its ledger,
  `settings-kickoff.md`, takes the frame rate first as G0.
