// Subsystem bring-up and the frame loop (architecture.md §2 "app", §3).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "engine/app/frame_pacing.h"
#include "engine/core/error.h"
#include "engine/core/types.h"
#include "engine/platform/event.h"
#include "engine/platform/game_integration.h"
#include "engine/platform/window.h"
#include "engine/render/settings.h"
#include "engine/replication/types.h"
#include "engine/rhi/types.h"
#include "engine/scene/graphics_model.h"

namespace engine::app {

using core::i32;
using core::u64;

struct EngineOptions
{
    // A directory is a project root and gets the full mount; a file is mounted
    // as a single entry `Script`. The scripts do not run
    // before the loop any more -- they are deferred, and their first resumption
    // is the first drain of the first tick.
    std::filesystem::path scriptPath;

    // The world's deterministic stream. Fixed rather than drawn from a clock,
    // because a replay stores the seed and nothing else (ADR 0025) -- and
    // because two runs of the same script must produce the same world hash,
    // which is the kernel's gate.
    u64 worldSeed = 1;

    // The project browser rather than a session (ADR 0055). Set by `--launcher`,
    // and set for a host started with no project at all -- which used to be a
    // usage error and is now the commonest way somebody meets this engine.
    bool launcher = false;

    // No window. The frame loop, the device and the render target all still
    // exist -- this is the CI harness and the shape a dedicated server would
    // take, not a mode where rendering is skipped.
    bool headless = false;

    // Zero means run until the window is closed. Any other value is a frame
    // budget, which is the only thing that makes a headless run terminate.
    u64 frames = 0;

    // What this process was started with, less the executable: what an editor
    // restarts itself with after its graphics device is lost.
    std::vector<std::string> arguments;

    // Loses the graphics device on this frame, as a driver reset would --
    // `--simulate-device-loss=N`, for the test that the engine survives one.
    // Zero never does.
    u64 simulateDeviceLossAt = 0;
    // **Takes the graphics device away for real on this frame**
    // (`--remove-device=N`): the driver's own removal, underneath the
    // backend, which nothing above is told of -- what a hosted runner's
    // software device did. For the test that the host then ends saying its
    // device is gone, where it died of an access violation. Zero never does.
    u64 removeDeviceAt = 0;

    // Holds the window on this frame for two seconds, as the system does while
    // its title bar is held -- `--simulate-window-hold=N`, for the test
    // that the loop goes on through it. Zero never does; so does a platform
    // with no such hold.
    u64 simulateWindowHoldAt = 0;

    // Where compiled surface shaders -- and the ones held back after a lost
    // device -- are kept; empty is `surface-cache` in the user directory.
    // `--surface-cache=DIR`, so a test never touches a person's own.
    std::filesystem::path surfaceCache;

    // Exit when the frame budget is spent instead of continuing to run.
    bool exitAfterFrames = false;
    // `--net-log-client-writes` (ADR 0186 section 5): see `WorldHostOptions`.
    bool logClientWrites = false;

    // Empty means take no screenshot. Requires `headless`: a windowed frame
    // renders into the swapchain, which has been presented and is gone by the
    // time anyone could read it.
    std::filesystem::path screenshotPath;
    // `--screenshot-every=N`: a picture at the end of every N-th frame, not
    // only the last, each named after `screenshotPath` with its number --
    // `shot.png` becomes `shot-000.png`, `shot-001.png`, ... -- so one run
    // photographs a scene that changes its camera on a schedule (the terrain
    // gallery). Zero takes the one picture at the end.
    u64 screenshotEvery = 0;
    // `--terrain-detail=full`: every terrain drawn at its finest level
    // whatever the distance -- the reference a coarse level is held against.
    // A test instrument.
    bool terrainFullDetail = false;
    // `--editor-drive=FILE`: input for the editor from a script
    // (`editor_drive.h`). Development builds only; empty drives nothing.
    std::filesystem::path editorDrive;
    // **The editor drawn with no window** (`--editor-shots=DIR`, with
    // `--edit --headless`): the editor's own interface is drawn into the
    // headless picture, and each `shot <name>` of the drive script writes
    // `DIR/<name>.png`. Nothing is put on a screen: the window the interface
    // is laid out in is the windowing layer's own offscreen kind, which is
    // memory and nothing else.
    std::filesystem::path editorShots;

