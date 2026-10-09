// The full terrain lighting/material path when none of the visible world's
// layers asks for hex tiling. All texture, normal, height, paint, rule and
// lighting calculations stay the same; only the unreachable hex branch is
// omitted. The renderer selects the original shader as soon as any layer
// enables hex tiling, including material edits after loading.
#define ENG_TERRAIN_NO_HEX
#include "../src/terrain.hlsl"
