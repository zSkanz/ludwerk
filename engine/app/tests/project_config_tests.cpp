// The three layers of `[graphics]` -- preset, file, command line -- and the one
// rule between them that is not obvious (D052).
//
// `project_config.cpp` had no test of its own until this defect: the flagship
// was the only project file in the repository, so the layering was exercised
// exactly once and always with the same answer. What that hid is what happens
// when a PLAYER contradicts the file, which is the case the layering exists for.

#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>

#include "engine/app/project_config.h"
#include "engine/core/toml_edit.h"
#include "engine/platform/file.h"

using namespace engine;

namespace {

// A project directory with one `project.toml` in it. Under the test binary's own
// temporary directory, and removed by the caller's scope guard below.
struct ProjectDir
{
    explicit ProjectDir(const std::string& contents)
    {
        path = std::filesystem::temp_directory_path() /
               ("engine-project-config-" + std::to_string(std::hash<std::string>{}(contents)));
        std::filesystem::create_directories(path);
        std::ofstream file(path / "project.toml", std::ios::binary);
        file << contents;
    }

    ~ProjectDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    ProjectDir(const ProjectDir&) = delete;
    ProjectDir& operator=(const ProjectDir&) = delete;

    std::filesystem::path path;
};

// Compared with a tolerance in f32 rather than through `doctest::Approx`, whose
// double operand would promote every one of these and trip Clang's
// -Wdouble-promotion in the Tier-2 build.
[[nodiscard]] bool sameMetres(core::f32 left, core::f32 right) noexcept
{
    return std::fabs(left - right) < 1e-3f;
}

// What the flagship's file says, reduced to the part this is about: a level, and
// two shadow keys that refine it.
constexpr const char* kAuthoredHigh = "[project]\n"
                                      "name = \"Test World\"\n"
                                      "\n"
                                      "[graphics]\n"
                                      "quality = \"high\"\n"
                                      "shadow_resolution = 2048\n"
                                      "shadow_distance = 140.0\n";

} // namespace

TEST_CASE("a project file refines the level it names")
{
    const ProjectDir project(kAuthoredHigh);
    const app::ProjectConfig config = app::loadProjectConfig(project.path, {});

    CHECK(config.name == "Test World");
    CHECK(config.graphics.quality == render::QualityLevel::High);
    // The file's own two keys, over the preset's.
    CHECK(config.graphics.shadowTileResolution == 2048u);
    CHECK(sameMetres(config.graphics.shadowDistance, 140.0f));
    // And everything it did not mention is still the preset's.
    CHECK(config.graphics.shadowCascades == render::settingsFor(render::QualityLevel::High).shadowCascades);
}

TEST_CASE("a player's preset replaces the file's refinements, not just its level")
{
    const ProjectDir project(kAuthoredHigh);

    app::GraphicsOverrides overrides;
    overrides.quality = render::QualityLevel::Low;
    const app::ProjectConfig config = app::loadProjectConfig(project.path, overrides);

    // **The whole of D052's second half.** Before it, `--quality=low` took the
    // Low preset and then let the file put a 4096-pixel shadow atlas back on
    // top of it -- the single heaviest dial in the file, on the machine that
    // just asked for less. A preset the player names is the preset they get.
    const render::GraphicsSettings low = render::settingsFor(render::QualityLevel::Low);
    CHECK(config.graphics.quality == render::QualityLevel::Low);
    CHECK(config.graphics.shadowTileResolution == low.shadowTileResolution);
    CHECK(sameMetres(config.graphics.shadowDistance, low.shadowDistance));

    // The file's non-graphics half is untouched: it is not a performance dial.
    CHECK(config.name == "Test World");
}

