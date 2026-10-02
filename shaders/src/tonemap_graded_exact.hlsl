// The graded resolve for a frame that has sprites drawn in their own colours
// (ADR 0153): `tonemap_graded.hlsl` whole, with the mask and the pass-through
// of `tonemap_exact.hlsl`. A colour correction grades the lit world and leaves
// such a sprite as it was painted.

#define ENG_TONEMAP_EXACT
#include "../src/tonemap_graded.hlsl"
