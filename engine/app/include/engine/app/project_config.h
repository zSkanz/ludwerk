// What `project.toml` tells the ENGINE (api-design.md §4).
//
// The project file has been the CLI's since the tooling loop -- `tools/cli/project.luau`
// reads it with the TOML subset in `tools/cli/toml.luau`. The flagship work is the first
// milestone where the host needs it too, and the reason is packaging: a game
// built with `ludwerk build` runs `engine-host` directly, with no Lute anywhere
// near it, so a window title and a graphics preset that only the CLI could read
// would be settings that stop applying the moment the game ships.
//
// Only the keys the host acts on are read here. Everything else in the file --
// `[dev] port`, `[assets]`, `[build]` -- belongs to tools that run before the
// engine does, and reading it here would be duplicating a decision.
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/app/frame_pacing.h"
#include "engine/app/luau_flags.h"
#include "engine/core/types.h"
#include "engine/platform/game_integration.h"
#include "engine/render/settings.h"
#include "engine/scene/graphics_model.h"
#include "engine/scene/world.h"

namespace engine::app {

// A graphics field the command line named explicitly. The distinction matters
// because the project file is a DEFAULT and a flag is an override: without it,
// a host that filled `GraphicsSettings` from its own defaults could not tell
// "the user asked for scale 1.0" from "nobody said anything".
struct GraphicsOverrides
{
    std::optional<render::QualityLevel> quality;
    std::optional<core::f32> renderScale;
    std::optional<core::u32> shadowResolution;
    std::optional<core::u32> shadowCascades;
    std::optional<core::f32> shadowDistance;
    std::optional<core::u32> lightBudget;
    std::optional<bool> bloom;
    std::optional<bool> ambientOcclusion;
    std::optional<render::AntiAliasingMode> antiAliasing;
    // `--upscaling=none|fsr1|fsr2` and `--sharpness=X` (ADRs 0158, 0164).
    std::optional<render::UpscalingMode> upscaling;
    std::optional<core::f32> sharpness;
    // `--frame-generation`, `--no-frame-generation` (ADR 0165).
    std::optional<bool> frameGeneration;
    std::optional<bool> autoExposure;
    std::optional<bool> contactShadows;
    std::optional<bool> foliageDecals;
    // `--lean`, `--no-lean`: the lean path for the window, over the level's
    // and the file's (the mobile lean path).
    std::optional<bool> leanPath;
    // `--reflections=LEVEL` and `--global-illumination=LEVEL` (ADRs 0202,
    // 0203), an `Enum.GraphicsLevel` each: low, medium, high, ultra.
    std::optional<core::i32> reflections;
    std::optional<core::i32> globalIllumination;
    // `--screen-reflections=LEVEL` and `--screen-bounce=LEVEL` (ADR 0209).
    std::optional<core::i32> screenSpaceReflections;
    std::optional<core::i32> screenSpaceBounce;
    // `--global-illumination-budget=MS`: the milliseconds of a frame the
    // bounced light may take. Given, a run with no window keeps to it too --
    // which is how what a player sees under a moving sun is measured.
    std::optional<core::f32> globalIlluminationBudget;
    // `--render-cap=N`: the most lines the world is rendered at, 0 for none.
    std::optional<core::u32> renderCap;
    // `--terrain-surface=full|lean|fast|flat` (ADR 0175, ADR 0179), over the level's and the file's.
    std::optional<render::GraphicsSettings::TerrainSurface> terrainSurface;
    // `--vsync`, `--no-vsync`, `--max-frame-rate=N`, `--background-frame-rate=N`,
    // `--unfocused-frame-rate=N` (ADR 0147), over `[display]`.
    std::optional<bool> vsync;
    std::optional<core::u32> maxFrameRate;
    std::optional<core::u32> backgroundFrameRate;
    std::optional<core::u32> unfocusedFrameRate;
    // `--force-surface=NAME`: a test instrument (ADR 0091), never a project setting.
    std::optional<std::string> forcedSurface;
    // `--debug-view=NAME`: a test instrument.
    std::optional<render::DebugView> debugView;
    std::optional<bool> instancing;
};

struct ProjectConfig
{
    // `[project] name`, or empty. The window falls back to the engine's own
    // titled window when a project does not name itself.
    std::string name;

