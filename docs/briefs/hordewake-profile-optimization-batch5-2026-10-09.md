# Hordewake optimization batch 5: shared property metadata lookup

## Authorization and current status

Continue optimization without reducing quality or changing gameplay. No phone
operations or PC pointer capture. Preserve the existing dirty checkout. Prior
normal Xbox v29 and benchmark v31 are the fallback/control (batch 4).

## First candidate rejected after physical measurement

`engine/scene/src/swarm.cpp`: ground-column cache validation scans terrain
membership, revisions and origins once on the first ground query of a synchronous
authority swarm step. Later agents reuse the validation for that step. A step
only updates agent bodies; scripts and terrain edits do not run between agents.
The next step validates again. Public standalone `swarmTerrainAt` calls still
validate every call, including several calls within the same engine tick.

Interpolation, ground-query distance, neighbor ordering, movement arithmetic,
physics fallback and all simulation rates are unchanged. The cache's maximum
column count is still checked on every query. More than eight terrains still use
the original uncached fallback. No new persistent validity flag or public API.

## Validation

- Windows: all 16 swarm cases, 2600 assertions passed, including new same-tick
  edits, translation, terrain insertion/removal, cache overflow and nine-terrain
  fallback. The existing stepped terrain-edit test checks exact ground equality.
- Linux also passed all 353 scene cases. UWP/offline Android builds passed.
- Physical v32 comparison below rejected the production change.

Xbox v32 completed the same sealed 1500-agent/12-weapon profile. Against v31:
native swarm 1.161541 -> 1.161224 ms, general FPS 52.3078 -> 52.2263. This does
not establish a useful gain. All 789 shader/game-pack files were identical.
Production `swarm.cpp` change reverted; the useful same-tick regression remains.
Raw logs/comparison: Downloads/Hordewake-Profile-1500-opt5-results.
Benchmark v32 is an experiment, not an accepted optimized runtime. Intermediate
optimized5 Windows/Android artifacts also contain this rejected experiment;
they must be refreshed before final delivery. Normal installed v29 is untouched.

## Second candidate: resolve property metadata once

`ClassRegistry::findProperty` gains an overload returning its subscription slot
with the descriptor, from the same existing lookup. `World::setProperty` uses
that slot instead of looking the same member up again after a successful write.
All validation, old/new comparison, setter calls, mutation accounting, inherited
slot numbering, observed properties and change queue behavior remain intact.
No class-specific fast path or generated property implementation is bypassed.

Tests extend inherited/redeclared/absent/invalid member resolution. All 353 scene
cases / 17767 assertions pass on Windows and Linux. All 124 Linux replication
cases / 69805 assertions pass. Windows seven graphical capture gates pass,
including full terrain texture bindings. UWP and offline Android builds pass.

Isolated lookup microbenchmark: six alternating samples per mode, four million
resolutions per sample. Separate descriptor+slot median 21.99155 ms; combined
11.70855 ms (-46.76%), same checksum. This measures only metadata resolution,
not whole property writes, game frame time or mobile performance. Source, build
command and raw samples: Downloads/Hordewake-Profile-1500-opt5b-results.

Physical benchmark v33 installed/launched; one-minute comparison pending.
All 789 shader/game-pack files match prior v31; only native runtime changed.
Normal v34 packaged, NOT yet installed. Normal Windows/Android optimized5
artifacts are being refreshed with the second candidate, replacing rejected
first-candidate exports. No phone operation or normal PC game launch.

## Physical result and acceptance

Xbox v33 completed: 52.470419 FPS, mean frame 19.058357 ms, best 12.921978,
p99 28.729235, worst 37.923200 ms. Actual mobs mean 1496.944 / min 1426 / max
1500, twelve weapons, High 1920x1080, VSync off, 5 GB Game budget, 20-second
warm-up then 60 seconds measured. Mean draws 629.951 versus v31's 648.118.
Native swarm CPU 1.154881 versus 1.161541 ms; Heartbeat 4.636104 versus 4.625548.
These differences do not establish a whole-game gain. FPS is effectively
unchanged and live draw workloads differ; worst frame is slightly worse.

Accepted only as eliminating a duplicate lookup in general property writes,
with the isolated lookup measurement and unchanged correctness gates. The
-46.76% applies exclusively to descriptor+slot resolution, not full writes,
swarm simulation or FPS. No hitch fix or physical Android performance claim.

Final exports refreshed: Downloads/Hordewake-0.8.49-optimized5-{windows,android,xbox}
and optimized5-windows.zip. Their runtime contains the property lookup change
and original terrain-validation implementation, not rejected experiment v32.
Android signature checked offline. Normal v34 is now being installed; benchmark
v33 stopped by the normal deployment script. Raw log, comparison JSON, isolated
microbenchmark source/raw samples, test/export logs, matching shader/game hashes
and artifact SHA256 manifest: Downloads/Hordewake-Profile-1500-opt5b-results.

## Final normal deployment and newly captured loading spike

Normal v34 installed/open, High 1920x1080, 5 GB Game budget, menu ~59.9 FPS.
Verification log: Downloads/Hordewake-0.8.49-optimized5-xbox/verification/xbox-v34-engine.log.
No error/device-lost entry in this captured log; no new screenshot attempted
because Portal screenshot failures from batch 4 remain unexplained.

While the game was open, the log recorded a lobby scene transition followed by
a 158.558873 ms frame: render-step scripts 147.684083 ms, simulation 9.310390 ms,
GPU/display wait .309045 ms. Adjacent message: `232 interface pictures loaded
in 149 ms`. This is a concrete UI-loading stall to investigate next. It is a
scene transition, not proof of the cause of the owner's intermittent mid-match
hitch. Do not describe the hitch as fixed or this correlation as a full trace
of each individual image/decode/upload cost. Do not interrupt the owner's game.

Next candidates: split and profile the UI picture cold-load path; retain native
cold terrain PSO work as another loading cost. No phone access, no PC pointer
capture, no shader-quality reduction. Batch 4 palette reuse remains accepted.
