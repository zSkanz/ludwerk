# examples/ — Always-Runnable Milestone Artifacts

Numbering matches `docs/roadmap.md`; every example is a real project plus an
automated headless gate script (screenshot/capture + asserted behavior):

| Example | Born in | Proves |
|---|---|---|
| `boot` | M0 | the host boots a sandboxed VM, runs Luau, and routes output through the i18n'd log (unnumbered: numbering starts at the first milestone with a window) |
| `00-clear` | M1 | window, RHI clear, debug draw, Luau-driven visuals, screenshot harness |
| `01-instances` | M2 | Instance tree over ECS, deferred signals, task, 500 scripted cubes |
| `02-meshes` | M4 | glTF loading, PBR, shadows, camera, day/night slider |
| `03-physics-playground` | M5 | Jolt bodies, contacts→Touched, CharacterBody, third-person camera |
| `04-obby` | M6 | IAS input, UI, tweens, audio, minimal animation — playable end-to-end |
| `05-streaming` | M7 | chunk streaming, floating origin, LOD/HLOD, memory ceilings |
| `10-open-world` | M8 | the v1 flagship: streamed open world + character + day/night + hot reload. The first example with a `project.toml` the engine reads, and the one the soak gate drives. Its middle 512 m is streamed `Terrain`, with a hill and a tunnel |
| `11-ocean` | post-v1 | a `Water` sea (ADR 0118): a simulated hull that pitches and rolls on the waves it is drawn on, sailed by impulses at a point, and cargo that floats by its density alone. Unnumbered — the first example not born of a milestone, and the one that measured what moving a part from Luau costs |
| `12-ragdoll` | post-v1 | `Ragdoll:Build` over a real rig: sixteen limbs, fifteen joints and a `Bone` per joint, all of them ordinary instances. The example the pose blend exists for — going down is a ramp rather than a flag, which is the difference between a fall and a glitch. Unnumbered by milestone, like `11-ocean` |
| `13-terrain` | post-v1 phase 2 | the sculpted ground: a grid of voxels meshed as surface nets, caves and all, and a brush from a script |
| `14-voxels` | post-v1 phase 2 | `VoxelService`: a block world registered, filled, mined with `Raycast` and collided — not the terrain. Blocks with images, leaves that cast their holes, glass, and a spring whose water runs down the hill into a pond |
| `15-multiplayer` | post-v1 phase 4 | one project in every posture: `--host`, `--join`, `--serve` or solo. The authority builds and moves the world, the replica is sent it, and `NetworkService.Authority` is the one question the script asks. Space honks, through a `RemoteEvent`, and H asks a `RemoteFunction` how often |
| `16-particles` | post-v1 phase 2 | `ParticleEmitter`: fire, smoke, sparks, a fountain and a scripted burst -- additive and blended in one draw, from parts and from attachments |
| `18-world-ui` | post-v1 phase 2 | `SurfaceGui` and `BillboardGui`: a scoreboard with rich text on a wall, name tags and health bars over crates, and a sign sized in metres -- all drawn in the world, hidden by what is in front. The scoreboard's "Next round" button is pressed like a screen's |
| `19-terrain-test` | post-v1 phase 2 | Every controlled case of the terrain report side by side -- flat, slope, hill, a ball added and taken away, tunnel, cave, wall, overhang, a chunk corner -- toured from above, the side and below, with the mesh and its normals one flag away |
| `17-cave` | post-v1 phase 2 | `Terrain` as a volume: a mountain with a tunnel dug into its side, a chamber and a skylight shaft, and a camera that flies through it -- dark inside, lit by its lamps |
| `20-platformer` | post-v1 phase 3 | the 2D layer: a level painted on a `Tilemap2D` from an ASCII map, a `Part2D` hero with coyote time, a jump buffer and variable jump height, coin and flag sensors through `Touched`, and an orthographic camera that follows |
| `21-navigation` | post-v1 phase 3 | `NavigationService`: a walker finds its own way through a maze of anchored parts, a new seeded goal at every arrival; a dropped block is walked round on the next query |
| `25-gradients-and-strokes` | owner's queue, 2026-09-27 | `UIGradient` and `UIStroke` (ADR 0110), every kind on one screen: a health bar with a sliding shine, a radial glow, a conical progress ring and colour wheel, repeated and mirrored stripes; outer, centre and inner strokes, round, bevel and miter corners, a double border, a stroke with its own gradient; outlined, hollow and scaled text, and rich text's `<stroke>` -- and a sign in the world |
| `26-security-cameras` | owner, 2026-09-26 (ADR 0107) | Six `CameraTexture`s feeding a wall of monitors on a `SurfaceGui` and a tablet in a `ScreenGui` -- a security office at night, the camera game the views were decided for |
| `27-mirrors-and-portals` | owner, 2026-09-26 (ADR 0107) | `@engine/views`: a mirror on a gallery wall and a door that is a portal onto a garden room across the map, seen by a camera walking past -- `Camera.ClipPlane` keeps what is behind the glass out |
| `28-arcade` | owner, 2026-09-26 (ADR 0107 §3) | two arcade cabinets, each a `SubWorld` running a small game in a world of its own, its picture on the cabinet's screen; press E to play one -- a coin goes in with `Send`, the arrow keys with `SetInputState`, and the score comes back with `Received` |
| `29-meadow` | game-ready plan, world (ADR 0116) | foliage: grass and flowers grown over rolling terrain by a `FoliageLayer`'s rules -- material, slope, height, never under a roof -- a tile at a time as the camera nears, culled on the GPU into indirect draws, and swaying in a gusting wind; no instance per blade |
| `30-interactions` | game-ready plan, toolkit (ADR 0126) | a room where a click is the engine's and the rest is the game's: a button with a `ClickDetector` that lights the lamp, a door opened by holding E in reach, and a drawer and a lever moved by the pointer's ray -- a prompt and a drag written in Luau |
| `32-menus` | game-ready plan, toolkit (ADR 0128) | one window for every screen and every hand: an inventory in a `UIGridLayout` that scrolls, a tutorial in a `UIPageLayout` going round, an options page with a slider on a `UIDragDetector` -- tabs sharing their bar by flex, the window a `CanvasGroup` that fades in as one and a `UIScale` that pops it open, a `UISizeConstraint` keeping it usable from a phone held upright to a wide monitor, and every button reached by the d-pad, the stick or the arrows |
| `31-lake-and-river` | game-ready plan, world (ADR 0118) | a lake and a river as two `Water`s over carved terrain, and logs the river carries into the lake by its flow alone -- a `Box` and a `Spline` water, and what the engine does where they meet |
| `33-options` | owner's queue, settings (ADR 0147, ADR 0154) | the engine's own options screen in one call, `require("@engine/settings").open()`, over a scene a quality level shows on -- thin columns for the shadows, lamps for the glow -- and a button that changes the language: the game's words from its own `i18n/` catalogs, and the screen's buttons re-worded from the same file |
| `24-scenes` | post-v1 phase 4 | scenes at run time: a menu, a lobby and an arena, each a complete place with its own lighting, UI and code; a game that goes between them with `SceneService:LoadScene`, alone or hosted and joined from its menu; readiness as attributes that replicate; a character per player; and a loading screen that stays across with `KeepOnSceneLoad` |

