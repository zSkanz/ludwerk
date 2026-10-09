# Second optimization batch: measured results

Shared hierarchy traversal and animation root grouping; one 60-second capture per device, after 20 seconds of warm-up, 1500 target enemies and twelve maximum-level weapons. The comparison below uses the first optimization batch as its reference.

| Device | Prior FPS | New FPS | Prior/new p99 ms | Prior/new worst ms | Prior/new draws |
|---|---:|---:|---:|---:|---:|
| Windows | 111.49 | 107.76 | 13.129 / 13.429 | 18.452 / 18.495 | 635.5 / 652.7 |
| Xbox Series S | 52.03 | 52.32 | 28.846 / 28.668 | 32.267 / 35.137 | 642.6 / 625.0 |
| Android SM-S938B | 37.95 | 32.47 | 36.985 / 40.640 | 50.438 / 48.027 | 689.1 / 688.5 |

## CPU means: first batch / second batch (ms per rendered frame)

| Scope | Windows | Xbox | Android |
|---|---:|---:|---:|
| animation.sample | 0.4155 / 0.4298 | 1.8451 / 1.7275 | 2.3772 / 2.8456 |
| animation.drivers | 0.1017 / 0.1003 | 0.5841 / 0.4724 | 0.7944 / 0.8182 |
| animation.poses | 0.2166 / 0.2277 | 0.9502 / 0.9327 | 1.0809 / 1.3851 |
| physics | 0.1865 / 0.1982 | 0.8004 / 0.7857 | 1.0324 / 1.2743 |
| scripts.Heartbeat | 1.0464 / 1.0776 | 4.5865 / 4.5568 | 6.8170 / 8.5344 |
| render.extract | 1.2672 / 1.2883 | 2.9254 / 2.9629 | 2.3615 / 2.5133 |
| render.world | 3.7861 / 3.9611 | 2.3702 / 2.3599 | 2.9604 / 3.1347 |
| wait.present | 0.6584 / 0.6745 | 1.3895 / 1.4183 | 6.6815 / 7.7019 |

## Additional previous-build control runs

To check the variation, the preserved first-batch Windows executable and Android APK were run again after the new builds. These are additional observations, not a controlled thermal experiment or identical frame replay.

| Device | Previous-build repeat FPS | New FPS | Repeat/new drivers ms | Repeat/new animation ms |
|---|---:|---:|---:|---:|
| Windows | 108.59 | 107.76 | 0.1061 / 0.1003 | 0.4304 / 0.4298 |
| Android SM-S938B | 30.46 | 32.47 | 1.1241 / 0.8182 | 3.3025 / 2.8456 |

## Interpretation and limits

- Xbox drivers fell 0.5841 -> 0.4724 ms (-19.1%); total animation fell 1.8451 -> 1.7275 ms (-6.4%). Xbox FPS changed only +0.56%; worst frame worsened 32.267 -> 35.137 ms and two frames exceeded 33 ms. Do not claim all metrics improved.
- Windows FPS was 107.76 versus the historical 111.49 and repeated previous build 108.59. Driver CPU cost was 0.1003 versus repeated previous 0.1061 ms; total animation essentially unchanged. New draw count was higher. No clear desktop FPS improvement from this batch.
- Android new run was 32.47 FPS versus historical 37.95 and repeated previous build 30.46. Battery sensor ranges: historical 30.0 -> 34.9 C; new 34.5 -> 37.1 C; repeated previous 37.2 -> 38.5 C, all AC-powered. This is not SoC temperature or proof of thermal throttling. Broad CPU costs rose across untouched areas too. These conditions do not establish an Android FPS gain or regression attributable to the changes.
- Exact populations, draw counts and all scope rows are preserved. Effects and simulation timing differ, and the Xbox run briefly reached 1362 enemies. CPU scopes include waits and are inclusive; do not sum parents and children. No direct GPU execution timestamps.
- Initial Windows 720p run was aborted and excluded. Benchmark v11 was accidentally packaged with a different publisher; it was not measured, v12 uses the established publisher and its log confirms Game mode (5120 MiB). The extra v11 package was removed.
- Traversal removes temporary stack allocations; scratch and grouping are shared engine changes, with no gameplay population, pose-rate, solver, effect or quality reductions. No claim that cold loading, the first-start graphics failure or intermittent hitches are solved.
