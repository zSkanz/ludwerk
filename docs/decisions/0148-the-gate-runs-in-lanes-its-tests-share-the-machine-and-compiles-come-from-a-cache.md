# 0148 — The gate runs in lanes, its tests share the machine, and compiles come from a cache

- Status: accepted
- Date: 2026-10-01
- Decided by: the owner, on 2026-10-01, through ludwerk-08: *"cada fluxo de
  trabalho está levando meia hora para rodar ... reduzir esses processos ...
  fazer tudo de uma vez"*. By his standing rule of 2026-09-30, the means are
  what professional teams run: an inner loop and an outer loop, a parallel
  test runner, and a compiler cache.

## Context

`scripts/localgate.ps1` ran its stages one after another and ctest one test
at a time. A full run on the development machine took about 38 minutes:

| docs | luau | format | windows | linux | shipping | android |
|---|---|---|---|---|---|---|
| 88 s | 87 s | 29 s | 720 s | 722 s | 520 s | 140 s |

Of the Windows stage, 362 s was ctest; of the Linux one, 462 s. Every item of
work ran the whole of it, however small.

## Decision

1. **Two loops.** The inner loop, per change, is the build and the tests the
   change touches: `scripts/localgate.ps1 -Only windows -Tests '<regex>'`
   (ctest `-R`), a conformance spec, the one picture gate concerned. The outer
   loop, the full gate, runs once per push -- a ledger stage or several items
   together -- and a documentation-only change runs `-Only docs`.
2. **ctest runs at the core count**, locally and on CI, and the tests that may
   not simply share the machine say so in `engine/app/CMakeLists.txt`:
   - **timed, so alone** (`RUN_SERIAL`): `perf_budget` and the two soaks;
   - **drawn on a real GPU, three at a time** (`RESOURCE_GROUPS gpu:1` against
     the three slots of `tests/ctest-resources.json`, which the Windows and
     macOS test presets pass); on Linux the GPU is lavapipe, the CPU, and they
     run free;
   - **one project folder, one test at a time** (`RESOURCE_LOCK
     project:<folder>`), because a run writes its log and caches beside it.
   A test executable that is its own long pole is cut into shards by doctest
   filters (`SHARD_CASES`, `SHARD_FILES` in `engine_add_module_tests`), every
   case in exactly one shard; `render` is three.
3. **The full run is three lanes at once**, each a copy of the script with
   `-Stages`: the docs lint; this machine's build -- luau, then windows, then
   android, which needs the windows stage's tools; and the container's --
   format, then linux, then shipping, inside Docker's half of the CPUs, which
   the owner asked for and which stays. The logs are printed whole, a lane at a
   time, and the run is as long as its longest lane. `-Serial` keeps the old
   order.
4. **sccache, at the version CI pins** (`SCCACHE_VERSION` in `ci.yml`, written
   once), compiles locally too. The root `CMakeLists.txt` finds it
   (`ENG_COMPILER_CACHE`, on when it is installed) and, for MSVC, puts the debug
   information in each object (`Embedded`), as CI already builds: a shared PDB
   is the one thing it cannot cache. `scripts/bootstrap.ps1` installs it beside
   the build trees; the Tier-2 image carries it, with its cache on the build
   volume. Nothing it touches ships.

## Consequences

- What running tests side by side found, fixed rather than serialised away:
  two runs of the engine from one folder fought over `engine.log` (the owner's
  queue, Q0) -- a second run now writes `engine_2.log` as the major engines
  number theirs, and never rotates a log another process holds; and two test
  executables cleared the same temporary folder.
- A new test that times itself, draws, or opens a folder another test opens
  joins the lists in `engine/app/CMakeLists.txt`. One that does not and should
  shows up as a flake under `-j`, which is where it is found.
- **A no-change run must build nothing**, and two things made it build: the
  API generators rewrote their outputs whether or not the text changed (they
  write only what differs now), and SPIRV-Cross stamped the time into a header
  on every configure, which recompiled every shader of every profile
  (`SOURCE_DATE_EPOCH` is set round its subdirectory, as reproducible builds
  set it).
- The full gate, warm: 38 minutes to under 7. The numbers, before and after,
  are in `docs/perf-baselines.md`.
