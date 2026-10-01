// **The terrain mesh, proved rather than looked at** (the owner's terrain
// report, 2026-09-23).
//
// A second opinion drawn from a video listed what the interior of the terrain
// looked like -- black regions, surfaces vanishing at some angles, stretched
// faces, possible cracks -- and asked for each hypothesis to be checked against
// the implementation rather than believed: inverted winding, degenerate or
// duplicated triangles, holes, seams between chunks, normals that disagree with
// their faces. Every one of those is a property of the triangles, so every one
// is asserted here, over the controlled cases the report named: flat ground, a
// slope, a hill, a ball added and a ball taken away, a tunnel, a cave, a
// vertical wall, an overhang and a ball across a chunk boundary.
//
// The surface of a field whose ground is all inside the meshed box is closed,
// so the whole of it is meshed region by region -- exactly as the renderer and
// the colliders cut it -- and the union is checked as one surface.
#include <algorithm>
#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "engine/asset/terrain_mesher.h"

using namespace engine;
using namespace engine::asset;

namespace {

[[nodiscard]] FieldSettings settingsOf()
{
    return FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f};
}

// A slab of ground 40 m square and 16 m deep, top at zero, well inside the box
// the checks mesh -- so every surface it has, walls and bottom included, is
// closed.
[[nodiscard]] TerrainField slab()
{
    TerrainField field(settingsOf());
    (void)fillBlock(field, core::DVec3{0.0, -8.0, 0.0}, core::Vec3{40.0f, 16.0f, 40.0f}, 1);
    return field;
}

using Point = std::tuple<float, float, float>;

[[nodiscard]] Point snap(const core::Vec3& p)
{
    const auto at = [](float v) { return std::round(v * 1024.0f) / 1024.0f; };
    return {at(p.x), at(p.y), at(p.z)};
}

