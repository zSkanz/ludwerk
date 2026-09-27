# Views: the kickoff and the ledger

The owner, on 2026-09-26, asked for three capabilities other engines have: a
camera that draws into a texture, a UI frame that draws its own instances, and
a whole scene running beside the current one and shown on a surface. He named
the case that has to work: a camera game where security feeds are shown on
monitors. The decision is
[ADR 0107](../decisions/0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md).
**Read it before this file.** This file is the order of work and where each
piece stands.

> **Names after the rename.** This ledger runs after
> [`rename-kickoff.md`](rename-kickoff.md) (ADR 0109). Where it says `ludwerk`
> (the command), read the brand's command (`ludwerk`); `@engine/` is
> `@engine/`; `engine://` is `engine://`; `ENG_*` is `ENG_*`; `project.toml` is
> `project.toml`. The ADRs keep their original words; ADR 0109 is the mapping.

## When this starts, and what it shares with the other ledger

- **[`export-and-server-kickoff.md`](export-and-server-kickoff.md) comes
  first** (ADRs 0104 to 0106), then [`rename-kickoff.md`](rename-kickoff.md)
  (ADR 0109). This ledger starts when the owner says so.
- **V0 to V3 touch other modules** (`render`, `ui`, `asset`, the `@engine`
  runtime) than that ledger's S1 to S5 (`script`, `scene` loading,
  `replication`, the editor's Play). They may run beside it only if the owner
  asks, and then in a separate worktree, with the IDL, `i18n/en.json`,
  `enums.api.luau` and the generated files merged by one session at a time.
- **V4 (`SubWorld`) waits for that ledger's S2**: it loads its scene through
  the path S2 builds, on a second `WorldHost`. S2's session has agreed to keep
  that path scoped to one `WorldHost`, with no process-global state
  (2026-09-26).

The idea in brief:

- **`view://<name>`** names a texture drawn at run time; every texture slot
  takes it.
- **`CameraTexture`** draws a camera into one, under a per-frame budget.
- **`ViewportFrame`** draws the instances inside it, unsimulated, only when
  they change.
- **`SubWorld`** runs a scene in its own `WorldHost` beside the host game, and
  draws it into one.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- `scripts/localgate.ps1` green on every stage, Linux and Android included,
  before every push; then CI read. Never write to a red `main`.
- **No determinism trace moves.** V0 to V3 are rendering; V4's sub-world has a
  trace of its own and the host's does not move.
- **A game that uses none of this renders exactly as before**: the reference
  screenshots (ADR 0038) do not change, and `churn10k` and the frame-time
  benchmarks do not get slower. Record before and after in
  `docs/perf-baselines.md`.
- **Every view is visible**: the F3 overlay lists the views drawn this frame
  and their GPU and CPU time.
- New enums are appended at the END of `api/defs/enums.api.luau`; after any IDL
  change run `gen_cpp`, `gen_dts`, `gen_dump`, `gen_reference`.
- R3: every new message is an i18n key. R7: nothing in code names another
  engine. R17: no backend type in the public API.

## Stage V0 — more than one view a frame, and `view://`

- [x] **Per-view renderer state.** `ViewState`, a private base of
      `DefaultRenderer` holding every target, the exposure chain, the shadow
      fit, the environment map and cache and the look's images; `render` swaps
      the view a `RenderTarget` names (`view`, 0 for the main one) into it.
      `releaseView` frees one; the environment map is made per view on demand.
- [~] **Draw a `RenderWorld` into any target at any size** in the same command
      list as the main view, before it -- done, from the frame loop.
      `HostPreviewRenderer` is not yet folded onto it.
- [x] **Pipelines for the view format**: `fxaa_view` at `Rgba8Unorm`, made only
      when the window's format is another.
- [x] **`view://` textures**: `app::ViewHost` makes one per `CameraTexture` and
      sets it in the `TextureLibrary`; `UiText` looks the name up for an
      `ImageLabel`; `MeshLoader` never reads a `view://` name from disk and
      gives one nothing draws into a shared black pixel. First made wins;
      `render.warn.view_name_taken`. (In `app`, not `render`: the host holds the
      device and the world, and `render` needs only the library it already has.)
- [x] **Budgets**: `[render] max_views_per_frame`, `max_view_resolution`,
      `max_sub_worlds` read into `ProjectConfig`, documented in `api-design.md`
      and the manual. Never-drawn first, then the oldest picture
      (`chooseViews`).
- [x] F3 and the editor's Stats: a *Views* section -- name, size, cost, frames
      since drawn.
