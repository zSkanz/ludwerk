// Same full terrain equations for worlds whose layers are all triplanar and
// none use hex tiling. The renderer checks both properties for every view.
#define ENG_TERRAIN_TRIPLANAR_ONLY
#define ENG_TERRAIN_NO_HEX
#include "../src/terrain.hlsl"
