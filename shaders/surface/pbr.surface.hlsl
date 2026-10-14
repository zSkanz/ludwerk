// The built-in surface, written as a surface shader (ADR 0091).
//
// **This file is a test of the contract, not the engine's surface.** The
// built-in path stays in `engine_forward.hlsli`; this is the same surface
// written only with what `engine/surface.hlsli` gives a user, and the render
// captures forced onto it must match the built-in's goldens. If they cannot,
// the contract is missing something a user would need, and the contract is
// what gets fixed.
//
// Its parameters are named after the material's built-in fields, which is
// what makes them those fields: a part's `Color`, its material's `Roughness`,
// its `ColorMap`, reach it as they reach the built-in surface.

#include "engine/surface.hlsli"

ENG_PARAM(float3, Color, float3(1.0, 1.0, 1.0), colour)
ENG_PARAM(float, Metalness, 0.0, range(0, 1))
ENG_PARAM(float, Roughness, 0.7, range(0, 1))
ENG_PARAM(float, NormalScale, 1.0, range(0, 4))
ENG_PARAM(float3, Emissive, float3(0.0, 0.0, 0.0), colour)
ENG_TEXTURE(ColorMap)
ENG_TEXTURE(NormalMap, normal)
ENG_TEXTURE(MetallicRoughnessMap)
ENG_TEXTURE(EmissiveMap)

void surfaceFragment(SurfaceInputs inputs, inout SurfaceOutput surface)
{
    const float4 base = ENG_SAMPLE(ColorMap, inputs.Uv0);
    surface.BaseColor = Color * base.rgb;
    surface.Alpha = base.a;
    const float3 metallicRoughness = ENG_SAMPLE(MetallicRoughnessMap, inputs.Uv0).rgb;
    surface.Roughness = Roughness * metallicRoughness.g;
    surface.Metallic = Metalness * metallicRoughness.b;
    // No map is the mesh's own normal, exactly -- not a flat texel's, which is
    // 128/255 and tilts it by a hair (what the proof caught on shadow edges).
    if (ENG_TEXTURE_SET(NormalMap))
        surface.Normal = surfaceNormalFromMap(ENG_SAMPLE(NormalMap, inputs.Uv0).rgb, NormalScale, inputs);
    surface.Emissive = Emissive * ENG_SAMPLE(EmissiveMap, inputs.Uv0).rgb;
}
