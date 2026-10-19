#ifndef ENG_SH_HLSLI
#define ENG_SH_HLSLI

// The sky's light as nine spherical-harmonic coefficients, read back in a
// direction: the surfaces' (`engine_brdf.hlsli`) and a particle's in the
// vertex stage (`particle.hlsl`), which has none of the frame's uniforms.

// Irradiance from nine spherical-harmonic coefficients. They arrive
// cosine-convolved and already divided by pi (environment.h), so the result
// multiplies straight by a diffuse albedo -- there is no further 1/pi here and
// adding one is the classic way an SH ambient ends up a third too dark.
float3 evaluateIrradiance(float4 coefficients[9], float3 n)
{
    float3 result = coefficients[0].rgb * 0.282095f;
    result += coefficients[1].rgb * (0.488603f * n.y);
    result += coefficients[2].rgb * (0.488603f * n.z);
    result += coefficients[3].rgb * (0.488603f * n.x);
    result += coefficients[4].rgb * (1.092548f * n.x * n.y);
    result += coefficients[5].rgb * (1.092548f * n.y * n.z);
    result += coefficients[6].rgb * (0.315392f * (3.0f * n.z * n.z - 1.0f));
    result += coefficients[7].rgb * (1.092548f * n.x * n.z);
    result += coefficients[8].rgb * (0.546274f * (n.x * n.x - n.y * n.y));
    // An SH reconstruction can ring below zero where the source has a sharp
    // feature -- and the sun's disc is exactly that. Negative irradiance is not
    // a dim surface, it is a black hole in an otherwise lit wall.
    return max(result, float3(0.0f, 0.0f, 0.0f));
}

#endif // ENG_SH_HLSLI
