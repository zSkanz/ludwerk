# 0171 — The GPU's time by pass, and the keys that take things out of a frame

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, at the head of the mobile performance batch
  (ludwerk-08, on the owner's request that the engine be made fast on a
  phone), under the standing rule to decide as professional engines do and
  record it
- Builds on: `[debug] frame_report_seconds` (the mobile ledger), ADR 0147
  (settings), the `--frame-stats` scopes (H0)

## Context

On the owner's phone the GPU is the frame: 99% busy at the phone's default
level, 21 to 35 frames a second, and the game's own scripts 2 ms of it. A
read-only audit named fourteen things the GPU does that it might not have to
and could not rank them, because nothing in the engine says what a pass costs
on the GPU. `--frame-stats` times scopes on the CPU, where recording a pass is
microseconds whatever the pass costs to draw.

The phone has no profiler attached and takes no command line. What comes back
from it is its log.

## How mature engines do it

- **Unreal**: `stat gpu` and `ProfileGPU`, per-pass times from timestamp
  queries; `r.*` console variables and show flags (`show Foliage`,
  `show Particles`) to take a feature out of the frame and see the difference.
- **Unity**: the Frame Debugger and the GPU module of the profiler; on a
  device, the same over a connection, and a development build's toggles.
- **Godot**: the Visual Profiler, per-pass CPU and GPU time, and
  `RenderingServer.viewport_set_measure_render_time`.

All three have both halves: a time per pass, and a way to remove a thing and
measure again. The second matters as much as the first -- a pass's time says
where the GPU is, and removing what it draws says why.

## Decision

1. **A frame can be timed pass by pass**: `[debug] gpu_pass_times = true` in
   the project's file, or `--gpu-pass-times`. `IDevice::setPassTiming` and
   `IDevice::passTimes` are the seam.

2. **By stopping the frame, not by a query.** SDL's GPU API has no timestamp
   query and gives no native handle to make one with. A timed frame is
   recorded as one command buffer per pass; each is submitted and waited for
   before the next is recorded, so the GPU is idle when a pass starts and the
   wait is what the pass took. It needs nothing from the driver and nothing
   patched into SDL, and it is the same on Vulkan, Direct3D 12 and Metal.

   A patch to SDL's Vulkan backend that wrote timestamps was the other way.
   It was not taken: it measures one backend, it is ours to carry through
   every SDL upgrade, and on a tile-based GPU a timestamp inside a command
   buffer is written when the tiler gets to it, not when the pass ends.

3. **What a pass is named.** The debug group it was recorded in, and its own
   name after a slash where the two differ: `shadow`, `forward`,
   `forward/decals`, `bloom/bloom-down`. Passes that follow each other under
   one name are one stop. Uploads outside every group are `upload`.

4. **What is drawn to the window is one entry.** A window's texture is
   acquired on the frame's own command buffer and presented by it, so those
   passes cannot leave it; and once one has been recorded, every pass after it
   stays in that buffer too -- in a buffer of its own it would run before
   what was recorded ahead of it. The entry is named after all of them:
   `rcas+ui`. `[debug] hide = "ui"` tells the two apart.

5. **The floor is measured, not assumed.** Each timed frame also stops once
   for a pass that clears one pixel. Its time, `floor`, is what a stop costs
   whatever is in it, and every pass's number carries it; a pass that reads
   near the floor cost nearly nothing.

6. **It is a measuring mode.** The CPU waits for the GPU after every pass, so
   the frame rate of a timed run is not the game's, and the report says so in
   the line itself. The picture is the same, bit for bit (checked on seven
   examples, on Direct3D 12 and on Vulkan). On a phone the GPU's clock may
   fall while it waits and stretch every number: the passes are compared with
   each other and their sum with an untimed frame, not read as absolutes.

7. **The times are said under the frame report**, every
   `frame_report_seconds`, as a frame's mean over the frames since the last
   line, the costliest first; and once more at the end of the run.

8. **The keys that take things out**, each off unless asked:
   - `[debug] hide = "foliage,terrain"` / `--hide=LIST` -- a list of what is
     not drawn: `parts`, `skinned`, `terrain`, `voxels`, `foliage`,
     `transparent`, `particles`, `ribbons`, `decals`, `sprites`, `world_ui`,
     `ui`, `lights`, `highlights`. Taken out of the frame's world after it is
     extracted: the simulation goes on, so the difference in the frame's time
     is what drawing the thing cost. A name that is none of these is said and
     hides nothing.
   - `[debug] shadow_taps = N` / `--shadow-taps=N` -- how many taps the sun's
     shadow filter takes, 1 through 16, over the level's own (ADR 0172). The count
     travels in a lane of the frame's uniforms that was unused, and the
     sixteen-tap filter is the code it was: the default picture is unchanged.
   - What `[graphics]` already has needs no key here: `shadow_cascades = 0`,
     `contact_shadows`, `ambient_occlusion`, `bloom`, `anti_aliasing`,
     `upscaling`, `foliage_density`, `render_cap`.

