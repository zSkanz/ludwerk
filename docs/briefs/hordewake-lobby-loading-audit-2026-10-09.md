# Hordewake Play-to-lobby loading audit

Status: loading batch 10 implemented, validated on PC/Windows and Linux, and exported.
Original audit and baseline below are retained for traceability.
PC only, windowless, no pointer capture, no phone/Xbox operations. Existing
loading10 Release folder/ZIP in Downloads is the latest delivered game. Local
cooperative play is a separate authorized workstream and is not in that artifact.

Confirmed source behavior:
- Play invokes SceneService:LoadScene("scenes/lobby.scene.json"). This replaces
  the scene rather than changing only an interface panel.
- Menu client and lobby server both call Backdrop.ground; lobby cover grows again.
  The same deterministic 240 m camp creates 241*241 height samples again, then
  writes/paints a new Terrain. TerrainLoader nodes are keyed by world, terrain
  instance/generation and node, not reusable across unrelated instances by default.
- Both scene client loaders request all UI pictures and every selectable hero
  through Pictures.load before constructing the UI. Existing caches must be
  measured: repeated PreloadAsync calls do not prove repeated disk reads.
- Both controllers wait for nearby terrain meshing, then wait 0.3 simulation
  seconds before calling Loading.hide. That function fades and destroys the
  card after another 0.4 simulation seconds. Backdrop.wait(8) is an upper bound,
  not an unconditional eight-second delay.

Evidence: C:/Users/juanr/Downloads/Hordewake-Loading-opt10-results/play-lobby-control.log.
Fixture: %LOCALAPPDATA%/Ludwerk/build/hordewake-loading-profile-v1. Required src,
assets, content, i18n and project.toml refreshed from current shipping source.
Fixture-only instrumentation waits for the menu loading card to disappear,
then requests solo Play and logs wall-clock stages. It never sets pointer capture.
Dev host: --headless --frames=2400 --exit --rhi=sdlgpu --width=1920 --height=1080
--frame-stats. Completes without error log entries. The fixture is not a release
artifact; source surfaces compile through the development host as usual.

Observed single-run wall-clock values on PC/dev with GPU validation:

| Stage | Seconds |
|---|---:|
| Play request to lobby client entry | 0.023308 |
| Ground height generation/write/paint | 0.070025 |
| Pictures preload wait | 0.120492 |
| Lobby UI/camp construction | 0.001811 |
| Backdrop terrain mesh wait | 0.817113 |
| Play request to game loading card destroyed | 1.126914 |

Stages overlap; do not sum them. Terrain is the largest observed blocking wait,
not necessarily only its meshing CPU: it also spans scheduling/uploads and frame
pacing. os.clock reports wall clock, while task.wait/tweens use simulation time;
headless fixed-step execution is not a direct measurement of monitor-paced user
latency. Native curtain, foliage and actual first complete presented frame must
be measured separately before claiming end-to-end loading solved. No console or
mobile timing may be inferred.

Priority: reuse/preload the camp and its ready geometry across menu/lobby where
semantically possible, or refactor their offline presentation into one scene.
Keep multiplayer/server authority and isolated scene cleanup correct. For a
generic engine cache, require bounded retention, complete geometry/material/LOD/
seam keys, proper invalidation and GPU lifetime handling; do not key it to this
game or retain arbitrary dead instances indefinitely. Distinguish parsed-scene
preparation from GPU-ready terrain and pipelines. Prewarm only needed pictures/
heroes and remove timer padding only after readiness-based visual checks.

A trivial cached deterministic height array could remove much of the 70 ms
regeneration, but does not address the dominant ~817 ms wait; do not present it
as instant loading. Fast transitions require work before the click and reuse,
not only higher gameplay FPS or faster disk APIs. Next validation should repeat
cold/warm transitions from a fully exported Release fixture, covering first
complete frame and absent terrain/textures, before architectural changes.


## Loading batch 10: accepted changes

The generic TerrainLoader retains up to 64 MiB / 1024 entries of packed CPU
geometry across scene replacement. It owns no dead-world references or device
handles. Keys include node position/LOD, all eight stitching levels, voxel
content digest over the existing mesher dependency span, voxel scale and floor/
ceiling. Edits and paint change the digest. Disk-backed fields bypass reuse.
Eviction is LRU, budget can be reduced/disabled; destroy clears the cache. Workers
only borrow immutable hit payloads. GPU buffers and library entries still belong
to each live node and are released normally. All-hit batches can integrate
without scheduling workers behind the native loading curtain.

The first experiment retained complete TerrainMesh/collider/morph copies as
well as packed vertices. Its 64 MiB working set thrashed and did not improve
loading; it was replaced with packed-only retention (~39 MiB in the camp).
Temporary diagnostic logging was removed. No unbounded retention or GPU cache.

Hordewake caches its deterministic camp height samples (~232 KiB), removes the
0.3 simulation-second padding after Backdrop.wait, and uses a 0.15-second fade
for ready menu/lobby transitions. Loading.hide retains its original duration
by default for other transitions. Full resource preloads and terrain readiness
checks remain. No ground texture binding or quality settings changed.

## Release validation

Three separate 1400-frame windowless executions per version, exported assets/
bytecode, proper Release/player host, GPU validation off, SDL_GPU on the same
PC. No parallel builds/tests during measured runs. Values are medians of the
three runs, wall-clock seconds; stages overlap.

| Stage | Control | Candidate |
|---|---:|---:|
| Ground generation/write/paint | 0.058128 | 0.049846 |
| Pictures preload (includes overlapping server work) | 0.093749 | 0.081788 |
| Terrain readiness wait | 0.728927 | 0.458113 |
| Play until game loading card destroyed | 0.887848 | 0.560503 |

About 36.9% reduction in the measured transition; terrain wait -37.2%.
This remains headless/fixed-step evidence, not monitor-paced or console latency,
and game card removal is not instrumentation of the first complete presented
frame/native curtain. Do not call it instant or claim all loading is solved.
Cold process startup is not what this comparison measures. Each execution first
loads the menu, then requests Play automatically. Scripts and asset set match
except the accepted loading changes and fixture-only timing/auto-Play hooks.

Windows and Linux terrain loader tests: 23 cases / 11813 assertions, pass on
both (unrelated cases and the explicit manual timing benchmark excluded).
Windows seven capture gates pass, including terrain texture binding regression.
Linux Clang build passes. All shipping game Luau source passes strict/new-solver
analysis. Captured Release lobby visually checked: textured terrain, vegetation,
characters and UI present. Normal Release headless startup smoke passes on
SDL_GPU and native D3D12, exit 0 with no error/fatal entries.

Artifacts: C:/Users/juanr/Downloads/Hordewake-0.8.49-loading10-windows and matching
.zip. Contains normal game, no automatic Play/profile hooks, and no local co-op
feature yet. Evidence and baseline/candidate exports remain under
Hordewake-Loading-opt10-results and %LOCALAPPDATA%/Ludwerk/build.

Remaining opportunities: GPU-ready camp reuse or shared menu/lobby presentation,
less LOD refinement scheduling/upload work, native-curtain/presented-frame
instrumentation and actual console/device measurements when available. Keep
multiplayer authority and terrain invalidation correct; no fake completion.

Next authorized workstream: hordewake-local-coop-2026-10-09.md. Owner chose
simultaneous independent card panels, paused until all players confirm.
