// **Frame interpolation: each pixel's motion and depth from its nearest
// neighbour, and that depth carried to where the pixel was** (ADR 0165):
// what FSR 2 makes for itself, made here for the interpolation, which runs
// whatever smooths the frame. AMD's `ReconstructAndDilate`.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPUT_MOTION_VECTORS 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPUT_DEPTH 1
#define FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME 0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_DILATED_MOTION_VECTORS 1
#define FFX_FRAMEINTERPOLATION_BIND_UAV_DILATED_DEPTH 2
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_reconstruct_dilated_velocity_and_previous_depth.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ReconstructAndDilate(int2(dispatchThreadId.xy));
}
