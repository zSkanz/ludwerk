// The terrain's vertex stage and what it hands a fragment, for the ground's
// compiled variants (ADR 0179): `terrain_fast.hlsl` and `terrain_flat.hlsl`.
// `terrain.hlsl` holds the same stage, line for line -- a node's vertices are
// read one way whichever fragment draws them (ADR 0140; the loader packs
// them) -- and a change to how a vertex is packed is a change to both.
//
// The includer has `ENG_UNIFORMS_OBJECT` defined and `engine_pbr.hlsli`
// included: `Model`, `ViewProjection`, `NormalMatrix`, `InstanceAlphaUnused`.
#ifndef ENG_TERRAIN_VERTEX_HLSLI
#define ENG_TERRAIN_VERTEX_HLSLI

// The geomorph (ADR 0140): this node's range and its neighbours'.
cbuffer GpuTerrainMorph : register(b1, space1)
{
    float4 Morph[9];
};

#include "engine_terrain_morph.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
};

struct TerrainInterpolants
{
    float3 ShadingPosition : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    // Field space: the coordinate the ground's textures and noise are pinned to.
    float3 Ground : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
    // How much of the sky the vertex sees (`skyVisibility` in the mesher).
    float Sky : TEXCOORD4;
    // How near the pixel is to each of the triangle's corners.
    float3 Corners : TEXCOORD5;
    nointerpolation uint3 Materials : TEXCOORD6;
    // What is painted over each corner, and how much of it shows, 0 to 255
    // (ADR 0114).
    nointerpolation uint3 Tops : TEXCOORD7;
    nointerpolation uint3 Covers : TEXCOORD8;
    float4 Position : SV_Position;
};

// A normal folded octahedrally into two floats (ADR 0140).
float3 terrainNormal(float2 folded)
{
    float3 n = float3(folded.x, folded.y, 1.0f - abs(folded.x) - abs(folded.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return normalize(n);
}

TerrainInterpolants VertexMain(VertexInput input)
{
    TerrainInterpolants output;
    const TerrainSeams seams = terrainSeams(input.Tangent.y);
    const float3 position = terrainMorphed(input.Position, float3(input.Normal.z, input.Tangent.x, input.Uv.y),
                                           seams.tag, mul(Model, float4(input.Position, 1.0f)).xyz);
    const float4 shadingPosition = mul(Model, float4(position, 1.0f));
    output.ShadingPosition = shadingPosition.xyz;
    output.Position = mul(ViewProjection, shadingPosition);
    output.ViewDepth = output.Position.w;
    output.Normal = mul((float3x3)NormalMatrix, terrainNormal(input.Normal.xy));
    output.Ground = position;
    const float corner = seams.corner;
    output.Sky = seams.sky;
    const uint packed = uint(input.Uv.x + 0.5f);
    output.Materials = uint3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u);
    output.Corners = float3(corner < 0.5f ? 1.0f : 0.0f, corner > 0.5f && corner < 1.5f ? 1.0f : 0.0f,
                            corner > 1.5f ? 1.0f : 0.0f);
    const uint tops = uint(input.Tangent.z + 0.5f);
    output.Tops = uint3(tops & 255u, (tops >> 8) & 255u, (tops >> 16) & 255u);
    const uint covers = uint(input.Tangent.w + 0.5f);
    output.Covers = uint3(covers & 255u, (covers >> 8) & 255u, (covers >> 16) & 255u);
    return output;
}

// Per terrain: the rules (ADR 0113 §2), as `asset::TerrainRuleShape` builds
// them, and the terrain's own numbers. `render::GpuTerrainSurfaceUniforms`.
cbuffer GpuTerrainSurfaceUniforms : register(b1, space3)
{
    float4 RuleSlope[16];
    float4 RuleHeight[16];
    float4 RuleMisc[16];
    uint4 RuleApplies[32];
    // x: 1 when the arrays hold every layer; y: the rule count; z: the layer
    // count; w: the terrain's height in the world.
    float4 TerrainParams;
    // w: the slope under which no rule covers anything, its noise allowed for.
    float4 TerrainDebug;
};

// Each layer, by material id (0 is air and unused): `render::GpuTerrainLayer`.
struct TerrainLayer
{
    // The colour with no texture yet; and in a, how hard its paint meets what
    // is under it.
    float4 Flat;
    // The material's colour factor in rgb, and one over its repeat in metres.
    float4 Tint;
    // Roughness factor, metalness factor, normal scale, and flags: 1
    // triplanar, 2 a height map.
    float4 Surface;
    // x how much a pattern tens of metres across varies the colour; y the
    // second sample's scale against the first (1 none); z hexagonal cells.
    float4 Tiling;
    // The light the layer gives off (`Material.Emissive`) in rgb.
    float4 Emissive;
};

#endif // ENG_TERRAIN_VERTEX_HLSLI