    // Where to write the recorded command stream. Only the capture backend
    // records one; asking any other backend for it is a usage error rather
    // than an empty file, because an empty golden would pass forever.
    std::filesystem::path capturePath;

    // Runs the conformance suite from this directory instead of a normal
    // session: every `*.spec.luau` under it is mounted as an entry `Script` and
    // the run ends by itself once the suite reports (api-design.md §3).
    std::filesystem::path conformanceRoot;

    // Runs the record/replay determinism gate over this directory instead of a
    // session. A replay creates no device and no window (see `replay.h`), so it
    // is a mode rather than a flag on a normal run.
    std::filesystem::path replayRoot;

    // Rewrites each scenario's `trace.txt` from this build instead of comparing
    // against it. The only way a legitimate semantic change gets a new golden.
    bool replayRecord = false;

    // Runs the editor-seam proof over this directory, which holds two projects
    // `a/` and `b/` (see `two_worlds.h`). A mode rather than a flag on a normal
    // run: what it compares is three sessions against each other, so there is
    // no single session for the frame budget or the screenshot path to describe.
    std::filesystem::path twoWorldsRoot;

    // Where that proof leaves its four PNGs, or empty to write none. The
    // assertions compare pixels in memory, so the files are evidence for a
    // human and never an input to the result.
    std::filesystem::path twoWorldsOutDir;

    // The replica's acceptance gate: this project booted as an authority and as its
    // replica in one process, rendered side by side (`runReplicaGate`). Its
    // evidence goes to `twoWorldsOutDir`.
    std::filesystem::path replicaGateProject;

    // Runs the simulation benchmarks over this directory. Like a replay it
    // opens no device: what it measures is the tick, and a tick that depended
    // on a swapchain would be the finding rather than the measurement.
    std::filesystem::path benchRoot;
    u64 benchRepeats = 3;

    // Where a conformance run writes its machine-readable per-case report.
    // Empty writes none. `ludwerk test` reads this rather than the console,
    // because every console line is catalog-resolved.
    std::filesystem::path testReportPath;

    // `ws://127.0.0.1:<port>/<path>` -- the dev server this engine dials out to
    // (ADR 0035). Empty means no watcher is attached and no control code runs
    // at all. The engine never listens, in any profile.
    std::string devControlUrl;
    // Proves this connection is the engine `ludwerk dev` launched: a loopback
    // listener is reachable by every process on the machine.
    std::string devControlToken;

    rhi::BackendId backend = rhi::BackendId::SdlGpu;
    // `--rhi=` named the backend. Without it a dedicated server takes the
    // no-op one: it draws nothing, and a device it never draws with is a GPU
    // driver -- or, on a machine with none, a software rasteriser -- it can
    // crash in.
    bool backendChosen = false;

    // Print a frame-time summary at exit. Off by default because the numbers
    // are wall-clock and a run that is not being measured should not pay for
    // keeping them (R10 forbids simulation reading them at all).
    // The visual editor rather than a game (ADR 0046). A normal
    // windowed session in every other respect: the same world, the same tick,
    // the same renderer -- what changes is that the world is drawn into a panel
    // and the shell around it can select and edit what it draws.
    //
    // Not a mode that hijacks the loop the way `--replay` and `--two-worlds`
    // do, and deliberately so: an editor that ran a different loop from the game
    // would be an editor that shows you something other than your game.
    bool editor = false;

