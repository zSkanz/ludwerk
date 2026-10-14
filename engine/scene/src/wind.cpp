#include "engine/scene/wind.h"

#include <algorithm>
#include <cmath>

#include "engine/core/dmath.h"

namespace engine::scene {
namespace {

// Single precision, as the shader computes it, through the engine's own sine
// so the answer is the same bits on every machine (R10).
[[nodiscard]] core::f32 sine(core::f32 x) noexcept
{
    return static_cast<core::f32>(core::dmath::sin(static_cast<double>(x)));
}

[[nodiscard]] core::f32 cosine(core::f32 x) noexcept
{
    return static_cast<core::f32>(core::dmath::cos(static_cast<double>(x)));
}

} // namespace

core::Vec3 windAt(const WindSettings& wind, core::Vec3 position, core::f32 time) noexcept
{
    const core::f32 speed =
        std::sqrt(wind.global.x * wind.global.x + wind.global.y * wind.global.y + wind.global.z * wind.global.z);
    if (!(speed > 1.0e-4f))
        return core::Vec3{};
    const core::Vec3 direction{wind.global.x / speed, wind.global.y / speed, wind.global.z / speed};

    // Along and across the wind, on the ground plane; a wind straight up or
    // down has no along, and gusts along x.
    const core::f32 flat = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    const core::f32 fx = flat > 1.0e-4f ? direction.x / flat : 1.0f;
    const core::f32 fz = flat > 1.0e-4f ? direction.z / flat : 0.0f;
    const core::f32 along = position.x * fx + position.z * fz - speed * time;
    const core::f32 across = -position.x * fz + position.z * fx;

    // Two travelling waves, one bent by the across position so a gust front is
    // not a straight line; -1 to 1.
    const core::f32 gust =
        sine(along * 0.11f + sine(across * 0.05f) * 1.7f) * 0.6f + sine(along * 0.037f + 1.3f) * 0.4f;
    const core::f32 strength = std::max(0.0f, 1.0f + std::clamp(wind.gusts, 0.0f, 1.0f) * gust);

    // The direction turned about the vertical by a slowly moving field.
    const core::f32 angle = std::clamp(wind.turbulence, 0.0f, 1.0f) * 0.7f *
                            (sine(position.x * 0.21f + time * 1.3f) * cosine(position.z * 0.17f - time * 0.9f));
    const core::f32 c = cosine(angle);
    const core::f32 s = sine(angle);
    const core::Vec3 turned{direction.x * c - direction.z * s, direction.y, direction.x * s + direction.z * c};
    const core::f32 magnitude = speed * strength;
    return core::Vec3{turned.x * magnitude, turned.y * magnitude, turned.z * magnitude};
}

} // namespace engine::scene
