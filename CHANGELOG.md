# Changelog

Every release of Ludwerk. The format is [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the versions are [semantic](https://semver.org/spec/v2.0.0.html).

**The public surface is `api/api-dump.json`**, which is generated from the API
definitions and compared in continuous integration. A change to it is an entry
here.

## [Unreleased]

### Fixed

- A server takes more than eight players from one network address: a
  household, a LAN house, a school or a mobile carrier. The limit is
  `[network] max_connections_per_address` or `--max-per-address=N`, 64 by
  default, 0 for no limit. A player over it gets a `JoinFailed` that says why,
  where before they saw "connection lost". A flood from one address is still
  dropped, judged by how fast it connects.
- A part larger than the streaming radius is sent to a player standing on it
  far from its centre. Interest is measured to the part's edge, so a 4 km floor
  no longer leaves the players on it falling through and corrected every tick.
- A script saved with a byte-order mark (Notepad, Windows PowerShell 5) checks
  clean in the editor and in `--check-scripts`, as it already ran in the game.
- A glTF node's morph weights are where its targets start, over its mesh's.
- A content file read in the middle of its author's save no longer drops
  what was loaded: a sound, a material, a viseme track or face map, a
  sequence, an animation graph and the roles beside a model keep what was
  read until a whole read succeeds, and a file left unreadable is said once,
  two seconds after its last change.
- A volume's light baked again, or its file changed while a development
  session runs, is the room's light at once, and only the file that changed
  is read again.
- The editor's Bake, on a machine whose settings work no volume out, works
  out the volume it was asked for and leaves every other its file's colour.
- In the Timeline, the name a `cast` track brings is in the list of bindings
  a track is added on, and a track on it is not marked unbound.
- Two parts welded together no longer push each other apart: a weld's two
  bodies do not collide, so a cabin welded on a chassis no longer drags it.

### Added

- Luau's flags are a project setting: `[luau.flags]` in `project.toml`, and
  Project Settings > Luau, which offers `if local` and exported values by name
  and every flag under Advanced. The game, the editor's checker,
  `ludwerk check` and VS Code read the same values. A player whose game has
  other values cannot join (protocol 53), and a replay recorded with other
  values is refused.
- `engine-host <project> --print-luau-flags` prints the values the tools that
  check scripts are given.
- In a sequence, `"$$..."` is a string that begins with `$` where a value
  goes; the writer puts the second `$` back.
- `assetc --cache <dir>`: a source is answered from what an earlier compile of
  the same bytes, name and options wrote there.
- Preferences, Performance: the editor's frame rate (the display's, 30 to 240,
  or unlimited), its slowing down in the background, the Viewport's quality
  while editing, and Play with the player's saved settings; the Viewport's
  quality is also a click away on its bar.
- Project Settings, Quality levels: the levels side by side, each cell the
  engine's value or the project's own, and a level for each platform. A
  level's own values are `[graphics.low]` to `[graphics.ultra]` in
  `project.toml`.

- Vehicles: a `VehicleController` on a chassis and a `VehicleWheel` for
  each wheel make a car that drives on a cast suspension, with an engine,
  gears and brakes. A player seated as its `Driver` drives it predicted, as a
  character walks, allowed three centimeters from the authority's car and
  pulled back gently, and drawn gliding when it is corrected. Protocol 52.
- `NetworkService:GetStats().PredictedDrift`: how far the parts this machine
  predicts were from the authority's at the last answer.
- `BasePart:Fracture`: a box part cut into pieces held by joints that break.
  A wall that stood keeps its footing and breaks where it is struck.
- `Cloth`: a skinned mesh that hangs as cloth, every vertex simulated, pinned
  by the joints `PinJoints` names, kept out of its body's `SpringCollider`s and
  the anchored parts near it, and blown by the wind. Picture only; off at the
  Low level and on a phone below High. `--skip=cloth` draws a run without it.

### Changed

- Luau 0.742, luau-lsp 1.70.1, StyLua 2.6.0, SDL 3.4.18, fastgltf 0.9.1 with
  simdjson 4.6.11, meshoptimizer 1.3 and xxHash 0.8.4. Bytecode exported by
  the previous build still loads: the compiler still writes version 9, and the
  VM now reads versions 3 to 14.
- Android games target API 36 (Android 16), which Google Play requires of new
  apps and updates. They still run from API 21. Building needs SDK platform 36
  and build-tools 36.0.0; `scripts/install-android.ps1` installs them.
- The wire protocol is 53. A build before this one cannot join a server of
  this one, or the other way round.
- In the editor, the scene is edited, and Play starts, in the project's
  graphics settings; a player's saved ones are Play's only when asked for.
- Models are compiled side by side, one to a worker, and give the same bytes
  as with one; the character example compiles in 3 s where it took 14.
- An export reads the editor's import cache: a model or a picture the editor
  already compiled is not compiled again, and the export says how many it was
  given. The cache's key now carries the mesh format, so every entry is
  compiled once more the first time.

## [0.0.1]

The first version under the name Ludwerk. What the engine has:

### Scripting

- Luau in strict mode, with generated type definitions and `luau-lsp` support.
- An `Instance` and `Service` API over a data-oriented core: instances,
  properties, attributes, tags, deferred signals, and a `task` scheduler.
- Modules as files, required by path, and code in three script services: one
  for the server, one for each client, one for the whole game.
- Hot reload: a saved script rebuilds the running world, and a saved asset is
  read again without restarting it.
- A script editor with completion, diagnostics and a debugger.

### Simulation

- A fixed-tick, deterministic simulation with interpolated rendering.
- Input recording and replay that reproduce a run exactly.
- 3D physics (Jolt) and 2D physics (Box2D): constraints, movers, queries,
  character bodies and ragdolls.
- Navigation: a navigation mesh built where it is asked for, agents, and
  swarms of many agents steered on open ground.

### Rendering

- Physically based forward rendering with cascaded shadows, clustered lights
  and image-based lighting.
- Reflection probes, irradiance volumes for bounced light, and reflections
  and bounce from the screen.
- Anti-aliasing (FXAA, SMAA, TAA), upscaling (FSR 1 and FSR 2) and frame
  generation.
- Post-processing, atmosphere and sky, particles, decals, beams and trails.
- Terrain with sculpting, materials and painting by rule, block worlds, water,
  foliage and wind.
- Materials as assets, and surface shaders written by hand or drawn as a node
  graph.
- Cameras that draw into textures, and worlds that run beside the main one.
- A 2D layer: 2D parts with sprites, sprite animation, tilemaps and joints.

### Animation

- Skeletal animation and animation graphs: states, blends, layers and events.
- Inverse kinematics, retargeting between skeletons, morph targets, and
  secondary motion for capes, tails and hair.
- A sequencer for cutscenes: cameras, animation, dialogue, sound and events on
  one timeline.

### Audio

- Positional audio with effects and mixing groups.
- Dialogue in several languages, with captions and lip sync.

### Interface

- A UI of frames, text, images, layouts, gradients and strokes, on the screen
  and in the world, driven by mouse, touch, keyboard or gamepad.
- Text and messages through a translation catalog, in the engine and in a game.

### Multiplayer

- One build that hosts, joins or runs as a dedicated server.
- Replication with client-side prediction and rollback, interest management
  and network ownership.
- Joining by address or by a code through a relay.
- Several local players at one machine, each with a part of the screen.

### Editor

- Explorer, properties, viewport manipulators and a content browser.
- Terrain, water and foliage tools; a timeline; a node editor for materials
  and animation graphs; a profiler.
- Play in the editor, with one player or a whole match.
- Export of a game for Windows, Linux and Android from one window.

### Tools

- `ludwerk`: `new`, `dev`, `edit`, `check`, `test`, `fmt` and `build`.
- `--log-keys`, and `ENG_LOG_KEYS=1` in the environment: every message of the
  engine is written with its catalog key before it, so that a script reading a
  log can match the key, which is the same in every language.

### Platforms

- Windows, Linux and Android. macOS is built in continuous integration.
