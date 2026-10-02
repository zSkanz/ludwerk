// The graphics settings as one model (ADR 0147): whose word a value is, what a
// level is, and what a script's write does to both.
#include <doctest/doctest.h>
#include <limits>

#include "engine/scene/graphics_model.h"

namespace {

namespace scene = engine::scene;
using scene::GraphicsModel;
using scene::GraphicsSetting;
using scene::GraphicsSource;

// A model with four levels that differ where the tests look: shadows, bloom
// and the foliage.
[[nodiscard]] GraphicsModel seeded()
{
    GraphicsModel model;
    const double resolution[4] = {512.0, 1024.0, 2048.0, 2048.0};
    const double cascades[4] = {2.0, 3.0, 4.0, 4.0};
    const double distance[4] = {70.0, 100.0, 120.0, 160.0};
    const double bloom[4] = {0.0, 1.0, 1.0, 1.0};
    const double foliage[4] = {0.5, 0.75, 1.0, 1.0};
    for (int level = 0; level < 4; ++level) {
        scene::GraphicsLayer& layer = model.presets[static_cast<size_t>(level)];
        layer.put(GraphicsSetting::ShadowQuality, static_cast<double>(level) + 1.0);
        layer.put(GraphicsSetting::ShadowResolution, resolution[level]);
        layer.put(GraphicsSetting::ShadowCascades, cascades[level]);
        layer.put(GraphicsSetting::ShadowDistance, distance[level]);
        layer.put(GraphicsSetting::Bloom, bloom[level]);
        layer.put(GraphicsSetting::FoliageDensity, foliage[level]);
    }
    return model;
}

} // namespace

TEST_CASE("a setting's value is the first layer that says it, and the source says which")
{
    GraphicsModel model = seeded();
    // Nobody: the level's, and for a setting no level sets, the engine's.
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(120.0));
    CHECK(model.source(GraphicsSetting::ShadowDistance) == GraphicsSource::Preset);
    CHECK(model.effective(GraphicsSetting::MaxFrameRate) == doctest::Approx(0.0));
    CHECK(model.source(GraphicsSetting::MaxFrameRate) == GraphicsSource::Engine);

    model.project.put(GraphicsSetting::ShadowDistance, 300.0);
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(300.0));
    CHECK(model.source(GraphicsSetting::ShadowDistance) == GraphicsSource::Project);

    model.player.put(GraphicsSetting::ShadowDistance, 200.0);
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(200.0));
    CHECK(model.source(GraphicsSetting::ShadowDistance) == GraphicsSource::Player);

    // A script's write is the newest word: over what the player saved.
    CHECK(model.write(GraphicsSetting::ShadowDistance, 150.0));
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(150.0));
    CHECK(model.source(GraphicsSetting::ShadowDistance) == GraphicsSource::Script);

    // And the command line is over everything: written, and not in force.
    model.commandLine.put(GraphicsSetting::ShadowDistance, 50.0);
    CHECK(model.write(GraphicsSetting::ShadowDistance, 400.0));
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(50.0));
    CHECK(model.source(GraphicsSetting::ShadowDistance) == GraphicsSource::CommandLine);
}

TEST_CASE("a write is checked against the setting's kind and clamped into its range")
{
    GraphicsModel model = seeded();
    CHECK(model.write(GraphicsSetting::ShadowDistance, 5000.0));
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(1000.0));
    CHECK(model.write(GraphicsSetting::RenderScale, 0.1));
    CHECK(model.effective(GraphicsSetting::RenderScale) == doctest::Approx(0.5));
    CHECK(model.write(GraphicsSetting::MaxFrameRate, 144.0));
    CHECK(model.effective(GraphicsSetting::MaxFrameRate) == doctest::Approx(144.0));

    // Not that kind at all.
    CHECK_FALSE(model.write(GraphicsSetting::MaxFrameRate, 59.5));
    CHECK_FALSE(model.write(GraphicsSetting::VSync, 2.0));
    CHECK_FALSE(model.write(GraphicsSetting::WindowMode, 7.0));
    CHECK_FALSE(model.write(GraphicsSetting::ShadowDistance, std::numeric_limits<double>::infinity()));
    CHECK(model.effective(GraphicsSetting::MaxFrameRate) == doctest::Approx(144.0));
}

