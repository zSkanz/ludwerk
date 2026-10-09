# The ludwerk CLI

Unknown flags are **refused**, not ignored.

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
`starter` is the only template. The project's `id` is made from the name in
lower case (`dev.local.my_game`), which is what an Android package may be
called; change `dev.local` to a domain of your own before you ship.

### ludwerk dev

```bash
ludwerk dev
ludwerk dev examples/06-scene
ludwerk dev --port=4700
ludwerk dev --headless
ludwerk dev --scene=scenes/arena.scene.json --scene-data='{"round": 3}'
```

Runs the project with a watcher attached: a saved `.luau` file rebuilds the
world. Defaults to the current directory.

`--port` overrides `[dev] port`. `--headless` runs without a window, which is
what a gate does. `--scene` starts the run in that scene instead of the
project's, and `--scene-data` is JSON its `GetLoadData` answers: see
[Scenes](manual:guides/scenes).

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

**Nothing to install for the types.** An installation carries its own
analyzer (`tools/bin/luau-lsp`), at the version its definitions were generated
for, so a project made a minute ago passes its first check. The formatter is
StyLua, which is not carried: if it is not on your `PATH` the check says the
formatting was not checked and passes on the types.

**A project is checked twice, and must pass both.** First by the analyzer,
which is what an editor extension shows. Then by the engine's own checker,
which is what a script's tab in the editor shows: every scene of the project
is loaded, and every script in it is checked as its tab would check it —
`engine-host <project> --check-scripts`, which you can run yourself. So a
project this command calls clean opens clean in the editor. An error from
either fails the check; the editor's warnings are printed and do not. A
problem is one line a terminal makes a link:

```
src/client/Hud.luau(12,5): error: Type 'number' could not be converted into 'string'
```

A `require` by path — `require("../shared/Match")` — is followed by both, by
the rule the engine runs one by: beside the requiring file for `./` and `../`,
from the project's root for anything else, a `.luaurc` alias for `@name/`, and
`Match.luau`, `Match.module.luau`, `Match/init.luau` in that order. A path
that names no file is an error.

An empty check — zero files collected — is a **failure**.

### ludwerk fmt

```bash
ludwerk fmt
```