    // `[project] id` -- reverse-DNS, and on Windows the identity the shell
    // groups taskbar buttons and pinned shortcuts by. Two games built with this
    // engine that shared one id would share one taskbar button.
    std::string id;

    // `[window] title`, or empty. **The game's own string, not the engine's**:
    // it is passed through rather than translated, which is the split
    // `log()`/`logText()` already draws and which `WindowDesc` reserved a
    // passthrough for with the first window.
    std::string windowTitle;

    // `[window] size`, or zero when the file does not say.
    core::i32 windowWidth = 0;
    core::i32 windowHeight = 0;

    // **How fast frames are made** (ADR 0147), `[display]`: `vsync`
    // (on unless it says otherwise), `max_frame_rate` (0, no cap) and
    // `background_frame_rate` (10, while the window cannot be seen; 0, no
    // throttle) and `unfocused_frame_rate` (0, none, while it can), each
    // under its flag.
    FramePacing pacing;

    // `[project] icon` -- a project-relative path to a PNG or `.ico`. A PNG is
    // what the host's window wears while the game is being made; `ludwerk build`
    // reads the same key to embed it in the packaged artifact.
    std::string icon;

    // `[project] scene` -- the scene a RUN of this project starts with, as a
    // content-relative path (`scenes/main.scene.json`).
    //
    // Declared rather than found by convention. A fixed filename the engine
    // looks for is a rule nobody can see in a project that has three scenes,
    // and "which one starts" is a decision a project makes -- the same decision
    // the first entry of Unity's build settings is. Empty means the project
    // starts with whatever its scripts build, which is every example before
    // `06-scene`.
    std::string scene;

    // `[network] server` -- where `NetworkService:Join()` with no address goes
    // (ADR 0106): `play.example.com:7777`.
    std::string networkServer;
    // `[network] relay` -- the relay a host registers with and a join by code
    // asks (ADR 0178): `relay.example.com:7789`. Empty: no relay, and a match
    // is joined by its address.
    std::string networkRelay;
    std::vector<std::string> enabledIntegrations{};
    std::unordered_map<std::string, std::string> integrationConfigurations{};
    platform::PlatformServiceConfiguration platformServices{};

    // `[network] role = "server"`: the project is a dedicated server's package,
    // and the player given no posture starts as `--serve` (ADR 0105). Written
    // by `ludwerk build --target=*-server` into the package's own copy, so the
    // server starts with no arguments.
    bool serverRole = false;

