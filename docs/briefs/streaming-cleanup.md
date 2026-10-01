# What the sixteen-kilometre world left open: the kickoff and the ledger

**ludwerk-08's order of 2026-10-01**, after P5c (`terrain-editing-perf.md`) and
the editor's catalog (`r3-editor-i18n.md`): *"A small cleanup ledger for the
open items, after R3."* Five things P5c measured, said and did not do, and
one before them that every push pays: CI. Before ADR 0147's G1 to G5, water
and F2.

| Stage | What | Decision |
|---|---|---|
| C0 | CI in twenty minutes or less: every push pays it | this ledger |
| C1 | The streaming's reads are as fast as the disk, not as the frame | this ledger |
| C2 | The session cache is one file, and the budget it stood in for goes | an ADR of its own, amending 0149 and 0150 §7 |
| C3 | The streaming manager asks only the cells near a focus | this ledger |
| C4 | The editor stays at thirty frames a second while it imports | this ledger |
| C5 | The block world: changed cells let go, and a far ground | an ADR of its own |

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- **The measurement comes before the fix**, on the worlds P5c made: the
  sixteen-kilometre import, the script that lays eight kilometres a tile a
  frame, `terrain_far_plane`'s twelve kilometres of flat ground. The numbers
  go in `docs/perf-baselines.md` beside the ones they replace.
- `terrain_far_flight` stays green and stays honest: ground in and out, the
  far ground read from its files, the second lap's memory within 15% of the
  first.
- **Built and run as a game** before it is called done -- `ludwerk build`,
  and the folder it makes -- because a project and a build of one read the
  ground by different roads (D411).
- The full `scripts/localgate.ps1` before each push, its test times read.
- **CI is read before the NEXT push, not waited for** (ludwerk-08,
  2026-10-01): after a push the next stage starts; before the push after it,
  the run before is read, and a red one is fixed first. Never a push over a
  run still going -- `cancel-in-progress` takes it. One push a stage.

## C0 — CI in twenty minutes

**Measured 2026-10-01**, on the run of `82b4492a` (`gh api .../jobs`): the
Windows job was 30 m 44 s, and of it **the build was 6 minutes** -- sccache at
97% -- and **the tests 22**: 95 tests summing to an hour of machine time
through `-j 4`, on a runner with four cores and no GPU, where every test that
draws is rasterised in software. `terrain_far_plane` 486 s,
`terrain_shadow_acne` 415 s, `terrain_far_flight` 330 s -- that one alone,
with every other test waiting, because it was listed as timed. macOS waited
behind all of it for the Linux leg's shaders: 42 minutes a push.

- [x] **The Windows tests in three jobs** (`build-test-windows`): each builds
  from the compiler cache and runs every third test (`ctest -I <shard>,,3`).
  No artifact between jobs -- a build tree is gigabytes to hand over, and the
  cache makes a second build cost what the hand-off would. One shard saves
  the cache; the others only read it.
- [x] **macOS waits for Linux alone**: Windows is out of `build-test`'s
  matrix, so `build-macos` needs the job whose shaders it takes and no other.
- [x] **`terrain_far_flight` takes its turn** and is not run alone: what it
  asserts of time is the streaming's share of a frame, in the thread's CPU
  time. Its whole-frame backstop is two seconds -- "did not stall".
- [x] **`terrain_far_plane` draws 180 frames, not 360**: what its far ground
  needs is frames (256 nodes built a frame, ninety for that world), and the
  360 paced at 60 were from when it needed time.
- [ ] Measured after: the run of this push, a job at a time, in
  `docs/perf-baselines.md`. If a shard is still past ten minutes, its
  longest test is what to shorten -- a fourth shard buys nothing.

## C1 — reads as fast as the disk

