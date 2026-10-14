// One roughness level of a reflection probe's map, or its diffuse light (ADR
// 0202): what `render::bakeEnvironmentLevel` and `bakeIrradianceSh` do for the
// sky on the processor, done here for a room, because a room's pictures are on
// the GPU and reading them back would stop the frame.
//
// A level is the map seen through a GGX lobe as wide as that level's
// roughness, under the same assumption the sky's is made with -- the view is
// the normal is the reflection -- so a probe's level means what the sky's
// level of the same number means. The diffuse light is the map through a
// cosine lobe, divided by pi, so a surface multiplies it straight by its
// colour.
//
// Each sample reads the source at the level whose texel is as wide as the
// sample stands for: thirty-two samples of a bright window then give a soft
// window, not thirty-two windows.

#include "engine_fullscreen.hlsli"

cbuffer GpuProbeFilterUniforms : register(b0, space3)
{
    // x the roughness, y how many samples, z the source's side in texels,
    // w one for the cosine lobe.
    float4 FilterParams;
};

Texture2D SourceTexture : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);

struct Interpolants
{
    float2 Uv : TEXCOORD0;
    float4 Position : SV_Position;
};

Interpolants VertexMain(uint vertexId : SV_VertexID)
{
    Interpolants output;
    fullscreenTriangle(vertexId, 0.0f, output.Position, output.Uv);
    return output;
}

static const float Pi = 3.14159265359f;

float3 octahedralDirection(float2 uv)
{
    const float2 f = uv * 2.0f - 1.0f;
    float3 d = float3(f.x, 1.0f - abs(f.x) - abs(f.y), f.y);
    const float fold = saturate(-d.y);
    d.x += d.x >= 0.0f ? -fold : fold;
    d.z += d.z >= 0.0f ? -fold : fold;
    return normalize(d);
}

float2 octahedralUv(float3 direction)
{
    const float3 d = normalize(direction);
    const float3 n = d / max(abs(d.x) + abs(d.y) + abs(d.z), 1e-6f);
    float2 f = float2(n.x, n.z);
    if (n.y < 0.0f)
    {
        f = float2((1.0f - abs(n.z)) * (n.x >= 0.0f ? 1.0f : -1.0f),
                   (1.0f - abs(n.x)) * (n.z >= 0.0f ? 1.0f : -1.0f));
    }
    return f * 0.5f + 0.5f;
}

// The second coordinate of a Hammersley point: the sample's index with its
// bits reversed.
float radicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float roughness = FilterParams.x;
    const uint count = uint(max(FilterParams.y, 1.0f));
    const bool cosine = FilterParams.w > 0.5f;
    const float3 normal = octahedralDirection(input.Uv);

    const float3 up = abs(normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);

    // A texel of the source, as a share of the sphere.
    const float texel = 4.0f * Pi / (FilterParams.z * FilterParams.z);
    const float alpha = max(roughness * roughness, 1e-3f);

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float total = 0.0f;
    [loop] for (uint index = 0u; index < count; ++index)
    {
        const float u1 = (float(index) + 0.5f) / float(count);
        const float u2 = radicalInverse(index);
        const float phi = 2.0f * Pi * u2;
        float3 toLight;
        float weight;
        float density;
        if (cosine)
        {
            const float sinTheta = sqrt(u1);
            const float cosTheta = sqrt(1.0f - u1);
            toLight = tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + normal * cosTheta;
            weight = 1.0f;
            density = cosTheta / Pi;
        }
        else
        {
            const float cosTheta = sqrt((1.0f - u1) / (1.0f + (alpha * alpha - 1.0f) * u1));
            const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
            const float3 halfway =
                tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + normal * cosTheta;
            toLight = 2.0f * dot(normal, halfway) * halfway - normal;
            weight = saturate(dot(normal, toLight));
            const float denominator = cosTheta * cosTheta * (alpha * alpha - 1.0f) + 1.0f;
            // The lobe's density over four: the view is the normal.
            density = alpha * alpha / (Pi * denominator * denominator) * 0.25f;
        }
        if (weight <= 0.0f)
        {
            continue;
        }
        const float covers = 1.0f / (float(count) * max(density, 1e-4f));
        const float level = max(0.5f * log2(covers / texel) + 1.0f, 0.0f);
        sum += SourceTexture.SampleLevel(SourceSampler, octahedralUv(toLight), level).rgb * weight;
        total += weight;
    }
    return float4(sum / max(total, 1e-4f), 1.0f);
}
