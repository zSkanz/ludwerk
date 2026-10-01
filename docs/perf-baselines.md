# Performance Baselines

Milestone gates compare against the numbers recorded here (roadmap: no >10%
regression vs the previous milestone until M8; absolute targets bind at M8).

## Methodology

- **Reference machine:** recorded at M1 the first time a perf number is
  captured (CPU, GPU, RAM, OS build, driver version) — append it below and
  never change it silently; a hardware change is an ADR + full re-baseline.
- **Fixed scenes:** each baseline names its scene (an example + a scripted
  camera/input path). Scenes are deterministic (seeded) so numbers are
  comparable.
- **Capture:** headless where possible (`luaug-host --bench=tests/bench` for
  sim numbers; windowed scripted runs for frame times), release/`dev` preset
  stated per row, 3 runs, median reported. Frame-time histograms (not just
  averages) for anything gate-relevant; hitch = frame > 33 ms.
- **A quiet machine, and it matters more than it sounds.** `churn10k` measured
  3.50 ms/tick while the Docker tier of `scripts/localgate.ps1` was building in
  the background, and 2.02 ms/tick three runs in a row once it finished — a 73%
  error, larger than any regression worth recording. Numbers taken beside
  another build are not numbers.
- **Storage:** one table per milestone, appended — never rewrite history.
  The CI perf smoke reads the latest table for its thresholds.
- **A quiet machine should be enforced, not requested.** The paragraph above is
  discipline written down, and the error it describes (73%) is larger than any
  regression worth recording. Sampling machine load before and after a run and
  marking a row suspect when the two disagree turns that discipline into
  something a bad baseline cannot slip past — the same move this repository
  already made with generated-file freshness and declared-vs-bound coverage.
- **Record a reduced-CPU row beside the reference one.** Every number here comes
  from a fast desktop, and a factor of 30 to budget there can be a factor of 3
  on a modest target — which is the scenario R16 exists for, since iOS has no
  JIT. Re-running the same benchmark pinned to a subset of cores costs a flag
  and gives the absolute targets that bind at M8 somewhere to fail early.

## Reference machine

Recorded 2026-08-20 at M2, the first milestone with a simulation to measure.
Changing any of it is an ADR plus a full re-baseline (see Methodology).

| | |
|---|---|
| CPU | Intel Core i5-14600K, 14 cores / 20 threads |
| GPU | NVIDIA GeForce RTX 4070 Ti SUPER, driver 32.0.16.1047 |
| RAM | 64 GB |
| OS | Windows 11 Pro, 10.0.26200 build 26200 |
| Toolchain | MSVC 14.50.35717, `win-msvc-dev` (RelWithDebInfo, profile `dev`) |

## Baselines

### M2 — simulation kernel

Captured with `luaug-host --bench=tests/bench --bench-repeats=5`, median of
5 runs, three times over; the spread across those three was under 1.5%.

**What a horde costs, and where the ceiling actually is.** Asked on 2026-08-20
as "could I build a survivors-like on this engine, it has to be optimized", and
answered by building one and measuring it rather than by estimating. The scene:
N enemies as anchored `MeshPart`s whose `Position` is written from Luau **every
tick**, each chasing a `CharacterBody` player that circles so the horde never
settles; one sun with shadows; 1080p; the default SDL3 GPU backend; measured
with `--frame-stats`, 300 frames, first ten dropped as warm-up.

| Enemies | Simulation | Shadow pass | Forward pass | Visible draws | Frame |
|---|---|---|---|---|---|
| 200 | 0.69 ms | 0.70 ms | 1.23 ms | 400 | **2.63 ms** |
| 500 | 0.87 ms | 1.78 ms | 2.60 ms | 990 | **5.25 ms** |
| 1,000 | 1.18 ms | 2.59 ms | 4.07 ms | 1,560 | **7.84 ms** |
| 2,000 | 1.84 ms | 2.40 ms | 6.86 ms | 2,092 | **11.10 ms** |

The simulation column is the whole engine below the renderer — the Luau chase
loop over every enemy, the Instance writes, the scene walk, physics — and two
thousand enemies cost 1.84 ms of it. That is not the ceiling and this table is
the evidence.

**The ceiling is one draw call per visible object**, and the proof is that the
2,000-enemy scene costs the same at every resolution:

```
 320x180 : median 10.21 ms
1920x1080: median 10.22 ms
3840x2160: median 10.22 ms
```

Identical, for a scene with 12,552 triangles in it. The GPU is idle; the frame
is CPU-side submission at roughly **2.6–3.3 µs per visible draw**, one
`bindUniforms` and one `draw` each (`renderer_default.cpp:407`), with nothing
batching the two thousand objects that share a single mesh. Off-screen casters
are already culled against the shadow radius (`render_world.cpp:282`), so the
shadow column is bounded rather than growing with the horde — the leak that was
looked for and is not there.

**So a survivors-like with two thousand enemies runs at 60 Hz today**, with five
milliseconds to spare, and nearly all of the spent budget is the one thing
M7.5 now names as scope: instanced draws. Enemies that *collide* with each other
rather than merely being drawn cost 4.7 µs each on top (below), which is why the
scene above writes positions instead — the same architecture the genre uses.

Not committed as a gate scene: it was measured outside the repository, because
M7.5 is where it becomes a number something defends.

**What fifty characters cost, and whether there is a ceiling.** Asked during
M5's review, on the concern that character-against-character is O(n²). It is
not, and the measurement is why: a `CharacterBody` collides with another one
through the rigid body inside its capsule, which the broad phase indexes like
any other. A registration list would have been quadratic; a tree query is not.
Measured on the reference machine by scaling `crowd50`'s scene, five repeats
each:

| Characters | Mean sim tick | Per character |
|---|---|---|
| 10 | 0.026 ms | 2.6 µs |
| 50 | 0.209 ms | 4.2 µs |
| 100 | 0.475 ms | 4.7 µs |
| 200 | 0.939 ms | 4.7 µs |

Four times the crowd costs four and a half times the tick, and the per-character
cost stops climbing at around 4.7 µs — that is linear with a constant, which is
what a broad phase buys. **There is no ceiling in the engine**: nothing refuses
the fifty-first character, and the honest limit is the tick budget. Two hundred
of them, all touching, is 0.94 ms of a 16.7 ms frame. Only `crowd50` is
committed as a gate; the other three points were measured outside the repository
so that CI pays for one scene rather than four.

**Where a budget in this table actually lives, because the answer is not
"CI".** Every one of them is the `budgetMs` in that scene's own
`tests/bench/<name>/scenario.json`, and the thing that enforces it is the
`perf_budget` CTest -- which runs in the LOCAL gate, on the machine these
numbers were measured on, and in CI as one of the same suite. A row that said
"the CI threshold" was describing a place rather than a mechanism, and it was
describing the wrong place: nothing in `.github/workflows` knows what a bench
scene costs.

**And the budgets are catastrophe detectors, not regression detectors.**
`bench.h` says so and this table is the reason it can: a runner's speed varies
by more than any regression worth catching, so the gate fails on something being
broken and the numbers below are where a change of a millisecond is noticed by a
person reading a diff. That is why `churn10k`'s budget is 32 ms against a
measured 7 -- more than four times the headroom, deliberately.

| Milestone | Scene | Preset | Metric | Value | Budget/Gate |
|---|---|---|---|---|---|
| M2 | `tests/bench/instances500` (500 parts in 10 models, one CFrame write each per tick) | `win-msvc-dev` | mean sim tick | **0.134 ms** | 4 ms — the roadmap's "500-instance scene ticks under budget" |
| M2 | `tests/bench/instances500` | `win-msvc-dev` | worst sim tick | 0.357 ms | — |
| M2 | `tests/bench/churn10k` (10,000 parts, 1,000 listeners, two thirds moving) | `win-msvc-dev` | mean sim tick | **2.02 ms** | 32 ms — `churn10k`'s own `budgetMs`, checked by the `perf_budget` test |
| M2 | `tests/bench/churn10k` | `win-msvc-dev` | worst sim tick | 2.95 ms | — |
| M3 | `tests/hotreload` one-script project | `win-msvc-dev` | reload span | **0.9 ms** | 500 ms — ADR 0024's hard requirement |
| M3 | `tests/hotreload` 500-instance project (5 models × 100 parts, all moving) | `win-msvc-dev` | reload span, worst of 3 | **1.6 ms** | 500 ms |
| M3 | `tests/hotreload` 500-instance project | `linux-clang-dev` (container) | reload span, worst of 3 | 0.7 ms | 500 ms |

### M5 — the world gets mass

Captured with `luaug-host --bench=tests/bench --bench-repeats=5`, median of 5
runs, three times over; the spread across those three was under 3%.

