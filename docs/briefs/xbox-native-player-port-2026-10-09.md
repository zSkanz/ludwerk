# Native UWP player port

Owner request (2026-10-09): port the real player so Hordewake can be tested on
the Series S in Developer Mode. This is authorized implementation and console
deployment work. Preserve pre-existing changes in the engine and game; no push.

## Current checkpoint

The real Hordewake player now runs on the owner's Xbox Series S. Native UWP
APPX `Ludwerk.Hordewake.Dev_0.8.49.3_x64__wnh08fea6rrsm` boots the exported
0.8.49 content pack, executes the game's Luau scripts and displays the actual
menu, animated hero, camp, particles, fonts and UI through native D3D12.
Device Portal screenshot was inspected (`%TEMP%/hordewake-xbox-v3-loaded.png`).

The owner changed Hordewake to **Game** in Dev Home. Relaunch measured a
5120 MiB app budget and approximately 61.7 FPS in the 1920x1080 high-quality menu.
The owner confirmed entering a match and playing with the physical controller.
The original App classification had limited the app to 1024 MiB and about 7.7 FPS.
Native backend retirement remains conservative (GPU waits each frame).

Follow-up defects reported by the owner: a brief gameplay freeze, and inability
to confirm cards or pause buttons. These are being corrected before completion:

- The preserved hardware `engine.previous.log` (`%TEMP%/hordewake-xbox-before-relaunch.log`)
  records a 932.5 ms frame, of which 910.2 ms was first-use creation of the essence
  surface pipelines; simulation was 1.7 ms. Extend existing ContentProvider
  preloading to `.surface.hlsl` URNs, sharing the renderer's normal pipeline cache.
  Hordewake explicitly preloads essence's shader during its loading curtain.
- Gameplay InputContext remained enabled under modals, so UiNavigation correctly
  reserved ButtonSouth and LeftThumbstick for gameplay. Hordewake now disables its
  gameplay context while cards, pause, stats or the end screen own input, restores
  it on return and suppresses held buttons until release. First card / Continue
  get focus for gamepad; focus loss / loss of the last active controller pauses.
- Deployed **0.8.49.5** includes the modal fixes and surface preloading, from a
  fresh sealed export of the original game's current scripts. Original Windows /
  Android 0.8.49 releases remain untouched. Owner subsequently confirmed playing
  with cards AND pause working correctly on the physical controller.
- The update returned to App classification. After the owner selected Game,
  several launches exited with DXGI_ERROR_DEVICE_REMOVED before the menu. A later
  launch of the SAME v5 package passed startup and reported 5120 MiB; see
  `%TEMP%/hordewake-xbox-v5-retry.log`. Do not claim a source fix caused recovery.
  Preserve the failed logs `%TEMP%/hordewake-xbox-v5-engine.log` and `.previous.log`.
- Source after v5 removes the recently added optimized-clear metadata and adds
  operation + GetDeviceRemovedReason diagnostics to native device failures.
  Both builds and full RHI tests pass. This candidate has NOT been packaged /
  deployed; do not interrupt the owner's v5 tests just to install diagnostics.

Build/package: `scripts/package-xbox-uwp-player.ps1`, template
`platforms/uwp/Player.AppxManifest.xml.in`, out-of-tree `uwp-x64-player` build.
Development artifacts: `%USERPROFILE%/Downloads/Hordewake-0.8.49-xbox/`.
SDKs/GDK are not embedded; private signing key remains non-exportable in the
current user's certificate store. Original circle/renderer probes and saves
are preserved.

Verified this continuation: RHI 27 cases / 1008 assertions; native UWP async IO
13 / 1678; desktop platform 56 / 2217; sealed-game script regression 1 / 13.
The first console build exposed two genuine AppContainer bugs, both fixed:
logs must use app-local storage rather than current_path; virtual packed script
folders must use lexically_relative rather than filesystem canonicalization.
UWP TLS uses Windows.Web.Http, with bounded streaming and OS certificate checks;
network multiplayer remains compiled, SDL and unused native Luau CodeGen are
excluded only from the opt-in UWP graph.

