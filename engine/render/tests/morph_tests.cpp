// Morph targets as the renderer holds them (ADR 0196), on numbers: the table
// the vertex stage reads, which targets a draw moves by, and which meshes the
// cache gives a table to.

#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/render/mesh_cache.h"
#include "engine/render/morph.h"
#include "engine/rhi/backends.h"

using namespace engine;
using core::f32;
using core::u32;
using core::Vec3;

namespace {

[[nodiscard]] asset::MorphTarget target(const char* name, std::initializer_list<asset::MorphDelta> deltas)
{
    asset::MorphTarget made;
    made.name = name;
    made.deltas = deltas;
    return made;
}

// What `engine_morph.hlsli` computes for one vertex, from the same table and
// the same block: the shader's own steps, so a change to either side that the
// other does not follow fails here.
[[nodiscard]] Vec3 moved(const render::MorphTable& table, const render::GpuMorphUniforms& block, u32 vertex,
                         Vec3 position)
{
    if (vertex < block.range[0] || vertex - block.range[0] >= block.range[1])
        return position;
    const u32 column = vertex - block.range[0];
    for (u32 index = 0; index < block.range[2]; ++index) {
        const render::GpuMorphDelta& delta = table.rows[block.rows[index] + column];
        position = position + Vec3{delta.position[0], delta.position[1], delta.position[2]} * block.weights[index];
    }
    return position;
}

} // namespace

