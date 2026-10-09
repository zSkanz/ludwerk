# Profile-driven optimization batch

Owner request: inspect as much as practical and optimize from the three physical
one-minute, 1500-enemy / twelve-weapon profiles, preserving quality and behavior.
Baseline: `hordewake-cross-device-profile-results-2026-10-09.md`.

## Implemented changes

- Game horde: reuse Swarm GetAgents/GetPositions destination arrays and target
  scratch rather than allocate population-sized Luau lists each fixed tick.
  APIs already support reusable destination arrays and truncate old tails.
- Engine animation: generation-aware per-sample marks reject duplicate mesh
  candidates before sorting. Final instance ordering, poses, sampling rates,
  blending and cache sharing remain unchanged. Add CPU scopes within the sampler
  to identify remaining track/driver/bone/preparation/pose/history overhead.
- Engine rendering: canonical float words in material lookup hashes are mixed
  once rather than byte-by-byte. Equality still resolves collisions and canonical
  zero remains equal; output order still follows insertion, never hash iteration.
- Engine physics: reject visual-only inert parts before growing the sparse body
  mirror to their entity index. Existing live bodies are still destroyed when
  made inert, and enabling collision/query/touch still creates their body.
  This removes mirror storage and sweep overhead for objects with no solver body.

## Validation completed

- Windows and Linux full render suites: 275 cases passed on each, with one
  intentionally skipped terrain-cost benchmark. Full scene suites: 351 cases /
  16693 assertions passed on each. After extending the high-index inert-part
  regression, both platforms passed the two D513 cases / 2016 assertions.
- Fresh optimized Windows host, UWP Release and Android ARM64 builds passed.
  Changed C++ files passed clang-format-18 and diff checks; changed HordeController
  passed StyLua. Production game source (102 files) passed type analysis. The
  overall game check is not fully green: existing formatting failures remain in
  ClientLoader, CharacterSettings, UnlockSettings and WorldController, and the
  whole-tree check includes unused RunService imports in old marketing copies.
  These unrelated files were not changed.
- Repeated physical 20-second warm-up + 60-second captures on PC, Series S and
  Android, with 1500 target enemies and twelve maximum-level weapons. No mouse
  capture or screenshots during measurement. See [measured results](hordewake-profile-optimization-results-2026-10-09.md).
- Windows FPS: 106.06 -> 111.49. Xbox: 52.25 -> 52.03. Android: 38.13 -> 37.95.
  CPU physics cost fell 33.0%, 25.6% and 17.5% respectively. Android p99 fell
  47.408 -> 36.985 ms; Xbox worst fell 36.464 -> 32.267 ms. Desktop worst rose
  slightly. One pass per device is not statistical proof or an identical replay;
  intermittent hitches are not declared solved. No GPU execution timestamps.

## Evidence and normal builds

Preserve baseline `C:/Users/juanr/Downloads/Hordewake-Profile-1500-results` and new
`C:/Users/juanr/Downloads/Hordewake-Profile-1500-opt-results`: raw logs, profiles.json,
report.md, comparison.md, comparison generator, check logs and artifact hashes.
The first isolated Windows export raced a simultaneous Android export over the
fixture's `.engine/export/all`; its failed output was not used. Exports were then
run sequentially, with ENG_PLAYER_HOST_WINDOWS explicitly selecting the fresh host.

Normal game builds, preserving regular project settings:

- `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized-windows/Hordewake.exe`.
  Built successfully; not launched to avoid capturing the owner's mouse.
- `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized-android/Hordewake-0.8.49.apk`.
  Installed as dev.local.hordewake; normal menu measured approximately 60.1 FPS.
- `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized-xbox/Ludwerk.Hordewake.Dev-0.8.49.10-x64.appx`.
  Installed in Game mode (5120 MiB). First normal startup lost the graphics device
  during resource retirement (HRESULT 2289696773, removedReason 2289696775).
  Preserve `verification/engine-first-device-lost.log`. Explicitly stopped both
  benchmark v9 and normal v10, then relaunched v10 successfully. Subsequent steady
  menu logs show 59.9 FPS, median 16.68 ms and worst approximately 17 ms with no
  new device-loss error in `verification/engine-relaunch.log`. This does not prove
  the first-start failure is resolved or establish its cause. Cold startup still
  had an 8.884-second stall and lifted loading after 18.021 seconds.

## Remaining investigation

Prioritize normal Xbox startup device-loss reproduction and cold-loading work.
For the heavy scene, use new animation driver/pose scopes to investigate repeated
hierarchy collection and per-mesh preparation, plus the remaining hot Heartbeat
scripts. Obtain GPU execution timestamps before attributing render/present waits
to GPU work. Preserve behavior and quality; do not change population, sampling,
effects or solver settings to manufacture a gain. The shared optimizations apply
across platforms; the measured FPS benefit is platform-dependent.
