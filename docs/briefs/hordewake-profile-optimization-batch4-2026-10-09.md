# Hordewake optimization batch 4: shared skin palette uploads

## Scope and constraints

Owner authorized continued optimization after restoring terrain textures.
Preserve all rendering and animation quality. No phone operations or PC pointer
capture. Keep preexisting dirty engine/game work intact; no commits or pushes.
Prior corrected normal Xbox v27 is fallback; prior heavy v26 is comparison.

## Implemented change

`engine/render/src/render_world.cpp`: extraction now stores one GPU bone range
per immutable `Pose` pointer already shared by `AnimationSystem`. A frame-local
lookup assigns offsets in first-seen traversal order. Pointer identity is lookup
only and cannot affect ordering. It is discarded at each extract, including
another view or world; no cross-frame invalidation scheme is needed.

The old code copied each shared palette again for every character. The renderer
then uploaded that expanded palette buffer for instanced skinning. Characters
still carry their own transforms, motion keys, visibility, materials and bounds.
Distinct poses, individual bone overrides, ragdolls and bind-pose rigs keep their
own behavior. No animation update interval or sample quantization changed.

## Validation so far

- New extraction regression: two equal animated rigs, a different animation
  speed, a bind-pose rig, two sections each, another tick and an individual joint
  override. Verifies every copied matrix value and range. Two shared poses occupy
  four matrices; overriding one rig creates the required third palette.
- Windows and Linux targeted test: one case, 654 assertions passed each.
- Windows full render suite: 279 passed / one intentional benchmark skip,
  101342 assertions passed.
- Seven existing/new capture gates passed Windows and Linux, including the
  terrain texture-binding regression. Existing goldens are unchanged.
- UWP native host and offline Android library rebuilt successfully.
- Clang-format 18 applied to changed C++; no game script or shader change.

## Physical measurement in progress

Unchanged sealed fixture: 1500 target enemies, twelve maximum-level weapons,
20-second warm-up, 60-second measurement, High 1920x1080, VSync false.
Benchmark package v28, publisher CN=LudwerkDevelopment, existing Benchmark
identity. Evidence: Downloads/Hordewake-Profile-1500-opt4-results.
Helper: %TEMP%/ludwerk-opt4-heavy-xbox-run.ps1.
Do not accept or claim FPS improvements until completed logs are compared.
Original comparison: v26 52.239933 FPS; extract.meshes mean 1.843401 ms,
render.extract mean 2.979570 ms; p99 frame 28.770842 ms.

Cold native pipeline compilation and the intermittent hitch remain open.
No cache experiment should be resurrected: see batch 3's driver failures.

## First completed Xbox result (repeat/control pending)

v28: 52.441239 FPS, 60.010023 s / 3147 frames; actual enemies mean 1496.976,
min 1426 / max 1500; twelve weapons. Mean 19.068962 ms, p99 28.799318,
worst 35.266084 ms. No >50 or >100 ms frames in this measured minute.

| CPU scope mean, ms | Prior v26 | New v28 |
|---|---:|---:|
| extract.meshes | 1.843401 | 1.836696 |
| render.extract | 2.979570 | 2.983516 |
| render.prepare | 0.694331 | 0.589420 |
| render.world | 2.397078 | 2.212867 |

Frame FPS is effectively unchanged; extraction CPU is effectively unchanged.
Preparation mean is ~15.1% lower, and its self median fell .453990 -> .360113 ms,
consistent with fewer palette bytes uploaded. This is one run, not proof of a
general FPS gain; draws differ (648.216 -> 629.582) and worst frame worsened.

Repacked preserved v26 runtime/content as Benchmark v30 (manifest version only)
to avoid downgrading the installed application. This is the OLD CONTROL, not an
optimized runtime. Original files/hashes recorded in control-input-sha256.json.
Control evidence: Downloads/Hordewake-Profile-1500-opt4-control-results.
New repeat v31 is packaged and awaits control completion. Normal optimized v29
is packaged but not yet installed. Windows/Android normal optimized4 exports are
complete; Android signature verified offline. No phone operation.

Old-runtime control v30 completed: 52.216037 FPS, render.prepare mean .691330 ms
(self median .450441 ms), reproducing prior v26 .694331 ms. Mean draws 648.149;
actual mobs mean 1497.056, min 1424/max 1500. p99 frame 28.864408 ms, worst
33.155871 ms. This supports that the new .589420 ms preparation reduction is not
just a drifting baseline. New v31 repeat is now being installed/captured.
All 50 Linux extraction cases passed (1066 assertions). Input-equivalence script
verified 789 shader/game-pack files byte-identical between old v26 and new v28;
only native runtime changed. Evidence lives in opt4-results/input-equivalence.json.

## Accepted measurement: repeated preparation reduction

New repeat v31 completed: 52.307768 FPS, preparation mean .584473 ms,
self median .352705 ms; actual mobs mean 1497.001, mean draws 648.118.
Old v30 mean draws 648.149, so this repeat has nearly identical draw load.
p99 frame 29.274358 ms and worst 36.868197 ms: tail did not improve.
No >50 or >100 ms frames in the measured minute; no hitch-fix claim.

Across two old and two new runs, preparation mean .692830 -> .586947 ms,
**15.28% lower**, about .106 ms saved per frame in this stage. FPS averages
52.228 -> 52.375: effectively unchanged. Extraction CPU is unchanged. Renderer
world means fell 2.390423 -> 2.228311 ms (~6.8%), but live draw workloads differ
in the first new run; do not call this an exact general engine speedup.

Accepted for lower duplicate palette copying/upload work with preserved matrix
values, features and capture goldens. It does not reduce shader quality or
animation fidelity and applies to every backend through shared extraction code.
Only Xbox was physically benchmarked this iteration; Windows/Linux tests and
offline Android build/export are not physical mobile performance evidence.

Full comparison: Downloads/Hordewake-Profile-1500-opt4-results/comparison.md
and comparison.json, generated by compare-runs.py with raw logs preserved in
the opt3-fixed / opt4 / opt4-control / opt4-repeat result folders.
Normal v29 install is now in progress; benchmark v31 was stopped first.

## Final normal deployment and handoff

Normal `Ludwerk.Hordewake.Dev_0.8.49.29_x64__wnh08fea6rrsm` installed and opened.
Menu log confirms ~59.9 FPS, High 1920x1080, 5 GB Game budget. Benchmark v31 is
stopped. Device Portal screenshot requests failed with error -2005270523;
preserved `verification/xbox-v29-screenshot-error.json`. Do not describe a new
normal screenshot as visually inspected. Game log remains running near 60 FPS
and reports no device_lost/error entry; this does not establish why Portal capture
failed. Prior v27 ground image, identical shader content and passing terrain
binding gate remain the visual regression evidence, alongside exact bone matrices.

Fresh normal artifacts: Downloads/Hordewake-0.8.49-optimized4-{windows,android,xbox}
and optimized4-windows.zip. Android APK signature checked; never installed on
owner's phone. SHA256 manifest, all test/export logs and measurement comparison:
Downloads/Hordewake-Profile-1500-opt4-results. No PC game launch or pointer capture.

Next candidates remain the native cold PSO compile stalls, Heartbeat/horde work,
and extracting a real intermittent hitch trace. Do not promise a hitch fix from
these one-minute captures; worst frames were slightly worse in new live runs.
Keep all material, texture, animation and game behavior capabilities intact.
