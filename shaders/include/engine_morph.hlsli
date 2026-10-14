// **Morph targets in the vertex stage** (ADR 0196): a table of deltas, a row a
// target and a delta a vertex, and a block naming the few targets this draw
// moves by and how far. What `render::buildMorphTable` and
// `render::morphUniforms` make (`engine/render/morph.h`).
//
// **A vertex finds its delta by its own number**, which is why the pipelines
// that include this draw only a mesh whose vertices start at nought in their
// buffer: with a base vertex, Direct3D numbers from the mesh and Vulkan and
// Metal from the buffer, and the same file would read two rows.
//
// The including file says where the block goes -- `ENG_MORPH_REGISTER`, the
// vertex stage's next free uniform slot: `b1` for a mesh with no skeleton and
// `b2` for one whose palette already has `b1`.
#pragma once

// Mirrors `render::GpuMorphDelta`, 16 bytes.
struct MorphDelta
{
    float3 Position;
    // Three signed ten-bit numbers, x in the low bits, each half the delta.
    uint Normal;
};

// A vertex stage's first storage buffer: `t0` in space 0 (SDL_gpu.h:2699-2730).
StructuredBuffer<MorphDelta> MorphDeltas : register(t0, space0);

// Mirrors `render::kMaxActiveMorphs`.
#define ENG_MAX_ACTIVE_MORPHS 8

// `render::GpuMorphUniforms`, 80 bytes.
cbuffer GpuMorphUniforms : register(ENG_MORPH_REGISTER, space1)
{
    // x: the first vertex the table covers. y: how many it covers. z: how
    // many targets are active.
    uint4 MorphRange;
    // Where each active target's rows begin in the table.
    uint4 MorphRows[2];
    float4 MorphWeights[2];
};

float morphTenBits(uint bits)
{
    // The sign is the tenth bit.
    return float(int(bits << 22) >> 22) / 511.0f;
}

// `position` and `normal` moved by the active targets. A vertex the table
// does not cover is left where it is; the normal comes back unnormalised, as
// it went in -- the fragment stage normalises after interpolation.
void applyMorphs(uint vertex, inout float3 position, inout float3 normal)
{
    if (vertex < MorphRange.x || vertex - MorphRange.x >= MorphRange.y) {
        return;
    }
    const uint column = vertex - MorphRange.x;
    const uint active = min(MorphRange.z, (uint)ENG_MAX_ACTIVE_MORPHS);
    for (uint index = 0; index < active; ++index) {
        const MorphDelta delta = MorphDeltas[MorphRows[index >> 2][index & 3] + column];
        const float weight = MorphWeights[index >> 2][index & 3];
        position += delta.Position * weight;
        normal += float3(morphTenBits(delta.Normal & 0x3FFu), morphTenBits((delta.Normal >> 10) & 0x3FFu),
                         morphTenBits((delta.Normal >> 20) & 0x3FFu)) *
                  (2.0f * weight);
    }
}