TEST_CASE("the level is the one chosen, and Custom when any quality setting is not its")
{
    GraphicsModel model = seeded();
    CHECK(model.qualityLevel() == scene::kQualityHigh);
    CHECK(model.preset() == scene::kQualityHigh);

    CHECK(model.write(GraphicsSetting::Bloom, 0.0));
    CHECK(model.qualityLevel() == scene::kQualityCustom);
    // The preset under it is still the one that was chosen.
    CHECK(model.preset() == scene::kQualityHigh);
    // Back to the level's own value is the level again.
    CHECK(model.write(GraphicsSetting::Bloom, 1.0));
    CHECK(model.qualityLevel() == scene::kQualityHigh);

    // A display setting is not part of a level.
    CHECK(model.write(GraphicsSetting::VSync, 0.0));
    CHECK(model.write(GraphicsSetting::MaxFrameRate, 60.0));
    CHECK(model.qualityLevel() == scene::kQualityHigh);

    // Custom is what a level reads as, never one that is chosen.
    CHECK_FALSE(model.write(GraphicsSetting::QualityLevel, static_cast<double>(scene::kQualityCustom)));
}

TEST_CASE("writing a level makes every quality setting that level's, whoever said otherwise")
{
    GraphicsModel model = seeded();
    model.project.put(GraphicsSetting::ShadowDistance, 300.0);
    model.player.put(GraphicsSetting::Bloom, 0.0);
    CHECK(model.write(GraphicsSetting::FoliageDensity, 0.2));
    CHECK(model.write(GraphicsSetting::VSync, 0.0));
    CHECK(model.qualityLevel() == scene::kQualityCustom);

    CHECK(model.write(GraphicsSetting::QualityLevel, static_cast<double>(scene::kQualityLow)));
    CHECK(model.qualityLevel() == scene::kQualityLow);
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(70.0));
    CHECK(model.effective(GraphicsSetting::ShadowResolution) == doctest::Approx(512.0));
    CHECK(model.effective(GraphicsSetting::Bloom) == doctest::Approx(0.0));
    CHECK(model.effective(GraphicsSetting::FoliageDensity) == doctest::Approx(0.5));
    // The display's settings stay.
    CHECK(model.effective(GraphicsSetting::VSync) == doctest::Approx(0.0));

    model.applyPreset(scene::kQualityUltra);
    CHECK(model.qualityLevel() == scene::kQualityUltra);
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(160.0));
    CHECK(model.effective(GraphicsSetting::Bloom) == doctest::Approx(1.0));
}

TEST_CASE("Auto is the level this machine was given, and reads as Auto")
{
    GraphicsModel model = seeded();
    model.autoLevel = scene::kQualityMedium;
    model.applyPreset(scene::kQualityAuto);
    CHECK(model.preset() == scene::kQualityMedium);
    CHECK(model.qualityLevel() == scene::kQualityAuto);
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(100.0));
    CHECK(model.write(GraphicsSetting::Bloom, 0.0));
    CHECK(model.qualityLevel() == scene::kQualityCustom);
}

TEST_CASE("a machine's own default level is what nobody's word falls to")
{
    GraphicsModel model = seeded();
    model.defaultLevel = scene::kQualityMedium;
    CHECK(model.qualityLevel() == scene::kQualityMedium);
    CHECK(model.effective(GraphicsSetting::ShadowCascades) == doctest::Approx(3.0));
    CHECK(model.source(GraphicsSetting::QualityLevel) == GraphicsSource::Engine);
    model.project.put(GraphicsSetting::QualityLevel, static_cast<double>(scene::kQualityUltra));
    CHECK(model.qualityLevel() == scene::kQualityUltra);
    CHECK(model.source(GraphicsSetting::QualityLevel) == GraphicsSource::Project);
}

TEST_CASE("a shadow quality is its resolution and its cascades")
{
    GraphicsModel model = seeded();
    // `Low` is one past `Off`.
    CHECK(model.write(GraphicsSetting::ShadowQuality, 1.0));
    CHECK(model.effective(GraphicsSetting::ShadowResolution) == doctest::Approx(512.0));
    CHECK(model.effective(GraphicsSetting::ShadowCascades) == doctest::Approx(2.0));
    CHECK(model.write(GraphicsSetting::ShadowQuality, 0.0));
    CHECK(model.effective(GraphicsSetting::ShadowCascades) == doctest::Approx(0.0));
    CHECK(model.write(GraphicsSetting::ShadowQuality, 3.0));
    CHECK(model.effective(GraphicsSetting::ShadowCascades) == doctest::Approx(4.0));
    CHECK(model.effective(GraphicsSetting::ShadowResolution) == doctest::Approx(2048.0));
}

