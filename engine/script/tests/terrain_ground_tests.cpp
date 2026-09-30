// **Ground on disk, read before it is read or written** (terrain audit TA16).
//
// A streamed terrain holds in memory only the cells round whoever is looking;
// the rest is on disk. Every verb that writes the ground asked for its square
// first (U1) -- but only 256 cells of it, and none of the verbs that READ did,
// so the documented `ReadVoxels` -> `WriteVoxels` copy over ground not loaded
// read air and wrote it back. Judged here by what the world was asked to load.
#include <doctest/doctest.h>
#include <ostream>
#include <vector>

#include "engine/scene/components.h"
#include "script_fixture.h"

using engine::script::testing::Fixture;
using namespace engine;

namespace {

struct Square
{
    core::DVec3 low;
    core::DVec3 high;
};

[[nodiscard]] bool covers(const std::vector<Square>& asked, double x, double z)
{
    for (const Square& square : asked) {
        if (square.low.x <= x && x <= square.high.x && square.low.z <= z && z <= square.high.z)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("reading the ground asks for it first, as writing it does (TA16e)")
{
    Fixture fixture;
    REQUIRE(fixture.booted);
    std::vector<Square> asked;
    fixture.world->setGroundLoader([&asked](core::DVec3 low, core::DVec3 high, core::u32) {
        asked.push_back(Square{low, high});
        return true;
    });
    const std::string made = fixture.failure(R"(
        local terrain = Instance.new("Terrain")
        terrain.Parent = workspace
    )");
    INFO(made);
    REQUIRE(made.empty());

    const auto after = [&](const char* source) {
        asked.clear();
        CHECK(fixture.failure(source).empty());
        return asked;
    };
    CHECK(covers(after("workspace:FindFirstChildOfClass(\"Terrain\"):ReadVoxels(vector.create(400, 0, 400), "
                       "vector.create(404, 4, 404))"),
                 402.0, 402.0));
    CHECK(covers(after("local _ = workspace:FindFirstChildOfClass(\"Terrain\"):HeightAt(-300, 250)"), -300.0, 250.0));
    CHECK(covers(after("workspace:FindFirstChildOfClass(\"Terrain\"):ApplyRules(vector.create(100, -8, 100), "
                       "vector.create(120, 8, 120))"),
                 110.0, 110.0));
    CHECK(covers(after("local _ = workspace:Raycast(vector.create(600, 50, 600), vector.create(0, -100, 0))"), 600.0,
                 600.0));
}

TEST_CASE("an edit reaching more ground than can be loaded at once is refused, and changes nothing (TA16a)")
{
    Fixture fixture;
    REQUIRE(fixture.booted);
    fixture.world->setGroundLoader([](core::DVec3, core::DVec3, core::u32) { return false; });
    CHECK(fixture.raises(R"(
        local terrain = Instance.new("Terrain")
        terrain.Parent = workspace
        terrain:FillBlock(vector.create(0, 0, 0), vector.create(8, 8, 8), 1)
    )",
                         "scene.err.terrain_ground_too_wide"));
    CHECK(fixture.failure("assert(workspace:FindFirstChildOfClass(\"Terrain\").CellCount == 0)").empty());
}

TEST_CASE("Clear lets go of the terrain's cells on disk, as the editor's Clear does (TA16c)")
{
    // Cleared in memory only, every cell not loaded streamed back in.
    Fixture fixture;
    REQUIRE(fixture.booted);
    REQUIRE(fixture
                .failure(R"(
        local terrain = Instance.new("Terrain")
        terrain.Parent = workspace
    )")
                .empty());
    scene::TerrainComponent* component = nullptr;
    fixture.world->terrains().forEach(
        [&component](core::InstanceId, scene::TerrainComponent& terrain) { component = &terrain; });
    REQUIRE(component != nullptr);
    component->cellIndex = "terrain/scenes/main.scene/index.json";
    CHECK(fixture.failure("workspace:FindFirstChildOfClass(\"Terrain\"):Clear()").empty());
    CHECK(component->cellIndex.empty());
}
