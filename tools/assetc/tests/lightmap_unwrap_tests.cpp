// A lightmap's coordinates made where a mesh has none (ADR 0253 §5): charted
// with no two triangles over a texel, the same bytes each time, and a file's
// own set kept unless it overlaps.
#include <doctest/doctest.h>
#include <vector>

#include "engine/asset/model.h"
#include "engine/asset/primitives.h"
#include "engine/assetc/lightmap_unwrap.h"
#include "engine/core/types.h"

using namespace engine::assetc;
using engine::asset::LightmapUv;
using engine::asset::Model;
using engine::asset::MorphDelta;
using engine::asset::MorphTarget;
using engine::asset::PrimitiveShape;
using engine::core::u32;
using engine::core::usize;

namespace {

// A primitive's mesh as a file with no second set would give it.
[[nodiscard]] Model bare(PrimitiveShape shape)
{
    Model model;
    model.mesh = engine::asset::makePrimitive(shape);
    model.mesh.lightmapUvs.clear();
    return model;
}

} // namespace

TEST_CASE("a mesh with no lightmap coordinates is charted with no two triangles over a texel")
{
    for (const PrimitiveShape shape : {PrimitiveShape::Ball, PrimitiveShape::Cylinder, PrimitiveShape::Capsule,
                                       PrimitiveShape::Wedge, PrimitiveShape::Block}) {
        Model model = bare(shape);
        const usize triangles = model.mesh.indices.size() / 3;
        const auto submeshes = model.mesh.submeshes;
        INFO("shape ", static_cast<int>(shape));
        REQUIRE(unwrapLightmap(model));
        CHECK(model.mesh.lightmapUvs.size() == model.mesh.vertices.size());
        CHECK(model.mesh.indices.size() / 3 == triangles);
        CHECK_FALSE(lightmapOverlaps(model.mesh));
        REQUIRE(model.mesh.submeshes.size() == submeshes.size());
        for (usize i = 0; i < submeshes.size(); ++i) {
            CHECK(model.mesh.submeshes[i].firstIndex == submeshes[i].firstIndex);
            CHECK(model.mesh.submeshes[i].indexCount == submeshes[i].indexCount);
        }
        for (const u32 index : model.mesh.indices)
            CHECK(index < model.mesh.vertices.size());
    }
}

TEST_CASE("the same mesh is charted the same way twice")
{
    Model first = bare(PrimitiveShape::Capsule);
    Model second = bare(PrimitiveShape::Capsule);
    REQUIRE(unwrapLightmap(first));
    REQUIRE(unwrapLightmap(second));
    REQUIRE(first.mesh.lightmapUvs.size() == second.mesh.lightmapUvs.size());
    CHECK(first.mesh.indices == second.mesh.indices);
    for (usize i = 0; i < first.mesh.lightmapUvs.size(); ++i) {
        CHECK(first.mesh.lightmapUvs[i].u == second.mesh.lightmapUvs[i].u);
        CHECK(first.mesh.lightmapUvs[i].v == second.mesh.lightmapUvs[i].v);
    }
}

TEST_CASE("a file's own lightmap coordinates are kept, and charted afresh only when they overlap")
{
    Model kept;
    kept.mesh = engine::asset::makePrimitive(PrimitiveShape::Block);
    const std::vector<LightmapUv> own = kept.mesh.lightmapUvs;
    REQUIRE_FALSE(own.empty());
    CHECK_FALSE(unwrapLightmap(kept));
    REQUIRE(kept.mesh.lightmapUvs.size() == own.size());
    CHECK(kept.mesh.lightmapUvs[0].u == own[0].u);

    // Every face over the same square: the texture coordinates a block is
    // drawn with, which a lightmap cannot use.
    Model stacked;
    stacked.mesh = engine::asset::makePrimitive(PrimitiveShape::Block);
    for (usize i = 0; i < stacked.mesh.vertices.size(); ++i)
        stacked.mesh.lightmapUvs[i] = LightmapUv{stacked.mesh.vertices[i].uv[0], stacked.mesh.vertices[i].uv[1]};
    CHECK(lightmapOverlaps(stacked.mesh));
    REQUIRE(unwrapLightmap(stacked));
    CHECK_FALSE(lightmapOverlaps(stacked.mesh));
}

TEST_CASE("a morph target's deltas follow their vertex to every copy the seams made of it")
{
    Model model = bare(PrimitiveShape::Block);
    MorphTarget target;
    target.name = "Lift";
    for (u32 vertex = 0; vertex < model.mesh.vertices.size(); ++vertex) {
        MorphDelta delta;
        delta.vertex = vertex;
        delta.position = model.mesh.vertices[vertex].position;
        target.deltas.push_back(delta);
    }
    model.morphs.push_back(target);
    REQUIRE(unwrapLightmap(model));
    const std::vector<MorphDelta>& deltas = model.morphs[0].deltas;
    REQUIRE(deltas.size() == model.mesh.vertices.size());
    for (usize i = 0; i < deltas.size(); ++i) {
        CHECK(deltas[i].vertex == i);
        CHECK(deltas[i].position.x == model.mesh.vertices[i].position.x);
        CHECK(deltas[i].position.y == model.mesh.vertices[i].position.y);
        CHECK(deltas[i].position.z == model.mesh.vertices[i].position.z);
    }
}

TEST_CASE("a skinned mesh is never charted")
{
    Model model = bare(PrimitiveShape::Ball);
    model.skin.resize(model.mesh.vertices.size());
    CHECK_FALSE(unwrapLightmap(model));
    CHECK(model.mesh.lightmapUvs.empty());
}
