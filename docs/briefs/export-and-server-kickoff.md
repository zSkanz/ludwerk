# Scenes, the server/client split, and export: the kickoff and the ledger

The owner, on 2026-09-26, asked for an area in the editor that exports a game
for Windows, Linux and Android. Name, icon, executable and the Android app
should all be set there. It should be practical, and fun to use rather than
complicated. The decision is
[ADR 0104](../decisions/0104-a-game-is-exported-from-one-window-for-windows-linux-and-android.md).

The same day he settled two things the export depends on:

- **Where code lives, and what each side's package carries**:
  [ADR 0105](../decisions/0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md).
  `ServerScriptService` and `ClientScriptService` in each scene,
  `GlobalScriptService` (`Server`, `Client`, `Shared`) for the whole game,
  `ScriptService` retired; server code runs only on the authority, client code
  never on a dedicated server, and in `dedicated` mode each package carries
  only its own side.
- **Scenes at run time**:
  [ADR 0106](../decisions/0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md).
  A scene is a complete place; `SceneService:LoadScene` changes it, and in a
  match every client follows the authority; `NetworkService:Join`, `Host` and
  `Disconnect` from a script; attributes replicate; the editor plays a match.

**Read all three ADRs before this file.** This file is the order of work and
where each piece stands. Stages S1 to S5 (scripts, scenes, network) come before
the export's build targets, because the build has to know what each side leaves
out and how a dedicated client starts.

**Already done before this ledger started** (2026-09-26, commits `920ca3b4`
and `7cd88431`): the player host builds for Android arm64 as `libmain.so`;
`platforms/android/player` and `scripts/android-player.ps1` package and install
a project; touch input reaches Luau; `UIService.ScreenOrientation` and
`Camera.ViewportSize` exist; `scripts/localgate.ps1` has an `android` stage;
`scripts/install-android.ps1` installs the tools. Stage 2's Android item builds
on that pipeline rather than starting it.

The idea in brief:

- **Identity once.** A game's identity (name, id, version, company, one PNG
  icon) is set once, in `luaug.toml` and on Project Settings' new App page.
- **Details per target.** Each target's few details go under
  `[export.<target>]`. Passwords never go there.
- **One exporter.** `luaug build` is the only exporter, and it gains Linux and
  Android and a JSON progress stream.
- **One window.** The editor's **File > Export...** window has three target
  cards, a big Export button, live steps, and a result card with **Open
  folder**, **Run it** and **Install on phone**.
- **Players in the box.** The editor distribution carries a prebuilt player
  per target, so desktop exports need no SDK and Android needs no NDK.
- **Each side gets its own code.** `[export] multiplayer = none | host |
  dedicated`; in `dedicated` mode a client package per target without any
  server code, plus `windows-server` and `linux-server` without any client
  code.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- `scripts/localgate.ps1` is green on all stages, Linux included, before every
  push. Then CI is read. Never write to a red `main`.
- **`luaug build` with no new flags keeps producing what it does today** for
  `win64`, apart from the defects fixed in stage 0. `tests/packaging` stays
  green throughout, and grows with each target.
- **No determinism trace moves.** Nothing here touches the simulation.
- **No secret reaches a file in the project.** A test writes a keystore
  password through the Export flow and asserts that it is in no file under the
  project root.
- R3: every new string in the window, the CLI and its errors is an i18n key.
- R7: nothing in code or UI names another engine. The survey lives in the ADR's
  Context and stays there.
- **Solo and host behave exactly as today** through all of stages S1 to S5:
  every migrated example plays as it did, and no determinism trace moves
  except where a stage says why (S4 changes the protocol, not the simulation).
- **A game with one scene is the same game.** Nothing in S2 or S3 asks it to
  change.

## Stage 0 — the defects the survey found

- [x] The player is a `WIN32` executable on Windows, so a double-clicked game
      opens no console window. It still logs to a file. Test it: build, launch
      detached, and assert no console window. If the check cannot be
      automated, check it by hand and record that.
