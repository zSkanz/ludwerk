// **Frame interpolation: the interpolated picture at half the size** (ADR
// 0165), the first level of the pyramid its holes are filled from.
//
// AMD's makes every level in one pass, with its single-pass downsampler,
// which writes thirteen images at once; a compute pass here writes eight at
// most. So a pass a level: this one the first, from the source, and
// `fsr3_fi_pyramid_next` each level after from the one before -- the same
// reduction of four into one that AMD's header gives its downsampler.
//
// A level is written twice: into an image of its own, which the next pass
// samples, and into the level of the one image with every level, which is
// what the passes after read. A pass that sampled one level of an image
// while writing the next is not one every backend allows.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_OUTPUT 0
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> rw_eng_level : register(u0, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> rw_eng_mip : register(u1, space1);

// AMD's `SpdReduce4` for the picture: the mean by each sample's weight, which
// is how much of it the interpolation managed.
float4 engReduceColours(float4 v0, float4 v1, float4 v2, float4 v3)
{
    const float sum = v0.w + v1.w + v2.w + v3.w;
    if (sum == 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return (v0 * v0.w + v1 * v1.w + v2 * v2.w + v3 * v3.w) / sum;
}

// AMD's `SpdLoadSourceImage` for the picture: the weight turned round -- how
// much of the pixel there is, where the interpolation wrote how much there is
// not -- and nothing from outside the part of the screen that is interpolated.
float4 engSource(int2 at)
{
    float4 colour = LoadFrameInterpolationOutput(at);
    colour.w = saturate(1.0f - colour.w);
    if (any(at < InterpolationRectBase()) || any(at >= InterpolationRectBase() + InterpolationRectSize()))
        colour.w = 0.0f;
    return colour;
}

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 at = int2(dispatchThreadId.xy);
    if (any(at >= DisplaySize() / 2))
        return;
    const int2 corner = at * 2;
    const float4 reduced = engReduceColours(engSource(corner), engSource(corner + int2(1, 0)),
                                            engSource(corner + int2(0, 1)), engSource(corner + int2(1, 1)));
    rw_eng_level[at] = reduced;
    rw_eng_mip[at] = reduced;
}
