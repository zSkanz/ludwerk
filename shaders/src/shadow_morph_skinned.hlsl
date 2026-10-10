// The depth-only pass for a SKINNED mesh with morph targets above nought (ADR
// 0196): `shadow_skinned.hlsl`, with each vertex moved by its targets first
// and by its joints after. Shadow maps and the camera's depth prepass.

#define ENG_UNIFORMS_SHADOW
#define ENG_UNIFORMS_SKIN
#include "engine_pbr.hlsli"
// The palette has the vertex stage's `b1`.
#define ENG_MORPH_REGISTER b2
#include "engine_morph.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    // `float4` and not `uint4`: see `pbr_skinned.hlsl` and D042.
    float4 Joints : TEXCOORD1;
    float4 Weights : TEXCOORD2;
    uint Vertex : SV_VertexID;
};

struct Interpolants
{
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    float3 position = input.Position;
    float3 unusedNormal = float3(0.0f, 1.0f, 0.0f);
    applyMorphs(input.Vertex, position, unusedNormal);
    const float4 posed = mul(skinMatrix(uint4(input.Joints), input.Weights), float4(position, 1.0f));
    output.Position = mul(LightViewProjection, mul(ShadowModel, posed));
    return output;
}

void FragmentMain()
{
}
