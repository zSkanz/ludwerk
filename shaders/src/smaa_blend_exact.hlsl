// SMAA's last pass for a frame with sprites drawn in their own colours
// (ADR 0153, 0158): `smaa_blend.hlsl` whole, with the sprites' mask and the
// pixels it marks left as the tonemap wrote them.
//
// A twin rather than a branch, as `tonemap_exact.hlsl` is and for its reason:
// a frame without such a sprite resolves through exactly the shader it always
// did.

#define ENG_SMAA_EXACT
// Through the include directory, because the compiler reads this file from
// memory and has no directory of its own to resolve a sibling against.
#include "../src/smaa_blend.hlsl"
