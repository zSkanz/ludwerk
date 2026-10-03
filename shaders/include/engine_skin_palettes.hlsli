// **Every skinned instance's palette, in one storage buffer** (H2): the
// frame's joint matrices back to back, each with its inverse bind folded in
// (`RenderWorld::bones`), and an instance naming where its own begin. What a
// run of skinned objects drawn in one call reads instead of the one-skeleton
// uniform block (`ENG_UNIFORMS_SKIN`).
#pragma once

struct SkinJoint
{
    column_major float4x4 Matrix;
};

// A vertex stage's first storage buffer: `t0` in space 0 (SDL_gpu.h:2699-2730).
StructuredBuffer<SkinJoint> SkinPalettes : register(t0, space0);

// `skinMatrix` (`engine_pbr.hlsli`) over the palette starting at `first`. A
// vertex whose weights sum to zero keeps its bind position.
float4x4 paletteSkin(uint first, uint4 joints, float4 weights)
{
    const float total = weights.x + weights.y + weights.z + weights.w;
    if (total <= 0.0f) {
        return float4x4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                        1.0f);
    }
    return SkinPalettes[first + joints.x].Matrix * weights.x + SkinPalettes[first + joints.y].Matrix * weights.y +
           SkinPalettes[first + joints.z].Matrix * weights.z + SkinPalettes[first + joints.w].Matrix * weights.w;
}