[[nodiscard]] core::Vec3 sub(const core::Vec3& a, const core::Vec3& b)
{
    return core::Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

struct Findings
{
    std::size_t triangles = 0;
    std::size_t degenerate = 0;
    std::size_t duplicated = 0;
    // Edges used by other than exactly two triangles: a hole (one) or a fin
    // (three or more).
    std::size_t openEdges = 0;
    std::size_t overusedEdges = 0;
    // Edges whose two triangles traverse them in the same direction, which is
    // what one triangle wound backwards against its neighbour looks like.
    std::size_t inconsistentEdges = 0;
    // Triangles whose winding faces away from the normals of their own
    // vertices, which is what "inside out" looks like to the shader.
    std::size_t facingAway = 0;
    std::size_t badNormals = 0;
    // The worst triangle's longest side over its height to that side: 1.15 for
    // an equilateral one, and large for a sliver.
    float worstAspect = 0.0f;
    // How many triangles are slivers by that measure (over 12).
    std::size_t slivers = 0;
};

// Meshes every region of the box [-64, 64) on x and z and [-96, 64) on y, 32
// lattice points a side, and checks the union. Below the world's floor on y,
// as the renderer's runs are: ground laid down to the floor has its bottom
// between the floor's voxel and the one under it.
[[nodiscard]] Findings check(const TerrainField& field)
{
    Findings found;
    std::map<std::tuple<Point, Point, Point>, int> seen;
    // Directed edge -> uses; the undirected count is the two directions summed.
    std::map<std::pair<Point, Point>, int> directed;

    for (core::i32 z = -64; z < 64; z += 32) {
        for (core::i32 y = -96; y < 64; y += 32) {
            for (core::i32 x = -64; x < 64; x += 32) {
                const MeshRegion region{
                    .minX = x, .minY = y, .minZ = z, .cellsX = 32, .cellsY = 32, .cellsZ = 32, .level = 0};
                const TerrainMesh meshed = meshField(field, region);
                const std::vector<Vertex>& vertices = meshed.mesh.vertices;
                const std::vector<core::u32>& indices = meshed.mesh.indices;
                for (const Vertex& vertex : vertices) {
                    if (std::abs(core::length(vertex.normal) - 1.0f) > 1e-3f)
                        ++found.badNormals;
                }
                for (std::size_t at = 0; at + 2 < indices.size(); at += 3) {
                    const Vertex& a = vertices[indices[at]];
                    const Vertex& b = vertices[indices[at + 1]];
                    const Vertex& c = vertices[indices[at + 2]];
                    ++found.triangles;

                    const core::Vec3 face = core::cross(sub(b.position, a.position), sub(c.position, a.position));
                    if (core::length(face) < 1e-6f) {
                        ++found.degenerate;
                    }
                    else {
                        const float ab = core::length(sub(b.position, a.position));
                        const float bc = core::length(sub(c.position, b.position));
                        const float ca = core::length(sub(a.position, c.position));
                        const float longest = std::max({ab, bc, ca});
                        const float aspect = longest * longest / core::length(face);
                        found.worstAspect = std::max(found.worstAspect, aspect);
                        if (aspect > 12.0f)
                            ++found.slivers;
                        const core::Vec3 average{a.normal.x + b.normal.x + c.normal.x,
                                                 a.normal.y + b.normal.y + c.normal.y,
                                                 a.normal.z + b.normal.z + c.normal.z};
                        if (core::dot(face, average) <= 0.0f)
                            ++found.facingAway;
                    }

                    std::array<Point, 3> corners{snap(a.position), snap(b.position), snap(c.position)};
                    for (int e = 0; e < 3; ++e)
                        directed[{corners[static_cast<std::size_t>(e)],
                                  corners[static_cast<std::size_t>((e + 1) % 3)]}] += 1;
                    std::sort(corners.begin(), corners.end());
                    if (++seen[{corners[0], corners[1], corners[2]}] == 2)
                        ++found.duplicated;
                }
            }
        }
    }

    for (const auto& [edge, uses] : directed) {
        if (edge.first > edge.second)
            continue;
        const auto reverse = directed.find({edge.second, edge.first});
        const int back = reverse == directed.end() ? 0 : reverse->second;
        const int total = uses + back;
        if (total < 2)
            ++found.openEdges;
        else if (total > 2)
            ++found.overusedEdges;
        else if (uses != 1)
            ++found.inconsistentEdges;
    }
    for (const auto& [edge, uses] : directed) {
        if (edge.first < edge.second)
            continue;
        if (directed.find({edge.second, edge.first}) == directed.end()) {
            // Only this direction exists, so the loop above never saw it.
            if (uses < 2)
                ++found.openEdges;
            else if (uses > 2)
                ++found.overusedEdges;
            else
                ++found.inconsistentEdges;
        }
    }
    return found;
}

// `maxAspect`: the worst triangle allowed, by `Findings::worstAspect`. Six for
// every ordinary shape -- quads are split along their shorter diagonal, which
// took the worst of these cases from 6 to 3.6 -- and looser only where a test
// says why.
void expectClean(const std::string& what, const TerrainField& field, float maxAspect = 6.0f)
{
    const Findings found = check(field);
    CAPTURE(what);
    CAPTURE(found.triangles);
    CAPTURE(found.worstAspect);
    REQUIRE(found.triangles > 100);
    CHECK(found.worstAspect < maxAspect);
    // A sliver is rare where it happens at all: at most one triangle in five
    // hundred, which is where the crease cases below land.
    CHECK(found.slivers * 500 <= found.triangles);
    CHECK(found.degenerate == 0);
    CHECK(found.duplicated == 0);
    CHECK(found.openEdges == 0);
    CHECK(found.overusedEdges == 0);
    CHECK(found.inconsistentEdges == 0);
    CHECK(found.facingAway == 0);
    CHECK(found.badNormals == 0);
}

} // namespace

TEST_CASE("flat ground, walls and a bottom make one closed surface, wound one way")
{
    expectClean("flat", slab());
}

TEST_CASE("a slope is a closed surface, wound one way")
{
    TerrainField field(settingsOf());
    constexpr core::u32 Columns = 40;
    std::vector<float> heights(Columns * Columns);
    for (core::u32 z = 0; z < Columns; ++z) {
        for (core::u32 x = 0; x < Columns; ++x)
            heights[z * Columns + x] = 0.6f * (static_cast<float>(x) - 20.0f);
    }
    (void)writeHeights(field, -20, -20, Columns, heights, 1);
    expectClean("slope", field);
}

TEST_CASE("a hill, a ball added and a ball taken away are closed surfaces, wound one way")
{
    TerrainField hill = slab();
    (void)growBall(hill, core::DVec3{0.0, 0.0, 0.0}, 10.0, 2.0f);
    (void)growBall(hill, core::DVec3{0.0, 1.0, 0.0}, 10.0, 2.0f);
    expectClean("hill", hill);

    TerrainField added = slab();
    (void)fillBall(added, core::DVec3{4.0, 0.0, -3.0}, 5.0, 1);
    expectClean("ball added", added);

    TerrainField taken = slab();
    (void)fillBall(taken, core::DVec3{-3.0, 0.0, 5.0}, 5.0, 0);
    expectClean("ball taken away", taken);
}

