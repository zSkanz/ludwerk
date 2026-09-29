# Changelog

Every release of Ludwerk. The format is [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the versions are [semantic](https://semver.org/spec/v2.0.0.html).

**The public surface is `api/api-dump.json`**, which is generated from the IDL
and diff-checked by CI. Anything that changes it is an entry here; anything that
does not is engine work and belongs in the git history rather than in this file.

## [Unreleased]

**The first version as Ludwerk, 0.0.1** (ADR 0109). The engine's name, the command and a
project's files change, with no compatibility for the old ones: rename a project made before
by hand -- `luaug.toml` to `project.toml`, `.luaug/` to `.engine/` (or delete it; it is a cache),
`require("@luaug/...")` to `require("@engine/...")`, a surface shader's
`#include "luaug/surface.hlsli"` and `LUAUG_*` macros to `engine/surface.hlsli` and `ENG_*`,
and any `LUAUG_*` environment variable to `ENG_*`. The command is `ludwerk`.


### Changed -- BREAKING

- **`SceneService.CurrentScene` is a `Scene`, not a path** (ADR 0124). Read
  `SceneService.CurrentScene.Path` where the path was compared.
- **A scene's scripts' close handlers go with the scene** (ADR 0124). A
  `game:BindToClose` registered by a scene's script is dropped when that scene
  closes, without running, and the editor warns; `scene:BindToClose` is the one
  for a level. The editor's Stop now runs the game's close handlers.

- **`luaug build` writes `dist/windows/`**, not `dist/win64/`, and the zip
  beside it. `--target=win64` still works. `scripts/android-player.ps1` is a
  wrapper around `luaug build --target=android`; its APK is
  `dist/android/<name>-<version>.apk` in the project.
- **The `player` profile is built with networking** (`LUAUG_ENABLE_REPLICATION`),
  so an exported game can host, join and serve; before, every exported game was
  single-player whatever its scripts asked for.

- **Code lives in three script services, and `ScriptService` is retired** (ADR
  0105). **Where a script is decides which machine runs it**:
  `ServerScriptService` (the scene's) and `GlobalScriptService.Server` run only
  where the world is decided -- solo, a host, a dedicated server -- and never
  reach a client that joined; `ClientScriptService` and
  `GlobalScriptService.Client` run where a player sits, never on a dedicated
  server; a script anywhere else runs everywhere, as before. Solo and a host run
  both, so a single-player game plays as it did.
  - **The moves.** `src/scripts/` is `src/client/` (read under its old name for
    this release, with a warning). The rules of a multiplayer game that sat
    behind `if NetworkService.Authority` go to `src/server/` or the scene's
    `ServerScriptService`. Modules both sides require go to `src/shared/`, which
    now mounts as `ModuleScript`s under `GlobalScriptService.Shared` -- one
    module whether required by path or by instance. A scene's own code goes in
    `src/scenes/<scene>/server/` and `client/`.
  - `game:GetService("ScriptService")` raises a keyed error naming the two new
    homes. A scene that saved something in `ScriptService` reads it into
    `ClientScriptService`.
  - `GlobalScriptService`'s folders `Server`, `Client` and `Shared` are made by
    the engine and cannot be renamed, moved or destroyed. What is authored under
    it that is not code is saved in `content/global.json`, not in a scene.
  - A script made in the editor inside one of the three services is written as a
    file under `src/`.
  - Every determinism trace was re-recorded: the world hash covers every
    instance, and the data model's services changed. The simulation did not.

### Added

- **Scripts that run at the display's rate** (ADR 0136).
  `RunService:BindToRenderStep(name, priority, fn)` and `UnbindFromRenderStep`,
  with `Enum.RenderPriority`, run every drawn frame by priority before
  `PreRender`; `BasePart`, `Attachment` and `Camera:GetRenderCFrame()` answer
  where a thing is drawn this frame there, and its `CFrame` anywhere else. A
  camera written in a render phase is *presented* -- drawn exactly as written,
  and the simulation's camera from the next tick -- so the simulation and the
  world hash never read a value written between two ticks. `@engine/camera`'s
  rigs turn and follow every frame, reading their `TurnAction` and
  `LookAction` from a render-rate context, and move once a tick where nothing
  draws. No determinism trace moved.
- **Water** (ADR 0118, the game-ready plan's B8). A `Water` is a sea
  (`Ocean`), a lake (`Box`) or a river (`Spline`, along its `WaterPoint`
  children, flowing at `FlowSpeed`); its `WaterWave` children, up to eight, are
  its waves. One function of them and `RunService.SimTime`, in the engine's own
  maths and in the shipped `water` surface shader, so what floats rides the
  surface that is drawn: a simulated part below the water's `Density` floats,
  held up at 27 points through its volume and dragged towards the current, the
  flow and the waves' own motion -- a hull pitches, rolls and rights itself.
  `BasePart.Buoyant` opts a part out; a part a weld drives is carried, not
  floated. `Water:GetHeightAt` and `GetNormalAt` for scripts, and
  `BasePart:ApplyImpulseAtPosition` and `ApplyAngularImpulse` for thrust and
  steering. Protocol 28. Every determinism trace was re-recorded: `Buoyant` is
  a new property in the world hash; a `water` scenario joins them.
  `examples/11-ocean` is rewritten on it -- a boat that is a body, no wave
  function of its own -- and `examples/31-lake-and-river` carries logs down a
  river into a lake.
- **`DragDetector`** (ADR 0126 F1b). A part or a model dragged with the pointer
  or a finger, with no input code: along a line, across a plane or the view,
  turned about an axis or as a trackball, or left to a script, within limits
  measured from where it rested; `DragStart`,
  `DragContinue` and `DragEnd` with the player. The drag travels as rays
  (protocol 26) and the authority moves the part; a physical drag hands the
  part to the player while it is pulled. `examples/30-interactions` has a
  drawer and a lever.
- **A voxel holds two materials and a blend** (ADR 0114, B3).
  `Terrain:PaintBall` takes `{ Mode, Strength, Falloff }`: `Replace`, `Blend`
  over what is there, `Under` it, or `Erase` the paint; the seam between two
  materials is a soft height blend. `.lterrain` version 4 and protocol 27 carry
  the paint; an unpainted terrain is the bytes it was.
- **Terrain tools** (B4): the editor's eyedropper (`Alt`+click), paint masks by
  slope, height and material, *Replace Material*, a noise hill generator, and
  heightmap export as 16-bit PNG or RAW.
- **The ground replicates** (ADR 0135). Terrain and block edits travel to every
  player as whole chunks, and a player joining late receives the ground as it
  is. `Terrain:GrowBall` raises and lowers the ground as the editor's brushes
  do.

- **A scene closes as a game does, and talks to the game by message** (ADR
  0124). The global `scene` is the scene open now; `scene:BindToClose` runs
  when it closes and a `LoadScene` waits for it (`[scene]
  close_grace_seconds`, 5 by default), so a level saves before the next opens.
  `game` and `scene` are mailboxes: `SendMessage(topic, ...)` and
  `BindToMessage(topic, fn)`, deferred, the values copied, and a binding goes
  with its script's scene.
- **`ClickDetector` and `ProximityPrompt`** (ADR 0126). A part is clicked, or a
  prompt near it triggered with a key, a gamepad button or a tap, with no input
  code: the engine casts the pointer once a tick, measures reach from the
  character, draws the prompt with its hold, and fires `MouseClick(player)` or
  `Triggered(player)`. In a match the client fires at once and the server fires
  after checking the player could reach it (protocol 21). The editor draws the
  reach of a selected one; `examples/30-interactions` is a room with a button
  and a door.
- **`ContentProvider:PreloadAsync`** (ADR 0131). Loads a list of content names
  and instances -- every asset an instance names -- before anything shows them,
  and yields until each has arrived; a callback reports each item's
  `Enum.AssetFetchStatus`. `ContentProvider.RequestQueueSize` counts what is
  still on its way. Every determinism trace was re-recorded: a new service is a
  new instance in the world hash.
- **A scene prepared in the background** (ADR 0125).
  `SceneService:LoadSceneAsync(path, { Activate = false })` returns a
  `SceneLoad` at once: the file is read and parsed off the main thread and its
  meshes loaded while the current scene plays, `Progress` fills a bar, `Ready`
  fires, and `Activate()` switches. `examples/24-scenes` prepares its arena so.
- **Foliage** (ADR 0116, the game-ready plan's B6). A `FoliageLayer` under a
  `Terrain` says where things grow -- the materials it grows on, slope, height,
  clumping, spacing, draw distance and seed, and never under a roof -- and its
  `FoliageMesh` children say what, in what share, how large and how much the
  wind moves them. No instance per blade: each tile of ground is grown on the
  job threads as the camera nears it, from the same rules and seed on every
  machine, and a compute pass on the GPU culls the field into indirect draws,
  picking a level of detail and fading it out at its distance. Visual only:
  not saved, replicated or hashed. The Terrain panel has a Foliage section and
  a brush that paints a layer's density by hand; `[render] foliage_density`
  and `foliage_shadow_distance` scale it (lower on Android). A field to the
  horizon costs 1.5 ms at 1080p (`tests/perf/foliage`); `examples/29-meadow`
  shows it, and the flagship's island grows grass. The RHI gained compute
  pipelines, storage buffers and indirect draws for it.
- **Wind** (ADR 0115, the game-ready plan's B5). `Workspace.GlobalWind`,
  `WindGusts` and `WindTurbulence`, and `Workspace:GetWindAt(position)`: one
  function of them and the simulation clock, the same in C++ and HLSL, so a
  script reads the wind the picture shows. Visual only -- it pushes no body and
  stays out of the world hash. Particles drift with it where
  `ParticleEmitter.WindAffectsDrift` is on; surface shaders read
  `SurfaceInputs.Wind` (contract 1.1; the block header is 64 bytes). The wind
  replicates as `Workspace`'s properties (protocol 19). A still world, the
  default, looks as it did.
- **Rules paint a terrain by slope and height** (ADR 0113 §2, the game-ready
  plan's B2). `Terrain:GetRules()` / `SetRules()`: each rule draws a layer
  where the ground is between two slopes and two heights, over the layers it
  names, with an edge as wide and ragged as it says, in order. A new terrain's
  one rule is the automatic rock it always had -- now something a project can
  turn off, retune or follow with a snow line. Rules are drawn, not written;
  `ApplyRules` and the editor's **Apply to Voxels** write them. The same
  evaluation runs on the CPU with the shader's noise, so what the game reads
  under a slope is what is drawn there. The editor's Terrain panel has a
  **Rules** section, live.
- **A terrain's layers are materials, with textures** (ADR 0113, the
  game-ready plan's B1). A voxel's material id is the terrain's layer of that
  number, a material asset; a new terrain's eight are the engine's own --
  grass, sand, rock, snow, mud, sandstone, basalt, ice -- built in, with
  colour, normal, surface and height textures drawn from noise, so id 3 is
  still rock and an old world opens as it was, now textured. `GetLayers` and
  `SetLayers` change the list (saved with the scene; the editor's Paint
  section replaces, adds and removes); layers are sampled triplanar at their
  material's `TileSize`, and a pixel blends the layers of its triangle's
  corners. Materials gain `HeightMap`, `Triplanar` and `BlendSharpness`.
  `RaiseBall` takes a material, and `WriteHeights` one per column. Each
  terrain now draws its own layers, where every terrain used to share the
  first one's palette.
- **An exported game carries bytecode, not source** (ADR 0112, the
  game-ready plan's A3). `ludwerk build` compiles every script in `game/src/`
  with the engine's own compiler -- `init.luau` ships as `init.luauc` -- and
  an error in a player's log still names the script and the line. Bytecode
  from another engine build is refused by a keyed error naming both versions.
  `[export] ship_source = true` (a **Ship source** box in the Export window)
  keeps the source. A build without a compiler now runs a compiled game.
- **An FBX or Collada file with bones brings its skeleton, skin and takes**,
  where it used to import as a static mesh. An `AnimationPlayer` under the
  `MeshPart` plays a take by the name after its `|`. The armature's scale (the
  centimetre conversion) is folded into the bones, the nodes above the
  skeleton cost no palette slots, and a rig past sixty-four bones keeps the
  ones that move a vertex. Projects recompile their meshes on the next open.
- **`SaveService`: a game keeps what outlives a run** (ADR 0111, the
  game-ready plan's A2). `GetSlotAsync(name)` hands back a `SaveSlot` --
  `Get`, `Set`, `Update`, `Remove`, `GetKeys`, `SaveAsync`, `Changed`,
  `Recovered` -- holding strings, numbers, booleans, the geometry and colour
  types, sequences and tables of them, each read back as the type it went in.
  What is set is written by itself, at most once a second, on close and when a
  phone backgrounds the game; a damaged file is read from its backup. Saves go
  to the player's folder named by `[project]` company and name, and to the
  project's `.engine/saves/` from the editor and `ludwerk dev`. `Version` and
  `OnMigrate` bring an older slot up to date. `[save] max_slot_bytes` and
  `max_slots` bound it. The editor has a Saves panel (View > Saves) and the
  CLI `ludwerk saves list|clear`.
- **A mesh dragged from the Content browser becomes a `MeshPart`**: onto the
  viewport it stands where it was dropped, onto a row of the Explorer it goes
  under it, and either way it measures what the mesh measures. A mesh given to
  a `MeshPart` in Properties sizes the part the same way.
- **`https://` in `net.request`, from the platform** (ADR 0063, the game-ready
  plan's A1): WinHTTP on Windows, the system's OpenSSL on Linux (loaded at the
  first request), `NSURLSession` on macOS and the Java stack on Android. The
  certificate is always checked against the device's trust store, with no way
  to turn that off. Redirects are followed for every scheme: at most five,
  never from `https` to `http`, a `303` as a `GET`, and no `Authorization` or
  `Cookie` to another host.
- **Cameras on screens** (ADR 0107, V0 and V1). A `CameraTexture` draws a
  `Camera` into `view://<ViewName>`, and anything that takes a texture shows
  it: an `ImageLabel` on a `SurfaceGui` or a `ScreenGui`, a `Decal`, a
  material's map.
  - Feeds are drawn before the main view, each with its own exposure, shadow
    fit and targets.
  - A budget (`[render] max_views_per_frame`, 4) draws the oldest picture
    first; `UpdateInterval` and `Resolution` set what each costs.
  - `Quality = Simple` leaves out shadows and effects.
  - F3 and the editor's Stats list every view and its cost.
  - A camera texture does not replicate, because a camera does not.
  - `examples/26-security-cameras`: a security office at night, six feeds on a
    monitor wall and one on a tablet, and three items turning in inventory
    slots.
- **`SubWorld`: a scene running beside this one** (ADR 0107, V4). `Load()`
  starts a scene in a world of its own -- its instances, scripts, physics and
  clock -- and draws its camera into a `view://` name.
  - The two talk only through `Send`/`Received` and, inside,
    `SceneService:SendToHost`/`HostMessageReceived`: plain values, delivered at the
    other side's next tick.
  - Its input is only what `SetInputState(action, value)` gives it.
  - It ticks once per tick of the world running it, and its world hash is its
    own. It runs its scene's own code, never the project's global code.
  - At most `[render] max_sub_worlds` (2) run at once, one level deep. It does
    not replicate.
  - `examples/28-arcade`: two playable cabinets.
- **Mirrors and portals** (ADR 0107, V3). `@engine/views` has
  `views.mirror(part)` and `views.portal(a, b)`. Each is a `CameraTexture`
  whose camera is moved every tick, with its picture on the part's front face.
  - The pictures are exact from any angle: the camera looks straight through
    the glass, and the glass is cut out of its picture with the new
    `ImageLabel.ImageRectOffset` and `ImageRectSize` (a negative size reads
    backwards).
  - `Camera.ClipPlane` and `ClipPlaneEnabled` give a camera an oblique near
    plane, so what stands behind the glass is not in the picture.
  - The RHI gains `RasterizerState::depthClip`, on for the world's geometry,
    which the oblique plane needs.
  - `examples/27-mirrors-and-portals`.
- **Instances in the UI** (ADR 0107, V2). A `ViewportFrame` draws the parts and
  models inside it, unsimulated, by its own light (`Ambient`, `LightColor`,
  `LightDirection`), with no sky, so with `BackgroundTransparency = 1` only
  they show.
  - With no `CurrentCamera` it frames its contents itself.
  - It is redrawn only when something inside it, its camera, its light or its
    size changes.
- **The editor, remade after VS Code** (the owner's queue of 2026-09-27, Q2):
  - Dark Modern and Light Modern colours and metrics.
  - A command palette (`Ctrl+Shift+P`, `F1`) holding every menu and toolbar
    action by name, plus "Insert: <class>" and "Color Theme" commands.
  - Quick open (`Ctrl+P`) for scenes, stamps, materials, shaders and
    scripts.
  - A status bar: run state, the open scene and whether it is saved, error
    and warning counts, the selection and the tool. It turns blue while the
    game runs.
  - An activity bar that switches the side bar between Explorer, Content,
    Run and Debug and the world tools, and folds it away (`Ctrl+B`).
  - VS Code's menus (File, Edit, Selection, View, Go, Run, Help), drawn from
    the palette's commands, and one toolbar row in place of the ribbon's
    four tabs.
  - The match's players and dedicated server at the top of Run and Debug
    and in the Run menu.
  - A Console filtered by level and text, with its prompt labelled.
  - Notifications as cards in the bottom-right corner.
  - "Insert Object" first on the Explorer's right-click menu.
  - Close buttons only on the active or hovered tab.
  - The keys VS Code users expect: `F5`, `Shift+F5`, `Ctrl+J`, `` Ctrl+` ``,
    `Ctrl+,`, `Ctrl+Shift+E`/`A`/`D`.

  - A Welcome page (Help > Welcome).
  - Search on the script colours and shortcuts pages of Preferences, and
    Preferences: Keyboard Shortcuts in the palette.
  - One heading style in Properties.

  A layout saved before this is rebuilt once, to the new arrangement.
  `--editor-drive=FILE` drives the editor from a script of input events, so
  every state of it can be photographed with nobody at the machine.

- **`UIGradient` and `UIStroke`** (ADR 0110). A gradient colours and fades the
  element it is under -- background, picture and text -- along a
  `ColorSequence` and a `NumberSequence`: **linear, radial or conical**, rotated
  and offset, with a `Scale` and **Clamp, Repeat or Mirror** tiling. A stroke
  outlines an element's **border** -- **outer, centred or inner**, with an
  offset, round, bevel or miter corners, and several per element ordered by
  `ZIndex` -- or a text object's **text**, hollow lettering included; its
  thickness is pixels or a fraction of the element (of the font size, on text),
  and a `UIGradient` under it colours it. Rich text takes
  `<stroke color thickness transparency joins sizing>`. Both work in a
  `SurfaceGui` and a `BillboardGui`. `examples/25-gradients-and-strokes` shows
  every one; `docs/manual/ui/gradients-and-strokes.md` explains them.
- **`ColorSequence`, `NumberSequence` and their keypoints** (ADR 0110): two to
  twenty stops from time 0 to time 1, immutable, compared by value. Properties
  and attributes hold them, a scene saves them, and attributes replicate them
  (protocol 18). A sequence in the Properties panel is a bar with a handle per
  stop.
- **`imgprobe`** beside `imgcmp`: asserts the colour at named points of a
  screenshot, which is how `ui_appearance_gate` checks eighteen claims about
  gradients and strokes without a golden image.
- **A game's identity, set once** (ADR 0104): `[project] company`, a validated
  `[project] version` (`X.Y.Z`), `[window] fullscreen` and `resizable`, and
  `[window] width` / `height` read as `size`. A PNG `[project] icon` is what the
  window wears while the game is being made. `assetc icon <png> --out <dir>`
  makes every platform's icons from that one picture: a multi-size `.ico`, a
  512-pixel PNG, and Android's launcher and adaptive icons.
- **A packaged game opens no console window** on Windows: the `player` and
  `shipping` hosts are Windows-subsystem programs, and one started from a
  terminal still prints there. `luaug build` leaves the editor's own files in
  `.luaug/` out of the game.
- **`luaug build` exports for Windows, Linux and Android** (ADR 0104):
  `--target=windows|linux|android` (`win64` is still `windows`), from any
  desktop that has the target's player. Windows: the folder, the game's icon and
  a VERSIONINFO (product, company, version) stamped into the executable, and a
  `.zip` beside it. Linux: the folder with a `.desktop` entry and a 512 px icon,
  and a `.tar.gz` whose headers carry the execute bit wherever it was made.
  Android: `dist/android/<name>-<version>.apk` from the prebuilt `libmain.so`
  and the shipped Gradle template -- no NDK -- with the package, version code,
  orientation and launcher icons from `[export.android]`, signed with a debug
  key made once per machine and checked by `apksigner`.
  `--progress=json` writes one line per step for the editor.
- **A dedicated game exports as two packages** (ADR 0105): `[export]
  multiplayer = "dedicated"` makes `windows`/`linux`/`android` its clients and
  `windows-server`/`linux-server` its server, and neither carries the other's
  scripts, scene services, storage or `global.json` folder -- pack and
  partition cache included. A server package starts serving with no arguments:
  its `luaug.toml` says `[network] role = "server"`.
- `assetc archive` writes a `.zip` or `.tar.gz` of a folder; `iconpatch
  version` stamps a VERSIONINFO and `iconpatch info` prints one.
- **Android release signing** (ADR 0104 §3): `[export.android] release = true`,
  `keystore` and `key_alias` sign the APK with the project's own key, its
  passwords from `LUAUG_ANDROID_KEYSTORE_PASSWORD` / `LUAUG_ANDROID_KEY_PASSWORD`
  or the per-user store -- never from `luaug.toml`. `luaug keystore new --alias
  --out [--name] [--validity] [--remember]` makes one and refuses to replace
  one. `luaug build --bump-version-code` raises `version_code` in
  `luaug.toml`, in place.
- **An installation exports every target** (ADR 0104 §6): the editor's folder
  carries `player/windows-x64/`, `player/linux-x64/` and `player/android-arm64/`
  and the Android Gradle template, so Linux and Android exports need nothing
  but the folder. `luaug android install-tools` installs a pinned JDK 17, the
  Android SDK (platform 35, build-tools 35.0.0, platform-tools) and Gradle 8.12
  into the user's LuauG folder -- no NDK -- and a second run installs only what
  is missing. `luaug build --status` says, as JSON, which targets this machine
  can export.
- **The editor exports** (ADR 0104 §4): **File > Export...** (Ctrl+Shift+B)
  and an Export button beside Play open a window with a card per target --
  ready, tools missing (with **Install Android tools**), or the phone on adb by
  model -- the selected target's settings written into `luaug.toml` as they
  change, Android signing with **Create keystore...** and a password it can
  remember on this machine, and a multiplayer selector that adds the two server
  cards. **Export** and **Export all** run `luaug build` as a worker process
  with a live step list, then a result card with the size and time, **Open
  folder**, **Run it** and **Install on phone**; the last ten exports per
  project are kept. Project Settings gains the App fields -- version, company,
  icon with desktop, taskbar and phone previews, fullscreen and resizable.

- **The editor plays a match** (ADR 0106): beside Play, a player count (1 to 4)
  and a *Dedicated server* box. With more than one player, Play saves the scene
  and starts a host (or a server) and its clients as separate windows, tiled
  on the screen, each one's output in the Output panel under its own name;
  Stop closes them all. The engine gains `--window=x,y,w,h`, `--label=` and
  `--log-file=` for it.

- **Attributes replicate** (ADR 0106, protocol 17): an attribute the authority
  sets on a replicated instance, a `Player`, a `Team` or `GlobalScriptService`
  reaches every replica that has it, and `AttributeChanged` fires there. **A
  peer must speak protocol 17**: this and the scene change are wire changes.
  `examples/24-scenes` gains a lobby whose `Ready` is an attribute.

- **The network from a script** (ADR 0106): `NetworkService:Join(address?)`,
  `Host(port?)` and `Disconnect()`, `State` (the new `Enum.NetworkState`) and
  the `Connected`, `JoinFailed` and `Disconnected` events. Joining replaces the
  scene with the server's and stops this machine's server code; leaving goes
  back to solo, the other players go, and server code starts again.
  `[network] server` in `luaug.toml` is where `Join()` goes with no address.
  `--host`, `--serve` and `--join=` are the same calls before the first tick.

- **Scenes at run time** (ADR 0106): `SceneService:LoadScene(path, data?)`
  swaps the whole scene -- world, services' contents and settings, its own
  `src/scenes/<scene>/` code -- between two ticks, with `SceneLoading` and
  `SceneLoaded` around it, `GetLoadData` in the new scene and `CurrentScene`
  saying where the game is. `GlobalScriptService` and a `ScreenGui` with the new
  `KeepOnSceneLoad` stay. In a match the authority changes scene and every
  client follows without reconnecting (protocol 16: a `SceneChange` message);
  a client's own `LoadScene` is refused. `examples/24-scenes` is a menu and an
  arena.

- **Touch input and a game on an Android phone.** A finger on a touchscreen is
  `InputBegan` / `InputChanged` / `InputEnded` with `Enum.UserInputType.Touch`,
  and the new `InputObject.TouchId` says which finger; a tap shorter than a
  frame still begins and ends, and the mouse the system makes out of a finger
  is not a second input. `examples/20-platformer` has on-screen controls that
  appear on the first touch and drive its actions through `SetVirtualState`.
  `scripts/android-player.ps1` builds a project into an APK -- the player host
  as `libmain.so`, the engine's content and the game, extracted to the app's
  storage on first launch -- and installs it with `-Install`; it plays on a
  Galaxy S25 Ultra. Textures on Android are uncompressed RGBA until the
  transcoder targets ASTC.
- **`UIService.ScreenOrientation`** (`Enum.ScreenOrientation`: `LandscapeLeft`,
  `LandscapeRight`, `LandscapeSensor` -- the default -- `Portrait`, `Sensor`):
  which ways up a game may be held, applied to the phone the moment it changes.
  **`Camera.ViewportSize`**: the size the world is drawn into, in pixels,
  following a resize or a turn.

- **A part may change its material's surface shader parameters** (ADR 0091),
  one by one, as it changes `Color`: a material names them in
  `instanceParameters`, and `BasePart:SetMaterialParameter`,
  `GetMaterialParameter` and `ClearMaterialParameter` take their names. The
  values are saved with the scene, copied by `Clone` and edited in the
  Properties panel; they are not replicated, and a texture is not one.
  `GetMaterialParameter` answers `nil` for a shader parameter nothing sets.
- **A lost graphics device is survived.** A shader that hangs the GPU made the
  driver reset and the engine crash inside SDL; now the engine says so, the
  editor offers to save and restart, and the surface shaders that were on
  screen are held back until they change. A game says so and closes, with
  exit code 5. `--simulate-device-loss=N` loses the device on frame N, and
  `--surface-cache=DIR` moves the compiled-surface cache.

### Changed

- **A terrain's materials come from Content** (D330), as in other engines: the
  Paint panel's `+` picks one of the project's materials, a material dragged
  from Content onto it adds it and onto a swatch replaces that layer, and a
  swatch's right-click replaces, opens or removes it. The panel no longer makes
  materials; *New Terrain Starter Materials* is in Content's right-click menu.
- **`Terrain:PaintBall`'s `Falloff` is how much of the radius fades** (D328):
  the inner rest is the whole `Strength` and the fade ends at nothing on the
  rim -- it was a mix of a hard brush and a soft one. A `Blend` or `Erase` stamp
  takes its share of what is left, so a soft stroke keeps a soft edge however
  many stamps pass.
- **The editor is laid out as a game engine** (the owner, 2026-09-27: the look
  may be the code editor's, the layout has to be an engine's). The Explorer
  alone on the left, Properties the full height on the right with Stats a tab
  beside it, and along the bottom, under the tree and the world, the Content
  browser, the Console and Run and Debug. Every open script is a tab beside the Viewport -- and
  one that would open undocked, from an older layout, opens there too. Play,
  pause and step sit in the middle of the toolbar, the transform tools on the
  left, and the snap steps, the tools and Export on the right. The browser
  shows the project's folders beside a grid wide enough for names. Layouts
  saved before are rebuilt once.
- **The material editor opens beside the Viewport, the Viewport's size**: a
  large preview turned by dragging, on a ball, a block or a cylinder, beside
  its fields in the Properties grid -- Surface, Lighting, Textures, Surface
  Shader and what a part may change. A texture slot shows its picture, takes a
  texture dragged onto it and clears with one click. Closing with unsaved
  changes asks. The Export window opens beside the Viewport too.
- **Preferences is one window of one size**: a search over every setting, the
  pages in a tree down the left, the settings in the Properties grid.
- **Anything in the Content browser drags onto a folder to move there** -- a
  folder cell, the folder tree or a step of the path -- and every scene,
  stamp, material, the start scene in `project.toml` and the open world that
  named it follow. A rename does the same. A field that takes a file lights up
  only for a file of its kind.
- **A printed table opens in the console**, and the tables inside it,
  closed until opened; the line says what is in it rather than its address.
  The same line printed again from the same place is one line with a count, and
  a script's line names where it was printed, which a click opens.
- **The reference grid fades between its spacings** as the camera climbs,
  instead of jumping to ten times the spacing at one height.
- **The collision wireframe is drawn where the parts are drawn**, between two
  ticks, and coloured by how a body moves.
- **Properties, from the owner's pass over it**: categories and the
  properties in them are in alphabetical order, a category is a heading that
  reads as one, Attributes and Tags are rows of the same grid, a number field
  works out what is typed into it (`0+2`, `1/2`, `90/4`), and an enum shows
  its item's name closed and its whole name open. Stats is drawn in the same
  grid. An instance whose `Enabled` is off is dimmed in the Explorer.
- **A material's textures tile by size on a part** (`TileSize`, 4 m by
  default): a face shows as many repeats as it is long, so the edges of a long,
  thin slab are no longer the whole texture squeezed into a strip. 0 stretches
  each texture over the face, as before; a `MeshPart` keeps its file's UVs. The
  material editor shows it under Textures, and its preview ball is one repeat
  across.
- **A folder with something in it is drawn filled**, in the Content browser's
  grid and tree and in the Explorer.
- **The activity bar marks a panel that is open**, wherever it is: a click
  closes it as its own X would, and a click on a closed one opens it with the
  keyboard in it.
- **An Android export's orientation is the start scene's
  `UIService.ScreenOrientation`**: `[export.android] orientation` is gone. Two
  answers to how the phone is held turned the screen the moment the game
  started whenever they differed.
- `SetMaterialParameter` with a name no built-in field has, on a part whose
  material does not declare it, raises *not declared* rather than *not a
  parameter*: such a name may be a surface shader's.

### Fixed

- Two flat-coloured terrain materials painted over each other meet in a
  crossfade, not a step at half cover (D329).
- **Typing reaches a `TextInput` in an exported game**, and on a phone the
  on-screen keyboard opens with it (D204).
- `TextInput.FocusLost` delivers `submitted`: true when Return left the field
  (D215). A focused, empty field shows its caret, not its placeholder (D216).
- **A client no longer fights the server over a character's speed**:
  `WalkSpeed`, `JumpSpeed`, `MaxSlopeAngle` and `AutoStepHeight` replicate, and
  a correction replays at the server's speeds (D205). Protocol version 20.
- **`.Parent` of a destroyed instance reads `nil`** rather than raising, so
  `if part.Parent then` works on any handle; every other access to a destroyed
  instance still raises `script.err.instance_dead` (D206, the owner's decision).
- **Joining the same server again from a script is the same player**, with the
  same `UserId` (D207).
- **A server that vanishes is noticed in ten seconds**, not thirty;
  `[network] timeout` in `project.toml` sets it (D208).
- **Leaving a match tells the server at once**, so the others stop seeing the
  player who left (D217).
- A headless client back in solo no longer reports an eighteen-trillion
  millisecond frame, and waits for the clock again (D209, D213).
- `Enum.KeyCode` has punctuation, Insert, PageUp, PageDown, CapsLock, NumLock
  and the keypad, appended after the existing items (D210).
- `ludwerk check` and `ludwerk fmt` skip `dist/`, and name the engine's
  definitions as the engine's (D211, D212).
- **Nothing a person made is lost or doubled by saving, moving or closing** (an
  audit of every way the editor writes, after the tester's report below):
  - The unsaved-changes dialog's **Save** saves everything unsaved -- the stamp,
    the scene, the material, a shader's tab -- *before* opening another scene or
    starting a new one, which it used to do after (losing the work); a save
    that fails stops the change. It names what it will save.
  - A `src/` script edited in its tab is written by the scene's save, a changed
    tab counts as unsaved work, and closing one keeps its text in the scene.
    A file changed outside the editor as well is never written over: the disk
    is kept and the editor's text goes to `.engine/conflicts/`.
  - **Saving during play is refused** (it saved the running game); **Save As
    never writes over another scene**.
  - Moving or renaming content rewrites only the paths that name it -- a folder
    called `terrain`, `Model` or `Tree` no longer rewrites a scene's ground key,
    a class or an instance's name.
  - A class this build does not have, and a stamp whose file is gone, are kept
    as they were written and saved back, rather than dropped for good; a
    `global.json` that could not be read is never written over or deleted.
  - **Deleting in the Content browser moves to `.engine/trash/`.**
  - A terrain's cells follow their folder; a scene renamed or duplicated takes
    its `src/scenes/<name>/` code with it; opening another material saves the
    one being edited first.
  - Nothing can be put inside the local `Player`, which a scene never saves;
    content cannot be moved during play, and a move ends the undo history
    rather than leaving steps that point at files that moved; a stamp cannot be
    pasted into itself or made from scripts that are files; a value the running
    game sets says it is not saved; text typed during play survives a
    lost-device save.
- **Scripts copied, pasted or moved between the script services no longer lose
  their code or come back twice** (reported by a tester: scripts dragged and
  pasted between client and server would not save, and were doubled after
  reopening). A copy of a script from `src/` held only a mark, so a paste
  elsewhere became an empty folder of its name, and one beside it vanished into
  the original; a drag was refused, which sent people to copy and paste. Now a
  copy carries the whole script, a drag moves it, and every save makes `src/`
  match the tree -- moving, renaming and writing files, taking the next free
  name rather than writing over one, and moving a file a script gave up to
  `.engine/trash/` instead of deleting it. A script's `Enabled`, attributes and
  tags survive a reopen too; a `ModuleScript` outside `src/shared/` is written
  as `Name.module.luau`.
- **An attribute holding a `Color3`, `CFrame`, `Vector2`, `UDim`, `UDim2` or
  `Rect` survives saving the scene.** Written untyped, a colour came back as a
  vector and the other five did not come back; the file now says the type for
  these, and reads the old spelling as before.
- **Stamps** (an audit of the whole feature): two children of one name no
  longer swap their overrides; a child renamed in one instance keeps its name
  and its edits; attributes, tags and a part's shader parameters on a linked
  instance survive a save; references to and from the children a stamp
  rebuilds survive saving the stamp; duplicating, copying and pasting a linked
  instance keeps it linked, and so does one under `GlobalScriptService`; a
  stamp at the content's root opens and places; `Instance.stamp("name")`
  finds `stamps/name.stamp.json` as documented; a stamp is never made over one
  that exists, nor inside a stamped instance; closing a stamp whose save failed
  keeps it open; a scene naming a stamp that is gone says so when it loads; and
  a stamp changed on disk during `ludwerk dev` moves its linked instances.
- **Editor defects the owner reported**: a script edited and edited back is
  not unsaved; F3 twice brings back the tabs that were in front, not the last
  one drawn; the Command Palette scrolls under the wheel; a submenu keeps its
  icon while it is open; a toast says "Undone: Edit Size" rather than
  "undid Edit"; and the default agent's settings sit under their own heading
  on `NavigationService`, where they are documented as the default agent's.
- **The editor asked to save scripts nobody had changed**: a script written
  with spaces was converted to tabs on opening and marked unsaved, and its
  scene with it -- which is every template and example. The conversion stays;
  the tabs are written with the first save of a real edit.
- **Ctrl+Shift+P, Ctrl+P, Ctrl+B, F5 and the rest did nothing with the caret
  in a script**, and what was typed next landed in the code: the code pane
  claims every key while it is active. The workbench's keys now work from any
  text field, as they do in the editor it follows; only a modal keeps them.
- **The bottom panel reopened on Stats every launch**, whichever tab had been
  left in front: the last panel to appear took the focus, and a node shows
  its focused window's tab. The Explorer takes the focus at launch, and each
  node's saved tab is put back.
- **Quick Open (Ctrl+P) could not find the code of another scene**
  (`src/scenes/<scene>/`), which is not in the world until that scene loads.
- **An orthographic camera lost what stood above it** (the owner's report,
  2026-09-27): a top-down camera standing lower than the top of a ball, a
  cylinder or a wedge showed only its shadow, and -- once depth was clipped for
  mirrors -- the tops of blocks went too. An orthographic camera now sees its
  whole column, from `FarPlane` behind where it stands to `FarPlane` in front;
  clustered lights, soft particles and the editor's picking follow.
  `ortho_gate` holds it.
- **The Export window showed an ImGui error box** ("Code uses
  SetCursorPos()/SetCursorScreenPos() to extend window/parent boundaries")
  and framed its target list in red: each target card moved the cursor to
  draw its icon and left it past the list's end. Errors of that kind now also
  reach the engine's log, once each.
- **Ctrl+Shift+S overwrote the open scene** instead of asking for a name.
- Escape now closes the Export window. A client of a dedicated server with no
  address says "no server address", instead of "joins ?".
- Two zero-length `memcpy`s from a null pointer that UBSan stops on: the UI
  gradient upload (every capture gate under the sanitizers) and an archive
  test.
- **The Android export works again.** The rename left the Gradle signing
  configuration named after the brand and its two uses named `engine`, so every
  Android export failed while evaluating `build.gradle`.
- **A dedicated server opens no graphics device.** It used to open one and draw
  every frame offscreen for nobody; on a Linux host with no GPU that device is
  Mesa's software rasteriser, and an exported server crashed in it seconds after
  starting. `--rhi=` still chooses one. **And it sleeps between ticks**: with
  no device to wait on it had spun a thousand empty frames a second on a whole
  core.
- **`ludwerk keystore new --alias upload --out release.keystore` works as its
  usage text shows.** Every option that carries a value takes `--name value` as
  well as `--name=value`; before, the space form made a keystore named `true`.
- **An exported desktop game no longer carries the editor's files** -- the
  new-project template and the editor's icon theme -- as the Android export
  already did not.
- **Messages spoke the old names**: the host's usage text (`luaug-host`, which
  now also lists `--host`, `--serve` and `--join`), `.luaug/types/`,
  `luaug.toml`, `LUAUG_TEXTURE` and `LUAUG_PARAM` in a surface shader's errors,
  `luaug build-assets`. The brand lint reads the message catalog now.
- **Home no longer carries the match controls** (player count, dedicated
  server): they are on the Test tab, whose settings Play on Home still plays.
- **A service's settings are saved with the scene when nothing is under it**,
  and a scene opened in the editor starts from the engine's settings instead of
  the last scene's (D203). A property marked `Transient` in the IDL is no longer
  written to a scene, which the IDL had said and the writer had not done:
  `Sound.TimePosition`, and now `DebugService.OverlayVisible`,
  `InputService.PointerLocked` and `PointerVisible`.

## Before Ludwerk

The engine was called **LuauG** until 2026-09-26, and released three versions under
that name. Ludwerk's own versions start again at 0.0.1 (ADR 0109): the releases
below are its history, kept as they were written.

## [2.0.0] — 2026-09-26

**A major version, because the public API broke**: a part's look is the
material it wears (ADR 0090), and `BasePart.Color` and `BasePart.Transparency`
are gone -- see *Changed -- BREAKING* for the migration. Around that change:
surface shaders, and an ocean made from one (ADR 0091); the world's look as
instances under `Lighting` (ADR 0096); voxel terrain that streams from disk;
multiplayer with prediction, interpolation, interest, remotes, teams, network
ownership, rollback and a published wire protocol, now at version 15;
navigation for several agent sizes, priced ground, links, crowds and the 2D
plane; and the 2D layer's joints, sprite animation and replication. A peer on
protocol 14 or older is refused.

### Changed -- BREAKING

- **A text's alignment is `TextXAlignment` / `TextYAlignment`** on `TextLabel`
  (and what extends it), where it was `HorizontalAlignment` /
  `VerticalAlignment`. The values are still `Enum.HorizontalAlignment` and
  `Enum.VerticalAlignment`, and a layout keeps its own `HorizontalAlignment`.
  A script writing the old names must be changed; a scene saved with them
  opens with them read under the new ones.

- **A material is an asset, and a part wears one** (ADR 0090). **This is a
  breaking change to the public API, and the release that carries it is a major
  version.**
  - `BasePart.Color` and `BasePart.Transparency` are **removed**. A part's look
    is its material's; it overrides only the parameters that material declares,
    with `SetMaterialParameter`, `GetMaterialParameter` and
    `ClearMaterialParameter`, and reads them as `BasePart.MaterialParameters`.
    The engine default material -- what a part wearing nothing wears --
    declares `Color` and `Transparency`, so `part.Color = c` becomes
    `part:SetMaterialParameter("Color", c)`.
  - The `Material` **class is removed**: `Instance.new("Material")` raises.
    A material is a `.material.json` under `content/`, with a `parent` for a
    variant and `instanceParameters` for what a part may change.
    `BasePart.Material` is a `Material?` handle.
  - `Material` is a data type: `Material.load(content)` returns the shared,
    read-only handle for an asset; `material:Clone()` returns a writable runtime
    copy. Nothing clones implicitly.
  - The scene format is version 2. A version 1 scene opens with its tints as
    overrides on the default material; `luaug migrate materials` converts a
    project's files.
  - The wire protocol is version 12: a part's material, its overrides and a
    runtime copy it wears replicate, where version 11 sent `Color` and
    `Transparency` and never the material.
  - A part's fade can no longer be tweened through `TweenService`, which moves
    properties of instances; a material parameter is neither.

### Added

- **Surface shaders** (ADR 0091): a material may name a `.surface.hlsl` the
  project writes -- two functions over the `luaug/surface.hlsli` contract, one
  that may move a vertex and one that decides what the surface is -- and every
  part wearing it is lit, shadowed and post-processed as any other.
  Parameters (`LUAUG_PARAM`) and up to five textures (`LUAUG_TEXTURE`) are
  fields of the material; a blended surface may read the scene behind it
  (`sceneDepthAt`, and `sceneColorAt` with `readsSceneColor`).
  `Material:GetShaderParameter` and `Material:SetShaderParameter` read and
  write them from a script, on a clone. The editor compiles a shader in the
  background as it is saved, writes a new one from a template, edits it with
  HLSL highlighting and its errors on their lines, and shows its parameters in
  the material panel. Building a game compiles every surface for SPIR-V, DXIL
  and MSL into the pack, and the game carries no compiler. The engine carries
  three grids a shader can displace, `luaug://mesh/grid-16`, `-64` and `-256`.
  `examples/11-ocean` draws its sea with one (6.7 ms a frame to 0.67 ms), and
  `examples/23-surfaces` is a flag, a dissolve and glass.

- **2D joints** (ADR 0102): `HingeConstraint2D` (limits and a motor),
  `SpringConstraint2D` (a soft or rigid distance) and `WeldConstraint2D`, each
  joining `Part0` and `Part1` at `Anchor0` and `Anchor1`, under the abstract
  `Constraint2D`.
- **`SpriteAnimator`** (ADR 0102): frames of a `Part2D`'s sprite sheet played on
  the simulation clock -- `FrameSize`, `Columns`, `SheetOffset`, `FirstFrame`,
  `FrameCount`, `FramesPerSecond`, `Looped`, `Playing` and the read-only
  `Frame`.
- **2D on the wire, completed** (ADR 0103, protocol 15): remote sprites are
  interpolated between snapshots, and `Tilemap2D` is replicated, its cells sent
  whole on arrival and then by changed blocks. A peer on protocol 14 is refused.

- **Sides and network ownership** (ADR 0099, protocol 14).
  - `Team` (`Color`, `AutoAssign`, `GetPlayers()`) under the new `TeamService`
    (`GetTeams()`), whose teams reach every replica. `Player.Team` is a
    player's side; a player who joins goes to the `AutoAssign` team with the
    fewest players.
  - `BasePart:SetNetworkOwner(player?)` hands a loose part to one player's
    machine, which simulates it and sends its state up; the authority follows
    it and hands it back on `nil`, when the part is anchored, or when the
    player leaves. `BasePart:GetNetworkOwner()` answers the owner.
  - The wire is protocol 14: the roster carries each player's team, channel 3
    is `Ownership`, and two messages are new. Peers of protocol 13 are refused.
- **Rollback** (ADR 0101): `RunService:SaveSimulation()` returns the 3D
  simulation as a `buffer` -- the solver's whole state, and every simulated
  part's and character's motion -- `RestoreSimulation(state)` puts it back
  (false, and nothing changed, when the simulated parts are not the same ones),
  and `StepSimulation()` steps one fixed tick without scripts or touches. A
  re-simulation from the same state with the same inputs is bit-exact.
- **The wire protocol is published** (ADR 0100): `docs/protocol/wire.md`,
  generated from the schema, states every message byte for byte, and the
  compatibility policy -- one version, matched exactly, changed whenever the
  bytes change.

- **Navigation for more than one body, over priced ground, across gaps, in
  crowds, and on the plane** (ADR 0098).
  - `NavigationService:DefineAgent(name, radius, height, maxClimb?, maxSlope?)`
    names another agent size with a mesh of its own, and `FindPath`
    takes its name as a last argument.
  - `NavigationArea`, under a part, labels the ground inside it (`Label`);
    `NavigationService:SetAreaCost(label, cost)` prices it for every agent,
    and `math.huge` forbids it. An unpriced label costs 1.
  - `NavigationLink` (`From`, `To`, `Bidirectional`, `Label`) joins two points
    the mesh does not -- a jump, a ladder. `FindPath` gains a third answer: the
    label of the link that begins at each waypoint, or `""` where the way on is
    walking.
  - `NavigationAgent`, under a part, walks it to its `Target` around every
    other agent, on the fixed tick, at `MaxSpeed`, over the mesh its
    `AgentType` names; `Reached` fires when it arrives.
  - `NavigationService:FindPath2D(from, to)` searches the plane: every
    colliding `Tilemap2D` cell and every anchored, colliding `Part2D` is a
    wall.

- **The look of a world is instances** (ADR 0096). Under `Lighting` they are
  the world's -- saved with the scene and replicated -- and under
  `Workspace.CurrentCamera` they are the viewer's own; anywhere else they do
  nothing and the editor says so. A world with none of them draws exactly as
  before.
  - `BloomEffect` (`Intensity`, `Size`, `Threshold`) governs the engine's
    bloom, and one that is disabled turns it off; `ColorCorrectionEffect`
    (`Brightness`, `Contrast`, `Saturation`, `TintColor`); `BlurEffect`
    (`Size`); `DepthOfFieldEffect` (`FocusDistance`, `InFocusRadius`,
    `NearIntensity`, `FarIntensity`); `SunRaysEffect` (`Intensity`,
    `Spread`). All five extend the abstract `PostEffect` (`Enabled`).
  - `Atmosphere` (`Density`, `Offset`, `Color`, `Decay`, `Glare`, `Haze`):
    distance and height fog lit by the time of day, meeting the sky at the
    horizon. With one, `Lighting.FogStart`, `FogEnd` and `FogColor` are kept
    and not used.
  - `Sky`: six pictures (`SkyboxBack` ... `SkyboxUp`, `SkyboxOrientation`)
    that are also what the world reflects; the sun (`SunTexture`,
    `SunAngularSize`), the moon (`MoonTexture`, `MoonAngularSize`), stars
    (`StarCount`), `CelestialBodiesShown`; and clouds (`CloudCover`,
    `CloudDensity`, `CloudColor`) drifting on `SimTime`. The sun's direction
    stays `ClockTime`'s.
  - `Lighting.EnvironmentDiffuseScale`, `EnvironmentSpecularScale`,
    `ShadowSoftness`, `GlobalShadows` and `AutoExposure`. Their defaults are
    the picture before them, to the bit; every determinism trace moved once
    for them.
  - `[graphics] depth_of_field` and `sun_rays` in `luaug.toml` (off in the Low
    preset, and depth of field in Medium), and in Project Settings.
  - The wire carries `Lighting`'s children and its five new properties:
    **protocol 13**, which a protocol 12 peer refuses.
  - `examples/22-atmosphere`, and the manual's new page, "Atmosphere, sky and
    clouds".

- **`TextLabel.TextTransparency`**: the words' own see-through, apart from the
  box's `BackgroundTransparency` (0 solid, 1 not drawn; drawn clamped).
- **The editor, from the owner's feedback pass**
  (`docs/briefs/editor-feedback-2026-09-24.md`): a ribbon across the top
  (Home, Model, Test, View); WASD flies without a held button; Ctrl+1 to
  Ctrl+4 and Ctrl+L for the tools; Alt+click selects inside a model; Shift+P
  flies free in play; Properties grouped by task with a filter that keeps its
  headings and a CFrame's rotation editable in degrees; Group as Folder; a plus
  on every instance, with every service's contents saved; a script editor with
  self-closing pairs, clickable suggestions that read the file's own code,
  type colouring, Ctrl+/, the line and word keys code editors share, and an
  error's message at the end of its line.

- **The material editor**: New Material and New Variant in the content
  browser, with a rendered ball on each material's row; a Material panel with
  its own undo that shows an edit in every world as it is made; a part's
  Properties show its material (picked, or dropped from the browser onto the
  field, an Explorer row or the part in the viewport) and each parameter the
  material lets it change.
- `assetc` compiles materials to their own pack kind, and a glTF import writes
  one material asset per material in the file.

- **Terrain debug views**: `Terrain wireframe` and `Terrain normals` in the
  editor's viewport settings, and `DebugService:ShowPanel("Terrain")` in a game.
  Triangles are drawn green, or red where one faces the wrong way, with a line
  along each vertex normal.
- **`examples/19-terrain-test`**: every controlled case of the terrain report
  side by side (flat, slope, hill, a ball added and taken away, tunnel, cave,
  wall, overhang, a chunk corner), toured from above, the side and below.

- **Terrain is a grid of voxels** (ADR 0082). Every voxel holds a material and
  an occupancy, and the surface is where the occupancy crosses one half:
  - caves, overhangs and flat ground are the same data, so nothing is special
    about any of them;
  - it is drawn as meshes with level of detail, built from each chunk's
    averages further away;
  - it collides chunk by chunk around whatever moves.

  New verbs: `FillCylinder`, `SmoothBall`, `FlattenBall`, `ReplaceMaterial`,
  `ReadVoxels`, `WriteVoxels`, `WorldToCell` and `CellCenterToWorld`. A world
  saved before this opens as it was and is saved as voxels from then on.
- `Workspace:Raycast` meets terrain anywhere, not only near things that move.
- **`TextLabel.RichText`** (F3): a label reads its text as markup -- `<b>`,
  `<i>`, `<u>`, `<s>`, `<font color size transparency>` and `<br/>`, with the
  five XML entities -- so colour, size and weight change part-way through one
  label. A tag it does not understand is drawn as text.
- **Multiplayer prediction, interpolation and interest** (ADR 0076):
  `Player.Character` names a player's part; a replica moves its own at once and
  the authority's snapshots correct it, draws everyone else between snapshots
  instead of stepping, and is sent only what is near its character. Decals and
  `Lighting` (the time of day, the light and the fog) replicate too.
- **`SurfaceGui` and `BillboardGui`** (F3): UI drawn in the world -- on a face of
  a part (`Enum.Face`, `PixelsPerMetre`) or over a point and facing the camera,
  sized in metres, in pixels, or both. The children are the screen's own
  classes, laid out the same way, and hidden by what is in front of them.
- **Terrain and block worlds stream from disk** (ADR 0075): a saved field of
  sixteen 64 m cells or more is cut into cells at play and streamed around the
  foci on the terrain radii. A cell somebody changed is never evicted, and the
  world waits for its ground on first load. `Terrain.CellSize` now names the
  grid it streams on.
- **Digging into a wall carves it**: the editor's dig aimed at steep ground, or
  with the box, takes volume out and bores forward at a steady speed while held.
- `examples/17-cave`: a tunnel into a mountain, dark inside and lit by its lamps.
- **Soft particles**: smoke and fire fade where they meet a surface instead of
  showing a hard line along it.
- **The terrain seam gate** (F1, H1): a tunnel crossing a streaming-cell
  boundary, a tile boundary and the edge between bricked and height-encoded
  ground. Every downward ray must hit the field, the collider and the drawn
  surface, and each hit must lie within a quarter-voxel of the field.
  `asset::sampleField` exposes the field's trilinear sampler, and
  `render::meshCaveColumn` exposes the cave mesh as it is drawn.
- **A dig near a hill no longer cuts it off, and a dig on a hill no longer
  deletes the ground below it** (D163): a column the brush never touched was
  written with the top or the bottom of the brush's range. This is what put
  floating plates and see-through holes in sculpted worlds.
- **Raising, lowering, smoothing and flattening work over caves** (D162): a
  column with a cave in it moves by its top and keeps the cave, where it used
  to be skipped and left a slot through the new ground. So do Generate Flat
  Ground and `WriteHeights`.
- **Digging into a selected terrain no longer draws a lid and boxes over the
  hole** (D161): the selection outline, which shows through everything, drew
  the cave meshes' buried sides. A cave draw is never outlined now.
- **A terrain larger than the atlas draws what is near the camera** (ADR
  0081). Past 16,384 tiles, a 2 km square at half a metre, the loader kept the
  first tiles in key order, and the editor drew a strip along one edge of the
  world. It now keeps the nearest, frees the far ones before uploading, and
  uploads outwards from the camera.
- **`ReplicatedStorage` and `ServerStorage`** (ADR 0080): two services that
  hold what is not the world, saved with the scene under a new optional
  `storage` key. What `ReplicatedStorage` keeps reaches every replica whatever
  its distance; `ServerStorage` stays on the authority. `scene.d.luau` types
  their contents on `game`. The Explorer takes a drop onto `Workspace` or a
  storage, which it refused before. Wire protocol 9.
- **`RemoteFunction`** (ADR 0079): a client asks with `InvokeServerAsync`,
  which yields until the authority's `OnServerInvoke` answers, and raises when
  the handler fails or there is none. The IDL gains callbacks, a function a
  script assigns and the engine calls, and the wire protocol is version 8.
  `examples/15-multiplayer` asks with H.
- **The editor's Terrain panel imports a heightmap and holds the terrain's
  settings**: a 16-bit PNG or RAW image laid over the ground at a size and
  between two heights, one undo step, and `VoxelSize`, `MinHeight` and
  `MaxHeight` beside it. The Blocks panel sets a type's images and opacity.
- **A dot reaches a child, and the scene is typed** (ADR 0078, superseding
  0061): `workspace.Player.Walker` reaches the child after the members, and
  `.luaug/types/scene.d.luau` declares the scene's tree so the analyzer types
  the path and still catches a typo. The editor writes it on open and on every
  save; `luaug setup` and `luaug-host --write-types` write it; `luaug check`
  and the starter's VS Code settings load it. Completion offers children after
  a dot.
- **`RemoteEvent`** (N2, ADR 0077): a game's own messages between machines.
  `FireServer`, `FireClient` and `FireAllClients`, received as
  `ServerReceived(player, ...)` and `ClientReceived(...)`. The authority names
  the sender from the connection. Values, instances and tables travel,
  reliably and in order. Solo and hosting, every call is delivered locally, so
  one script runs in every posture. `examples/15-multiplayer` has a horn. The
  wire protocol is version 7. A manual page, *Multiplayer*, covers the whole
  networked surface.
- **Buttons in the world are pressed** (F3): a `TextButton` on a `SurfaceGui`
  or `BillboardGui` fires `Activated` and the hover events like one on the
  screen. The pointer's ray finds it, anything solid in front hides it, and
  `AlwaysOnTop` wins. `examples/18-world-ui` has a "Next round" button on its
  scoreboard.
- **`Terrain:WriteHeights(corner, columns, heights, material?)`**: a heightmap
  in one call, one height per column, row after row. It is the verb for ground
  that comes from a generator or an image. The flagship's middle is written
  with it in under a second, where one `FillBlock` per column took 28.
- **Water flows** (V1): `VoxelService:SetBlockFluid(id, reach, ticksPerStep)`
  makes a block type a fluid. It pours down first, spreads up to seven blocks
  sideways (shallower with each one), and drains when its source is taken. It
  does not collide, `Raycast` passes through it, and `GetFluidDepth` says how
  full a block is. A world now holds up to 4,095 block types: the rest of a
  stored id is the block's state. `SetFluidReaction(from, touching, result)`
  says what one fluid becomes where it touches another: lava meeting water
  sets as stone.
- **The flagship stands on terrain** (F1, H3): the middle 512 m of
  `examples/10-open-world` is one streamed `Terrain`, with a hill and a tunnel
  through it, in place of 1,024 16-metre boxes. `tools/sculpt-ground` and
  `tools/merge_ground.luau` make and place it.
- **A replica keeps what a script holds.** An instance that leaves a replica's
  interest becomes a husk, reparented to nil, and fires
  `StreamingService.InstanceStreamedOut`, exactly as an evicted chunk's does.
  One the authority destroyed is destroyed. The wire protocol is version 6.

### Changed

- **`Lighting.Ambient` lights enclosed spaces, and the new
  `Lighting.OutdoorAmbient` lights open ones** (ADR 0084). A surface takes a
  blend of the two by how much sky it sees, so a cave is dim rather than black.
  Both start at the old `Ambient` default. A script that set `Ambient` to light
  its parts should set `OutdoorAmbient` too, since parts are outdoors.

- **The simulation is deterministic across platforms** (level C, ADR 0083). The
  same seed and the same operations give the same world hash on Windows, Linux
  and macOS. The engine ships its own `sin`, `cos`, `exp`, `log`, `pow` and the
  rest, a script's `math` and `^` use them, and no compiler may fuse a
  multiply-add the source did not write. A replay recorded on one machine plays
  back on another, and each determinism scenario has one trace for every
  platform.

- **`Terrain.VoxelSize` defaults to one metre** (was half a metre). A terrain
  that was already sculpted keeps its own.
- `Terrain.HeightAt` answers the top of the ground in a column, over a cave
  too, and between columns it blends the four around the point.
- `Terrain` brushes return how many voxels they changed, and `WriteHeights`
  counts voxels rather than columns.
- `Terrain.MinHeight` and `MaxHeight` are the world's floor and ceiling: no
  voxel is written outside them.
- `Terrain.Compact` has nothing left to do and returns 0; every edit leaves the
  voxels compact.
- `Terrain.CellCount` counts chunks of 32 voxels a side.
- **The terrain brush works on the ground as it now is, while the button is
  held.** A held Add piles up, a held raise keeps climbing and a held dig goes
  deeper, at a rate the brush's strength sets. It used to aim at the ground as
  the stroke began, and could not see its own work.
- **The terrain brush has six tools, and none of them moves a column.** Add and
  Subtract stamp a ball (or box) centred where you aim, so a click on the side
  of a cliff builds out from it rather than standing a pillar under it. The new
  Grow and Erode move the surface along its own slope (`growBall`). Smooth and
  Flatten are unchanged.
- Ground laid where there was none -- `WriteHeights`, Generate Flat Ground, a
  first raise on empty terrain -- is a slab 32 m deep rather than a column to
  `MinHeight`. A terrain's edges show walls and a bottom, all the same way.

### Fixed

- The shadow under an overhang no longer shows a row of teeth along its edge,
  and the terrain's quads are split along their shorter diagonal, so sharp rims
  no longer make long thin triangles (the owner's terrain report).
- **One ctrl+Z undoes one brush stroke** (D168). Every stroke of a session had
  joined one undo step, so a single ctrl+Z took back all of the terrain's
  edits.
- **A long sound streams instead of being decoded whole** (D129). Past ten
  seconds a file's encoded bytes are kept and each voice decodes just ahead of
  the speakers: three minutes of music costs the file's size, not about 70 MB.
  Seeking and looping work the same on a stream.
- **The tick reads a sound's length from its file's header** (D129), so a sound
  made and played on the same tick no longer decodes its whole file on the
  tick. Ogg Vorbis is included: its length is read from its last page.
- A looped sound lost one frame at every loop point, a faint click on every
  loop.
- A ball of terrain on the side of a cliff no longer stands a dark stripe down
  the wall under it. The terrain's sky term marches rays, so an overhang shades
  what is near it, not everything below it.
- **Digging caves no longer lags, and a cave no longer breaks where it meets the
  ground** (D164): terrain is one grid of voxels, so there is no join between
  two encodings to fault. A dig rebuilds only the mesh and the collider of the
  chunk it changed.
- A partitioned scene kept its block world (D157); a terrain larger than about a
  square kilometre reopened after a save (D159); terrain edges stopped hanging
  curtains to the floor as the level of detail changed.
- A tree of `Cutout` leaf blocks casts its leaves, holes and all, rather than a
  solid square.
- A script holding a streamed instance keeps a working handle when its chunk is
  evicted, and a husk nothing holds any more is destroyed (D160).

## [1.1.0] — 2026-09-23

**The editor.** v1.0.0 shipped an engine you wrote games for in a text editor;
this is the phase that gave it a window. Nine milestones, E1 through E9.

### The editor

- **`luaug edit`** — an application with a menu bar and dockable panels, a
  viewport you click, fly and select in, play/pause/step/stop/save, a content
  browser whose folders and context menus open a scene by double-clicking it,
  undo and redo, and a scene format that is what a project's world data IS
  rather than an export of it.
- **Direct manipulation**: translate, rotate and scale gizmos, creating
  instances, reparenting by drag, multi-select, `Group` and `Ungroup`, a snap
  step you can change with a grid that shows it, and a gizmo that sits where you
  say over a selection rather than where an average lands.
- **Prefabs as stamps** (ADRs 0048, 0051, 0060): `content/` holds SOURCES and an
  instance in the world may be a LINK to one. Editing a linked instance is an
  OVERRIDE and the link survives — the reverse of what ADR 0048 first said,
  reversed while the milestone was building it. A placed stamp can be asked
  which of its properties are its own, and each one reverted or applied back.
- **A launcher** (ADR 0055): `luaug-host` with no project opens a project
  browser instead of printing a usage error — recent projects, a new one from
  a template, a folder picker.
- **A script editor** (ADR 0057): any number of scripts as tabs beside the
  viewport, Luau colour from the engine's own lexer, find and replace, errors
  underlined where the parser puts them, autocomplete from the reflection
  tables, and a working debugger with breakpoints, stepping, the call stack and
  the locals — the script parked and the frame loop still drawing.
- **A look of its own** (ADR 0056): one theme as data rather than
  `StyleColorsDark` plus nine literals, Inter instead of a 13 px bitmap face,
  and a palette measured against WCAG rather than argued about.
- **A downloadable folder** (ADR 0054), and an Explorer that costs what is open
  rather than what exists.

### Assets

- **A world you author becomes a world that streams** (ADR 0053), with no
  generator script and nothing sorted by hand. `Model.StreamingMode` makes the
  model the unit rather than the part.
- **Everything arrives compiled** (ADR 0065). A loose `.gltf` is a source, not
  an asset: opening a project compiles what has no compiled form, in every host
  mode, so a clone from git works with no command. What this buys is what the
  compiler was always producing and the runtime was not reading — LOD chains,
  meshlets, and BC7 with mips instead of raw RGBA8. It is also the only reason
  an FBX has ever loaded.
- **A model becomes a `Model` of named parts**, one per primitive, each
  addressable as a URN fragment and each able to carry its own material.

### The public surface

New classes: `Attachment`, `Bone`, `Constraint`, `BallSocketConstraint`,
`HingeConstraint`, `FixedConstraint`, `Ragdoll`, `Material`, `BaseScript`,
`ModuleScript`. New enum: `AlphaMode`.

- `BasePart.Material` — a part points at a material, and `BasePart.Color`
  multiplies it, so white on both sides is the identity and no existing scene
  changes (ADR 0060). `BasePart.CanTouch`.
- `Model.Scale` — absolute rather than cumulative, about the pivot, and one
  undo takes it back. `Model.StreamingMode`.
- `MeshPart.MeshSize` — authored rather than derived, so a headless run and a
  rendered one cannot disagree about how big a mesh is.
- `PointLight.Enabled` and `SpotLight.Enabled` stop being inert and cast.
- `StreamingService` gains separate structure and terrain radii.

### Also in this release: the first of phases 2 and 4

Built while the editor phase waited for its sign-off, and shipped with it
rather than held back. Each works end to end and each has limits of its own,
listed below.

- **Terrain** (ADRs 0066, 0067, 0071): `workspace.Terrain`, one signed-distance
  field stored as height tiles where the ground is a height and as voxel bricks
  where it is not, so caves and overhangs cost only where they are. Sculpted
  from a script or the editor's brush, drawn from a height atlas on the GPU,
  collided, saved with the scene.
- **Block worlds** — `VoxelService`, which is not the terrain: registered block
  types with images per face, cutout and translucent blocks, place and break,
  a raycast to the block under the crosshair, colliders, and a block tool in
  the editor.
- **Multiplayer on a LAN** (ADRs 0069, 0070): `--host`, `--serve` and `--join`,
  snapshots diffed against acknowledged baselines, `NetworkService` and
  `Player`, and what a player did reaching the authority as intent.
- **Particles and decals** (ADR 0072): `ParticleEmitter` and `Decal`.
- **Sharper shadows and contact shadows**, and **colour textures that are
  finally drawn in the colours they were painted in** (ADR 0073): every
  compiled colour texture had been uploaded as linear and drawn pale.

New classes: `Terrain`, `VoxelService`, `NetworkService`, `Player`,
`ParticleEmitter`, `Decal`. New enums: `NetworkTopology`, `ParticleShape`,
`BlockOpacity`.

### Known limits, stated plainly

Still not built, each with an owner in the roadmap's post-v1 phases: world-space
UI (`SurfaceGui`, billboards), rich text, navmesh pathfinding, a 2D workflow and
mobile. Terrain and block worlds do not stream from disk yet; multiplayer has no
client prediction or interest management and its transport is unencrypted, so
it is for a LAN; particles are not soft; a decal darkens and tints and cannot
brighten.

**E5, E7, E8 and E9 were signed off on 2026-09-23 on the owner's delegation.**
The one row each had that only a person could close is named in its brief.

## [1.0.0] — 2026-08-22

The first release. Eleven milestones, M0 through M8, each signed off by a human
after playing what it built.

### The engine

- **A sandboxed Luau 0.734 VM** embedded directly, with `luaL_sandbox` on in
  every profile, `--!strict` under the new type solver everywhere, and the
  legacy scheduling globals (`wait`, `spawn`, `delay`, `tick`) absent rather
  than deprecated.
- **An Instance tree over a hand-rolled deterministic ECS**: `game`,
  `GetService`, `Instance.new`, `Parent`, `FindFirstChild`, attributes, tags,
  and `Clone`. A destroyed instance's handle stops resolving at the end of the
  drain in which `Destroying` fired, which is a keyed error instead of a silent
  read of a corpse.
- **Deferred-only signals** (ADR 0015) with `:Connect`, `:Once`, `:Wait` and
  `GetPropertyChangedSignal`, and `Signal<T...>` as a global datatype in place
  of `BindableEvent`.
- **A fixed 60 Hz simulation tick** with a variable render clock, five phase
  signals, and `task.spawn/defer/delay/wait/cancel`. **The renderer interpolates
  between ticks**, so a display faster than the simulation shows smooth motion.
- **Determinism as a gate, not a claim**: same build, same platform, same seed
  and inputs produce the same world hash at every checkpoint, replayed over
  10,000 ticks in CI (ADR 0025).

### Rendering

- **Forward PBR** with metallic-roughness materials, glTF 2.0 meshes, primitive
  parts, and a `Lighting` service that drives a physically-derived sky.
- **Four cascaded shadow maps** in one atlas, with a world-constant filter
  radius, normal-offset bias, a blend band rather than a switch at a plane, and
  a fit that keeps its box while the box still covers what casts.
- **Clustered forward shading** on a 16×9×24 grid: the light count stopped being
  eight.
- **Image-based lighting** by the split sum, with the sky as the environment, so
  a metal reflects the hour the script set.
- **A post chain**: depth prepass, screen-space ambient occlusion, automatic
  exposure with an artist control in EV stops, bloom, and FXAA.
- **Instanced draws**: 4,002 visible objects in 22 draw calls, against 15,390
  before.
- **A graphics settings family** (ADR 0044): `low`, `medium`, `high`, `ultra`,
  plus render scale, shadow resolution, cascade count and distance, light budget
  and post toggles — from `luaug.toml` and the command line, never from a scene.

### Physics

- **Jolt 5.6**, with rigid bodies, contacts surfaced as `Touched`, collision
  groups, raycasts and shapecasts, transform welds, and a `CharacterBody` that
  walks, jumps, climbs steps and rides moving platforms.

### The world

- **An offline asset pipeline**: assimp to canonical glTF, a content-addressed
  pack, and a build that is byte-identical across processes.
- **Chunk streaming** with a minimum ring that must be resident before a focus
  advances into it, hysteresis on eviction, and a materialisation budget in
  milliseconds rather than in chunks.
- **A per-world floating origin** (ADR 0014), verified identical at 10⁷ metres.
- **A job system** with work stealing, dependencies and a stable-merge commit.

### The game layer

- **The Input Action System** (ADR 0029): actions, contexts, bindings,
  rebindable at runtime, promptable, with a non-device channel a HUD button
  drives.
- **A UI tree** with `ScreenGui`, frames, labels, buttons, images, scrolling,
  `UDim2` layout, and text through a vendored Inter.
- **Tweens**, **audio** on the simulation timeline, and **skeletal animation**.

### Tooling

- **`luaug new`**, **`dev`** (hot reload under 500 ms, measured at 1.7 ms),
  **`run`**, **`test`**, **`check`**, **`fmt`**, **`build-assets`**, and
  **`build`** — which produces a folder you can send to somebody: the player,
  the engine's content, and the game.
- **Application identity**: a game built with `luaug build` carries its own icon
  in the artifact, verified by reading the resource back out of it.
- **A typed IDL** as the single source of truth for the public surface, from
  which the C++ registration, the `.d.luau` definitions, the api-dump and the
  API reference are all generated and freshness-gated.

### Known limits, stated plainly

Not in v1, each with an owner in the roadmap's post-v1 phases: a visual editor,
particles, decals, terrain, `SurfaceGui`, rich text, navmesh pathfinding, a 2D
workflow, multiplayer and replication, and mobile. `Sound.Content` plays a
generated tone rather than a file. `Enum.CollisionFidelity` round-trips while
every value collides as a box. Properties the engine stores and does not read
are marked `Inert` in the inspector and the api-dump, and a gate stops a new one
appearing quietly.

[2.0.0]: https://github.com/zSkanz/LuauG/releases/tag/v2.0.0
[1.1.0]: https://github.com/zSkanz/LuauG/releases/tag/v1.1.0
[1.0.0]: https://github.com/zSkanz/LuauG/releases/tag/v1.0.0
