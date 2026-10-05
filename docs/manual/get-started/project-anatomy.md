# Anatomy of a project

A new project, from `ludwerk new` or from the editor's project browser, is a
`project.toml`, a scene and one script file.

```text
my-game/
├─ project.toml                   what this project is
├─ content/
│  └─ scenes/main.scene.json    the world
├─ src/client/Main.luau         greets you when you press Play
└─ .engine/                      generated, gitignored
```

The starter scene holds a small level and the code that belongs to its parts
(ADR 0092):

- the spinner's own `Script` turns it;
- a `ModuleScript` in `ReplicatedStorage` holds the settings it requires.

A directory is a project when it holds a `project.toml` **or** one of the code
folders below. A project without a `project.toml` is legal: it takes every
default.

## Code in files, when you want it

A project can also keep code in files, edited in VS Code, kept in git and
hot-reloaded by `ludwerk dev`. None of this is required, and the two ways mix in
one project:

```text
my-game/
├─ .luaurc                          strict mode, and the require aliases
├─ src/
│  ├─ client/main.luau              runs where a player sits
│  ├─ server/rules.luau             runs where the world is decided
│  ├─ shared/greeting.luau          a module both sides require
│  └─ scenes/arena/
│     ├─ server/countdown.luau      the arena scene's own server code
│     └─ client/scoreboard.luau     and its own player code
├─ i18n/en.json                     the game's own text, a file a language
└─ tests/example.test.luau
```

- **`src/client/`, `src/server/` and `src/shared/`** are the game's code, for
  every scene: `Script`s under `GlobalScriptService.Client` and `.Server`, and
  `ModuleScript`s under `.Shared` (ADR 0105).
- **`src/scenes/<scene>/client/` and `server/`** are one scene's code, under
  that scene's `ClientScriptService` and `ServerScriptService`.
- Each subdirectory becomes a `Folder`: `src/client/systems/spawn.luau` is a
  `Script` named `spawn` inside a `Folder` named `systems`. The file is that
  script's source, so the scene does not write it.
- **Every other `.luau` file is a module**, reached by `require` with a path and
  never in the tree.
- `src/scripts/`, the folder before these, is read as `src/client/` for one
  release, with a warning.

See [Scripts, modules and requires](manual:concepts/scripts) for both.

## src/shared and the alias

`.luaurc` declares the aliases, and both the analyzer and the engine read them:

```json
{
  "languageMode": "strict",
  "aliases": { "shared": "src/shared" }
}
```

```luau
local Greeting = require("@shared/greeting")
```

Resolution order for a require: engine-provided `@` modules first; then `@self`,
meaning the requiring file's own directory; then the `.luaurc` aliases; then
`./` and `../` relative to the requiring file; then a bare specifier as a path
from the project root.

`.luau` is appended if absent, then `init.luau` is tried.

> **`.luaurc` takes no `$comment` key.** The runtime treats an unknown key as an
> error rather than ignoring it, so a comment there breaks requires.

## project.toml

```toml
[project]
name = "My Game"
id = "com.example.mygame"
version = "0.1.0"
company = "Example"
icon = "branding/icon.png"
scene = "scenes/main.scene.json"

[window]
title = "My Game"
size = [1280, 720]
fullscreen = false
resizable = true

[display]
vsync = true
max_frame_rate = 0
background_frame_rate = 10

[dev]
port = 4560

[network]
server = "play.example.com:7777"

[assets]
content = "content"

[graphics]
quality = "high"
```

