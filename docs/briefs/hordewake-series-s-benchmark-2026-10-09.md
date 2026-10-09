# Hordewake Series S stress benchmark

**Completed:** eight clean physical Series S stages on v0.8.49.5, 1080p high,
5120 MiB Game allocation. [Measured results](hordewake-series-s-benchmark-results-2026-10-09.md).
FPS ON/OFF:100 mobs59.91/58.85;300 mobs51.90/50.73;550 mobs46.41/45.29;
1000 mobs+12 weapons39.38/38.29. No measured frame exceeded50ms; worst33.89ms.
Startup audio crash fixed in this benchmark's UWP output; owner confirms sound.
Normal installed v0.8.49.6 remains separate and does not include the new audio binary.
Durable raw logs/JSON/CSV/report/captures: Downloads/Hordewake-Benchmark-xbox/results.
Benchmark stays open at the final overload scene. Notes below preserve the investigation.

Owner mandate: measure the actual Xbox Series S with many mobs, weapons and
effects. No PC benchmark results may be presented as console performance.
Owner objected to PC mouse capture; all owned PC benchmark players were closed.
Do not start another PC benchmark window.

## Current checkpoint

- Normal Hordewake v0.8.49.6 remains installed separately, with saves preserved.
  Its owner-played Game-mode trace measured 75 seconds of post-loading windows,
  58.1–60.2 FPS at 1080p high, worst 69.66 ms. Previous recurring ~520 ms stalls
  did not recur in this interval; this is not a completed stress benchmark.
- Separate app: Hordewake Benchmark,
  identity Ludwerk.Hordewake.Benchmark, AUMID
  Ludwerk.Hordewake.Benchmark_wnh08fea6rrsm!Player.
- Historical revision: 0.8.49.2. Portal deployment returned
  HTTP200 / Success true. Downloads/Hordewake-Benchmark-xbox contains the APPX,
  VCLibs dependency, public development certificate, README and SHA256.txt.
  APPX SHA256:
  3a27f726cab6680c4b4be75ee4ef0191eb07970e2961a95d620be6553eca7396.
- Revision 1 closed at startup after loading the initial scene tree. No script
  error, player-result/fatal or OS crash dump; engine.last-run says ended
  externally. Do not claim a proven cause or benchmark result.
- Owner requested Game by default. Set the console preference through its own
  Portal API: PUT /ext/settings/defaultuwpcontenttypetogame {"Value":"True"}.
  GET confirms true. Portal declares RequiresReboot Yes, so a restart was sent.
  Console returned to Dev Home, original packages present, account unchecked.
  The old restart curl client waited on the disconnected connection and was
  stopped; use --max-time on subsequent requests, do not send another reboot.
- After restart, launching v2 remotely returned HRESULT 0x8004090A. Portal
  screenshot showed the owner's test account not signed in. Owner was asked
  to sign in again and open Hordewake Benchmark; owner has now signed in. Do not treat the
  remote launch rejection as a new renderer crash.
- V2 player-entry.txt records memoryBudgetBytes / memoryUsageBytes before
  engineHostMain. Check for 5368709120 bytes before accepting Game-mode results.
  New native UWP entry instrumentation compiled successfully.
- See the latest revision checkpoint below; older startup notes are historical.

### Sign-in follow-up and revision 3

Owner confirmed signed in. V2 launches now reach engineHostMain with
memoryBudgetBytes=5368709120 (Game allocation), usage=6578176 at entry. It still
closes in less than a second after the seven-instance run scene loads, before
any script error or benchmark-start. No player-fatal/result is present. Thus the
startup failure is not explained by App's 1 GB allocation or the prior sign-in
rejection. Preserved new evidence in %TEMP%/benchmark-v2-*. Do not claim root cause.

V3 changes only the isolated fixture bootstrap: a minimal empty scene waits two
seconds to let native activation/presentation run, then calls the normal
asynchronous LoadScene into the benchmark run. tools/prepare_console_benchmark.py
now generates this bootstrap for future fixtures. Export completed successfully;
v0.8.49.3 packaged and deployment accepted (poll state before launching). It uses
the same separate identity and native binary as v2; normal installed game is intact.
The Portal usermode crashcontrol GET reports CrashDumpEnabled=false; absence of
an OS dump does not establish absence of a native crash.

