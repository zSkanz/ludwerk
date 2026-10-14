// **How AMD's frame generation headers are compiled** (ADR 0165): the switches
// its optical flow and its frame interpolation read, said once for every pass
// of the engine's that includes them.

#ifndef ENG_FSR3_OPTIONS_HLSLI
#define ENG_FSR3_OPTIONS_HLSLI

#define FFX_GPU 1
#define FFX_HLSL 1
// Full floats: a half is narrower on some devices and not on others, and a
// frame that differs by the device is not one this engine draws.
#define FFX_HALF 0
// The shader model the passes are written to, which decides nothing here but
// how AMD's types are spelled.
#define FFX_HLSL_SM 60

// Frame interpolation: the depth is nearest at zero, the engine's velocity is
// measured without the jitter, and it is the size the world was rendered at.
#define FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH 0
#define FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS 0
#define FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS 1

#endif