TEST_CASE("a morph table covers the vertices its targets touch, and no others")
{
    // A mesh of a thousand vertices whose two targets move vertices 400 to
    // 407 between them: a face on a body. The table is eight vertices wide,
    // not a thousand.
    const std::vector<asset::MorphTarget> targets{
        target("Smile", {{400, Vec3{0.0f, 0.1f, 0.0f}, Vec3{0.0f, 0.0f, 0.5f}}, {403, Vec3{0.2f, 0.0f, 0.0f}, Vec3{}}}),
        target("Blink", {{407, Vec3{0.0f, -0.3f, 0.0f}, Vec3{0.0f, -1.5f, 0.0f}}}),
    };
    const render::MorphTable table = render::buildMorphTable(targets, 1000);
    REQUIRE_FALSE(table.empty());
    CHECK(table.firstVertex == 400);
    CHECK(table.vertexCount == 8);
    CHECK(table.targetCount == 2);
    REQUIRE(table.rows.size() == 16);

    // A row a target; nought wherever the target does not reach.
    CHECK(static_cast<double>(table.rows[0].position[1]) == doctest::Approx(0.1));
    CHECK(static_cast<double>(table.rows[3].position[0]) == doctest::Approx(0.2));
    CHECK(table.rows[1].position[0] == 0.0f);
    CHECK(table.rows[1].normal == 0u);
    CHECK(static_cast<double>(table.rows[8 + 7].position[1]) == doctest::Approx(-0.3));
    CHECK(table.rows[8 + 0].position[1] == 0.0f);

    // The normal's delta survives its ten bits to a part in five hundred of
    // its range of four.
    const Vec3 turned = render::unpackMorphNormal(table.rows[8 + 7].normal);
    CHECK(static_cast<double>(turned.y) == doctest::Approx(-1.5).epsilon(0.01));
    CHECK(turned.x == 0.0f);
    const Vec3 smiled = render::unpackMorphNormal(table.rows[0].normal);
    CHECK(static_cast<double>(smiled.z) == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("a morphed vertex lands where its targets and their weights say")
{
    const std::vector<asset::MorphTarget> targets{
        target("A", {{2, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}}, {3, Vec3{0.0f, 2.0f, 0.0f}, Vec3{}}}),
        target("B", {{3, Vec3{0.0f, 0.0f, 4.0f}, Vec3{}}}),
        target("C", {{5, Vec3{8.0f, 8.0f, 8.0f}, Vec3{}}}),
    };
    const render::MorphTable table = render::buildMorphTable(targets, 10);
    REQUIRE(table.firstVertex == 2);
    REQUIRE(table.vertexCount == 4);

    const std::array<f32, 3> weights{0.5f, -0.25f, 0.0f};
    const render::MorphDraw draw = render::selectMorphs(weights);
    REQUIRE(draw.count == 2);
    const render::GpuMorphUniforms block =
        render::morphUniforms(draw, table.firstVertex, table.vertexCount, table.targetCount);
    CHECK(block.range[2] == 2);

    const Vec3 at{10.0f, 10.0f, 10.0f};
    // Outside the table: not moved, and not read.
    CHECK(moved(table, block, 0, at).x == 10.0f);
    CHECK(moved(table, block, 9, at).x == 10.0f);
    // One target.
    CHECK(static_cast<double>(moved(table, block, 2, at).x) == doctest::Approx(10.5));
    // Two on the same vertex, one of them the other way.
    const Vec3 both = moved(table, block, 3, at);
    CHECK(static_cast<double>(both.y) == doctest::Approx(11.0));
    CHECK(static_cast<double>(both.z) == doctest::Approx(9.0));
    // A target at nought moves nothing, though the table has it.
    CHECK(moved(table, block, 5, at).x == 10.0f);
}

TEST_CASE("a body whose weights are all at nought has no morph draw at all")
{
    // **What puts it back in its run**: no row is made for it, in the same
    // frame its weights return, so nothing downstream can tell it from a
    // mesh that never had a target.
    const std::array<f32, 4> rest{0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(render::selectMorphs(rest).count == 0);
    CHECK(render::selectMorphs({}).count == 0);

    // Under the floor is nought -- a blend of clips leaves crumbs -- and the
    // floor is far under anything that shows: a thousandth of a target.
    const std::array<f32, 3> crumbs{render::kMorphWeightFloor * 0.5f, -render::kMorphWeightFloor * 0.9f, 0.0f};
    CHECK(render::selectMorphs(crumbs).count == 0);
    CHECK(render::kMorphWeightFloor < 0.002f);

    // Just over it, either way, is a weight.
    const std::array<f32, 2> small{render::kMorphWeightFloor * 2.0f, -render::kMorphWeightFloor * 2.0f};
    const render::MorphDraw draw = render::selectMorphs(small);
    REQUIRE(draw.count == 2);
    CHECK(draw.weight[1] < 0.0f);

    // A weight that is not a number is not a weight.
    const std::array<f32, 1> broken{std::nanf("")};
    CHECK(render::selectMorphs(broken).count == 0);
}

TEST_CASE("of more targets than a draw can move by, the largest are kept, in target order")
{
    // Twelve above nought, growing, with the largest two the other way: the
    // eight kept are the eight largest by size, whatever their sign.
    std::array<f32, 12> weights{};
    for (core::usize index = 0; index < weights.size(); ++index)
        weights[index] = 0.05f * static_cast<f32>(index + 1);
    weights[10] = -weights[10];
    weights[11] = -weights[11];
    const render::MorphDraw draw = render::selectMorphs(weights);
    REQUIRE(draw.count == render::kMaxActiveMorphs);
    for (u32 index = 0; index < draw.count; ++index) {
        CHECK(draw.target[index] == 4 + index);
        CHECK(draw.weight[index] == weights[4 + index]);
    }

    // The other way round -- the largest first -- keeps the same eight.
    std::array<f32, 12> falling{};
    for (core::usize index = 0; index < falling.size(); ++index)
        falling[index] = 0.05f * static_cast<f32>(falling.size() - index);
    const render::MorphDraw first = render::selectMorphs(falling);
    REQUIRE(first.count == render::kMaxActiveMorphs);
    for (u32 index = 0; index < first.count; ++index)
        CHECK(first.target[index] == index);

    // A target the table does not have is left out of the block, not read.
    render::MorphDraw past;
    past.count = 2;
    past.target = {1, 7};
    past.weight = {0.5f, 0.5f};
    const render::GpuMorphUniforms block = render::morphUniforms(past, 0, 10, 3);
    CHECK(block.range[2] == 1);
    CHECK(block.rows[0] == 10);
}

TEST_CASE("targets that move nothing, or more than a mesh may hold, make no table")
{
    CHECK(render::buildMorphTable({}, 100).empty());
    const std::vector<asset::MorphTarget> still{target("Still", {})};
    CHECK(render::buildMorphTable(still, 100).empty());
    // A delta past the mesh is not a vertex of it.
    const std::vector<asset::MorphTarget> stray{target("Stray", {{500, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}}})};
    CHECK(render::buildMorphTable(stray, 100).empty());

    // Two hundred targets from one end of a hundred-thousand-vertex mesh to
    // the other would be 320 MB: refused, and it says what it would weigh.
    std::vector<asset::MorphTarget> wide;
    for (int index = 0; index < 200; ++index)
        wide.push_back(target("Wide", {{0, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}}, {99999, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}}}));
    const render::MorphTable refused = render::buildMorphTable(wide, 100000);
    CHECK(refused.empty());
    CHECK(refused.refusedBytes == 200ull * 100000ull * 16ull);
}