| Section | Keys |
|---|---|
| `[project]` | `name` (becomes the built executable's name), `id` (reverse-DNS; groups taskbar buttons on Windows, and the Android package), `version` (`X.Y.Z`, stamped by every export), `company`, `icon` (one square PNG, 1024 pixels is best: every export makes its own sizes from it), `scene` |
| `[debug]` | `overlay_key` (the key that opens the overlay in a development run: `"F3"` unless you say, `"None"` for no key) and `frame_report_seconds` (a line about the frames every so many seconds). The measuring keys, each off unless asked: `gpu_pass_times` (the GPU's time by pass, under that line), `hide` (a list of what is not drawn, as `"foliage,terrain"`), `skip` (a list of the lighting terms left out, as `"shadow,environment"`), `shadow_taps` (1 to 16), `log_ui_touches`, and `launch_arguments` (an Android build takes the host's flags from the intent that launches it) — see [Measuring a frame](manual:rendering/quality) |
| `[window]` | `title`, `size` (or `width` and `height`), `fullscreen`, `resizable`. On a phone a game always fills the display, the system's bars hidden, and `UIService.SafeAreaInsets` says where a HUD may go |
| `[display]` | How fast frames are made. `vsync` — a frame waits for the display's refresh; on unless it says otherwise, and always on a phone. `max_frame_rate` — frames a second at most, 0 for no cap; the only limit with `vsync` off, and a cap under the refresh with it on. `background_frame_rate` — the rate while the window is unfocused or minimised, 10 by default, 0 for no throttle. On a phone a game that names no cap is held to 60, and its frames are paced at the highest of 60, 40 or 30 they fit; `adaptive_frame_rate = false` turns that stepping off. The simulation ticks at its own rate whatever these say. Also `window_mode` (`windowed`, `borderless`, `fullscreen`), `resolution`, `monitor`, and `remember_player_settings` — off for a game that keeps its own. See [Graphics and display settings](manual:rendering/settings) |
| `[dev]` | `port` — default 4560 |
| `[network]` | `server` — where `NetworkService:Join()` goes with no address; `relay` — the relay a host registers with and a join code is looked up at (`relay.example.com:7789`); `timeout` — seconds a silent connection is kept, 10 by default |
| `[assets]` | `content` — where the asset compiler reads from |
| `[graphics]` | The quality family. See [Graphics quality settings](manual:rendering/quality) |
| `[render]` | `max_views_per_frame`, `max_view_resolution` — what camera textures may cost. See [Views](manual:rendering/views) |

The TOML subset is deliberately small: comments, tables, strings, numbers,
booleans and single-line arrays. A multi-line string, an inline table, an array
of tables or a date is an **error** rather than a silent misread.

Two things to know about it as it stands:

- **`[assets] content` is read by the asset compiler and not by the engine**,
  which mounts `content/` by name. Renaming it will compile from one place and
  mount another.
- **`[permissions]`, `[memory]` and `[build]` parse and are reserved.** Nothing
  reads them yet.

## content/

What `asset://` names. Meshes, textures, audio, fonts and scenes, in whatever
directory layout suits you — the URN is the path relative to this directory.

In development it is mounted directly, so a file dropped in is available with no
build step.

## .engine/

Generated, gitignored, and safe to delete:

| Path | Is |
|---|---|
| `types/engine.d.luau` | The engine's type definitions, for the analyzer. |
| `content.lpack` · `content.manifest.json` | The compiled content. |
| `content/**.lchunk` · `content.chunks.json` | Compiled streaming chunks. |
| `types/scene.d.luau` | The scene's own tree, typed, so `workspace.Level.Ground` type-checks. |
| `editor-layout.v3.ini` · `editor.json` | Editor panel layout and last-open scene. |

## tests/

`tests/**/*.test.luau` is your project's own suite, run by the pure runner.
`tests/conformance/**/*.spec.luau` is the engine's shape, run against a headless
engine. See [Testing](manual:guides/testing).

## Reserved names

Where code runs is where it is in the tree, and outside the script services a
script's own `RunContext` (ADRs 0105 and 0138; see
[Where my code runs](manual:concepts/scripts)).

## Where to look next

- [Your first world](manual:get-started/first-world)
- [The ludwerk CLI](manual:get-started/cli)
- [Content and asset URNs](manual:assets/content)
