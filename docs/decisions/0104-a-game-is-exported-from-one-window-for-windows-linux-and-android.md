# 0104 — A game is exported from one window, for Windows, Linux and Android

- Status: accepted
- Date: 2026-09-26
- Decided by: the owner, on 2026-09-26. He asked for an area in the editor
  where a game is built into an executable for Windows, Linux and Android, and
  where its icon, name, executable and Android app are set. It should be
  practical and fun, not complicated. The shapes below are the agent's, after
  surveying how four engines do it.
- Amends: [0045](0045-a-packaged-game-is-a-folder-that-ships-source.md).
  Its `win64`-only target list grows to three targets. Its folder layout and
  its source-not-bytecode rule stand.
- Amended by: [0105](0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md)
  -- a multiplayer mode (`none | host | dedicated`), server targets, and a
  dedicated game's client package without the server's code or storage.
- Relates to: [0054](0054-the-editor-ships-as-a-folder-and-the-cli-finds-its-own-install.md)
  (the distribution carries the player binaries),
  [0044](0044-graphics-settings-are-host-settings.md)
  (`luaug.toml`), [0091](0091-a-material-may-name-a-surface-shader-the-user-writes.md)
  (a build carries compiled shaders only)

## Context

`luaug build` exists, but it only makes a Windows folder, and it must run on
Windows. Android is a separate PowerShell and Gradle script. That script makes a
debug APK with no icon, `versionCode = 1`, and the engine's version as the
app's. Linux has a player binary, built in Docker for the gate, and no export at
all. The editor has no Build or Export entry. Its Project Settings dialog edits
name, id, title and resolution, and nothing about how the game is shipped.

The survey found other things wrong along the way:

- A packaged Windows game is probably a console program. `luaug_host` is not a
  `WIN32` executable.
- `luaug build` copies `.luaug/` whole, which includes the editor's own layout
  and settings files.
- `[project] version` is read by nothing.
- `[window] width` and `height` are ignored, while ten examples use them.
- `[project] icon` is `.ico` only, and the host never shows it.

### How other engines do it

- **Godot:** Project > Export opens one dialog.
  - On its left is a list of *presets*: one per target, several allowed,
    stored in `export_presets.cfg`. On its right are that preset's options,
    with icons among them.
  - Export needs *export templates*, which are prebuilt players for the exact
    engine version. They are downloaded once.
  - Android signing keys live in the preset file. That is a known footgun,
    because the file gets committed with passwords in it.
- **Unity 6:** File > Build Profiles.
  - Platforms are on the left, and a profile per build configuration can
    override the global Player Settings (icon, company, product name,
    resolution, splash).
  - Identity lives in Player Settings, and the build window only picks a
    target and presses Build.
- **Defold:** Project > Bundle > *platform* opens a small dialog: variant,
  format (APK or AAB), keystore, and Create Bundle.
  - Icons, version code and package name are in `game.project`, the one
    project file.
  - **With no keystore given, it generates a debug key and signs with it.** The
    first APK on a phone costs nothing.
- **Unreal:** Platforms menu > *platform* > Package Project. Identity is spread
  across Project Settings pages (Description, Packaging, Android).

The shape they converge on has three parts:

1. **Identity is set once, in the project.** That covers name, version, icon
   and company.
2. **Per-platform details are set beside the target.** That covers package id,
   signing, architecture and orientation.
3. **The export window is a list of targets with an Export button.**

The friction they share comes from three places: installing SDKs, passwords in
committed files, and one icon per size per platform.

## Decision

### 1. Identity is one section of `luaug.toml`, set in Project Settings

What a game *is* is written once, and every target reads it:

```toml
[project]
name = "Sky Hopper"            # the display name: window, launcher, Android label
id = "com.skanz.skyhopper"     # reverse-DNS; the Android package, the Windows AppUserModelID
version = "1.2.0"              # shown to players; every target stamps it
company = "Skanz"              # Windows file properties, the Linux .desktop
icon = "branding/icon.png"     # ONE square PNG, 1024 px recommended; see 2

[window]
title = "Sky Hopper"
size = [1280, 720]
fullscreen = false             # new: the desktop player starts fullscreen
resizable = true               # new
```

- `[project] version` stops being decorative. The Windows file version, the
  Android `versionName` and the Linux `.desktop` file carry it.
- `[window] width` and `height` are read as an alias of `size`. Every example
  that already uses them keeps working.