    // `[network] encrypt = false` -- a development run's matches in the clear
    // (ADR 0224). A shipping build encrypts whatever this says.
    bool networkEncrypt = true;
    // `[network] server_key` -- the key of the server `[network] server`
    // names, in hexadecimal (ADR 0224 §3): what its operator's log printed.
    std::string networkServerKey;
    // `[network] ticket_public_key` -- the Ed25519 key the game's backend signs
    // its tickets with, base64 (ADR 0226).
    std::string networkTicketKey;
    // `[permissions] websocket_client` (ADR 0223 §2): a client's game may open
    // `net.websocket`.
    bool websocketClient = false;
    // `[server] admin`, `admin_port` and `admin_allow` (ADR 0219): a dedicated
    // server listening for its administrators, where, and from which addresses
    // beyond its own. The port is the game's plus one when not given.
    bool serverAdmin = false;
    core::u16 serverAdminPort = 0;
    std::vector<std::string> serverAdminAllow;
    // `[ui] font_fallbacks` (ADR 0259): the fonts a character a label's own
    // font has not got is drawn with, in order, before the default face.
    std::vector<std::string> fontFallbacks;
    // `[server] health`, `health_port` and `health_allow` (ADR 0220): a
    // world's `/health` and `/metrics`, on the game's port number over TCP
    // when no other is given.
    bool serverHealth = false;
    // `[server] dormant_radius` (ADR 0234): metres from every player's focus
    // past which a character sleeps; 0 never; below 0, twice the load radius.
    core::f64 serverDormantRadius = -1.0;
    // `[server] physics_threads` (ADR 0228): the worker threads a dedicated
    // server's physics steps on, beside the tick's own; four by default, and
    // fewer where one machine runs several worlds.
    core::u32 serverPhysicsThreads = 4;
    // `[server] time_dilation` and `time_dilation_floor` (ADR 0233).
    bool serverTimeDilation = false;
    // `[xr] enabled` (ADR 0244 §6): try a headset at start, and draw on the
    // screen when there is none.
    bool xrEnabled = false;
    // `[xr] required`: a game that is only VR -- without a headset it says
    // so and does not start.
    bool xrRequired = false;
    // `[xr] mirror` (ADR 0244 §5): the window shows the left eye, or, false,
    // nothing.
    bool xrMirror = true;
    // `[xr] spectator` (ADR 0250): whether the window may show
    // `XRService.SpectatorCamera` while a headset draws; false keeps it the
    // mirror whatever a script sets.
    bool xrSpectator = true;
    // `[xr] spectator_every`: the spectator's picture is drawn every this
    // many of the headset's frames, 1 to 8, and shown in between.
    std::optional<core::u32> xrSpectatorEvery;
    // `[xr] hands` (ADR 0245 §4): the hands themselves where a headset
    // tracks them; false for a game played only with controllers.
    bool xrHands = true;
    // `[xr] depth` (ADR 0244 §5): each eye's depth handed to the runtime, for
    // its reprojection; false to compare a headset without it. **Off by
    // default on a standalone headset**: measured on a Quest 3, the frame's
    // GPU time went from 11-16 ms with it to 10-12 without, and the frame
    // rate from 62-70 to 70-72.
#if defined(__ANDROID__)
    bool xrDepth = false;
#else
    bool xrDepth = true;
#endif
    bool xrScreenEffects = false;
    // `[xr] space_warp` (ADR 0252): "off", "auto" or "on" as 0, 1, 2; unset,
    // the platform's default (`EngineOptions::xrSpaceWarp`).
    std::optional<core::u8> xrSpaceWarp;
    // `[xr] foveation`: "off", "low", "medium" or "high"; unset, the
    // platform's default (`EngineOptions::xrFoveation`).
    std::optional<core::u32> xrFoveation;
    // The rest of `[xr]`'s quality (`EngineOptions::xrMsaa` and after), each
    // unset where the project leaves the platform's.
    std::optional<core::u32> xrMsaa;
    std::optional<core::f32> xrRenderScale;
    std::optional<core::u32> xrShadowResolution;
    std::optional<core::f32> xrShadowDistance;
    std::optional<core::u32> xrShadowCascades;
    std::optional<core::f32> xrShadowFirstSplit;
    std::optional<core::f32> xrRefreshRate;
    core::f64 serverTimeDilationFloor = 0.25;
    core::u16 serverHealthPort = 0;
    std::vector<std::string> serverHealthAllow;

