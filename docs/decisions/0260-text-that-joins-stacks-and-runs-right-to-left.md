# 0260 — Text that joins, stacks and runs right to left

- Status: accepted. Option A, with the Thai and Lao dictionary, approved by
  the project's maintainer on 2026-10-21 (R5, R6): HarfBuzz (MIT) for
  shaping, SheenBidi (Apache-2.0) for bidi, and a word-break dictionary under
  a permissive licence -- ICU's data, under the Unicode licence, or one like
  it; nothing LGPL or GPL. Each at a pinned release, vendored through
  `vendor.luau` and never edited in place; built lean, through the engine's
  own font loading; the size each adds measured on every shipping platform
  before it ships.
- Built: labels, rich text and text fields shape, order and break text of
  every script above, with HarfBuzz 15.0.0 and SheenBidi 3.0.0, and Thai and
  Lao break by ICU 79.1's word lists.
- Date: 2026-10-21
- Builds on: ADR 0011 (stb_truetype for the UI's text), ADR 0259 (the faces
  a character falls back on, and where a CJK line may end)

## The question

The UI lays text out one codepoint at a time, left to right, each
codepoint's own glyph at its own advance. That is right for Latin, Greek,
Cyrillic, Chinese, Japanese and Korean. It is wrong for:

- **Arabic and Persian**: each letter has up to four forms by its neighbours,
  some pairs become one glyph (lam-alef), and the line runs right to left.
- **Hebrew**: right to left, and its marks sit on its letters.
- **Devanagari, Bengali, Tamil and the other Indic scripts**: vowel signs go
  before the consonant they follow in memory, consonants combine into
  conjuncts, and marks stack.
- **Thai, Lao, Khmer and Burmese**: marks stack above and below. These
  scripts write a sentence with no spaces between words, and a line breaks
  only between words.
