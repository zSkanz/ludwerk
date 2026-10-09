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
// shading, and the terrain's rules paint by slope and height over it.
//
// The vertex layout is `asset::Vertex`, the same 48 bytes every static mesh
// uses, so a node travels through `MeshCache` like any other mesh, read the
// terrain's own way (ADR 0140; the loader packs it): the normal octahedral in
// x and y, the sky, the triangle corner and the vertex's seams in the tangent's
// y as `sky + 2 * corner + 8 * tag`, the triangle's three layers in the UV's x,
// and the geomorph's offset in `(normal.z, tangent.x, uv.y)`
// (`engine_terrain_morph.hlsli`).

#define ENG_UNIFORMS_OBJECT
#define ENG_UNIFORMS_FRAME
#include "engine_forward.hlsli"
#include "engine_terrain_surface.hlsli"

// The geomorph (ADR 0140): this node's range and its neighbours'.
cbuffer GpuTerrainMorph : register(b1, space1)
{
    float4 Morph[9];
};

#include "engine_terrain_morph.hlsli"

// Per terrain.
cbuffer GpuTerrainSurfaceUniforms : register(b1, space3)
{
    // The rules (ADR 0113 §2), as `asset::TerrainRuleShape` builds them: the
    // slope band's four edges in `1 - normal.y`, the height band's in metres,
    // then the layer, the noise, the height jitter and whether it is on, then
    // the 256 layers it covers as bits.
    float4 RuleSlope[16];
    float4 RuleHeight[16];
    float4 RuleMisc[16];
    uint4 RuleApplies[32];
    // x: 1 when the arrays hold every layer; y: the rule count; z: the layer
    // count; w: the terrain's height in the world.
    float4 TerrainParams;
    // x: the debug view drawn instead of the ground (`render::DebugView`,
    // terrain audit T0): 0 none, 1 holes, 2 level, 3 sky, 4 shadow, 5
    // occlusion, 6 bend, 7 albedo, 8 material. y: 1 for the lean ground
    // (ADR 0175). z: never set -- see where the stand-ins are named.
    float4 TerrainDebug;
};

Texture2DArray LayerColorTexture : register(t13, space2);
SamplerState LayerColorSampler : register(s13, space2);
Texture2DArray LayerNormalTexture : register(t14, space2);
SamplerState LayerNormalSampler : register(s14, space2);
Texture2DArray LayerSurfaceTexture : register(t15, space2);
SamplerState LayerSurfaceSampler : register(s15, space2);

// **Each layer, by material id** (0 is air and unused): `render::GpuTerrainLayer`.
// A storage buffer rather than the block above, which SDL_GPU binds to Vulkan
// 4 KiB at a time (D379).
struct TerrainLayer
{
    // What the layer looks like with no texture yet: its colour, flat; and in
    // a, how hard its paint meets what is under it (`BlendSharpness`).
    float4 Flat;
    // The material's colour factor in rgb, and one over its repeat in metres.
    float4 Tint;
    // Roughness factor, metalness factor, normal scale, and flags: 1
    // triplanar, 2 a height map.
    float4 Surface;
    // **How the repeat is broken up** (ADR 0113's amendment): x how much a
    // pattern tens of metres across varies the colour; y the second sample's
    // scale against the first (1 none); z 1 when the maps are laid on a
    // hexagonal grid of random cells.
    float4 Tiling;
    // **The light the layer gives off** (`Material.Emissive`) in rgb: added
    // to the lit ground, by as much of the pixel as the layer is.
    float4 Emissive;
};
StructuredBuffer<TerrainLayer> TerrainLayers : register(t16, space2);

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
    // What is painted over each corner, and how much of it shows, 0 to 255
    // (ADR 0114).
    nointerpolation uint3 Tops : TEXCOORD7;
    nointerpolation uint3 Covers : TEXCOORD8;
    // The node's level of detail, for the debug view of levels.
    nointerpolation float Level : TEXCOORD9;
    float4 Position : SV_Position;
};

