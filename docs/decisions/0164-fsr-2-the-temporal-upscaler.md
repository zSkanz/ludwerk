# 0164 — FSR 2: the world upscaled from the frames before it

- Status: accepted
- Date: 2026-10-04
- Decided by: the owner (2026-10-03, approving ADR 0158's batch: every
  upscaler on the market that R6 allows); the design is the agent's, under the
  standing rule to decide as professional engines do and record it
- Builds on: ADR 0158 (anti-aliasing and upscaling, whose second delivery this
  is the first half of), ADR 0116 (compute passes), ADR 0147 (the settings),
  ADR 0042 (vendoring narrowed by an include list)

## Context

ADR 0158 gave the engine two ways to draw the world below the window's
resolution: stretched, and FSR 1. Both read one finished frame. What one frame
does not hold cannot be put back by either: at half the resolution a bar a
pixel wide is there or it is not, and an edge is in steps twice as tall.

A temporal upscaler reads many frames. Each is drawn a fraction of a pixel off
from the last, so over a few dozen of them every part of every output pixel
has been rendered, and a pass that knows how each pixel moved can build the
output from all of them. It is also an anti-aliasing: the same accumulation at
a scale of 1 is a sharper TAA.

ADR 0158 left it out for one reason, and said so: FSR 2's passes are compute
shaders that write images, and the RHI's compute passes wrote buffers only.

## How mature engines do it

- **Unreal**: one slot in the frame for the temporal upscaler -- its own TSR,
  or FSR 2, DLSS or XeSS as plugins. It takes TAA's place. It runs after the
  scene and before the post-processing that wants the output's resolution:
  bloom, exposure and the tonemap read what it wrote. The scale is the screen
  percentage; the plugins add their named modes over it.
- **Unity (HDRP)**: FSR 2 as one of the dynamic-resolution upscalers, in TAA's
  place, before the post chain. A reactive mask is made from the transparent
  pass.
- **Godot 4**: FSR 2.2 as a 3D scaling mode beside bilinear and FSR 1. It
  vendors AMD's source and drives the shaders through its own rendering
  device, not AMD's Direct3D or Vulkan backends. Selecting it turns the
  engine's TAA off. At a scale of 1 it is offered as the anti-aliasing.

All three: one upscaler a frame, in the temporal pass's place, before
exposure and bloom, with the scale as the dial and AMD's ratios as names for
it. Godot's is the integration this engine's RHI allows.

## Decision

1. **`Enum.UpscalingMode.FSR2`**, beside `None` and `FSR1`, written
   `upscaling = "fsr2"` in a project's file and `--upscaling=fsr2` on the
   command line. On a frame it upscales it **takes the temporal pass's place
   and the upscale's**, and `AntiAliasing` is not read. At a `RenderScale` of
   1 it is the anti-aliasing alone.

2. **The scale is `RenderScale`, and its floor is now a third.** AMD's names
   are scales and nothing else: quality 0.67, balanced 0.59, performance 0.5,
   ultra performance 0.33. The settings screen steps through them. No second
   setting names them: a level that only writes another setting would be two
   places to read one number from. The floor was a half while the only
   upscales read one frame; it is the game's to choose.

