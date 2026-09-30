#include <algorithm>
#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <variant>

#include "../../scene/generated/class_descriptors.gen.h"
#include "engine/app/editor.h"
#include "engine/app/inspector.h"
#include "engine/app/script_complete.h"
#include "engine/app/script_package.h"
#include "engine/app/world_host.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/input/input.h"
#include "engine/platform/file.h"
#include "engine/render/draw_poses.h"
#include "engine/render/lighting.h"
#include "engine/render/render_world.h"
#include "engine/render/transform_history.h"
#include "engine/scene/components.h"
#include "engine/scene/physics_sync.h"
#include "engine/scene/players.h"
#include "engine/scene/scene_file.h"
#include "engine/script/signals.h"
#include "engine/script/tasks.h"
#include "engine_test_nearly.h"
#include "lua.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;
using engine::testing::nearly;

namespace {

// Where `src/client/` mounts (ADR 0105): `GlobalScriptService.Client`.
[[nodiscard]] core::InstanceId clientScripts(app::WorldHost& host)
{
    const scene::World& w = host.world();
    const core::InstanceId global = w.findFirstChildOfClass(
        host.runtime().dataModel(), w.classes().findId(w.atoms().lookup("GlobalScriptService")));
    return global.valid() ? w.findFirstChild(global, w.atoms().lookup("Client")) : core::InstanceId{};
}

} // namespace

TEST_CASE("an empty world still boots, with game and every service under it")
{
    Captured log;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot({}).has_value());

    CHECK(host.workspace().valid());
    CHECK(host.world().alive(host.runtime().dataModel()));

    // **Every service, not the five that had earned their way in one at a time**
    // (D099). The count is measured against the registry rather than written
    // down, because a number in a test is a number somebody has to remember to
    // change and this one moved five times without anybody deciding it should.
    core::usize services = 0;
    const scene::ClassRegistry& classes = host.world().classes();
    for (scene::ClassId id = 1; id < static_cast<scene::ClassId>(classes.classCount()); ++id) {
        const scene::ClassDescriptor* descriptor = classes.find(id);
        if (descriptor != nullptr && hasFlag(descriptor->flags, scene::ClassFlags::Service))
            ++services;
    }
    CHECK(services > 5);
    CHECK(host.world().childCount(host.runtime().dataModel()) == services);
}

// --- The M4.5 gate addition: `Lighting` resolution, at the HOST --------------
//
// M4 tested the environment at the extractor, handing it a `Lighting` id the
// test had made itself. Every assertion passed, and the step that was actually
// broken -- the host resolving the service -- was the one step nothing covered.
// These three test that step, and each of them fails against M4's code.

TEST_CASE("the host resolves Lighting on a world no script ever touched")
{
    Captured log;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot({}).has_value());

    // The whole defect in one line: the id was cached before anything created
    // the service, so it was invalid for the life of the world and `extract`
    // answered with `RenderEnvironment`'s defaults every frame.
    REQUIRE(host.lighting().valid());
    CHECK(host.world().alive(host.lighting()));
    CHECK(host.world().lighting().find(host.lighting()) != nullptr);
    // A boot service, so it is an ordinary child of `game` and `GetService`
    // returns the same instance rather than a second one.
    CHECK(host.world().parentOf(host.lighting()) == host.runtime().dataModel());
}

TEST_CASE("the environment the renderer sees is the one the world holds")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        local lighting = game:GetService("Lighting")
        lighting.ClockTime = 6.5
        lighting.GeographicLatitude = 35
        lighting.Ambient = Color3.new(0.25, 0.5, 0.75)
        lighting.Brightness = 3.25
        lighting.FogColor = Color3.new(0.1, 0.2, 0.3)
        lighting.FogStart = 40
        lighting.FogEnd = 220
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    const scene::LightingComponent* held = host.world().lighting().find(host.lighting());
    REQUIRE(held != nullptr);

    render::RenderWorld snapshot;
    render::extract(host.world(), host.workspace(), host.lighting(), render::MeshLibrary{}, 1.0f, 0.0f, nullptr, 0.0f,
                    nullptr, snapshot);

    // Field by field rather than "it is not the default": a defaults comparison
    // passes the moment someone changes a default, and the point of this
    // assertion is that the snapshot carries what the SCRIPT wrote.
    CHECK(nearly(snapshot.environment.ambient.r, held->ambient.r));
    CHECK(nearly(snapshot.environment.ambient.g, held->ambient.g));
    CHECK(nearly(snapshot.environment.ambient.b, held->ambient.b));
    CHECK(nearly(snapshot.environment.sunBrightness, held->brightness));
    CHECK(nearly(snapshot.environment.fogColor.r, held->fogColor.r));
    CHECK(nearly(snapshot.environment.fogStart, held->fogStart));
    CHECK(nearly(snapshot.environment.fogEnd, held->fogEnd));

    // The sun is derived rather than stored, so it is compared against the
    // function of the two properties that define it -- which is also what makes
    // it a pure function of the world and not of the frame (R10).
    const core::Vec3 expected = render::sunDirection(held->clockTime, held->geographicLatitude);
    CHECK(nearly(snapshot.environment.sunDirection.x, expected.x));
    CHECK(nearly(snapshot.environment.sunDirection.y, expected.y));
    CHECK(nearly(snapshot.environment.sunDirection.z, expected.z));
    // And 6.5 is a morning sun: low and to the east. Written out because every
    // check above would also pass if `sunDirection` returned straight up for
    // everything, which is precisely the image M4 shipped.
    CHECK(snapshot.environment.sunDirection.x > 0.5f);
    CHECK(snapshot.environment.sunDirection.y < 0.5f);
}

TEST_CASE("two clock times give the renderer two different suns")
{
    const auto sunAt = [](const char* clock) {
        Captured log;
        Project project;
        project.write("src/client/main.luau", std::string(R"(
            local lighting = game:GetService("Lighting")
            lighting.GeographicLatitude = 35
            lighting.ClockTime = )") + clock);

        app::WorldHost host;
        REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
        host.tick();

        render::RenderWorld snapshot;
        render::extract(host.world(), host.workspace(), host.lighting(), render::MeshLibrary{}, 1.0f, 0.0f, nullptr,
                        0.0f, nullptr, snapshot);
        return snapshot.environment.sunDirection;
    };

    // The differential the goldens could not make. Two worlds differing in one
    // property must reach the renderer differently; while `Lighting` was
    // unreachable these were byte-identical, and so was every image.
    const core::Vec3 morning = sunAt("7.5");
    const core::Vec3 afternoon = sunAt("15.5");
    CHECK(core::length(morning - afternoon) > 0.5f);
}

// --- The M5 gate additions: the mirror at the HOST, and a close that waits ---

TEST_CASE("the host hands the physics mirror a Workspace on a world no script touched")
{
    Captured log;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot({}).has_value());

    // M4.5's whole defect in the physics module's shape. `PhysicsSync` cannot
    // resolve `Workspace` itself -- `scene` has no notion of the DataModel root
    // -- so the host hands it over, and an id that never arrived would make
    // every part in every world weightless while nothing anywhere errored.
    //
    // A build with no physics backend has no mirror at all, which is a
    // different and honest state; this asserts the wiring where there is one.
    REQUIRE(host.physics() != nullptr);
    CHECK(host.physics()->workspace() == host.workspace());
    CHECK(host.physics()->workspace().valid());
}

TEST_CASE("a part in a world nobody scripted still falls, because the mirror is wired")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local crate = Instance.new("Part")
        crate.Name = "Crate"
        crate.Position = vector.create(0, 40, 0)
        crate.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int tick = 0; tick < 60; ++tick)
        host.tick();

    const core::InstanceId crate = host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Crate"));
    REQUIRE(crate.valid());
    const scene::PartComponent* part = host.world().parts().find(crate);
    REQUIRE(part != nullptr);
    // A second of falling is about five metres. The assertion is that it moved
    // at all: a mirror that was never given a Workspace produces a world where
    // nothing does, and every test that only checks the API would still pass.
    CHECK(part->cframe.position.y < 39.0);
}

TEST_CASE("a script saves the simulation, rolls back to it and steps again to exactly where it was (ADR 0101)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        local floor = Instance.new("Part")
        floor.Anchored = true
        floor.Size = vector.create(40, 1, 40)
        floor.Position = vector.create(0, -0.5, 0)
        floor.Parent = workspace
        local boxes = {}
        for i = 1, 6 do
            local box = Instance.new("Part")
            box.Size = vector.create(1, 1, 1)
            box.Position = vector.create(i * 0.3, 1 + i * 1.6, 0)
            box.Parent = workspace
            boxes[i] = box
        end
        -- And a character walking into them: its controller's state is the
        -- solver's too, kept outside the system, and must come back with it.
        local hero = Instance.new("CharacterBody")
        hero.Position = vector.create(-6, 3, 0)
        hero.Parent = workspace
        table.insert(boxes, hero)
        -- Its input, every tick -- and again for every tick stepped again,
        -- which is what re-simulating with the inputs means.
        RunService.PreSimulation:Connect(function()
            hero:Move(vector.create(1, 0, 0))
        end)
        local function mark(name: string)
            local folder = Instance.new("Folder")
            folder.Name = name
            folder.Parent = workspace
        end

        local ticks = 0
        local saved: buffer? = nil
        RunService.PostSimulation:Connect(function()
            ticks += 1
            if ticks == 20 then
                saved = RunService:SaveSimulation()
            elseif ticks == 60 and saved then
                local first = {}
                for i, box in boxes do
                    first[i] = box.CFrame
                end
                if hero.Position.X > -5 then
                    mark("Walked")
                end
                if not RunService:RestoreSimulation(saved) then
                    mark("NotRestored")
                end
                for _ = 1, 40 do
                    hero:Move(vector.create(1, 0, 0))
                    RunService:StepSimulation()
                end
                local same = true
                for i, box in boxes do
                    if box.CFrame ~= first[i] then
                        same = false
                    end
                end
                mark(if same then "Same" else "Different")
                -- A body the state does not know of makes it another world.
                local extra = Instance.new("Part")
                extra.Parent = workspace
                RunService:StepSimulation()
                if not RunService:RestoreSimulation(saved) then
                    mark("Refused")
                end
            end
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int tick = 0; tick < 62; ++tick)
        host.tick();

    const auto marked = [&host](std::string_view name) {
        return host.world().findFirstChild(host.workspace(), host.world().atoms().lookup(name)).valid();
    };
    CHECK_FALSE(marked("NotRestored"));
    CHECK(marked("Walked"));
    CHECK(marked("Same"));
    CHECK_FALSE(marked("Different"));
    CHECK(marked("Refused"));
}

TEST_CASE("a twist a script gave before the save is stepped again after the restore (ADR 0118)")
{
    // `ApplyAngularImpulse` waits for the next step like `ApplyImpulse`: a save
    // between the two must carry it, or the world rolled back to is one where
    // the twist was never given.
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        workspace.Gravity = vector.zero
        local spinner = Instance.new("Part")
        spinner.Size = vector.create(4, 1, 1)
        spinner.Position = vector.create(0, 5, 0)
        spinner.Parent = workspace
        local function mark(name: string)
            local folder = Instance.new("Folder")
            folder.Name = name
            folder.Parent = workspace
        end

        local ticks = 0
        local saved: buffer? = nil
        RunService.PostSimulation:Connect(function()
            ticks += 1
            if ticks == 5 then
                spinner:ApplyAngularImpulse(vector.create(0, 3, 0))
                saved = RunService:SaveSimulation()
            elseif ticks == 25 and saved then
                local first = spinner.CFrame
                if first.LookVector.z > -0.999 then
                    mark("Turned")
                end
                if not RunService:RestoreSimulation(saved) then
                    mark("NotRestored")
                end
                for _ = 1, 20 do
                    RunService:StepSimulation()
                end
                mark(if spinner.CFrame == first then "Same" else "Different")
            end
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int tick = 0; tick < 27; ++tick)
        host.tick();

    const auto marked = [&host](std::string_view name) {
        return host.world().findFirstChild(host.workspace(), host.world().atoms().lookup(name)).valid();
    };
    CHECK_FALSE(marked("NotRestored"));
    CHECK(marked("Turned"));
    CHECK(marked("Same"));
    CHECK_FALSE(marked("Different"));
}