- [x] `luaug build` stops copying the editor's files from `.luaug/`
      (`editor-layout*.ini`, `editor.json`, `import/`, `check/`). It keeps the
      pack, its manifest and `partition/`. The Android stager uses the same
      list: one list, used by both.
- [x] `[window] width` / `height` are read as an alias of `size`. `size` wins
      when both are given.
- [x] `iconpatch verify` in `luaug build` passes `--not <engine icon>`, as the
      packaging test does.
- [x] `[project] icon` in the dev host: the window shows it (the docs already
      say it does), or the docs stop saying so. Prefer making it true.

## Stage S0 — a scene saves its services' settings (ADR 0106 §1, D203)

- [x] A service is written when a setting differs from the engine's, or it has
      an attribute, not only when something is under it; `clearScene` puts
      every service's settings and attributes back to the engine's. The IDL's
      `Transient` reaches `PropertyDesc::transient` and the scene writer skips
      it; `DebugService.OverlayVisible`, `InputService.PointerLocked` and
      `PointerVisible` are transient. Test: "a service's settings are saved
      with nothing under it, and a new scene puts them back".

## Stage S1 — three script services (ADR 0105)

- [x] IDL: `ServerScriptService`, `ClientScriptService` and
      `GlobalScriptService` (`Tags = Service, NotCreatable`); the three fixed
      folders `Server`, `Client`, `Shared` under the global one, made by the
      engine and refused rename, reparent and destroy with keyed errors. Icons
      in the theme; the Explorer shows `GlobalScriptService` above the scene's
      services, set apart.
- [x] **The run rule**, in the one place scripts are started (the runtime's
      entry-script walk): `ServerScriptService` and
      `GlobalScriptService.Server` start only while `Authority` is true;
      `ClientScriptService` and `GlobalScriptService.Client` never on
      `--serve`; anywhere else, everywhere, as today.
- [x] **Never replicated**: the two server containers and the two client ones
      are not sent; a replica empties its own server containers on join, as it
      does `ServerStorage` -- reuse that path. What the authority writes to
      `GlobalScriptService` itself and under `Shared` DOES replicate (after S4
      for attributes).
- [x] **Files**: `src/server/`, `src/client/`, `src/shared/`,
      `src/scenes/<scene>/server/` and `src/scenes/<scene>/client/` mount as in
      ADR 0105 §3; hot reload covers them; a script made in the editor inside
      any of the three services is written as a file there. Non-code content of
      `GlobalScriptService` saves to `content/global.json`.
- [x] **`ScriptService` retired**: a scene's `storage.ScriptService` is read
      into its `ClientScriptService`; `GetService("ScriptService")` raises a
      keyed error naming the two new homes; `src/scripts/` is read as
      `src/client/` with a warning, for one release.
- [x] **Tests**: a dedicated server runs no client script; a joined client runs
      -- and never receives -- no server script; solo and host run both;
      `Shared` modules require from both sides; every existing trace's world
      hash is unchanged. Conformance where the headless runner can express it,
      `tests/serve` / two-world tests where it takes two processes.
- [x] **Migrate** every example and template (`src/scripts` to `src/client`,
      and `examples/15-multiplayer`'s rules to `src/server` or the scene's
      `ServerScriptService`), `luaug new`'s templates, and the language
      service's knowledge of where scripts live.
- [x] Docs: `concepts/scripts.md` (the three services, with ADR 0105's run
      table), the multiplayer guide's "one question" section and the services
      table, `project-anatomy.md`, and CHANGELOG under *Changed -- BREAKING*
      with the moves.

## Stage S2 — scenes at run time (ADR 0106 §1-2)

