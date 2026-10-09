# Hordewake: Xbox Series S benchmark

Measured on the owner's physical Series S in Developer Mode, UWP Game allocation (5120 MiB), native D3D12, 1920x1080 high. Package 0.8.49.5. No frame generation, render scale 1, frame cap 0. Four loads repeated with VSync ON and OFF; 15 seconds warm-up followed by 30 seconds per measured sample. No screenshots or controller input during the clean pass.

Seed7, Warrior, forest map1. Hero protected, combat damage reduced, population replenished in small batches, card progression disabled to hold weapon levels. Stage four deliberately exceeds normal game limits. This is one controlled stress pass, not commercial GDKX performance or a claim of 120 Hz support.

| Target mobs | Weapons / levels | VSync | FPS | Actual mobs mean (min-max) | p99 ms | Worst ms | Frames >33 / >50 / >100 ms |
|---:|:---|:---:|---:|:---|---:|---:|:---|
| 100 | 1 / 1 | ON | 59.91 | 99.9 (61-100) | 18.22 | 23.86 | 0 / 0 / 0 |
| 300 | 6 / 4 | ON | 51.90 | 299.4 (236-300) | 21.73 | 28.28 | 0 / 0 / 0 |
| 550 | 6 / max | ON | 46.41 | 548.1 (460-550) | 24.87 | 29.32 | 0 / 0 / 0 |
| 1000 | 12 / max | ON | 39.38 | 998.2 (961-1000) | 29.04 | 32.93 | 0 / 0 / 0 |
| 100 | 1 / 1 | OFF | 58.85 | 99.9 (66-100) | 18.29 | 23.74 | 0 / 0 / 0 |
| 300 | 6 / 4 | OFF | 50.73 | 299.3 (224-300) | 21.99 | 27.25 | 0 / 0 / 0 |
| 550 | 6 / max | OFF | 45.29 | 547.8 (446-550) | 25.28 | 31.01 | 0 / 0 / 0 |
| 1000 | 12 / max | OFF | 38.29 | 998.1 (934-1000) | 30.32 | 33.89 | 2 / 0 / 0 |

Peak sampled process memory: 996.8 MiB. Mean FPS includes every frame in each sample; p99 is a frame-time percentile, not a separately computed 1% low FPS.

The earlier screenshot-assisted pass is retained as diagnostic evidence. It had ~500 ms GPU waits around captured stages; clean results above must be used for latency conclusions. Screenshots were collected outside this clean measurement. The owner's separately reported PC/Android hitch remains an independent investigation.

UWP startup fix: XAudio2 device output replaces the crashing WASAPI activation path in this port only; mixer/effects/decoders and desktop/mobile backends are retained. Owner confirmed music/effects. Windows and Linux audio suites each passed 41 cases / 1157 assertions; Android arm64 audio and UWP Release compiled.

Reproduce with hordewake/tools/prepare_console_benchmark.py and tools/benchmarks/console_stress.luau. Raw log, JSON and CSV accompany this report. Normal game package and saves remain separate.