    // `[project] scene` -- the scene a run of this project starts with,
    // content-relative. Empty means the project starts with whatever its scripts
    // build, which is every example before `06-scene`.
    //
    // The EDITOR may open a different one: it remembers what the person had open
    // and falls back to this. Which scene a run starts with is the project's
    // decision; which scene an editor opens is the person's.
    std::string startupScene;
    // **`--scene=` named it**: this run starts there whatever the
    // project file says and whatever an editor had open -- a level is tested
    // without clicking through the menu. `startupSceneData` is `--scene-data=`,
    // JSON, which the scene's `GetLoadData` answers.
    bool startupSceneFromFlag = false;
    // `--max-per-address` was given: the project's `[network]
    // max_connections_per_address` does not replace it.
    bool maxPerAddressFromFlag = false;
    // `--print-luau-flags`: the project's Luau flags as the tools that check
    // scripts take them (ADR 0217), printed, and the run ends.
    bool printLuauFlags = false;
    std::string startupSceneData;
    // `[network] server` (ADR 0106): where `NetworkService:Join()` goes with no
    // address.
    std::string defaultServer;
    // `[network] relay`, or `--relay=` (ADR 0178): where `Host` registers and
    // a `Join` by code asks when the call names no relay.
    std::string defaultRelay;
    // **Whether this run's matches are encrypted** (ADR 0224): false only for
    // `--insecure` or `[network] encrypt = false`, and never in a shipping
    // build.
    bool encryptMatches = true;
    // `[network] server_key`: the key of `defaultServer`, in hexadecimal.
    std::string defaultServerKey;
    // `[network] ticket_public_key`, base64.
    std::string ticketPublicKey;
    // `[permissions] websocket_client`.
    bool websocketClient = false;
    // **Administration from another machine** (ADR 0219): `--admin` or
    // `[server] admin`, its port (0: the game's plus one) and who beyond
    // 127.0.0.1 may reach it.
    bool adminListen = false;
    core::u16 adminPort = 0;
    std::vector<std::string> adminAllow;
    // **A world's health** (ADR 0220): `--health[=PORT]` or `[server]
    // health`, its port (0: the game's, over TCP) and who beyond 127.0.0.1
    // may ask.
    bool healthListen = false;
    // **Light bots** (`--bots=N` with `--join`, ADR 0236): this process is
    // that many players and nothing else.
    core::u32 botCount = 0;
    std::string botBehavior = "wander";
    core::f64 botSeconds = 0.0;
    std::string botIntent = "Move";
    core::u16 healthPort = 0;
    std::vector<std::string> healthAllow;
    // **A world a manager started** (`--managed=NAME`, ADR 0220): its name,
    // and the manager's public key it trusts passes from (`--manager-key=`,
    // base64).
    std::string managedWorld;
    std::string managerKey;
    // `[project] id`: bound into every match's handshake, so a game joins only
    // its own.
    std::string projectId;
    std::vector<std::string> enabledIntegrations{};
    std::unordered_map<std::string, std::string> integrationConfigurations{};
    platform::PlatformServiceConfiguration platformServices{};
    // `[render]` (ADR 0107): how many camera textures a frame draws, and the
    // largest side one may have.
    core::u32 maxViewsPerFrame = 4;
    core::u32 maxViewResolution = 1024;
    // `[render] max_highlights` (ADR 0129).
    core::u32 maxHighlights = 32;
    // `[render] max_sub_worlds` (ADR 0107 §3).
    core::u32 maxSubWorlds = 2;
    // `[server] dormant_radius` (ADR 0234), below 0 the default.
    core::f64 dormantRadius = -1.0;
    // `[server] physics_threads` (ADR 0228).
    core::u32 physicsThreads = 4;
    // `[network] rewind_cap_ms` (ADR 0235).
    core::f64 rewindCapMs = 200.0;
    // `[network] kick_over_limit` (ADR 0241 §1).
    core::u32 kickOverLimit = 0;
    // `[server] time_dilation` and `time_dilation_floor`, or
    // `--time-dilation[=floor]` (ADR 0233): an overloaded world slows its
    // clock, down to the floor, instead of stalling.
    bool timeDilation = false;
    core::f64 timeDilationFloor = 0.25;
    bool timeDilationFromFlag = false;
    // `[render] foliage_density` and `foliage_shadow_distance` (ADR 0116).
    core::f32 foliageDensity = 1.0f;
    core::f32 foliageShadowDistance = 30.0f;

    // Where `SaveService` writes, and its limits (ADR 0111): decided in `main`
    // from what kind of run this is. Empty keeps saves in memory.
    std::filesystem::path saveDirectory;
    // Whether `--saves=DIR` named it: nothing chooses it for the run then.
    bool saveDirectoryGiven = false;
    core::u64 saveMaxSlotBytes = 4u * 1024u * 1024u;
    core::u32 saveMaxSlots = 64;
    // `[scene] close_grace_seconds` (ADR 0124).
    core::f64 sceneCloseGrace = 5.0;
    // `[script] max_memory_mb`; 0 for no cap.
    core::u32 scriptMemoryMb = 0;
    // The editor, `dev` and a match's windows: warnings a player never reads.
    bool developerWarnings = false;

    // The returning-focus soak check (the revisited-place check). Zero asserts nothing,
    // like every other soak threshold: only the caller running a particular
    // fly-through knows whether its path comes back.
    core::f32 soakReturnRadiusMetres = 0.0f;

