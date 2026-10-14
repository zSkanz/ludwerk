// FSR 2, its second pass (ADR 0164): where the last frame's depth says
// something else was in front, the history there is of another surface and is
// not to be trusted -- and the frame's colour is made ready for the passes
// after it, in the space they accumulate in.
//
// The pass is AMD's (third_party/fidelityfx_fsr2, MIT), through the engine's
// callbacks; this file is its bindings and its entry point, after AMD's own
// `ffx_fsr2_depth_clip_pass.hlsl`.

#include "engine_fsr2_options.hlsli"

#define FSR2_BIND_SRV_DILATED_MOTION_VECTORS 0
#define FSR2_BIND_SRV_DILATED_DEPTH 1
#define FSR2_BIND_SRV_REACTIVE_MASK 2
#define FSR2_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK 3
#define FSR2_BIND_SRV_PREVIOUS_DILATED_MOTION_VECTORS 4
#define FSR2_BIND_SRV_INPUT_MOTION_VECTORS 5
#define FSR2_BIND_SRV_INPUT_COLOR 6
#define FSR2_BIND_SRV_INPUT_EXPOSURE 7
// Named by the headers and read by no code this pass keeps: after the ones
// that are, so those are a run from zero with no gap -- which is how the RHI
// binds them.
#define FSR2_BIND_SRV_INPUT_DEPTH 8

#define FSR2_BIND_UAV_DILATED_REACTIVE_MASKS 0
#define FSR2_BIND_UAV_PREPARED_INPUT_COLOR 1
// The previous depth, read through the binding the reconstruction wrote it
// by (`engine_fsr2_callbacks.hlsli`).
#define FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH 2

#define FSR2_BIND_CB_FSR2 0

#include "engine_fsr2_callbacks.hlsli"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_common.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_sample.h"
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_depth_clip.h"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    DepthClip(int2(dispatchThreadId.xy));
}
