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

- [ ] `Camera.ClipPlane` (a plane in world space, or none): an oblique near
      plane so what is behind a mirror or a portal is not drawn into it.
- [ ] `@engine/views`: `views.mirror(part: BasePart): CameraTexture` and
      `views.portal(a: BasePart, b: BasePart)`, which make the camera and move
      it every frame, reflected or offset from `Workspace.CurrentCamera`.
- [ ] Documented recursion: a mirror in a mirror shows last frame's picture.
- [ ] Tests: a part behind the mirror's plane is absent from its texture; the
      portal's texture matches a camera placed at the far side.

## Stage V4 — `SubWorld` (after the other ledger's S2)

- [ ] IDL: `SubWorld` (`Scene`, `ViewName`, `Resolution`, `Quality`,
      `UpdateInterval`, `Running`, `Load`, `Unload`, `Loaded`,
      `SetInputState`, `Send`, `Received`); inside a sub-world,
      `SceneService.HostMessage` and `SceneService:SendToHost`.
- [ ] A second `WorldHost` per sub-world (registries, VM, physics, navigation),
      as `two_worlds.cpp` builds them; its scene through S2's load path; it
      runs the scene's `ServerScriptService` and `ClientScriptService` as a
      solo game, and not the project's `GlobalScriptService`.
- [ ] Ticks one for one with the host at the host's fixed step; `Running =
      false` pauses it; its world hash is its own.
- [ ] Input: the sub-world's `InputService` reads only what `SetInputState`
      gives it. Messages: plain values in the `RemoteEvent` encoding, delivered
      at the next tick on the other side.
- [ ] Budget: `max_sub_worlds`; a third `Load()` is a keyed error. A sub-world
      cannot hold a `SubWorld`.
- [ ] Not replicated: a `SubWorld` on a client runs there alone.
- [ ] **The example**: an arcade cabinet in the camera game (or its own
      example): walk up, press E, the cabinet's screen takes the input and
      plays a small 2D game; its score comes back through `Received`.
- [ ] Tests: the host's trace is unchanged with a sub-world running; the
      sub-world's own trace is stable across runs; a script in the sub-world
      cannot find a host instance; `Send`/`Received` round-trip; unloading
      frees its VM and physics (memory back within a margin).

## Stage V5 — documentation and closing

- [ ] A manual page, *Views*: cameras on screens, the camera game walked
      through, mirrors and portals, inventory previews, sub-worlds, and the
      budgets with their costs.
- [ ] CHANGELOG entries; PROGRESS.md; `api-design.md`.
- [ ] This ledger's Findings section.

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
- **Each view's exposure is its own, and it shows**: a camera looking at a dark
  wall opens up. The gate's feeds are lighter than the main view for exactly
  this reason, and the gate asserts it.
