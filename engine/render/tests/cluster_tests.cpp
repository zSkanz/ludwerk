#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/render/clusters.h"

using namespace engine;
using engine::render::buildClusters;
using engine::render::ClusterGrid;
using engine::render::clusterIndexOf;
using engine::render::clusterSliceConstants;
using engine::render::clusterSliceOf;
using engine::render::kClusterCount;
using engine::render::kClusterOffsetShift;
using engine::render::kClusterSlices;
using engine::render::kClusterTilesX;
using engine::render::kClusterTilesY;
using engine::render::kMaxClusteredLights;
using engine::render::RenderCamera;
using engine::render::RenderLight;

namespace {

[[nodiscard]] RenderCamera testCamera() noexcept
{
    RenderCamera camera;
    camera.valid = true;
    camera.nearPlane = 0.1f;
    camera.farPlane = 400.0f;
    camera.projection =
        core::perspective(55.0f * 3.14159265f / 180.0f, 16.0f / 9.0f, camera.nearPlane, camera.farPlane);
    // The camera sits at the origin of the snapshot's space and looks down -Z,
    // which is what an identity view means here.
    camera.view = core::Mat4{};
    camera.viewProjection = camera.projection;
    return camera;
}

// The two numbers a grid texel packs, unpacked the way the shader does.
void unpackCluster(const ClusterGrid& clusters, core::u32 cluster, core::u32& outOffset, core::u32& outCount)
{
    const core::f32 packed = clusters.grid[cluster];
    outOffset = static_cast<core::u32>(packed / kClusterOffsetShift);
    outCount = static_cast<core::u32>(packed - static_cast<core::f32>(outOffset) * kClusterOffsetShift);
}

[[nodiscard]] RenderLight lightAt(core::Vec3 position, core::f32 range) noexcept
{
    RenderLight light;
    light.position = position;
    light.range = range;
    light.brightness = 1.0f;
    return light;
}

} // namespace

TEST_CASE("a cluster's index is its texel's row-major position, and both sides agree")
{
    // The bug this exists for: the assignment numbered clusters slice-major
    // while the shader read the grid texture row-major, so every fragment looked
    // up some other part of the screen. Both sides were internally consistent,
    // every unit test passed, and it was visible only as light clipped into hard
    // horizontal bands.
    //
    // The shader computes `x = slice * TILES_X + tileX` and `y = tileY` against
    // a texture `kClusterGridWidth` wide. This is that, written once.
    for (core::u32 slice = 0; slice < kClusterSlices; ++slice) {
        for (core::u32 tileY = 0; tileY < kClusterTilesY; ++tileY) {
            for (core::u32 tileX = 0; tileX < kClusterTilesX; ++tileX) {
                const core::u32 texelX = slice * kClusterTilesX + tileX;
                CHECK(clusterIndexOf(tileX, tileY, slice) == tileY * engine::render::kClusterGridWidth + texelX);
                CHECK(clusterIndexOf(tileX, tileY, slice) < kClusterCount);
            }
        }
    }
}

TEST_CASE("exponential slicing puts the near plane in slice zero and the far one in the last")
{
    core::f32 scale = 0.0f;
    core::f32 bias = 0.0f;
    clusterSliceConstants(0.1f, 500.0f, scale, bias);

    CHECK(clusterSliceOf(0.1f, scale, bias) == 0u);
    CHECK(clusterSliceOf(0.05f, scale, bias) == 0u);
    CHECK(clusterSliceOf(499.0f, scale, bias) == kClusterSlices - 1);
    CHECK(clusterSliceOf(100000.0f, scale, bias) == kClusterSlices - 1);

    // Monotone, and that is the property the shader depends on: it computes the
    // same expression and must land in the same slice the assignment used.
    core::u32 previous = 0;
    for (core::f32 depth = 0.1f; depth < 500.0f; depth *= 1.15f) {
        const core::u32 slice = clusterSliceOf(depth, scale, bias);
        CHECK(slice >= previous);
        previous = slice;
    }

    // And it is exponential rather than uniform, which is the whole reason for
    // the logarithm: half the slices are spent inside the first two per cent of
    // the range under a uniform scheme, and here they are not.
    CHECK(clusterSliceOf(1.0f, scale, bias) > 4u);
    CHECK(clusterSliceOf(1.0f, scale, bias) < kClusterSlices / 2);
}

