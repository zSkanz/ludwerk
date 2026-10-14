// **Frame interpolation: the game's motion field at half the size** (ADR
// 0165), the first level of the pyramid a place no vector reached is filled
// from.
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

#define FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_X 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_Y 1
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> rw_eng_level : register(u0, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> rw_eng_mip : register(u1, space1);

// AMD's `SpdReduce4` for the vector field: the mean of the entries that hold a
// vector, which is the ones with a priority.
float4 engReduceVectors(float4 v0, float4 v1, float4 v2, float4 v3)
{
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float weights = 0.0f;
    const float w0 = float(v0.z > 0.0f);
    const float w1 = float(v1.z > 0.0f);
    const float w2 = float(v2.z > 0.0f);
    const float w3 = float(v3.z > 0.0f);
    sum = v0 * w0 + v1 * w1 + v2 * w2 + v3 * w3;
    weights = w0 + w1 + w2 + w3;
    return sum / (weights > 1e-03f ? weights : 1.0f);
}

// AMD's `SpdLoadSourceImage` for the vector field.
float4 engSource(int2 at)
{
    VectorFieldEntry entry;
    UnpackVectorFieldEntries(LoadGameFieldMv(at), entry);
    return float4(entry.fMotionVector, entry.uHighPriorityFactor, entry.uLowPriorityFactor);
}

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 at = int2(dispatchThreadId.xy);
    if (any(at >= RenderSize() / 2))
        return;
    const int2 corner = at * 2;
    const float4 reduced = engReduceVectors(engSource(corner), engSource(corner + int2(1, 0)),
                                            engSource(corner + int2(0, 1)), engSource(corner + int2(1, 1)));
    rw_eng_level[at] = reduced;
    rw_eng_mip[at] = reduced;
}