**Every simulation number in this table is measured against a scene that now
contains rigid bodies, and the M2 rows are not comparable to it.** A `BasePart`
that is not `Anchored` is a Jolt body from this milestone on, so `instances500`
and `churn10k` did not get slower doing the same work -- they are doing more of
it, in a world that has a simulation in it. Anchoring their parts is what keeps
what they were written to measure measurable (see `churn10k`'s own comment); the
physics cost that remains is the mirror's per-tick sweep over ten thousand
static bodies plus Jolt's own broad-phase pass over them.

**The physics tick is recorded in three stages**, which is the roadmap's ask
("one number says a budget was missed and three say which stage missed it")
answered with the three stages that are separable at this seam: `apply` is the
scene's writes going down, `step` is the solver, `writeback` is the result
coming back. It is not broadphase / narrowphase / solver, and `UNCONFIRMED.md`
U-56 records why -- Jolt exposes that split only through a profiler that dumps
to a file and taxes every configuration to enable.

| Milestone | Scene | Preset | Metric | Value | Budget/Gate |
|---|---|---|---|---|---|
| **M5** | `tests/bench/physics1k` (1,000 active bodies: 25 towers of 40 crates, so the islands stay awake) | `win-msvc-dev` | mean sim tick | **2.02 ms** | 16 ms — the roadmap's "physics tick budget for 1,000 active bodies" |
| M5 | `tests/bench/physics1k` | `win-msvc-dev` | worst sim tick | 4.51 ms | — |
| M5 | `tests/bench/physics1k` | `win-msvc-dev` | physics: apply / step / writeback | 0.024 / 1.78 / 0.214 ms | — |
| M5 | `tests/bench/instances500` (500 parts, one CFrame write each per tick, now also 500 static bodies) | `win-msvc-dev` | mean sim tick | **0.62 ms** | 4 ms |
| M5 | `tests/bench/instances500` | `win-msvc-dev` | physics: apply / step / writeback | 0.081 / 0.313 / 0.078 ms | — |
| M5 | `tests/bench/churn10k` (10,000 anchored parts, 1,000 listeners, two thirds moving) | `win-msvc-dev` | mean sim tick | **4.96 ms** | 32 ms |
| M5 | `tests/bench/churn10k` | `win-msvc-dev` | worst sim tick | 9.13 ms | — |
| M5 | `tests/bench/churn10k` | `win-msvc-dev` | physics: apply / step / writeback | 1.60 / 1.23 / 0.026 ms | — |
| M5 | `tests/bench/crowd50` (50 `CharacterBody` shoulder to shoulder, all walking into a wall, so the crowd stays a crowd) | `win-msvc-dev` | mean sim tick | **0.22 ms** | 16 ms |
| M5 | `tests/bench/crowd50` | `win-msvc-dev` | worst sim tick | 0.68 ms | — |
| M5 | `tests/bench/crowd50` | `win-msvc-dev` | physics: apply / step / writeback | 0.191 / 0.019 / 0.004 ms | — |
| M5 | `examples/03-physics-playground` (the deliverable: 18 dynamic crates, a seesaw, ramps, a character, the Jolt wireframe on) | `win-msvc-dev` | median frame, 1080p | **1.11 ms** | 16.7 ms — a 60 fps frame |
| M5 | `examples/03-physics-playground` | `win-msvc-dev` | worst frame | 1.93 ms | — |
| M5 | `examples/03-physics-playground` | `win-msvc-dev` | draws / triangles | 0 / 0 — every part is a debug wireframe (D022) |
| **M6** | `tests/bench/platforms200` (200 platforms written every tick, 200 written every 15th so each transitions both ways 20 times, 600 anchored parts nobody writes) | `win-msvc-dev` | mean sim tick | **0.60 ms** | 16 ms |
| M6 | `tests/bench/platforms200` | `win-msvc-dev` | worst sim tick | 2.8 ms | — |
| M6 | `tests/bench/platforms200` | `win-msvc-dev` | physics: apply / step / writeback | 0.14 / 0.29 / 0.08 ms | — |
| M6 | `tests/bench/churn10k` **after D031** (the same scene: two thirds of its anchored parts are written every tick, so two thirds of them are now KINEMATIC) | `win-msvc-dev` | mean sim tick | **7.32 ms** | 32 ms |
| M6 | `tests/bench/churn10k` | `win-msvc-dev` | physics: apply / step / writeback | 0.43 / 3.89 / 0.78 ms | — |
| M6 | `tests/bench/churn10k` | GitHub `windows-latest` | mean sim tick | 19.6 ms | the runner is 2.7x this machine, which is why the budget is a detector and this file is the instrument |
| M6 | `examples/04-obby` (the deliverable: the course, two tweened platforms, a skinned rig, a `ScreenGui` with a list layout, five sounds) | `win-msvc-dev` | median frame, 1080p | **0.53 ms** | 16.7 ms — a 60 fps frame |
| M6 | `examples/04-obby` | `win-msvc-dev` | worst frame | 1.84 ms | — |
| M6 | `examples/04-obby` | `win-msvc-dev` | draws / triangles | 15 / 172 — solid parts now, where M5's playground was 0 / 0 |
| M6 | `luaug_ui_tests` (a laid-out tree, then two more frames touching nothing) | `win-msvc-dev` | `layoutStats().solverRuns` on an idle frame | **0** | 0 — asserted, not measured |
| **E9** | `tests/bench/sockets200` (200 anchored posts, each with a free arm on a `BallSocketConstraint`; half limited, half not) | `win-msvc-dev` | mean sim tick | **0.66 ms** | 16 ms |
| E9 | `tests/bench/sockets200` | `win-msvc-dev` | worst sim tick | 1.11 ms | — |
| E9 | `tests/bench/sockets200` | `win-msvc-dev` | physics: apply / step / writeback | 0.019 / 0.584 / 0.051 ms | — |
| **E9** | `tests/bench/ragdoll10` (10 humanoids, 160 bodies and 150 joints in **10 islands**, dropped and still moving at tick 300) | `win-msvc-dev` | mean sim tick | **0.31 ms** | 16 ms |
| E9 | `tests/bench/ragdoll10` | `win-msvc-dev` | worst sim tick | 0.75 ms | — |
| E9 | `tests/bench/ragdoll10` | `win-msvc-dev` | physics: apply / step / writeback | 0.012 / 0.262 / 0.034 ms | — |
| E9 | `tests/bench/physics1k` **after the constraint family** (the same scene, unchanged) | `win-msvc-dev` | mean sim tick | **2.00 ms** | 16 ms — unchanged from M5's 2.02, which is the claim |
| E9 | `tests/bench/churn10k` **after the constraint family** (the same scene, unchanged) | `win-msvc-dev` | mean sim tick | **6.98 ms** | 32 ms — 7.32 at M6, so the three new per-tick passes cost a scene with no joints in it nothing |

| **F1** | `tests/bench/terrain_sculpt` (128 m of ground, one brush stamp every tick at 2, 4 and 8 m, one dig in seven, one paint in seven), re-measured on the voxel grid (ADR 0082; it was 4.27 ms on the hybrid) | `win-msvc-dev` | mean sim tick | **0.19 ms** | 16 ms |
| F1 | `tests/bench/terrain_sculpt` | `win-msvc-dev` | worst sim tick | 1.44 ms (was 15.06) | — |
| ADR 0082 | meshing one full-detail terrain node, 32 by 32 voxels and its surface, what a dig rebuilds (`luaug_render_tests --test-case="what meshing a terrain node costs" --no-skip`) | `win-msvc-dev` | per node | **3.7 ms** at 1 m, **5.2 ms** at 0.5 m (walls, bottoms and the openness rays over the whole sphere; 2.9 and 3.3 ms without the rays) | — |
| **F1 H2/H3** | `openworld_soak`, the flagship **with its middle 512 m as streamed terrain** (a 1 m heightmap written by `Terrain:WriteHeights`, a hill and a tunnel; 5,939 frames of walking and flying) | `win-msvc-dev` | median / p99 / worst frame | **2.04 / 3.10 / 5.19 ms**, 0 hitches | 33 ms p99 |
| F1 H2/H3 | `openworld_soak` | `win-msvc-dev` | worst streaming pump | 1.04 ms | — |
| F1 H2/H3 | `openworld_soak` | `win-msvc-dev` | peak resident memory | **56 MiB** (47 MiB with boxes for ground) | 192 MiB |
| F1 | Sculpting that ground in `tools/sculpt-ground`: 263,169 columns | `win-msvc-dev` | whole run, boot to written scene | **0.78 s** with one `WriteHeights`; 28 s as one `FillBlock` per column | — |

**H2's terrain fly-over is the flagship's soak, not a bench of its own.** The
bench runner steps a world host, and a world host has no streamer: a
`terrain_stream` scene there would measure a terrain that never streams. The
flagship's soak runs the whole frame -- both streamers, the collider mirror
and the renderer -- over a path that crosses the terrain's cell boundaries
every leg. It passed on the first run. Terrain's cost is ten MiB of peak for
a hundred cells, with no frame over 5.2 ms.

**`WriteHeights` exists because of that tool.** Its first version wrote the
island one `FillBlock` per column, 263,169 calls in 28 seconds. Each box's
vertical sides also turned the whole perimeter into brick columns, a wall to
the world's floor that nobody asked for. One heightmap call writes the same
ground in under a second, as the height layer alone.

**This bench should have been F1's first commit and was its last, and the cost of
that is the honest part of this table.** The plan said it in as many words --
"the first commit of each milestone is the bench, not the feature" -- and skipping
it meant the owner found the performance by using the editor and finding every
stroke slow. What the first measurement reported, on the code as shipped:

| Operation | Before | After | |
|---|---|---|---|
| Generate a 128 m square of ground | **285 ms** | **1.0 ms** | 285x |
| `FillBall`, radius 8 m, one stamp | 2.47 ms | 0.83 ms | 3.0x |
| `FillBall`, radius 4 m, one stamp | 0.62 ms | 0.18 ms | 3.4x |
| `SmoothBall`, radius 4 m, one stamp | 0.15 ms | 0.03 ms | 5.0x |
| Meshing one 32-cubed region | 10.4 ms | 4.1 ms | 2.5x |

**Four causes, and they are the same mistake in four places**: work done per
column that only needed doing per tile, and per sample that only needed doing per
column.

- **A column write cloned and re-hashed its whole tile.** `writeHeight` built a
  5 KB tile and took an xxh3 over it to change four bytes, so a stroke touching
  two hundred columns moved and hashed a megabyte. Now a tile is mutated in place
  where the field alone owns it, cloned where a snapshot still holds it, and its
  digest is computed when somebody asks rather than at every write.
- **Making flat ground went through the general brush.** Carving a box that
  reaches below the world floor makes every column's promotion examination walk
  the whole reserved range -- 256 samples for a result that is one number.
  `fillFlat` says the same thing in the encoding's own terms.
- **Every sample re-resolved its own storage.** Two binary searches and a pair of
  floor-divisions per lattice step, for a column walked dozens of steps deep.
  Resolved once per column now.
- **The mesher sampled each lattice point eight times**, once per cell that
  shares it, and cached its vertex identity in a red-black tree with a
  twenty-four-byte key. The lattice is read once into a flat array and the cache
  is a hash map -- **and the comment claiming R10 required the tree was simply
  wrong**: the container is never iterated, and emission order comes from the
  walk.

Plus one that is not in the table because it is per frame rather than per call:
the loader meshed a slab **64 cells tall** around every tile's surface, a margin
standing in for "whatever bricks reach". Bricks are measured now, and flat ground
meshes 8 cells instead of 64.

**A ragdoll is cheaper than two hundred sockets, and the ratio is the point.**
`sockets200` is 400 bodies in 200 two-body islands and costs 0.58 ms of solver;
`ragdoll10` is 160 bodies in 10 sixteen-body islands and costs 0.26 ms. Per BODY
the ragdoll is slightly dearer -- a sixteen-body island has to be solved together
every iteration, where two hundred two-body islands are two hundred independent
problems the solver splits apart -- and per SCENE it is far cheaper, because ten
characters is a hundred and sixty bodies and two hundred joints is four hundred.

The number worth carrying forward: **a ragdoll costs about 26 microseconds of
solver while it is moving**, so a fight with ten of them is a quarter of a
millisecond and thirty of them would still be under one. An island that has gone
to sleep costs nothing at all, which is why the bench drops them rather than
posing them -- ten settled heaps would have measured the sleep heuristic and
reported it as a ragdoll price.

**The constraint family's three new per-tick passes cost a jointless scene
nothing, and that was the gate.** Attachment resolution, the second `applyScene`
walk and constraint retirement are each a walk over a pool, and every one of
those pools is empty in `physics1k` and `churn10k` -- which is what their rows
above say: 2.00 against M5's 2.02, and 6.98 against M6's 7.32. Both are inside
run-to-run noise on this machine, and neither moved in the direction a new
per-tick walk would move them.


**UI cost is relayout and not draw, and the roadmap asked for the two to be
measured separately.** The draw half is in the obby row above: fifteen draws for
a course, a HUD and a menu, of which one is the whole UI -- the 2D pass is one
draw per scissor RUN, so a panel with a list layout and four labels is one.

The relayout half is a COUNTER rather than a duration, and deliberately: at this
scale a timing assertion measures the clock, and "about zero microseconds" is the
shape of gate that passes while doing nothing. `LayoutStats::solverRuns` counts
solver passes, a `ScreenGui` runs one only when something marked it dirty, and
`luaug_ui_tests` asserts that two further frames over an untouched tree run
**zero**. A static HUD costs nothing per frame, which is the claim, and a
regression that dirtied a tree on every property read would show as a number that
climbs rather than as a millisecond nobody would notice.

**What D031's motion switch costs, and it is `churn10k` that priced it rather
than the scene built for the purpose.** Four hundred platforms with four hundred
transitions over three hundred ticks do not show above the noise: 0.14 ms of
apply, and the six hundred still parts stay in the static layer and cost nothing.
That is the number the fix was designed to produce.

`churn10k` is the one that moved, from **4.96 ms a tick to 7.32**, and the
increase is honest: two thirds of its ten thousand anchored parts are written
every tick, so two thirds of them are now kinematic bodies in the broadphase
layer Jolt re-fits each tick. The step went from 1.23 ms to 3.89 and that is
Jolt doing work it was previously not asked to do -- for parts that were moving
all along and were being teleported. The budget is 16 ms and it is still met.

**Two costs found by measuring rather than by reasoning, both fixed before this
row was written**, and they are the reason this file exists:

  * Writeback went from 0.03 ms to **9.9 ms**. Jolt reports every kinematic body
    as active, always, so the mirror was copying six thousand solver transforms a
    tick back into components whose transforms the SCRIPT owns. A kinematic body
    is not written back now.
  * Apply went from 1.60 ms to **6.96 ms**. The pending-move list was
    deduplicated by scanning it, which is quadratic in the number of kinematic
    writes in a tick; six thousand of them cost four milliseconds. It appends and
    `step` applies in order, so the last write still wins.

The number that would have shown and does not is the one the narrow fix avoided.
`Anchored` meaning kinematic always would have put the six hundred still parts --
and every floor and wall of every real world -- into the layer that is re-fitted
every tick, and `churn10k`'s ten thousand are what that costs at scale. The
hysteresis is what bounds the transition count: inside twelve ticks a platform
transitions once and stays, and `platforms200`'s second population deliberately
writes outside it to price the pathological case.

**The mirror costs about 160 ns per body per tick to decide that nothing
changed**, which is the 1.60 ms `apply` row above over ten thousand static
bodies. Two cheap wins were taken while measuring -- the in-world test is
memoised by parent, and the body records moved from a hash map to a
slot-indexed vector walked in the same ascending order the component pool is,
which together took `apply` from 2.27 ms to 1.60 ms and `writeback` from 0.127
to 0.026. What remains is a dirty-flag design: the mirror rebuilds a
`BodyDesc` per body per tick and compares it, where a scene that changes
nothing should touch nothing. That belongs with M7, which is the milestone that
puts tens of thousands of objects in a world and streams them.

### M4.5 — the renderer, re-measured against a scene it actually reads

Captured with `luaug-host <project> --headless --width=1920 --height=1080
--frames=400 --exit --frame-stats`, median of the 389 frames after ten warm-up
frames, three runs. The spread across the three runs was under 4% (0.4530,
0.4578 and 0.4682 ms).

**Headless, and it matters for reading these numbers.** A windowed run presents
through the swapchain and is pinned to the refresh rate, which would report
16.67 ms for any scene this side of impossible -- a measurement of the monitor.
Headless renders the identical pass list into an offscreen target and never
presents, so what is timed is the frame's own work. What it therefore excludes:
present, vsync, and swapchain acquisition.

**Draw calls and triangles are recorded beside frame time** because the roadmap
asks for the *why* next to the *what*: a frame that got slower with the same
draw count is a different problem from one that got slower because it drew more.

**The M4 rows below are superseded and kept.** Every one of them was measured
against a renderer that never read `Lighting`: the sun stood straight up, fog was
off, and the shadow map was built from a direction nothing in the scene had
asked for. A number recorded against a defect is not a baseline, and M5's "no
regression greater than 10%" clause would have been measured against it. They
stay visible rather than being edited in place, because a superseded measurement
that quietly becomes the current one is how a baseline stops meaning anything.

| Milestone | Scene | Preset | Metric | Value | Budget/Gate |
|---|---|---|---|---|---|
| **M4.5** | `examples/02-meshes` (4 meshes + a transparent pane, 5 materials, sun + 1 point light, shadow map, opaque and blended passes, HDR + tonemap) | `win-msvc-dev` | median frame, 1080p | **0.46 ms** | 16.7 ms — a 60 fps frame |
| M4.5 | `examples/02-meshes` | `win-msvc-dev` | worst frame | 1.79 ms | — |
| M4.5 | `examples/02-meshes` | `win-msvc-dev` | draws / triangles | 10 / 60 | — |
| M4.5 | `examples/02-meshes`, **pinned to 2 cores** | `win-msvc-dev` | median frame, 1080p | 0.42 ms | — |
| M4.5 | `examples/02-meshes`, pinned to 2 cores | `win-msvc-dev` | worst frame | 1.77 ms | — |
| M4.5 | `tests/rendercapture/meshes` (the gate scene) | `win-msvc-dev` | median frame, 1080p | 0.48 ms | — |
| ~~M4~~ | `examples/02-meshes` (4 meshes, 4 materials, sun + 1 point light, shadow map, HDR + tonemap) | `win-msvc-dev` | median frame, 1080p | 0.46 ms | superseded — measured with `Lighting` unreachable |
| ~~M4~~ | `examples/02-meshes` | `win-msvc-dev` | worst frame | 2.06 ms | superseded |
| ~~M4~~ | `examples/02-meshes` | `win-msvc-dev` | draws / triangles | 8 / 48 | superseded |
| ~~M4~~ | `examples/02-meshes`, pinned to 2 cores | `win-msvc-dev` | median frame, 1080p | 0.42 ms | superseded |
| ~~M4~~ | `examples/02-meshes`, pinned to 2 cores | `win-msvc-dev` | worst frame | 6.65 ms | superseded |
| ~~M4~~ | `tests/rendercapture/meshes` (the gate scene, 2 mesh parts) | `win-msvc-dev` | median frame, 1080p | 0.46 ms | superseded |

**What re-measuring actually changed, and it is worth reading before the next
one.** The median did not move: 0.46 ms then, 0.46 ms now, with a whole second
pass and two more draws added. What moved is the **worst frame, from 2.06 ms to
1.79 ms** — and on two cores from 6.65 ms to 1.77 ms, which is a factor of four.
Nothing in this milestone made a frame cheaper; what changed is that the shadow
map's texel grid no longer slides every frame, so the shadow pass's memory
traffic stopped varying with the camera. The lesson is the one the M4 row already
half-stated: at this scene size the median measures fixed cost and the tail
measures whether something is thrashing. **The tail was the number carrying the
defect, and the median never noticed.**

**What the numbers say, and what they do not.** 0.46 ms is 2.8% of a 60 Hz
frame for a scene with a shadow pass, a sky, a forward PBR pass and a tonemap.
That is not a claim that the renderer is fast: it is a claim that **this scene is
too small to measure a renderer with**. Eight draws and forty-eight triangles
exercise every pass and stress none of them, so what this row actually baselines
is the per-frame *fixed* cost -- pipeline binds, uniform uploads, four render
passes at 1080p -- which is the part that does not go away when a scene grows.
The scene that measures the renderer arrives at M7 with something to stream.

**The reduced-CPU row is the interesting one.** Pinned to two cores the median
does not move (0.42 vs 0.46 ms, inside the run-to-run spread) while the worst
frame triples. So this frame is not CPU-bound at all; what two cores cost is
scheduling tail, not throughput. R16's concern -- that a factor of 30 here can
be a factor of 3 on a device without a JIT -- is not yet visible in this scene,
and recording that it is not visible is the point of the row.

**What the numbers say.** The gate scene sits at 0.8% of a 60 Hz frame, with a
factor of 30 to the budget: at M2 the simulation kernel is not what will make a
frame late. The churn scene is the interesting one — 10,000 property writes and
1,000 subscribed signals per tick cost 2.02 ms, which is 12% of a frame, and it
is that low only because a write that does not change the value raises nothing
(M2 brief, Decision 6). That design choice is worth roughly the whole
measurement: a third of the writes in that scene are no-ops by construction.

**What the reload numbers say, and what they do not.** ADR 0024 set 500 ms as a
hard requirement and the measurement comes in at 1.6 ms — a factor of three
hundred. That is not a triumph; it is a statement about what is being measured.
The span is the FrameStart safe point through `PostReload` returning, on a
project whose scripts compile in under a millisecond, and the whole of the
budget's difficulty was always going to be somewhere else: the bytecode cache
ADR 0024 names and M3 did not need to build, and the assets and shaders a real
project reloads alongside its scripts. What these numbers establish is that the
*mechanism* — destroy a world, build another, carry the state bag and the
preserved instances across — costs nothing worth counting. The budget becomes
interesting again in M4, when a reload has meshes and pipelines behind it.

The Linux number being lower than the Windows one is not a portability finding
either: it is a container with no window, no device and a warm page cache.

### M7.5 — the renderer's second half, and the first scene it is CPU-bound on

Captured with `luaug-host <project> --headless --width=1920 --height=1080
--frames=300 --exit --frame-stats`, median of the 289 frames after ten warm-up
frames, three runs. The spread across the three runs of the horde scene was 1.5%
(3.826, 3.776 and 3.832 ms).

**These horde numbers are the RE-MEASURED ones, and the first set was taken on a
frame that was not drawing the scene (D043).** Both instanced shaders assembled
the per-instance model matrix transposed, so every instanced vertex left the
frustum: the field of two thousand enemies rendered as an empty floor with only
the player on it. The measurement said 3.72 ms at 22 draw calls, which is the
number a working instanced path also gives -- twenty-four thousand triangles at
1080p are not what this frame is spending its time on, so removing all of them
moved the median by 3%. **A performance number cannot tell you whether the frame
contained the scene**, and neither could the draw-call gate this milestone was
asked for, nor the command-stream goldens, which were all correct. The check that
can is `screenshot_gate_instanced`, and it is standing now.

**Two numbers per row now, and the second one is the point.** `DrawCalls` and
`VisibleObjects` were the same number until this milestone: a run of objects that
share a mesh and a material is one call now, so "how many objects are visible"
and "how many calls were issued" stopped being the same question. The roadmap's
gate for the instanced path is exactly the two of them side by side, and a row
where they are equal is a row where instancing did nothing.

**The horde, and the control that makes it a measurement.**
`tests/perf/horde` is two thousand enemies sharing one mesh and one material,
chasing a circling player, positions written from Luau every tick, under a
shadow-casting sun. It was measured outside the repository at M2 and is committed
now, because M7.5 is where the answer became a number something defends.

| Horde, 2,000 enemies | Frame | Draw calls | Visible objects |
|---|---|---|---|
| Instanced, as shipped | **3.83 ms** | **22** | 4,002 |
| Same build, instanced path disabled | 31.45 ms | 15,390 | 4,002 |
| Instanced, pinned to two cores | 4.04 ms | 22 | 4,002 |

**The control row is also the correctness check now.** With D043 fixed, the
instanced frame and the same frame with the instanced path disabled are
BYTE-IDENTICAL at 960x540 -- zero differing pixels. Before the fix, 87% of the
frame differed. That comparison costs one extra render and is the only instrument
that could have told these two rows apart, because everything else about them --
the command stream, the counters, the timings -- was consistent with both.

The control row is the same binary with one constant raised past any real run --
the same sort, the same passes, the same shaders. That is what makes it a control
rather than a comparison against history, which would have been comparing two
different renderers.

**The reduced-CPU row says this frame is nearly single-threaded.** Two cores cost
about a tenth of it rather than half, so what this workload wants is one fast
core rather than many. R16's concern is about the low end, and the answer this
scene gives is specific: a device with fewer cores does not suffer here, and a
device with a slower core suffers proportionally.

**Per-feature cost, each measured by disabling that pass alone and rebuilding.**
The whole M7.5 chain is about a millisecond at 1080p on the reference machine.

| Feature | Cost | Against |
|---|---|---|
| Three extra shadow cascades | 0.22 - 0.28 ms | One cascade, which is what M4 shipped |
| Depth prepass | 0.20 - 0.22 ms | No prepass; it is a second geometry submission and it is what makes depth samplable |
| FXAA | 0.19 ms | The tonemap resolving straight to the swapchain |
| Bloom | at the noise floor | Nine passes over five levels |
| Automatic exposure | 0.09 - 0.18 ms | Three passes, the last of them 1×1 |
| Ambient occlusion | at the noise floor | Sixteen taps at half resolution plus two blur passes |
| **Whole chain** | **about 0.9 ms** | |

**The measurement's own noise floor is about 0.08 ms**, and the ranges above are
the two independent sweeps rather than an average of them. The sweep was run
twice, on different builds of the same tree, and the three large rows agreed to
within 0.06 ms while the three small ones did not -- bloom came out at 0.13 ms
once and at MINUS 0.02 ms the other time, which is the measurement saying it
cannot resolve that pass rather than the pass being free. The two smallest rows are at the edge of what
this method can resolve, and they are reported as such rather than to three
decimals of false precision. A GPU timestamp query would resolve them properly
and the RHI has none; adding one is an ADR the milestone that needs it should
write.

| Milestone | Scene | Preset | Metric | Value | Budget/Gate |
|---|---|---|---|---|---|
| M7.5 | `tests/perf/horde` (2,000 enemies, one mesh, one material, 1080p) | `win-msvc-dev` | median frame | **3.83 ms** | re-measured after D043 |
| M7.5 | `tests/perf/horde` | `win-msvc-dev` | draw calls / visible objects | **22 / 4,002** | not equal, which is the gate |
| M7.5 | `tests/perf/horde`, instanced path disabled | `win-msvc-dev` | median frame | 31.45 ms | the control |
| M7.5 | `tests/perf/horde`, instanced vs. disabled | `win-msvc-dev` | differing pixels | **0** | the check the counters could not make |
| M7.5 | `tests/perf/horde`, two cores | `win-msvc-dev` | median frame | 4.04 ms | the reduced-CPU row |
| M7.5 | `examples/02-meshes` (1080p) | `win-msvc-dev` | median frame | **1.51 ms** | — |
| M7.5 | `examples/02-meshes` with a frozen sun | `win-msvc-dev` | median frame | 1.41 ms | isolates the environment prefilter |
| M7.5 | `examples/02-meshes` | `win-msvc-dev` | draw calls / visible objects | 61 / 11 | eleven objects across six passes |

**The shadow kernel went from nine taps to twenty-five after the milestone**, so
a shadow edge has a penumbra wide enough to hide the texel grid it was
rasterised on (D044). It costs 0.54 ms at 1080p in `examples/02-meshes` -- 1.51
to 2.01 -- which is a third of that frame and the largest single cost the
renderer has taken since M7.5 closed. **The tap count is the dial** if that is
judged too much: nine could not span the penumbra without banding, which is the
whole reason for the change, but sixteen might.

**`examples/02-meshes` went from 0.46 ms at M4.5 to 1.51 ms**, and that is not a
regression in the sense the 10% clause means. M4.5's frame was a shadow pass, a
sky, a forward pass and a tonemap; this one is a shadow ATLAS of four cascades, a
depth prepass, ambient occlusion and two blurs, the forward pass, three exposure
passes, nine bloom passes, a tonemap and an anti-aliasing resolve. Eleven objects
and seventy-two triangles do not move that number -- **the passes do**, and the
per-feature table above is what says which.

**It was 2.98 ms until the measurement was read properly**, and the story is
worth the paragraph because the diagnosis is the M2 horde's diagnosis exactly.
The frame cost 2.96 ms at 320x180 and 2.96 ms at 1920x1080 -- identical, which is
what says the cost is not fragments. It was the CPU environment prefilter, which
this milestone had priced at zero:

| | `examples/02-meshes`, 1080p |
|---|---|
| As first written | 2.98 ms |
| The same scene with its sun FROZEN | 1.41 ms |
| Shipped | **1.51 ms** |

The frozen-sun row is the control: it isolates the prefilter from everything else
the milestone added. Three things closed the gap and all three were found by that
one comparison. `evaluateSky` called `pow` twice per evaluation and a full
prefilter is a quarter of a million evaluations. The rebuild threshold was half a
degree, which in a ninety-second day meant the chain never got ahead of the sun.
And **the job pool M7 built had no caller at all** -- nothing in the engine ever
called `jobs::init`, so every `parallelFor` in it had been taking the documented
serial path.

What remains is about 0.10 ms a frame for an environment that follows a moving
sun, against 1.41 ms for a frozen one.

The comparison the 10% clause is actually for is the horde row, and there is no
M7 number to compare it against because the scene did not exist in the repository
then. The control row is what stands in for one: same build, same scene, one
constant.

### M8 — the flagship, and the absolute targets binding

`examples/10-open-world`: a character on streamed terrain, 289 chunks of which
about 4,300 instances are resident, a moving sun, a HUD, physics, and the M7.5
render chain. Measured with `--headless --width=1920 --height=1080` and the
demo's own autopilot, which walks a circuit for twenty-five seconds and flies a
wider one for twenty-five.

**The ten-minute soak, which is the gate.**

| | |
|---|---|
| Frames | 35,939 measured (36,000 run, 60 warm-up dropped) |
| Median | **5.35 ms** — 187 fps |
| p99 | **8.79 ms** — 114 fps |
| Worst | 17.23 ms, **one frame in 35,939** |
| Frames over 16.7 ms | **1** |
| Streaming hitches over 33 ms | **0** (worst streaming step 3.34 ms) |
| Peak resident | 168 MiB, and the final figure is the same number |
| Instances | 4,354 early, 4,285 late — flat, which is what a streamed world does |
| Draws / visible objects | **72 / 1,032** |

Sixty frames per second at 1080p is met with three times the headroom at the
median, and the tail is a single frame rather than a distribution with a shoulder
in it.

**Per-feature cost, from a sweep of the same scene at 6,000 frames.** Each row is
the same binary with one flag, so these are differences rather than separate
builds — which is what `--quality`, `--shadow-cascades` and the rest exist for
(ADR 0044).

| Variant | Median | p99 | Worst | Frames over 16.7 ms |
|---|---|---|---|---|
| Baseline (`high`) | 6.25 ms | 11.33 ms | 51.87 ms | 8 |
| `--shadow-cascades=0` | 4.79 ms | 8.86 ms | 35.72 ms | 3 |
| `--no-bloom --no-ambient-occlusion --no-anti-aliasing` | 4.60 ms | 7.90 ms | 17.16 ms | 1 |
| `--no-auto-exposure` | 4.59 ms | 8.16 ms | 10.65 ms | 0 |
| `--quality=low` | 4.33 ms | 7.83 ms | 10.17 ms | 0 |
| `--render-scale=0.5` | 4.55 ms | 8.07 ms | 12.14 ms | 0 |
| **Baseline again, last** | **4.83 ms** | **8.37 ms** | 13.04 ms | 0 |

**Read the first and last rows together before reading any of the ones between:
they are the same run, and they differ by 23%.** A sequence of GPU runs is not a
sequence of independent measurements — the first one gets a cool card at its
boost clock and everything after it does not. An earlier version of this sweep,
run without a warm-up, reported a 2.9 ms baseline and then showed `--quality=low`
as *slower* than `high`, which is the shape a measurement takes when the variable
is the order rather than the flag.

**So the per-feature numbers above are worth about half a millisecond each and
should be read as "roughly a millisecond and a half of shadows, a millisecond and
a half of post".** The way to get better ones is to interleave the variants and
repeat, which is what the next person who needs a real number should do rather
than trusting this table to more precision than it has.

**The one frame that misses 60 fps used to be sixty of them.** The demo's
autopilot originally began its flight by placing the character nine hundred
metres away, which replaces the entire resident set in one tick; the burst of
materialisation that followed was the only thing in a ten-minute run over 33 ms.
It spirals out over six seconds now. A player never teleports, and a soak that
did was measuring something no player would ever do.

**Shadow distance is paid for in resolution, everywhere, at once** (D052), and
this is the table to look at before choosing one. A cascade's texel is its box
divided by its tile, and the far cascade's box is set by the frustum's
cross-section at the shadow distance — so the last cascade is where the whole
choice shows up. Measured on the flagship's own camera (70 degrees, 16:9), by
dumping each fitted box:

| Tile | Distance | Far cascade starts | Far cascade texel |
|---|---|---|---|
| 1024 | 120 m (the `high` preset) | 31 m | 0.35 m |
| 1024 | 180 m (the flagship's first answer) | 44 m | **0.52 m** |
| 2048 | 220 m (`ultra`, before D052) | 44 m | 0.32 m |
| 2048 | 160 m (`ultra`, now) | 39 m | 0.23 m |
| 2048 | 140 m (the flagship, now) | 35 m | **0.20 m** |

Two things fall out of it. **Doubling the tile and shortening the distance is
free**: 2.81 ms median against 2.89 ms for the pair it replaced, over 2,400
frames at 1080p run twice each in alternating order — a larger tile costs fill
and a shorter distance hands it back in culled casters. And **`ultra` was
spending its four-times atlas on range rather than density**, which is how a
preset above `high` ended up drawing a fifty-metre shadow on a grid nine per
cent finer than the preset below it.

**The diffuse ambient costs 0.2 ms a frame and buys a world that does not
pulse** (D053). Re-projecting the sky onto nine coefficients four times as often
as the specular chain rebuilds, and walking the shader's copy towards the result
rather than handing it over whole, moved the flagship's median from 2.85 ms to
3.07 ms over four alternating 2,400-frame runs. What it bought is measured on a
still scene under a moving sun: the frame-to-frame change went from a mean of
42,577 with a 297,810 spike to a mean of 8,039 with a 10,250 one — peak over
mean 6.99 to 1.27. Baking it exactly, every frame, costs 0.55 ms instead of
0.2 and measures no better.

**A shadow edge steps by one texel whatever else is true, and what changes with
distance is how often** (D054). Measured on a still probe under the flagship's
clock: the near cascade's lattice drifts about half a texel per frame near noon,
so its edge moves every other frame and reads as motion; the far cascade drifts
a twentieth of a texel and holds still for twenty-three frames before jumping,
which reads as a jump. Widening the texel-band floor from two to six and
rotating the 5x5 kernel per pixel — measured with the casters at forty-five
metres, consecutive frames subtracted:

| | Pixels changing >2 levels | >4 levels | Worst single change |
|---|---|---|---|
| Floor 3 texels, fixed grid | 227 | 62 | 37 |
| Floor 4, rotated | 117 | 20 | 35 |
| **Floor 6, rotated** | **80** | **14** | **14** |

And then the taps themselves, because a filter returns a COUNT of them and the
smallest change it can express is one: with twenty-five, one texel of the map
flipping moved a shadow edge 3.58 pixels; with forty-nine over the same radius
it moves 0.75. **Neither change is measurable in a frame** — the flagship's
median was 2.98 ms and 2.99 ms over two 2,400-frame runs with the 7x7 kernel,
against 3.09 with the 5x5 one, which is inside the run-to-run spread this page
warns about. At 1080p this scene is not bound by shadow taps.

**Render interpolation costs nothing measurable** (D047). Forcing `alpha` to zero
on the same scene moves the median by less than the run-to-run spread, because
almost every part in an open world is static and the comparison in front of the
slerp is two `CFrameD` equality tests.

**Why the budgets are so loose.** They are catastrophe detectors, not
instruments. A CI runner's speed varies by more than the regression anyone would
want to catch, so a budget tight enough to notice 10% would be red every other
week and would train everyone to ignore it. The threshold catches the change that
made a tick ten times slower; this table is where a 10% regression is actually
visible, and comparing against it is a human's job at each milestone gate.

Standing absolute targets, **bound at M8** on the reference machine. Each is
followed by what it measured, so a later regression is a comparison rather than
a judgement:
- `examples/10-open-world`: 60 fps at 1080p; zero streaming hitches > 33 ms in
  the scripted fly-through; 10-minute soak with bounded memory delta and zero
  crashes. **Met**: median 5.35 ms, p99 8.79 ms, one frame of 35,939 over
  16.7 ms, zero hitches, peak resident equal to final resident at 168 MiB.
- Hot reload (`luaug dev`): < 500 ms from file save to behavior change
  (ADR 0024), measured by the M3 E2E test.
- Sim: 500-instance scripted scene (M2 benchmark) and 1,000 active physics
  bodies (M5 benchmark) within their recorded budgets; the
  10k-parts/1k-listeners property-churn benchmark within its CI threshold.
- Script GC: ≤ 1 ms GC step at 60 fps under the M7 streaming scene load.

### E4 — the editor's Explorer, measured as work rather than as a clock

**A number the machine cannot move.** Everything above this section is
milliseconds on the reference machine, and the methodology at the top of this
page spends four paragraphs on how easily a millisecond lies. This one does not
need them: what the Explorer costs per frame is the number of instances its walk
visits, and that number is a property of the algorithm — the same on a reference
desktop, on a busy laptop and in a container.

The panel drew a preorder over **every instance in the world** every frame, and
then walked that list a second time to drop everything under a closed node. The
*drawing* was already virtualised — `ImGuiListClipper` over the visible rows —
which is exactly why nothing ever showed this in a profile: the cost was in
deciding what to draw, not in drawing it.

Measured by `inspector_tests.cpp`, which is why the numbers are reproducible
rather than recorded:

| World | Rows on screen | Instances visited, before | After |
|---|---|---|---|
| 4 branches × 50 leaves (**205** instances) | 4 | 205 | **5** |
| 4 branches × 500 leaves (**2,005** instances) | 4 | 2,005 | **5** |
| Same, with one branch opened | 54 | 2,005 | 55 |

**The two worlds are an order of magnitude apart and the walk costs the same**,
which is the assertion the test makes — equality rather than a small bound, for
the reason E5's partition peak had to: a bound that is merely small passes while
the defect is still there. Break-verified by making the walk descend
unconditionally, which reports 205 and 2,005 again.

What this does not say is how many milliseconds it was, and that is deliberate:
the walk allocated nothing per node and the panel was never visibly slow, so a
frame time would have shown a regression only on a world large enough to make it
one. The cost was linear in a world an editor is expected to open, and now it is
not.

## Jolt on a fixed thread pool (S6.10, ADR 0064)

The M5 roadmap said "single-threaded first; Jolt's job system wired to the engine
job system when M7 lands it", and asked whether the recorded hashes would survive
it. M7 landed and the question was never asked. It is the kind that is answered
by running it.

Same machine, same build, `win-msvc-dev`, `--bench-repeats=1`. **The A/B rather
than only the new number**, because the interesting fact is the delta and it
would be unrecoverable from a table of absolutes:

| Bench | Measure | `JobSystemSingleThreaded` | `JobSystemThreadPool`, 4 threads |
|---|---|---|---|
| `physics1k` | mean sim tick | 2.013 ms | **0.904 ms** |
| `physics1k` | physics step | 1.760 ms | **0.651 ms** |
| `physics1k` | worst sim tick | 4.563 ms | **1.871 ms** |
| `churn10k` | mean sim tick | 7.107 ms | **5.224 ms** |
| `churn10k` | physics step | 3.725 ms | **1.853 ms** |
| `churn10k` | worst sim tick | 174.17 ms | **40.03 ms** |
| `ragdoll10` | physics step | 0.264 ms | **0.131 ms** |

**The worst tick is the number worth reading twice.** `churn10k`'s fell from 174
ms to 40 ms, which is the difference between a visible stall and a dropped frame.

**And the hashes survived**, which was the open question. `tests/determinism/churn`
— ten thousand ticks, and the one whose parts Jolt actually simulates —
reproduced its committed hash `d3dd9b68722aa0fa` on Tier 1 and Tier 2, unchanged.
No trace was re-recorded for this change.

**The thread count is part of the hash.** Jolt is deterministic across runs
provided the count is the same, so `kPhysicsThreads` is a physics constant in the
way gravity is: changing it means re-recording every trace. That is also why the
solver does not run on the engine job pool, which sizes itself from the machine —
a trace recorded here would stop being reproducible on a machine with a different
core count, which is the one property a committed trace cannot lose.

## Terrain colliders (F1 A3, ADR 0066)

Measured 2026-08-27 on the reference machine, by
`luaug_physics_tests --test-case="what a terrain collider costs" --no-skip`. The
case is skipped by default: a number that varies with the machine must not gate
anything, and a measurement nobody can reproduce on their own hardware is not a
measurement.

**Nothing in this repository measured a shape BUILD before this.** `physics1k`
and `churn10k` move and re-target bodies without ever reshaping one, so every
cost figure under ADR 0066 — including the one the whole hybrid rests on — was a
guess until now.

| shape | operation | 128² samples | 256² samples |
|---|---|---|---|
| height field | create | 0.516 ms | 1.829 ms |
| height field | **`SetHeights`, 16² rectangle** | **0.0078 ms** | **0.0081 ms** |
| height field | rebuild through `updateBody` | 0.457 ms | 1.760 ms |

**`SetHeights` is 58× cheaper than a rebuild at 128² and 217× at 256², and the
ratio grows because the two scale differently.** An in-place edit is O(the
rectangle edited) — 0.0078 ms against 0.0081 ms while the field quadrupled — and
a rebuild is O(the field). That is ADR 0066's central claim, and it is the
difference between a brush that drags at any framerate and one that gets slower
as the world gets more detailed.

| shape | triangles | create |
|---|---|---|
| triangle mesh | 7,938 | 2.74 ms |
| triangle mesh | 32,258 | 12.11 ms |

**The triangle-mesh number is the one that changes a design decision.** A cell
that gives up on the height encoding and converts to voxel bricks costs about
twelve milliseconds to collide — most of a frame at 60 Hz — so **a bricked cell
cannot have its collider rebuilt synchronously during a drag**, while a
height-encoded one costs eight microseconds and can. The gap across that boundary
is roughly 1,500×.

Two consequences, both for F1 rather than for this file. The give-up width that
decides which side of that gap a cell lands on has to be a tunable rather than a
constant, which the A5 slope survey concluded independently. And a bricked cell's
collider needs a budgeted, off-frame rebuild path that a height-encoded one does
not — so "the failure mode is the baseline" is true of correctness and not of
cost.

## Jolt's cross-platform switch (ADR 0074)

Measured 2026-09-23, same machine, same build, `win-msvc-dev`,
`--bench-repeats=3`, two runs each way, the mean of the two. The A/B, for the
reason the thread-pool table gives one.

| Bench | Measure | `CROSS_PLATFORM_DETERMINISTIC` off | on |
|---|---|---|---|
| `physics1k` | physics step | 0.645 ms | **0.649 ms** |
| `churn10k` | physics step | 1.808 ms | **1.809 ms** |
| `churn10k` | mean sim tick | 5.216 ms | **5.229 ms** |
| `ragdoll10` | physics step | 0.136 ms | **0.134 ms** |
| `platforms200` | physics step | 0.108 ms | **0.108 ms** |

**Under 1% where it shows at all**, against upstream's documented 8%. What the
switch bought is in the ADR: the `character` scenario's Windows and Linux traces
became byte-identical.

**And `churn10k` is not a problem this table has to solve.** Its 5.2 ms is
ten thousand parts, two thirds of them moved from Luau every tick through
property writes that raise signals to a thousand listeners -- about 3 ms of
physics (apply 0.46, step 1.81, write-back 0.79) and about 2 ms of scripted
churn, which is some 300 ns a write. The apply already calls the backend only
for what changed; what it spends is building and comparing a description per
body, about 46 ns each, and a dirty flag would save a fraction of half a
millisecond at the price of a second source of truth. Engines that move ten thousand scripted objects a
frame land in the same few milliseconds; the ones that do much better do it by
not running a script per object, which is a game's decision and not a kernel's.

## A user's game, and instancing by colour (D182–D184)

Measured 2026-09-24 on the reference machine, from the `player`-and-`editor`
package (`scripts/package.ps1`) -- **the GPU debug layer off (D183)**, which is
the first time a windowed number here was taken without it. The scenes: the
stress harness around the owner's friend's SNAKE game (8 snakes of 250
segments, every segment its own `Color`), `examples/11-ocean`, and
`tests/bench/instances500`. `--frames=1810 --exit --frame-stats`, median of the
measured frames.

**A/B, interleaved**: A is the package without D184, B with it, built from the
same tree; four rounds A,B,A,B headless, two windowed runs each. The machine was
shared with other work that evening (a Python process at a steady load, chat
clients), so the rounds were interleaved to put the noise on both sides; the
earlier before/after pairs taken an hour apart disagreed by more than the
effect, which is why this table is the A/B and not those.

| Scene | Draws A → B | Headless A | Headless B | Windowed A | Windowed B |
|---|---|---|---|---|---|
| SNAKE 8×250 (2013 objects) | 756 → **4** | 3.78 ms | 3.88 ms | 8.84 ms | 9.23 ms |
| `11-ocean` (329 objects) | 119 → **17** | 3.35 ms | 3.44 ms | — | — |
| `instances500` (no camera) | 0 → 0 | 0.87 ms | 0.85 ms | — | — |

**The draw count is what D184 was for, and it fell by 150× and 7×.** The frame
time did not follow. Headless the two are within the rounds' own spread (A's
SNAKE medians ran 3.49–4.00 ms); windowed, B is about 4% slower in both pairs.
The likely reason, and it is a hypothesis: an instanced batch is culled WHOLE
(ADR 0043), and a run of two thousand segments across the whole map is now one
batch that every shadow cascade draws in full, where 756 draws were each
rejected by the cascades they missed. The follow-up is to split a run into
spatial pieces before it becomes a batch, and to measure that against this row.

**A windowed frame on this machine is its presentation, not its work.**
`instances500` has no camera, draws nothing, and still sits at ~8.6 ms
windowed, where the same run headless is 0.85 ms: the swap chain paces the loop.
Every windowed number in this file before today was also taken with the D3D12
debug layer on (D183) -- with it, the SNAKE row measured 8.70 ms against 8.63
without, so the layer's CPU cost here is small; what made it matter was that a
message from it was fatal.

**D182 held**: the windowed SNAKE repro that died with EXECUTION ERROR #646 ran
three times to the end with the layer asked for (`--gpu-debug`), 756 draws a
frame, before D184 took the draws away.

## The look of a world (ADR 0096)

Every effect at 1920x1080 on the reference machine, **the packaged build**
(the release profile, no D3D12 debug layer), in `tests/look` -- the valley at
17:00 with each effect alone -- by `tools/repo/look_captures.py`'s scene.

**How a GPU cost was measured here, since a headless frame's own time is not
one.** A headless frame does not wait for the GPU: `--frame-stats` gave 0.16 ms
a frame for this scene at 1080p with or without any effect, which is the CPU
submitting and nothing else. A run that ends in a screenshot does wait -- the
readback needs every frame before it -- so each variant ran 200 frames and
2,200 frames, and the difference in whole-run wall time over the 2,000 frames
between is what a frame costs once the GPU is the limit; the start-up, the
same for both, cancels. Three rounds each, median:

| Effect | Variant | Cost at 1080p | Budget (ADR 0096) |
|---|---|---|---|
| none | -- | 0.615 ms a frame (the base) | -- |
| `BloomEffect` | `Intensity` 4, `Size` 48, `Threshold` 0.6 | -0.02 ms | the engine's own bloom |
| `ColorCorrectionEffect` | warm, contrast and saturation | +0.04 ms | ~0 |
| `BlurEffect` | `Size` 4 | +0.10 ms | 0.3 ms |
| `BlurEffect` | `Size` 80 | +0.04 ms | 0.3 ms |
| `DepthOfFieldEffect` | focus at 20 m | +0.08 ms | 0.6 ms |
| `SunRaysEffect` | `Intensity` 0.8, `Spread` 1 | +0.08 ms | 0.4 ms |
| `Atmosphere` | defaults | -0.03 ms | 0.2 ms |
| `Atmosphere` | `Density` 0.6, `Glare` 3, `Haze` 2 | -0.01 ms | 0.2 ms |
| `Sky` | six 1024-texel pictures | +0.09 ms | -- |
| `Sky` | `CloudCover` 0.5 | +0.02 ms | -- |

**Every effect is inside its budget, and every one is at this method's noise
floor**: a round of one variant spread by up to 0.15 ms, and the base itself
by 0.055 ms. The honest reading is "under a tenth of a millisecond each on
this machine", not the second decimal -- the same floor M7.5's table records
for bloom and ambient occlusion, and for the same reason: the RHI has no
timestamp query (ADR 0037), and a pass this cheap is below what a wall clock
around a whole run resolves. A small blur costing more than a wide one is that
floor, and it is also true to the design: a blur of 4 runs its Gaussian at
full resolution, and one of 80 a thirty-second of the frame down.

| `Sky` bake | 1024-texel faces into a 2048-texel picture, sixteen bands | Budget |
|---|---|---|
| Packaged build, five runs | 24.5 to 30.4 ms, **off the frame thread** | 50 ms |
| `dev` build, three runs | 53 to 72 ms | -- |

The first measurement, with eight bands and a radiance picture that read every
texel of its source, was 39.5 to 48.8 ms: inside the budget with no room. Two
changes bought the margin back. Sixteen bands keep every worker busy to the end
instead of leaving the last one alone with an eighth of the picture; and the
radiance picture -- 64 texels, each the average of a 32-by-32 block -- reads one
texel in four each way, which for a blur that wide is the same average, and
the prefilter it feeds blurs it further still. The five runs above were taken with the
Linux gate stage building in Docker alongside, so they are not an idle machine's.

**CityBench against Godot, the owner's benchmark** (`luaug-playground/benchmark`),
before Stage 1 and after Stage 10, three rounds each at raised priority. The
city has none of these instances, so this is the check that the renderer's
default path lost nothing:

| | LuauG median | Godot median |
|---|---|---|
| Before (package of `905e0c5a`) | 1.58, 1.63, 1.55 ms | 2.19, 2.31, 2.13 ms |
| After (package of `4ba0d790`) | 1.51, 1.54, 1.50 ms | 1.90, 2.02, 1.91 ms |


## User surface shaders (ADR 0091)

What a surface shader costs besides its draws: the frame that first draws it
creates its ten shaders and every pipeline on the render thread, and a built
game carries its bytecode for three backends. Measured 2026-09-26 on
`win-msvc-dev` (D3D12, the GPU debug layer on, as in every dev build), with a
warm compiler cache, `luaug-host <example> --headless --frames=90`; three runs.

| Surface | Shaders and pipelines created | In the pack (SPIR-V + DXIL + MSL) |
|---|---|---|
| `11-ocean` / `ocean.surface.hlsl` | 5.6, 5.5, 5.0 ms -- **5.5 ms** | 423 196 bytes |
| `23-surfaces` / `flag.surface.hlsl` | 4.6, 4.2, 4.1 ms -- **4.2 ms** | 402 243 bytes |
| `23-surfaces` / `dissolve.surface.hlsl` | 3.5, 3.4, 3.3 ms -- **3.4 ms** | 425 542 bytes |
| `23-surfaces` / `glass.surface.hlsl` | 3.1, 3.0, 3.3 ms -- **3.1 ms** | 385 159 bytes |

Once per surface per run -- the renderer keeps the pipelines until the shader
changes -- so this is a hitch of a few milliseconds the first time a surface
comes on screen, and nothing after. Compiling one is separate and never on a
frame: a cold compile of the ocean is 882 ms on the editor's worker, and a warm
one reads the cache in about 1 ms (`examples/11-ocean/README.md`).

## Foliage (ADR 0116)

What a field of grass to the horizon costs, and the budget it is held to.
`tests/perf/foliage` is a 1,024 m square of flat terrain with one layer at ten
instances a square metre drawn to 250 m -- about two million instances resident
around the camera, all of them tested by the GPU cull every frame -- in a gusting
wind, seen from a standing height while the camera turns. The control is the same
scene with the layer's `Enabled` off. Measured 2026-09-28 on `win-msvc-dev`
(D3D12, the GPU debug layer on), `engine-host tests/perf/foliage --headless
--width=1920 --height=1080 --frames=300 --exit --frame-stats`; three runs each.

| 1080p | Median frame | Worst frame | Draw calls |
|---|---|---|---|
| Layer off (the terrain alone) | 2.25, 2.32, 2.31 ms -- **2.31 ms** | 15.7 ms | 308 |
| Layer on | 3.79, 3.89, 3.81 ms -- **3.81 ms** | 30.3 ms | 324 |
| **Foliage** | **+1.50 ms** | | **+16** |

**The budget is two milliseconds a frame for a field to the horizon at 1080p on
the reference machine**, measured this way; the first measurement leaves a
quarter of it spare. The sixteen draws are the cull's indirect draws -- one per
section of each level of each mesh, whatever the number of instances -- and they
do not grow with the field.

The worst frame is the first frames' one-time work, not growth: halving the
tiles grown per frame (8 to 4) left it where it was (31.3 and 32.7 ms), and the
layer-off control has a worst frame of its own. A soak of a walk through a
streamed field is the check that is still owed, as M8's was for the flagship.

## A replica stepping its island again (ADR 0133)

What predicting the crates near a player's character costs the client. The
scene is the crate-push test in `engine/app/tests/network_session_tests.cpp`:
a character and three loose 2 m crates, pushed for ten seconds (661 ticks at
60 Hz) over the memory transport, clean and at three seeds of 2% loss, 5%
reordering and two polls of jitter. Measured 2026-09-29 on `win-msvc-dev`,
three runs of the four; the runs agreed to within 10%, and the medians are
below.

| Each tick | Keeping the island (character + 3 crates) |
|---|---|
| Median of 661 | **10.4 µs** (6.9 ms in all) |

| Per run | Re-simulations | Ticks stepped again | Time | Per tick stepped again |
|---|---|---|---|---|
| Clean | 13 | 26 | 1.85 ms | 71 µs |
| Loss, seed 9 | 14 | 52 | 3.31 ms | 64 µs |
| Loss, seed 5 | 15 | 56 | 3.85 ms | 69 µs |
| Loss, seed 21 | 15 | 57 | 3.66 ms | 64 µs |

**About seventy microseconds a tick stepped again, restore included, and under
two re-simulations a second while pushing.** That is 0.3 ms of every second of
play here, and keeping the island is 0.6 ms more. The bound is a re-simulation
on every snapshot (30 a second at this rate) of about eight ticks each: 240
ticks a second, 17 ms of every second, 1.7% of one core. The island includes
the solver's contact cache: without it a replay was not the live step, and the
cost of keeping it doubled the per-tick figure (it was 5.4 µs). Most of the re-simulations are not corrections: a replica
steps the island again whenever it disagrees with the authority by 10 µm or
more, because a disagreement that small and left alone grew to a centimetre the
next time two crates met.

## One drawn position per instance (ADR 0134)

What routing every visual consumer through `render::DrawPoses` costs a frame.
The package before the change (`a6cf7b93`) against the package with it, the
same `player` profile, `engine-host <example> --headless --frames=900 --exit
--frame-stats --width=1920 --height=1080`, three runs each, measured
2026-09-29 on a quiet machine; the "before" runs were taken twice, an hour
apart, and are both shown.

| Median frame, 1080p | Before | After |
|---|---|---|
| `10-open-world` | 2.52, 2.50 ms | 2.54 ms |
| `20-platformer` | 0.121, 0.119 ms | 0.122 ms |
| `28-arcade` | 0.566, 0.564 ms | 0.572 ms |

**Within the spread of the two "before" sets, or a hair over it: about 1% on
the open world.** A headless run draws at its tick and passes no history, so
this is the cost of the calls and not of the between-tick answers, which are
kept for the frame; the windowed frame, which does interpolate, is held at the
display's 120 Hz on this machine and has no uncapped mode to measure it with.
The interpolation itself is what the extraction already did before this
change -- the same function, once per instance.

## The audit's performance band (2026-09-29)

The P1 performance items of the audit of 2026-09-28
([`audit-2026-09-28.md`](briefs/audit-2026-09-28.md)). The package before the
band (`a6cf7b93`) against the package with it, the `player` profile,
`engine-host <project> --headless --exit --frame-stats --width=1920
--height=1080`, three runs each, measured 2026-09-29. `perf_colors` is ten
thousand plain parts in ten thousand colours and `perf_bodies` five thousand
loose bodies on a floor, 300 frames; the examples ran 900 frames and
`14-voxels` 1,800.

| 1080p, median / worst frame | Before | After |
|---|---|---|
| `perf_colors` | 68.3 / 77-82 ms | 5.6 / 7.4-8.2 ms |
| `perf_bodies` | 1.23-1.34 / 9.6-11.7 ms | 1.41-1.55 / 6.8-8.1 ms |
| `10-open-world` | 2.50 / -- ms | 2.31 / 22-31 ms |
| `20-platformer` | 0.12 ms | 0.17 ms |
| `28-arcade` | 0.565 ms | 0.92 ms |
| `14-voxels` | 0.29 / 37 ms, ~8% of frames 8-17 ms | 0.57 / 6.3-8.0 ms, p95 2.6 ms |

**The colours are the material index** (R4): ten thousand colours were fifty
million comparisons a frame. **The headless medians of the small scenes went
up, and that is the fix and not a cost** (R8): a run with no display queued
frames for the GPU without a bound, which is what grew its memory by a
gigabyte a second (11-ocean now stays at 168 MiB where it reached 2.3 GiB in
four seconds), and a median taken that way measured the CPU racing ahead. It
now waits on the oldest of three frames in flight, and the new phase line puts
the difference in "waiting on the GPU": 0.35 ms of 28-arcade's 0.92.

**14-voxels** is now attributed: its slow frames are CPU drawing (p95 7.4 ms),
not simulation (p95 1.1 ms). The cause was that one block mined remeshed the
27 chunks round it -- their key digested each neighbour whole, where the mesh
reads a one-block shell -- and the physics mirror rebuilt the 7 face
neighbours' colliders the same way. Both now check the shell before acting:
with the band but without that fix, the p95 was 11.1 ms, of which the drawing
7.4; with it, 2.6 ms and 1.8.

## Water (ADR 0118)

What the water costs to draw and to float things in. Measured 2026-09-29 on
`win-msvc-dev` (D3D12, the GPU debug layer on), `engine-host <project>
--headless --width=1920 --height=1080 --frames=300 --exit --frame-stats`; three
runs each, the medians below.

**The ocean example, before and after it moved onto `Water`.** Before: nine
grids of 256 quads placed from Luau, a surface shader of its own, a boat
placed on the surface four times a tick. After: one `Water`, three rings of the
shared grid drawn by the engine's water surface, and a simulated hull.

| `examples/11-ocean` | Median frame | Simulation | Draws | Triangles |
|---|---|---|---|---|
| Before (its own shader, a driven boat) | 0.64, 0.69, 0.68 ms -- **0.68 ms** | 0.106 ms | 39 | 786 752 |
| After (`Water`, a simulated hull) | 0.72, 0.71, 0.70 ms -- **0.71 ms** | 0.095 ms | 55 | 2 097 484 |

The sea reaches nine times as far -- three rings out to 1.3 km, where the
example's nine grids stopped at 144 m -- which is the extra draws and
triangles; the frame is three hundredths of a millisecond more for it.

**Floating.** Four hundred crates, 1.5 x 1.2 x 1.8 m, riding a sea of three
waves, against the same four hundred falling with no water -- every body awake
in both.

| 400 bodies | Simulation a tick |
|---|---|
| No water | 0.175 ms |
| Afloat | 0.620 ms |
| **The water's push and drag** | **+0.45 ms, 1.1 µs a body** |

The first version cost 2.07 ms for the same four hundred, 5.2 µs a body. Two
changes took it down, and they are in the code's comments because the first
thing anyone would try instead is the one that did not help: evaluating the
waves with fewer transcendental calls (the double angle from the single's sine
and cosine) saved a third; what saved the rest was not computing the waves
per cell at all -- a wave's phase is linear in the cell's offset, so the 27
cells' sines and cosines come from four pairs by the angle-sum rule, filled in a
cascade, and the depth fade from four exponentials. A world with no `Water`
pays nothing: the pass returns before it looks at a body.

## Scripts at the display's rate (ADR 0136)

What the render phase costs now that the default camera runs in it: the input
a frame reads at `Rate = Render`, the render steps -- `@engine/camera`'s rig
among them -- and `PreRender`, timed around the whole phase each frame.
Measured 2026-09-29 on `win-msvc-dev` with a window on a 120 Hz display,
`engine-host examples/10-open-world --frames=600 --exit --frame-stats`; three
runs.

| `examples/10-open-world` | The render phase a frame | p95 |
|---|---|---|
| The rig following the character every frame | 0.052, 0.050, 0.049 ms -- **0.050 ms** | 0.06 ms |

Fifty microseconds a frame is 6 ms of every second at 120 Hz and 12 ms at 240,
about one percent of one core. The frame itself did not move: 8.6 to 8.9 ms,
the display's own interval. `--frame-stats` prints the phase's line only where
a window runs it.


## A script runs while it is live (ADR 0137)

What finding the scripts a move carries costs: `World::setParent` compares each
moved instance's class with `Script`'s, in the walk of the subtree it already
made, and the runtime starts the ones that became live. A clone of a 10 000-part
model is parented into `Workspace` and a frame run, five times, best and mean
of the five. Measured 2026-09-29 on `win-msvc-editor`, headless, the null RHI,
three runs of each binary, alternating: before is the package built from
`09501c84`, after is S1. The 1 000 scripts each add one to an attribute, so the
count shows they started: 5 000 after, none before.

| Clone of a 10 000-part model | Before (best of five) | After (best of five) |
|---|---|---|
| No scripts | 191.5, 220.1, 191.9 ms | 198.3, 195.0, 203.9 ms |
| With 1 000 scripts | 192.0, 198.8, 207.2 ms -- none started | 203.8, 217.4, 201.6 ms -- all started |

Inside the run-to-run spread both ways: the clone and the physics taking ten
thousand parts cost the 200 ms, and a thousand scripts starting costs less than
that spread. A move with no script in it walks nothing that it did not already
walk; a move that brings scripts in starts them in the order of the moves,
without a walk of the world.

## The script-sides audit (S3)

Two cases the audit asked for beside ADR 0137's clone: a scene change whose
scene carries 1 000 scripts, and 200 stamps, each with two scripts and five
parts, placed in one tick. Measured 2026-09-29 on `win-msvc-editor`, headless,
the null RHI, three runs of each binary, alternating: before is the package
built from `09501c84` (before S1), after is S2. Each is the median of its runs
inside one process (six loads, five placements).

| Case | Before | After |
|---|---|---|
| `SceneService:LoadScene` to `SceneLoaded`, 1 000 scripts | 19.6, 20.5, 19.7 ms | 19.4, 19.7, 19.2 ms |
| 200 stamps placed and a frame run | 9.3, 8.9, 9.5 ms -- their scripts never started | 11.8, 11.9, 11.9 ms -- all 400 started |

The scene change did not move. The stamps' 2.5 ms is the 400 starts that did
not happen before -- about six microseconds a script, compiling and running a
one-line file -- plus numbering each placed instance's origin (ADR 0138 §6).

## A stopped script's connections (D360)

Fifty clones of a model whose script connects to `Heartbeat` are destroyed and
fifty more made every tick, so fifty are alive at a time; the time from tick to
tick, averaged over 250 ticks. Measured 2026-09-29 on `win-msvc-editor`,
headless, the null RHI, ludwerk-08's repro project. Before is S3 (`e81e4b27`),
where a stop only suppressed its run's handlers.

| Scripts that have lived | 12 500 | 50 000 | 100 000 | 137 500 |
|---|---|---|---|---|
| Before | 1.23 ms | 5.46 ms | 12.13 ms | 18.86 ms |
| After | 0.59 ms | 0.57 ms | 0.63 ms | 0.58 ms |

Flat after: a stop disconnects its run's connections once the drain is over,
so the signal's list holds the fifty that are alive.

## The terrain flight (terrain audit T0)

What a camera flying over terrain costs while every level of detail is rebuilt
under it: from 1 200 m down to 60 m over the middle of the ground and back, once
over 1 500 frames -- the flight the audit measured. Measured 2026-09-30 on
`win-msvc-dev` (D3D12, the GPU debug layer on), `engine-host <project> --headless
--frames=1500 --exit --frame-stats --width=1920 --height=1080`; three runs each,
before any of the audit's fixes.

| Flight | Median | p95 | p99 | Worst |
|---|---|---|---|---|
| The owner's place (a copy of it, 1 024 m square, a metre voxel) | 0.83, 0.91, 0.89 ms | 7.77, 7.07, 7.39 ms | 11.41, 11.01, 10.57 ms | 24.5, 20.5, 20.0 ms |
| The gallery (`tests/perf/terrainflight`) | 0.43, 0.44, 0.43 ms | 0.90, 0.91, 0.92 ms | 4.82, 4.87, 4.78 ms | 9.4, 9.4, 9.1 ms |

**On the owner's place the p95 is the drawing on the CPU** (7.4, 6.7 and
7.0 ms of it): the frames in which nodes are meshed. The audit's T5 holds this
to a p99 under 16.6 ms and a worst frame with no terrain spike; the worst
frame, 20 to 25 ms, is that spike. The gallery is a small world -- a few
hundred metres across -- and its tail is its handful of nodes being rebuilt.

