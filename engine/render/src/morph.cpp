#include "engine/render/morph.h"

#include <algorithm>
#include <cmath>

namespace engine::render {

using core::f32;
using core::u32;
using core::u64;
using core::Vec3;

namespace {

// A signed ten-bit number for a value within one of nought.
[[nodiscard]] u32 tenBits(f32 value) noexcept
{
    const f32 scaled = std::round(std::clamp(value, -1.0f, 1.0f) * 511.0f);
    return static_cast<u32>(static_cast<core::i32>(scaled)) & 0x3FFu;
}

[[nodiscard]] f32 fromTenBits(u32 bits) noexcept
{
    // The sign is the tenth bit.
    const core::i32 value = static_cast<core::i32>(bits << 22) >> 22;
    return static_cast<f32>(value) / 511.0f;
}

} // namespace

u32 packMorphNormal(Vec3 delta) noexcept
{
    return tenBits(delta.x * 0.5f) | (tenBits(delta.y * 0.5f) << 10) | (tenBits(delta.z * 0.5f) << 20);
}

Vec3 unpackMorphNormal(u32 packed) noexcept
{
    return Vec3{fromTenBits(packed & 0x3FFu), fromTenBits((packed >> 10) & 0x3FFu),
                fromTenBits((packed >> 20) & 0x3FFu)} *
           2.0f;
}

MorphTable buildMorphTable(std::span<const asset::MorphTarget> targets, u32 meshVertexCount)
{
    MorphTable table;
    if (targets.empty() || meshVertexCount == 0)
        return table;

    // The vertices any target moves, first to last. A delta past the mesh is
    // a file the importer should have refused, and is left out here.
    u32 first = meshVertexCount;
    u32 last = 0;
    for (const asset::MorphTarget& target : targets) {
        for (const asset::MorphDelta& delta : target.deltas) {
            if (delta.vertex >= meshVertexCount)
                continue;
            first = std::min(first, delta.vertex);
            last = std::max(last, delta.vertex);
        }
    }
    if (first > last)
        return table;

    const u32 vertexCount = last - first + 1;
    const u64 bytes = static_cast<u64>(vertexCount) * targets.size() * sizeof(GpuMorphDelta);
    if (bytes > kMaxMorphTableBytes) {
        table.refusedBytes = bytes;
        return table;
    }

    table.firstVertex = first;
    table.vertexCount = vertexCount;
    table.targetCount = static_cast<u32>(targets.size());
    // Nought everywhere a target does not reach.
    table.rows.resize(static_cast<core::usize>(vertexCount) * targets.size());
    for (core::usize index = 0; index < targets.size(); ++index) {
        GpuMorphDelta* row = table.rows.data() + index * vertexCount;
        for (const asset::MorphDelta& delta : targets[index].deltas) {
            if (delta.vertex >= meshVertexCount)
                continue;
            GpuMorphDelta& out = row[delta.vertex - first];
            out.position[0] = delta.position.x;
            out.position[1] = delta.position.y;
            out.position[2] = delta.position.z;
            out.normal = packMorphNormal(delta.normal);
        }
    }
    return table;
}

void growBoundsForMorphs(asset::Mesh& mesh, std::span<const asset::MorphTarget> targets)
{
    const core::usize count = mesh.vertices.size();
    if (targets.empty() || count == 0)
        return;
    // Where each vertex can be taken, for the vertices a target moves.
    std::vector<core::AABB> reach(count);
    std::vector<core::u8> moves(count, 0);
    bool any = false;
    for (const asset::MorphTarget& target : targets) {
        for (const asset::MorphDelta& delta : target.deltas) {
            if (delta.vertex >= count)
                continue;
            const Vec3 at = mesh.vertices[delta.vertex].position;
            core::expand(reach[delta.vertex], at + delta.position);
            core::expand(reach[delta.vertex], at - delta.position);
            moves[delta.vertex] = 1;
            any = true;
        }
    }
    if (!any)
        return;
    for (core::usize vertex = 0; vertex < count; ++vertex) {
        if (moves[vertex] != 0)
            core::expand(mesh.bounds, reach[vertex]);
    }
    for (asset::Submesh& submesh : mesh.submeshes) {
        const core::usize from = std::min<core::usize>(submesh.firstIndex, mesh.indices.size());
        const core::usize to = std::min<core::usize>(from + submesh.indexCount, mesh.indices.size());
        for (core::usize index = from; index < to; ++index) {
            const u32 vertex = mesh.indices[index];
            if (vertex < count && moves[vertex] != 0)
                core::expand(submesh.bounds, reach[vertex]);
        }
    }
}

MorphDraw selectMorphs(std::span<const f32> weights) noexcept
{
    MorphDraw draw;
    for (core::usize index = 0; index < weights.size(); ++index) {
        const f32 size = std::abs(weights[index]);
        if (!(size > kMorphWeightFloor))
            continue;
        if (draw.count < kMaxActiveMorphs) {
            draw.target[draw.count] = static_cast<u32>(index);
            draw.weight[draw.count] = weights[index];
            ++draw.count;
            continue;
        }
        // Full: the smallest kept gives way to a larger one, and the earlier
        // of two as small stays -- so the answer does not depend on anything
        // but the weights.
        u32 least = 0;
        for (u32 kept = 1; kept < draw.count; ++kept) {
            if (std::abs(draw.weight[kept]) < std::abs(draw.weight[least]))
                least = kept;
        }
        if (size > std::abs(draw.weight[least])) {
            // Out of the middle, keeping target order.
            for (u32 kept = least; kept + 1 < draw.count; ++kept) {
                draw.target[kept] = draw.target[kept + 1];
                draw.weight[kept] = draw.weight[kept + 1];
            }
            draw.target[draw.count - 1] = static_cast<u32>(index);
            draw.weight[draw.count - 1] = weights[index];
        }
    }
    return draw;
}

GpuMorphUniforms morphUniforms(const MorphDraw& draw, u32 firstVertex, u32 vertexCount, u32 targetCount) noexcept
{
    GpuMorphUniforms block;
    block.range[0] = firstVertex;
    block.range[1] = vertexCount;
    u32 active = 0;
    for (u32 index = 0; index < draw.count && index < kMaxActiveMorphs; ++index) {
        if (draw.target[index] >= targetCount)
            continue;
        block.rows[active] = draw.target[index] * vertexCount;
        block.weights[active] = draw.weight[index];
        ++active;
    }
    block.range[2] = active;
    return block;
}

} // namespace engine::render
