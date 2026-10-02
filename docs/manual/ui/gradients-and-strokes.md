# Gradients and strokes

Two instances change how an element looks without an image: a `UIGradient`
colours and fades it, and a `UIStroke` outlines it. Like `UICorner`, both change
the drawing and never the layout or the hit test
(ADR 0110).

## UIGradient

Parent one to an element. Whatever the element draws -- its background, its
picture, its text -- is multiplied by the gradient's colour and faded by its
transparency, point by point:

```luau
local bar = Instance.new("Frame")
bar.Size = UDim2.new(0, 300, 0, 24)
bar.BackgroundColor3 = Color3.new(1, 1, 1)
bar.Parent = screen

local health = Instance.new("UIGradient")
health.Color = ColorSequence.new({
	ColorSequenceKeypoint.new(0, Color3.fromRGB(220, 40, 40)),
	ColorSequenceKeypoint.new(0.5, Color3.fromRGB(240, 200, 40)),
	ColorSequenceKeypoint.new(1, Color3.fromRGB(60, 200, 80)),
})
health.Parent = bar
```

A white element shows the gradient's colours exactly; a coloured one is tinted
by them. `Transparency` is a `NumberSequence`: 0 leaves the element as opaque as
it was, 1 hides it, so a panel that fades out to the right is
`NumberSequence.new(0, 1)`.

### Sequences

`ColorSequence` and `NumberSequence` hold up to twenty stops. The first is at
time 0, the last at time 1, and times never fall; between two stops the value is
their straight mix, and two stops at one time make a hard edge.

```luau
ColorSequence.new(Color3.new(1, 0, 0))                       -- one colour
ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1))  -- red to blue
NumberSequence.new(0, 1)                                     -- opaque to clear
```

Both are values: a property or an attribute holds one, a scene saves it, and it
replicates. They are immutable -- build a new one to change it -- and cannot be
tweened; tween `Offset` or `Rotation` instead, which is also cheaper.

### Shape

`Type` is the gradient's shape:

- **`Linear`** runs across the element in the direction `Rotation` points
  (degrees, clockwise; 0 is left to right), from the edge the direction enters
  the box to the edge it leaves. Turned 45 degrees, it still starts in one
  corner and ends in the opposite one.
- **`Radial`** runs out from the element's centre to a radius of
  (width + height) / 4 -- a circle, even on a wide element. `Rotation` does
  nothing to it. A spotlight, a vignette, a glowing button.
- **`Conical`** sweeps clockwise round the centre, a whole turn, starting where
  `Rotation` points (0 is to the right). A progress ring, a colour wheel.

`Offset` moves the centre by fractions of the element's size: `Vector2.new(0.5,
0)` is half a width to the right. Animating `Offset` slides a gradient across an
element -- a shine passing over a button.

### Scale and tiling

`Scale` is how much of the element one run of the sequence spans: 1 is once
across, 0.25 a quarter. What fills the rest is `TileMode`:

- **`Clamp`** -- the end colours carry on.
- **`Repeat`** -- the sequence again from its start. Stripes, with a seam where
  the two ends differ.
- **`Mirror`** -- backwards, then forwards. Stripes with no seam.

For a radial gradient `Scale` is the radius, and rings come from `Repeat`; for a
conical one it is the sweep, never more than a turn.

A progress ring is a conical gradient with a hard edge moved along it:

```luau
local ring = Instance.new("UIGradient")
ring.Type = Enum.GradientType.Conical
ring.Rotation = -90 -- start at the top
local function setProgress(fraction: number)
	local at = math.clamp(fraction, 0.001, 0.999)
	ring.Transparency = NumberSequence.new({
		NumberSequenceKeypoint.new(0, 0),
		NumberSequenceKeypoint.new(at, 0),
		NumberSequenceKeypoint.new(at, 1),
		NumberSequenceKeypoint.new(1, 1),
	})
end
```

## UIStroke

Parent one to an element to outline it. On a text object it outlines the
**text**; on anything else, the **border**. `ApplyStrokeMode = Border` outlines a
text object's border instead.

```luau
local title = Instance.new("TextLabel")
title.Text = "GAME OVER"
title.TextColor3 = Color3.new(1, 1, 1)
title.Parent = screen

local outline = Instance.new("UIStroke")
outline.Thickness = 3
outline.Color = Color3.new(0, 0, 0)
outline.Parent = title
```

- `Thickness` is pixels. With `StrokeSizingMode = ScaledSize` it is a fraction
  of the element's shorter side instead -- of the font size, on text -- so the
  stroke grows with what it outlines.
- `Transparency` is the stroke's own, apart from the element's: a text stroke
  on text whose `TextTransparency` is 1 is hollow lettering.
- `LineJoinMode` is how a corner turns: `Round`, `Bevel` (cut across) or
  `Miter` (a point). A `UICorner` makes a border stroke round.

### Border strokes

`BorderStrokePosition` puts the stroke wholly **outside** the edge (the
default), **centred** on it, or wholly **inside** it. `BorderOffset` moves it
further out, or in when negative; its scale is a fraction of the shorter side.

**Several border strokes may share one element**, drawn in `ZIndex` order, lower
behind. A double border is two strokes, one offset past the other:

```luau
local inner = Instance.new("UIStroke")
inner.Thickness = 2
inner.Color = Color3.fromRGB(255, 210, 90)
inner.Parent = button

local outer = Instance.new("UIStroke")
outer.Thickness = 2
outer.Color = Color3.fromRGB(120, 70, 20)
outer.BorderOffset = UDim.new(0, 3)
outer.Parent = button
```

A stroke never moves the layout, so an outer stroke can overlap the element
beside it; leave room with `UIPadding` or the gaps of a `UIListLayout`.

### A gradient on a stroke

A `UIGradient` parented to a `UIStroke` colours the stroke and leaves the
element alone. The element and its stroke can each have their own:

```luau
local rainbow = Instance.new("UIGradient")
rainbow.Color = ColorSequence.new({
	ColorSequenceKeypoint.new(0, Color3.fromRGB(255, 60, 60)),
	ColorSequenceKeypoint.new(0.5, Color3.fromRGB(60, 255, 120)),
	ColorSequenceKeypoint.new(1, Color3.fromRGB(80, 120, 255)),
})
rainbow.Parent = outline
```

### In rich text

A text object with `RichText` takes a `<stroke>` tag around the words to
outline:

```
You won <stroke color="#00A2FF" thickness="2" transparency="0.25" joins="miter">25 gems</stroke>.
```

`color` is `#rrggbb` or `rgb(r,g,b)`; `thickness` (or `th`) and
`transparency` (or `tr`) are numbers; `joins` is `round`, `bevel` or `miter`;
`sizing` is `fixed` or `scaled`.

## What it costs

Both are drawn by the pass that draws every other element, with no extra draw:
a gradient is one texture read per pixel, a border stroke one more quad. A text
stroke is an outlined copy of each glyph, made once per glyph, size and
thickness and kept -- which is why animating a text stroke's `Thickness` is slow
while animating a border stroke's is not.

## Where to look next

- [Layout with UDim2](manual:ui/layout) -- `UICorner`, the third modifier
- [Text and images](manual:ui/text-and-images)
- [`UIGradient`](api:UIGradient) · [`UIStroke`](api:UIStroke) ·
  [`ColorSequence`](api:ColorSequence) · [`NumberSequence`](api:NumberSequence)
