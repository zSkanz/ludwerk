# 0172 — What a level spends: the shadow filter by level, passes that draw nothing, and a handheld's bloom

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, in the mobile performance batch (ludwerk-08, on the
  owner's request that the engine be made fast on a phone), under the
  standing rule to decide as professional engines do and record it
- Builds on: ADR 0044 (graphics quality), ADR 0147 (settings), ADR 0158
  (anti-aliasing and upscaling), ADR 0171 (the GPU's time by pass)

## Context

On the owner's phone, at its cheapest level, the GPU does not hold sixty
frames a second in a fight of three hundred draws. An audit of the renderer
named what it does at every level alike:

- The sun's shadow is filtered with sixteen taps at every level. `Low` chose a
  smaller map and fewer cascades and paid the same sixteen four-texel reads at
  every lit pixel, each with its own sine, cosine and square root.
- The sixteen are taken where they decide nothing: on a face turned from the
  sun, under a sun of no brightness, past the last cascade, and in every pixel
  of a game with its shadows off, where they read an empty map.
- Passes that draw nothing still run. The sun's atlas is cleared and stored
  with no cascade to draw; the local lights' atlas, four million texels of
  depth, is cleared and stored in every scene with no casting lamp; a feature
  that is off -- occlusion, contact shadows, bloom -- is a clear of its whole
  target every frame; and the forward pass is opened again after the
  particles to draw a world interface that is usually not there.
- Bloom is nine passes at every level that has it.
- A handheld's `Medium` draws contact shadows: a pass over every pixel with
  twenty-one depth reads each.

On a tile-based GPU every pass is a load and a store of its target, so a pass
that draws nothing is not free.

## How mature engines do it

- **Unity (URP)**: soft shadow quality is a setting -- Low is a four-tap
  filter, Medium a 5x5 tent, High a 7x7 -- and the mobile defaults are the low
  ones; cascades and distance are separate dials. Bloom has a "max
  iterations" number, lower on mobile.
- **Unreal (mobile)**: `r.ShadowQuality` selects the filter, one to five; the
  mobile renderer's CSM filter is cheaper than the desktop's by default;
  bloom quality is `r.BloomQuality`, and mobile has no contact shadows.
- **Godot**: `shadow filter quality` from Hard to Soft Ultra changes the
  sample count; the mobile renderer defaults to Soft Low.

All three make the filter's cost a property of the level, and none runs a
screen-space contact pass at a phone's default.

## Decision

1. **The shadow filter's taps follow the shadow quality**: four at `Low`,
   eight at `Medium`, sixteen at `High` and `Ultra`
   (`GraphicsSettings::shadowTaps`, zero for sixteen). A player who turns
   `ShadowQuality` down alone gets the cheaper filter with the smaller map.
   No new key: it is what a shadow quality is.

2. **The disc is constants.** Vogel's disc as sixteen points in the shader,
   the first n of them scaled to radius one, turned by one angle a pixel: one
   sine and cosine a fragment where each tap had its own. Each of the three
   counts is a loop the compiler unrolls; a loop it cannot unroll was measured
   and costs more than it saves. At sixteen the picture is the one it was --
   no pixel of seven examples differs.

3. **No lookup that decides nothing.** A surface the sun does not light, and
   a sun of no brightness, skip the shadow map and the contact mask: what they
   would have said was multiplied by zero. A cascade the settings leave out
   -- or all of them, with `Lighting.GlobalShadows` off -- is not looked up:
   its far plane is out of reach and the answer is "lit" without a read.

4. **A pass with nothing to draw is not begun.**
   - The sun's atlas, when no cascade is drawn. Nothing reads it then (3).
   - The local lights' atlas, when no light casts. The shader skips that
     lookup already.
   - Occlusion, contact shadows and bloom that are off are cleared once, and
     again only after the targets are made anew or the feature has drawn.
   - The forward pass is reopened after the particles only when there is a
     world interface to draw in it.

5. **On a handheld**:
   - bloom has three levels, five passes where there were nine
     (`GraphicsSettings::bloomLevels`); the two it loses are the widest and
     the faintest;
   - there are no contact shadows below `High`.

   Both are a handheld's presets, not rules: `contact_shadows = true` in
   `[graphics.android]` turns them back on.

## What it does not do

- The band that blends one cascade into the next still looks both up. At four
  taps that is eight in the band and it was thirty-two; leaving the band out
  at `Low` would be a seam where the filter changes width.
- The interface is still a pass of its own over the finished picture. Drawing
  it inside the renderer's last pass touches every path that ends a frame --
  FXAA, SMAA, FSR 1's two, frame generation, the selection outline -- and
  waits for the phone to say what that pass costs (`[debug] hide = "ui"`).
- Nothing here changes a desktop's `High` or `Ultra`.

## Consequences

- `Low` and `Medium` draw a softer-edged shadow with more grain in the
  penumbra: four and eight samples of the same disc. FXAA and the upscale are
  after it.
- The render capture goldens lose the passes that are no longer begun, and
  the lavapipe and screenshot goldens taken at `Low` or `Medium` change with
  the filter.
- Measured on a desktop GPU at 3840 by 2160 (the forward pass, least of four
  runs): 1.84 to 1.72 ms at eight taps, 1.65 to 1.60 at four, 1.43 to 1.36
  with no cascades. A desktop barely feels a tap; the numbers that decide the
  rest of the batch are the phone's, taken with ADR 0171's instrument.
- Protocol unchanged (40).