TEST_CASE("a character stepped again from where it was, through the commands it was given, goes where it went")
{
    // **The replay a replica corrects its prediction with** (ADR 0076, as
    // amended), against the real controller: a walk into a wall and a jump
    // off the ground, recorded one command a tick, then played back from the
    // first tick's position, vertical velocity and ground. Where it ends is
    // where the simulation took it -- or a correction would put the player
    // somewhere the authority never will.
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local function block(name: string, position: vector, size: vector)
            local part = Instance.new("Part")
            part.Name = name
            part.Size = size
            part.Position = position
            part.Anchored = true
            part.Parent = workspace
        end
        block("Ground", vector.create(0, -1, 0), vector.create(200, 2, 200))
        block("Wall", vector.create(7, 5, 0), vector.create(2, 10, 20))
        local walker = Instance.new("CharacterBody")
        walker.Name = "Walker"
        walker.Position = vector.create(0, 3, 0)
        walker.Parent = workspace
        local ticks = 0
        game:GetService("RunService").Heartbeat:Connect(function()
            ticks += 1
            if ticks > 90 then
                walker:Move(vector.create(1, 0, 0.25))
            end
            if ticks == 105 then
                walker:Jump()
            end
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    // A second and a half to land and settle.
    for (int tick = 0; tick < 91; ++tick)
        host.tick();
    const core::InstanceId walker =
        host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Walker"));
    REQUIRE(walker.valid());
    scene::PhysicsSync* physics = host.physics();
    REQUIRE(physics != nullptr);

    scene::CharacterReplayStart start;
    start.transform = host.world().parts().find(walker)->cframe;
    start.verticalVelocity = host.world().characterBodies().find(walker)->verticalVelocity;
    start.grounded = host.world().characterBodies().find(walker)->grounded;
    CHECK(start.grounded);

    std::vector<scene::CharacterCommand> commands;
    std::vector<core::DVec3> walked;
    for (int tick = 0; tick < 40; ++tick) {
        host.tick();
        const std::optional<scene::CharacterCommand> command = physics->lastCommand(walker);
        REQUIRE(command.has_value());
        commands.push_back(*command);
        walked.push_back(host.world().parts().find(walker)->cframe.position);
    }
    // It walked, met the wall, and jumped.
    CHECK(walked.back().x > 3.0);
    CHECK(walked.back().x < 6.0);

    const std::vector<core::CFrameD> replayed = physics->replay(walker, start, commands);
    REQUIRE(replayed.size() == commands.size());
    for (std::size_t at = 0; at < walked.size(); ++at) {
        CAPTURE(at);
        CHECK(replayed[at].position.x == doctest::Approx(walked[at].x).epsilon(1e-4));
        CHECK(replayed[at].position.y == doctest::Approx(walked[at].y).epsilon(1e-4));
        CHECK(replayed[at].position.z == doctest::Approx(walked[at].z).epsilon(1e-4));
    }
    // And it left the character there.
    CHECK(host.world().parts().find(walker)->cframe.position.x == doctest::Approx(walked.back().x).epsilon(1e-4));
}

TEST_CASE("a replay from a remembered tick is the live step again, to the bit (ADR 0133)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local function block(name: string, position: vector, size: vector)
            local part = Instance.new("Part")
            part.Name = name
            part.Size = size
            part.Position = position
            part.Anchored = true
            part.Parent = workspace
        end
        block("Ground", vector.create(0, -1, 0), vector.create(200, 2, 200))
        block("Ledge", vector.create(7, 0.75, 0), vector.create(4, 1.5, 20))
        local walker = Instance.new("CharacterBody")
        walker.Name = "Walker"
        walker.Position = vector.create(0, 3, 0)
        walker.JumpSpeed = 6
        walker.Parent = workspace
        local ticks = 0
        game:GetService("RunService").Heartbeat:Connect(function()
            ticks += 1
            if ticks > 90 then
                walker:Move(vector.create(1, 0, 0.1))
            end
            if ticks == 100 then
                walker:Jump()
            end
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int tick = 0; tick < 91; ++tick)
        host.tick();
    const core::InstanceId walker =
        host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Walker"));
    REQUIRE(walker.valid());
    scene::PhysicsSync* physics = host.physics();
    REQUIRE(physics != nullptr);
    physics->remember(1000);
    scene::CharacterReplayStart start;
    start.tick = 1000;
    start.transform = host.world().parts().find(walker)->cframe;
    start.verticalVelocity = host.world().characterBodies().find(walker)->verticalVelocity;
    start.grounded = host.world().characterBodies().find(walker)->grounded;

    // Walked onto the ledge's edge and over it, a command a tick, the island
    // remembered after every step as a replica remembers it.
    std::vector<scene::CharacterCommand> commands;
    std::vector<core::DVec3> walked;
    for (int tick = 0; tick < 40; ++tick) {
        host.tick();
        const std::optional<scene::CharacterCommand> command = physics->lastCommand(walker);
        REQUIRE(command.has_value());
        commands.push_back(*command);
        walked.push_back(host.world().parts().find(walker)->cframe.position);
        physics->remember(1001 + static_cast<core::u64>(tick));
    }

    const std::vector<core::CFrameD> replayed = physics->replay(walker, start, commands);
    REQUIRE(replayed.size() == commands.size());
    for (std::size_t at = 0; at < walked.size(); ++at) {
        CAPTURE(at);
        CHECK(replayed[at].position.x == walked[at].x);
        CHECK(replayed[at].position.y == walked[at].y);
        CHECK(replayed[at].position.z == walked[at].z);
    }
}

TEST_CASE("a replay of a character pushing crates is the live step again, to the bit (ADR 0133)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local function block(name: string, position: vector, size: vector, anchored: boolean)
            local part = Instance.new("Part")
            part.Name = name
            part.Size = size
            part.Position = position
            part.Anchored = anchored
            part.Parent = workspace
        end
        block("Ground", vector.create(0, -1, 0), vector.create(200, 2, 200), true)
        block("Crate1", vector.create(3, 0.75, 0), vector.create(1.5, 1.5, 1.5), false)
        block("Crate2", vector.create(4.7, 0.75, 0.4), vector.create(1.5, 1.5, 1.5), false)
        block("Crate3", vector.create(6.4, 0.75, -0.5), vector.create(1.5, 1.5, 1.5), false)
        local walker = Instance.new("CharacterBody")
        walker.Name = "Walker"
        walker.Size = vector.create(2, 4, 2)
        walker.Position = vector.create(0, 3, 0)
        walker.WalkSpeed = 8
        walker.Parent = workspace
        local ticks = 0
        game:GetService("RunService").Heartbeat:Connect(function()
            ticks += 1
            if ticks > 90 then
                walker:Move(vector.create(1, 0, 0.05))
            end
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    scene::World& world = host.world();
    const auto named = [&](const char* name) {
        return world.findFirstChild(host.workspace(), world.atoms().lookup(name));
    };
    const core::InstanceId walker = named("Walker");
    const std::array<core::InstanceId, 3> crates{named("Crate1"), named("Crate2"), named("Crate3")};
    REQUIRE(walker.valid());
    scene::PhysicsSync* physics = host.physics();
    REQUIRE(physics != nullptr);

    // Replayed from every tick of the push, a step at a time, as a replica
    // replays whatever the authority answers.
    for (int tick = 0; tick < 91; ++tick)
        host.tick();
    physics->remember(1000);
    int compared = 0;
    for (int step = 0; step < 50; ++step) {
        const core::u64 from = 1000 + static_cast<core::u64>(step);
        scene::CharacterReplayStart start;
        start.tick = from;
        start.transform = world.parts().find(walker)->cframe;
        start.verticalVelocity = world.characterBodies().find(walker)->verticalVelocity;
        start.grounded = world.characterBodies().find(walker)->grounded;
        for (const core::InstanceId crate : crates) {
            const scene::RigidBodyComponent* motion = world.rigidBodies().find(crate);
            start.bodies.push_back(scene::CharacterReplayStart::Body{crate, world.parts().find(crate)->cframe,
                                                                     motion->linearVelocity, motion->angularVelocity});
        }

        host.tick();
        const std::optional<scene::CharacterCommand> command = physics->lastCommand(walker);
        REQUIRE(command.has_value());
        physics->remember(from + 1);
        const core::DVec3 live = world.parts().find(walker)->cframe.position;
        std::array<core::DVec3, 3> liveCrates{};
        for (std::size_t at = 0; at < crates.size(); ++at)
            liveCrates[at] = world.parts().find(crates[at])->cframe.position;

        const std::array<scene::CharacterCommand, 1> one{*command};
        const std::vector<core::CFrameD> replayed = physics->replay(walker, start, one);
        REQUIRE(replayed.size() == 1);
        CAPTURE(step);
        CHECK(replayed[0].position.x == live.x);
        CHECK(replayed[0].position.y == live.y);
        CHECK(replayed[0].position.z == live.z);
        for (std::size_t at = 0; at < crates.size(); ++at) {
            CAPTURE(at);
            const core::DVec3 again = world.parts().find(crates[at])->cframe.position;
            CHECK(again.x == liveCrates[at].x);
            CHECK(again.y == liveCrates[at].y);
            CHECK(again.z == liveCrates[at].z);
        }
        ++compared;
    }
    // The crates were pushed: the test was not comparing crates at rest.
    CHECK(world.parts().find(crates[0])->cframe.position.x > 4.0);
    CHECK(compared == 50);
}

TEST_CASE("a BindToClose handler that yields is waited for")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        game:BindToClose(function()
            -- A tenth of a second of sim time. Before M5 this handler was cut
            -- off at the first drain after the shutdown and the attribute below
            -- was never written (D016).
            task.wait(0.1)
            game:SetAttribute("ClosedCleanly", true)
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    host.close();

    const scene::Value closed =
        host.world().getAttribute(host.runtime().dataModel(), host.world().atoms().lookup("ClosedCleanly"));
    const auto* flag = std::get_if<bool>(&closed);
    REQUIRE(flag != nullptr);
    CHECK(*flag);
}

TEST_CASE("a BindToClose handler that yields is waited for with the game paused (audit A1)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        game:BindToClose(function()
            task.wait(0.1)
            game:SetAttribute("ClosedCleanly", true)
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();
    // Paused -- the editor's pause, a menu's -- a tick did nothing, and the
    // close loop spun on it for the whole grace period, resuming nothing.
    host.world().engineState().paused = true;

    host.close(2.0);

    const scene::Value closed =
        host.world().getAttribute(host.runtime().dataModel(), host.world().atoms().lookup("ClosedCleanly"));
    const auto* flag = std::get_if<bool>(&closed);
    REQUIRE(flag != nullptr);
    CHECK(*flag);
    CHECK_FALSE(log.contains("still running after"));
}

TEST_CASE("a BindToClose handler that never finishes is cut off at the grace period")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        game:BindToClose(function()
            while true do
                task.wait()
            end
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    // A tenth of a second of WALL clock rather than the thirty-second default:
    // the cap exists so a handler that never finishes cannot hold the process
    // open, and a test for it must not hold the suite open either.
    host.close(0.1);

    // The point is that it returned. What it also does is say so, because a
    // shutdown that dropped somebody's save silently would be worse than one
    // that took thirty seconds.
    //
    // Matched on the catalog's TEXT rather than on the key: `core::log` formats
    // through the catalog, so a line only carries its key when the catalog does
    // not have one. The `[script.err.…]` checks elsewhere in this file match a
    // key because a script error's message is key-prefixed by `EngineError`,
    // which is a different path.
    CHECK(log.contains("still running after"));
}

TEST_CASE("a single file mounts as one entry Script and runs at the first tick")
{
    Captured log;
    Project project;
    project.write("main.luau", R"(
        local marker = Instance.new("Folder")
        marker.Name = "Ran"
        marker.Parent = workspace
        assert(script ~= nil, "the script global is bound")
        assert(script.ClassName == "Script")
        assert(script.Name == "main")
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root / "main.luau")).has_value());

    // Deferred, then drained: an entry script's first resumption is a scheduled
    // event like any other, and the boot drain is where it happens --
    // api-design.md §3 puts "start each Script … via task.defer" before "first
    // frame", so the world is built before anything is rendered.
    CHECK(host.world().childCount(host.workspace()) == 1);
    // And no clock advanced doing it.
    CHECK(host.world().engineState().tick == 0);

    host.tick();
    CHECK(host.world().childCount(host.workspace()) == 1);
    CHECK_FALSE(log.contains("[script.err."));
}

TEST_CASE("a directory mounts src/client as a tree, with subdirectories as Folders")
{
    Captured log;
    Project project;
    project.write("src/client/boot.luau", "");
    project.write("src/client/enemy/patrol.luau", "");
    project.write("src/client/enemy/chase.luau", "");
    // Outside src/client, so it is a module rather than an entry script and
    // never appears in the tree.
    project.write("src/shared/util.luau", "return {}");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());

    scene::World& world = host.world();
    const core::InstanceId scriptService = clientScripts(host);
    REQUIRE(scriptService.valid());

    CHECK(world.findFirstChild(scriptService, world.atoms().lookup("boot")).valid());
    const core::InstanceId enemy = world.findFirstChild(scriptService, world.atoms().lookup("enemy"));
    REQUIRE(enemy.valid());
    CHECK(world.childCount(enemy) == 2);
    CHECK(world.findFirstChild(enemy, world.atoms().lookup("patrol")).valid());
    CHECK_FALSE(world.findFirstChild(scriptService, world.atoms().lookup("util")).valid());
}

TEST_CASE("a file written into src/client while editing mounts where opening the project would put it")
{
    // **The second half of making a script in a script service**: the editor
    // writes the file and the host mounts it now, into the folder that
    // already exists -- not a second `enemy` beside the first.
    Captured log;
    Project project;
    project.write("src/client/enemy/patrol.luau", "");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    CHECK(host.projectRoot() == project.root);

    project.write("src/client/enemy/spawner.luau", "print('spawned')");
    const core::InstanceId made = host.mountScriptFile("enemy/spawner.luau");
    REQUIRE(made.valid());

    scene::World& world = host.world();
    const core::InstanceId scriptService = clientScripts(host);
    const core::InstanceId enemy = world.findFirstChild(scriptService, world.atoms().lookup("enemy"));
    REQUIRE(enemy.valid());
    CHECK(world.parentOf(made) == enemy);
    CHECK(world.childCount(scriptService) == 1);
    CHECK(world.atoms().text(world.name(made)) == "spawner");
    const std::optional<scene::Value> source = world.getProperty(made, world.atoms().lookup("Source"));
    REQUIRE(source.has_value());
    CHECK(std::get<std::string>(*source) == "print('spawned')");

    // A file that is not there mounts nothing.
    CHECK_FALSE(host.mountScriptFile("missing.luau").valid());
}

TEST_CASE("entry scripts start in path-sorted order, whatever order the walk found them")
{
    Captured log;
    Project project;
    project.write("src/client/c.luau", R"(
        local m = Instance.new("Folder")
        m.Name = "3"
        m.Parent = workspace
    )");
    project.write("src/client/a.luau", R"(
        local m = Instance.new("Folder")
        m.Name = "1"
        m.Parent = workspace
    )");
    project.write("src/client/b.luau", R"(
        local m = Instance.new("Folder")
        m.Name = "2"
        m.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());

    // Child order is parenting order, so the names read back in start order --
    // which R10 requires to be a property of the paths rather than of the
    // directory iterator.
    std::vector<core::InstanceId> children;
    host.world().collectChildren(host.workspace(), children);
    REQUIRE(children.size() == 3);
    CHECK(host.world().atoms().text(host.world().name(children[0])) == "1");
    CHECK(host.world().atoms().text(host.world().name(children[1])) == "2");
    CHECK(host.world().atoms().text(host.world().name(children[2])) == "3");
}

