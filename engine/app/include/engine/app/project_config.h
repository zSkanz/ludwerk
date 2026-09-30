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
#include <string>
#include <string_view>

#include "engine/core/types.h"
#include "engine/render/settings.h"

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

    // **The game's identity** (ADR 0104 §1): what every export stamps.
    // `[project] version` is `X.Y.Z` or empty -- anything else is reported and
    // left empty rather than stamped wrong -- and `[project] company` is who
    // makes it.
    std::string version;
    std::string company;
    // `[window] fullscreen` and `resizable`: how the player's window starts.
    bool fullscreen = false;
    bool resizable = true;

    render::GraphicsSettings graphics;
};

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
[[nodiscard]] ProjectConfig loadProjectConfig(const std::filesystem::path& projectRoot,
                                              const GraphicsOverrides& overrides, std::string* diagnostic = nullptr);

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

// The same resolution without a file, for a bare script or a project that has
// none.
[[nodiscard]] render::GraphicsSettings resolveGraphics(const GraphicsOverrides& overrides);

} // namespace engine::app
