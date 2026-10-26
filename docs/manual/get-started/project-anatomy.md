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

The starter scene holds a small level and the code that belongs to its parts:

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
  `ModuleScript`s under `.Shared`.
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
| `[project]` | `name` (becomes the built executable's name), `id` (reverse-DNS; groups taskbar buttons on Windows, and the Android package), `version` (`X.Y.Z`, stamped by every export), `company` (names the folder players' saves are in: keep it after release), `publisher` and `copyright` (what the built executable says; `publisher` falls back to `company`), `icon` (one square PNG, 1024 pixels is best: every export makes its own sizes from it), `scene`, `default_locale` (see [Localization](manual:guides/i18n)) |
| `[debug]` | `overlay_key` (the key that opens the overlay in a development run: `"F3"` unless you say, `"None"` for no key) and `frame_report_seconds` (a line about the frames every so many seconds). The measuring keys, each off unless asked: `gpu_pass_times` (the GPU's time by pass, under that line), `hide` (a list of what is not drawn, as `"foliage,terrain"`), `skip` (a list of the lighting terms left out, as `"shadow,environment"`), `shadow_taps` (1 to 16), `log_ui_touches`, `astc` (`false` sends textures as RGBA to a GPU that samples ASTC), and `launch_arguments` (an Android build takes the host's flags from the intent that launches it) — see [Measuring a frame](manual:rendering/quality) |
| `[window]` | `title`, `size` (or `width` and `height`), `fullscreen`, `resizable`. On a phone a game always fills the display, the system's bars hidden, and `UIService.SafeAreaInsets` says where a HUD may go |
| `[display]` | How fast frames are made. `vsync` — a frame waits for the display's refresh; on unless it says otherwise, and always on a phone. `max_frame_rate` — frames a second at most, 0 for no cap; the only limit with `vsync` off, and a cap under the refresh with it on. `background_frame_rate` — the rate while the window cannot be seen (minimized, hidden, or wholly covered where the system reports it), 10 by default, 0 for no throttle. `unfocused_frame_rate` — the rate while it can be seen but another window has the keyboard, 0 (no throttle) by default. Online, neither is below 60. On a phone a game that names no cap is held to 60, and its frames are paced at the highest of 60, 40 or 30 they fit; `adaptive_frame_rate = false` turns that stepping off. The simulation ticks at its own rate whatever these say. Also `window_mode` (`windowed`, `borderless`, `fullscreen`), `resolution`, `monitor`, `adapter` (a graphics card by its name, or a part of it), and `remember_player_settings` — off for a game that keeps its own. See [Graphics and display settings](manual:rendering/settings) |
| `[dev]` | `port` — default 4560 |
| `[network]` | `server` — where `NetworkService:Join()` goes with no address; `relay` — the relay a host registers with and a join code is looked up at (`relay.example.com:7789`); `timeout` — seconds a silent connection is kept, 10 by default; `max_connections_per_address` — how many players one network address may be on your server, 64 by default and 0 for any number; `client_budget_kb_per_second` — kilobytes of the world's state a second each player is sent at most, what matters most to them first, 48 by default and 0 for no budget ([A budget for each player](manual:guides/multiplayer#a-budget-for-each-player)); `predict_radius` — how far from its character, in meters, a player's machine simulates the loose parts it could touch, 8 by default; `predict_max_bodies` — how many such parts at most, nearest first, with the parts that move on their own, 32 by default; `predict_linger_ticks` — how long one out of reach is still simulated, 30 ticks by default; `predict_moving_lead_ms` — how far ahead, in milliseconds, a part moving on its own that is neither tweened nor moved by a world step is carried on by its motion, 250 by default ([Platforms and lifts](manual:guides/multiplayer#platforms-and-lifts)); `rewind_cap_ms` — the furthest back, in milliseconds, a shot is checked against what its shooter saw, 200 by default and 0 for never ([A shot checked as its shooter saw it](manual:guides/multiplayer#a-shot-checked-as-its-shooter-saw-it)); `kick_over_limit` — a player whose calls go past the remotes' limits this many times in a second is removed, 0 for never and by default ([Limits, suspicions and mutes](manual:guides/multiplayer#limits-suspicions-and-mutes)); `server_key` — the key of the `server`, as its log printed it, so even the first join checks it; `encrypt = false` — matches in the clear, for development only, ignored by a shipping build ([A match is encrypted](manual:guides/multiplayer#a-match-is-encrypted)); `ticket_public_key` — the key the game's backend signs its login tickets with, base64 ([Who may join, and accounts](manual:guides/multiplayer#who-may-join-and-accounts)) |
| `[assets]` | `content` — where the asset compiler reads from |
| `[luau.flags]` | Luau's flags the project turns on early, each `true`, `false` or a whole number: the same values reach the game, the editor, `ludwerk check` and VS Code. See [Luau language settings](manual:concepts/luau-flags) |
| `[graphics]` | The quality family. See [Graphics quality settings](manual:rendering/quality) |
| `[render]` | `max_views_per_frame`, `max_view_resolution`, `max_sub_worlds` — what camera textures may cost (see [Views](manual:rendering/views)); `max_highlights` (see [Highlights, beams and trails](manual:rendering/effects)); `foliage_density`, `foliage_shadow_distance` (see [Foliage](manual:world/foliage)) |
| `[export]` | How `ludwerk build` packages the game, per target. See [Shipping a game](manual:guides/shipping) |
| `[save]` | `max_slot_bytes` and `max_slots` — how much a game may save. See [Saving the game](manual:guides/saves) |
| `[scene]` | `close_grace_seconds` — how long closing waits for a scene's handlers. See [Scenes at run time](manual:guides/scenes) |
| `[ui]` | `font_fallbacks` — the fonts a character a label's own font has not got is drawn with, in order, before the default font: `["asset://fonts/NotoSansCJKjp-Regular.otf"]`. See [Text and images](manual:ui/text-and-images) |
| `[script]` | `max_memory_mb` — the cap on the scripts' memory: 1024 on a desktop and 512 on a phone unless you say. A table that grows without end stops the scripts before it stops the machine. `timeout` — the seconds a script may run without yielding before it is stopped, 5 unless you say. `load_timeout` — the same for a script's first run and a module's first run, 30 unless you say. Both count the script's own running time, not the clock's. See [task, and asking for time](manual:concepts/task) |
| `[xr]` | `enabled = true` — try a headset at start, through OpenXR, and draw on the screen when there is none; `required = true` — a game played only in a headset, which without one says so and does not start; `mirror = false` — the window stays black instead of showing the left eye; `hands = false` — controllers only, never the hands themselves; `depth = false` — the headset is not given each eye's depth; `--xr` and `--no-xr` on the command line override `enabled` and `required`. See [VR](manual:guides/vr) |

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

## Where code runs

Where code runs is decided by where it is in the tree, and outside the script
services by a script's own `RunContext`. See
[Scripts, modules and requires](manual:concepts/scripts).

## Where to look next

- [Your first world](manual:get-started/first-world)
- [The ludwerk CLI](manual:get-started/cli)
- [Content and asset URNs](manual:assets/content)
