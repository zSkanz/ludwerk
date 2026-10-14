// Particles simulated on the GPU (ADR 0160): `particle.hlsl` whole, with a
// vertex stage that reads each particle from the buffer the compute pass
// writes (`particle_sim.hlsl`) instead of from an instance stream.
//
// **A twin rather than a branch**, as `sprite_exact.hlsl` is: a frame with no
// such emitter draws through exactly the shader it always did.

#define ENG_PARTICLE_GPU
// Through the include directory, because the compiler reads this file from
// memory and has no directory of its own to resolve a sibling against.
#include "../src/particle.hlsl"