TEST_CASE("require resolves a module once and caches it")
{
    Captured log;
    Project project;
    project.write("src/shared/counter.luau", R"(
        local Counter = { value = 0 }
        Counter.value += 1
        return Counter
    )");
    project.write("src/client/main.luau", R"(
        local first = require("src/shared/counter")
        local second = require("src/shared/counter")
        assert(first == second, "one evaluation per module per VM")
        assert(first.value == 1, "evaluated once, not twice")

        local marker = Instance.new("Folder")
        marker.Name = "Required"
        marker.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    CHECK(host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Required")).valid());
    CHECK_FALSE(log.contains("[script.err."));
}

TEST_CASE("a relative require is relative to the requiring file")
{
    Captured log;
    Project project;
    project.write("src/shared/math/vec.luau", "return { name = 'vec' }");
    project.write("src/shared/math/init.luau", R"(
        local vec = require("./vec")
        return { vec = vec }
    )");
    project.write("src/client/main.luau", R"(
        local math2 = require("src/shared/math")
        assert(math2.vec.name == "vec")

        local marker = Instance.new("Folder")
        marker.Name = "Relative"
        marker.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    CHECK(host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Relative")).valid());
    CHECK_FALSE(log.contains("[script.err."));
}

TEST_CASE("a .luaurc alias resolves, and an unknown key does not break require")
{
    Captured log;
    Project project;
    // A `$schema` line is exactly what the vendored config reader treats as a
    // hard error that aborts the whole require (U-42). Ours ignores it.
    project.write(".luaurc", R"({ "$schema": "https://example/luaurc", "aliases": { "shared": "src/shared" } })");
    project.write("src/shared/util.luau", "return { ok = true }");
    project.write("src/client/main.luau", R"(
        local util = require("@shared/util")
        assert(util.ok == true)

        local marker = Instance.new("Folder")
        marker.Name = "Aliased"
        marker.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    CHECK(host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Aliased")).valid());
    CHECK_FALSE(log.contains("[script.err."));
}

TEST_CASE("a cyclic require raises rather than exhausting the C stack")
{
    Captured log;
    Project project;
    project.write("src/shared/a.luau", "local b = require('./b') return { b = b }");
    project.write("src/shared/b.luau", "local a = require('./a') return { a = a }");
    project.write("src/client/main.luau", "require('src/shared/a')");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    // The vendored implementation has no guard at all and recurses until the C
    // stack dies (U-36). This is the guard.
    CHECK(log.contains("[script.err.require_cycle]"));
}

TEST_CASE("a module that fails keeps failing, with the same error")
{
    Captured log;
    Project project;
    project.write("src/shared/broken.luau", "error('deliberate')");
    project.write("src/client/main.luau", R"(
        local first = select(2, pcall(require, "src/shared/broken"))
        local second = select(2, pcall(require, "src/shared/broken"))
        assert(tostring(first):find("deliberate", 1, true) ~= nil, tostring(first))
        assert(tostring(second):find("deliberate", 1, true) ~= nil, tostring(second))

        local marker = Instance.new("Folder")
        marker.Name = "Cached"
        marker.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    // The failure is cached and re-raised rather than the module being re-run
    // in the hope of a different answer (api-design.md §3) -- which is a thing
    // the vendored implementation does NOT do (U-35).
    CHECK(host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Cached")).valid());
}

TEST_CASE("a module that does not exist says so")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", "require('src/shared/nothing')");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    CHECK(log.contains("[script.err.module_not_found]"));
}

TEST_CASE("@engine/testing resolves, because it ships as content")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        local testing = require("@engine/testing")
        assert(type(testing.describe) == "function")
        assert(type(testing.run) == "function")

        local marker = Instance.new("Folder")
        marker.Name = "Testing"
        marker.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    CHECK(host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Testing")).valid());
    CHECK_FALSE(log.contains("[script.err."));
}

TEST_CASE("a Script whose Enabled is false at boot never starts")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        local marker = Instance.new("Folder")
        marker.Name = "ShouldNotRun"
        marker.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());

    scene::World& world = host.world();
    const core::InstanceId scriptService = clientScripts(host);
    const core::InstanceId entry = world.findFirstChild(scriptService, world.atoms().lookup("main"));
    REQUIRE(entry.valid());

    // `boot` already deferred it, so this test is really about the property
    // being read at start time. Setting it after boot is documented as having
    // no effect, which is why the check below is the interesting one.
    world.setProperty(entry, world.atoms().intern("Enabled"), scene::Value{false});

    host.tick();
    // It ran: the flag is read when the script is started, and `boot` started
    // it. That is exactly what api-design.md §3 says -- writing `Enabled` after
    // boot neither stops a running script nor starts one that did not run.
    CHECK(world.childCount(host.workspace()) == 1);
}

TEST_CASE("game.Loaded reaches a handler connected at file scope")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        game.Loaded:Connect(function()
            local marker = Instance.new("Folder")
            marker.Name = "Loaded"
            marker.Parent = workspace
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();

    // The fire has to be RAISED after every entry script's first resumption
    // rather than merely queued behind them: a fire captures its connection
    // list when it is raised (§3.1), so a handler connected at file scope would
    // otherwise miss its own fire.
    CHECK(host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Loaded")).valid());
}

TEST_CASE("the world ticks, and a Heartbeat handler sees the clock advance")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        local run = game:GetService("RunService")
        local part = Instance.new("Part")
        part.Name = "Mover"
        part.Parent = workspace

        run.Heartbeat:Connect(function(dt)
            assert(math.abs(dt - 1 / 60) < 1e-12, tostring(dt))
            part.Position = Vector3.new(run.SimTime, 0, 0)
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());

    for (int index = 0; index < 10; ++index)
        host.tick();

    render::RenderWorld snapshot;
    render::extract(host.world(), host.workspace(), host.lighting(), render::MeshLibrary{}, 1.0f, 0.0f, nullptr, 0.0f,
                    nullptr, snapshot);
    REQUIRE(snapshot.parts.size() == 1);
    // Ten: the script connected during the boot drain, so it saw every one of
    // the ten ticks.
    // The tolerance is f32's, because `Position` is a vector and the round
    // trip through one is where the precision goes -- `CFrame` keeps the f64.
    CHECK(snapshot.parts[0].cframe.position.x == doctest::Approx(10.0 / 60.0).epsilon(1e-6));
    CHECK_FALSE(log.contains("[script.err."));
}

TEST_CASE("two runs of the same script and seed produce the same world hash")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        local run = game:GetService("RunService")
        local rng = Random.new(7)
        for index = 1, 20 do
            local part = Instance.new("Part")
            part.Name = "P" .. index
            part.Position = Vector3.new(rng:NextNumber(), rng:NextNumber(), rng:NextNumber())
            part.Parent = workspace
        end

        run.Heartbeat:Connect(function()
            for _, part in workspace:GetChildren() do
                part.Position = part.Position + Vector3.new(0, 1 / 60, 0)
            end
        end)
    )");

    const auto runOnce = [&](core::u64 seed) {
        app::WorldHost host;
        REQUIRE_FALSE(host.boot(bootOptions(project.root, seed)).has_value());
        for (int index = 0; index < 120; ++index)
            host.tick();
        return host.world().worldHash();
    };

    // The level-B guarantee recorded replays rest on: same build, same
    // platform, same seed, same script -- same hash (ADR 0025).
    CHECK(runOnce(1234u) == runOnce(1234u));
}

TEST_CASE("nothing in this engine is marked inert any more")
{
    // **This case used to assert the opposite**, and its reversal is the point.
    // `PointLight.Shadows` and `SpotLight.Shadows` were the last two properties
    // carrying the marker: stored, returned faithfully, and acted on by nothing.
    // They are the only two the marker ever had at once, and they cast now
    // (`render::shadow.h`), so the marker has no members left.
    //
    // The case is kept rather than deleted because the assertion it makes is
    // still worth making, in the other direction: a property that goes back to
    // being inert should have to say so here, and the marker's whole purpose is
    // that a surface which does nothing admits it rather than looking finished.
    Captured log;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot({}).has_value());
    const scene::ClassRegistry& classes = host.world().classes();
    const core::AtomTable& atoms = host.world().atoms();

    for (const char* className : {"PointLight", "SpotLight"}) {
        const scene::ClassId id = classes.findId(atoms.lookup(className));
        REQUIRE(id != scene::InvalidClass);
        const scene::PropertyDesc* shadows = classes.findProperty(id, atoms.lookup("Shadows"));
        REQUIRE(shadows != nullptr);
        CHECK_FALSE(shadows->inert);
        // Still backed, which it always was. What changed is that something
        // reads it.
        CHECK(shadows->get != nullptr);
        CHECK(shadows->set != nullptr);

        const scene::PropertyDesc* brightness = classes.findProperty(id, atoms.lookup("Brightness"));
        REQUIRE(brightness != nullptr);
        CHECK_FALSE(brightness->inert);
    }
}

TEST_CASE("the render module's positional classes are PVInstances too")
{
    // At the host rather than in `engine/script`, because `MeshPart` and
    // `Camera` are registered by `render` and that module's classes do not
    // exist in a fixture holding scene's alone. Which is also the shape of the
    // audit mistake this whole scope item came from: a sweep that searched one
    // module for a value used in another concluded it was unused.
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        local camera = Instance.new("Camera")
        assert(camera:IsA("PVInstance"), "Camera is positional")
        assert(Instance.new("MeshPart"):IsA("PVInstance"), "MeshPart is positional")
        assert(not Instance.new("PointLight"):IsA("PVInstance"), "a light is not")

        camera.CFrame = CFrame.new(0, 5, 0)
        assert(camera:GetPivot().Position == Vector3.new(0, 5, 0))

        -- An offset moves where the camera turns about, which is what makes an
        -- orbit camera a `PivotTo` rather than trigonometry at every call site.
        camera.PivotOffset = CFrame.new(0, 0, -10)
        assert(camera:GetPivot().Position == Vector3.new(0, 5, -10))

        camera:PivotTo(CFrame.new(1, 2, 3))
        assert(camera.CFrame.Position == Vector3.new(1, 2, 13))

        local marker = Instance.new("Folder")
        marker.Name = "PivotOk"
        marker.Parent = workspace
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    CHECK_FALSE(log.contains("[script.err."));
    CHECK(host.world().childCount(host.workspace()) == 1);
}

TEST_CASE("dragging Size and CFrame through their extremes does not take the host down")
{
    // The reproduction attempt for the one reported defect with no repro: the
    // host died while values were being dragged in the inspector, and the
    // captured log held the two lines an ordinary run prints -- the signature of
    // a fault rather than of any C++ error path.
    //
    // A drag is not one write. It is a write every frame, each one read back
    // from the property it just set, and it passes through whatever values the
    // mouse sweeps over on the way -- including zero, negative and absurd. This
    // drives exactly that loop through `Inspector`, so it goes through
    // `enqueue` -> FrameStart drain -> `World::setProperty`, and extracts a
    // frame each time so the renderer sees every value too.
    //
    // It has not reproduced the crash. That is worth recording rather than
    // deleting: it rules out the write path itself, which narrows what remains
    // to the ImGui half and to the defocus the human's note mentions.
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        local part = Instance.new("MeshPart")
        part.Name = "Target"
        part.Parent = workspace

        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.lookAt(Vector3.new(6, 4, 6), Vector3.new(0, 0, 0))
        camera.Parent = workspace
        workspace.CurrentCamera = camera
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());

    const core::InstanceId target =
        host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Target"));
    REQUIRE(target.valid());

    const core::NameAtom sizeProperty = host.world().atoms().lookup("Size");
    const core::NameAtom cframeProperty = host.world().atoms().lookup("CFrame");

    // The values a drag actually sweeps through, plus the ones a fast drag
    // overshoots into. Infinity and NaN are here because `DragScalar` with a
    // speed and no bounds will reach them, and because a NaN in a transform is
    // how a renderer stops being a renderer.
    const std::array<core::f32, 8> sweep{1.0f,  0.5f, 0.0f,  -1.0f,
                                         -0.0f, 1e6f, 1e30f, std::numeric_limits<core::f32>::infinity()};

    app::Inspector inspector;
    inspector.select(target);

    render::RenderWorld snapshot;
    for (std::size_t frame = 0; frame < sweep.size() * 4; ++frame) {
        const core::f32 value = sweep[frame % sweep.size()];
        // Read back first, exactly as the widget does: a drag edits the value
        // the panel is showing, not a value it remembers.
        const std::optional<scene::Value> current = host.world().getProperty(target, sizeProperty);
        REQUIRE(current.has_value());

        inspector.enqueue(target, sizeProperty, scene::Value{core::Vec3{value, value, value}});

        core::CFrameD moved;
        moved.position = core::DVec3{static_cast<core::f64>(value), 1.0, static_cast<core::f64>(value)};
        inspector.enqueue(target, cframeProperty, scene::Value{moved});

        inspector.applyPending(host.world());
        host.tick();
        render::extract(host.world(), host.workspace(), host.lighting(), render::MeshLibrary{}, 1.0f, 0.0f, nullptr,
                        0.0f, nullptr, snapshot);
    }

    // Still alive, still answering, and the panel can still format what it
    // holds -- `formatValue` is the other thing that touches every value.
    CHECK(host.world().alive(target));
    CHECK_FALSE(app::formatValue(host.world(), *host.world().getProperty(target, sizeProperty)).empty());
    CHECK_FALSE(app::formatValue(host.world(), *host.world().getProperty(target, cframeProperty)).empty());
}

// --- D067: the scene is applied BEFORE the entry scripts run -----------------
//
// ADR 0047's lifecycle is load-then-start and `scene_file.h` says so in its
// opening paragraph; E1 shipped it the other way round, and the bill arrived
// the first time somebody opened the editor on a project whose script builds
// its own world. A scene load REPLACES the contents of `Workspace`, so applied
// after the boot drain it destroyed every instance the script had just made
// while the VM kept its references and its connections -- and the first tick
// after Play raised `instance_dead` once a frame forever.
//
// These two cases are the order, stated as behaviour rather than as sequence:
// what the script builds survives a boot scene, and what the scene brings is
// there for the script to find.

namespace {

[[nodiscard]] core::InstanceId bootChildNamed(app::WorldHost& host, std::string_view name)
{
    return host.world().findFirstChild(host.workspace(), host.world().atoms().lookup(name));
}

constexpr std::string_view kOnePartScene =
    R"({"format":"scene","version":1,"root":{"class":"Workspace","name":"Workspace",)"
    R"("children":[{"class":"Part","name":"FromTheScene","properties":{}}]}})";

} // namespace

TEST_CASE("a boot scene does not destroy what the entry scripts built")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local part = Instance.new("Part")
        part.Name = "BuiltByScript"
        part.Parent = workspace
    )");
    project.write("content/scenes/main.scene.json", kOnePartScene);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    CHECK(host.bootSceneApplied());
    CHECK(host.bootSceneReport().instances == 1);

    const core::InstanceId built = bootChildNamed(host, "BuiltByScript");
    REQUIRE(built.valid());
    // The whole of the defect in one line: with the load after the drain this
    // id was destroyed and every script handle to it was dead.
    CHECK(host.world().alive(built));
    CHECK(bootChildNamed(host, "FromTheScene").valid());
}

TEST_CASE("an empty boot scene leaves a scripted world alone")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local part = Instance.new("Part")
        part.Name = "BuiltByScript"
        part.Parent = workspace
    )");
    // Exactly what `New Scene` followed by `Save As` writes, which is the file
    // that was actually on disk when this was reported.
    project.write("content/scenes/main.scene.json",
                  R"({"format":"scene","version":1,"root":{"class":"Workspace",)"
                  R"("name":"Workspace","properties":{"CurrentCamera":null},"children":[]}})");

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    const core::InstanceId built = bootChildNamed(host, "BuiltByScript");
    REQUIRE(built.valid());
    CHECK(host.world().alive(built));
}

TEST_CASE("a scene script compiled for a package runs, and its text is gone from the file (S0.3)")
{
    // A package's scene carried its scripts' text: the bytecode option covered
    // `src/` only. Compiled in place, the file holds bytecode, and the script
    // it holds still runs.
    Captured log;
    Project project;
    project.write(
        "content/scenes/main.scene.json",
        R"({"format":"scene","version":2,"root":{"class":"Workspace","name":"Workspace","children":[)"
        R"({"class":"Script","name":"Builder","properties":{"Source":)"
        R"("-- SOURCE-ONLY-MARK\nlocal p = Instance.new(\"Part\")\np.Name = \"BuiltByCompiled\"\np.Parent = workspace\n"}}]}})");

    app::ScriptPackageReport report;
    REQUIRE(app::compileContentScripts(project.root / "content", report));
    CHECK(report.compiled == 1);
    std::string text;
    REQUIRE(platform::readTextFile(project.root / "content" / "scenes" / "main.scene.json", text));
    CHECK(text.find("SOURCE-ONLY-MARK") == std::string::npos);
    CHECK(text.find(scene::CompiledSourcePrefix) != std::string::npos);
    // Again: compiled already, nothing to do.
    REQUIRE(app::compileContentScripts(project.root / "content", report));
    CHECK(report.compiled == 0);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());
    host.tick();
    CHECK(bootChildNamed(host, "BuiltByCompiled").valid());
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());

    // Saved again, a compiled script stays compiled -- and still no text.
    const std::string saved = scene::writeScene(host.world());
    CHECK(saved.find(scene::CompiledSourcePrefix) != std::string::npos);
}

TEST_CASE("a replica starts no script its join is about to destroy (S0.4)")
{
    // A script inside a part the authority replicates ran its file scope once
    // on a joined client and was then destroyed with the part: the join
    // cleared the scene after the first drain. Cleared before the first
    // script starts, it never runs there at all.
    Captured log;
    Project project;
    project.write("content/scenes/main.scene.json",
                  R"({"format":"scene","version":2,"root":{"class":"Workspace","name":"Workspace","children":[)"
                  R"({"class":"Part","name":"Crate","children":[)"
                  R"json({"class":"Script","name":"Talker","properties":{"Source":"print(\"talker ran\")"}}]}]}})json");

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    options.networkTopology = scene::NetworkTopology::Replica;
    REQUIRE_FALSE(host.boot(options).has_value());
    for (int tick = 0; tick < 3; ++tick)
        host.tick();
    CHECK_FALSE(log.contains("talker ran"));
    CHECK_FALSE(bootChildNamed(host, "Crate").valid());

    // The same scene alone runs it: the script is fine, its side is not.
    Captured solo;
    app::WorldHost alone;
    options.networkTopology = scene::NetworkTopology::Solo;
    REQUIRE_FALSE(alone.boot(options).has_value());
    alone.tick();
    CHECK(solo.contains("talker ran"));
}

TEST_CASE("a scene the entry scripts can see, because it is there before they run")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local found = workspace:FindFirstChild("FromTheScene")
        local marker = Instance.new("Part")
        marker.Name = if found then "SawTheScene" else "SawNothing"
        marker.Parent = workspace
    )");
    project.write("content/scenes/main.scene.json", kOnePartScene);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    CHECK(bootChildNamed(host, "SawTheScene").valid());
    CHECK_FALSE(bootChildNamed(host, "SawNothing").valid());
}

TEST_CASE("a boot scene that will not read is reported and the world still boots")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local part = Instance.new("Part")
        part.Name = "BuiltByScript"
        part.Parent = workspace
    )");
    project.write("content/scenes/main.scene.json", "{ this is not a scene");

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    CHECK_FALSE(host.bootSceneApplied());
    CHECK(bootChildNamed(host, "BuiltByScript").valid());
}

TEST_CASE("a project with a scene AND world-building scripts is told it has two of everything")
{
    // D074, and the honest cost of D067's fix rather than a regression from it:
    // once the scene loads BEFORE the scripts, a project whose code also builds
    // a world has two sources for one world and the engine cannot merge them.
    // What it can do is say so with both numbers, the first time, instead of
    // leaving somebody to find two characters in their own scene.
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local part = Instance.new("Part")
        part.Name = "BuiltByScript"
        part.Parent = workspace
    )");
    project.write("content/scenes/main.scene.json", kOnePartScene);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    CHECK(bootChildNamed(host, "FromTheScene").valid());
    CHECK(bootChildNamed(host, "BuiltByScript").valid());
    CHECK(log.contains("two sources"));
}

TEST_CASE("a scene with no world-building scripts says nothing at all")
{
    // The arrangement ADR 0047 asks projects to become, and `examples/06-scene`
    // is one: the world is the file and the script is only what it does. A
    // warning here would be noise on the shape that is correct.
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        RunService.Heartbeat:Connect(function() end)
    )");
    project.write("content/scenes/main.scene.json", kOnePartScene);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    CHECK(bootChildNamed(host, "FromTheScene").valid());
    CHECK_FALSE(log.contains("two sources"));
}

// --- A script is an instance, and an instance is what runs (ADR 0057) --------
//
// Before this, `startScripts` walked the mounted-FILE list: a Script the scene
// brought, or one somebody made in the editor, was saved with its `Source` and
// never ran, while a mounted one ran with its `Source` empty. Two disconnected
// halves of one idea. These cases are the idea, joined.

namespace {

// A scene holding one `Script` whose source builds a part, so "did it run" is a
// question about the world rather than about a log line.
[[nodiscard]] std::string sceneWithScript(std::string_view name, std::string_view body, bool enabled = true)
{
    std::string out = R"({"format":"scene","version":1,"root":{"class":"Workspace","name":"Workspace","children":[)";
    out += R"({"class":")";
    out += name;
    out += R"(","name":"SceneScript","properties":{"Source":")";
    out += body;
    out += R"(")";
    if (!enabled)
        out += R"(,"Enabled":false)";
    out += R"(}}]}})";
    return out;
}

} // namespace

TEST_CASE("a Script the scene brought runs, because an instance is what runs")
{
    Captured log;
    Project project;
    project.write("content/scenes/main.scene.json",
                  sceneWithScript("Script", R"(local p = Instance.new(\"Part\") p.Name = \"MadeByTheScene\" )"
                                            R"(p.Parent = workspace)"));

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    // The whole of ADR 0057 in one assertion: nothing mounted this, no file
    // exists for it, and it ran.
    CHECK(bootChildNamed(host, "MadeByTheScene").valid());
}

TEST_CASE("a disabled Script in the scene does not run")
{
    Captured log;
    Project project;
    project.write("content/scenes/main.scene.json",
                  sceneWithScript("Script",
                                  R"(local p = Instance.new(\"Part\") p.Name = \"ShouldNotExist\" )"
                                  R"(p.Parent = workspace)",
                                  /*enabled=*/false));

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    CHECK_FALSE(bootChildNamed(host, "ShouldNotExist").valid());
}

TEST_CASE("a ModuleScript in the scene still only runs when it is required")
{
    Captured log;
    Project project;
    // The exact class rather than `IsA`: a ModuleScript shares `Source` with a
    // Script and deliberately does not start by itself.
    project.write("content/scenes/main.scene.json",
                  sceneWithScript("ModuleScript", R"(local p = Instance.new(\"Part\") p.Name = \"NotByItself\" )"
                                                  R"(p.Parent = workspace return {})"));

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    CHECK_FALSE(bootChildNamed(host, "NotByItself").valid());
}

TEST_CASE("a mounted script carries its file in its own Source")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", "local x = 1\nreturn x\n");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(app::testing::bootOptions(project.root)).has_value());

    const scene::World& w = host.world();
    const core::InstanceId service = clientScripts(host);
    REQUIRE(service.valid());
    const core::InstanceId script = w.findFirstChild(service, w.atoms().lookup("init"));
    REQUIRE(script.valid());

    // What ADR 0050 decided and the mount never did. Without this the script
    // editor would open a tab on an empty string.
    const std::optional<scene::Value> source = w.getProperty(script, w.atoms().lookup("Source"));
    REQUIRE(source.has_value());
    const auto* text = std::get_if<std::string>(&source.value());
    REQUIRE(text != nullptr);
    CHECK(*text == "local x = 1\nreturn x\n");
}

// --- A script runs when you press play (ADR 0058) ----------------------------
//
// `startScripts` is one option and one branch, which is exactly why it is worth
// four cases: a branch that is right for every caller but one, and wrong for
// that one, looks identical to a branch that is wrong for every caller but one.

namespace {

// Counts what is under `Workspace`, which is the countable form of "a project
// opened in the editor shows what its scene holds and nothing else".
[[nodiscard]] core::usize workspaceChildren(app::WorldHost& host)
{
    core::usize count = 0;
    for (core::InstanceId child = host.world().firstChild(host.workspace()); child.valid();
         child = host.world().nextSibling(child)) {
        ++count;
    }
    return count;
}

// A project whose scene holds one part and whose script builds another. The two
// halves are what every case below tells apart.
void writeSceneAndScript(Project& project)
{
    project.write("src/client/init.luau", R"(
        local p = Instance.new("Part")
        p.Name = "BuiltByScript"
        p.Parent = workspace
    )");
    project.write("content/scenes/main.scene.json", kOnePartScene);
}

} // namespace

