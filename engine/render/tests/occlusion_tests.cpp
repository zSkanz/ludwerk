// **What a wall hides is not drawn** (ADR 0262): the software depth buffer
// and its test, against boxes and spheres placed by hand.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/core/math.h"
#include "engine/render/occlusion.h"
#include "engine/render/render_world.h"

using engine::core::f32;
using engine::core::Mat4;
using engine::core::Vec3;
using engine::render::OcclusionCuller;

namespace {

// A camera at the origin looking down -Z, 60 degrees up and down, the
// buffer's own aspect: what the snapshot's camera-relative view-projection is
// for a camera with no turn.
[[nodiscard]] Mat4 lookingDownZ()
{
    const f32 nearPlane = 0.1f;
    const f32 farPlane = 1000.0f;
    const f32 aspect = static_cast<f32>(OcclusionCuller::Width) / static_cast<f32>(OcclusionCuller::Height);
    return engine::core::perspective(60.0f * 3.14159265f / 180.0f, aspect, nearPlane, farPlane);
}

// The unit cube scaled to `size` and moved to `at`.
[[nodiscard]] Mat4 box(Vec3 at, Vec3 size)
{
    Mat4 m;
    m.m[0][0] = size.x;
    m.m[1][1] = size.y;
    m.m[2][2] = size.z;
    m.m[3][0] = at.x;
    m.m[3][1] = at.y;
    m.m[3][2] = at.z;
    return m;
}

constexpr f32 NearPlane = 0.1f;

} // namespace

TEST_CASE("a wall hides what is wholly behind it and nothing in front of it or beside it")
{
    OcclusionCuller culler;
    const Mat4 camera = lookingDownZ();
    // A wall ten metres off, ten wide and ten tall.
    const std::array<Mat4, 1> walls{box({0.0f, 0.0f, -10.0f}, {10.0f, 10.0f, 1.0f})};
    culler.drawBoxes(camera, walls, NearPlane);

    // Behind it, well inside its outline: hidden.
    CHECK(culler.hides(camera, {0.0f, 0.0f, -20.0f}, 1.0f, NearPlane));
    // In front of it: drawn.
    CHECK_FALSE(culler.hides(camera, {0.0f, 0.0f, -5.0f}, 1.0f, NearPlane));
    // Behind it but reaching past its edge as the camera sees it -- the edge,
    // five metres out at ten, is nearly sixteen out at thirty: drawn.
    CHECK_FALSE(culler.hides(camera, {14.0f, 0.0f, -30.0f}, 3.0f, NearPlane));
    // And the same distance behind, wholly inside its outline: hidden.
    CHECK(culler.hides(camera, {8.0f, 0.0f, -30.0f}, 3.0f, NearPlane));
    // Behind its plane but well beside it: drawn.
    CHECK_FALSE(culler.hides(camera, {40.0f, 0.0f, -30.0f}, 1.0f, NearPlane));
    // Straddling the wall itself: its nearest point is in front, drawn.
    CHECK_FALSE(culler.hides(camera, {0.0f, 0.0f, -10.0f}, 1.0f, NearPlane));
}

TEST_CASE("nothing drawn hides nothing, and a sphere at the camera is always drawn")
{
    OcclusionCuller culler;
    const Mat4 camera = lookingDownZ();
    culler.drawBoxes(camera, {}, NearPlane);
    CHECK_FALSE(culler.hides(camera, {0.0f, 0.0f, -20.0f}, 1.0f, NearPlane));

    const std::array<Mat4, 1> walls{box({0.0f, 0.0f, -10.0f}, {20.0f, 10.0f, 1.0f})};
    culler.drawBoxes(camera, walls, NearPlane);
    CHECK_FALSE(culler.hides(camera, {0.0f, 0.0f, -0.5f}, 1.0f, NearPlane));
}

TEST_CASE("a thin post hides nothing it does not wholly cover, though its pixels are touched")
{
    // A post a few pixels wide in the buffer: a sphere behind it as wide as
    // the post is not hidden, because the edges of the post are only partly
    // inside the pixels they cross.
    OcclusionCuller culler;
    const Mat4 camera = lookingDownZ();
    const std::array<Mat4, 1> posts{box({0.0f, 0.0f, -10.0f}, {0.3f, 10.0f, 0.3f})};
    culler.drawBoxes(camera, posts, NearPlane);
    CHECK_FALSE(culler.hides(camera, {0.0f, 0.0f, -20.0f}, 0.4f, NearPlane));
}