The draws column `--frame-stats` prints counts parts, not terrain nodes, so it
is left out here.

### After T2 (ADR 0140)

The same flights, the same way, once a coarse node is meshed from the level-0
surface gathered under it, the level chosen by projected error, and the seams
stitched and sliding together.

| Flight | Median | p95 | p99 | Worst |
|---|---|---|---|---|
| The owner's place | 2.71, 2.79, 2.74 ms | 23.0, 22.2, 22.4 ms | 34.5, 31.4, 33.9 ms | 62.0, 60.1, 58.8 ms |
| The gallery | 0.55, 0.58, 0.57 ms | 1.07, 1.01, 1.10 ms | 8.9, 9.6, 9.5 ms | 17.3, 17.3, 16.3 ms |

**A regression, stated as one.** A coarse node is now built from the fine
surface under it and round it, where it was built from a mip, and the owner's
place streams cells in under the camera for the whole flight, each rebuilding
the coarse nodes it lands in. The first measurement of T2 was a p95 of 109 ms
and a worst frame of 1.3 s; what brought it to this is in ADR 0140's
consequences. The median is the frames with no building: a view drawn to its
error budget draws more nodes than one drawn by distance did, and a lower
quality draws fewer. T5 (TA14) moves meshing off the main
thread and holds the p99 under 16.6 ms.

