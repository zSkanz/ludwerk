// A sound behind a wall is muffled (ADR 0255), against the physics world a
// game has: walls made by a script, a character crossing, a model a sound is
// in, and what all that costs a frame.
#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/app/world_host.h"
#include "engine/audio/audio.h"
#include "engine/core/math.h"
#include "engine/platform/process.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

namespace {

[[nodiscard]] core::InstanceId named(app::WorldHost& host, const char* name)
{
    scene::World& world = host.world();
    std::vector<core::InstanceId> all;
    world.collectDescendants(host.workspace(), all);
    for (const core::InstanceId id : all) {
        if (world.name(id) == world.atoms().lookup(name))
            return id;
    }
    return {};
}

// An ear at `x` on the ground, facing -Z: each case is three hundred metres
// from the next, past every sound's reach, so one case's walls are on no
// other's rays.
[[nodiscard]] core::CFrameD earAt(double x)
{
    return core::lookAtCFrame(core::DVec3{x, 2.0, 0.0}, core::DVec3{x, 2.0, -10.0}, core::Vec3{0.0f, 1.0f, 0.0f});
}

} // namespace

TEST_CASE("ADR 0255: walls between muffle a sound, a character crossing does not, nor the model it is in")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        game:GetService("AudioService").OcclusionEnabled = true
        local function block(name: string, at: vector, size: vector, parent: Instance?)
            local part = Instance.new("Part")
            part.Name = name
            part.Anchored = true
            part.Size = size
            part.Position = at
            part.Parent = parent or workspace
            return part
        end
        local function sound(name: string, at: vector, parent: Instance?)
            local holder = block(name .. "Holder", at, vector.create(1, 1, 1), parent)
            local heard = Instance.new("Sound")
            heard.Name = name
            heard.Looped = true
            heard.RollOffMaxDistance = 100
            heard.Parent = holder
            heard:Play()
        end

        -- In the open.
        sound("Open", vector.create(0, 2, -10))
        -- Behind one wall, and behind two.
        block("Wall", vector.create(300, 2, -5), vector.create(8, 8, 0.5))
        sound("Behind", vector.create(300, 2, -10))
        block("WallA", vector.create(600, 2, -4), vector.create(8, 8, 0.5))
        block("WallB", vector.create(600, 2, -6), vector.create(8, 8, 0.5))
        sound("BehindTwo", vector.create(600, 2, -10))
        -- Partly hidden: a wall to the right of the line and one above it,
        -- each clear of the line itself.
        block("Right", vector.create(904.1, 2, -5), vector.create(8, 8, 0.5))
        block("Above", vector.create(900, 6.1, -5), vector.create(8, 8, 0.5))
        sound("Partly", vector.create(900, 2, -10))
        -- A character standing between.
        local body = Instance.new("CharacterBody")
        body.Size = vector.create(2, 4, 2)
        body.Position = vector.create(1200, 2, -5)
        body.Parent = workspace
        sound("PastCharacter", vector.create(1200, 2, -10))
        -- In a car: its body is between the ear and the engine.
        local car = Instance.new("Model")
        car.Name = "Car"
        car.Parent = workspace
        block("CarBody", vector.create(1500, 2, -7), vector.create(4, 3, 6), car)
        sound("Engine", vector.create(1500, 2, -9), car)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int at = 0; at < 5; ++at)
        host.tick();

    const auto muffled = [&](const char* name, double x) {
        const core::InstanceId id = named(host, name);
        REQUIRE(id.valid());
        const core::CFrameD ear = earAt(x);
        // Each case's ear looks at its own; one update looks at every sound
        // it hears, at most twelve, and there is one.
        host.audio().update(host.world(), core::InstanceId{}, &ear);
        return host.audio().occlusionOf(id);
    };
    CHECK(muffled("Open", 0.0) == 0.0f);
    CHECK(static_cast<double>(muffled("Behind", 300.0)) == doctest::Approx(1.0));
    CHECK(static_cast<double>(muffled("BehindTwo", 600.0)) == doctest::Approx(1.5));
    CHECK(static_cast<double>(muffled("Partly", 900.0)) == doctest::Approx(0.4));
    CHECK(muffled("PastCharacter", 1200.0) == 0.0f);
    CHECK(muffled("Engine", 1500.0) == 0.0f);
    // And a game that turns it off hears the wall as nothing.
    host.world().engineState().occlusionEnabled = false;
    CHECK(muffled("Behind", 300.0) == 0.0f);
}

// A measurement: the machine's number, said, and bounded only far above it.
// Run where `ENG_TIMED_TESTS` is set: the perf stage, alone.
TEST_CASE("ADR 0255: sixty-four sounds behind walls, what a frame costs" *
          doctest::skip(!engine::platform::environmentFlag("ENG_TIMED_TESTS")))
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        game:GetService("AudioService").OcclusionEnabled = true
        for row = 0, 7 do
            local wall = Instance.new("Part")
            wall.Anchored = true
            wall.Size = vector.create(40, 8, 0.5)
            wall.Position = vector.create(0, 2, -6 - row * 4)
            wall.Parent = workspace
            for column = 0, 7 do
                local holder = Instance.new("Part")
                holder.Anchored = true
                holder.Size = vector.create(1, 1, 1)
                holder.Position = vector.create(column * 4 - 14, 2, -8 - row * 4)
                holder.Parent = workspace
                local heard = Instance.new("Sound")
                heard.Looped = true
                heard.RollOffMaxDistance = 100
                heard.Parent = holder
                heard:Play()
            end
        end
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int at = 0; at < 5; ++at)
        host.tick();
    const core::CFrameD ear = earAt(0.0);
    for (const int rays : {64, 20}) {
        host.world().engineState().occlusionRays = rays;
        double total = 0.0;
        double worst = 0.0;
        constexpr int Frames = 600;
        for (int frame = 0; frame < Frames; ++frame) {
            host.audio().update(host.world(), core::InstanceId{}, &ear);
            const double ms = host.audio().stats().occlusionMs;
            total += ms;
            worst = std::max(worst, ms);
            CHECK(host.audio().stats().occlusionRays <= static_cast<core::u32>(rays));
        }
        MESSAGE("occlusion, 64 sounds, " << rays << " rays a frame: " << total / Frames << " ms a frame, worst "
                                         << worst << " ms, " << host.audio().stats().occludedSounds << " muffled");
        CHECK(total / Frames < 2.0);
    }
}
