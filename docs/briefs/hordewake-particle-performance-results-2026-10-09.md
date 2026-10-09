# Shared particle palettes: Series S before/after

Physical Series S; UWP Game 5120 MiB; native D3D12, 1920x1080 High, scale1, frame generation OFF, frame cap0. Same sealed game fixture, seed, weapons and populations. Each case15s warm-up+30s sample. No Portal screenshots or controller input during measurement. Before=queue-overlap v0.8.49.6; after=particle-palette v0.8.49.7. Original v5 data is preserved separately.

Seven explicit picture bindings plus scene depth let one instanced draw keep alternating textures in the original transparency order. No texture atlas, texture resampling, particle-count reduction, lower resolution, lost effects or bindless-device requirement. Instance stride remains64 bytes. This shared renderer/shader change applies to SDL_GPU and native D3D12; the earlier queue optimization remains native-D3D12-specific.

| Mobs | Weapons | VSync | Before FPS | After FPS | Draws before / after | Fewer draws | p99 ms before / after | Worst ms after |
|---:|---:|:---:|---:|---:|:---:|---:|:---:|---:|
| 100 | 1 | ON | 59.94 | 59.94 | 573 / 485 | 15.2% | 16.84 / 16.81 | 17.05 |
| 300 | 6 | ON | 59.94 | 59.94 | 764 / 555 | 27.4% | 16.78 / 16.82 | 17.09 |
| 550 | 6 | ON | 59.71 | 59.77 | 982 / 583 | 40.7% | 17.95 / 17.78 | 28.74 |
| 1000 | 12 | ON | 55.83 | 56.17 | 1392 / 604 | 56.6% | 27.42 / 27.45 | 30.03 |
| 100 | 1 | OFF | 74.93 | 74.92 | 575 / 488 | 15.0% | 16.83 / 16.82 | 17.97 |
| 300 | 6 | OFF | 67.44 | 67.64 | 775 / 556 | 28.2% | 16.82 / 16.83 | 33.40 |
| 550 | 6 | OFF | 61.84 | 62.05 | 969 / 589 | 39.2% | 20.91 / 21.02 | 39.04 |
| 1000 | 12 | OFF | 54.93 | 56.17 | 1401 / 606 | 56.7% | 26.91 / 27.25 | 29.75 |

Peak sampled process memory: 1001.2MiB. Frames above33.333/50/100ms: 6/0/0. Raw JSON/CSV preserve actual population and weapon counts.

Fewer draw calls do not imply an equal percentage of FPS improvement. VSync keeps light cases near60; the extreme case still has other significant costs. One controlled before/after pass gives no statistical confidence interval. Engine FPS above60 with VSync OFF does not establish physical display refresh or120Hz support. Loading spikes and the separately reported PC/mobile hitch remain separate investigations.

Validation: Windows/Linux particles18 cases/1601 assertions each; Windows RHI29/1181 including actual seven-texture pixel readback and ordered alpha blending; Windows/UWP and full Android ARM64 player build. Shaders compile as DXIL/SPIR-V/MSL. SDL_GPU headless180-frame rendering checks on NVIDIA4070TiSUPER/D3D12 and IntelUHD770/Vulkan produce visually matching textured/procedural effects (mean per-channel image difference<0.3/255). These are functional checks, not PC FPS benchmarks. No mouse was captured. Android hardware and Metal runtime were not tested; no platform-specific FPS gain is claimed without device measurements.

Candidate APPX SHA256:56294e3f45237d76811d10faf3637692bf353038cc198c1b9b4445b7344abe52. Normal game remains separate; normal v0.8.49.8 is deployed and launched with the same engine/shaders, normal game export and 5120MiB Game budget. OFF550 had a39.04ms worst frame versus33.37ms before; six sample frames exceeded33.333ms versus four in v6. Do not claim universal pacing improvement from this pass.
