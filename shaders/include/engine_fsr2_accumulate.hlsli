// FSR 2's accumulation, which `fsr2_accumulate` and `fsr2_accumulate_sharpen`
// are two builds of (ADR 0164): the first writes the picture, the second
// leaves it for the sharpening pass to write.
//
// The pass is AMD's (third_party/fidelityfx_fsr2, MIT), through the engine's
// callbacks; this file is its bindings and its entry point, after AMD's own
// `ffx_fsr2_accumulate_pass.hlsl`.

#include "engine_fsr2_options.hlsli"

#define FSR2_BIND_SRV_INPUT_EXPOSURE 0
#define FSR2_BIND_SRV_DILATED_REACTIVE_MASKS 1
#define FSR2_BIND_SRV_DILATED_MOTION_VECTORS 2
#define FSR2_BIND_SRV_INTERNAL_UPSCALED 3
#define FSR2_BIND_SRV_LOCK_STATUS 4
#define FSR2_BIND_SRV_PREPARED_INPUT_COLOR 5
#define FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS 6
#define FSR2_BIND_SRV_LUMA_HISTORY 7
// Named by the headers and read by no code this build keeps -- the kernel is
// computed, the exposure is the engine's -- so after the ones that are: those
// are a run from zero with no gap, which is how the RHI binds them.
#define FSR2_BIND_SRV_LANCZOS_LUT 8
#define FSR2_BIND_SRV_UPSCALE_MAXIMUM_BIAS_LUT 9
#define FSR2_BIND_SRV_AUTO_EXPOSURE 10

#define FSR2_BIND_UAV_INTERNAL_UPSCALED 0
#define FSR2_BIND_UAV_LOCK_STATUS 1
#define FSR2_BIND_UAV_NEW_LOCKS 2
#define FSR2_BIND_UAV_LUMA_HISTORY 3
// Last: the build that sharpens does not write it.
#define FSR2_BIND_UAV_UPSCALED_OUTPUT 4

#define FSR2_BIND_CB_FSR2 0

#include "engine_fsr2_callbacks.hlsli"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_common.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_sample.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_upsample.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_postprocess_lock_status.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_reproject.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_accumulate.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Accumulate(int2(dispatchThreadId.xy));
}