TEST_CASE("a flag beats the file and the preset both")
{
    const ProjectDir project(kAuthoredHigh);

    app::GraphicsOverrides overrides;
    overrides.quality = render::QualityLevel::Low;
    overrides.shadowResolution = 1024;
    const app::ProjectConfig config = app::loadProjectConfig(project.path, overrides);

    // Typed by the same person as the preset, so it is not the file's
    // refinement coming back -- it is the outer layer being explicit.
    CHECK(config.graphics.shadowTileResolution == 1024u);
    CHECK(config.graphics.quality == render::QualityLevel::Low);
}

TEST_CASE("a project says how fast its frames are made, and a flag says otherwise")
{
    // ADR 0147, G0: `[display]`. With no file a window waits for its display,
    // has no cap, and is drawn ten times a second while nobody looks.
    const app::ProjectConfig bare = app::loadProjectConfig({}, {});
    CHECK(bare.pacing.vsync);
    CHECK(bare.pacing.maxFrameRate == 0u);
    CHECK(bare.pacing.backgroundFrameRate == 10u);

    const ProjectDir project("[project]\nname = \"Paced\"\n\n[display]\nvsync = false\nmax_frame_rate = 144\n"
                             "background_frame_rate = 0\n");
    const app::ProjectConfig config = app::loadProjectConfig(project.path, {});
    CHECK_FALSE(config.pacing.vsync);
    CHECK(config.pacing.maxFrameRate == 144u);
    CHECK(config.pacing.backgroundFrameRate == 0u);

    // A rate nobody can mean is not taken: the default stands.
    const ProjectDir absurd("[display]\nmax_frame_rate = 100000\nbackground_frame_rate = -3\n");
    const app::ProjectConfig kept = app::loadProjectConfig(absurd.path, {});
    CHECK(kept.pacing.maxFrameRate == 0u);
    CHECK(kept.pacing.backgroundFrameRate == 10u);

    // `--vsync --max-frame-rate=60` over the file, key by key.
    app::GraphicsOverrides flags;
    flags.vsync = true;
    flags.maxFrameRate = 60;
    const app::FramePacing typed = app::pacingWith(config.pacing, flags);
    CHECK(typed.vsync);
    CHECK(typed.maxFrameRate == 60u);
    CHECK(typed.backgroundFrameRate == 0u);
}

TEST_CASE("a project with no file is the defaults plus the command line")
{
    app::GraphicsOverrides overrides;
    overrides.shadowDistance = 60.0f;

    const app::ProjectConfig config =
        app::loadProjectConfig(std::filesystem::temp_directory_path() / "engine-absent", overrides);

    CHECK(config.graphics.quality == render::QualityLevel::High);
    CHECK(sameMetres(config.graphics.shadowDistance, 60.0f));
    CHECK(config.name.empty());
}

// --- Writing a setting back (S5.7) -------------------------------------------

TEST_CASE("a setting written back is read back, and the file keeps its comments")
{
    // The round trip the Settings dialog is: read, change one thing, write, and
    // the next `loadProjectConfig` sees it. Everything else in the file -- the
    // paragraph at the top especially -- has to survive, because every project
    // file in this repository has one and a dialog that ate it would eat it the
    // first time anybody moved a slider.
    const ProjectDir project(R"(# What this project is, and why these settings are what they are.

[project]
name = "Before"

[graphics]
quality = "high"   # authored against, not demanded
)");

    std::string diagnostic;
    REQUIRE_MESSAGE(
        app::writeProjectSetting(project.path, "project.name", engine::core::tomlString("After"), &diagnostic),
        diagnostic);

    const app::ProjectConfig config = app::loadProjectConfig(project.path, app::GraphicsOverrides{});
    CHECK(config.name == "After");
    CHECK(config.graphics.quality == render::QualityLevel::High);

    std::string text;
    REQUIRE(engine::platform::readTextFile(project.path / "project.toml", text));
    CHECK(text.find("# What this project is") != std::string::npos);
    CHECK(text.find("# authored against, not demanded") != std::string::npos);
}