TEST_CASE("a light in front of the camera reaches clusters; one behind it reaches none")
{
    const RenderCamera camera = testCamera();
    ClusterGrid clusters;

    const std::vector<RenderLight> lights{lightAt(core::Vec3{0.0f, 0.0f, -5.0f}, 4.0f)};
    buildClusters(camera, lights, clusters);
    CHECK(clusters.lightCount == 1u);
    CHECK(clusters.indexCount > 0u);

    // Entirely behind the near plane. Assigning it would be a light that costs
    // every fragment in a tile and contributes nothing.
    const std::vector<RenderLight> behind{lightAt(core::Vec3{0.0f, 0.0f, 40.0f}, 4.0f)};
    buildClusters(camera, behind, clusters);
    CHECK(clusters.lightCount == 1u);
    CHECK(clusters.indexCount == 0u);
}

TEST_CASE("the ninth light lights something, which is the whole point of this pass")
{
    // M4 shipped eight lights per draw, unculled, and ADR 0038 names that as one
    // of the four gaps. The claim being checked is the narrow one that matters:
    // a light past the eighth is assigned rather than silently dropped.
    const RenderCamera camera = testCamera();
    ClusterGrid clusters;

    std::vector<RenderLight> lights;
    for (core::u32 index = 0; index < 24; ++index) {
        const auto offset = static_cast<core::f32>(index) - 11.5f;
        lights.push_back(lightAt(core::Vec3{offset * 0.7f, 0.0f, -12.0f}, 3.0f));
    }
    buildClusters(camera, lights, clusters);
    CHECK(clusters.lightCount == 24u);

    std::vector<bool> assigned(lights.size(), false);
    for (core::u32 cluster = 0; cluster < kClusterCount; ++cluster) {
        core::u32 offset = 0;
        core::u32 count = 0;
        unpackCluster(clusters, cluster, offset, count);
        for (core::u32 i = 0; i < count; ++i) {
            const auto index = static_cast<core::usize>(clusters.indices[offset + i]);
            REQUIRE(index < assigned.size());
            assigned[index] = true;
        }
    }
    for (core::usize index = 0; index < assigned.size(); ++index)
        CHECK(assigned[index]);
}

TEST_CASE("a cluster's run is contiguous, ascending, and bounded")
{
    const RenderCamera camera = testCamera();
    ClusterGrid clusters;

    std::vector<RenderLight> lights;
    for (core::u32 index = 0; index < 40; ++index) {
        const auto angle = static_cast<core::f32>(index) * 0.7f;
        lights.push_back(lightAt(core::Vec3{std::cos(angle) * 3.0f, std::sin(angle) * 2.0f, -9.0f}, 6.0f));
    }
    buildClusters(camera, lights, clusters);

    core::u32 previousEnd = 0;
    for (core::u32 cluster = 0; cluster < kClusterCount; ++cluster) {
        core::u32 offset = 0;
        core::u32 count = 0;
        unpackCluster(clusters, cluster, offset, count);
        if (count == 0)
            continue;

        // Runs are laid out in cluster order and never overlap, which is what
        // the prefix sum buys and what the shader assumes when it reads `count`
        // entries starting at `offset`.
        CHECK(offset >= previousEnd);
        previousEnd = offset + count;
        CHECK(previousEnd <= clusters.indexCount);

        // Ascending by light index within the run: the two passes visit lights
        // in the same order, so this is a pure function of the snapshot (R10)
        // even though nothing here is simulation.
        core::f32 previous = -1.0f;
        for (core::u32 i = 0; i < count; ++i) {
            CHECK(clusters.indices[offset + i] > previous);
            previous = clusters.indices[offset + i];
        }
    }
}

