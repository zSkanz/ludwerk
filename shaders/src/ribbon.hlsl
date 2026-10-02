// Ribbons (ADR 0129): a `Beam`'s band and a `Trail`'s wake, as triangles the
// frame built on the CPU -- where each corner is, its colour there, and how
// far along and across the ribbon it lies.
//
// **Blended as a particle is**, premultiplied, in the particles' own pass:
// the alpha written is the opacity times what is NOT emission, so one
// pipeline covers a tyre mark that hides the road and a laser that only adds
// light, and everything between.
//
// **Tested against the scene by hand, and soft where it meets it**, for the
// reason a particle is: the pass has no depth attachment, it reads the depth
// the opaque surfaces wrote. A fragment behind the scene is dropped; one just
// in front of it fades over the ribbon's own half-width, up to a metre, so a
// beam does not draw a hard line where it enters a wall.

cbuffer GpuParticleUniforms : register(b0, space1)
{
    float4x4 ViewProjection;
    // The particles' camera axes: a ribbon's corners are already where they
    // are, so it reads neither.
    float4 CameraRight;
    float4 CameraUp;
};

cbuffer GpuParticleLighting : register(b0, space3)
{
    float4 Ambient;
    float4 SunLight;
    float4 ParticleFogColor;
    // x: fog start, z: 1 / (end - start), zero when fog is off.
    float4 ParticleFogRange;
    // x near plane, y far plane, zw one over the target's size in pixels. A
    // NEGATIVE near plane is an orthographic camera, whose depth is linear.
    float4 ParticleDepth;
};

// The opaque scene's depth, and the ribbon's own image -- a white pixel for a
// ribbon with none.
Texture2D SceneDepth : register(t0, space2);
SamplerState SceneDepthSampler : register(s0, space2);
Texture2D RibbonTexture : register(t1, space2);
SamplerState RibbonSampler : register(s1, space2);

struct VertexInput
{
    // xyz camera-relative position, w half the ribbon's width there.
    float4 PositionHalf : TEXCOORD0;
    // Linear colour, and opacity.
    float4 Color : TEXCOORD1;
    // x along the ribbon, y across it, z emission, w how much the scene's
    // light colours it.
    float4 Params : TEXCOORD2;
};

struct Interpolants
{
    float4 Position : SV_Position;
    float4 Color : TEXCOORD0;
    float4 Params : TEXCOORD1;
    float Distance : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
    float HalfWidth : TEXCOORD4;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    output.Position = mul(ViewProjection, float4(input.PositionHalf.xyz, 1.0f));
    output.Color = input.Color;
    output.Params = input.Params;
    output.Distance = length(input.PositionHalf.xyz);
    output.ViewDepth = output.Position.w;
    output.HalfWidth = input.PositionHalf.w;
    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float4 image = RibbonTexture.Sample(RibbonSampler, input.Params.xy);

    const bool orthographic = ParticleDepth.x < 0.0f;
    const float near = orthographic ? -ParticleDepth.y : ParticleDepth.x;
    const float far = ParticleDepth.y;
    const float device = SceneDepth.SampleLevel(SceneDepthSampler, input.Position.xy * ParticleDepth.zw, 0.0f).r;
    const float scene = orthographic ? near + device * (far - near)
                                     : (near * far) / max(far - device * (far - near), 1e-6f);
    const float own = orthographic ? near + input.Position.z * (far - near) : input.ViewDepth;
    const float soft = saturate((scene - own) / clamp(input.HalfWidth, 1e-3f, 1.0f));

    const float alpha = input.Color.a * image.a * soft;
    if (alpha <= 0.002f)
        discard;

    const float3 base = input.Color.rgb * image.rgb;
    const float emission = saturate(input.Params.z);
    const float influence = saturate(input.Params.w);
    const float fog = saturate((input.Distance - ParticleFogRange.x) * ParticleFogRange.z);
    // Lit as the open air is, by as much as it is told to be: at no influence
    // it is its own colour at any hour.
    const float3 lit = base * lerp(float3(1.0f, 1.0f, 1.0f), Ambient.rgb + SunLight.rgb, influence);
    const float3 blended = lerp(lit, ParticleFogColor.rgb, fog);
    const float3 added = lit * (1.0f - fog);
    const float3 color = lerp(blended, added, emission);

    return float4(color * alpha, alpha * (1.0f - emission));
}
