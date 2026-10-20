// The forward pass for a run of parts whose light is baked (ADR 0253): what
// `pbr_instanced.hlsl` draws, and two things more. Each instance brings where
// its part's rows begin in the frame's table of lightmap rectangles, and each
// vertex its place in the part's lightmap -- a stream of its own that only
// this pipeline binds, so every other draw's vertices stay as they are. The
// vertex stage turns the one into a page and a place on it by the other; the
// fragment stage reads the page for the light that reaches the surface and
// for how much of the sun it sees (`engine_forward.hlsli`, `ENG_BAKED`).
//
// A primitive's charts stand in a grid of cells, a face a cell, and each cell
// is its own rectangle of a page sized by what that face spans: the cell the
// coordinate falls in picks the rectangle. A mesh's are one square.

#define ENG_UNIFORMS_OBJECT
#define ENG_UNIFORMS_FRAME
#define ENG_UNIFORMS_MATERIAL
#define ENG_INSTANCE_TINT
#define ENG_BAKED
#include "engine_forward.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
    // Slot 1, per instance: the model matrix as four columns, then the
    // instance's own alpha and base colour. `render::GpuInstance`, 80 bytes.
    float4 ModelColumn0 : TEXCOORD4;
    float4 ModelColumn1 : TEXCOORD5;
    float4 ModelColumn2 : TEXCOORD6;
    float4 ModelColumn3 : TEXCOORD7;
    float4 InstanceAlphaTint : TEXCOORD8;
    // Slot 2, per instance: where its part's rows begin, and 1 where the
    // mask is the sun's shadow this frame.
    float4 Baked : TEXCOORD9;
    // Slot 3, per vertex: its place in the part's lightmap.
    float2 LightmapUv : TEXCOORD10;
};

// Every baked part's rows (`render::RenderLightmap::rects`): its grid, then
// a rectangle a chart -- page plus left, top, width, height.
StructuredBuffer<float4> LightmapRects : register(t0, space0);

// A chart's margin inside its cell (`asset::kPrimitiveChartMargin`).
static const float ChartMargin = 0.04f;

float3 lightmapPlace(float2 uv, uint row, uint layersPerPage)
{
    const float4 grid = LightmapRects[row];
    float2 local = uv;
    uint cell = 0u;
    if (grid.x > 0.5f)
    {
        const float2 cells = grid.xy;
        const float2 scaled = uv * cells;
        // The cell the coordinate falls in, held inside the grid: a vertex on
        // a chart's own edge is its chart's, which the margin keeps it within.
        const float2 index = clamp(floor(scaled), float2(0.0f, 0.0f), cells - 1.0f);
        cell = uint(index.y * cells.x + index.x);
        local = (scaled - index - ChartMargin) / (1.0f - 2.0f * ChartMargin);
    }
    const float4 rect = LightmapRects[row + 1u + cell];
    const float page = floor(rect.x);
    return float3(rect.x - page + local.x * rect.z, rect.y + local.y * rect.w, page * float(layersPerPage));
}

// `core::Mat4` stores `m[column][row]`, so the four attributes ARE the matrix's
// four columns -- and **HLSL's `float4x4(a, b, c, d)` constructor fills ROWS**.
// Handing it four columns therefore builds the TRANSPOSE, which is what this
// shader did until somebody looked at the horde instead of counting its draw
// calls. `mul(M^T, p)` puts a dot product of the translation into `w`, so
// every instanced vertex went somewhere off-screen and the pass drew nothing.
//
// `column_major` on the uniform blocks is not the same statement: it describes
// how a matrix is LAID OUT IN MEMORY, and a vertex attribute has no layout to
// declare -- it arrives as four separate vectors and how they are assembled is
// this function's business.
float4x4 instanceModel(VertexInput input)
{
    return transpose(float4x4(input.ModelColumn0, input.ModelColumn1, input.ModelColumn2, input.ModelColumn3));
}

// How long the surface's own U and V directions are in the world, per object
// unit: what turns a primitive's 0..1 face UV into metres.
float2 uvMetresScale(float3x3 model, float3 normal, float4 tangent)
{
    const float3 bitangent = cross(normal, tangent.xyz) * (tangent.w < 0.0f ? -1.0f : 1.0f);
    return float2(length(mul(model, tangent.xyz)), length(mul(model, bitangent)));
}

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;

    const float4x4 model = instanceModel(input);
    const float4 shadingPosition = mul(model, float4(input.Position, 1.0f));
    output.ShadingPosition = shadingPosition.xyz;
    output.Position = mul(ViewProjection, shadingPosition);
    output.ViewDepth = output.Position.w;

    // The cofactor matrix of the rotation-scale block, so a non-uniform scale
    // tilts the normal instead of shearing it. Read straight off the attributes
    // rather than out of `model`: these are the transformed basis vectors, which
    // is what the matrix's COLUMNS are, and indexing a `float4x4` gives rows.
    const float3 a = input.ModelColumn0.xyz;
    const float3 b = input.ModelColumn1.xyz;
    const float3 c = input.ModelColumn2.xyz;
    //
    // **The three cross products are the cofactor matrix's COLUMNS**, as
    // `normalMatrixOf` stores them -- and `float3x3(a, b, c)` fills ROWS, the
    // same trap `instanceModel` above names. Built without the
    // transpose it was the cofactor's transpose, which for a rotation is its
    // inverse: every instanced part was lit as if turned the other way, a
    // quarter-turned crate bright on its shaded side.
    const float3x3 normalMatrix = transpose(float3x3(cross(b, c), cross(c, a), cross(a, b)));

    output.Normal = mul(normalMatrix, input.Normal);
    // A tangent is a direction along the surface, not a covector, so it rides on
    // the model matrix rather than on the cofactor one.
    output.Tangent = float4(mul((float3x3)model, input.Tangent.xyz), input.Tangent.w);
    output.Uv = input.Uv;
    output.UvMetres = input.Uv * uvMetresScale((float3x3)model, input.Normal, input.Tangent);
    output.InstanceAlpha = input.InstanceAlphaTint.x;
    output.InstanceTint = input.InstanceAlphaTint.yzw;
    // A page's layers: one, the mask its light's alpha; two, the mask the
    // layer after its light, which in blocks has none.
    const uint layersPerPage = max(uint(input.Baked.z + 0.5f), 1u);
    output.LightmapPlace = lightmapPlace(input.LightmapUv, uint(input.Baked.x + 0.5f), layersPerPage);
    output.MaskHolds = input.Baked.y * float(layersPerPage);

    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    return shadeForward(input);
}
