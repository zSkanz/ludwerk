# Hordewake optimization batch 9 - sustained PC investigation

Status: validated on PC; final normal Windows Release/player folder and ZIP delivered.
The user requested several meaningful optimization cycles in one turn. This
batch implements improvements in four areas rather than stopping after one.

Constraints: PC only, windowless/offscreen, no pointer capture. Xbox is off;
phone access remains withdrawn. Neither device was queried or deployed to.
Preserve the large pre-existing dirty engine and game worktrees. Native D3D12
sources are pre-existing untracked work: git diff does not protect them.

## Accepted changes

1. Shared material extraction (`engine/render/src/render_world.cpp`): borrow
   immutable per-frame authored material blocks instead of deep-copying shader
   names and surface values for every part. A deque keeps addresses stable.
   Copy surface values only for actual declared overrides and publish owning
   blocks only for distinct materials. Cache authored surface hashes within
   the frame; preserve equal explicit overrides and signed-zero deduplication.
   No borrowed pointer escapes into the published RenderWorld.
2. SDL_GPU (`engine/rhi/src/sdlgpu/sdlgpu_device.cpp`): retain byte snapshots
   of uniforms per stage/slot and skip identical pushes. Timed command-buffer
   switches replay the snapshots. Skip identical native pipeline binds within
   a render pass; reset the pipeline cache at each new pass.
3. Native D3D12 (`engine/rhi/src/d3d12/commands.{h,cpp}`): reuse identical
   per-recording uniform upload addresses and avoid redundant PSO, graphics
   root, topology and vertex/index view commands. Graphics, compute and blit
   update a common PSO tracker. Resource transitions and root arguments still
   execute for every draw. Snapshot addresses reset with the fenced recording;
   reuse does not extend their lifetime beyond the owning recording.
4. Animation (`engine/render/src/animation.cpp`): omit duplicate own-mesh
   driver entries and merge sorted mesh/driver walks instead of searching for
   every rig. Driver keys sort by instance index, generation and track, matching
   mesh traversal. Existing generation-first pose keys are unchanged. Track
   blend order, culling intervals and pose outputs are preserved.
5. Game (`C:/Users/juanr/Documents/hordewake/src/server/Run/Rules/HordeController.module.luau`):
   native Follow stores the exact facing direction; Visible resolves atan2 only
   when requested. Normal native replication does not call Visible. Held attack
   headings and recycled slots clear pending directions. Non-native movement
   keeps eager angle calculation. Bodies still turn in the engine crowd system.

No geometry, enemy count, resolution, cascades, lighting, shader code, effects
or animation quality settings were reduced. Shared extraction/animation and
game changes apply across supported platforms. SDL changes apply to SDL_GPU
platforms; native D3D12 changes apply only where that backend is enabled.
Physical Xbox/Android performance remains unmeasured in this batch.

## Evidence and reproduction

Evidence: `C:/Users/juanr/Downloads/Hordewake-Heavy-opt9-results`.
Baseline host: `engine-host-opt9-control.exe` beside the development host.
Heavy fixture: `%LOCALAPPDATA%/Ludwerk/build/hordewake-profile-1500-opt2-v1`.
Target 1500 mobs, 12 maximum weapons, Warrior/forest/seed 7, High 1920x1080.
Run command, switching backend and host as appropriate:

```powershell
engine-host.exe <fixture> --headless --frames=5100 --exit --rhi=sdlgpu --width=1920 --height=1080 --frame-stats
```

Profiling begins after simulated 20 seconds and records the remaining 65
simulated seconds. This is not a 65-second wall-clock test. All measured runs
are serial, with no simultaneous compilation/heavy tests. Two controls and two
final candidates per backend. The script switches the fixture's horde module
between exact pre-batch and candidate source; the fixture's old module matches
shipping pre-batch source. Preserve its benchmark-specific ConsoleBenchmark.

The PC GPU is NVIDIA GeForce RTX 4070 Ti SUPER. Both native and SDL_GPU use
D3D12 here, via independent RHI implementations. Comparative hosts are
RelWithDebInfo/dev with GPU validation enabled on both sides. These are
matched development-profile CPU results, not Release FPS guarantees and not
GPU execution timings. Parent scopes are inclusive. Live populations/effects
vary slightly. Synthetic benchmark-result 60 FPS/16.666667 ms fields are invalid
for performance and excluded. Worst frames did not improve consistently; do
not claim that all stutters are fixed.

