// **Optical flow: whether the scene was cut** (ADR 0165): this frame's
// histograms against the last one's. AMD's `ComputeSCDHistogramsDivergence`.

#include "engine_fsr3_options.hlsli"

#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_HISTOGRAM 0
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM 1
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_TEMP 2
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT 3
#define FFX_OPTICALFLOW_BIND_CB_COMMON 0

#include "engine_fsr3_of_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_compute_scd_divergence.h"

[numthreads(256, 1, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
                 uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    ComputeSCDHistogramsDivergence(int3(dispatchThreadId), int2(groupThreadId.xy), int(groupIndex), int2(groupId.xy),
                                   int2(256, 1));
}
