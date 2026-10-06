# Shipping a game

A game is exported from one window, for Windows, Linux and Android, from any
desktop. **File > Export...** (Ctrl+Shift+B), or the Export button beside
Play.

```bash
# the same exports, from a terminal or a CI script
ludwerk build --target=windows
ludwerk build --target=linux
ludwerk build --target=android
```

The editor runs exactly these commands and draws what they say, so the button
and a script make the same bytes.

## Before the first export: who the game is

**Edit > Project Settings > App** holds what every target stamps, and it is
written into `project.toml`:

```toml
[project]
name = "Sky Hopper"            # the window, the launcher, the file properties
id = "com.studio.skyhopper"    # reverse-DNS; the Android package unless it names its own
version = "1.2.0"              # X.Y.Z: the Windows file version, the Android version name
company = "Studio"             # the Windows file properties, the Linux launcher entry
icon = "branding/icon.png"     # ONE square PNG; 1024 px is best

[window]
title = "Sky Hopper"
size = [1280, 720]
fullscreen = false
resizable = true
```

**One picture, every size.** The export makes a Windows `.ico` (16 to 256), a
512 px Linux icon, and Android's launcher icons -- the legacy ones and the
adaptive one, on the background colour you choose. The App page previews the
icon on a desktop, on a taskbar and on a phone's home screen. An `.ico` still
works as the icon, for Windows only.

## The Export window

- **The target cards**, on the left: Windows, Linux and Android, each saying
  whether this machine can export it. *Ready*; *tools missing*, with an
  **Install Android tools** button; or, for Android, *phone connected* with the
  phone's model when one is on USB with debugging allowed.
- **The selected target's settings**, on the right, written into `project.toml`
  as you change them (see below). The game's identity is shown above them, with
  a link to Project Settings. The output folder defaults to `dist/<target>/`.
- **Export** exports the selected target; **Export all** exports every ready
  one, one after another.

While an export runs, the editor stays usable, and a list of steps ticks over:
checking the project, packing content, copying the player, partitioning the
world, making the icons, stamping them, archiving -- and for Android assembling
the APK and checking its signature. A step that fails turns red with its
message; **Show log** has the whole output.

When it finishes, a card shows the size and the time it took, with **Open
folder**, **Run it** on a desktop target, and **Install on phone** for an APK
when a phone is connected: it installs and launches, so the game is on the
phone in seconds. The last ten exports of the project are listed under the
buttons; clicking one opens its folder.

## What each target makes

| Target | Output | |
|---|---|---|
| Windows | `dist/windows/<Executable>.exe`, `content/`, `game/`, and a `.zip` beside the folder | No console window. The icon and the file properties (product, company, version, description) are in the executable. |
| Linux | `dist/linux/<executable>`, `content/`, `game/`, `<executable>.desktop`, `icon.png`, and a `.tar.gz` beside the folder | The archive carries the execute bit, wherever it was made. |
| Android | `dist/android/<name>-<version>.apk` | Signed with the debug key, or your release key. |

The archive beside the folder is the thing to send somebody. It is named
`<name>-<version>-<target>`.

**The player's executable finds its game by convention**: given no arguments,
it mounts the `game/` directory beside itself. That is the whole mechanism --
a convention rather than a configuration file, because a configuration file is
a second thing that can go missing.

### Every target, from any desktop

The editor's folder carries a prebuilt player for each target, under
`player/`. A Windows or Linux export is a copy, a rename, a stamp and an
archive: no compiler, no Docker. An Android export uses the prebuilt player
too -- no NDK -- and assembles the APK with Gradle, which needs a JDK 17, the
Android SDK and Gradle. **Install Android tools** (or `ludwerk android
install-tools`) puts all three in your Ludwerk folder, never on PATH and never
in `JAVA_HOME`, and a second run installs only what is missing. Installing the
SDK accepts its licences on your behalf.

## Per-target settings

```toml
[export.windows]
executable = "SkyHopper"          # default: the name

[export.linux]
executable = "skyhopper"          # default: the name, in lower case

[export.android]
package = "com.studio.skyhopper"  # default: [project] id
version_code = 7                  # a store refuses an update whose code is not higher
icon_background = "#1E90FF"       # the adaptive icon's background
icon_foreground = "branding/foreground.png"  # optional: a foreground drawn for the safe zone
release = false                   # false: the debug key; true: the keystore below
keystore = "keys/release.keystore"
key_alias = "skyhopper"
```