Evidence includes logs, compare.py/report.py, per-file batch patches, before
copies, final source copies/SHA256 manifest, a source-derived Visible parity
harness, final-comparison.md and final-summary.json.

## Validation

- Windows full render: 282 cases / 103508 assertions pass. Two explicitly
  skipped manual timing benchmarks are not functional test failures.
- Final Windows extraction: 52 cases / 3225 assertions pass. New regression
  covers 300 materials/cache growth, revisiting the first material, parameter
  overrides, ignored parameters, equal overrides/signed zero, live material
  edits and immutable old snapshots.
- Animation: 43 cases / 248 assertions pass, including lower recycled instance
  slots with newer generations, independent tracks and blend results.
- Windows RHI: 31 cases / 1500 assertions pass, no skips. New pixel test covers
  repeated uniforms, edits at the same source address, two pipelines, multiple
  passes/frames and timed command-buffer replay using compiled shipping shaders.
  Existing native tests cover compute, mip/layer views and fenced frame overlap.
- Seven Windows capture gates pass, including terrain texture bindings.
- Linux/Clang build passes. Missing texture initializers in the new regression
  were corrected after Clang diagnosed them. Extraction/animation: 95 cases /
  3473 assertions; RHI: 23 cases / 652 assertions; all pass. Seven Linux capture
  gates pass. Docker's final xvfb-run initially stalled as PID 1 before ctest;
  rerunning with --init passes.
- Actual old/new Visible function source passes 10144 snapshot comparisons,
  including repeated reads, held headings, signed zero and infinite directions.
- All 103 shipping src Luau files pass type analysis using current generated
  .engine/types definitions. First broad invocation used obsolete scratch
  .engine/check definitions and failed on existing gamepad API references;
  correcting the invocation passes. Changed game file passes StyLua.
  This does not claim a green stock whole-project lint: unrelated formatting
  and marketing-copy lint failures from previous batches remain outside scope.
- Clang-format 18 check and touched-file whitespace checks pass.
- Offline UWP engine_rhi_d3d12 library compiles. This is neither a new physical
  console test nor a newly deployed Xbox package.

## Rejected or deferred directions

- Extra SDL sampler/texture caches: SDL already avoids much duplicate backend
  binding; wrapper caches risk resource cycling errors without demonstrated gain.
- Blind Luau native codegen: debugger/local inspection and deterministic math
  need validation first; UWP does not use the same codegen path.
- Fewer shadows/effects or stale shadow caching: would alter requested quality.
- Profiler-overhead tuning: improves measured instrumentation more than normal
  gameplay; not presented as a shipping-game FPS win.
- Physics has a small measured median (~0.3 ms), so it was not the largest
  target. Small unrelated movements in its timing are not a physics code win.

Remaining substantial costs include geometry/shadow submission, game Heartbeat,
crowd simulation and work grouped inside render.lights. Split the latter's
cluster uploads, shadow fit and particle/foliage work before attributing its
whole time to one subsystem. Investigate real long stalls separately: current
warm fixed-window profiles cannot prove the earlier intermittent stall fixed.

## Matched measurements

### SDL_GPU

| CPU scope | Before ms | After ms | Change |
|---|---:|---:|---:|
| frame | 9.6033 | 9.1284 | -4.95% |
| simulation | 3.2952 | 3.1152 | -5.46% |
| scripts.Heartbeat | 1.5918 | 1.4910 | -6.33% |
| animation.sample | 0.7135 | 0.6381 | -10.57% |
| animation.drivers | 0.1715 | 0.1167 | -31.92% |
| animation.poses | 0.3703 | 0.3491 | -5.73% |
| render.extract | 1.0322 | 0.9737 | -5.67% |
| extract.meshes | 0.6225 | 0.5748 | -7.65% |
| render.world | 3.6059 | 3.4188 | -5.19% |
| render.shadows | 1.1977 | 1.0885 | -9.12% |
| render.forward | 1.2378 | 1.1940 | -3.55% |
| render.lights | 0.2862 | 0.2847 | -0.51% |
| physics | 0.2958 | 0.2851 | -3.62% |
| wait.present | 0.5176 | 0.4956 | -4.25% |

