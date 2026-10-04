# 0159 — A scene is shown once it has arrived, and a script can say when that is

- Status: accepted
- Date: 2026-10-03
- Decided by: the agent, on the owner's request through ludwerk-08
  (2026-10-03, the horde test game), under the standing rule to decide as
  professional engines do and record it
- Amends: D507's loading curtain, which covered the first scene only

## Context

The owner's horde game builds its world at run time: its scene's scripts write
the terrain's heights when it loads and make the map's parts from code. After
the menu's Play button:

1. the new scene appeared empty, and the map popped in as the scripts built it;
2. then the ground meshed in a few nodes at a time while he was already
   playing.

The causes:

- **The terrain is meshed at a play-time pace.** The loader builds four nodes
  a batch and puts up six meshes a frame, which keeps a frame of play smooth.
  It does the same while there is no play to keep smooth.
- **Nothing a script can see says the ground is meshed.** The loader knows
  (`pending()`), and only tests asked it. `StreamingService:LoadAreaAsync`
  waits for the ground to be resident, which ground a script has just written
  already is, so it answers at once.
- **The loading curtain is raised once.** D507's curtain hides the first
  scene's arrival, and lifts when the mesh and texture loaders are idle. It is
  never raised again on a scene change, and it never asks about the terrain.
- **`LoadSceneAsync` cannot wait for what a scene's scripts build.** Its
  `Ready` covers the file and the meshes the file names.

## How mature engines do it

- **Unreal** keeps its loading screen up after a level change until World
  Partition has streamed the cells round the player. A game can keep it up
  longer.
- **Unity** loads a scene asynchronously with activation held
  (`allowSceneActivation`). A game's own loading screen stays up until the
  game says it is ready.
- **Godot** leaves the loading screen to the game. It gives the game the
  pieces to know when the resources are in.

The common shape: the engine has a default notion of "arrived", and the game
may hold the screen past it.

## Decision

1. **The curtain is raised for every scene**, not only the first. On a game's
   window, never the editor's:
   - at boot;
   - whenever `EngineState::sceneLoads` changes: every `LoadScene`, and a
     `LoadSceneAsync` once it finishes.

   It is the backdrop the world is drawn behind. The game's interface draws
   over it: a `ScreenGui` with `KeepOnSceneLoad`, or the new scene's own. That
   is the game's loading card.
2. **It lifts when the scene has arrived** (`LoadingCurtain`). That means
   three frames running in which all of these hold:
   - the mesh and texture loaders have nothing in flight;
   - the ground within 96 m of the camera is meshed;
   - no script holds it.
3. **Or after waiting long enough**: ten seconds, or a minute while a script
   holds it. When that happens the engine logs a warning that names the holds
   still in place and whether the ground was meshed, and sets the holds back to
   none.
4. **Behind the curtain the ground is built flat out.** Two hundred and
   fifty-six nodes a batch, and everything a batch built put up at once (the
   budget a screenshot run already had). The play-time budget comes back when
   the curtain lifts.
5. **Meshed has one meaning** (`TerrainLoader::areaMeshed`): every node the
   last `sync` wanted within the radius is drawable at the level it is wanted
   at, built for the ground as it is now, **and the loader wants nothing more
   anywhere**. The loader records each node it wanted and could not draw yet,
   as a box in world space, and asks whether any of those boxes reaches the
   sphere. The second condition is there because the levels are refined one
   `sync` at a time, and a node put up beside the area changes the levels
   and seams of the nodes in it. An area with nothing missing in one frame was
   measured missing ground again in the next, a dozen times while a terrain
   settled. With no view nothing is wanted, and the answer is yes: there is
   nothing to wait for.
6. **The script API**:
   - `Terrain:IsMeshed(center, radius) -> boolean`, as of the last frame.
   - `Terrain:WaitForMeshAsync(center, radius, timeout = 30) -> boolean` yields
     until the area is meshed and returns true, or returns false once
     `timeout` seconds of simulation have passed. It is answered at the end of
     a frame, at the same safe point as `LoadAreaAsync`.
   - `SceneService:HoldLoading()` and `ReleaseLoading()` keep the curtain up
     past "arrived", counted. More releases than holds are ignored.
   - A hold is the world's (`EngineState::loadingHolds`), not a script's. A
     global script's loading card can hold across a scene change, which is
     the reason for it, and the minute's limit is what guards a hold that is
     never released.

## Consequences

- A game that builds its world from code shows it whole, from behind its own
  loading card, without writing a loading system. A game with nothing to
  build sees the curtain for three frames of a scene change.
- The terrain's detail near the camera is right on the first frame shown,
  where it used to arrive in sixes. The cost is a few frames of loading at the
  speed the machine can build, behind a picture that was empty anyway.
- `LoadAreaAsync` keeps its meaning: resident, the parts' cells and the
  ground's. A script that wants the ground drawn asks `WaitForMeshAsync`
  after it.
- Tests:
  - `areaMeshed` is false while the loader still wants ground there, and true
    once it has drawn it (`terrain_loader_tests.cpp`);
  - the curtain lifts only once the loaders are idle, the ground is meshed and
    no hold is left, and gives up at ten seconds, or a minute with a hold
    (`frame_pacing_tests.cpp`);
  - `WaitForMeshAsync` resumes only when the host says the area is meshed, or
    at its timeout (`world_host_tests.cpp`).