    // **Write the world the scripts just built to a scene, then exit.**
    //
    // The one thing ADR 0047's migration needs and cannot do without: a project
    // whose world is in its code has no way to get that world into a file, and
    // retyping it by hand into JSON is not a migration anybody performs. This
    // boots the project, lets the entry scripts build whatever they build, and
    // writes it -- the same `writeScene` the editor's Save calls, so what comes
    // out is what the editor would have written.
    //
    // Headless and one frame. It is a capture, not a run.
    std::filesystem::path saveScenePath;

    // **Write the scene's tree as types and exit** (ADR 0078): the project's
    // `.engine/types/scene.d.luau`, from its scene as the editor holds it --
    // whole, with no script run and nothing partitioned away. What `ludwerk setup`
    // and `ludwerk check` run so a project that never opened the editor is typed.
    bool writeTypesOnly = false;

    // **Work out every volume that names a file, write the files, and exit**
    // (`--bake-irradiance`, ADR 0207): the project is opened as a game is,
    // with no window, every `IrradianceVolume` whose `Baked` names a file is
    // worked out by rays whatever this machine's settings, and each is
    // written to the file it names under `content/`. Needs a device: the
    // light is the frame's, and a run with none draws no frame.
    bool bakeIrradiance = false;

    // **Check every script of the scene as the editor's script pane would, and
    // exit** (ADR 0093): `--check-scripts`, which `ludwerk check` runs so that
    // what the command says is what the editor shows. The scene as the editor
    // holds it, no script run. Only where the editor is built.
    bool checkScriptsOnly = false;

    // **Die of a fault once the crash handler is in, on purpose**
    // (`--fault-on-purpose`): the instrument of the gate that checks what a
    // crash leaves for the next run to find (ADR 0187). A crash cannot be
    // checked from inside the process that has it.
    bool faultOnPurpose = false;

    // **Lay the scene's terrain from a source, save it and exit** (ADR 0149
    // §2): what `ludwerk terrain import` runs. The scene as the editor holds
    // it -- whole, no script run, nothing partitioned -- its terrain laid a
    // tile at a time with the ground behind it in the session cache, and saved
    // as the editor saves it: the cells, then the scene that names them.
    struct TerrainImport
    {
        enum class Kind : core::u8
        {
            // A sixteen-bit image, or a folder of tiles of one.
            Heightmap,
            // Hills from noise (`asset::hillHeights`).
            Hills,
            // A Luau file that returns `function(x, z)`.
            Function,
        };
        Kind kind = Kind::Heightmap;
        std::filesystem::path source;
        // Metres across.
        core::f32 size = 1024.0f;
        // Black and white for a heightmap, the hills' floor and top: world
        // metres.
        core::f32 low = 0.0f;
        core::f32 high = 64.0f;
        core::u8 material = 1;
        core::u32 seed = 1;
        core::u32 octaves = 5;
        core::f32 scale = 128.0f;
    };
    std::optional<TerrainImport> terrainImport;

    // **Partition the project's scene and exit** (ADR 0053).
    //
    // The same work a run does on the way to its first frame, done on its own
    // so that `ludwerk build` can pre-warm the cache -- and it is the same code,
    // not a second path, which is the whole reason the ADR asked for it here
    // rather than in the asset compiler. The artifact carries `.engine/`, so a
    // cache warmed by this travels with the game and the player's first launch
    // pays nothing.
    bool partitionOnly = false;

