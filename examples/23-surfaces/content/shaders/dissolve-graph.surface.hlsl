// Written by dissolve-graph.surfacegraph.json -- edit the graph, not this file: it is written
// again whenever the graph is saved, and a change made here is lost. To keep
// the surface as code, delete the graph; this file is then yours.

#define ENG_NO_VERTEX
#include "engine/surface.hlsli"

ENG_PARAM(float3, Base, float3(0.55, 0.58, 0.62), colour)
ENG_PARAM(float, Threshold, 0.0, range(0, 1))
ENG_PARAM(float3, Rim, float3(6.0, 2.2, 0.4), colour)
ENG_PARAM(float, RimWidth, 0.06, range(0, 0.3))

float graphHash(float3 p)
{
    p = frac(p * 0.3183099f + 0.1f);
    p *= 17.0f;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Value noise on a lattice, smoothed: nought to one.
float graphNoise(float3 p)
{
    const float3 cell = floor(p);
    const float3 f = frac(p);
    const float3 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(lerp(graphHash(cell), graphHash(cell + float3(1, 0, 0)), u.x),
                     lerp(graphHash(cell + float3(0, 1, 0)), graphHash(cell + float3(1, 1, 0)), u.x), u.y),
                lerp(lerp(graphHash(cell + float3(0, 0, 1)), graphHash(cell + float3(1, 0, 1)), u.x),
                     lerp(graphHash(cell + float3(0, 1, 1)), graphHash(cell + float3(1, 1, 1)), u.x), u.y),
                u.z);
}

void surfaceFragment(SurfaceInputs inputs, inout SurfaceOutput surface)
{
    const float fine = graphNoise(inputs.WorldPosition * 7.0f);
    const float coarse = graphNoise(inputs.WorldPosition * 3.0f);
    const float noise = lerp(fine, coarse, 0.65f);
    const float kept = step(Threshold, noise);
    const float rimAmount = Threshold > 0.0f ? 1.0f - saturate((noise - Threshold) / RimWidth) : 0.0f;
    const float3 glow = Rim * rimAmount;
    surface.BaseColor = Base;
    surface.Alpha = kept;
    surface.Metallic = 0.6f;
    surface.Roughness = 0.5f;
    surface.Emissive = glow;
}
