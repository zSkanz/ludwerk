// Automatic exposure, pass one of three: the HDR frame reduced to a small grid
// of LOG luminance.
//
// **Log rather than linear**, and the difference is the whole reason exposure
// works: a linear average is dominated by the brightest pixels, so one specular
// highlight closes the aperture on the whole scene. The geometric mean -- which
// is what an average of logarithms is -- reads as the brightness a person would
// say the scene has.
//
// A sparse four-tap read rather than an exhaustive one. This is a perceptual
// control and not a measurement: the difference between averaging every pixel
// and averaging a bilinear sample of sixteen per output texel is far below the
// eye's ability to notice the exposure being slightly off.

#define ENG_UNIFORMS_LUMINANCE
#include "engine_pbr.hlsli"
#include "engine_fullscreen.hlsli"

Texture2D SourceTexture : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);

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
    const float2 texel = LuminanceTexelRate.xy;

    // **A view seen on one side of a line** (ADR 0205) is black on the other,
    // and metered whole it read as a dark room. The line goes through the
    // picture's middle, so the point opposite one on the dark side is on the
    // side that is seen: the meter reads that one instead, and what it
    // averages is the side a player looks at. `zw` is nought for every other
    // view, and nothing is turned round.
    float2 uv = input.Uv;
    if (dot(uv - 0.5f, LuminanceRange.zw) > 0.0f)
        uv = 1.0f - uv;

    float3 total = float3(0.0f, 0.0f, 0.0f);
    total += SourceTexture.SampleLevel(SourceSampler, uv + float2(-texel.x, -texel.y), 0.0f).rgb;
    total += SourceTexture.SampleLevel(SourceSampler, uv + float2(texel.x, -texel.y), 0.0f).rgb;
    total += SourceTexture.SampleLevel(SourceSampler, uv + float2(-texel.x, texel.y), 0.0f).rgb;
    total += SourceTexture.SampleLevel(SourceSampler, uv + float2(texel.x, texel.y), 0.0f).rgb;

    // Rec.709 luminance, which is the space the tonemap operates in.
    const float luminance = dot(total * 0.25f, float3(0.2126f, 0.7152f, 0.0722f));
    // The epsilon is not a guard: a scene with a genuinely black region would
    // otherwise contribute negative infinity to the average and take the whole
    // frame with it.
    return float4(log2(max(luminance, 1e-4f)), 0.0f, 0.0f, 1.0f);
}
