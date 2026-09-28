# 0129 — Highlight, Beam and Trail

- Status: accepted (to be built; see `docs/briefs/toolkit-kickoff.md`, F4)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving the visual items of a survey
  of public documentation (R7).
- Builds on: [0072](0072-particles-are-a-picture-simulated-on-the-frame.md)
  (particles are a picture simulated on the frame), [0110](0110-a-gradient-colours-and-a-stroke-outlines-a-ui-element.md)
  (`ColorSequence` and `NumberSequence`).

## Context

Three effects every game reaches for have no instance: an outline on what is
selected or on an ally seen through a wall; a beam or a laser between two points;
a trail behind a sword or a car.

## Decision

### 1. `Highlight`

- `Adornee` (a part or model; the parent when empty), `FillColor`,
  `FillTransparency`, `OutlineColor`, `OutlineTransparency`, `DepthMode`
  (`AlwaysOnTop`, `Occluded`), `Enabled`.
- Drawn after the opaque pass: the adornee's meshes into a mask, the outline
  from the mask's edge at a constant width on screen, the fill over the mask.
- Budget: `[render] max_highlights` (default 32); past it, the nearest win and
  F3 says how many were dropped.

### 2. `Beam`

- Between `Attachment0` and `Attachment1`: `Color` (`ColorSequence`),
  `Transparency` (`NumberSequence`), `Width0`, `Width1`, `CurveSize0`,
  `CurveSize1` (a cubic between the two), `Segments`, `Texture`,
  `TextureLength`, `TextureMode` (`Stretch`, `Wrap`, `Static`), `TextureSpeed`,
  `FaceCamera`, `LightEmission`, `LightInfluence`, `ZOffset`, `Enabled`.

### 3. `Trail`

- Between `Attachment0` and `Attachment1` as they move: `Lifetime`,
  `MinLength`, `MaxLength`, `Color`, `Transparency`, `WidthScale`
  (`NumberSequence`), `Texture`, `TextureLength`, `TextureMode`, `FaceCamera`,
  `LightEmission`, `LightInfluence`, `Enabled`; `Clear()`.

### 4. The rules they share

- **Visual only**: on the frame, not the tick, like particles; nothing enters the
  trace. A trail samples its attachments' interpolated positions per frame.
- Beams and trails are drawn as ribbons in the transparent pass, sorted with
  particles; soft against depth as particles are.
- They replicate as instances; each client draws its own.
- A dedicated server draws none.

## Consequences

- Selection, sight through walls, lasers, ropes of light, sword swings and tyre
  marks, each as one instance.
