# Hordewake profile-driven optimization results

One 60-second physical capture per device before and after; 20-second warm-up, 1500 target enemies and twelve maximum-level weapons. Same presets/backend per device. Raw populations and draw counts remain in the JSON/logs. A single pass describes this run and cannot establish statistical significance.

| Device | FPS before | FPS after | Change | p99 before/after ms | Worst before/after ms | >33 ms before/after |
|---|---:|---:|---:|---:|---:|---:|
| Windows | 106.06 | 111.49 | +5.12% | 13.709 / 13.129 | 18.273 / 18.452 | 0 / 0 |
| Xbox Series S | 52.25 | 52.03 | -0.42% | 29.019 / 28.846 | 36.464 / 32.267 | 3 / 0 |
| Android SM-S938B | 38.13 | 37.95 | -0.48% | 47.408 / 36.985 | 50.947 / 50.438 | 187 / 65 |

## CPU scope means (ms/rendered frame, before / after)

| Scope | Windows | Series S | Android |
|---|---:|---:|---:|
| frame.simulation | 2.253 / 2.034 | 9.436 / 8.908 | 12.012 / 12.271 |
| scripts.Heartbeat | 1.100 / 1.046 | 4.713 / 4.587 | 6.359 / 6.817 |
| animation.sample | 0.465 / 0.416 | 1.945 / 1.845 | 2.429 / 2.377 |
| physics | 0.278 / 0.186 | 1.075 / 0.800 | 1.252 / 1.032 |
| physics.apply | 0.208 / 0.123 | 0.852 / 0.592 | 0.737 / 0.531 |
| physics.solve | 0.037 / 0.034 | 0.098 / 0.097 | 0.420 / 0.413 |
| render.extract | 1.360 / 1.267 | 3.115 / 2.925 | 2.403 / 2.361 |
| extract.meshes | 0.928 / 0.849 | 1.941 / 1.804 | 1.572 / 1.510 |
| render.world | 3.892 / 3.786 | 2.362 / 2.370 | 2.922 / 2.960 |
| wait.present | 0.673 / 0.658 | 1.160 / 1.390 | 6.862 / 6.681 |

## Interpretation and limits

- Physics CPU cost fell on all three devices. Series S 1.075 to 0.800 ms (-25.6%), Android 1.252 to 1.032 ms (-17.5%), Windows 0.278 to 0.186 ms (-33.0%). This changes no solver settings: visual-only objects no longer inflate the sparse mirror.
- Animation sample and render extraction CPU means fell on all three devices in this pass. Sampler subscopes now expose drivers/poses as next investigation candidates.
- Desktop FPS rose 5.12%; Xbox (-0.42%) and phone (-0.48%) were effectively unchanged in mean FPS. Desktop worst increased 0.179 ms: do not claim every metric improved.
- Phone p99 fell 47.408 to 36.985 ms (-22.0%); >33 ms frames fell 187 to 65, >50 ms from six to one. Xbox worst fell 36.464 to 32.267 ms, with >33 ms from three to zero. No >100 ms frames in either run; this does not prove the intermittent hitch is gone.
- Phone battery sensor was 30.0 to 34.9 C in the new run versus 27.4 to 32.8 C before, both AC-powered. Actual mobile resolution/presentation/fast-terrain policy differs from desktop as in the baseline; do not use these as equal-work hardware rankings.
- Draw count changed: Windows 635.3 to 635.5, Xbox 624.0 to 642.6, Android 688.5 to 689.1. Game/effect timing varies; these passes are not frame-by-frame replays.
- Timings are inclusive main-thread CPU wall time, including waits. Do not sum parents and children. GPU execution time was not directly measured.
