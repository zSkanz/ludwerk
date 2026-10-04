// How AMD's FSR 2 headers are compiled here (ADR 0164): what its own build
// passes on the command line, said once for every pass.
#ifndef ENG_FSR2_OPTIONS_HLSLI
#define ENG_FSR2_OPTIONS_HLSLI

#define FFX_GPU 1
#define FFX_HLSL 1
// Whole floats: half precision is a saving on a phone and a difference
// between devices everywhere.
#define FFX_HALF 0
#define FFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0
// The upsample's kernel from its table; the reprojection's computed.
#define FFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2
#define FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE 0
// The picture is scene-linear, before the tone curve.
#define FFX_FSR2_OPTION_HDR_COLOR_INPUT 1
// The engine's motion vectors (`taa_velocity`, `motion`): at the size the
// world is rendered at, and unjittered at both ends.
#define FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1
#define FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS 0
// Depth runs from 0 at the near plane to 1 at the far one.
#define FFX_FSR2_OPTION_INVERTED_DEPTH 0

#endif
