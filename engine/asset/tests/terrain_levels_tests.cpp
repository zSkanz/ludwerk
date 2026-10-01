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
// ADR 0140's gathered levels took it off the shapes, flat ground and the plate.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <functional>
#include <limits>
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

TEST_CASE("every shape of the gallery still has triangles at every level")
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

TEST_CASE("flat ground at a quarter-metre height is within 2 cm at every level")
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

TEST_CASE("the top of a filled block is flat: every vertex on it, every normal up")
{
    // The audit's photos/08 showed streaks on flat ground at a low sun, and a
    // lamp dark notches, at full detail too. Not the mesh: the top is flat to
    // the vertex and the normal, borders of the region included (D372 was the
    // grain's noise). Kept, so a mesher change that dents flat ground says so.
    TerrainField field(settingsOf());
    (void)fillBlock(field, {0.0, 2.0, 0.0}, {160.0f, 4.0f, 160.0f}, 1);
    for (core::u32 level = 0; level <= 1; ++level) {
        CAPTURE(level);
        const TerrainMesh meshed = meshAt(field, level, 64, -8, 12);
        float worstHeight = 0.0f;
        float lowestUp = 1.0f;
        int seen = 0;
        for (const Vertex& vertex : meshed.mesh.vertices) {
            if (vertex.position.y < 3.0f || std::abs(vertex.position.x) > 70.0f || std::abs(vertex.position.z) > 70.0f)
                continue;
            worstHeight = std::max(worstHeight, std::abs(vertex.position.y - 4.0f));
            lowestUp = std::min(lowestUp, vertex.normal.y);
            ++seen;
        }
        REQUIRE(seen > 0);
        CHECK(worstHeight < 0.01f);
        CHECK(lowestUp > 0.99f);
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

TEST_CASE("ground under a plate a coarse level does not draw is not darkened by it")
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

TEST_CASE("paint that became the ground, beside paint that has not, shows the paint across the line (D396)")
{
    // Sand blended over grass at full strength becomes sand ground, with
    // nothing over it; beside it, at nine tenths, it stays grass with sand
    // over. A vertex gathering both averaged the first as a cover of nothing:
    // along the line the cover halved and the grass showed through.
    TerrainField field(settingsOf());
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 64.0f, 0.0f, 1);
    PaintOptions blend;
    blend.mode = PaintMode::Blend;
    blend.strength = 1.0f;
    (void)paintBall(field, core::DVec3{0.0, 0.0, 0.0}, 6.0, 2, blend);
    blend.strength = 0.9f;
    (void)paintBall(field, core::DVec3{8.0, 0.0, 0.0}, 6.0, 2, blend);
    for (core::u32 level = 0; level <= 1; ++level) {
        CAPTURE(level);
        const TerrainMesh meshed = meshAt(field, level, 32, -8, 8);
        int seen = 0;
        for (const Vertex& vertex : meshed.mesh.vertices) {
            // The line and a metre or two either side of it, well inside both.
            if (vertex.normal.y < 0.9f || vertex.position.x < 4.0f || vertex.position.x > 8.0f ||
                std::abs(vertex.position.z) > 2.0f)
                continue;
            ++seen;
            const auto corner = static_cast<core::u32>(vertex.uv[1] + 0.5f);
            const auto unpack = [corner](float packed) {
                return (static_cast<core::u32>(packed + 0.5f) >> (8u * corner)) & 0xFFu;
            };
            const core::u32 ground = unpack(vertex.uv[0]);
            const core::u32 top = unpack(vertex.tangent[2]);
            const core::u32 cover = unpack(vertex.tangent[3]);
            CAPTURE(vertex.position.x);
            CAPTURE(ground);
            CAPTURE(top);
            CAPTURE(cover);
            CHECK((ground == 2u || (top == 2u && cover >= 200u)));
        }
        REQUIRE(seen > 0);
    }
}

TEST_CASE("paint on the ground survives every level")
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
    (void)paintBall(field, core::DVec3{0.0, 0.0, 0.0}, 40.0, 2, over);
    for (core::u32 level = 0; level <= TopLevel; ++level) {
        CAPTURE(level);
        const TerrainMesh meshed = meshAt(field, level, 96, -40, 16);
        int painted = 0;
        int seen = 0;
        // Seventeen metres: a level-5 cell is 32 across, its vertex at the
        // middle of the ground it gathers.
        eachTopVertex(meshed, 17.0f, [&](const Vertex& vertex) {
            ++seen;
            // The three corners' paint packed, and which corner this is.
            const auto tops = static_cast<core::u32>(vertex.tangent[2] + 0.5f);
            const auto corner = static_cast<core::u32>(vertex.uv[1] + 0.5f);
            if (((tops >> (8u * corner)) & 0xFFu) == 2u)
                ++painted;
        });
        REQUIRE(seen > 0);
        CHECK(painted == seen);
    }
}

TEST_CASE("the ground under a plate sees the sky round its edges: air under an overhang is air (TA11)")
{
    // The sky term's column map was one span from a column's bottom to its
    // top, so the air under a plate counted as rock and the ground there was
    // black whatever lay beside it.
    TerrainField field(settingsOf());
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
    (void)fillBlock(field, core::DVec3{0.0, 10.5, 0.0}, core::Vec3{24.0f, 1.0f, 24.0f}, 1);
    for (const core::u32 level : {0u, 1u}) {
        CAPTURE(level);
        const TerrainMesh meshed = meshAt(field, level, 96, -40, 32);
        float lightest = 0.0f;
        int seen = 0;
        eachTopVertex(meshed, 4.0f, [&](const Vertex& vertex) {
            if (vertex.position.y > 2.0f)
                return;
            lightest = std::max(lightest, vertex.tangent[1]);
            ++seen;
        });
        REQUIRE(seen > 0);
        CHECK(lightest > 0.15f);
        CHECK(lightest < 0.9f);
    }
}