### Native startup fault isolated to audio; revision 4 candidate

V3 also closes, after printing benchmark-boot's wait marker. Enabled dumps for
the benchmark only via POST /api/debug/dump/usermode/crashcontrol?packageFullName=...
(Portal's own XboxCrashData.js). Downloaded complete 369321621-byte
%TEMP%/hordewake-benchmark-5184.dmp. Exception thread2068, 0xc0000005,
MMDevAPI.dll+0x3c749. Stack memory contains engine RVAs 0x220d20, 0x2279f4,
0x2274e9, 0x2565f5. Re-linking the SAME objects with /MAP /DEBUG /OPT:REF /OPT:ICF
mapped these to ma_context_get_IAudioClient_UWP__wasapi+0x140,
ma_device_init_internal__wasapi+0x1a4, ma_device_init__wasapi+0x619,
ma_device_init+0x6e5. Do not confuse the first /DEBUG map without /OPT:REF /ICF
(different addresses) with the matching optimized map. The access violation
occurs in the asynchronous Windows audio activation path, not scene rendering.
The vendored UWP implementation passes a stack completion handler; its exact
OS-side lifetime failure is not separately proven. Vendor files were not edited.

Candidate fix: engine/audio/src/uwp_output.{h,cpp}, XAudio2 output only when
ENG_PLATFORM_UWP. Existing mixer/decoders/effects/timeline preserved; desktop
and Android retain miniaudio output. Three preallocated 480-frame stereo float
buffers, callback context owned until DestroyVoice completes, no per-buffer
allocation. AudioSystem's render callback is shared by both outputs. UWP Release
build passed. Formatted with clang-format18. Benchmark v0.8.49.4 packaged,
deployment in progress. Must validate hardware startup/audio and stages; no
claim of fixed startup until it remains running. Native build now has /DEBUG,
/MAP and /OPT:REF /OPT:ICF only in local build cache, not global source defaults.

### V4 startup confirmed; stable workload revision 5

V4 stays open: Portal screenshot shows actual forest combat, 100 mobs and then
six weapons/effects. /ext/app/runningtitle still returns empty while the UWP is
running; do NOT use that endpoint as a UWP liveness test. Use advancing logs and
screenshots. No new dump appeared. Owner asked whether music/effects are audible;
physical audio confirmation is pending. Windows AND Linux audio suites passed
41 cases / 1157 assertions each after the shared callback refactor.

V4 diagnostic results preserved in %TEMP%/benchmark-v4-engine.log:
100 target / 99.92 mean / one weapon =58.43 FPS, p99 19.15 ms, worst515.71ms.
300 target /299.17 mean /six weapons =49.26 FPS, p99 22.53ms,worst524.45ms.
The 510 ms wait was specifically resource_retirement -> begin_frame, not
frame-rate cap / presentation sleep. This reproduces a remaining strong GPU
wait; no claim the cross-platform hitch is fully fixed. Actual rendered captures
%TEMP%/benchmark-v4-screen2.png and benchmark-v4-stage-current.png.

V4 automatically chose cards, which changed weapon levels mid-sample. These
are diagnostic loads, not the final fixed-level comparison. Stopped only the
benchmark and preserved its log before updating. V5 sets GameSettings.FirstLevel
to 1e9 in the isolated fixture (essence still renders/collects), removes automatic
card choices, and thus keeps requested weapon levels stable. Export/package
completed; v0.8.49.5 deployment accepted, poll with the SAME cookie jar to obtain
the per-session deployment state. The old no-cookie204 did not track the accepted
deployment. All eight fixed-level V5 stages still need measurement.

V5 deployed successfully (cookie-bound Portal state200 Success true), launched
and advancing through actual combat. SHA256:
5a61624033b94b21e43e63b0eff486e41c07530515fd161da6d3a0725fec74c9.
Owner physically confirmed music and effects are audible with XAudio2.
Android arm64-player engine_audio also compiled successfully after the shared
render callback change. Collector running in exec session29174, script
%TEMP%/ludwerk-xbox-benchmark-collect.ps1. It writes advancing full engine.log and
per-stage actual console screenshots to Downloads/Hordewake-Benchmark-xbox/results,
exits once benchmark-complete is logged (12-minute timeout preserves partial data).
Do not relaunch or reinstall while collecting. No PC benchmark window is open.

### Clean measurement restart (latest active checkpoint)

V5 first three ON results included screenshot requests during measurement;
all three show one ~507-518ms GPU wait. Portal screenshot capture itself may
stall the GPU. Thus those tails cannot be attributed to the game's workload
alone. V4 likewise had manual screenshots. Preserve these as diagnostic data,
not final latency conclusions. This does not establish a root cause for the
owner's PC/Android hitches.

Stopped collector29174 with Ctrl-C (only local script), preserved
results/series-s-v5-engine.log and its screenshots. Removed ALL screenshot
requests from %TEMP%/ludwerk-xbox-benchmark-collect.ps1. Stopped/relaunched ONLY
the benchmark, same installed v0.8.49.5. New clean collector session44073 writes
results/series-s-v5-clean-engine.log. It reads logs every15s and does not invoke
GPU capture. No controller input, app update, screenshot or PC game launch
during this pass. Wait for eight records plus benchmark-complete, then capture
the completed overload scene if desired. Existing busy-scene images already
satisfy visual workload evidence. Owner confirmed actual audio audible.

## Workload and instrumentation

Eight automated stages: four workloads with VSync ON, then repeated with OFF.
Seed 7, forest map 1, Warrior, high preset, render scale 1, frame generation OFF.
Each scene warms for 15 seconds after client loading, then measures 30 seconds.

| Target mobs | Weapons | Levels | Interpretation |
| --- | --- | --- | --- |
| 100 | 1 | 1 | Low-load baseline |
| 300 | 6 | 4 | Busy combat |
| 550 | 6 | Maximum | Normal game limits |
| 1000 | 12 | Maximum | Deliberate overload beyond normal balance |

Combat damage is reduced to sustain population; the hero is protected. Spawn
creation is spread over ticks and dead mobs replenished. Report ACTUAL live
counts and weapons, not only spawn requests. Frame results include FPS,
median/p95/p99/worst ms, counts over 33/50/100 ms, draws and actual population.
Memory, quality and resolution are also in the engine's five-second logs.
This measures controlled stress, not representative balanced progression.

Game repo tools:
- tools/benchmarks/console_stress.luau
- tools/prepare_console_benchmark.py --output <empty external directory>
- summarize: --summarize <engine.log> --output <results.json>

Independent fixture: %LOCALAPPDATA%/Ludwerk/build/hordewake-console-benchmark-v3.
Sealed export: build/hordewake-benchmark-export-v1. Sources are copies, not
hardlinks; cursor lock is disabled in fixture. Original runtime has no harness.
An earlier hardlinked soak fixture briefly appended an owned harness to the
original loader; it was removed, the original path atomically replaced to break
the link, and the mistake disclosed. Existing installed packages were unaffected.

## Next actions

1. After owner sign-in/open, read v2 player-entry.txt and engine.log from Portal
   LocalState. Full package:
   Ludwerk.Hordewake.Benchmark_0.8.49.2_x64__wnh08fea6rrsm.
2. If startup still fails, preserve previous log/run markers, inspect budget and
   distinguish external termination from script/native failure before changing code.
3. Observe all eight [benchmark-result] JSON records on actual hardware, capture
   screenshots during busy stages, summarize FPS/tails/memory/actual workload.
4. Compare ON/OFF before deciding whether 60 FPS is presentation-limited. UWP
   Dev Mode performance does not establish GDKX/commercial performance.
5. Keep normal Hordewake installed; update documentation/artifact README with
   measured results and limitations, not a claim of completed optimization.

Portal: https://192.168.15.32:11443. Cookie jar in %TEMP%; never print tokens.
Read logs through /api/filesystem/apps/file (knownfolderid LocalAppData,
path %5CLocalState). /api/taskmanager/processes is unsupported on this Xbox.
Deploy /api/app/packagemanager/package, poll state before launch.
Launch /api/taskmanager/app with base64 AUMID and CSRF cookie/header.
See [native port handoff](xbox-native-player-port-2026-10-09.md) for exact API usage.
