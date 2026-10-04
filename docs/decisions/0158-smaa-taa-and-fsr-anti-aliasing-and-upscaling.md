# 0158 — SMAA, TAA and FSR: anti-aliasing and upscaling

- Status: accepted
- Date: 2026-10-03
- Decided by: the owner approved the batch on 2026-10-03, relayed by ludwerk-08
  (SMAA, TAA, FSR 1, FSR 2/3 with frame generation; DLSS and XeSS out). The
  agent decided how, under the standing rule to decide as professional engines
  do and record it.
- Amends: the M7.5 brief's Decision 10 (FXAA only, no velocity buffer). Its
  reasoning, that a velocity target with no consumer is bandwidth for nothing,
  still holds. The consumer now exists.
- Dependencies: `third_party/smaa` (MIT) and `third_party/fidelityfx_fsr1`
  (MIT), pinned in the manifest. This ADR is their approval record (R5, R6).

## Context

The engine smoothed edges one way: FXAA, on at every quality level. The owner
sees jagged edges in still frames and crawling and shimmer in motion in the
paint-war FPS: wire fences, thin poles, far grass, a weapon's edge. FXAA finds
an edge by its contrast and softens across it. It cannot fix a line thinner
than a pixel, which appears in one frame and is gone in the next, because no
single frame holds it.

A world drawn below the window's resolution (`RenderScale`, and the handheld
cap) was also brought up by a bilinear stretch. That softens every edge it
crosses, and it does so on the hardware that most needs the scale, the phone.

## How mature engines do it

- **Unreal**: TAA, then TSR, its own temporal upscaler. FXAA is still
  available. FSR and DLSS ship as plugins. Velocity is written for moving
  objects, and the camera's motion is reconstructed from depth.
- **Unity** (HDRP and URP): FXAA, SMAA and TAA in the camera's options. FSR 1
  and FSR 2, and DLSS on HDRP. Motion vectors come from the camera, plus a
  per-object pass for renderers that moved.
- **Godot 4**: FXAA, TAA and MSAA. FSR 1 and FSR 2 as the scaling modes, with
  a sharpness setting.

The shared shape:

- A spatial pass on the finished picture.
- A temporal pass that needs two things the renderer already declared, a
  jittered projection and a motion vector per pixel.
- FSR 1 as the spatial upscaler, using AMD's portable source.

## Decision

1. **`Enum.AntiAliasingMode` is `Off`, `FXAA`, `SMAA`, `TAA`.**
   `GraphicsService.Upscaling` is `Enum.UpscalingMode` `None` or `FSR1`.
   `GraphicsService.Sharpness` is 0 to 1. All three are graphics settings
   (ADR 0147), in the anti-aliasing group. They live in the project file
   (`anti_aliasing = "smaa"`, `upscaling`, `sharpness`), on the command line
   (`--anti-aliasing=taa`, `--upscaling=fsr1`, `--sharpness=0.4`) and on the
   settings screen.

2. **SMAA 1x is its authors' reference shader**, vendored and compiled into
   three passes: luma edge detection, blending weights through the published
   area and search tables, and neighbourhood blending. It runs on the
   tonemapped picture, where FXAA runs. Its texture macros sample through the
   sampler beside each texture, because SDL_GPU pairs them slot for slot, so
   the vendored file is unchanged.

3. **TAA runs on the world's main view only.** Each frame is jittered by a
   Halton (2, 3) sequence of eight positions. The jitter is applied after
   extraction (`jitterCamera`), from `IRenderer::cameraJitter`.

   Velocity is written only on a temporal frame, in two passes:
   - **The camera's motion**, from the depth. The pixel goes back to the world
     through this frame's inverse view-projection, then into the last frame's
     camera. This covers everything that does not move by itself: ground,
     foliage, blocks and water.
   - **What moved by itself.** A draw carries a motion key, its instance. Each
     part whose transform changed since the last frame is drawn again with
     both transforms, against the prepass depth.

   A skinned mesh carries the motion of its whole, not of its joints. The
   resolve's neighbourhood clip takes the rest.

   The resolve works on the HDR picture, before exposure and bloom:
   - The nearest depth in each 3x3 block lends the block its motion.
   - The history is sampled by a Catmull-Rom filter.
   - The history is clipped toward a YCoCg variance box of this frame's
     neighbourhood.
   - The blend takes 10% of this frame at rest and up to 25% in motion,
     weighted by inverse luma.

   A camera that jumps more than 100 m, a resize, or a frame without TAA
   discards the history.

   A `ViewportFrame` or a sub-world asked for TAA uses SMAA. It is drawn when
   it changes and keeps no history, which also keeps a weapon layer from
   trailing.

