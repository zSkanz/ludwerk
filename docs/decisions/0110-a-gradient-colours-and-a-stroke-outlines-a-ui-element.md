# 0110 — A gradient colours a UI element and a stroke outlines it

- Status: accepted (to be built next; see `docs/briefs/owner-queue-2026-09-27.md`, Q1)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, in conversation: *"UIGradient - Com os
  novos betas do roblox também / UIStroke - Com os novos betas do roblox
  também"*, and research into what those two classes are today, betas
  included. The research read the other platform's public documentation and
  announcements (its reference pages, its enum pages, its rich-text guide and
  the announcements of September 2025 and September 2026); nothing else. R7
  holds: concepts and names from public documentation, never code, and nothing
  in the engine's code names where they came from.
- Relates to: ADR 0040 (the UI), D030 (`UICorner`, the one appearance
  modifier so far), ADR 0034 (naming), ADR 0107 (a `ViewportFrame` will take a
  gradient too).

## Context

The UI has one appearance modifier, `UICorner`, drawn as a signed distance in
the 2D shader so a rounded panel costs what a square one does. What a HUD is
made of past that -- a health bar that fades from green to red, a button with
a glow, an outline on a title so it reads over any background, a radial
progress ring -- is not possible without an image made for it.

The platform whose API shape this engine follows has two classes for exactly
this, and in 2025 and 2026 it extended both:

- A **gradient** child that colours and fades its parent along a colour
  sequence and a number sequence, rotated and offset -- and, since September
  2026, radial and conical shapes, repeat and mirror tiling, and a scale on
  the sequence's extent.
- A **stroke** child that outlines its parent's text or border -- and, since
  the 2025 release, a thickness scaled to the parent, a position inside, on or
  outside the border, an offset, several strokes on one element ordered by
  their own `ZIndex`, and a `<stroke>` tag in rich text.

Neither can be written without the other half of the API: a colour sequence
and a number sequence are values this engine does not have. Particles got
around that with start and end pairs; a gradient is more than two stops.

## Decision

### 1. Two value types: `ColorSequence` and `NumberSequence`

- `ColorSequence` holds up to 20 `ColorSequenceKeypoint`s (`Time`, `Value`:
  a `Color3`); `NumberSequence` up to 20 `NumberSequenceKeypoint`s (`Time`,
  `Value`, `Envelope`). Times rise from exactly 0 to exactly 1.
- Built from a module, camelCase as ADR 0034 has it:
  `ColorSequence.new(color)`, `ColorSequence.new(from, to)`,
  `ColorSequence.new({keypoints})`, `ColorSequenceKeypoint.new(time, color)`,
  and the same three and one for numbers (`NumberSequenceKeypoint.new(time,
  value, envelope?)`). Read off the object, PascalCase: `.Keypoints`, `.Time`,
  `.Value`, `.Envelope`. Immutable, compared by value.
- They are ordinary values: properties and attributes hold them, a scene file
  saves them, they replicate and the world hash covers them. **Appended** to
  the value union, so no wire tag moves. Not tweenable.
- `Envelope` is kept and not used by the UI -- it means something only to a
  particle, which does not take a sequence yet.

### 2. `UIGradient`

A child of a `GuiObject` (`Frame`, `TextLabel`, `TextButton`, `ImageLabel`,
`ImageButton`; later a `ViewportFrame`) or of a `UIStroke`. It multiplies the
colour of what its parent draws by the sequence, and its opacity by one minus
the transparency sequence. It changes the drawing, never the layout or the hit
test, as `UICorner` does.

| Property | Type | Default | What it is |
|---|---|---|---|
| `Color` | `ColorSequence` | white to white | the colours along the gradient |
| `Transparency` | `NumberSequence` | 0 to 0 | the see-through along it |
| `Offset` | `Vector2` | (0, 0) | the centre moved by fractions of the parent's size |
| `Rotation` | number | 0 | degrees, clockwise |
| `Enabled` | boolean | true | off draws the parent as if it had none |
| `Type` | `Enum.GradientType` | `Linear` | `Linear`, `Radial`, `Conical` |
| `TileMode` | `Enum.GradientTileMode` | `Clamp` | `Clamp`, `Repeat`, `Mirror` |
| `Scale` | number | 1 | the sequence's extent: below 1 it repeats per `TileMode` |

- **Linear**: across the parent in the direction `Rotation` points (0 is left
  to right), from the edge where that direction enters the box to the edge
  where it leaves -- so a rotated gradient still starts and ends at the
  corners it reaches.
