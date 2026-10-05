// **The fast ground** (ADR 0179): the terrain's forward pass as a shader with
// nothing in it but what a pixel of it does.
//
// `terrain.hlsl` is one shader for every ground there is: the full one and the
// lean one, hexagonal cells, three planes, three corners' layers, three
// painted over them, sixteen rules each able to read a layer of its own, and
// the lit surface every mesh shares with its four, eight or sixteen shadow
// taps -- each behind a branch on a uniform. A desktop's driver takes the
// branch. A phone's may work both sides out and keep one, which is cheaper
// for it than branching when the sides are small, and here they are not: the
// lean ground was a third of the full one's reads and an eighth faster.
//
// So this one is compiled with the rest left out, and what it does is fixed:
//
//   - **At most four layers a pixel**: the two heaviest of the triangle's
//     corners, the heaviest painted over them, and the rule that covers most.
//   - **The colour alone**, read once a layer at one plane -- the one the
//     ground faces most -- at a level worked out once, and a second time at
//     the far scale for the heaviest corner's layer only: five reads at the
//     most, where the full ground can make a hundred.
//   - **The mesh's own normal**, a layer's roughness for nothing: the ground is
//     lit by the sun through its shadow -- four taps of one cascade -- the
//     sky's irradiance, the ambient and the lights of its cluster, diffuse
//     alone. No reflection of the sky, no contact shadow, no screen-space
//     occlusion, no shadow from a lamp.
//   - **Five textures bound**, not sixteen: the layers' colours, the sun's
//     shadow map and the three light tables.
//
// What it gives up is what a small screen shows least. The rules still cover
// what the CPU says they cover (`asset::drawnMaterial`): their bands and
// their noise are the full ground's, and only the blend of several at once
// is not.

#define ENG_UNIFORMS_OBJECT
#define ENG_UNIFORMS_FRAME
#include "engine_brdf.hlsli"
#include "engine_terrain_vertex.hlsli"

Texture2DArray LayerColorTexture : register(t0, space2);
SamplerState LayerColorSampler : register(s0, space2);
Texture2D<float> ShadowMap : register(t1, space2);
SamplerState ShadowSampler : register(s1, space2);
Texture2D<float> ClusterGridTexture : register(t2, space2);
SamplerState ClusterGridSampler : register(s2, space2);
Texture2D<float> LightIndexTexture : register(t3, space2);
SamplerState LightIndexSampler : register(s3, space2);
Texture2D LightDataTexture : register(t4, space2);
SamplerState LightDataSampler : register(s4, space2);
// After the five textures, as SDL_GPU numbers a stage's storage buffers.
StructuredBuffer<TerrainLayer> TerrainLayers : register(t5, space2);

// The layer arrays are this many texels across (`Size`, where the renderer
// makes them).
static const float TerrainLayerTexels = 512.0f;

