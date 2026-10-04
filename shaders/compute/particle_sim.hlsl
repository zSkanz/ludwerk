// Particles simulated on the GPU (ADR 0160): one emitter's particles born,
// moved, collided and aged in place, a thread a particle.
//
// **The buffer is the particles.** Each slot holds one, alive while its age is
// under its lifetime; the CPU says only how many are born this frame and from
// which slot on, round the buffer like a ring, so the oldest slots are the
// ones reused. Nothing is read back: the draw reads the same buffer.
//
// **Born from a hash, not a generator**: a particle's direction, place and
// turn come from its slot, the emitter's seed and the frame it was born on --
// the same on every run, which is what a golden image needs, and with no state
// carried between threads.
//
// **Collided two ways**, as the engines that do this do it. The scene's depth:
// the particle is projected into the last frame's picture and tested against
// the depth there -- everything on the screen, and nothing off it or behind
// something. The terrain: a map of the ground's heights round the camera, read
// where the particle is, in view or not.

struct SimParticle
{
    // From the emitter's own origin, which the CPU keeps in doubles.
    float3 Position;
    float Age;
    float3 Velocity;
    // Zero for a slot never born.
    float Lifetime;
    // Radians, and radians a second.
    float Rotation;
    float Spin;
    // A Random flipbook's frame.
    float Frame;
    // 1 once it has stuck where it landed.
    float Stuck;
};

Texture2D<float> DepthTexture : register(t0, space0);
SamplerState DepthSampler : register(s0, space0);
Texture2D<float> GroundTexture : register(t1, space0);
SamplerState GroundSampler : register(s1, space0);
RWStructuredBuffer<SimParticle> Particles : register(u0, space1);

cbuffer GpuParticleSim : register(b0, space2)
{
    // The last frame's camera: camera-relative world to clip, and back.
    column_major float4x4 PrevViewProjection;
    column_major float4x4 PrevInverseViewProjection;
    // xyz: where particles are born, from the emitter's origin; w: seconds.
    float4 EmitterPlace;
    // xyz: the emitter's up; w: metres a second at birth.
    float4 EmitterUp;
    // xyz: its side; w: the cone's half-angle, radians.
    float4 EmitterSide;
    // xyz: the box they are born in; w: seconds each lives.
    float4 EmitterExtent;
    // xyz: metres a second squared; w: the share of speed lost a second.
    float4 Acceleration;
    // xyz: what the wind carries them by, metres a second.
    float4 Wind;
    // Radians: the turn at birth and its spread, the spin and its spread.
    float4 Turn;
    // x: what it meets (1 depth, 2 terrain, 3 both); y: 0 bounce, 1 stick,
    // 2 kill; z: bounce; w: friction.
    float4 Collide;
    // x: how far from its middle it touches; y: the speed under which a bounce
    // rests; z: a Random flipbook's frame count.
    float4 Touch;
    // xyz: the emitter's origin from the last frame's camera.
    float4 OriginFromCamera;
    // x near, y far (negative near: orthographic); zw: one over the depth's size.
    float4 DepthParams;
    // xy: the ground map's corner from the emitter's origin, in x and z;
    // z: one over its side in metres; w: the emitter origin's height.
    float4 Ground;
    // x: the first slot born this frame; y: how many; z: slots; w: the seed.
    uint4 Spawn;
    // x: a number for this frame's births; y: 1 when the ground map is there.
    uint4 Frame;
};

static const float kPi = 3.14159265f;

// PCG's output step: one word well mixed from another.
uint scramble(uint value)
{
    value = value * 747796405u + 2891336453u;
    const uint word = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
    return (word >> 22u) ^ word;
}

// The `index`-th number in [0, 1) of the particle born in `slot` this frame.
float chance(uint slot, uint index)
{
    const uint word = scramble(scramble(slot ^ (Spawn.w * 0x9E3779B9u)) + Frame.x * 0x85EBCA6Bu + index * 0xC2B2AE35u);
    return float(word >> 8u) * (1.0f / 16777216.0f);
}

void born(uint slot, out SimParticle particle)
{
    const float3 up = EmitterUp.xyz;
    const float3 side = EmitterSide.xyz;
    const float3 back = cross(side, up);
    // Uniform over the cap of the cone, as the CPU's are.
    const float cosTheta = 1.0f - chance(slot, 0u) * (1.0f - cos(EmitterSide.w));
    const float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = chance(slot, 1u) * 2.0f * kPi;
    const float3 direction = up * cosTheta + (side * cos(phi) + back * sin(phi)) * sinTheta;
    const float3 local = float3(chance(slot, 2u) - 0.5f, chance(slot, 3u) - 0.5f, chance(slot, 4u) - 0.5f) *
                         EmitterExtent.xyz;
    particle.Position = EmitterPlace.xyz + side * local.x + up * local.y + back * local.z;
    particle.Velocity = direction * EmitterUp.w;
    particle.Age = 0.0f;
    // A tenth either way, so a stream does not pulse.
    particle.Lifetime = EmitterExtent.w * (0.9f + 0.2f * chance(slot, 5u));
    particle.Rotation = Turn.x + Turn.y * (2.0f * chance(slot, 6u) - 1.0f);
    particle.Spin = Turn.z + Turn.w * (2.0f * chance(slot, 7u) - 1.0f);
    particle.Frame = floor(chance(slot, 8u) * max(Touch.z, 1.0f));
    particle.Stuck = 0.0f;
}

