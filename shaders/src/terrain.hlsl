// The forward pass for terrain (ADR 0082): every node of every terrain's
// level-of-detail quadtree, meshed from the voxels on the CPU.
//
// **A terrain's layers are materials** (ADR 0113), and their textures are three
// arrays -- colour, normal, surface -- with one slice per layer, bound per
// terrain. A pixel blends up to three layers: the mesher gives every vertex its
// triangle's three material ids and which corner it is, so the corner weights
// interpolate into how far the pixel is from each. Each layer is sampled
// triplanar, pinned to the field's own coordinates, at its own repeat. The
// procedural variation of the old ground stays on top as a subtle macro
// shading, and steep ground still turns to the rock layer.
//
// The vertex layout is `asset::Vertex`, the same 48 bytes every static mesh
// uses, so a node travels through `MeshCache` like any other mesh: the
// material id's old seat (the tangent's x) and the sky (its y) are where they
// were, and the UVs carry the triangle's three ids and the corner.

#define ENG_UNIFORMS_OBJECT
#define ENG_UNIFORMS_FRAME
#include "engine_forward.hlsli"
#include "engine_terrain_surface.hlsli"

// Per terrain, indexed by material id (0 is air and unused).
cbuffer GpuTerrainSurfaceUniforms : register(b1, space3)
{
    // What a layer looks like with no texture yet: its colour, flat.
    float4 LayerFlat[256];
    // The material's colour factor in rgb, and one over its repeat in metres.
    float4 LayerTint[256];
    // Roughness factor, metalness factor, normal scale, and 1 for triplanar.
    float4 LayerSurface[256];
    // x: 1 when the arrays hold every layer; y: the layer steep ground turns
    // to, or 0 for none; z: the layer count.
    float4 TerrainParams;
};

Texture2DArray LayerColorTexture : register(t13, space2);
SamplerState LayerColorSampler : register(s13, space2);
Texture2DArray LayerNormalTexture : register(t14, space2);
SamplerState LayerNormalSampler : register(s14, space2);
Texture2DArray LayerSurfaceTexture : register(t15, space2);
SamplerState LayerSurfaceSampler : register(s15, space2);

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
};

struct TerrainInterpolants
{
    float3 ShadingPosition : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    // Field space: the coordinate the ground's textures and noise are pinned to.
    float3 Ground : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
    // How much of the sky the vertex sees (`skyVisibility` in the mesher).
    float Sky : TEXCOORD4;
    // How near the pixel is to each of the triangle's corners.
    float3 Corners : TEXCOORD5;
    nointerpolation uint3 Materials : TEXCOORD6;
    float4 Position : SV_Position;
};

TerrainInterpolants VertexMain(VertexInput input)
{
    TerrainInterpolants output;
    const float4 shadingPosition = mul(Model, float4(input.Position, 1.0f));
    output.ShadingPosition = shadingPosition.xyz;
    output.Position = mul(ViewProjection, shadingPosition);
    output.ViewDepth = output.Position.w;
    output.Normal = mul((float3x3)NormalMatrix, input.Normal);
    // The mesher works in the field's own space, so the untransformed
    // position IS the field coordinate.
    output.Ground = input.Position.xyz;
    output.Sky = saturate(input.Tangent.y);
    const uint packed = uint(input.Uv.x + 0.5f);
    output.Materials = uint3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u);
    const uint corner = uint(input.Uv.y + 0.5f);
    output.Corners = float3(corner == 0u ? 1.0f : 0.0f, corner == 1u ? 1.0f : 0.0f, corner == 2u ? 1.0f : 0.0f);
    return output;
}

struct LayerSample
{
    float3 Albedo;
    // World space, already bent by the layer's normal map.
    float3 Normal;
    float Roughness;
    float Metalness;
    float Occlusion;
};

LayerSample weighted(LayerSample sum, LayerSample value, float weight)
{
    sum.Albedo += value.Albedo * weight;
    sum.Normal += value.Normal * weight;
    sum.Roughness += value.Roughness * weight;
    sum.Metalness += value.Metalness * weight;
    sum.Occlusion += value.Occlusion * weight;
    return sum;
}