9. **`[debug] log_ui_touches = true`** / `--log-ui-touches`: a line for each
   finger that comes down, with the whole name and the rectangle of the
   element of the interface that took it, or that none did. Not a measure of
   time; it is here because it is the same kind of key -- what a phone cannot
   be asked any other way.

10. **Every report says which keys are in force**, and so does the start of
    the run. A measurement is never read without knowing what it measured.

11. **A phone's build may take the host's flags when it is launched**
    (amended the same day, on ludwerk-08's finding that a matrix of fourteen
    settings was fourteen builds and fourteen installs). The activity hands
    the host what the launching intent's `args` extra says, split at spaces:

    ```text
    adb shell am force-stop <id>
    adb shell am start -n <id>/engine.player.PlayerActivity --es args "--gpu-pass-times --hide=ui"
    ```

    The host reads them only when the game's `project.toml` says
    `[debug] launch_arguments = true`. An installed game can be launched by
    any app on the phone, so a game that ships must not be one a stranger's
    intent can hand `--hide=` or `--join=` to: without the key the arguments
    are ignored, and the log says so. `--render-cap=N` is added as the flag of
    `[graphics] render_cap`, the one key of the matrix that had none.

12. **`[debug] skip = "shadow,environment"` / `--skip=LIST`: the terms of a
    lit surface that are left out** (amended the same day, on the phone's
    first numbers: the forward pass was two thirds of the frame, and `hide`
    says which objects cost it and not which part of lighting them). Any of
    `sun`, `shadow`, `contact`, `lights`, `environment`, `ambient`,
    `occlusion`, `fog`, `normal_map`, `material_maps`, and `unlit` for the
    base colour alone. Each is a branch on a uniform in the forward shaders --
    a lane of the frame's uniforms that was unused, zero on every frame
    nobody measures -- so the picture and its cost are unchanged without the
    key. `hide` by `skip` attributes the forward pass.

    Amended 2026-10-05, and taken back the same night: **the ground's
    fragment was given names of its own** (`ground`, `ground_maps`,
    `ground_gradients`, `ground_detail`, `ground_far`, `ground_blend`,
    `ground_paint`, `ground_rules`, `ground_noise`, `ground_flat_rules`),
    each a branch more in `terrain.hlsl`. They said what they were for on the
    desktop -- at 7680 by 4320, where the ground is 2.8 ms of the forward
    pass: its lighting 1.2, its map reads 0.9, the rules 0.5, its noise 0.1,
    and drawing it at all 0.2 -- and went to the phone in one package. But
    the shader that resulted is one Direct3D's software rasteriser could not
    run: on the machines that build `main`, with no GPU, the ground's
    pictures came out wrong and one run stopped on an access violation, where
    a GPU and Vulkan's software rasteriser drew them as before. A shader with
    every kind of ground behind branches was already the trouble (ADR 0179);
    more branches in it to measure the first ones was the same mistake. What
    takes the ground apart now is what replaced it: shaders compiled apart,
    `terrain_fast.hlsl` and `terrain_flat.hlsl`.

## What it does not do

- No depth prepass switch: the opaque pass draws at the prepass's depth and
  the sort relies on it. Its time is a line of the report.
- No cheap terrain path yet: that is the terrain tier the mobile batch
  builds. `hide = "terrain"` says what the ground costs in all.
- No timing in a shipped game by default, and no script API: the keys are a
  project's, read at start.

## Consequences

- `ICmdList` under a timed frame changes command buffers under the renderer.
  Uniform data pushed to a buffer belongs to that buffer, so the list keeps
  what was last pushed, by stage and slot, and pushes it again into each new
  one. Debug groups are kept as names and not given to the driver: a group
  opened in one buffer cannot be closed in the next.
- The debug overlay records through the native handle; it is given the buffer
  being recorded into, and a buffer the list did not use is submitted rather
  than cancelled, since the overlay may have.
- `GraphicsSettings::shadowTaps` is an instrument, carried across a player's
  settings changes as `debugView` and `instancing` are.
- Protocol unchanged (40).