- **Any text mixing directions**, such as an Arabic sentence with a number
  or an English name in it. The order on screen is not the order in memory
  (the Unicode Bidirectional Algorithm, UAX #9).

A game translated into one of these shows letters that do not join, marks in
the wrong place, and sentences backwards. Accented Latin written with
combining marks rather than precomposed letters has the same problem, in a
smaller way.

## What doing it takes

Three pieces, each a known algorithm with a reference implementation:

1. **Bidi**: split a paragraph into runs of one direction, and order the
   runs of each line for display (UAX #9).
2. **Shaping**: turn a run of codepoints in one script and one face into
   glyph indices and positions, by the face's own `GSUB` and `GPOS` tables
   and the script's rules (the OpenType shaping model).
3. **Line breaking** for scripts with no spaces between words: a dictionary
   of words for Thai, Lao, Khmer and Burmese. UAX #14 rules alone break them
   nowhere, or everywhere.

stb_truetype stays the rasterizer: it draws a glyph by its index
(`stbtt_MakeGlyphBitmap`) and does not have to know how the index was chosen.

## Options

| | Bidi | Shaping | Licence | Notes |
|---|---|---|---|---|
| **A. HarfBuzz + SheenBidi** | SheenBidi | HarfBuzz | MIT; Apache-2.0 | The reference shaper. Every browser, Godot, Unity's text and Android use HarfBuzz. SheenBidi is a small, complete UAX #9 in C. |
| **B. HarfBuzz alone** | none | HarfBuzz | MIT | Shapes runs, but has no bidi: a mixed line comes out in the wrong order. Bidi would have to be written here. |
| **C. kb_text_shape** | included | included | MIT or public domain | One C header: OpenType shaping for the major scripts, bidi, and UAX #14 and #29 segmentation. Newer and less proven than HarfBuzz, and its coverage of the Indic and Southeast Asian scripts has to be checked against HarfBuzz's output. |
| **D. ICU** | included | none | Unicode licence | Bidi, line breaking and the Thai, Lao, Khmer and Burmese dictionaries, with several megabytes of data. Shaping still needs HarfBuzz. |

FriBidi and libthai are LGPL and are left out (R6).

### What each costs in a player

These are estimates from the projects' own reports and from other engines
that ship them. They are not measurements, and the decision is not taken on
them. Before it is taken, the player profile is built with and without each
option for Windows, Linux, Android arm64 and the web, and the stripped
binaries are compared.

| | Desktop | Android arm64 | Web (wasm) |
|---|---|---|---|
| HarfBuzz, full | ~1.2 MB | ~1.0 MB | ~0.9 MB |
| HarfBuzz, built lean (no subsetter, no `CFF2` instancing, no Graphite) | ~0.5 MB | ~0.45 MB | ~0.4 MB |
| SheenBidi | ~0.1 MB | ~0.1 MB | ~0.1 MB |
| kb_text_shape | ~0.4-0.6 MB | about the same | about the same |
| ICU, common and the dictionaries | 2-4 MB | 2-4 MB | 2-4 MB |

A Thai dictionary is a separate cost. ICU's dictionary is about 0.2-0.3 MB,
under the Unicode licence. Shipped as a content file, it can be loaded only
by a game that has Thai text.

## The shape of the change, whichever is chosen

- A label's text is split into **runs**: one paragraph direction (UAX #9),
  then one script, one direction and one face of the ADR 0259 chain in each
  run. Fallback picks the face per run, not per character, so a run is
  shaped in one face.
- A run is shaped into glyphs. **The glyph store keys by glyph index for a
  shaped run** and by codepoint for the rest. The key gains a bit to say
  which, so ASCII text is cached as it is now.
- Lines break on the logical text, between clusters. A cluster is never
  split: a base letter and its marks stay together. Each line is then
  reordered for display.
- **A fast path that skips all of it**: a run with only Latin, Greek,
  Cyrillic or CJK and no combining marks is laid out as it is today. Shaping
  runs only where it changes something, so the HUDs games have now cost
  nothing more. This matters on a phone and in the web build.
- A text field's caret moves through clusters, and moves visually in mixed
  text. A selection of mixed text can be several rectangles.
- `TextXAlignment.Left` stays left. A right-to-left paragraph may want its
  start on the right, so either a new value for the paragraph's start, or a
  label property. To decide with the API.

## Recommendation

**A: HarfBuzz built lean, with SheenBidi.** HarfBuzz is the shaper other
renderers are checked against. A defect in it is a defect every browser has,
and it is found and fixed upstream. SheenBidi is small and complete. Together
they are about 0.6 MB in a player, under two permissive licences.

C is the alternative if size outweighs proof. It is about the same size as
lean HarfBuzz and carries its own bidi and segmentation. It needs the same
measurements, and a comparison of its output with HarfBuzz's over a corpus
of the scripts above.

Thai line breaking, in either case: ICU's dictionary as an optional content
file, read by a small longest-match breaker written here. ICU itself is not
needed for this.

## What was built

- **The pins.** HarfBuzz 15.0.0, the latest release when this was built. It
  is four days younger than the work, and it was taken over 14.6.0 for two
  fixes to memory errors when a font changes faces: a use-after-free and a
  heap overflow. SheenBidi 3.0.0. ICU 79.1, of which only `thaidict.txt` and
  `laodict.txt` are vendored, read as content by a breaker written here (the
  fewest letters in no word, then the fewest words, over the whole run).
- **The build.** HarfBuzz's single-file build with `HB_LEAN`, `HB_NO_AAT`,
  `HB_NO_BEYOND_64K`, `HB_NO_CUBIC_GLYF` and `HB_OPTIMIZE_SIZE`, and both
  libraries compiled for size (`/O1`, `-Os`). A label is shaped when its text
  changes and the result is kept, so the speed given up is never spent per
  frame. Kept from what `HB_MINI` would remove: the shaping a font with no
  `GSUB` falls back on, and the old `kern` table.
- **The cost in a player**, measured as a stripped Release program that
  shapes a line and orders it against one that does not:

  | | HarfBuzz | SheenBidi | Together |
  |---|---|---|---|
  | Windows x64 (MSVC) | 365 KB | 71 KB | 435 KB |
  | Linux x64 (Clang) | 480 KB | 76 KB | 557 KB |
  | Android arm64 (NDK 27.2) | 397 KB | 28 KB | 425 KB |
  | Web | not measured: there is no web build yet | | |

  Compiled for speed instead, they were 523, 757 and 510 KB. The word lists
  are 0.5 MB (Thai) and 0.7 MB (Lao) of content, carried by a game only when
  its own text has a letter of that script.
- **The fast path.** Text with no character of a script that needs shaping
  is laid out by the code that laid it out before, untouched. Its glyphs, its
  lines and its caret are the same to the bit.
- **The glyph store** keys a shaped glyph by its index in its face, above
  every codepoint, and by the chain and which face of it drew the glyph.
- **Rich text** is cut into runs where its style changes too, each shaped at
  its own size. The face is shaped across the change, so a word's letters
  stay joined when one of them is another color.
- **The caret.** A text field's caret, a press and a selection follow each
  character where it is drawn. The arrow keys move on screen in a line with
  right-to-left text in it. Backspace after a letter with a mark takes the
  mark, as system keyboards do, and an emoji or a flag still goes whole.
  Character boundaries take every mark of the Unicode data, and the conjuncts
  a virama makes.

## Not decided here

- **The web's cost**, measured when there is a web build.
- **Khmer and Burmese word lists**, ICU's too, when a game asks for them.
- **The right-to-left alignment property's name and values**, decided with
  the API when the work reaches it.
