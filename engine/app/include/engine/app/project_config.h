// What `project.toml` tells the ENGINE (api-design.md §4, roadmap M8).
//
// The project file has been the CLI's since M3 -- `tools/cli/project.luau`
// reads it with the TOML subset in `tools/cli/toml.luau`. M8 is the first
// milestone where the host needs it too, and the reason is packaging: a game
// built with `ludwerk build` runs `engine-host` directly, with no Lute anywhere
// near it, so a window title and a graphics preset that only the CLI could read
// would be settings that stop applying the moment the game ships.
//
// Only the keys the host acts on are read here. Everything else in the file --
// `[dev] port`, `[assets]`, `[build]` -- belongs to tools that run before the
// engine does, and reading it here would be duplicating a decision.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/frame_pacing.h"
#include "engine/core/types.h"
#include "engine/render/settings.h"
#include "engine/scene/graphics_model.h"

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
    std::optional<bool> antiAliasing;
    std::optional<bool> autoExposure;
    std::optional<bool> contactShadows;
    // `--vsync`, `--no-vsync`, `--max-frame-rate=N`, `--background-frame-rate=N`
    // (ADR 0147, G0), over `[display]`.
    std::optional<bool> vsync;
    std::optional<core::u32> maxFrameRate;
    std::optional<core::u32> backgroundFrameRate;
    // `--force-surface=NAME`: a test instrument (ADR 0091), never a project setting.
    std::optional<std::string> forcedSurface;
    // `--debug-view=NAME`: a test instrument (terrain audit T0).
    std::optional<render::DebugView> debugView;
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
    // passthrough for at M1.
    std::string windowTitle;

    // `[window] size`, or zero when the file does not say.
    core::i32 windowWidth = 0;
    core::i32 windowHeight = 0;

    // **How fast frames are made** (ADR 0147, G0), `[display]`: `vsync`
    // (on unless it says otherwise), `max_frame_rate` (0, no cap) and
    // `background_frame_rate` (10; 0, no throttle), each under its flag.
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

    // `[network] role = "server"`: the project is a dedicated server's package,
    // and the player given no posture starts as `--serve` (ADR 0105). Written
    // by `ludwerk build --target=*-server` into the package's own copy, so the
    // server starts with no arguments.
    bool serverRole = false;

    // `[network] timeout` -- seconds the other end may go silent before this
    // machine says it is gone (D208). From 1 to 120; ten by default.
    core::u32 networkTimeoutSeconds = 10;

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

    // `[script] max_memory_mb` (audit S8): the cap on the scripts' heap. The
    // owner's defaults, 2026-09-28: generous, so no ordinary game meets it,
    // and a table growing without end stops before the machine does.
#if defined(__ANDROID__)
    core::u32 scriptMemoryMb = 512;
#else
    core::u32 scriptMemoryMb = 1024;
#endif

    // `[debug] frame_report_seconds`: a line in the log every so many seconds
    // saying how the frames went -- for a machine nobody is sitting at with a
    // profiler, a phone above all. Zero, the default, says nothing.
    core::f64 frameReportSeconds = 0.0;
    // `[debug] overlay_key`: the key that opens the host's overlay in a run of
    // the project being made, by `Enum.KeyCode`'s name for it -- "F3" unless
    // the project says, "None" for no key at all. The host takes that key: the
    // game does not hear it (D461).
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

// **How a game's window starts** (D416): on a desktop, what `[window]
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
// The file's pacing under the command line's (ADR 0147, G0): a flag typed
// beats `[display]`, key by key.
[[nodiscard]] FramePacing pacingWith(FramePacing file, const GraphicsOverrides& overrides) noexcept;

//
// `handheld` is what the build is (`Handheld`); a test says otherwise to ask
// what a phone would be given.
[[nodiscard]] ProjectConfig loadProjectConfig(const std::filesystem::path& projectRoot,
                                              const GraphicsOverrides& overrides, std::string* diagnostic = nullptr,
                                              bool handheld = Handheld);

// Writes one setting into `<projectRoot>/project.toml`, leaving the rest of the
// file byte for byte (S5.7).
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
[[nodiscard]] render::GraphicsSettings graphicsSettingsOf(const scene::GraphicsModel& model, bool handheld = Handheld,
                                                          const render::GraphicsSettings* instruments = nullptr);
// And to the frame's pacing. A handheld's sync is always on.
[[nodiscard]] FramePacing pacingOf(const scene::GraphicsModel& model, bool handheld = Handheld) noexcept;

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
[[nodiscard]] bool writePlayerGraphics(const std::filesystem::path& file, const scene::GraphicsLayer& choices,
                                       std::string_view locale = {});
[[nodiscard]] bool readPlayerGraphics(const std::filesystem::path& file, scene::GraphicsLayer& choices,
                                      std::vector<std::string>* refused = nullptr, std::string* locale = nullptr);

// The same resolution without a file, for a bare script or a project that has
// none.
[[nodiscard]] render::GraphicsSettings resolveGraphics(const GraphicsOverrides& overrides, bool handheld = Handheld);

// **Whose saves a run keeps** (ADR 0111, D445).
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