TEST_CASE("boot mounts the scripts and the editor does not start them")
{
    Captured log;
    Project project;
    writeSceneAndScript(project);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    options.startScripts = false;
    REQUIRE_FALSE(host.boot(options).has_value());

    // **Counted, not sampled.** "Nothing the script built" is a claim about the
    // whole of `Workspace`, and a check for one absent name would pass just as
    // well on a world where the script had built something else.
    CHECK(workspaceChildren(host) == 1);
    CHECK(bootChildNamed(host, "FromTheScene").valid());
    CHECK_FALSE(bootChildNamed(host, "BuiltByScript").valid());

    // **Mounted is the other half and it is the half that makes this usable.**
    // The `Script` is in the tree, the Explorer shows it, `Source` is editable
    // and a tab can open it -- what waits is the first resumption.
    CHECK(host.mountedScriptCount() == 1);
}

TEST_CASE("every other way of running starts them at boot, as it always did")
{
    Captured log;
    Project project;
    writeSceneAndScript(project);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    // The default, spelled out: a game, `ludwerk dev`, a headless run, the
    // conformance runner and a replay all want behaviour the moment the world
    // exists. This case is the regression the editor's one branch could cause.
    REQUIRE(options.startScripts);
    REQUIRE_FALSE(host.boot(options).has_value());

    CHECK(workspaceChildren(host) == 2);
    CHECK(bootChildNamed(host, "BuiltByScript").valid());
}

TEST_CASE("play starts the scripts a mount-only boot left waiting")
{
    Captured log;
    Project project;
    writeSceneAndScript(project);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    options.startScripts = false;
    REQUIRE_FALSE(host.boot(options).has_value());
    REQUIRE_FALSE(bootChildNamed(host, "BuiltByScript").valid());

    // What the play button does, and nothing else -- the world is the world it
    // was, and the scripts are the ones the boot mounted.
    script::startScripts(host.runtime().state());
    host.tick();

    CHECK(bootChildNamed(host, "BuiltByScript").valid());
    CHECK(workspaceChildren(host) == 2);
}

TEST_CASE("stop throws the VM away, so a second play is the same as the first")
{
    Captured log;
    Project project;
    // **A required module is VM state and nothing else**, which is what makes it
    // the probe this needs. Restoring the world at stop cannot touch it: a
    // module's cached result lives in the registry, so a second play that found
    // `n` already at one is a second play running on the first one's VM.
    //
    // Everything else that accumulates is invisible to a world comparison in
    // exactly this way -- connections, globals, queued work -- and that is why
    // "two plays in a row are identical" went unreported for so long.
    project.write(".luaurc", R"({ "aliases": { "shared": "src/shared" } })");
    project.write("src/shared/state.luau", R"(
        return { n = 0 }
    )");
    project.write("src/client/init.luau", R"(
        local state = require("@shared/state")
        state.n += 1
        local p = Instance.new("Part")
        p.Name = "Run" .. tostring(state.n)
        p.Parent = workspace

        -- Due long after the stop below. If the VM outlives a stop, so does
        -- this, and a world nobody is playing grows a part on its own.
        task.delay(1.0, function()
            local late = Instance.new("Part")
            late.Name = "Late"
            late.Parent = workspace
        end)
    )");
    project.write("content/scenes/main.scene.json", kOnePartScene);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    options.startScripts = false;
    REQUIRE_FALSE(host.boot(options).has_value());

    const scene::WorldSnapshot before = host.world().snapshot();

    script::startScripts(host.runtime().state());
    host.tick();
    CHECK(bootChildNamed(host, "Run1").valid());
    CHECK(workspaceChildren(host) == 2);

    // The editor's stop, in the order the editor performs it: the world first,
    // then the VM -- so the new runtime binds to the tree as it stands.
    host.world().restore(before);
    REQUIRE_FALSE(host.restartRuntime().has_value());
    CHECK(workspaceChildren(host) == 1);

    // Well past the delay. Nothing is playing, so nothing may appear.
    for (int index = 0; index < 90; ++index)
        host.tick();
    CHECK_FALSE(bootChildNamed(host, "Late").valid());
    CHECK(workspaceChildren(host) == 1);

    // **And the second play is the first play.** `Run1` and not `Run2`, which is
    // the whole claim: the module was evaluated again because there was no VM
    // left holding its result.
    script::startScripts(host.runtime().state());
    host.tick();
    CHECK(bootChildNamed(host, "Run1").valid());
    CHECK_FALSE(bootChildNamed(host, "Run2").valid());
    CHECK(workspaceChildren(host) == 2);
}

TEST_CASE("a rebuilt runtime adopts the services it found rather than making more")
{
    // The failure this rules out is the one M4 shipped for four milestones: a
    // service the host cached and the VM rebuilt are two different instances,
    // and every read afterwards answers with the struct defaults.
    Captured log;
    Project project;
    writeSceneAndScript(project);

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    options.startScripts = false;
    REQUIRE_FALSE(host.boot(options).has_value());

    const core::InstanceId dataModel = host.runtime().dataModel();
    const core::InstanceId workspace = host.workspace();
    REQUIRE(dataModel.valid());
    REQUIRE(workspace.valid());
    const core::usize servicesBefore = [&] {
        core::usize count = 0;
        for (core::InstanceId child = host.world().firstChild(dataModel); child.valid();
             child = host.world().nextSibling(child)) {
            ++count;
        }
        return count;
    }();

    REQUIRE_FALSE(host.restartRuntime().has_value());

    CHECK(host.runtime().dataModel() == dataModel);
    CHECK(host.workspace() == workspace);
    // Counted, because a second `Workspace` beside the first is exactly what an
    // adoption that silently created would look like.
    core::usize servicesAfter = 0;
    for (core::InstanceId child = host.world().firstChild(dataModel); child.valid();
         child = host.world().nextSibling(child)) {
        ++servicesAfter;
    }
    CHECK(servicesAfter == servicesBefore);

    // The mount table is a fact about the project and survives the VM that held
    // it -- without it every chunk name and every `Ctrl+S` forgets its file.
    CHECK(host.mountedScriptCount() == 1);
}

TEST_CASE("a husk lives while a script holds it, and is swept once nothing does")
{
    Captured log;
    Project project;
    project.write("main.luau", R"(
        local streaming = game:GetService("StreamingService")
        local run = game:GetService("RunService")
        local held = Instance.new("Part")
        held.Name = "Held"
        held.Parent = workspace
        do
            local loose = Instance.new("Part")
            loose.Name = "Loose"
            loose.Parent = workspace
        end
        local seen = 0
        streaming.InstanceStreamedOut:Connect(function(instance)
            if instance == held then
                seen += 1
            end
        end)
        run.Heartbeat:Connect(function()
            held.Name = if seen > 0 then "Seen" else "Held"
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root / "main.luau")).has_value());
    scene::World& world = host.world();
    const core::InstanceId held = world.findFirstChild(host.workspace(), world.atoms().intern("Held"));
    const core::InstanceId loose = world.findFirstChild(host.workspace(), world.atoms().intern("Loose"));
    REQUIRE(held.valid());
    REQUIRE(loose.valid());

    // **Held is what the VM still has a handle to**, once the collector has
    // taken what nothing references.
    lua_gc(host.runtime().state(), LUA_GCCOLLECT, 0);
    CHECK(host.instanceHeld(held));
    CHECK_FALSE(host.instanceHeld(loose));

    // Both reported as husks, the way the glue reports what it kept: the
    // signal's own argument holds each until its handlers have run.
    REQUIRE_FALSE(world.setParent(held, core::InstanceId{}).has_value());
    REQUIRE_FALSE(world.setParent(loose, core::InstanceId{}).has_value());
    const auto resident = [](core::DVec3, core::f64) { return true; };
    host.publishStreamingResults({held, loose}, resident);
    CHECK(host.huskCount() == 2);
    host.tick();

    // The handler saw its instance, and the handle still resolves on a husk.
    CHECK(world.atoms().text(world.name(held)) == "Seen");
    CHECK_FALSE(world.parentOf(held).valid());

    // Nothing holds the other one now, and the next publish sweeps it.
    lua_gc(host.runtime().state(), LUA_GCCOLLECT, 0);
    host.publishStreamingResults({}, resident);
    CHECK(host.huskCount() == 1);
    host.tick();
    CHECK(world.alive(held));
    CHECK_FALSE(world.alive(loose));

    // Parented back into the world by a script, it is not a husk any more.
    REQUIRE_FALSE(world.setParent(held, host.workspace()).has_value());
    host.publishStreamingResults({}, resident);
    CHECK(host.huskCount() == 0);
    CHECK(world.alive(held));
    CHECK_FALSE(log.contains("[script.err."));
}

TEST_CASE("every global the editor offers and lints against is one the VM really has")
{
    // **The list the script editor completes from and the unknown-global lint
    // reads** (`engineGlobals`), checked against a booted VM. `Material` joined
    // the engine with ADR 0090 and not the list, so the editor underlined every
    // `Material.load` as an unknown global -- the owner's screenshot.
    app::WorldHost host;
    REQUIRE_FALSE(host.boot({}).has_value());
    for (const std::string_view name : app::engineGlobals()) {
        // A type alias and nothing at runtime (api-design.md §2.3); and the
        // running script's own instance, which a console line has none of.
        if (name == "Content" || name == "script")
            continue;
        CAPTURE(std::string(name));
        CHECK_FALSE(host.runtime().evaluate("assert(" + std::string(name) + " ~= nil)").has_value());
    }
}

TEST_CASE("the starter template plays: its scripts run from the scene and turn the spinner")
{
    // **The template a new project starts from, played** (ADR 0092). Its
    // scripts live in its scene -- one inside the part it turns, a module in
    // ReplicatedStorage, and `src/client/Main.luau` -- which no `.luau` analysis in
    // the gate reaches, so this is the check that they run.
    Captured log;
    Project project;
    std::string scene;
    {
        std::ifstream file(std::filesystem::path(ENG_TEST_CATALOG).parent_path().parent_path() / "templates" /
                               "starter" / "content" / "scenes" / "main.scene.json",
                           std::ios::binary);
        REQUIRE(file.good());
        scene.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    project.write("content/scenes/main.scene.json", scene);
    {
        std::ifstream file(std::filesystem::path(ENG_TEST_CATALOG).parent_path().parent_path() / "templates" /
                               "starter" / "src" / "client" / "Main.luau",
                           std::ios::binary);
        REQUIRE(file.good());
        project.write("src/client/Main.luau",
                      std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
    }

    app::WorldHost host;
    app::WorldHostOptions options = app::testing::bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());
    REQUIRE(host.bootSceneApplied());

    const core::InstanceId level = bootChildNamed(host, "Level");
    REQUIRE(level.valid());
    const core::InstanceId spinner = host.world().findFirstChild(level, host.world().atoms().lookup("Spinner"));
    REQUIRE(spinner.valid());
    const core::NameAtom frame = host.world().atoms().lookup("CFrame");
    const auto look = [&]() {
        const std::optional<scene::Value> value = host.world().getProperty(spinner, frame);
        REQUIRE(value.has_value());
        return std::get<core::CFrameD>(*value).rotation.m[0][0];
    };
    const core::f32 before = look();
    for (int tick = 0; tick < 30; ++tick)
        host.tick();

    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("Hello from the engine!"));
    CHECK(look() != doctest::Approx(static_cast<double>(before)));
}

// --- ADR 0105: three script services ------------------------------------------

namespace {

[[nodiscard]] core::InstanceId serviceOf(app::WorldHost& host, std::string_view className)
{
    const scene::World& w = host.world();
    return w.findFirstChildOfClass(host.runtime().dataModel(), w.classes().findId(w.atoms().lookup(className)));
}

[[nodiscard]] core::InstanceId childNamed(app::WorldHost& host, core::InstanceId parent, std::string_view name)
{
    return host.world().findFirstChild(parent, host.world().atoms().lookup(name));
}

} // namespace

TEST_CASE("the five code folders mount into the services their code runs from")
{
    Captured log;
    Project project;
    project.write("src/client/hud.luau", "print('client')");
    project.write("src/server/rules.luau", "print('server')");
    project.write("src/shared/Racing.luau", "return { speed = 8 }");
    project.write("src/scenes/arena/server/countdown.luau", "print('arena server')");
    project.write("src/scenes/arena/client/scoreboard/panel.luau", "print('arena client')");
    // Another scene's code is that scene's, and does not mount here.
    project.write("src/scenes/menu/client/buttons.luau", "print('menu')");
    project.write("content/scenes/arena.scene.json", R"({"format":"scene","version":2,"root":{}})");

    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "arena.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());

    scene::World& w = host.world();
    const core::InstanceId global = serviceOf(host, "GlobalScriptService");
    REQUIRE(global.valid());
    CHECK(childNamed(host, childNamed(host, global, "Client"), "hud").valid());
    CHECK(childNamed(host, childNamed(host, global, "Server"), "rules").valid());
    const core::InstanceId racing = childNamed(host, childNamed(host, global, "Shared"), "Racing");
    REQUIRE(racing.valid());
    CHECK(w.classOf(racing) == w.classes().findId(w.atoms().lookup("ModuleScript")));

    CHECK(childNamed(host, serviceOf(host, "ServerScriptService"), "countdown").valid());
    const core::InstanceId board = childNamed(host, serviceOf(host, "ClientScriptService"), "scoreboard");
    REQUIRE(board.valid());
    CHECK(childNamed(host, board, "panel").valid());
    CHECK_FALSE(childNamed(host, serviceOf(host, "ClientScriptService"), "buttons").valid());

    for (int tick = 0; tick < 2; ++tick)
        host.tick();
    CHECK(log.contains("client"));
    CHECK(log.contains("server"));
    CHECK(log.contains("arena server"));
    CHECK(log.contains("arena client"));
    CHECK_FALSE(log.contains("menu"));
}

TEST_CASE("src/scripts is read as src/client for one release, and says so")
{
    Captured log;
    Project project;
    project.write("src/scripts/old.luau", "print('still here')");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    CHECK(childNamed(host, clientScripts(host), "old").valid());
    CHECK(log.contains("src/scripts/ is read as src/client/"));
    host.tick();
    CHECK(log.contains("still here"));
}

TEST_CASE("server code runs only on the authority and client code never on a dedicated server")
{
    const auto run = [](scene::NetworkTopology topology, bool& server, bool& client, bool& anywhere) {
        Captured log;
        Project project;
        project.write("src/server/rules.luau", "print('ran:server')");
        project.write("src/client/hud.luau", "print('ran:client')");
        // A script elsewhere in the world runs on every machine, as before.
        project.write("src/shared/Marker.luau", "return true");
        project.write("content/scenes/main.scene.json",
                      R"json({"format":"scene","version":2,"root":{"children":[)json"
                      R"json({"class":"Script","name":"Door","properties":{"Source":"print('ran:anywhere')"}}]}})json");
        app::WorldHost host;
        app::WorldHostOptions options = bootOptions(project.root);
        options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
        options.networkTopology = topology;
        REQUIRE_FALSE(host.boot(options).has_value());
        for (int tick = 0; tick < 2; ++tick)
            host.tick();
        server = log.contains("ran:server");
        client = log.contains("ran:client");
        anywhere = log.contains("ran:anywhere");
    };

    bool server = false;
    bool client = false;
    bool anywhere = false;
    run(scene::NetworkTopology::Solo, server, client, anywhere);
    CHECK(server);
    CHECK(client);
    CHECK(anywhere);

    run(scene::NetworkTopology::Host, server, client, anywhere);
    CHECK(server);
    CHECK(client);
    CHECK(anywhere);

    run(scene::NetworkTopology::Dedicated, server, client, anywhere);
    CHECK(server);
    CHECK_FALSE(client);
    CHECK(anywhere);

    run(scene::NetworkTopology::Replica, server, client, anywhere);
    CHECK_FALSE(server);
    CHECK(client);
    CHECK(anywhere);
}

TEST_CASE("a script in the world runs where its RunContext says, once per machine (ADR 0138 §3)")
{
    // The owner's door: a stamp's server half and client half, both in the
    // world. Before ADR 0138 a script outside the services ran on every
    // machine whatever it was, so the dedicated server loaded the sounds' code
    // and every player the rules'.
    const auto run = [](scene::NetworkTopology topology) {
        Captured log;
        Project project;
        project.write(
            "content/scenes/main.scene.json",
            R"json({"format":"scene","version":2,"root":{"children":[)json"
            R"json({"class":"Script","name":"Rules","properties":{"Source":"print('rc:server')","RunContext":"Server"}},)json"
            R"json({"class":"Script","name":"Sound","properties":{"Source":"print('rc:client')","RunContext":"Client"}},)json"
            R"json({"class":"Script","name":"Both","properties":{"Source":"print('rc:shared')"}}]}})json");
        app::WorldHost host;
        app::WorldHostOptions options = bootOptions(project.root);
        options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
        options.networkTopology = topology;
        REQUIRE_FALSE(host.boot(options).has_value());
        for (int tick = 0; tick < 3; ++tick)
            host.tick();
        const auto count = [&](std::string_view needle) {
            return std::count_if(log.lines.begin(), log.lines.end(),
                                 [&](const std::string& line) { return line.find(needle) != std::string::npos; });
        };
        return std::array<std::ptrdiff_t, 3>{count("rc:server"), count("rc:client"), count("rc:shared")};
    };

    // Solo and a host are the server and a player at once: every side, once.
    CHECK(run(scene::NetworkTopology::Solo) == std::array<std::ptrdiff_t, 3>{1, 1, 1});
    CHECK(run(scene::NetworkTopology::Host) == std::array<std::ptrdiff_t, 3>{1, 1, 1});
    CHECK(run(scene::NetworkTopology::Dedicated) == std::array<std::ptrdiff_t, 3>{1, 0, 1});
    CHECK(run(scene::NetworkTopology::Replica) == std::array<std::ptrdiff_t, 3>{0, 1, 1});
}