- Project Settings gains an **App** page with these fields. It shows a live
  preview of the icon as each platform will show it: a desktop tile, a taskbar
  button, and a phone home screen with the label under it.

### 2. One icon in, every size out

A project names **one PNG**. The export makes every size each platform needs.
No author opens an icon editor.

- **Windows:** a multi-size `.ico` (16, 24, 32, 48, 64, 128, 256), written into
  the executable by `iconpatch`.
- **Linux:** a 512 px PNG beside the binary, named in the `.desktop` file.
- **Android:**
  - legacy launcher icons at mdpi through xxxhdpi;
  - an adaptive icon, with the PNG as its foreground on a background colour
    from `[export.android] icon_background`.
  - An optional `icon_foreground` PNG replaces the foreground, for an author
    who drew one with the safe zone in mind.

The resizer is the engine's own. The PNG decoder is already vendored, and a box
or Lanczos downscale is a page of C++ in `assetc`. It is not a new dependency.
An `.ico` given as the icon still works, for Windows only, as today.

### 3. Per-target settings live under `[export.<target>]`, and secrets never do

```toml
[export.windows]
executable = "SkyHopper"       # default: the name, sanitised

[export.linux]
executable = "skyhopper"

[export.android]
package = "com.skanz.skyhopper"   # default: [project] id
version_code = 7                  # the export offers to bump it
icon_background = "#1E90FF"
release = false                   # false: debug-signed, installs anywhere; true: the keystore below
keystore = "keys/release.keystore"
key_alias = "skyhopper"
```

**Passwords are never in `luaug.toml`.** This is the lesson Godot paid for. A
keystore password comes from:

- the per-user store beside the editor's preferences, which is outside the
  project and never committed, or
- `LUAUG_ANDROID_KEYSTORE_PASSWORD` and `LUAUG_ANDROID_KEY_PASSWORD`, for CI.

The Export window asks for a password once and offers to remember it on this
machine.

**With `release = false`, the export signs with a debug key it generates on
first use**, as Defold does. *(Amended 2026-10-02, D465: the key is Android's
own, `~/.android/debug.keystore` with its standard alias and passwords, made
there when the machine has none -- as Unity signs a debug build -- and
`[export.android] debug_keystore` names another. It was first kept under the
engine's per-user folder, whose name comes from the brand: a package without
its brand file signed with one key and the repository with another.)* The first APK takes no setup. A **Create
keystore...** button makes a release key: alias, validity, a name for the
certificate, and a password. It warns once, plainly, that losing the file means
never updating the app on the store again.

### 4. The editor has one Export window

Open it with **File > Export...** (Ctrl+Shift+B), or with an Export button on
the toolbar beside Play.

The layout:

- **Left:** three target cards, each with the platform's mark and its state.
  - Windows is always ready.
  - Linux is always ready: the player is prebuilt, so no Docker is needed.
  - Android shows either "ready", "tools missing (Install)" or "phone
    connected: Galaxy S25".
- **Right:** the selected target's few settings from section 3, with the
  identity fields from section 1 shown read-only and a link to Project
  Settings. Also the output folder, defaulting to `dist/<target>/`.
- **Bottom:** a big **Export** button, and **Export all** for every ready
  target.

While an export runs:

- a progress bar moves through named steps (checking the project, packing
  content, compiling shaders, copying the player, stamping the icon, signing),
  each ticking over as it finishes;
- the editor stays usable, because the export runs as a worker process.

When it finishes, a result card shows:

- the size, and the time it took;
- **Open folder**;
- **Run it**, on desktop targets;
- **Install on phone**, when an Android device is connected over adb. It
  installs and launches, so the game appears on the phone in seconds.

A failed step turns red and shows its message in words, not a log dump. A
**Show log** link opens the full log in the Console.

This is the fun part, and it is also the practical part. The shortest path from
a scene to a game on somebody's phone is two clicks: **Export**, then **Install
on phone**.

A short **recent exports** list under the button remembers each export's
target, version, time and size. Clicking one opens its folder.

### 5. The editor and the CLI are one pipeline

The Export window runs `luaug build` and reads its progress. It does not
re-implement it.

- `luaug build --target=windows|linux|android` is the only exporter. The old
  `win64` stays as an alias.
- `--progress=json` makes it write one JSON line per step to stdout:
  `{"step":"pack","state":"done","ms":812}`. The editor draws its progress bar
  from those lines. A human at a terminal sees the same steps as plain lines.
