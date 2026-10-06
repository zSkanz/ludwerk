// The measuring keys (ADR 0171): what `[debug] hide` takes out of a frame, what
// the report says is in force, and how the GPU's time by pass is said.

#include <array>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/app/debug_measure.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"
#include "inspector_fixture.h"

namespace app = engine::app;
namespace render = engine::render;
namespace rhi = engine::rhi;

TEST_SUITE_BEGIN("debug measure");

TEST_CASE("hide reads a list of names, and says the ones it does not know")
{
    std::vector<std::string> unknown;
    const app::DebugHide hide = app::parseDebugHide(" foliage, terrain ,grass,,ui", &unknown);
    CHECK(hide.foliage);
    CHECK(hide.terrain);
    CHECK(hide.ui);
    CHECK_FALSE(hide.parts);
    CHECK_FALSE(hide.worldUi);
    CHECK(unknown == std::vector<std::string>{"grass"});
    // Said back in the list's own order, whatever order it was asked in.
    CHECK(app::debugHideText(hide) == "terrain,foliage,ui");

    CHECK_FALSE(app::parseDebugHide("").any());
    CHECK(app::debugHideText(app::DebugHide{}).empty());

    // Every name the list offers is one it reads.
    for (const std::string_view name : app::debugHideNames()) {
        std::vector<std::string> none;
        CHECK(app::parseDebugHide(name, &none).any());
        CHECK(none.empty());
    }
}

TEST_CASE("what is hidden is taken out of the frame, and nothing else")
{
    render::RenderWorld world;
    const auto draw = [](bool terrain, bool voxel, engine::core::u32 bones, bool transparent) {
        render::DrawItem item;
        item.terrain = terrain;
        item.voxelBlock = voxel;
        item.boneCount = bones;
        item.transparent = transparent;
        return item;
    };
    world.draws = {draw(false, false, 0, false), draw(true, false, 0, false), draw(false, true, 0, false),
                   draw(false, false, 12, false), draw(false, false, 0, true)};
    world.decals.resize(2);
    world.particles.resize(3);
    world.lights.resize(4);
    world.worldUiVertices.resize(6);
    world.worldUiRuns.resize(1);

    render::RenderWorld untouched = world;
    app::applyDebugHide(untouched, app::DebugHide{});
    CHECK(untouched.draws.size() == 5);
    CHECK(untouched.decals.size() == 2);

    render::RenderWorld skinned = world;
    app::applyDebugHide(skinned, app::parseDebugHide("skinned"));
    REQUIRE(skinned.draws.size() == 4);
    for (const render::DrawItem& item : skinned.draws)
        CHECK(item.boneCount == 0);
    CHECK(skinned.particles.size() == 3);

    // A part is what is none of the other kinds: the ground, the blocks and
    // the characters stay.
    render::RenderWorld parts = world;
    app::applyDebugHide(parts, app::parseDebugHide("parts"));
    REQUIRE(parts.draws.size() == 3);
    CHECK(parts.draws[0].terrain);
    CHECK(parts.draws[1].voxelBlock);
    CHECK(parts.draws[2].boneCount == 12);

    render::RenderWorld blended = world;
    app::applyDebugHide(blended, app::parseDebugHide("transparent"));
    CHECK(blended.draws.size() == 4);

    render::RenderWorld many = world;
    app::applyDebugHide(many, app::parseDebugHide("terrain,voxels,decals,particles,lights,world_ui"));
    CHECK(many.draws.size() == 3);
    CHECK(many.decals.empty());
    CHECK(many.particles.empty());
    CHECK(many.lights.empty());
    CHECK(many.worldUiVertices.empty());
    CHECK(many.worldUiRuns.empty());
}

TEST_CASE("skip reads a list of lighting terms as the bits the shaders read")
{
    namespace Skip = render::MeasureSkip;
    std::vector<std::string> unknown;
    const engine::core::u32 skip = app::parseDebugSkip("shadow, environment ,gloss,unlit", &unknown);
    CHECK(skip == (Skip::Shadow | Skip::Environment | Skip::Unlit));
    CHECK(unknown == std::vector<std::string>{"gloss"});
    CHECK(app::debugSkipText(skip) == "shadow,environment,unlit");
    CHECK(app::parseDebugSkip("") == 0);
    CHECK(app::debugSkipText(0).empty());
    // A pass of the frame is a name of the same list.
    CHECK(app::parseDebugSkip("depth_prepass") == Skip::DepthPrepass);

    // Every name is one bit, and no two names share one.
    engine::core::u32 all = 0;
    for (const std::string_view name : app::debugSkipNames()) {
        const engine::core::u32 bit = app::parseDebugSkip(name);
        CHECK(bit != 0);
        CHECK((bit & (bit - 1)) == 0);
        CHECK((all & bit) == 0);
        all |= bit;
    }
    // What the shader is given is a float: every bit together is still exact.
    CHECK(static_cast<engine::core::u32>(static_cast<float>(all)) == all);

    CHECK(app::debugKeysInForce(false, app::DebugHide{}, 0, false, Skip::Sun | Skip::Fog) == "skip=sun,fog");
}

