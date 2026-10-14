// SMAA's last pass (ADR 0158): each pixel blended with its neighbours by the
// weights the second pass found -- into the target, or into the picture an
// upscale reads next.

#define ENG_SMAA_BINDS_COLOR
#define ENG_SMAA_BINDS_BLEND
Texture2D colorTex : register(t0, space2);
SamplerState colorTexSampler : register(s0, space2);
Texture2D blendTex : register(t1, space2);
SamplerState blendTexSampler : register(s1, space2);
#if defined(ENG_SMAA_EXACT)
// The sprites drawn in their own colours (ADR 0153), which no edge blending
// may touch: a pixel-art edge is the art.
Texture2D ExactTexture : register(t2, space2);
SamplerState ExactSampler : register(s2, space2);
#endif

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
    float4 offset;
    SMAANeighborhoodBlendingVS(input.Uv, offset);
    const float3 blended = SMAANeighborhoodBlendingPS(input.Uv, offset, colorTex, blendTex).rgb;
#if defined(ENG_SMAA_EXACT)
    const float exact = ExactTexture.SampleLevel(ExactSampler, input.Uv, 0.0f).r;
    return float4(lerp(blended, colorTex.SampleLevel(colorTexSampler, input.Uv, 0.0f).rgb, exact), 1.0f);
#else
    return float4(blended, 1.0f);
#endif
}