| Whole-frame tail | Before ms | After ms |
|---|---:|---:|
| Mean of run p95Ms | 11.2975 | 10.7837 |
| Mean of run worstMs | 17.0689 | 17.1301 |

Worst values are isolated outliers, not proof that all stutters were removed.

### Native D3D12

| CPU scope | Before ms | After ms | Change |
|---|---:|---:|---:|
| frame | 9.8935 | 9.2619 | -6.38% |
| simulation | 3.3059 | 3.1368 | -5.11% |
| scripts.Heartbeat | 1.5956 | 1.4960 | -6.24% |
| animation.sample | 0.7092 | 0.6317 | -10.93% |
| animation.drivers | 0.1683 | 0.1152 | -31.53% |
| animation.poses | 0.3701 | 0.3470 | -6.24% |
| render.extract | 1.0317 | 0.9716 | -5.83% |
| extract.meshes | 0.6300 | 0.5770 | -8.41% |
| render.world | 3.9151 | 3.5492 | -9.35% |
| render.shadows | 1.2469 | 1.0364 | -16.88% |
| render.forward | 1.2077 | 1.1261 | -6.76% |
| render.lights | 0.5608 | 0.5443 | -2.93% |
| physics | 0.3003 | 0.2994 | -0.28% |
| wait.present | 0.3982 | 0.3881 | -2.51% |

| Whole-frame tail | Before ms | After ms |
|---|---:|---:|
| Mean of run p95Ms | 11.5687 | 10.9036 |
| Mean of run worstMs | 16.9784 | 18.1287 |

Worst values are isolated outliers, not proof that all stutters were removed.


## Release/player validation and delivery

A proper Release/player host was built, then ENG_RHI_D3D12 was enabled in its
machine-local CMake cache and the host rebuilt. No repository default was
changed. This host supports default SDL_GPU and optional native D3D12, without
the dev overlay/REPL and without default GPU validation.

The first Release run used the source fixture, whose essence surface was not
compiled for DXIL; it drew an error material. Its preliminary 5.1653/5.3738 ms
medians are invalid and explicitly discarded. Logs remain as
release-source-*-invalid-missing-surface.log; compare.py excludes error runs.
The agent exported the complete benchmark fixture with assetc instead, then
ran the exported executable serially for 5100 frames on each backend. Both
exit 0, load the essence shader's ten shaders/pipelines, keep GPU validation
off and have no error/fatal log entries. These valid Release observations are
one run per backend, not an isolated before/after estimate:

| Release backend | Frame median ms | Frame p95 ms | Worst frame ms |
|---|---:|---:|---:|
| SDL_GPU | 5.1851 | 6.5957 | 9.2636 |
| Native D3D12 | 5.4728 | 6.8034 | 11.4796 |

Do not attribute the dev-to-Release difference solely to this batch's code:
profile configuration and graphics validation differ. All performance numbers
remain PC/offscreen observations, not console/mobile measurements.

Final normal game export uses that Release/player host, with two compiled
custom surfaces. Both exported-game 240-frame backend smoke runs exit 0,
without GPU validation or error/fatal messages. Exported executable PE .text
matches the tested Release host; ZIP executable matches the folder executable.
The archive contains exactly one copy of each entry and places the optional
D3D12 launcher beside Hordewake.exe under Hordewake/.

- Folder: `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized9-windows/`
- ZIP: `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized9-windows.zip`
- Executable: 15805952 bytes; ZIP: 99255549 bytes.
- `Hordewake.exe`: default SDL_GPU.
- `Hordewake-D3D12.cmd`: optional native backend.
- `BUILD-NOTES.txt`: profile/backend notes included in the archive.
- Final artifact hashes: evidence `artifact-sha256.json`.
- Initial dev export hashes retained as `artifact-dev-sha256.json`; that dev
  artifact was superseded by the Release/player delivery.
- Benchmark export: `%LOCALAPPDATA%/Ludwerk/build/hordewake-profile-1500-opt9-release-export`.
  Its executable is Hordewake-Benchmark.exe; launch with Start-Process -Wait
  -PassThru -WindowStyle Hidden, using --headless. Player is a GUI subsystem
  executable: bare PowerShell invocation may return before process completion.
  Capture its working directory's engine.log after it exits.

No phone or console access, deployment, source reset, commit or push occurred.
Further physical validation remains pending device availability. Continue from
this source and the retained controls; do not repeat already-passing checks
without a new change or unresolved concern.
