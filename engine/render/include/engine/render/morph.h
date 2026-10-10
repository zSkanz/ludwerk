// **Morph targets, as the renderer holds them** (ADR 0196): the table a vertex
// stage reads, and which of a mesh's targets a draw moves by this frame.
//
// A file keeps a target sparse -- a delta for each vertex it moves, and a face
// of fifty targets moves a few hundred vertices each. A vertex shader cannot
// walk a sparse list, so the card gets it DENSE: a row a target, a delta a
// vertex, sixteen bytes each. Dense over the vertices any target touches and
// not over the whole mesh: a character whose targets are all on its face pays
// for the face.
//
// Nothing here knows a device. `MeshCache` uploads the table and the renderer
// binds it; a test reads it as numbers.
#pragma once

#include <array>
#include <span>
#include <vector>

#include "engine/asset/model.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::render {

// How many targets one draw moves by at once. A face that is talking has a
// handful above nought at any moment, out of fifty; the eight largest are
// taken and the rest dropped for that frame.
inline constexpr core::u32 kMaxActiveMorphs = 8;

// A weight this near nought is nought: the body it belongs to is drawn as if
// it had no targets, in the run with its neighbours. Small enough that what
// is dropped moves no vertex by a visible amount, and above the noise a
// blend of clips leaves behind.
inline constexpr core::f32 kMorphWeightFloor = 1.0f / 1024.0f;

// The table is refused past this, with a warning: a mesh that wants more has
// targets over its whole body, and is better split.
inline constexpr core::u64 kMaxMorphTableBytes = 128ull * 1024ull * 1024ull;

// One vertex of one target, as `engine_morph.hlsli` reads it: where the
// vertex moves to at a weight of one, and how its normal turns, packed.
struct GpuMorphDelta
{
    core::f32 position[3];
    // Three signed ten-bit numbers, x in the low bits, each half the delta
    // (a normal's delta is within two of nought).
    core::u32 normal;
};

static_assert(sizeof(GpuMorphDelta) == 16, "GpuMorphDelta is a StructuredBuffer row; see engine_morph.hlsli");

[[nodiscard]] core::u32 packMorphNormal(core::Vec3 delta) noexcept;
// What the shader reads back out of it.
[[nodiscard]] core::Vec3 unpackMorphNormal(core::u32 packed) noexcept;

struct MorphTable
{
    // The vertices the rows cover: `vertexCount` of them from `firstVertex`.
    core::u32 firstVertex = 0;
    core::u32 vertexCount = 0;
    core::u32 targetCount = 0;
    // Target by target, `vertexCount` rows each.
    std::vector<GpuMorphDelta> rows;
    // What the table would have weighed, when that is why there is none.
    core::u64 refusedBytes = 0;

    [[nodiscard]] bool empty() const noexcept { return rows.empty(); }
};

// The dense table of `targets` over a mesh of `meshVertexCount` vertices.
// Empty for a mesh with no targets, for targets that move nothing, and for a
// table past `kMaxMorphTableBytes`.
[[nodiscard]] MorphTable buildMorphTable(std::span<const asset::MorphTarget> targets, core::u32 meshVertexCount);

// **A mesh's bounds reach as far as its targets can take it.** What the
// renderer culls by, and what a shadow map is fitted to, is the mesh at rest;
// a target that lifts a brow by a centimetre stays inside that, and one that
// doubles a pillar does not -- its top would be culled while still on screen.
// Each vertex is taken to where each target puts it at a weight of one and of
// minus one, and the mesh's bounds and those of every submesh that uses the
// vertex grow to hold it. Weights past one, or several targets pulling the
// same way, can still leave it: that is a model to give larger bounds, as in
// any engine.
void growBoundsForMorphs(asset::Mesh& mesh, std::span<const asset::MorphTarget> targets);

// The targets one draw moves by, and how far each.
struct MorphDraw
{
    core::u32 count = 0;
    std::array<core::u32, kMaxActiveMorphs> target{};
    std::array<core::f32, kMaxActiveMorphs> weight{};
};

// **Which targets a body is drawn with**: those whose weight is further from
// nought than the floor, the largest `kMaxActiveMorphs` of them, in target
// order. A count of nought is a body drawn as if it had no targets at all --
// which is what puts it back in its run the frame its weights return.
[[nodiscard]] MorphDraw selectMorphs(std::span<const core::f32> weights) noexcept;

// The vertex stage's block for a morphed draw (`engine_morph.hlsli`).
struct GpuMorphUniforms
{
    // The table's first vertex, its vertex count, how many targets are
    // active, and nothing.
    core::u32 range[4]{};
    // Where each active target's rows begin in the table.
    core::u32 rows[kMaxActiveMorphs]{};
    core::f32 weights[kMaxActiveMorphs]{};
};

static_assert(sizeof(GpuMorphUniforms) == 80, "GpuMorphUniforms is a cbuffer layout; see engine_morph.hlsli");

// `draw`'s block for a table of `vertexCount` vertices from `firstVertex` and
// `targetCount` targets. A target the table does not have is left out.
[[nodiscard]] GpuMorphUniforms morphUniforms(const MorphDraw& draw, core::u32 firstVertex, core::u32 vertexCount,
                                             core::u32 targetCount) noexcept;

} // namespace engine::render