TEST_CASE("a project with no file yet gets one")
{
    // Otherwise the Settings dialog works on some projects and silently does
    // nothing on the rest -- and "the rest" is every project before somebody
    // first names its window.
    const std::filesystem::path fresh = std::filesystem::temp_directory_path() / "engine-project-config-fresh";
    std::error_code ignored;
    std::filesystem::remove_all(fresh, ignored);
    std::filesystem::create_directories(fresh);

    std::string diagnostic;
    REQUIRE_MESSAGE(
        app::writeProjectSetting(fresh, "window.title", engine::core::tomlString("A New Game"), &diagnostic),
        diagnostic);

    const app::ProjectConfig config = app::loadProjectConfig(fresh, app::GraphicsOverrides{});
    CHECK(config.windowTitle == "A New Game");
    std::filesystem::remove_all(fresh, ignored);
}

TEST_CASE("a write that would leave the file unreadable is refused before it happens")
{
    // The check that stops a Settings dialog turning a working project into one
    // the engine will not open. The edit is textual, so a value that is not a
    // TOML literal produces a file the reader refuses -- and writing it anyway
    // would be a dialog whose whole job is to be safe to poke at doing the one
    // unsafe thing.
    const ProjectDir project(R"([project]
name = "Intact"
)");

    std::string diagnostic;
    // Not run through `tomlString`, which is exactly the mistake this guards.
    CHECK_FALSE(app::writeProjectSetting(project.path, "project.name", "not a literal", &diagnostic));
    CHECK_FALSE(diagnostic.empty());

    const app::ProjectConfig config = app::loadProjectConfig(project.path, app::GraphicsOverrides{});
    CHECK(config.name == "Intact");
}

TEST_CASE("a game on a handheld fills the display, whatever its window key says (D416)")
{
    // On a phone a game was drawn in the strip between the status bar and the
    // navigation pill. `[window] fullscreen` is a desktop's question -- and the
    // editor's Project Settings writes it as `false` for every game that never
    // asked -- so on a handheld it is not read as "leave the bars up".
    {
        const ProjectDir project("[project]\nname = \"Silent\"\n");
        const app::ProjectConfig config = app::loadProjectConfig(project.path, app::GraphicsOverrides{});
        CHECK(app::startsFullscreen(config, true));
        // A desktop game starts in a window, as it did.
        CHECK_FALSE(app::startsFullscreen(config, false));
    }
    {
        const ProjectDir project("[window]\nfullscreen = false\n");
        const app::ProjectConfig config = app::loadProjectConfig(project.path, app::GraphicsOverrides{});
        CHECK(app::startsFullscreen(config, true));
        CHECK_FALSE(app::startsFullscreen(config, false));
    }
    {
        const ProjectDir project("[window]\nfullscreen = true\n");
        const app::ProjectConfig config = app::loadProjectConfig(project.path, app::GraphicsOverrides{});
        CHECK(app::startsFullscreen(config, true));
        CHECK(app::startsFullscreen(config, false));
    }
    // No project file at all: the same answer as a file that says nothing.
    CHECK(app::startsFullscreen(app::ProjectConfig{}, true));
    CHECK_FALSE(app::startsFullscreen(app::ProjectConfig{}, false));
}

TEST_CASE("the window size round-trips as two integers")
{
    const ProjectDir project(R"([window]
size = [1280, 720]
)");

    const std::array<engine::core::f64, 2> wanted{1920.0, 1080.0};
    std::string diagnostic;
    REQUIRE_MESSAGE(
        app::writeProjectSetting(project.path, "window.size", engine::core::tomlNumberArray(wanted), &diagnostic),
        diagnostic);

    const app::ProjectConfig config = app::loadProjectConfig(project.path, app::GraphicsOverrides{});
    CHECK(config.windowWidth == 1920);
    CHECK(config.windowHeight == 1080);

    // As integers, so a save is not a diff on a value nobody changed.
    std::string text;
    REQUIRE(engine::platform::readTextFile(project.path / "project.toml", text));
    CHECK(text.find("[1920, 1080]") != std::string::npos);
}