    // `[network] timeout` -- seconds the other end may go silent before this
    // machine says it is gone. From 1 to 120; ten by default.
    core::u32 networkTimeoutSeconds = 10;
    // `[network] max_connections_per_address` -- players one address may be
    // on this game's server: a household, a LAN house, a school, a carrier's
    // NAT. Nought for any number; 64 by default.
    core::u32 networkMaxPerAddress = 64;
    // `[network] client_budget_kb_per_second` (ADR 0229): kilobytes of the
    // world's state a second each player is sent at most; 0 for no budget.
    core::u32 networkClientBudgetKb = 48;
    // **What a replica predicts besides its characters** (ADRs 0133, 0254):
    // `[network] predict_radius` in metres, `predict_max_bodies`,
    // `predict_linger_ticks`, and how far ahead a part moving on its own is
    // carried on by its motion, `predict_moving_lead_ms`.
    core::f64 networkPredictRadius = 8.0;
    core::u32 networkPredictMaxBodies = 32;
    core::u32 networkPredictLingerTicks = 30;
    core::u32 networkPredictMovingLeadMs = 250;
    // `[network] rewind_cap_ms` (ADR 0235): the furthest back, in
    // milliseconds, a shot is checked against what its shooter saw. From 0 --
    // never -- to 1000; 200 by default.
    core::u32 networkRewindCapMs = 200;
    // `[network] kick_over_limit` (ADR 0241 §1): a player whose calls go past
    // the remotes' limits this many times in a second is removed. From 0 --
    // never, the default -- to 10000.
    core::u32 networkKickOverLimit = 0;

    // **`[luau.flags]`** (ADR 0217): Luau's flags the project sets, each a
    // switch or a number, in the file's order. Applied before the first script
    // is compiled, and told to every tool that checks one.
    std::vector<LuauFlagSetting> luauFlags;

    // **What the views may cost** (ADR 0107), `[render]`: how many camera
    // textures are drawn in one frame, the largest side one may have, and how
    // many sub-worlds may run. Settings rather than constants, because a
    // camera game on a phone and one on a desktop want different answers.
    core::u32 maxViewsPerFrame = 4;
    core::u32 maxViewResolution = 1024;
    core::u32 maxSubWorlds = 2;
    // `[render] max_highlights` (ADR 0129): how many `Highlight`s a frame
    // draws, each a mask and a composite; past it the nearest win.
    core::u32 maxHighlights = 32;

    // **Foliage** (ADR 0116), `[render]`: the fraction of every layer drawn,
    // and how far from the camera it casts shadows. Lower on a phone, where a
    // field to the horizon costs a desktop's frame.
#if defined(__ANDROID__)
    core::f32 foliageDensity = 0.5f;
    core::f32 foliageShadowDistance = 15.0f;
#else
    core::f32 foliageDensity = 1.0f;
    core::f32 foliageShadowDistance = 30.0f;
#endif

    // `[save]` (ADR 0111): how large one slot may grow, and how many a game
    // may keep.
    core::u64 saveMaxSlotBytes = 4u * 1024u * 1024u;
    core::u32 saveMaxSlots = 64;

    // `[scene] close_grace_seconds` (ADR 0124): how long a scene change waits
    // for the old scene's `scene:BindToClose` handlers.
    core::f64 sceneCloseGrace = 5.0;

    // `[script] max_memory_mb`: the cap on the scripts' heap. The
    // defaults are generous, so no ordinary game meets it,
    // and a table growing without end stops before the machine does.
#if defined(__ANDROID__)
    core::u32 scriptMemoryMb = 512;
#else
    core::u32 scriptMemoryMb = 1024;
#endif
    // `[script] timeout` and `load_timeout`: the seconds of its own
    // running time a script may go without yielding before it is stopped,
    // and the longer limit for what it does as it loads -- its body, the
    // modules it requires. Running time, not the wall's: a busy machine
    // stops nothing.
    core::f64 scriptTimeout = 5.0;
    core::f64 scriptLoadTimeout = 30.0;

