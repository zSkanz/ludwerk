// The forward pass for a SKINNED mesh with morph targets above nought (ADR
// 0196): `pbr_skinned.hlsl`, with each vertex moved by its targets first and
// by its joints after -- the order glTF gives, and the one that lets a smile
// made on a head at rest ride that head wherever its neck turns it.

#define ENG_UNIFORMS_OBJECT
#define ENG_UNIFORMS_FRAME
#define ENG_UNIFORMS_MATERIAL
#define ENG_UNIFORMS_SKIN
#include "engine_forward.hlsli"
// The palette has the vertex stage's `b1`.
#define ENG_MORPH_REGISTER b2
#include "engine_morph.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
    // `float4`, not `uint4`: see `pbr_skinned.hlsl` and D042.
    float4 Joints : TEXCOORD4;
    float4 Weights : TEXCOORD5;
    uint Vertex : SV_VertexID;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;

    float3 position = input.Position;
    float3 normal = input.Normal;
    applyMorphs(input.Vertex, position, normal);

    const float4x4 skin = skinMatrix(uint4(input.Joints), input.Weights);
    const float4 posed = mul(skin, float4(position, 1.0f));
    const float4 shadingPosition = mul(Model, posed);
    output.ShadingPosition = shadingPosition.xyz;
    output.Position = mul(ViewProjection, shadingPosition);
    output.ViewDepth = output.Position.w;

    // As `pbr_skinned.hlsl`: the skin's rotation, then the object's cofactor.
    output.Normal = mul((float3x3)NormalMatrix, mul((float3x3)skin, normal));
    output.Tangent = float4(mul((float3x3)Model, mul((float3x3)skin, input.Tangent.xyz)), input.Tangent.w);
    output.Uv = input.Uv;
    output.UvMetres = input.Uv;
    output.InstanceAlpha = InstanceAlphaUnused.x;

    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    return shadeForward(input);
}
