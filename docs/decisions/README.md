# Architecture Decision Records

Every settled decision lives here as one numbered file. ADRs are the project's
decision memory: before re-debating anything, read the ADR; to change a
decision, write a **new** ADR that supersedes the old one (never edit history).

## Process

- Format: MADR-lite (template below). Keep them short — context and
  consequences, not essays.
- Numbering: `NNNN-kebab-title.md`, monotonically increasing.
- Status: `accepted` | `superseded by NNNN` | `proposed` (proposed ADRs are
  escalation items — see `MASTER_PROMPT.md` §10; only a human accepts them).
- **Docs-follow-reality rule:** when implementation legitimately diverges from
  `docs/architecture.md` or `docs/api-design.md`, write the ADR *and* update
  the doc **in the same commit**. A stale spec is a bug.
- ADRs 0001–0030 record the decisions made during the planning phase
  (August 2026), by the user (Skanz) and the planning session, informed by the
  frozen research reports in `docs/research/`.

## Template

```markdown
# NNNN — Title

- Status: accepted
- Date: YYYY-MM-DD
- Supersedes: — (or NNNN)

## Context
Why a decision was needed; the forces at play.

## Decision
What was decided, stated imperatively.

## Consequences
What becomes easier/harder; costs accepted; follow-ups.
```

## Index