Additional regression evidence (2026-10-09):

- Full Linux gate: 145 CTest tests, 1642 conformance cases and three hot-reload
  checks passed (`%TEMP%/ludwerk-uwp-linux-gate.log`). After the preload/modal
  changes, incremental Linux build and focused host tests passed: 5 cases /
  64 assertions (`%TEMP%/ludwerk-linux-modal-tests-direct.log`). An xvfb-run
  wrapper stalled before executing the binary; that owned test container was
  stopped, and the direct SDL dummy-driver run completed. No test failure hidden.
- Windows focused host tests: 5 / 64, full RHI 27 / 1008. Real-game gamepad
  regression passed; card desktop / compact tests, nested pause, price checks,
  profiler cleanup, music switching and 600-enemy/boss stress passed.
- Real D3D12 game sequence with validation: menu -> lobby -> run -> shutdown,
  2066 frames / 2100 ticks. Essence pipeline creation occurs BEFORE run's loading
  curtain lifts; no GPU validation or script errors. Gameplay ~60 FPS at 720p.
  `%TEMP%/hordewake-native-d3d12-prewarm.log`.
- Hardware v5 App-mode trace also prepares essence pipelines (911.9 ms) before
  the run curtain lifts. This validates moving the cost, not GPU compilation
  elimination. Need final Game-mode gameplay profile after physical retest.
- Game source analysis reports no type errors; full source CLI check is still
  nonzero for existing formatting in CharacterSettings, UnlockSettings and
  WorldController. Full project check additionally encounters two unused variables
  in marketing capture copies. The five changed runtime files pass StyLua.
  Do not report a completely green full game check.
- Docs gate, API definition lint and module layers passed. API documentation /
  dump / types regenerated for the existing ContentProvider preload behavior.

New follow-up after physical v5 modal confirmation: the owner reported another
strong gameplay hitch. `%TEMP%/hordewake-xbox-v5-gameplay-confirmed.log` shows
approximately 525 ms frames (about 519 ms in GPU/display wait; only 2.6 ms sim),
also periodically present in the menu. Essence prewarming was 938.8 ms under
the run curtain and is NOT this new gameplay hitch. Steady gameplay ~59.5 FPS
at 1080p high / 5 GiB. No script or device-loss error in that successful session.
Owner was asked to enable VSync in Pause -> Graphics for a one-minute A/B test;
await answer / read logs before assuming VSync fixes it.

Candidate source additionally synchronizes input-device availability AFTER
platform::pumpEvents, so disconnected handlers see the current availability
rather than a frame-old snapshot. Native slow operations now log separately
(resource retirement / pipeline retirement / begin frame / submit+present).
No candidate deployment yet; v5 is the physically tested build.

Remaining: explicit held-A check, controller
disconnect pause (verify availability/event ordering), lifecycle/save round-trip,
stable Game-mode match profile. UWP pointer-lock warning on gamepad-only devices
is a remaining misleading diagnostic; no mouse backend exists in this Xbox port.
Do not label this a perfect or complete console port while these are pending.

The following renderer-check evidence is an earlier prerequisite checkpoint.
Hardware evidence from `renderer-check.log`, version 0.1.0.10:

```text
NATIVE_D3D12_DEVICE_READY
SHADER_MODEL=96 QUERY_HRESULT=0
ADAPTER=SraKmd_arden SOFTWARE=0
NATIVE_D3D12_SWAPCHAIN_READY
SHIPPING_DXIL_PIPELINE_HANDLE=1
NATIVE_RHI_RESOURCES_AND_LAYOUT_READY
NATIVE_D3D12_SHIPPING_SHADERS_READY
NATIVE_D3D12_PRESENTED_120_FRAMES
```

96 is 0x60 / Shader Model 6.0. No software rasterizer was used on the console.
The native factory/device imports work through WindowsApp.lib. The early
renderer-check startup bug was initializing a single-threaded apartment before
the exception handler; use `winrt::init_apartment()` inside the handler, as the
working CoreApplication probe does. A diagnostic LoadPackagedLibrary experiment
refused the system graphics DLL (error 126); it was removed. Do not infer that
D3D12 is missing on Xbox from that loader experiment.

