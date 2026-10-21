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
#include "engine_tonemap.hlsli"

Texture2D SourceTexture : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);
// **The world's interface as the display shows it** (ADR 0250), drawn apart
// from the scene into a layer the tonemap lays over the picture -- metered as
// part of it, as it is where the tonemap is folded into a headset eye's pass
// and the interface is in the picture: laid over at the exposure of the frame
// before, then the curve and that exposure undone. Transparent where there is
// none, and the picture is read as it always was.
Texture2D UiLayerTexture : register(t1, space2);
SamplerState UiLayerSampler : register(s1, space2);
// The adapted average the frame before was drawn at.
Texture2D<float> PreviousTexture : register(t2, space2);
SamplerState PreviousSampler : register(s2, space2);

// One tap of the picture, with the interface over it where it has one.
// `LuminanceFirst.x`: the exposure compensation's gain, `exp2` of its stops.
float3 meteredTap(float2 uv)
{
    const float4 texel = SourceTexture.SampleLevel(SourceSampler, uv, 0.0f);
    const float3 scene = texel.rgb;
    // **A picture already tonemapped** (a headset's eye with the tonemap
    // folded in): `w` is one over the exposure it was drawn with, and what
    // the meter wants is the scene before it -- the curve undone, then the
    // exposure. Tap by tap, never on their average: the inverse is steepest
    // near white, so a lit edge averaged with a shadow first read darker
    // than it is. Nought for the 16-bit picture every other frame meters.
    // Negative where it was written encoded, into a plain 8-bit image (a lean
    // window): decoded first.
    if (LuminanceTexelRate.w != 0.0f)
        return untonemapPbrNeutral(LuminanceTexelRate.w > 0.0f ? scene : decodeSrgb(scene)) * abs(LuminanceTexelRate.w);
    const float4 layer = UiLayerTexture.SampleLevel(UiLayerSampler, uv, 0.0f);
    // **Nothing drawn, nothing measured** (the room seen through behind a
    // headset's picture): such a pixel counts as the light measured the
    // frame before, so an empty view neither opens the exposure nor shuts it.
    if (LuminanceFirst.y > 0.5f && texel.a + layer.a <= 0.0f)
    {
        const float before = max(PreviousTexture.SampleLevel(PreviousSampler, float2(0.5f, 0.5f), 0.0f), 1e-4f);
        return float3(before, before, before);
    }
    if (layer.a <= 0.0f)
        return scene;
    const float measured = max(PreviousTexture.SampleLevel(PreviousSampler, float2(0.5f, 0.5f), 0.0f), 1e-4f);
    const float exposure = EngineExposureKey / measured * LuminanceFirst.x;
    const float3 shown = layer.rgb + tonemapPbrNeutral(max(scene * exposure, float3(0.0f, 0.0f, 0.0f))) * (1.0f - layer.a);
    return untonemapPbrNeutral(shown) / exposure;
}

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
    total += meteredTap(uv + float2(-texel.x, -texel.y));
    total += meteredTap(uv + float2(texel.x, -texel.y));
    total += meteredTap(uv + float2(-texel.x, texel.y));
    total += meteredTap(uv + float2(texel.x, texel.y));

    // Rec.709 luminance, which is the space the tonemap operates in.
    const float luminance = dot(total * 0.25f, float3(0.2126f, 0.7152f, 0.0722f));
    // The epsilon is not a guard: a scene with a genuinely black region would
    // otherwise contribute negative infinity to the average and take the whole
    // frame with it.
    return float4(log2(max(luminance, 1e-4f)), 0.0f, 0.0f, 1.0f);
}