// One projection of one layer: the plane's coordinates and their derivatives,
// taken before any branch so the mip is the right one wherever it is read.
struct Plane
{
    float2 Uv;
    float2 Dx;
    float2 Dy;
};

Plane planeOf(float2 uv, float2 dx, float2 dy, float scale)
{
    Plane plane;
    plane.Uv = uv * scale;
    plane.Dx = dx * scale;
    plane.Dy = dy * scale;
    return plane;
}

// The layer's three maps at one plane; the normal is the map's tangent-space
// xy, scaled.
void readPlane(Plane plane, float slice, float normalScale, out float3 albedo, out float2 bend, out float3 surface)
{
    const float3 at = float3(plane.Uv, slice);
    albedo = LayerColorTexture.SampleGrad(LayerColorSampler, at, plane.Dx, plane.Dy).rgb;
    bend = (LayerNormalTexture.SampleGrad(LayerNormalSampler, at, plane.Dx, plane.Dy).xy * 2.0f - 1.0f) * normalScale;
    surface = LayerSurfaceTexture.SampleGrad(LayerSurfaceSampler, at, plane.Dx, plane.Dy).rgb;
}

LayerSample sampleLayer(uint id, float3 ground, float3 dx, float3 dy, float3 normal, float3 planes)
{
    const float4 tint = LayerTint[id];
    const float4 settings = LayerSurface[id];
    const float slice = float(id) - 1.0f;
    const float scale = tint.a;

    float3 albedo = 0.0f;
    float3 surface = 0.0f;
    float3 bent = normal;
    if (settings.w > 0.5f) {
        // **Triplanar**, each plane only where the surface faces it enough to
        // matter, and the normal bent along each plane's own axes.
        float kept = 0.0f;
        [branch] if (planes.y > 0.01f)
        {
            float3 a;
            float2 b;
            float3 s;
            readPlane(planeOf(ground.xz, dx.xz, dy.xz, scale), slice, settings.z, a, b, s);
            albedo += a * planes.y;
            surface += s * planes.y;
            bent += float3(b.x, 0.0f, b.y) * planes.y;
            kept += planes.y;
        }
        [branch] if (planes.x > 0.01f)
        {
            float3 a;
            float2 b;
            float3 s;
            readPlane(planeOf(ground.zy, dx.zy, dy.zy, scale), slice, settings.z, a, b, s);
            albedo += a * planes.x;
            surface += s * planes.x;
            bent += float3(0.0f, b.y, b.x) * planes.x;
            kept += planes.x;
        }
        [branch] if (planes.z > 0.01f)
        {
            float3 a;
            float2 b;
            float3 s;
            readPlane(planeOf(ground.xy, dx.xy, dy.xy, scale), slice, settings.z, a, b, s);
            albedo += a * planes.z;
            surface += s * planes.z;
            bent += float3(b.x, b.y, 0.0f) * planes.z;
            kept += planes.z;
        }
        const float share = 1.0f / max(kept, 1e-5f);
        albedo *= share;
        surface *= share;
    }
    else {
        float2 b;
        readPlane(planeOf(ground.xz, dx.xz, dy.xz, scale), slice, settings.z, albedo, b, surface);
        bent += float3(b.x, 0.0f, b.y);
    }

    LayerSample result;
    result.Albedo = albedo * tint.rgb;
    result.Normal = normalize(bent);
    result.Occlusion = surface.r;
    result.Roughness = saturate(surface.g * settings.x);
    result.Metalness = saturate(surface.b * settings.y);
    return result;
}

// What a layer is before its textures arrive: flat, at its material's
// roughness.
LayerSample flatLayer(uint id, float3 normal)
{
    LayerSample result;
    result.Albedo = LayerFlat[id].rgb;
    result.Normal = normal;
    result.Roughness = saturate(LayerSurface[id].x);
    result.Metalness = saturate(LayerSurface[id].y);
    result.Occlusion = 1.0f;
    return result;
}

