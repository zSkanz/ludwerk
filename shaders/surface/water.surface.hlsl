// The engine's water surface (ADR 0118): what a `Water` is drawn with.
//
// **The waves are the simulation's, term for term** (`engine/scene/water.h`):
// the renderer hands each water's terms over as parameters -- `WaveA` the wave
// number, the amplitude and the direction; `WaveB` the angular speed, the phase
// and the steepness -- and this evaluates the same sum at the same clock
// (`inputs.Time` is `RunService.SimTime`), so a boat floats on the surface it
// is drawn on. Each wave is `A (sin p - s/2 cos 2p)`: a sine sharpened at its
// crest, and still one closed form.
//
// The look is the ocean example's, which this replaced: clear where the water
// is thin over what is under it, deep where it is not, foam where it meets a
// hull or the shore, and what is under it bent by the surface.

#include "engine/surface.hlsli"

ENG_PARAM(float3, Deep, float3(0.012, 0.055, 0.105), colour)
ENG_PARAM(float3, Shallow, float3(0.05, 0.28, 0.32), colour)
ENG_PARAM(float3, Foam, float3(0.85, 0.9, 0.92), colour)
// How deep the water must be before it is its deep colour, in metres.
ENG_PARAM(float, Clarity, 6.0, range(0.5, 30))
// How wide the foam line is where the water meets something, in metres.
ENG_PARAM(float, FoamWidth, 0.35, range(0, 4))
// How far what is under the water bends, as a fraction of the screen.
ENG_PARAM(float, Refraction, 0.03, range(0, 0.2))

ENG_PARAM(float, WaveCount, 0.0)
ENG_PARAM(float4, WaveA0, float4(0, 0, 1, 0))
ENG_PARAM(float4, WaveA1, float4(0, 0, 1, 0))
ENG_PARAM(float4, WaveA2, float4(0, 0, 1, 0))
ENG_PARAM(float4, WaveA3, float4(0, 0, 1, 0))
ENG_PARAM(float4, WaveA4, float4(0, 0, 1, 0))
ENG_PARAM(float4, WaveA5, float4(0, 0, 1, 0))
ENG_PARAM(float4, WaveA6, float4(0, 0, 1, 0))
ENG_PARAM(float4, WaveA7, float4(0, 0, 1, 0))
ENG_PARAM(float4, WaveB0, float4(0, 0, 0, 0))
ENG_PARAM(float4, WaveB1, float4(0, 0, 0, 0))
ENG_PARAM(float4, WaveB2, float4(0, 0, 0, 0))
ENG_PARAM(float4, WaveB3, float4(0, 0, 0, 0))
ENG_PARAM(float4, WaveB4, float4(0, 0, 0, 0))
ENG_PARAM(float4, WaveB5, float4(0, 0, 0, 0))
ENG_PARAM(float4, WaveB6, float4(0, 0, 0, 0))
ENG_PARAM(float4, WaveB7, float4(0, 0, 0, 0))

// One wave's height and its two slopes at a column.
float3 wave(float4 a, float4 b, float2 xz, float t)
{
    const float p = a.x * dot(a.zw, xz) - b.x * t + b.y;
    const float height = a.y * (sin(p) - 0.5f * b.z * cos(2.0f * p));
    const float along = a.y * (cos(p) + b.z * sin(2.0f * p)) * a.x;
    return float3(height, a.zw * along);
}

// The surface above a column: its height over the still surface, and its
// two slopes.
float3 sea(float2 xz, float t)
{
    const float4 a[8] = {WaveA0, WaveA1, WaveA2, WaveA3, WaveA4, WaveA5, WaveA6, WaveA7};
    const float4 b[8] = {WaveB0, WaveB1, WaveB2, WaveB3, WaveB4, WaveB5, WaveB6, WaveB7};
    const int count = int(WaveCount + 0.5f);
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (int index = 0; index < 8; ++index)
    {
        if (index < count)
            sum += wave(a[index], b[index], xz, t);
    }
    return sum;
}

void surfaceVertex(inout SurfaceVertex vertex, SurfaceInputs inputs)
{
    // The tile is flat and scaled across, never up: the wave is taken at the
    // vertex's WORLD column and added to its height.
    vertex.Position.y += sea(inputs.WorldPosition.xz, inputs.Time).x;
}

void surfaceFragment(SurfaceInputs inputs, inout SurfaceOutput surface)
{
    const float3 waves = sea(inputs.WorldPosition.xz, inputs.Time);
    surface.Normal = normalize(float3(-waves.y, 1.0f, -waves.z));
    surface.Metallic = 0.0f;

    // **How much water is behind this pixel**: the distance to what is under
    // it, less the distance to the surface itself.
    const float2 bend = surface.Normal.xz * Refraction;
    const float behind = sceneDepthAt(inputs.ScreenUv + bend);
    // Refract only into what is behind the water, never into what is in front.
    const float2 uv = behind > inputs.ScreenPosition.w ? inputs.ScreenUv + bend : inputs.ScreenUv;
    const float thickness = max(sceneDepthAt(uv) - inputs.ScreenPosition.w, 0.0f);
    const float murk = saturate(thickness / Clarity);

    // Foam where the water is thin: against a hull, a crate, the shore.
    const float edge = FoamWidth > 0.0f ? 1.0f - saturate(thickness / FoamWidth) : 0.0f;
    const float foam = edge * edge;

    // What is under the water, tinted by how much water it is seen through, is
    // light the surface lets through -- emitted, so it is not lit twice. Foam
    // is opaque, so it lets through none.
    const float3 under = sceneColorAt(uv);
    surface.Emissive = lerp(under * Shallow * 2.2f, Deep * 0.5f, murk) * (1.0f - foam);
    surface.BaseColor = lerp(lerp(Shallow, Deep, murk) * 0.45f, Foam, foam);
    surface.Roughness = lerp(0.14f, 0.85f, foam);
    surface.Alpha = 1.0f;
}