TEST_CASE("a script's RunContext is written only when it is not Shared, and hashed likewise (ADR 0138 §7)")
{
    Project project;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    scene::World& w = host.world();
    const core::InstanceId script = w.create(w.classes().findId(w.atoms().lookup("Script")));
    REQUIRE(w.setParent(script, host.workspace()) == std::nullopt);
    const core::u64 before = w.worldHash();
    CHECK(scene::writeScene(w).find("RunContext") == std::string::npos);

    REQUIRE(w.setProperty(script, w.atoms().intern("RunContext"),
                          scene::Value{scene::EnumValue{scene::generated::RunContextEnumId, 1}}) ==
            scene::World::SetResult::Changed);
    CHECK(w.worldHash() != before);
    const std::string written = scene::writeScene(w);
    const std::size_t at = written.find("\"RunContext\"");
    REQUIRE(at != std::string::npos);
    CHECK(written.find("\"Server\"", at) == written.find_first_of('"', written.find(':', at)));
}

TEST_CASE("a script moved into a script service takes its side, keeps it when taken out, and undo puts it back")
{
    // ADR 0138 §2: server code taken out of `ServerScriptService` stays server
    // code, and never becomes `Shared` by accident, which would ship it to
    // every player.
    Project project;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    scene::World& w = host.world();
    const core::InstanceId root = host.runtime().dataModel();
    app::Editor editor;
    app::Inspector inspector;
    const core::NameAtom runContext = w.atoms().intern("RunContext");
    const auto sideOf = [&](core::InstanceId id) {
        return std::get<scene::EnumValue>(w.getProperty(id, runContext).value()).value;
    };

    REQUIRE(
        editor.createInstance(w, w.classes().findId(w.atoms().lookup("Script")), host.workspace(), root, inspector));
    const core::InstanceId script = inspector.selection();
    REQUIRE(w.alive(script));
    CHECK(sideOf(script) == 2);

    const std::array<core::InstanceId, 1> one{script};
    REQUIRE(editor.reparent(w, one, serviceOf(host, "ServerScriptService"), root, inspector));
    CHECK(sideOf(script) == 1);
    REQUIRE(editor.reparent(w, one, host.workspace(), root, inspector));
    CHECK(w.parentOf(script) == host.workspace());
    CHECK(sideOf(script) == 1);

    // One step each: undoing the move out, then the move in, gives back Shared.
    REQUIRE(editor.undo(w, inspector));
    REQUIRE(editor.undo(w, inspector));
    CHECK(sideOf(script) == 2);

    // The Insert menu's entries ask for a side; a service overrides it.
    REQUIRE(
        editor.createInstance(w, w.classes().findId(w.atoms().lookup("Script")), host.workspace(), root, inspector, 0));
    CHECK(sideOf(inspector.selection()) == 0);
    REQUIRE(editor.createInstance(w, w.classes().findId(w.atoms().lookup("Script")),
                                  serviceOf(host, "ServerScriptService"), root, inspector, 0));
    CHECK(sideOf(inspector.selection()) == 1);
}

TEST_CASE("a stamp saved with a script of each side reads back with each side (script sides S3)")
{
    // The editor saves a stamp through `writeStamp`, and a door's two halves
    // must come back as they went -- a `Shared` written as nothing.
    Project project;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    scene::World& w = host.world();
    const core::InstanceId door = w.create(w.classes().findId(w.atoms().lookup("Model")));
    const core::NameAtom runContext = w.atoms().intern("RunContext");
    for (const core::i32 side : {0, 1, 2}) {
        const core::InstanceId half = w.create(w.classes().findId(w.atoms().lookup("Script")));
        w.setName(half, w.atoms().intern("Half" + std::to_string(side)));
        REQUIRE(
            w.setProperty(half, runContext, scene::Value{scene::EnumValue{scene::generated::RunContextEnumId, side}}) !=
            scene::World::SetResult::UnknownProperty);
        REQUIRE_FALSE(w.setParent(half, door).has_value());
    }
    const std::string saved = scene::writeStamp(w, door);
    const core::InstanceId read = scene::readStamp(w, saved, core::InstanceId{}, "door");
    REQUIRE(read.valid());
    for (const core::i32 side : {0, 1, 2}) {
        const core::InstanceId half = w.findFirstChild(read, w.atoms().lookup("Half" + std::to_string(side)));
        REQUIRE(half.valid());
        CHECK(std::get<scene::EnumValue>(w.getProperty(half, runContext).value()).value == side);
    }
}

TEST_CASE("a script moved in the tick a scene changes is stopped or started all the same (script sides S3)")
{
    // The scene load's start walk used to throw the moved-scripts queue away:
    // a running script taken out of the world in that tick went on running,
    // and one put into `GlobalScriptService` never started.
    Captured log;
    Project project;
    const std::string templates =
        R"json("storage":{"ReplicatedStorage":{"class":"ReplicatedStorage","name":"ReplicatedStorage","children":[)json"
        R"json({"class":"Script","name":"Ticker","properties":{"Source":"game:GetService('RunService').Heartbeat:Connect(function() print('audit-tick') end)"}},)json"
        R"json({"class":"Script","name":"Starter","properties":{"Source":"print('audit-late-start')"}}]}})json";
    project.write("content/scenes/a.scene.json",
                  R"json({"format":"scene","version":2,"root":{},)json" + templates + "}");
    project.write("content/scenes/b.scene.json", R"json({"format":"scene","version":2,"root":{}})json");
    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "a.scene.json";
    options.bootScenePath = "scenes/a.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());
    scene::World& w = host.world();
    const core::InstanceId storage =
        w.findFirstChildOfClass(host.runtime().dataModel(), w.classes().findId(w.atoms().lookup("ReplicatedStorage")));
    const core::InstanceId global = w.findFirstChildOfClass(
        host.runtime().dataModel(), w.classes().findId(w.atoms().lookup("GlobalScriptService")));
    const core::InstanceId shared = w.findFirstChild(global, w.atoms().lookup("Shared"));
    const core::InstanceId ticker = w.clone(w.findFirstChild(storage, w.atoms().lookup("Ticker")));
    const core::InstanceId starter = w.clone(w.findFirstChild(storage, w.atoms().lookup("Starter")));
    REQUIRE_FALSE(w.setParent(ticker, shared).has_value());
    for (int tick = 0; tick < 5; ++tick)
        host.tick();
    const auto ticks = [&]() {
        return std::count_if(log.lines.begin(), log.lines.end(),
                             [](const std::string& line) { return line.find("audit-tick") != std::string::npos; });
    };
    REQUIRE(ticks() > 0);

    // Between one drain and the next, then the scene change: what a handler
    // of a late phase, or the host, does.
    REQUIRE_FALSE(w.setParent(ticker, core::InstanceId{}).has_value());
    REQUIRE_FALSE(w.setParent(starter, shared).has_value());
    REQUIRE_FALSE(host.loadScene("scenes/b.scene.json").has_value());
    host.tick();
    const auto settled = ticks();
    for (int tick = 0; tick < 10; ++tick)
        host.tick();
    CHECK(ticks() == settled);
    CHECK(log.contains("audit-late-start"));
}

TEST_CASE("changing scenes again and again does not keep the old scenes' scripts alive (script sides S3)")
{
    // A run's globals table was held by its record until the slot was reused
    // by a script with a different generation, which overwrote the reference
    // without letting it go: every scene change kept every script it closed.
    Project project;
    std::string scripts;
    for (int index = 0; index < 40; ++index) {
        scripts += std::string(index == 0 ? "" : ",") + R"json({"class":"Script","name":"Heavy)json" +
                   std::to_string(index) + R"json(","properties":{"Source":"Big = table.create(20000, 0)"}})json";
    }
    for (const char* name : {"a", "b"}) {
        project.write(std::string("content/scenes/") + name + ".scene.json",
                      R"json({"format":"scene","version":2,"root":{"children":[)json" + scripts + "]}}");
    }
    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "a.scene.json";
    options.bootScenePath = "scenes/a.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());
    host.tick();
    lua_State* L = host.runtime().state();
    const auto heapKb = [&]() {
        lua_gc(L, LUA_GCCOLLECT, 0);
        return lua_gc(L, LUA_GCCOUNT, 0);
    };
    const int before = heapKb();
    for (int change = 0; change < 8; ++change) {
        REQUIRE_FALSE(host.loadScene(change % 2 == 0 ? "scenes/b.scene.json" : "scenes/a.scene.json").has_value());
        host.tick();
        host.tick();
    }
    // One scene's scripts hold about 6 MB; eight changes that kept them all
    // would hold forty-odd.
    CHECK(heapKb() - before < 16 * 1024);
}

TEST_CASE("an Enabled write in the editor, with nothing playing, starts nothing (script sides S3)")
{
    Captured log;
    Project project;
    project.write(
        "content/scenes/main.scene.json",
        R"json({"format":"scene","version":2,"root":{"children":[)json"
        R"json({"class":"Script","name":"Sleeper","properties":{"Source":"print('sleeper-ran')","Enabled":false}}]}})json");
    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    options.bootScenePath = "scenes/main.scene.json";
    options.startScripts = false;
    REQUIRE_FALSE(host.boot(options).has_value());
    scene::World& w = host.world();
    const core::InstanceId sleeper = w.findFirstChild(host.workspace(), w.atoms().lookup("Sleeper"));
    REQUIRE(sleeper.valid());
    (void)w.setProperty(sleeper, w.atoms().intern("Enabled"), scene::Value{true});
    for (int tick = 0; tick < 3; ++tick)
        host.tick();
    CHECK_FALSE(log.contains("sleeper-ran"));
}

TEST_CASE("a world with no script runtime keeps no queue of moved scripts (script sides S3)")
{
    // The editor's stages, the preview renderer and the partitioner build
    // scripts into worlds nothing drains; the queue grew in each for ever.
    Project project;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    scene::World scratch(host.classes(), host.enums(), host.atoms(), 7u);
    const core::InstanceId folder = scratch.create(scratch.classes().findId(scratch.atoms().lookup("Folder")));
    for (int index = 0; index < 3; ++index) {
        const core::InstanceId script = scratch.create(scratch.classes().findId(scratch.atoms().lookup("Script")));
        REQUIRE_FALSE(scratch.setParent(script, folder).has_value());
    }
    CHECK(scratch.takeMovedScripts().empty());
}

TEST_CASE("only code is written as bytecode, and only code is compiled for a package (script sides S3)")
{
    // Text that happens to begin like bytecode, in an attribute, was written
    // as `luauc:` and read back as that literal; an attribute called `Source`
    // was compiled as if it were a script.
    Project project;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    scene::World& w = host.world();
    const core::InstanceId part = w.create(w.classes().findId(w.atoms().lookup("Part")));
    REQUIRE_FALSE(w.setParent(part, host.workspace()).has_value());
    REQUIRE(w.setAttribute(part, w.atoms().intern("Blob"), scene::Value{std::string("\x01\x02packed")}));
    CHECK(scene::writeScene(w).find(scene::CompiledSourcePrefix) == std::string::npos);

    project.write("content/scenes/main.scene.json",
                  R"json({"format":"scene","version":2,"root":{"children":[)json"
                  R"json({"class":"Part","name":"Sign","attributes":{"Source":"not luau at all +++"}},)json"
                  R"json({"class":"Script","name":"Real","properties":{"Source":"print(1)"}}]}})json");
    app::ScriptPackageReport report;
    CHECK(app::compileContentScripts(project.root / "content", report));
    std::string written;
    REQUIRE(platform::readTextFile(project.root / "content" / "scenes" / "main.scene.json", written));
    CHECK(written.find("not luau at all +++") != std::string::npos);
    CHECK(report.compiled == 1);
}

TEST_CASE("an authored instance's origin names its scene, and Play numbers what was edited as the file would be read "
          "(script sides S3)")
{
    // The origin said only the tree, so an instance of one scene that lived on
    // into another took the other's scripts on a replica; and an edit in the
    // editor -- an instance inserted -- left every number after it one off
    // from what a client reading the saved file counts.
    Project project;
    project.write("content/scenes/a.scene.json",
                  R"json({"format":"scene","version":2,"root":{"children":[{"class":"Part","name":"Door"}]}})json");
    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "a.scene.json";
    options.bootScenePath = "scenes/a.scene.json";
    options.startScripts = false;
    REQUIRE_FALSE(host.boot(options).has_value());
    scene::World& w = host.world();
    const core::InstanceId door = w.findFirstChild(host.workspace(), w.atoms().lookup("Door"));
    REQUIRE(door.valid());
    CHECK(w.atoms().text(w.originOf(door).asset) == "scene:scenes/a.scene.json#Workspace");
    CHECK(w.originOf(door).index == 0);

    const core::InstanceId inserted = w.create(w.classes().findId(w.atoms().lookup("Part")));
    REQUIRE_FALSE(w.setParent(inserted, host.workspace()).has_value());
    (void)w.moveChild(host.workspace(), inserted, 0);
    scene::renumberOrigins(w);
    CHECK(w.originOf(inserted).index == 0);
    CHECK(w.originOf(door).index == 1);
    CHECK(w.originOf(inserted).asset == w.originOf(door).asset);
}

TEST_CASE("a script that stops takes its connections and its waits with it (script sides S4)")
{
    // Found by running (ludwerk-08, 2026-09-29): a stopped run's handlers were
    // suppressed and never disconnected, so every clone of a projectile with a
    // script that connected to Heartbeat left its handler on the signal for
    // ever, and every tick walked them all -- 19 ms a tick after 137 500.
    Captured log;
    Project project;
    project.write(
        "content/scenes/main.scene.json",
        R"json({"format":"scene","version":2,"root":{},"storage":{"ReplicatedStorage":{)json"
        R"json("class":"ReplicatedStorage","name":"ReplicatedStorage","children":[)json"
        R"json({"class":"Model","name":"Bullet","children":[{"class":"Script","name":"Fly",)json"
        R"json("properties":{"Source":"local n = 0 game:GetService('RunService').Heartbeat:Connect(function() n += 1 end) task.wait(1e6)"}}]}]}}})json");
    project.write("src/client/churn.luau", R"(
        local template = game:GetService("ReplicatedStorage"):WaitForChild("Bullet")
        local live: { Instance } = {}
        game:GetService("RunService").Heartbeat:Connect(function()
            for _, bullet in live do
                bullet:Destroy()
            end
            table.clear(live)
            for _ = 1, 20 do
                local bullet = template:Clone()
                bullet.Parent = workspace
                table.insert(live, bullet)
            end
        end)
    )");
    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
    options.bootScenePath = "scenes/main.scene.json";
    REQUIRE_FALSE(host.boot(options).has_value());
    for (int tick = 0; tick < 10; ++tick)
        host.tick();
    lua_State* L = host.runtime().state();
    const core::usize settled = script::liveConnectionCount(L);
    const core::usize waiting = script::pendingTimerCount(L);
    for (int tick = 0; tick < 100; ++tick)
        host.tick();
    // Two thousand clones have lived; twenty are alive.
    CHECK(script::liveConnectionCount(L) <= settled + 5);
    CHECK(script::pendingTimerCount(L) <= waiting + 5);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

TEST_CASE("a shared module is one module, whether required by path or by instance")
{
    Captured log;
    Project project;
    project.write("src/shared/Counter.luau", "local m = { hits = 0 }\nm.hits += 1\nreturn m");
    project.write("src/client/a.luau", R"(
        local byPath = require("../shared/Counter")
        local global = game:GetService("GlobalScriptService")
        local byInstance = require((global:FindFirstChild("Shared") :: Instance):FindFirstChild("Counter") :: any)
        print(`same:{byPath == byInstance} hits:{byPath.hits}`)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("same:true hits:1"));
}

TEST_CASE("GlobalScriptService's authored contents come from content/global.json, not from a scene")
{
    Captured log;
    Project project;
    project.write("src/client/read.luau", R"(
        local global = game:GetService("GlobalScriptService")
        local settings = (global:FindFirstChild("Shared") :: Instance):FindFirstChild("Settings")
        print(`settings:{settings ~= nil} mode:{global:GetAttribute("Mode")}`)
    )");

    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootGlobalText = R"({"format":"global","version":1,"root":{"class":"GlobalScriptService",)"
                             R"("name":"GlobalScriptService","attributes":{"Mode":"ctf"},"children":[)"
                             R"({"class":"Folder","name":"Shared","mounted":true,"children":[)"
                             R"({"class":"Folder","name":"Settings"}]}]}})";
    REQUIRE_FALSE(host.boot(options).has_value());
    host.tick();
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("settings:true mode:ctf"));

    // And it writes back the same way, the fixed folder as its mark.
    const std::string written = scene::writeGlobal(host.world());
    CHECK(written.find("\"Settings\"") != std::string::npos);
    CHECK(written.find("\"mounted\":true") != std::string::npos);
    CHECK(scene::writeScene(host.world()).find("GlobalScriptService") == std::string::npos);
}

// --- ADR 0106: scenes at run time -------------------------------------------

