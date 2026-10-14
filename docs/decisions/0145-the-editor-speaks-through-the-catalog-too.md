# 0145 — The editor speaks through the catalog too

- Status: accepted
- Date: 2026-09-30
- Supersedes: [0046](0046-the-editor-is-a-mode-of-the-engine-binary.md)'s
  exemption of the editor from R3, and `debug_overlay.h`'s.

## Context

ADR 0046 exempted what the editor draws from R3: it is for whoever builds a
game, never for a player, so its labels were literals. But the
editor will ship in other languages. Every word the editor shows -- a button,
a tooltip, a menu, a status line, an error, a dialog, the brush's chip -- has
to be one a translator can reach. Only the text of a person's own scripts
stays as they wrote it.

The terrain editor was converted in the terrain audit (253 keys). The
rest of the editor held some 535 raw words where `i18nlint` can see them, and
more where it cannot yet: names in tables, sentences built from pieces.

## Decision

1. **R3 applies to the editor and the overlay**, as to the engine. A word the
   editor shows is a catalog key (`ENG_TR`, read with `core::tr`); a sentence
   with values in it is one key with `{named}` slots, never words glued
   together, so a language can put the values where its grammar wants them.
2. **`i18nlint` reads the editor's own sinks**: the ImGui calls that put text
   on the screen and the editor's helpers round them, by the argument each
   shows, and the first member of an `EditorStatus{...}`. A literal there with
   a word in it -- past an ImGui `##id`, printf conversions and escapes -- is
   one nobody can translate.
3. **A baseline that only shrinks** (`tools/repo/i18n_editor_baseline.json`):
   each file may show at most its count of raw words, and a file showing
   fewer fails until its count is lowered, so what one change translates the
   next cannot spend. It reaches zero when the plan closes, and goes.
   **Reached zero on 2026-10-01 and removed**: the lint allows no file a raw word, and reads every literal of the
   editor's files and the CLI's for a sentence, not only the calls it knows
   to show one.
4. **What stays a literal**: an ImGui id (`##value`), a format with no word
   (`%.2f`), a keyboard shortcut's name, a file extension, a URI scheme --
   data, not prose. Units are words (`m`, `deg`) and go through the catalog.

## Consequences

- The plan converts the editor file by file, the baseline falling with
  each; the lint's reach grows past the direct calls -- tables of names,
  sentences built with `+` -- as the plan reaches them.
- `debug_overlay.h`'s note and this repository's other statements of the old
  exemption are corrected as the files are.
