// **Optical flow: a level's motion as the guess for the level twice its
// size** (ADR 0165): doubled, and for each of the four blocks a coarse one
// becomes, the best of it and three neighbours. AMD's
// `ScaleOpticalFlowAdvanced`.

#include "engine_fsr3_options.hlsli"

#define FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT 0
#define FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT 1
#define FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW 2
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_NEXT_LEVEL 0
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT 1
#define FFX_OPTICALFLOW_BIND_CB_COMMON 0
// The group's size, which AMD's header sizes its shared memory by.
#define FFX_OPTICALFLOW_THREAD_GROUP_WIDTH 4
#define FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT 4
#define FFX_OPTICALFLOW_THREAD_GROUP_DEPTH 4

#include "engine_fsr3_of_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_scale_optical_flow_advanced_v5.h"

[numthreads(FFX_OPTICALFLOW_THREAD_GROUP_WIDTH, FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT, FFX_OPTICALFLOW_THREAD_GROUP_DEPTH)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    ScaleOpticalFlowAdvanced(int3(dispatchThreadId), int3(groupThreadId));
}