- [x] `SceneService` in the IDL: `CurrentScene`, `LoadScene(path, data?)`,
      `GetLoadData()`, `SceneLoading`, `SceneLoaded`. The change happens at the
      safe point between ticks; the editor's scene opening and `LoadScene` share
      one path (clear to the engine's settings, read the file, mount the
      scene's script folders, start its scripts).
- [x] `GlobalScriptService` and its scripts survive the change; the old scene's
      threads stop; `ScreenGui.KeepOnSceneLoad` carries a screen across.
- [x] **In a match**: only the authority may `LoadScene` (a client's call is a
      keyed error); a new wire message tells replicas the path and the data;
      each loads it from its own package and the world arrives by replication;
      a late joiner loads the server's current scene. `Player`s and their
      attributes survive; characters do not.
- [x] Tests: two scenes with different `Lighting` and UI, a global script's
      variable survives three changes, `GetLoadData` round-trips, a replica
      follows the host's change without reconnecting, a replica's own
      `LoadScene` is refused. An example (`examples/26-scenes` or the next
      free number): menu, lobby, arena, solo and hosted. **`examples/24-scenes`
      is the menu, the lobby and the arena.**
- [x] Docs: a manual page for scenes at run time; `api-design.md`.

## Stage S3 — the network from a script (ADR 0106 §3)

- [x] `NetworkService:Join(address?)`, `Host(port?)`, `Disconnect()`, `State`
      (`Enum.NetworkState`, appended at the END of the enum list), `Connected`,
      `JoinFailed`, `Disconnected`, with keyed reasons. `[network] server` in
      `luaug.toml` is `Join()`'s default.
- [x] `--host`, `--serve`, `--join=` become the same calls before the first
      tick. `--serve` cannot `Join`/`Host`/`Disconnect`.
- [x] Joining unloads this machine's scene and loads the server's; `Authority`
      changes only at that safe point, and server code stops (join) or starts
      fresh (leave) per ADR 0105 §2.
- [x] Tests: solo menu -> `Join` -> in the host's scene -> host stops ->
      `Disconnected` -> solo again with server code restarted; `JoinFailed`
      against a closed port; `Host` from solo lets a second process join.
- [x] Docs: the multiplayer guide's opening becomes the script API, with the
      flags as the shortcut; the note that `Authority` can change.

## Stage S4 — attributes replicate (ADR 0106 §4)

- [x] Attributes on every replicated instance, `Player`, `Team`,
      `GlobalScriptService` and `Shared` travel authority -> replica, as a
      diffed field like the rest of replication; `AttributeChanged` and
      `GetAttributeChangedSignal` fire on the replica.
- [x] Protocol version bumped; `docs/protocol/wire.md` regenerated; the
      CHANGELOG says a peer must match.
- [x] Tests: set on the authority, seen on the replica within the snapshot
      interval; removed (nil) likewise; interest-limited like the instance.
      A lobby in the S2 example uses `Ready` attributes and no `RemoteEvent`
      for it.

## Stage S5 — the editor plays a match (ADR 0106 §5)

- [x] Beside Play: a player count (1 to 4) and a *Dedicated server* toggle.
      Play then starts a host (or `--serve`) and N clients of the project as
      separate processes, tiled; each one's log in the Output panel under its
      own name; Stop ends them all. Plain Play stays solo in the viewport.
- [x] Test the process orchestration headless (start, names, stop all).
- [>] The tiling, checked by hand on a real screen and recorded here.
      **Not done by the agent**: it has no screen to look at. The tiles are a
      pure function under test; what the windows do with them on a desktop is
      the owner's to see.

## Stage 1 — identity, and one icon in

- [x] New keys: `[project] company`, `[window] fullscreen` and
      `[window] resizable`. The desktop player honours fullscreen and
      resizable. Update `docs/api-design.md`'s `luaug.toml` section and
      `project-anatomy.md`.
- [x] `[project] version` is validated (semver `X.Y.Z`) and carried through
      `ProjectConfig`.
- [x] `assetc icon <png> --out <dir> --targets windows,linux,android`:
  - a multi-size `.ico` (16 to 256);
  - a 512 px PNG;
  - Android `mipmap-*` legacy icons;
  - an adaptive icon: foreground, plus a background colour as a colour
    resource.

  It uses the vendored PNG code and the engine's own resampler, with no new
  dependency. It is tested byte-for-byte against checked-in expected sizes and
  dimensions.
- [x] A too-small PNG (under 256 px) exports with a warning. A non-square one
      is refused with a keyed error that names both dimensions.

## Stage 2 — `luaug build` for three targets

- [x] `--target=windows|linux|android`, with `win64` as an alias of
      `windows`.
- [x] `--progress=json`: one line per step, in the shape
      `{"step":…, "state":"start|done|fail", "ms":…, "message":…}`. Without
      it, the same steps go out as plain lines. The step names are a fixed,
      documented list.
- [x] Finding the player per target: `installRoot()/player/<target>/`, then
      the dev build dirs, then `LUAUG_PLAYER_HOST_<TARGET>`.
- [x] **Windows:** `[export.windows] executable`; the icon from stage 1; a
      `.zip` beside the folder.
- [x] **Windows:** `iconpatch version <exe> --product --company --version
      --description` writes a VERSIONINFO resource. A test reads it back.
- [x] **Linux:** copy the player, set the execute bit, and write
      `<exe>.desktop` and `icon.png`, into a `.tar.gz` whose tar headers carry
      mode 0755. Make it from Windows. A test in the Linux gate container
      unpacks it, runs the game headless, and runs one tick.
- [x] **Android (debug):** `scripts/android-player.ps1`'s pipeline moves into
      `luaug build`, with these changes:
  - the prebuilt `libmain.so`, not an NDK build;
  - Gradle on the shipped template;
  - the package, version name and code, orientation and icons from
    `luaug.toml`.

  The whole-file regex reading of `luaug.toml` goes away. The script becomes
  a thin wrapper, or goes. The output is `dist/android/<name>-<version>.apk`.
- [x] Android debug signing: a key generated once per machine under the
      user's LuauG folder, and reused after that.
- [x] **`[export] multiplayer = none | host | dedicated`** (default `none`).
      In `none` and `host`, one package per target with everything, as today.
- [x] **`dedicated`, client packages**: every scene in the pack WITHOUT its
      `ServerScriptService` and `ServerStorage` subtrees, `global.json` without
      `Server`, and no `src/server/` or `src/scenes/*/server/`. The client
      starts in its first scene and joins when its scripts call
      `NetworkService:Join()` (default address `[network] server`).
- [x] **`dedicated`, server packages** (`--target=windows-server|linux-server`):
      every scene WITHOUT its `ClientScriptService`, `global.json` without
      `Client`, and no `src/client/` or `src/scenes/*/client/`. Starts as
      `--serve` with no arguments (the exported `luaug.toml` carries the role).
      No icon step. A test runs the Linux server headless in the gate container
      for a few ticks.
- [x] **The sentinel test**: a project with a distinct string in a server
      script, a client script, a `ServerStorage` value and each scene's two
      services; export both sides; assert each side's strings are nowhere in
      the other's output, pack included.

## Stage 3 — Android release signing

- [x] `[export.android] release`, `keystore` and `key_alias`, with passwords
      from the per-user store or the `LUAUG_ANDROID_*` environment variables
      only.
- [x] `luaug keystore new --alias --out`, which the editor's **Create
      keystore...** button calls. It warns once about losing the key.
- [x] `version_code`: `--bump-version-code` increments it in `luaug.toml`,
      written in place so that comments are kept.
- [x] A release APK is verified with `apksigner verify` in the test.

## Stage 4 — the players in the distribution

- [x] `scripts/package.ps1` and `tools/repo/package.luau` lay out
      `player/windows-x64/`, `player/linux-x64/` (built in the Tier-2
      container, copied out of its volume) and `player/android-arm64/`
      (`libmain.so`, the SPIR-V content, and the Gradle template).
- [x] `tests/installed` exports each target from the packaged folder, outside
      the repository, which is the proof that matters.
- [x] **Install Android tools** (`luaug android install-tools`): JDK 17, SDK
      platform 35, build-tools and Gradle, with no NDK, into the user's LuauG
      folder, with progress lines. It is idempotent, and checked by version.

## Stage 5 — the editor

- [x] **Project Settings > App**: name, id, version, company, icon (a picker
      plus the preview tiles: desktop, taskbar, phone home screen with the
      label), window title, size, fullscreen and resizable. Apply writes
      in place, as it does today.
- [x] **File > Export...** (Ctrl+Shift+B), and an Export button on the toolbar
      beside Play.
  - [x] A **Multiplayer** selector above the cards: *Single player* / *Players
        host* / *Dedicated server* (`[export] multiplayer`), with the server
        address field in the dedicated mode.
  - [x] Three target cards with state: ready / tools missing (Install) /
        phone connected (its model name from `adb devices -l`). In the
        dedicated mode, two more: **Windows server** and **Linux server**, and
        the client cards say "client -- joins <address>".
  - [x] The selected target's settings from ADR 0104 §3. Identity is shown
        read-only, with a link to Project Settings. The output folder has a
        picker.
  - [x] Android: a signing row. It reads "Debug key" or "Release:
        keys/release.keystore". There is a **Create keystore...** button, and
        a password field that offers to remember the password on this machine.
  - [x] **Export** and **Export all**. The export runs `luaug build
        --progress=json` as a worker process, and the editor stays usable.
  - [x] A live step list. Each step ticks when it finishes and turns red when
        it fails, with its message in words and a **Show log** link.
  - [x] A result card: size, time, **Open folder**, **Run it** (desktop) and
        **Install on phone** (adb install and launch, with its own progress).
        A small celebration on success, such as a brief check-mark animation,
        is welcome if it takes a few lines of ImGui. Nothing garish.
  - [x] Recent exports: the last ten per project (target, version, time and
        size), kept in the per-user editor state. Clicking one opens its
        folder.
- [x] The icons for the three targets and the Export action go into the theme.
      Regenerate `icon_ids.gen.h`.

## Stage 6 — documentation and closing

- [x] `docs/manual/guides/shipping.md` is rewritten around the Export window,
      with a screenshot per target and the CLI equivalents. It covers signing,
      and what losing a keystore means. Update `cli.md`.
- [x] ADR 0045's amendment note, and the ADR index rows for 0104, 0105 and
      0106 (0105's and 0106's rows are written).
- [x] The shipping guide explains the three multiplayer modes and what each
      side's package leaves out, with ADR 0105's services table.
- [x] CHANGELOG entries. PROGRESS.md. `scripts/android-player.ps1` is retired,
      or documented as a wrapper.
- [x] This ledger's Findings section: what the ADR assumed that reality
      corrected.

## Findings

- **S1 moves every determinism trace, at tick 0.** The world hash covers every
  instance -- id, class name, name, parent -- so replacing `ScriptService` with
  three services and adding `GlobalScriptService`'s three folders changes the
  hash before anything simulates. The five traces were re-recorded; each still
  reproduces twice in process and across platforms. "No trace moves" could not
  hold for a stage that changes the service tree, and the stage is where that is
  said.
- **Before S1, a `Script` in `ServerStorage` ran on every machine.**
  `startScripts` walked the whole data model with no container filter at all.
  ADR 0105 keeps "anywhere else runs everywhere", so a script in a storage
  still runs; only the four script containers are gated.
- **A replica's scripts started before it cleared its server storage**:
  `clearForReplica` runs after `boot()` returns. The run rule reads the
  topology, which is set before the first script, so a replica never starts a
  server script even though the clear comes later.
- **The packaging gate runs the `player` build, and only the `shipping` stage
  rebuilds it.** `-Only windows` after a change to how a project mounts ran a
  stale player that still looked for `src/scripts`, and failed on "the packaged
  game did not run its script". The full gate builds `shipping` after
  `windows`, so the first full run after such a change can report the same.
- **A single-file project has no side.** Mounted under `Client` it stopped
  running under `--serve` (`serve_ends_on_match`); it now mounts directly under
  `GlobalScriptService`, outside its folders, and runs everywhere as it did.
- **`src/shared/` was already a require-by-path folder** in two examples and
  several tests. Mounting it as `ModuleScript`s would have evaluated a module
  twice if one script required it by path and another by instance; a path that
  resolves to a mounted `ModuleScript` now goes through the instance's cache.
- **Every new service moves the traces again.** `SceneService` is one more
  instance under the data model, so S2 re-records them as S1 did; nothing about
  the simulation changed.
- **The protocol moves twice, not once.** ADR 0106 expected one version for
  the scene message and attributes together. They land in separate pushes, and
  a push that changes the bytes changes the version (ADR 0100), so the scene
  change is protocol 16 and attributes will be 17.
- **A scene loaded at run time is read whole.** The streaming grid lives in the
  frame loop, not in the host, and the boot scene is the only one partitioned
  into it. `LoadScene` of a scene that needs streaming is a later item; the
  guide says so.
- **A replica applies the scene change inside `receive`**, not at the tick's
  end. The spawns of the new scene's instances come in the same send, right
  after the `SceneChange`, and a replica that changed scene later would clear
  them with the scene they had just arrived in.
- **The network had no owner a test could reach.** The connection lived in
  the frame loop as a bare `IReplication`, so a script's `Join` had nowhere to
  land and two processes were the only way to test one. `NetworkSession` owns
  it now, with the transport as a factory: the engine runs it over ENet and the
  tests run two over the memory transport, in one process.
- **A join from a script does not dial again.** ADR 0085 made a launched
  client redial for ever; a game that joined from its menu wants
  `Disconnected` instead, and to decide what next. `Config::redial` tells the
  two apart, and `--join=` keeps what it did.
- **Leaving puts back the server code from files only.** A replica's server
  code went when it joined -- what the files mounted and what the scene
  carried. `restartServerCode` mounts `src/server/` and the scene's
  `src/scenes/<scene>/server/` again; a script the scene file held under
  `ServerScriptService` comes back when the game loads a scene, which is what
  it does after a leave.
- **Attributes travel as a message, not as a field.** A field is a fixed
  64-byte cell and an instance's attributes are a map of any size, so they go
  as `Attributes` (protocol 17): an owner's whole map, sent when it changes
  against one shadow per owner and once to a replica new to it -- the tilemap's
  shape again. `Player`s and `GlobalScriptService` are owners with no network
  id, named by user id and by a fixed tag.
- **What is under `GlobalScriptService.Shared` does not replicate yet.** Its
  attributes and the service's own do; instances put under `Shared` would need
  the service to be a `Contents` service on the wire, whose index is part of
  every service's network id -- a change of its own, not this stage's.
- **A client's readiness reaches the server as intent, not as an attribute.**
  Attributes go one way. The lobby in `examples/24-scenes` binds R to an
  `InputAction`, the server reads `GetIntent`, and the attribute it writes is
  what every client sees.
- **A match's windows needed three flags the engine did not have.**
  `--window=x,y,w,h` puts a window where the editor tiles it, `--label=` says
  which window it is in its title, and `--log-file=` gives each process its own
  log -- four processes writing one `luaug.log` in one directory would be one
  unreadable file.
- **The processes are SDL's**, as the shader compiler's already were:
  `SDL_Process` with a non-blocking pipe, so the editor reads what each window
  says between frames without a thread per child.
- **The console check is automated, from the PE header.** Whether Windows opens
  a console for a program is its optional header's `Subsystem`, so the
  packaging test reads it out of the packaged executable -- 2, `WINDOWS_GUI` --
  whenever the packaged host is the `player`. A Windows-subsystem player
  started from a terminal attaches to it, so its output is still seen there.
- **`linux` is a macro under GNU C++.** A field named `linux` compiles on MSVC
  and breaks every Clang build in `gnu++20` mode, where it is predefined as 1;
  the icon option is `linuxDesktop`.
- **The resampler is the engine's own**, a box filter over premultiplied
  colour: `stb_image_resize2` is vendored but compiled nowhere, and a page of
  C++ is less than a second library's worth of build.
- **The `player` profile had no networking.** `LUAUG_ENABLE_REPLICATION`
  was on for `dev`, `debug` and `editor` and off for `player`, so every
  exported game was single-player whatever its scripts did -- and a dedicated
  game's client package could not have joined its server. The `player`
  presets and the Android player build now turn it on; ENet was already
  linked into every profile.
- **The partition cache carried the whole project's scripts.** It holds the
  scene as the game mounts it, source and all, and `luaug build` warmed it on
  the project and copied it: the sentinel test found the server's script in a
  dedicated client's `.luaug/partition/`. It is now made on the PACKAGE, after
  the layout, by the host booting the package's own `game/` -- so the step
  order is `check, pack, layout, partition, ...`, not the ledger's.
- **The build tree's newest tool was not the one found.** `findTool` took the
  first preset in a fixed list, `win-msvc-editor` before `win-msvc-dev`, and
  the gate only ever links the editor's host -- so its `assetc` was weeks old
  and had no `icon` command. `findTool` now takes the newest of the build
  trees' copies; the variable still wins outright.
- **The player variable is tried first, not last.** The ledger listed
  `LUAUG_PLAYER_HOST_<TARGET>` after the installation and the build tree; an
  override that loses to whatever is on disk is not one, and it is how the
  Linux gate points the export at the player it has just built.
- **The CLI's TOML reader refused a CRLF line.** "Trailing text after the
  value: \r" -- on any line appended to `luaug.toml` from Windows. The reader
  takes either line ending now, as the format allows.
- **Gradle runs through `java`, not `gradle.bat`.** A batch file on Windows is
  `cmd /c`, whose quoting a game name or a path with a space in it breaks;
  `java -classpath gradle-gradle-cli-main-*.jar org.gradle.launcher.GradleMain`
  is what the batch file runs anyway. `apksigner` the same way, as a jar.
- **`--` in an XML comment is not XML.** Naming `--target=android` inside the
  Android manifest's header comment failed the manifest merger; the comment
  says it in words.
- **The archives are `assetc`'s.** `assetc archive` writes the `.zip` and the
  `.tar.gz` (ustar, mode per entry, no timestamps, so the same folder makes the
  same bytes) with the deflate `stb_image_write` already compiles, and the
  asset tests inflate them back with `stb_image`'s. No archiver, no new
  dependency, the same bytes from any desktop.
- **VERSIONINFO is built as bytes, not compiled from an `.rc`**: the player is
  one prebuilt binary every game copies, so its file properties are stamped on
  the copy as the icon is. The writer and its reader live in `iconpatch`'s
  library and are tested on both tiers; `iconpatch info` prints what an
  executable carries, which is how the packaging test reads it back.
- **A Windows target built on Linux keeps the engine's icon and no version.**
  `iconpatch` is `UpdateResourceW`, so on Linux the export says it was not
  found and ships without them; a PE resource writer of our own is later.
- **`luaug build`'s own folder is `dist/windows` now**, not `dist/win64`:
  `win64` still works as the name of the target, and resolves to `windows`.
- **No password is on a command line, anywhere.** `keytool` takes
  `-storepass:env`, and Gradle reads `ORG_GRADLE_PROJECT_luaug.*Password` from
  its environment as project properties, so the export hands both over in the
  child's environment -- a password in an argument is readable by every other
  process on the machine. `luaug keystore new` takes its own from
  `LUAUG_ANDROID_KEYSTORE_PASSWORD` for the same reason.
- **The per-user store is `android-keys.json` in SDL's preference folder**
  (`%APPDATA%\LuauG\LuauG`, `~/.local/share/LuauG/LuauG`), which is
  `platform::paths().userDir`: beside the editor's preferences, as the ADR
  says, and keyed by the keystore's absolute path. The debug key and the
  toolchains stay in the user's LuauG folder (`%LOCALAPPDATA%\LuauG`).
- **A keystore is never replaced.** `keystore new` refuses an existing file:
  a key overwritten is every update of every game signed with it lost.
- **The gate's Android fallback build root was `LuauG<backspace>uild`**: a
  `\b` a heredoc had turned into a control character, so a run with no
  `LUAUG_BUILD_ROOT` set built the Android player somewhere nothing looked.
- **The Gradle template read the repository around it.** Its pins came from
  `cmake/toolchains/android.cmake` and `CMakeLists.txt`, and SDL's Java from
  `third_party/` -- none of which an installation has. The packager now writes
  `pins.properties` and `sdl-java/` beside the template, and the template
  reads those when they are there; `ndkVersion` is named only when that NDK
  is installed, because an installation's toolchain has none.
- **An unstripped Android player is 160 MB.** Gradle strips `libmain.so`
  only with the NDK installed, so an APK from an installation would have
  carried every symbol. `scripts/package.ps1` strips it with the NDK's
  `llvm-strip` on the way into the folder (16 MB), and the Linux player with
  `strip` in the container.
- **`cmd /c` cannot run a quoted path the runtime quoted.** Lute passes each
  argument with backslash-escaped quotes, which `cmd` does not read, so
  `sdkmanager.bat` "was not recognised". `install-tools` writes the two
  `sdkmanager` lines into a script file and runs that.
- **The JDK is pinned by exact build**, `17.0.20.1+1` through Adoptium's
  version endpoint, where `scripts/install-android.ps1` asked for "latest 17".
- **A clean machine was simulated, not borrowed**: `install-tools` into a
  scratch `LOCALAPPDATA` with no build root, no `JAVA_HOME` and no SDK on the
  environment, then an APK exported with only what it installed -- no NDK --
  and signed with a debug key made in that folder.
- **The Export window is the CLI's, down to its cards.** Which targets are
  ready is `luaug build --status`, the same searches an export makes; the steps
  are the `--progress=json` lines; what was made is a final `result` line. The
  editor's side (`export_runner.h`) is parsing and state with no ImGui, and is
  what the tests drive; `debug_overlay.cpp` only draws it.
- **A child process needed a working directory and an environment.** `lute`
  in a source tree is rokit's shim, which finds the pinned interpreter from
  the directory it runs in, and a keystore password must not be an argument.
  `ChildProcess` and `runProcess` take both now, through SDL's own properties.
- **The target marks are drawings of what each runs on**, a monitor, a
  terminal and a phone, not the platforms' logos, which are other people's
  marks.
- **The window was not looked at in this session.** Its model is tested, its
  exports are the CLI's tested ones, and it compiles clean on both tiers; the
  pixels of it are the owner's to see first. A capture of the desktop is not a
  way to look at the editor -- it captures whatever else is on the screen.
- **The shipping guide has no screenshots.** The ledger asked for one per
  target; nothing here can take a picture of the editor (the ImGui shell does
  not render headlessly, and a capture of the desktop captures whatever else is
  on it), so the guide describes the window and gives every CLI equivalent.
  The pictures are the owner's to add when he looks at it.
- **`scripts/android-player.ps1` stays, as a wrapper.** It builds the arm64
  player from this repository -- what an installation ships prebuilt -- and
  hands over to `luaug build --target=android`, which is the exporter.
- **Undoing the creation of a file-backed script leaves the file.** The editor
  writes the file when the script is made; undo takes the instance out of the
  world, and the next open mounts the file again. Deleting a file from the
  editor is its own feature, not this stage's.