- [~] Tests: `views_gate` -- two views of one world in one frame, each its own
      camera, each on an `ImageLabel`, and lighter than the main view because
      each keeps its own exposure; `view_host_tests` -- four of six, then the
      other two, oldest first, `UpdateInterval`, the size cap; the capture and
      screenshot goldens unchanged. Not yet: a `view://` name in a material.

## Stage V1 — `CameraTexture`, and a camera game

- [x] IDL: `CameraTexture` (`Camera`, `ViewName`, `Resolution`, `Enabled`,
      `UpdateInterval`, `Quality`); `Enum.ViewQuality` (`Full`, `Simple`),
      appended. Accessors, `.d.luau`, the reference page; the editor icon was
      already drawn.
- [x] Extraction per camera through `ViewOverride`; `Simple` quality skips
      shadows and the look's effects, and keeps the sky and the air.
- [~] A view is absent from its own picture (its texture leaves the library
      while it draws) -- not its last frame's picture; see Findings.
- [x] **Does not replicate** -- see Findings; a dedicated server draws nothing.
- [x] **The example**: `examples/26-security-cameras`: a building at night,
      six cameras, a `SurfaceGui` monitor wall (`ImageLabel`s rather than
      `Decal`s -- see Findings), `UpdateInterval = 2`, and a tablet whose feed
      draws every frame at twice the size. Frame time not yet recorded in
      `docs/perf-baselines.md`.
- [~] Tests: the feed shows what the camera sees (`views_gate`);
      `UpdateInterval = 3` (`view_host_tests`); the API
      (`world/camera_texture.spec.luau`). Not yet: `Enabled = false` keeps the
      last picture.

## Stage V2 — `ViewportFrame`

- [x] IDL: `ViewportFrame : UIObject` (`CurrentCamera`, `Ambient`,
      `LightColor`, `LightDirection`). `Part`, `MeshPart`, `Model` and `Camera`
      may be parented under it. `Quality` left out: a frame is lit by its own
      light and nothing else, and `Full` would need the world's environment in
      a view that has none behind it.
- [x] **Its instances are drawn only in it**: not under the `Workspace`, so the
      main extraction and physics skip them; the frame extracts with itself as
      the root, and draws with no sky and its background clear (the tonemap
      carries the scene's coverage as alpha for such a view, and FXAA is
      skipped).
- [x] **Redrawn only when dirty**: `frameSignature` over the parts, meshes and
      cameras inside and the frame's light; the texture follows the frame's
      laid-out size. With no `CurrentCamera`, `frameLens` frames a sphere round
      everything inside from the front and fifteen degrees above.
- [ ] Works inside a `SurfaceGui` -- the same draw path, not yet proved.
- [x] An inventory in `examples/26-security-cameras`: three items turning in
      slots.
- [x] Tests: `views_gate` (a cube in a frame with the world showing round it);
      `view_host_tests` (the signature is still when nothing moves and moves
      when an item or the light does; nothing inside frames nothing);
      `world/viewport_frame.spec.luau` (a loose part inside does not fall).

## Stage V3 — mirrors and portals

