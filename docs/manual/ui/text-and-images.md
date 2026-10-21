# Text and images

## TextLabel

```luau
--!strict
local title = Instance.new("TextLabel")
title.Size = UDim2.new(1, -32, 0, 48)
title.Text = "Score: 0"
title.TextSize = 40
title.TextColor = Color3.fromRGB(240, 236, 224)
title.TextXAlignment = Enum.HorizontalAlignment.Center
title.TextYAlignment = Enum.VerticalAlignment.Center
title.BackgroundTransparency = 1
title.Parent = screen
```

| Property | Default | Notes |
|---|---|---|
| `TextLabel.Text` | `""` | UTF-8. |
| `TextLabel.TextColor` | black | **No `3` suffix.** |
| `TextLabel.TextTransparency` | 0 | The words' own; the box has `BackgroundTransparency`. |
| `TextLabel.TextSize` | 14 | The em size **in pixels**. |
| `TextLabel.Font` | `""` | An `asset://` URI to a TrueType or OpenType file. |
| `TextLabel.FallbackFont` | `""` | A font for the characters `Font` has not got. See Fonts. |
| `TextLabel.LetterSpacing` | 0 | Room after each character, as a fraction of `TextSize`. |
| `TextLabel.TextXAlignment` | `Center` | `Enum.HorizontalAlignment` |
| `TextLabel.TextYAlignment` | `Center` | `Enum.VerticalAlignment` |
| `TextLabel.TextWrapped` | `false` | |
| `TextLabel.TextScaled` | `false` | |
| `TextLabel.TextOverflow` | `Overflow` | `Enum.TextOverflow`: `Overflow`, `Clip`, `Ellipsis` |

**The text's alignment is named for the text**: `TextXAlignment` and
`TextYAlignment`, so a label's words and a layout's children are never
confused. Their values are the enums a `UIListLayout` uses,
`Enum.HorizontalAlignment` and `Enum.VerticalAlignment`. A scene saved when
these were a label's `HorizontalAlignment` and `VerticalAlignment` opens with
them read under the new names.

A character none of the label's fonts has draws a box rather than nothing: a
label that silently drops characters is the failure mode that ships.

## Fonts

`Font` empty is the engine's own default face, and so is the literal name
`Inter`. A name that cannot be resolved falls back to the default and says so
**once**, rather than once a frame.

A build whose content directory has no font at all falls back further, to a
built-in face that is ASCII only, one weight and no kerning — text still draws,
which is the point of having a fallback.

### Fonts for other scripts

One font holds one family of scripts. Inter has Latin, Greek and Cyrillic, and
no Chinese, Japanese or Korean. **A character the label's font has not got is
drawn with the next font that has it**:

1. the label's `Font`;
2. the label's `FallbackFont`, when it has one;
3. the fonts in `[ui] font_fallbacks`, in order;
4. Inter.

A game translated into Japanese lists a Japanese font once and keeps its own
font for everything else:

```toml
[ui]
font_fallbacks = ["asset://fonts/NotoSansCJKjp-Regular.otf"]
```

`FallbackFont` is for one label that needs a script the rest of the game does
not, such as a name typed in another alphabet. A font may be a `.ttf`, an
`.otf`, or a collection (`.ttc`), whose first face is used.

A line is as tall as the fonts its characters use: the highest of their
ascents and the deepest of their descents. A label in one font measures the
same as before. Every line of a label keeps the same height, so a paragraph
that mixes scripts keeps even spacing. The zero-width space, the joiners and
the direction marks draw nothing, whichever font is asked.

Japanese, Simplified Chinese and Traditional Chinese draw some of the same
characters differently, so list the font made for your game's language.

### Letter spacing

`LetterSpacing` adds room after each character as a fraction of the text's
size: 0.05 at a `TextSize` of 40 is two pixels a letter, and a negative value
draws letters closer. Wrapping, scaling and the ellipsis measure with it, and so
does a text field's caret. It is not added after a Chinese, Japanese or Korean
character, which the font already sets at its full width. Use it instead of
spaces between letters, which break kerning, selection and screen readers.

