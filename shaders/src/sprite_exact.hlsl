// Sprites in a frame where at least one is drawn in its own colours
// (ADR 0153): `sprite.hlsl` whole, with a second target -- the mask the
// resolve reads. Every sprite of such a frame goes through this one, exact or
// lit, so they keep their order among themselves; a lit one writes zero.

#define ENG_SPRITE_EXACT
#include "../src/sprite.hlsl"
