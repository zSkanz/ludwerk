# Profile-driven optimization, second batch

Owner authorized continuing after the first [measured batch](hordewake-profile-optimization-2026-10-09.md).
Preserve all unrelated dirty changes, quality, simulation and input behavior.

## Implemented changes

- World descendant collection walks existing parent/child/sibling links in
  preorder, appending to the caller's vector without a temporary traversal stack.
  The walk remains iterative, bounded by the requested root, and observes current
  hierarchy order. This benefits all users of the shared scene implementation.
- Animation groups live driver tracks by root within each sample. Descendants
  and skinned-mesh filtering are collected once for a root shared by several
  tracks, using persistent scratch vectors. The next sample collects again, so
  reparenting and asynchronously arriving skeletons need no cache invalidation.
  Final mesh/driver order, times, weights, poses and sampling rates are unchanged.
- Inspected HeroController nearest-target work: it already caches standing hero
  positions by simulation time. No additional snapshot or reduced AI frequency
  was introduced, avoiding changes to target/death behavior.

## Regression checks

New animation case covers two weighted tracks, late-added clothing, late skeleton
arrival, detaching/reattaching the mesh, and stopping one track. Detached meshes
keep their last pose in the existing API; the initial test incorrectly expected
the pose to disappear and was corrected to assert that it stops following.
New scene case covers appending preorder through a 1024-level subtree, an external
sibling, empty/invalid roots and sibling reordering.

Windows full render suite passed 276 cases / 100676 assertions, Linux 276 /
100680; both retain one intentional skipped terrain-cost benchmark. Full scene
suites passed 352 cases / 17730 assertions on both. The Linux animation-only
suite also passed 42 cases / 241 assertions. Fresh Windows host, UWP Release and
Android ARM64 players built. Changed C++ files pass pinned clang-format-18 and
diff checks. No game source or public API changed in this batch.

Some initial test runs used the earlier incorrect detached-pose expectation;
their failure logs remain in TEMP. Stopped those owned runs before relinking
(Windows had a file-in-use link failure), then rebuilt and ran the corrected
suites. One quoted Docker test-name filter selected zero tests; it is not used
as validation. Source-file selection and full suites above executed the cases.

## Physical measurements completed

See [all results and control runs](hordewake-profile-optimization-batch2-results-2026-10-09.md).
Evidence: `C:/Users/juanr/Downloads/Hordewake-Profile-1500-opt2-results`, including
raw logs, profiles.json, controls.json, report.md and comparison generator.

- Xbox driver preparation 0.5841 -> 0.4724 ms (-19.1%); animation total
  1.8451 -> 1.7275 ms (-6.4%). FPS essentially unchanged (52.03 -> 52.32).
  Worst frame worsened to 35.137 ms; no claim that all metrics improved.
- Windows 107.76 FPS versus historical 111.49 and repeated previous build
  108.59. No clear FPS gain. Drivers 0.1003 versus control 0.1061 ms; total
  animation essentially unchanged, and the new run drew more objects.
- Android 32.47 FPS versus historical 37.95 and previous-build repeat 30.46.
  Battery sensor temperatures differ substantially (new 34.5 -> 37.1 C,
  historical 30.0 -> 34.9, repeat 37.2 -> 38.5). These are not controlled
  thermal conditions or SoC measurements. Do not attribute the FPS difference
  to this change or claim a mobile FPS win. Broad untouched CPU areas slowed too.
- All captures used 60 measured seconds after 20-second warm-up, 1500 target
  enemies and twelve maximum weapons. Same device-specific rendering policy;
  no GPU execution timestamps, identical-frame replay or statistical proof.
- Initial 720p Windows run was stopped/excluded and restarted explicitly at
  1920x1080. Initial Xbox v11 used an unintended default publisher and was never
  measured; removed only that extra package via Device Portal. v12 uses the
  established CN=LudwerkDevelopment publisher and confirmed 5120 MiB Game budget.
  Avoid concurrent Portal helpers sharing a cookie file; fresh isolated cookies
  were used to relaunch v12 after polling/authentication problems.

## Updated normal builds

- Windows: `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized2-windows/Hordewake.exe`.
  Exported successfully. Normal game not launched on PC to avoid pointer capture.
- Android: `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized2-android/Hordewake-0.8.49.apk`.
  Installed/launched dev.local.hordewake. Menu measured 60.1 FPS at normal project
  Low / 1560x720 settings; startup log beside APK.
- Xbox: `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized2-xbox/Ludwerk.Hordewake.Dev-0.8.49.13-x64.appx`.
  Installed/launched in Game mode, menu measured 60.3 FPS. No device-loss error
  in this startup's `verification/engine-steady.log`; earlier failure remains
  unresolved. Cold startup still stalled 8553.89 ms and loading lifted after
  17502.45 ms, with the existing readiness warning. Do not call that fixed.

Both benchmark apps and owned PC benchmark processes were stopped after captures.
Normal Android/Xbox games were left open. Prior builds and baseline evidence
remain. No commit/push, no SDK changes or new dependencies.

## Execution paths

New isolated fixture: `%LOCALAPPDATA%/Ludwerk/build/hordewake-profile-1500-opt2-v1`.
Windows export: `hordewake-profile-1500-opt2-export-v1` in the same build root.
Set ENG_PLAYER_HOST_WINDOWS explicitly to the fresh win-msvc-dev host; --dev-host
alone may pick an old player. Export targets sequentially from a shared fixture.
Pointer locking remains disabled in the isolated fixture. No PC mouse capture.
Temporary build logs: `%TEMP%/ludwerk-opt2-*`.

## Remaining priorities

The first batch's normal Xbox first-start device loss and cold-loading stall
remain open; neither is claimed fixed by this traversal change. Investigate
cold asset/shader preparation separately from in-match timing. For further FPS
work, obtain GPU execution timestamps and a better repeatable effects workload;
use temperature-controlled repeated mobile passes before judging regressions.