TEST_CASE("only a mesh whose vertices start at nought in their buffer is given a morph table")
{
    // **The assertion the morph pipelines rest on.** The shader finds a
    // vertex's deltas by the vertex's number, and a mesh that is a slice of a
    // shared buffer is numbered from the slice on Direct3D and from the
    // buffer on Vulkan and Metal -- the same file would read two rows. So a
    // pooled mesh and a dynamic one get no table, and the renderer draws no
    // mesh through a morph pipeline that has none.
    rhi::DeviceResult device = rhi::createNullDevice({.backend = rhi::BackendId::Null});
    REQUIRE(device != nullptr);
    rhi::ICmdList* cmd = device->beginFrame();
    REQUIRE(cmd != nullptr);

    asset::Mesh mesh;
    mesh.vertices.resize(12);
    mesh.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    asset::Submesh whole;
    whole.indexCount = 12;
    mesh.submeshes.push_back(whole);
    const std::vector<asset::MorphTarget> targets{target("A", {{4, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}}})};
    const render::MorphTable table = render::buildMorphTable(targets, 12);
    REQUIRE_FALSE(table.empty());

    render::MeshCache cache;
    const render::MeshHandle owned = cache.create(*device, *cmd, mesh, render::MeshUsage::Static);
    REQUIRE(cache.attachMorphs(*device, *cmd, owned, table));
    const render::MeshCache::Resolved* resolved = cache.resolve(owned);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->morph.valid());
    CHECK(resolved->vertexOffset == 0);
    CHECK(resolved->morphFirstVertex == 4);
    CHECK(resolved->morphVertexCount == 1);
    CHECK(resolved->morphTargetCount == 1);
    // Once.
    CHECK_FALSE(cache.attachMorphs(*device, *cmd, owned, table));

    // Two pooled meshes share a page; the second starts past nought.
    (void)cache.create(*device, *cmd, mesh, render::MeshUsage::Pooled);
    const render::MeshHandle pooled = cache.create(*device, *cmd, mesh, render::MeshUsage::Pooled);
    REQUIRE(cache.resolve(pooled) != nullptr);
    CHECK(cache.resolve(pooled)->vertexOffset != 0);
    CHECK_FALSE(cache.attachMorphs(*device, *cmd, pooled, table));
    CHECK_FALSE(cache.resolve(pooled)->morph.valid());

    cache.beginFrame(*device);
    const render::MeshHandle dynamic = cache.create(*device, *cmd, mesh, render::MeshUsage::Dynamic);
    CHECK_FALSE(cache.attachMorphs(*device, *cmd, dynamic, table));

    // And a released mesh takes its table with it.
    cache.release(*device, owned);
    CHECK(cache.resolve(owned) == nullptr);
    cache.destroy(*device);
}

TEST_CASE("a mesh's bounds reach as far as each morph target can take it")
{
    // Two triangles, each a submesh of its own, in the plane of the floor. A
    // target lifts one vertex of the first by two: what the renderer culls
    // by, and fits a shadow map to, has to hold it there -- and where a
    // weight of minus one would put it -- while the submesh the target does
    // not reach keeps the bounds it had.
    asset::Mesh mesh;
    mesh.vertices.resize(6);
    const std::array<Vec3, 6> places{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f},
                                     Vec3{5.0f, 0.0f, 0.0f}, Vec3{6.0f, 0.0f, 0.0f}, Vec3{5.0f, 0.0f, 1.0f}};
    for (core::usize index = 0; index < places.size(); ++index) {
        mesh.vertices[index].position = places[index];
        core::expand(mesh.bounds, places[index]);
    }
    mesh.indices = {0, 1, 2, 3, 4, 5};
    for (u32 slot = 0; slot < 2; ++slot) {
        asset::Submesh submesh;
        submesh.firstIndex = slot * 3;
        submesh.indexCount = 3;
        submesh.material = slot;
        for (u32 corner = 0; corner < 3; ++corner)
            core::expand(submesh.bounds, places[slot * 3 + corner]);
        mesh.submeshes.push_back(submesh);
    }
    const core::AABB untouched = mesh.submeshes[1].bounds;

    const std::vector<asset::MorphTarget> targets{target("Lift", {{1, Vec3{0.0f, 2.0f, 0.5f}, Vec3{}}})};
    render::growBoundsForMorphs(mesh, targets);
    CHECK(static_cast<double>(mesh.bounds.max.y) == doctest::Approx(2.0));
    CHECK(static_cast<double>(mesh.bounds.min.y) == doctest::Approx(-2.0));
    CHECK(static_cast<double>(mesh.bounds.min.z) == doctest::Approx(-0.5));
    CHECK(static_cast<double>(mesh.submeshes[0].bounds.max.y) == doctest::Approx(2.0));
    CHECK(mesh.submeshes[1].bounds == untouched);
    // And it reaches no further along what the target does not move.
    CHECK(static_cast<double>(mesh.bounds.max.x) == doctest::Approx(6.0));

    // No targets, no change.
    const core::AABB before = mesh.bounds;
    render::growBoundsForMorphs(mesh, {});
    CHECK(mesh.bounds == before);
}
