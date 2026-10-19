// The forward pass's fragment stage, shared by `pbr.hlsl` and `pbr_skinned.hlsl`.
//
// **It was a copy until the lighting work.** The input and UI work's note in `pbr_skinned.hlsl` said the right
// thing -- the fragment stage "is shared and is `pbr.hlsl`'s, compiled again"
// -- and then said it in a second file with the whole function pasted into it.
// That held while the function was one screen. This milestone gives it image-
// based lighting, cascades, a cluster lookup and an occlusion term, and two
// copies of that is two shaders that will disagree on the day one of them is
// edited alone.
//
// So the texture slots, the interpolants and the shading live here, and each
// entry file supplies only what actually differs: its vertex INPUT layout and
// its vertex stage. That difference is real and is why there are still two
// pipelines -- one shader with a branch would make every
// static mesh in every world carry joint and weight attributes it never reads.
//
// **Every one of the eleven fragment texture slots must always have something
// bound.** The material's `TextureFlags` are multipliers rather than branches
// (`shader_types.h` says why), so the sample happens whether the material has
// that texture or not, and a slot the renderer leaves empty is an unbound
// descriptor read. A 1x1 default is the whole fix.
#ifndef ENG_FORWARD_HLSLI
#define ENG_FORWARD_HLSLI

#include "engine_brdf.hlsli"

// Slot order is `MaterialDef`'s own field order -- base colour, normal,
// metallic-roughness, emissive -- which is also `TextureFlags`' xyzw order, with
// the shadow map and then the two image-based-lighting tables appended.
//
// **A surface shader's wrapper leaves these four out** (ADR 0091): its surface
// is the user's, so the built-in maps are not read, and the slots are the
// user's first four textures instead -- SDL_GPU allows sixteen samplers a
// stage, and the lighting below takes nine of them.
#if !defined(ENG_SURFACE_WRAPPER)
Texture2D BaseColorTexture : register(t0, space2);
SamplerState BaseColorSampler : register(s0, space2);
Texture2D NormalTexture : register(t1, space2);
SamplerState NormalSampler : register(s1, space2);
Texture2D MetallicRoughnessTexture : register(t2, space2);
SamplerState MetallicRoughnessSampler : register(s2, space2);
Texture2D EmissiveTexture : register(t3, space2);
SamplerState EmissiveSampler : register(s3, space2);
#endif
// The four cascades, as a 2x2 atlas (shadow.h says why). Sampled with `Gather`
// and an explicit bilinear comparison rather than through a comparison sampler,
// so the sampler here is a plain point one.
Texture2D<float> ShadowMap : register(t4, space2);
SamplerState ShadowSampler : register(s4, space2);
// The prefiltered environment, octahedral, one mip per roughness step. See
// `engine_brdf.hlsli`'s image-based-lighting section and
// `engine/render/src/environment.cpp`.
// An array (ADR 0202): the sky in its first layer, then two layers a
// reflection probe -- its reflection by roughness, and its diffuse light --
// and after them the slices of the irradiance volumes (ADR 0203).
Texture2DArray EnvironmentMap : register(t5, space2);
SamplerState EnvironmentSampler : register(s5, space2);
// The split-sum BRDF table: N·V on u, roughness on v, Fresnel scale in R and
// bias in G. Independent of the environment, so it is baked once.
Texture2D BrdfLut : register(t6, space2);
SamplerState BrdfSampler : register(s6, space2);
// The clustered light tables (clusters.h). Three textures because the frozen
// RHI has no storage buffer and a fragment shader's only bulk-data route is a
// sampled texture; point-sampled at texel centres, which is an exact fetch.
//
// The samplers are declared and used rather than skipped: SDL_GPU's binding
// model is texture-and-sampler PAIRS, and a shader that declared a texture
// alone would report a resource count the device then rejects.
Texture2D<float> ClusterGridTexture : register(t7, space2);
SamplerState ClusterGridSampler : register(s7, space2);
Texture2D<float> LightIndexTexture : register(t8, space2);
SamplerState LightIndexSampler : register(s8, space2);
Texture2D LightDataTexture : register(t9, space2);
SamplerState LightDataSampler : register(s9, space2);

// The LOCAL shadow atlas: four by four tiles of 512, one for a spot and six for
// a point (`render::shadow.h`). Separate from `ShadowMap` above, which is the
// sun's four cascades -- a cascade is orthographic and remembers its box between
// frames, a local light is perspective and does not move unless the light does,
// so one texture for both would be one resolution for two budgets.
Texture2D<float> LocalShadowAtlas : register(t11, space2);
SamplerState LocalShadowAtlasSampler : register(s11, space2);

// 1 where the light reaches the fragment, 0 where something is in the way.
//
// **Returns 1 for every light that casts nothing**, which is what makes this
// free in a scene with no shadow-casting local light: the tile is -1, the branch
// is uniform across the wave, and nothing is sampled.
float localShadowFactor(Surface surface, GpuLight light)
{
    const float firstTile = light.Color.w;
    if (firstTile < 0.0f || LocalShadowParams.x <= 0.0f)
        return 1.0f;

    // A point light is the one whose cone admits everything -- the same test
    // `evaluatePunctualLight` below makes, and deliberately the same spelling so
    // the two can never disagree about which kind of light this is.
    const bool isPoint = light.DirectionCosAngle.w <= -1.0f;
    const float3 fromLight = surface.Position - light.PositionRange.xyz;
    const uint tile = uint(firstTile) + (isPoint ? localShadowFace(fromLight) : 0u);
    if (float(tile) >= LocalShadowParams.x)
        return 1.0f;

    const float4 lightClip = mul(LocalShadowViewProjection[tile], float4(surface.Position, 1.0f));
    if (lightClip.w <= 0.0f)
        return 1.0f;

    const float3 projected = lightClip.xyz / lightClip.w;
    // Outside its own tile is not shadowed. A spot's cone is inside its
    // projection by construction (`kSpotFovMargin`), so this is the rim of the
    // map and not the rim of the light.
    if (any(abs(projected.xy) > 1.0f) || projected.z < 0.0f || projected.z > 1.0f)
        return 1.0f;

    // Clip space to the tile's own texels, then to atlas coordinates. The atlas
    // is row-major four by four, which `render::localShadowTileRect` asserts and
    // this arithmetic has to match.
    const float2 tileUv = float2(projected.x * 0.5f + 0.5f, 0.5f - projected.y * 0.5f);
    const float tilesPerSide = 4.0f;
    const float2 tileOrigin = float2(float(tile % 4u), float(tile / 4u)) / tilesPerSide;

    // **Clamped inside the tile, which is the same tax the cascade atlas pays**:
    // a filter tap near a tile edge would otherwise read the neighbouring
    // light's depth and put its shadow in this one.
    const float texel = LocalShadowParams.y;
    const float radius = LocalShadowParams.w;
    const float2 tileMin = tileOrigin + texel;
    const float2 tileMax = tileOrigin + (1.0f / tilesPerSide) - texel;

    const float compare = projected.z - LocalShadowParams.z;

    // Four taps in a rotated-square pattern rather than one. A single tap is a
    // hard binary edge at any resolution; four is enough to read as a penumbra
    // at the tile sizes this atlas uses and is what the sun's own filter starts
    // from.
    float lit = 0.0f;
    const float2 offsets[4] = {
        float2(-0.7f, -0.7f),
        float2(0.7f, -0.7f),
        float2(-0.7f, 0.7f),
        float2(0.7f, 0.7f),
    };
    [unroll] for (uint tap = 0; tap < 4; ++tap)
    {
        const float2 uv = clamp(tileOrigin + tileUv / tilesPerSide + offsets[tap] * radius * texel, tileMin, tileMax);
        const float depth = LocalShadowAtlas.SampleLevel(LocalShadowAtlasSampler, uv, 0).r;
        lit += compare <= depth ? 1.0f : 0.0f;
    }
    return lit * 0.25f;
}
// Screen-space ambient occlusion, from the depth prepass (`ssao.hlsl`). Read in
// SCREEN space rather than interpolated, because it is a property of the pixel
// and not of the surface.
Texture2D<float> OcclusionTexture : register(t10, space2);
SamplerState OcclusionSampler : register(s10, space2);