LayerSample layerAt(uint id, float3 ground, float3 dx, float3 dy, float3 normal, float3 planes)
{
    if (TerrainParams.x > 0.5f && float(id) <= TerrainParams.z)
        return sampleLayer(id, ground, dx, dy, normal, planes);
    return flatLayer(id, normal);
}

float4 FragmentMain(TerrainInterpolants input) : SV_Target0
{
    const float3 normal = normalize(input.Normal);
    const float3 dx = ddx(input.Ground);
    const float3 dy = ddy(input.Ground);
    float3 planes = abs(normal);
    planes *= planes;
    planes *= planes;
    planes /= max(planes.x + planes.y + planes.z, 1e-5f);

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

    LayerSample mix = (LayerSample)0;
    mix = weighted(mix, layerAt(ids.x, input.Ground, dx, dy, normal, planes), corners.x);
    [branch] if (corners.y > 0.001f)
        mix = weighted(mix, layerAt(ids.y, input.Ground, dx, dy, normal, planes), corners.y);
    [branch] if (corners.z > 0.001f)
        mix = weighted(mix, layerAt(ids.z, input.Ground, dx, dy, normal, planes), corners.z);

    // **Steep ground is the rock layer**, whatever it was painted, with the
    // ragged edge the old ground had -- except where the layer already is rock
    // or basalt. `TerrainParams.y` names the layer; zero turns it off.
    const TerrainVariation variation = terrainVariation(input.Ground, normal);
    const uint rockLayer = uint(TerrainParams.y + 0.5f);
    if (rockLayer != 0u) {
        const float keepsItself = (ids.x == ENG_TERRAIN_ROCK || ids.x == ENG_TERRAIN_BASALT ? corners.x : 0.0f) +
                                  (ids.y == ENG_TERRAIN_ROCK || ids.y == ENG_TERRAIN_BASALT ? corners.y : 0.0f) +
                                  (ids.z == ENG_TERRAIN_ROCK || ids.z == ENG_TERRAIN_BASALT ? corners.z : 0.0f);
        const float rock = variation.Rockiness * (1.0f - keepsItself);
        [branch] if (rock > 0.001f)
        {
            const LayerSample stone = layerAt(rockLayer, input.Ground, dx, dy, normal, planes);
            mix.Albedo = lerp(mix.Albedo, stone.Albedo, rock);
            mix.Normal = lerp(mix.Normal, stone.Normal, rock);
            mix.Roughness = lerp(mix.Roughness, stone.Roughness, rock);
            mix.Metalness = lerp(mix.Metalness, stone.Metalness, rock);
            mix.Occlusion = lerp(mix.Occlusion, stone.Occlusion, rock);
        }
    }

    // **The forward layout's four material slots are still read**, as the
    // neutral stand-ins the renderer binds there -- white, flat, white, black --
    // so the texture slots stay the contiguous run SDL_GPU binds from zero.
    const float2 standIn = input.Ground.xz;
    const float3 albedo = mix.Albedo * variation.Shade * BaseColorTexture.Sample(BaseColorSampler, standIn).rgb;
    const float roughness = mix.Roughness * MetallicRoughnessTexture.Sample(MetallicRoughnessSampler, standIn).g;
    const float2 flat = NormalTexture.Sample(NormalSampler, standIn).xy * 2.0f - 1.0f;
    const float3x3 frame = tangentFrame(normal, float4(1.0f, 0.0f, 0.0f, 1.0f));
    const float3 nudge = mul(normalize(float3(variation.NormalNudge + flat, 1.0f)), frame) - normal;
    const float3 shadingNormal = normalize(normalize(mix.Normal) + nudge);

    Surface surface = makeSurface(input.ShadingPosition, shadingNormal, albedo, mix.Metalness, roughness);
    float3 color = lightSurface(surface, input.ShadingPosition, shadingNormal, input.ViewDepth, input.Position.xy,
                                input.Sky * lerp(1.0f, mix.Occlusion, 0.6f));
    color += EmissiveTexture.Sample(EmissiveSampler, standIn).rgb;
    color = applyFog(color, FogColor.rgb, FogRange, length(input.ShadingPosition));
    return float4(color, 1.0f);
}