## Code and verification

- `engine/rhi/src/d3d12/context.{h,cpp}`: native D3D12 device, queue, CoreWindow
  swapchain, three command allocators, per-frame fence reuse, resize, suspension,
  finite fence polling with device-removal detection, explicit resource teardown.
- `engine/rhi/src/d3d12/resources.{h,cpp}`: typed monotonically allocated buffer
  and texture handles, default-heap resources, depth/array/mip target views,
  per-subresource barriers, aligned reusable upload pages, partial texture
  updates, blocking buffer/texture readback, deferred destruction and memory
  accounting. `collect()` currently waits idle between frames; the final adapter
  should retire/reuse pages per frame fence for overlap rather than call it on
  every draw. It cannot be called during a recording.
- `engine/rhi/src/d3d12/pipelines.{h,cpp}`: reflected graphics/compute root
  signatures, native pipeline states, vertex layouts, MRT/depth/blend states
  and deferred pipeline destruction, using the existing DXIL register spaces.
- `engine/rhi/src/d3d12/descriptors.{h,cpp}`: shader-visible descriptor heaps,
  per-draw snapshots, cached sampler tables, copied/aligned frame uniforms,
  sampled/storage textures and raw SRV/UAV buffers. UAV usage and mip bounds are
  validated before recording native views; exhaustion returns an error.
- `engine/rhi/src/d3d12/commands.{h,cpp}` now implements the existing `ICmdList`
  privately: render passes, graphics bindings, vertex/index/indirect draws,
  transfers, compute passes, copied uniforms and UAV ordering. **Not a selectable
  backend yet.** Filtered blits and mip generation explicitly report `E_NOTIMPL`;
  the owner must inspect `status()` before submission. Finish those operations
  and the `IDevice` adapter before exposing this implementation to the player.
- Windows RHI suite: **26 cases, 989 assertions passed, no skips**. This includes
  **4 native D3D12 cases / 518 assertions**. Actual WARP D3D12
  execution checks allocator reuse, GPU buffer copies, odd-width padded texture
  rows, partial updates, rendered clear pixels, bounds refusal, stale handles,
  deferred destruction and accounting. Shipping fullscreen DXIL renders two
  independently sampled halves, one through indexed indirect execution, checking
  descriptor snapshots and actual pixel readback. Shipping `foliage_finalize`
  compute DXIL checks native storage bindings, copied uniform lifetime and ordered
  repeated UAV writes against expected indirect arguments. Invalid binding ranges
  and UAV usage are refused. Terminal `Context::abandon()` discards unsubmitted
  commands; readback verifies shutdown does not execute the discarded copy.
  This is not a mock implementation.
- UWP compiler: context/resources/pipelines/descriptors/commands/renderer check built under `/W4 /WX` and
  `WINAPI_FAMILY_APP`. Native engine context also uses normal warning gates.
- `platforms/uwp/renderer_check.cpp`: isolated native graphics integration
  validation, existing `debug_line` vertex/fragment DXIL, own APPX identity.
- `scripts/package-xbox-uwp-probe.ps1 -NativeRenderer -ShaderContent <host-content>`
  builds and signs that check. Original probe mode/output is preserved.

Build outputs: `%LOCALAPPDATA%/Ludwerk/build/uwp-x64-probe` and
`%LOCALAPPDATA%/Ludwerk/build/xbox-native-renderer-check`.
Renderer APPX identity: `Engine.UwpRendererCheck_0.1.0.10_x64__3397hj3nyz3y8`;
AUMID: `Engine.UwpRendererCheck_3397hj3nyz3y8!RendererCheck`.
Original `Engine.UwpProbe` installation and saved state were not replaced.
Console URL remains the owner's local `https://192.168.15.32:11443`.
Local logs: `%TEMP%/ludwerk-native-d3d12-{build,uwp,package,xbox}.log`.
Certificate private keys stay in the user certificate store.

