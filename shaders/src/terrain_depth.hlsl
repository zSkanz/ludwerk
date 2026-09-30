// Terrain into the sun's cascades and the local shadow atlas (ADR 0082).
//
// **The ground has no far side**, so it is drawn with no culling and pushed
// away from the light instead. Every other mesh culls its front faces in the
// shadow pass and stores the back of a solid, so a lit surface never shadows
// itself (D051). The ground is one surface, and drawn as it is the map would hold
// exactly the depth the ground is then compared against: the whole terrain would
// acne into a dark rectangle the size of the cascade. The renderer's cascade
// loop says how far to push; a local light pushes nothing.

cbuffer GpuShadowUniforms : register(b0, space1)
{
    column_major float4x4 ViewProjection;
    column_major float4x4 Model;
};

// x: how far to push, in the light's clip depth; then the geomorph (ADR 0140),
// so a shadow is cast by the ground as it is drawn.
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
    // Scaled by w so it is the same depth offset for a perspective projection
    // as for an orthographic one.
    output.Position.z += Push.x * output.Position.w;
    return output;
}

void FragmentMain(Interpolants input)
{
}
