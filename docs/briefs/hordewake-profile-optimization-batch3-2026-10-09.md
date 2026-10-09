# Optimization batch 3: cold loading investigation

Owner authorized continuing optimization after batch 2. Shared rendering,
simulation quality, game rules and input behavior must remain intact. No agents
spawned, SDK/vendor changes, commits or pushes. Preserve unrelated dirty files.

## Current investigation

Normal Xbox v13 menu startup still stalled 8553.89 ms and lifted loading after
17502.45 ms. `DefaultRenderer::warm` creates seven optional pipeline families
synchronously behind the curtain. Added CPU subscopes per family to determine
whether this is the cause rather than assuming it. No behavior change yet.

Isolated normal-menu fixture (not the heavy stress boot):
`%LOCALAPPDATA%/Ludwerk/build/hordewake-loading-profile-v1`, exported to
`hordewake-loading-profile-export-v1`. Diagnostic menu ClientLoader requests
30 seconds of CPU capture with zero warm-up and emits `[loading-profile-*]`
records. Pointer lock is explicitly disabled in this copy. Original game source
and normal exports are untouched. Preparation script is
`%TEMP%/ludwerk-prepare-loading-profile.py`; build/export logs `%TEMP%/ludwerk-opt3-*`
and `ludwerk-loading-profile-export.log`.

Next: package/run this fixture on Series S in Game mode using established
CN=LudwerkDevelopment publisher, inspect the cold capture, then implement and
validate an optimization supported by the measurements. Do not claim a loading
fix or FPS gain before evidence. Use separate Portal cookie files and sequential
exports from each fixture; preserve all prior captured results.

## Checkpoint: environment arithmetic and cold-pipeline isolation

Shared `environment.cpp` now precomputes local GGX samples per roughness level,
constructs the tangent basis once per texel, and precomputes the BRDF sample
vectors once per row. Sample count, resolution, evaluation and accumulation order
are unchanged. Windows before/after full mesh command captures are byte-identical
(`Downloads/Hordewake-Loading-Profile-results/capture-{before,after}.jsonl`).
Windows full render/scene suites passed before the subsequent diagnostic changes;
Linux environment tests passed 10 cases / 16927 assertions. UWP and Android hosts
built. Fresh final suites/builds are being run; do not substitute these earlier
results for later shader validation.

An out-of-tree serial CPU harness compiled both HEAD environment.cpp and the
working implementation with MSVC /O2. Two alternating nine-run rounds per version,
first sample excluded each round (16 retained), produced medians:

| Kernel | Before ms | After ms |
|---|---:|---:|
| 64x64 BRDF | 76.59635 | 15.30165 |
| Six-level 128-base prefilter | 9.8507 | 6.6360 |

Output checksums match for every corresponding run. This is a serial kernel
microbenchmark, not the threaded game or an FPS improvement. Sources, script and
raw samples: `C:/Users/juanr/Downloads/Hordewake-Environment-Optimization-results`.

Xbox loading profiles v14/v15/v16 isolate the cold stall. **Correction:** the
initial render.prepare parent timing did not establish environment lighting as
the cause. With child scopes, environment worst was 18.87 ms (BRDF 9.63,
irradiance .53, prefilter 8.71), while render.prepare still spent ~8.2 seconds.
In v16, a graphics-pipeline creation directly under render.prepare spent
8156.57 ms, in the terrain forward path; terrain preparation pipelines themselves
were ~97 ms. Evidence directories are Downloads/Hordewake-Loading-Profile-results,
Hordewake-Loading-Profile-after-results and Hordewake-Loading-Profile-detail-results.
Names can repeat under different parents: use the full scope tree, not name alone.

Added prepare subscopes and native D3D12 graphics/compute creation scopes. v17
adds prepare.terrain_forward and experimentally keeps terrain readHexPlane's
three corners in a `[loop]` instead of expanding `[unroll]`. It retains the same
three ordered samples and equations; **this shader experiment is not yet accepted**
until cold timing, runtime cost and visual behavior are checked. No other loop or
terrain quality setting changed. SPIR-V/MSL/DXIL compilation passed. Current v17
isolated loading profile is being captured on the physical Xbox. Never claim the
intermittent gameplay hitch or cold stall fixed based on these findings alone.

## Checkpoint: owner steered investigation toward cold loading

Owner observed unusually slow loading and asked to continue investigating. Then
explicitly withdrew the phone from testing to use it: **do not run ADB, install,
launch, profile or otherwise operate the phone from this checkpoint onward**.
Offline Android builds are allowed. PC tests must not capture the owner's mouse.