**The debug key is Android's own**: `~/.android/debug.keystore`, the one the
SDK's tools make and Android Studio signs a debug build with. A debug APK from
this engine and one from any other tool on the machine therefore update one
another, and the key does not move when the engine is updated or renamed. A
machine that has none gets one made there the first time.

Two machines have two debug keys, and a phone will not update a game across
keys: it says the signatures do not match, and the Export window says what that
means -- uninstall the game from the phone once (which deletes what it saved
there) and install again. A team that passes test builds around shares one
key instead:

```toml
[export.android]
debug_keystore = "keys/team-debug.keystore"   # relative to the project
debug_key_alias = "androiddebugkey"           # the default; passwords are "android"
```

**A game installed by a build from before 2026-10-02** was signed with a key
the engine then kept in its own folder, not Android's, and every build since
is signed with Android's -- so the phone refuses the update once. Uninstalling
the game once ends it for good. To update without uninstalling -- to keep what
the game saved on the phone -- point that game at the key it was installed
with, which is one of these on the machine that built it:

```toml
[export.android]
# Windows: %LOCALAPPDATA%\Ludwerk\android\debug.keystore, or the same under
# %LOCALAPPDATA%\engine\ for a build from a package with no brand file.
debug_keystore = "C:/Users/you/AppData/Local/Ludwerk/android/debug.keystore"
debug_key_alias = "enginedebugkey"
```

Which one a phone holds, its SHA-256 beside each key's: `adb shell pm path
<package>`, `adb pull` that file, then `apksigner verify --print-certs` on it
and `keytool -list -v -storepass android -keystore <key>` on each.

**Which way up the phone is held** is not a setting here: it is the start
scene's `UIService.ScreenOrientation`, which the APK is held at from its first
frame and which a script may change later. One answer, so the screen does not
turn the moment the game starts.

**Bump on export** (`ludwerk build --target=android --bump-version-code`) raises
`version_code` by one in `project.toml` before it exports, leaving the rest of
the file as it was.

**When a phone will not open the game**, it says why: a dialog with the error
and where the log is. The log of a run, and of the run before it, is in the
game's own folder on the phone -- `Android/data/<package>/files/engine.log`
and `engine.previous.log` -- which a file manager or a cable opens with no
permission asked; a crash report lands beside them. Ask a tester for those
files whether the game opened or not.

## Signing an Android game

**Debug**, the default, needs no setup: the first export makes a key for this
machine and every later one uses it, so a new APK installs over the last one.
A debug APK installs on any phone with installing from unknown sources
allowed, and a store will not take it.

**Release** is the key a store knows your game by. **Create keystore...** in
the Export window (or `ludwerk keystore new`) makes one under `keys/`: an alias,
the name on the certificate, a validity, and a password.

> **Keep the keystore and its password safe, and back them up.** A store
> accepts an update only when it is signed with the same key. Lose the file,
> or forget its password, and the game can never be updated again -- only
> published anew, under another package name.

**The password is never in `project.toml`.** The export reads it from, in order:

- `ENG_ANDROID_KEYSTORE_PASSWORD` (and `ENG_ANDROID_KEY_PASSWORD` when the
  key's differs), which is how a CI job has it;
- the per-user store beside the editor's preferences, outside every project,
  where the Export window keeps it when you tick *remember on this machine*.

It reaches `keytool` and Gradle through their environment, never their command
line, where other programs on the machine could read it.

## Multiplayer: what each package leaves out

The **Multiplayer** selector above the cards is `[export] multiplayer`:

| Mode | Packages | A player's package carries | The server's package carries |
|---|---|---|---|
| Single player (`none`, the default) | one per target | everything -- it runs solo, as the authority | -- |
| Players host (`host`) | one per target | everything -- any player may host | -- |
| Dedicated server (`dedicated`) | a **client** per target, and a **server** for Windows and Linux | everything **except** `ServerScriptService`, `GlobalScriptService.Server`, `ServerStorage`, `src/server/` and `src/scenes/*/server/` | everything **except** `ClientScriptService`, `GlobalScriptService.Client`, `src/client/` and `src/scenes/*/client/` |

**A dedicated client carries none of the server's code or storage** -- not in
its scripts, not in its scenes, not in its content pack and not in its
partition cache -- and the server none of the clients'. It is decided by what
is left out of the package, never by a flag the game reads, and the test suite
exports a project with a distinct string in each place and searches every byte
of the other side's package for it.

In dedicated mode the Export window adds **Windows server** and **Linux
server** cards, the client cards say which address they join, and a server
address field appears: `[network] server`, where `NetworkService:Join()` goes
with no address. A client starts in its first scene -- usually a menu -- and
joins when its scripts call `Join()`.

A server package (`ludwerk build --target=windows-server` or `linux-server`) has
no window and no icon, and **starts serving with no arguments**: its own
`project.toml` says `[network] role = "server"`. Start it with `--host` to play
on it as well.

What each service is, on each side (solo and a host have both columns):

| Service | Dedicated server | Client that joined |
|---|---|---|
| `GlobalScriptService` | `Server` runs, `Shared` is there, `Client` absent | `Client` runs, `Shared` is there, `Server` absent; the service's attributes arrive |
| `ServerScriptService` | runs | absent |
| `ClientScriptService` | absent | runs |
| `ServerStorage` | has it | absent from a dedicated package; emptied on join |
| `ReplicatedStorage` | has it | receives all of it |
| `Workspace` | decides | receives what is near its character |

## What goes into `game/`

**One pack**, `game/.engine/content.lpack`: the game's content, its scripts
(compiled, below), its catalogues, `project.toml` and `.luaurc`, sealed into
one file (ADR 0183). Beside it, only what is streamed from files of its own --
a streamed terrain's cells, the partition cache, made on the package itself so
a dedicated side's cache holds only its own scene. It does **not** hold the
editor's files: layouts, `editor.json`, the import and check caches.

- **No name is readable in it.** What a script asks for as
  `asset://textures/base.png` is found by the hash of that name; the list of
  the game's files, and every scene, stamp, material, script and catalogue,
  is compressed.
