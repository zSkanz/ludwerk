# Changelog

Every release of Ludwerk. The format is [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the versions are [semantic](https://semver.org/spec/v2.0.0.html).

**The public surface is `api/api-dump.json`**, which is generated from the API
definitions and compared in continuous integration. A change to it is an entry
here.

## [Unreleased]

### Fixed

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

### Added

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

### Changed

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