4. **FSR 1 is AMD's portable header**, vendored and compiled into two fragment
   passes.
   - **EASU** brings the anti-aliased picture from the render size to the
     target's size.
   - **RCAS** sharpens into the target. `Sharpness` maps 0 to 1 onto RCAS's
     2 to 0 stops.

   RCAS also follows TAA at full scale when `Sharpness` is above zero. One
   patch to `ffx_a.h` spells three vector ternaries with `select`, because
   HLSL 2021 refuses them.

5. **The pass order** is:
   - forward → velocity → look passes → TAA → exposure → bloom → tonemap;
   - then FXAA or SMAA → EASU → RCAS.

   Any of the last three makes the tonemap write `ldr_`. A frame drawn
   through FXAA alone is the command stream it always was.

6. **Presets**:

   | Preset | Anti-aliasing | Upscaling |
   |---|---|---|
   | Low | FXAA | FSR 1, at its three-quarter scale |
   | Medium | SMAA | none |
   | High | SMAA | none |
   | Ultra | TAA | none |

   A handheld uses FSR 1 at every level, FXAA below High, and never TAA.

   High was the M7.5 output that every golden was recorded against. SMAA in
   it is the one change, and the goldens were recorded again for it.

7. **Out of scope**:
   - **DLSS and XeSS.** They are closed binaries, against R6. A future opt-in
     plugin is the owner's decision.
   - **FSR 2/3 temporal upscaling and frame generation** are this batch's
     second delivery. FSR 2's reconstruction of the previous depth writes
     with atomics, which needs storage textures in compute. The RHI does not
     have them yet (ADR 0116 gave compute storage buffers only). That
     extension, and the present-path decision frame generation needs, come
     with it.

## Consequences

- A temporal frame costs:
  - a velocity target (RG16F, added to the RHI's formats);
  - two HDR history images;
  - a fullscreen camera-motion pass;
  - one draw per moving part.

  A frame without TAA pays none of it.
- SMAA costs three passes and two small tables uploaded once. FSR 1 costs two
  passes and an image at the target's size.
- The capture goldens were recorded again for High's SMAA. A frame through
  FXAA (`--anti-aliasing=fxaa`) still matches the M7.5 stream.

## Amendment, 2026-10-03: the UI and the 2D layer

The engines that scale a picture scale its 3D. Godot's resolution scaling
reaches the 3D alone, and its 2D is drawn at full resolution. Unity, Unreal
and Godot all draw the UI after the upscale. A pixel-perfect 2D camera turns
anti-aliasing off and scales by whole numbers. Measured against that:

- **The screen's UI was right already.** It is drawn after the world's picture
  is resolved and scaled, at the window's resolution, so it is never scaled,
  smoothed or jittered.
- **A picture of sprites alone** (`spritesOnly`: sprites, and no mesh, terrain
  or foliage) is drawn at the window's resolution whatever `RenderScale` says.
  It upscales nothing and takes no temporal pass; TAA falls to SMAA for it, as
  for a view. A handheld's FSR 1 at three quarters no longer softens a 2D
  phone game. Particles, decals and world UI do not make a picture 3D.
- **Sprites among 3D surfaces follow the 3D picture**: its scale, its upscale
  and its temporal pass.
- **A view into a texture is UI** (D528): a `ViewportFrame`, a sub-world's or
  a camera's picture is drawn at its frame's own pixel size whatever the
  render scale says. At the world's scale it came out at a fraction of that,
  and the UI showed it in hard blocks.
- **Except a sprite drawn in its own colours** (ADR 0153), which is never
  jittered, blended, filtered or sharpened:
  - it is placed by the camera without the jitter;
  - the TAA resolve, FSR 1's upscale and RCAS's sharpening pass its pixels
    through, by the sprite mask;
  - scaled up, by FSR 1 or by the resolve, it takes the nearest texel.

  Such a frame used to fall to SMAA whole. The 3D around the pixel art now
  keeps its temporal pass.

`anti_aliasing_2d` holds all four:
- the label is the same pixels at half scale through FSR 1 and TAA as at full
  scale with none;
- a `ViewportFrame` is the same pixels at half scale through FSR 1 as at full
  scale;
- sprites alone at half scale through FSR 1 and TAA are the frame drawn at
  full scale through SMAA;
- a one-pixel checkerboard among 3D surfaces is only its two colours through
  FSR 1 and TAA, through TAA alone, and through FSR 1 and SMAA.

**For the second delivery**: frame generation interpolates the world alone.
The UI is drawn onto every presented frame, interpolated and real, at the
display's rate. This is the composition form AMD's integration guide prefers.
