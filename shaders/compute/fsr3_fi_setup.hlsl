// **Frame interpolation: the images its passes keep with atomics, emptied**
// (ADR 0165), and the count of frames since the picture was last cut. AMD's
// `setupFrameinterpolationResources` -- and the two depths the passes after
// keep the nearest of, set to the furthest there is, which AMD's runtime
// clears from outside: a clear to a float's bits is not one every backend
// does to an image of integers the same way.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_SCENE_CHANGE_DETECTION 0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_X 0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_Y 1
#define FFX_FRAMEINTERPOLATION_BIND_UAV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X 2
#define FFX_FRAMEINTERPOLATION_BIND_UAV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y 3
#define FFX_FRAMEINTERPOLATION_BIND_UAV_DISOCCLUSION_MASK 4
#define FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME 5
#define FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME 6
// A buffer, after every texture.
#define FFX_FRAMEINTERPOLATION_BIND_UAV_COUNTERS 7
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_setup.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 at = int2(dispatchThreadId.xy);
    setupFrameinterpolationResources(at);
    StoreReconstructedDepthInterpolatedFrame(at, 1.0f);
    rw_reconstructed_depth_previous_frame[at] = asuint(1.0f);
}