// The contact-shadow mask (`contact_shadow.hlsl`): 1 lit, 0 where a short ray
// towards the sun passed behind something the depth buffer holds. Screen
// space, like the occlusion above; it darkens the SUN and nothing else.
Texture2D<float> ContactShadowTexture : register(t12, space2);
SamplerState ContactShadowSampler : register(s12, space2);

// **A blended surface's own answer from the scene behind it** (ADR 0209):
// rgb what its mirror ray landed on, a how sure. It has no depth in the
// prepass for the pass above to march from, but its own shader knows where it
// is and which way it faces, and marches the scene itself
// (`sceneReflectionAt`, `engine/surface.hlsli`). Nought unless a surface set
// it: every other shader is lit as it was.
static float4 EngineSurfaceReflection = float4(0.0f, 0.0f, 0.0f, 0.0f);

// **How flat the surface is where it is shaded smooth** (ADR 0209): 1 on a
// floor, a wall, a box's face; 0 on a ball, a pillar, a character. The pass
// that marches the screen rebuilds a normal from depth, and depth is the
// MESH: on a curved thing shaded smooth that is the facet's normal, not the
// one the surface is lit by, and a chrome ball came out of it cut into flat
// patches -- each facet a little mirror facing its own way -- where the
// probe, read through the shading normal, had shown a smooth one. No target
// holds the shading normal for the pass to march with (ADR 0209, section 1),
// so the surface says what it is and keeps the probe.
//
// Told by the vertex normal, as the mesh has it BEFORE any normal map: where
// it turns across the screen the surface is curved and shaded smooth. On a
// flat face it is the same at every pixel, however bumpy its map. As turns
// to a metre, so the answer is the same near and far: a radius of fifty
// metres and more is flat, of twelve and less is not.
static float EngineScreenFlatness = 1.0f;

float screenFlatness(float3 vertexNormal, float3 position)
{
    const float curvature = length(fwidth(vertexNormal)) / max(length(fwidth(position)), 1e-6f);
    return saturate(1.0f - (curvature - 0.02f) / 0.06f);
}

// **What the screen says** (ADR 0209), read at this pixel as the two above
// are. The reflection: where this pixel's mirror ray landed in the last
// frame's lit picture, as the colour there times how sure the landing is, and
// how sure -- with a chain of its own, so a rougher surface reads a level
// further down. The bounce: the light gathered from the screen round this
// pixel, and how much of the hemisphere the screen answered.
//
// Not in every shader that is lit: a stage has sixteen samplers, the ground's
// and a blended surface's are spent, and the ones that are lean are lean on
// purpose. `ENG_NO_SCREEN_LIGHT` leaves both out, and what such a surface
// reflects is the probes' and the sky's, as it was. A surface shader's stage
// has its fifth texture where these two stand for the built-in one, so it
// names its own slots.
#if !defined(ENG_NO_SCREEN_LIGHT)
#if !defined(ENG_SCREEN_REFLECTION_REGISTERS)
#define ENG_SCREEN_REFLECTION_REGISTERS t13, s13
#define ENG_SCREEN_BOUNCE_REGISTERS t14, s14
#endif
#define ENG_SCREEN_TEXTURE(name, t, s)                                                                                \
    Texture2D name##Texture : register(t, space2);                                                                    \
    SamplerState name##Sampler : register(s, space2);
#define ENG_SCREEN_TEXTURE_AT(name, registers) ENG_SCREEN_TEXTURE(name, registers)
ENG_SCREEN_TEXTURE_AT(ScreenReflection, ENG_SCREEN_REFLECTION_REGISTERS)
ENG_SCREEN_TEXTURE_AT(ScreenBounce, ENG_SCREEN_BOUNCE_REGISTERS)

