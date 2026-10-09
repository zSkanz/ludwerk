# Hordewake cross-device one-minute profile

## Requested workload

Windows, physical Xbox Series S and the owner's wireless-ADB Android phone.
One measured wall-clock minute per device, after 20 seconds of warm-up:
1500 target enemies, twelve max-level weapons, boss/effects, Warrior, forest,
seed 7, High quality, render scale 1, frame generation off, VSync off, no FPS cap.
Record actual population, resolution, hardware, backend and package identity.
Preserve existing eight-stage benchmark results; use a new isolated fixture.
Do not capture the PC mouse or take Xbox screenshots during measurement.

## Progress / handoff

- Preparing bounded CPU scope captures through the existing core profiler and
  DebugService. Includes mean/best/worst/p99 per frame, not simulation-tick FPS.
- Android initially absent; after owner enabled wireless debugging, mDNS found
  `192.168.15.91:46387`, and `adb connect` succeeded.
- Game fixture preparer: `tools/prepare_profile_benchmark.py` in the Hordewake
  repository. It copies the existing fixture then changes only the isolated copy.
- COMPLETE: all three real devices ran 60 measured seconds after 20 seconds of
  warm-up. Results: [full tables](hordewake-cross-device-profile-results-2026-10-09.md).
- Windows: 106.064 FPS, mean 9.428 ms, best 5.846, worst 18.273, p99 13.709.
- Series S: 52.247 FPS, mean 19.140 ms, best 13.670, worst 36.464, p99 29.019.
- Android: 38.132 FPS, mean 26.225 ms, best 20.515, worst 50.947, p99 47.408.
- Actual mean enemy count 1496.8--1497.2; all three reached 1500 and had twelve
  weapons. Dead enemies are replenished, so the instantaneous count can fall.
- Evidence directory: `C:/Users/juanr/Downloads/Hordewake-Profile-1500-results`:
  `profiles.json`, `report.md`, `generate-report.py`, raw per-device logs,
  artifact SHA-256 manifest, Windows/Linux test logs and phone battery snapshots.
- Fresh isolated fixture: `%LOCALAPPDATA%/Ludwerk/build/hordewake-profile-1500-v1`;
  sealed export: `hordewake-profile-1500-export-v1` under the same build root.
- Xbox benchmark package `Ludwerk.Hordewake.Benchmark_0.8.49.8_x64__wnh08fea6rrsm`
  in `Downloads/Hordewake-Profile-1500-xbox`; Game budget verified at 5120 MiB.
  Android APK in `Downloads/Hordewake-Profile-1500-android`, separate application
  ID `dev.local.hordewake.benchmark`; it was stopped after collection. The PC
  benchmark was closed after collection, with no pointer locking.
- The Windows export initially chose the existing old release player even with
  `--dev-host` (that flag is fallback-only). Those runs were rejected, logs kept
  as `windows-invalid-old-player*`. The valid run uses the freshly compiled
  optimized `win-msvc-dev` host and its complete engine content directory copied
  into the isolated export. For future exports, explicitly set
  `ENG_PLAYER_HOST_WINDOWS` rather than assume `--dev-host` overrides a player.
- Core API: bounded `requestCapture` starts/stops at frame boundaries; mean,
  best and p99 extend existing ScopeReport. DebugService `CaptureProfile` and
  `GetProfileReport` expose it for remote shipped-player diagnostics. Generated
  IDL descriptors, runtime types, API dump and reference updated.
- Validation: Windows and Linux each passed seven core profiler tests / 55
  assertions and two matching script service tests / six assertions. Fresh
  Windows/UWP/Android full hosts built; all three physical captures exercised
  the completed-report table serialization and CPU scopes. API check passes.

## Interpretation

Scopes are inclusive main-thread CPU wall-clock times. Parents include children;
do not add parent and child rows. Wait scopes can reveal back pressure but do not
measure GPU execution. Native D3D12 has no direct GPU timestamp capture here;
mark GPU duration unavailable rather than infer it from draw submission time.
Best values may be zero for subsystems not run on every render frame. One run per
device is descriptive, not a statistically significant platform ranking.

Windows and Xbox rendered 1920x1080 High; Android 2340x1080 High (21.9% more
pixels), with the game's fast terrain variant. Android stayed VSync/compositor
paced despite the script requesting off; Windows used immediate, Xbox mailbox.
Do not present these as equal-work hardware rankings. No samples exceeded 100 ms;
this does not establish that the earlier intermittent hitch has been eliminated.

## Findings and next optimization candidates

- Xbox simulation averages 9.436 ms: Heartbeat scripts 4.713, animation sampling
  1.945, native swarms 1.155, physics 1.075 (children of simulation). Extraction
  is another 3.115 ms per frame, including 1.941 ms of mesh extraction. These
  are material CPU costs, not a problem attributable to physics alone.
- Android simulation averages 12.012 ms, including Heartbeat 6.359 and animation
  2.429. Presentation wait averages 6.862 ms and peaks at 37.188; GPU/compositor
  timing is needed before assigning that entire wait to GPU shader execution.
- Physics solve itself averages only 0.098 ms on Xbox versus physics apply
  0.852 ms; synchronization/body updates merit inspection before changing Jolt
  solver quality. Android solve/apply average 0.420/0.737 ms.
- Windows world-render CPU averages 3.892 ms and extraction 1.360; different
  backends and much higher render frequency make raw per-frame CPU comparisons
  insufficient for hardware ranking. Fixed ticks remain at 60 Hz.
- Particle update and UI layout are relatively small in this capture. Do not
  infer that all particle GPU effects are cheap from CPU update time alone.
- Preserve this profile as the next baseline; change one candidate at a time
  and repeat the matched scenario, retaining p99 and worst-frame regressions.