| # | Decision |
|---|----------|
| [0001](0001-apache-2-license.md) | Apache-2.0, single license |
| [0002](0002-luau-pinned-direct-embed.md) | Luau 0.734 pinned, embedded directly in the C++ core |
| [0003](0003-lute-unmodified-tooling-runtime.md) | Lute 1.0.0 as the unmodified tooling runtime |
| [0004](0004-sdl3-platform-layer.md) | SDL3 as the platform layer |
| [0005](0005-custom-rhi-sdlgpu-default-bgfx-mobile.md) | Custom RHI; SDL3 GPU default backend; bgfx in the mobile phase |
| [0006](0006-hlsl-via-sdl-shadercross.md) | HLSL authored shaders via SDL_shadercross |
| [0007](0007-jolt-3d-physics.md) | Jolt 5.6 as the default 3D physics backend |
| [0008](0008-box2d-2d-physics-post-v1.md) | Box2D 3.1 for the post-v1 2D layer |
| [0009](0009-miniaudio-module-is-the-seam.md) | miniaudio; the audio module itself is the swappable seam |
| [0010](0010-asset-stack.md) | Asset stack: fastgltf + meshoptimizer + basis/KTX2 + stb; assimp offline-only |
| [0011](0011-imgui-debug-clay-game-ui.md) | ImGui for debug UI; Clay behind Roblox-style in-game UI Instances |
| [0012](0012-networking-v1-primitives-only.md) | v1 networking = low-level primitives only (GNS + ENet behind ITransport) |
| [0013](0013-vector3-native-luau-vector.md) | Vector3 IS the native Luau vector (3-wide, f32) |
| [0014](0014-f64-world-coords-floating-origin.md) | f64 world coordinates + per-region floating origin |
| [0015](0015-deferred-only-signals.md) | Deferred-only signals; no legacy scheduling globals |
| [0016](0016-fixed-tick-rollback-oriented.md) | Fixed-tick deterministic simulation with rollback-oriented foundations |
| [0017](0017-no-editor-v1-code-first.md) | No visual editor in v1; code-first DX |
| [0018](0018-strict-luau-new-solver.md) | All Luau `--!strict` under the new type solver, CI-enforced |
| [0019](0019-i18n-day-one.md) | i18n from day one: key + catalog, English-only launch |
| [0020](0020-clean-room-legal-posture.md) | Clean-room legal posture toward Roblox |
| [0021](0021-vendoring-in-tree.md) | Vendoring: in-tree `third_party/` + manifest + patches |
| [0022](0022-recast-detour-post-v1.md) | Recast/Detour integration deferred post-v1 (seam only) |
| [0023](0023-backend-selection-build-time.md) | Backend selection at build time; explicit factory, no plugin ABI |
| [0024](0024-hot-reload-fast-world-restart.md) | Hot reload: fast world restart is the canonical v1 model |
| [0025](0025-determinism-guarantee.md) | The precise v1 determinism guarantee |
| [0026](0026-child-name-index-duplicates.md) | Child-name index supports duplicate sibling names |
| [0027](0027-irenderer-renderworld-contract.md) | IRenderer contract with RenderWorld as the stable boundary |
| [0028](0028-instance-facade-over-ecs.md) | Instance facade over a hand-rolled deterministic ECS |
| [0029](0029-input-action-system-only.md) | Input Action System is the only input model |
| [0030](0030-std-convergence.md) | Implement Lute's `@std` surface in the game runtime |
| [0031](0031-build-provenance-header.md) | Build provenance header; M0 grounding gate amended |
| [0032](0032-binary-toolchain-artifacts-fetched-not-vendored.md) | Binary toolchain artifacts are fetched and hash-pinned, never vendored |
| [0033](0033-hand-written-json-reader-in-core.md) | One hand-written JSON reader in `core`, not a dependency |
| [0034](0034-luau-casing-objects-modules-files.md) | Luau casing: objects vs modules, and the file-level rule |
| [0035](0035-engine-is-a-websocket-client-of-the-dev-server.md) | The engine is a WebSocket client of the dev server; only the dev server listens |
| [0036](0036-simdjson-vendored-for-fastgltf.md) | simdjson is vendored for fastgltf, and fastgltf's downloader is made unreachable |
| [0037](0037-rhi-interface-frozen-at-m4.md) | The RHI interface is frozen at the end of M4 |
| [0038](0038-visual-fidelity-is-a-v1-target.md) | What the renderer draws is judged against a stated reference, and visual fidelity is a v1 target |
| [0039](0039-input-context-rate-and-total-enums.md) | An InputContext declares its dispatch rate, and the IAS's enums are total |
| [0040](0040-udim2-layout-is-arithmetic-not-a-solver.md) | UDim2 layout is arithmetic, not a constraint problem; v1 does not call Clay |
| [0041](0041-inputservice-gains-a-raw-event-surface.md) | `InputService` gains a raw event surface, fed from the IAS dispatch |
| [0042](0042-vendoring-may-narrow-to-the-paths-a-build-uses.md) | A vendored row may narrow to the upstream paths the build uses |
| [0043](0043-per-instance-vertex-stepping.md) | The frozen RHI gains per-instance vertex stepping, and nothing else |
| [0044](0044-graphics-settings-are-host-settings.md) | Graphics settings are host settings, in three layers |
| [0045](0045-a-packaged-game-is-a-folder-that-ships-source.md) | A packaged game is a folder, and it ships Luau source |
| [0046](0046-the-editor-is-a-mode-of-the-engine-binary.md) | The editor is a mode of the engine binary, drawn in ImGui |
| [0047](0047-the-world-is-data-and-scripts-are-behaviour.md) | The world is data and scripts are behaviour |
| [0048](0048-content-is-the-source-and-an-instance-is-a-link-to-it.md) | Content is the source, an instance is a link to it, and editing breaks the link |
| [0049](0049-a-stamp-is-a-source-and-an-instance-carries-its-mark.md) | A Stamp is a source, an instance carries its mark, and editing it breaks the mark |
| [0050](0050-a-script-is-an-ordinary-instance-and-its-source-is-a-property.md) | A script is an ordinary instance, and its source is a property |
| [0051](0051-a-prefab-is-inherited-and-an-edit-is-an-override.md) | A prefab is inherited, an edit is an override, and a copy is the other thing |
| [0052](0052-the-content-tree-is-what-the-project-holds.md) | The content tree, and why it was taken out the same day |
| [0053](0053-the-grid-decides-when-and-the-model-decides-what.md) | The grid decides *when*, the model decides *what*, and partitioning is the tool's job |
| [0054](0054-the-editor-ships-as-a-folder-and-the-cli-finds-its-own-install.md) | The editor ships as a folder, and the CLI finds its own installation |
| [0055](0055-the-launcher-is-the-engine-with-no-project-open.md) | The launcher is the engine with no project open |
| [0056](0056-the-shell-has-one-theme-and-it-is-square.md) | The shell has one theme, it is data, and it is square |
| [0057](0057-a-script-is-an-instance-and-the-editor-edits-one-thing.md) | A script is an instance, and the editor edits one thing |
| [0058](0058-a-script-runs-when-you-press-play.md) | A script runs when you press play, and not when you open the project |
| [0059](0059-enabled-is-about-resumption-and-nothing-else.md) | `Enabled` is about resumption, and nothing else |
| [0060](0060-a-material-is-an-instance-and-a-stamp-is-how-one-is-shared.md) | ~~A material is an instance, a stamp is how one is shared~~ -- superseded by 0090; a trace moves when the hash's inputs do, and that part stands |
| [0061](0061-a-child-is-not-a-member-and-the-refusal-says-so.md) | A child is not a member, and the refusal says so in three places |
| [0062](0062-a-changed-asset-reloads-itself-and-eval-stays-reserved.md) | A changed asset reloads itself, and `eval` stays reserved |
| [0063](0063-https-stays-refused-and-tls-comes-from-the-platform.md) | `https://` stays refused, and when TLS arrives it comes from the platform |
| [0064](0064-jolt-solves-on-a-fixed-thread-pool.md) | Jolt solves on a fixed thread pool, and the count is part of the hash |
| [0065](0065-a-loose-gltf-is-not-a-runtime-format.md) | A loose `.gltf` is not a runtime format; everything arrives compiled |
| [0066](0066-the-physics-seam-learns-two-static-shapes.md) | The physics seam learns two static shapes: height field and triangle mesh |
| [0067](0067-terrain-is-one-field-with-two-encodings.md) | ~~Terrain is one signed-distance field with two encodings~~ -- superseded by 0082 |
| [0069](0069-replication-reads-state-and-diffs-it.md) | Replication reads state and diffs it, against a declared wire schema; the change queue cannot serve |
| [0070](0070-a-port-is-opened-by-a-posture-and-never-by-a-script.md) | A listening socket is opened by a command-line posture and by nothing else; ADR 0035 narrowed |
| [0071](0071-terrain-ground-is-drawn-from-a-height-atlas.md) | ~~Terrain ground is drawn from a height atlas by a CDLOD grid~~ -- superseded by 0082; the RHI keeps `uploadTextureRegion` |
| [0072](0072-particles-are-a-picture-simulated-on-the-frame.md) | Particles are a picture simulated on the frame, one instanced premultiplied draw; soft by closing the pass, the RHI stays frozen; decals multiply in a pass of their own |
| [0073](0073-colour-textures-reach-the-gpu-as-srgb.md) | Colour textures reach the GPU as sRGB, compiled or loose; the RHI gains sRGB BC formats |
| [0074](0074-jolt-runs-cross-platform-deterministic.md) | Jolt runs cross-platform deterministic at no measurable cost; the engine's transcendentals are what stands between it and level C |
| [0076](0076-replicas-predict-their-own-and-draw-the-rest-between-snapshots.md) | Replicas predict their own character, draw everyone else between snapshots, and are sent only what is near their character |
| [0075](0075-terrain-and-block-worlds-stream-in-cells.md) | Terrain and block worlds stream from disk in 64 m cells of their own; an edited cell is never evicted, and the world waits for its ground once |
| [0077](0077-game-messages-cross-the-wire-through-a-remote-event.md) | Game messages cross the wire through a replicated `RemoteEvent`; the sender is named by the connection, values travel encoded by the script module, and messages ride the Control channel behind the spawns they name |
| [0078](0078-a-dot-reaches-a-child-and-the-scene-is-typed.md) | A dot reaches a child after the members, and the scene's tree is typed from the scene itself (supersedes 0061) |
| [0079](0079-a-client-asks-through-a-remote-function.md) | A client asks the authority through a `RemoteFunction` and waits for the answer; the server never waits on a client |
| [0080](0080-replicated-storage-and-server-storage.md) | `ReplicatedStorage` and `ServerStorage` hold what is not the world; saved with the scene, one reaching every replica and one staying on the authority |
| [0081](0081-the-editor-holds-the-field-and-the-gpu-holds-what-is-near.md) | The editor holds the whole field; the terrain atlas keeps the tiles nearest the camera; a scene of cells waits for a 1 GiB field |
| [0082](0082-terrain-is-a-grid-of-voxels.md) | Terrain is one grid of voxels, a material and an occupancy each, in row-packed 32-cubed chunks; drawn as a quadtree of meshes built from mips, collided by chunk near movers; `.lterrain` version 3 reads version 2 |
| [0083](0083-the-simulation-is-deterministic-across-platforms.md) | The simulation is deterministic across platforms (level C): the engine's own transcendentals, Luau's `math` and `^` through them, no fused multiply-add, one trace per scenario |
| [0084](0084-ambient-is-for-enclosed-spaces-outdoor-ambient-for-open-ones.md) | `Lighting.Ambient` lights enclosed spaces and `OutdoorAmbient` open ones, blended by how much sky a surface sees; a cave is dim, not black |
| [0085](0085-a-player-is-the-same-player-after-a-reconnect.md) | A player token, given at the first welcome and presented on every reconnect, keeps a player's `UserId`; a replica redials by itself, and a rejoin sends the world afresh (protocol 10) |
| [0086](0086-streaming-cells-are-cubes.md) | A streaming cell has a vertical band as well as a square, centred on sea level, so a cave and the ground over it are two cells; terrain stays in columns |
| [0087](0087-a-large-terrain-is-a-folder-of-cells-and-the-editor-pages-it.md) | A terrain of 256 cells or more is saved as a folder of cells beside its scene; the editor streams it around its camera, keeps unsaved edits, and a save writes only the cells that changed |
| [0088](0088-2d-on-the-wire.md) | `Part2D` replicates on protocol 11, animation state included, a `Vector2` as a `Vector3` with a zero z; a replica drives it kinematically; `Tilemap2D` stays off the wire, as `Terrain` does |
| [0089](0089-navigation-is-a-tiled-navmesh-built-where-it-is-asked-for.md) | `NavigationService` builds Recast tiles where queries go -- rebuilt when a fingerprint of what stands in them changes -- for one agent; walkable is anchored, colliding parts and terrain; paths are waypoints and a completeness flag |
| [0090](0090-a-material-is-an-asset-a-part-wears-one-and-a-script-clones-one.md) | A material is a `.material.json` asset in `content/` and never an instance; a variant has a parent; a part wears one and has no `Color` or `Transparency` of its own, only the parameters its material declares; at runtime an asset is read-only and `Clone()` makes the copy a script changes (supersedes 0060) |
| [0091](0091-a-material-may-name-a-surface-shader-the-user-writes.md) | A material may name a surface shader: two HLSL functions (`luaugVertex`, `luaugSurface`) against a versioned `surface.hlsli`, from which the engine builds every pass; its parameters are material fields; scene depth and opt-in scene colour; compiled asynchronously in the editor and never in a game; the editor ships a DXC built from source (amends 0032). A node graph is the second tier and comes later |
| [0092](0092-a-script-lives-in-the-instance-it-is-put-in.md) | A script lives in the instance it is put in and is saved with the scene, in any service or inside any instance; a script the `src/scripts` mount read is MARKED (`World::mounted`), not recognised by where it is, and what is authored inside it is written as a mark that finds it again (amends 0057) |
| [0093](0093-the-script-editor-types-with-luau-analysis.md) | The script editor types code with `Luau.Analysis` in editor builds only (never a shipped game): completion, signature help and type errors across `require`, resolved through the live tree; the old solver at this pin, because the new one cannot generalize `Instance.new`'s overload set (amends 0057 §5) |
| [0094](0094-signal-collector-and-promise-are-the-engines-own.md) | `Signal` audited against GoodSignal and kept deferred, with `DisconnectAll`; `Collector` (Janitor's surface, cleaned newest first) and `Promise` (evaera's semantics, `Enum.PromiseState`, `ExpectAsync`) are Luau compiled at build time with the runtime's own options and installed as read-only globals |
| [0095](0095-a-light-can-stand-on-its-own.md) | `PointLight` and `SpotLight` gain a `CFrame`: in the world when nothing holds them, relative to the part or attachment that does -- the identity keeps every held light where it was (the owner's call) |
| [0096](0096-atmosphere-post-effects-and-a-sky-are-instances-under-lighting.md) | The look of a world is instances under `Lighting` (or the current camera, for a viewer's own): `Atmosphere`, a six-image `Sky` with sun, moon, stars and clouds, and bloom, colour correction, blur, depth of field and sun rays; a world with none of them draws as before; the sun stays on the clock; supersedes the mandate's M1 |
| [0097](0097-a-frames-fixed-costs-come-first-and-static-geometry-persists-when-measured.md) | A frame's fixed costs are removed first -- one staging buffer per frame for every upload, primitives resolved once, a fused part matrix -- each invisible to what is drawn; the per-instance persistent render scene is designed here and built when a measurement asks for it |
| [0098](0098-navigation-agent-types-areas-links-crowds-and-the-plane.md) | Navigation grows five ways over ADR 0089: named agent types with a mesh each, `NavigationArea` labels priced by `SetAreaCost` (a cost of `math.huge` forbids), `NavigationLink` jumps and ladders whose labels `FindPath` reports per waypoint, `NavigationAgent` crowds over the vendored DetourCrowd on the sim tick, and `FindPath2D` over tilemaps and `Part2D` walls |
| [0099](0099-teams-and-network-ownership.md) | `Team` under a replicated `TeamService`, with `Player.Team` riding the roster and `AutoAssign` putting joiners on the smallest side; `BasePart:SetNetworkOwner` hands a loose part to one player's machine, which simulates it and sends its state on channel 3 while the authority follows it kinematically (protocol 14) |
| [0100](0100-the-wire-protocol-is-published-and-versioned.md) | The wire protocol is published: `docs/protocol/wire.md` is generated from the schema, message layouts are schema data checked by `wirecheck`, compatibility is one version matched exactly and changed whenever the bytes change, and numbers are never reused |
| [0101](0101-rollback-saves-restores-and-steps-the-simulation.md) | Rollback in the GGPO shape: `RunService:SaveSimulation` returns the 3D simulation (the Jolt solver's whole state and every simulated part's and character's motion) as a `buffer`, `RestoreSimulation` puts it back into the same bodies or refuses, and `StepSimulation` re-steps a tick without scripts or touches; the game keeps its own Luau state |
| [0102](0102-2d-joints-join-two-sprites-and-a-sprite-sheet-plays-itself.md) | 2D joints and sprite animation: `HingeConstraint2D`, `SpringConstraint2D` and `WeldConstraint2D` join two `Part2D`s at a point on each through `IPhysics2D`, rebuilt with either body or on any edit; `SpriteAnimator` plays a sheet's frames on the sim clock by writing its sprite's `ImageRectOffset`, and ends by setting `Playing` false |
| [0103](0103-2d-on-the-wire-is-interpolated-and-a-tilemap-replicates-by-blocks.md) | 2D on the wire, completed: a remote `Part2D` is drawn between snapshots as a `BasePart` is, its rotation the short way round, and `Tilemap2D` is replicated -- its properties as fields, its cells in a `TilemapBlocks` message a block of 16 by 16 at a time, whole after the spawn and then each changed block, against one shadow per tilemap (protocol 15; supersedes 0088 decision 6) |
| [0104](0104-a-game-is-exported-from-one-window-for-windows-linux-and-android.md) | A game is exported from one window: identity (name, id, version, company, one PNG icon) once in `luaug.toml`, per-target details under `[export.<target>]` with no secret ever in the project, `luaug build` the only exporter for Windows, Linux and Android with a JSON progress stream, prebuilt players in the editor distribution, and a File > Export... window ending in Open folder, Run it and Install on phone (amends 0045) |
| [0105](0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md) | Code lives in three script services: `ServerScriptService` and `ClientScriptService` in each scene, `GlobalScriptService` (`Server`, `Client`, `Shared`) for the whole game; server code runs only on the authority and never replicates, client code never on a dedicated server; `ScriptService` is retired and `src/server`, `src/client`, `src/shared` and `src/scenes/<scene>/{server,client}` map onto them; the export gains `[export] multiplayer = none|host|dedicated`, and in `dedicated` each side's package carries only its own code, beside `windows-server` and `linux-server` targets (amends 0104 and 0092) |
| [0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md) | A scene is a complete place: every service but `GlobalScriptService` belongs to it, it saves every setting somebody changed and opens from the engine's; `SceneService:LoadScene(path, data)` changes it at run time, and in a match the authority's change takes every client along; `NetworkService:Join`, `Host` and `Disconnect` from a script; attributes replicate; the editor plays a match with up to four clients (amends 0080) |
| [0107](0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md) | A camera draws into a texture (`CameraTexture`, named `view://<name>` wherever a texture is taken, under a per-frame view budget), a `ViewportFrame` draws the unsimulated instances inside it only when they change, and a `SubWorld` runs a scene in its own `WorldHost` beside the host game, talking only through messages and input; `Camera.ClipPlane` and `@luaug/views` for mirrors and portals; to be built after 0104-0106's ledger |
| [0108](0108-the-engine-is-driven-by-ai-through-one-tool-registry-an-mcp-server-and-an-ai-panel.md) | The engine is driven by AI through one tool registry reached three ways: `luaug mcp` (MCP 2026-07-28 over stdio, the dev server grown up, able to launch an engine), the editor's AI panel (threads, Ask/Plan/Agent/Autonomous modes, models, context chips, approvals, a changes review, checkpoints), and external agents hosted over the Agent Client Protocol; about fifty tools with one undo step per call, permission classes and a never-list, keys only in the OS credential store, the agent loop outside the editor, and autonomous runs under a budget (amends 0035: every client authenticates) |
| [0109](0109-the-engine-has-an-internal-name-that-never-changes-and-its-brand-is-ludwerk.md) | The engine has an internal name that never changes -- `engine::`, `ENG_*`, `project.toml`, `.engine/`, `@engine/` -- and its brand is Ludwerk, read from `branding/brand.toml` alone; no compatibility with the old names, and the versions restart at 0.0.1 (amends every ADR that names the old brand in an identifier) |
| [0110](0110-a-gradient-colours-and-a-stroke-outlines-a-ui-element.md) | A gradient colours a UI element and a stroke outlines it: `ColorSequence` and `NumberSequence` become values; `UIGradient` (linear, radial and conical, clamp, repeat and mirror tiling, a scale) and `UIStroke` (text or border, pixel or scaled thickness, outer, centre or inner, an offset, several per element by `ZIndex`, round, bevel and miter joins, and a `<stroke>` rich-text tag), drawn on the one UI pipeline with a gradient table and stroked glyphs in the atlas |
| [0111](0111-a-game-saves-through-saveservice-into-the-players-own-folder.md) | A game saves through `SaveService`: named slots of attribute values and tables, in `SDL_GetPrefPath(company, name)/saves/` (`.engine/saves/` in the editor), written atomically with a `.bak` and an xxh3 checksum, at most once a second, on close and on going to the background; a game version and a migration callback; not replicated |
| [0112](0112-an-exported-game-carries-bytecode-not-source.md) | An exported game carries Luau bytecode (O2, debug level 1, version checked at load) and no source; `[export] ship_source = true` keeps source (amends 0045 and 0104) |
| [0113](0113-terrain-layers-are-engine-materials-and-rules-paint-by-slope-and-height.md) | Terrain layers are engine material assets (`Terrain.Layers`, up to 255; eight textured defaults keep ids 1 to 8), drawn from texture arrays, triplanar; the built-in slope rock becomes `Terrain.Rules` by slope and height, drawn per pixel, evaluated the same on the CPU for raycasts, and applied to voxels on request (completes 0090's second stage; amends 0082) |
| [0114](0114-a-voxel-holds-two-materials-and-a-blend.md) | A voxel holds an occupancy, a base and a top material and how much of the top shows; paint modes `Replace`, `Blend`, `Under` and `Erase` with strength and falloff; the seam is a per-pixel height blend (`.lterrain` v4; amends 0082) |
| [0115](0115-wind-is-a-workspace-property-that-moves-what-is-drawn.md) | Wind is `Workspace.GlobalWind` with gusts and turbulence, visual only, one function of `SimTime` in HLSL and C++ (`Workspace:GetWindAt`); particles opt in; surface shaders receive it |
| [0116](0116-foliage-is-drawn-from-rules-over-terrain-and-never-simulated.md) | Foliage is `FoliageLayer` rules and `FoliageMesh` entries under a `Terrain`, placed as a pure function of the terrain and a seed per chunk on the job threads, never simulated, replicated or hashed, drawn by a GPU-driven path with compute culling and LODs, swayed by the wind, thinned by quality, with a painted density mask |
| [0117](0117-a-material-carries-friction-and-a-footstep-sound.md) | A material carries `Friction`, `Restitution`, `FootstepSound` and `Tags`; terrain triangles carry their layer's; `RaycastResult.Material` and `Humanoid.FloorMaterial` |
| [0118](0118-water-is-one-wave-definition-read-by-the-renderer-and-by-physics.md) | `Water` (ocean, box, spline) is one list of waves evaluated deterministically on the CPU and in HLSL; buoyancy and drag are forces at sample points in the physics step; `ApplyImpulseAtPosition` and `ApplyAngularImpulse`; the ocean example is rewritten on it |
| [0119](0119-networkservice-opens-connections-and-a-project-declares-what-it-may-do.md) | `NetworkService` opens HTTP(S) requests, WebSockets, TCP and UDP over the core `@std/net` shares; a connection belongs to its script and scene; outgoing is open by default and listening needs `[permissions] listen` (amends 0070) |
| [0120](0120-the-transport-is-encrypted-and-a-player-is-a-key.md) | The match transport is encrypted with a Noise `IK` handshake and ChaCha20-Poly1305 over ENet (libsodium); a server and a player are key pairs, `Player.PublicKey` is identity; a relay and hole punching reach a host behind a NAT (amends 0012 and 0085) |
| [0121](0121-a-script-can-write-an-image-a-sound-and-a-mesh.md) | A script can write an image (`EditableImage`, a texture name any slot takes), a sound (`AudioStream`) and a mesh (`EditableMesh`, CPU-side, with `CollisionMode` `None`, `Static` or `Dynamic`) |
| [0122](0122-a-videoplayer-decodes-av1-and-opus-and-the-platforms-h264.md) | `VideoPlayer` decodes WebM with AV1 and Opus (dav1d, libopus) and MP4 with H.264 through the platform's decoder, into a texture and the audio mix, from content or a URL |
| [0123](0123-scripts-run-in-parallel-inside-actors-and-commit-in-a-fixed-order.md) | Scripts under an `Actor` run in its own Luau VM and may enter windows A (inside the tick) and B (render rate) through `task.desynchronize` and `ConnectParallel`; members are checked against their audited `ThreadSafety`; the world is read as the window found it; threads resume, messages arrive and `SharedTable` writes apply in actor order, never completion order |
| [0124](0124-a-scene-closes-as-a-game-does-and-a-handler-dies-with-its-script.md) | `game:BindToClose` runs when the game closes and `scene:BindToClose` when the scene does (a `LoadScene` away, or the game closing with it open); a global `scene` and `SceneService.CurrentScene` are a `Scene` object; a handler is owned by the script that registered it, and a scene script's game handlers are dropped with its scene (warned in the editor); the editor's Stop is the game closing; `game` and `scene` are mailboxes (`SendMessage`, `BindToMessage`) local to a machine, carrying attribute values copied and instances by reference (amends 0106 and 0111) |
| [0125](0125-a-scene-is-prepared-in-the-background-and-activated-when-the-game-says.md) | `SceneService:LoadSceneAsync(path, options)` returns a `SceneLoad` (`Progress`, `Status`, `Ready`, `Activate`, `Cancel`): the scene is read and parsed on a job and its assets warmed under a budget while the current one runs, and switched at a safe point when the game activates it, the old scene's `scene:BindToClose` first; `LoadScene` is the same with `Activate = true` (amends 0106) |
| [0126](0126-a-part-can-be-clicked-prompted-and-dragged-without-code.md) | `ClickDetector`, `ProximityPrompt` (with `ProximityPromptService` and a themed default look) and `DragDetector` make a part clickable, promptable and draggable without code, on mouse, touch and gamepad, validated by the authority in a match |
| [0127](0127-movers-and-constraints-move-a-part-without-code.md) | Movers (`LinearVelocity`, `AngularVelocity`, `AlignPosition`, `AlignOrientation`, `VectorForce`, `Torque`) and constraints (a motor and a servo on `HingeConstraint`, `PrismaticConstraint`, `RopeConstraint`, `RodConstraint`, `SpringConstraint`, `NoCollisionConstraint`) in the physics step, deterministic and in the rollback snapshot |
| [0128](0128-ui-lays-out-in-grids-and-pages-adapts-to-any-screen-and-is-driven-by-a-gamepad.md) | UI gains `UIGridLayout`, `UIPageLayout`, flex in `UIListLayout` with `UIFlexItem`, `UIScale`, aspect-ratio, size and text-size constraints, `CanvasGroup`, `UIDragDetector`, and gamepad and keyboard selection -- all arithmetic, per ADR 0040 |
| [0129](0129-highlight-beam-and-trail.md) | `Highlight` (outline and fill, through walls or not, under a budget), `Beam` and `Trail` as ribbons in the transparent pass; visual only |
| [0130](0130-parts-combine-into-solids-and-two-more-shapes.md) | Parts combine into solids (`UnionAsync`, `SubtractAsync`, `IntersectAsync`; `UnionOperation`, `NegateOperation`, Separate) through Manifold, pending the owner's approval of the dependency; `CornerWedge` and `Truss` shapes |
| [0131](0131-sound-effects-vibration-and-preloading.md) | Nine sound effects on miniaudio's graph (no new dependency), `HapticService` over SDL3 and the Android vibrator, and `ContentProvider:PreloadAsync` on ADR 0125's warming path |
| [0132](0132-players-chat-through-textchatservice.md) | `TextChatService` with channels, commands, whispers, a default window and bubbles, every message through the authority (length, rate, membership, a game's filter); no moderation service of the engine's own |
| [0133](0133-a-replica-predicts-what-its-character-touches-and-replays-it-exactly.md) | A replica predicts the loose parts near its character (radius, cap, linger) and corrects character and parts together by restoring the authority's state and re-simulating the island with the real physics step; `stepController`'s separate replay path is retired, and the snapshot carries every field the controller reads; the server stays authoritative (amends 0076) |
| [0134](0134-everything-visual-reads-one-drawn-position-per-instance.md) | Where each instance is drawn this frame is resolved once (`render::DrawPoses`) and every visual consumer reads it -- world UI, particles, views, 2D, prompts, the pointer, the editor's pick; the history is captured before a snapshot; alpha zero is the tick before; sound stays with the simulation; `drawcheck.luau` holds it |
| [0135](0135-the-ground-replicates-terrain-and-block-edits-travel-as-whole-chunks.md) | The ground replicates: the server sends the workspace terrain's and the block world's chunks that differ from the scene to a replica that joins, and each changed chunk after, whole and reliable, with the layers, rules and block types; a replica with no terrain makes one; protocol 24 |
| [0136](0136-scripts-run-at-the-displays-rate-and-a-camera-written-there-is-presented.md) | Scripts run at the display's rate: `BindToRenderStep` by priority before `PreRender`, `GetRenderCFrame` where a thing is drawn, a camera written in a render phase presented as written and latched into the simulated one at the next tick; the default rig turns every frame; no trace moves |
| [0137](0137-a-script-runs-while-it-is-in-the-running-world.md) | A script runs exactly while it is live -- enabled, in the world, outside an inert storage, its side running here -- starting and stopping on every path in and out; a run owns its threads and handlers; `ServerStorage` and `ReplicatedStorage` are inert; a script never writes `Source`; topology changes re-check every script |
| [0138](0138-a-script-carries-the-side-it-runs-on.md) | `Script.RunContext` (`Server`, `Client`, `Shared` once a machine), set by the service a script is in; each package strips the other side's code per script and compiles scene scripts; a replica runs its own copy, re-attached by scene identity or a stamp reference on the wire |
| [0139](0139-a-text-input-edits-as-every-other-does.md) | `TextInput` edits as every text field does: selection, word moves and deletes, the clipboard, undo, the mouse, whole characters, input-method composition; `MultiLine`, `Editable`, `MaxLength`, `Masked`, focus conventions, `KeyboardType`, a readable and writable caret; `FocusLost` gains a reason, and `Submitted`, `TextChanged`, `InputRejected`, focus methods and a focused-field query are added |
| [0140](0140-a-coarse-level-of-the-terrain-is-the-fine-surface-gathered.md) | A coarse terrain level is the level-0 surface gathered: a cell's vertex is the point nearest its level-0 tangent planes, its material and paint theirs, a coarse edge a quad each way the surface crosses it; seams stitched on sides and corners, no skirts; the level chosen by projected error (4, 3, 2, 1.5 px by quality) with hysteresis and a geomorph a seam's vertices slide through together; the sky term from the drawn level; the mips go (amends 0082) |
| [0141](0141-terrain-is-built-off-the-main-thread-and-drawn-where-it-is-built.md) | Every terrain mesh is built off the main thread by a few workers and put up a few a frame; a node keeps two meshes and is drawn only with one built for the seams the drawn set gives it, the frame before's ground drawn where it is not; what is drawn is judged against the ground last put up, so it is of one revision; `--pace` for headless flights (amends 0140) |
| [0142](0142-terrain-shadows-are-pushed-by-their-slope.md) | The terrain culls back faces in the shadow pass and is pushed by its own depth slope times the filter reach, plus a texel, up to N.L 0.1, in cascades and local tiles; contact shadows ignore the plane a ray starts on; the grain noise hashes its lattice by integers; `--debug-view=shadow` and `bend` with `imgshadow` and `imgsteps` prove them (amends 0082) |
