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

- [ ] **Per-view renderer state.** Exposure history, the shadow fit, the
      environment chain and the screen-sized targets are keyed by a view id
      instead of held once (`renderer_default.cpp`: `ensureTargets`, the
      "history of one view" block). The main view is view 0 and behaves exactly
      as today.
- [ ] **Draw a `RenderWorld` into any target at any size** in the same command
      list as the main view, before it. `HostPreviewRenderer` is the precedent;
      fold it onto the new path rather than keeping two.
- [ ] **Pipelines for the view format**: views draw `Rgba8Unorm`; if the main
      target's format differs, the pipelines exist for both.
- [ ] **`view://` textures**: a registry in `render` (name -> texture handle,
      size, owner) that `TextureLibrary` and the UI image provider both consult.
      A name nothing has drawn into is black, not the error texture. Duplicate
      names: first wins, a keyed warning names both.
- [ ] **Budgets**: `[render] max_views_per_frame` (4), `max_view_resolution`
      (1024), `max_sub_worlds` (2) in `project.toml`, documented in
      `api-design.md`'s `project.toml` section. The scheduler draws the views
      whose picture is oldest first.
- [ ] F3: a *Views* section -- each view's name, size, last drawn frame, and
      time.
- [ ] Tests: two views of one world in one frame, each with its own exposure
      (a bright and a dark camera converge separately); a `view://` name
      resolves in a material and in an `ImageLabel`; the budget draws four of
      six and the other two next frame; the main view's pixels are unchanged by
      the existence of the machinery (reference screenshot).

## Stage V1 — `CameraTexture`, and a camera game

- [ ] IDL: `CameraTexture` (`Camera`, `ViewName`, `Resolution`, `Enabled`,
      `UpdateInterval`, `Quality`); `Enum.ViewQuality` (`Full`, `Simple`),
      appended. Accessors, `.d.luau`, the reference page, an editor icon.
- [ ] Extraction per camera through `ViewOverride`; `Simple` quality skips
      shadows and post.
- [ ] A camera that sees its own texture samples last frame's picture.
- [ ] Replicates as an ordinary instance; a dedicated server draws nothing
      (and creates no texture).
- [ ] **The example**: `examples/24-security-cameras` (or the next free
      number). A small building, six cameras, a monitor wall in the office
      (`Decal`s showing `view://cam1` to `view://cam6`, `UpdateInterval = 2`),
      and a tablet in the `ScreenGui` that shows one feed full-screen and
      switches between them -- the owner's case, playable, with the frame time
      recorded in `docs/perf-baselines.md`.
- [ ] Tests: the feed shows what the camera sees (a coloured part in front of
      it reads back as that colour); `Enabled = false` keeps the last picture
      and draws no view; `UpdateInterval = 3` draws on one frame in three.

## Stage V2 — `ViewportFrame`

- [ ] IDL: `ViewportFrame : GuiObject` (`CurrentCamera`, `Ambient`,
      `LightColor`, `LightDirection`, `Quality`). Parts and models may be
      parented under it (update `canParentInto` and the Explorer).
- [ ] **Its instances are drawn only in it**: the main extraction skips them
      (they are not under the `Workspace`, so physics already does), and the
      frame extracts with itself as the root.
- [ ] **Redrawn only when dirty**: a change to anything inside, to the camera,
      or to the frame's size marks it; otherwise the last picture is reused.
      With no `CurrentCamera`, a camera that frames the contents' bounds.
- [ ] Works inside a `SurfaceGui`.
- [ ] An inventory in an example (the camera game's tablet, or
      `examples/18-world-ui`): items turning in slots.
- [ ] Tests: forty still frames draw forty views once and zero after; turning
      one redraws one; a part inside is not simulated (it does not fall).

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

(Filled in as the work finds things.)
