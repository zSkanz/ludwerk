# The editor through the catalog (R3): the kickoff and the ledger

**The owner's order of 2026-09-29**, through ludwerk-08: *"tudo que é texto com
exceção de script deve ser possível traduzir não esqueça disso pq vamos lançar
outros idiomas no futuro"*. In the queue after the terrain audit (whose T4
converted the terrain editor) and before F2. ADR 0145 is the decision.

| Stage | Decision |
|---|---|
| E0 — the rule, the lint and the baseline | [ADR 0145](../decisions/0145-the-editor-speaks-through-the-catalog-too.md) |
| E1 — `debug_overlay.cpp`: panels, menus, Properties, Content | this ledger |
| E2 — `editor.cpp`: the status line and its messages | this ledger |
| E3 — the script editor and the world panels | this ledger |
| E4 — what the lint cannot see yet: tables of names, sentences built of pieces, units | this ledger |
| E5 — the command line's words, and the close | this ledger |

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- **A word on the screen is a key**; a sentence with values in it is one key
  with `{named}` slots, never words glued together.
- **The baseline only falls** (`tools/repo/i18n_editor_baseline.json`): every
  stage lowers it by what it converted, and `i18nlint` fails a count that grew
  or one that fell without the baseline saying so.
- **The editor looks the same in English**: a converted panel is photographed
  with `--editor-drive` before and after where its layout could move.
- The full `scripts/localgate.ps1` before each push. Stage only what this work
  wrote.

## Stage E0 — the rule, the lint and the baseline

- [x] ADR 0145 supersedes ADR 0046's exemption; `debug_overlay.h` says so.
- [x] `i18nlint` reads the editor's sinks: the ImGui calls that show text and
  the editor's helpers (`labeledIconButton`, `iconButton`, `terrainTile`,
  `iconMenuItem`, `toolButton`, `sectionName`, `brushDown`), by the argument
  each shows, and the first member of `EditorStatus{...}`.
- [x] The baseline: 535 raw words in four files (`debug_overlay.cpp` 384,
  `editor.cpp` 123, `script_editor_panel.cpp` 21, `world_panels.cpp` 7).
- [x] **Found on the way**: the lint's call finder took the parenthesis before
  a sink's name for its own, so a sink called inside a condition --
  `if (iconMenuItem(...))`, `if (log(...))` -- was read as the condition's one
  argument and never checked. It looks after the name now; no engine sink was
  hiding a literal.

## Stage E1 — `debug_overlay.cpp`

- [x] Every word the lint's sinks reach, 384 of them: 336 converted by a
  script -- a key per area (the function the words are in) and words, the
  English as it was -- and 48 by hand, each sentence with values one key with
  named slots and a count a plural (`one`/`other`). The same words in two
  areas are two keys: a translator may need two words.
- [x] A label that is an ImGui id keeps its `##id` (`labelled`), and a
  measurement keeps its decimals in a slot (`fixed`).

## Stage E2 — `editor.cpp`

- [x] Every `EditorStatus` built from a literal (132) and every history label
  (34): one key each, the paths, names and counts in slots, "instance(s)" a
  plural. `EditorStatus::message` is said to be in the reader's language.
- [ ] A status assembled in a variable before it reaches `EditorStatus` (E4).

## Stage E3 — the script editor and the world panels

- [x] `script_editor_panel.cpp` (21) and `world_panels.cpp` (7).

## Stage E4 — what the lint cannot see yet

- [x] The editor's own helpers taught to the lint and cleared: section and
  property rows, dialogs and their buttons, preference hints and the
  preferences' search (`shows`, matched against the words as shown), stats
  rows, texture slots, the status line's `report` and the content browser's
  refusals (124 more).
- [x] **The command palette**: a command's title is the catalog's, its id the
  English it always had, so the menus and `runCommand` find it in any
  language; "Undo {label}", "Show {panel}", "Insert: {class}" with slots.
- [x] **Window and dialog titles**: the words are the catalog's and the
  `###id` stays, so a saved layout, `SetWindowFocus` and `OpenPopup` keep
  finding them; the About box's title is "About {brand}".
- [ ] Method calls the lint's call finder does not reach (`editor.report(...)`
  from `engine.cpp`), statuses assembled in variables, names in tables (the
  themes', the preference pages', the stream layers'), measurements built as
  printf formats (`"%.2f ms"` in the stats), `CalcTextSize` of a word that is
  now translated. A broad sweep of prose-shaped literals in `engine/app`
  finds some 500, most of them data -- JSON fields, flags, generated code,
  Luau syntax -- and the rest this list.
- [ ] The lint taught each of these as it is cleared.
- [ ] The editor photographed in English before and after, panel by panel
  (`--editor-drive`); deferred while the owner is at the machine.

## Stage E5 — the command line, and the close

- [ ] `ludwerk`'s own words (`tools/cli`), held by the lint's CLI half.
- [ ] The baseline at zero, and gone; `CLAUDE.md`'s R3 digest is the owner's
  to change.
