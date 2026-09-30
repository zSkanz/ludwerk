// The terrain's geomorph (ADR 0140), for `terrain.hlsl` and `terrain_depth.hlsl`
// alike, so a vertex slides the same in every pass. Include it after the block
// that declares `Morph`.
//
// A vertex carries its offset to its parent's as `(normal.z, tangent.x, uv.y)`,
// and in the sky's float `sky + 2 * corner + 8 * tag`: the tag's low four bits
// the seams it sits on (low x, high x, low z, high z; two for a corner), the
// rest the level it was gathered at. A draw carries nine rows of
// `(start, end, level, 0)` in `Morph`: its own, then the node drawn beside
// each side and corner -- low x, high x, low z, high z, then low x and low z,
// high x and low z, low x and high z, high x and high z -- with a level of -1
// where what is drawn there is finer, or nothing.

#ifndef ENG_TERRAIN_MORPH_HLSLI
#define ENG_TERRAIN_MORPH_HLSLI

struct TerrainSeams
{
    float sky;
    float corner;
    uint tag;
};

TerrainSeams terrainSeams(float packed)
{
    TerrainSeams seams;
    const float tag = floor(packed / 8.0f);
    const float rest = packed - 8.0f * tag;
    seams.corner = floor(rest * 0.5f);
    seams.sky = saturate(rest - 2.0f * seams.corner);
    seams.tag = uint(tag + 0.5f);
    return seams;
}

// **The range a vertex slides over**: the smallest of those of the nodes of
// its level that draw it -- its own node, and each drawn beside a seam it is
// on. Every one of them works the same answer out, so a seam's vertex is in
// one place in all of their meshes, sliding or not.
float2 terrainMorphRange(uint tag)
{
    const float level = float(tag >> 4);
    const uint seams = tag & 15u;
    float start = 3.0e38f;
    float end = 3.0e38f;
    bool drawn = false;
    [unroll] for (uint row = 0; row < 9; ++row) {
        bool shares = row == 0;
        if (row >= 1 && row <= 4)
            shares = (seams & (1u << (row - 1u))) != 0u;
        else if (row == 5)
            shares = (seams & 5u) == 5u;
        else if (row == 6)
            shares = (seams & 6u) == 6u;
        else if (row == 7)
            shares = (seams & 9u) == 9u;
        else if (row == 8)
            shares = (seams & 10u) == 10u;
        if (shares && abs(Morph[row].z - level) < 0.5f) {
            start = min(start, Morph[row].x);
            end = min(end, Morph[row].y);
            drawn = true;
        }
    }
    return drawn ? float2(start, end) : float2(0.0f, 0.0f);
}

// **Where a vertex is drawn: slid towards its parent's** as its distance from
// the camera -- `cameraRelative`, the unslid vertex in camera-relative space --
// nears the one at which the nodes that draw it give way to their parents.
float3 terrainMorphed(float3 position, float3 offset, uint tag, float3 cameraRelative)
{
    const float2 range = terrainMorphRange(tag);
    const float distance = length(cameraRelative);
    const float slide = range.y > range.x ? saturate((distance - range.x) / (range.y - range.x)) : 0.0f;
    return position + offset * slide;
}

#endif
