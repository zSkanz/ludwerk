// How bright the frame is, a patch at a time (ADR 0164): the logarithm of the
// luminance averaged over thirty-two pixels by thirty-two, which is what FSR
// 2's accumulation reads to tell a surface that changed its shading from one
// that moved.
//
// **The engine's own, and not AMD's pass.** AMD makes a whole chain of
// averages in one dispatch, the threads keeping count of each other with an
// atomic counter in an image, and reads back one level of it. Not every
// shading language the engine compiles to has that; the one level is what is
// made here, each patch from the pixels under it.
#include "engine_fsr2_options.hlsli"

#define FSR2_BIND_SRV_INPUT_COLOR 0
#define FSR2_BIND_UAV_EXPOSURE_MIP_LUMA_CHANGE 0
#define FSR2_BIND_CB_FSR2 0

#include "engine_fsr2_callbacks.hlsli"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_common.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 patch = int2(dispatchThreadId.xy);
    const int2 patches = LumaMipDimensions();
    if (any(patch >= patches))
        return;
    // The pixels under this patch, sixteen taps a side: each between four
    // pixels, where a linear sampler gives their average.
    const float2 renderSize = float2(RenderSize());
    const float2 span = renderSize / float2(patches);
    const float2 corner = float2(patch) * span;
    float sum = 0.0f;
    float taken = 0.0f;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float2 at = corner + (float2(x, y) + 0.5f) * span / 16.0f;
            if (any(at >= renderSize))
                continue;
            float2 uv = (at + Jitter()) / renderSize;
            uv = ClampUv(uv, RenderSize(), InputColorResourceDimensions());
            const float3 colour = SampleInputColor(uv) / PreExposure();
            sum += log(max(FSR2_EPSILON, RGBToLuma(colour)));
            taken += 1.0f;
        }
    }
    rw_img_mip_shading_change[patch] = float4(taken > 0.0f ? sum / taken : 0.0f, 0.0f, 0.0f, 0.0f);
}
