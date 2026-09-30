// **Where two levels of detail meet** (terrain audit T0, TA2): the ground a
// fine node draws beside a coarse one, looked at along a grid of rays.
//
// A seam is judged the way a camera judges it. Rays come down at an angle
// across the line where the two nodes meet, from either side, and each must
// meet the front of some triangle the two draw. A ray that meets none has seen the sky through the ground: the owner's
// "recorte".
//
// **Stitched** (ADR 0140): the fine node is meshed knowing the level drawn
// beside it, and gathers its cells along that side at that level, so the two
// share the seam's vertices. The skirt that hung one way is gone.
#include <algorithm>
#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <map>
#include <optional>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/render/terrain_loader.h"

using namespace engine;
using namespace engine::render;

namespace {

struct Triangle
{
    core::Vec3 a;
    core::Vec3 b;
    core::Vec3 c;
};

// The triangles a node draws.
void gather(const asset::TerrainMesh& mesh, std::vector<Triangle>& out)
{
    const auto& vertices = mesh.mesh.vertices;
    const auto& indices = mesh.mesh.indices;
    for (core::usize at = 0; at + 2 < indices.size(); at += 3)
        out.push_back(Triangle{vertices[indices[at]].position, vertices[indices[at + 1]].position,
                               vertices[indices[at + 2]].position});
}

[[nodiscard]] core::Vec3 minus(core::Vec3 a, core::Vec3 b)
{
    return core::Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

// Whether a ray meets the FRONT of a triangle: a back face is culled, and
// what is behind it shows through.
[[nodiscard]] bool meetsFront(core::Vec3 from, core::Vec3 direction, const Triangle& t)
{
    const core::Vec3 e1 = minus(t.b, t.a);
    const core::Vec3 e2 = minus(t.c, t.a);
    const core::Vec3 face = core::cross(e1, e2);
    if (core::dot(face, direction) >= 0.0f)
        return false;
    const core::Vec3 p = core::cross(direction, e2);
    const float det = core::dot(e1, p);
    if (std::abs(det) < 1e-9f)
        return false;
    const float inv = 1.0f / det;
    const core::Vec3 s = minus(from, t.a);
    const float u = core::dot(s, p) * inv;
    if (u < -1e-5f || u > 1.0f + 1e-5f)
        return false;
    const core::Vec3 q = core::cross(s, e1);
    const float v = core::dot(direction, q) * inv;
    if (v < -1e-5f || u + v > 1.0f + 1e-5f)
        return false;
    return core::dot(e2, q) * inv > 0.0f;
}

// Rays down across the seam at x = `seam`, from both sides at `slope` metres
// down per metre across, over z in [z0, z1): how many see through.
[[nodiscard]] int seeThrough(const std::vector<Triangle>& triangles, float seam, float z0, float z1, float top)
{
    int through = 0;
    for (float z = z0; z < z1; z += 0.37f) {
        for (float offset = -3.0f; offset <= 3.0f; offset += 0.41f) {
            for (const float sideways : {-1.0f, 1.0f}) {
                const core::Vec3 direction = core::normalize(core::Vec3{sideways, -1.0f, 0.0f});
                // Started high above the seam, so it crosses it on the way down.
                const float lift = top + 40.0f;
                const core::Vec3 from{seam + offset - sideways * lift, lift, z};
                const bool met = std::any_of(triangles.begin(), triangles.end(),
                                             [&](const Triangle& t) { return meetsFront(from, direction, t); });
                through += met ? 0 : 1;
            }
        }
    }
    return through;
}

} // namespace

TEST_CASE("two nodes of one level meet with no gap -- the instrument, proved on a seam that is closed")
{
    asset::TerrainField field(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    // Wide enough that every seam tested is in the middle of ground.
    (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 2048.0f, 3.4f, 1);
    (void)asset::fillBall(field, core::DVec3{64.0, -6.0, 16.0}, 22.0, 1);
    std::vector<Triangle> triangles;
    gather(meshTerrainNode(field, TerrainNodeKey{0, 1, 0}), triangles);
    gather(meshTerrainNode(field, TerrainNodeKey{0, 2, 0}), triangles);
    CHECK(seeThrough(triangles, 64.0f, 1.0f, 31.0f, 30.0f) == 0);
}

TEST_CASE("a fine node and the coarse one beside it leave no gap, on flat ground and on a hill")
{
    asset::TerrainField field(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    // Wide enough that every seam tested is in the middle of ground.
    (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 2048.0f, 3.4f, 1);
    // A hill across each seam, so the two levels disagree about a slope.
    for (const double seam : {64.0, 128.0, 256.0})
        (void)asset::fillBall(field, core::DVec3{seam, -6.0, 16.0}, 22.0, 1);

    for (const core::u32 level : {0u, 1u, 2u}) {
        CAPTURE(level);
        // The fine node ends at x = its right edge; the coarse node one level
        // up starts there.
        const auto width = static_cast<core::i32>(1u << level);
        const TerrainNodeKey fine{level, 1, 0};
        const TerrainNodeKey coarse{level + 1, 1, 0};
        std::vector<Triangle> triangles;
        gather(meshTerrainNode(field, fine, TerrainSides{0, static_cast<core::u8>(level + 1), 0, 0}), triangles);
        gather(meshTerrainNode(field, coarse), triangles);
        REQUIRE_FALSE(triangles.empty());
        const float seam = static_cast<float>(2 * width) * 32.0f;
        // Along the seam, less a coarse cell at each end: the corners are the
        // business of the nodes beside these two, which this does not draw.
        const auto corner = static_cast<float>(2 << level) + 1.0f;
        CHECK(seeThrough(triangles, seam, corner, static_cast<float>(width) * 32.0f - corner, 30.0f) == 0);
    }
}

TEST_CASE("a floating slab's seam is closed whichever side the coarse node is on, and two levels apart")
{
    // The gallery's 4 m slab, whose seam between levels 0 and 1 opened half a
    // coarse cell wide when the coarse node was on the low side.
    asset::TerrainField field(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)asset::fillBlock(field, core::DVec3{0.0, -2.0, 0.0}, core::Vec3{600.0f, 4.0f, 600.0f}, 1);
    (void)asset::fillBall(field, core::DVec3{64.0, 2.0, 20.0}, 9.0, 1);
    struct Pair
    {
        TerrainNodeKey fine;
        TerrainSides sides;
        TerrainNodeKey coarse;
        float seam;
    };
    const std::vector<Pair> pairs{
        // Coarse on the high side, then on the low side.
        {TerrainNodeKey{0, 1, 0}, TerrainSides{0, 1, 0, 0}, TerrainNodeKey{1, 1, 0}, 64.0f},
        {TerrainNodeKey{0, 2, 0}, TerrainSides{1, 0, 0, 0}, TerrainNodeKey{1, 0, 0}, 64.0f},
        // Two levels apart.
        {TerrainNodeKey{0, 3, 0}, TerrainSides{0, 2, 0, 0}, TerrainNodeKey{2, 1, 0}, 128.0f},
    };
    for (const Pair& pair : pairs) {
        CAPTURE(pair.fine.x);
        CAPTURE(pair.coarse.level);
        std::vector<Triangle> triangles;
        gather(meshTerrainNode(field, pair.fine, pair.sides), triangles);
        gather(meshTerrainNode(field, pair.coarse), triangles);
        REQUIRE_FALSE(triangles.empty());
        const auto corner = static_cast<float>(1 << pair.coarse.level) + 1.0f;
        CHECK(seeThrough(triangles, pair.seam, corner, 32.0f - corner, 30.0f) == 0);
    }
}

namespace {

// Levels meeting at the corner x 128, z 128 (ADR 0140): three level-0 nodes
// round it and a level-2 node only on its diagonal, coarser than either side.
struct Corner
{
    asset::TerrainField field{asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f}};
    std::vector<TerrainNodeKey> drawn{
        TerrainNodeKey{0, 3, 3},
        TerrainNodeKey{0, 4, 3},
        TerrainNodeKey{0, 3, 4},
        TerrainNodeKey{2, 1, 1},
    };

    Corner()
    {
        (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 2048.0f, 3.4f, 1);
        // Slopes across both seams and over the corner, so the levels disagree.
        for (const core::DVec3 at :
             {core::DVec3{128.0, -6.0, 128.0}, core::DVec3{128.0, -8.0, 100.0}, core::DVec3{100.0, -8.0, 128.0}})
            (void)asset::fillBall(field, at, 22.0, 1);
        std::sort(drawn.begin(), drawn.end());
    }

    [[nodiscard]] asset::TerrainMesh mesh(TerrainNodeKey key) const
    {
        return meshTerrainNode(field, key, terrainStitchSides(drawn, key));
    }
};

// Rays down onto the ground round `(x, z)`, from every bearing, steep and
// shallow: how many see through. Only the triangles within reach are tried.
[[nodiscard]] int seeThroughAround(const std::vector<Triangle>& triangles, float x, float z, float top)
{
    constexpr float Reach = 6.0f;
    constexpr float Pi = 3.14159265f;
    std::vector<Triangle> near;
    for (const Triangle& t : triangles) {
        const float cx = (t.a.x + t.b.x + t.c.x) / 3.0f;
        const float cz = (t.a.z + t.b.z + t.c.z) / 3.0f;
        if (std::abs(cx - x) < Reach + 70.0f && std::abs(cz - z) < Reach + 70.0f)
            near.push_back(t);
    }
    int through = 0;
    for (float dz = -Reach; dz <= Reach; dz += 0.43f) {
        for (float dx = -Reach; dx <= Reach; dx += 0.43f) {
            for (const float elevation : {0.7f, 1.1f, 1.5f}) {
                for (int bearing = 0; bearing < 12; ++bearing) {
                    const float angle = static_cast<float>(bearing) * Pi / 6.0f + 0.1f;
                    const core::Vec3 direction{std::cos(elevation) * std::cos(angle), -std::sin(elevation),
                                               std::cos(elevation) * std::sin(angle)};
                    // Aimed at a point on the corner's level, from far enough
                    // back to come down from above everything.
                    const core::Vec3 aim{x + dx, 0.0f, z + dz};
                    const float back = (top + 40.0f) / std::sin(elevation);
                    const core::Vec3 from{aim.x - direction.x * back, aim.y - direction.y * back,
                                          aim.z - direction.z * back};
                    const bool met = std::any_of(near.begin(), near.end(),
                                                 [&](const Triangle& t) { return meetsFront(from, direction, t); });
                    through += met ? 0 : 1;
                }
            }
        }
    }
    return through;
}

} // namespace

TEST_CASE("where three levels meet at a corner, the corner is closed")
{
    // The corner's cell is shared by four nodes and takes the coarsest: the
    // level-0 node there was stitched only to its sides, and a slit opened at
    // the corner where the diagonal node was coarser. The terrain gallery
    // showed it (`terrain_gallery_holes`); from above, the diagonal node's own
    // ring covers the corner, so here this guards the corner rather than
    // reproducing that slit.
    const Corner corner;
    std::vector<Triangle> triangles;
    for (const TerrainNodeKey key : corner.drawn)
        gather(corner.mesh(key), triangles);
    REQUIRE_FALSE(triangles.empty());
    // From every side, onto the corner and the seams near it.
    CHECK(seeThroughAround(triangles, 128.0f, 128.0f, 30.0f) == 0);
}

TEST_CASE("a seam's vertices slide the same in every node that draws them, whatever each node's range")
{
    // **The geomorph opened the seams** (ADR 0140): each node slid its
    // vertices over its own range, the vertices on a seam are drawn by every
    // node there, and nodes chosen by their own error slide over different
    // ranges -- so a seam's vertex was in two places while it slid, and the
    // gallery showed a crack along every seam. A vertex now slides over the
    // smallest range of the nodes of its level that draw it, which every one
    // of them works out alike.
    const Corner corner;
    // A different range for every node.
    const auto rangeOf = [](TerrainNodeKey key) -> std::array<core::f32, 2> {
        const core::i32 turn = (key.x + 2 * key.z) % 3;
        const core::i32 width = (key.x + key.z) % 2;
        const auto start = static_cast<core::f32>(20 + 13 * static_cast<core::i32>(key.level) + 5 * turn);
        return {start, start + static_cast<core::f32>(40 + 9 * width)};
    };
    for (const core::Vec3 camera : {core::Vec3{128.0f, 50.0f, 128.0f}, core::Vec3{70.0f, 30.0f, 170.0f}}) {
        CAPTURE(camera.x);
        struct Seen
        {
            core::Vec3 slid;
            TerrainNodeKey by;
        };
        std::map<std::array<core::f32, 3>, Seen> seen;
        int shared = 0;
        int sliding = 0;
        for (const TerrainNodeKey key : corner.drawn) {
            const asset::TerrainMesh mesh = corner.mesh(key);
            REQUIRE(mesh.morphs.size() == mesh.mesh.vertices.size());
            REQUIRE(mesh.morphTags.size() == mesh.mesh.vertices.size());
            const TerrainMorph morph = terrainMorphOf(corner.drawn, key, rangeOf);
            for (core::usize at = 0; at < mesh.mesh.vertices.size(); ++at) {
                const core::Vec3 position = mesh.mesh.vertices[at].position;
                const core::f32 distance = core::length(minus(position, camera));
                const core::Vec3 slid = terrainSlid(position, mesh.morphs[at], mesh.morphTags[at], morph, distance);
                sliding += core::length(minus(slid, position)) > 1e-4f ? 1 : 0;
                const auto [found, fresh] =
                    seen.try_emplace(std::array<core::f32, 3>{position.x, position.y, position.z}, Seen{slid, key});
                if (fresh || found->second.by == key)
                    continue;
                ++shared;
                CAPTURE(position.x);
                CAPTURE(position.y);
                CAPTURE(position.z);
                CHECK(core::length(minus(slid, found->second.slid)) < 1e-4f);
            }
        }
        // The case is a real one: vertices shared across nodes, and sliding.
        CHECK(shared > 50);
        CHECK(sliding > 50);
    }
}
