// A mesh's own triangles asked what a ray meets (ADR 0203, `TriangleTree`):
// what lets light through a doorway that a hull round the building closes.

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/app/mesh_rays.h"

namespace {

using namespace engine;
using app::TriangleTree;
using core::f32;
using core::u32;
using core::Vec3;

// Two triangles for a quad, wound so its normal is `cross(b - a, d - a)`.
void quad(std::vector<Vec3>& points, std::vector<u32>& indices, Vec3 a, Vec3 b, Vec3 c, Vec3 d)
{
    const auto first = static_cast<u32>(points.size());
    points.insert(points.end(), {a, b, c, d});
    indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}

// A cube of side two about the origin, every face wound to look outwards.
void cube(std::vector<Vec3>& points, std::vector<u32>& indices)
{
    quad(points, indices, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1});
    quad(points, indices, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1});
    quad(points, indices, {-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1});
    quad(points, indices, {-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1});
    quad(points, indices, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1});
    quad(points, indices, {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1});
}

// Every triangle asked in turn: what the tree must agree with.
[[nodiscard]] bool slowly(const std::vector<Vec3>& points, const std::vector<u32>& indices, Vec3 origin, Vec3 direction,
                          f32 reach, f32& distance)
{
    bool met = false;
    distance = reach;
    for (std::size_t index = 0; index + 2 < indices.size(); index += 3) {
        const Vec3 a = points[indices[index]];
        const Vec3 ab = points[indices[index + 1]] - a;
        const Vec3 ac = points[indices[index + 2]] - a;
        const Vec3 p = core::cross(direction, ac);
        const f32 det = core::dot(ab, p);
        if (std::abs(det) < 1.0e-12f)
            continue;
        const Vec3 s = origin - a;
        const f32 u = core::dot(s, p) / det;
        const Vec3 q = core::cross(s, ab);
        const f32 v = core::dot(direction, q) / det;
        const f32 along = core::dot(ac, q) / det;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f || along < 0.0f || along >= distance)
            continue;
        distance = along;
        met = true;
    }
    return met;
}

} // namespace