    bool frameStats = false;
    // `--frame-stats-warmup=SECONDS`: how long after each scene load the
    // world is settling, and its frames are not in the slowest-frames list.
    core::f64 frameStatsWarmupSeconds = 3.0;
    // `--frame-report=SECONDS` and `[debug] frame_report_seconds`: a line in
    // the log every so many seconds saying how the frames went. Zero is off.
    core::f64 frameReportSeconds = 0.0;
    // **The measuring keys** (ADR 0171; `debug_measure.h`): `--gpu-pass-times`,
    // `--hide=LIST`, `--shadow-taps=N` and `--log-ui-touches`, and `[debug]`'s
    // keys of the same names. The taps travel in `graphics.shadowTaps`; this
    // is what the flag said, which beats the file.
    bool gpuPassTimes = false;
    // **A profile to take home** (`--profile-capture=FILE`, ADR 0210): the
    // last minute of frames -- where each one's time went, by system and by
    // script -- written to FILE as the run ends, to be opened in the editor's
    // Profiler. For a machine the editor is not on. `--profile-lines` has the
    // script's lines sampled into it as well.
    std::string profileCapturePath;
    bool profileLines = false;
    // `--global-illumination-budget=MS` was given: a run with no window
    // keeps the volumes' work to it, as a watched game does, where it
    // otherwise gives a frame all it has room for.
    bool indirectBudgetKept = false;
    // `--gpu-frame-times`: every frame waits for the GPU to finish what it
    // was sent before the frame is called over, so `--frame-stats` says what
    // a frame costs the CPU and the GPU together. One wait a frame, the same
    // in every run -- where `gpu_pass_times` stops after every pass, which a
    // frame of four views has four times as many of (ADR 0205's numbers).
    bool gpuFrameTimes = false;
    std::string debugHide;
    // `--skip=LIST` and `[debug] skip`, joined; its bits travel in
    // `graphics.measuredSkip`.
    std::string debugSkip;
    // `--shadow-under-casters`, `--no-shadow-under-casters`: 1 and 0,
    // and -1 where neither was said.
    int debugShadowUnderCasters = -1;
    core::u32 debugShadowTaps = 0;
    bool logUiTouches = false;
    // `[debug] overlay_key`, as a key: what opens the overlay, and what the
    // game therefore never hears in a host that has one. `Unknown` is
    // no key.
    platform::Key overlayKey = platform::Key::F3;
    // **`--pace=HZ`: a headless frame waits for its share of a second** before
    // the next begins, the wait left out of `--frame-stats`. Headless runs a
    // frame as soon as the last is done, so a flight of 1 500 frames is over in
    // a second or two -- and whatever works beside the frame (the terrain
    // built off the main thread) is measured against a camera moving
    // thirty times too fast. Zero runs flat out.
    core::u32 paceHz = 0;

    // **How fast a window's frames are made** (ADR 0147): the display's
    // sync, a cap, and the rate while nobody is looking. A headless run and
    // one with `--pace` are not paced by it.
    FramePacing pacing;