TEST_CASE("the report names every measuring key in force, and nothing when none is")
{
    CHECK(app::debugKeysInForce(false, app::DebugHide{}, 0, false).empty());
    CHECK(app::debugKeysInForce(true, app::parseDebugHide("foliage,terrain"), 4, true) ==
          "gpu_pass_times, hide=terrain,foliage, shadow_taps=4, log_ui_touches");
    CHECK(app::debugKeysInForce(false, app::DebugHide{}, 8, false) == "shadow_taps=8");
}

TEST_CASE("a finger that comes down is said with the element that took it, or as the game's")
{
    const auto catalog = engine::core::engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(catalog.ok, catalog.diagnostic);

    app::testing::Fixture fixture;
    engine::scene::World world{fixture.classes, fixture.enums, fixture.atoms, 7u};
    const engine::core::InstanceId hud = world.create(fixture.folderClass);
    world.setName(hud, fixture.atoms.intern("Hud"));
    const engine::core::InstanceId pad = world.create(fixture.folderClass);
    world.setName(pad, fixture.atoms.intern("Pad"));
    (void)world.setParent(pad, hud);
    const engine::core::InstanceId stick = world.create(fixture.folderClass);
    world.setName(stick, fixture.atoms.intern("Stick"));
    engine::scene::UIObjectComponent object;
    object.absolutePosition = engine::core::Vec2{40.0f, 600.4f};
    object.absoluteSize = engine::core::Vec2{220.0f, 219.6f};
    world.uiObjects().add(stick, object);
    (void)world.setParent(stick, pad);

    CHECK(app::wholeName(world, stick) == "Hud.Pad.Stick");
    CHECK(app::wholeName(world, hud) == "Hud");
    CHECK(app::wholeName(world, engine::core::InstanceId{}).empty());

    std::vector<std::string> said;
    const engine::core::LogSink previous = engine::core::setLogSink(
        [&said](engine::core::LogLevel, std::string_view message) { said.emplace_back(message); });
    app::logUiTouch(world, stick, 3, engine::core::Vec2{120.2f, 700.0f});
    app::logUiTouch(world, engine::core::InstanceId{}, 4, engine::core::Vec2{900.0f, 300.0f});
    engine::core::setLogSink(previous);

    REQUIRE(said.size() == 2);
    // Which finger, where, on what, and the rectangle that caught it.
    CHECK(said[0].find("Finger 3") != std::string::npos);
    CHECK(said[0].find("120, 700") != std::string::npos);
    CHECK(said[0].find("Hud.Pad.Stick") != std::string::npos);
    CHECK(said[0].find("40, 600") != std::string::npos);
    CHECK(said[0].find("220 by 220") != std::string::npos);
    CHECK(said[1].find("Finger 4") != std::string::npos);
    CHECK(said[1].find("900, 300") != std::string::npos);
    CHECK(said[1].find("the game's") != std::string::npos);
}

TEST_CASE("the GPU's time by pass is a frame's mean, the costliest first, the floor apart")
{
    app::PassTimeLedger ledger;
    CHECK(ledger.frames() == 0);
    CHECK(ledger.line().passes.empty());

    const std::array<rhi::PassTime, 4> first{
        rhi::PassTime{.name = "shadow", .milliseconds = 2.0, .submits = 1},
        rhi::PassTime{.name = "forward", .milliseconds = 6.0, .submits = 1},
        rhi::PassTime{.name = "floor", .milliseconds = 0.2, .submits = 1},
        rhi::PassTime{.name = "tonemap+ui", .milliseconds = 1.0, .submits = 1},
    };
    const std::array<rhi::PassTime, 4> second{
        rhi::PassTime{.name = "shadow", .milliseconds = 4.0, .submits = 1},
        rhi::PassTime{.name = "forward", .milliseconds = 8.0, .submits = 2},
        rhi::PassTime{.name = "floor", .milliseconds = 0.4, .submits = 1},
        rhi::PassTime{.name = "tonemap+ui", .milliseconds = 2.0, .submits = 1},
    };
    ledger.add(first);
    ledger.add(second);
    // A frame that timed nothing is not a frame of the mean.
    ledger.add({});
    CHECK(ledger.frames() == 2);

    const app::PassTimeLedger::Line line = ledger.line();
    CHECK(line.passes == "forward 7.00, shadow 3.00, tonemap+ui 1.50");
    CHECK(line.total == doctest::Approx(11.5));
    CHECK(line.submits == doctest::Approx(3.5));
    CHECK(line.floor == doctest::Approx(0.3));

    ledger.clear();
    CHECK(ledger.frames() == 0);
    CHECK(ledger.line().passes.empty());
}

TEST_SUITE_END();