### Built off the main thread (TA14, ADR 0141)

**Paced.** Headless frames run flat out finish the flight in a second, and
ground built beside the frame is then measured against a camera thirty times
too fast -- so from here the flight is flown at 60 frames a second
(`--pace=60`), the wait left out of the figures. The streamer runs on the wall
clock, so a paced flight also streams in more of the ground than an unpaced
one, and a paced frame starts on a processor that has slept: the gallery's
median, with nothing to build, is 0.98 ms paced against 0.57 unpaced. Compare
paced with paced.

| Flight, `--pace=60` | Median | p95 | p99 | Worst |
|---|---|---|---|---|
| The owner's place | 1.72, 1.72, 1.69 ms | 5.91, 4.96, 5.96 ms | 8.38, 7.88, 9.33 ms | 22.9, 25.0, 18.5 ms |
| The gallery | 0.98, 0.98, 0.97 ms | 1.85, 1.89, 1.86 ms | 5.13, 5.13, 4.97 ms | 16.1, 12.4, 13.3 ms |

The p99 is under T5's 16.6 ms, on the flight that was 31 to 35 ms with the
building in the frame. **The worst frame is not yet free of terrain**:
scheduling the workers was once caught at 2.6 to 6.7 ms on its own, and the
rest of the tail is putting meshes up; both are left to T5.

