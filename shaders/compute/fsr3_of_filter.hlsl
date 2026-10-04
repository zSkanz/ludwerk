// **Optical flow: a block whose motion is unlike its neighbours' takes the
// middle one of theirs** (ADR 0165). AMD's `FilterOpticalFlow`.

#include "engine_fsr3_options.hlsli"

#define FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS 0
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW 0
#define FFX_OPTICALFLOW_BIND_CB_COMMON 0

#include "engine_fsr3_of_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_filter_optical_flow_v5.h"

[numthreads(16, 4, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
                 uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    FilterOpticalFlow(int2(dispatchThreadId.xy), int2(groupThreadId.xy), int2(groupId.xy), int(groupIndex));
}