    // `[debug] frame_report_seconds`: a line in the log every so many seconds
    // saying how the frames went -- for a machine nobody is sitting at with a
    // profiler, a phone above all. Zero, the default, says nothing.
    core::f64 frameReportSeconds = 0.0;
    // **The measuring keys** (ADR 0171; `debug_measure.h`), each off unless the
    // project's file asks: `[debug] gpu_pass_times` (the GPU's time by pass,
    // under the frame report), `hide` (a list of what is not drawn),
    // `shadow_taps` (the sun's shadow filter, 1 through 15 taps) and
    // `log_ui_touches` (which element took each finger).
    bool gpuPassTimes = false;
    std::string debugHide;
    // `[debug] skip`: a list of the lighting terms left out (`debug_measure.h`).
    std::string debugSkip;
    core::u32 shadowTaps = 0;
    bool logUiTouches = false;
    // **`[debug] launch_arguments`: whether a phone's build takes arguments
    // when it is launched** (ADR 0171) -- the host's flags, as an intent's
    // `args` extra. Off unless the project says: an installed game is launched
    // by anything on the phone, and a game that ships is not one a stranger's
    // intent may hand `--hide=` to.
    bool launchArguments = false;
    // `[debug] astc = false`: see `EngineOptions::astcTextures`.
    bool astcTextures = true;
    // `[debug] overlay_key`: the key that opens the host's overlay in a run of
    // the project being made, by `Enum.KeyCode`'s name for it -- "F3" unless
    // the project says, "None" for no key at all. The host takes that key: the
    // game does not hear it.
    std::string overlayKey = "F3";

    // **The game's identity** (ADR 0104 §1): what every export stamps.
    // `[project] version` is `X.Y.Z` or empty -- anything else is reported and
    // left empty rather than stamped wrong -- and `[project] company` is who
    // makes it.
    std::string version;
    std::string company;
    // `[window] fullscreen` and `resizable`: how the player's window starts on
    // a desktop. See `startsFullscreen`.
    bool fullscreen = false;
    bool resizable = true;

    render::GraphicsSettings graphics;

    // **The same, as the layers they came from** (ADR 0147): what each preset
    // gives, what the file said and what the command line said, apart -- so a
    // script's write and a player's saved choice can be put between them, and
    // `GraphicsService:GetSource` can say whose a value is. `graphics` above
    // is what these resolve to before either has spoken.
    scene::GraphicsModel graphicsModel;
    // **Each level's own values**, `[graphics.low]` to `[graphics.ultra]`
    // (ADR 0147, section 6, the quality matrix): what the project makes of a
    // level, over what the engine's level gives. Already in `graphicsModel`'s
    // presets; apart here, so the editor can say which values are the
    // project's and which the engine's.
    std::array<scene::GraphicsLayer, scene::kQualityPresets> levelSettings;
    // `[display] remember_player_settings`: whether the player's choices are
    // kept in their folder and read at start. On unless a game manages its own.
    bool rememberPlayerSettings = true;