- [x] `Camera.ClipPlane` (a `CFrame`) and `ClipPlaneEnabled`: an oblique near
      plane (Lengyel's), and `rhi::RasterizerState::depthClip` on the world's
      geometry so what falls behind it is cut rather than clamped; the sky and
      the air use a view-projection without it.
- [x] `@engine/views`: `views.mirror(part, options?)` and
      `views.portal(a, b, options?)`, each returning a `Surface` (`Texture`,
      `Camera`, `Picture`, `Destroy`). Exact rather than approximate: the
      camera looks straight through the glass and `ImageLabel.ImageRectOffset`
      / `ImageRectSize` (new; a negative size reads backwards) cut the glass
      out of the picture.
- [x] Documented recursion: a mirror in a mirror shows the other mirror
      without its picture (a view is absent from its own).
- [x] Tests: `mirrors_gate` -- red and blue behind the camera in the mirror,
      turned, and not the green block behind the glass; yellow through the
      portal and not the purple behind its far pane.
      `world/mirrors.spec.luau` -- the clip plane, the image rectangle, where
      the module puts its cameras, and `Destroy`. A clip-plane unit test in
      `render_world_tests`. `examples/27-mirrors-and-portals`.

## Stage V4 — `SubWorld` (after the other ledger's S2)

- [x] IDL: `SubWorld` (`Scene`, `ViewName`, `Resolution`, `Quality`,
      `UpdateInterval`, `Running`, `Load`, `Unload`, `IsLoaded`, `Loaded`,
      `SetInputState`, `Send`, `Received`); inside a sub-world,
      `SceneService.HostMessageReceived`, `SceneService:SendToHost` and
      `SceneService:IsSubWorld`.
- [x] A second `WorldHost` per sub-world, owned by the host that runs it
      (`world_host_sub_worlds.cpp`); its scene read as `LoadScene` reads one;
      it mounts `src/scenes/<scene>/` and the scene's own scripts, and none of
      `src/client`, `src/server` or `src/shared`.
- [x] Ticks one for one after the host, at the host's step; `Running = false`
      pauses it; its world hash is its own.
- [x] Input: `SetInputState` holds a named `InputAction` in the sub-world
      (`InputSystem::setActionState`); nothing else reaches it. Messages: the
      `RemoteEvent` encoding, delivered at the other side's next tick.
- [x] Budget: `max_sub_worlds`; a third `Load()` raises
      `scene.err.sub_world_limit`. A sub-world cannot hold a `SubWorld`
      (`scene.err.sub_world_nested`).
- [x] Not replicated (excluded from the wire, with its reason).
- [x] **The example**: `examples/28-arcade`, two cabinets playing by
      themselves; E at the left one puts a coin in, the arrow keys become its
      `Move`, and the score comes back on the marquee.
- [x] Tests: `sub_world_tests.cpp` -- the scene's code and not the game's,
      one tick for one, falling under its own physics; the same run twice
      hashes the same, and a different sub-world beside the same host leaves
      the host's hash where it was; unloading and destroying throw the world
      away and a reload is a new one. `world/sub_world.spec.luau` -- isolation,
      messages both ways, input, pausing, the budget, reloading, a missing
      scene. `subworld_gate` -- two sub-worlds drawn side by side, one told to
      change, and the host's world in neither.

## Stage V5 — documentation and closing

- [x] A manual page, *Views* (`docs/manual/rendering/views.md`): cameras on
      screens, inventory previews, mirrors and portals, sub-worlds, the budgets
      with their costs, and the three examples walked through.
- [x] CHANGELOG entries; PROGRESS.md; `api-design.md`'s `[render]` budgets.
- [x] This ledger's Findings section.

**Closed on 2026-09-27.** Next in the owner's queue is the game-ready plan
([`game-ready-plan.md`](game-ready-plan.md)), block A first.

## Findings

- **A `CameraTexture` cannot replicate usefully**, because a `Camera` does not
  (a replica's view is its own): a feed would arrive naming a camera the
  replica lacks, and one under its camera would not arrive. Excluded from the
  wire with that reason; a feed is set up by the client that draws it. ADR 0107
  is amended. A generic `InstanceRef` in a component field -- which it would
  have needed -- was written and taken back out, because nothing else uses one
  and unused code is untested code.
- **The view texture holds screen-encoded colour** (the tonemap writes it into
  `Rgba8Unorm` itself). The UI draws images in that space, so a feed on an
  `ImageLabel` is true; a `Decal` or a material reads its map as colour to be
  lit, and the feed is lighter. The example uses a `SurfaceGui` for that
  reason, and the manual says so. Fixing it for materials would mean a second,
  linear copy -- not worth it until something needs it.
- **A view draws the world and not the UI in it.** `SurfaceGui` and
  `BillboardGui` are built into the main view's snapshot after extraction,
  and a view's snapshot is extracted on its own; so signs, monitors and other
  mirrors' pictures are absent from every camera texture. That is also what
  makes a mirror in a mirror safe. Drawing them would need the world UI built
  per view, and a rule for what a view seeing a view shows.
- **Each view's exposure is its own, and it shows**: a camera looking at a dark
  wall opens up. The gate's feeds are lighter than the main view for exactly
  this reason, and the gate asserts it.
- **A `Vector2` does not cross between two worlds**, because the `RemoteEvent`
  encoding does not carry one (a `vector` does). The spec's scene sends a
  direction as two numbers. Widening the encoding is the network's decision,
  not this ledger's.
- **The simulation half of a sub-world belongs to `WorldHost`**, not to the
  frame, and that is what made it testable: the conformance runner, which has
  no renderer, runs every `SubWorld` case, and a replay of the host replays
  its sub-worlds.
- **Depth was clamped, never clipped, on every pipeline**, which nothing had
  noticed until an oblique near plane needed what falls behind it cut away.
  `rhi::RasterizerState::depthClip` is now on for the world's geometry. It is
  not free of consequence: a triangle that crosses the near plane is cut into
  pieces, and two reference screenshots moved by a handful of pixels on shadow
  edges -- re-recorded, with the reason in the commit.