TEST_CASE("the same snapshot builds the same tables, byte for byte")
{
    const RenderCamera camera = testCamera();
    std::vector<RenderLight> lights;
    for (core::u32 index = 0; index < 30; ++index) {
        const auto angle = static_cast<core::f32>(index) * 1.3f;
        lights.push_back(lightAt(core::Vec3{std::cos(angle) * 5.0f, std::sin(angle) * 3.0f, -14.0f}, 7.0f));
    }

    ClusterGrid first;
    ClusterGrid second;
    buildClusters(camera, lights, first);
    buildClusters(camera, lights, second);

    CHECK(first.grid == second.grid);
    CHECK(first.indices == second.indices);
    CHECK(first.lightData == second.lightData);
    CHECK(first.indexCount == second.indexCount);
}

TEST_CASE("a light is assigned to the tile it is actually over, not to its mirror")
{
    // The one axis that is easy to get backwards and invisible in a symmetric
    // scene: tile Y counts DOWN the screen because the shader derives it from
    // `SV_Position.y`, while normalised device Y runs up. A light placed high in
    // the world must land in a LOW tile index.
    const RenderCamera camera = testCamera();
    ClusterGrid clusters;
    const std::vector<RenderLight> lights{lightAt(core::Vec3{0.0f, 3.0f, -10.0f}, 1.0f)};
    buildClusters(camera, lights, clusters);
    REQUIRE(clusters.indexCount > 0u);

    core::u32 lowestTileY = kClusterTilesY;
    core::u32 highestTileY = 0;
    for (core::u32 cluster = 0; cluster < kClusterCount; ++cluster) {
        core::u32 offset = 0;
        core::u32 count = 0;
        unpackCluster(clusters, cluster, offset, count);
        if (count == 0)
            continue;
        const core::u32 tileY = cluster / engine::render::kClusterGridWidth;
        lowestTileY = tileY < lowestTileY ? tileY : lowestTileY;
        highestTileY = tileY > highestTileY ? tileY : highestTileY;
    }

    CHECK(highestTileY < kClusterTilesY / 2);
    CHECK(lowestTileY <= highestTileY);
}

TEST_CASE("more lights than the budget are refused rather than wrapped")
{
    const RenderCamera camera = testCamera();
    ClusterGrid clusters;

    std::vector<RenderLight> lights;
    for (core::u32 index = 0; index < kMaxClusteredLights + 40; ++index)
        lights.push_back(lightAt(core::Vec3{0.0f, 0.0f, -8.0f}, 2.0f));
    buildClusters(camera, lights, clusters);

    // The budget is a budget and it is enforced at the table's edge. A count
    // that wrapped would index past the light texture, which is the difference
    // between "the two hundred and fifty-seventh light does not light" and "the
    // frame reads whatever is next in memory".
    CHECK(clusters.lightCount == kMaxClusteredLights);
    for (core::u32 index = 0; index < clusters.indexCount; ++index)
        CHECK(clusters.indices[index] < static_cast<core::f32>(kMaxClusteredLights));
    // And a pile of overlapping lights is exactly the case the per-cluster bound
    // exists for, so it must report having bound something.
    CHECK(clusters.overflowClusters > 0u);
}

