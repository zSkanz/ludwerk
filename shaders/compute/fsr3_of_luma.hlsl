// **Optical flow: the picture as a luminance** (ADR 0165), eight bits a pixel,
// which is what motion is found in: AMD's `PrepareLuma`, each thread four
// pixels.

#include "engine_fsr3_options.hlsli"

#define FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR 0
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT 0
#define FFX_OPTICALFLOW_BIND_CB_COMMON 0

#include "engine_fsr3_of_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_prepare_luma.h"

[numthreads(16, 16, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    PrepareLuma(int2(dispatchThreadId.xy), int(groupIndex));
}