TEST_CASE("ADR 0203: a ray meets the near face of a mesh, and says which way the face looks")
{
    std::vector<Vec3> points;
    std::vector<u32> indices;
    cube(points, indices);
    TriangleTree tree;
    tree.build(points, indices);
    REQUIRE(tree.triangleCount() == 12);

    TriangleTree::Hit hit;
    // From outside, along x: the face at x = -1, three metres on, looking back.
    REQUIRE(tree.cast(Vec3{-4.0f, 0.2f, 0.1f}, Vec3{1.0f, 0.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.distance == doctest::Approx(3.0));
    CHECK(hit.normal.x == doctest::Approx(-1.0));
    // From inside: the back of the face at x = 1, its normal along the ray.
    REQUIRE(tree.cast(Vec3{0.0f, 0.2f, 0.1f}, Vec3{1.0f, 0.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.distance == doctest::Approx(1.0));
    CHECK(hit.normal.x == doctest::Approx(1.0));
    // Past it, and short of it.
    CHECK_FALSE(tree.cast(Vec3{-4.0f, 3.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 100.0f, hit));
    CHECK_FALSE(tree.cast(Vec3{-4.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 2.5f, hit));
}

TEST_CASE("ADR 0203: a ray goes through a doorway that is in the mesh")
{
    // A wall in the plane x = 0, six wide and three high, with a door a metre
    // wide and two high in the middle of it: the wall either side, and above.
    std::vector<Vec3> points;
    std::vector<u32> indices;
    quad(points, indices, {0, 0, -3}, {0, 3, -3}, {0, 3, -0.5f}, {0, 0, -0.5f});
    quad(points, indices, {0, 0, 0.5f}, {0, 3, 0.5f}, {0, 3, 3}, {0, 0, 3});
    quad(points, indices, {0, 2, -0.5f}, {0, 3, -0.5f}, {0, 3, 0.5f}, {0, 2, 0.5f});
    TriangleTree tree;
    tree.build(points, indices);

    TriangleTree::Hit hit;
    const Vec3 along{1.0f, 0.0f, 0.0f};
    // Through the door.
    CHECK_FALSE(tree.cast(Vec3{-2.0f, 1.0f, 0.0f}, along, 50.0f, hit));
    // Into the wall beside it, and into the wall above it.
    CHECK(tree.cast(Vec3{-2.0f, 1.0f, 1.5f}, along, 50.0f, hit));
    CHECK(tree.cast(Vec3{-2.0f, 2.5f, 0.0f}, along, 50.0f, hit));
    // From the other side the same, the wall's back.
    CHECK(tree.cast(Vec3{2.0f, 1.0f, -1.5f}, Vec3{-1.0f, 0.0f, 0.0f}, 50.0f, hit));
    CHECK_FALSE(tree.cast(Vec3{2.0f, 1.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}, 50.0f, hit));
}

TEST_CASE("ADR 0203: a ray's reach and its answer are in the direction's own lengths")
{
    std::vector<Vec3> points;
    std::vector<u32> indices;
    cube(points, indices);
    TriangleTree tree;
    tree.build(points, indices);
    // The same ray with a direction half as long: twice as many of them to
    // the face. What a stretched mesh's own space makes of a world's ray.
    TriangleTree::Hit hit;
    REQUIRE(tree.cast(Vec3{-4.0f, 0.0f, 0.0f}, Vec3{0.5f, 0.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.distance == doctest::Approx(6.0));
    CHECK_FALSE(tree.cast(Vec3{-4.0f, 0.0f, 0.0f}, Vec3{0.5f, 0.0f, 0.0f}, 5.0f, hit));
}

TEST_CASE("ADR 0203: the tree answers what every triangle asked in turn would")
{
    // A rippled sheet of eight thousand triangles, and five hundred rays from
    // a generator that is the same on every machine.
    std::vector<Vec3> points;
    std::vector<u32> indices;
    const u32 side = 64;
    for (u32 z = 0; z <= side; ++z) {
        for (u32 x = 0; x <= side; ++x) {
            const f32 fx = static_cast<f32>(x) * 0.25f - 8.0f;
            const f32 fz = static_cast<f32>(z) * 0.25f - 8.0f;
            points.push_back(Vec3{fx, std::sin(fx * 1.3f) * std::cos(fz * 0.9f), fz});
        }
    }
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) {
            const u32 a = z * (side + 1) + x;
            indices.insert(indices.end(), {a, a + side + 1, a + 1, a + 1, a + side + 1, a + side + 2});
        }
    }
    TriangleTree tree;
    tree.build(points, indices);
    REQUIRE(tree.triangleCount() == side * side * 2);

    u32 seed = 20261010u;
    const auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<f32>(seed >> 8) / 16777216.0f;
    };
    u32 met = 0;
    for (u32 ray = 0; ray < 500; ++ray) {
        const Vec3 origin{next() * 20.0f - 10.0f, next() * 6.0f - 3.0f, next() * 20.0f - 10.0f};
        const Vec3 direction = core::normalize(Vec3{next() * 2.0f - 1.0f, next() * 2.0f - 1.0f, next() * 2.0f - 1.0f});
        f32 expected = 0.0f;
        const bool should = slowly(points, indices, origin, direction, 40.0f, expected);
        TriangleTree::Hit hit;
        const bool did = tree.cast(origin, direction, 40.0f, hit);
        CHECK(did == should);
        if (did && should) {
            CHECK(hit.distance == doctest::Approx(static_cast<double>(expected)).epsilon(1.0e-4));
            ++met;
        }
    }
    // Most of them meet it: the comparison is of answers, not of misses.
    CHECK(met > 100);
}

TEST_CASE("ADR 0203: a triangle with no area, or a point that is not there, is left out")
{
    std::vector<Vec3> points{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}};
    // One good triangle, one of three points in a line, one naming a ninth point.
    const std::vector<u32> indices{0, 1, 2, 0, 1, 3, 0, 1, 9};
    TriangleTree tree;
    tree.build(points, indices);
    CHECK(tree.triangleCount() == 1);
    TriangleTree empty;
    empty.build({}, {});
    CHECK(empty.empty());
    TriangleTree::Hit hit;
    CHECK_FALSE(empty.cast(Vec3{0, 0, -1}, Vec3{0, 0, 1}, 10.0f, hit));
}

TEST_CASE("ADR 0203: of many boxes a ray is shown the few it enters, and stops at what it met")
{
    // A row of a hundred boxes a metre wide along x, far out in the world --
    // where a place is too large a number to walk a ray in.
    const core::DVec3 far{4.0e6, 100.0, -2.0e6};
    std::vector<core::DVec3> lows;
    std::vector<core::DVec3> highs;
    for (int index = 0; index < 100; ++index) {
        lows.push_back(far + core::DVec3{static_cast<double>(index) * 3.0, 0.0, 0.0});
        highs.push_back(far + core::DVec3{static_cast<double>(index) * 3.0 + 1.0, 1.0, 1.0});
    }
    app::BoxTree tree;
    tree.build(lows, highs);
    REQUIRE_FALSE(tree.empty());

    // Along the row from before its start: every box is entered when nothing
    // stops the ray, nearest first or nearly.
    std::vector<u32> seen;
    tree.cast(far + core::DVec3{-5.0, 0.5, 0.5}, Vec3{1.0f, 0.0f, 0.0f}, 1000.0f, [&](u32 box, f32 reach) {
        seen.push_back(box);
        return reach;
    });
    CHECK(seen.size() == 100);
    CHECK(seen.front() < 4);

    // Stopped at the first box it met, it is shown hardly any more.
    seen.clear();
    tree.cast(far + core::DVec3{-5.0, 0.5, 0.5}, Vec3{1.0f, 0.0f, 0.0f}, 1000.0f, [&](u32 box, f32 reach) {
        seen.push_back(box);
        // The ray met this box's near face.
        return std::min(reach, 5.0f + static_cast<f32>(box) * 3.0f);
    });
    CHECK(seen.size() < 8);
    CHECK(std::find(seen.begin(), seen.end(), 0u) != seen.end());

    // Across the row it enters one; beside the row, none.
    seen.clear();
    tree.cast(far + core::DVec3{30.5, 0.5, -10.0}, Vec3{0.0f, 0.0f, 1.0f}, 100.0f, [&](u32 box, f32 reach) {
        seen.push_back(box);
        return reach;
    });
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == 10);
    seen.clear();
    tree.cast(far + core::DVec3{30.5, 5.0, -10.0}, Vec3{0.0f, 0.0f, 1.0f}, 100.0f, [&](u32 box, f32 reach) {
        seen.push_back(box);
        return reach;
    });
    CHECK(seen.empty());

    app::BoxTree none;
    none.build({}, {});
    CHECK(none.empty());
}
