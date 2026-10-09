// Particles (F2): one camera-facing square per instance, shaped in the shader.
//
// **No vertex buffer at all.** Each particle is an instance -- position and
// size, colour, and how it blends -- and its six corners come from the vertex
// index, so ten thousand particles are one instanced draw of six vertices.
//
// **One pipeline for both kinds of particle**, blended premultiplied: the
// colour is multiplied by its opacity here, and the alpha written is the
// opacity times what is NOT emission. At emission 0 that is an ordinary blend;
// at 1 the alpha is zero and the colour simply adds -- fire and sparks -- and
// between is both, with no second pipeline and no sort between the two kinds.
//
// **Soft, and tested against the scene by hand.** The pass has no depth
// attachment: it reads the depth the opaque surfaces wrote as a texture, which
// a pass with it attached cannot (decals are drawn the same way). A fragment
// behind the scene is dropped, which is the depth test; one in front of it
// fades out over the last stretch before the surface it would cross, which is
// what stops a puff of smoke drawing a hard line where it meets the ground.
// The stretch is the particle's own half-size, up to a metre: a spark stays
// crisp and a cloud fades over the cloud's depth.

cbuffer GpuParticleUniforms : register(b0, space1)
{
    float4x4 ViewProjection;
    // The camera's own right and up, in the camera-relative world space every
    // position here is in: a particle faces the camera by being built on them.
    float4 CameraRight;
    float4 CameraUp;
};

cbuffer GpuParticleLighting : register(b0, space3)
{
    // What lights a particle that is not emitting: the ambient, and the sun
    // arriving from any direction -- a particle has no normal worth the name.
    float4 Ambient;
    float4 SunLight;
    float4 ParticleFogColor;
    // x: fog start, z: 1 / (end - start), zero when fog is off.
    float4 ParticleFogRange;
    // x near plane, y far plane, zw one over the target's size in pixels. A
    // NEGATIVE near plane is an orthographic camera (the 2D layer), whose
    // depth is linear already.
    float4 ParticleDepth;
};

// The opaque scene's depth, as the prepass and the forward pass left it.
Texture2D SceneDepth : register(t0, space2);
SamplerState SceneDepthSampler : register(s0, space2);
// **Its picture** (ADR 0160), or a white pixel for a particle drawn as its
// shape. Colour, decoded from sRGB by the sampler, times the particle's.
Texture2D ParticleTexture : register(t1, space2);
SamplerState ParticleSampler : register(s1, space2);

#ifndef ENG_PARTICLE_GPU
// A bounded palette preserves depth order without a draw per texture change.
// Explicit resources work on ordinary SDL_GPU and native D3D12 devices without
// bindless descriptors. Keep the seven slots in sync with MaxParticleTextures.
Texture2D ParticleTexture2 : register(t2, space2);
SamplerState ParticleSampler2 : register(s2, space2);
Texture2D ParticleTexture3 : register(t3, space2);
SamplerState ParticleSampler3 : register(s3, space2);
Texture2D ParticleTexture4 : register(t4, space2);
SamplerState ParticleSampler4 : register(s4, space2);
Texture2D ParticleTexture5 : register(t5, space2);
SamplerState ParticleSampler5 : register(s5, space2);
Texture2D ParticleTexture6 : register(t6, space2);
SamplerState ParticleSampler6 : register(s6, space2);
Texture2D ParticleTexture7 : register(t7, space2);
SamplerState ParticleSampler7 : register(s7, space2);

float4 particlePicture(uint slot, float2 uv, float2 dx, float2 dy)
{
    switch (slot) {
    case 2u: return ParticleTexture2.SampleGrad(ParticleSampler2, uv, dx, dy);
    case 3u: return ParticleTexture3.SampleGrad(ParticleSampler3, uv, dx, dy);
    case 4u: return ParticleTexture4.SampleGrad(ParticleSampler4, uv, dx, dy);
    case 5u: return ParticleTexture5.SampleGrad(ParticleSampler5, uv, dx, dy);
    case 6u: return ParticleTexture6.SampleGrad(ParticleSampler6, uv, dx, dy);
    case 7u: return ParticleTexture7.SampleGrad(ParticleSampler7, uv, dx, dy);
    default: return ParticleTexture.SampleGrad(ParticleSampler, uv, dx, dy);
    }
}
#endif