**Explained 2026-10-01, by reading**: the async IO service answers 150 to 250
reads a second because its in-flight budget is four (`initIo`'s default) and a
read stops counting against it only when `pumpIo` harvests it -- on the frame's
thread, once a pump. Four reads a pump at sixty pumps a second is 240. The
disk was never the limit: unpaced, with a millisecond frame, the same service
read 2 600 cells a second. P5c went round it for the one case that could not
wait (the ring the simulation is held for is read where it stands).

- [x] **A read stops counting when it completes, not when the frame collects
  it** (`async_io.cpp`, the harvester): a thread waits on SDL's queue and
  lands each result as it arrives -- bytes copied, status set, the place in
  flight freed -- and `pumpIo` only hands out what has landed. Callbacks still
  fire on the pump's thread, in the order the reads landed. The budget is
  eight reads at once, and it bounds the disk and nothing else.
- [x] **What waits to be collected is bounded in bytes**: 64 MiB
  (`DefaultIoReadyCeiling`), past which nothing more is admitted until
  something is taken. A frame that stalls comes back to that much and no
  more.
- [x] The queue's own tests hold a read in flight until they pump, so they
  ask for the old harvest (`initIo(1, true)`); three new ones for the new:
  reads land with no pump at all, callbacks keep their thread and their
  order, and the ceiling holds and lets the rest through.
- [x] **Measured** on the sixteen-kilometre world, the dev build, paced at 60
  (`docs/perf-baselines.md`):

  | Ten kilometres' fast travel | Ground under the player |
  |---|---|
  | Reads freed once a pump, the held ring not read where it stands | 2 003 ms |
  | **Reads freed as they land**, the same | **270 to 287 ms** |
  | And the held ring read where it stands (P5c) | 236 to 251 ms |

  The flight at 200 m/s is the same either way -- 6 547 cells in against
  6 649, median 4.1 ms, p99 12 ms: sixty cells a second was inside what four
  a pump could give, with the streamer pumping several times a frame. What
  the budget starved was a burst: a ring of four hundred cells at once.
- [x] **The held ring's synchronous read stays**: it is 35 ms faster than the
  service still, and it is what a world held for its ground has when there
  is no service at all.

## C2 — the session cache as one file

A file a cell costs half a millisecond to create on Windows whatever it holds
and however many threads write (D409): 34 000 cells of flat ground, four
megabytes of them, were twenty seconds. ADR 0150 §7 holds changed ground to a
budget instead -- 256 MiB of voxels, some 420 MiB of the process -- so that a
world that fits is never written. The budget is what the file's cost bought.

- [ ] Cells appended to a file of the run and found by an offset: a cell
  written out costs what encoding it does. A layer (Play, an import) is a
  file of its own, dropped whole.
- [ ] Reads of a part of a file, from the async service and where the ground
  is held for; the far ground's reader the same.
- [ ] A save writes the cells out of the cache and does not rename them.
- [ ] The budget goes (`FieldStreamer::LooseBudgetBytes`): everything far is
  written as it was under ADR 0149, and the script that lays eight kilometres
  is back near 640 MiB at the speed it has now.
- [ ] Asked, not assumed: **the saved terrain as one file too** -- an index
  and a pack beside it, not a folder of 65 536 files. An export copies two
  files, a phone extracts two, an import's save is a rename again. It is a
  format change to ADR 0087 with old projects to read; decided in the ADR.

## C3 — the manager asks only what is near

`StreamingManager::tick` scores every row of its index against every focus,
every tick: a third of a millisecond at 65 536 cells, and
`minimumRingResident` walks them again.

- [ ] Rows bucketed by where they are, a grid a layer; a tick visits the
  buckets a focus's drop radius reaches and the rows that are not unloaded.
- [ ] The same answer, proved: the streaming tests' scores and order
  unchanged (R10 -- the order chunks load in is a property of the operation
  sequence).
- [ ] Measured at 65 536 and at 262 144 cells.

## C4 — the editor during an import

The editor lays a tile of 256 columns inside each frame: 70 to 200 ms, eight
to ten frames a second, and a "frame took 90 ms" warning in the console every
five seconds of it.

- [ ] A tile laid in slices of a frame's budget, its state kept between
  them -- or on a worker with the frame free; whichever leaves what is laid
  voxel for voxel what one table lays (`terrain_import_tests`).
- [ ] Thirty frames a second or better throughout a sixteen-kilometre import,
  looked at (`--editor-drive`), with a cancel in the middle.
- [ ] The frame scheduler's warning is not said for a frame the editor knows
  is long.

## C5 — the block world

A streamed block world has no far ground at all, and a cell of it somebody
built in is never let go (ADR 0149 §1 is the terrain's).

- [ ] Changed block cells go to the session cache as terrain's do.
- [ ] A far ground for blocks: what a coarse level of a block world is --
  the terrain's is a gathered surface (ADR 0140), and a block is not a
  surface -- decided in the ADR before any of it is built.
