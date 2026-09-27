# 0107 — A camera draws into a texture, a frame draws its own instances, and a scene runs beside another

- Status: accepted (to be built after the ledger of ADRs 0104 to 0106; see
  `docs/briefs/views-kickoff.md`)
- Date: 2026-09-26
- Decided by: the owner, on 2026-09-26, in conversation. He asked whether a game
  could show another scene on a wall, the way other engines can, and asked for
  all three of the capabilities below: *"deixe anotado que esses 3 é o que quero
  que nossa engine tenha capacidade de fazer"*. He named the case that has to
  work: a camera game in the style of a security-office horror game, where the
  feeds are shown on monitors.
- Relates to: [0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md)
  (`SceneService` loads a scene INSTEAD of the current one; a sub-world loads
  one BESIDE it), [0105](0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md)
  (which script services a sub-world runs), ADR 0038 (visual fidelity is judged
  against a stated reference).

## Context

What exists on 2026-09-26 (file references are where the survey found them):

- **The editor already draws the world into a texture.** Its Viewport panel is
  an `Rgba8Unorm` texture made `ColorTarget | Sampled` (`editor.cpp`,
  `ViewportTarget::resize`), which the renderer, the debug draw and the game UI
  draw into and the panel shows as an image. The RHI needs nothing new for a
  texture that is drawn into and then sampled.
- **A second, scratch world is already drawn into a texture**:
  `HostPreviewRenderer` extracts a scratch `scene::World` through a
  `ViewOverride` into a `Sampled | ColorTarget` texture, borrowing the main
  renderer.
- **Two whole worlds already run in one process**: `two_worlds.cpp` keeps two
  `WorldHost`s -- two registries, two Luau VMs, two Jolt systems, two renderers
  -- and a test checks that neither disturbs the other's pixels. The script
  runtime and physics are per world.
- **One frame draws one view.** `RenderWorld` holds one `RenderCamera`; a
  renderer carries the history of one view (exposure, the shadow fit, the
  environment chain) and rebuilds its screen-sized targets when the size
  changes. `render::extract` already takes a root and a `ViewOverride`.
- **Textures are named by `asset://` URNs only.** Materials resolve theirs
  through `render::TextureLibrary` (a name-to-handle map with a public `set`);
  UI images through the image provider. Nothing names a texture made at run
  time.
- `SurfaceGui` and `BillboardGui` are drawn directly as world-space quads, each
  run carrying its own texture handle. There is no `ViewportFrame`, no camera
  target, and no way to load a scene next to the current one.

**What already works for a camera game**: showing one feed at a time is
switching `Workspace.CurrentCamera` and drawing the monitor's UI over it. What
does not: several feeds at once (a wall of monitors), a feed on a screen inside
the world, a mirror, a scope, a minimap, an item turning in an inventory slot,
a playable arcade cabinet inside the game.

The engines surveyed do the same three things. A camera renders into a texture
asset that any material samples. A scene-capture component renders into a
render target that a material samples. A sub-viewport owns a world of its own
-- physics and all -- and shows as a texture on any surface. The engine the
owner knows has a UI frame that draws the models placed inside it, lit simply
and not simulated. This decision takes all three shapes.

## Decision

### 0. A texture made at run time has a name: `view://`

- **`view://<name>`** names a texture something draws into at run time. It is
  accepted wherever a texture URN is: `ImageLabel.Image`, `ImageButton.Image`,
  `Decal.Texture`, a material's maps and a surface shader's texture parameter.
- The name is the drawer's `ViewName` property (below), so a material file can
  say `"colorMap": "view://lobby-tv"` and whatever draws into `lobby-tv` shows
  there. Two drawers with one name: the first made wins, and the second logs a
  keyed warning naming both.
- Until something has drawn into it, a `view://` texture is black, not the error
  texture: a screen that is off.
- It resolves through the same `TextureLibrary` and image provider as any other
  texture; nothing that samples one knows the difference.

### 1. `CameraTexture`: a camera draws into a texture

A new instance, `CameraTexture`, anywhere in the world or in `ReplicatedStorage`:

| Property | What it is |
|---|---|
| `Camera: Camera?` | the camera it draws from; nil draws nothing |
| `ViewName: string` | the `view://` name it draws into |
| `Resolution: Vector2` | the texture's size, in pixels; capped (1024 on a side by default, `[render] max_view_resolution`) |
| `Enabled: boolean` | off keeps the last picture and costs nothing |
| `UpdateInterval: number` | draw every N frames (1 = every frame); a wall of monitors at 4 is a quarter of the cost |
| `Quality: Enum.ViewQuality` | `Full` (shadows, post), `Simple` (no shadows, no post) -- the default for a feed |

- **Any number may exist; a budget draws them.** At most
  `[render] max_views_per_frame` (4 by default) are drawn in one frame, oldest
  picture first, so a wall of twelve monitors round-robins instead of costing
  twelve views. The F3 overlay shows the views drawn and their time.
- **Drawn before the main view**, so a feed in the world shows this frame's
  picture. A camera that sees its own texture sees last frame's: no recursion.
- **Each view has its own history** (exposure, shadow fit): the renderer's
  per-view state is keyed by view, not held once.
- **Rendering only**: nothing it does reaches the simulation, so no determinism
  trace moves, and a dedicated server draws nothing. It replicates like any
  instance, so a server can place the cameras of a camera game and every client
  draws its own feeds.