namespace {

[[nodiscard]] int occurrences(const Captured& log, std::string_view needle)
{
    int count = 0;
    for (const std::string& line : log.lines)
        count += line.find(needle) != std::string::npos ? 1 : 0;
    return count;
}

// Two scenes that differ in their light, their UI and their code.
void writeTwoScenes(Project& project)
{
    project.write(
        "content/scenes/a.scene.json",
        R"json({"format":"scene","version":2,"root":{"children":[)json"
        R"json({"class":"Script","name":"Beat","properties":{"Source":)json"
        R"json("game:GetService('RunService').Heartbeat:Connect(function() print('A-alive') end)"}}]},)json"
        R"json("storage":{"Lighting":{"class":"Lighting","name":"Lighting","properties":{"ClockTime":6}},)json"
        R"json("UIService":{"class":"UIService","name":"UIService","children":[)json"
        R"json({"class":"ScreenGui","name":"MenuUI"}]}}})json");
    project.write(
        "content/scenes/b.scene.json",
        R"json({"format":"scene","version":2,"root":{},)json"
        R"json("storage":{"Lighting":{"class":"Lighting","name":"Lighting","properties":{"ClockTime":18}}}})json");
    // Scene B's own code, from its folder.
    project.write("src/scenes/b/client/hud.luau", "print('b-hud started')");
}

[[nodiscard]] app::WorldHostOptions sceneOptions(const Project& project)
{
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "a.scene.json";
    options.bootScenePath = "scenes/a.scene.json";
    return options;
}

} // namespace

TEST_CASE("a script that survives a scene change keeps its one run, and is never started again (S0.5)")
{
    // A kept screen's script was started again by every scene change while its
    // first run went on: two file scopes, one script.
    Captured log;
    Project project;
    project.write(
        "content/scenes/a.scene.json",
        R"json({"format":"scene","version":2,"root":{},"storage":{)json"
        R"json("UIService":{"class":"UIService","name":"UIService","children":[)json"
        R"json({"class":"ScreenGui","name":"Loading","properties":{"KeepOnSceneLoad":true},"children":[)json"
        R"json({"class":"Script","name":"Kept","properties":{"Source":)json"
        R"json("script:SetAttribute('Runs', ((script:GetAttribute('Runs') :: number?) or 0) + 1)"}}]}]}}})json");
    project.write("content/scenes/b.scene.json", R"json({"format":"scene","version":2,"root":{}})json");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(sceneOptions(project)).has_value());
    host.tick();
    // The kept screen's own script, counted by itself: the scene's next copy of
    // the screen is another instance, with its own run.
    scene::World& world = host.world();
    const core::InstanceId screen = childNamed(host, serviceOf(host, "UIService"), "Loading");
    REQUIRE(screen.valid());
    const core::InstanceId kept = world.findFirstChild(screen, world.atoms().lookup("Kept"));
    REQUIRE(kept.valid());
    const auto runs = [&world, kept] {
        const scene::Value value = world.getAttribute(kept, world.atoms().intern("Runs"));
        const auto* number = std::get_if<core::f64>(&value);
        return number != nullptr ? *number : 0.0;
    };
    REQUIRE(runs() == doctest::Approx(1.0));
    REQUIRE_FALSE(host.loadScene("scenes/b.scene.json").has_value());
    host.tick();
    REQUIRE_FALSE(host.loadScene("scenes/a.scene.json").has_value());
    host.tick();
    REQUIRE(world.alive(kept));
    CHECK(runs() == doctest::Approx(1.0));
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

TEST_CASE("a game changes scene at run time, and the game's own code and a kept screen go with it")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    project.write("src/client/flow.luau", R"(
        local SceneService = game:GetService("SceneService")
        local Lighting = game:GetService("Lighting")
        local UIService = game:GetService("UIService")

        local loading = Instance.new("ScreenGui")
        loading.Name = "Loading"
        loading.KeepOnSceneLoad = true
        loading.Parent = UIService

        local changes = 0
        SceneService.SceneLoaded:Connect(function(path: string)
            changes += 1
            local data = SceneService:GetLoadData()
            local menu = UIService:FindFirstChild("MenuUI") ~= nil
            local kept = UIService:FindFirstChild("Loading") ~= nil
            print(`loaded:{path} changes:{changes} clock:{Lighting.ClockTime} round:{data.Round} `
                .. `current:{SceneService.CurrentScene.Path} menu:{menu} kept:{kept}`)
            if changes < 3 then
                local next = if path == "scenes/a.scene.json" then "scenes/b.scene.json" else "scenes/a.scene.json"
                SceneService:LoadScene(next, { Round = changes + 1 })
            end
        end)
        print(`start current:{SceneService.CurrentScene.Path} clock:{Lighting.ClockTime}`)
        SceneService:LoadScene("scenes/b.scene.json", { Round = 1 })
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(sceneOptions(project)).has_value());
    for (int tick = 0; tick < 12; ++tick)
        host.tick();

    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("start current:scenes/a.scene.json clock:6"));
    CHECK(log.contains("loaded:scenes/b.scene.json changes:1 clock:18 round:1 current:scenes/b.scene.json "
                       "menu:false kept:true"));
    CHECK(log.contains("loaded:scenes/a.scene.json changes:2 clock:6 round:2 current:scenes/a.scene.json "
                       "menu:true kept:true"));
    CHECK(log.contains("loaded:scenes/b.scene.json changes:3 clock:18 round:3"));
    // Scene B's own code started each time B loaded.
    CHECK(occurrences(log, "b-hud started") == 2);

    // In B now: scene A's script is gone with its scene, and its threads do not
    // run.
    const int alive = occurrences(log, "A-alive");
    for (int tick = 0; tick < 5; ++tick)
        host.tick();
    CHECK(occurrences(log, "A-alive") == alive);
    CHECK(host.world().engineState().currentScene == "scenes/b.scene.json");
}

TEST_CASE("a LoadScene that cannot happen leaves the open scene as it was, and says so (audit A8)")
{
    // A scene that is not there, and one that does not read. The open scene
    // must not close for either: before, a missing one ran its close handlers
    // and stayed, and a malformed one was torn down before the parse failed --
    // old instances left, scripts gone, `SceneLoaded` never fired.
    Captured log;
    Project project;
    writeTwoScenes(project);
    project.write("content/scenes/broken.scene.json", R"json({"format":"scene","version":2,"root":{"children":[)json");
    project.write("src/client/try.luau", R"(
        local SceneService = game:GetService("SceneService")
        local failures = 0
        SceneService.SceneLoadFailed:Connect(function(path: string, message: string)
            failures += 1
            print(`failed:{path} said:{#message > 0} current:{SceneService.CurrentScene.Path}`)
            if failures == 1 then
                SceneService:LoadScene("scenes/broken.scene.json")
            end
        end)
        SceneService.SceneLoaded:Connect(function(path: string)
            print(`loaded:{path}`)
        end)
        game:BindToClose(function() end)
        SceneService:LoadScene("scenes/missing.scene.json")
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(sceneOptions(project)).has_value());
    for (int tick = 0; tick < 12; ++tick)
        host.tick();

    CHECK(log.contains("failed:scenes/missing.scene.json said:true current:scenes/a.scene.json"));
    CHECK(log.contains("failed:scenes/broken.scene.json said:true current:scenes/a.scene.json"));
    CHECK_FALSE(log.contains("loaded:"));
    // Scene A is still whole and still running: its script goes on beating.
    const int alive = occurrences(log, "A-alive");
    host.tick();
    CHECK(occurrences(log, "A-alive") == alive + 1);
    CHECK(host.world().engineState().currentScene == "scenes/a.scene.json");
    const core::InstanceId ui = host.world().findFirstChildOfClass(
        host.runtime().dataModel(), host.world().classes().findId(host.world().atoms().lookup("UIService")));
    REQUIRE(ui.valid());
    CHECK(host.world().findFirstChild(ui, host.world().atoms().lookup("MenuUI")).valid());
}

TEST_CASE("LoadScene is the authority's: a client in a match is refused, and the change waits for the tick's end")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    project.write("src/client/try.luau", R"(
        local SceneService = game:GetService("SceneService")
        local ok = pcall(function()
            SceneService:LoadScene("scenes/b.scene.json")
        end)
        print(`refused:{not ok}`)
    )");
    app::WorldHostOptions options = sceneOptions(project);
    options.networkTopology = scene::NetworkTopology::Replica;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(options).has_value());
    host.tick();
    CHECK(log.contains("refused:true"));
    CHECK(host.world().engineState().currentScene == "scenes/a.scene.json");
}

TEST_CASE("a host loads a scene by path and it is the one path the editor takes too")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(sceneOptions(project)).has_value());

    REQUIRE_FALSE(host.loadScene("scenes/b.scene.json").has_value());
    CHECK(host.world().engineState().currentScene == "scenes/b.scene.json");
    const core::InstanceId client = serviceOf(host, "ClientScriptService");
    CHECK(childNamed(host, client, "hud").valid());

    // Back to A: B's code goes with B.
    REQUIRE_FALSE(host.loadScene("scenes/a.scene.json").has_value());
    CHECK_FALSE(childNamed(host, client, "hud").valid());

    // A scene that is not there is refused, and the world stays where it was.
    CHECK(host.loadScene("scenes/missing.scene.json").has_value());
    CHECK(host.world().engineState().currentScene == "scenes/a.scene.json");
}

TEST_CASE(
    "a scene changed at run time is partitioned as the boot's was, and the old scene's cells are let go (audit A4)")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    app::WorldHostOptions options = sceneOptions(project);
    // What the engine gives the host: the grid the boot scene meets, and the
    // way to let every streamed cell go. Recorded here.
    std::vector<std::string> partitioned;
    int resets = 0;
    options.partitionScene = [&](scene::World&, const std::filesystem::path& scene) {
        partitioned.push_back(scene.filename().string());
        return std::filesystem::path{};
    };
    options.resetStreaming = [&] { ++resets; };
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(options).has_value());
    REQUIRE(partitioned.size() == 1);
    CHECK(partitioned[0] == "a.scene.json");
    CHECK(resets == 0);

    // Only the boot scene was ever partitioned, and its cells went on
    // streaming into whatever scene came next -- a return to it doubled them.
    REQUIRE_FALSE(host.loadScene("scenes/b.scene.json").has_value());
    CHECK(resets == 1);
    REQUIRE(partitioned.size() == 2);
    CHECK(partitioned[1] == "b.scene.json");
    REQUIRE_FALSE(host.loadScene("scenes/a.scene.json").has_value());
    CHECK(resets == 2);
    CHECK(partitioned.size() == 3);
}

// --- A scene closes as a game does (ADR 0124) -----------------------------------

namespace {

// Where in the log a line containing `needle` first appears, or -1.
[[nodiscard]] int lineOf(const Captured& log, std::string_view needle)
{
    for (std::size_t index = 0; index < log.lines.size(); ++index) {
        if (log.lines[index].find(needle) != std::string::npos)
            return static_cast<int>(index);
    }
    return -1;
}

} // namespace

TEST_CASE("a scene change waits for the old scene's close handlers and drops its game:BindToClose")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    // Scene A's own code: what it saves when it ends, and a registration for
    // the game's close that it will not live to see.
    project.write("src/scenes/a/client/level.luau", R"(
        scene:BindToClose(function()
            task.wait(0.25)
            print("A scene close done")
        end)
        game:BindToClose(function()
            print("A game close ran")
        end)
    )");
    project.write("src/client/game.luau", R"(
        local SceneService = game:GetService("SceneService")
        local function onClose()
            print("global game close")
        end
        -- The same function twice runs twice.
        game:BindToClose(onClose)
        game:BindToClose(onClose)
        local a = SceneService.CurrentScene
        print(`at boot: scene {scene.Name} path {a.Path} same {scene == a} open {a:IsOpen()}`)
        SceneService.SceneLoaded:Connect(function(path: string)
            local refused = not pcall(function()
                a:BindToClose(function() end)
            end)
            print(`loaded {path}: a open {a:IsOpen()} scene {scene.Name} refused {refused}`)
            scene:BindToClose(function()
                print("B scene close")
            end)
        end)
        task.defer(function()
            SceneService:LoadScene("scenes/b.scene.json")
        end)
    )");

    app::WorldHostOptions options = sceneOptions(project);
    options.developer = true;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(options).has_value());
    // A quarter of a second is fifteen ticks: the change must not land before.
    for (int tick = 0; tick < 10; ++tick)
        host.tick();
    CHECK(host.world().engineState().currentScene == "scenes/a.scene.json");
    CHECK_FALSE(log.contains("A scene close done"));
    for (int tick = 0; tick < 20; ++tick)
        host.tick();

    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("at boot: scene a path scenes/a.scene.json same true open true"));
    CHECK(host.world().engineState().currentScene == "scenes/b.scene.json");
    const int done = lineOf(log, "A scene close done");
    const int loaded = lineOf(log, "loaded scenes/b.scene.json: a open false scene b refused true");
    CHECK(done >= 0);
    CHECK(loaded > done);
    // Dropped, with the editor's word for it, and never run.
    CHECK(log.contains("registered game:BindToClose in scene a"));

    // The game closing: the open scene's, then the game's.
    host.close(1.0);
    CHECK_FALSE(log.contains("A game close ran"));
    const int sceneClose = lineOf(log, "B scene close");
    CHECK(sceneClose >= 0);
    CHECK(lineOf(log, "global game close") > sceneClose);
    CHECK(occurrences(log, "global game close") == 2);
}

TEST_CASE("a scene close that outlasts its grace period is cut off and the change goes ahead")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    project.write("src/scenes/a/client/level.luau", R"(
        scene:BindToClose(function()
            task.wait(100)
            print("never")
        end)
        task.defer(function()
            game:GetService("SceneService"):LoadScene("scenes/b.scene.json")
        end)
    )");
    app::WorldHostOptions options = sceneOptions(project);
    options.sceneCloseGrace = 0.5;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(options).has_value());
    for (int tick = 0; tick < 20; ++tick)
        host.tick();
    CHECK(host.world().engineState().currentScene == "scenes/a.scene.json");
    for (int tick = 0; tick < 20; ++tick)
        host.tick();
    CHECK(host.world().engineState().currentScene == "scenes/b.scene.json");
    CHECK(log.contains("still running after"));
    CHECK_FALSE(log.contains("never"));
}

TEST_CASE("game and scene are mailboxes: messages cross between a scene and the game, copied")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    project.write("src/scenes/a/client/level.luau", R"(
        scene:BindToMessage("OpenGate", function(side: string, info: { count: number })
            print(`gate {side} {info.count}`)
            info.count = 99
            game:SendMessage("CoinCollected", 1, { nested = { value = 5 } })
        end)
        game:BindToMessage("Ping", function()
            print("a heard ping")
        end)
    )");
    project.write("src/client/hud.luau", R"(
        local SceneService = game:GetService("SceneService")
        local sent = { count = 3 }
        game:BindToMessage("CoinCollected", function(amount: number, t: { nested: { value: number } })
            print(`coin {amount} {t.nested.value}`)
        end)
        task.defer(function()
            scene:SendMessage("OpenGate", "north", sent)
            task.wait(0.1)
            print(`sender kept {sent.count}`)
            local ok, err = pcall(function()
                scene:SendMessage("Gate", { a = { b = function() end } })
            end)
            print(`refused {not ok} {err}`)
            game:SendMessage("Nobody")
            SceneService:LoadScene("scenes/b.scene.json")
        end)
        SceneService.SceneLoaded:Connect(function()
            -- Scene A's binding went with scene A.
            game:SendMessage("Ping")
        end)
    )");
    app::WorldHostOptions options = sceneOptions(project);
    options.developer = true;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(options).has_value());
    for (int tick = 0; tick < 30; ++tick)
        host.tick();

    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("gate north 3"));
    CHECK(log.contains("coin 1 5"));
    CHECK(log.contains("sender kept 3"));
    CHECK(log.contains("refused true"));
    CHECK(log.contains("argument 1.a.b"));
    CHECK(log.contains("Nobody listens to Nobody in game"));
    CHECK(host.world().engineState().currentScene == "scenes/b.scene.json");
    CHECK_FALSE(log.contains("a heard ping"));
    CHECK(log.contains("Nobody listens to Ping in game"));
}

// --- A scene prepared in the background (ADR 0125) -------------------------------

TEST_CASE("LoadSceneAsync prepares a scene while this one runs, and switches when the game says")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    project.write("src/scenes/b/client/mail.luau", R"(
        scene:BindToMessage("Hello", function(text: string)
            print(`b got {text}`)
        end)
    )");
    project.write("src/client/flow.luau", R"(
        local SceneService = game:GetService("SceneService")
        local loading = SceneService:LoadSceneAsync("scenes/b.scene.json", { Activate = false, Data = { Round = 7 } })
        print(`status {loading.Status.Name} progress {loading.Progress} open {loading.Scene:IsOpen()}`)
        loading.Ready:Connect(function()
            print(`ready {loading.Progress} {loading.Status.Name} still {SceneService.CurrentScene.Name}`)
            loading.Scene:SendMessage("Hello", "from the game")
            task.wait(0.1)
            print("activating")
            loading:Activate()
        end)
        loading.Finished:Connect(function(opened: boolean)
            print(`finished {opened} {loading.Status.Name} open {loading.Scene:IsOpen()}`)
        end)
        SceneService.SceneLoading:Connect(function(path: string)
            print(`loading event {path}`)
        end)
        SceneService.SceneLoaded:Connect(function(path: string)
            print(`loaded event {path} data {SceneService:GetLoadData().Round}`)
            local second = SceneService:LoadSceneAsync("scenes/a.scene.json", { Activate = false })
            local third = SceneService:LoadSceneAsync("scenes/missing.scene.json")
            print(`second {second.Status.Name}`)
            third.Finished:Connect(function(opened: boolean)
                print(`third {opened} {third.Status.Name} {third.Error ~= nil}`)
            end)
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(sceneOptions(project)).has_value());
    for (int tick = 0; tick < 3; ++tick)
        host.tick();
    CHECK(log.contains("status Preparing progress 0 open false"));
    CHECK(log.contains("ready 1 Ready still a"));
    // Prepared, not in: scene A goes on running until the game says.
    CHECK(host.world().engineState().currentScene == "scenes/a.scene.json");
    const int alive = occurrences(log, "A-alive");
    for (int tick = 0; tick < 3; ++tick)
        host.tick();
    CHECK(occurrences(log, "A-alive") > alive);
    CHECK_FALSE(log.contains("activating"));

    for (int tick = 0; tick < 12; ++tick)
        host.tick();
    // The one error is the missing scene's, which the last load asks for.
    const bool onlyTheMissingScene = log.firstError().empty() || log.firstError().find("missing") != std::string::npos;
    CHECK_MESSAGE(onlyTheMissingScene, log.firstError());
    CHECK(host.world().engineState().currentScene == "scenes/b.scene.json");
    const int activating = lineOf(log, "activating");
    const int loadingEvent = lineOf(log, "loading event scenes/b.scene.json");
    const int got = lineOf(log, "b got from the game");
    const int loaded = lineOf(log, "loaded event scenes/b.scene.json data 7");
    CHECK(activating >= 0);
    CHECK(loadingEvent > activating);
    // The message held for the prepared scene arrives after its scripts start
    // and before `SceneLoaded`.
    CHECK(got > loadingEvent);
    CHECK(loaded > got);
    CHECK(log.contains("finished true Done open true"));
    // A second load cancels the first; a scene that is not there fails.
    CHECK(log.contains("second Cancelled"));
    CHECK(log.contains("third false Failed true"));
    CHECK(host.world().engineState().currentScene == "scenes/b.scene.json");
}

