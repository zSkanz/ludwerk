# 0125 — A scene is prepared in the background and activated when the game says

- Status: accepted, built 2026-09-28 with 0124 (`docs/briefs/foundation-kickoff.md`, A2c). As built:
  meshes are warmed and nothing else yet, `[scene] max_prepared_bytes` is not read, clients in a match load
  at the switch rather than preparing ahead, and `LoadScene` keeps its own path (same tick as before).
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, in conversation about loading screens,
  choosing this shape and asking for it to be built together with ADR 0124:

  ```luau
  local loading = SceneService:LoadSceneAsync("scenes/fase2.scene.json", { Activate = false })
  loading.Progress  -- 0..1, for a progress bar
  loading.Ready:Wait()      -- loaded in the background, not yet in
  fadeOut()
  loading:Activate()        -- now the switch happens (scene:BindToClose runs here)
  ```

- Amends: [0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md)
  ("loading the next scene in the background while the old one plays is later,
  not now" — this is that later).
- Builds on: [0124](0124-a-scene-closes-as-a-game-does-and-a-handler-dies-with-its-script.md)
  (the scene's close runs at activation), [0075](0075-terrain-and-block-worlds-stream-in-cells.md)
  (streaming), the asset system's asynchronous I/O (M7).

## Context

`LoadScene` records a request, and at the tick's safe point
`WorldHost::loadScene` does everything **on the main thread, at once**: reads
the file, lifts out a `KeepOnSceneLoad` screen, destroys the old scene, parses
and builds the new one, starts its scripts and fires `SceneLoaded`. The game
stops for that tick: unnoticed for a small scene, a hitch for a large one, and a
loading screen's animation freezes through it. Meshes and textures already
arrive asynchronously and terrain streams in, so the hitch is the parse and the
instantiation — and then assets popping in after the switch.

Every engine a person would compare this one to has the same four pieces: a
layer that survives the change (here `GlobalScriptService` and
`KeepOnSceneLoad`, already built), loading in the background with progress,
holding the activation until the game says, and warming assets first. The last
three are what this record adds.

## Decision

### 1. The call and its handle

- `SceneService:LoadSceneAsync(path, options?): SceneLoad` returns at once and
  starts preparing `path` while the current scene keeps running.
  `options.Data` is what `LoadScene`'s `data` is; `options.Activate`
  (default `true`) says whether to switch as soon as it is ready.
- `SceneLoad` (not creatable, not in the tree):
  - `Scene` — the prepared scene's `Scene` object (ADR 0124): messages sent
    to it queue until it opens;
  - `Path`, `Progress` (0 to 1), `Status` (`Enum.SceneLoadStatus`: `Preparing`,
    `Ready`, `Activating`, `Done`, `Failed`, `Cancelled`), `Error` (a string when
    `Failed`);
  - `Ready: Signal<>` and `Finished: Signal<boolean>` (deferred, R8);
  - `Activate()` — switch now if ready, or as soon as it is; `Cancel()` — drop
    what was prepared.
- `LoadScene(path, data?)` stays and becomes `LoadSceneAsync` with
  `Activate = true` and no handle to watch: nothing that calls it changes.
- One load at a time: a second `LoadSceneAsync` cancels the first, and says so
  in the log.

### 2. Preparing, off the main thread

- The file is read and parsed **on a job**, into a detached representation of
  the scene (its instances, properties, scripts' sources and references), not
  into the live `World`, which only the main thread touches.
- **Assets are warmed** from what the parse names: meshes, textures, materials,
  sounds, and the terrain and block cells within `[scene] warm_radius` of the
  scene's spawn (or its camera). They are loaded and uploaded to the GPU through
  the existing asynchronous path and held by the handle.
- `Progress` is measured, not faked: the parse's bytes, then the warmed assets'
  bytes, over their totals, rising monotonically.
- **A budget**: while a load is held, the assets of two scenes are resident.
  `[scene] max_prepared_bytes` (default a quarter of the asset budget, lower on
  Android) caps what is warmed; past it, the rest arrives as it does today, by
  streaming after the switch. The F3 overlay and Stats show a prepared scene and
  its bytes.

### 3. Activating

- `Activate()` takes effect at the next safe point between ticks, as
  `LoadScene` does: `SceneLoading` fires, **the old scene's
  `scene:BindToClose` handlers run and are waited for** (ADR 0124), saves are
  flushed, the old scene is torn down, and the prepared scene is instantiated
  from its representation — with its assets already resident, which is what
  makes this step short. Then its scripts start and `SceneLoaded` fires.
- Instantiation stays on the main thread and inside one safe point; what moved
  off it is the reading, the parsing and the assets.

### 4. Around it

- **Determinism (R10)**: preparing enters no simulation state; the switch
  happens at a tick boundary exactly as `LoadScene`'s does, and the replay
  records `Activate` as the input it is.
- **In a match**: only the authority may call it, as `LoadScene` (a client's
  call is a keyed error). When the authority starts one, every client starts
  preparing the same scene from its own package; when the authority activates,
  each client activates at the same tick, waiting (behind its loading screen)
  if it is not ready yet.
- **In a `SubWorld`**: the same call loads that sub-world's next scene.
- **The editor's Play** behaves as an exported game.

## Consequences

- A loading screen that animates, a progress bar with a real number, a fade
  before the switch, and a new scene that does not pop its assets in.
- Memory peaks while two scenes are resident; the budget bounds it.
- The scene reader gains a detached target, which is also what a future
  "additive scene" would need.

## Not decided here

- Additive scenes (two scenes live at once in one world).
- Preloading arbitrary assets from a script (`PreloadAsync`), separate from a
  scene.