// A depth-buffer value as metres along the last camera's view.
float linearDepth(float device)
{
    const bool orthographic = DepthParams.x < 0.0f;
    const float near = orthographic ? -DepthParams.y : DepthParams.x;
    const float far = DepthParams.y;
    return orthographic ? near + device * (far - near) : (near * far) / max(far - device * (far - near), 1e-6f);
}

// Where the picture's surface at `uv` is, from the last camera.
float3 surfaceAt(float2 uv)
{
    const float device = DepthTexture.SampleLevel(DepthSampler, uv, 0.0f);
    const float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, device, 1.0f);
    const float4 world = mul(PrevInverseViewProjection, clip);
    return world.xyz / world.w;
}

// What `particle` meets where it now is: the surface's place for it, a radius
// off, and the surface's normal. False for nothing.
bool meets(SimParticle particle, float radius, float step, out float3 place, out float3 normal)
{
    place = particle.Position;
    normal = float3(0.0f, 1.0f, 0.0f);
    const uint what = uint(Collide.x + 0.5f);

    // The ground, wherever it is.
    if ((what & 2u) != 0u && Frame.y != 0u) {
        const float2 uv = (particle.Position.xz - Ground.xy) * Ground.z;
        if (all(uv > 0.0f) && all(uv < 1.0f)) {
            const float top = GroundTexture.SampleLevel(GroundSampler, uv, 0.0f) - Ground.w;
            if (particle.Position.y - radius < top) {
                const float reach = 1.0f / 128.0f;
                const float metres = reach / Ground.z;
                const float east = GroundTexture.SampleLevel(GroundSampler, uv + float2(reach, 0.0f), 0.0f);
                const float west = GroundTexture.SampleLevel(GroundSampler, uv - float2(reach, 0.0f), 0.0f);
                const float south = GroundTexture.SampleLevel(GroundSampler, uv + float2(0.0f, reach), 0.0f);
                const float north = GroundTexture.SampleLevel(GroundSampler, uv - float2(0.0f, reach), 0.0f);
                normal = normalize(float3(-(east - west) / (2.0f * metres), 1.0f, -(south - north) / (2.0f * metres)));
                place = float3(particle.Position.x, top + radius, particle.Position.z);
                return true;
            }
        }
    }

    // What is drawn: the last frame's depth where the particle is on screen.
    if ((what & 1u) != 0u) {
        const float3 fromCamera = particle.Position + OriginFromCamera.xyz;
        const float4 clip = mul(PrevViewProjection, float4(fromCamera, 1.0f));
        if (clip.w > 1e-4f) {
            const float3 ndc = clip.xyz / clip.w;
            const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
            if (all(uv > 0.0f) && all(uv < 1.0f) && ndc.z > 0.0f && ndc.z < 1.0f) {
                const float device = DepthTexture.SampleLevel(DepthSampler, uv, 0.0f);
                if (device < 1.0f) {
                    const float scene = linearDepth(device);
                    const float own = linearDepth(ndc.z);
                    // Past the surface, and by no more than it could have
                    // come in a frame: what is far behind a wall went round it.
                    const float thick = max(1.0f, length(particle.Velocity) * step * 2.0f + radius);
                    if (own > scene - radius && own < scene + thick) {
                        const float3 here = surfaceAt(uv);
                        const float3 across = surfaceAt(uv + float2(DepthParams.z, 0.0f)) - here;
                        const float3 down = surfaceAt(uv + float2(0.0f, DepthParams.w)) - here;
                        float3 facing = normalize(cross(down, across));
                        // Towards the camera, which is the side it was seen from.
                        if (dot(facing, here) > 0.0f)
                            facing = -facing;
                        normal = facing;
                        place = here + facing * radius - OriginFromCamera.xyz;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

[numthreads(64, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    const uint slot = id.x;
    const uint slots = Spawn.z;
    if (slot >= slots)
        return;

    // This frame's births: `Spawn.y` slots from `Spawn.x`, round the ring.
    if ((slot + slots - Spawn.x) % slots < Spawn.y) {
        SimParticle fresh;
        born(slot, fresh);
        Particles[slot] = fresh;
        return;
    }

    SimParticle particle = Particles[slot];
    if (particle.Lifetime <= 0.0f || particle.Age >= particle.Lifetime)
        return;
    const float step = EmitterPlace.w;
    particle.Age += step;
    if (particle.Stuck > 0.5f) {
        Particles[slot] = particle;
        return;
    }

    const float keep = max(0.0f, 1.0f - Acceleration.w * step);
    particle.Velocity = (particle.Velocity + Acceleration.xyz * step) * keep;
    particle.Position += (particle.Velocity + Wind.xyz) * step;
    particle.Rotation += particle.Spin * step;

    if (Collide.x > 0.5f) {
        float3 place;
        float3 normal;
        if (meets(particle, Touch.x, step, place, normal)) {
            particle.Position = place;
            const uint response = uint(Collide.y + 0.5f);
            if (response == 2u) {
                particle.Age = particle.Lifetime;
            }
            else if (response == 1u) {
                particle.Stuck = 1.0f;
                particle.Velocity = float3(0.0f, 0.0f, 0.0f);
                particle.Spin = 0.0f;
            }
            else {
                const float into = dot(particle.Velocity, normal);
                if (into < 0.0f) {
                    const float3 along = particle.Velocity - normal * into;
                    const float back = -into * saturate(Collide.z);
                    particle.Velocity = along * (1.0f - saturate(Collide.w)) + normal * (back < Touch.y ? 0.0f : back);
                }
            }
        }
    }
    Particles[slot] = particle;
}