### Digging where somebody stands (terrain audit T5, P4)

`tests/bench/terrain_dig`: a character walking a circle of 24 m on a 160 m
field and digging a 1.5 m ball ahead of itself every tick -- a player with a
shovel. `terrain_sculpt` has nothing that moves, so it never built a collider:
the ground collides only near what moves, and that is where digging costs.
`win-msvc-dev`, 300 ticks, 2026-09-30.

| | Mean tick | Worst tick |
|---|---|---|
| Before | 10.3 ms | 38.7 ms |
| The bench on the engine's job pool, as the engine runs | 5.1 ms | 13.2 ms |
| And a collider meshed without the sky term and the geomorph | **2.0 ms** | **3.8 ms** |

**Where it went.** 10.15 of the 10.3 ms was the terrain colliders' pass, 7.6 of
it meshing 2.7 chunks a tick at 2.8 ms each, one after another: the bench ran
with no pool (the engine starts one; the bench did not), and a collider's
mesh paid for what only drawing reads -- the openness rays, the column map
they march and each vertex's slide towards its parent, which gathered the
level above. A tick's rebuilds are meshed side by side now and committed in the
world's order, and a collider's mesh is its triangles alone.

**The navmesh (TA18).** A crater in one corner of a 192 m field rebuilt 49 of
49 navmesh tiles; it rebuilds 4 or fewer, and a tile's terrain is meshed as a
collider.