3. **AMD's source, not AMD's runtime.** FSR 2.2.1 is vendored as its shader
   headers alone (MIT; `third_party/fidelityfx_fsr2`, narrowed by the include
   list). The algorithm is those headers. AMD's C++ runtime, with its
   Direct3D 12 and Vulkan backends, is not used: it binds resources by bare
   register and creates them through the platform's API, and the engine draws
   through its RHI on every platform. What the runtime does besides -- the
   frame's constants, the order of the passes, which image of a pair is read
   -- is written in the renderer after `ffx_fsr2.cpp`, field for field.

   The headers are written to be compiled against a **callbacks header** the
   integrator supplies, and the engine's is derived from AMD's by a script
   (`tools/repo/fsr2_callbacks.py`) whose every change is listed at the head
   of the file it writes (`shaders/include/engine_fsr2_callbacks.hlsli`). The
   vendored copy is never edited, and a newer FSR 2 is the script run again.
   The changes that are decisions:

   - **A sampler a texture.** The RHI binds a texture with its sampler, and a
     texture a shader only loads from is another kind of binding to it, in
     another range of slots. Every texel read goes through the texture's own
     sampler at the texel's middle, which gives that texel exactly.
   - **Three formats for every image a pass writes**: sixteen-bit float in
     four channels, thirty-two-bit float in one, eight bits in four. They are
     the ones every device stores to with no feature asked of it. AMD's are
     narrower where it keeps two channels or one, and those are a Vulkan
     extension the RHI does not ask for. The price is memory, below.
   - **The previous depth is AMD's, atomic where the target has atomics.**
     The reconstruction pushes each pixel's depth onto the four pixels it
     reprojects to and keeps the nearest with an atomic minimum on an integer
     image. Direct3D and Vulkan have that, and it is what they run. Metal's
     shading language has had atomics on an image only since its version
     3.1, so there the minimum is read, compared and written: two threads
     that push onto one pixel in one dispatch can leave the further depth,
     and a pixel that came out from behind something is missed for a frame.
     The build says which target a compute shader is compiled for
     (`ENG_SHADER_MSL`); what a shader binds may not depend on it.

     Two other ways were built and measured before this one. A float image
     written without the atomic on every target gave the same number on the
     flicker gate and a picture a few hundred pixels off on a scene of moving
     things. A **gather** -- each pixel written once, from the pixels round
     where its own motion says it came from -- was the same on every device,
     cost a pass of its own (0.8 ms at 3840 x 2160), and missed the case the
     image exists for: something that crossed a pixel a frame ago is not
     round where the background's motion points.

     The image is read by the next pass through the binding it is written
     by: an integer image is not sampled.
   - **The luminance is one image**, thirty-two pixels a patch, made by a pass
     of the engine's own. AMD's is a mip of a chain built with atomics, and
     that level is the only one its passes read. The rest of the chain is its
     automatic exposure, which the engine's own replaces: the passes are given
     the luminance the engine metered last frame, made a gain as the tonemap
     makes it.

4. **Where it runs.** After the look's scene passes -- depth of field, sun
   rays, blur -- and before exposure, bloom and the tonemap. What it writes is
   the scene at the window's size, still unexposed. Exposure and the first
   level of bloom read it at that size; the bloom chain itself stays the
   render size's. The tonemap then writes the window directly: no spatial
   anti-aliasing, no second upscale, no sharpening after it.

5. **Sharpening is FSR 2's own RCAS**, on the scene before exposure, by
   `Sharpness` as AMD maps it: nothing at 0, its most at 1. At 0 the pass is
   not run.

6. **The jitter is Halton's, as TAA's is, and longer the smaller the world is
   drawn**: eight samples for each output pixel a rendered one covers, AMD's
   rule. Motion is the velocity target TAA already writes.

7. **What blends is said to it.** A flame, smoke, glass and a label in the
   world write no depth and have no motion of their own. FSR 2 would follow
   the surface behind them and drag them into streaks, as TAA does. On a frame
   with anything blended, the scene is kept as it is before the first of
   them, and a pass after the last measures how far each pixel has moved from
   it (`fsr2_reactive.hlsl`). That is the **reactive mask** FSR 2 takes: a
   pixel in it is built mostly from this frame. Each frame also keeps six
   tenths of the last frame's mask, so the pixel a spark has just left, which
   has the spark in its history and nothing in this frame to say so, is
   distrusted for a few frames more. A sprite drawn in its own colours
   (ADR 0153) is in the mask whole.

8. **Where it does not run, FSR 1 does, and the engine says so once.** The
   world's main camera, with perspective, on a device with compute shaders.
   Otherwise the frame is smoothed by `AntiAliasing` and upscaled by FSR 1:
   - a device without compute, or without the memory for the images
     (`render.warn.fsr2_unavailable`, logged once);
   - a camera without perspective: the passes read distance out of depth as a
     perspective projection writes it;
   - a view into a texture, a picture of sprites alone and a picture with
     nothing behind it, for ADR 0158's reasons for TAA.

9. **No preset chooses it.** Low stays FSR 1 at three quarters: a machine that
   needs Low is short of exactly the GPU time FSR 2 costs. A handheld's
   presets never choose it. It is the game's and the player's to turn on.

