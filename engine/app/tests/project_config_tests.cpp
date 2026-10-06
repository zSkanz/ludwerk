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
#include <vector>

#include "engine/app/project_config.h"
#include "engine/core/toml_edit.h"
#include "engine/platform/event.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"

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

TEST_CASE("a project names FSR 2 and a scale down to a third, and a flag names it over the file (ADR 0164)")
{
    const ProjectDir project("[project]\nname = \"Upscaled\"\n[graphics]\nupscaling = \"fsr2\"\nrender_scale = 0.1\n");
    const app::ProjectConfig config = app::loadProjectConfig(project.path, {});
    CHECK(config.graphics.upscaling == render::UpscalingMode::Fsr2);
    // A third is the floor: FSR 2's furthest mode. It was a half.
    CHECK(sameMetres(config.graphics.renderScale, 1.0f / 3.0f));

    // No preset chooses it, on a desktop or in the hand.
    for (const render::QualityLevel level : {render::QualityLevel::Low, render::QualityLevel::Medium,
                                             render::QualityLevel::High, render::QualityLevel::Ultra}) {
        CHECK(render::settingsFor(level).upscaling != render::UpscalingMode::Fsr2);
        CHECK(render::handheldSettings(render::settingsFor(level)).upscaling != render::UpscalingMode::Fsr2);
    }

    app::GraphicsOverrides flags;
    flags.upscaling = render::UpscalingMode::Fsr2;
    const ProjectDir plain("[project]\nname = \"Plain\"\n[graphics]\nupscaling = \"fsr1\"\n");
    CHECK(app::loadProjectConfig(plain.path, flags).graphics.upscaling == render::UpscalingMode::Fsr2);
}