TEST_CASE("a tunnel and a cave are closed surfaces, wound one way, inside as well as out")
{
    TerrainField tunnel = slab();
    for (int step = -24; step <= 0; step += 2)
        (void)fillBall(tunnel, core::DVec3{static_cast<double>(step), -6.0, 1.0}, 3.0, 0);
    expectClean("tunnel", tunnel);

    TerrainField cave = slab();
    (void)fillBall(cave, core::DVec3{2.0, -8.0, -2.0}, 5.5, 0);
    expectClean("cave", cave);
}

TEST_CASE("a vertical wall and an overhang are closed surfaces, wound one way")
{
    TerrainField wall(settingsOf());
    (void)fillBlock(wall, core::DVec3{-10.0, -4.0, 0.0}, core::Vec3{20.0f, 24.0f, 30.0f}, 1);
    expectClean("wall", wall);

    TerrainField overhang = slab();
    (void)fillBall(overhang, core::DVec3{20.0, -1.0, 0.0}, 4.0, 1);
    expectClean("overhang", overhang);
}

TEST_CASE("a ball across a chunk boundary is one surface, with no seam")
{
    TerrainField field = slab();
    // Centred on the corner of four chunks, where x, y and z all cross one.
    (void)fillBall(field, core::DVec3{0.0, 0.0, 0.0}, 6.0, 0);
    // And a ball of ground floated inside that hole: a union of two shapes,
    // whose field is the larger of two ramps and so has a crease where they
    // meet. Surface nets puts a cell's vertex at the mean of its crossings, and
    // along a crease those crowd, so a couple of triangles in twelve thousand
    // come out thin (an aspect near 27). Closed, wound one way and shaded by
    // the field's gradient all the same.
    (void)fillBall(field, core::DVec3{0.37, -0.61, 0.43}, 3.0, 1);
    expectClean("chunk boundary", field, 30.0f);
}

TEST_CASE("a surface through the voxel centres themselves is closed and wound, if thin in places")
{
    // **The one shape surface nets cannot give sound triangles, stated.** A
    // ball centred ON a voxel centre with a whole radius puts its surface
    // through lattice samples, so the crossings of four neighbouring cells meet
    // within a hundredth of a voxel of one point and their vertices crowd it:
    // slivers of a few centimetres, at an aspect near 28. The topology is
    // unharmed -- closed, wound one way, no duplicate -- and the normals come
    // from the field's gradient rather than from these triangles, so they
    // shade as the surface around them does. Moving the vertices apart would
    // move the surface off where the field puts it, which the collider and the
    // raycast would then disagree with; this records the cost instead.
    TerrainField field = slab();
    (void)fillBall(field, core::DVec3{0.0, 0.0, 0.0}, 6.0, 0);
    (void)fillBall(field, core::DVec3{0.5, -0.5, 0.5}, 3.0, 1);
    expectClean("through the lattice", field, 30.0f);
}

// --- Any sequence of brushes, at every level (terrain-editing ledger, P3) -----
//
// The owner, 2026-10-01: *"uma malha se cruzando formando um X ... passo um
// smooth e resolve"*. Nobody reproduced it by hand, so it is looked for the
// way such things are found: random sequences of every brush -- raises and
// digs at full strength, sharp boxes, smooths -- from fixed seeds, each
// ground meshed at three levels and every triangle checked against every
// triangle near it. A surface that crosses itself, or folds back over an
// edge, is what an X on the screen is.

namespace {

// splitmix64: the same sequence on every compiler, which the standard's
// distributions are not.
struct Dice
{
    core::u64 state;