Glyphs are rasterized on demand and cached by face, size and codepoint -- and
**a size is one of a few**: every whole pixel to 24, every second pixel to 64,
then 80, 100 and 128. Text at a size between two of them is rasterized at the
one above and drawn a little smaller, never by more than a twelfth below 64. So
a `TextSize` a tween is animating -- a damage number that pops from 14 to 40 --
costs about twenty-seven sets of glyphs however many frames it takes, and a
label that grows and shrinks all match long does not fill the cache. Text at a
whole number of pixels up to 64, which is what a `TextSize` somebody typed is,
is rasterized at exactly that size. Past 128 pixels it is drawn up from 128,
and is as soft as that makes it: a title that large wants a picture.

**`TextSize` is in window pixels and is not scaled for you.**
`UIService.DisplayScale` is what a game multiplies by when it wants text to be
the same physical size on two displays.

## Wrapping and scaling

`TextWrapped` breaks lines at the box's width. It breaks at spaces, and
**mid-word only for a word wider than the box** — a word cut in half at a random
letter is worse than one that overhangs.

Chinese, Japanese and Korean break between characters, by the usual rules for
those languages. No line begins with a closing bracket, a comma, a full stop
or a small kana (`、。，．！？）」』ッャ`). No line ends with an opening bracket
(`（「『`): the character moves to the next line with its neighbor. Put a
zero-width space (U+200B) where a line may break that otherwise would not. Put
a no-break space (U+00A0) between two words that must stay on the same line.

`TextScaled` ignores `TextSize` and rasterizes at whatever size fills the box,
preserving aspect. **Re-rasterized rather than stretched**: there is no distance
field here, and a stretched bitmap is what "scaled text" usually means and looks
like. The size that fills the box is drawn from the nearest of the sizes above
(see Fonts), so a label in the world -- a name over a head, a different size at
every distance -- costs those few sizes and not one for every step the camera
takes.

## Text that does not fit

`TextOverflow` says what happens to text its box does not hold:

- **`Overflow`**, the default, draws it past the box.
- **`Clip`** cuts it at the box's edges.
- **`Ellipsis`** cuts it short at a whole character and ends it with an
  ellipsis, so what is drawn fits.

```luau
--!strict
local name = Instance.new("TextLabel")
name.Size = UDim2.fromOffset(160, 24)
name.TextXAlignment = Enum.HorizontalAlignment.Left
name.TextOverflow = Enum.TextOverflow.Ellipsis
name.Text = "Wolfgang Amadeus Mozart the Second"   -- drawn as "Wolfgang Amadeus M..."
```

**It is measured with the label's own font, at the size it is drawn** -- do not
cut a string by its length, which no proportional font agrees with: narrow
letters keep more of a name than wide ones. `Text` is not changed; a script
reads back what it wrote.

On one line, a line wider than the box is ended. With `TextWrapped`, the
lines the box is tall enough for are kept and the last of them is ended -- a
description in a card three lines tall shows three lines.

Nothing is cut from a label `TextScaled` fits to its box, or along an axis
`AutomaticSize` grows. Under `RichText`, `Ellipsis` cuts at the box as `Clip`
does. A font without an ellipsis character gets three full stops.

## TextInput

`TextInput` extends `TextLabel` and adds an editable field, on one line or
several, with selection, the clipboard, undo and an input method's
composition: see [Text input](manual:ui/text-input).

```luau
--!strict
local field = Instance.new("TextInput")
field.Size = UDim2.new(0, 220, 0, 32)
field.PlaceholderText = "your name"
field.Parent = screen

field.Focused:Connect(function()
    print("typing here now")
end)

field:GetPropertyChangedSignal("Text"):Connect(function()
    print(field.Text)
end)
```

`TextInput.PlaceholderText` is drawn dimmed in place of `Text` while the field
is empty and unfocused.

