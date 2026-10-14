// **The side of a line a view is not seen on, taken out before it is drawn**
// (ADR 0205). Two players' views put together across a line through the
// window's middle are each one side of it, and a view drawn whole would be a
// window's worth of pixels lit to be thrown away. The line is known before
// either view is drawn, so this writes the NEAR plane into the depth over the
// side that is not kept: every surface, the sky and everything blended fails
// its depth test there, and no material is run for a pixel nobody sees.
//
// A triangle over the whole target, discarded on the side that is kept. No
// colour: the pass has none.

#include "engine_fullscreen.hlsli"

cbuffer GpuViewSideUniforms : register(b0, space3)
{
    // xy the line's normal in pixels, y down, pointing AWAY from the side that
    // is kept; z how many pixels past the line are still drawn -- the line's
    // own half width, and what a filter reads across it.
    float4 SideLine;
    // xy the target, in pixels.
    float4 SideSize;
};

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

void FragmentMain(Interpolants input)
{
    const float2 place = (input.Uv - 0.5f) * SideSize.xy;
    if (dot(place, SideLine.xy) <= SideLine.z)
        discard;
}