TEST_CASE("depth of field and sun rays are the machine's to turn off, by preset or by key (ADR 0096)")
{
    // The presets: a weak machine draws neither, and the engine's own level
    // draws both -- a world that asks for them there gets them.
    CHECK_FALSE(render::settingsFor(render::QualityLevel::Low).depthOfField);
    CHECK_FALSE(render::settingsFor(render::QualityLevel::Low).sunRays);
    CHECK(render::settingsFor(render::QualityLevel::Medium).sunRays);
    CHECK_FALSE(render::settingsFor(render::QualityLevel::Medium).depthOfField);
    CHECK(render::settingsFor(render::QualityLevel::High).depthOfField);
    CHECK(render::settingsFor(render::QualityLevel::High).sunRays);

    const ProjectDir project("[graphics]\n"
                             "quality = \"high\"\n"
                             "depth_of_field = false\n"
                             "sun_rays = false\n");
    const app::ProjectConfig config = app::loadProjectConfig(project.path, {});
    CHECK_FALSE(config.graphics.depthOfField);
    CHECK_FALSE(config.graphics.sunRays);
}

TEST_CASE("numbers no setting can hold are ignored before they are converted (audit F14)")
{
    // A negative or huge value converted to an unsigned integer is undefined
    // behaviour, not a large number; each of these is a key the file did not
    // give, and the preset's value stands.
    const ProjectDir project("[project]\n"
                             "name = \"Wild\"\n"
                             "[graphics]\n"
                             "quality = \"high\"\n"
                             "shadow_resolution = -5\n"
                             "shadow_cascades = 1e30\n"
                             "light_budget = -1\n"
                             "[window]\n"
                             "size = [1e30, 720]\n");
    const app::ProjectConfig config = app::loadProjectConfig(project.path, {});
    const render::GraphicsSettings preset = render::settingsFor(render::QualityLevel::High);
    CHECK(config.graphics.shadowTileResolution == preset.shadowTileResolution);
    CHECK(config.graphics.shadowCascades == preset.shadowCascades);
    CHECK(config.graphics.lightBudget == preset.lightBudget);
    const app::ProjectConfig defaults = app::loadProjectConfig(std::filesystem::path("no-such-project-dir"), {});
    CHECK(config.windowWidth == defaults.windowWidth);
    CHECK(config.windowHeight == defaults.windowHeight);
}

TEST_CASE("a handheld starts a level lower and renders the world under a cap (ADR 0147, the mobile ledger)")
{
    // The owner's phone ran the desktop's `High` at every one of its 1440
    // rows, and the frame rate halved inside a minute as it warmed.
    const ProjectDir project("[project]\nname = \"Silent\"\n");
    const app::ProjectConfig phone = app::loadProjectConfig(project.path, app::GraphicsOverrides{}, nullptr, true);
    CHECK(phone.graphics.quality == render::QualityLevel::Medium);
    CHECK(phone.graphics.renderResolutionCap == 900u);
    // 3120 by 1440: rendered at 900 rows, the same shape.
    CHECK(sameMetres(render::effectiveRenderScale(phone.graphics, 3120u, 1440u), 900.0f / 1440.0f));
    // Held upright the shorter side is the width, and the cap is on that.
    CHECK(sameMetres(render::effectiveRenderScale(phone.graphics, 1440u, 3120u), 900.0f / 1440.0f));
    // A display already under the cap is rendered whole.
    CHECK(sameMetres(render::effectiveRenderScale(phone.graphics, 1280u, 720u), 1.0f));

    // A desktop is what it was: `High`, every pixel.
    const app::ProjectConfig desktop = app::loadProjectConfig(project.path, app::GraphicsOverrides{}, nullptr, false);
    CHECK(desktop.graphics.quality == render::QualityLevel::High);
    CHECK(desktop.graphics.renderResolutionCap == 0u);
    CHECK(sameMetres(render::effectiveRenderScale(desktop.graphics, 3840u, 2160u), 1.0f));

    // No project at all: the same two answers.
    CHECK(app::resolveGraphics(app::GraphicsOverrides{}, true).quality == render::QualityLevel::Medium);
    CHECK(app::resolveGraphics(app::GraphicsOverrides{}, true).renderResolutionCap == 900u);
    CHECK(app::resolveGraphics(app::GraphicsOverrides{}, false).quality == render::QualityLevel::High);

    // A level somebody named is that level, with the handheld's cap for it.
    app::GraphicsOverrides low;
    low.quality = render::QualityLevel::Low;
    CHECK(app::resolveGraphics(low, true).renderResolutionCap == 720u);
    app::GraphicsOverrides ultra;
    ultra.quality = render::QualityLevel::Ultra;
    CHECK(app::resolveGraphics(ultra, true).renderResolutionCap == 0u);
}

