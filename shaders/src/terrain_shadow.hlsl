// Terrain into the sun's cascades and the local shadow atlas (ADR 0082, the
// terrain audit's TA8 and TA10).
//
// **The ground has no far side to store**, so it cannot do what every other
// mesh does -- cull its front faces and leave a lit surface nothing to shadow
// itself with (D051). It culls its BACK faces instead, so what the map holds is
// the side the light falls on, and pushes that away from the light by what a
// receiver's filter needs to read past it: a receiver on the same surface,
// comparing its depth with taps up to the filter's reach away, finds them
// deeper by the slope times that reach. So the push is the slope -- this
// fragment's own depth per texel, as the rasteriser interpolates it -- times
// the reach, and a constant of a texel under it. Ground face-on to the light
// is pushed by a texel; ground at a grazing angle by as much as it needs, up
// to where the light meets it at a tenth, past which it is dark anyway and a
// larger push would only let light under what stands on it.
//
// A single push for every slope, which is what this replaced, was either too
// small at a low sun -- acne speckling the ground and faceting the terminator
// of every round shape -- or, grown for it, detached every shadow in the far
// cascade by metres.

cbuffer GpuShadowUniforms : register(b0, space1)
{
    column_major float4x4 ViewProjection;
    column_major float4x4 Model;
};

// x: the constant push, y: the filter's reach in texels, z: the most the push
// may be -- x and z in the light's clip depth. Then the geomorph (ADR 0140),
// so a shadow is cast by the ground as it is drawn.
cbuffer GpuTerrainShadowPush : register(b1, space1)
{
    float4 Push;
    float4 Morph[9];
};

#include "engine_terrain_morph.hlsli"

// The terrain's reading of the vertex (`terrain.hlsl`): the geomorph's offset
// is `(normal.z, tangent.x, uv.y)`.
struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
};

struct Interpolants
{
    float4 Position : SV_Position;
    // The push, handed on: the fragment stage has no block of its own here.
    nointerpolation float3 Push : TEXCOORD0;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    const float3 position =
        terrainMorphed(input.Position, float3(input.Normal.z, input.Tangent.x, input.Uv.y),
                       terrainSeams(input.Tangent.y).tag, mul(Model, float4(input.Position, 1.0f)).xyz);
    output.Position = mul(ViewProjection, mul(Model, float4(position, 1.0f)));
    output.Push = Push.xyz;
    return output;
}

float FragmentMain(Interpolants input) : SV_Depth
{
    const float depth = input.Position.z;
    // Across a texel in x and in y, summed: never under the slope along the
    // steepest direction, which is the one a tap of the disc may lie in.
    const float slope = abs(ddx(depth)) + abs(ddy(depth));
    return saturate(depth + input.Push.x + min(slope * input.Push.y, input.Push.z));
}
