// The sun's and the local lights' shadow pass for a block world's CUTOUT faces
// -- leaves (V1, `VoxelService`).
//
// `shadow_depth` writes every fragment of a face, so a leaf block cast a solid
// square while the block itself drew with holes. This is the same depth-only
// pass with the one test the forward shader makes: the face's image is read at
// the same place in the same atlas, and a pixel whose alpha says "hole" is not
// written. The shadow then has the holes the leaves do.
//
// Only the cutout mesh of a chunk is drawn through this. The opaque faces stay
// on `shadow_depth`, which reads no texture, because every one of their
// fragments is there.
//
// The palette is read at the vertex stage (a fragment stage that reads it is a
// pipeline D3D12 refuses), so the tile's rectangle is worked out per vertex
// and passed down, exactly as `voxel.hlsl` does.

#define ENG_UNIFORMS_SHADOW
#include "engine_pbr.hlsli"

// Only the tiles and the params are read here.
// **Each block type, by id minus one**: `render::GpuVoxelBlock`. A storage
// buffer rather than the block it was, which SDL_GPU binds to Vulkan 4 KiB at
// a time -- the sides and undersides were past it (D380).
struct VoxelBlock
{
    float4 Top;
    float4 Side;
    float4 Bottom;
    // The atlas tile each face's image is in: x top, y sides, z bottom, and -1
    // for a face with no image. w: the alpha a translucent type draws at.
    float4 Tiles;
};
StructuredBuffer<VoxelBlock> VoxelBlocks : register(t0, space0);

// Mirrors `GpuVoxelParams`: x the block size in metres; y tiles per atlas row;
// z one tile's size in atlas UV; w half a texel of a tile, in the tile's own
// UV, for the inset.
cbuffer GpuVoxelParams : register(b1, space1)
{
    float4 VoxelParams;
};

Texture2D BlockAtlas : register(t0, space2);
SamplerState BlockAtlasSampler : register(s0, space2);

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
};

struct Interpolants
{
    // Where on the grid this is, in blocks, and the face's normal -- both in
    // the chunk's own space, so the image lands where the forward pass put it.
    float3 Grid : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    // The face's image as a rectangle of the atlas: xy its corner, z its size
    // (zero for a face with no image, which is then solid), w the inset.
    float4 TileRect : TEXCOORD2;
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    output.Position = mul(LightViewProjection, mul(ShadowModel, float4(input.Position, 1.0f)));
    output.Normal = input.Normal;
    output.Grid = input.Position / max(VoxelParams.x, 1e-4f);

    const uint id = uint(input.Tangent.x + 0.5f);
    const uint slot = min(max(id, 1u) - 1u, 255u);
    const float tile = input.Normal.y > 0.5f ? VoxelBlocks[slot].Tiles.x
                       : input.Normal.y < -0.5f ? VoxelBlocks[slot].Tiles.z
                                                : VoxelBlocks[slot].Tiles.y;
    if (tile >= 0.0f) {
        const float row = floor(tile / VoxelParams.y);
        output.TileRect = float4(float2(tile - row * VoxelParams.y, row) * VoxelParams.z, VoxelParams.z, VoxelParams.w);
    }
    else {
        output.TileRect = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return output;
}

void FragmentMain(Interpolants input)
{
    if (input.TileRect.z <= 0.0f)
        return;
    // The forward pass's own face coordinates (`voxel.hlsl`): a hole there is
    // a hole here, texel for texel.
    const float3 normal = input.Normal;
    const float3 g = input.Grid;
    float2 faceUv;
    if (abs(normal.y) > 0.5f)
        faceUv = float2(frac(g.x), normal.y > 0.0f ? frac(g.z) : 1.0f - frac(g.z));
    else if (abs(normal.x) > 0.5f)
        faceUv = float2(normal.x > 0.0f ? 1.0f - frac(g.z) : frac(g.z), 1.0f - frac(g.y));
    else
        faceUv = float2(normal.z > 0.0f ? frac(g.x) : 1.0f - frac(g.x), 1.0f - frac(g.y));
    faceUv = clamp(faceUv, input.TileRect.w, 1.0f - input.TileRect.w);
    const float2 atlasUv = input.TileRect.xy + faceUv * input.TileRect.z;
    if (BlockAtlas.SampleLevel(BlockAtlasSampler, atlasUv, 0.0f).a < 0.5f)
        discard;
}
