# Series S native queue overlap: measured before and after

Physical Series S, native D3D12, UWP Game budget 5120 MiB; 1920x1080 High, scale 1, frame generation OFF, frame cap 0. Baseline package 0.8.49.5, candidate 0.8.49.6. Same sealed game export, shaders, seed, weapons/levels and benchmark. Each row: 15 s warm-up + 30 s sample. No Portal screenshots, player input or PC game windows during measurement.

Engine change: completed-fence resource/pipeline retirement and upload reuse replace two queue-wide waits per frame. Shader-visible descriptor heaps use the existing three fenced frame slots. Explicit resize/readback/teardown waits remain. No Hordewake-specific branch or visual-quality reduction.

| Mobs | Weapons | VSync | Before FPS | After FPS | Gain | p99 ms before / after | Worst ms before / after |
|---:|---:|:---:|---:|---:|---:|:---:|:---:|
| 100 | 1 | ON | 59.91 | 59.94 | +0.1% | 18.22 / 16.84 | 23.86 / 17.15 |
| 300 | 6 | ON | 51.90 | 59.94 | +15.5% | 21.73 / 16.78 | 28.28 / 16.96 |
| 550 | 6 | ON | 46.41 | 59.71 | +28.7% | 24.87 / 17.95 | 29.32 / 28.48 |
| 1000 | 12 | ON | 39.38 | 55.83 | +41.8% | 29.04 / 27.42 | 32.93 / 29.08 |
| 100 | 1 | OFF | 58.85 | 74.93 | +27.3% | 18.29 / 16.83 | 23.74 / 19.84 |
| 300 | 6 | OFF | 50.73 | 67.44 | +32.9% | 21.99 / 16.82 | 27.25 / 33.43 |
| 550 | 6 | OFF | 45.29 | 61.84 | +36.5% | 25.28 / 20.91 | 31.01 / 33.37 |
| 1000 | 12 | OFF | 38.29 | 54.93 | +43.4% | 30.32 / 26.91 | 33.89 / 29.15 |

Peak sampled process memory: 1000.0 MiB (baseline 996.8 MiB). Candidate sample counts above 33.333 / 50 / 100 ms: 4 / 0 / 0. JSON/CSV retain actual mob populations, draw counts, tails and per-case counts.

These are one before/after controlled pass, not confidence intervals. The 1000-mob/12-weapon stage deliberately exceeds normal game limits. Loading/scene-transition stalls remain outside these gameplay samples; startup shader preparation and render-script scene teardown are not claimed solved. Independently reported PC/Android hitches still need their own capture. Default SDL_GPU and Android do not use this native D3D12 backend, so these gains must not be attributed to them. No matched Unity/Unreal/Godot comparison was run.

Validation: Windows RHI 28 cases / 1137 assertions passed, no skips; native gate regression verifies three queued frames preserve distinct textures, uniforms and destroyed resources/pipelines, plus recording-time retirement serial. UWP Release /W4 /WX and Windows host built. APPX SHA256:367963fde7514f386ae869782a39166a7bb75362ed991774e63becda766f8d74. Normal game saves/identity remain separate.
