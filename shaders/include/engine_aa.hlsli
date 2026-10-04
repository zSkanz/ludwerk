// What the anti-aliasing and upscaling passes share (ADR 0158): their uniform
// blocks. Kept out of `engine_pbr.hlsli` for the reason `engine_look.hlsli` is:
// a world drawn with none of these passes compiles none of this.
#ifndef ENG_AA_HLSLI
#define ENG_AA_HLSLI

#if defined(ENG_UNIFORMS_SMAA)
// `render::GpuSmaaUniforms`, 16 bytes: SMAA's `SMAA_RT_METRICS` -- one texel,
// and the size, of the image the passes read.
cbuffer GpuSmaaUniforms : register(b0, space3)
{
    float4 SmaaMetrics;
};
#endif

#if defined(ENG_UNIFORMS_EASU)
// `render::GpuEasuUniforms`, 64 bytes: `FsrEasuCon`'s four constants, as the
// CPU computes them from the input and output sizes.
cbuffer GpuEasuUniforms : register(b0, space3)
{
    uint4 EasuCon0;
    uint4 EasuCon1;
    uint4 EasuCon2;
    uint4 EasuCon3;
};
#endif

#if defined(ENG_UNIFORMS_RCAS)
// `render::GpuRcasUniforms`, 16 bytes: `FsrRcasCon`'s constant, the
// sharpness as a float's bits in x.
cbuffer GpuRcasUniforms : register(b0, space3)
{
    uint4 RcasCon;
};
#endif

#if defined(ENG_UNIFORMS_REPROJECT)
// `render::GpuReprojectUniforms`, 144 bytes: how far each pixel moved since the
// last frame, from the camera alone.
cbuffer GpuReprojectUniforms : register(b0, space3)
{
    // This frame's clip space -- jittered, as the depth was drawn -- back to the
    // camera-relative world.
    column_major float4x4 ReprojectInverse;
    // That world into the LAST frame's clip space, unjittered: the last
    // camera, with its origin moved to this one's.
    column_major float4x4 ReprojectPrevious;
    // xy this frame's jitter in UV units.
    float4 ReprojectJitter;
};
#endif

#if defined(ENG_UNIFORMS_MOTION)
// `render::GpuMotionUniforms`, 256 bytes, vertex `b0 space1`: one moving
// draw's place now and a frame ago.
cbuffer GpuMotionUniforms : register(b0, space1)
{
    // Clip from the camera's world, as the depth was drawn -- jittered -- and
    // that world from the object: **two matrices and not their product**,
    // because the depth pass multiplies them a vertex at a time, and a
    // product made on the CPU rounds to another depth (D548).
    column_major float4x4 MotionViewProjection;
    column_major float4x4 MotionModel;
    // Clip from object, unjittered, and a frame ago.
    column_major float4x4 MotionCurrent;
    column_major float4x4 MotionPrevious;
};
#endif

#if defined(ENG_UNIFORMS_TAA)
// `render::GpuTaaUniforms`, 48 bytes.
cbuffer GpuTaaUniforms : register(b0, space3)
{
    // xy one texel, zw the size.
    float4 TaaTexel;
    // x the weight of this frame at rest, y in motion, z 1 when there is a
    // history to blend with, w the width of the neighbourhood's box in
    // standard deviations.
    float4 TaaBlend;
    // xy this frame's jitter in UV units.
    float4 TaaJitter;
};
#endif

#endif // ENG_AA_HLSLI
