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
        "[graphics]\nshadow_distance = 5\nrender_cap = 100\n\n[display]\nvsync = false\nmax_frame_rate = 90\n",
    };
    std::array<app::GraphicsOverrides, 3> overrides;
    overrides[1].quality = render::QualityLevel::Medium;
    overrides[1].shadowResolution = 1024;
    overrides[2].bloom = false;
    overrides[2].renderScale = 0.8f;
    overrides[2].antiAliasing = render::AntiAliasingMode::Off;
    overrides[1].upscaling = render::UpscalingMode::Fsr1;
    overrides[1].sharpness = 0.6f;
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
                CHECK(pacing.maxFrameRate == expected.maxFrameRate);
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
                             "anti_aliasing = \"off\"\n\n"
                             "[display]\nwindow_mode = \"fullscreen\"\nmonitor = 1\nresolution = [1920, 1080]\n"
                             "brightness = 0.25\nbackground_frame_rate = 0\nremember_player_settings = false\n");
    const app::ProjectConfig config = app::loadProjectConfig(project.path, {});
    const scene::GraphicsModel& model = config.graphicsModel;
    const auto value = [&model](scene::GraphicsSetting setting) { return model.effective(setting); };

    CHECK(value(scene::GraphicsSetting::ShadowQuality) == doctest::Approx(1.0));
    CHECK(value(scene::GraphicsSetting::TerrainDetail) == doctest::Approx(2.0));
    CHECK(value(scene::GraphicsSetting::FoliageDensity) == doctest::Approx(0.25));
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
