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
- [x] A status assembled in a variable before it reaches `EditorStatus` (E4, below).

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
- [x] **What the first sinks could not see, taught to the lint and cleared**
  -- 195 more words by the sinks, and the sentences no sink reaches:
  - an argument that does not BEGIN with its literal: the arms of a `?:`
    (`mixed ? "mixed" : "select one part"`), the pieces of a `+`, a
    `std::string("...")`, and the words after a `"%s"` (`shownLiterals`);
  - method calls: `editor.report(...)` from the engine's loop, `query.shows`;
  - the helpers the first pass did not know -- `searchField`, `statRow`'s
    value, `iconBeginMenu`, `welcomeLink`, `statusItem`, `findToggle`,
    `scriptAction`, `preferenceReset`, the panels' own `toggle` and
    `opButton` lambdas -- and `Combo`'s list of choices, `snprintf` into a
    label, and **`CalcTextSize` of an English word**: a button measured in
    English is the wrong width in every other language, so each measures the
    words it shows;
  - **names in tables**: the Properties headings (an id in the table, words
    in the catalog), the preference pages and their groups, the script
    editor's 32 colours and 32 actions, the themes, the stream layers, the
    snap steps, the welcome page's keys;
  - **statuses assembled in variables**: saving and loading a scene, a stamp
    applied and saved, a file moved, an import, the script files a save
    wrote, the terrain's cells -- each clause a key with its count a plural,
    and how two are joined a key too (`with_note`, `with_item`,
    `with_clause`, `with_aside`);
  - the status bar whole, the export window's states and results, the
    launcher's messages, a match's window names, the project settings'
    refusals, the shader editor's errors;
  - **the script editor's own words about a script**: its checks ("`x` is
    never used", "unknown global"), a suggestion's kind beside it ("service",
    "keyword", "in this file"). A type's name -- `number`, `function`, a
    class -- is the language's and stays, and so does what Luau's own
    analysis says.
- [x] **A sentence anywhere in the editor's files** (`editorProse`): a
  literal with two words in it, outside a key's own `ENG_TR(...)`, fails
  unless it is named as data in the lint -- a command's id, a heading's id,
  Luau the editor writes. Thirty-one of them, each said once. Checked alive:
  with one taken off the list, the lint names every use of it.
- [x] **Left as it was, and said**: a value as Properties writes it (`nil`,
  `true`, `pos x, y, z`) is a notation a script reads back and the
  inspector's tests compare; an error's `detail` is developer context
  (`error.h`).
- [x] The editor photographed in English, before and after
  (`--editor-drive`, pictures in the session's scratchpad): the shell with
  nothing selected, Properties with a service selected, the terrain panel,
  Preferences, the Export window, a game running. The same words in the same
  places; 883 app tests green on the real catalog.
- [ ] **In another language**: there is none to try. A second catalog is
  what shows the widths that were fitted to English by eye -- the first one
  added should be photographed panel by panel.

## Stage E5 — the command line, and the close

- [x] `ludwerk`'s own words (`tools/cli`): what was left was what no `print`
  carries -- the TOML reader's and the JSON reader's refusals, handed back in
  a table and shown as the reason a `project.toml` or a scene is malformed,
  and the dev server's one line (23 keys). The lint's CLI half reads every
  literal of a CLI file now (`cliProse`): two words outside a `tr(...)` fail,
  unless named as what they are -- TAP, JUnit, a launcher's text, a desktop
  entry.
- [x] The baseline at zero, and gone: `tools/repo/i18n_editor_baseline.json`
  is deleted and the lint allows no file a raw word. `CLAUDE.md`'s R3 digest
  is the owner's to change.

**Closed 2026-10-01.** 468 keys in E4 and 23 in E5; the two catalogs hold
2 209. What is not translated, and why, is in the lint: `EditorDataLiterals`,
`CliDataLiterals`, `SinkElsewhere`.