TEST_CASE("a SceneLoad cancelled before it activates changes nothing")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    project.write("src/client/flow.luau", R"(
        local SceneService = game:GetService("SceneService")
        local loading = SceneService:LoadSceneAsync("scenes/b.scene.json", { Activate = false })
        loading.Finished:Connect(function(opened: boolean)
            print(`finished {opened} {loading.Status.Name}`)
        end)
        loading.Ready:Connect(function()
            loading:Cancel()
            local refused = not pcall(function()
                loading:Activate()
            end)
            print(`activate refused {refused}`)
        end)
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(sceneOptions(project)).has_value());
    for (int tick = 0; tick < 10; ++tick)
        host.tick();
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("finished false Cancelled"));
    CHECK(log.contains("activate refused true"));
    CHECK(host.world().engineState().currentScene == "scenes/a.scene.json");
}

// --- Preloading (ADR 0131 §3) ------------------------------------------------------

TEST_CASE("PreloadAsync waits for every item, reports each, and names what an instance holds")
{
    Captured log;
    Project project;
    project.write("content/models/crate.gltf", "{}");
    project.write("content/ui/logo.png", "not really a picture");
    project.write("src/client/preload.luau", R"(
        local ContentProvider = game:GetService("ContentProvider")
        local label = Instance.new("ImageLabel")
        label.Image = "asset://ui/logo.png"
        local statuses = {}
        ContentProvider:PreloadAsync({ "asset://models/crate.gltf", "asset://models/missing.gltf", label },
            function(item: string | Instance, status: Enum.AssetFetchStatus)
                local name = if typeof(item) == "string" then item else (item :: Instance).ClassName
                table.insert(statuses, `{name}={status.Name}`)
            end)
        table.sort(statuses)
        print(`preloaded {table.concat(statuses, " ")} queue {ContentProvider.RequestQueueSize}`)
        local refused = not pcall(function()
            ContentProvider:PreloadAsync({ 42 } :: any)
        end)
        print(`refused {refused}`)
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int tick = 0; tick < 3; ++tick)
        host.tick();
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("preloaded ImageLabel=Success asset://models/crate.gltf=Success "
                       "asset://models/missing.gltf=Failure queue 0"));
    CHECK(log.contains("refused true"));
}

// --- The display's rate (ADR 0136) --------------------------------------------------

namespace {

// Whether the script left a Folder of this name in the workspace.
[[nodiscard]] bool leftMark(app::WorldHost& host, std::string_view name)
{
    return host.world().findFirstChild(host.workspace(), host.world().atoms().lookup(name)).valid();
}

// The workspace's current camera's component.
[[nodiscard]] scene::CameraComponent& currentCamera(app::WorldHost& host)
{
    const scene::WorkspaceComponent* space = host.world().workspaces().find(host.workspace());
    REQUIRE(space != nullptr);
    scene::CameraComponent* camera = host.world().cameras().find(space->currentCamera);
    REQUIRE(camera != nullptr);
    return *camera;
}

} // namespace