Hardware verification is version 0.1.0.10's resource/pipeline path. The newer
`ICmdList` command encoder is compiled for AppContainer and exercised by local GPU
tests; it has not yet replaced the direct commands in the installed validation
app. Do not report the new compute/indirect tests as physical Xbox tests.

Owner-facing estimate on continuation: **approximately 35% toward first full
Hordewake execution**, an engineering estimate by milestones, not measured code
coverage or a promise of remaining duration. The full game is still not runnable
on Xbox; most integration risk remains in the steps below.

## Remaining implementation, in order

### Active continuation after owner requested completion

- Added the `rhi_blit` host-compiled shader and native restricted mip/layer views.
  Filtered blits preserve RGBA; array mip generation renders each mip from only
  its preceding mip, with isolated subresource states. No `E_NOTIMPL` remains in
  the command encoder. Five native GPU cases / 537 assertions passed.
- Added opt-in `ENG_RHI_D3D12`, native `IDevice` and Windows presentation interop.
  Device uses direct backbuffer imports (no full-frame proxy copy), retires imports
  before resize, reports loss and rejects recording failures. Single primary
  window; SDL remains the desktop/editor default. Upload/descriptor reuse still
  fences between frames and needs profiling on console before claiming performance.
- **Actual Hordewake menu rendered with the new native backend on Windows**, 120
  frames, GPU debug enabled, exit 0; screenshot inspected. Command:
  `engine-host.exe <hordewake-project> --headless --frames=120 --exit --rhi=d3d12 --gpu-debug --screenshot=<png>`.
  Evidence: `%TEMP%/hordewake-native-d3d12-menu.{log,png}`.
- Added explicit `ENG_BUILD_UWP_PLAYER` root build mode. Native platform backend,
  CoreApplication entry into the existing `engineHostMain`, CoreWindow/WGI input,
  sandboxed filesystem/mapping, background/save deferral, native asynchronous IO
  and honest unavailable desktop-tool capabilities. Shared event-name/path-name
  tables extracted without changing desktop vocabulary. No vendor files edited.
- Actual out-of-tree UWP player graph is compiling at
  `%LOCALAPPDATA%/Ludwerk/build/uwp-x64-player`; log
  `%TEMP%/ludwerk-uwp-player-build.log`, temporary build helper
  `%TEMP%/ludwerk-uwp-player-build.cmd`. Requires the EXISTING cross-build setting
  `ENG_HOST_LUAUEMBED=<win-msvc-dev/engine/script/engine_luauembed.exe>`.
  First graph reached native platform/render/Jolt successfully; host builtin
  embedding path corrected. Full link/package/console game execution still pending.
- Added desktop tests reusing the existing asynchronous IO suite against native
  IO sources (`engine_platform_uwp_io_tests`); not yet built/run. Must verify
  callbacks, cancellation, priorities, memory ceiling and stale generations.
- NEXT: resolve actual UWP compile/link diagnostics, run native IO contract tests
  and desktop backend regression, package existing sealed Hordewake export with
  the new player + updated shader pack, deploy through the authorized Device Portal,
  inspect player logs and test real game on Series S. Existing 0.8.49 releases and
  original probe remain untouched. Historical list below is partially superseded
  by this active checkpoint; do not treat the incomplete build as a shipped game.

1. Complete the native RHI adapter: reflected graphics/compute root layouts,
   pipelines, descriptor binding, uniforms, draws/indirect draws, mip generation,
   scaled blits, swapchain acquisition, lost-device behavior and fenced retirement.
   Reuse the existing RHI descriptors and DXIL resource spaces. Do not expose a
   selectable backend whose methods silently discard operations.
2. Integrate the adapter with the existing engine renderer and verify a real
   offscreen scene/game on Windows before putting the full runtime in UWP.
3. Implement the UWP platform backend/CoreApplication host: CoreWindow events,
   Windows.Gaming.Input into existing InputSystem/UiNavigation, background/focus
   release, suspension/resume, installed assets and writable LocalFolder paths.