TEST_CASE("a project turns frame generation on, a flag says otherwise, and no level does either (ADR 0165)")
{
    CHECK_FALSE(app::loadProjectConfig(std::filesystem::path{}, {}).graphics.frameGeneration);

    // The display's, beside the sync it holds on: `[graphics]` is the level's.
    const ProjectDir project("[project]\nname = \"Made\"\n[display]\nframe_generation = true\n");
    const app::ProjectConfig made = app::loadProjectConfig(project.path, {});
    CHECK(made.graphics.frameGeneration);
    CHECK(app::projectKeyOf(scene::GraphicsSetting::FrameGeneration) == "display.frame_generation");
    CHECK(made.graphicsModel.source(scene::GraphicsSetting::FrameGeneration) == scene::GraphicsSource::Project);
    // The level is still the level: it is not one of the things a level says.
    CHECK(made.graphicsModel.qualityLevel() == made.graphicsModel.preset());
    const ProjectDir misplaced("[project]\nname = \"Misplaced\"\n[graphics]\nframe_generation = true\n");
    CHECK_FALSE(app::loadProjectConfig(misplaced.path, {}).graphics.frameGeneration);
    // And a level from the command line does not take it away, as it takes a
    // file's refinements of its own level (D052).
    app::GraphicsOverrides low;
    low.quality = render::QualityLevel::Low;
    CHECK(app::loadProjectConfig(project.path, low).graphics.frameGeneration);

    // It costs every input half a frame: a level of quality is not who asks.
    for (const render::QualityLevel level : {render::QualityLevel::Low, render::QualityLevel::Medium,
                                             render::QualityLevel::High, render::QualityLevel::Ultra}) {
        CHECK_FALSE(render::settingsFor(level).frameGeneration);
        CHECK_FALSE(render::handheldSettings(render::settingsFor(level)).frameGeneration);
    }

    app::GraphicsOverrides off;
    off.frameGeneration = false;
    CHECK_FALSE(app::loadProjectConfig(project.path, off).graphics.frameGeneration);
    app::GraphicsOverrides on;
    on.frameGeneration = true;
    const ProjectDir plain("[project]\nname = \"Plain\"\n");
    const app::ProjectConfig asked = app::loadProjectConfig(plain.path, on);
    CHECK(asked.graphics.frameGeneration);
    // And the model a script reads says the same, and whose word it is.
    CHECK(asked.graphicsModel.effective(scene::GraphicsSetting::FrameGeneration) == 1.0);
    CHECK(asked.graphicsModel.source(scene::GraphicsSetting::FrameGeneration) == scene::GraphicsSource::CommandLine);
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

TEST_CASE("a project asks for the measuring keys, and has none unless it does (ADR 0171)")
{
    const ProjectDir silent("[project]\nname = \"Silent\"\n");
    const app::ProjectConfig none = app::loadProjectConfig(silent.path, app::GraphicsOverrides{});
    CHECK_FALSE(none.gpuPassTimes);
    CHECK(none.debugHide.empty());
    CHECK(none.shadowTaps == 0);
    CHECK_FALSE(none.logUiTouches);

    const ProjectDir measuring("[debug]\ngpu_pass_times = true\nhide = \"foliage, terrain\"\n"
                               "skip = \"shadow,fog\"\nshadow_taps = 4\nlog_ui_touches = true\n");
    const app::ProjectConfig asked = app::loadProjectConfig(measuring.path, app::GraphicsOverrides{});
    CHECK(asked.gpuPassTimes);
    CHECK(asked.debugHide == "foliage, terrain");
    CHECK(asked.debugSkip == "shadow,fog");
    CHECK(none.debugSkip.empty());
    CHECK(asked.shadowTaps == 4);
    CHECK(asked.logUiTouches);

    // The taps are an instrument: the settings a player changes keep them.
    engine::render::GraphicsSettings instruments;
    instruments.measuredShadowTaps = 4;
    CHECK(app::graphicsSettingsOf(asked.graphicsModel, false, &instruments).measuredShadowTaps == 4);
    CHECK(app::graphicsSettingsOf(asked.graphicsModel, false, nullptr).measuredShadowTaps == 0);
    // Sixteen is the most there are.
    instruments.measuredShadowTaps = 40;
    CHECK(engine::render::clampSettings(instruments).measuredShadowTaps == 16);
}

TEST_CASE("the shadow filter's taps follow the shadow quality: four, eight, sixteen (ADR 0172)")
{
    using engine::render::QualityLevel;
    CHECK(engine::render::settingsFor(QualityLevel::Low).shadowTaps == 4);
    CHECK(engine::render::settingsFor(QualityLevel::Medium).shadowTaps == 8);
    // Zero is the filter's sixteen.
    CHECK(engine::render::settingsFor(QualityLevel::High).shadowTaps == 0);
    CHECK(engine::render::settingsFor(QualityLevel::Ultra).shadowTaps == 0);

    // And a player who turns the shadows down alone gets the cheaper filter.
    const ProjectDir project("[project]\nname = \"Taps\"\n");
    app::ProjectConfig config = app::loadProjectConfig(project.path, app::GraphicsOverrides{}, nullptr, false);
    CHECK(app::graphicsSettingsOf(config.graphicsModel, false, nullptr).shadowTaps == 0);
    REQUIRE(config.graphicsModel.write(engine::scene::GraphicsSetting::ShadowQuality, 1.0));
    CHECK(app::graphicsSettingsOf(config.graphicsModel, false, nullptr).shadowTaps == 4);
    REQUIRE(config.graphicsModel.write(engine::scene::GraphicsSetting::ShadowQuality, 2.0));
    CHECK(app::graphicsSettingsOf(config.graphicsModel, false, nullptr).shadowTaps == 8);
    REQUIRE(config.graphicsModel.write(engine::scene::GraphicsSetting::ShadowQuality, 4.0));
    CHECK(app::graphicsSettingsOf(config.graphicsModel, false, nullptr).shadowTaps == 0);
}

TEST_CASE("a handheld's levels spend less on what a small screen shows least (ADR 0172)")
{
    using engine::render::QualityLevel;
    for (const QualityLevel level :
         {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        const engine::render::GraphicsSettings desk = engine::render::settingsFor(level);
        const engine::render::GraphicsSettings hand = engine::render::handheldSettings(desk);
        // Five passes of bloom where a desk has nine.
        CHECK(desk.bloomLevels == 5);
        CHECK(hand.bloomLevels == 3);
        // No contact shadows below High.
        if (level == QualityLevel::Low || level == QualityLevel::Medium)
            CHECK_FALSE(hand.contactShadows);
        else
            CHECK(hand.contactShadows == desk.contactShadows);
        // And no grass drawn again under a decal below High (ADR 0185): on a
        // desk only the lowest level leaves it out.
        CHECK(desk.foliageDecals == (level != QualityLevel::Low));
        if (level == QualityLevel::Low || level == QualityLevel::Medium)
            CHECK_FALSE(hand.foliageDecals);
        else
            CHECK(hand.foliageDecals);
        // The filter's taps are the level's on both.
        CHECK(hand.shadowTaps == desk.shadowTaps);
    }
    engine::render::GraphicsSettings wild;
    wild.bloomLevels = 40;
    CHECK(engine::render::clampSettings(wild).bloomLevels == 5);
    wild.bloomLevels = 0;
    CHECK(engine::render::clampSettings(wild).bloomLevels == 2);
}

TEST_CASE("a phone's launch arguments are read only by a game whose project allows them (ADR 0171)")
{
    const ProjectDir shipped("[project]\nname = \"Shipped\"\n");
    CHECK_FALSE(app::loadProjectConfig(shipped.path, app::GraphicsOverrides{}).launchArguments);
    CHECK_FALSE(app::launchArgumentsAllowed(shipped.path));
    const ProjectDir measured("[debug]\nlaunch_arguments = true\n");
    CHECK(app::launchArgumentsAllowed(measured.path));
    // No packaged game at all is no permission.
    CHECK_FALSE(app::launchArgumentsAllowed(std::filesystem::path{}));
}

TEST_CASE("a handheld's frames are capped at sixty and paced at a rate they hold, unless the game says (ADR 0173)")
{
    const ProjectDir plain("[project]\nname = \"Plain\"\n");
    const app::FramePacing desk = app::loadProjectConfig(plain.path, app::GraphicsOverrides{}, nullptr, false).pacing;
    CHECK(desk.maxFrameRate == 0);
    CHECK_FALSE(desk.adaptive);
    const app::ProjectConfig hand = app::loadProjectConfig(plain.path, app::GraphicsOverrides{}, nullptr, true);
    CHECK(hand.pacing.maxFrameRate == app::HandheldFrameRate);
    CHECK(hand.pacing.adaptive);
    // The settings a player changes keep both.
    CHECK(app::pacingOf(hand.graphicsModel, true).maxFrameRate == app::HandheldFrameRate);
    CHECK(app::pacingOf(hand.graphicsModel, true).adaptive);
    CHECK(app::pacingOf(hand.graphicsModel, false).maxFrameRate == 0);

    // A game that names a cap keeps it, and one that wants no pacing has none.
    const ProjectDir fast("[display]\nmax_frame_rate = 120\nadaptive_frame_rate = false\n");
    const app::FramePacing asked = app::loadProjectConfig(fast.path, app::GraphicsOverrides{}, nullptr, true).pacing;
    CHECK(asked.maxFrameRate == 120);
    CHECK_FALSE(asked.adaptive);
    // And a desk's game cannot ask for a handheld's pacing.
    const ProjectDir wishful("[display]\nadaptive_frame_rate = true\n");
    CHECK_FALSE(app::loadProjectConfig(wishful.path, app::GraphicsOverrides{}, nullptr, false).pacing.adaptive);
}

TEST_CASE("the lean ground is Low's, and a handheld's Medium's, unless the project says (ADR 0175)")
{
    using engine::render::GraphicsSettings;
    using engine::render::QualityLevel;
    CHECK(engine::render::settingsFor(QualityLevel::Low).leanTerrain());
    CHECK_FALSE(engine::render::settingsFor(QualityLevel::Medium).leanTerrain());
    CHECK_FALSE(engine::render::settingsFor(QualityLevel::High).leanTerrain());
    CHECK_FALSE(engine::render::settingsFor(QualityLevel::Ultra).leanTerrain());
    const auto hand = [](QualityLevel level) {
        return engine::render::handheldSettings(engine::render::settingsFor(level));
    };
    // A handheld's Low and Medium are the fast ground (ADR 0179), which is
    // not the lean one; from High up it is the full ground, as on a desk.
    using Surface = GraphicsSettings::TerrainSurface;
    CHECK(hand(QualityLevel::Low).terrainSurfaceNow() == Surface::Fast);
    CHECK(hand(QualityLevel::Medium).terrainSurfaceNow() == Surface::Fast);
    CHECK(hand(QualityLevel::Low).compiledTerrain());
    CHECK_FALSE(hand(QualityLevel::Low).leanTerrain());
    CHECK(hand(QualityLevel::High).terrainSurfaceNow() == Surface::Full);
    CHECK(engine::render::settingsFor(QualityLevel::Low).terrainSurfaceNow() == Surface::Lean);
    // And the project's word is over the handheld's level too.
    GraphicsSettings handLean = hand(QualityLevel::Low);
    handLean.terrainSurface = Surface::Lean;
    CHECK(handLean.leanTerrain());
    CHECK_FALSE(handLean.compiledTerrain());

    // The project's word, over the level's.
    const ProjectDir full("[graphics]\nquality = \"low\"\nterrain_surface = \"full\"\n");
    const app::ProjectConfig saidFull = app::loadProjectConfig(full.path, app::GraphicsOverrides{});
    CHECK(saidFull.graphics.terrainSurface == GraphicsSettings::TerrainSurface::Full);
    CHECK_FALSE(saidFull.graphics.leanTerrain());
    const ProjectDir lean("[graphics]\nquality = \"ultra\"\nterrain_surface = \"lean\"\n");
    const app::ProjectConfig saidLean = app::loadProjectConfig(lean.path, app::GraphicsOverrides{});
    CHECK(saidLean.graphics.leanTerrain());
    // And the flag over the file.
    app::GraphicsOverrides flag;
    flag.terrainSurface = GraphicsSettings::TerrainSurface::Full;
    CHECK_FALSE(app::loadProjectConfig(lean.path, flag).graphics.leanTerrain());

    // The ground's compiled variants (ADR 0179) are the project's word too,
    // and neither is the lean ground: each is a shader of its own.
    const ProjectDir fast("[graphics]\nquality = \"low\"\nterrain_surface = \"fast\"\n");
    const app::ProjectConfig saidFast = app::loadProjectConfig(fast.path, app::GraphicsOverrides{});
    CHECK(saidFast.graphics.terrainSurface == GraphicsSettings::TerrainSurface::Fast);
    CHECK(saidFast.graphics.compiledTerrain());
    CHECK_FALSE(saidFast.graphics.leanTerrain());
    flag.terrainSurface = GraphicsSettings::TerrainSurface::Flat;
    CHECK(app::loadProjectConfig(lean.path, flag).graphics.terrainSurface == GraphicsSettings::TerrainSurface::Flat);
    CHECK_FALSE(saidLean.graphics.compiledTerrain());
    CHECK(app::graphicsSettingsOf(saidFast.graphicsModel, false, &saidFast.graphics).compiledTerrain());

    // No setting of a player's is it: what the project said outlives theirs.
    CHECK(app::graphicsSettingsOf(saidLean.graphicsModel, false, &saidLean.graphics).leanTerrain());
    CHECK_FALSE(app::graphicsSettingsOf(saidFull.graphicsModel, false, &saidFull.graphics).leanTerrain());
    // With nothing said, a player who turns the level down gets the lean ground.
    const ProjectDir plain("[project]\nname = \"Plain\"\n");
    app::ProjectConfig config = app::loadProjectConfig(plain.path, app::GraphicsOverrides{}, nullptr, false);
    CHECK_FALSE(app::graphicsSettingsOf(config.graphicsModel, false, &config.graphics).leanTerrain());
    REQUIRE(config.graphicsModel.write(engine::scene::GraphicsSetting::QualityLevel, 0.0));
    CHECK(app::graphicsSettingsOf(config.graphicsModel, false, &config.graphics).leanTerrain());
}

TEST_CASE("--render-cap is the project's render_cap, said over it")
{
    const ProjectDir capped("[graphics]\nrender_cap = 900\n");
    CHECK(app::loadProjectConfig(capped.path, app::GraphicsOverrides{}).graphics.renderResolutionCap == 900);
    app::GraphicsOverrides flag;
    flag.renderCap = 540;
    const app::ProjectConfig said = app::loadProjectConfig(capped.path, flag);
    CHECK(said.graphics.renderResolutionCap == 540);
    CHECK(app::graphicsSettingsOf(said.graphicsModel, false, nullptr).renderResolutionCap == 540);
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

TEST_CASE("D461: a project names the key that opens the overlay, or says there is none")
{
    // F3 opened the overlay and reached the game's own F3 in the same press.
    // The host takes its key; a game that wants F3 moves the host's.
    const auto keyOf = [](std::string_view toml) {
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("engine-overlay-key-" + std::to_string(platform::nowNs()));
        std::filesystem::create_directories(root);
        std::ofstream(root / "project.toml") << toml;
        std::string diagnostic;
        const app::ProjectConfig config = app::loadProjectConfig(root, {}, &diagnostic);
        std::error_code error;
        std::filesystem::remove_all(root, error);
        return platform::keyFromName(config.overlayKey);
    };
    CHECK(keyOf("[project]\nname = 'a'\n") == platform::Key::F3);
    CHECK(keyOf("[debug]\noverlay_key = 'F9'\n") == platform::Key::F9);
    // No key: the overlay is opened from the menu, and every key is the game's.
    CHECK(keyOf("[debug]\noverlay_key = 'None'\n") == platform::Key::Unknown);
    CHECK(keyOf("[debug]\noverlay_key = 'not a key'\n") == platform::Key::Unknown);
}

// --- The same settings, as a model's layers (ADR 0147) ---------------------------

namespace {

// Field by field: the model resolves to the settings the loader resolved.
void checkSameSettings(const render::GraphicsSettings& left, const render::GraphicsSettings& right)
{
    CHECK(left.quality == right.quality);
    CHECK(sameMetres(left.renderScale, right.renderScale));
    CHECK(left.renderResolutionCap == right.renderResolutionCap);
    CHECK(left.shadowTileResolution == right.shadowTileResolution);
    CHECK(left.shadowCascades == right.shadowCascades);
    CHECK(sameMetres(left.shadowDistance, right.shadowDistance));
    CHECK(left.lightBudget == right.lightBudget);
    CHECK(left.bloom == right.bloom);
    CHECK(left.ambientOcclusion == right.ambientOcclusion);
    CHECK(left.contactShadows == right.contactShadows);
    CHECK(sameMetres(left.terrainPixelError, right.terrainPixelError));
    CHECK(left.antiAliasing == right.antiAliasing);
    CHECK(left.upscaling == right.upscaling);
    CHECK(sameMetres(left.sharpness, right.sharpness));
    CHECK(left.frameGeneration == right.frameGeneration);
    CHECK(left.depthOfField == right.depthOfField);
    CHECK(left.sunRays == right.sunRays);
    CHECK(left.autoExposure == right.autoExposure);
}

} // namespace

TEST_CASE("the model's layers resolve to exactly what the loader resolved")
{
    // Every way the three sources meet: no file, a file that refines its
    // level, a level from the command line, flags, a platform's own table,
    // values past what the renderer honours -- on a desktop and in the hand.
    const std::array<const char*, 5> files{
        "",
        kAuthoredHigh,
        "[graphics]\nquality = \"low\"\nbloom = true\nrender_scale = 0.3\nshadow_resolution = 3000\n"
        "light_budget = 9999\nanti_aliasing = false\ncontact_shadows = true\n",
        "[graphics]\nquality = \"ultra\"\nshadow_cascades = 2\ndepth_of_field = false\nsun_rays = false\n"
        "auto_exposure = false\nambient_occlusion = false\nanti_aliasing = \"taa\"\nsharpness = 0.5\n\n"
        "[graphics.android]\nquality = \"low\"\nrender_cap = 600\nupscaling = \"none\"\n",
        "[graphics]\nshadow_distance = 5\nrender_cap = 100\n\n"
        "[display]\nvsync = false\nmax_frame_rate = 90\nframe_generation = true\n",
    };
    std::array<app::GraphicsOverrides, 3> overrides;
    overrides[1].quality = render::QualityLevel::Medium;
    overrides[1].shadowResolution = 1024;
    overrides[2].bloom = false;
    overrides[2].renderScale = 0.8f;
    overrides[2].antiAliasing = render::AntiAliasingMode::Off;
    overrides[1].upscaling = render::UpscalingMode::Fsr1;
    overrides[1].sharpness = 0.6f;
    overrides[1].frameGeneration = true;
    overrides[2].vsync = true;
    overrides[2].maxFrameRate = 30;

    for (const char* file : files) {
        const ProjectDir project(file);
        for (const app::GraphicsOverrides& flags : overrides) {
            for (const bool handheld : {false, true}) {
                std::string diagnostic;
                const app::ProjectConfig config = app::loadProjectConfig(
                    file[0] == '\0' ? std::filesystem::path{} : project.path, flags, &diagnostic, handheld);
                CAPTURE(file);
                CAPTURE(handheld);
                checkSameSettings(app::graphicsSettingsOf(config.graphicsModel, handheld), config.graphics);

                const app::FramePacing pacing = app::pacingOf(config.graphicsModel, false);
                const app::FramePacing expected = app::pacingWith(config.pacing, flags);
                CHECK(pacing.vsync == expected.vsync);
                // The model holds what the game said; a handheld's cap of
                // sixty where it said none (ADR 0173) is the loader's and
                // `pacingOf`'s to add.
                CHECK(app::handheldPacing(pacing, handheld).maxFrameRate == expected.maxFrameRate);
                CHECK(app::pacingOf(config.graphicsModel, handheld).maxFrameRate == expected.maxFrameRate);
                CHECK(pacing.backgroundFrameRate == expected.backgroundFrameRate);
                // In the hand the display's sync is on whatever anybody says.
                CHECK(app::pacingOf(config.graphicsModel, true).vsync);
            }
        }
    }
}

TEST_CASE("the model says whose each value is: the flag's, the file's, the level's")
{
    const ProjectDir project(kAuthoredHigh);
    app::GraphicsOverrides flags;
    flags.bloom = false;
    const app::ProjectConfig config = app::loadProjectConfig(project.path, flags);
    const scene::GraphicsModel& model = config.graphicsModel;

    CHECK(model.source(scene::GraphicsSetting::Bloom) == scene::GraphicsSource::CommandLine);
    CHECK(model.source(scene::GraphicsSetting::ShadowDistance) == scene::GraphicsSource::Project);
    CHECK(model.source(scene::GraphicsSetting::QualityLevel) == scene::GraphicsSource::Project);
    CHECK(model.source(scene::GraphicsSetting::ShadowCascades) == scene::GraphicsSource::Preset);
    CHECK(model.source(scene::GraphicsSetting::MaxFrameRate) == scene::GraphicsSource::Engine);
    // High, with a flag and a refinement over it.
    CHECK(model.preset() == scene::kQualityHigh);
    CHECK(model.qualityLevel() == scene::kQualityCustom);
}

TEST_CASE("a project file names any setting by its name in snake case")
{
    const ProjectDir project("[graphics]\nshadow_quality = \"low\"\nterrain_detail = 2.0\nfoliage_density = 0.25\n"
                             "texture_quality = \"medium\"\nmaximum_lod_level = 2\nanisotropic_filtering = 4\n"
                             "animation_detail = 4\nanti_aliasing = \"off\"\n\n"
                             "[display]\nwindow_mode = \"fullscreen\"\nmonitor = 1\nresolution = [1920, 1080]\n"
                             "brightness = 0.25\nbackground_frame_rate = 0\nremember_player_settings = false\n");
    const app::ProjectConfig config = app::loadProjectConfig(project.path, {});
    const scene::GraphicsModel& model = config.graphicsModel;
    const auto value = [&model](scene::GraphicsSetting setting) { return model.effective(setting); };

    CHECK(value(scene::GraphicsSetting::ShadowQuality) == doctest::Approx(1.0));
    CHECK(value(scene::GraphicsSetting::TerrainDetail) == doctest::Approx(2.0));
    CHECK(value(scene::GraphicsSetting::FoliageDensity) == doctest::Approx(0.25));
    CHECK(value(scene::GraphicsSetting::AnimationDetail) == doctest::Approx(4.0));
    CHECK(value(scene::GraphicsSetting::TextureQuality) == doctest::Approx(1.0));
    CHECK(value(scene::GraphicsSetting::MaximumLODLevel) == doctest::Approx(2.0));
    CHECK(value(scene::GraphicsSetting::AnisotropicFiltering) == doctest::Approx(4.0));
    CHECK(value(scene::GraphicsSetting::AntiAliasing) == doctest::Approx(0.0));
    CHECK(value(scene::GraphicsSetting::WindowMode) == doctest::Approx(2.0));
    CHECK(value(scene::GraphicsSetting::Monitor) == doctest::Approx(1.0));
    CHECK(value(scene::GraphicsSetting::ResolutionWidth) == doctest::Approx(1920.0));
    CHECK(value(scene::GraphicsSetting::ResolutionHeight) == doctest::Approx(1080.0));
    CHECK(value(scene::GraphicsSetting::Brightness) == doctest::Approx(0.25));
    CHECK(value(scene::GraphicsSetting::BackgroundFrameRate) == doctest::Approx(0.0));
    CHECK_FALSE(config.rememberPlayerSettings);
    // Twice the detail is half the pixels a cell may cover.
    CHECK(sameMetres(app::graphicsSettingsOf(model, false).terrainPixelError, 1.0f));

    // `[window] fullscreen` is the whole display as a window.
    const ProjectDir old("[window]\nfullscreen = true\n");
    CHECK(app::loadProjectConfig(old.path, {}).graphicsModel.effective(scene::GraphicsSetting::WindowMode) ==
          doctest::Approx(1.0));
}

TEST_CASE("each level gives every group something, and the levels are in order")
{
    const app::ProjectConfig config = app::loadProjectConfig({}, {});
    const scene::GraphicsModel& model = config.graphicsModel;
    for (core::i32 level = scene::kQualityLow; level < scene::kQualityUltra; ++level) {
        CAPTURE(level);
        CHECK(model.presetValue(scene::GraphicsSetting::ShadowDistance, level) <
              model.presetValue(scene::GraphicsSetting::ShadowDistance, level + 1));
        CHECK(model.presetValue(scene::GraphicsSetting::TerrainDetail, level) <
              model.presetValue(scene::GraphicsSetting::TerrainDetail, level + 1));
        CHECK(model.presetValue(scene::GraphicsSetting::FoliageDensity, level) <=
              model.presetValue(scene::GraphicsSetting::FoliageDensity, level + 1));
        CHECK(model.presetValue(scene::GraphicsSetting::ShadowQuality, level) == doctest::Approx(level + 1.0));
    }
    // A machine with nothing said is at its own level, and it is that level.
    CHECK(model.qualityLevel() == scene::kQualityHigh);
    const app::ProjectConfig phone = app::loadProjectConfig({}, {}, nullptr, true);
    CHECK(phone.graphicsModel.qualityLevel() == scene::kQualityMedium);
}

TEST_CASE("the player's choices are written to their file and read back, and what cannot be used is named")
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("engine-player-settings-" + std::to_string(platform::nowNs()));
    const std::filesystem::path file = folder / "settings.json";

    scene::GraphicsLayer choices;
    choices.put(scene::GraphicsSetting::QualityLevel, 1.0);
    choices.put(scene::GraphicsSetting::VSync, 0.0);
    choices.put(scene::GraphicsSetting::MaxFrameRate, 144.0);
    choices.put(scene::GraphicsSetting::RenderScale, 0.75);
    choices.put(scene::GraphicsSetting::WindowMode, 2.0);
    REQUIRE(app::writePlayerGraphics(file, choices));

    scene::GraphicsLayer read;
    std::vector<std::string> refused;
    REQUIRE(app::readPlayerGraphics(file, read, &refused));
    CHECK(refused.empty());
    CHECK(read.said == choices.said);
    CHECK(read.at(scene::GraphicsSetting::RenderScale) == doctest::Approx(0.75));
    CHECK(read.at(scene::GraphicsSetting::MaxFrameRate) == doctest::Approx(144.0));
    CHECK(read.at(scene::GraphicsSetting::VSync) == doctest::Approx(0.0));

    // Only what was chosen is in the file.
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(text.find("\"VSync\": false") != std::string::npos);
    CHECK(text.find("ShadowDistance") == std::string::npos);

    // A hand-edited file: a setting of another build, a value of the wrong
    // kind, a choice that is not one, a level nobody may choose. Each is left
    // out and named; the rest is taken.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{\"version\": 1, \"settings\": {\"VSync\": false, \"Teleport\": 3, \"Bloom\": 1, "
               "\"WindowMode\": 9, \"QualityLevel\": 4, \"MaxFrameRate\": 59.5, \"ShadowDistance\": 5000}}";
    }
    refused.clear();
    REQUIRE(app::readPlayerGraphics(file, read, &refused));
    CHECK(read.says(scene::GraphicsSetting::VSync));
    CHECK_FALSE(read.says(scene::GraphicsSetting::Bloom));
    CHECK_FALSE(read.says(scene::GraphicsSetting::WindowMode));
    CHECK_FALSE(read.says(scene::GraphicsSetting::QualityLevel));
    CHECK_FALSE(read.says(scene::GraphicsSetting::MaxFrameRate));
    // In range after all: clamped, as a script's write is.
    CHECK(read.at(scene::GraphicsSetting::ShadowDistance) == doctest::Approx(1000.0));
    CHECK(refused.size() == 5);

    // No file, and a file that is not one: nothing read, nothing changed.
    CHECK_FALSE(app::readPlayerGraphics(folder / "absent.json", read));
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "not json";
    }
    CHECK_FALSE(app::readPlayerGraphics(file, read));
    CHECK(read.said.none());

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}
