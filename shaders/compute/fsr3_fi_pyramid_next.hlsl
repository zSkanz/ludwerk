// **Frame interpolation: a level of a pyramid from the level before** (ADR
// 0165) -- the vector field's or the picture's, by `EngColours`.
// `fsr3_fi_vector_pyramid` says why a pass a level, and why a level is
// written twice.

Texture2D<float4> r_eng_level : register(t0, space0);
SamplerState s_eng_level : register(s0, space0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> rw_eng_level : register(u0, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> rw_eng_mip : register(u1, space1);

cbuffer EngPyramid : register(b0, space2)
{
    // The size of the level read.
    int2 EngSourceSize;
    // Whether it is the picture's pyramid, and the vector field's otherwise.
    int EngColours;
    int EngPad;
};

// AMD's `SpdReduce4` for the vector field: the mean of the entries that hold a
// vector, which is the ones with a priority.
float4 engReduceVectors(float4 v0, float4 v1, float4 v2, float4 v3)
{
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float weights = 0.0f;
    const float w0 = float(v0.z > 0.0f);
    const float w1 = float(v1.z > 0.0f);
    const float w2 = float(v2.z > 0.0f);
    const float w3 = float(v3.z > 0.0f);
    sum = v0 * w0 + v1 * w1 + v2 * w2 + v3 * w3;
    weights = w0 + w1 + w2 + w3;
    return sum / (weights > 1e-03f ? weights : 1.0f);
}

// AMD's `SpdReduce4` for the picture: the mean by each sample's weight, which
// is how much of it the interpolation managed.
float4 engReduceColours(float4 v0, float4 v1, float4 v2, float4 v3)
{
    const float sum = v0.w + v1.w + v2.w + v3.w;
    if (sum == 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return (v0 * v0.w + v1 * v1.w + v2 * v2.w + v3 * v3.w) / sum;
}

float4 engSource(int2 at)
{
    return r_eng_level.SampleLevel(s_eng_level, (float2(at) + 0.5f) / float2(EngSourceSize), 0);
}

[numthreads(8, 8, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 at = int2(dispatchThreadId.xy);
    if (any(at >= EngSourceSize / 2))
        return;
    const int2 corner = at * 2;
    const float4 v0 = engSource(corner);
    const float4 v1 = engSource(corner + int2(1, 0));
    const float4 v2 = engSource(corner + int2(0, 1));
    const float4 v3 = engSource(corner + int2(1, 1));
    const float4 reduced = EngColours != 0 ? engReduceColours(v0, v1, v2, v3) : engReduceVectors(v0, v1, v2, v3);
    rw_eng_level[at] = reduced;
    rw_eng_mip[at] = reduced;
}
