// Decals (F2): an image projected onto whatever lies inside a box.
//
// **Drawn between the opaque surfaces and the transparent ones**, in a pass of
// its own with no depth attachment -- which is what lets it READ the depth the
// opaque surfaces wrote. The box is drawn, each pixel it covers looks up what
// is actually there, turns that back into a position, and paints only if the
// position is inside the box.
//
// **Multiplied into the lit colour**, and that is the whole lighting model: a
// surface's light is its albedo times what reaches it, so multiplying the lit
// colour by the image is multiplying the albedo by it -- the decal keeps the
// surface's sun, shadow and bounce exactly, with no second lighting pass. What
// it gives up is brightening: a white pixel is no change, and nothing a decal
// does can make a surface lighter than it was.

cbuffer GpuDecalUniforms : register(b0, space1)
{
    // The unit box (-0.5 to 0.5 on each axis) into camera-relative world space.
    float4x4 BoxToWorld;
    float4x4 ViewProjection;
};

cbuffer GpuDecalFragment : register(b0, space3)
{
    float4x4 WorldToBox;
    float4x4 InverseViewProjection;
    // The image's multiplier and how much of it lands (1 - transparency).
    float4 DecalColor;
    // xy: 1 / render size; z: 1 when there is an image.
    float4 DecalParams;
    // The box's projection axis in world space, for the grazing-angle fade.
    // w: 1 when `MaskTexture` holds the depth of what no decal paints, 2 when
    // `FoliageTexture` holds the depth of the foliage one does, 3 for both.
    float4 DecalAxis;
    // rgb: the light an Alpha decal is lit by; a: its glow (ADR 0160).
    float4 DecalLight;
};

Texture2D DecalTexture : register(t0, space2);
SamplerState DecalSampler : register(s0, space2);
Texture2D<float> DepthTexture : register(t1, space2);
SamplerState DepthSampler : register(s1, space2);
// **The depth of the parts that receive no decal** (`BasePart.ReceivesDecals`),
// drawn alone. Where it is the depth of the picture, what is seen there is one
// of them, and the decal leaves it as it is; where the picture is nearer or
// further, something else is seen and is painted.
Texture2D<float> MaskTexture : register(t2, space2);
SamplerState MaskSampler : register(s2, space2);
// **The depth of the foliage a decal paints** (`FoliageLayer.ReceivesDecals`,
// ADR 0185), drawn alone. Where it is the depth of the picture, what is seen
// there is a blade of it, and the decal lands whichever way the blade faces.
Texture2D<float> FoliageTexture : register(t3, space2);
SamplerState FoliageSampler : register(s3, space2);

struct Interpolants
{
    float4 Position : SV_Position;
};

Interpolants VertexMain(uint vertexId : SV_VertexID)
{
    // A cube as twelve triangles, from the vertex index: the six faces, each
    // two triangles, corners picked from the eight by a table.
    const uint faces[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                            2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    const uint corner = faces[vertexId % 36u];
    const float3 local = float3((corner & 1u) ? 0.5f : -0.5f, (corner & 2u) ? 0.5f : -0.5f,
                                (corner & 4u) ? 0.5f : -0.5f);
    Interpolants output;
    output.Position = mul(ViewProjection, mul(BoxToWorld, float4(local, 1.0f)));
    return output;
}

float4 FragmentMain(Interpolants input, bool isFront : SV_IsFrontFace) : SV_Target0
{
    // **Only the box's far side paints.** Seen from outside, every pixel of the
    // box crosses a near face and a far one, and a multiply applied twice is a
    // decal twice as dark; the far side is crossed once whether the camera is
    // outside the box or in it.
    if (isFront)
        discard;
    const float2 uv = input.Position.xy * DecalParams.xy;
    const float depth = DepthTexture.SampleLevel(DepthSampler, uv, 0.0f);
    // The sky: nothing there to paint.
    if (depth >= 1.0f)
        discard;
    // A part that receives none. The two depths are of one surface drawn
    // twice with the same vertices, so they agree to the last bits; the slack
    // is for a cutout, whose depth the picture took from another shader.
    const bool masked = fmod(DecalAxis.w, 2.0f) > 0.5f;
    if (masked && abs(MaskTexture.SampleLevel(MaskSampler, uv, 0.0f) - depth) <= 2.0e-6f)
        discard;

    // Back to a position, and into the box.
    const float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    const float4 world4 = mul(InverseViewProjection, clip);
    const float3 world = world4.xyz / world4.w;
    const float3 local = mul(WorldToBox, float4(world, 1.0f)).xyz;
    if (any(abs(local) > 0.5f))
        discard;

    // **Faded where the surface turns away from the projection**, or an image
    // projected onto a floor smears down the side of every step it crosses.
    const float3 normal = normalize(cross(ddy(world), ddx(world)));
    const float facing = abs(dot(normal, normalize(DecalAxis.xyz)));
    float fade = saturate((facing - 0.15f) / 0.35f);
    // **Foliage takes a decal as the ground under it does** (ADR 0185). A
    // blade of grass is a card standing on end: edge-on to every mark laid on
    // the ground, and by the fade above the one thing in a painted field left
    // clean. Its facing says nothing about whether the mark is on it -- it is
    // in the box, and it is not the side of a step.
    if (DecalAxis.w > 1.5f && abs(FoliageTexture.SampleLevel(FoliageSampler, uv, 0.0f) - depth) <= 2.0e-6f)
        fade = 1.0f;

    float4 image = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (DecalParams.z > 0.5f)
        image = DecalTexture.Sample(DecalSampler, float2(local.x + 0.5f, 0.5f - local.y));

    const float amount = saturate(image.a * DecalColor.a * fade);
    const float3 colour = image.rgb * DecalColor.rgb;
    // **Laid over, or added** (ADR 0160) -- what a multiply cannot do, which
    // is brighten: a red warning multiplied onto grass is brown.
    if (DecalParams.w > 1.5f) {
        // Additive: the picture's light, once and as much again as it glows.
        return float4(colour * (1.0f + DecalLight.a) * amount, 0.0f);
    }
    if (DecalParams.w > 0.5f) {
        // Alpha, premultiplied: lit as the surface under it is lit, and
        // glowing on top of that.
        return float4((colour * DecalLight.rgb + colour * DecalLight.a) * amount, amount);
    }
    // Multiplied by the blend into what is there: one where nothing lands.
    return float4(lerp(float3(1.0f, 1.0f, 1.0f), colour, amount), 1.0f);
}
