# 0154 — A script asks for text by key, in the player's language

- Status: accepted (built, 2026-10-02)
- Date: 2026-10-02
- Decided by: ludwerk-08 for the owner, on 2026-10-02, from six test games that
  each hardcoded Portuguese for want of it, and from ADR 0147's ready-made
  options screen, which is a Luau module the engine ships and had no way to
  read the engine's catalog. The scope below is his; the mechanism is the
  builder's.
- Builds on: R3 (no hardcoded user-facing strings), the catalog of M0
  (`core::Catalog`), [0147](0147-graphics-and-display-settings-are-one-model-the-project-sets-the-player-chooses-a-script-reads-and-writes.md)
  (the player's own file).

## Context

The engine has been localized since its first milestone: every message it
produces is a key and parameters, resolved against `i18n/en.json`. A game had
none of it. `docs/manual/guides/i18n.md` said so in as many words -- "the game
catalog is a convention with no reader" -- and showed twelve lines of Luau a
game could carry instead. Every game made with the engine since has carried
them, or has written its text in one language.

## Decision

### 1. `LocalizationService`

A service of the player's side, like `GraphicsService`:

- **`Locale`** (read and write): the locale text is asked for in, as `en` or
  `pt-BR`. It starts as the player's saved choice, else the system's language,
  else `[project] default_locale`, else `en` -- each narrowed to a catalog the
  project has.
- **`GetLocales()`**: the locales the project has a catalog for, in order.
- **`Translate(key, arguments?)`**: the text for `key`, with `{name}`
  placeholders filled from `arguments`.
- **`LocaleChanged(locale)`**.

**A missing key returns the key and warns once per key.** Never an error: a
missing translation must not stop a game.

### 2. A project's catalogs

`i18n/<locale>.json` at the project's root: one flat object, key to text. They
are read at start, read again when one changes while a game is being made, and
carried into an export. `ludwerk check` holds that every key of the default
locale is in every other.

### 3. The engine's own text, through the same call

The engine's catalog is under the project's, under the keys it already has. A
module the engine ships asks `LocalizationService` like any script.

### 4. What it is not

- **Text properties stay plain strings.** No instance is translated behind a
  script's back, and there are no translation tables as instances.
- **The editor's language is another setting.** What a person making a game
  reads and what a player of it reads are two choices.

### 5. The player's choice is kept

With the graphics settings, in the same file.

## Consequences

- A game's text is in files a translator can be handed, and a language menu is
  a list and an assignment.
- An engine-shipped module's captions follow the player's language as soon as
  the engine has a catalog for it.

## Not decided here

- Plural rules beyond what the catalog already has, and gender or list
  formatting (ICU).
- Translating instances automatically.
- Right-to-left layout and font fallback by script.

## As built, 2026-10-02

- **`scene::Localization`** holds the catalogs, a `core::Catalog` each; the
  host fills it and the world holds a pointer. `Locale` is in the world's
  engine state, set at the world's making -- a script's first line reads the
  locale it will keep.
- **Where a key is looked for**: the project's catalog for `Locale`, the
  project's default locale, the engine's catalog for `Locale` (and for its
  language alone), the engine's own. **The engine's keys are read as they
  are**, with no prefix added -- they are already written area-first
  (`scene.err.`, `engine.editor.`) -- and **a project's key of the same name
  wins**, which is how a game rewords a caption of the engine's.
- **Narrowing**: a locale is itself where there is a catalog, else the catalog
  of its language (`pt` for `pt-PT`), else any of that language in order
  (`pt-BR` for `pt`), else refused. Written `pt-BR` however it was typed. A
  project with no catalog at all has one locale, its default.
- **A write to `Locale` is the player choosing and is kept at once**, in
  `settings.json` as `"locale"`, without a `SaveAsync`: a language is picked
  from a menu, not applied and then saved. The locale the system suggested is
  not written until somebody chooses.
- **`LocaleChanged` also fires when a catalog is read again** in a development
  run, with the locale it already had: the words moved, and whatever set a
  label's text sets it again.
- **Plurals are what the catalog already had**: a value may be an object keyed
  by plural category, chosen by an argument named `count`, with the rule of the
  catalog's own language. A whole number is passed as one, so it selects.
- **A dedicated server** reads the default locale whatever `Locale` says, and
  a write raises. The script-side lint counts the service as the player's.
- **`platform::preferredLocales`** is the system's list.
- **The conformance run has catalogs of its own**, `tests/conformance/i18n`.
- **The old guide named `assets/i18n/en.json`**, which nothing ever read or
  scaffolded. The catalogs are at `i18n/`, beside `src/` and `content/`.
- Tests: `localization_tests.cpp` (which catalog, which locale),
  `service_tests.cpp` (the service, the warning, a machine with no player),
  `localization.spec.luau`, `check.test.luau` (the lint).
