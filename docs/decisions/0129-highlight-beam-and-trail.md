# 0129 — Highlight, Beam and Trail

- Status: accepted and built (2026-10-02; `docs/briefs/toolkit-kickoff.md`, F4)
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

## As built, 2026-10-02

- **A highlight is drawn over the FINISHED picture**, after the tone curve,
  not "after the opaque pass": it is a mark on the picture -- this one, an
  ally, pick this up -- and its colour must be the colour the game said at
  noon and at midnight. It is the editor's own selection silhouette with a
  game's colours: one mask and one composite a highlight, which is what the
  budget is a budget of.
- **`Occluded` is tested by hand**, against the scene's depth read as a
  texture, with room for the surface itself: the mask pass has no depth
  attachment, and two vertex stages do not land on the same depth to the bit.
- **Nested highlights**: the nearest marked thing above a draw wins.
- **Ribbons are drawn in the particles' pass, before the particles**, each
  kind in order within itself -- not sorted one against the other, for the
  reason particles are not sorted against glass: a per-quad sort in a per-draw
  list.
- **`TextureMode`** is `Stretch` (once over the whole length), `Wrap` (every
  `TextureLength` metres, sliding with a trail's moving end) and `Static`
  (every `TextureLength` metres, fixed where it was laid). `TextureSpeed` is
  in repeats a second when stretched and metres a second when wrapped.
- **`Beam.FaceCamera` is true by default and `Trail.FaceCamera` false**: a
  ray of light wants to face whoever looks, and a swing is the surface it
  swept.
- **They do not travel yet** (section 4 said they replicate as instances): a
  highlight names what it marks by reference, a beam and a trail name two
  attachments, and the wire has neither a reference field but `Parent`, nor
  `Attachment`, nor a sequence. Excluded with those reasons; a box in the
  network's leftovers.
- Every property is presentation: none is in the world hash, and a trail's
  pieces are the renderer's.
