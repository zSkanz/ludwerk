# Heavy-workload optimization, batch 7 (2026-10-09)

## Scope and device restrictions

Prioritize the largest measured costs instead of isolated lookup/UI improvements. Xbox is off; the phone remains unavailable. PC measurements must be windowless, without pointer capture. No console or phone requests were made.

## Evidence and current work

The retained Windows 1500-mob/12-weapon profile identifies geometry submission (~2.46 ms across geometry passes), extraction (~1.29 ms), and Heartbeat (~1.08 ms per rendered frame) as significant CPU costs. GPU execution is not measured by those CPU scopes.

- A windowless heavy-game diagnostic at 30 simulated seconds found 2430 draw items, 1530 skinned items, 2296 batched items and 35 batches. All selected mesh LODs were zero. Skinned capacity and widespread LOD fragmentation were therefore rejected as explanations for this sample.
- Experiment: instance pairs rather than groups of three. Draw count changed only modestly (638.17 -> 628.82 average in two initial windowless runs, slightly different live workloads). Reverted; no shipping threshold change.
- Candidate engine change: memoize ancestor membership within each extraction. Component pools reuse shared ancestry without traversing unrelated UI/scripts. Generation-aware entries; no cache survives an extract, so reparenting and other roots cannot inherit stale answers.
- Candidate game change: borrow HeroController's existing tick snapshot once for the horde follow batch. Queries preserve the same hero ordering, ties, positions and distance arithmetic. They avoid controller dispatch and repeated clock checks for every enemy. NearestAt remains available for other callers.

## Initial results, not whole-game FPS claims

Synthetic complete extraction of 1500 meshes under a shared 12-level hierarchy, five samples of 500 extracts: median CPU .345074 -> .216239 ms (-37.34%). This is a deliberately deep synthetic hierarchy, not Hordewake's measured extraction gain.

Windowless Null-RHI game workload, 1500 target mobs / 12 weapons, 20 simulated seconds warm-up and twelve five-second aggregate windows: horde.follow .7000 -> .6275 ms per simulation tick (-10.36%); total horde .8708 -> .8000 ms (-8.13%). Rendering is absent in this diagnostic; do not report its throughput as playable FPS.

## Final validation and comparison

Two controls, two candidates and one final-build sanity run completed on SDL_GPU offscreen. The fixture uses a synthetic 60 Hz simulation clock, so its [benchmark-result] FPS fields are constants and must not be used as performance evidence. --frame-stats records actual wall-clock CPU scope timings. Profiling is reset at 20 simulated seconds and remains recording through exit. Fixture changes are benchmark-only.

Evidence directory: C:/Users/juanr/Downloads/Hordewake-Heavy-opt7-results. Temporary renderer CSV instrumentation was removed. Fresh normal Windows build exported to Downloads/Hordewake-0.8.49-optimized7-windows and the matching ZIP. The benchmark fixture remains configured as a profiling fixture; it is separate from the normal project. Neither candidate has been validated on physical Xbox/Android.


### Repeated actual-game costs

Two control/two candidate runs, mean of scope medians, same 1500-target/12-weapon scenario:

| CPU cost | Before ms | After ms | Change |
|---|---:|---:|---:|
| Simulation (inclusive) | 3.4115 | 3.3200 | -2.68% |
| Heartbeat | 1.6776 | 1.6015 | -4.54% |
| Extraction (inclusive) | 1.0474 | 1.0365 | -1.05% |
| Mesh extraction | .6685 | .6488 | -2.95% |
| Render world (CPU, inclusive) | 3.6662 | 3.6250 | -1.12% |

Horde follow mean per simulation tick: .8825 -> .8175 ms (-7.37%). Total horde: 1.0771 -> 1.0100 ms (-6.23%). The engine extraction effect in this game is small and cannot be cleanly separated from run-to-run variation. Do not present the synthetic hierarchy's 37% as a Hordewake FPS gain. Remaining shadow/geometry submission costs still dominate rendering.

Final-build sanity run (including a flat-world shortcut that avoids allocating membership scratch for direct children): simulation 3.2767 ms, Heartbeat 1.5817 ms, extraction 1.0278 ms. Final synthetic hierarchy median .218452 ms, versus old .345074 (-36.69%). No large whole-game FPS or mid-match hitch fix is claimed.

### Checks and limits

- Full render suite: Windows 280 cases / 101388 assertions; Linux 280 / 101390. Two intentionally skipped benchmarks. These full suites preceded the final flat-world shortcut.
- Final shortcut validated with 51 extraction cases / 1112 assertions and all seven capture gates on Windows and Linux. Clang-format 18 check passed.
- Pure nearest-target parity: 8008 assertions, including empty/one/eight heroes, ties, refreshed arrays, removed entries and non-finite distances. Changed game files pass StyLua.
- Shipping src: all 103 files passed Luau type analysis. The stock whole-project check remains blocked by pre-existing unused variables in marketing capture copies; restricting analysis to src reaches existing formatting failures in ClientLoader, CharacterSettings, UnlockSettings and WorldController. Those unrelated files were not reformatted. This is not a fully green whole-project CLI check.
- Normal Windows export completed (265 meshes / 672 textures), including the new helper module. ZIP: 109961297 bytes. Executable: 47888384 bytes. No interactive PC launch or pointer capture.
- No physical Xbox/Android validation, no mobile access, no Xbox requests. Production C++ and Luau changes are shared across targets, but device gains need physical retesting when allowed.

Evidence includes comparison.md/json, raw control/candidate/final logs, draw diagnostic, before/candidate game sources, isolated production patches, full/final test logs and artifact SHA256 values. Rejected pair batching and temporary renderer CSV instrumentation are absent from production.
