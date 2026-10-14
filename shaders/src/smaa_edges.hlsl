// SMAA's first pass (ADR 0158): where the edges are, by luma, on the
// tonemapped and sRGB-encoded picture -- perceptual, as FXAA's is, because an
// edge is what a person sees. Two channels: an edge on the left, one on top.

#define ENG_SMAA_BINDS_COLOR
Texture2D colorTex : register(t0, space2);
SamplerState colorTexSampler : register(s0, space2);

#include "engine_smaa.hlsli"

// **The reference's vertex functions run per pixel**: they need
// `SMAA_RT_METRICS`, which is in the fragment stage's block, and a vertex
// stage reading it would need a block of its own in another space (SDL_GPU
// gives the vertex stage space1). They are a few multiplies.
struct Interpolants
{
    float2 Uv : TEXCOORD0;
    float4 Position : SV_Position;
};

Interpolants VertexMain(uint vertexId : SV_VertexID)
{
    Interpolants output;
    fullscreenTriangle(vertexId, 0.0f, output.Position, output.Uv);
    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    float4 offset[3];
    SMAAEdgeDetectionVS(input.Uv, offset);
    return float4(SMAALumaEdgeDetectionPS(input.Uv, offset, colorTex), 0.0f, 0.0f);
}