**Focus is taken by a press and released by a press elsewhere**, and there is
deliberately no settable focused property — two fields could then both believe
they had it. `TextInput.Focused` and `TextInput.FocusLost` are the events.

Typing writes `Text` through the same path a script assignment uses, so
`Instance.GetPropertyChangedSignal` fires for it. A focused field also takes the
keyboard away from the Input Action System, so movement keys do not fire while
somebody is typing.

## ImageLabel

```luau
--!strict
local panel = Instance.new("ImageLabel")
panel.Size = UDim2.new(0, 240, 0, 96)
panel.Image = "asset://ui/panel.png"
panel.ScaleType = Enum.ScaleType.Slice
panel.SliceCenter = Rect.new(Vector2.new(12, 12), Vector2.new(52, 52))
panel.BackgroundTransparency = 1
panel.Parent = screen
```

| Property | Default | Notes |
|---|---|---|
| `ImageLabel.Image` | `""` | A compiled texture or an ordinary image file. |
| `ImageLabel.ImageColor` | white | Multiplied into the image. |
| `ImageLabel.ScaleType` | `Stretch` | |
| `ImageLabel.SliceCenter` | empty | Where the nine-slice cuts are, in **source pixels**. |

`ImageColor` multiplies, so one white glyph sheet draws in any color.

An empty `Image` draws nothing but still fills the background. A URI that names
nothing and a picture still loading both draw as a flat `ImageColor` rectangle
— the same thing, because from a frame's point of view they are.

`Enum.ScaleType` has exactly three items:

- **`Stretch`** — the whole image into the whole box. Aspect ratio is **not**
  preserved.
- **`Slice`** — nine-slice: the corners at their own size, the edges stretching
  along one axis, the middle along both. This is how a panel keeps its rounded
  corners at any size.
- **`Tile`** — repeated at its own size until the box is full. Bounded: past
  about four thousand tiles it stretches instead, because a one-pixel image
  tiled over a full-screen frame is nearly a million quads.

`SliceCenter` is kept as given rather than corrected. A slice quietly fixed up
is a panel that draws wrong with no way to find out why. A box narrower than its
own two corners collapses the middle rather than drawing the corners over each
other.

## The button classes

`TextButton` extends `TextLabel` and `ImageButton` extends `ImageLabel`, and
**neither adds a property**. `UIObject.Activated`, `UIObject.PointerEntered` and
`UIObject.PointerExited` live on `UIObject`, because anything can be pressed —
including a plain `Frame`.

The classes exist because they are the ones a reader recognizes.

## Rich text

Turn on `RichText` and a label reads its `Text` as markup, so color, size and
weight change part-way through one line:

```luau
label.RichText = true
label.Text = "Health <b><font color=\"#ff5050\">12</font></b> / 100<br/>"
    .. "<i>Poisoned</i> <font size=\"12\" transparency=\"0.4\">(3s)</font>"
```

`<b>`, `<i>`, `<u>` and `<s>` are bold, italic, underlined and struck through;
`<font>` takes `color` (`#rrggbb` or `rgb(r, g, b)`), `size` and
`transparency`; `<br/>` breaks the line. Write `&lt;`, `&gt;`, `&amp;`,
`&quot;` and `&apos;` for the characters themselves. A tag the label does not
understand is drawn as text, so a typo shows as itself. Bold and italic come
from the label's one face -- thickened and leaned -- rather than from separate
font files, and a `TextInput` always shows its text plain.

## What is not here

Complex scripts are not shaped: codepoints are laid out left to right, so
Arabic, Devanagari and Thai do not join, and Thai does not break between its
words. The same classes draw in the world
under a `SurfaceGui` or a `BillboardGui`: see [UI in the world](manual:ui/world-space).

## Where to look next

- [Buttons and interaction](manual:ui/interaction)
- [Content and asset URNs](manual:assets/content) — what an `asset://` URI is
- [`TextLabel`](api:TextLabel) · [`ImageLabel`](api:ImageLabel)
