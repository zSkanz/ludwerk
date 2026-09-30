// What the ground looks like, shared by every shader that draws terrain.
//
// **One look for all of it.** Terrain is one kind of mesh now (ADR 0082); this
// was split out when the ground and the caves were drawn by different shaders
// and had to agree, and it stays the place the look is defined.
//
// Until the terrain has texture sets, the variation is procedural: three
// octaves of value noise, laid triplanar and pinned to the FIELD's own coordinates so nothing swims
// when the camera moves -- patches of about 37 m, clumps of about 4 m and grain
// of about half a metre -- and a slope rule that turns steep ground to rock the
// way the reference terrains' "autoshader" does.

#ifndef ENG_TERRAIN_SURFACE_HLSLI
#define ENG_TERRAIN_SURFACE_HLSLI

// The palette ids the slope rule treats as already rock.
#define ENG_TERRAIN_ROCK 3u
#define ENG_TERRAIN_BASALT 7u

float terrainHash(float2 cell)
{
    float3 p = frac(float3(cell.xyx) * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

// **A lattice point's value, from its integer coordinates** (the terrain
// audit's bright streaks at a low sun, photos/08). The noise below reads each
// lattice point from the four cells round it, and `terrainHash` -- a `frac` of
// products in floats -- is so sensitive to its last bit that a point reached as
// `cell + 1` in one cell and as `cell` in the next could hash to two different
// values: the noise stepped along that cell's edge, and the grain's normal,
// the noise's slope, drew a bright line and a dark one there. An integer hash
// has no last bit to lose.
float terrainLattice(int2 cell)
{
    uint h = uint(cell.x) * 0x8DA6B343u ^ uint(cell.y) * 0xD8163841u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) / 16777215.0f;
}

float terrainNoise(float2 position)
{
    const float2 corner = floor(position);
    const float2 f = position - corner;
    const float2 u = f * f * (3.0f - 2.0f * f);
    const int2 cell = int2(corner);
    const float a = terrainLattice(cell);
    const float b = terrainLattice(cell + int2(1, 0));
    const float c = terrainLattice(cell + int2(0, 1));
    const float d = terrainLattice(cell + int2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

struct TerrainDetail
{
    float3 Albedo;
    // A tangent-space nudge for the normal: the grain, raked by a low sun.
    float2 NormalNudge;
};

// The detail noise at one planar projection: three octaves and the grain's
// slope along both of the plane's axes.
struct TerrainOctaves
{
    float Macro;
    float Clump;
    float Grain;
    float2 GrainSlope;
};

TerrainOctaves terrainOctaves(float2 plane)
{
    TerrainOctaves octaves;
    octaves.Macro = terrainNoise(plane * (1.0f / 37.0f));
    octaves.Clump = terrainNoise(plane * (1.0f / 4.3f) + 17.0f);
    octaves.Grain = terrainNoise(plane * (1.0f / 0.55f) + 41.0f);
    octaves.GrainSlope =
        float2(terrainNoise((plane + float2(0.07f, 0.0f)) * (1.0f / 0.55f) + 41.0f) - octaves.Grain,
               terrainNoise((plane + float2(0.0f, 0.07f)) * (1.0f / 0.55f) + 41.0f) - octaves.Grain);
    return octaves;
}

TerrainOctaves terrainWeighted(TerrainOctaves sum, TerrainOctaves octaves, float weight)
{
    sum.Macro += octaves.Macro * weight;
    sum.Clump += octaves.Clump * weight;
    sum.Grain += octaves.Grain * weight;
    sum.GrainSlope += octaves.GrainSlope * weight;
    return sum;
}

// `albedo` is the blended palette colour, `ground` the point's field-space
// position in metres, `normal` its surface normal, `rockAlready` whether the
// material there is one the slope rule leaves alone, and `rock` the rock colour.
//
// **Triplanar.** The noise is laid on the three axis planes and blended by how
// squarely the surface faces each. Laid on the ground plane alone -- which is
// what this did -- every cliff and every cave wall showed it stretched straight
// down into vertical streaks, one per texel of the plane above. Ground that
// faces one plane squarely (almost all of it) pays for that plane only: the
// branches skip a plane whose weight is nothing.
TerrainDetail terrainDetail(float3 albedo, float3 ground, float3 normal, bool rockAlready, float3 rock)
{
    // **Each octave fades out as it shrinks below a pixel.** Noise finer than
    // the pixels drawing it does not look like grain: it crawls as the camera
    // moves, and on a specular surface it sparkles. `fwidth` is how many metres
    // one pixel spans here -- taken before any branch, where derivatives are
    // still defined.
    const float3 span = fwidth(ground);
    const float footprint = max(span.x, max(span.y, span.z));
    const float grainFade = saturate(1.0f - footprint / 0.25f);
    const float clumpFade = saturate(1.0f - footprint / 2.0f);

    float3 weights = abs(normal);
    weights *= weights;
    weights *= weights;
    weights /= max(weights.x + weights.y + weights.z, 1e-5f);

    TerrainOctaves blended = (TerrainOctaves)0;
    [branch] if (weights.y > 0.01f)
        blended = terrainWeighted(blended, terrainOctaves(ground.xz), weights.y);
    [branch] if (weights.x > 0.01f)
        blended = terrainWeighted(blended, terrainOctaves(ground.zy), weights.x);
    [branch] if (weights.z > 0.01f)
        blended = terrainWeighted(blended, terrainOctaves(ground.xy), weights.z);
    // The skipped planes' share, so the octaves still average to a half.
    const float kept = (weights.y > 0.01f ? weights.y : 0.0f) + (weights.x > 0.01f ? weights.x : 0.0f) +
                       (weights.z > 0.01f ? weights.z : 0.0f);
    const float scale = 1.0f / max(kept, 1e-5f);
    const float macro = blended.Macro * scale;
    const float clump = blended.Clump * scale;
    const float grain = blended.Grain * scale;

    // Steep ground is rock, whatever it was painted: grass does not hold to a
    // cliff. The threshold wanders with the clump noise so the boundary is a
    // ragged edge rather than a contour line.
    const float slope = 1.0f - saturate(normal.y);
    const float rockiness = smoothstep(0.24f, 0.36f, slope + (clump - 0.5f) * 0.12f);
    if (!rockAlready)
        albedo = lerp(albedo, rock, rockiness);

    // Brightness at three scales and a slight hue drift at the largest: a field
    // is not one green.
    const float shade = 1.0f + (macro - 0.5f) * 0.28f + (clump - 0.5f) * 0.16f * clumpFade +
                        (grain - 0.5f) * 0.10f * grainFade;
    albedo *= shade;
    albedo *= lerp(float3(1.04f, 0.97f, 0.94f), float3(0.95f, 1.03f, 1.02f), macro);

    TerrainDetail detail;
    detail.Albedo = albedo;
    detail.NormalNudge = blended.GrainSlope * scale * (0.8f * grainFade);
    return detail;
}

// **The same variation, apart from any colour** (ADR 0113): what a textured
// layer is multiplied by, how steep-and-rocky the point is, and the grain's
// nudge -- so layers with real textures keep the large-scale break-up the flat
// palette had, at a third of its strength, since a texture brings its own.
struct TerrainVariation
{
    float3 Shade;
    float Rockiness;
    float2 NormalNudge;
};

TerrainVariation terrainVariation(float3 ground, float3 normal)
{
    const float3 span = fwidth(ground);
    const float footprint = max(span.x, max(span.y, span.z));
    const float grainFade = saturate(1.0f - footprint / 0.25f);

    float3 weights = abs(normal);
    weights *= weights;
    weights *= weights;
    weights /= max(weights.x + weights.y + weights.z, 1e-5f);

    TerrainOctaves blended = (TerrainOctaves)0;
    [branch] if (weights.y > 0.01f)
        blended = terrainWeighted(blended, terrainOctaves(ground.xz), weights.y);
    [branch] if (weights.x > 0.01f)
        blended = terrainWeighted(blended, terrainOctaves(ground.zy), weights.x);
    [branch] if (weights.z > 0.01f)
        blended = terrainWeighted(blended, terrainOctaves(ground.xy), weights.z);
    const float kept = (weights.y > 0.01f ? weights.y : 0.0f) + (weights.x > 0.01f ? weights.x : 0.0f) +
                       (weights.z > 0.01f ? weights.z : 0.0f);
    const float scale = 1.0f / max(kept, 1e-5f);
    const float macro = blended.Macro * scale;
    const float clump = blended.Clump * scale;

    TerrainVariation variation;
    const float slope = 1.0f - saturate(normal.y);
    variation.Rockiness = smoothstep(0.24f, 0.36f, slope + (clump - 0.5f) * 0.12f);
    variation.Shade = (1.0f + (macro - 0.5f) * 0.12f) *
                      lerp(float3(1.02f, 0.99f, 0.98f), float3(0.98f, 1.01f, 1.01f), macro);
    variation.NormalNudge = blended.GrainSlope * scale * (0.3f * grainFade);
    return variation;
}

#endif // ENG_TERRAIN_SURFACE_HLSLI