    // **`--gpu-debug`: the GPU debug layer, asked for by name**. Off
    // means whatever the profile decides -- see `gpuValidationWanted`.
    bool gpuDebug = false;
    // **`--gpu-least`: the device a GPU with the fewest features is given**
    // -- no depth clamping -- on a GPU that has more: how a desk draws
    // as such a phone does.
    bool gpuLeast = false;
    // **A headset, when there is one** (ADR 0244 §6): `[xr] enabled`, or
    // `--xr`; `--no-xr` turns it off whatever the project says. Without a
    // headset the game is drawn on the screen as it always is.
    bool xr = false;
    // Set by `--xr` or `--no-xr`, so the project's word does not replace it.
    bool xrFromFlag = false;
    // **`--xr=simulated`** (ADR 0248 §4): the engine's own headset, with no
    // runtime and no window -- headless too. With `--screenshot`, each eye's
    // picture is written beside it (`-left`, `-right`).
    bool xrSimulated = false;
    // **A standalone headset's drawing** (`--xr-profile=standalone|pc`): each
    // eye in one pass with two samples a pixel and the lean shadows, as a
    // Quest draws it -- by default on a standalone headset, and on a PC when
    // asked, to see there what a Quest shows. The simulated headset's eyes
    // are then a Quest 3's size; its field of view is a Quest 3's already.
#if defined(__ANDROID__)
    bool xrStandalone = true;
#else
    bool xrStandalone = false;
#endif
    // `--xr-simulated-store=<file>`: where the simulated headset keeps the
    // anchors a script saves, for a second run to load (ADR 0247 §3).
    std::string xrSimulatedStore;
    // The name the headset's runtime lists the game under.
    std::string xrApplication{};
    // `[xr] required`: no headset, no game -- said, and the run ends.
    bool xrRequired = false;
    // `[xr] mirror`: while a headset draws, the window shows its left eye,
    // or nothing.
    bool xrMirror = true;
    // `[xr] spectator`: whether the window may show the spectator's camera.
    bool xrSpectator = true;
    // `[xr] spectator_every`: drawn every this many headset frames.
    core::u32 xrSpectatorEvery = 2;
    // `[xr] hands`: the hands themselves, where the headset tracks them.
    bool xrHands = true;
    // `[xr] depth`, or `--xr-depth=on|off` over it: each eye's depth handed to
    // the runtime with its colour, where it takes depth (ADR 0244 §5). Off
    // by default on a standalone headset (`ProjectConfig::xrDepth`).
#if defined(__ANDROID__)
    bool xrDepth = false;
#else
    bool xrDepth = true;
#endif
    bool xrDepthFromFlag = false;
    // `[xr] screen_effects`: occlusion, contact shadows and the screen's
    // reflections and bounce drawn in a headset's eyes, which by default
    // they are not.
    bool xrScreenEffects = false;
    // `[xr] space_warp` (ADR 0252): 0 off, 1 auto, 2 on. Auto is off until
    // the frames made between are measured on a headset (§8).
    core::u8 xrSpaceWarp = 1;
    bool xrSpaceWarpFromFlag = false;
    // `[xr] foveation`, or `--xr-foveation=off|low|medium|high` over it: how
    // much less a headset's eye is shaded away from where it looks, 0 to 3,
    // where the device has a fragment density map. Medium by default on a
    // standalone headset -- on a Quest 3 high saved no more time and showed
    // steps on the edges toward the lens's rim -- and off on a PC.
#if defined(__ANDROID__)
    core::u32 xrFoveation = 2;
#else
    core::u32 xrFoveation = 0;
#endif
    bool xrFoveationFromFlag = false;
    // **A headset's quality** (`[xr]`, `[xr.android]` over it on a
    // standalone headset): samples a pixel (1 none; 2, 4 or 8; nought the
    // profile's -- two on a standalone headset, one on a PC), and the
    // renderer's `HeadsetQuality`, nought being the level's or the measured
    // value. The first cascade's depth in meters: 1.1, the reach of the hands
    // and of a table before them, at 1.9 mm a texel of 2048 -- at 2 a texel
    // was twice that, and so was the lift that keeps a base on its shadow.
    core::u32 xrMsaa = 0;
    core::f32 xrRenderScale = 1.0f;
    core::u32 xrShadowResolution = 0;
    core::f32 xrShadowDistance = 0.0f;
    core::u32 xrShadowCascades = 0;
    core::f32 xrShadowFirstSplit = 1.1f;
    // `[xr] refresh_rate` in hertz: 72 on a standalone headset until 90 is
    // measured with foveation on, the runtime's own on a PC.
#if defined(__ANDROID__)
    core::f32 xrRefreshRate = 72.0f;
#else
    core::f32 xrRefreshRate = 0.0f;
#endif
    // `--xr-eyes-shot=<path>`: both eyes' pictures as drawn, before the
    // runtime has them, written as `<name>-left` and `<name>-right` at the
    // 600th frame a headset draws, beside the eyes' geometry in the log.
    std::filesystem::path xrEyesShot;
    // `--gpu=<name>`: which graphics API to draw through (`vulkan`,
    // `direct3d12`, `metal`). Empty takes `rhi::defaultDriver()`.
    std::string gpuDriver;
    // **`[debug] astc = false`, `--no-astc`**: textures as RGBA on a GPU that
    // samples ASTC (ADR 0180) -- to tell a fault of that path from another.
    bool astcTextures = true;

    // **The posture, from the command line and from nowhere else** (ADR 0070):
    // `--host`, `--serve` or `--join`. Solo when none was given, which builds no
    // replication object at all.
    replication::Config network;

    // The streaming work's gate, as a flag. Empty writes no report and asserts nothing.
    //
    // The HOST enforces it rather than a script or a separate checker, the
    // same way `--replay` compares against its own golden: the numbers exist
    // only inside the frame loop, and a gate that has to be re-derived from a
    // log line is a gate that rots the first time the line is reworded.
    // A failed soak is a non-zero exit; `soak.h` holds the arithmetic.
    std::filesystem::path soakReportPath;

    // The DECLARED ceiling. Zero asserts no ceiling -- a number only whoever
    // is running a particular fly-through can supply, and one this code has
    // no business inventing a default for.
    u64 soakCeilingBytes = 0;

    // What "the world loaded" means for this particular scene. Zero asserts
    // nothing; see `soak.h` for why a soak needs to be told.
    u64 soakMinimumInstances = 0;
    // The same for a world that is ground: cells in, and out again.
    u64 soakMinimumGroundCells = 0;
    // How many per cent the last quarter's peak memory may be over the second
    // quarter's. Zero asserts nothing.
    u64 soakMemoryGrowthPercent = 0;
    // The whole frame's 99th percentile the soak allows, in milliseconds, for
    // a run on a device whose frame is not the engine's to keep -- a software
    // rasteriser. Zero keeps `SoakThresholds`' own.
    u64 soakFrameP99Ms = 0;

