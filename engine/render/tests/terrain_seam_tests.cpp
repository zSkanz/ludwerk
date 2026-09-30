// **Where two levels of detail meet** (terrain audit T0, TA2): the ground a
// fine node draws beside a coarse one, looked at along a grid of rays.
//
// A seam is judged the way a camera judges it. Rays come down at an angle
// across the line where the two nodes meet, from either side, and each must
// meet the front of some triangle the two draw -- the fine node's surface, its
// skirt on the side against the coarse node, or the coarse node's surface. A
// ray that meets none has seen the sky through the ground: the owner's
// "recorte".
//
// **Expected to fail until T2**: the one skirt hangs from the fine side only,
// downwards, so where the coarse side is the higher the gap is open.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
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

// The triangles a node draws: its surface, and its skirt on the sides in
// `skirts` (1 low x, 2 high x, 4 low z, 8 high z) -- as `extract` chooses them.
void gather(const asset::TerrainMesh& mesh, core::u8 skirts, std::vector<Triangle>& out)
{
    for (core::usize section = 0; section < mesh.mesh.submeshes.size(); ++section) {
        const core::u8 side = section < mesh.sectionSides.size() ? mesh.sectionSides[section] : core::u8{0};
        if (side != 0 && (side & skirts) == 0)
            continue;
        const asset::Submesh& sub = mesh.mesh.submeshes[section];
        for (core::u32 at = 0; at + 2 < sub.indexCount; at += 3) {
            const auto& vertices = mesh.mesh.vertices;
            const auto& indices = mesh.mesh.indices;
            out.push_back(Triangle{vertices[indices[sub.firstIndex + at]].position,
                                   vertices[indices[sub.firstIndex + at + 1]].position,
                                   vertices[indices[sub.firstIndex + at + 2]].position});
        }
    }
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
    (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 512.0f, 3.4f, 1);
    (void)asset::fillBall(field, core::DVec3{64.0, -6.0, 16.0}, 22.0, 1);
    std::vector<Triangle> triangles;
    gather(meshTerrainNode(field, TerrainNodeKey{0, 1, 0}), 0, triangles);
    gather(meshTerrainNode(field, TerrainNodeKey{0, 2, 0}), 0, triangles);
    CHECK(seeThrough(triangles, 64.0f, 1.0f, 31.0f, 30.0f) == 0);
}

TEST_CASE("a fine node and the coarse one beside it leave no gap, on flat ground and on a hill" *
          doctest::should_fail())
{
    asset::TerrainField field(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 512.0f, 3.4f, 1);
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
        gather(meshTerrainNode(field, fine), 2, triangles);
        gather(meshTerrainNode(field, coarse), 0, triangles);
        REQUIRE_FALSE(triangles.empty());
        const float seam = static_cast<float>(2 * width) * 32.0f;
        CHECK(seeThrough(triangles, seam, 1.0f, static_cast<float>(width) * 32.0f - 1.0f, 30.0f) == 0);
    }
}