// What a surface this rough reflects of the screen at `screenUv`: rgb the
// colour, a how much of the probe's answer it replaces. **Nought where the
// screen has no answer** -- the caller keeps the probe's, never black.
float4 screenReflectionAt(float2 screenUv, float roughness)
{
    // A mirror reads the picture as it is; rougher, a level further down its
    // chain, which is the blur a rough surface makes of what it reflects.
    // Past the roughness where that is a smear with nothing in it, the probe.
    const float fade = saturate(1.0f - roughness / 0.6f) * ScreenLightParams.x;
    if (fade <= 0.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float4 landed =
        ScreenReflectionTexture.SampleLevel(ScreenReflectionSampler, screenUv, roughness / 0.6f * ScreenLightParams.z);
    // Stored as colour times how sure, so the chain blurs a hit into a miss
    // without darkening it.
    const float sure = saturate(landed.a);
    return float4(sure > 1e-4f ? landed.rgb / landed.a : float3(0.0f, 0.0f, 0.0f), sure * fade);
}

// The light that bounces to this pixel off what is on the screen: rgb the
// light, a the share of the hemisphere the screen answered for -- the share
// of a volume's or the ambient light's it takes the place of.
float4 screenBounceAt(float2 screenUv)
{
    if (ScreenLightParams.y <= 0.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float4 gathered = ScreenBounceTexture.SampleLevel(ScreenBounceSampler, screenUv, 0.0f);
    const float share = saturate(gathered.a);
    return float4(share > 1e-4f ? gathered.rgb / gathered.a : float3(0.0f, 0.0f, 0.0f), share * ScreenLightParams.y);
}
#endif

// An exact texel fetch through a point sampler.
float clusterFetch1(Texture2D<float> texture, SamplerState pointSampler, uint x, uint y, uint width, uint height)
{
    const float2 uv = (float2(float(x), float(y)) + 0.5f) / float2(float(width), float(height));
    return texture.SampleLevel(pointSampler, uv, 0.0f).r;
}

float4 clusterFetch4(Texture2D texture, SamplerState pointSampler, uint x, uint y, uint width, uint height)
{
    const float2 uv = (float2(float(x), float(y)) + 0.5f) / float2(float(width), float(height));
    return texture.SampleLevel(pointSampler, uv, 0.0f);
}

// Every light that reaches this fragment's cluster, and no others. That bound --
// not the frame's total -- is what a fragment pays for, and it is the whole
// point of clustering: the first renderer iterated eight unculled lights on every pixel of every
// draw whether they reached it or not.
// **How far in front of the camera a fragment is**: what the shadow
// cascades and the light clusters are stated in.
//
// The vertex stage hands over the clip position's w, which under a perspective
// projection is exactly that. Under an orthographic one it is 1 for every
// fragment: each chose the first cascade -- a few metres round the camera --
// so a camera looking down from twenty metres saw no shadow of the sun
// anywhere, and every light's cluster was the nearest slice's. There the
// distance is the fragment's place along the camera's forward axis, which the
// frame carries for it; the place is relative to the camera already.
float viewDepthOf(float3 shadingPosition, float interpolated)
{
    const float3 forward = LightCountUnused.yzw;
    return dot(forward, forward) > 0.0f ? dot(shadingPosition, forward) : interpolated;
}

float3 evaluateClusteredLights(Surface surface, float2 pixel, float viewDepth)
{
    if (LightCountUnused.x <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const uint tileX = min((uint)(pixel.x * ViewportParams.z * float(ENG_CLUSTER_TILES_X)),
                           (uint)(ENG_CLUSTER_TILES_X - 1));
    const uint tileY = min((uint)(pixel.y * ViewportParams.w * float(ENG_CLUSTER_TILES_Y)),
                           (uint)(ENG_CLUSTER_TILES_Y - 1));
    // Olsson and Assarsson's exponential slicing, and the logarithm is the whole
    // reason one grid serves a near plane at 0.1 and a far one in the hundreds.
    const float rawSlice = log2(max(viewDepth, 1e-6f)) * ClusterParams.x + ClusterParams.y;
    const uint slice = (uint)clamp(rawSlice, 0.0f, float(ENG_CLUSTER_SLICES - 1));

    const float packed = clusterFetch1(ClusterGridTexture, ClusterGridSampler, slice * ENG_CLUSTER_TILES_X + tileX,
                                       tileY, ENG_CLUSTER_GRID_WIDTH, ENG_CLUSTER_GRID_HEIGHT);
    const uint offset = (uint)(packed / ENG_CLUSTER_OFFSET_SHIFT);
    const uint count = min((uint)(packed - float(offset) * ENG_CLUSTER_OFFSET_SHIFT),
                           (uint)ENG_MAX_LIGHTS_PER_CLUSTER);

    float3 color = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0u; i < count; ++i)
    {
        const uint entry = offset + i;
        const float encoded = clusterFetch1(LightIndexTexture, LightIndexSampler, entry % ENG_LIGHT_INDEX_WIDTH,
                                            entry / ENG_LIGHT_INDEX_WIDTH, ENG_LIGHT_INDEX_WIDTH,
                                            ENG_LIGHT_INDEX_HEIGHT);
        const uint lightIndex = (uint)(encoded + 0.5f);

        // `GpuLight`'s own three rows, rebuilt from three texels. The struct is
        // unchanged; only where its bytes come from is.
        GpuLight light;
        light.PositionRange =
            clusterFetch4(LightDataTexture, LightDataSampler, 0u, lightIndex, 3u, ENG_MAX_CLUSTERED_LIGHTS);
        light.Color =
            clusterFetch4(LightDataTexture, LightDataSampler, 1u, lightIndex, 3u, ENG_MAX_CLUSTERED_LIGHTS);
        light.DirectionCosAngle =
            clusterFetch4(LightDataTexture, LightDataSampler, 2u, lightIndex, 3u, ENG_MAX_CLUSTERED_LIGHTS);

        color += evaluatePunctualLight(surface, light, localShadowFactor(surface, light));
    }
    return color;
}

struct Interpolants
{
    // Camera-relative: `Model` carries the camera-relative translation, which is
    // what makes the eye the origin of this space and the lights' documented
    // camera-relative positions directly comparable.
    float3 ShadingPosition : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
    // `1 - BasePart.Transparency`, carried down because `GpuObjectUniforms` is
    // a vertex-stage block (b0 space1) and the fragment stage cannot see it.
    // Constant across a triangle, so the interpolation is a formality.
    float InstanceAlpha : TEXCOORD4;
    // Distance along the camera's forward axis, in metres, which is what the
    // cascade splits are stated in. Carried rather than derived from
    // `SV_Position`, which would need the projection this stage is not given --
    // and for a perspective matrix it is exactly the clip w.
    float ViewDepth : TEXCOORD5;
#if defined(ENG_INSTANCE_TINT)
    // The instance's own base colour: a run of parts that differ only by
    // colour shares one material, and each brings its colour in the instance
    // stream. `nointerpolation` because it is one value per instance, and an
    // interpolated constant is only nearly constant -- the pixels must be the
    // ones the material's own factor gave.
    nointerpolation float3 InstanceTint : TEXCOORD6;
#endif
    // The UV in metres along the surface: `Uv` times how long the tangent and
    // the bitangent are in the world. On a primitive, whose faces each span
    // one object unit, that is where on the face in metres -- what a material's
    // tile size divides (`EmissiveFactor.w`).
    float2 UvMetres : TEXCOORD7;
    float4 Position : SV_Position;
};

// The tangent frame, guarding the case glTF permits and the importer leaves
// zeroed: a mesh with no tangents at all. `normalize` of a zero vector is a NaN
// that spreads across the whole pixel, and with a flat normal map any frame
// gives the same answer, so an arbitrary perpendicular is the correct fallback.
float3x3 tangentFrame(float3 normal, float4 tangent)
{
    const float tangentLength = length(tangent.xyz);
    const float3 arbitrary =
        abs(normal.y) < 0.999f ? normalize(cross(float3(0.0f, 1.0f, 0.0f), normal)) : float3(1.0f, 0.0f, 0.0f);
    const float3 t = tangentLength > 1e-5f ? tangent.xyz / tangentLength : arbitrary;
    // glTF's own definition: the bitangent is cross(normal, tangent) times the
    // stored handedness sign.
    const float3 b = cross(normal, t) * tangent.w;
    return float3x3(t, b, normal);
}

// **Everything a lit surface receives**: the shadowed sun, the clustered
// lights, the environment and the ambient -- everything `shadeForward` does
// after it has decided what the surface IS, and before emission and fog.
//
// Split out so a shader that builds its surface some other way -- terrain,
// whose normal and colour come from textures the vertex never carried -- is
// lit by the same code rather than by a copy that will drift from it.
// `pixel` is `SV_Position.xy`. `sky` is how much of the sky the point sees, 0
// to 1: it scales the environment and the ambient exactly as the occlusion
// pass does, and for the same reason touches neither the sun (which has a
// shadow map) nor a lamp. Only a surface that knows it is underground -- a
// cave -- passes anything but one.
// **What a measurement leaves out of a lit surface** (ADR 0171, `[debug]
// skip`): a set of bits in `ClusterParams.w`, zero on every frame nobody is
// measuring. Each is a term of the lighting below, taken out by a branch on a
// uniform, so a frame's time with it and without it is what the term costs --
// on the phone, where nothing else can say. `render::MeasureSkip` is the same
// list.
static const uint EngineSkipSun = 1u;
static const uint EngineSkipShadow = 2u;
static const uint EngineSkipContact = 4u;
static const uint EngineSkipLights = 8u;
static const uint EngineSkipEnvironment = 16u;
static const uint EngineSkipAmbient = 32u;
static const uint EngineSkipOcclusion = 64u;
static const uint EngineSkipFog = 128u;
static const uint EngineSkipNormalMap = 256u;
static const uint EngineSkipMaterialMaps = 512u;
static const uint EngineSkipUnlit = 1024u;
// Not a term left out but a rule left off: a receiver under a caster
// compared as every receiver was. `GraphicsSettings::shadowUnderCasters`.
static const uint EngineSkipShadowUnder = 8192u;

// --- What a room reflects, and the light it gives back (ADRs 0202, 0203) ----

struct IndirectLight
{
    // The probes' reflection and diffuse light, each already times its share.
    float3 Specular;
    float3 Diffuse;
    // What is left for the sky: one where no probe reaches.
    float SkyShare;
    // The volumes' diffuse light that takes the place of the sky's and the
    // flat ambient, already times its share; and that share -- one inside a
    // volume, nought past its blend.
    float3 Bounced;
    float BouncedShare;
    // And their light that is ADDED to the sky's and the flat ambient: a
    // volume of what bounces alone. Already times its share too.
    float3 BouncedAdded;
};

float3 intoBox(float4 rows[3], float3 place)
{
    return float3(dot(rows[0].xyz, place) + rows[0].w, dot(rows[1].xyz, place) + rows[1].w,
                  dot(rows[2].xyz, place) + rows[2].w);
}

// One slice of a volume: four coefficients a colour, in three texels side by
// side, a tile of thirty-two texels each.
void readVolumeSlice(float2 cell, float slice, float firstLayer, float texel, out float4 a, out float4 b, out float4 c)
{
    const float layer = firstLayer + floor(slice * 0.25f);
    const float row = slice - floor(slice * 0.25f) * 4.0f;
    const float2 origin = float2(0.0f, row * 32.0f);
    a = EnvironmentMap.SampleLevel(EnvironmentSampler, float3((origin + cell) * texel, layer), 0.0f);
    b = EnvironmentMap.SampleLevel(EnvironmentSampler, float3((origin + cell + float2(32.0f, 0.0f)) * texel, layer),
                                   0.0f);
    c = EnvironmentMap.SampleLevel(EnvironmentSampler, float3((origin + cell + float2(64.0f, 0.0f)) * texel, layer),
                                   0.0f);
}

// A volume's light on a surface facing `normal`, at a place in its box:
// already over pi, as the sky's irradiance is.
float3 readVolume(uint row, float3 local, float3 normal)
{
    const float3 halfSize = VolumeRows[row + 3u].xyz;
    const float3 counts = VolumeRows[row + 4u].xyz;
    // **A volume with no points** (ADR 0207): one colour for the whole of its
    // box -- the mean of a light worked out somewhere else and kept, where
    // this machine works none out. Nothing is fetched for it.
    if (counts.x <= 0.0f)
        return VolumeRows[row + 4u].yzw * (VolumeRows[row + 5u].x * VolumeRows[row + 6u].rgb);
    // Samples stand at the middle of their cells, so the half cell at each
    // face reads the nearest one.
    const float3 grid = clamp((local + halfSize) / (2.0f * halfSize) * counts - 0.5f, 0.0f, counts - 1.0f);
    const float slice = floor(grid.z);
    const float next = min(slice + 1.0f, counts.z - 1.0f);
    const float2 cell = grid.xy + 0.5f;
    float4 a0, b0, c0, a1, b1, c1;
    readVolumeSlice(cell, slice, VolumeRows[row + 4u].w, VolumeRows[row + 5u].y, a0, b0, c0);
    readVolumeSlice(cell, next, VolumeRows[row + 4u].w, VolumeRows[row + 5u].y, a1, b1, c1);
    const float between = grid.z - slice;
    const float4 a = lerp(a0, a1, between);
    const float4 b = lerp(b0, b1, between);
    const float4 c = lerp(c0, c1, between);
    // The constant, then one coefficient an axis.
    const float3 light = a.rgb + float3(a.a, b.r, b.g) * normal.x + float3(b.b, b.a, c.r) * normal.y +
                         float3(c.g, c.b, c.a) * normal.z;
    return max(light, 0.0f) * (VolumeRows[row + 5u].x * VolumeRows[row + 6u].rgb);
}

// **A probe's first level, read smooth**. A probe is one map a hundred
// and twenty-eight texels a side for the whole sphere: a texel is a degree
// and a half. A nearly mirror floor reads it magnified twenty times, and
// between four texels a plain read is a ramp along each axis with a crease
// where two ramps meet -- so an edge that runs across the map's grid came out
// as the grid itself, a fringe of feathers under every bright thing the floor
// reflected. Read through a cubic instead (the four-read form: each read is
// the engine's own blend of two texels by two), the same texels have no
// crease. Four reads where there was one, only on a surface smooth enough to
// ask for the first level at all.
//
// It does not make the picture sharper than it is: a true mirror is the
// screen's reflections, where they are on.
float3 probeFirstLevel(float2 uv, float layer)
{
    uint width = 0;
    uint height = 0;
    uint layers = 0;
    uint levels = 0;
    EnvironmentMap.GetDimensions(0u, width, height, layers, levels);
    const float2 size = float2(float(max(width, 1u)), float(max(height, 1u)));
    const float2 at = uv * size - 0.5f;
    const float2 whole = floor(at);
    const float2 f = at - whole;
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    const float2 w0 = (1.0f - 3.0f * f + 3.0f * f2 - f3) / 6.0f;
    const float2 w1 = (4.0f - 6.0f * f2 + 3.0f * f3) / 6.0f;
    const float2 w2 = (1.0f + 3.0f * f + 3.0f * f2 - 3.0f * f3) / 6.0f;
    const float2 w3 = f3 / 6.0f;
    const float2 near = w0 + w1;
    const float2 far = w2 + w3;
    const float2 nearAt = (whole - 0.5f + w1 / near) / size;
    const float2 farAt = (whole + 1.5f + w3 / far) / size;
    const float3 a = EnvironmentMap.SampleLevel(EnvironmentSampler, float3(nearAt.x, nearAt.y, layer), 0.0f).rgb;
    const float3 b = EnvironmentMap.SampleLevel(EnvironmentSampler, float3(farAt.x, nearAt.y, layer), 0.0f).rgb;
    const float3 c = EnvironmentMap.SampleLevel(EnvironmentSampler, float3(nearAt.x, farAt.y, layer), 0.0f).rgb;
    const float3 d = EnvironmentMap.SampleLevel(EnvironmentSampler, float3(farAt.x, farAt.y, layer), 0.0f).rgb;
    return near.y * (near.x * a + far.x * b) + far.y * (near.x * c + far.x * d);
}

IndirectLight gatherIndirect(Surface surface, float3 position, float3 normal)
{
    IndirectLight result;
    result.Specular = float3(0.0f, 0.0f, 0.0f);
    result.Diffuse = float3(0.0f, 0.0f, 0.0f);
    result.SkyShare = 1.0f;
    result.Bounced = float3(0.0f, 0.0f, 0.0f);
    result.BouncedShare = 0.0f;
    result.BouncedAdded = float3(0.0f, 0.0f, 0.0f);

    // **Probes.** A surface reads the probe that the point a step in front of
    // it is inside -- the same rule a volume is read by, and for the same
    // wall: a room's own floor and walls lie ON its box, and the dark side of
    // a thin wall must not reflect the bright room. Highest priority first,
    // each taking what it holds of what is left, two at most a pixel. Past a
    // probe's faces its say among probes is gone within five centimetres;
    // against the sky it fades over its own blend.
    const float roughness = sqrt(surface.Alpha);
    const float3 reflection = reflect(-surface.View, surface.Normal);
    const float level = roughness * (EnvironmentParams.x - 1.0f);
    const float3 ahead = position + normal * IndirectParams.z;
    // How far past its faces a probe or a volume still has a say among its
    // own kind: a fifth of the step, five centimetres. Two boxes that meet
    // inside a wall then blend inside the wall, where nothing is drawn; were
    // it the whole step, the floor at the foot of a thin wall would be lit a
    // hand's width into the dark room by the bright one.
    const float seam = IndirectParams.z * 0.2f;
    const uint probes = uint(IndirectParams.x);
    uint used = 0u;
    float left = 1.0f;
    float taken = 0.0f;
    float held = 0.0f;
    [loop] for (uint probe = 0u; probe < probes && used < 2u && left > 0.004f; ++probe)
    {
        const uint row = probe * 6u;
        const float4 rows[3] = {ProbeRows[row], ProbeRows[row + 1u], ProbeRows[row + 2u]};
        const float3 halfSize = ProbeRows[row + 3u].xyz;
        const float3 beyond = abs(intoBox(rows, ahead)) - halfSize;
        const float outside = max(max(beyond.x, max(beyond.y, beyond.z)), 0.0f);
        const float fade = saturate(1.0f - outside * ProbeRows[row + 3u].w) * ProbeRows[row + 5u].z;
        const float say = min(saturate(1.0f - outside / seam) + fade * 0.001f, 1.0f);
        if (say <= 0.0f)
        {
            continue;
        }
        const float share = say * left;
        float3 direction = reflection;
        if (ProbeRows[row + 5u].y > 0.5f)
        {
            // Where the reflected ray leaves the box, seen from where the
            // pictures were taken: a floor then reflects the wall where the
            // wall is, and not where it would be were the room endless.
            const float3 local = clamp(intoBox(rows, position), -halfSize, halfSize);
            float3 along = float3(dot(rows[0].xyz, reflection), dot(rows[1].xyz, reflection),
                                  dot(rows[2].xyz, reflection));
            along = float3(abs(along.x) < 1e-5f ? 1e-5f : along.x, abs(along.y) < 1e-5f ? 1e-5f : along.y,
                           abs(along.z) < 1e-5f ? 1e-5f : along.z);
            const float3 reach = (halfSize * sign(along) - local) / along;
            const float3 seen = local + along * min(reach.x, min(reach.y, reach.z)) - ProbeRows[row + 4u].xyz;
            direction = seen.x * rows[0].xyz + seen.y * rows[1].xyz + seen.z * rows[2].xyz;
        }
        const float layer = ProbeRows[row + 4u].w;
        const float amount = share * ProbeRows[row + 5u].x;
        const float2 probeUv = octahedralUv(direction);
        float3 seenThere;
        [branch]
        if (level < 1.0f)
        {
            // Between the first level and the second, as the chain is read.
            seenThere = lerp(probeFirstLevel(probeUv, layer),
                             EnvironmentMap.SampleLevel(EnvironmentSampler, float3(probeUv, layer), 1.0f).rgb, level);
        }
        else
        {
            seenThere = EnvironmentMap.SampleLevel(EnvironmentSampler, float3(probeUv, layer), level).rgb;
        }
        result.Specular += seenThere * amount;
        result.Diffuse += EnvironmentMap
                              .SampleLevel(EnvironmentSampler, float3(octahedralUv(surface.Normal), layer + 1.0f),
                                           IndirectParams.w)
                              .rgb *
                          amount;
        left -= share;
        taken += share;
        held = max(held, fade);
        ++used;
    }
    if (taken > 0.0f)
    {
        // Between them the probes take what the nearest of them holds of the
        // surface, and the sky the rest.
        const float scale = held / taken;
        result.Specular *= scale;
        result.Diffuse *= scale;
        result.SkyShare = 1.0f - held;
    }

    // **Volumes.** A surface reads the volume that the point a step in front
    // of it is inside: the dark side of a wall steps into the dark room,
    // however thin the wall. Past a volume's faces its say among volumes is
    // gone within five centimetres -- so two only mix where both nearly
    // hold the point, which is the plane of a doorway -- while against the
    // flat ambient it fades over the volume's own blend.
    const uint volumes = uint(IndirectParams.y);
    if (volumes > 0u)
    {
        float3 light = float3(0.0f, 0.0f, 0.0f);
        float total = 0.0f;
        float covered = 0.0f;
        float added = 0.0f;
        // **The levels round the camera** (ADR 0206), finest first: each
        // takes what the finer ones left, by how far in from its faces the
        // point is -- so a level gives way to the wider one over its last
        // cells, and the widest to no volume at all.
        float3 levelLight = float3(0.0f, 0.0f, 0.0f);
        float levelTaken = 0.0f;
        float levelAdded = 0.0f;
        [loop] for (uint volume = 0u; volume < volumes; ++volume)
        {
            const uint row = volume * 7u;
            const float4 rows[3] = {VolumeRows[row], VolumeRows[row + 1u], VolumeRows[row + 2u]};
            const float3 local = intoBox(rows, ahead);
            const float3 beyond = abs(local) - VolumeRows[row + 3u].xyz;
            const float nearest = max(beyond.x, max(beyond.y, beyond.z));
            const float outside = max(nearest, 0.0f);
            const float margin = VolumeRows[row + 5u].w;
            [branch]
            if (margin < 0.0f)
            {
                const float weight = saturate(nearest / margin) * (1.0f - levelTaken) * VolumeRows[row + 5u].z;
                [branch]
                if (weight > 0.0f)
                {
                    levelLight += readVolume(row, local, surface.Normal) * weight;
                    levelTaken += weight;
                    levelAdded += weight * VolumeRows[row + 6u].w;
                }
                continue;
            }
            const float fade = saturate(1.0f - outside * VolumeRows[row + 3u].w) * VolumeRows[row + 5u].z;
            float say = saturate(1.0f - outside / seam) + fade * 0.001f;
            // **A tile gives way to the tile beside it** across the cells the
            // two share: by how far in from its faces the point is. Alone, as
            // at the room's own walls, it is the whole of the sum whatever
            // its weight.
            if (margin > 0.0f && nearest < 0.0f)
            {
                say *= max(saturate(-nearest / margin), 0.001f);
            }
            if (say <= 0.0f)
            {
                continue;
            }
            light += readVolume(row, local, surface.Normal) * say;
            total += say;
            added += say * VolumeRows[row + 6u].w;
            covered = max(covered, fade);
        }
        if (total > 0.0f)
        {
            // Of what the volumes cover, the share of those whose light is
            // added stays the sky's and the ambient's as well.
            const float kept = added / total;
            const float3 room = light / total;
            result.BouncedShare = covered * (1.0f - kept);
            result.Bounced = room * result.BouncedShare;
            result.BouncedAdded = room * (covered * kept);
        }
        if (levelTaken > 0.0f)
        {
            // A room's own volume has the room: the levels light what no
            // room covers.
            const float left = 1.0f - covered;
            const float kept = levelAdded / levelTaken;
            result.Bounced += levelLight * (left * (1.0f - kept));
            result.BouncedShare += levelTaken * left * (1.0f - kept);
            result.BouncedAdded += levelLight * (left * kept);
        }
    }
    return result;
}

// **How much of the light from around reaches a surface past the small things
// near it**: a cube resting on a table shades the table round its base, and
// a ball held over it the spot under it. Each occluder is a sphere; what it
// hides of the sky over a point is its form factor seen from there, the
// square of its radius over its distance times how squarely the surface
// faces it, faded out between two and four radii. A point on the occluder's
// own surface faces away from its centre and is not darkened by it. In a
// headset's eye, where the screen's own occlusion is not drawn.
float nearbyOcclusion(float3 position, float3 normal)
{
    float open = 1.0f;
    const uint count = min(uint(OccluderParams.x), 8u);
    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const float4 occluder = Occluders[i];
        const float3 toward = occluder.xyz - position;
        const float distanceSq = max(dot(toward, toward), 1e-6f);
        const float radiusSq = occluder.w * occluder.w;
        [branch]
        if (distanceSq < 16.0f * radiusSq)
        {
            const float distance = sqrt(distanceSq);
            const float facing = saturate(dot(normal, toward) / distance);
            const float fade = saturate(2.0f - distance / (2.0f * occluder.w));
            open *= 1.0f - saturate(radiusSq / distanceSq) * facing * fade * OccluderParams.y;
        }
    }
    return open;
}

// **And the sun, where a surface touches one** (a headset's, with
// `nearbyOcclusion`'s spheres): a receiver within a centimetre of a sphere
// whose ray to the sun passes through it is in its shadow. The shadow map
// loses that last centimetre under a ball -- a headset's receiver is lifted
// a texel along its normal (`EngineHeadsetNormalOffsetTexels`), and within
// the square root of twice the radius times that lift of where a ball
// touches the table, the lifted point is inside the ball and compared as lit.
// Each sphere is inside what it stands for, so this shades only what the
// thing itself shades.
float nearbySunShadow(float3 position, float3 toSun)
{
    float lit = 1.0f;
    const uint count = min(uint(OccluderParams.x), 8u);
    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const float4 occluder = Occluders[i];
        const float3 toward = occluder.xyz - position;
        const float distanceSq = dot(toward, toward);
        const float reach = occluder.w + 0.01f;
        [branch]
        if (distanceSq < reach * reach)
        {
            const float along = dot(toward, toSun);
            const float missSq = max(distanceSq - along * along, 0.0f);
            const float through = along > 0.0f ? 1.0f - smoothstep(0.9f * occluder.w, occluder.w, sqrt(missSq)) : 0.0f;
            const float near = saturate((reach - sqrt(distanceSq)) / 0.005f);
            lit = min(lit, 1.0f - through * near);
        }
    }
    return lit;
}

