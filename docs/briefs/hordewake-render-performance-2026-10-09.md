# Engine and Hordewake render performance

Owner authorized engine and game optimizations while preserving quality, API
capability and established style. Aim for competitive performance; parity with
Unity/Unreal/Godot requires matched workloads and is not established by this work.
No new PC game windows or mouse capture. Actual benchmark target is Series S.

## Baseline and acceptance

Physical Series S, UWP Game5120MiB,1080p high, render scale1, frame generation
off, four fixed-level workloads,15s warm-up+30s sample, VSync ON then OFF.
Baseline package0.8.49.5 and raw results remain in Downloads/Hordewake-Benchmark-xbox/results.
ON FPS:59.91/51.90/46.41/39.38; OFF:58.85/50.73/45.29/38.29 for
100/300/550/1000 mobs with1/6/6/12 weapons. No measured frame>50ms, worst33.89ms.
See hordewake-series-s-benchmark-results-2026-10-09.md for actual population/tails.
Do not take Portal screenshots during measured samples: prior captures confounded
GPU waits by ~500ms. Capture only after benchmark-complete.

## First implementation: native queue overlap

The D3D12 adapter currently drains the queue in both Resources::collect and
Pipelines::collect every frame. Upload pages and shader-visible heaps are global,
so removing the waits alone would corrupt in-flight GPU data.

Replace with completed-fence retirement for GPU objects and upload-page reuse;
use descriptor heaps per existing Context frame slot. Context::begin already
waits only for that slot's last submission. Keep blocking waits for explicit
readback, resize/detach and teardown. Ensure recording-time retirements protect
the not-yet-submitted command list. Test the queue blocked by a CPU-signaled gate
to prove uploads/descriptors and destroyed objects survive in-flight frames.
No game settings, shadows, resolution or effects are reduced by this change.

Required verification: headless Windows native WARP command/readback/compute tests,
UWP Release build, static formatting, actual same eight console benchmark cases.
Retain baseline data and record any change in FPS, tails, actual counts and memory.
If behavior regresses, diagnose before keeping the candidate.

## Next investigations after measurements

Profile CPU recording and GPU work separately. Audit binding/descriptor churn,
draw grouping, shadows/particles and game allocations/reuse where measured costs
justify it. Do not assume scripts dominate from total frame time. Native D3D12
improvements apply to this optional Windows/UWP backend; default SDL_GPU and
Android must be checked independently before claiming cross-platform gains.

## Checkpoint

First optimization batch implemented and all eight hardware cases completed.
See [before/after results](hordewake-render-performance-results-2026-10-09.md).
Benchmark v0.8.49.6 measured and stopped; normal game v0.8.49.7 deployed and
opened successfully with the 5120 MiB Game budget, menu samples 60.2/59.9 FPS.
This is native D3D12 progress, not completion of all cross-platform optimization.

## Candidate v0.8.49.6 verification checkpoint

Implemented completed-fence retirement, upload-page fence ownership, three
shader-visible descriptor heap slots, and per-slot reset after Context::begin.
Surface resize/detach still explicitly drains before releasing borrowed targets.
An open command list reserves its retirement serial; waitIdle while recording
waits for submitted work without consuming that serial.

Windows native D3D12 tests passed (6 cases / 666 assertions finally), including
three submissions behind a CPU-signaled queue gate, distinct texture pixels,
compute uniform snapshots, destroyed resources/pipelines and readback. Full RHI
passed 28 cases / 1137 assertions. Extended the gate test to cover recording-time
waitIdle serial preservation; rerun passed. UWP Release /W4 /WX build and signed
APPX creation passed. No PC game/window was launched.

Candidate APPX: Downloads/Hordewake-Benchmark-xbox/
Ludwerk.Hordewake.Benchmark-0.8.49.6-x64.appx.
SHA256:367963fde7514f386ae869782a39166a7bb75362ed991774e63becda766f8d74.
Same sealed game export and shaders as v5 baseline; only native queue ownership
changed. Portal deployment completed with Success true; launched with Game
budget5368709120 bytes. Wait for deploy-state completion before launching: during
installation the default-account staged package is visible but not ready. Logs
are shared across package revisions; never mistake old benchmark-complete for a
fresh candidate result.

