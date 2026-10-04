// **Frame interpolation: the frame between** (ADR 0165): each pixel from the
// frame before and from this one along the game's motion, and again along
// optical flow's, the better matched of the two taken; what neither reaches
// is left for the inpainting. AMD's `computeFrameinterpolation`.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_PREVIOUS_INTERPOLATION_SOURCE 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE 1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DISOCCLUSION_MASK 2
#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_PYRAMID 3
// Integers, loaded: after the sampled ones.
#define FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_X 4
#define FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_Y 5
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X 6
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y 7
// A buffer, after every texture.
#define FFX_FRAMEINTERPOLATION_BIND_SRV_COUNTERS 8
#define FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT 0
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    computeFrameinterpolation(int2(dispatchThreadId.xy));
}
