// SMAA's second pass (ADR 0158): along each edge the first found, how much of
// each side's colour every pixel takes -- looked up in the precomputed area
// and search tables its authors publish with the shader.

#define ENG_SMAA_BINDS_EDGES
#define ENG_SMAA_BINDS_AREA
#define ENG_SMAA_BINDS_SEARCH
Texture2D edgesTex : register(t0, space2);
SamplerState edgesTexSampler : register(s0, space2);
Texture2D areaTex : register(t1, space2);
SamplerState areaTexSampler : register(s1, space2);
Texture2D searchTex : register(t2, space2);
SamplerState searchTexSampler : register(s2, space2);

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
    float2 pixel;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(input.Uv, pixel, offset);
    // One sample a pixel, so no subsample to tell apart.
    return SMAABlendingWeightCalculationPS(input.Uv, pixel, offset, edgesTex, areaTex, searchTex,
                                           float4(0.0f, 0.0f, 0.0f, 0.0f));
}
