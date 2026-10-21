// A surface shader first worn in play (ADR 0257): the built-in surface
// written as one, with a number the test changes every run -- different
// bytecode, so the driver has nothing of it cached and makes its pipelines
// cold, as a player's first session does.

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
    // The run's own number, far below anything a pixel shows.
    surface.Alpha = base.a + 0.0 * 1.0e-9;
    const float3 metallicRoughness = ENG_SAMPLE(MetallicRoughnessMap, inputs.Uv0).rgb;
    surface.Roughness = Roughness * metallicRoughness.g;
    surface.Metallic = Metalness * metallicRoughness.b;
    // No map is the mesh's own normal, exactly -- not a flat texel's, which is
    // 128/255 and tilts it by a hair (what the proof caught on shadow edges).
    if (ENG_TEXTURE_SET(NormalMap))
        surface.Normal = surfaceNormalFromMap(ENG_SAMPLE(NormalMap, inputs.Uv0).rgb, NormalScale, inputs);
    surface.Emissive = Emissive * ENG_SAMPLE(EmissiveMap, inputs.Uv0).rgb;
}
