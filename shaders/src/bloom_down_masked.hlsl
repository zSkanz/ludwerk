// The bloom chain's first level for a frame that has sprites drawn in their
// own colours (ADR 0153): `bloom_down.hlsl` whole, with every tap multiplied by
// what is NOT such a sprite. Only the first level -- the ones below it read a
// level that already left the sprites out.
//
// A twin rather than a branch, as `tonemap_exact.hlsl` is and for its reason.

#define ENG_BLOOM_MASKED
#include "../src/bloom_down.hlsl"