TEST_CASE("D531: every point a light reaches is in a cluster that holds it, at any aspect")
{
    // The shading contract, asked of the grid directly: a fragment within a
    // light's range finds the light in its own cluster. A light whose sphere
    // lay to one side of the view axis had its screen extent projected at its
    // nearest depth only -- but there its inner edge is the farthest from the
    // centre of the screen, and the tiles between it and the inner edge seen at
    // its far depth were never given the light. Lit up to a straight vertical
    // edge, then not: the band across the lamp-lit wall.
    const core::f32 aspects[]{16.0f / 9.0f, 1583.0f / 907.0f, 21.0f / 9.0f, 4.0f / 3.0f};
    const core::Vec3 places[]{
        core::Vec3{6.0f, 1.0f, -12.0f},
        core::Vec3{-7.0f, -1.5f, -10.0f},
        core::Vec3{12.0f, 2.0f, -20.0f},
        core::Vec3{-3.0f, 0.5f, -5.0f},
        core::Vec3{9.0f, -3.0f, -30.0f},
        core::Vec3{0.5f, 0.2f, -8.0f},
        core::Vec3{-15.0f, 4.0f, -25.0f},
        // Wholly to one side of the view's axis, as lamps along a wall are.
        core::Vec3{20.0f, 1.0f, -22.0f},
        core::Vec3{-24.0f, 2.0f, -30.0f},
        core::Vec3{17.0f, -1.0f, -40.0f},
        core::Vec3{2.0f, 18.0f, -30.0f},
    };
    int checked = 0;
    for (const core::f32 aspect : aspects) {
        RenderCamera camera = testCamera();
        camera.projection = core::perspective(70.0f * 3.14159265f / 180.0f, aspect, camera.nearPlane, camera.farPlane);
        camera.viewProjection = camera.projection;
        for (const core::Vec3 place : places) {
            const core::f32 range = 14.0f;
            ClusterGrid clusters;
            const std::vector<RenderLight> lights{lightAt(place, range)};
            buildClusters(camera, lights, clusters);
            // Points through the sphere, on a lattice.
            for (int a = -6; a <= 6; ++a) {
                for (int b = -6; b <= 6; ++b) {
                    for (int c = -6; c <= 6; ++c) {
                        const core::Vec3 point{place.x + range * static_cast<core::f32>(a) / 6.5f,
                                               place.y + range * static_cast<core::f32>(b) / 6.5f,
                                               place.z + range * static_cast<core::f32>(c) / 6.5f};
                        const core::f32 dx = point.x - place.x;
                        const core::f32 dy = point.y - place.y;
                        const core::f32 dz = point.z - place.z;
                        if (dx * dx + dy * dy + dz * dz >= range * range * 0.98f)
                            continue;
                        // Where the shader finds it: the projection's column
                        // vector convention, then the tile and the slice.
                        const core::Mat4& m = camera.projection;
                        const core::f32 clipX =
                            m.m[0][0] * point.x + m.m[1][0] * point.y + m.m[2][0] * point.z + m.m[3][0];
                        const core::f32 clipY =
                            m.m[0][1] * point.x + m.m[1][1] * point.y + m.m[2][1] * point.z + m.m[3][1];
                        const core::f32 clipW =
                            m.m[0][3] * point.x + m.m[1][3] * point.y + m.m[2][3] * point.z + m.m[3][3];
                        const core::f32 depth = -point.z;
                        if (depth <= camera.nearPlane || clipW <= 0.0f)
                            continue;
                        const core::f32 ndcX = clipX / clipW;
                        const core::f32 ndcY = clipY / clipW;
                        if (ndcX <= -1.0f || ndcX >= 1.0f || ndcY <= -1.0f || ndcY >= 1.0f)
                            continue;
                        const auto tileX = static_cast<core::u32>((ndcX * 0.5f + 0.5f) * kClusterTilesX);
                        const auto tileY = static_cast<core::u32>((0.5f - ndcY * 0.5f) * kClusterTilesY);
                        const core::u32 slice = clusterSliceOf(depth, clusters.sliceScale, clusters.sliceBias);
                        core::u32 offset = 0;
                        core::u32 count = 0;
                        unpackCluster(clusters, clusterIndexOf(tileX, tileY, slice), offset, count);
                        CAPTURE(aspect);
                        CAPTURE(place.x);
                        CAPTURE(place.z);
                        CAPTURE(tileX);
                        CAPTURE(tileY);
                        CAPTURE(slice);
                        CHECK(count == 1u);
                        ++checked;
                    }
                }
            }
        }
    }
    CHECK(checked > 2000);
}