- **Radial**: from the centre (plus `Offset`) outwards, a circle of radius
  `(width + height) / 4`. `Rotation` does nothing.
- **Conical**: clockwise round the centre (plus `Offset`), a full turn,
  starting where `Rotation` points -- 0 is to the right, as a linear gradient
  at 0 runs towards the right.
- **`Scale`** divides the position along the sequence: the distance between
  stops for linear, the radius for radial, the sweep for conical (never more
  than a turn). What lies past the sequence's end is the end colours
  (`Clamp`), the sequence again (`Repeat`), or the sequence reversed and again
  (`Mirror`).
- The first gradient child counts; one parent, one gradient.

### 3. `UIStroke`

A child of a `GuiObject`: an outline on its parent's text, or on its border.

| Property | Type | Default | What it is |
|---|---|---|---|
| `Color` | `Color3` | black | the stroke's colour; a `UIGradient` child colours it instead |
| `Thickness` | number | 1 | pixels, or a fraction (see `StrokeSizingMode`) |
| `Transparency` | number | 0 | its own see-through, apart from the parent's |
| `Enabled` | boolean | true | |
| `ApplyStrokeMode` | `Enum.ApplyStrokeMode` | `Contextual` | `Contextual`: the text on a text object, the border otherwise; `Border`: always the border |
| `LineJoinMode` | `Enum.LineJoinMode` | `Round` | `Round`, `Bevel`, `Miter` -- how a corner is turned; a `UICorner` makes it round |
| `StrokeSizingMode` | `Enum.StrokeSizingMode` | `FixedSize` | `FixedSize`: pixels; `ScaledSize`: a fraction of the parent's shorter side, or of the font size on text |
| `BorderStrokePosition` | `Enum.BorderStrokePosition` | `Outer` | `Outer`, `Center`, `Inner`: where on the border the stroke lies |
| `BorderOffset` | `UDim` | (0, 0) | moves a border stroke outwards (negative: inwards); scale against the shorter side |
| `ZIndex` | number | 1 | the order among the element's strokes, lower behind |

- **Several border strokes on one element** draw in `ZIndex` order, ties in
  child order. A text object takes one contextual stroke -- the first.
- A border stroke draws over the element's own drawing; a text stroke draws
  behind the glyphs it outlines. Neither affects layout: an outer stroke can
  overlap the element beside it.
- **Rich text** gains `<stroke color thickness transparency joins sizing>`
  (`th` and `tr` as short forms; `joins` is `round | bevel | miter`, `sizing`
  is `fixed | scaled`), which strokes the text it encloses as a `UIStroke`
  would.

### 4. How it is drawn

- **One pipeline, still.** The UI vertex grows the gradient's frame (where
  this fragment is in the element's box, the box's half-size, the type, tile,
  rotation, offset and scale) and the stroke's band; the shader evaluates the
  gradient and the stroke's coverage per fragment beside the rounded corner it
  already does. A quad with neither costs a compare.
- **A sequence is a row of a gradient table**: 256 texels of colour and
  opacity per distinct gradient, built on the CPU each frame a sequence
  changes, one texture for every gradient in the frame. Hard stops soften over
  a 256th of the span -- the same limit the other platform documents.
- **A text stroke is a stroked glyph in the atlas**: the glyph's coverage,
  dilated by the stroke's radius with the join's kernel (a disc, an octagon, a
  square), cached beside the plain glyph under a key that carries the radius
  in quarter pixels. Animating a text stroke's thickness mints glyphs, which
  is why the documentation says not to.
- The world's UI (`SurfaceGui`, `BillboardGui`) draws the same quads with the
  same two functions in its own shader.

## Consequences

- A HUD's health bar, a glowing button, a title readable over anything, a
  progress ring and a colour wheel are a few instances, with no image.
- Two value types join the API, and with them a sequence editor in the
  Properties panel: a bar showing the gradient, its keypoints dragged along it.
- The UI vertex grows. A HUD is hundreds of quads, so the size is noise; it is
  measured before and after all the same.
- Nothing on the simulation path changes, so no determinism trace moves; the
  world hash covers the new values because properties are hashed.

### Rejected

- **Start and end pairs, as particles have.** Two stops is not a gradient; a
  health bar with a yellow middle is the first thing anybody draws.
- **A second pipeline for gradients and strokes.** A HUD interleaves plain and
  graded quads in one run; switching pipeline per element is the cost the one
  pipeline exists to avoid.
- **A signed-distance glyph atlas for text strokes.** It would change how
  every glyph is drawn to serve an outline on a few; a dilated coverage glyph
  changes nothing for text without a stroke.