A disk PSO-cache experiment was implemented/tested on Windows WARP (8 native
cases / 734 assertions passed, including persisted restore, different target,
corrupted cache and unwritable directory). Physical Xbox LoadingProfile v19,
v20 and v21 failed before menu. v21 reported shader-cache support query S_OK,
flags 99; GetCachedBlob returned 2289696773 (DXGI_ERROR_DEVICE_REMOVED), device
reason 2289696800 (DXGI_ERROR_DRIVER_INTERNAL_ERROR), with no saved cache files.
Do not infer public PC cache API support guarantees this Xbox UWP path works.
The exact failing API boundary has not been proved beyond the cache-enabled
build's failure. **Entire disk-cache implementation, tests and new catalog keys
were removed from the working tree**. Experiment sources are archived outside
the repository in Downloads/Hordewake-Pipeline-Cache-Experiment; raw failed
logs are in Hordewake-Loading-Profile-cache-{first,repeat,cap,explain}-results.
Do not redeploy experimental v19-v21. Normal v18 remains the last normal build.
Reference documentation for the attempted API:
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_cached_pipeline_state
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12pipelinestate-getcachedblob
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_shader_cache_support_flags

Current alternative is shared, material-driven shader permutations, independent
of SDK or device model. `terrain_no_hex.hlsl` includes the original terrain
shader with only the unreachable hex texture-sampling branch compiled out.
Renderer scans **all current view terrains and layers every frame**, choosing
this only when no layer's tiling[2] > .5. Hex remains available through original
terrain shader immediately after an edit or change of view. All normal, height,
paint, rule, texture, lighting and reflection paths remain; no quality tier was
changed. Failure to load/create the variant falls back to original full terrain.
No game code changed.

Physical v22, with cache code removed and no-hex variant enabled, opened normally
and captured 30 seconds: terrain-forward creation worst **4454.22 ms**, versus
v16 original **8156.57 ms** (~45% lower). render.prepare worst 4587.48 ms,
render.world 5209.12 ms. Loading lifted after **14160.62 ms**, versus v16/v17
~17.8 seconds. Menu reached ~60 FPS. User changed graphics to Ultra during/after
this diagnostic; do not claim matched game FPS or exact total-load improvement
from single runs. Native DXIL fragment sizes: original 202644 bytes, no-hex
147900 bytes. These data do **not** establish the intermittent gameplay hitch fixed.
Evidence: Downloads/Hordewake-Loading-Profile-permutation-results.

A further full-quality variant, `terrain_triplanar_no_hex.hlsl`, removes the
planar projection branch only when every layer is triplanar and none is hex.
Original/full and general no-hex variants remain as fallbacks. 132004-byte DXIL
fragment. Current physical v23 loading profile is testing it; results pending.
Private selection helpers: engine/render/src/terrain_variants.h. Two regression
cases in terrain_loader_tests cover multiple terrains, edits, pending maps,
hex threshold and independent height-map/projection flags. Windows tests and
six command-capture goldens passed; Linux build/tests in progress at checkpoint.
Host compilation is fresh UWP, Windows; Android not yet rebuilt after permutations.

Current tools: %TEMP%/ludwerk-loading-triplanar-xbox-run.ps1 (v23 capture),
%TEMP%/ludwerk-opt3-permutation-{windows.cmd,windows.log,linux.log}. Preserve prior
cold records and distinguish inclusive scope trees. After capture, measure the
unchanged 1500-enemy/12-weapon fixture for 60 seconds on Xbox to check runtime
cost; refresh normal exports only after accepting the final permutation.

## Ground texture regression: priority correction

Owner reported missing ground textures on Xbox after terrain permutations.
Stopped Benchmark v25 and LoadingProfile v23; discard v25 performance results
for comparison because visual parity failed. Found a binding-classification bug:
`forward != terrainPipeline_` treated every new full-quality permutation as a
compact/flat shader, omitting its sixteen textures. Corrected classification to
explicit Fast/Flat handles; full-quality permutations must use the original full
bindings including array textures, lighting, debug and lean uniforms. Physical
visual verification and refreshed package pending. Do not accept permutations
based only on shader compilation, selection unit tests or capture goldens that
do not draw terrain. Normal v18 remains the fallback. No phone operations.

## Ground correction validated and deployed (Xbox normal v27)

Root cause proved: full shader permutations entered the Fast/Flat binding branch
because it classified every handle different from the original full handle as
compact. Explicit Fast/Flat comparison now routes both full permutations through
all sixteen textures and original debug/lean uniforms. Shared renderer fix applies
to all backends; no texture, lighting or material feature was removed.

