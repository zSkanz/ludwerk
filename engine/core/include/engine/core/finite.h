// **One rule for a number that reaches the simulation** (audit E3): finite, or
// refused where it comes in. A NaN reaching the solver is not a wrong number, it
// is a body that leaves the world and takes its contact island with it; an
// infinity is a shape that cannot be built. The same test serves a script's
// write, a binding's argument and a peer's message -- what differs is only what
// happens to a value that fails it: a script is told, a peer's value is dropped.
#pragma once

#include <algorithm>
#include <cmath>

#include "engine/core/math.h"

namespace engine::core {

// Past these a value is a mistake or an attack, not a world: a coordinate a
// thousand times the size of any map the engine streams, a speed no simulated
// thing reaches.
inline constexpr f64 MaxWorldCoordinate = 1e9;
inline constexpr f32 MaxSpeed = 1e5f;

[[nodiscard]] inline bool isFinite(f32 value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] inline bool isFinite(f64 value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] inline bool isFinite(Vec2 v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y);
}

[[nodiscard]] inline bool isFinite(Vec3 v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] inline bool isFinite(DVec3 v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] inline bool isFinite(const Mat3& m) noexcept
{
    for (const auto& row : m.m) {
        for (const f32 element : row) {
            if (!std::isfinite(element))
                return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool isFinite(const CFrameD& frame) noexcept
{
    return isFinite(frame.position) && isFinite(frame.rotation);
}

// A rotation from outside: finite, and orthonormal with a positive determinant
// to within `tolerance` -- float drift passes, a scale, a shear or a mirror
// does not. The solver takes a rotation as a rotation, and a skewed one is a
// body whose inertia means nothing.
[[nodiscard]] inline bool isRotation(const Mat3& r, f32 tolerance = 1e-2f) noexcept
{
    if (!isFinite(r))
        return false;
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            f32 dot = 0.0f;
            for (int k = 0; k < 3; ++k)
                dot += r.m[a][k] * r.m[b][k];
            if (std::abs(dot - (a == b ? 1.0f : 0.0f)) > tolerance)
                return false;
        }
    }
    const f32 determinant = r.m[0][0] * (r.m[1][1] * r.m[2][2] - r.m[1][2] * r.m[2][1]) -
                            r.m[0][1] * (r.m[1][0] * r.m[2][2] - r.m[1][2] * r.m[2][0]) +
                            r.m[0][2] * (r.m[1][0] * r.m[2][1] - r.m[1][1] * r.m[2][0]);
    return determinant > 0.0f;
}

// A position a world can hold: finite and within `MaxWorldCoordinate`.
[[nodiscard]] inline bool isWorldPosition(DVec3 v) noexcept
{
    return isFinite(v) && std::abs(v.x) <= MaxWorldCoordinate && std::abs(v.y) <= MaxWorldCoordinate &&
           std::abs(v.z) <= MaxWorldCoordinate;
}

// A value from outside made safe: a non-finite component becomes 0, and every
// component is held within `limit`.
[[nodiscard]] inline f32 sanitize(f32 value, f32 limit) noexcept
{
    return std::isfinite(value) ? std::clamp(value, -limit, limit) : 0.0f;
}

[[nodiscard]] inline Vec3 sanitize(Vec3 v, f32 limit) noexcept
{
    return Vec3{sanitize(v.x, limit), sanitize(v.y, limit), sanitize(v.z, limit)};
}

} // namespace engine::core