### The seam between chunks' colliders (ADR 0143)

`engine_scene_seam_tests --test-case="what each join costs*" --no-skip`:
4,096 chunks of rolling ground over a 4 km square, `win-msvc-dev`, 2026-09-30,
the machine loaded to about 35% by other work -- compare rows with rows.

| Join | Build | Memory | Ray | Overlap | One chunk changed |
|---|---|---|---|---|---|
| One body per chunk (before) | 7.4 s | 118 MiB | 2.0 us | 10.2 us | 1.9 ms |
| One mutable compound | 7.6 s | 118 MiB | 8.3 us | 10.9 us | 1.8 ms |
| One static compound, rebuilt | 7.4 s | 118 MiB | 2.0 us | 8.1 us | 3.1 ms |
| **A body per chunk with its band (chosen)** | 25.3 s, meshing included | **171 MiB** | **2.0 us** | **7.7 us** | 6.3 ms, meshing included |

At 16,384 chunks a mutable compound's ray is 25 us and a static compound's
rebuild 9.5 ms; a body per chunk stays at 2 us. `tests/bench/terrain_dig` in
one run, band against none: 4.6 against 3.9 ms a tick. The moving bodies'
`mEnhancedInternalEdgeRemoval`, A/B in one run: within 2% on every bench.

