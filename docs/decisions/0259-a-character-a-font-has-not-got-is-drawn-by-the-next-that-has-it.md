# 0259 — A character a font has not got is drawn by the next that has it

- Status: accepted
- Date: 2026-10-21
- Builds on: ADR 0011 (stb_truetype for the UI's text), ADR 0110 (text
  strokes), ADR 0168 (an ellipsis where text does not fit), ADR 0169 (the
  glyph atlas in pages)

## The question

A label draws with one face. A face holds one family of scripts: Inter has
Latin, Greek and Cyrillic and no ideographs; a CJK face has ideographs and
kana and is tens of megabytes. A game translated into Japanese, Chinese or
Korean shows a box for every character its Latin face lacks. Its other
choices are to ship the CJK face as every label's font, which changes how
its Latin text looks, or to set `Font` per language from a script.

Wrapping has the same gap. A line breaks at spaces, and Chinese and Japanese
put none between words: a paragraph of them is one word wider than the box,
cut wherever it overflows, and a line can begin with a full stop.

## Decision

### 1. A chain of faces

A character is drawn by the first face in this list that has it:

1. the label's `Font`;
2. the label's `FallbackFont`, when it has one;
3. the project's `[ui] font_fallbacks`, in order;
4. the engine's default face, Inter.

Each face is in the list once, and only if it loaded. A name that does not
load is warned about once, as `Font` is, and left out. The box is drawn only
when no face in the list has the character.

Characters that take no room and draw nothing are drawn as nothing in every
face, whether or not the face has them: the zero-width space, the zero-width
joiners, the word joiner, the byte-order mark, and the marks that steer the
direction of text. A face that lacks them would otherwise draw a box in the
middle of a translated line.

```toml
[ui]
font_fallbacks = ["asset://fonts/NotoSansCJKjp-Regular.otf"]
```

`FallbackFont` is a `Content` of kind `Font` on `TextLabel`, and so on
`TextButton` and `TextInput`. It is for one label in a script the rest of the game does not
need, such as a player's name typed in another alphabet. It is not on the
wire and not in the world's hash: it changes only what is drawn.

A file may be a TrueType or an OpenType (CFF) face, or a collection (`.ttc`),
from which the first face is read.

### 2. The glyph store keys by the chain

The store's key stays face, size and codepoint (ADR 0011). The face half of
the key is now the chain, a hash of the faces in it, rather than the face
that drew the glyph. The face that draws a character depends on the faces
before it, so a cache hit must not have to ask them; that question is asked
once, when the glyph is first made. Two chains of the same faces share one
key, so two names for one face still share their glyphs. Each glyph records
which face of its chain drew it.

### 3. A line is as tall as the faces it uses

A line's ascent is the highest ascent among the faces its characters were
drawn with, and its height adds the deepest descent among them. The label's
own face is always counted, with its own line gap. Every line of a plain
label takes the faces used by the whole label, so the lines of one paragraph
are one step apart whichever script each holds. A rich-text line is measured
by its own glyphs, as before.

Every glyph is placed from the top of the line as the label's own face
measures it, and the whole line is lowered by however much higher it reaches.
A glyph from a fallback face therefore stands on the same baseline as the
letters around it. A label that uses only its own face measures exactly what
it did before, to the bit: none of this arithmetic runs for it.

A text field takes its line height and baseline from its whole text, and
draws each line on that one baseline.

### 4. Where a line may end

This is the part of the Unicode line-breaking algorithm (UAX #14) that text
with no spaces needs:

- **A line may break before or after** an ideograph, a kana, a Hangul
  syllable, a CJK punctuation mark or a full-width form. Latin text beside
  them still breaks only at its spaces.
- **Kinsoku.** No line begins with a closing bracket or quote, a comma, a
  full stop, `!`, `?`, `:`, `;`, a small kana, the long-vowel mark, or the
  marks that repeat what is before them. This applies to both the CJK forms
  (`、。，．！？）」』`) and the Latin ones. No line ends with an opening
  bracket or quote (`（「『`). A character that would break those rules goes
  down with the one before it, or the one after.
- **U+200B**, the zero-width space, is a place to break: after it, never
  before it. **U+00A0**, the no-break space, its narrow form U+202F, and the
  word joiner U+2060 hold together what is on either side of them.
- A line breaks at the last place these rules allow. A word is cut mid-way
  only when its line has no such place, which means a word wider than the
  box, as before.

Ellipsis truncation (ADR 0168) cuts the lines this breaking makes. It uses
the first face in the chain that has an ellipsis, and three full stops when
none has one. Rich text breaks by the same rules over its styled glyphs.

## Script surface

- `TextLabel.FallbackFont` (`Content`, kind `Font`, `""`): presentation,
  outside the world's hash.
- `[ui] font_fallbacks` in `project.toml`: a list of `asset://` paths.

## Proof

`engine/ui/tests/fallback_tests.cpp`. The tests use the vendored Inter and a
face made in the test: a square a full em wide for every CJK character, with
metrics unlike Inter's.

- Latin, Cyrillic and CJK in one label draw no box. Without the fallback,
  each ideograph is a box.
- A label in its own face alone measures what it did with no fallbacks set.
- A label's own `FallbackFont` is used with no project fallbacks.
- A glyph from the fallback face stands on the same baseline as a Latin
  letter.
- Long Japanese and Chinese paragraphs at four widths:
  - every line fits;
  - none begins or ends against the kinsoku rules;
  - together the lines are the whole text.
- A full stop goes down with the ideograph before it. An opening bracket goes
  down with what it opens.
- A zero-width space breaks and draws nothing; a no-break space holds.
- A Japanese line cut short ends with an ellipsis inside its box.
- Rich text in Japanese wraps by the same rules.

## Not decided here

- **Shaping and direction.** Arabic letters join, Devanagari and Thai stack
  their marks, and Arabic and Hebrew run right to left. These need a shaper
  and the bidirectional algorithm. ADR 0260 is the draft that weighs them,
  and a new dependency waits on its approval (R5, R6).
- **Thai, Lao, Khmer and Burmese line breaking.** These write words with no
  spaces and break between words, which takes a dictionary. That belongs with
  the shaper.
- **A CJK face the engine ships.** A game names the face it needs; the engine
  does not carry tens of megabytes for every game.
- **Which face of a family to prefer by language.** Japanese, Simplified and
  Traditional Chinese draw some of the same codepoints differently. A game
  lists the face for its language; choosing one by `LocalizationService`
  locale is later work.
