// Terrain into the depth prepass (ADR 0082), where it is drawn as it is. Its
// shadows are `terrain_shadow.hlsl`'s, which pushes it away from the light.

cbuffer GpuShadowUniforms : register(b0, space1)
{
    column_major float4x4 ViewProjection;
    column_major float4x4 Model;
};

// x, unread here: the shadow's push (`terrain_shadow.hlsl`), which shares the
// block. Then the geomorph (ADR 0140), so the prepass writes the ground where
// it is drawn.
cbuffer GpuTerrainShadowPush : register(b1, space1)
{
    float4 Push;
    float4 Morph[9];
};

#include "engine_terrain_morph.hlsli"

// The terrain's reading of the vertex (`terrain.hlsl`): the geomorph's offset
// is `(normal.z, tangent.x, uv.y)`.
struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
};

struct Interpolants
{
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    const float3 position =
        terrainMorphed(input.Position, float3(input.Normal.z, input.Tangent.x, input.Uv.y),
                       terrainSeams(input.Tangent.y).tag, mul(Model, float4(input.Position, 1.0f)).xyz);
    output.Position = mul(ViewProjection, mul(Model, float4(position, 1.0f)));
    return output;
}

void FragmentMain(Interpolants input)
{
}