10. **The RHI: a compute pass writes textures.** `beginComputePass` takes the
    textures it writes beside the buffers, a mip of each
    (`ComputeTextureWrite`). A texture says so when it is made:
    `TextureUsage::ComputeStorageWrite`, or `ComputeStorageReadWrite` for one
    a pass reads back through the binding it writes it by -- an atomic is
    both -- in thirty-two-bit single-channel formats, the ones every device
    allows that for. `TextureFormat::R32Uint` is new, for the image the
    atomic is done on. The capture device records a written texture only
    where there is one, so every stream recorded before is the stream it was.

11. **Frame generation is not in this ADR.** It needs frames presented that
    were never rendered, paced between the ones that were, with the UI drawn
    on each -- a decision about the present path, and its own delivery.

## Consequences

- **Memory.** Fifty bytes a rendered pixel and fifty-two an output pixel:

  | Window | At quality (0.67) | At performance (0.5) |
  |---|---|---|
  | 1920 x 1080 | 154 MB | 134 MB |
  | 2560 x 1440 | 274 MB | 238 MB |
  | 3840 x 2160 | 615 MB | 535 MB |

  About a third more than AMD's own images take, which is the price of the
  three formats. The images are released the frame the setting is turned off.

- **Time**, on a GeForce RTX 4070 Ti SUPER through Direct3D 12, the shipping
  build, a scene whose own frame is a millisecond:

  | Window | Scale 1 | 0.67 | 0.59 | 0.5 | 0.33 |
  |---|---|---|---|---|---|
  | 1920 x 1080 | 0.52 ms | 0.38 | 0.36 | 0.33 | 0.25 |
  | 2560 x 1440 | 0.92 ms | 0.63 | 0.59 | 0.53 | 0.45 |
  | 3840 x 2160 | 1.92 ms | 1.33 | 1.21 | 1.10 | 0.91 |

  What the passes add to the frame, sharpening included: the frame with them
  less the same frame without, the least of three runs' medians. TAA adds
  0.1 to 0.4 ms at the same sizes, FSR 1 with SMAA about as much. A slower
  GPU pays in proportion; a device whose compute is its CPU -- CI's -- pays
  seconds.

  **Reading a texel through a sampler cost a quarter of that until the
  texel's position came from the frame's constants**: asking the texture its
  size at every read was 0.7 ms of 3.1 at 3840 x 2160.

- **Thin things against a far background shimmer at a scale of 1.** A pole
  half a pixel wide in front of the sky is rendered in some frames and not in
  others. FSR 2 reads the frames without it as the sky having come out from
  behind something, and drops its history there. TAA blurs the pole instead.
  On the flicker gate's scene: FXAA 0.23, TAA 0.13, FSR 2 at a scale of 1
  0.34, at 0.67 0.10, at 0.5 0.08 -- at the lower scales the poles are not
  drawn at all. It is the algorithm's and not the integration's: the number
  is the same to four digits with the atomic, without it and with the gather.
  A scene of wires at full resolution is one for TAA.

- **The wire is untouched**, and so is every golden: the setting is off unless
  it is asked for, and a frame without it issues the commands it always did.

- **The passes compile for Vulkan and Metal as they do for Direct3D**, and
  the gate below runs wherever CI has a device. No phone has run it. The
  images' formats and the passes ask nothing a Vulkan or Metal device with
  compute lacks.

- Tests:
  - `temporal_upscale_gate`: a still scene of edges and thin bars, drawn at
    full resolution and at half of it. FSR 2 from half must leave fewer than
    three fifths of the visibly wrong pixels FSR 1 leaves, and fewer at a
    scale of 1 than from half. Measured here: 3254 against 8353 of 230 400,
    and 612.
  - the RHI's: a compute pass that writes a texture records which, and one
    that writes none records what it always did; the two usages are apart.
  - by hand, and not a gate: `Upscaling`, `RenderScale`, `AntiAliasing` and
    `Sharpness` changed every five frames for 420 frames under Direct3D's
    debug layer, with particles in view, without a message from it.
  - settings: the scale clamps at a third; `fsr2` is read from a project's
    file and from the command line.

## Not decided here

- **Frame generation** (FSR 3), as above.
- **A transparency-and-composition mask.** FSR 2 takes a second mask, for
  pixels whose colour changes without moving -- a scrolling texture, a screen
  in the world. Nothing in the engine says which those are yet, and the
  reactive mask covers what blends.
- **DLSS and XeSS** stay out, against R6 (ADR 0158).
