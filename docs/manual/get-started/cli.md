# The ludwerk CLI

Eight commands. Unknown flags are **refused**, not ignored.

```bash
ludwerk --help
ludwerk --version
```

## The commands

### ludwerk new

```bash
ludwerk new my-game
ludwerk new my-game --template=starter
```

Scaffolds a project. The name must be letters, digits, underscores and hyphens.
`starter` is the only template.

### ludwerk dev

```bash
ludwerk dev
ludwerk dev examples/06-scene
ludwerk dev --port=4700
ludwerk dev --headless
```

Runs the project with a watcher attached: a saved `.luau` file rebuilds the
world. Defaults to the current directory.

`--port` overrides `[dev] port`. `--headless` runs without a window, which is
what a gate does.

A project that mounts **no entry scripts** is refused rather than started —
otherwise it would attach, watch and reload an empty world forever while
reporting success.

### ludwerk edit

```bash
ludwerk edit
ludwerk edit examples/06-scene
```

Runs the project with the editor in place of the debug overlay. No flags: an
editor needs a window, so there is no headless form.

### ludwerk test

```bash
ludwerk test
ludwerk test tests/conformance/tween
ludwerk test --junit=results.xml --report=report.json
```

Runs the conformance suite on the headless engine with the null renderer.
Defaults to `tests/conformance`. TAP goes to standard output either way;
`--junit` adds XML.

Exits non-zero for a failure, for **zero cases**, and for a report it could not
read.

### ludwerk check

```bash
ludwerk check
ludwerk check --definitions=runtime/types/engine.d.luau
```

The analyzer with the engine's generated definitions, plus the formatter in
check mode. `--definitions` takes a comma-separated list.

An empty check — zero files collected — is a **failure**.

### ludwerk fmt

```bash
ludwerk fmt
```

Formats every Luau file.

### ludwerk build-assets

```bash
ludwerk build-assets
ludwerk build-assets --verify --output=dist/content.lpack
```

Compiles the content directory into a pack and a manifest. `--verify` builds
twice and compares byte for byte.

### ludwerk build

```bash
ludwerk build --target=windows
ludwerk build --target=linux --output=dist/linux --force
ludwerk build --target=android --bump-version-code
ludwerk build --target=linux-server
ludwerk build --status
```

Exports the game ([Shipping a game](manual:guides/shipping)). The targets are
`windows` (`win64` is its old name), `linux`, `android`, and for a game whose
`[export] multiplayer` is `dedicated`, `windows-server` and `linux-server`.
Every target builds from any desktop whose installation carries its player.

| Flag | Does |
|---|---|
| `--target=NAME` | the target; `windows` when omitted |
| `--output=DIR` | where the folder goes; `dist/<target>` when omitted |
| `--force` | clear an output directory this tool did not write |
| `--progress=json` | one JSON line per step on stdout, for a program to read |
| `--bump-version-code` | raise `[export.android] version_code` by one in `project.toml` first |
| `--dev-host` | package this machine's development host when no player is built: it carries the debug overlay, the inspector and a REPL, so never release it |
| `--status` | which targets this machine can export, as one JSON line |

With `--progress=json`, each step writes `{"step": ..., "state": "start"}`
and then `"done"` or `"fail"` with `"ms"` and, on a failure, `"message"`; a
line `{"note": ...}` is a sentence for a person, and the last line is
`{"result": {...}}` with the folder, the executable, the archive or the APK.
Lines that are not JSON are the tools' own output. The steps, in order:

| Step | |
|---|---|
| `check` | the project's settings, the player for the target, and the tools it needs |
| `pack` | the content pack -- per side, for a dedicated game |
| `layout` | the player, the engine's content and the game, into the folder |
| `partition` | the streaming cells, pre-warmed into the package |
| `icon` | one PNG, every size the target needs |
| `stamp` | the icon and the version, into a Windows executable |
| `archive` | the `.zip` or `.tar.gz` beside the folder |
| `assemble` | the APK, by Gradle |
| `sign` | the APK's signature, checked by `apksigner` |

### ludwerk keystore

```bash
ENG_ANDROID_KEYSTORE_PASSWORD=... ludwerk keystore new --alias skyhopper --out keys/release.keystore --name "Studio" --validity 25 --remember
ENG_ANDROID_KEYSTORE_PASSWORD=... ludwerk keystore remember --out keys/release.keystore
```

