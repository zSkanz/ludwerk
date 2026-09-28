// The foliage cull (ADR 0116): one run of a tile's instances of one mesh,
// kept or dropped by the quality's density, the draw distance and the camera's
// frustum, and appended to its mesh's list of visible instances.
//
// **Thinned by `random`**, which the placement drew once per instance: a lower
// density keeps exactly the instances a higher one kept whose value is below
// it, so turning quality down thins the field rather than reshuffling it.

struct FoliageInstance
{
    float3 Position;
    float Yaw;
    float3 Normal;
    float Random;
};

struct FoliageVisible
{
    float3 Position;
    float Yaw;
    float3 Up;
    float Scale;
    float Random;
    float Fade;
    float2 Unused;
};

StructuredBuffer<FoliageInstance> Instances : register(t0, space0);
RWStructuredBuffer<FoliageVisible> Visible : register(u0, space1);
RWStructuredBuffer<uint> Counters : register(u1, space1);

// The most levels of detail a foliage mesh is drawn at.
#define FOLIAGE_MAX_LODS 4u

// `render::GpuFoliageCull`, 192 bytes.
cbuffer GpuFoliageCull : register(b0, space2)
{
    // The camera's frustum, camera-relative: inside where dot(n, p) + d >= 0.
    float4 Planes[6];
    // xyz the tile's origin relative to the camera, w the mesh's radius.
    float4 OriginRadius;
    uint First;
    uint Count;
    uint Bucket;
    uint BucketBase;
    uint Capacity;
    float Density;
    float DrawDistance;
    float FadeStart;
    float ScaleMin;
    float ScaleMax;
    float Sink;
    float Align;
    float RandomRotation;
    uint LodCount;
    float2 Unused;
    // Past `LodDistances[i]` metres (for a unit-scale instance), level i + 1:
    // where the coarser level's error stops showing as a pixel.
    float4 LodDistances;
};

[numthreads(64, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Count)
        return;
    const FoliageInstance instance = Instances[First + id.x];
    if (instance.Random >= Density)
        return;
    const float3 position = OriginRadius.xyz + instance.Position;
    const float distance = length(position);
    if (distance > DrawDistance)
        return;
    // The scale from a value decorrelated from the thinning rank, so a lower
    // density does not also keep only the small ones.
    const float scale = lerp(ScaleMin, ScaleMax, frac(instance.Random * 7919.137f));
    const float radius = OriginRadius.w * scale;
    [unroll]
    for (uint plane = 0; plane < 6; ++plane) {
        if (dot(Planes[plane].xyz, position) + Planes[plane].w < -radius)
            return;
    }

    // The level: the coarsest whose error, at this distance and this size, is
    // still under a pixel. A bigger instance keeps its finer level further out.
    uint lod = 0;
    [unroll]
    for (uint level = 0; level + 1 < FOLIAGE_MAX_LODS; ++level) {
        if (level + 1 < LodCount && distance > LodDistances[level] * scale)
            lod = level + 1;
    }

    uint slot;
    InterlockedAdd(Counters[Bucket * FOLIAGE_MAX_LODS + lod], 1u, slot);
    if (slot >= Capacity)
        return;

    FoliageVisible visible;
    const float3 up = normalize(lerp(float3(0.0f, 1.0f, 0.0f), instance.Normal, Align));
    visible.Position = position - up * Sink;
    visible.Yaw = RandomRotation > 0.5f ? instance.Yaw : 0.0f;
    visible.Up = up;
    visible.Scale = scale;
    visible.Random = instance.Random;
    visible.Fade = DrawDistance > FadeStart ? saturate((DrawDistance - distance) / (DrawDistance - FadeStart)) : 1.0f;
    visible.Unused = float2(0.0f, 0.0f);
    // Each level has a list of its own, `Capacity` long, after the bucket's base.
    Visible[BucketBase + lod * Capacity + slot] = visible;
}