// The rules' noise and what a rule may cover: `terrain.hlsl`'s, line for line,
// which is `asset::terrainRuleNoise`'s -- so what this ground draws at a place
// is what the CPU says is there.
uint terrainRuleHash(int x, int z)
{
    uint h = uint(x) * 0x8DA6B343u ^ uint(z) * 0xD8163841u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

float terrainRuleUnit(uint h)
{
    return float(h & 0xFFFFFFu) / 16777215.0f;
}

float terrainRuleNoise(float x, float z)
{
    const float px = x * (1.0f / 4.3f);
    const float pz = z * (1.0f / 4.3f);
    const float cx = floor(px);
    const float cz = floor(pz);
    const float fx = px - cx;
    const float fz = pz - cz;
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uz = fz * fz * (3.0f - 2.0f * fz);
    const int ix = int(cx);
    const int iz = int(cz);
    const float a = terrainRuleUnit(terrainRuleHash(ix, iz));
    const float b = terrainRuleUnit(terrainRuleHash(ix + 1, iz));
    const float c = terrainRuleUnit(terrainRuleHash(ix, iz + 1));
    const float d = terrainRuleUnit(terrainRuleHash(ix + 1, iz + 1));
    const float nearRow = a + (b - a) * ux;
    const float farRow = c + (d - c) * ux;
    return nearRow + (farRow - nearRow) * uz;
}

bool ruleCovers(uint rule, uint id)
{
    const uint4 words = RuleApplies[rule * 2u + id / 128u];
    const uint word = words[(id % 128u) / 32u];
    return (word & (1u << (id % 32u))) != 0u;
}

// What the pixel is, worked out once for every layer it reads.
struct FastGround
{
    // The plane the ground faces most, for a layer laid on three, and the
    // one a layer laid from above always takes.
    float2 Facing;
    float2 Above;
    // The mip a repeat of one metre asks for; a layer adds its own repeat's.
    float Level;
    // The pixel's one noise, 37 m across: the colour's drift, and how much of
    // the far scale shows.
    float Patch;
    float ViewDepth;
};

// A layer's colour: read once, and for `far` once more at its far scale.
float3 fastLayer(uint id, FastGround ground, bool far)
{
    const TerrainLayer layer = TerrainLayers[id];
    const float scale = layer.Tint.a;
    const bool triplanar = (uint(layer.Surface.w + 0.5f) & 1u) != 0u;
    const float2 uv = (triplanar ? ground.Facing : ground.Above) * scale;
    const float slice = float(id) - 1.0f;
    const float level = max(ground.Level + log2(max(scale, 1e-6f)), 0.0f);
    float3 albedo = LayerColorTexture.SampleLevel(LayerColorSampler, float3(uv, slice), level).rgb;
    [branch] if (far && layer.Tiling.y < 0.999f)
    {
        // Turned as well as scaled, so the two lattices never line up.
        const float2x2 turn = float2x2(0.82533561f, -0.56464247f, 0.56464247f, 0.82533561f);
        const float2 wide = mul(turn, uv) * layer.Tiling.y + 0.37f;
        const float wideLevel = max(level + log2(max(layer.Tiling.y, 1e-6f)), 0.0f);
        const float3 distant = LayerColorTexture.SampleLevel(LayerColorSampler, float3(wide, slice), wideLevel).rgb;
        const float away = smoothstep(2.0f, 24.0f, ground.ViewDepth * scale);
        albedo = lerp(albedo, distant, saturate(lerp(0.25f, 0.75f, away) + (ground.Patch - 0.5f) * 1.5f));
    }
    // The colour's drift across a field: brighter and a little warmer one
    // way, darker and cooler the other, as much as the material asks.
    const float n = ground.Patch - 0.5f;
    const float3 shift = float3(1.0f + 0.55f * n, 1.0f + 0.45f * n, 1.0f + 0.3f * n);
    albedo *= layer.Tint.rgb * lerp(float3(1.0f, 1.0f, 1.0f), shift, layer.Tiling.x);
    // A layer whose maps have not arrived, or that no layer names: flat.
    const bool textured = TerrainParams.x > 0.5f && float(id) <= TerrainParams.z;
    return textured ? albedo : layer.Flat.rgb;
}

// An exact texel fetch through a point sampler (`engine_forward.hlsli`'s).
float fastFetch1(Texture2D<float> table, SamplerState pointSampler, uint x, uint y, uint width, uint height)
{
    const float2 uv = (float2(float(x), float(y)) + 0.5f) / float2(float(width), float(height));
    return table.SampleLevel(pointSampler, uv, 0.0f).r;
}

float4 fastFetch4(Texture2D table, SamplerState pointSampler, uint x, uint y, uint width, uint height)
{
    const float2 uv = (float2(float(x), float(y)) + 0.5f) / float2(float(width), float(height));
    return table.SampleLevel(pointSampler, uv, 0.0f);
}

// The lights of the pixel's cluster (`evaluateClusteredLights`), casting no
// shadow of their own here.
float3 fastLights(Surface surface, float2 pixel, float viewDepth)
{
    [branch] if (LightCountUnused.x <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    const uint tileX =
        min((uint)(pixel.x * ViewportParams.z * float(ENG_CLUSTER_TILES_X)), (uint)(ENG_CLUSTER_TILES_X - 1));
    const uint tileY =
        min((uint)(pixel.y * ViewportParams.w * float(ENG_CLUSTER_TILES_Y)), (uint)(ENG_CLUSTER_TILES_Y - 1));
    const float rawSlice = log2(max(viewDepth, 1e-6f)) * ClusterParams.x + ClusterParams.y;
    const uint slice = (uint)clamp(rawSlice, 0.0f, float(ENG_CLUSTER_SLICES - 1));
    const float packed = fastFetch1(ClusterGridTexture, ClusterGridSampler, slice * ENG_CLUSTER_TILES_X + tileX, tileY,
                                    ENG_CLUSTER_GRID_WIDTH, ENG_CLUSTER_GRID_HEIGHT);
    const uint offset = (uint)(packed / ENG_CLUSTER_OFFSET_SHIFT);
    const uint count =
        min((uint)(packed - float(offset) * ENG_CLUSTER_OFFSET_SHIFT), (uint)ENG_MAX_LIGHTS_PER_CLUSTER);
    float3 color = float3(0.0f, 0.0f, 0.0f);
    [loop] for (uint i = 0u; i < count; ++i)
    {
        const uint entry = offset + i;
        const float encoded =
            fastFetch1(LightIndexTexture, LightIndexSampler, entry % ENG_LIGHT_INDEX_WIDTH,
                       entry / ENG_LIGHT_INDEX_WIDTH, ENG_LIGHT_INDEX_WIDTH, ENG_LIGHT_INDEX_HEIGHT);
        const uint lightIndex = (uint)(encoded + 0.5f);
        GpuLight light;
        light.PositionRange = fastFetch4(LightDataTexture, LightDataSampler, 0u, lightIndex, 3u, ENG_MAX_CLUSTERED_LIGHTS);
        light.Color = fastFetch4(LightDataTexture, LightDataSampler, 1u, lightIndex, 3u, ENG_MAX_CLUSTERED_LIGHTS);
        light.DirectionCosAngle =
            fastFetch4(LightDataTexture, LightDataSampler, 2u, lightIndex, 3u, ENG_MAX_CLUSTERED_LIGHTS);
        color += evaluatePunctualLight(surface, light, 1.0f);
    }
    return color;
}

// The sun's shadow: the pixel's cascade, four taps of it, and no blend into
// the next (`sampleSunShadow` and `sampleCascade`, with the count compiled
// in). Past the last cascade drawn, lit.
float fastSunShadow(float3 position, float3 normal, float nol, float viewDepth, float2 pixel)
{
    uint width = 0;
    uint height = 0;
    ShadowMap.GetDimensions(width, height);
    const float2 atlasSize = float2(float(max(width, 1u)), float(max(height, 1u)));

    const uint cascade = viewDepth <= CascadeFar.x ? 0u : (viewDepth <= CascadeFar.y ? 1u : (viewDepth <= CascadeFar.z ? 2u : 3u));
    const float texelWorld = CascadeTexelWorld[cascade];
    const float depthRange = max(CascadeDepthRange[cascade], 1e-3f);
    const float radiusTexels = clamp(ShadowParams.x / max(texelWorld, 1e-6f), 1.5f, 6.0f);

    const float3 toLight = normalize(SunDirectionBrightness.xyz);
    float3 offset = normal * ((1.0f - saturate(nol)) * ShadowParams.y * texelWorld);
    offset -= toLight * dot(toLight, offset);
    const float4 lightClip = mul(CascadeViewProjection[cascade], float4(position + offset, 1.0f));
    const float3 ndc = lightClip.xyz / lightClip.w;
    const bool outside = any(abs(ndc.xy) > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f ||
                         CascadeFar[cascade] > EngineShadowNoCascade;

    const float2 local = ndc.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    const float slope = min(sqrt(saturate(1.0f - nol * nol)) / max(nol, 0.05f), EngineShadowMostSlope);
    const float reference = ndc.z - (ShadowParams.w + texelWorld * slope * EngineShadowSlopeTexels) / depthRange;
    const float2 texelUv = 1.0f / atlasSize;
    const float2 radiusUv = radiusTexels * texelUv;
    const float2 tile = float2(float(cascade & 1u), float(cascade >> 1u)) * 0.5f;
    const float2 inset = radiusUv + texelUv;
    const float2 lowest = tile + inset;
    const float2 highest = tile + float2(0.5f, 0.5f) - inset;
    const float2 centre = tile + local * 0.5f;

    float turnSin = 0.0f;
    float turnCos = 0.0f;
    sincos(shadowKernelAngle(pixel), turnSin, turnCos);
    float lit = 0.0f;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const float2 tap = EngineShadowDisc[i];
        const float2 turned = float2(tap.x * turnCos - tap.y * turnSin, tap.x * turnSin + tap.y * turnCos);
        const float2 uv = clamp(centre + turned * (radiusUv * 0.5f), lowest, highest);
        lit += shadowTapPcf(ShadowMap, ShadowSampler, uv, reference, atlasSize);
    }
    return outside ? 1.0f : lit * 0.25f;
}

float4 FragmentMain(TerrainInterpolants input) : SV_Target0
{
    const float lengthSquared = dot(input.Normal, input.Normal);
    const float3 normal = lengthSquared > 1e-12f ? input.Normal * rsqrt(lengthSquared) : float3(0.0f, 1.0f, 0.0f);
    const float3 forward = LightCountUnused.yzw;
    const float viewDepth = dot(forward, forward) > 0.0f ? dot(input.ShadingPosition, forward) : input.ViewDepth;

    FastGround ground;
    const float3 facing = abs(normal);
    ground.Above = input.Ground.xz;
    ground.Facing = facing.y >= facing.x && facing.y >= facing.z ? input.Ground.xz
                                                                 : (facing.x >= facing.z ? input.Ground.zy : input.Ground.xy);
    const float3 dx = ddx(input.Ground);
    const float3 dy = ddy(input.Ground);
    ground.Level = 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12f) * (TerrainLayerTexels * TerrainLayerTexels));
    ground.Patch = terrainRuleNoise(input.Ground.x * (4.3f / 37.0f), input.Ground.z * (4.3f / 37.0f));
    ground.ViewDepth = viewDepth;

    // The triangle's layers, a repeat folded into the first corner that has it.
    uint3 ids = input.Materials;
    float3 corners = input.Corners;
    if (ids.y == ids.x) {
        corners.x += corners.y;
        corners.y = 0.0f;
    }
    if (ids.z == ids.x) {
        corners.x += corners.z;
        corners.z = 0.0f;
    }
    else if (ids.z == ids.y) {
        corners.y += corners.z;
        corners.z = 0.0f;
    }
    // **The two heaviest**: the third's share goes to them as they stand to
    // each other. Where three materials meet at a point it is two of them.
    const bool xLeast = corners.x <= corners.y && corners.x <= corners.z;
    const bool yLeast = !xLeast && corners.y <= corners.z;
    const uint firstId = xLeast ? ids.y : ids.x;
    const uint secondId = xLeast || yLeast ? ids.z : ids.y;
    const float firstShare = xLeast ? corners.y : corners.x;
    const float secondShare = xLeast || yLeast ? corners.z : corners.y;
    const bool swap = secondShare > firstShare;
    const uint heavy = swap ? secondId : firstId;
    const uint light = swap ? firstId : secondId;
    const float lightShare = min(firstShare, secondShare) / max(firstShare + secondShare, 1e-5f);

    float3 albedo = fastLayer(heavy, ground, true);
    [branch] if (lightShare > 0.001f)
        albedo = lerp(albedo, fastLayer(light, ground, false), lightShare);

    // **What is painted over it** (ADR 0114): the layer that shows most, by
    // how much of the pixel is painted. A crossfade: no height to blend by.
    [branch] if (any(input.Covers > 0u))
    {
        uint3 tops = input.Tops;
        uint3 covers = input.Covers;
        // A corner whose own ground is what is painted over the others shows
        // it wholly (D396).
        [unroll] for (uint corner = 0; corner < 3u; ++corner)
        {
            const uint own = input.Materials[corner];
            const bool paintedHere = (covers.x > 0u && tops.x == own) || (covers.y > 0u && tops.y == own) ||
                                     (covers.z > 0u && tops.z == own);
            if (covers[corner] == 0u && paintedHere) {
                tops[corner] = own;
                covers[corner] = 255u;
            }
        }
        float3 shares = input.Corners * (float3(covers) / 255.0f);
        if (tops.y == tops.x) {
            shares.x += shares.y;
            shares.y = 0.0f;
        }
        if (tops.z == tops.x) {
            shares.x += shares.z;
            shares.z = 0.0f;
        }
        else if (tops.z == tops.y) {
            shares.y += shares.z;
            shares.z = 0.0f;
        }
        const float cover = saturate(shares.x + shares.y + shares.z);
        const uint lead = shares.x >= shares.y && shares.x >= shares.z ? tops.x : (shares.y >= shares.z ? tops.y : tops.z);
        [branch] if (cover > 0.001f)
            albedo = lerp(albedo, fastLayer(lead, ground, false), smoothstep(0.0f, 1.0f, cover));
    }

    // **The rules** (ADR 0113 §2), each by how far the pixel is inside its
    // slope and height bands, ragged by its noise -- the full ground's
    // arithmetic -- and the one that covers most laid over the pixel. Not
    // asked at all where the ground is flatter than the flattest of them
    // begins (`TerrainDebug.w`).
    const float slopeNow = 1.0f - saturate(normal.y);
    const uint ruleCount = slopeNow < TerrainDebug.w ? 0u : min(uint(TerrainParams.y + 0.5f), 16u);
    [branch] if (ruleCount > 0u)
    {
        const float ruleNoise = terrainRuleNoise(input.Ground.x, input.Ground.z);
        const float worldY = input.Ground.y + TerrainParams.w;
        float most = 0.0f;
        uint mostLayer = 0u;
        [loop] for (uint rule = 0u; rule < ruleCount; ++rule)
        {
            const float4 misc = RuleMisc[rule];
            const float jitter = (ruleNoise - 0.5f) * misc.y;
            const float slope = slopeNow + jitter;
            const float height = worldY + jitter * misc.z;
            const float4 slopeBand = RuleSlope[rule];
            const float4 heightBand = RuleHeight[rule];
            float cover = smoothstep(slopeBand.x, slopeBand.y, slope) * (1.0f - smoothstep(slopeBand.z, slopeBand.w, slope)) *
                          smoothstep(heightBand.x, heightBand.y, height) *
                          (1.0f - smoothstep(heightBand.z, heightBand.w, height));
            cover *= (ruleCovers(rule, input.Materials.x) ? input.Corners.x : 0.0f) +
                     (ruleCovers(rule, input.Materials.y) ? input.Corners.y : 0.0f) +
                     (ruleCovers(rule, input.Materials.z) ? input.Corners.z : 0.0f);
            cover *= step(0.5f, misc.w);
            if (cover > most) {
                most = cover;
                mostLayer = uint(misc.x + 0.5f);
            }
        }
        [branch] if (most > 0.001f)
            albedo = lerp(albedo, fastLayer(mostLayer, ground, false), most);
    }

    // --- Lit: the sun through its shadow, the sky's irradiance, the ambient,
    // and the lights of the cluster. Diffuse alone.
    const float3 sunDirection = normalize(SunDirectionBrightness.xyz);
    const float nol = saturate(dot(normal, sunDirection));
    float3 color = float3(0.0f, 0.0f, 0.0f);
    [branch] if (SunDirectionBrightness.w > 0.0f && nol > 0.0f)
    {
        const float shadow = fastSunShadow(input.ShadingPosition, normal, nol, viewDepth, input.Position.xy);
        // Less the four in a hundred a dielectric reflects, as the full
        // surface's diffuse lobe is.
        color = albedo * (1.0f - EngineDielectricF0) / EnginePi * SunColorUnused.rgb *
                (SunDirectionBrightness.w * shadow * nol);
    }
    const Surface surface = makeSurface(input.ShadingPosition, normal, albedo, 0.0f, 1.0f);
    color += fastLights(surface, input.Position.xy, viewDepth);
    // The sky's light reaches as much of the ground as sees the sky; and the
    // ambient is the outdoor one there, the indoor one under a roof (ADR 0084).
    color += albedo * evaluateIrradiance(IrradianceSh, normal) * (EnvironmentParams.y * input.Sky);
    color += lerp(Ambient.rgb, OutdoorAmbient.rgb, input.Sky) * albedo;
    color = applyFog(color, FogColor.rgb, FogRange, length(input.ShadingPosition));
    return float4(color, 1.0f);
}