- **A mirror and a portal** are a `CameraTexture` whose camera a script moves
  every frame (reflected across the mirror's plane; placed behind the far
  portal). The engine adds what a script cannot do: `Camera.ClipPlane`, an
  oblique near plane, so the wall behind a mirror is not drawn into it. Helpers
  in `@luaug/views` (`views.mirror(part)`, `views.portal(a, b)`) wrap the
  scripting.

### 2. `ViewportFrame`: a UI element that draws its own instances

A new `GuiObject`, `ViewportFrame`, for an item turning in an inventory slot, a
character preview, a 3D icon:

| Property | What it is |
|---|---|
| `CurrentCamera: Camera?` | a `Camera` inside the frame; nil frames what is inside automatically |
| `Ambient: Color3`, `LightColor: Color3`, `LightDirection: Vector3` | its own simple lighting |
| `BackgroundTransparency` (inherited) | 1 shows only the instances, over whatever is behind |

- **The instances inside it are its world**: parts and models parented under a
  `ViewportFrame` are drawn in it and nowhere else. They are not under the
  `Workspace`, so nothing simulates them -- no physics, no collisions -- and a
  script moves them by setting `CFrame`, which is exactly what a turning item
  wants.
- **Drawn only when something inside changed** (a property, the camera, the
  frame's size): an inventory of forty still items costs forty pictures once,
  not forty views a frame. Its texture is the frame's `AbsoluteSize` times
  `DisplayScale`.
- **Simple lighting by default**, no shadows; `ViewportFrame.Quality =
  Full` asks for the world's environment lighting when a character preview
  wants it.
- It works in a `SurfaceGui` too, so a 3D model can turn on a screen in the
  world.

### 3. `SubWorld`: a scene runs beside the current one

A new instance, `SubWorld`, for a playable arcade cabinet, a game on a
computer screen inside the game, a snow globe with its own weather:

| Member | What it is |
|---|---|
| `Scene: string` | the scene file it runs (`scenes/arcade.scene.json`) |
| `ViewName: string` | the `view://` name its current camera draws into |
| `Resolution: Vector2`, `Quality`, `UpdateInterval` | as `CameraTexture` |
| `Running: boolean` | false pauses its simulation (its picture stays) |
| `Load()` / `Unload()` | start and stop it; `Loaded` fires when it is running |
| `SetInputState(action, value)` | drives one of ITS input actions -- the host game decides what reaches it |
| `Send(...)`, `Received: (...)` | messages between the two games, plain values only |

- **A separate world, completely**: its own `WorldHost` -- registries, Luau VM,
  physics, navigation, its own deterministic tick and world hash -- exactly as
  `two_worlds.cpp` already runs two. Nothing in it can reach the host's
  instances, and nothing in the host can reach its instances: they talk through
  `Send` and `Received` (inside the sub-world, `SceneService.HostMessage` and
  `SceneService:SendToHost(...)`), and the host drives its input.
- **It runs the scene's own scripts as a solo game**: that scene's
  `ServerScriptService` and `ClientScriptService`. Not the project's
  `GlobalScriptService` -- that is the host game's (ADR 0105). A scene that
  needs shared code requires it from `ReplicatedStorage`.
- **It ticks with the host**: one sub-world tick per host tick, at the host's
  fixed step, so a replay of the host replays the sub-world too.
- **Its scene is loaded by the same path `SceneService` uses** (ADR 0106 §2),
  on the sub-world's own `WorldHost`. The ADR 0106 work keeps that path scoped
  to one `WorldHost`; this depends on it.
- **Local only, in this decision**: a sub-world does not replicate. In a match,
  each client may run its own (an arcade each player plays alone). A sub-world
  shared by players is later, and its own decision.
- **Budget**: at most `[render] max_sub_worlds` (2 by default) running at once,
  because each is a VM, a physics system and a renderer's state. A third
  `Load()` raises a keyed error naming the limit.
- A sub-world can itself hold a `CameraTexture` or a `ViewportFrame`. It cannot
  hold a `SubWorld`: one level deep.

## Consequences

- **A camera game is ordinary**: cameras placed in the world, one
  `CameraTexture` per monitor, `ImageLabel`s or `Decal`s showing `view://`
  names, a budget that keeps a wall of monitors affordable.
- **Mirrors, scopes, minimaps, rear-view mirrors and portals** are a
  `CameraTexture` and a few lines of script, with `@luaug/views` for the common
  two.
- **The renderer learns to draw more than one view a frame**, which is the one
  piece of engine work all three share -- and the one split-screen local
  multiplayer would build on, later, as its own decision.
- **Performance is visible**: every view is a line in the F3 overlay with its
  time, and the budgets are settings, not constants.
- **No determinism trace moves** for 1 and 2 (rendering only). A `SubWorld` has
  a trace of its own and leaves the host's alone.

### Rejected

- **A camera that draws straight into a material with no name.** A material is
  a file; it cannot hold a reference to an instance. A name both can agree on is
  what lets a material file show a live feed.
- **`ViewportFrame` instances that are simulated.** A preview that falls under
  gravity is not a preview; a frame that wants physics is a `SubWorld`.
- **A sub-world that shares the host's VM or registries.** Isolation is the
  point -- an arcade game's bug cannot touch the host game -- and two full
  `WorldHost`s already run side by side.
- **Drawing every `CameraTexture` every frame.** A wall of monitors would cost a
  view each; a budget and `UpdateInterval` make it a choice.
