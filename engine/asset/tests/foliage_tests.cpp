// Where foliage grows (ADR 0116): a pure function of the ground, the rules and
// the seed.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/asset/foliage.h"
#include "engine/asset/terrain_mesher.h"

using namespace engine;
using namespace engine::asset;

namespace {

// A tile of flat ground at `height`, one chunk column of 1 m voxels.
[[nodiscard]] TerrainMesh flatTile(float height)
{
    TerrainField field(FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 128.0f, height, 1);
    return meshField(field, MeshRegion{.minX = 0, .minY = 0, .minZ = 0, .cellsX = 32, .cellsY = 32, .cellsZ = 32});
}

[[nodiscard]] FoliageRules grass(core::u32 seed = 7)
{
    FoliageRules rules;
    rules.density = 2.0f;
    rules.minSpacing = 0.0f;
    rules.seed = seed;
    rules.meshWeights = {1.0f, 3.0f};
    return rules;
}

[[nodiscard]] bool sameInstances(const FoliageTile& a, const FoliageTile& b)
{
    if (a.instances.size() != b.instances.size() || a.meshStart != b.meshStart)
        return false;
    for (std::size_t at = 0; at < a.instances.size(); ++at) {
        const FoliageInstance& x = a.instances[at];
        const FoliageInstance& y = b.instances[at];
        if (x.position.x != y.position.x || x.position.y != y.position.y || x.position.z != y.position.z ||
            x.yaw != y.yaw || x.random != y.random)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("the same ground and seed grow the same field, and another seed another")
{
    const TerrainMesh ground = flatTile(4.0f);
    const FoliageTile first = growFoliage(ground, 0.0f, {}, grass(), 3, -2);
    const FoliageTile again = growFoliage(ground, 0.0f, {}, grass(), 3, -2);
    REQUIRE_FALSE(first.instances.empty());
    CHECK(sameInstances(first, again));

    CHECK_FALSE(sameInstances(first, growFoliage(ground, 0.0f, {}, grass(8), 3, -2)));
    // The tile is in the seed too: two tiles of the same ground are not copies.
    CHECK_FALSE(sameInstances(first, growFoliage(ground, 0.0f, {}, grass(), 4, -2)));
}

TEST_CASE("a tile grows about its density on flat ground, split between meshes by weight")
{
    const FoliageTile tile = growFoliage(flatTile(4.0f), 0.0f, {}, grass(), 0, 0);
    // 32 by 32 metres at two a square metre, give or take the rounding of each
    // triangle's share.
    const double count = static_cast<double>(tile.instances.size());
    CHECK(count > 2048.0 * 0.9);
    CHECK(count < 2048.0 * 1.1);
    REQUIRE(tile.meshStart.size() == 3);
    const double firstShare = static_cast<double>(tile.meshStart[1] - tile.meshStart[0]) / count;
    CHECK(firstShare > 0.2);
    CHECK(firstShare < 0.3);
    for (const FoliageInstance& instance : tile.instances) {
        CHECK(std::abs(instance.position.y - 4.0f) < 0.08f);
        CHECK(instance.random >= 0.0f);
        CHECK(instance.random < 1.0f);
    }
}

TEST_CASE("the rules keep it off the wrong slope, height and material")
{
    const TerrainMesh ground = flatTile(4.0f);

    FoliageRules steep = grass();
    steep.slopeMin = 20.0f; // Flat ground is not steep enough.
    CHECK(growFoliage(ground, 0.0f, {}, steep, 0, 0).instances.empty());

    FoliageRules low = grass();
    low.heightMax = 3.0f; // The ground is at 4 m.
    CHECK(growFoliage(ground, 0.0f, {}, low, 0, 0).instances.empty());
    // The same rule, with the field placed lower in the world.
    CHECK_FALSE(growFoliage(ground, -10.0f, {}, low, 0, 0).instances.empty());

    FoliageRules elsewhere = grass();
    elsewhere.materialDensity = {0.0f, 0.0f, 1.0f}; // Grows on material 2; the ground is 1.
    CHECK(growFoliage(ground, 0.0f, {}, elsewhere, 0, 0).instances.empty());
    elsewhere.materialDensity = {0.0f, 0.5f};
    const std::size_t half = growFoliage(ground, 0.0f, {}, elsewhere, 0, 0).instances.size();
    CHECK(half > 1024 * 9 / 10);
    CHECK(half < 1024 * 11 / 10);
}

TEST_CASE("instances keep their minimum spacing")
{
    FoliageRules spaced = grass();
    spaced.density = 20.0f;
    spaced.minSpacing = 0.5f;
    const FoliageTile tile = growFoliage(flatTile(4.0f), 0.0f, {}, spaced, 0, 0);
    REQUIRE(tile.instances.size() > 100);
    // Sampled, not every pair: the first hundred against all.
    for (std::size_t at = 0; at < 100; ++at) {
        for (std::size_t other = 0; other < tile.instances.size(); ++other) {
            if (other == at)
                continue;
            const core::Vec3 a = tile.instances[at].position;
            const core::Vec3 b = tile.instances[other].position;
            const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
            CHECK(d >= 0.5f - 1e-4f);
        }
    }
}

TEST_CASE("a lower quality keeps a subset of the field, never a reshuffle")
{
    // `[render] foliage_density` keeps the instances whose `random` is below it,
    // so the half kept at 0.5 is inside the three quarters kept at 0.75.
    const FoliageTile tile = growFoliage(flatTile(4.0f), 0.0f, {}, grass(), 0, 0);
    std::size_t half = 0;
    std::size_t threeQuarters = 0;
    for (const FoliageInstance& instance : tile.instances) {
        half += instance.random < 0.5f ? 1 : 0;
        threeQuarters += instance.random < 0.75f ? 1 : 0;
        if (instance.random < 0.5f)
            CHECK(instance.random < 0.75f);
    }
    CHECK(half < threeQuarters);
    const double ratio = static_cast<double>(half) / static_cast<double>(tile.instances.size());
    CHECK(ratio > 0.45);
    CHECK(ratio < 0.55);
}

TEST_CASE("nothing grows with no mesh to grow")
{
    FoliageRules none = grass();
    none.meshWeights.clear();
    const FoliageTile tile = growFoliage(flatTile(4.0f), 0.0f, {}, none, 0, 0);
    CHECK(tile.instances.empty());
    CHECK(tile.meshStart.size() == 1);
}

TEST_CASE("a painted mask scales what the rules grow, column by column")
{
    const TerrainMesh ground = flatTile(4.0f);
    const std::size_t full = growFoliage(ground, 0.0f, {}, grass(), 0, 0).instances.size();

    FoliageRules none = grass();
    none.mask.assign(ChunkEdge * ChunkEdge, 0);
    CHECK(growFoliage(ground, 0.0f, {}, none, 0, 0).instances.empty());

    FoliageRules half = grass();
    half.mask.assign(ChunkEdge * ChunkEdge, 128);
    const std::size_t halved = growFoliage(ground, 0.0f, {}, half, 0, 0).instances.size();
    CHECK(halved > full * 45 / 100);
    CHECK(halved < full * 55 / 100);

    // One half of the tile bare, by column.
    FoliageRules west = grass();
    west.mask.assign(ChunkEdge * ChunkEdge, 255);
    for (std::size_t z = 0; z < ChunkEdge; ++z)
        for (std::size_t x = 0; x < ChunkEdge / 2; ++x)
            west.mask[z * ChunkEdge + x] = 0;
    for (const FoliageInstance& instance : growFoliage(ground, 0.0f, {}, west, 0, 0).instances)
        CHECK(instance.position.x >= 16.0f);
}
