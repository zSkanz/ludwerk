// One layer of a terrain's layer arrays, drawn from a material's maps
// (ADR 0113; the terrain audit's TA13).
//
// **Drawn rather than copied.** A copy scales mip 0 of the source into the
// layer with one bilinear tap per texel, so a map over twice the layer's size
// skipped texels and shimmered with moiré; this reads the source at the mip
// whose size fits the layer, between two mips, as a sampler would.
//
// **The surface layer is packed here**: the material's height in R, from its
// height map, and its roughness and metalness in G and B from its
// metallic-roughness map. glTF leaves that map's R undefined unless the same
// image is also the occlusion map, and the terrain read it as occlusion and as
// height -- a material with a plain map drew its ambient at four tenths and
// lost every height blend. The mesh path never read it, for the same reason.

#include "engine_fullscreen.hlsli"

cbuffer GpuTerrainPackUniforms : register(b0, space3)
{
    // x: 1 for the surface layer, 0 for a map drawn as it is; y: 1 when the
    // second map is the material's height; z: the layer's size in texels.
    float4 Pack;
};

Texture2D Source : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);
Texture2D Height : register(t1, space2);
SamplerState HeightSampler : register(s1, space2);

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

// The mip of `map` whose larger side is the layer's, or the last it has.
float mipFor(Texture2D map)
{
    uint width = 0;
    uint height = 0;
    uint levels = 0;
    map.GetDimensions(0u, width, height, levels);
    const float larger = float(max(max(width, height), 1u));
    return clamp(log2(larger / max(Pack.z, 1.0f)), 0.0f, float(max(levels, 1u) - 1u));
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float4 source = Source.SampleLevel(SourceSampler, input.Uv, mipFor(Source));
    if (Pack.x < 0.5f)
        return source;
    // No height map is level ground: the middle, which neither wins nor loses
    // a height blend.
    const float height = Pack.y > 0.5f ? Height.SampleLevel(HeightSampler, input.Uv, mipFor(Height)).r : 0.5f;
    return float4(height, source.g, source.b, 1.0f);
}
