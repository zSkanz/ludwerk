// **Frame interpolation: the depth of the frame between** (ADR 0165): this
// frame's, carried half of each pixel's motion back. AMD's
// `reconstructPreviousDepth`.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_MOTION_VECTORS 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH 1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DISTORTION_FIELD 2
// Named by the header and read by no code this pass keeps: after the ones
// that are, so those are a run from zero with no gap -- which is how the RHI
// binds them.
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE 3
#define FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME 0
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_reconstruct_previous_depth.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    reconstructPreviousDepth(int2(dispatchThreadId.xy));
}