    [[nodiscard]] core::u64 next() noexcept
    {
        core::u64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // In [low, high).
    [[nodiscard]] double range(double low, double high) noexcept
    {
        return low + (high - low) * (static_cast<double>(next() >> 11) / 9007199254740992.0);
    }
    [[nodiscard]] core::u32 below(core::u32 count) noexcept { return static_cast<core::u32>(next() % count); }
};

// A slab sculpted by `strokes` random brushes.
[[nodiscard]] TerrainField sculpted(core::u64 seed, int strokes)
{
    TerrainField field = slab();
    Dice dice{seed * 0x51ED27F1ull + 17};
    for (int stroke = 0; stroke < strokes; ++stroke) {
        const core::DVec3 at{dice.range(-14.0, 14.0), dice.range(-6.0, 6.0), dice.range(-14.0, 14.0)};
        const double radius = dice.range(2.0, 7.0);
        switch (dice.below(8)) {
        case 0:
            (void)growBall(field, at, radius, static_cast<float>(dice.range(1.0, 4.0)), 1);
            break;
        case 1:
            (void)growBall(field, at, radius, static_cast<float>(-dice.range(1.0, 4.0)));
            break;
        case 2:
            (void)fillBall(field, at, radius, 1);
            break;
        case 3:
            (void)fillBall(field, at, radius, 0);
            break;
        case 4:
            (void)smoothBall(field, at, radius, 1.0f);
            break;
        case 5:
            (void)flattenBall(field, at, radius, static_cast<float>(dice.range(-4.0, 4.0)), 1.0f);
            break;
        case 6:
            (void)raiseBall(field, at, radius, static_cast<float>(dice.range(-3.0, 3.0)), 1);
            break;
        default: {
            // A sharp box, added or cut: the edges a smooth vertex cannot sit on.
            const core::Vec3 size{static_cast<float>(dice.range(1.0, 6.0)), static_cast<float>(dice.range(1.0, 6.0)),
                                  static_cast<float>(dice.range(1.0, 6.0))};
            (void)fillBlock(field, at, size, dice.below(2) == 0 ? core::u8{1} : core::u8{0});
            break;
        }
        }
    }
    return field;
}

struct Triangle
{
    std::array<core::Vec3, 3> p;
};

[[nodiscard]] core::Vec3 add(const core::Vec3& a, const core::Vec3& b)
{
    return core::Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

// Whether the segment `a`-`b` passes through the inside of `t`: not its edges,
// not its corners, not the segment's own ends.
[[nodiscard]] bool pierces(const core::Vec3& a, const core::Vec3& b, const Triangle& t)
{
    constexpr float Inside = 1e-3f;
    const core::Vec3 direction = sub(b, a);
    const core::Vec3 e1 = sub(t.p[1], t.p[0]);
    const core::Vec3 e2 = sub(t.p[2], t.p[0]);
    const core::Vec3 h = core::cross(direction, e2);
    const float det = core::dot(e1, h);
    if (std::abs(det) < 1e-9f)
        return false;
    const float inverse = 1.0f / det;
    const core::Vec3 s = sub(a, t.p[0]);
    const float u = core::dot(s, h) * inverse;
    if (u < Inside || u > 1.0f - Inside)
        return false;
    const core::Vec3 q = core::cross(s, e1);
    const float v = core::dot(direction, q) * inverse;
    if (v < Inside || u + v > 1.0f - Inside)
        return false;
    const float along = core::dot(e2, q) * inverse;
    return along > Inside && along < 1.0f - Inside;
}

struct Crossed
{
    std::size_t triangles = 0;
    // Pairs of triangles that share no corner and pass through each other.
    std::size_t crossing = 0;
    // Edges whose two triangles face opposite ways: the surface folded flat.
    std::size_t folded = 0;
    core::Vec3 where{};
};

// The whole box of `check` as one region at `level`, every triangle against
// those near it.
[[nodiscard]] Crossed crossings(const TerrainField& field, core::u32 level)
{
    const core::i32 shift = static_cast<core::i32>(level);
    const MeshRegion region{.minX = -64 >> shift,
                            .minY = -96 >> shift,
                            .minZ = -64 >> shift,
                            .cellsX = 128u >> level,
                            .cellsY = 160u >> level,
                            .cellsZ = 128u >> level,
                            .level = level};
    prepareRegion(field, region);
    const TerrainMesh meshed = meshField(field, region);
    const std::vector<Vertex>& vertices = meshed.mesh.vertices;
    const std::vector<core::u32>& indices = meshed.mesh.indices;

    std::vector<Triangle> triangles;
    std::vector<core::Vec3> normals;
    std::map<std::pair<Point, Point>, std::vector<std::size_t>> byEdge;
    for (std::size_t at = 0; at + 2 < indices.size(); at += 3) {
        const Triangle t{
            {vertices[indices[at]].position, vertices[indices[at + 1]].position, vertices[indices[at + 2]].position}};
        const core::Vec3 face = core::cross(sub(t.p[1], t.p[0]), sub(t.p[2], t.p[0]));
        if (core::length(face) < 1e-6f)
            continue;
        const std::size_t index = triangles.size();
        triangles.push_back(t);
        normals.push_back(core::normalize(face));
        for (std::size_t e = 0; e < 3; ++e) {
            Point from = snap(t.p[e]);
            Point to = snap(t.p[(e + 1) % 3]);
            if (to < from)
                std::swap(from, to);
            byEdge[{from, to}].push_back(index);
        }
    }

    Crossed found;
    found.triangles = triangles.size();
    for (const auto& [edge, users] : byEdge) {
        if (users.size() == 2 && core::dot(normals[users[0]], normals[users[1]]) < -0.95f) {
            ++found.folded;
            found.where = triangles[users[0]].p[0];
        }
    }

    // A grid as coarse as the level's cells: a triangle is asked only of those
    // whose boxes share a cell with it.
    const float cell = static_cast<float>(1u << level) * 2.0f;
    std::map<std::tuple<int, int, int>, std::vector<std::size_t>> grid;
    const auto cellOf = [cell](float v) { return static_cast<int>(std::floor(v / cell)); };
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        const Triangle& t = triangles[index];
        const core::Vec3 low{std::min({t.p[0].x, t.p[1].x, t.p[2].x}), std::min({t.p[0].y, t.p[1].y, t.p[2].y}),
                             std::min({t.p[0].z, t.p[1].z, t.p[2].z})};
        const core::Vec3 high{std::max({t.p[0].x, t.p[1].x, t.p[2].x}), std::max({t.p[0].y, t.p[1].y, t.p[2].y}),
                              std::max({t.p[0].z, t.p[1].z, t.p[2].z})};
        for (int z = cellOf(low.z); z <= cellOf(high.z); ++z) {
            for (int y = cellOf(low.y); y <= cellOf(high.y); ++y) {
                for (int x = cellOf(low.x); x <= cellOf(high.x); ++x)
                    grid[{x, y, z}].push_back(index);
            }
        }
    }
    std::set<std::pair<std::size_t, std::size_t>> asked;
    for (const auto& [where, held] : grid) {
        for (std::size_t i = 0; i < held.size(); ++i) {
            for (std::size_t j = i + 1; j < held.size(); ++j) {
                const std::size_t a = std::min(held[i], held[j]);
                const std::size_t b = std::max(held[i], held[j]);
                if (!asked.insert({a, b}).second)
                    continue;
                const Triangle& ta = triangles[a];
                const Triangle& tb = triangles[b];
                bool shares = false;
                for (const core::Vec3& pa : ta.p) {
                    for (const core::Vec3& pb : tb.p)
                        shares = shares || snap(pa) == snap(pb);
                }
                if (shares)
                    continue;
                bool through = false;
                for (std::size_t e = 0; e < 3 && !through; ++e) {
                    through = pierces(ta.p[e], ta.p[(e + 1) % 3], tb) || pierces(tb.p[e], tb.p[(e + 1) % 3], ta);
                }
                if (through) {
                    ++found.crossing;
                    found.where = add(ta.p[0], core::Vec3{});
                }
            }
        }
    }
    return found;
}

} // namespace

TEST_CASE("no sequence of brushes leaves a surface that crosses itself or folds")
{
    constexpr int Seeds = 16;
    constexpr int Strokes = 24;
    for (core::u64 seed = 0; seed < Seeds; ++seed) {
        CAPTURE(seed);
        const TerrainField field = sculpted(seed, Strokes);

        // **At full detail, where somebody sculpts: one closed surface, wound
        // one way, that neither crosses itself nor folds.** A cell two sheets
        // pass through gives each its own vertex (`cellSheets`); before it, a
        // run like these left up to 27 edges shared by four faces and 35
        // triangles lit from behind.
        const Findings found = check(field);
        CAPTURE(found.triangles);
        REQUIRE(found.triangles > 100);
        CHECK(found.degenerate == 0);
        CHECK(found.duplicated == 0);
        CHECK(found.openEdges == 0);
        CHECK(found.inconsistentEdges == 0);
        CHECK(found.badNormals == 0);
        const Crossed fine = crossings(field, 0);
        CHECK(fine.crossing == 0);
        CHECK(fine.folded == 0);
        // **What is left, stated**: a neck thinner than a voxel between two
        // hollows, or two masses -- the field says they join through the
        // middle of a lattice face, and one vertex a sheet a cell cannot hold
        // a tube that thin. Its four faces meet on one edge; they do not
        // cross. At most one in two thousand triangles of ground made to have
        // them, and the faces lit from behind beside it one in five hundred.
        CAPTURE(found.overusedEdges);
        CAPTURE(found.facingAway);
        CHECK(found.overusedEdges * 2000 <= found.triangles);
        CHECK(found.facingAway * 500 <= found.triangles);

        // **A coarse level is the same ground gathered** (ADR 0140): a slab
        // thinner than its cell is a sheet drawn both ways, which is a fold by
        // design, and two sheets in one coarse cell share its vertex. It is
        // drawn only from where its error is under the pixel budget; what is
        // held here is that it stays rare.
        for (core::u32 level = 1; level <= 2; ++level) {
            CAPTURE(level);
            const Crossed coarse = crossings(field, level);
            CAPTURE(coarse.triangles);
            CAPTURE(coarse.crossing);
            CHECK(coarse.crossing * 50 <= coarse.triangles);
        }
    }
}
