// FSR 2, the first of its passes over the frame as it was rendered (ADR 0164):
// each pixel takes the depth nearest the camera among its neighbours and that
// neighbour's motion -- an edge moves with what is in front -- and every depth
// is carried back along its motion to where it was, which is the last frame's
// depth as this frame knows it.
//
// The pass is AMD's (third_party/fidelityfx_fsr2, MIT), through the engine's
// callbacks; this file is its bindings and its entry point, after AMD's own
// `ffx_fsr2_reconstruct_previous_depth_pass.hlsl`.

#include "engine_fsr2_options.hlsli"

#define FSR2_BIND_SRV_INPUT_MOTION_VECTORS 0
#define FSR2_BIND_SRV_INPUT_DEPTH 1
#define FSR2_BIND_SRV_INPUT_COLOR 2
#define FSR2_BIND_SRV_INPUT_EXPOSURE 3

#define FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH 0
#define FSR2_BIND_UAV_DILATED_MOTION_VECTORS 1
#define FSR2_BIND_UAV_DILATED_DEPTH 2
#define FSR2_BIND_UAV_LOCK_INPUT_LUMA 3

#define FSR2_BIND_CB_FSR2 0

#include "engine_fsr2_callbacks.hlsli"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_common.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_sample.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_reconstruct_dilated_velocity_and_previous_depth.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ReconstructAndDilate(int2(dispatchThreadId.xy));
}