#ifdef ENG_PARTICLE_GPU
// One particle as the compute pass keeps it (`particle_sim.hlsl`).
struct SimParticle
{
    float3 Position;
    float Age;
    float3 Velocity;
    float Lifetime;
    float Rotation;
    float Spin;
    float Frame;
    float Stuck;
};
StructuredBuffer<SimParticle> SimParticles : register(t0, space0);

cbuffer GpuParticleLook : register(b1, space1)
{
    // xyz: the emitter's origin from the camera; w: emission.
    float4 OriginEmission;
    // x columns, y rows, z `Enum.ParticleFlipbookMode`, w frames a second.
    float4 Flipbook;
    // x shape, y 1 when drawn from its picture.
    float4 Kind;
    // Its colour and opacity at sixteen places along its life, the start and
    // end values and the curves already multiplied.
    float4 ColorOverLife[16];
    // Its size at the same sixteen, four to a row.
    float4 SizeOverLife[4];
};

struct VertexInput
{
    uint Vertex : SV_VertexID;
    uint Instance : SV_InstanceID;
};
#else
struct VertexInput
{
    // xyz camera-relative position, w width in metres.
    float4 PositionSize : TEXCOORD0;
    // Linear colour times brightness, and opacity.
    float4 Color : TEXCOORD1;
    // x emission, y shape, z rotation in radians, w one-based picture slot
    // (zero for a procedural shape).
    float4 Params : TEXCOORD2;
    // The picture's frame: left, top, right, bottom.
    float4 Frame : TEXCOORD3;
    uint Vertex : SV_VertexID;
};
#endif

struct Interpolants
{
    float4 Position : SV_Position;
    float4 Color : TEXCOORD0;
    float2 Corner : TEXCOORD1;
    float2 Params : TEXCOORD2;
    float Distance : TEXCOORD3;
    // Its distance along the view axis and its half-size, for the fade.
    float ViewDepth : TEXCOORD4;
    float HalfSize : TEXCOORD5;
    float2 Uv : TEXCOORD6;
    nointerpolation float Textured : TEXCOORD7;
};

// One corner of one particle: `centre` camera-relative, `size` metres across,
// `params` as an instance carries them, `frame` the picture's rectangle.
Interpolants cornerOf(uint vertex, float3 centre, float size, float4 color, float4 params, float4 frame)
{
    // Two triangles, in the order a square is cut: 0 1 2, 0 2 3.
    const float2 corners[6] = {float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
                               float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(-1.0f, 1.0f)};
    const float2 corner = corners[vertex % 6u];
    const float halfSize = size * 0.5f;
    // Turned in the plane facing the camera (ADR 0160).
    const float turnCos = cos(params.z);
    const float turnSin = sin(params.z);
    const float2 turned = float2(corner.x * turnCos - corner.y * turnSin, corner.x * turnSin + corner.y * turnCos);
    const float3 position = centre + (CameraRight.xyz * turned.x + CameraUp.xyz * turned.y) * halfSize;

    Interpolants output;
    output.Position = mul(ViewProjection, float4(position, 1.0f));
    output.Color = color;
    output.Corner = corner;
    output.Params = params.xy;
    output.Distance = length(centre);
    output.ViewDepth = output.Position.w;
    output.HalfSize = halfSize;
    // The top of the picture is the top of the particle.
    output.Uv = float2(corner.x < 0.0f ? frame.x : frame.z, corner.y < 0.0f ? frame.w : frame.y);
    output.Textured = params.w;
    return output;
}

