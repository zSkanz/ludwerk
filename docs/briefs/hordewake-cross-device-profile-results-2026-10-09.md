# Hordewake: one-minute real-device stress profile

1500 target enemies, 12 maximum-level weapons, Warrior/forest/seed 7, boss and effects. 20 s warm-up, then 60 s measured. High quality, scale 1, no frame generation, no software FPS cap. VSync requested off: Windows immediate, Xbox mailbox; Android actually remained VSync/compositor paced.

Windows: i5-14600K + RTX 4070 Ti SUPER, SDL_GPU/D3D12, optimized RelWithDebInfo dev host. Xbox: physical Series S, native D3D12 UWP player in Game mode (5120 MiB). Android: SM-S938B, SM8750, Android 16, SDL_GPU/Vulkan ARM64 player. Phone plugged into AC; battery sensor 27.4 to 32.8 C across the run (not SoC temperature).

| Device | Resolution | FPS | Mean ms | Best ms | Worst ms | p99 ms | Mean enemies | Draws/frame |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Windows | 1920x1080 high | 106.06 | 9.428 | 5.846 | 18.273 | 13.709 | 1497.2 | 635.3 |
| Xbox Series S | 1920x1080 high | 52.25 | 19.140 | 13.670 | 36.464 | 29.019 | 1496.8 | 624.0 |
| Android SM-S938B | 2340x1080 high | 38.13 | 26.225 | 20.515 | 50.947 | 47.408 | 1496.8 | 688.5 |

## CPU scopes

All values are milliseconds per rendered frame, including frames without a simulation tick. Parents include children: do not add simulation to scripts, physics, animation or swarms, or render extraction to mesh extraction. Best = 0 can mean no tick that frame. Each row's worst occurred independently; maxima cannot be added. CPU scopes include waits; GPU execution time was not directly measured.

### Windows

| Scope | Mean ms | Best ms | Worst ms | p99 ms |
|---|---:|---:|---:|---:|
| Simulation (inclusive) | 2.253 | 0.000 | 8.140 | 5.683 |
| Game scripts / Heartbeat | 1.100 | 0.000 | 5.875 | 3.543 |
| Animation sampling | 0.465 | 0.000 | 2.378 | 1.088 |
| Native swarms | 0.263 | 0.000 | 2.104 | 0.613 |
| Physics (inclusive) | 0.278 | 0.000 | 4.710 | 0.669 |
| Physics apply | 0.208 | 0.000 | 4.551 | 0.514 |
| Physics solve | 0.037 | 0.000 | 0.920 | 0.129 |
| Render extraction | 1.360 | 1.061 | 2.893 | 1.780 |
| Mesh extraction | 0.928 | 0.714 | 2.388 | 1.250 |
| World render CPU | 3.892 | 3.213 | 8.940 | 4.899 |
| Particle update CPU | 0.073 | 0.040 | 3.103 | 0.142 |
| Render scripts | 0.160 | 0.104 | 0.747 | 0.258 |
| UI layout | 0.008 | 0.005 | 0.139 | 0.022 |
| Frame slot wait | 0.001 | 0.000 | 0.071 | 0.002 |
| Presentation wait | 0.673 | 0.522 | 2.119 | 0.940 |

Frames above 33.33/50/100 ms: 0/0/0. Actual enemies min/max: 1424/1500. Frame sample: 6364 frames / 60.001360 s; CPU capture: 6364 frames / 60.009690 s.

### Xbox Series S

| Scope | Mean ms | Best ms | Worst ms | p99 ms |
|---|---:|---:|---:|---:|
| Simulation (inclusive) | 9.436 | 0.000 | 28.658 | 19.285 |
| Game scripts / Heartbeat | 4.713 | 0.000 | 18.689 | 11.153 |
| Animation sampling | 1.945 | 0.000 | 4.458 | 3.953 |
| Native swarms | 1.155 | 0.000 | 2.454 | 2.194 |
| Physics (inclusive) | 1.075 | 0.000 | 9.963 | 2.108 |
| Physics apply | 0.852 | 0.000 | 9.709 | 1.683 |
| Physics solve | 0.098 | 0.000 | 0.236 | 0.203 |
| Render extraction | 3.115 | 2.495 | 5.244 | 3.833 |
| Mesh extraction | 1.941 | 1.539 | 3.440 | 2.424 |
| World render CPU | 2.362 | 1.970 | 9.627 | 3.961 |
| Particle update CPU | 0.142 | 0.095 | 1.477 | 0.831 |
| Render scripts | 0.287 | 0.210 | 0.715 | 0.421 |
| UI layout | 0.020 | 0.011 | 0.099 | 0.057 |
| Frame slot wait | 0.931 | 0.050 | 11.865 | 5.276 |
| Presentation wait | 1.160 | 0.151 | 10.017 | 9.312 |

Frames above 33.33/50/100 ms: 3/0/0. Actual enemies min/max: 1362/1500. Frame sample: 3135 frames / 60.003964 s; CPU capture: 3136 frames / 60.013892 s.

### Android SM-S938B

| Scope | Mean ms | Best ms | Worst ms | p99 ms |
|---|---:|---:|---:|---:|
| Simulation (inclusive) | 12.012 | 3.596 | 41.972 | 28.055 |
| Game scripts / Heartbeat | 6.359 | 1.578 | 23.466 | 16.651 |
| Animation sampling | 2.429 | 0.792 | 10.426 | 6.516 |
| Native swarms | 1.440 | 0.397 | 5.205 | 3.903 |
| Physics (inclusive) | 1.252 | 0.315 | 9.721 | 3.390 |
| Physics apply | 0.737 | 0.208 | 8.628 | 2.525 |
| Physics solve | 0.420 | 0.033 | 2.310 | 1.114 |
| Render extraction | 2.403 | 1.049 | 8.576 | 6.620 |
| Mesh extraction | 1.572 | 0.652 | 5.968 | 4.737 |
| World render CPU | 2.922 | 1.237 | 12.573 | 7.631 |
| Particle update CPU | 0.119 | 0.050 | 0.591 | 0.362 |
| Render scripts | 0.286 | 0.101 | 1.114 | 0.855 |
| UI layout | 0.017 | 0.005 | 0.198 | 0.055 |
| Frame slot wait | 0.124 | 0.048 | 0.783 | 0.346 |
| Presentation wait | 6.862 | 0.286 | 37.188 | 28.888 |

Frames above 33.33/50/100 ms: 187/6/0. Actual enemies min/max: 1424/1500. Frame sample: 2288 frames / 60.002576 s; CPU capture: 2289 frames / 60.017264 s.

## Limits and evidence

One pass per device, not statistical proof. Android renders 21.9% more pixels than 1920x1080 and uses the project's fast terrain variant; animation/platform policies can also differ. Use this as a profile of the actual platform configuration, not equal-work hardware ranking. The first Windows export selected an old prebuilt player; its runs were rejected. The measured Windows copy uses the freshly compiled engine host and shader/content directory. No mouse locking, synthetic-clock headless runs, screenshots during sampling, or artificial FPS cap. Frame-delta and CPU-scope windows can differ by one boundary frame. Raw scopes are retained in profiles.json and each engine log; absent scopes are not assumed zero.