TEST_CASE("a group is set to a level at once, and reads the level its settings are at")
{
    GraphicsModel model = seeded();
    REQUIRE(model.groupLevel(scene::GraphicsGroup::Shadows).has_value());
    CHECK(*model.groupLevel(scene::GraphicsGroup::Shadows) == scene::kQualityHigh);

    model.setGroupLevel(scene::GraphicsGroup::Shadows, scene::kQualityLow);
    CHECK(*model.groupLevel(scene::GraphicsGroup::Shadows) == scene::kQualityLow);
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(70.0));
    CHECK(model.effective(GraphicsSetting::ShadowCascades) == doctest::Approx(2.0));
    // The other groups are where they were, and the level as a whole is not one.
    CHECK(*model.groupLevel(scene::GraphicsGroup::Foliage) == scene::kQualityHigh);
    CHECK(model.qualityLevel() == scene::kQualityCustom);

    // One of its settings by itself: no level's.
    CHECK(model.write(GraphicsSetting::ShadowDistance, 85.0));
    CHECK_FALSE(model.groupLevel(scene::GraphicsGroup::Shadows).has_value());

    // Cinematic is Ultra until something is finer, and reads as Ultra.
    model.setGroupLevel(scene::GraphicsGroup::Shadows, scene::kLevelCinematic);
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(160.0));
    CHECK(*model.groupLevel(scene::GraphicsGroup::Shadows) == scene::kQualityUltra);

    // Two levels that give a group the same values: the chosen one is it.
    CHECK(*model.groupLevel(scene::GraphicsGroup::Reflections) == scene::kQualityHigh);
}

TEST_CASE("reset forgets a script's word and the player's, and the project's stands")
{
    GraphicsModel model = seeded();
    model.project.put(GraphicsSetting::ShadowDistance, 300.0);
    model.player.put(GraphicsSetting::VSync, 0.0);
    CHECK(model.write(GraphicsSetting::Bloom, 0.0));
    model.resetToDefaults();
    CHECK(model.effective(GraphicsSetting::Bloom) == doctest::Approx(1.0));
    CHECK(model.effective(GraphicsSetting::VSync) == doctest::Approx(1.0));
    CHECK(model.effective(GraphicsSetting::ShadowDistance) == doctest::Approx(300.0));
    CHECK(model.forgetPlayer);
}

TEST_CASE("the player's choices are what was saved under what a script wrote since")
{
    GraphicsModel model = seeded();
    model.player.put(GraphicsSetting::VSync, 0.0);
    model.player.put(GraphicsSetting::MaxFrameRate, 60.0);
    CHECK(model.write(GraphicsSetting::MaxFrameRate, 144.0));
    CHECK(model.write(GraphicsSetting::Bloom, 0.0));
    const scene::GraphicsLayer choices = model.playerChoices();
    CHECK(choices.at(GraphicsSetting::VSync) == doctest::Approx(0.0));
    CHECK(choices.at(GraphicsSetting::MaxFrameRate) == doctest::Approx(144.0));
    CHECK(choices.says(GraphicsSetting::Bloom));
    // What nobody chose is not in the file: a better default later reaches it.
    CHECK_FALSE(choices.says(GraphicsSetting::ShadowDistance));
}

TEST_CASE("the revision moves when a value in force does, and not otherwise")
{
    GraphicsModel model = seeded();
    const engine::core::u64 before = model.revision();
    // Said, to the value it already had.
    CHECK(model.write(GraphicsSetting::Bloom, 1.0));
    CHECK(model.revision() == before);
    CHECK(model.write(GraphicsSetting::Bloom, 0.0));
    CHECK(model.revision() != before);

    // The host's layers put back leave a script's word where it was.
    GraphicsModel host = seeded();
    host.commandLine.put(GraphicsSetting::VSync, 0.0);
    model.takeHostLayers(host);
    CHECK(model.effective(GraphicsSetting::Bloom) == doctest::Approx(0.0));
    CHECK(model.effective(GraphicsSetting::VSync) == doctest::Approx(0.0));
}

TEST_CASE("every setting has a name a script can ask by")
{
    CHECK(scene::graphicsSettingNamed("VSync") == GraphicsSetting::VSync);
    CHECK(scene::graphicsSettingNamed("ShadowQuality") == GraphicsSetting::ShadowQuality);
    CHECK_FALSE(scene::graphicsSettingNamed("Vsync").has_value());
    CHECK(scene::graphicsSettingInfo(GraphicsSetting::Bloom).applied);
    CHECK_FALSE(scene::graphicsSettingInfo(GraphicsSetting::Brightness).applied);
    for (engine::core::usize index = 0; index < scene::kGraphicsSettingCount; ++index) {
        const scene::GraphicsSettingInfo& info = scene::graphicsSettingInfo(static_cast<GraphicsSetting>(index));
        CHECK(info.engineDefault >= info.lowest);
        CHECK(info.engineDefault <= info.highest);
    }
}