#ifdef ENG_PARTICLE_GPU
Interpolants VertexMain(VertexInput input)
{
    const SimParticle particle = SimParticles[input.Instance];
    // **A slot with nobody in it is a square of no size**: every corner at one
    // point, which draws nothing.
    if (particle.Lifetime <= 0.0f || particle.Age >= particle.Lifetime) {
        Interpolants none;
        none.Position = float4(0.0f, 0.0f, 0.0f, 0.0f);
        none.Color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        none.Corner = float2(0.0f, 0.0f);
        none.Params = float2(0.0f, 0.0f);
        none.Distance = 0.0f;
        none.ViewDepth = 0.0f;
        none.HalfSize = 0.0f;
        none.Uv = float2(0.0f, 0.0f);
        none.Textured = 0.0f;
        return none;
    }
    // Where along its life, and the sixteen places either side of it.
    const float life = saturate(particle.Age / particle.Lifetime);
    const float along = life * 15.0f;
    const uint low = min(uint(along), 14u);
    const float between = along - float(low);
    const float4 color = lerp(ColorOverLife[low], ColorOverLife[low + 1u], between);
    const float size =
        lerp(SizeOverLife[low / 4u][low % 4u], SizeOverLife[(low + 1u) / 4u][(low + 1u) % 4u], between);

    // The frame of its flipbook: on a loop, once over its life, or its own.
    const uint columns = max(uint(Flipbook.x + 0.5f), 1u);
    const uint rows = max(uint(Flipbook.y + 0.5f), 1u);
    const uint frames = columns * rows;
    const uint mode = uint(Flipbook.z + 0.5f);
    const uint own = uint(particle.Frame + 0.5f);
    uint shown = (uint(particle.Age * Flipbook.w) + own) % frames;
    if (mode == 1u)
        shown = min(uint(life * float(frames)), frames - 1u);
    else if (mode == 2u)
        shown = own % frames;
    const float2 cell = float2(1.0f / float(columns), 1.0f / float(rows));
    const float2 corner = float2(float(shown % columns), float(shown / columns)) * cell;

    return cornerOf(input.Vertex, particle.Position + OriginEmission.xyz, size, color,
                    float4(OriginEmission.w, Kind.x, particle.Rotation, Kind.y),
                    float4(corner.x, corner.y, corner.x + cell.x, corner.y + cell.y));
}
#else
Interpolants VertexMain(VertexInput input)
{
    return cornerOf(input.Vertex, input.PositionSize.xyz, input.PositionSize.w, input.Color, input.Params,
                    input.Frame);
}
#endif

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float radius = length(input.Corner);
    const uint shape = uint(input.Params.y + 0.5f);
    float coverage = 1.0f;
    float3 picture = float3(1.0f, 1.0f, 1.0f);
#ifndef ENG_PARTICLE_GPU
    // Derivatives must be computed before branching on the instance's picture;
    // neighbouring pixel lanes may belong to different particles.
    const float2 uvDx = ddx(input.Uv);
    const float2 uvDy = ddy(input.Uv);
#endif
    if (input.Textured > 0.5f) {
        // A picture is its own outline: the shape is not cut out of it.
#ifdef ENG_PARTICLE_GPU
        const float4 texel = ParticleTexture.Sample(ParticleSampler, input.Uv);
#else
        const float4 texel = particlePicture(uint(input.Textured + 0.5f), input.Uv, uvDx, uvDy);
#endif
        picture = texel.rgb;
        coverage = texel.a;
    }
    else if (shape == 0u) {
        // Soft: a falloff to nothing at the edge, squared so the centre is
        // dense and the rim is air.
        const float fall = saturate(1.0f - radius);
        coverage = fall * fall;
    }
    else if (shape == 1u) {
        // Disc: a crisp edge, anti-aliased across one pixel's width.
        const float edge = max(fwidth(radius), 1e-4f);
        coverage = 1.0f - smoothstep(1.0f - edge, 1.0f, radius);
    }

    // The scene's depth at this pixel, linear, against the particle's own.
    // An orthographic camera's depth runs from its far distance behind it to
    // the same in front (`render_world.cpp`).
    const bool orthographic = ParticleDepth.x < 0.0f;
    const float near = orthographic ? -ParticleDepth.y : ParticleDepth.x;
    const float far = ParticleDepth.y;
    const float device = SceneDepth.SampleLevel(SceneDepthSampler, input.Position.xy * ParticleDepth.zw, 0.0f).r;
    const float scene = orthographic ? near + device * (far - near)
                                     : (near * far) / max(far - device * (far - near), 1e-6f);
    const float own = orthographic ? near + input.Position.z * (far - near) : input.ViewDepth;
    const float soft = saturate((scene - own) / clamp(input.HalfSize, 1e-3f, 1.0f));

    const float alpha = input.Color.a * coverage * soft;
    if (alpha <= 0.002f)
        discard;

    const float emission = saturate(input.Params.x);
    const float fog = saturate((input.Distance - ParticleFogRange.x) * ParticleFogRange.z);
    // Blended particles are lit and fogged like a surface; emitting ones carry
    // their own light and simply fade into the fog.
    const float3 base = input.Color.rgb * picture;
    const float3 lit = base * (Ambient.rgb + SunLight.rgb);
    const float3 blended = lerp(lit, ParticleFogColor.rgb, fog);
    const float3 added = base * (1.0f - fog);
    const float3 color = lerp(blended, added, emission);

    return float4(color * alpha, alpha * (1.0f - emission));
}