// **A normal folded octahedrally into two floats** (ADR 0140; the loader packs
// it so, to free the normal's z for the geomorph).
float3 terrainNormal(float2 folded)
{
    float3 n = float3(folded.x, folded.y, 1.0f - abs(folded.x) - abs(folded.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return normalize(n);
}

TerrainInterpolants VertexMain(VertexInput input)
{
    TerrainInterpolants output;
    const TerrainSeams seams = terrainSeams(input.Tangent.y);
    const float3 position = terrainMorphed(input.Position, float3(input.Normal.z, input.Tangent.x, input.Uv.y),
                                           seams.tag, mul(Model, float4(input.Position, 1.0f)).xyz);
    const float4 shadingPosition = mul(Model, float4(position, 1.0f));
    output.ShadingPosition = shadingPosition.xyz;
    output.Position = mul(ViewProjection, shadingPosition);
    output.ViewDepth = output.Position.w;
    output.Normal = mul((float3x3)NormalMatrix, terrainNormal(input.Normal.xy));
    // The mesher works in the field's own space, so the untransformed
    // position IS the field coordinate.
    output.Ground = position;
    const float corner = seams.corner;
    output.Sky = seams.sky;
    const uint packed = uint(input.Uv.x + 0.5f);
    output.Materials = uint3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u);
    output.Corners = float3(corner < 0.5f ? 1.0f : 0.0f, corner > 0.5f && corner < 1.5f ? 1.0f : 0.0f,
                            corner > 1.5f ? 1.0f : 0.0f);
    // The paint's two slots of the tangent, packed as the materials are.
    const uint tops = uint(input.Tangent.z + 0.5f);
    output.Tops = uint3(tops & 255u, (tops >> 8) & 255u, (tops >> 16) & 255u);
    const uint covers = uint(input.Tangent.w + 0.5f);
    output.Covers = uint3(covers & 255u, (covers >> 8) & 255u, (covers >> 16) & 255u);
    output.Level = InstanceAlphaUnused.y;
    return output;
}

// **What a debug view draws in place of the ground** (terrain audit T0), or a
// negative alpha for none. Each answers one of the audit's questions alone:
// where the ground is at all, which level drew it, and what the sky term, the
// sun's shadow and the occlusion say about it.
float4 terrainDebugColor(TerrainInterpolants input, float3 normal)
{
    const uint view = uint(TerrainDebug.x + 0.5f);
    if (view == 1u)
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (view == 2u) {
        static const float3 Levels[6] = {
            float3(0.95f, 0.95f, 0.95f), float3(0.25f, 0.55f, 1.0f), float3(0.2f, 0.8f, 0.35f),
            float3(1.0f, 0.85f, 0.2f),   float3(1.0f, 0.5f, 0.15f),  float3(0.85f, 0.2f, 0.2f),
        };
        const float3 lit = Levels[min(uint(input.Level + 0.5f), 5u)];
        // A little shape, so a level's surface still reads as one.
        return float4(lit * (0.55f + 0.45f * saturate(normal.y * 0.5f + 0.5f)), 1.0f);
    }
    if (view == 3u)
        return float4(input.Sky.xxx, 1.0f);
    if (view == 4u) {
        const float3 sunDirection = normalize(SunDirectionBrightness.xyz);
        const float sunNol = saturate(dot(normal, sunDirection));
        const float shadow = sampleSunShadow(ShadowMap, ShadowSampler, input.ShadingPosition, normal, sunNol,
                                             viewDepthOf(input.ShadingPosition, input.ViewDepth),
                                             input.Position.xy);
        const float contact =
            ContactShadowTexture.SampleLevel(ContactShadowSampler, input.Position.xy * ViewportParams.zw, 0.0f);
        // The map in red, the contact mask in green: yellow is lit by both.
        // Blue where the ground faces the sun by more than a grazing angle, so
        // a check can tell a lit face the map darkens (acne, terrain audit
        // TA8) from a face turned away: white is right, blue alone is wrong.
        // Faces by its normal AND by its triangle: along a coarse shape's
        // terminator a triangle turned from the sun is shaded by normals that
        // still face it, and the map is right to hold it dark.
        float3 facet = cross(ddy(input.ShadingPosition), ddx(input.ShadingPosition));
        facet = dot(facet, normal) < 0.0f ? -facet : facet;
        const float facetNol = dot(normalize(facet), sunDirection);
        return float4(shadow, contact, min(sunNol, facetNol) >= 0.1f ? 1.0f : 0.0f, 1.0f);
    }
    if (view == 5u) {
        const float occlusion =
            OcclusionTexture.SampleLevel(OcclusionSampler, input.Position.xy * ViewportParams.zw, 0.0f);
        return float4(occlusion.xxx, 1.0f);
    }
    return float4(0.0f, 0.0f, 0.0f, -1.0f);
}

// **The rules' noise, line for line `asset::terrainRuleNoise`**: an integer
// hash on a 4.3 m lattice over the field's x and z, so the CPU's answer to
// "what is drawn here" is this one.
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

struct LayerSample
{
    float3 Albedo;
    // World space, already bent by the layer's normal map.
    float3 Normal;
    float Roughness;
    float Metalness;
    // What the sky's light is darkened by: the height's cracks, where the
    // material has a height map, and nothing where it has none.
    float Occlusion;
    // For the height blend where a painted layer meets what is under it
    // (ADR 0114).
    float Height;
    // The light it gives off: its material's `Emissive`, and no map of it.
    float3 Emissive;
};

LayerSample weighted(LayerSample sum, LayerSample value, float weight)
{
    sum.Albedo += value.Albedo * weight;
    sum.Normal += value.Normal * weight;
    sum.Roughness += value.Roughness * weight;
    sum.Metalness += value.Metalness * weight;
    sum.Occlusion += value.Occlusion * weight;
    sum.Height += value.Height * weight;
    sum.Emissive += value.Emissive * weight;
    return sum;
}

// A height from its map, on the scale the engine's own layers were drawn with:
// a crack at 0.55, a top at 1 -- so their occlusion and their height blends are
// what they were when the surface map's R held this.
float layerHeight(float map)
{
    return 0.55f + 0.45f * map;
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
void readPlainPlane(Plane plane, float slice, float normalScale, out float3 albedo, out float2 bend,
                    out float3 surface)
{
    const float3 at = float3(plane.Uv, slice);
    albedo = LayerColorTexture.SampleGrad(LayerColorSampler, at, plane.Dx, plane.Dy).rgb;
    bend = (LayerNormalTexture.SampleGrad(LayerNormalSampler, at, plane.Dx, plane.Dy).xy * 2.0f - 1.0f) * normalScale;
    surface = LayerSurfaceTexture.SampleGrad(LayerSurfaceSampler, at, plane.Dx, plane.Dy).rgb;
}

// **Hex tiling** (ADR 0113's amendment; after Mikkelsen, "Practical Real-Time
// Hex-Tiling", 2022): the plane is covered by a triangle grid whose vertices
// are the centres of hexagonal cells; each cell reads the maps offset and
// turned by its own random amount, and a pixel blends the three cells round it
// by its barycentric weights, sharpened so the blend is a thin band. The hash
// is the rules' integer one, so the cells are the same on every machine.
void readHexPlane(Plane plane, float slice, float normalScale, out float3 albedo, out float2 bend,
                  out float3 surface)
{
    // Skewed into a triangle grid, one cell about one repeat across.
    const float2 st = plane.Uv * 3.4641016f;
    const float2 skewed = float2(st.x - 0.57735027f * st.y, 1.15470054f * st.y);
    const float2 base = floor(skewed);
    float3 temp = float3(skewed - base, 0.0f);
    temp.z = 1.0f - temp.x - temp.y;
    const float s = temp.z < 0.0f ? 1.0f : 0.0f;
    const float s2 = 2.0f * s - 1.0f;
    float3 weights = float3(-temp.z * s2, s - temp.y * s2, s - temp.x * s2);
    const float2 vertices[3] = {base + float2(s, s), base + float2(s, 1.0f - s), base + float2(1.0f - s, s)};
    // Sharpened: a pixel is one cell's except near the borders.
    weights = pow(max(weights, 0.0f), 7.0f);
    weights /= max(weights.x + weights.y + weights.z, 1e-6f);

    albedo = 0.0f;
    bend = 0.0f;
    surface = 0.0f;
    [unroll] for (uint corner = 0; corner < 3u; ++corner)
    {
        const uint h = terrainRuleHash(int(vertices[corner].x), int(vertices[corner].y));
        const float angle = terrainRuleUnit(h) * 6.2831853f;
        const float2 offset = float2(terrainRuleUnit(h * 0x9E3779B9u), terrainRuleUnit(h * 0x85EBCA6Bu));
        const float c = cos(angle);
        const float sn = sin(angle);
        // Turned about the cell's own centre, then moved.
        const float2x2 turn = float2x2(c, -sn, sn, c);
        Plane cell;
        cell.Uv = mul(turn, plane.Uv) + offset;
        cell.Dx = mul(turn, plane.Dx);
        cell.Dy = mul(turn, plane.Dy);
        float3 a;
        float2 b;
        float3 m;
        readPlainPlane(cell, slice, normalScale, a, b, m);
        // The normal's bend turned back into the plane's axes.
        const float2x2 back = float2x2(c, sn, -sn, c);
        albedo += a * weights[corner];
        bend += mul(back, b) * weights[corner];
        surface += m * weights[corner];
    }
}

// **How far the pixel is**, set once per pixel: the second sample's blend is
// by distance.
static float s_viewDepth = 0.0f;

// **The lean ground** (ADR 0175), set once per pixel from `TerrainDebug.y`: the
// level's -- Low, and a handheld's Medium -- or the project's own word
// (`[graphics] terrain_surface`). A layer's maps are read once at a plane and
// its colour once more at the far scale: no hexagonal cells, no normal or
// surface at the far scale, one noise for the whole pixel where there were
// eleven, a plane only where the ground faces it by a quarter, and none of the
// procedural variation over it. The ground is the largest thing on a phone's
// screen, and its fragment was the frame.
static bool s_lean = false;
// The lean ground's one noise, 37 m across: the colour's drift and how much of
// the far scale shows, both. Set once per pixel.
static float s_leanPatch = 0.5f;

// The maps at one plane, with the repeat broken up: hex tiling where the
// material asks for it, and the same maps again at `tiling.y` of the scale,
// blended in with distance -- a quarter near, three quarters far.
void readPlane(Plane plane, float slice, float normalScale, float4 tiling, out float3 albedo, out float2 bend,
               out float3 surface)
{
#ifndef ENG_TERRAIN_NO_HEX
    [branch] if (tiling.z > 0.5f && !s_lean)
        readHexPlane(plane, slice, normalScale, albedo, bend, surface);
    else
#endif
        readPlainPlane(plane, slice, normalScale, albedo, bend, surface);
    [branch] if (tiling.y < 0.999f && s_lean)
    {
        // **The colour alone at the far scale**: one read where the full
        // ground takes three and bends them by two noises. It is the colour's
        // repeat the eye finds across a field -- drawn without this, a grass
        // texture is a grid of its own yellow patches -- and the normal's and
        // the roughness's repeat it does not.
        const float2x2 turn = float2x2(0.82533561f, -0.56464247f, 0.56464247f, 0.82533561f);
        const float3 wide = float3(mul(turn, plane.Uv) * tiling.y + 0.37f, slice);
        const float3 a = LayerColorTexture
                             .SampleGrad(LayerColorSampler, wide, mul(turn, plane.Dx) * tiling.y,
                                         mul(turn, plane.Dy) * tiling.y)
                             .rgb;
        const float far = smoothstep(2.0f, 24.0f, s_viewDepth * tiling.w);
        albedo = lerp(albedo, a, saturate(lerp(0.25f, 0.75f, far) + (s_leanPatch - 0.5f) * 1.5f));
    }
    [branch] if (tiling.y < 0.999f && !s_lean)
    {
        // Turned by 0.6 radians as well as scaled, so the two lattices never
        // line up: scaled alone, a sixth of the scale repeats every six fine
        // repeats and the sum is a grid six times larger.
        const float2x2 turn = float2x2(0.82533561f, -0.56464247f, 0.56464247f, 0.82533561f);
        Plane wide;
        wide.Uv = mul(turn, plane.Uv) * tiling.y + 0.37f;
        // And bent by a slow noise, a third of its repeat either way over
        // about one and a half: turned, its own lattice still repeats, only
        // along other axes. Too slow a bend to count in the gradients.
        const float2 bendAt = wide.Uv * (4.3f / 1.5f);
        wide.Uv += (float2(terrainRuleNoise(bendAt.x, bendAt.y), terrainRuleNoise(bendAt.x + 53.0f, bendAt.y - 17.0f)) -
                    0.5f) *
                   0.66f;
        wide.Dx = mul(turn, plane.Dx) * tiling.y;
        wide.Dy = mul(turn, plane.Dy) * tiling.y;
        float3 a;
        float2 b;
        float3 m;
        readPlainPlane(wide, slice, normalScale, a, b, m);
        b = mul(transpose(turn), b);
        // The distance in repeats of the fine sample (`tiling.w` is one over
        // its metres), so a small tile and a large one give way alike: a
        // quarter near, three quarters far. And more or less of it from patch
        // to patch, a noise some two and a half repeats across: a share that
        // is the same everywhere dims the fine repeat without breaking it.
        const float far = smoothstep(2.0f, 24.0f, s_viewDepth * tiling.w);
        const float patch = terrainRuleNoise(plane.Uv.x * 1.72f, plane.Uv.y * 1.72f);
        const float share = saturate(lerp(0.25f, 0.75f, far) + (patch - 0.5f) * 1.5f);
        albedo = lerp(albedo, a, share);
        bend = lerp(bend, b, share);
        surface = lerp(surface, m, share);
    }
}

// **The colour's large-scale drift** on a textured layer (ADR 0113's
// amendment): two octaves of the rules' noise, 37 and 13 m, centred on one.
// Not on plain ground, which stays plain (TA12).
float3 tilingVariation(float3 albedo, float3 ground, float strength)
{
    // A material that asks for none is not given two noises to multiply by
    // one.
    [branch] if (strength <= 0.0f)
        return albedo;
    // The lean ground has the broad octave already -- it is the pixel's one
    // noise -- and takes it for both.
    float broad = s_leanPatch;
    float fine = s_leanPatch;
    [branch] if (!s_lean)
    {
        broad = terrainRuleNoise(ground.x * (4.3f / 37.0f), ground.z * (4.3f / 37.0f));
        fine = terrainRuleNoise(ground.x * (4.3f / 13.0f) + 71.0f, ground.z * (4.3f / 13.0f) - 29.0f);
    }
    const float n = broad * 0.7f + fine * 0.3f - 0.5f;
    // Brighter and a little warmer one way, darker and cooler the other.
    const float3 shift = float3(1.0f + 0.55f * n, 1.0f + 0.45f * n, 1.0f + 0.3f * n);
    return albedo * lerp(float3(1.0f, 1.0f, 1.0f), shift, strength);
}

LayerSample sampleLayer(uint id, float3 ground, float3 dx, float3 dy, float3 normal, float3 planes)
{
    const float4 tint = TerrainLayers[id].Tint;
    const float4 settings = TerrainLayers[id].Surface;
    // The repeat's breaking-up, with the fine repeat's metres in w for the
    // distance blend.
    const float4 tiling = float4(TerrainLayers[id].Tiling.xyz, tint.a);
    const float slice = float(id) - 1.0f;
    const float scale = tint.a;
    // Flags (`TerrainLoader::appendRenderTerrains`): 1 triplanar, 2 a height map.
    const uint flags = uint(settings.w + 0.5f);

    // **A normal map's +Y is up the image** (glTF), and up the image is DOWN
    // the plane's second coordinate, which the image's V runs along -- so each
    // plane bends by its map's x along its first axis and against its second.
    // Along both, which is what this did, lit every bump from the wrong side
    // along one axis (TA13).
    float3 albedo = 0.0f;
    float3 surface = 0.0f;
    float3 bent = normal;
#ifdef ENG_TERRAIN_TRIPLANAR_ONLY
    if (true) {
#else
    if ((flags & 1u) != 0u) {
#endif
        // **Triplanar**, each plane only where the surface faces it enough to
        // matter, and the normal bent along each plane's own axes.
        float kept = 0.0f;
        [branch] if (planes.y > 0.01f)
        {
            float3 a;
            float2 b;
            float3 s;
            readPlane(planeOf(ground.xz, dx.xz, dy.xz, scale), slice, settings.z, tiling, a, b, s);
            albedo += a * planes.y;
            surface += s * planes.y;
            bent += float3(b.x, 0.0f, -b.y) * planes.y;
            kept += planes.y;
        }
        [branch] if (planes.x > 0.01f)
        {
            float3 a;
            float2 b;
            float3 s;
            readPlane(planeOf(ground.zy, dx.zy, dy.zy, scale), slice, settings.z, tiling, a, b, s);
            albedo += a * planes.x;
            surface += s * planes.x;
            bent += float3(0.0f, -b.y, b.x) * planes.x;
            kept += planes.x;
        }
        [branch] if (planes.z > 0.01f)
        {
            float3 a;
            float2 b;
            float3 s;
            readPlane(planeOf(ground.xy, dx.xy, dy.xy, scale), slice, settings.z, tiling, a, b, s);
            albedo += a * planes.z;
            surface += s * planes.z;
            bent += float3(b.x, -b.y, 0.0f) * planes.z;
            kept += planes.z;
        }
        const float share = 1.0f / max(kept, 1e-5f);
        albedo *= share;
        surface *= share;
    }
    else {
        float2 b;
        readPlane(planeOf(ground.xz, dx.xz, dy.xz, scale), slice, settings.z, tiling, albedo, b, surface);
        bent += float3(b.x, 0.0f, -b.y);
    }

    // The surface layer is packed when the arrays are drawn
    // (`terrain_pack.hlsl`): the material's height in R, from its height map,
    // then its metallic-roughness map's G and B.
    LayerSample result;
    result.Albedo = tilingVariation(albedo * tint.rgb, ground, tiling.x);
    result.Normal = terrainUnit(bent, normal);
    result.Height = layerHeight(surface.r);
    result.Occlusion = (flags & 2u) != 0u ? result.Height : 1.0f;
    result.Roughness = saturate(surface.g * settings.x);
    result.Metalness = saturate(surface.b * settings.y);
    result.Emissive = TerrainLayers[id].Emissive.rgb;
    return result;
}

// What a layer is before its textures arrive: flat, at its material's
// roughness.
LayerSample flatLayer(uint id, float3 normal)
{
    LayerSample result;
    result.Albedo = TerrainLayers[id].Flat.rgb;
    result.Normal = normal;
    result.Roughness = saturate(TerrainLayers[id].Surface.x);
    result.Metalness = saturate(TerrainLayers[id].Surface.y);
    result.Occlusion = 1.0f;
    result.Height = layerHeight(0.5f);
    result.Emissive = TerrainLayers[id].Emissive.rgb;
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
    const float3 normal = terrainUnit(input.Normal, float3(0.0f, 1.0f, 0.0f));
    const float4 debugColor = terrainDebugColor(input, normal);
    if (debugColor.a >= 0.0f)
        return debugColor;
    s_viewDepth = viewDepthOf(input.ShadingPosition, input.ViewDepth);
    s_lean = TerrainDebug.y > 0.5f;
    const float3 dx = ddx(input.Ground);
    const float3 dy = ddy(input.Ground);
    float3 planes = abs(normal);
    planes *= planes;
    planes *= planes;
    planes /= max(planes.x + planes.y + planes.z, 1e-5f);
    // Lean: a plane only where the ground faces it by a quarter. A slope of
    // thirty degrees is then one plane's, where it was two at nine to one.
    [branch] if (s_lean)
    {
        planes *= step(float3(0.25f, 0.25f, 0.25f), planes);
        planes /= max(planes.x + planes.y + planes.z, 1e-5f);
        s_leanPatch = terrainRuleNoise(input.Ground.x * (4.3f / 37.0f), input.Ground.z * (4.3f / 37.0f));
    }

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

    // **How much of the pixel is plain ground**, a material no layer names
    // (TA12): a new terrain's ground is plain matte grey (ADR 0113's
    // amendment), and the ground's variation -- the warm and cool drift that
    // breaks up a field of one material -- is no part of that. It read as brown
    // blotches, ball-sized on a ball. Carried through the paint and the rules
    // below, which can lay a layer over it.
    const uint layerCount = uint(TerrainParams.z + 0.5f);
    float plain = (ids.x > layerCount ? corners.x : 0.0f) + (ids.y > layerCount ? corners.y : 0.0f) +
                  (ids.z > layerCount ? corners.z : 0.0f);

    // **The layer the pixel is drawn as**, for the material view: the corner
    // that weighs most, then the paint and the rules from half, as the CPU
    // decides it (`asset::drawnMaterial`).
    uint dominant = corners.x >= corners.y && corners.x >= corners.z ? ids.x : (corners.y >= corners.z ? ids.y : ids.z);

    LayerSample mix = (LayerSample)0;
    mix = weighted(mix, layerAt(ids.x, input.Ground, dx, dy, normal, planes), corners.x);
    [branch] if (corners.y > 0.001f)
        mix = weighted(mix, layerAt(ids.y, input.Ground, dx, dy, normal, planes), corners.y);
    [branch] if (corners.z > 0.001f)
        mix = weighted(mix, layerAt(ids.z, input.Ground, dx, dy, normal, planes), corners.z);

    // **What is painted over it** (ADR 0114): the layers over each corner,
    // weighed by how near the pixel is to the corner and how much of it shows
    // there, and laid over the ground by a height blend -- each layer's
    // occlusion is its height, so what is painted over fills the cracks of what
    // is under before it covers its tops, and the painted layer's material's
    // `BlendSharpness` says how hard that edge is. At no paint and at full cover it is exactly the one or the
    // other: the height only moves the middle.
    [branch] if (any(input.Covers > 0u))
    {
        uint3 tops = input.Tops;
        uint3 covers = input.Covers;
        // **A corner whose own ground is what is painted over the others shows
        // it wholly** (D396): paint blended until it is all that shows becomes
        // the ground, with nothing over it -- and across a triangle from such a
        // corner to one still painted, the cover fell to half in the middle and
        // the ground under the paint showed through as a line inside the
        // painted patch.
        [unroll] for (uint corner = 0; corner < 3u; ++corner)
        {
            const uint own = ids[corner];
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
        [branch] if (cover > 0.001f)
        {
            LayerSample over = (LayerSample)0;
            [branch] if (shares.x > 0.0005f)
                over = weighted(over, layerAt(tops.x, input.Ground, dx, dy, normal, planes), shares.x);
            [branch] if (shares.y > 0.0005f)
                over = weighted(over, layerAt(tops.y, input.Ground, dx, dy, normal, planes), shares.y);
            [branch] if (shares.z > 0.0005f)
                over = weighted(over, layerAt(tops.z, input.Ground, dx, dy, normal, planes), shares.z);
            const float inverse = 1.0f / max(shares.x + shares.y + shares.z, 1e-5f);
            over.Albedo *= inverse;
            over.Normal *= inverse;
            over.Roughness *= inverse;
            over.Metalness *= inverse;
            over.Occlusion *= inverse;
            over.Height *= inverse;
            over.Emissive *= inverse;
            const float lift = (over.Height - mix.Height) * cover * (1.0f - cover) * 4.0f;
            const uint lead = shares.x >= shares.y && shares.x >= shares.z ? tops.x : (shares.y >= shares.z ? tops.y : tops.z);
            // **The sharpness is of the edge the heights draw** (D329): where
            // the two are the same height there is no edge to draw, and a hard
            // one made a threshold at half cover -- a flat-coloured material
            // painted over another came out stepped, voxel by voxel, however
            // soft the brush. There it is a crossfade by the cover instead.
            const float relief = saturate(abs(over.Height - mix.Height) * 4.0f);
            const float width = lerp(0.5f, lerp(0.5f, 0.02f, saturate(TerrainLayers[lead].Flat.a)), relief);
            const float shows = smoothstep(0.5f - width, 0.5f + width, cover + lift);
            mix.Albedo = lerp(mix.Albedo, over.Albedo, shows);
            const float overPlain = ((tops.x > layerCount ? shares.x : 0.0f) + (tops.y > layerCount ? shares.y : 0.0f) +
                                     (tops.z > layerCount ? shares.z : 0.0f)) *
                                    inverse;
            plain = lerp(plain, overPlain, shows);
            dominant = shows > 0.5f ? lead : dominant;
            mix.Normal = lerp(mix.Normal, over.Normal, shows);
            mix.Roughness = lerp(mix.Roughness, over.Roughness, shows);
            mix.Metalness = lerp(mix.Metalness, over.Metalness, shows);
            mix.Occlusion = lerp(mix.Occlusion, over.Occlusion, shows);
            mix.Height = lerp(mix.Height, over.Height, shows);
            mix.Emissive = lerp(mix.Emissive, over.Emissive, shows);
        }
    }

    // **The rules, in order** (ADR 0113 §2): each covers the pixel by how far
    // it is inside the rule's slope and height bands, ragged by the rule's
    // noise, and by how much of the pixel's triangle is a layer the rule may
    // cover -- and paints its own layer over what came before by that much.
    const TerrainVariation variation = terrainVariation(input.Ground, normal, s_lean);
    const uint ruleCount = min(uint(TerrainParams.y + 0.5f), 16u);
    const float worldY = input.Ground.y + TerrainParams.w;
    // The rules' own noise, where there is a rule to be ragged by it.
    float ruleNoise = 0.5f;
    [branch] if (ruleCount > 0u)
        ruleNoise = terrainRuleNoise(input.Ground.x, input.Ground.z);
    [loop] for (uint rule = 0u; rule < ruleCount; ++rule)
    {
        const float4 misc = RuleMisc[rule];
        if (misc.w < 0.5f)
            continue;
        const float jitter = (ruleNoise - 0.5f) * misc.y;
        const float slope = 1.0f - saturate(normal.y) + jitter;
        const float height = worldY + jitter * misc.z;
        const float4 slopeBand = RuleSlope[rule];
        const float4 heightBand = RuleHeight[rule];
        float cover = smoothstep(slopeBand.x, slopeBand.y, slope) * (1.0f - smoothstep(slopeBand.z, slopeBand.w, slope)) *
                      smoothstep(heightBand.x, heightBand.y, height) *
                      (1.0f - smoothstep(heightBand.z, heightBand.w, height));
        cover *= (ruleCovers(rule, ids.x) ? corners.x : 0.0f) + (ruleCovers(rule, ids.y) ? corners.y : 0.0f) +
                 (ruleCovers(rule, ids.z) ? corners.z : 0.0f);
        [branch] if (cover > 0.001f)
        {
            const LayerSample painted = layerAt(uint(misc.x + 0.5f), input.Ground, dx, dy, normal, planes);
            mix.Albedo = lerp(mix.Albedo, painted.Albedo, cover);
            plain = lerp(plain, uint(misc.x + 0.5f) > layerCount ? 1.0f : 0.0f, cover);
            dominant = cover > 0.5f ? uint(misc.x + 0.5f) : dominant;
            mix.Normal = lerp(mix.Normal, painted.Normal, cover);
            mix.Roughness = lerp(mix.Roughness, painted.Roughness, cover);
            mix.Metalness = lerp(mix.Metalness, painted.Metalness, cover);
            mix.Occlusion = lerp(mix.Occlusion, painted.Occlusion, cover);
            mix.Height = lerp(mix.Height, painted.Height, cover);
            mix.Emissive = lerp(mix.Emissive, painted.Emissive, cover);
        }
    }

    // **The forward layout's four material slots stay in the shader and are
    // never read** (ADR 0175). The renderer binds its neutral stand-ins there
    // -- white, flat, white, black -- so the texture slots stay the contiguous
    // run SDL_GPU binds from zero, and a slot the shader does not name is a
    // slot the compiler removes. They were read to keep them: four texture
    // reads at every pixel of the ground, to multiply by one and add nothing.
    // Named inside a branch no frame takes -- `TerrainDebug.z` is never set --
    // they are kept and cost nothing.
    float3 standInColor = float3(1.0f, 1.0f, 1.0f);
    float standInRoughness = 1.0f;
    float3 standInGlow = float3(0.0f, 0.0f, 0.0f);
    [branch] if (TerrainDebug.z > 0.5f)
    {
        const float2 standIn = input.Ground.xz;
        standInColor = BaseColorTexture.SampleLevel(BaseColorSampler, standIn, 0.0f).rgb;
        standInRoughness = MetallicRoughnessTexture.SampleLevel(MetallicRoughnessSampler, standIn, 0.0f).g;
        standInColor *= NormalTexture.SampleLevel(NormalSampler, standIn, 0.0f).z;
        standInGlow = EmissiveTexture.SampleLevel(EmissiveSampler, standIn, 0.0f).rgb;
    }
    const float3 shade = lerp(variation.Shade, float3(1.0f, 1.0f, 1.0f), saturate(plain));
    const float3 albedo = mix.Albedo * shade * standInColor;
    const float roughness = mix.Roughness * standInRoughness;
    const float3 shadingNormal = terrainUnit(terrainUnit(mix.Normal, normal) + variation.Bend, normal);

    Surface surface = makeSurface(input.ShadingPosition, shadingNormal, albedo, mix.Metalness, roughness);
    // The shadow's lookup is offset along the MESH's normal (TA8): the map
    // holds the mesh, and a normal a layer bends points the offset off it.
    float3 color = lightSurface(surface, input.ShadingPosition, normal, input.ViewDepth, input.Position.xy,
                                input.Sky * lerp(1.0f, mix.Occlusion, 0.6f));
    color += standInGlow;
    // What the ground gives off, as a mesh's emission is added: after the
    // light, before the fog -- a glow far off is behind the air too.
    color += mix.Emissive;
    color = applyFog(color, FogColor.rgb, FogRange, length(input.ShadingPosition));
    // The bend view, drawn here rather than with the others: it is what all of
    // the above did to the mesh's normal, four times over so a crease shows.
    if (uint(TerrainDebug.x + 0.5f) == 6u)
        return float4(saturate((shadingNormal - normal) * 4.0f + 0.5f), 1.0f);
    // And the albedo view, the colour before any light.
    if (uint(TerrainDebug.x + 0.5f) == 7u)
        return float4(albedo, 1.0f);
    // And the material view: a colour a layer, none of them black.
    if (uint(TerrainDebug.x + 0.5f) == 8u) {
        const float id = float(dominant);
        return float4(frac(float3(0.618f, 0.382f, 0.791f) * id + float3(0.1f, 0.3f, 0.6f)) * 0.8f + 0.2f, 1.0f);
    }
    return float4(color, 1.0f);
}