    // The quality family, already resolved through its three layers by the time
    // it gets here (project_config.h): a preset, the project file, the flags.
    // The host applies it to the renderer and never re-derives it.
    render::GraphicsSettings graphics;
    // **The same, as the layers it came from** (ADR 0147), for what is put
    // between them while the game runs: a script's writes through
    // `GraphicsService`, and the player's saved choices. `graphics` and
    // `pacing` are what these resolve to before either has spoken.
    scene::GraphicsModel graphicsModel;
    // `[display] remember_player_settings`: the player's choices are read from
    // `settings.json` beside the saves before the first frame, and
    // `GraphicsService:SaveAsync` writes them there.
    bool rememberPlayerSettings = true;
    // `--locale=` and `--voice-locale=`: the text and the voice language for
    // this run, in front of what the player saved and never written to it.
    // For a developer checking a language, and for a test.
    std::string localeOverride;
    std::string voiceLocaleOverride;
    // `[project] default_locale` (ADR 0154).
    std::string defaultLocale = "en";

    // The game's own window title, passed through rather than translated -- it
    // is the game's string and not the engine's (R3, and the split
    // `log()`/`logText()` draws). Empty uses the engine's titled window.
    std::string windowTitle;
    // **One of a match's windows** (ADR 0106 §5): where the editor put it
    // (`--window=x,y,w,h`), and which it is (`--label=Client 1`), which the
    // title carries so four windows can be told apart.
    std::optional<platform::WindowPlacement> windowPlacement;
    std::string windowLabel;
    // `--log-file=`: where this process's log goes, when the editor runs
    // several and one `engine.log` would be four processes writing one file.
    std::filesystem::path logFile;
    // `[project] icon`, resolved: a PNG here is what the window wears, over
    // whatever icon the executable carries (ADR 0104, stage 0).
    std::filesystem::path projectIcon;
    // `[window] fullscreen` and `resizable` (ADR 0104 §1). A game that names no
    // project fills a handheld's display all the same.
#if defined(__ANDROID__)
    bool fullscreen = true;
#else
    bool fullscreen = false;
#endif
    bool resizable = true;
    // `--hidden-window`: a window with its swapchain, never shown -- what a
    // test of a window's own paths runs with.
    bool hiddenWindow = false;

    i32 width = 1280;
    i32 height = 720;
};

// **Whether the GPU device is created with its debug layer**. The
// `debug` and `dev` profiles are where engine work happens, so they validate;
// `player`, `shipping` and `editor` are what somebody downloads, and there the
// layer only costs frame time and turns any message it has into a crash. An
// explicit `--gpu-debug` asks for it in any profile.
[[nodiscard]] bool gpuValidationWanted(std::string_view profile, bool optIn) noexcept;

// **Whether this run asks to be scheduled ahead of ordinary work**:
// every run that is a game somebody plays or waits on -- a window that is
// not the editor, and a windowless one that serves, joins or measures. A game's
// window was thought favoured enough by being in front; under a busy machine it
// was not, and a join took frames of 4 to 40 s, all of them inside the graphics
// driver's own calls, until the process was raised. The editor and a plain
// headless run (a test, a capture) stay at the desktop's level.
[[nodiscard]] bool scheduledAhead(bool headless, bool editor, bool networked, bool measuring) noexcept;

// Runs to completion. Returns the first error that stopped it, or nothing on a
// clean exit.
[[nodiscard]] std::optional<core::EngineError> run(const EngineOptions& options);

// The project browser (ADR 0055): what the host does when it is started with no
// project at all, which used to be a usage error.
//
// **A loop of its own, and that is the decision rather than a shortcut.** There
// is no world here, no Luau VM, no physics and no scheduler -- a launcher that
// booted a simulation to show a list of folders would be absurd. What it shares
// with `run` is the window, the device and the overlay, which is exactly the
// part that draws.
//
// Choosing a project starts the editor as a NEW PROCESS and returns; everything
// a project decides is resolved at boot, so swapping it inside a running host
// would touch every seam for a screen that runs once.
//
// Only `width` and `height` are read from `options`. A headless launcher is not
// a thing that can exist, and the caller is expected not to ask for one.
[[nodiscard]] std::optional<core::EngineError> runLauncher(const EngineOptions& options);

} // namespace engine::app