Assets used by examples must be permissively licensed and recorded in
`THIRD_PARTY_NOTICES.md`. Keep binary assets tiny until the git-LFS ADR (M4);
the streaming example generates its world procedurally for this reason.

## Running one by hand

Every example folder carries a `run.bat`. It resolves the host binary under
`ENG_BUILD_ROOT` — builds are out-of-tree (R14), so the path is not something
worth remembering — and passes any extra flags straight through:

```
examples\01-instances\run.bat
examples\01-instances\run.bat --headless --frames=120 --exit --screenshot=out.png
```

It works from any working directory, and `ENG_PRESET` selects a different
build profile (`win-msvc-debug`, `win-msvc-shipping`); it defaults to
`win-msvc-dev`. If the binary is missing the script says which preset to build
rather than failing with a path.

These are a convenience for humans and nothing more: the automated gates invoke
`engine-host` directly from CMake, so no gate depends on a shell script. A
`run.bat` that rots is a broken convenience, not a red milestone.

**Adding one to a new example.** Copy the `run.bat` from the example whose shape
matches and change only its last statement — that line is the single place the
two supported project shapes differ (api-design.md §4):

| Shape | Copy from | Last statement |
|---|---|---|
| One file mounted as one `Script` | `00-clear` | `"%ENG_HOST%" "%~dp0init.luau" %*` |
| A project directory whose `src/client/**/*.luau` become entry `Script`s | `01-instances` | strip the trailing backslash from `%~dp0`, then pass the directory |

The trailing-backslash strip in the directory form is not decoration: `%~dp0`
always ends in one, and `"...\"` escapes the closing quote and corrupts the
argument.
