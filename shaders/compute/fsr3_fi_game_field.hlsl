// **Frame interpolation: the game's own motion, as seen from the frame
// between** (ADR 0165): each pixel's vector written where it is half way,
// the nearest and the best matched winning a place. AMD's
// `computeGameFieldMvs`.

#include "engine_fsr3_options.hlsli"

#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_MOTION_VECTORS 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH 1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_PREVIOUS_INTERPOLATION_SOURCE 2
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE 3
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DISTORTION_FIELD 4
#define FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_X 0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_Y 1
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0

#include "engine_fsr3_fi_callbacks.hlsli"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_common.h"
#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_game_motion_vector_field.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    computeGameFieldMvs(int2(dispatchThreadId.xy));
}
