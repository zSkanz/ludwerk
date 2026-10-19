# 0249 — The local gate runs what a change can affect

- Status: accepted
- Date: 2026-10-18
- Builds on: ADR 0148 (the gate in lanes, the inner and outer loops)

## Context

ADR 0148 split the gate into an inner loop (`-Only windows -Tests <regex>`)
and a full run once per push. The regex was written by hand, so the full run
was what got run. On 2026-10-18 the Linux stage alone took about 30 minutes
for a change to one shader and one renderer file: 221 tests, most of them
unrelated to the change. The slowest were `netcode_vehicle` (353 s, run one
at a time), `terrain_far_flight` (305 s), the two `netcode_acceptance` runs
(180 s each) and `terrain_shadow_acne` (167 s).

## Decision

1. **`scripts/localgate.ps1 -Affected`** is the gate of the inner loop and
   of every push inside a batch. `tools/repo/affected.luau` reads what
   changed between the merge base with `-Base` (`origin/dev` unless said)
   and the working tree, new files included. It writes a plan: which
   stages run, which tests, and why. The run that a person started makes
   the plan once and hands it to its lanes.
2. **A file reaches tests three ways:**
   - **its module**: a file under `engine/<module>/` reaches that module's
     unit tests and those of every module that includes it, from the real
     `#include "engine/<module>/..."` lines;
   - **its area**: a module, or a file of `engine/app/` by its name, names
     areas (`render`, `physics`, `net`, `data`, `xr`, `editor`...), and an
     area names the tests that run the host on it;
   - **its path**: a test whose command names the file, or the folder of
     the driver it runs, runs.
3. **Rules that widen the set on purpose:**
   - shaders, `engine/render` or `engine/rhi` → every test that starts a
     host which draws, and the lavapipe stage;
   - `engine/physics` or `engine/scene` → determinism and netcode;
   - `engine/script` or `runtime/` → conformance, the script checks and
     the examples;
   - CMake files, `cmake/`, `third_party/`, the gate's own scripts, the
     bottom of the engine (`core`, `jobs`, `platform`), or a file no rule
     maps → the full suite. A `CMakeLists.txt` whose change only
     registers tests -- lines of `add_test`, its properties, comments --
     runs the tests it names instead;
   - an `@engine/` module reaches the script area and the area of its
     subject (`xrgrab` the headset's and the netcode's tests).
   A smoke set always runs: the host boots, a scene is drawn, a capture is
   taken, and two runs of one world agree. A test no rule knows always runs.
4. **Every stage**, not only the tests:
   - `windows` and `linux` build, and run the plan's tests. Without
     `-Affected`, both still run everything;
   - `android` and `shipping` run only when what only they build changed:
     their folders and scripts, the export, or changed lines near
     `__ANDROID__` or a profile's `#if`;
   - `luau` runs when Luau, a project file, a catalogue or the script API
     changed; `format` when C++ or a shader did; `docs` when a page, a
     catalogue or code did (its legal sweep reads code), once, without the
     second pass a clone would make;
   - `perf` runs when an area it measures changed, and its tests leave the
     `windows` and `linux` stages, as in the full run;
   - `lavapipe` runs when the renderer changed;
   - the `windows` stage's steps after its tests (conformance through the
     CLI, the hot-reload round trip, the packaging chain) each run when
     their subject changed.
5. **The full suite still runs**: in CI on every push (`main` moves only on
   a full green CI), and locally once per batch before its last push, or
   whenever the plan falls back to it.
6. **A run says what it chose**: every stage it skips, with why, and
   `ran 34 of 231 tests` with each test's reason. A skipped test is never a
   surprise.

## Measured

A plan of two tests: the `windows` stage in 4.9 s with the build current,
and the `linux` stage in 61 s, most of it the container's build check.
A change to one file of the UWP player runs 9 of 231 tests. A change to a
shader and the renderer runs 189: everything that draws.

## Not yet

- The docs lint, the Luau check and the formatter still read the whole
  tree when they run. Each could read only the changed files and what they
  generate.
- The slowest tests are as long as they were. Each could run fewer or
  smaller frames, or a narrower scene, where its assertion allows.

## Consequences

- A wrong edge in the plan can skip a test that would have failed. CI's
  full run on every push catches it, and so does the batch's full local
  run before then.
- `affected.luau` holds the areas. A new subject of the host, or a test
  named outside its area's patterns, runs on every change until it is
  added there.
