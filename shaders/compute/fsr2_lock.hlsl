// FSR 2, its third pass (ADR 0164): a thin bright thing -- a wire, a railing
// -- is found by its brightness against its neighbours and locked, so the
// passes after it keep what they have gathered of it instead of clamping it
// away. The reconstructed depth is put back to the far plane for the next
// frame here too.
//
// The pass is AMD's (third_party/fidelityfx_fsr2, MIT), through the engine's
// callbacks; this file is its bindings and its entry point, after AMD's own
// `ffx_fsr2_lock_pass.hlsl`.

#include "engine_fsr2_options.hlsli"

#define FSR2_BIND_SRV_LOCK_INPUT_LUMA 0
#define FSR2_BIND_UAV_NEW_LOCKS 0
#define FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH 1
#define FSR2_BIND_CB_FSR2 0

#include "engine_fsr2_callbacks.hlsli"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_common.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_sample.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_lock.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ComputeLock(dispatchThreadId.xy);
}