Completed collection ran without screenshots, using
%TEMP%/ludwerk-xbox-overlap-collect.ps1. Candidate logs are isolated in
Downloads/Hordewake-Benchmark-xbox/results/optimization-v6/
series-s-v6-clean-engine.log. Baseline v5 data remains untouched. All eight
results are saved as JSON/CSV plus a comparison report. ON FPS for
100/300/550/1000 mobs:59.94/59.94/59.71/55.83; OFF:74.93/67.44/61.84/54.93.
Largest gain:43.4%. No measured sample frame above50ms; four above33.333ms,
all in OFF300/550 cases (their worst frames rose to33.43/33.37ms despite better
p99). Peak sampled process1000.0MiB versus baseline996.8MiB. No quality settings
changed. Actual means99.9/299.3/547.7-547.9/998.1 remain comparable to baseline.
An AFTER-measurement screenshot was saved and visually checked: mob horde,
boss, shadows, particles and weapon effects render without apparent corruption.
Loading/scene-transition spikes remain outside these samples and are not claimed
solved; neither are the owner's independent PC/mobile hitches. OFF averages above
60 describe engine frame production, not proof of physical120Hz presentation.

Normal package v0.8.49.7 includes the queue optimization and prior UWP XAudio2
startup fix, using the unchanged normal-game export, not the stress fixture.
SHA256:f59e816a120af9306a0a5da333107ad6a9cfbcb0d7cd68461258bebab0a11914.
Downloads/Hordewake-0.8.49-xbox/Ludwerk.Hordewake.Dev-0.8.49.7-x64.appx.

## Remaining performance work

- Capture CPU recording and GPU pass costs in the overload case before choosing
  another render change. Gameplay scopes at1000 mobs show run~2.45ms, including
  horde~1.68ms and follow~1.36ms (nested, not additive). This is not a full CPU/GPU
  breakdown and does not justify rewriting game logic on its own.
- Audit of SDL_GPU begin/submit found bounded in-flight fences already present;
  its per-pass timing mode intentionally drains and must not be used as normal
  gameplay performance. Do not copy the native D3D12 diagnosis onto SDL/mobile.
- Capture shared renderer/script allocation and first-use spikes on PC without
  launching a window that grabs the owner's mouse. Android hardware is not
  attached; compilation alone cannot establish mobile frame-time improvements.
- Normal v7 remains open for the owner's interactive controller testing. Saved
  normal-v7-startup-engine.log and normal-v7-player-entry.txt beside its APPX.
  No error/device-lost lines in startup; no new OS crash dump after benchmark.
  Cold loading still took18.53s, including9.31s main-thread shader/content work,
  and logged a loading-readiness warning with0 outstanding holds. Investigate
  loading responsiveness/readiness separately; menu samples after loading are
  60.2/59.9 FPS, worst22.37/17.30ms. Do not count cold preparation as gameplay or
  claim it solved. No new controller interaction was automated in normal v7;
  owner confirmations for cards/pause belong to earlier game versions.

## Second batch: shared particle texture palettes

Owner authorized more optimization and asked for cross-platform applicability.
The native queue fix is Windows/native D3D12/UWP only. New candidate changes the
shared renderer and particle shader, used by SDL_GPU and native D3D12 alike.

Observed hardware logs: the extreme stage issues hundreds of particle draws,
e.g.877 of1517 total in the last v6 sample. Existing batching breaks on every
texture change to preserve alpha ordering. New batches hold seven distinct
pictures plus scene depth in eight explicit sampler bindings; procedural shapes
use slot0 and consume no texture slot. Particles remain in their original depth
order, with identical instance stream size, opacity, light/fog, UVs and effects.
No texture atlas resampling, bindless feature or game-specific branch is added.
Static HLSL resource branches use SampleGrad with derivatives computed before
branching; the GPU-simulated emitter variant retains its two-texture layout.

Changed: renderer_default.cpp, private particle_batches.h, shader_types.h comment,
shaders/src/particle.hlsl, particle batching unit cases, native D3D12 pixel test.
Shaders compile to DXIL/SPIR-V/MSL; reflection reports8 CPU/2 GPU samplers.
Pixel test covers all seven slots and overlapping7/2/7 half-alpha instances in one
draw. First run exposed only a test clear-alpha assumption (default clear alpha1),
corrected to transparent clear; full tests are now running. Do not claim final
validation or console gains until the runs below finish.

