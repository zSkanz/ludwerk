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
| P5 | A world the size of a city's: 8 km and past it | this ledger |
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

- [ ] The owner's "X" was not reproduced by ludwerk-08 (the arcs he saw were
  the camera inside a sculpted ball). A fuzz test of random sculpt sequences
  -- raise, lower, add, dig, smooth at full strength, sharp shapes -- meshed
  at every level, every triangle checked: none crossing another, none folded
  against its neighbours, no edge shared by more than two. The suspect is the
  surface vertex placed outside its cell on a sharp feature.

## P4 — saving only what changed

- [x] **Not reproduced, and the save already does it** (closed so by ludwerk-08). The report was every
  one of 256 cell files with a new time after a save. A copy of the owner's
  place, raised in five places and saved: **5 cells and the index written**,
  the other 251 untouched (`saveTerrain` writes a cell only when
  `terrainCellUntouched` says its chunks changed, and `terrain_cells_tests`
  holds it). ludwerk-08's own copies show the same: 2 to 9 files newer than
  the copy, the rest the copy's time -- the 256 were the copy itself.

## P5 — a city's worth of world

- [ ] The V-shaped notches in the far silhouette of an 8 km world at a 2 m
  voxel, from (-3500, 900, -3500) towards (1500, 0, 1500).
- [ ] A 16 384 by 16 384 world at a 1 m voxel, saved as cells and streamed:
  memory bounded, the flight smooth.

## P6 — the frame rate follows the monitor

- [x] **Moved** (ludwerk-08, 2026-10-01): the owner asked for every graphics
  and display setting, in the editor and from a script, as Unity's and
  Unreal's are. ADR 0147 decides the whole model; its ledger,
  `settings-kickoff.md`, takes the frame rate first as G0.
