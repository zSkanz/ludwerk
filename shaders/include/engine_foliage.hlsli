// Foliage (ADR 0116): what the cull writes and how a vertex stage places it.
//
// An instance the cull kept, 48 bytes, camera-relative -- written by
// `foliage_cull.hlsl` and read here by `SV_InstanceID` plus the bucket's base,
// never by `firstInstance`, which D3D12's `SV_InstanceID` does not include.
#ifndef ENG_FOLIAGE_HLSLI
#define ENG_FOLIAGE_HLSLI

#include "engine/wind.hlsli"

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

StructuredBuffer<FoliageVisible> FoliageList : register(t0, space0);

// `render::GpuFoliageUniforms`, 64 bytes, the vertex stage's second block.
cbuffer GpuFoliageUniforms : register(b1, space1)
{
    // xyz the workspace's `GlobalWind`, w the simulation's seconds.
    float4 FoliageWind;
    // x gusts, y turbulence, z the mesh's `WindResponse / Stiffness`, w the
    // bucket's first slot in the list, as a float of an integer.
    float4 FoliageWindParams;
    // x the mesh's lowest y, y its height, z the shadow distance (depth pass
    // only), w unused.
    float4 FoliageMesh;
    // xyz where the camera is in the world, for the wind's own position.
    float4 FoliageCameraOrigin;
};

FoliageVisible foliageInstance(uint instance)
{
    return FoliageList[(uint)FoliageWindParams.w + instance];
}

// The instance's basis: `Up` as the y axis, turned by `Yaw` about it.
float3x3 foliageBasis(FoliageVisible visible)
{
    const float3 up = normalize(visible.Up);
    const float3 reference = abs(up.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 across = normalize(cross(up, reference));
    const float3 along = cross(across, up);
    const float c = cos(visible.Yaw);
    const float s = sin(visible.Yaw);
    const float3 x = across * c + along * s;
    const float3 z = along * c - across * s;
    // Columns x, up, z.
    return float3x3(x.x, up.x, z.x, x.y, up.y, z.y, x.z, up.z, z.z);
}

// **Where a vertex of a foliage mesh goes**, camera-relative: scaled, turned,
// leaned onto the ground, and bent by the wind from its base -- by how high it
// is up the mesh, squared, so a stem's foot stays put and its tip moves most,
// with a phase per instance so a field does not sway as one sheet.
float3 foliagePosition(FoliageVisible visible, float3 local, float3x3 basis)
{
    float3 placed = mul(basis, local * visible.Scale);
    const float height = saturate((local.y - FoliageMesh.x) / max(FoliageMesh.y, 1.0e-3f));
    const float3 world = visible.Position + FoliageCameraOrigin.xyz;
    const float3 wind =
        engineWindAt(FoliageWind.xyz, FoliageWindParams.x, FoliageWindParams.y, world, FoliageWind.w);
    const float phase = visible.Random * 6.2831853f;
    const float flutter = 0.85f + 0.15f * sin(FoliageWind.w * 2.3f + phase);
    // A metre a second of wind bends a unit-high stem by about a centimetre; the
    // response and the stiffness scale it, and a bend never reaches the height.
    const float3 bend = wind * (0.012f * FoliageWindParams.z * flutter) * (height * height) * visible.Scale;
    const float bendLength = length(bend);
    const float limit = 0.6f * FoliageMesh.y * visible.Scale;
    placed += bendLength > limit ? bend * (limit / bendLength) : bend;
    return visible.Position + placed;
}

#endif // ENG_FOLIAGE_HLSLI
