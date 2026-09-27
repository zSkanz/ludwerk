// What the 2D pass and the world's UI both do to a fragment (ADR 0110, D030):
// round its element's corner, lay a gradient over it, and cut a stroke's band
// out of it.
//
// Every one of these is arithmetic on the quad it is drawn with -- no second
// pipeline, no second draw. A quad with none of them pays three compares.

// The signed distance from a point to a rounded rectangle centred on the
// origin: negative inside, zero on the edge, positive outside. The standard
// form, and the reason a corner needs no extra geometry.
float uiRoundedRectDistance(float2 local, float2 half, float radius)
{
    const float2 outside = abs(local) - (half - radius);
    return length(max(outside, 0.0f)) + min(max(outside.x, outside.y), 0.0f) - radius;
}

// The distance a stroke's band is measured in, by how it turns a corner.
// `Round` is the true distance, whose outer corners are arcs; `Miter` keeps
// the corner square; `Bevel` cuts it at forty-five degrees. A rounded element
// (a `UICorner`) is always round -- its corner has no point to mitre.
float uiStrokeDistance(float2 local, float2 half, float radius, float join)
{
    if (radius > 0.0f || join < 0.5f)
        return uiRoundedRectDistance(local, half, radius);
    const float2 outside = abs(local) - half;
    const float square = max(outside.x, outside.y);
    if (join > 1.5f)
        return square;
    return max(square, (outside.x + outside.y) * 0.70710678f);
}

// How much of a fragment a stroke's band covers: the distance between
// `inner` and `outer`, softened over one pixel of the distance's own gradient.
float uiStrokeCoverage(float distance, float inner, float outer)
{
    const float edge = max(fwidth(distance), 1e-4f);
    return saturate((distance - inner) / edge + 0.5f) * saturate((outer - distance) / edge + 0.5f);
}

// Where along its sequence a fragment is, before tiling.
//
// `kind` packs the shape (1 linear, 2 radial, 3 conical) and the tiling
// (0 clamp, 1 repeat, 2 mirror) as `shape + 4 * tile`. `local` is the
// fragment's place from the element's centre and `half` the element's
// half-size, both in the element's own upright pixels; `offset` moves the
// centre, in the same pixels; `angle` is radians clockwise on screen.
float uiGradientPosition(float2 local, float2 half, float shape, float angle, float scale, float2 offset)
{
    const float2 at = local - offset;
    float t;
    if (shape < 1.5f) {
        // From the edge the direction enters the box to the edge it leaves:
        // the box's extent along the direction, which is what keeps a rotated
        // gradient reaching its corners.
        const float2 direction = float2(cos(angle), sin(angle));
        const float extent = max(abs(direction.x) * half.x + abs(direction.y) * half.y, 1e-4f);
        t = dot(at, direction) / (2.0f * extent) + 0.5f;
    }
    else if (shape < 2.5f) {
        // A circle of radius (width + height) / 4 -- the mean of the two
        // half-sizes.
        const float radius = max((half.x + half.y) * 0.5f, 1e-4f);
        t = length(at) / radius;
    }
    else {
        // Clockwise from where `angle` points, a whole turn.
        const float turn = atan2(at.y, at.x) - angle;
        t = frac(turn / 6.28318531f);
    }
    return t / max(scale, 1e-4f);
}

// The tiling: the end colours past the ends, the sequence again, or the
// sequence reversed and again.
float uiGradientTile(float t, float tile)
{
    if (tile < 0.5f)
        return saturate(t);
    if (tile < 1.5f)
        return frac(t);
    return 1.0f - abs(frac(t * 0.5f) * 2.0f - 1.0f);
}

// The gradient's colour and opacity at this fragment, out of its row of the
// table. `row` is the row's `v`; below zero the quad has no gradient and this
// is white.
float4 uiGradient(Texture2D<float4> table, SamplerState tableSampler, float2 local, float2 half, float kind, float row,
                  float angle, float scale, float2 offset)
{
    if (row < 0.0f)
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    const float tile = floor(kind / 4.0f);
    const float shape = kind - tile * 4.0f;
    const float t = uiGradientTile(uiGradientPosition(local, half, shape, angle, scale, offset), tile);
    // The texel centres: 0 is the first texel's, 1 the last's.
    const float u = t * (255.0f / 256.0f) + 0.5f / 256.0f;
    return table.SampleLevel(tableSampler, float2(u, row), 0.0f);
}