float3 lightSurface(Surface surface, float3 shadingPosition, float3 normal, float viewDepth, float2 pixel, float sky)
{
    const uint skipped = uint(ClusterParams.w);
    // `unlit`: what the surface is, and nothing that lights it.
    [branch]
    if ((skipped & EngineSkipUnlit) != 0u)
    {
        return surface.DiffuseColor;
    }
    viewDepth = viewDepthOf(shadingPosition, viewDepth);
    const float3 sunDirection = normalize(SunDirectionBrightness.xyz);
    float3 color = float3(0.0f, 0.0f, 0.0f);
    // **A surface the sun does not light asks nothing of the shadow map**.
    // `shadeDirect` answers black for a surface turned from the
    // light, and a sun of no brightness lights nothing, so what the lookups
    // would have said was multiplied by zero: half the faces of a scene, and
    // every face of a night. Skipped, the picture is the same.
    if ((skipped & EngineSkipSun) == 0u && SunDirectionBrightness.w > 0.0f && dot(surface.Normal, sunDirection) > 0.0f)
    {
        const float sunNol = saturate(dot(normal, sunDirection));
        float shadow = 1.0f;
        [branch]
        if ((skipped & EngineSkipShadow) == 0u)
        {
            EngineShadowUnderCasters = (skipped & EngineSkipShadowUnder) == 0u;
            shadow = sampleSunShadow(ShadowMap, ShadowSampler, shadingPosition, normal, sunNol, viewDepth, pixel);
        }
        // The darker of the shadow map and the contact mask: the map knows what
        // is off screen and loses the last few centimetres to its biases; the
        // mask has those centimetres and knows nothing off screen.
        float contact = 1.0f;
        [branch]
        if ((skipped & EngineSkipContact) == 0u)
        {
            contact = ContactShadowTexture.SampleLevel(ContactShadowSampler, pixel * ViewportParams.zw, 0.0f);
        }
        const float sunShadow = min(min(shadow, contact), nearbySunShadow(shadingPosition, sunDirection));
        const float3 sunRadiance = SunColorUnused.rgb * (SunDirectionBrightness.w * sunShadow);
        color = shadeDirect(surface, sunDirection, sunRadiance);
    }

    [branch]
    if ((skipped & EngineSkipLights) == 0u)
    {
        color += evaluateClusteredLights(surface, pixel, viewDepth);
    }

    // The environment, on both lobes, and this is what the lighting work exists for: until
    // now `Lighting.Ambient` was applied flat to both, which `pbr.hlsl`'s own
    // comment called "the degenerate case of the split-sum approximation where
    // the environment is one colour". It is no longer one colour -- it is the
    // sky, prefiltered by roughness, so a metal reflects the hour the script
    // set instead of reflecting nothing.
    //
    // The ambient is ADDED to the irradiance rather than replaced by it: a
    // stand-in for bounced light there is still none of.
    // The occlusion term multiplies the environment and the ambient below, and
    // NOTHING else. A surface's occlusion of the environment says nothing about
    // whether the sun reaches it, and the sun has a shadow map that answers
    // exactly that; applying it to direct light is the most common way an
    // ambient-occlusion pass ends up looking like dirt.
    const float2 screenUv = pixel * ViewportParams.zw;
    float rawOcclusion = 1.0f;
    [branch]
    if ((skipped & EngineSkipOcclusion) == 0u)
    {
        rawOcclusion = OcclusionTexture.SampleLevel(OcclusionSampler, screenUv, 0.0f);
    }
    float screenOcclusion = lerp(1.0f, rawOcclusion, EnvironmentParams.z);
    [branch]
    if (OccluderParams.x > 0.5f)
    {
        screenOcclusion *= nearbyOcclusion(shadingPosition, normal);
    }
    const float occlusion = screenOcclusion * sky;

    // **A frame with a probe or a volume in it** (ADRs 0202, 0203): what
    // they cover of this surface is theirs, and the sky and the flat ambient
    // keep the rest. A frame with neither takes the branch below it, which is
    // the arithmetic it always was.
    float ambientShare = 1.0f;
    // **And a frame that reads the screen** (ADR 0209) takes this branch
    // too: with no probe and no volume in it the gather below answers that
    // all of the surface is the sky's, and the screen's share is taken out of
    // that. A frame with none of the three is the arithmetic it always was.
#if !defined(ENG_NO_SCREEN_LIGHT)
    const float screenLight = ScreenLightParams.x + ScreenLightParams.y;
#else
    const float screenLight = 0.0f;
#endif
    [branch]
    if (IndirectParams.x + IndirectParams.y + screenLight + EngineSurfaceReflection.a > 0.0f)
    {
        const IndirectLight indirect = gatherIndirect(surface, shadingPosition, normal);
        ambientShare = indirect.SkyShare * (1.0f - indirect.BouncedShare);
        [branch]
        if ((skipped & EngineSkipEnvironment) == 0u)
        {
            const float roughness = sqrt(surface.Alpha);
            const float2 ab = BrdfLut.SampleLevel(BrdfSampler, float2(surface.NoV, roughness), 0.0f).rg;
            const float3 fresnel = surface.SpecularF0 * ab.x + ab.yyy;
            const float3 reflection = reflect(-surface.View, surface.Normal);
            const float3 skyReflected =
                EnvironmentMap
                    .SampleLevel(EnvironmentSampler, float3(octahedralUv(reflection), 0.0f),
                                 roughness * (EnvironmentParams.x - 1.0f))
                    .rgb;
            // The sky's share is scaled by how much sky the surface sees, as
            // it was; a room's own light is not the sky's to scale.
            const float3 skyDiffuse = evaluateIrradiance(IrradianceSh, surface.Normal) * (sky * indirect.SkyShare);
            float3 diffuse = (skyDiffuse + indirect.Diffuse) * (1.0f - indirect.BouncedShare) +
                             indirect.Bounced + indirect.BouncedAdded;
#if !defined(ENG_NO_SCREEN_LIGHT)
            // What bounces off the screen takes the place of that share of
            // everything above and of the flat ambient below: the light from
            // those directions is the screen's, and is counted once.
            //
            // **And it is not darkened again by the screen's occlusion.** The
            // directions that occlusion counts as shut are the ones a ray
            // landed in, and what it landed on is already as dark as it is:
            // times the occlusion below, a corner was shut twice. Divided
            // here by what it is multiplied by there -- held off nought, so
            // a crease cannot be made a lamp -- and with no share of the
            // screen's this line leaves `diffuse` exactly as it found it.
            const float4 bounced = screenBounceAt(screenUv);
            diffuse = lerp(diffuse, bounced.rgb / max(screenOcclusion, 0.25f), bounced.a);
            ambientShare *= 1.0f - bounced.a;
#endif
            float3 specular = skyReflected * (sky * indirect.SkyShare) + indirect.Specular;
            // What a blended surface found of the scene behind it, first.
            specular = lerp(specular, EngineSurfaceReflection.rgb, EngineSurfaceReflection.a);
#if !defined(ENG_NO_SCREEN_LIGHT)
            // The screen first, the probe and the sky where the screen has no
            // answer: by how sure the landing was, and never more than that.
            const float4 onScreen = screenReflectionAt(screenUv, roughness);
            specular = lerp(specular, onScreen.rgb, onScreen.a * EngineScreenFlatness);
#endif
            color += (surface.DiffuseColor * diffuse + specular * fresnel) * (EnvironmentParams.y * screenOcclusion);
        }
    }
    else
    {
        // The sky's own light -- its irradiance and its reflection -- reaches
        // only as much of the surface as sees the sky.
        [branch]
        if ((skipped & EngineSkipEnvironment) == 0u)
        {
            color += evaluateEnvironment(surface, EnvironmentMap, EnvironmentSampler, BrdfLut, BrdfSampler,
                                         IrradianceSh, EnvironmentParams.x, EnvironmentParams.y, occlusion);
        }
    }
    // And `Ambient` on the DIFFUSE lobe only, which is a change of side rather
    // than a change of mind. The first renderer's comment argued for putting it on both, and the
    // argument was right at the time: "a mirror in a uniformly lit white room is
    // white, not black", so a specular lobe with no environment behind it had to
    // get something or every metal rendered black. There is an environment
    // behind it now, and it answers that case properly -- adding a flat term on
    // top of a prefiltered one is counting the same light twice, and it shows up
    // as metal that cannot be made dark.
    //
    // **Which ambient is a question of where the surface is** (ADR 0084):
    // `OutdoorAmbient` for one that sees the open sky, `Ambient` for one that
    // sees none of it -- a cave, a tunnel, under an overhang -- and a blend by
    // how much sky it sees between. It used to be `Ambient` scaled BY the sky
    // term, which zeroed it in exactly the places a stand-in for bounced light
    // exists for, and a cave came out black wherever a lamp did not reach (the
    // terrain report). The screen-space occlusion still darkens it.
    if ((skipped & EngineSkipAmbient) == 0u)
    {
        const float3 ambient = lerp(Ambient.rgb, OutdoorAmbient.rgb, sky);
        // A stand-in for bounced light, and so only where nothing has worked
        // the bounced light out: a probe's share and a volume's are not its.
        color += ambient * surface.DiffuseColor * (screenOcclusion * ambientShare);
    }
    return color;
}