4. Compile the actual runtime dependency graph for AppContainer: Jolt, assets,
   Luau, audio, networking, scene/render/UI, without SDL desktop/video, editor,
   shader compiler, unrestricted filesystem assumptions or console SDKs.
5. Run an actual engine scene on Xbox; then package Hordewake through the existing
   exporter/module framework. Validate menu, lobby, gameplay, cards, pause,
   reconnect, saves, suspension and device loss. Physical tests must be identified
   separately from synthetic action regressions.

Shader binding contract verified in vendored SDL D3D12 source: vertex SRV/sampler
space 0, vertex CBV space 1; fragment SRV/sampler space 2, fragment CBV space 3.
Compute SRV/sampler space 0, UAV space 1, CBV space 2. Raw storage buffers use
R32_TYPELESS with raw view flags. No SDL enums should enter the public API.

References: [CoreWindow swapchains](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcorewindow),
[D3D12 environment](https://learn.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-environment-set-up),
[shader models](https://learn.microsoft.com/en-us/windows/uwp/gaming/tutorial--assembling-the-rendering-pipeline).

## Hardware follow-up: lobby transition and owner instructions

Owner physically confirmed selecting Play with the controller. First console
run then stayed on loading. Evidence showed DEVICE_REMOVED (2289696773 /
0x887a0005) and an existing app-loop bug: a null beginFrame continued before
checking device loss, producing bogus ~75,000 FPS logs with a frozen picture.
Fixed that early branch to exit on loss and briefly sleep when unavailable.
The native adapter now logs the first failing HRESULT. v0.8.49.4 contains this
fix, UI-thread marshalled suspend/resume and a GPU drain before completing the
suspension deferral. Raw UTF-8 D3D12 BeginEvent payloads were removed (they are
not valid PIX encoding); nesting validation remains.

**Do not relaunch while the owner is changing App type in Dev Home.** They
asked for researched instructions, which were provided: close the sidebar with
B, highlight Hordewake in Games and apps, press View (overlapping squares),
View details, App type -> Game, confirm A, B to return. Source with screenshots:
https://walbourn.github.io/directx-and-uwp-on-xbox-one/
Game mode is not yet confirmed. v4 still reports 1024 MiB; its previous log
records a failed look_focus_prepare pipeline and device removal. Do not claim
that removing markers fixed the driver issue. Remeasure only after Game mode.

Windows native D3D12 real-game sequence was exercised from an isolated copy:
menu -> lobby -> run, 2005 frames / 2100 ticks, with --gpu-debug. Correct test
scene data uses numeric Map=1 (not a biome string). No GPU validation errors
or script errors in the corrected run; gameplay sustained ~60 FPS at 1280x720.
Evidence: %TEMP%/hordewake-native-d3d12-transition-valid.log and .png.
Only the isolated copy has NativePortValidation.luau; original game unchanged.

Latest source (after packaged v4) additionally sets proper non-comparison
sampler state and optimized texture clear values; both Windows and UWP builds
passed, RHI still 27/1008. This source is not yet deployed. Package next revision
with -PublisherSubject CN=LudwerkDevelopment to preserve the current test
package family/certificate. The generic packager defaults to CN=EngineDevelopment.
Docs lint passed; layer gate 16 modules / 0 violations; Linux full build is
running through localgate (log %TEMP%/ludwerk-uwp-linux-gate.log).

## Cross-platform hitch investigation (owner follow-up)

Owner confirms the spontaneous strong hitch also occurs on PC and mobile; this
is a shared game/engine performance issue, not an Xbox-only acceptance item.
The VSync-on A/B on Xbox reproduced the same approximately 0.52 s waits; see
`%TEMP%/hordewake-xbox-v5-vsync-enabled.log`. VSync is not a demonstrated fix.
Candidate 0.8.49.6 was packaged/signed at
`%LOCALAPPDATA%/Ludwerk/build/hordewake-xbox-diagnostic/` but not deployed yet.
Additional source instrumentation now splits long waits in the shared player
into frame cap, present pacing, begin-frame, swapchain acquisition and submission.
It records requested sleep separately, so the existing combined wait bucket
cannot be mistaken for proof of a GPU fault. This source is not yet built.
No common root cause or hitch correction has been established yet.

Cross-platform continuation evidence:
- Shared wait diagnostics compile on Windows desktop and UWP Release; i18n lint
  passed. Full Windows application suite passed: 1042 cases / 38847 assertions,
  two skipped. The attempted filter was not a doctest --test-case option, so
  this invocation actually ran the full suite; do not label it focused coverage.
- Xbox diagnostic v6 uploaded, registered and launched; APPX path in its log
  confirms v6. It reverted to App/1024 MiB and later stopped with record-commands
  E_FAIL / GetDeviceRemovedReason S_OK after look_focus_prepare pipeline failed.
  Game classification requested from owner; no claim of stable v6 Game execution.
- A real Windows SDL GPU run using current original game scripts recorded around
  60 FPS, with no post-loading 100+ ms wait in the observed interval. That run
  eventually stopped gameplay at a card, so it is insufficient gameplay soak.
- Isolated fixture at build/native-port-game-check now runs an unattended
  three-minute match with repeated Choose messages and health buffs. Its global
  NativePortValidation script was replaced with a 180-second shutdown only,
  removing the previous menu/lobby/run transitions at 8/16/35 seconds.
  Original project scripts are not instrumented. Current log:
  %TEMP%/hordewake-shared-hitch-pc-soak-active.log. Earlier soak-active and soak
  files contain transitions or paused cards and must not be reported as long
  active-play validation. Android adb reports no attached physical device.

Owner v6 Game-mode retest: owner reports a noticeable improvement. Preserved
`%TEMP%/hordewake-xbox-v6-gameplay.log` confirms 5120 MiB and 15 post-loading
five-second samples (75 seconds), 58.1-60.2 FPS at 1920x1080 high, worst 69.66 ms.
No recurring ~520 ms stall, native long-wait warning or device loss in that
interval. Initial menu loading and run construction still have separate heavy
frames under the curtain. V6 differs from v5 by optimized-clear metadata rollback,
native failure/wait diagnostics and the post-pump availability snapshot; this is
not a controlled single-variable experiment and does not prove a common cause.
Owner does not recall the precise PC/mobile hitch timing. Latest Xbox v6 APPX
copied to Downloads, README/hash list updated; v5 retained. Shared extra wait
instrumentation is newer than v6 and is NOT in the deployed package.

Owner additionally requests a rendered Series S benchmark with many mobs, weapons
and effects, and asks whether ~60 FPS is a cap. Build an isolated benchmark APPX
with a separate identity, keeping the playable Hordewake package/saves unchanged.
Game-side reproducible fixture tools now live at hordewake/tools/benchmarks/
console_stress.luau and tools/prepare_console_benchmark.py. Cases: 100/1 weapon,
300/6 weapons, 550/6 max-level weapons (normal game cap), 1000/12 max-level
weapons (explicit overload beyond normal balance). Both VSync on/off, high quality,
render scale 1, frame generation off, seeded map 1, 15-second warmup + 30-second
samples. Actual live counts, actual weapons, FPS, p95/p99/worst frame, draw counts
and threshold counts must be reported, not merely requested spawn totals. Combat
damage reduced in fixture to sustain mobs; invulnerability avoids test death.
First fixture v1 preparation stopped at a too-strict newline assertion; no source
project was changed. Current fixture v2 starts a PC D3D12 smoke/benchmark; read
%TEMP%/hordewake-benchmark-pc.log before packaging. No benchmark results yet.
Earlier PC hitch soak completed 180 seconds / 27583 frames / 10800 ticks. Its
only long-frame warning was initial load (145.9 ms), no recurring 500 ms stall.
Android shared-diagnostic native compile and Linux compile completed successfully;
physical Android validation remains unavailable.

Benchmark setup correction: native-port-game-check was created by an earlier
session with NTFS hardlinks. Appending the unattended soak harness to its client
loader also appended it to the original game loader. This owned injection was
removed immediately after discovery, with atomic file replacement to break the
hardlink; fsutil now lists only the original path. Original game runtime has no
Benchmark/HITCH_SOAK/harness marker. Installed packages were generated before the
injection and unaffected. Earlier claim that original source was never instrumented
was incorrect; the short-lived change was corrected and communicated to owner.
New benchmark tool uses shutil.copytree (independent files) and only copies .engine/
import cache, never saves/session/export state. First v3 smoke caught two harness
bugs: server setup ran before client's Hello produced a hero, and wrong preset
method name. Fixed async server setup / bounded hero wait and ApplyPreset. Latest
PC smoke log: %TEMP%/hordewake-benchmark-pc-v3-fixed.log; no script errors at start,
benchmark-start marker present. Re-exporting corrected fixture before deployment.

Owner objected to mouse capture during the PC benchmark. Immediately terminated
all owned benchmark engine-host processes (20356/50108) and verified no engine-host
remained. Benchmark prepare tool and independent v3 fixture now replace only the
PointerLocked=locked assignment with false; original gameplay code is unchanged.
Do not launch another PC window that can capture the owner's mouse. Xbox remains
the target of this benchmark. First PC smoke stage is valid harness coverage only:
100-mob mean99.92 (min61/max100), one actual weapon, ~99.97 FPS on owner's
4070 Ti SUPER / display; do not attribute that result to Series S. Full eight
stages did not complete before the owner's interruption. Corrected export is
still running and must be inspected before Xbox packaging.

Benchmark APPX deployment succeeded (Portal state HTTP200 Success true):
Ludwerk.Hordewake.Benchmark_0.8.49.1_x64__wnh08fea6rrsm,
AUMID Ludwerk.Hordewake.Benchmark_wnh08fea6rrsm!Player.
Artifact Downloads/Hordewake-Benchmark-xbox/
Ludwerk.Hordewake.Benchmark-0.8.49.1-x64.appx,
SHA256 1d526a522800641daa6da91791c8a7ac52a71f47aeffb597050f9e705a917dfd.
Owner asked to classify the separate benchmark as Game and open; do not launch
another PC benchmark. Current task is hardware measurement of eight cases, not
just packaging. README describes workload limitations and actual-count reporting.

Owner reports benchmark closes immediately; no measured console stages yet.
Preserved %TEMP%/benchmark-xbox-engine.log and .previous.log show exit after
initial scene tree, zero script errors, no player-result/fatal or OS crash dump.
engine.last-run says ended externally; do not label this a proven graphics crash.
Owner asks Game by default. Portal's own settings implementation uses PUT
/ext/settings/defaultuwpcontenttypetogame {Value: True}; GET confirms true and
RequiresReboot Yes. Owner informed of applying preference + required restart.
Default affects UWP packages on this test console, not the distributed manifest.
Source Microsoft Device Portal documentation /preference-settings confirms it.
The setting is reversible; normal game/probe packages and saves remain installed.
Current command includes verified-setting check before POST /api/control/restart;
check its session/output before issuing a duplicate reboot. Benchmark still needs
successful startup and all eight physical Series S samples.

Console default Game preference remains true after restart. Portal screenshot
%TEMP%/xbox-benchmark-restart-state.png shows Dev Home with all original packages
present, none running; no second reboot sent. The initial restart curl client
(34236 / parent42924) remained waiting on the old connection and was stopped
only after console returned; use --max-time for future requests. Native UWP
player-entry.txt now records memoryBudgetBytes and memoryUsageBytes before
engineHostMain; UWP Release compile passed. This instrumentation is generic.
Benchmark v0.8.49.2 packaged/deployed successfully (Portal200 Success true),
same separate benchmark identity. Launched to diagnose startup budget under
the now-enabled default Game policy. No results claimed until logs verify them.

Latest follow-up: owner signed in; Game budget5368709120 bytes confirmed.
Benchmark-only OS dump isolates an AV in MMDevAPI / miniaudio UWP WASAPI
activation. Added UWP-only XAudio2 output, preserving mixer/decoders/effects and
desktop/mobile backends. Hardware now runs actual benchmark combat and owner
confirmed music/effects. Windows+Linux audio41cases/1157assertions each passed;
Android audio cross-build and UWP Release build passed. No vendor/SDK changes.

Separate benchmark v0.8.49.5 freezes card progression to keep weapon levels
stable. Eight high1080p workloads (VSync ON/OFF). Initial captured runs showed
~500ms resource_retirement/begin_frame waits, but Portal screenshots can affect
the GPU, so those tails are not final workload latency. Clean pass collector
session44073 reads logs only into Downloads/Hordewake-Benchmark-xbox/results/
series-s-v5-clean-engine.log. Do not reinstall/relaunch/capture during samples.
/ext/app/runningtitle returns empty for the running UWP; use advancing logs.
Full current checkpoint: hordewake-series-s-benchmark-2026-10-09.md.

Final clean hardware pass completed all eight stages (collector44073 exit0).
Saved durable report in hordewake-series-s-benchmark-results-2026-10-09.md and
Downloads/Hordewake-Benchmark-xbox/results/series-s-report.md, raw clean engine
log, JSON, CSV, player-entry and AFTER-measurement overload screenshot. Means
ON/OFF:100mobs1weapon59.91/58.85,300mobs6level4 51.90/50.73,
550mobs6max46.41/45.29,1000mobs12max39.38/38.29 FPS. Peak sampled process996.8MiB,
Game budget5120MiB. Actual live means99.9/299.3/548/998, actual weapon counts
1/6/6/12. No sample frame>50ms; only two>33.333ms in OFF overload, worst33.89ms.
No new OS dump. The ~500ms captured-run waits did not recur in this clean pass;
GPU capture is a measurement confounder, not a proven explanation for every
owner-reported PC/mobile hitch. VSync OFF did not improve these workloads;
this does not establish a fixed Series S hardware FPS cap or 120Hz support.
Benchmark remains open in its final overload scene. Normal game stays installed
separately v0.8.49.6. No PC player was launched. UWP audio fix is in source and
benchmark package; do not claim the old normal v6 binary already contains it.

## Native render optimization completed (2026-10-09)

Completed-fence retirement/upload reuse and three fenced descriptor heap slots
replace two queue-wide waits per frame. Preserves quality and explicit surface/
readback/teardown synchronization. Windows RHI28/1137 passed, UWP and Windows
host built. All eight same-fixture Series S stages completed: ON100/300/550/1000
mobs=59.94/59.94/59.71/55.83 FPS; OFF=74.93/67.44/61.84/54.93. Gain up to43.4%,
no sample frame>50ms. OFF300/550 each had isolated~33ms tails; see full report.
Native D3D12 only; do not claim these gains for SDL_GPU/Android or parity with
other engines. Loading and independent PC/mobile hitches remain unproven.

Benchmark v6 complete and stopped. AFTER-measurement screenshot visually checked.
Normal Hordewake v0.8.49.7 deployed successfully and launched with5120MiB Game
budget. It includes the prior XAudio2 startup fix as well as the queue optimization,
using the normal export, not the stress fixture. Source/measurement/artifacts:
[handoff](hordewake-render-performance-2026-10-09.md) and
[before/after results](hordewake-render-performance-results-2026-10-09.md).

Shared particle palettes completed and normal v0.8.49.8 deployed/launched. Seven
picture slots preserve depth order/quality with no bindless requirement. Same
Series S load has up to56.7% fewer total calls; extreme OFF54.93->56.17FPS (small
additional FPS gain), OFF550 worst39.04ms (previous33.37). Cross-platform common
renderer: Windows/Linux particles18/1601 each, full Windows RHI29/1181; full
AndroidARM64/UWP build; headless SDL_GPU D3D12/NVIDIA and Vulkan/Intel image
checks passed without PC window/mouse capture. Android hardware/Metal runtime
untested. All eight raw/results/comparison preserved separately as particles-v7.
See [shared particle report](hordewake-particle-performance-results-2026-10-09.md)
and [performance handoff](hordewake-render-performance-2026-10-09.md).
