// **The terrain at every level of detail, L0 to L5** (terrain audit T0).
//
// Until this file one mesher test built one coarse level -- L1, flat ground
// and a ball, to 2 m -- and nothing built L2 to L5, which is where the owner's
// ground vanished, stepped and pocked at a distance. Each case here is a
// promise the audit's bar makes, asked of every level: the shapes it found
// broken, the ground they sit on, and what the mesher says about them.
//
// **A case that proves a defect not yet fixed is marked `should_fail`**, so the
// suite stays green while it waits and the fix that makes it pass has to take
// the mark off (the ledger, `docs/briefs/terrain-audit-2026-09-29-full.md`).
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <functional>
#include <string>
#include <vector>

#include "engine/asset/terrain_mesher.h"

using namespace engine;
using namespace engine::asset;

namespace {

constexpr core::u32 TopLevel = 5;

[[nodiscard]] FieldSettings settingsOf()
{
    return FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f};
}

// **The region round `[-extent, extent)` metres across and `[low, high)` up**,
// at `level`: what a node of that level meshes, cut to the shape under test.
[[nodiscard]] TerrainMesh meshAt(const TerrainField& field, core::u32 level, core::i32 extent, core::i32 low,
                                 core::i32 high)
{
    const core::i32 step = 1 << level;
    const auto down = [step](core::i32 v) { return v >= 0 ? v / step : -((-v + step - 1) / step); };
    MeshRegion region;
    region.level = level;
    region.minX = down(-extent);
    region.minZ = down(-extent);
    region.minY = down(low);
    region.cellsX = static_cast<core::u32>(std::max(2, down(extent) - region.minX + 1));
    region.cellsZ = region.cellsX;
    region.cellsY = static_cast<core::u32>(std::max(2, down(high) - region.minY + 1));
    return meshField(field, region);
}

struct Shape
{
    const char* name;
    std::function<void(TerrainField&)> build;
    // The box it fills, in metres, to mesh round.
    core::i32 extent;
    core::i32 low;
    core::i32 high;
};

[[nodiscard]] std::vector<Shape> gallery()
{
    return {
        {"a 2 m slab", [](TerrainField& f) { (void)fillBlock(f, {0.0, 1.0, 0.0}, {96.0f, 2.0f, 96.0f}, 1); }, 64, -8,
         12},
        {"a 4 m slab", [](TerrainField& f) { (void)fillBlock(f, {0.0, 2.0, 0.0}, {96.0f, 4.0f, 96.0f}, 1); }, 64, -8,
         12},
        {"a ball of radius 2", [](TerrainField& f) { (void)fillBall(f, {0.5, 8.5, 0.5}, 2.0, 1); }, 16, 0, 16},
        {"a ball of radius 4", [](TerrainField& f) { (void)fillBall(f, {0.5, 8.5, 0.5}, 4.0, 1); }, 16, 0, 16},
        {"a ball of radius 8", [](TerrainField& f) { (void)fillBall(f, {0.5, 16.5, 0.5}, 8.0, 1); }, 32, 0, 32},
        {"two merged balls",
         [](TerrainField& f) {
             (void)fillBall(f, {-3.0, 8.5, 0.5}, 4.0, 1);
             (void)fillBall(f, {3.0, 8.5, 0.5}, 4.0, 1);
         },
         16, 0, 16},
    };
}

// Up-facing vertices well inside `[-inner, inner]` on both axes.
template <typename Visit>
void eachTopVertex(const TerrainMesh& meshed, float inner, Visit visit)
{
    for (const Vertex& vertex : meshed.mesh.vertices) {
        if (vertex.normal.y < 0.9f || std::abs(vertex.position.x) > inner || std::abs(vertex.position.z) > inner)
            continue;
        visit(vertex);
    }
}

} // namespace

TEST_CASE("every shape of the gallery still has triangles at every level" * doctest::should_fail())
{
    // TA1: a coarse cell's occupancy is the mean of the cube under it, so a
    // shape thinner than about half a cell falls under the threshold and is
    // not drawn at all -- the owner's ground gone at 512 m.
    for (const Shape& shape : gallery()) {
        TerrainField field(settingsOf());
        shape.build(field);
        for (core::u32 level = 0; level <= TopLevel; ++level) {
            CAPTURE(shape.name);
            CAPTURE(level);
            const TerrainMesh meshed = meshAt(field, level, shape.extent, shape.low, shape.high);
            CHECK_FALSE(meshed.mesh.indices.empty());
        }
    }
}

