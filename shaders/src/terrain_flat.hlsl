// **The ground as one flat colour** (ADR 0179): the terrain's forward pass
// with nothing in its fragment -- one colour, no texture, no buffer, no light.
// Not a look: an instrument. The forward pass's time with this in place of the
// ground is what drawing the ground costs whatever its fragment does -- its
// triangles, its fill, the pass itself -- and everything over it is the
// fragment's. `--terrain-surface=flat`.
//
// Compiled apart rather than switched on a uniform inside `terrain.hlsl`,
// because whether a phone's driver takes a branch is the very thing being
// asked. And it reads nothing at all: on the desktop this was measured with
// the layer's own flat colour read from the layers' buffer, and that one read
// -- in a fragment with no texture and no uniform beside it -- cost more than
// the whole lean ground. A floor is what has nothing in it.

#define ENG_UNIFORMS_OBJECT
#include "engine_pbr.hlsli"
#include "engine_terrain_vertex.hlsli"

float4 FragmentMain(TerrainInterpolants input) : SV_Target0
{
    // A meadow's green at about the brightness the lit ground has, so the
    // passes after this one -- exposure, bloom, the tone curve -- are handed a
    // picture like the one they are timed against.
    return float4(0.05f, 0.2f, 0.05f, 1.0f);
}
