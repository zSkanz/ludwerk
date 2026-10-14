// Restricted array views select a single source mip/layer for native copies.
#include "engine_fullscreen.hlsli"
Texture2DArray<float4> Source : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);
struct Interpolants
{
    float2 Uv : TEXCOORD0;
    float4 Position : SV_Position;
};
Interpolants VertexMain(uint vertexId : SV_VertexID)
{
    Interpolants result;
    fullscreenTriangle(vertexId, 0.0f, result.Position, result.Uv);
    return result;
}
float4 FragmentMain(Interpolants input) : SV_Target0
{
    return Source.SampleLevel(SourceSampler, float3(input.Uv, 0.0f), 0.0f);
}
