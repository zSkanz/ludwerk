// FSR 2, the pass that makes the picture (ADR 0164): this frame's samples
// brought up to the display's size through a Lanczos kernel, the frames before
// it carried along their motion to the same place, and the two weighed -- by
// how far the history can be trusted there -- into one.
#define FFX_FSR2_OPTION_APPLY_SHARPENING 0
#include "engine_fsr2_accumulate.hlsli"
