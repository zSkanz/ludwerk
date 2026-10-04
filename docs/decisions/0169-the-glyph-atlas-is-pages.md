# 0169 — The glyph atlas is pages, and the one nobody has shown makes room

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, on a finding from play on the owner's phone
  (ludwerk-08, the horde test game; D552), under the standing rule to decide
  as professional engines do and record it
- Builds on: the UI's text (roadmap M7), D543 (a glyph's rows are what is
  uploaded), G6 (a frame's text is built against one atlas), D437 (a tree
  drawn for a reference height)

## Context

The glyph atlas was one square of 1024 texels, and when it filled it was
emptied whole. The file said why: "growing it would mean reuploading
everything and re-deriving every cached UV, and the store already has a
clear-and-refill path for the case where it genuinely fills up".

It genuinely fills up on any dense screen. An interface laid out for 540
lines on a 1080-line phone, or for 1080 on a 2160-line window, rasterises
every glyph at twice the size: four times the texels. On the owner's phone
the log said "The glyph cache filled (703 entries) and was cleared" two
minutes into a run and again a minute later, each followed by a frame of 60
to 110 ms -- every glyph on the screen rasterised again at once. The game
asked for six sizes of digits.

There was a second ceiling under the first: two thousand entries, glyphs of
every size together, cleared the same way.

## How mature engines do it

- **Unreal (Slate)**: the font cache is atlas pages, a new page when one is
  full, up to a count; past it the cache is flushed, and it says so.
- **Unity (TextMesh Pro)**: dynamic atlases with "multi atlas textures" -- a
  new texture when the first is full.
- **Godot**: a list of textures a font size, a new one when a glyph does not
  fit.
- **Dear ImGui (since 1.92), Skia, browsers**: a glyph cache with eviction of
  what was least recently used.
- **Signed distance fields** take the size out of the key altogether: one
  glyph for every size. They are another rasteriser, another shader and
  another look at small sizes.

Pages, a budget, and eviction by use.

## Decision

1. **The atlas is pages**: each a square of 1024 texels of coverage and a
   texture of its own, made when the page before it is full, **eight at
   most** -- eight megabytes of coverage, thirty-two on the GPU as the UI's
   one shader reads them. A small interface has one, as it always had.

2. **A page that fills is followed by another, and nothing already sent is
   sent again.** No texture is resized and no cached place in a texture
   moves: a glyph's quad names its page, as its texture -- 1 for the first
   page to 8 for the last -- and a picture's texture is numbered after them.
   The UI's runs already break where the texture changes.

3. **Past the last page, the page nobody has shown for longest is emptied**
   for the next glyph. Its glyphs are forgotten and rasterised again if they
   come back; every other page is as it was. "Shown" is by frame: a page
   holding any glyph the frame being built has asked for is not one that is
   emptied, so what is on the screen is never what goes.

4. **One frame that asks for more than every page holds is cleared, as
   before**, said in the log, and the frame built again (G6). That is an
   interface showing more large text at once than eight megabytes of it; the
   warning says so.

5. **An emptied page is uploaded as it is filled**, a glyph's rows at a
   time (D543), and so is a new one: never a page whole. A glyph's rows now
   include the row of padding above and below it, so what an edge can sample
   travels with the glyph and what is left of the old pixels beside it is on
   no glyph's rows.

6. **Sixteen thousand entries** where there were two thousand: the ceiling
   that made forty sizes of sixty characters a clear whatever the atlas held.

7. **Not signed distance fields.** They would end the question of size and
   change how every small label in every game looks; that is a decision
   about the engine's text, not a fix for a full cache.

## Consequences

- **Measured**: a 3840 x 2160 window on a tree drawn for 1080 lines, forty
  labels of forty sizes with a number that changes every frame, nine hundred
  frames. Before: the store cleared 128 times, the worst frame 6.8 ms in the
  shipping build with nothing else drawn. After: no clear, no eviction,
  worst frame 1.8 ms in the development build.
- **Memory** grows with what is shown: a megabyte of coverage and four on
  the GPU a page, up to eight pages.
- **The first upload of a page is the rows written on it**, not four
  megabytes: a game's first frame of text sends a shelf.
- A picture's texture index starts after the pages
  (`ui::kFirstImageTexture`); nothing a script sees changed.
- Tests: `glyph_tests.cpp` -- thirty sizes shown at once fill more than one
  page with no clear and nothing rasterised twice; screen after screen of
  sizes and outlines past the budget evicts and never clears, every quad
  naming a page there is; the screen being built rasterises nothing when
  built again; one frame past the whole budget clears and is built twice.
  `ui_text_tests.cpp` -- the first upload, a new glyph and an emptied atlas
  each send a shelf's rows.

## Not decided here

- A budget a game or a platform sets.
- Signed distance fields.