Added `capture_gate_terrain_bindings`, a real terrain scene and a command-stream
assertion requiring sixteen textures before an actual permutation draw. Terrain
meshing is synchronous for command captures, as it already is for screenshots,
so a fast headless run cannot silently test an empty world. Production scheduling
is unchanged. Proved the test by temporarily compiling the broken branch: it
failed with `Full terrain draw bound 0 textures; expected all sixteen.` Restored
the correction and all seven capture gates passed Windows and Linux. Do not count
an earlier LNK1168 file-lock build failure as the negative proof; the archived
negative test log contains the actual assertion. Two selection tests (12 asserts)
also pass; full Windows render suite earlier passed 278 cases/100688 assertions.
Clang-format 18 and StyLua passed on changed code/new fixture; Android offline
native rebuild passed. No phone operations occurred.

Corrected unchanged heavy fixture packaged/deployed as Benchmark v26:
- 60.011562 s, 3135 frames, 52.239933 FPS; prior opt2 Xbox 52.3183 FPS.
- Mean 19.142444 ms, best 13.237509, p99 28.770842, worst 33.057427 ms.
- Target 1500, actual mean 1497.043 (min 1424/max 1500); twelve max-level weapons.
- Mean draws 648.216; High, 1920x1080, VSync false, 5 GB Game budget.
- No reported >50 or >100 ms frames in that measured minute. This is not proof
  the owner's intermittent gameplay hitch is fixed. General FPS is unchanged.
- Screenshot taken only AFTER the completed measured minute.
Evidence: Downloads/Hordewake-Profile-1500-opt3-fixed-results, including raw v26
log, screenshot, passing capture stream, Windows gates and negative proof.
Discard earlier v25 run; it lacked full terrain bindings and is not comparable.

Normal corrected game is installed/open as
`Ludwerk.Hordewake.Dev_0.8.49.27_x64__wnh08fea6rrsm`.
APPX: Downloads/Hordewake-0.8.49-optimized3-xbox/Ludwerk.Hordewake.Dev-0.8.49.27-x64.appx.
Portal screenshot `verification/xbox-v27-menu.jpg` visibly shows grass and textured
dirt around the campfire. `verification/xbox-v27-engine.log`: menu ~59.9-60.2 FPS,
High 1920x1080, loading curtain 13741.588 ms. Earlier original captures were ~17.8 s,
but different settings/runtime conditions mean this is an observation rather than
an exact controlled end-to-end percentage. Permutation PSO creation ~3.69 s vs
original ~8.16 s is still a cold-start stall, not eliminated. Phone forbidden.

Windows and Android normal artifacts in optimized3 folders are being refreshed
with corrected renderer; no PC game launch or pointer capture. New Xbox package
uses the corrected native renderer and shader content; the later capture-only
scheduling change does not affect physical gameplay.

Final artifact checkpoint: Windows normal export and optimized3 Windows ZIP are
refreshed successfully. Android normal export APK is refreshed and signature
verified, built offline only; no installation or physical Android visual claim.
Corrected exports remain in Downloads/Hordewake-0.8.49-optimized3-{windows,android,xbox}.
SHA256 manifest and export logs: Downloads/Hordewake-Profile-1500-opt3-fixed-results.
Xbox normal v27 remains open at the menu; no further console benchmark running.
All source branches are restored to corrected code after the negative regression
experiment; never deploy diagnostic `engine-host-negative-proof.exe` in the private
Windows build directory. Normal exports use the corrected main host.
Next work: remaining cold native PSO compilation and real intermittent hitch;
measure root causes without reducing material/shader quality. Do not use phone.

## Next iteration: frame-local shared skin palette reuse (in progress)

Owner authorized further optimization. Latest heavy Xbox profile: extract.meshes
~1.8434 ms; animation already shares immutable Pose pointers for identical inputs,
but extraction copies each palette again for every rig. New frame-local pointer
lookup keeps one bone range per existing shared pose, in first-seen output order.
No animation samples, detail levels, texture bindings or per-instance transforms
change. Independent poses and copy-on-write ragdoll overrides must remain separate.
New regression test verifies matrix values for two shared rigs, an independent
moment, a bind-pose rig, multiple sections, another tick and an individual override.
Changes are not yet accepted; tests and physical heavy comparison pending.
Phone forbidden, PC pointer untouched. Existing normal Xbox v27 is still fallback.
