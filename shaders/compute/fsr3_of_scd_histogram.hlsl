// **Optical flow: how bright the picture is, in nine parts** (ADR 0165): the
// histograms a cut from one scene to another is found by. AMD's
// `GenerateSceneChangeDetectionHistogram`.

#include "engine_fsr3_options.hlsli"

#define FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT 0
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_HISTOGRAM 0
#define FFX_OPTICALFLOW_BIND_CB_COMMON 0

#include "engine_fsr3_of_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_generate_scd_histogram.h"

[numthreads(32, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
                 uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    GenerateSceneChangeDetectionHistogram(int3(dispatchThreadId), int2(groupThreadId.xy), int(groupIndex),
                                          int2(groupId.xy), int2(32, 8));
}
