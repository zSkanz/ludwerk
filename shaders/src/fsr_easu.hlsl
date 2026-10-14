// FSR 1's upscale (ADR 0158): EASU, AMD's edge-adaptive spatial upsampling,
// from its portable header (third_party/fidelityfx_fsr1, MIT) -- a picture
// rendered at a fraction of the output brought up to it with its edges kept,
// where a bilinear upscale softens them. On the tonemapped picture, after the
// anti-aliasing and before the sharpening, which is where AMD puts it.

#define ENG_UNIFORMS_EASU
#include "engine_aa.hlsli"
#include "engine_fullscreen.hlsli"

Texture2D SourceTexture : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);
// **Which pixels are a sprite drawn in its own colours** (ADR 0153), at the
// source's size and through a point sampler -- black on a frame without one.
// Pixel art is scaled up by the nearest texel, never filtered: its pixels are
// what it is.
Texture2D<float> ExactTexture : register(t1, space2);
SamplerState ExactSampler : register(s1, space2);

#define A_GPU 1
#define A_HLSL 1
#include "../../third_party/fidelityfx_fsr1/ffx-fsr/ffx_a.h"

// The three channels of the four texels round `p`, each gathered at once.
AF4 FsrEasuRF(AF2 p)
{
    return SourceTexture.GatherRed(SourceSampler, p);
}
AF4 FsrEasuGF(AF2 p)
{
    return SourceTexture.GatherGreen(SourceSampler, p);
}
AF4 FsrEasuBF(AF2 p)
{
    return SourceTexture.GatherBlue(SourceSampler, p);
}

#define FSR_EASU_F 1
#include "../../third_party/fidelityfx_fsr1/ffx-fsr/ffx_fsr1.h"

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
    AF3 colour;
    // The pixel from the coordinate rather than from `SV_Position`, as
    // `fsr_rcas.hlsl` says why: the output's size is the input's over
    // `EasuCon0.xy`, the input's over the output's.
    uint width;
    uint height;
    SourceTexture.GetDimensions(width, height);
    const float2 output = float2(width, height) / asfloat(EasuCon0.xy);
    FsrEasuF(colour, AU2(input.Uv * output), EasuCon0, EasuCon1, EasuCon2, EasuCon3);
    const float exact = ExactTexture.SampleLevel(ExactSampler, input.Uv, 0.0f);
    if (exact > 0.0f)
        colour = lerp(colour, SourceTexture.SampleLevel(ExactSampler, input.Uv, 0.0f).rgb, exact);
    return float4(colour, 1.0f);
}
