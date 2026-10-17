# examples/ — Always-Runnable Projects

Every example is a real project plus an automated headless gate script
(screenshot/capture + asserted behavior):

| Example | Proves |
|---|---|
| `boot` | The host boots a sandboxed VM, runs Luau, and routes output through the i18n'd log (unnumbered: numbering starts at the first milestone with a window) |
| `00-clear` | window, RHI clear, debug draw, Luau-driven visuals, screenshot harness |
| `01-instances` | Instance tree over ECS, deferred signals, task, 500 scripted cubes |
| `02-meshes` | glTF loading, PBR, shadows, camera, day/night slider |
| `03-physics-playground` | Jolt bodies, contacts→Touched, CharacterBody, third-person camera |
| `04-obby` | IAS input, UI, tweens, audio, minimal animation — playable end-to-end |
| `05-streaming` | chunk streaming, floating origin, LOD/HLOD, memory ceilings |
| `10-open-world` | The v1 flagship: streamed open world + character + day/night + hot reload. The first example with a `project.toml` the engine reads, and the one the soak gate drives. Its middle 512 m is streamed `Terrain`, with a hill and a tunnel |
| `11-ocean` | a `Water` sea (ADR 0118): a simulated hull that pitches and rolls on the waves it is drawn on, sailed by impulses at a point, and cargo that floats by its density alone. Unnumbered — the first example not born of a milestone, and the one that measured what moving a part from Luau costs |
| `12-ragdoll` | `Ragdoll:Build` over a real rig: sixteen limbs, fifteen joints and a `Bone` per joint, all of them ordinary instances. The example the pose blend exists for — going down is a ramp rather than a flag, which is the difference between a fall and a glitch. Unnumbered by milestone, like `11-ocean` |
| `13-terrain` | The sculpted ground: a grid of voxels meshed as surface nets, caves and all, and a brush from a script |
| `14-voxels` | `VoxelService`: a block world registered, filled, mined with `Raycast` and collided — not the terrain. Blocks with images, leaves that cast their holes, glass, and a spring whose water runs down the hill into a pond |
| `15-multiplayer` | one project in every posture: `--host`, `--join`, `--serve` or solo. The authority builds and moves the world, the replica is sent it, and `NetworkService.Authority` is the one question the script asks. Space honks, through a `RemoteEvent`, and H asks a `RemoteFunction` how often |
| `16-particles` | `ParticleEmitter`: fire, smoke, sparks, a fountain and a scripted burst -- additive and blended in one draw, from parts and from attachments |
| `18-world-ui` | `SurfaceGui` and `BillboardGui`: a scoreboard with rich text on a wall, name tags and health bars over crates, and a sign sized in metres -- all drawn in the world, hidden by what is in front. The scoreboard's "Next round" button is pressed like a screen's |
| `19-terrain-test` | Every controlled case of the terrain report side by side -- flat, slope, hill, a ball added and taken away, tunnel, cave, wall, overhang, a chunk corner -- toured from above, the side and below, with the mesh and its normals one flag away |
| `17-cave` | `Terrain` as a volume: a mountain with a tunnel dug into its side, a chamber and a skylight shaft, and a camera that flies through it -- dark inside, lit by its lamps |
| `20-platformer` | the 2D layer: a level painted on a `Tilemap2D` from an ASCII map, a `Part2D` hero with coyote time, a jump buffer and variable jump height, coin and flag sensors through `Touched`, and an orthographic camera that follows |
| `21-navigation` | `NavigationService`: a walker finds its own way through a maze of anchored parts, a new seeded goal at every arrival; a dropped block is walked round on the next query |
| `25-gradients-and-strokes` | `UIGradient` and `UIStroke` (ADR 0110), every kind on one screen: a health bar with a sliding shine, a radial glow, a conical progress ring and colour wheel, repeated and mirrored stripes; outer, centre and inner strokes, round, bevel and miter corners, a double border, a stroke with its own gradient; outlined, hollow and scaled text, and rich text's `<stroke>` -- and a sign in the world |
| `26-security-cameras` | Six `CameraTexture`s feeding a wall of monitors on a `SurfaceGui` and a tablet in a `ScreenGui` -- a security office at night, the camera game the views were decided for |
| `27-mirrors-and-portals` | `@engine/views`: a mirror on a gallery wall and a door that is a portal onto a garden room across the map, seen by a camera walking past -- `Camera.ClipPlane` keeps what is behind the glass out |
| `28-arcade` | two arcade cabinets, each a `SubWorld` running a small game in a world of its own, its picture on the cabinet's screen; press E to play one -- a coin goes in with `Send`, the arrow keys with `SetInputState`, and the score comes back with `Received` |
| `29-meadow` | foliage: grass and flowers grown over rolling terrain by a `FoliageLayer`'s rules -- material, slope, height, never under a roof -- a tile at a time as the camera nears, culled on the GPU into indirect draws, and swaying in a gusting wind; no instance per blade |
| `30-interactions` | a room where a click is the engine's and the rest is the game's: a button with a `ClickDetector` that lights the lamp, a door opened by holding E in reach, and a drawer and a lever moved by the pointer's ray -- a prompt and a drag written in Luau |
| `32-menus` | one window for every screen and every hand: an inventory in a `UIGridLayout` that scrolls, a tutorial in a `UIPageLayout` going round, an options page with a slider on a `UIDragDetector` -- tabs sharing their bar by flex, the window a `CanvasGroup` that fades in as one and a `UIScale` that pops it open, a `UISizeConstraint` keeping it usable from a phone held upright to a wide monitor, and every button reached by the d-pad, the stick or the arrows |
| `31-lake-and-river` | a lake and a river as two `Water`s over carved terrain, and logs the river carries into the lake by its flow alone -- a `Box` and a `Spline` water, and what the engine does where they meet |
| `33-options` | the engine's own options screen in one call, `require("@engine/settings").open()`, over a scene a quality level shows on -- thin columns for the shadows, lamps for the glow -- and a button that changes the language: the game's words from its own `i18n/` catalogs, and the screen's buttons re-worded from the same file |
| `39-interior` | a bright room and a dark room that share a wall ten centimetres thick: a `ReflectionProbe` and an `IrradianceVolume` under an invisible part the size of each room, so a polished floor and a chrome ball reflect the room they are in and each room is lit by what its own walls bounce; the dark room stays dark beside the bright one, a walker crosses from one to the other, a lamp switched on is followed by both (`Dynamic`), and the keys turn `GraphicsService.Reflections` and `GlobalIllumination` off and on |
| `40-cinematic` | a cutscene that is a file: two speakers, a camera that cuts and moves and looks at whoever speaks, three lines in two languages with their mouths, a lamp that comes up, a door that opens, and two events -- played, paused and skipped from a script, with the game's own camera never written. Opens in the editor with the `SequencePlayer` to select and the Timeline to open on it |
| `41-couch` | two to four at one machine, each with a view of the window: side by side, in quarters, or one picture that parts across a line which turns as two walk round each other (`@engine/splitscreen`); an interface each, stepped by each one's own controller; a sound heard by whoever is nearest | `run.bat` -- W A S D and the arrows for two at one keyboard, a controller a player, Tab or Y to share the window another way |
| `43-ambush` | an ambush in an alley, told to a sequence as it runs: a guard a `cast` track makes of a stamp and walks in; a flash keyed to `"$Blow"`, the player's attribute the game rolls; a camera on one curve whose key behind a wall is `keepInSight`; a cut `afterLine`; a shake when the blow lands; `BindCamera` leaning the lens in as the two close; and what comes after written as a table and played by `LoadSequence` | `run.bat` -- Enter or Start for another blow |
| `42-nightfall` | a courtyard as the sun goes down: light that bounces in the open from an `IrradianceVolume` that `FollowsCamera` in three levels, added to an authored ambient (`BounceOnly`), under a sun that moves and a torch that flickers every frame inside a budget; and a sequence that keys the hour on `Lighting`, fades in behind the bars of a wider picture, shows a caption, keeps the camera at a walker's shoulder with `relativeTo`, stops at a hold and is skipped a shot at a time | `run.bat` -- Space or A to go on from the hold, N or X for the next shot, Enter to play it again |
| `44-vehicle` | a car of parts on four wheels that are casts, not parts: a `VehicleController` with an engine and a gearbox, `VehicleWheel`s with a suspension and a tire each, driven by two input actions, over a ramp and through cones | `run.bat` -- W A S D or the stick to drive, Space or B for the hand brake, R or Y to put it back on its wheels |
| `45-breaking-wall` | a wall cut into forty pieces by `BasePart:Fracture`, held by joints that break and kept on its footing; a heavy ball punches a hole where it strikes, and each joint that gives puffs dust from a `ParticleEmitter` | `run.bat` -- Space or A to roll a ball, R or Y to build it again |
| `46-cloth-cape` | a cape that is cloth: a mesh of its own worn over the body's pose and a `Cloth` that hangs it from a joint on the shoulders, on a figure that runs, stops dead, dashes and is teleported, one in the wind and one whose cape the wind lays on a wall | `run.bat` |
| `47-realms` | an online game of several worlds run by the manager from `servers.toml`: a town where every player arrives, fields one `Travel` away, and a dungeon made for each party that stands on the gate (`CreateInstanceAsync`, `TravelGroup`), closed a minute after they leave; a loading screen over each hop and the operator's announcements, from two `@engine` modules | `run.bat` -- the servers; `run.bat play` -- a player; or *This game's worlds* in the editor |
| `38-dubbing` | three speakers and six lines in two languages: the voice's language chosen apart from the text's, a second language that is a folder (`content/l10n/pt-BR/`), a line that lasts as long as its longest language, subtitles in one call (`@engine/subtitles`) shown when what is heard is not what is read, the options screen's Audio page, and three mouths moved three ways by a `LipSync` -- a track beside each recording, three bands of the sound, loudness alone |
| `37-character` | three heroes of different builds and joint names on ONE library of clips and one animation graph -- free characters and clips, CC0, with their licences beside them: walk, run, strafe, jump, a slash, a shot and its reload with no game code doing the mixing (the server sets one attribute to attack); a walker with its feet on the stairs and a staff in both hands, heads that follow the player, a crossbow that hangs from a `Bone` with its string drawn by the reload clip and is handed to the other hand, and a hood that is a piece worn through `MeshPart.PoseFrom` and changed while the game runs |
| `36-face` | three busts of one model with five shape keys: one talks by its clip's weight channels, one smiles, frowns and blinks by `MeshPart:SetMorphWeight`, one is at rest and costs nothing; in the editor a bust's keys are sliders |
| `35-cape` | a cape on a figure: three `SpringBone`s on three columns of joints and a `SpringCollider` for the body, through a run in a circle, a dead stop, a dash and a teleport, beside a figure standing in the wind |
| `34-settings-list` | a phone's settings list in a `ScrollFrame` with no scrolling code: thrown by a finger and gliding, giving at its ends and springing back, a strip of wallpapers inside it that scrolls sideways while the list scrolls down, a bar whose thumb drags, the d-pad walking down the rows with the list keeping the selected one in sight, and a canvas as tall as its rows by `AutomaticCanvasSize` |
| `24-scenes` | scenes at run time: a menu, a lobby and an arena, each a complete place with its own lighting, UI and code; a game that goes between them with `SceneService:LoadScene`, alone or hosted and joined from its menu; readiness as attributes that replicate; a character per player; and a loading screen that stays across with `KeepOnSceneLoad` |

Assets used by examples must be permissively licensed and recorded in
`THIRD_PARTY_NOTICES.md`. Keep binary assets tiny until the git-LFS ADR;
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
|---|---|
| One file mounted as one `Script` | `00-clear` | `"%ENG_HOST%" "%~dp0init.luau" %*` |
| A project directory whose `src/client/**/*.luau` become entry `Script`s | `01-instances` | strip the trailing backslash from `%~dp0`, then pass the directory |

The trailing-backslash strip in the directory form is not decoration: `%~dp0`
always ends in one, and `"...\"` escapes the closing quote and corrupts the
argument.