    // `[project] default_locale` (ADR 0154): the locale a key falls back to,
    // and the one a player starts in when the system's is none of the game's.
    std::string defaultLocale = "en";
};

// Whether this is a device held in the hand: a phone or a tablet, where a game
// has the whole display or it has a strip of it.
#if defined(__ANDROID__)
inline constexpr bool Handheld = true;
#else
inline constexpr bool Handheld = false;
#endif

// **How a game's window starts**: on a desktop, what `[window]
// fullscreen` says. On a handheld, the whole display whatever it says --
// immersive, the system's bars hidden until swiped for, the frame drawn under
// the camera cutout, and `UIService.SafeAreaInsets` saying where a HUD may go.
// The key is not read there because it is a desktop's question, and the
// editor's Project Settings writes `false` for every game that never asked it.
[[nodiscard]] constexpr bool startsFullscreen(const ProjectConfig& config, bool handheld = Handheld) noexcept
{
    return handheld || config.fullscreen;
}

// Reads `<projectRoot>/project.toml`, resolves the graphics family through its
// three layers -- preset, then the file, then the overrides -- and clamps the
// result.
//
// **A project with no file is not an error**, and neither is a project whose
// file says nothing about graphics: both mean "the defaults, plus whatever the
// command line said". A file that fails to PARSE is a different thing and is
// reported: a config format that quietly ignores what it cannot read is how a
// setting ends up not applying.
//
// `diagnostic` is filled on a parse failure and is developer-facing (R3 exempt,
// like every other config diagnostic in the engine).
// The file's pacing under the command line's (ADR 0147): a flag typed
// beats `[display]`, key by key.
[[nodiscard]] FramePacing pacingWith(FramePacing file, const GraphicsOverrides& overrides) noexcept;

//
// `handheld` is what the build is (`Handheld`); a test says otherwise to ask
// what a phone would be given.
[[nodiscard]] ProjectConfig loadProjectConfig(const std::filesystem::path& projectRoot,
                                              const GraphicsOverrides& overrides, std::string* diagnostic = nullptr,
                                              bool handheld = Handheld);

// Writes one setting into `<projectRoot>/project.toml`, leaving the rest of the
// file byte for byte.
//
// **An edit, not a save.** A project file is a document somebody wrote -- every
// one in this repository opens with a paragraph explaining why its settings are
// what they are -- so a Settings dialog that serialised `ProjectConfig` back
// would delete all of it the first time anybody changed a window title.
// `core::setTomlValue` finds the line and replaces what is right of the `=`.
//
// `key` is the dotted path the reader uses (`window.title`, `graphics.quality`)
// and `rendered` is the TOML literal -- `core::tomlString` and friends produce
// it. A project with no file yet gets one.
//
// Returns false and fills `diagnostic` when the file cannot be read or written,
// or when the result would not parse -- which is the check that stops a Settings
// dialog turning a working project into one the engine refuses to open.
[[nodiscard]] bool writeProjectSetting(const std::filesystem::path& projectRoot, std::string_view key,
                                       std::string_view rendered, std::string* diagnostic = nullptr);

// Takes one setting out of `<projectRoot>/project.toml`, leaving the rest of
// the file byte for byte: what a dialog does to say a setting is the level's
// again, where writing the level's value would pin it. A key that is not there
// is not an error.
[[nodiscard]] bool removeProjectSetting(const std::filesystem::path& projectRoot, std::string_view key,
                                        std::string* diagnostic = nullptr);

// **The Luau flags, written as Project Settings shows them** (ADR 0217): each
// that differs from the engine's default under `[luau.flags]`, and each the
// file said (`written`) that no longer does taken out -- never pinned at the
// default. A name this Luau does not have stays in the file as it was: it is
// the file's, and said at every start. `settings` is what is in force after.
struct LuauFlagsWritten
{
    bool ok = true;
    bool changed = false;
    std::vector<LuauFlagSetting> settings;
};
[[nodiscard]] LuauFlagsWritten writeLuauFlagSettings(const std::filesystem::path& projectRoot,
                                                     std::span<const LuauFlagInfo> flags,
                                                     const std::map<std::string, core::i64, std::less<>>& written,
                                                     std::string* diagnostic = nullptr);

// A setting's key in a project file: its table and its name in snake case
// -- `graphics.shadow_quality`, `display.window_mode` -- and the words a file
// names each of its choices by, in their order; empty for a setting that is
// not one of a set.
[[nodiscard]] std::string projectKeyOf(scene::GraphicsSetting setting);
[[nodiscard]] std::span<const std::string_view> projectChoicesOf(scene::GraphicsSetting setting) noexcept;

// **What the model's values are to the renderer** (ADR 0147): the preset in
// force with every setting the layers say over it, clamped. `instruments`
// carries what is no setting -- a forced surface, a debug view -- from the
// run's own start. For a model as `loadProjectConfig` seeded it this is
// `ProjectConfig::graphics` exactly, which a test holds.
// Whether the packaged game at `packagedProject` lets its launcher hand it
// arguments: its `[debug] launch_arguments`. A folder with no project file
// does not.
[[nodiscard]] bool launchArgumentsAllowed(const std::filesystem::path& packagedProject);

// **What a handheld adds to a game's pacing** (ADR 0173): a cap of
// `HandheldFrameRate` where the game names none, and the adaptive rate. A
// desk's is returned as it came.
[[nodiscard]] FramePacing handheldPacing(FramePacing pacing, bool handheld) noexcept;

[[nodiscard]] render::GraphicsSettings graphicsSettingsOf(const scene::GraphicsModel& model, bool handheld = Handheld,
                                                          const render::GraphicsSettings* instruments = nullptr);
// And to the frame's pacing. A handheld's sync is always on.
[[nodiscard]] FramePacing pacingOf(const scene::GraphicsModel& model, bool handheld = Handheld) noexcept;

// **The texture quality a machine with this much memory starts at**:
// `Enum.TextureQuality`'s number -- 0 low, a quarter of each texture's size;
// 1 medium, a half; 2 high, all of it. Under three gigabytes is low, under
// five is medium. Every quality level starts there: how much of a texture a
// machine can hold is its memory's to say, not its graphics card's speed --
// a phone with twelve gigabytes at `Medium` keeps its textures whole, and
// one with two does not load at `High` what it cannot hold. Nought, for a
// machine that does not say, is high. A game or a player sets another.
[[nodiscard]] core::i32 textureQualityForMemory(core::u64 systemMemoryBytes) noexcept;
// How many of a texture's largest levels the model's texture quality leaves
// out: what `render::MeshLoader::setTextureSkip` is given.
[[nodiscard]] core::u32 textureSkipOf(const scene::GraphicsModel& model) noexcept;

// The host's layers of a model with no project file: the presets and the
// command line's.
void seedGraphicsModel(scene::GraphicsModel& model, const GraphicsOverrides& overrides, bool handheld = Handheld);

// **The player's own choices, in their folder** (ADR 0147 section 4):
// `settings.json`, a setting's name to its value, holding only what they
// changed. Writing replaces the file whole. Reading takes what it understands:
// a name that is no setting, or a value that is not that setting's kind, is
// left out and named in `refused` -- reported, not applied.
//
// The same file keeps the language they chose (ADR 0154), as `"locale"`: empty
// is "none chosen", and is not written.
//
// And what they hear (ADR 0200): the voice language they chose, as
// `"voice_locale"` (not written while it follows the text language); their
// volumes, as `"audio"`, each only where it is not full; and how they want
// subtitles, as `"subtitles"`, each only where they said. A file from before
// any of them reads as "nothing said".
struct PlayerHearing
{
    std::string voiceLocale;
    scene::PlayerSound sound;
};
[[nodiscard]] bool writePlayerGraphics(const std::filesystem::path& file, const scene::GraphicsLayer& choices,
                                       std::string_view locale = {}, const PlayerHearing* hearing = nullptr);
[[nodiscard]] bool readPlayerGraphics(const std::filesystem::path& file, scene::GraphicsLayer& choices,
                                      std::vector<std::string>* refused = nullptr, std::string* locale = nullptr,
                                      PlayerHearing* hearing = nullptr);

// The same resolution without a file, for a bare script or a project that has
// none.
[[nodiscard]] render::GraphicsSettings resolveGraphics(const GraphicsOverrides& overrides, bool handheld = Handheld);

// **Whose saves a run keeps** (ADR 0111).
enum class SaveHome : core::u8
{
    // The project's own `.engine/saves/`: a run of a project that is being
    // made.
    Project,
    // The player's folder for the game: the game somebody was given.
    Player,
};

// The player's folder is the PACKAGED game's -- the `game/` beside the
// executable, which is what an export is -- and only on a run that is nobody's
// test of it. Everything else is a project being made, whoever started it and
// however: the editor's Play, `ludwerk dev`, a match's windows, and the host
// handed a project folder on the command line.
//
// It was the other way round -- the project's folder only for the three runs
// the engine could name -- so a plain `engine-host <project>` saved over the
// developer's own copy of the game in `%APPDATA%`, which the saves page
// promised no test run would.
[[nodiscard]] constexpr SaveHome saveHomeFor(bool packagedGame, bool developmentRun) noexcept
{
    return packagedGame && !developmentRun ? SaveHome::Player : SaveHome::Project;
}

} // namespace engine::app
