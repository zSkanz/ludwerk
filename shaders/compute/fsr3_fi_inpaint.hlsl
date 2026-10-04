// **Frame interpolation: the holes filled** (ADR 0165), each from the
// nearest of the picture's coarser levels that has something there. AMD's
// `computeInpainting` -- which writes only the pixels it changes, into the
// image it reads. Here it reads the interpolated picture and writes another,
// so every pixel is copied across first.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_PYRAMID 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_PRESENT_BACKBUFFER 1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE 2
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OUTPUT 3
// Integers, loaded: after the sampled ones.
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_SCENE_CHANGE_DETECTION 4
#define FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT 0
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_inpainting.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 at = int2(dispatchThreadId.xy);
    if (any(at >= DisplaySize()))
        return;
    StoreFrameinterpolationOutput(at, LoadFrameInterpolationOutput(at));
    computeInpainting(at);
}