TEST_CASE("flat ground at a quarter-metre height is within 2 cm at every level" * doctest::should_fail())
{
    // TA2: a linear crossing between two coarse samples is biased, by up to
    // 2.7 m at L5 -- steps at every seam and a node that jumps in height when
    // its level changes.
    for (const float height : {3.0f, 3.25f, 3.5f, 3.75f}) {
        TerrainField field(settingsOf());
        (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, height, 1);
        for (core::u32 level = 0; level <= TopLevel; ++level) {
            CAPTURE(height);
            CAPTURE(level);
            const TerrainMesh meshed = meshAt(field, level, 96, -40, 16);
            float worst = 0.0f;
            int seen = 0;
            eachTopVertex(meshed, 64.0f, [&](const Vertex& vertex) {
                worst = std::max(worst, std::abs(vertex.position.y - height));
                ++seen;
            });
            REQUIRE(seen > 0);
            CHECK(worst < 0.02f);
        }
    }
}

TEST_CASE("a coarse cell of a thick block takes the block's material")
{
    // What already holds: a coarse cell wholly inside one material is that
    // material. The case TA7 breaks is below.
    TerrainField field(settingsOf());
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
    (void)fillBlock(field, core::DVec3{0.0, 8.0, 0.0}, core::Vec3{48.0f, 16.0f, 48.0f}, 2);
    for (core::u32 level = 1; level <= 3; ++level) {
        CAPTURE(level);
        const TerrainMesh meshed = meshAt(field, level, 64, -40, 32);
        int wrong = 0;
        int seen = 0;
        eachTopVertex(meshed, 16.0f, [&](const Vertex& vertex) {
            if (vertex.position.y < 12.0f)
                return;
            ++seen;
            if (static_cast<int>(vertex.tangent[0] + 0.5f) != 2)
                ++wrong;
        });
        REQUIRE(seen > 0);
        CHECK(wrong == 0);
    }
}

TEST_CASE("open flat ground sees the whole sky at every level")
{
    // The sky term's promise on the one surface nothing shades.
    TerrainField field(settingsOf());
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
    for (core::u32 level = 0; level <= TopLevel; ++level) {
        CAPTURE(level);
        const TerrainMesh meshed = meshAt(field, level, 96, -40, 16);
        float darkest = 1.0f;
        eachTopVertex(meshed, 48.0f, [&](const Vertex& vertex) { darkest = std::min(darkest, vertex.tangent[1]); });
        CHECK(darkest > 0.99f);
    }
}

TEST_CASE("ground under a plate a coarse level does not draw is not darkened by it" * doctest::should_fail())
{
    // TA3: the sky term was computed from the level-0 columns and applied to
    // coarse vertices, so a feature the coarse level lost still shaded the
    // ground under it -- a dark imprint under nothing.
    TerrainField field(settingsOf());
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
    (void)fillBlock(field, core::DVec3{0.0, 10.5, 0.0}, core::Vec3{24.0f, 1.0f, 24.0f}, 1);
    const core::u32 level = 4;
    const TerrainMesh meshed = meshAt(field, level, 96, -40, 32);
    // Whether the level drew the plate at all.
    bool plate = false;
    for (const Vertex& vertex : meshed.mesh.vertices)
        plate = plate || vertex.position.y > 6.0f;
    float darkest = 1.0f;
    eachTopVertex(meshed, 8.0f, [&](const Vertex& vertex) {
        if (vertex.position.y < 2.0f)
            darkest = std::min(darkest, vertex.tangent[1]);
    });
    // Drawn, the plate may shade the ground; not drawn, it must not.
    if (!plate)
        CHECK(darkest > 0.99f);
}

TEST_CASE("paint on the ground survives every level" * doctest::should_fail())
{
    // TA7: a coarse cell took its materials from its fullest voxel, the first
    // met walking up from the bottom -- a deep one nobody painted, so paint
    // vanished from L2 on.
    TerrainField field(settingsOf());
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
    // Painted over, half-way, as a brush does: the ground stays grass and sand
    // shows on it. At full strength a blend is simply the new ground.
    PaintOptions over;
    over.mode = PaintMode::Blend;
    over.strength = 0.5f;
    (void)paintBall(field, core::DVec3{0.0, 0.0, 0.0}, 24.0, 2, over);
    for (core::u32 level = 0; level <= TopLevel; ++level) {
        CAPTURE(level);
        const TerrainMesh meshed = meshAt(field, level, 96, -40, 16);
        int painted = 0;
        int seen = 0;
        eachTopVertex(meshed, 12.0f, [&](const Vertex& vertex) {
            ++seen;
            const auto tops = static_cast<core::u32>(vertex.tangent[2] + 0.5f);
            if ((tops & 0xFFu) == 2u)
                ++painted;
        });
        REQUIRE(seen > 0);
        CHECK(painted == seen);
    }
}
