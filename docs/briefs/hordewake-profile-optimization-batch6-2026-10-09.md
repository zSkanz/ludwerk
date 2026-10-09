# Hordewake optimization batch 6: bounded UI image decoding on PC

## Authorization and constraints

Owner turned Xbox off and requested continued optimization on PC. Do not query,
launch or deploy to Xbox. Phone remains withdrawn. Do not capture the PC mouse
or launch the normal interactive game. Use windowless native tests/benchmarks,
headless graphics captures, Linux validation and offline builds/exports.
Preserve all preexisting checkout changes and visual/gameplay quality.

## Investigation

Prior v34 Xbox log captured a lobby transition: 158 ms frame, 148 ms render-step
scripts, adjacent message `232 interface pictures loaded in 149 ms`. This is a
lead, not a proven decode trace: the game uses ContentProvider/mesh-loader warm
textures, and the reported elapsed time also spans preload waits. UI creation
and coroutine resumption must be separated before attributing that frame.

Independent concrete defects found in `app::UiText::loadPendingImages`:

- Packed/compiled images bypass deferred jobs and decode on the frame thread.
- Loose requests exceeding four in-flight entries fall through to synchronous
  decoding instead of waiting in the queue. Thus the advertised bound does not
  actually bound main-thread work for a large newly opened panel.

## Work in progress

New regression requests nine distinct image names from loose files and a sealed
pack. The first deferred sync must upload none and leave every request pending;
eventually all must load with their complete mip chains. Baseline test build is
in progress, before changing production code.

Planned fix: apply the queue bound before resolving/decoding any new request,
route packed bytes through owned worker storage, share the existing decode path,
and count queued requests as pending so waiters do not treat them as completed.
Keep synchronous/capture mode unchanged. Preserve teardown waits and all mipmaps.
No speedup or hitch fix claimed before validation and measurement.

## Implemented and measured

`UiText` now gates all new deferred requests before resolution. At most four
reads/decodes are in flight, and at most four requests are attempted per sync,
including when async IO is unavailable. Packed bytes are copied to owned job
storage and use the same decode/mip generation job as loose-file completions.
Queued requests count as pending, so settling/waiters do not finish early.
Teardown still waits for jobs and releases IO handles. Synchronous/capture mode
keeps the original path. The native loading curtain now includes pending UI
pictures in its existing loaders-idle condition.

Windows regressions: 13 UI cases / 310 assertions pass; 20 frame pacing cases /
218 assertions pass. Added checks for overflow, both storage modes, no-IO
fallback, mount removal, entry growth and teardown. The corrected batch
regression fails against the old production implementation (raw evidence saved).
Seven graphical capture gates pass, including terrain full bindings; unchanged
capture goldens. Linux host/UI/frame-pacing builds/tests and all seven graphical
gates pass. All 13 UI cases / 310 assertions also pass on Linux, including the
parity regression: compares texture format and all five RGBA pixels of the two-by-two
fixture and its one-by-one mip, by their recorded upload payloads.

### Isolated PC benchmark

Null RHI, no window or pointer capture. A sealed synthetic pack has 232 distinct
names referring to deterministic 128x128 RGBA PNG data, each with eight mip
levels. Four worker threads; three samples per mode; 1 ms sleeps between syncs.
All 232 images and 1856 mip levels completed in every sample.

| Deferred path, median | Old | New |
|---|---:|---:|
| First sync CPU ms | 55.074900 | 0.030200 |
| Worst sync CPU ms | 55.074900 | 0.052300 |
| Sum of sync CPU ms | 55.074900 | 0.903000 |
| Elapsed completion ms | 55.158200 | 89.522000 |
| Sync calls | 1 | 59 |

This shows removal of the decode burst from the frame thread, not a whole-game
speedup or less total CPU work. GPU upload/driver time is NOT measured by Null
RHI. Elapsed loading is longer in this paced harness; never claim loading became
faster from these numbers. Synchronous mode still finishes in one call. No
physical Xbox/phone measurement, no diagnosis/fix of the mid-match hitch.

Raw before/after logs, regression failure/pass and reproducible comparison script:
Downloads/Hordewake-UI-Loading-opt6-results. Fresh normal Windows export is
complete: Downloads/Hordewake-0.8.49-optimized6-windows and optimized6-windows.zip.
Xbox remains off; no requests were sent to it or to the phone. No normal PC game
window was opened. SHA256 manifest and export log are in the evidence folder.

## Handoff / remaining work

This batch fixes confirmed loader defects. It does not prove the 158 ms lobby
frame was caused by this loader: ContentProvider's reported time includes waits
and resumes game code, and it preloads through MeshLoader. Profile interface
construction and preload continuation separately next. Remaining native cold
PSO compilation and the intermittent mid-match hitch are still open.

The default game's deferred path now obeys its existing queue contract. The
shared implementation also benefits other builds, but this iteration has no
physical Android/Xbox measurement. No new SDK, public script API, quality toggle
or change to game counts/animation rates. No commit/push/dependency edits.