TEST_CASE("a platform has a table of its own, over the project's (ADR 0147)")
{
    const ProjectDir project("[graphics]\n"
                             "quality = \"high\"\n"
                             "shadow_distance = 140.0\n"
                             "\n"
                             "[graphics.android]\n"
                             "quality = \"low\"\n"
                             "render_cap = 600\n"
                             "bloom = true\n");
    // On a phone: the platform's level, the project's refinement, the
    // platform's own on top.
    const app::ProjectConfig phone = app::loadProjectConfig(project.path, app::GraphicsOverrides{}, nullptr, true);
    CHECK(phone.graphics.quality == render::QualityLevel::Low);
    CHECK(sameMetres(phone.graphics.shadowDistance, 140.0f));
    CHECK(phone.graphics.renderResolutionCap == 600u);
    CHECK(phone.graphics.bloom);

    // On a desktop the phone's table is not read.
    const app::ProjectConfig desktop = app::loadProjectConfig(project.path, app::GraphicsOverrides{}, nullptr, false);
    CHECK(desktop.graphics.quality == render::QualityLevel::High);
    CHECK(desktop.graphics.renderResolutionCap == 0u);

    // A cap is a project's to give a desktop too, and zero takes a phone's away.
    const ProjectDir capped("[graphics]\nrender_cap = 1080\n\n[graphics.android]\nrender_cap = 0\n");
    CHECK(app::loadProjectConfig(capped.path, app::GraphicsOverrides{}, nullptr, false).graphics.renderResolutionCap ==
          1080u);
    CHECK(app::loadProjectConfig(capped.path, app::GraphicsOverrides{}, nullptr, true).graphics.renderResolutionCap ==
          0u);
}

TEST_CASE("a project asks for a line about its frames every few seconds")
{
    const ProjectDir silent("[project]\nname = \"Silent\"\n");
    CHECK(app::loadProjectConfig(silent.path, app::GraphicsOverrides{}).frameReportSeconds == 0.0);
    const ProjectDir reporting("[debug]\nframe_report_seconds = 10\n");
    CHECK(app::loadProjectConfig(reporting.path, app::GraphicsOverrides{}).frameReportSeconds == 10.0);
}

TEST_CASE("D445: only the packaged game, run as a game, keeps its saves in the player's folder")
{
    // `engine-host <project>` by hand wrote into `%APPDATA%/<company>/<name>`
    // -- the folder the developer's own installed copy of the game keeps its
    // saves in -- because the project's folder was for the three runs the
    // engine could name and the player's was for everything else.
    using app::SaveHome;
    // A project handed to the host: a test run, whoever started it.
    CHECK(app::saveHomeFor(false, false) == SaveHome::Project);
    // The editor's Play, `ludwerk dev`, a match's window.
    CHECK(app::saveHomeFor(false, true) == SaveHome::Project);
    // The export, double-clicked.
    CHECK(app::saveHomeFor(true, false) == SaveHome::Player);
    // The export, driven by a development session: still a test.
    CHECK(app::saveHomeFor(true, true) == SaveHome::Project);
}