Build sessions: Windows particle tests7261, Linux render build75987, UWP55530.
Logs in %TEMP%/ludwerk-particle-tests.log, ludwerk-particle-linux-build.log and
ludwerk-uwp-map.log. Keep prior v5/v6 hardware results untouched. Next: finish
Windows/Linux/Android validation, package benchmark0.8.49.7 using the same sealed
export but NEW shader content, deploy and run all eight clean console stages,
save under results/particles-v7, compare against queue-overlap v6 and baseline v5.
Normal game remains v0.8.49.7 until candidate passes; next normal revision is8.
No PC game window or mouse capture. Android hardware is unavailable: compile and
SPIR-V validation cannot prove actual device FPS or rendering correctness there.

Second-batch validation completed: Windows and Linux full particles source file
18 cases/1601 assertions each; full Windows RHI29/1181, no skipped cases; Android
ARM64 engine_render and UWP Release compile; DXIL/SPIR-V/MSL generated; CPU
reflection8 samplers, GPU-simulated particle reflection2. Formatting passed.
Benchmark v7 SHA256:56294e3f45237d76811d10faf3637692bf353038cc198c1b9b4445b7344abe52.
Deployment succeeded, startup log is fresh, Game budget5368709120 bytes. Clean
collector12893 now running, no screenshots until all eight stages complete.

Additional shared-backend verification: built the full Android ARM64 engine_host
(libmain.so), not just engine_render. Ran a copied example16 fixture with three
solid test textures plus procedural particles through SDL_GPU headlessly for180
frames/179ticks on NVIDIA4070TiSUPER/D3D12 and IntelUHD770/Vulkan. Both exit0,
write valid PNGs and visually match. Vulkan logs lack of VK_EXT_headless_surface,
but offscreen rendering/readback succeeds; no window/surface was required. This
is a functional check, not a PC performance benchmark. Fixture is independent
copies in build/particle-palette-sdl-check, no source-game mutation or mouse capture.

Second hardware batch complete (collector12893 exit0). All eight fresh records,
JSON/CSV/comparison and raw log saved in Downloads/Hordewake-Benchmark-xbox/
results/particles-v7. [Shared particle results](hordewake-particle-performance-results-2026-10-09.md).
ON100/300/550/1000 FPS=59.94/59.94/59.77/56.17; OFF=74.92/67.64/62.05/56.17.
Mean draws ON573->485,764->555,982->583,1392->604; extreme OFF1401->606,
56.7% fewer. Additional FPS improvement is small (extreme OFF~2.3%); do not
present draw-call reduction as FPS gain or claim other bottlenecks solved.
Six frames>33.333ms, none>50ms; OFF550 worst39.04ms versus previous33.37ms,
p99=21.02 versus20.91ms. Keep this tail regression visible; one run cannot
attribute its cause. Peak1001.2MiB versus prior1000.0MiB. Screenshot only after
measurement. Normal v8 packaged and deploying with unchanged normal game export.
Normal APPX SHA256:7dde8756cfc50fa140c2679dbf3eed184c4e8c1b9f4a1b3a69088332f2f9c642.

Normal v0.8.49.8 deployment completed (Portal200 Success true) and launched with
5368709120-byte Game budget. Benchmark stopped after collection. No new OS crash
dump (only the old benchmarkv3 WASAPI dump remains). AFTER-measurement overload
screenshot visually checked: mob horde/boss, shadows, transparent effects and
weapon effects intact; different game time is not a pixel comparison against
baseline. Normal entry/startup logs are saved beside its APPX in Downloads.

Next bottleneck work: measure CPU recording and GPU passes separately before
another change; large draw-count reduction produced only modest FPS improvement.
Keep OFF550 tail (39.04ms) and cold-loading responsiveness/readiness in the queue.
No shader-quality tradeoff or game logic change was made in this batch. Existing
Windows/Android distributed builds require rebuilding/re-exporting to receive the
new common renderer and shader blobs; Android compilation is not physical QA.

Normal v8 startup confirmed: loading lifted at18.71s; two subsequent menu samples59.9FPS, worst16.98/16.96ms. Startup readiness warning remains; no new error/device-loss line. Leave normal game open. Baseline/queue-only/shared-palette reports remain separate.
