# 0168 — Text that does not fit its box: `TextLabel.TextOverflow`

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, on a finding from play (ludwerk-08, the horde test
  game), under the standing rule to decide as professional engines do and
  record it
- Builds on: the UI's text (roadmap M7), ADR 0128 (layout), F3 (rich text)

## Context

A `TextLabel` had no way to cut text its box does not hold. A game showing a
player's name in a row of a table cut the string by counting characters,
which no proportional font agrees with: twelve `i` are a third the width of
twelve `W`.

## How mature engines do it

- **The web**: `text-overflow: clip | ellipsis`, beside `overflow`.
- **Unreal (UMG)**: a text block's overflow policy -- clip, or ellipsis.
- **Godot**: `Label.text_overrun_behavior` -- none, trim, ellipsis -- and
  `clip_text`.
- **Unity (TextMesh Pro)**: `overflowMode` -- overflow, ellipsis, truncate,
  masking and others.

One property on the label, with three answers every one of them has: shown,
cut, ended with an ellipsis.

## Decision

1. **`TextLabel.TextOverflow`**, an `Enum.TextOverflow`, on `TextLabel` and
   so on `TextButton`:
   - **`Overflow`**, the default: drawn past the box, as it always was.
   - **`Clip`**: cut at the box's edges, wherever in a letter that falls.
   - **`Ellipsis`**: cut short at a whole character and ended with an
     ellipsis, so that what is drawn fits.

   The name is the web's and Unreal's word for it, and the three items are
   the three every engine above has.

2. **Measured with the label's own font, at the size it is drawn**, each
   time it is drawn: narrow letters keep more of a name than wide ones.
   `Text` is never changed -- a script reads back what it wrote.

3. **On one line, each line wider than the box is ended. Wrapped, the lines
   the box is tall enough for are kept and the last of them is ended**; at
   least one line is always kept. A cut never leaves a space before the
   ellipsis, and never lands inside a character.

4. **The ellipsis is the face's own character (U+2026), or three full stops
   in a face that has none** -- the built-in face, a game's font that stops
   at ASCII -- where the character would draw as the box a missing one is.

5. **Where it does nothing**: a label `TextScaled` fits to its box; an axis
   `AutomaticSize` grows; a `TextInput`, which scrolls its text.

6. **Under `RichText`, `Ellipsis` cuts at the box as `Clip` does.** Markup
   has no one place to end -- a cut inside a tag's reach would have to close
   it -- and that is its own piece of work, not done here.

## Consequences

- A name in a table is one property.
- The measuring is the drawing's own line breaking, so what is cut is what
  would not have been seen.
- Tests (`engine/ui/tests/appearance_tests.cpp`): a name wider than its box
  is drawn past it, cut at it by the box's own scissor with every glyph
  still there, or ended inside it with fewer; narrow letters keep at least
  as many characters as wide ones; text that fits is the text; wrapped, a
  box two lines tall keeps two lines and one three tall keeps three; scaled
  text is not cut.