TEST_CASE("a box reaching past the near plane still hides what it wholly covers beyond")
{
    // A floor-to-ceiling wall the camera stands in front of, its near
    // triangles cut: what is left still hides what is behind it.
    OcclusionCuller culler;
    const Mat4 camera = lookingDownZ();
    const std::array<Mat4, 1> walls{box({0.0f, 0.0f, -3.0f}, {60.0f, 40.0f, 1.0f})};
    culler.drawBoxes(camera, walls, NearPlane);
    CHECK(culler.hides(camera, {0.0f, 0.0f, -30.0f}, 2.0f, NearPlane));
}

namespace {

// **A ridge**: a grid of ten-metre tiles from z = -60 to -20 and x = -50 to
// 50, ten metres under the camera, rising from flat ground at -20 to twelve
// metres at -40 -- two over the camera's eye -- and dropping to a valley
// behind it.
[[nodiscard]] engine::render::OccluderGrid ridge()
{
    engine::render::OccluderGrid grid;
    grid.origin = {-50.0f, -10.0f, -60.0f};
    grid.tile = 10.0f;
    grid.columns = 10;
    grid.rows = 4;
    const std::array<f32, 5> rowHeights{0.0f, 0.0f, 12.0f, 6.0f, 0.0f};
    for (const f32 height : rowHeights) {
        for (engine::core::u32 i = 0; i <= grid.columns; ++i)
            grid.heights.push_back(height);
    }
    grid.solid.assign(static_cast<std::size_t>(grid.columns) * grid.rows, 1);
    return grid;
}

} // namespace

TEST_CASE("the ground: a ridge hides the valley behind it, and not what stands over its line")
{
    OcclusionCuller culler;
    const Mat4 camera = lookingDownZ();
    const std::array<engine::render::OccluderGrid, 1> grids{ridge()};
    culler.drawOccluders(camera, {}, grids, NearPlane);

    // Low in the valley behind it: hidden -- though the valley's floor, drawn
    // too, is further than the thing is.
    CHECK(culler.hides(camera, {0.0f, -8.0f, -55.0f}, 1.0f, NearPlane));
    CHECK(culler.hides(camera, {-20.0f, -7.0f, -52.0f}, 1.5f, NearPlane));
    // High over the ridge's line: drawn.
    CHECK_FALSE(culler.hides(camera, {0.0f, 10.0f, -55.0f}, 1.0f, NearPlane));
    // In front of it, over the ground: drawn.
    CHECK_FALSE(culler.hides(camera, {0.0f, -3.0f, -25.0f}, 1.0f, NearPlane));
}

TEST_CASE("the ground: a tile that is not solid is a hole the valley is seen through")
{
    OcclusionCuller culler;
    const Mat4 camera = lookingDownZ();
    engine::render::OccluderGrid grid = ridge();
    // The front slope's two tiles either side of x = 0.
    for (const engine::core::u32 row : {2u, 3u}) {
        for (const engine::core::u32 column : {4u, 5u})
            grid.solid[static_cast<std::size_t>(row) * grid.columns + column] = 0;
    }
    const std::array<engine::render::OccluderGrid, 1> grids{grid};
    culler.drawOccluders(camera, {}, grids, NearPlane);

    CHECK_FALSE(culler.hides(camera, {0.0f, -8.0f, -55.0f}, 1.0f, NearPlane));
    // Behind the slope still standing: hidden.
    CHECK(culler.hides(camera, {-30.0f, -8.0f, -55.0f}, 1.0f, NearPlane));
}

