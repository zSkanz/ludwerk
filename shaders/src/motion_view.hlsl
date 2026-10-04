// **Each pixel's motion in place of the picture** (`--debug-view=motion`,
// D548): a test instrument. What `taa_velocity.hlsl` and `motion.hlsl` wrote,
// as a colour a check can read -- red and green the motion along x and y, mid
// grey for none and the whole of a channel for sixteen pixels either way; blue
// where the pixel moved at all, which is what tells a thing that moves from
// the still world behind it whatever its speed.

#include "engine_fullscreen.hlsli"

Texture2D MotionTexture : register(t0, space2);
SamplerState MotionSampler : register(s0, space2);

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
    uint width;
    uint height;
    MotionTexture.GetDimensions(width, height);
    // In UV units as written: in pixels, which is what a person counts.
    const float2 pixels = MotionTexture.SampleLevel(MotionSampler, input.Uv, 0.0f).rg * float2(width, height);
    // A sixteenth of a pixel: under it is the rounding of a still camera's
    // two matrices, not a motion.
    const float moved = dot(pixels, pixels) > 1.0f / 256.0f ? 1.0f : 0.0f;
    return float4(saturate(0.5f + pixels / 32.0f), moved, 1.0f);
}
