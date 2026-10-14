// `SubWorld` (ADR 0107 §3), from the host's side: what a sub-world mounts, that
// its trace is its own, and that unloading one gives it back. The script
// surface -- messages, input, the budget -- is `world/sub_world.spec.luau`'s.

#include <doctest/doctest.h>
#include <string>

#include "engine/app/world_host.h"
#include "engine/scene/components.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::hasChildNamed;
using engine::app::testing::Project;

namespace {

// A scene whose world moves under its own physics: a block that falls.
constexpr std::string_view ArcadeScene = R"({
 "format": "scene",
 "version": 2,
 "root": {
  "class": "Workspace",
  "name": "Workspace",
  "children": [
   {"class": "Part", "name": "Block", "properties": {"CFrame": [0, 20, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]}},
   {"class": "Part", "name": "Floor", "properties": {"Anchored": true, "Size": [40, 1, 40]}}
  ]
 }
})";

// The same scene with the block somewhere else: another sub-world, the same
// host.
constexpr std::string_view OtherScene = R"({
 "format": "scene",
 "version": 2,
 "root": {
  "class": "Workspace",
  "name": "Workspace",
  "children": [
   {"class": "Part", "name": "Block", "properties": {"CFrame": [3, 30, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]}},
   {"class": "Part", "name": "Floor", "properties": {"Anchored": true, "Size": [40, 1, 40]}}
  ]
 }
})";

// The host game: a marker of its own, and a cabinet it switches on.
constexpr std::string_view HostScript = R"(
    local marker = Instance.new("Folder")
    marker.Name = "GlobalRan"
    marker.Parent = workspace

    local cabinet = Instance.new("SubWorld")
    cabinet.Name = "Cabinet"
    cabinet.Scene = "scenes/arcade.scene.json"
    cabinet.Parent = workspace
    cabinet:Load()
)";

void writeProject(const Project& project, std::string_view scene)
{
    project.write("content/scenes/arcade.scene.json", scene);
    project.write("src/client/main.luau", HostScript);
    // The scene's own code, which the sub-world runs as a game played alone.
    project.write("src/scenes/arcade/server/boot.luau", R"(
        local marker = Instance.new("Folder")
        marker.Name = "SceneCodeRan"
        marker.Parent = workspace
    )");
}

[[nodiscard]] core::InstanceId cabinetOf(app::WorldHost& host)
{
    return host.world().findFirstChild(host.workspace(), host.world().atoms().lookup("Cabinet"));
}

struct Hashes
{
    core::u64 host = 0;
    core::u64 inner = 0;
};

[[nodiscard]] Hashes run(std::string_view scene)
{
    Project project;
    writeProject(project, scene);
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    for (int tick = 0; tick < 90; ++tick)
        host.tick();
    app::WorldHost* inner = host.subWorld(cabinetOf(host));
    REQUIRE(inner != nullptr);
    return Hashes{host.world().worldHash(), inner->world().worldHash()};
}

} // namespace

TEST_CASE("a sub-world runs its scene's own code and none of the game's (ADR 0107)")
{
    Captured log;
    Project project;
    writeProject(project, ArcadeScene);

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    // Booted at the end of the tick `Load()` was called in -- the boot drain
    // is not a tick, so the first one.
    CHECK(host.subWorlds().empty());
    host.tick();
    REQUIRE(host.subWorlds().size() == 1);
    app::WorldHost* inner = host.subWorld(cabinetOf(host));
    REQUIRE(inner != nullptr);

    CHECK(hasChildNamed(host, "GlobalRan"));
    CHECK(hasChildNamed(*inner, "SceneCodeRan"));
    CHECK_FALSE(hasChildNamed(*inner, "GlobalRan"));
    CHECK_FALSE(hasChildNamed(host, "SceneCodeRan"));
    CHECK(inner->world().engineState().subWorld);

    // One tick of its own for each of the host's, and it falls under its own
    // physics.
    const core::u64 before = inner->world().engineState().tick;
    for (int tick = 0; tick < 30; ++tick)
        host.tick();
    CHECK(inner->world().engineState().tick == before + 30);
    const core::InstanceId block =
        inner->world().findFirstChild(inner->workspace(), inner->world().atoms().lookup("Block"));
    REQUIRE(block.valid());
    CHECK(inner->world().parts().find(block)->cframe.position.y < 20.0);
    CHECK(log.firstError().empty());
}

TEST_CASE("a sub-world's trace is its own, and the host's does not move with it (ADR 0107)")
{
    Captured log;
    const Hashes first = run(ArcadeScene);
    const Hashes again = run(ArcadeScene);
    const Hashes other = run(OtherScene);

    // The same run twice is the same two worlds.
    CHECK(first.host == again.host);
    CHECK(first.inner == again.inner);
    // A different sub-world beside the same host: the sub-world moved, and
    // nothing of it reached the host's trace.
    CHECK(first.inner != other.inner);
    CHECK(first.host == other.host);
    CHECK(log.firstError().empty());
}

TEST_CASE("unloading a sub-world, or destroying it, throws its world away (ADR 0107)")
{
    Captured log;
    Project project;
    writeProject(project, ArcadeScene);

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();
    REQUIRE(host.subWorlds().size() == 1);
    const core::u64 serial = host.subWorlds()[0].serial;

    // `Unload()` is what a script calls; the host's half is the wanted list.
    scene::EngineState& state = host.world().engineState();
    state.subWorldsWanted.clear();
    host.tick();
    CHECK(host.subWorlds().empty());
    CHECK(state.subWorldsLoaded.empty());

    // Loaded again, it is a new world with a new serial, from the start.
    state.subWorldsWanted.push_back(cabinetOf(host));
    host.tick();
    REQUIRE(host.subWorlds().size() == 1);
    CHECK(host.subWorlds()[0].serial != serial);

    // Destroyed, it is unloaded at the next tick.
    (void)host.world().destroy(cabinetOf(host));
    host.tick();
    CHECK(host.subWorlds().empty());
    CHECK(log.firstError().empty());
}

TEST_CASE("a sub-world on a dedicated server runs its server code and none of its client code (ADR 0137 §6)")
{
    // A sub-world always booted solo, so on a server with no display it ran its
    // scene's client code -- a HUD for nobody.
    Captured log;
    Project project;
    project.write("content/scenes/arcade.scene.json", ArcadeScene);
    // On a dedicated server the game's code is its server code.
    project.write("src/server/main.luau", HostScript);
    project.write("src/scenes/arcade/server/boot.luau", R"(
        local marker = Instance.new("Folder")
        marker.Name = "SceneCodeRan"
        marker.Parent = workspace
    )");
    project.write("src/scenes/arcade/client/hud.luau", R"(
        local marker = Instance.new("Folder")
        marker.Name = "SceneClientRan"
        marker.Parent = workspace
    )");

    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.networkTopology = scene::NetworkTopology::Dedicated;
    REQUIRE_FALSE(host.boot(options).has_value());
    host.tick();
    host.tick();
    app::WorldHost* inner = host.subWorld(cabinetOf(host));
    REQUIRE(inner != nullptr);
    CHECK(hasChildNamed(*inner, "SceneCodeRan"));
    CHECK_FALSE(hasChildNamed(*inner, "SceneClientRan"));
}