TEST_CASE("a box seen across its corner: no pixel kept is nearer than the box over it")
{
    // Every pixel the buffer keeps is wholly covered, and at least as far as
    // the box is anywhere over it: each corner of the pixel cast as a ray.
    OcclusionCuller culler;
    const Mat4 camera = lookingDownZ();
    // Six metres a side, turned 0.6 radians about y and seen from below a
    // corner, so three faces meet in the middle of its picture.
    const f32 turn = 0.6f;
    Mat4 placed = box({4.0f, 4.0f, -15.0f}, {6.0f, 6.0f, 6.0f});
    placed.m[0][0] = 6.0f * std::cos(turn);
    placed.m[0][2] = -6.0f * std::sin(turn);
    placed.m[2][0] = 6.0f * std::sin(turn);
    placed.m[2][2] = 6.0f * std::cos(turn);
    const std::array<Mat4, 1> boxes{placed};
    culler.drawOccluders(camera, boxes, {}, NearPlane);
    const Mat4 inverse = engine::core::inverse(camera);
    const Mat4 back = engine::core::inverse(placed);
    int checked = 0;
    for (engine::core::u32 y = 0; y < OcclusionCuller::Height; ++y) {
        for (engine::core::u32 x = 0; x < OcclusionCuller::Width; ++x) {
            const f32 kept = culler.depthAt(x, y);
            if (!(kept < 1.0e30f))
                continue;
            // The pixel's four corners: a ray through each, into the box's
            // own space, where it meets the unit cube's far side at most.
            for (const f32 dx : {0.0f, 1.0f}) {
                for (const f32 dy : {0.0f, 1.0f}) {
                    const f32 nx = (static_cast<f32>(x) + dx) / OcclusionCuller::Width * 2.0f - 1.0f;
                    const f32 ny = 1.0f - (static_cast<f32>(y) + dy) / OcclusionCuller::Height * 2.0f;
                    const Vec3 farPoint = engine::core::transformPoint(inverse, Vec3{nx, ny, 1.0f});
                    const Vec3 direction = engine::core::normalize(farPoint);
                    const Vec3 origin = engine::core::transformPoint(back, Vec3{});
                    const Vec3 heading = engine::core::transformPoint(back, direction) - origin;
                    // Slab test: where the ray is inside the unit cube.
                    f32 enter = 0.0f;
                    f32 leave = 1.0e30f;
                    for (int axis = 0; axis < 3; ++axis) {
                        const f32 o = axis == 0 ? origin.x : axis == 1 ? origin.y : origin.z;
                        const f32 d = axis == 0 ? heading.x : axis == 1 ? heading.y : heading.z;
                        if (std::fabs(d) < 1.0e-9f) {
                            if (o < -0.5f || o > 0.5f)
                                leave = -1.0f;
                            continue;
                        }
                        const f32 t0 = (-0.5f - o) / d;
                        const f32 t1 = (0.5f - o) / d;
                        enter = std::max(enter, std::min(t0, t1));
                        leave = std::min(leave, std::max(t0, t1));
                    }
                    // Every corner of a kept pixel's ray meets the box, and
                    // reaches its surface no further than the depth kept.
                    REQUIRE(enter <= leave);
                    const f32 depthThere = enter * -direction.z;
                    CHECK(depthThere <= kept * 1.0001f);
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 400);
}

TEST_CASE("the ground's grid: a corner is the lowest top round it, and a tile needs the three after it")
{
    // Three by three tiles, four by four given: all five metres of solid
    // ground on a floor at zero, but one lower at two metres and one a hole.
    constexpr engine::core::u32 Size = 3;
    std::vector<engine::render::GroundTile> tiles((Size + 1) * (Size + 1),
                                                  engine::render::GroundTile{5.0f, 0.0f, true});
    tiles[1 * (Size + 1) + 1].top = 2.0f;
    tiles[3 * (Size + 1) + 3].solid = false;
    engine::render::OccluderGrid grid;
    engine::render::makeGroundGrid(tiles, Size, Size, grid);
    REQUIRE(grid.heights.size() == (Size + 1) * (Size + 1));
    REQUIRE(grid.solid.size() == Size * Size);
    // The low tile's four corners are two metres; the rest five.
    CHECK(grid.heights[1 * (Size + 1) + 1] == 2.0f);
    CHECK(grid.heights[2 * (Size + 1) + 2] == 2.0f);
    CHECK(grid.heights[0] == 5.0f);
    CHECK(grid.heights[3 * (Size + 1) + 0] == 5.0f);
    // Every tile is drawn but the one whose next across is the hole.
    for (engine::core::u32 j = 0; j < Size; ++j) {
        for (engine::core::u32 i = 0; i < Size; ++i)
            CHECK(grid.solid[j * Size + i] == ((i == 2 && j == 2) ? 0 : 1));
    }
}

TEST_CASE("the ground's grid: what drawing one costs")
{
    // Thirty-two tiles a side of rolling ground, sixteen metres each, the
    // camera two metres over it: the grid a terrain hands the culler.
    engine::render::OccluderGrid grid;
    grid.origin = {-256.0f, -2.0f, -256.0f};
    grid.tile = 16.0f;
    grid.columns = 32;
    grid.rows = 32;
    for (engine::core::u32 j = 0; j <= grid.rows; ++j) {
        for (engine::core::u32 i = 0; i <= grid.columns; ++i)
            grid.heights.push_back(3.0f * std::sin(static_cast<f32>(i) * 0.8f) * std::cos(static_cast<f32>(j) * 0.6f));
    }
    grid.solid.assign(static_cast<std::size_t>(grid.columns) * grid.rows, 1);
    OcclusionCuller culler;
    const Mat4 camera = lookingDownZ();
    const std::array<engine::render::OccluderGrid, 1> grids{grid};
    constexpr int Rounds = 50;
    const auto started = std::chrono::steady_clock::now();
    for (int round = 0; round < Rounds; ++round)
        culler.drawOccluders(camera, {}, grids, NearPlane);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / Rounds;
    MESSAGE("one grid of 32 by 32 tiles: " << ms << " ms");
    CHECK(ms > 0.0);
}
