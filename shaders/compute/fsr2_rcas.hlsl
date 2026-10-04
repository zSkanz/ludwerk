// FSR 2's sharpening (ADR 0164): RCAS over the accumulated picture, which a
// temporal upscale leaves a little soft.
//
// The pass is AMD's (third_party/fidelityfx_fsr2, MIT), through the engine's
// callbacks; this file is its bindings and its entry point, after AMD's own
// `ffx_fsr2_rcas_pass.hlsl`.

#include "engine_fsr2_options.hlsli"

#define FSR2_BIND_SRV_INPUT_EXPOSURE 0
#define FSR2_BIND_SRV_RCAS_INPUT 1
#define FSR2_BIND_UAV_UPSCALED_OUTPUT 0
#define FSR2_BIND_CB_FSR2 0
#define FSR2_BIND_CB_RCAS 1

#include "engine_fsr2_callbacks.hlsli"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_common.h"

cbuffer cbRCAS : FFX_FSR2_DECLARE_CB(FSR2_BIND_CB_RCAS)
{
    uint4 rcasConfig;
};

uint4 RCASConfig()
{
    return rcasConfig;
}

float4 LoadRCAS_Input(FfxInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_rcas_input, iPxPos);
}

#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_rcas.h"

[numthreads(64, 1, 1)]
void ComputeMain(uint3 localThreadId : SV_GroupThreadID, uint3 workGroupId : SV_GroupID,
                 uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RCAS(localThreadId, workGroupId, dispatchThreadId);
}