### A layer's repeat broken up (ADR 0113's amendment)

What the far sample and hex tiling cost. `win-msvc-dev` (D3D12, the GPU debug
layer on), an RTX 4070 Ti SUPER, 2026-09-30; the gallery's layers as they
repeated (`TilingVariation` 0, `TilingFarScale` 1), as they are by default,
and with `HexTiling` on.

**The flight** (`tests/perf/terrainflight`, `--pace=60`, 1920 by 1080, three
runs each):

| Layers | Median | p95 | p99 | Waiting on the GPU |
|---|---|---|---|---|
| Repeating | 1.75, 1.64, 1.71 ms | 2.94, 2.92, 2.87 ms | 5.48, 5.14, 5.58 ms | 0.26, 0.25, 0.26 ms |
| Broken up (default) | 1.62, 1.56, 1.94 ms | 2.90, 2.97, 2.85 ms | 5.55, 5.30, 5.82 ms | 0.26, 0.25, 0.35 ms |
| Hex tiling | 1.64, 1.66, 2.01 ms | 2.89, 2.90, 2.97 ms | 5.29, 5.34, 6.10 ms | 0.26, 0.25, 0.36 ms |

**Within the runs' own spread**: the flight is drawn on the CPU and waits on
the GPU a quarter of a millisecond, so what a pixel costs does not show in it.

**Ground filling the screen** -- the tiling gallery's view across each field
from a person's height, the lower half of the picture ground, unpaced, 3840
by 2160, 900 frames, two runs each:

| Layers | Median | Waiting on the GPU |
|---|---|---|
| Repeating | 1.93, 1.90 ms | 0.69, 0.44 ms |
| Broken up (default) | 1.98, 2.02 ms | 0.77, 0.78 ms |
| Hex tiling | 2.06, 2.17 ms | 0.85, 0.92 ms |

The default costs about 0.1 ms a frame at 4K on this card, hex tiling about
0.2: each map read twice, and four times. A weaker GPU pays more of it, a
phone most; the owner decides whether hex becomes a default.

### The flight's worst frame (terrain audit T5)

The same flights, `--pace=60`, 1920 by 1080, 2026-09-30, three runs each.
**First, today's machine against the TA14 commit**: rebuilt at `4a7405f3` in a
worktree, the owner's place flew a p95 of 8.3 to 8.9 ms, a p99 of 14.4 to 15.1
and a worst of 44 to 57 -- not the table above's, so the rows below are
compared with these, not with that. And `main` had gone back from there: a p95
of 12.3 to 12.7, a p99 of 19.6 to 23.3, a worst of 42 to 50. Bisected to D383
(`a04783ca`): a node built empty is built again when its ground comes -- right,
and on a flight whose loaded cells came and went with the camera's height it
was rebuilding all the time.

| The owner's place | Median | p95 | p99 | Worst |
|---|---|---|---|---|
| `main` before | 3.41, 3.65, 3.52 ms | 12.7, 12.3, 12.7 ms | 19.6, 20.9, 23.3 ms | 45.6, 50.2, 41.6 ms |
| Ground streamed across the ground (D397) | 2.71, 2.72, 2.72 ms | 6.7, 6.9, 7.3 ms | 8.3, 8.7, 8.9 ms | 17.6, 25.7, 16.7 ms |
| **And the workers below the main thread, and a terrain's pipelines made while it loads** | **2.73, 2.69, 2.66 ms** | **6.96, 6.95, 7.03 ms** | **8.8, 8.7, 8.5 ms** | **17.9, 15.2, 16.6 ms** |

| The gallery | Median | p95 | p99 | Worst |
|---|---|---|---|---|
| After TA14 (above) | 0.98, 0.98, 0.97 ms | 1.85, 1.89, 1.86 ms | 5.13, 5.13, 4.97 ms | 16.1, 12.4, 13.3 ms |
| Now | 1.03, 1.04, 1.04 ms | 1.54, 1.77, 1.62 ms | 3.39, 3.81, 3.70 ms | 6.4, 6.0, 6.0 ms |

**What the worst frames were**, found with a timeline of each frame over 8 ms:

- **`jobs::schedule` for 2 to 12 ms** while handing the ground's build to the
  workers -- not running it, waiting: a worker woken by the call came back
  with Windows' boost and took the core of the thread that woke it, every
  core being busy with the ground. The workers now run below normal priority,
  and no call took over 1 ms in three flights. Splitting the pool's one
  condition variable in two, so a finished job wakes only those waiting for
  one, was measured too: within the runs' spread, and left out.
- **16 ms of `render` in the first frame with ground**, once: the terrain's
  pipelines and its layers' arrays, made in the first frame a terrain has
  ground to draw. A terrain with none yet is handed to the renderer too now,
  so they are made while the world loads.
- **What is left**: frames whose ground goes up (4 to 7 ms of uploads when a
  dozen nodes land in one frame) and frames waiting on the GPU or the display
  for 9 to 12 ms, a few a flight. None is over 33 ms.

### Far ground drawn from cells (ADR 0144)

The fixture `tests/screenshots/terrainfar`: ground 3 km across at a metre
voxel -- about 2 200 cells -- streamed at 300 m, the camera 2 km off, nothing
of what it sees resident but the edge. `--pace=60`, 1280 by 720, cold (no
partition cache, nothing gathered), 2026-09-30.

| | Median | p99 | Worst | Peak resident |
|---|---|---|---|---|
| Before: only what is resident is drawn -- from 2 km, nothing | 0.77 ms* | 1.90 ms* | 5.7 ms* | 128 MiB* |
| The same ground held whole, every chunk in memory | 3.07 ms | 7.23 ms | 33.9 ms | 339 MiB |
| **Streamed, drawn whole from its cells** | **2.77 to 3.04 ms** | **7.1 to 8.8 ms** | **20 to 27 ms** | **315 to 323 MiB** |

\* The package's optimised build, which is the only build of before at hand
and draws nothing far: what drawing nothing costs, not a baseline to beat.

**Cold**: the far ground's outline is drawn within 0.3 s of the first frame,
its finest detail within 20 to 22 s, built off the frame's thread; a second
lane and reading only the cells round what a node lacks each changed that by
under a second -- it is the meshing, as much as the same ground held whole
needs. Memory is bounded by three rows of decoded cells across a node, and the
cells' summaries: a few hundred bytes a chunk.

**The owner's place**, whose ground is all within the radius: the flight's p95
and p99 are as they were the same evening (8.3 to 9.2 and 10.2 to 11.2 ms,
against 8.7 and 11.1 for 391dc046 rebuilt beside it); the median is 0.5 ms
higher, 3.5 against 3.0, which is the far ground drawn in the first seconds
before its cells come in.

### A brush stamp (the terrain-editing ledger, P1 and P2)

`docs/briefs/terrain-editing-perf.md`, 2026-10-01, `win-msvc-dev` on the
development machine. A stamp is timed round the call (`editbench`: 512 m of
rolling hills at a 1 m voxel, a stamp a frame for 300 frames; the unit bench
is `engine_asset_tests --test-case="what a smooth stamp costs" --no-skip`, on
the pool), and the editor drive is the owner's place with ludwerk-08's thirty
smooth drags (`--editor-drive`, `--frame-stats`).

| | Before | After |
|---|---|---|
| `SmoothBall` r 8, `editbench` | 82.7 ms | **2.3 ms** |
| `SmoothBall` r 4 / 16, 1 m voxels, unit bench | 12.5 / 98.2 ms | **0.5 / 3.2 ms** |
| `SmoothBall` r 16 / 32, 0.5 m voxels, unit bench | 217 / 853 ms | 8.9 / 41 ms |
| `PaintBall` r 24 blend, `editbench` | 4.8 ms | **2.5 ms** |
| `GrowBall` r 16 / `FlattenBall` r 10 / dig r 6, `editbench` | 2.4 / 0.4 / 0.5 ms | 2.1 / 0.3 / 0.3 ms |
| The editor, smoothing the owner's place: p95 / p99 / over 33 ms | 420 / 561 ms / 210 frames | **14.4 / 17.7 ms / 0** |

