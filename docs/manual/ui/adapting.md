# Fitting any screen

A `UDim2` already makes one layout right at every size. These are for what a
fraction and an offset cannot say: "never narrower than this", "always square",
"the whole window, a little larger", "this text, but readable"
(ADR 0128). Each is parented to the element it is about.

**Constraints apply after an element's own `Size` and before its children are
laid out**, as a clamp -- arithmetic, like everything else in the layout.

## UISizeConstraint

Keeps its parent between two sizes, in the screen's own units:

```luau
local panel = Instance.new("Frame")
panel.Size = UDim2.fromScale(0.3, 1) -- a third of the window

local limits = Instance.new("UISizeConstraint")
limits.MinSize = Vector2.new(240, 0) -- and never too narrow to read
limits.MaxSize = Vector2.new(480, 0) -- nor wider than is comfortable
limits.Parent = panel
```

**A `MaxSize` axis of zero is no limit**, which is what a new one has.
`math.huge` is taken as the same and reads back as zero -- a number a scene file
can hold. Where the two disagree, `MinSize` wins.

## UIAspectRatioConstraint

Keeps its parent one shape. `AspectRatio` is width over height: 1 is a square,
`16 / 9` a widescreen picture.

| `AspectType` | The element becomes |
|---|---|
| `FitWithinMaxSize` | The largest box of that shape inside its own size. |
| `ScaleWithParentSize` | As long as its **parent** on `DominantAxis` (`Width` or `Height`), and whatever the shape makes of the other axis -- which may be more than the parent has. |

```luau
local icon = Instance.new("ImageLabel")
icon.Size = UDim2.fromScale(0.8, 0.8) -- most of a slot of any shape
local square = Instance.new("UIAspectRatioConstraint")
square.Parent = icon -- and square in all of them
```

The anchor places the box the element ends up with, so a centred element stays
centred. With a `UISizeConstraint` on the same element, the size limit has the
last word: "never under 240 wide" is the harder promise.

## UIScale

Scales its parent **and everything inside it**: what is drawn, and where a
press lands.

```luau
local scale = Instance.new("UIScale")
scale.Parent = window

window.Visible = true
scale.Scale = 0.9
TweenService:Create(scale, TweenInfo.new(0.2), { Scale = 1 }):Play()
```

The element takes `Scale` times its size -- a list or a grid it is in makes
room for that -- and its insides are laid out as if nothing had changed and
then scaled with it: offsets, text, corners and strokes alike. That is the
difference from changing `Size`, which would leave a 20-unit label 20 units
tall in a window twice as large.

Scales multiply: one inside another, and both inside a `ScreenGui` with a
`ReferenceHeight`. A HUD a player can make larger is one `UIScale` on its root.

## UITextSizeConstraint

Keeps **scaled** text between two sizes. A `TextScaled` label grows to fill its
box, which on a phone is unreadable and on a monitor is a billboard:

```luau
title.TextScaled = true
local limits = Instance.new("UITextSizeConstraint")
limits.MinTextSize = 18
limits.MaxTextSize = 48
limits.Parent = title
```

In the element's units, like `TextSize`. It does nothing to a label whose text
is not scaled: that one is `TextSize`, as written.

## CanvasGroup

A `Frame` whose descendants are drawn together into a picture, and the picture
shown as one. What it is for is fading a window:

```luau
local window = Instance.new("CanvasGroup")
window.Size = UDim2.fromScale(0.6, 0.6)
window.Parent = screen
-- ... the window's contents, as children ...

window.GroupTransparency = 1
TweenService:Create(window, TweenInfo.new(0.3), { GroupTransparency = 0 }):Play()
```

Fading each child by itself makes them show through each other on the way -- a
button's label through the button, the button through the panel.
`GroupTransparency` fades the finished picture; `GroupColor` tints it.

What to know:

- **It clips.** Nothing inside a group is drawn outside its box.
- **`ZIndex` inside it orders its contents among themselves.** On the screen
  the group is one thing, at its own `ZIndex`.
- **The picture is drawn again only when something in it changes.** A still
  window costs one quad a frame. A group holding a `ViewportFrame` or a
  `UIGradient` is drawn every frame, since those change without the group
  being told.
- **A group in a group** is a picture in a picture, and works.
- **Up to 32 of them show at once**, none larger than 4096 pixels a side. Past
  either, a group draws as the frame it is -- its contents in place, unfaded.
- **On a `SurfaceGui` or a `BillboardGui` it is a frame**: a world canvas is
  drawn in the world's pass, which has no pictures to draw into.
- A press lands on what is inside it exactly as it would in a `Frame`.

## Three sizes

The layout that holds on a phone held upright, a laptop and a wide monitor is
usually: a `ScreenGui.ReferenceHeight`, so units are the same fraction of every
screen; sizes in scale; a `UISizeConstraint` where a fraction gets too small or
too large; a grid or a wrapping list where the count across should change with
the width. `examples/32-menus` is that window; run it at three sizes:

```
run.bat --width=1280 --height=720
run.bat --width=640 --height=960
run.bat --width=1920 --height=1080
```

## Where to look next

- [Layout with UDim2](manual:ui/layout)
- [Grids, pages and flex](manual:ui/layouts)
- [`UISizeConstraint`](api:UISizeConstraint) ·
  [`UIAspectRatioConstraint`](api:UIAspectRatioConstraint) ·
  [`UIScale`](api:UIScale) · [`UITextSizeConstraint`](api:UITextSizeConstraint) ·
  [`CanvasGroup`](api:CanvasGroup)
