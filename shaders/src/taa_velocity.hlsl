// How far every pixel moved since the last frame, from the camera alone
// (ADR 0158): the depth back to the world, and the world into the last
// frame's camera. Everything that does not move itself -- the ground, the
// foliage, the blocks, every anchored part -- is exactly this; what moves is
// drawn over it by `motion.hlsl`.
//
// In UV units, the way the temporal pass reads it: where this pixel's surface
// was is `uv - velocity`. Unjittered both ends, so a still camera reads zero.

#define ENG_UNIFORMS_REPROJECT
#include "engine_aa.hlsli"
#include "engine_fullscreen.hlsli"

Texture2D DepthTexture : register(t0, space2);
SamplerState DepthSampler : register(s0, space2);

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

float2 FragmentMain(Interpolants input) : SV_Target0
{
    // At the pixel's own centre, through a point sampler: `Load` would make
    // the depth a storage texture to SDL_GPU.
    const float depth = DepthTexture.SampleLevel(DepthSampler, input.Uv, 0.0f).r;
    const float2 ndc = float2(input.Uv.x * 2.0f - 1.0f, 1.0f - input.Uv.y * 2.0f);
    // The sky is at the far plane: far enough that only the camera's turn
    // moves it, which is what a sky does.
    const float4 world = mul(ReprojectInverse, float4(ndc, depth, 1.0f));
    const float4 before = mul(ReprojectPrevious, float4(world.xyz / world.w, 1.0f));
    if (before.w <= 1e-6f)
        return float2(0.0f, 0.0f);
    const float2 beforeNdc = before.xy / before.w;
    const float2 beforeUv = float2(beforeNdc.x * 0.5f + 0.5f, 0.5f - beforeNdc.y * 0.5f);
    return (input.Uv - ReprojectJitter.xy) - beforeUv;
}
