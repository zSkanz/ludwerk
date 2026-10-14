// **Frame interpolation: what the frame between cannot take from the frame
// before, and what it cannot take from this one** (ADR 0165): two masks, by
// depth. AMD's `computeDisocclusionMask`.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_PYRAMID 1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DISTORTION_FIELD 2
// Integers, loaded: after the sampled ones.
#define FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_X 3
#define FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_Y 4
#define FFX_FRAMEINTERPOLATION_BIND_SRV_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME 5
#define FFX_FRAMEINTERPOLATION_BIND_SRV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME 6
#define FFX_FRAMEINTERPOLATION_BIND_UAV_DISOCCLUSION_MASK 0
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_disocclusion_mask.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    computeDisocclusionMask(int2(dispatchThreadId.xy));
}
