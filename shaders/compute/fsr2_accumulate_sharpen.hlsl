// FSR 2's accumulation where the picture is sharpened after it (ADR 0164):
// the same pass, leaving the picture for `fsr2_rcas` to write.
#define FFX_FSR2_OPTION_APPLY_SHARPENING 1
#include "engine_fsr2_accumulate.hlsli"
