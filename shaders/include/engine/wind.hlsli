// **The wind** (ADR 0115), line for line `scene::windAt`: the workspace's
// `GlobalWind`, `WindGusts` and `WindTurbulence`, a world position and the
// simulation's seconds. A surface shader reads it as `SurfaceInputs.Wind`;
// anything else of the engine's that sways includes this.

#ifndef ENG_WIND_HLSLI
#define ENG_WIND_HLSLI

float3 engineWindAt(float3 globalWind, float gusts, float turbulence, float3 position, float time)
{
    const float speed = length(globalWind);
    if (!(speed > 1.0e-4f))
        return float3(0.0f, 0.0f, 0.0f);
    const float3 direction = globalWind / speed;

    const float flat = length(direction.xz);
    const float fx = flat > 1.0e-4f ? direction.x / flat : 1.0f;
    const float fz = flat > 1.0e-4f ? direction.z / flat : 0.0f;
    const float along = position.x * fx + position.z * fz - speed * time;
    const float across = -position.x * fz + position.z * fx;

    const float gust = sin(along * 0.11f + sin(across * 0.05f) * 1.7f) * 0.6f + sin(along * 0.037f + 1.3f) * 0.4f;
    const float strength = max(0.0f, 1.0f + saturate(gusts) * gust);

    const float angle = saturate(turbulence) * 0.7f * (sin(position.x * 0.21f + time * 1.3f) *
                                                        cos(position.z * 0.17f - time * 0.9f));
    const float c = cos(angle);
    const float s = sin(angle);
    const float3 turned = float3(direction.x * c - direction.z * s, direction.y, direction.x * s + direction.z * c);
    return turned * (speed * strength);
}

#endif // ENG_WIND_HLSLI