TEST_CASE("render steps run by priority before PreRender, stop when unbound, and never without a frame (ADR 0136)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        local order = {}
        local function mark(name: string)
            local folder = Instance.new("Folder")
            folder.Name = name
            folder.Parent = workspace
        end
        RunService:BindToRenderStep("late", Enum.RenderPriority.Last.Value, function()
            table.insert(order, "late")
        end)
        RunService:BindToRenderStep("camera", Enum.RenderPriority.Camera.Value, function(dt: number)
            table.insert(order, if dt > 0 then "camera" else "camera?")
        end)
        -- Bound after, at the same priority: after it.
        RunService:BindToRenderStep("follow", Enum.RenderPriority.Camera.Value, function()
            table.insert(order, "follow")
        end)
        RunService:BindToRenderStep("first", Enum.RenderPriority.First.Value, function()
            table.insert(order, "first")
        end)
        RunService.PreRender:Connect(function()
            table.insert(order, "pre")
            mark("frame:" .. table.concat(order, ","))
            table.clear(order)
        end)
        local ticks = 0
        RunService.Heartbeat:Connect(function()
            ticks += 1
            if #order > 0 then
                mark("ran on a tick")
            end
            if ticks == 3 then
                RunService:UnbindFromRenderStep("camera")
                RunService:UnbindFromRenderStep("never bound")
            end
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    // Ticks with no frame drawn: a headless run. Nothing ran.
    for (int tick = 0; tick < 2; ++tick)
        host.tick();
    CHECK_FALSE(leftMark(host, "ran on a tick"));

    host.preRender(1.0 / 240.0);
    CHECK(leftMark(host, "frame:first,camera,follow,late,pre"));
    host.tick();
    host.preRender(1.0 / 240.0);
    CHECK(leftMark(host, "frame:first,follow,late,pre"));
    CHECK_FALSE(leftMark(host, "ran on a tick"));
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

TEST_CASE("a camera written in a render phase is drawn as written and is the next tick's, and the hash never sees it "
          "(ADR 0136)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.new(0, 10, 0)
        camera.Parent = workspace
        workspace.CurrentCamera = camera
        local function mark(name: string)
            local folder = Instance.new("Folder")
            folder.Name = name
            folder.Parent = workspace
        end
        local frames = 0
        local read = 0
        RunService:BindToRenderStep("camera", Enum.RenderPriority.Camera.Value, function()
            frames += 1
            camera.CFrame = CFrame.new(frames * 5, 10, 0)
            -- A render phase reads what it presented. Kept, and marked on the
            -- tick: a marker made here would itself be a change the hash sees.
            read = camera.CFrame.Position.x
        end)
        local ticks = 0
        RunService.Heartbeat:Connect(function()
            ticks += 1
            mark(`read presented {read}`)
            -- The simulation reads the presented camera from the next tick on.
            mark(`tick {ticks} sees {camera.CFrame.Position.x}`)
            if ticks == 4 then
                camera.CFrame = CFrame.new(-1, 10, 0)
            end
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();
    CHECK(leftMark(host, "tick 1 sees 0"));

    const core::u64 before = host.world().worldHash();
    host.preRender(1.0 / 240.0);
    // Presented: drawn as written, the simulated camera untouched, the hash too.
    scene::CameraComponent& camera = currentCamera(host);
    CHECK(camera.presenting);
    CHECK(camera.presented.position.x == doctest::Approx(5.0));
    CHECK(camera.cframe.position.x == doctest::Approx(0.0));
    CHECK(host.world().worldHash() == before);
    render::DrawPoses poses;
    render::TransformHistory history;
    history.capture(host.world());
    poses.begin(host.world(), &history, 0.5f);
    CHECK(poses.camera(host.world().workspaces().find(host.workspace())->currentCamera).position.x ==
          doctest::Approx(5.0));

    // Two frames before the next tick: it takes the last.
    host.preRender(1.0 / 240.0);
    host.tick();
    CHECK(leftMark(host, "read presented 10"));
    CHECK(leftMark(host, "tick 2 sees 10"));
    host.tick();
    CHECK(leftMark(host, "tick 3 sees 10"));

    // Written in a simulation phase: the simulation's at once, and no longer
    // presented -- drawn between ticks again.
    host.tick();
    CHECK(leftMark(host, "tick 4 sees 10"));
    CHECK_FALSE(currentCamera(host).presenting);
    CHECK(currentCamera(host).cframe.position.x == doctest::Approx(-1.0));
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

TEST_CASE("GetRenderCFrame is where a part is drawn in a render phase and its CFrame on a tick (ADR 0136)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        local part = Instance.new("Part")
        part.Name = "Mover"
        part.Anchored = true
        part.Position = vector.create(0, 0, 0)
        part.Parent = workspace
        local function mark(name: string)
            local folder = Instance.new("Folder")
            folder.Name = name
            folder.Parent = workspace
        end
        RunService.Heartbeat:Connect(function()
            part.Position += vector.create(1, 0, 0)
            if part:GetRenderCFrame() == part.CFrame then
                mark("a tick reads the CFrame")
            end
        end)
        RunService:BindToRenderStep("read", 0, function()
            mark(`drawn at {part:GetRenderCFrame().Position.x}, simulated at {part.CFrame.Position.x}`)
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    render::TransformHistory history;
    for (int tick = 0; tick < 3; ++tick) {
        history.capture(host.world());
        host.tick();
    }
    CHECK(leftMark(host, "a tick reads the CFrame"));
    // Half way between the last two ticks: 2.5, while the simulation is at 3.
    render::DrawPoses poses;
    poses.begin(host.world(), &history, 0.5f);
    host.preRender(1.0 / 240.0, &poses);
    CHECK(leftMark(host, "drawn at 2.5, simulated at 3"));
    // No poses -- a frame drawn at its tick -- answers the simulated place.
    host.preRender(1.0 / 240.0);
    CHECK(leftMark(host, "drawn at 3, simulated at 3"));
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

TEST_CASE(
    "the default camera turns in the frame the mouse moves and holds its subject still on screen at 240 Hz (ADR 0136)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        local cameraRigs = require("@engine/camera")
        local subject = Instance.new("Part")
        subject.Name = "Subject"
        subject.Anchored = true
        subject.Position = vector.create(0, 0, 0)
        subject.Parent = workspace
        local context = Instance.new("InputContext")
        context.Rate = Enum.InputRate.Render
        context.Parent = workspace
        local look = Instance.new("InputAction")
        look.Name = "Look"
        look.Type = Enum.InputActionType.Direction2D
        look.Parent = context
        local binding = Instance.new("InputBinding")
        binding.KeyCode = Enum.KeyCode.MouseMovement
        binding.Parent = look
        local rig = cameraRigs.thirdPerson({ Subject = subject, Smoothing = 1000 })
        rig.LookAction = look
        local ticks = 0
        RunService.Heartbeat:Connect(function(dt: number)
            ticks += 1
            -- 8 m/s, and a teleport on tick 30.
            subject.Position += vector.create(8 * dt, 0, 0)
            if ticks == 30 then
                subject.Position += vector.create(200, 0, 0)
            end
            rig:Update(dt)
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.world().engineState().pointerLocked = true;
    scene::World& world = host.world();
    const core::InstanceId subject = world.findFirstChild(host.workspace(), world.atoms().lookup("Subject"));
    REQUIRE(subject.valid());

    render::TransformHistory history;
    render::DrawPoses poses;
    const auto frameAt = [&](core::f32 alpha, core::Vec2 mouse) {
        input::DeviceState device;
        device.pointerDelta = mouse;
        host.input().setSnapshot(device);
        poses.begin(world, &history, alpha);
        host.preRender(1.0 / 240.0, &poses);
        poses.begin(world, &history, alpha);
    };
    // Where the subject sits in the camera's own space, as drawn.
    const auto onScreen = [&]() {
        const core::CFrameD eye = currentCamera(host).presented;
        return core::toVec3(core::transformPoint(core::inverse(eye), poses.part(subject).position));
    };

    // A still mouse, then one that moved: the camera turned in that frame.
    history.capture(world);
    host.tick();
    frameAt(0.0f, core::Vec2{});
    const core::Vec3 lookBefore =
        core::transformDirection(currentCamera(host).presented, core::Vec3{0.0f, 0.0f, -1.0f});
    frameAt(0.25f, core::Vec2{60.0f, 0.0f});
    const core::Vec3 lookAfter = core::transformDirection(currentCamera(host).presented, core::Vec3{0.0f, 0.0f, -1.0f});
    CHECK(std::abs(lookAfter.x - lookBefore.x) + std::abs(lookAfter.z - lookBefore.z) > 0.05);

    // 8 m/s, four frames a tick: once settled, the subject stays where it is
    // on the screen, within a centimetre at eleven metres -- about a pixel --
    // through the teleport, which the camera takes with it.
    std::optional<core::Vec3> held;
    core::f32 worst = 0.0f;
    for (int tick = 0; tick < 40; ++tick) {
        history.capture(world);
        host.tick();
        for (int frame = 0; frame < 4; ++frame) {
            frameAt(static_cast<core::f32>(frame) / 4.0f, core::Vec2{});
            if (tick < 5)
                continue;
            const core::Vec3 now = onScreen();
            if (!held.has_value())
                held = now;
            worst = std::max(worst, core::length(now - *held));
        }
    }
    CHECK(worst < 0.01f);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

// --- Clicked and prompted without code (ADR 0126) ---------------------------------

namespace {

// A pointer at `pixel` and whatever keys are held, as a device would give it.
void pointAt(app::WorldHost& host, core::Vec2 pixel, std::initializer_list<core::i32> held = {})
{
    input::DeviceState device;
    device.pointer = pixel;
    for (const core::i32 key : held)
        device.held[static_cast<core::usize>(key)] = true;
    host.input().setSnapshot(device);
}

constexpr core::i32 KeyE = 18;
constexpr core::i32 KeyMouseLeft = 67;

} // namespace

TEST_CASE("a ClickDetector is hovered and clicked through the pointer, and not out of reach")
{
    Captured log;
    Project project;
    project.write("src/client/clicks.luau", R"(
        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.new(0, 0, 0)
        camera.Parent = workspace
        workspace.CurrentCamera = camera
        local function button(name: string, z: number, reach: number)
            local part = Instance.new("Part")
            part.Name = name
            part.Anchored = true
            part.Size = Vector3.new(4, 4, 1)
            part.CFrame = CFrame.new(0, 0, z)
            part.Parent = workspace
            local detector = Instance.new("ClickDetector")
            detector.MaxActivationDistance = reach
            detector.Parent = part
            detector.MouseClick:Connect(function(player: Player)
                print(`{name} clicked by player {player.UserId}`)
            end)
            detector.MouseHoverEnter:Connect(function()
                print(`{name} hovered`)
            end)
            return part
        end
        local near = button("Near", -10, 32)
        task.delay(0.2, function()
            near:Destroy()
            button("Far", -40, 32)
        end)
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.world().engineState().viewportSize = core::Vec2{800.0f, 600.0f};
    const core::Vec2 centre{400.0f, 300.0f};

    pointAt(host, centre);
    for (int tick = 0; tick < 3; ++tick)
        host.tick();
    pointAt(host, centre, {KeyMouseLeft});
    host.tick();
    pointAt(host, centre);
    host.tick();
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("Near hovered"));
    CHECK(log.contains("Near clicked by player 1"));

    // Forty metres away is past its reach: neither hovered nor clicked.
    for (int tick = 0; tick < 15; ++tick)
        host.tick();
    pointAt(host, centre, {KeyMouseLeft});
    host.tick();
    pointAt(host, centre);
    host.tick();
    CHECK_FALSE(log.contains("Far hovered"));
    CHECK_FALSE(log.contains("Far clicked"));
}

TEST_CASE("a held ProximityPrompt triggers after its duration, and one per key shows")
{
    Captured log;
    Project project;
    project.write("src/client/prompts.luau", R"(
        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.new(0, 0, 0)
        camera.Parent = workspace
        workspace.CurrentCamera = camera
        local function prompt(name: string, z: number, hold: number)
            local part = Instance.new("Part")
            part.Name = name
            part.Anchored = true
            part.CFrame = CFrame.new(0, 0, z)
            part.Parent = workspace
            local p = Instance.new("ProximityPrompt")
            p.HoldDuration = hold
            p.Parent = part
            p.PromptShown:Connect(function(inputType: Enum.ProximityPromptInputType)
                print(`{name} shown for {inputType.Name}`)
            end)
            p.PromptButtonHoldBegan:Connect(function()
                print(`{name} hold began`)
            end)
            p.Triggered:Connect(function(player: Player)
                print(`{name} triggered by player {player.UserId}`)
            end)
            p.TriggerEnded:Connect(function()
                print(`{name} ended`)
            end)
            return p
        end
        prompt("Door", -3, 0.25)
        prompt("Crate", -6, 0)
        -- A character where the camera is: reach and sight are measured from
        -- inside it, so it must not hide the prompts from itself.
        local body = Instance.new("Part")
        body.Anchored = true
        body.Size = Vector3.new(1, 2, 1)
        body.Parent = workspace
        local me = game:GetService("NetworkService").LocalPlayer
        if me then
            me.Character = body
        end
        game:GetService("ProximityPromptService").PromptTriggered:Connect(function(p: ProximityPrompt)
            print(`service saw {(p.Parent :: Instance).Name}`)
        end)
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.world().engineState().viewportSize = core::Vec2{800.0f, 600.0f};

    pointAt(host, core::Vec2{400.0f, 300.0f});
    for (int tick = 0; tick < 3; ++tick)
        host.tick();
    // Both on E, so only the nearer shows.
    CHECK(log.contains("Door shown for Keyboard"));
    CHECK_FALSE(log.contains("Crate shown"));

    pointAt(host, core::Vec2{400.0f, 300.0f}, {KeyE});
    for (int tick = 0; tick < 5; ++tick)
        host.tick();
    CHECK(log.contains("Door hold began"));
    CHECK_FALSE(log.contains("Door triggered"));
    for (int tick = 0; tick < 15; ++tick)
        host.tick();
    CHECK(log.contains("Door triggered by player 1"));
    CHECK(log.contains("service saw Door"));
    pointAt(host, core::Vec2{400.0f, 300.0f});
    for (int tick = 0; tick < 2; ++tick)
        host.tick();
    CHECK(log.contains("Door ended"));
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

TEST_CASE("the authority fires a client's click with its player only within reach of the character")
{
    Captured log;
    Project project;
    project.write("src/server/check.luau", R"(
        local part = Instance.new("Part")
        part.Name = "Lever"
        part.Anchored = true
        part.CFrame = CFrame.new(100, 0, 0)
        part.Parent = workspace
        local detector = Instance.new("ClickDetector")
        detector.MaxActivationDistance = 10
        detector.Parent = part
        detector.MouseClick:Connect(function(player: Player)
            print(`lever pulled by player {player.UserId}`)
        end)
        local body = Instance.new("Part")
        body.Name = "Body"
        body.Anchored = true
        body.Parent = workspace
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();
    scene::World& world = host.world();
    const core::InstanceId player = scene::localPlayerOf(world);
    const core::InstanceId body = world.findFirstChild(host.workspace(), world.atoms().lookup("Body"));
    const core::InstanceId lever = world.findFirstChild(host.workspace(), world.atoms().lookup("Lever"));
    REQUIRE(player.valid());
    REQUIRE(body.valid());
    REQUIRE(lever.valid());
    world.players().find(player)->character = body;
    const core::InstanceId detector = world.firstChild(lever);

    // Forged from far away: the body is a hundred metres off.
    world.engineState().detectorInbox.push_back(
        scene::DetectorMessage{detector, player, scene::DetectorMessage::Kind::Click, 0});
    host.tick();
    CHECK_FALSE(log.contains("lever pulled"));

    world.parts().find(body)->cframe.position = core::DVec3{96.0, 0.0, 0.0};
    world.engineState().detectorInbox.push_back(
        scene::DetectorMessage{detector, player, scene::DetectorMessage::Kind::Click, 0});
    host.tick();
    CHECK(log.contains("lever pulled by player 1"));
}

// --- Dragged without code (ADR 0126 §3) --------------------------------------------

namespace {

[[nodiscard]] core::CFrameD frameOf(app::WorldHost& host, std::string_view name)
{
    scene::World& world = host.world();
    const core::InstanceId id = world.findFirstChild(host.workspace(), world.atoms().lookup(name));
    REQUIRE(id.valid());
    return world.parts().find(id)->cframe;
}

// The pointer pressed at `from`, walked to `to` over `steps` ticks, and let go.
void dragAcross(app::WorldHost& host, core::Vec2 from, core::Vec2 to, int steps)
{
    pointAt(host, from);
    host.tick();
    pointAt(host, from, {KeyMouseLeft});
    host.tick();
    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        pointAt(host, core::Vec2{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t}, {KeyMouseLeft});
        host.tick();
    }
    pointAt(host, to);
    host.tick();
}

} // namespace

TEST_CASE("a drawer drags along its line, within its limits from where it rested (ADR 0126 §3)")
{
    Captured log;
    Project project;
    project.write("src/client/drawer.luau", R"(
        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.new(0, 0, 0)
        camera.Parent = workspace
        workspace.CurrentCamera = camera
        local drawer = Instance.new("Part")
        drawer.Name = "Drawer"
        drawer.Anchored = true
        drawer.Size = Vector3.new(2, 1, 1)
        drawer.CFrame = CFrame.new(0, 0, -10)
        drawer.Parent = workspace
        local drag = Instance.new("DragDetector")
        drag.DragStyle = Enum.DragDetectorDragStyle.TranslateLine
        drag.Axis = vector.create(1, 0, 0)
        drag.MinDragTranslation = 0
        drag.MaxDragTranslation = 3
        drag.Parent = drawer
        drag.DragStart:Connect(function(player: Player, _origin: vector, _direction: vector, hit: vector)
            print(`drag began by player {player.UserId} at z {math.floor(hit.z * 10 + 0.5) / 10}`)
        end)
        drag.DragEnd:Connect(function(player: Player)
            print(`drag ended by player {player.UserId}`)
        end)
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.world().engineState().viewportSize = core::Vec2{800.0f, 600.0f};

    // A little to the right: the drawer follows the pointer along x.
    dragAcross(host, core::Vec2{400.0f, 300.0f}, core::Vec2{480.0f, 300.0f}, 4);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("drag began by player 1 at z -9.5"));
    CHECK(log.contains("drag ended by player 1"));
    const double moved = frameOf(host, "Drawer").position.x;
    CHECK(moved > 1.0);
    CHECK(moved < 3.0);
    CHECK(frameOf(host, "Drawer").position.z == doctest::Approx(-10.0));

    // Far to the right: held at three metres out.
    const core::Vec2 at{400.0f + static_cast<float>(moved) * 40.0f, 300.0f};
    dragAcross(host, at, core::Vec2{790.0f, 300.0f}, 4);
    CHECK(frameOf(host, "Drawer").position.x == doctest::Approx(3.0).epsilon(0.01));

    // And far back to the left: held where it rested, not three metres short
    // of where this drag began.
    dragAcross(host, core::Vec2{400.0f + 3.0f * 40.0f, 300.0f}, core::Vec2{10.0f, 300.0f}, 4);
    CHECK(frameOf(host, "Drawer").position.x == doctest::Approx(0.0).epsilon(0.01));
}

TEST_CASE("a lever turns about its axis, and no further than its angles (ADR 0126 §3)")
{
    Captured log;
    Project project;
    project.write("src/client/lever.luau", R"(
        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.new(0, 0, 0)
        camera.Parent = workspace
        workspace.CurrentCamera = camera
        local lever = Instance.new("Part")
        lever.Name = "Lever"
        lever.Anchored = true
        lever.Size = Vector3.new(0.4, 3, 0.4)
        lever.CFrame = CFrame.new(0, 0, -10)
        lever.Parent = workspace
        local drag = Instance.new("DragDetector")
        drag.DragStyle = Enum.DragDetectorDragStyle.RotateAxis
        drag.Axis = vector.create(0, 0, 1)
        drag.MinDragAngle = -45
        drag.MaxDragAngle = 45
        drag.Parent = lever
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.world().engineState().viewportSize = core::Vec2{800.0f, 600.0f};

    // Taken near its top and pulled far to the right: turned clockwise, as
    // far as forty-five degrees.
    dragAcross(host, core::Vec2{400.0f, 250.0f}, core::Vec2{790.0f, 250.0f}, 6);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    const core::CFrameD turned = frameOf(host, "Lever");
    const core::Vec3 up = turned.rotation * core::Vec3{0.0f, 1.0f, 0.0f};
    CHECK(up.x == doctest::Approx(std::sqrt(0.5)).epsilon(0.02));
    CHECK(up.y == doctest::Approx(std::sqrt(0.5)).epsilon(0.02));
    // Turned about its middle: it stays where it is.
    CHECK(turned.position.x == doctest::Approx(0.0));
}

TEST_CASE("the authority begins a client's drag only within reach, moves it itself, and hands a pulled crate over")
{
    Captured log;
    Project project;
    project.write("src/server/drags.luau", R"(
        local function thing(name: string, x: number, anchored: boolean, response: Enum.DragDetectorResponseStyle)
            local part = Instance.new("Part")
            part.Name = name
            part.Anchored = anchored
            part.CFrame = CFrame.new(x, 0, 0)
            part.Parent = workspace
            local drag = Instance.new("DragDetector")
            drag.DragStyle = Enum.DragDetectorDragStyle.TranslateLine
            drag.Axis = vector.create(0, 0, 1)
            drag.ResponseStyle = response
            drag.MaxActivationDistance = 10
            drag.Parent = part
            drag.DragStart:Connect(function(player: Player)
                print(`{name} taken by player {player.UserId}`)
            end)
            drag.DragEnd:Connect(function(player: Player)
                print(`{name} let go by player {player.UserId}`)
            end)
        end
        thing("Slider", 100, true, Enum.DragDetectorResponseStyle.Geometric)
        thing("Crate", 104, false, Enum.DragDetectorResponseStyle.Physical)
        local body = Instance.new("Part")
        body.Name = "Body"
        body.Anchored = true
        body.Parent = workspace
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();
    scene::World& world = host.world();
    const core::InstanceId body = world.findFirstChild(host.workspace(), world.atoms().lookup("Body"));
    const core::InstanceId slider = world.findFirstChild(host.workspace(), world.atoms().lookup("Slider"));
    const core::InstanceId crate = world.findFirstChild(host.workspace(), world.atoms().lookup("Crate"));
    REQUIRE(body.valid());
    REQUIRE(slider.valid());
    REQUIRE(crate.valid());
    // A player across the network, whose character is the body.
    const core::InstanceId player = world.create(world.classes().findId(world.atoms().intern("Player")));
    REQUIRE(player.valid());
    scene::PlayerComponent* remote = world.players().find(player);
    REQUIRE(remote != nullptr);
    remote->userId = 7;
    remote->local = false;
    remote->character = body;
    const core::InstanceId sliding = world.firstChild(slider);
    const core::InstanceId pulling = world.firstChild(crate);

    const auto send = [&](core::InstanceId detector, scene::DetectorMessage::Kind kind, core::DVec3 origin,
                          core::Vec3 direction, core::DVec3 hit) {
        scene::DetectorMessage message{detector, player, kind, 0};
        message.origin = origin;
        message.direction = direction;
        message.hit = hit;
        world.engineState().detectorInbox.push_back(message);
    };

    // From a hundred metres off: refused.
    world.parts().find(body)->cframe.position = core::DVec3{0.0, 0.0, 0.0};
    send(sliding, scene::DetectorMessage::Kind::DragStart, core::DVec3{100.0, 5.0, 0.0}, core::Vec3{0.0f, -1.0f, 0.0f},
         core::DVec3{100.0, 0.5, 0.0});
    host.tick();
    CHECK_FALSE(log.contains("Slider taken"));

    // Beside it: taken, and moved by the authority from the rays it is sent --
    // the ray straight down onto the line two metres along it.
    world.parts().find(body)->cframe.position = core::DVec3{97.0, 0.0, 0.0};
    send(sliding, scene::DetectorMessage::Kind::DragStart, core::DVec3{100.0, 5.0, 0.0}, core::Vec3{0.0f, -1.0f, 0.0f},
         core::DVec3{100.0, 0.5, 0.0});
    host.tick();
    CHECK(log.contains("Slider taken by player 7"));
    send(sliding, scene::DetectorMessage::Kind::DragContinue, core::DVec3{100.0, 5.0, 2.0},
         core::Vec3{0.0f, -1.0f, 0.0f}, core::DVec3{});
    host.tick();
    CHECK(world.parts().find(slider)->cframe.position.z == doctest::Approx(2.0).epsilon(0.01));
    send(sliding, scene::DetectorMessage::Kind::DragEnd, core::DVec3{}, core::Vec3{}, core::DVec3{});
    host.tick();
    CHECK(log.contains("Slider let go by player 7"));

    // A crate pulled is the player's while it is pulled, and the authority's
    // after (ADR 0099).
    world.parts().find(body)->cframe.position = core::DVec3{101.0, 0.0, 0.0};
    send(pulling, scene::DetectorMessage::Kind::DragStart, core::DVec3{104.0, 5.0, 0.0}, core::Vec3{0.0f, -1.0f, 0.0f},
         core::DVec3{104.0, 0.5, 0.0});
    host.tick();
    CHECK(log.contains("Crate taken by player 7"));
    CHECK(world.rigidBodies().find(crate)->networkOwner == 7u);
    send(pulling, scene::DetectorMessage::Kind::DragEnd, core::DVec3{}, core::Vec3{}, core::DVec3{});
    host.tick();
    CHECK(world.rigidBodies().find(crate)->networkOwner == 0u);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

// --- Water (ADR 0118) -----------------------------------------------------------------

TEST_CASE("a box of half the water's density floats half under, and a pushed plank rolls and rights itself (ADR 0118)")
{
    Captured log;
    Project project;
    project.write("src/server/float.luau", R"(
        local water = Instance.new("Water")
        water.Parent = workspace
        local function block(name: string, size: vector, at: vector): Part
            local part = Instance.new("Part")
            part.Name = name
            part.Size = size
            part.Density = 0.5
            part.Anchored = false
            part.Position = at
            part.Parent = workspace
            return part
        end
        block("Box", vector.create(2, 2, 2), vector.create(0, 3, 0))
        block("Plank", vector.create(6, 1, 2), vector.create(20, 1, 0))
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int tick = 0; tick < 60 * 8; ++tick)
        host.tick();
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    scene::World& world = host.world();
    const auto partNamed = [&](std::string_view name) {
        const core::InstanceId id = world.findFirstChild(host.workspace(), world.atoms().lookup(name));
        REQUIRE(id.valid());
        return id;
    };
    // Half under: its middle at the surface.
    CHECK(world.parts().find(partNamed("Box"))->cframe.position.y == doctest::Approx(0.0).epsilon(0.15));

    // Pushed down at one end: it rolls...
    const core::InstanceId plank = partNamed("Plank");
    const core::DVec3 at = world.parts().find(plank)->cframe.position;
    scene::RigidBodyComponent& body = *world.rigidBodies().find(plank);
    body.pendingImpulse = body.pendingImpulse + core::Vec3{0.0f, -4.0f, 0.0f};
    body.pendingAngularImpulse =
        body.pendingAngularImpulse + core::cross(core::Vec3{2.5f, 0.0f, 0.0f}, core::Vec3{0.0f, -4.0f, 0.0f});
    for (int tick = 0; tick < 10; ++tick)
        host.tick();
    const core::Vec3 rolled = world.parts().find(plank)->cframe.rotation * core::Vec3{0.0f, 1.0f, 0.0f};
    CHECK(std::abs(rolled.x) > 0.02f);
    // ...and the water rights it.
    for (int tick = 0; tick < 60 * 6; ++tick)
        host.tick();
    const core::Vec3 upright = world.parts().find(plank)->cframe.rotation * core::Vec3{0.0f, 1.0f, 0.0f};
    CHECK(upright.y > 0.99f);
    CHECK(std::abs(world.parts().find(plank)->cframe.position.x - at.x) < 3.0);
}

// --- Paths from scripts stay in the project (audit F5) ----------------------------

TEST_CASE("require and LoadScene refuse a path that is not a path under the project (audit F5)")
{
    Captured log;
    Project project;
    writeTwoScenes(project);
    // A module a backslash spelling would reach on Windows, where the old check
    // saw `src\shared\Secret` as one harmless segment.
    project.write("src/shared/Secret.luau", "return 'the module'");
    project.write("src/client/escape.luau", R"(
        local SceneService = game:GetService("SceneService")
        for _, name in { "src\\shared\\Secret", "C:/Windows/win", "//host/share/x" } do
            local ok = pcall(require, name)
            print(`require {name} refused {not ok}`)
        end
        for _, path in { "C:/Windows/win.ini", "../outside.scene.json", "scenes\\b.scene.json", "//host/x.json" } do
            local ok = pcall(function()
                SceneService:LoadScene(path)
            end)
            print(`scene {path} refused {not ok}`)
        end
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(sceneOptions(project)).has_value());
    host.tick();
    CHECK(log.contains("require src\\shared\\Secret refused true"));
    CHECK(log.contains("require C:/Windows/win refused true"));
    CHECK(log.contains("require //host/share/x refused true"));
    CHECK(log.contains("scene C:/Windows/win.ini refused true"));
    CHECK(log.contains("scene ../outside.scene.json refused true"));
    CHECK(log.contains("scene scenes\\b.scene.json refused true"));
    CHECK(log.contains("scene //host/x.json refused true"));

    // The host refuses it too, for a path that did not come through a script --
    // an authority's scene a replica follows.
    CHECK(host.loadScene("..\\..\\x.scene.json").has_value());
    CHECK(host.world().engineState().currentScene == "scenes/a.scene.json");
}

TEST_CASE("a block size that narrows to nothing is refused, from a file and from a script (audit F9)")
{
    // 1e-300 is positive as a double and zero as the float it becomes; 1e300
    // becomes an infinity. Every loop over a block's metres divides by it.
    Captured log;
    Project project;
    project.write("content/scenes/a.scene.json",
                  R"json({"format":"scene","version":2,"root":{},"storage":{},"voxels":{"blockSize":1e-300}})json");
    project.write("src/client/try.luau", R"(
        local VoxelService = game:GetService("VoxelService")
        print(`read:{VoxelService.BlockSize}`)
        local tiny = pcall(function()
            VoxelService.BlockSize = 1e-300
        end)
        local huge = pcall(function()
            VoxelService.BlockSize = 1e300
        end)
        print(`tiny:{tiny} huge:{huge} now:{VoxelService.BlockSize}`)
    )");
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "a.scene.json";
    options.bootScenePath = "scenes/a.scene.json";
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(options).has_value());
    host.tick();
    CHECK(log.contains("read:1"));
    CHECK(log.contains("tiny:false huge:false now:1"));
}
