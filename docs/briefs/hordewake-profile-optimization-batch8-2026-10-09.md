# Hordewake optimization batch 8 — native D3D12 submission

Status: validated on PC; normal Windows folder and ZIP exported.

The user requested the largest remaining bottlenecks, PC only. Xbox is off;
phone permission remains withdrawn. All runs are windowless/offscreen, with
no pointer capture. Preserve the existing dirty engine/game worktrees.

## Accepted change

Native D3D12 recreates sampled resource descriptors on every draw, including
unchanged bindings. `Descriptors` now reuses immutable SRV tables within one
command recording. Keys include resource native identity, resource counts and
restricted mip/layer views. Samplers retain their existing separate cache.
Writes remain fresh. Every bind still performs resource state transitions,
even on a cache hit. Heaps are bound once per recording, and both caches and
the heap-bound flag reset after the owning frame slot has been fenced.

This is native D3D12 only; SDL_GPU and Vulkan do not use this implementation.
No shadow, image, animation or simulation quality settings changed.

## Measurement protocol

Evidence: `C:/Users/juanr/Downloads/Hordewake-Heavy-opt8-results`.
Baseline executable: `engine-host-opt8-control.exe` beside the development host.
Fixture: `%LOCALAPPDATA%/Ludwerk/build/hordewake-profile-1500-opt2-v1`.
Serial `--headless --frames=5100 --exit --rhi=d3d12 --width=1920
--height=1080 --frame-stats` runs. Target 1500 mobs, 12 weapons, High graphics.
Profile starts at simulated 20 s. Synthetic benchmark-result FPS remains
invalid; use actual wall-clock CPU frame/profile scopes instead.

Two controls and two candidates completed serially, each with 5100 frames.
Mean of the two CPU scope medians (parents inclusive):

| Scope | Before ms | After ms | Change |
|---|---:|---:|---:|
| Frame wall time | 10.7537 | 9.9339 | -7.62% |
| Render world CPU | 4.6640 | 3.9489 | -15.33% |
| Shadows CPU | 1.4032 | 1.2652 | -9.84% |
| Prepass CPU | .3548 | .3150 | -11.24% |
| Forward CPU | 1.6620 | 1.2191 | -26.64% |
| Lights CPU | .6533 | .5663 | -13.33% |

Simulation (-.97%) and extraction (-.36%) are effectively unchanged; this
change targets native graphics submission. Neither GPU execution time nor
physical Xbox gains were measured. No universal hitch fix is claimed.
Live populations/effects vary slightly. Raw logs and comparison.py reproduce
the table; comparison.json preserves medians, p95 and worst per run.

## Validation

New native regression samples real pixels before/after intervening render
writes, rotates seven frames, checks the debug queue, and rebinds a table
66000 times per frame to exceed the old descriptor exhaustion limit.
Existing native GPU tests include changing resource bindings, compute buffers,
array mip/layer views, asynchronous frame overlap and alpha ordering.

- Windows native GPU suite: 8 cases / 840 assertions, all pass using WARP
  with the D3D12 debug layer. New regression checks the debug queue for errors
  and corruption. Seven frame rotations and 462000 repeated table binds pass.
- Full Windows RHI suite: 30 cases / 1311 assertions, no failures or skips.
- Seven Windows capture gates pass, including terrain bindings.
- Clang-format 18 applied to the three touched files.
- Offline UWP `engine_rhi_d3d12` library compiles successfully. This is a
  compile check, not a fresh Xbox package or physical device validation.
- Exported normal game completed a 240-frame native D3D12 windowless smoke
  run, exit 0. Exported executable's PE `.text` SHA256 matches the tested
  development host (resource stamping changes the full file size/hash).
- No game scripts, platform SDKs, shader code, renderer quality settings,
  third-party sources or public RHI APIs were changed in this batch.

The baseline development binary is retained separately. Native D3D12 source
files are pre-existing untracked work in this checkout: do not discard that
directory because `git diff` omits it. Evidence includes final source copies.

## Delivered artifacts

- `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized8-windows/`
- `C:/Users/juanr/Downloads/Hordewake-0.8.49-optimized8-windows.zip`
- `Hordewake-D3D12.cmd` explicitly selects native D3D12. The normal executable
  retains the default backend. Do not claim this optimization benefits that
  default SDL_GPU backend.
- Exported executable: 47888384 bytes. Artifact hashes are recorded in the
  evidence directory's `artifact-sha256.json`. The export CLI emitted no
  progress text in this invocation; exit 0, output timestamps, native code
  section parity and the exported-game smoke run confirm the fresh artifact.

## Remaining priorities

Shadows and geometry submission remain material CPU costs. SDL_GPU/Vulkan
require independent measurement and implementation; this cache does not run
there. Future common renderer improvements should preserve cascades, effects,
geometry, animation and terrain quality. Xbox/Android retesting stays pending
until the user makes those devices available again.
