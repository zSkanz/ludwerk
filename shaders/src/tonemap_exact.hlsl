// The resolve for a frame that has sprites drawn in their own colours
// (ADR 0153): `tonemap.hlsl` whole, with the one-channel mask the sprite pass
// wrote and the pass-through it selects.
//
// **A twin rather than a branch**, for the reason `tonemap_graded.hlsl` gives:
// a frame without such a sprite must resolve through exactly the shader it
// always did, and the renderer makes this pipeline the first frame one exists.

#define ENG_TONEMAP_EXACT
// Through the include directory, because the compiler reads this file from
// memory and has no directory of its own to resolve a sibling against.
#include "../src/tonemap.hlsl"