- The CLI's targets are the editor's targets, so a CI script and the button
  make the same bytes.

### 6. Every target is exported from any desktop, with no SDK for desktop targets

The editor distribution (ADR 0054) carries a **prebuilt player per target**.
This is what Godot calls export templates, shipped in the box rather than
downloaded:

```
player/windows-x64/luaug-host.exe + content/
player/linux-x64/luaug-host        + content/
player/android-arm64/libmain.so    + content/ + gradle/ (the project template)
```

- **Windows and Linux** exports are a copy, a rename, a stamp and an archive.
  They work from any desktop, and need no compiler and no Docker. The Linux
  player is built by `scripts/package.ps1` in the Tier-2 container that
  already builds it for the gate.
- **Android** uses the prebuilt `libmain.so`. The author never needs the NDK.
  The APK is still assembled by Gradle from the shipped template, so the export
  needs a JDK, the Android SDK and Gradle.
  - The Export window detects them.
  - When they are missing, **Install Android tools** downloads the pinned JDK
    17, SDK platform 35, build-tools and Gradle into the user's LuauG folder:
    `scripts/install-android.ps1`'s subset, without the NDK. It shows progress
    as it goes.
  - Assembling the APK without Gradle, by patching a prebuilt template APK as
    Godot does, is recorded as a later option, not taken here. It means
    rewriting a binary manifest and signing in our own code. Gradle is what
    already works.

### 7. What each target produces

| Target | Output | Notes |
|---|---|---|
| Windows | `dist/windows/<Executable>.exe`, `content/`, `game/`, and a `.zip` beside it | A `WIN32` subsystem player, so no console window. The icon and a VERSIONINFO resource (product name, company, version, description) are stamped in; `iconpatch` gains a `version` command. |
| Linux | `dist/linux/<executable>`, `content/`, `game/`, `<executable>.desktop`, `icon.png`, packed as a `.tar.gz` | The execute bit is set in the archive, so it survives being made on Windows. AppImage is later. |
| Android | `dist/android/<name>-<version>.apk` | Debug-signed or release-signed per section 3. `versionName` is the game's version, `versionCode` the configured one, and the icon and orientation are the project's. AAB, for the store, is a checkbox added in a later stage. |

On every target, `game/` carries:

- `luaug.toml` and `.luaurc`;
- the source, and the content pack;
- the partition cache from `.luaug/`.

It does **not** carry editor files: layouts, `editor.json`, the import and
check caches.

## Consequences

- **The Android debug key is per machine** and generated on first use. Two
  machines that export the same project make APKs that do not update each
  other. That is the nature of a debug key, and the Android panel says so in
  one line.
- **The editor distribution grows** by a Linux player, an Android player and a
  Gradle template: tens of megabytes. It stays one download.
- **Export from Linux and macOS** follows once those distributions exist. The
  pipeline is `luaug build` in Luau plus prebuilt players, so no part of it is
  Windows-only except `iconpatch`. Section 2's `.ico` writer and a
  VERSIONINFO writer are portable code.
- **Out of scope, on purpose:**
  - iOS and macOS targets. iOS is not an open phase (R15).
  - Web.
  - Store upload, including itch.io's `butler` and Play Console.
  - A splash screen.
  - Code signing of the Windows `.exe`.

  Each is a small addition to this shape, not a redesign of it.

### Rejected

- **A separate presets file, as Godot's `export_presets.cfg`.** A second file
  is a second place to look. `luaug.toml` already holds everything else about
  the project, and `[export.*]` sections read as what they are.
- **Many presets per target from the start**, such as a demo build and a full
  build. Unity's profiles and Godot's presets both support it. One
  configuration per target is what a first game needs, and a `--profile=` on
  `[export.<target>.<profile>]` can come later without changing anything here.
- **Downloading players on demand.** It is smaller, but it makes the first
  export depend on the network and on a release server. The engine is one
  folder, and so are its players.
- **Doing the export inside the editor process.** A crash in a packer would
  take the author's unsaved scene with it, and the CLI would drift from the
  button.

## Amended 2026-09-27: the orientation is the start scene's

`[export.android] orientation` is gone. The manifest's `screenOrientation` is
the start scene's `UIService.ScreenOrientation`, read at export: the manifest
holds the phone before any scene has loaded, the scene once one has, and two
settings for one question turned the screen at launch whenever they differed.
The export window shows the value and does not set it.