- **A damaged copy says so.** Every entry is checked against the hash it is
  filed under while the game loads; a download cut short, or a file changed
  since, is "this game's files are damaged" and not a game that half runs.
- **Nothing in your game changes.** Content resolves by the names it always
  had, errors name the script and the line, and saves are not in this folder.
- **It is not a lock.** The format is public and `assetc unseal <game> --out
  <folder>` writes the files back out. It keeps your project from being
  handed over as a folder to browse; it does not keep a determined person out.

To ship the folder of files instead -- `project.toml`, `src/`, `i18n/` and
the pack with its manifest, as every build before this made:

```toml
[export]
packed = false
```

If no content pack was built, the loose `content/` and `assets/` trees are
copied instead -- shipping both would double the size of every game, and
shipping neither would be a game with no art.

## It ships bytecode, not source

Every script under `src/` is compiled to Luau bytecode as the game is laid
out, by the engine's own compiler with the options it runs scripts with, and
the source is removed: `init.luau` ships as `init.luauc`, inside the pack. No
`.luau` is in the package (ADR 0112).

- **An error still names the script and the line.** The bytecode keeps line
  information, so a player's log says `src/client/init.luau:42` as your own
  run would; what it drops is local variable names.
- **A package runs only on the engine build that exported it**, or one that
  reads the same bytecode versions. Run by another, a script is refused with a
  message naming both, and exporting again fixes it.
- **Bytecode is not encryption.** It removes the plain-text copy, and a
  determined reader can still decompile it. Anything that must not be in the
  game — a key, a token, an endpoint you do not want found — belongs on your
  backend. A dedicated game's server code is the exception, because it is not
  in the client's package at all.

To ship the source instead — a game meant to be read or modded — tick **Ship
source** in the Export window, or:

```toml
[export]
ship_source = true
```

A game that ships its source is left as files, for the same reason; add
`packed = true` to seal it all the same.

## Before you ship

```bash
ludwerk check
ludwerk test
ludwerk build-assets --verify
ludwerk build --target=windows
```

`--verify` builds the content twice and compares byte for byte. If the two
differ, something in the pipeline is not deterministic, and it fails rather than
shipping a lottery.

Then run the exported game and play it -- **Run it** on the result card, or
the executable with no arguments. The packaging gate in this repository does
exactly that, because "it built" and "it runs" are different facts.

## Development-only services are gone

`HotReloadService` is compiled out. A script that reaches for it unconditionally
gets nothing:

```luau
--!strict
local ok, hotReload = pcall(function(): Instance
    return game:GetService("HotReloadService")
end)
if ok then
    -- development: `hotReload` is the service
    print(hotReload.Name)
end
```

`DebugService` remains, with its overlay compiled out — the methods become
no-ops, so debug drawing left in shared code costs nothing.

## Where to look next

- [The asset pipeline](manual:assets/pipeline)
- [The ludwerk CLI](manual:get-started/cli)
- [Scenes](manual:guides/scenes)
- [Talking to a backend](manual:guides/backend)