Makes the key an Android game is released with, and refuses to replace one
that exists. The password comes from the environment, never the command line;
`--remember` keeps it in the per-user store so a release export finds it. A
remembered password is for one project and one keystore: `--project` names
the project (the current folder when it is left out), and a release export of
another project that names the same keystore does not find it. The store is
a file only you can read, not an encrypted one; the environment variable is
the way to sign without it.

### ludwerk android install-tools

```bash
ludwerk android install-tools
```

A JDK 17, the Android SDK (platform 35, build-tools 35.0.0, platform-tools)
and Gradle 8.12, into your Ludwerk folder -- what an Android export needs. No
NDK: the player is prebuilt. A second run installs only what is missing.

## Exit codes

| Code | Means |
|---|---|
| 0 | Success. |
| 1 | The work failed: a test failed, the analyzer complained, a build broke. |
| 2 | You asked for something impossible: an unknown command, an unknown flag, an unsupported target, a path that is not a project. |
| 3 | The tool could not load its own message catalog. |

The split between 1 and 2 is the useful one: **2 is a usage error and 1 is a
result**. A script driving the CLI can tell "I typed it wrong" from "the tests
failed".

## The engine binary

The CLI launches a separate host executable. Running it directly is what a gate,
a benchmark or a capture does:

```text
engine-host [script.luau | project-dir]
  [--headless --frames=N --exit] [--width=N --height=N]
  [--screenshot=FILE] [--frame-stats] [--rhi=NAME]
  [--quality=low|medium|high|ultra --render-scale=F
   --shadow-resolution=N --shadow-cascades=N --shadow-distance=F
   --light-budget=N --[no-]bloom --[no-]ambient-occlusion
   --[no-]contact-shadows --[no-]anti-aliasing --[no-]auto-exposure]
  [--screenshot-every=N --debug-view=VIEW --terrain-detail=full|distance]
  [--pace=HZ]
  | --run-tests=DIR | --replay=DIR [--record-replay] | --version | --help
```

**Three test instruments**, for a picture that proves something:

- `--screenshot-every=N` takes a picture every N frames, not only the last,
  each named after `--screenshot` with its number: `shot.png` becomes
  `shot-000.png`, `shot-001.png` and so on. A scene that moves its camera on a
  schedule is photographed from every place in one run. Terrain is then built
  in the frame that draws it, rather than beside it, so each picture is of
  ground settled for that frame.
- `--debug-view=VIEW` draws, in place of the picture: `holes` (the sky magenta
  and every terrain white, for counting sky seen through the ground), `level`
  (each terrain node in the colour of its level of detail), `sky` (the
  terrain's sky term), `shadow` (the sun's shadow on terrain: the map in red,
  the contact mask in green) or `occlusion`.
- `--terrain-detail=full` draws every terrain at its finest level whatever the
  distance: the shape as it was sculpted, which a coarse level is held
  against.

`--pace=HZ` makes a headless run wait out each frame's share of a second, the
wait left out of `--frame-stats`: without it a headless flight is over in a
second or two, and ground built beside the frame is measured against a camera
moving far faster than anyone flies.

Four refusals worth knowing, all of them exit 2:

- `--headless` needs `--frames=N`, or nothing would ever stop it.
- `--screenshot` needs `--headless`.
- `--capture-out` needs the capture backend.
- The editor needs a window.

`--window=x,y,w,h` places the window, `--label=Name` adds a name to its title,
and `--log-file=path` writes the log somewhere other than `engine.log`. The
editor's Play with players uses all three for each window of a match.

**Given no arguments at all**, the host looks for a `game/` directory beside its
own executable and mounts it. That is what makes a packaged build
double-clickable — see [Shipping a game](manual:guides/shipping).

## What the CLI is

A wrapper around the pinned Lute runtime rather than a compiled binary. A
downloaded Ludwerk carries that runtime and a `ludwerk.cmd` beside it, so putting the
folder on your `PATH` is the whole installation; a source tree has
`scripts/ludwerk.ps1` and `scripts/ludwerk.sh` doing the same job for the same
reason. Either way, every command on this page works as written.

**It finds its own engine.** The host it launches is the one in the installation
it belongs to, whatever directory you are standing in — or the one in your build
tree, when you have one. `ENG_HOST` overrides both, and `ludwerk build`
deliberately ignores that override: what you pointed a dev server at must not
decide what your players get.

## Where to look next

- [Anatomy of a project](manual:get-started/project-anatomy)
- [Hot reload](manual:guides/hot-reload)
- [Shipping a game](manual:guides/shipping)