| Bench (`perf_budget`) | Mean tick | Worst | Budget |
|---|---|---|---|
| `tests/bench/terrain_smooth` (128 m of hills, a smooth a tick at 8 m, every fourth at 16 m) | **2.93 ms** | 5.75 ms | 16 ms |
| `tests/bench/terrain_paint` (the same hills, a blend a tick at 24 m) | **3.55 ms** | 5.91 ms | 16 ms |

## A world larger than memory (ADR 0149)

The terrain-editing ledger's P5b. A 1 m voxel throughout; this machine, the
`win-msvc-dev` build, headless, 2026-10-01. Memory is the process's private
bytes, sampled twice a second from outside.

**Laying it.** `--import-terrain=hills` (what `ludwerk terrain import hills`
runs), tiles of 256 columns, the ground behind the tile written to the session
cache and the save moving the cache's files into the scene's folder:

| | Cells | Time | Of it, writing cells out | Peak |
|---|---|---|---|---|
| 8 192 m | 16 641 | 106 s | 37 s | 386 MiB |
| 16 383 m | 65 536 | **411 s** | 148 s | **617 MiB** |

Linear in the world: four times the ground, 3.9 times the seconds. The cells
are 4.0 GB on disk, some 61 KiB each. Before the save moved the files -- it
read and wrote each -- the 16 km save alone was past seven minutes.

**A script laying it at run time**, `Terrain:WriteHeights` a tile of 512
columns a Heartbeat, 8 km, the camera at a corner with the 512 m terrain
radius. The ground the field holds stays between 2 600 and 2 900 chunks
throughout -- before the cache it was every chunk of the world:

| | Time | Peak |
|---|---|---|
| The camera sees 1 km (`FarPlane = 1000`) | 91 s | 643 MiB, level from the 24th second on |
| The camera sees the world (`FarPlane = 20000`) | 94 s | 1 120 MiB, rising with the world |

The difference is the far ground, drawn from the cache's cells (ADR 0144): a
summary of every chunk of every cell the camera can see, and their coarse
meshes -- 740 000 live blocks of 128 bytes or less at 4 km.

**Flying over it**: the 16 km world as saved, corner to corner at 200 m/s an
axis and 150 m up, `FarPlane = 1500`, 512 m terrain radius, `--pace=60`, 6 600
frames. The same on the build before the stage (`6263333f`, the package),
which could not have made the world:

| | Resident chunks | Median | p95 | p99 | Over 33 ms | Peak |
|---|---|---|---|---|---|---|
| This build | 1 970 to 2 340 | 17.0 ms | 129.8 ms | 481 ms | 756 | 2 069 MiB |
| `6263333f` | 1 950 to 2 910 | 15.6 ms | 109.1 ms | 405 ms | 746 | 2 047 MiB |

Not a frame rate, and not the cache's doing: the ledger's open item, and the
reason for P5c. The far ground reads level-0 cells to draw coarse nodes and
re-derives every drawn node's content from all the cells under it whenever one
comes or goes; 65 536 cells is thirty times the fixture it was measured on.

**These two rows were taken with `--screenshot`**, which builds every node of
the terrain inside the frame that asks for it: they are that, and not the
game's frame. Without it the same flight on `6263333f` is the "before" row of
the next section. The memory is the same either way.

## A world that size, flown (ADR 0150)

The terrain-editing ledger's P5c. The sixteen-kilometre world of the section
before -- 65 536 cells of hills at a 1 m voxel, 4.0 GB -- on this machine, the
`win-msvc-dev` build with the GPU's debug layer on, headless, `--pace=60`, no
`--screenshot`, 2026-10-01. Memory is the process's private bytes, sampled
twice a second from outside. `tests/perf/farflight` is the flight.

**The targets**, and what was measured against them. p99 under 16.6 ms and no
frame over 33 ms once past the first seconds; memory level, and bounded by
what is in view:

| | Median | p95 | p99 | Worst | Over 33 ms | Memory |
|---|---|---|---|---|---|---|
| Flight corner to corner, 200 m/s an axis, `FarPlane` 1 500 | 4.6 ms | 8.9 ms | **11.9 ms** | 22.9 ms | **0** | level at 790 MiB, peak 839 |
| The same, `FarPlane` 6 000 | 5.7 ms | 9.5 ms | **11.5 ms** | 24.5 ms | **0** | level at 810 MiB, peak 870 |
| Camera still, `FarPlane` 6 000 (180 draws, 385 000 triangles) | 5.7 ms | 6.2 ms | **8.3 ms** | 9.8 ms | **0** | 801 MiB, flat |
| Before: the flight at 1 500 on `6263333f`, the package | 18.0 ms | 28.4 ms | 33.5 ms | -- | 77 | 2.6 GiB and rising |

**The bound.** What the flight holds is the ground inside the terrain radius
and two cells (the streaming's own hysteresis), the meshes of the nodes in
view, 384 of the far ground's files decoded (some 64 MiB), the summaries of
1 024 cells -- and the world's index, the one part that follows the world and
not the view: 1.9 KiB a cell, 125 MiB here. Of the 800 MiB, 350 are the
process before any ground (the dev build, the debug layer).

**Fast travel**: a teleport of ten kilometres with
`StreamingService.PauseOutsideLoadedArea`, the simulation held until the
ground under the player is resident.

| | Ground under the player | Worst frame after |
|---|---|---|
| This build | **252 to 268 ms** | 13 ms |
| Before the held ring was read where it stands | 2.4 s | 35 to 49 ms, three seconds on, as the meshes left behind were released at once |

With the streaming's reads freed as they land and not once a pump (the cleanup
ledger's C1), the same travel **without** the held ring read where it stands
is 270 to 287 ms, where it was 2 003: the async service was four reads a pump.
The flight is unchanged by it -- sixty cells a second was inside that.

**Laying it**, far ground's files and all, with `--import-terrain=hills`:

| | Time | Of it, writing cells out | Of it, the far ground's files | Peak |
|---|---|---|---|---|
| 16 383 m, 65 536 cells | **430 s** | 56 s | about 100 s | **575 MiB** |

The far ground's files are 957 MB in 5 376 files, a quarter of what the cells
weigh. The memory an import gains as it goes is 1.9 KiB a cell: the index.
The section before has 411 s and 617 MiB without the files, 148 s of it
writing cells out: they are encoded on every worker now (D409).

**A project saved before the files existed** -- the same world, its index
with no signatures and no far ground on disk -- opened with the camera still
and `FarPlane` 6 000: the files are made in the background at 1.9 blocks a
second, 232 of the 324 in the first two minutes, the whole world in under
three. While they are: median 5.1 ms, p99 8.2 ms, worst 12.5 ms, no frame
over 16.7 ms. A block made alone on one thread is 1.2 to 2 s.

**A script laying a world at run time**, `Terrain:WriteHeights` a tile of 512
columns a Heartbeat, the camera at a corner with the 512 m terrain radius.
Changed ground is held to 256 MiB of voxels before any is written (D409),
which is some 420 MiB of the process -- the price of not writing a world that
fits:

| | Time | Peak |
|---|---|---|
| 8 km, the camera sees 1 km | 69 s | 1 062 MiB |
| 8 km, the camera sees the world (`FarPlane = 20000`) | 73 s | 1 170 MiB |
| 16 km, the camera sees 1 km | 298 s | 1 116 MiB |
| 8 km and 20 km seen, before ADR 0150 | 94 s | 1 120 MiB and rising with the world |
| 8 km and 1 km seen, everything far written out (ADR 0149 as it was) | 95 s | 640 MiB |

Four times the world is four times the seconds and 54 MiB more: the index.
What rose with the world before was a summary of every chunk of every cell
the camera could see, kept for good.

**A world a script makes in one frame**: `terrain_far_plane`'s twelve
kilometres of flat ground, 35 344 cells, 305 MiB held and 4 MiB as cells.

| | Before the first frame |
|---|---|
| ADR 0149 as it was: every cell past the radius written out | 38 s after the 13 s the script takes, on Windows; thirteen minutes in the Linux container, through a mounted source tree |
| Held, less the 49 MiB that do not fit | 3 s |

Of the 38 s, 18 were asking for the cache's folder once a cell and 3 were
taking each cell out of the field one at a time; the 17 left are the files
themselves, 0.5 ms each whatever is in them and however many threads write.

**The gate's miniature** (`terrain_far_flight`): four kilometres, 4 356 cells
and 36 blocks, laid and flown out and back twice, 4 200 frames at
320 x 180, paced at 240. On this machine 5 287 cells come in and 5 060 go out, 200 or more of
the far ground's files are read and no cell is read to make one, and the peak
is 330 MiB with the second lap 2% over the first; on lavapipe in the Linux
container the peak is 560 MiB and the median frame 24 ms, which is why the
frame is not what it gates.

## The local gate (ADR 0148)

`scripts/localgate.ps1`, the full run, on the development machine (20 logical
cores, Docker capped at 10), 2026-10-01. Warm is a run with nothing changed
since the last; before ADR 0148 even that rebuilt -- the generators rewrote
their outputs and SPIRV-Cross restamped a header, so every shader of every
profile was compiled again.

| Stage | Before, one after another | After, three lanes at once (warm) |
|---|---|---|
| docs | 88 s | 76 s |
| luau | 87 s | 91 s |
| format | 29 s | 28 s |
| windows | 720 s (ctest 362 s) | 139 s (ctest 90 s) |
| linux | 722 s (ctest 462 s) | 225 s (ctest 169 s) |
| shipping | 520 s | 156 s |
| android | 140 s | 25 s |
| **Wall** | **about 38 min** | **6.8 min** (409 s) |

After a header every module includes changed (`log.h`), the same run was
12.4 minutes: the three Linux release profiles recompile, which no cache
shortens, and are the long pole. Four runs of 2026-10-01 with code changed:
556 to 915 s.

## CI (the cleanup ledger's C0)

A push to `main`, from its first job starting to its last ending, on GitHub's
hosted runners (four cores, no GPU), 2026-10-01:

| | Before ADR 0148 | After it | With the Windows tests in three jobs |
|---|---|---|---|
| The run | 41 to 55 min | 36 to 43 min | to be measured on this push |
| Linux job | -- | 6 to 7.5 min | |
| Windows job | -- | 25 to 31 min | |
| macOS job | -- | 7 to 17 min, after both | |

Where the Windows job's 30 m 44 s went on `82b4492a`: configure 40 s, build
5 min (sccache 868 hits of 898), **tests 22 min** -- 95 tests, 3 581 s of them
through `-j 4`. The longest: `terrain_far_plane` 486 s, `terrain_shadow_acne`
415 s, `terrain_far_flight` 330 s (run alone), `app` 252 s,
`terrain_gallery_holes` 203 s, `terrain_far` 202 s. On the development
machine's GPU the same six are 28, 40, 45, 83, 6 and 38 s: the runner draws
in software.

| | Before | After |
|---|---|---|
| ctest, `win-msvc-dev`, 97 entries | 362 s, one at a time | **90 s** at 20 jobs |
| `engine_render_tests` as one entry / as three shards | 87 s | 38, 30 and under 20 s side by side |
| A clean rebuild of `win-msvc-dev` (`ninja -t clean`, then build) | 576 s | **66 s** through sccache, 892 of 892 objects from the cache -- measured before D404 turned the cache off for a localised MSVC, which this machine's is; 270 s without it |
| The inner loop: `-Only windows -Tests '<regex>'`, nothing to build, two tests | -- | 4 s |

The ten slowest ctest entries of the serial run, which is where the shards and
the slots came from: `render` 87 s, `app` 41 s, `terrain_far` 41 s, `asset`
27 s, `openworld_soak` 22 s, `terrain_shadow_acne` 16 s, `capture_gate_look`
15 s, `streaming_soak` 15 s, `example_boot_22-atmosphere` 15 s, `net` 13 s.

**The hosted Windows runner draws on a software rasteriser** (WARP), four
cores: a picture there is minutes of CPU, and beside three others at `-j 4`
it is two to four times that. Its normal times, so the 900 s hang guard the
drawn tests carry does not hide one that has gone wrong -- a time far past
these is a regression, whether or not the guard is reached:

| ctest, `win-msvc-dev` on CI | One at a time (36823092231) | `-j 4` (36835788369) |
|---|---|---|
| `terrain_shadow_acne` | 196 s | 497 s |
| `terrain_far` | 100 s | 428 s |
| `terrain_gallery_holes` | 44 s | 162 s |
| `graphics_settings_differential`, `foliage_card_shadows` | -- | 147 s, 144 s |
| `render` (one entry) / its three shards | 127 s | 92 s the longest |
| `app` | 63 s | 95 s |
| **Total** | **1 163 s** | **869 s** |
