// **Frame interpolation: the motion optical flow found, as seen from the
// frame between** (ADR 0165). AMD's `computeOpticalFlowVectorField`.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_PREVIOUS_INTERPOLATION_SOURCE 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE 1
// Integers, loaded: after the sampled ones.
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW 2
// Named by the header and read by no code this pass keeps -- the confidence
// from nothing at all (the callbacks say why): after the ones that are, so
// those are a run from zero with no gap, which is how the RHI binds them.
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH 3
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_CONFIDENCE 4
#define FFX_FRAMEINTERPOLATION_BIND_UAV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X 0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y 1
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_optical_flow_vector_field.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    computeOpticalFlowVectorField(int2(dispatchThreadId.xy));
}