Formats every Luau file, with [StyLua](https://github.com/JohnnyMorganz/StyLua/releases):
put `stylua` on your `PATH`, or in the installation's `tools/bin` folder. It is
the one tool an installation does not carry.

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
| `seal` | the scripts, the catalogues and the settings into the pack, unless `[export] packed = false` |
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
  [--scene=PATH [--scene-data=JSON]]
  [--screenshot=FILE] [--frame-stats] [--rhi=NAME] [--gpu=NAME]
  [--quality=low|medium|high|ultra --render-scale=F
   --shadow-resolution=N --shadow-cascades=N --shadow-distance=F
   --light-budget=N --[no-]bloom --[no-]ambient-occlusion
   --[no-]contact-shadows --[no-]foliage-decals
   --[no-]anti-aliasing --anti-aliasing=MODE --upscaling=MODE
   --sharpness=F --[no-]frame-generation --[no-]auto-exposure]
  [--screenshot-every=N --debug-view=VIEW --terrain-detail=full|distance
   --no-instancing]
  [--pace=HZ]
  [--[no-]vsync --max-frame-rate=N --background-frame-rate=N]
  | --run-tests=DIR | --replay=DIR [--record-replay] | --version | --help
```

`--gpu=NAME` names the graphics API the run draws through -- `vulkan`,
`direct3d12`, `metal` -- in place of the system's own choice. A name no
device can be made through warns and falls back. It is what
[a player with a wrong picture](manual:guides/shipping)
is asked to try.

**Four test instruments**, for a picture that proves something:

- `--screenshot-every=N` takes a picture every N frames, not only the last,
  each named after `--screenshot` with its number: `shot.png` becomes
  `shot-000.png`, `shot-001.png` and so on. A scene that moves its camera on a
  schedule is photographed from every place in one run. It builds up to 256
  terrain nodes a frame, so each picture is of ground settled for its case.
- `--debug-view=VIEW` draws, in place of the picture: `holes` (the sky magenta
  and every terrain white, for counting sky seen through the ground), `level`
  (each terrain node in the colour of its level of detail), `sky` (the
  terrain's sky term), `shadow` (the sun's shadow on terrain: the map in red,
  the contact mask in green, blue where the ground faces the sun, the sky
  black -- `imgshadow` counts faces to the sun either shadow darkens),
  `occlusion`, `bend` (what the shading does to the mesh's normal, four
  times over, the sky black -- `imgsteps` counts where it jumps), `albedo`
  (the colour the ground is lit as), `material` (the layer each pixel is
  drawn as, a colour a layer) or `motion` (over the finished picture, how far
  each pixel moved since the last frame: red and green along x and y, mid
  grey for none and a channel's whole range for sixteen pixels either way,
  blue where it moved at all -- what the temporal pass and frame generation
  are told).
- `--terrain-detail=full` draws every terrain at its finest level whatever the
  distance: the shape as it was sculpted, which a coarse level is held
  against.
- `--no-instancing` draws every object with a call of its own: the picture
  that runs of a mesh -- static or skinned -- drawn in one call have to match
  pixel for pixel.

**`--frame-stats`** prints, after the run, the frames' median and tail, and
then where the time went, scope by scope: the simulation's phases, the
physics' steps, animation, the scripts by phase, the network, the UI, the
renderer's passes and the waits, each with its median, p95, worst frame, how
many times it ran a frame, and its `self` -- what no scope inside it accounts
for. A cost nobody has named yet shows up as a parent's `self`. Then the
slowest frames, each on its own -- the worst five and any other over twice the
median -- as the tree of that one frame: what a player felt as a hitch, which
no median names. The Luau collector's steps are `scripts.gc`.

What arrives in a frame is under `content.*` -- `content.textures`,
`content.meshes.load`, `content.terrain` (with `terrain.upload`, the meshes put
up; `terrain.edit`, an edit built in the frame; `terrain.build`, what is handed
to the workers), `content.blocks`, `content.water`, `content.foliage` -- and the
drawing's own stretches are `draw.choose`, `draw.effects`, `draw.sky`,
`draw.interface`, `draw.submit`, `draw.uploads`, `draw.views` and `draw.render`.
A scene's first frames are where these are large; a frame of play in which one
of them is, is a defect to report.

The log's own line about a long frame -- "Frame took N ms -- simulation and
its scripts ..., render-step scripts ..., waiting for the GPU and the display
..., the rest ..." -- splits that frame the same way, with no flag: the first
thing to read when a player says the game hitched.

`--pace=HZ` makes a headless run wait out each frame's share of a second, the
wait left out of `--frame-stats`: without it a headless flight is over in a
second or two, and ground built beside the frame is measured against a camera
moving far faster than anyone flies.

**A window's frames are paced**, over the project's `[display]`: `--vsync`
and `--no-vsync` say whether a frame waits for the display, `--max-frame-rate=N`
caps it (0 for no cap), and `--background-frame-rate=N` is the rate while the
window is unfocused or minimised (10 unless the project says otherwise; 0 for
no throttle). A headless run, and one with `--pace`, is not paced by them.

**A run that takes a picture builds its terrain in the frame** that draws it,
rather than beside it as a game does: headless frames run flat out, and the
picture is of what the frames before it built, not of what a worker had
finished by then.

Five refusals worth knowing, all of them exit 2:

- `--headless` needs `--frames=N`, or nothing would ever stop it.
- `--scene-data` needs JSON.
- `--screenshot` needs `--headless`.
- `--capture-out` needs the capture backend.
- The editor needs a window.

`--gpu-least` makes the graphics device a GPU with the fewest features is
given -- no depth clamping -- on whatever GPU the machine has: how a desk
draws as such a phone does. `--no-astc` sends textures as RGBA to a GPU that
samples ASTC.

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

## Optional platform tools and integrations

```bash
ludwerk modules status --project=my-game
ludwerk modules install android --accept-licenses
ludwerk modules install microsoft-gdk --accept-licenses
ludwerk modules update microsoft-gdk --accept-licenses
ludwerk modules verify microsoft-gdk
ludwerk modules remove microsoft-gdk
```

Windows and Linux use the installation's prebuilt players. Android and Microsoft
GDK tooling are optional. Installing shared tools does not enable a project service,
and enabling a service does not install an SDK. The editor exposes these operations
through **Platforms and Services**, reached from Project Settings or Export.

A compatible prebuilt native module can also be installed without the SDK or a
C++ compiler. Supply the archive and its published SHA-256:

```bash
ludwerk modules install microsoft-gdk --accept-licenses --package=module.zip --sha256=PUBLISHED_SHA256
```

The archive's module ID, pinned version, ABI and individual file checksums must
match. The editor offers the same package installation under the integration's
card. The SDK installer checks its pinned download hash; Verify also detects
changes to essential extracted SDK files. Removal applies only to this manager's
owned module version. Built-in players and externally installed Android tools
are kept.
