// **Optical flow: the luminance at half the size**, and run again for half of
// that, six times (ADR 0165): motion is searched for coarsest first.
//
// AMD's makes the six levels in one pass, with its single-pass downsampler.
// This is a pass a level, each the average of four pixels of the one before
// -- the same reduction, read from the level as it was stored where AMD's
// carries the fractions on. A level differs from AMD's by less than one step
// of the two hundred and fifty-six, and a pass that writes one image needs
// nothing of the device.

#include "engine_fsr3_options.hlsli"

// The level read, and the one written: both "the luminance" to the callbacks.
#define FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT 0
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT 0
#define FFX_OPTICALFLOW_BIND_CB_COMMON 0

#include "engine_fsr3_of_callbacks.hlsli"

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // `OpticalFlowPyramidLevel` is the level read.
    const int2 from = max(DisplaySize() >> OpticalFlowPyramidLevel(), int2(1, 1));
    const int2 size = max(from >> 1, int2(1, 1));
    const int2 at = int2(dispatchThreadId.xy);
    if (any(at >= size))
        return;
    const int2 corner = at * 2;
    const int2 last = from - 1;
    const float sum = float(LoadOpticalFlowInput(min(corner, last))) +
                      float(LoadOpticalFlowInput(min(corner + int2(1, 0), last))) +
                      float(LoadOpticalFlowInput(min(corner + int2(0, 1), last))) +
                      float(LoadOpticalFlowInput(min(corner + int2(1, 1), last)));
    StoreOpticalFlowInput(at, uint(sum * 0.25f));
}