float3 lightSurface(Surface surface, float3 shadingPosition, float3 normal, float viewDepth, float2 pixel)
{
    return lightSurface(surface, shadingPosition, normal, viewDepth, pixel, 1.0f);
}

#if defined(ENG_UNIFORMS_MATERIAL) && !defined(ENG_SURFACE_WRAPPER)
// Linear HDR into an `Rgba16Float` target. Nothing here tonemaps and nothing
// here encodes sRGB -- `tonemap.hlsl` does both, once, on the way out.
float4 shadeForward(Interpolants input)
{
    const float metallicFactor = MetallicRoughnessNormalCutoff.x;
    const float roughnessFactor = MetallicRoughnessNormalCutoff.y;
    const float normalScale = MetallicRoughnessNormalCutoff.z;
    const float alphaCutoff = MetallicRoughnessNormalCutoff.w;

    // **Textures by size on a primitive's faces** (a material's `TileSize`, in
    // `EmissiveFactor.w`): the same metre of texture on a 40 m face and on a
    // 1 m edge. Zero keeps the mesh's own UVs.
    const float2 uv = EmissiveFactor.w > 0.0f ? input.UvMetres / EmissiveFactor.w : input.Uv;

    float4 baseColor = BaseColorFactor;
#if defined(ENG_INSTANCE_TINT)
    baseColor.rgb = input.InstanceTint;
#endif
    const float4 sampledBase = BaseColorTexture.Sample(BaseColorSampler, uv);
    baseColor *= lerp(float4(1.0f, 1.0f, 1.0f, 1.0f), sampledBase, TextureFlags.x);
    // The two sources of transparency multiply: a glTF material can be
    // see-through on its own, and a script can make an otherwise opaque mesh
    // see-through with `BasePart.Transparency`. Honouring only one leaves a case
    // that renders wrong, and the alpha is what `extract` sorted the draw by, so
    // the two have to agree on this product.
    baseColor.a *= input.InstanceAlpha;

    // The cutoff doubles as the alpha mode: the renderer writes 0 for Opaque and
    // Blend materials and the real cutoff for Mask ones, because the frozen
    // contract has no field for `AlphaMode`. A cutoff of 0 can never discard,
    // since alpha is never negative, so one `clip` serves all three modes.
    clip(baseColor.a - alphaCutoff);

    // glTF packs occlusion, roughness and metalness into R, G and B of one
    // image. Occlusion is deliberately not read: the spec leaves R undefined
    // unless the same texture is also referenced as the occlusion map, and
    // `MaterialDef` has no occlusion strength to gate it with, so reading it
    // would darken correct materials at random.
    // `[debug] skip` (ADR 0171): the maps a measurement leaves unread.
    const uint skipped = uint(ClusterParams.w);
    float3 sampledMetallicRoughness = float3(1.0f, 1.0f, 1.0f);
    [branch]
    if ((skipped & EngineSkipMaterialMaps) == 0u)
    {
        sampledMetallicRoughness = MetallicRoughnessTexture.Sample(MetallicRoughnessSampler, uv).rgb;
    }
    const float roughness = roughnessFactor * lerp(1.0f, sampledMetallicRoughness.g, TextureFlags.z);
    const float metallic = metallicFactor * lerp(1.0f, sampledMetallicRoughness.b, TextureFlags.z);

    const float3 geometricNormal = normalize(input.Normal);
    float3 normal = geometricNormal;
    [branch]
    if ((skipped & EngineSkipNormalMap) == 0u)
    {
        const float3x3 frame = tangentFrame(geometricNormal, input.Tangent);
        float3 tangentNormal = NormalTexture.Sample(NormalSampler, uv).xyz * 2.0f - 1.0f;
        tangentNormal.xy *= normalScale;
        const float3 mappedNormal = normalize(mul(normalize(tangentNormal), frame));
        normal = normalize(lerp(geometricNormal, mappedNormal, TextureFlags.y));
    }

    Surface surface = makeSurface(input.ShadingPosition, normal, baseColor.rgb, metallic, roughness);
    // Widened by the normal variation this pixel covers, before anything reads
    // it -- the sun, the punctual lights and the environment all shade through
    // `Alpha`, and a highlight that is stable for one of them and not the others
    // would be worse than no correction at all.
    surface.Alpha = antiAliasedAlpha(surface.Alpha, geometricNormal);
    EngineScreenFlatness = screenFlatness(geometricNormal, input.ShadingPosition);
    EngineGeometricNormal = geometricNormal;

    // The sun. Its direction already points from the world towards it, so the
    // dot against a normal needs no negation. Its COLOUR is new with the lighting work: the
    // light it casts warms as it approaches the horizon, derived from its
    // elevation rather than authored (environment.h).
    float3 color = lightSurface(surface, input.ShadingPosition, normal, input.ViewDepth, input.Position.xy);

    float3 emissive = EmissiveFactor.rgb;
    [branch]
    if ((skipped & EngineSkipMaterialMaps) == 0u)
    {
        emissive *= lerp(float3(1.0f, 1.0f, 1.0f), EmissiveTexture.Sample(EmissiveSampler, uv).rgb, TextureFlags.w);
    }
    color += emissive;

    // The eye is this space's origin, so the distance to it is the length of the
    // shading position. Fog is applied to emissive too: something glowing behind
    // fog is still behind fog.
    if ((skipped & EngineSkipFog) == 0u)
    {
        color = applyFog(color, FogColor.rgb, FogRange, length(input.ShadingPosition));
    }

    return float4(color, baseColor.a);
}

#endif // ENG_UNIFORMS_MATERIAL

#endif // ENG_FORWARD_HLSLI