TEST_CASE("a node slid all the way is its parent's surface: the geomorph's targets are the level above's vertices")
{
    // ADR 0140: a node about to give way to its parent has slid onto it, so
    // the swap changes no pixel. Every vertex's target is a vertex the parent
    // draws.
    TerrainField field(settingsOf());
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
    (void)fillBall(field, core::DVec3{0.5, 0.0, 0.5}, 14.0, 1);
    for (const core::u32 level : {0u, 1u, 2u}) {
        CAPTURE(level);
        const TerrainMesh child = meshAt(field, level, 32, -40, 24);
        const TerrainMesh parent = meshAt(field, level + 1, 64, -40, 24);
        REQUIRE(child.morphs.size() == child.mesh.vertices.size());
        std::vector<core::Vec3> targets;
        for (const Vertex& vertex : parent.mesh.vertices)
            targets.push_back(vertex.position);
        int missed = 0;
        int checked = 0;
        for (std::size_t at = 0; at < child.mesh.vertices.size(); ++at) {
            const core::Vec3 p = child.mesh.vertices[at].position;
            if (std::abs(p.x) > 24.0f || std::abs(p.z) > 24.0f)
                continue;
            const core::Vec3 slid{p.x + child.morphs[at].x, p.y + child.morphs[at].y, p.z + child.morphs[at].z};
            ++checked;
            const bool found = std::any_of(targets.begin(), targets.end(), [&](const core::Vec3& t) {
                return std::abs(t.x - slid.x) < 1e-3f && std::abs(t.y - slid.y) < 1e-3f &&
                       std::abs(t.z - slid.z) < 1e-3f;
            });
            missed += found ? 0 : 1;
        }
        REQUIRE(checked > 0);
        CHECK(missed == 0);
    }
}

TEST_CASE("a run of ground nothing closes ends at its column's own top, not the sky")
{
    // **The owner's place at 500 m**: under the bulge of a mound a column held
    // the underside, facing down, while the wall above it was too steep to
    // count and the cap lay in the next column -- and the run went up to the
    // sky, a pillar every ray past it met: a row of dark streaks down the flank.
    const float nothing = std::numeric_limits<float>::quiet_NaN();
    // An underside at 5 m, the wall above it steep, the column's own top at 9.
    const auto pillar = terrainColumnRuns({{5.0f, -0.7f}, {7.0f, 0.1f}}, 9.0f, 5.0f);
    REQUIRE(pillar.size() == 1);
    CHECK(pillar.front().first == 5.0f);
    CHECK(pillar.front().second == 9.0f);
    // With nothing of the column above it, it is the ground to the top of the
    // world, as it was.
    const auto open = terrainColumnRuns({{5.0f, -0.7f}}, 5.0f, 5.0f);
    REQUIRE(open.size() == 1);
    CHECK(open.front().second > 1.0e29f);
    // The ordinary cases stand: ground below a surface facing up, and air under
    // an overhang between two runs.
    const auto ground = terrainColumnRuns({{3.0f, 0.9f}}, 3.0f, 3.0f);
    REQUIRE(ground.size() == 1);
    CHECK(ground.front().first < -1.0e29f);
    CHECK(ground.front().second == 3.0f);
    const auto overhang = terrainColumnRuns({{0.0f, 0.9f}, {10.0f, -0.9f}, {11.0f, 0.9f}}, 11.0f, nothing);
    REQUIRE(overhang.size() == 2);
    CHECK(overhang[1].first == 10.0f);
    CHECK(overhang[1].second == 11.0f);
}

TEST_CASE("the upper flank of a ball sees the sky at a coarse level, wherever in its column a vertex falls")
{
    // **The owner's place at 1 000 m**: a coarse column is a whole cell -- 8 m
    // at level 3 -- and its run is the whole ball as that column sees it. A
    // vertex on the ball fell inside that run, or not, by where in the cell it
    // was: counted as under a roof, and its sideways rays blocked by the
    // column they started in, it went dark in blocks.
    TerrainField field(settingsOf());
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
    (void)fillBall(field, core::DVec3{0.0, 2.0, 0.0}, 14.0, 1);
    for (const core::u32 level : {2u, 3u}) {
        CAPTURE(level);
        const TerrainMesh meshed = meshAt(field, level, 96, -40, 32);
        float darkest = 1.0f;
        int seen = 0;
        for (const Vertex& vertex : meshed.mesh.vertices) {
            // The upper flank, facing out and up.
            if (vertex.normal.y < 0.0f || vertex.normal.y > 0.6f || vertex.position.y < 3.0f)
                continue;
            darkest = std::min(darkest, vertex.tangent[1]);
            ++seen;
        }
        REQUIRE(seen > 0);
        // Measured: 0.58 and 0.62 before, 0.70 and 0.77 after -- a flank that
        // faces out sees the ground in front of it with the lower half of what
        // it faces.
        CHECK(darkest > 0.66f);
    }
}
