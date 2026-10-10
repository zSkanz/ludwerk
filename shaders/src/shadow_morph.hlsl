// The depth-only pass for a mesh with morph targets above nought and no
// skeleton (ADR 0196): `shadow_depth.hlsl`, with each vertex moved by its
// targets first. The sun's and the lamps' shadow maps and the camera's depth
// prepass all draw through it -- a shadow is cast by the shape that is drawn,
// and the forward pass tests against the depth this writes.

#define ENG_UNIFORMS_SHADOW
#include "engine_pbr.hlsli"
#define ENG_MORPH_REGISTER b1
#include "engine_morph.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
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
    output.Position = mul(LightViewProjection, mul(ShadowModel, float4(position, 1.0f)));
    return output;
}

void FragmentMain()
{
}
