// The forward pass for a mesh with MORPH TARGETS above nought and no skeleton
// (ADR 0196): `pbr.hlsl`, with each vertex moved by its targets first.
//
// A file of its own for the reason `pbr_skinned.hlsl` gives: what differs is
// what the vertex stage reads -- here a storage buffer and a second block --
// and every static mesh in every world would otherwise declare both. Its
// pipelines are made the first frame a body has a target above nought, so a
// world with none builds nothing.

#define ENG_UNIFORMS_OBJECT
#define ENG_UNIFORMS_FRAME
#define ENG_UNIFORMS_MATERIAL
#include "engine_forward.hlsli"
#define ENG_MORPH_REGISTER b1
#include "engine_morph.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
    uint Vertex : SV_VertexID;
};

// As `pbr.hlsl` has it.
float2 uvMetresScale(float3x3 model, float3 normal, float4 tangent)
{
    const float3 bitangent = cross(normal, tangent.xyz) * (tangent.w < 0.0f ? -1.0f : 1.0f);
    return float2(length(mul(model, tangent.xyz)), length(mul(model, bitangent)));
}

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;

    float3 position = input.Position;
    float3 normal = input.Normal;
    applyMorphs(input.Vertex, position, normal);

    const float4 shadingPosition = mul(Model, float4(position, 1.0f));
    output.ShadingPosition = shadingPosition.xyz;
    output.Position = mul(ViewProjection, shadingPosition);
    output.ViewDepth = output.Position.w;

    output.Normal = mul((float3x3)NormalMatrix, normal);
    // The tangent is the file's own: a target's tangent deltas are not kept.
    output.Tangent = float4(mul((float3x3)Model, input.Tangent.xyz), input.Tangent.w);
    output.Uv = input.Uv;
    output.UvMetres = input.Uv * uvMetresScale((float3x3)Model, input.Normal, input.Tangent);
    output.InstanceAlpha = InstanceAlphaUnused.x;

    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    return shadeForward(input);
}
