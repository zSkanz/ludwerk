#ifndef ENG_TONEMAP_HLSLI
#define ENG_TONEMAP_HLSLI

// The exposure's key, the curve and the dither: the tonemap pass's, and a
// headset eye's forward pass's where the tonemap is folded into it
// (`foldOutput`). One place, so the two cannot drift apart.

// What the frame's average luminance is mapped onto: the exposure "key".
//
// **Photography's answer is 0.18, and 0.18 is wrong here**, for a reason worth
// stating rather than fudging: 0.18 is middle grey in PHOTOMETRIC units, and
// this engine's radiance unit is arbitrary. `Lighting.Brightness` defaults to
// 2.6 because 2.6 looked right, not because it is 2.6 of anything. The constant
// that maps an arbitrary unit onto display grey is therefore a CALIBRATION, and
// calibrating it against 0.18 would be borrowing a number from a scale nothing
// here is on.
//
// 0.45 is that calibration, and the method was: every scene in the repository
// was authored against the fixed exposure of 1.0 this pass replaced, so the key
// is chosen to leave those scenes at the level they were authored at. That makes
// automatic exposure a STABILISER -- it holds a scene's level as its light
// changes -- rather than a change of level applied to all existing content.
//
// The day the engine gains photometric lights, this becomes 0.18 and every
// scene's brightness becomes a number in lux.
static const float EngineExposureKey = 0.45f;

// Khronos PBR Neutral, linear Rec.709 in, [0, 1] linear Rec.709 out. Ported
// unchanged from the reference implementation so a difference against the glTF
// viewer is a bug in our lighting rather than in our curve.
float3 tonemapPbrNeutral(float3 color)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;

    // Lifts the darkest channel just enough to keep a near-black from taking on
    // a colour cast when the other two channels are compressed.
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
    {
        return color;
    }

    // A hyperbola on the brightest channel alone, with the other two scaled by
    // the same ratio: that is what keeps the hue where the artist put it.
    const float d = 1.0f - startCompression;
    const float newPeak = 1.0f - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    const float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return lerp(color, float3(newPeak, newPeak, newPeak), g);
}

// **One 8-bit step of noise, before the picture is rounded to 8 bits.** A glow
// fading into a dark sky crosses a handful of the 256 levels over hundreds of
// pixels, and each crossing is a contour ring the eye finds at once; half a
// step either way, different at each pixel, turns the ring into grain nobody
// sees. From the pixel's place and never from time, so a still picture is the
// same picture every frame -- and a golden is a golden.
float3 ditherOutput(float3 encoded, float2 pixel)
{
    // Interleaved gradient noise (Jimenez, 2014): even over any small block.
    const float noise = frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
    return encoded + (noise - 0.5f) / 255.0f;
}

// **The curve undone**, for a meter that reads a picture already tonemapped
// (a headset eye's with the tonemap folded in): the compression's hyperbola
// solved for the peak it came from, the hue's scale kept and its desaturation
// not -- the meter wants a luminance. Held below 0.995 of white, past which
// the peak runs to infinity: a very bright scene reads somewhat dimmer than
// it is, and the exposure follows it a little more slowly.
float3 untonemapPbrNeutral(float3 mapped)
{
    const float startCompression = 0.8f - 0.04f;
    const float d = 1.0f - startCompression;
    const float peak = max(mapped.r, max(mapped.g, mapped.b));
    float3 color = mapped;
    if (peak >= startCompression)
    {
        const float held = min(peak, 0.995f);
        const float original = d * d / (1.0f - held) - d + startCompression;
        color *= original / max(peak, 1e-4f);
    }
    // The toe solved as well: below 0.08 the curve took `x - 6.25x^2` off
    // the darkest channel `x`, leaving `6.25x^2` of it, so a mapped darkest
    // channel `m` under 0.04 came from `0.4 sqrt(m)`. A flat 0.04 put back
    // there read a near-black as several times brighter than it was.
    const float darkest = max(min(color.r, min(color.g, color.b)), 0.0f);
    const float offset = darkest < 0.04f ? 0.4f * sqrt(darkest) - darkest : 0.04f;
    return color + offset;
}

// **What a forward pass with the tonemap folded into it writes** (a headset's
// eye, ADR 0250; a lean window): `exposure` nought is the 16-bit target's
// linear HDR as it always was; otherwise the colour exposed, through the
// curve and dithered as the tonemap pass dithers. **Positive**, decoded again
// -- linear, into an 8-bit sRGB image whose hardware encodes it (a runtime's
// eye). **Negative**, left encoded -- into a plain 8-bit image, which a
// window's swapchain is.
float4 foldOutput(float4 color, float exposure, float2 pixel)
{
    if (exposure == 0.0f)
        return color;
    const float3 exposed = max(color.rgb * abs(exposure), float3(0.0f, 0.0f, 0.0f));
    const float3 shown = ditherOutput(encodeSrgb(tonemapPbrNeutral(exposed)), pixel);
    return float4(exposure > 0.0f ? decodeSrgb(shown) : shown, color.a);
}

#endif // ENG_TONEMAP_HLSLI
