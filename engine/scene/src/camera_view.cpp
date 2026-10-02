#include "engine/scene/camera_view.h"

#include <cmath>

namespace engine::scene {
namespace {

using core::f32;
using core::f64;

constexpr f64 DegreesToRadians = 3.14159265358979323846 / 180.0;

// The camera's own axes in the world: its right, its up, and where it looks
// (the negated third column, `math.h`).
struct Axes
{
    core::DVec3 right;
    core::DVec3 up;
    core::DVec3 forward;
};

[[nodiscard]] Axes axesOf(const core::CFrameD& frame) noexcept
{
    const core::Mat3& m = frame.rotation;
    return Axes{
        core::DVec3{static_cast<f64>(m.m[0][0]), static_cast<f64>(m.m[0][1]), static_cast<f64>(m.m[0][2])},
        core::DVec3{static_cast<f64>(m.m[1][0]), static_cast<f64>(m.m[1][1]), static_cast<f64>(m.m[1][2])},
        core::DVec3{-static_cast<f64>(m.m[2][0]), -static_cast<f64>(m.m[2][1]), -static_cast<f64>(m.m[2][2])},
    };
}

[[nodiscard]] f64 dot(const core::DVec3& a, const core::DVec3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Half the picture's height and width, in metres at one metre's distance for
// a perspective camera and at every distance for an orthographic one.
struct Extent
{
    f64 halfWidth = 1.0;
    f64 halfHeight = 1.0;
    bool orthographic = false;
};

[[nodiscard]] Extent extentOf(const CameraComponent& camera, core::Vec2 viewport) noexcept
{
    const f64 aspect =
        viewport.x > 0.0f && viewport.y > 0.0f ? static_cast<f64>(viewport.x) / static_cast<f64>(viewport.y) : 1.0;
    Extent extent;
    extent.orthographic = camera.projection == 1;
    extent.halfHeight = extent.orthographic ? static_cast<f64>(camera.orthographicSize)
                                            : std::tan(static_cast<f64>(camera.fieldOfView) * DegreesToRadians * 0.5);
    extent.halfWidth = extent.halfHeight * aspect;
    return extent;
}

} // namespace

ViewportPoint worldToViewport(const CameraComponent& camera, const core::CFrameD& cameraFrame, core::Vec2 viewport,
                              core::DVec3 point) noexcept
{
    const Axes axes = axesOf(cameraFrame);
    const Extent extent = extentOf(camera, viewport);
    const core::DVec3 from{point.x - cameraFrame.position.x, point.y - cameraFrame.position.y,
                           point.z - cameraFrame.position.z};
    ViewportPoint out;
    out.depth = dot(from, axes.forward);
    // An orthographic camera sees its whole column, behind where it stands as
    // well as in front; a perspective one sees nothing at or behind its eye.
    if (!extent.orthographic && !(out.depth > 0.0))
        return out;
    const f64 scale = extent.orthographic ? 1.0 : out.depth;
    const f64 x = dot(from, axes.right) / (extent.halfWidth * scale);
    const f64 y = dot(from, axes.up) / (extent.halfHeight * scale);
    out.pixel = core::Vec2{static_cast<f32>((x * 0.5 + 0.5) * static_cast<f64>(viewport.x)),
                           static_cast<f32>((0.5 - y * 0.5) * static_cast<f64>(viewport.y))};
    const f64 far = static_cast<f64>(camera.farPlane);
    const bool inDepth = extent.orthographic ? (out.depth >= -far && out.depth <= far)
                                             : (out.depth >= static_cast<f64>(camera.nearPlane) && out.depth <= far);
    out.onScreen = inDepth && x >= -1.0 && x <= 1.0 && y >= -1.0 && y <= 1.0;
    return out;
}

ViewRay viewportToRay(const CameraComponent& camera, const core::CFrameD& cameraFrame, core::Vec2 viewport,
                      core::Vec2 pixel) noexcept
{
    const Axes axes = axesOf(cameraFrame);
    const Extent extent = extentOf(camera, viewport);
    const f64 x = viewport.x > 0.0f ? static_cast<f64>(pixel.x) / static_cast<f64>(viewport.x) * 2.0 - 1.0 : 0.0;
    const f64 y = viewport.y > 0.0f ? 1.0 - static_cast<f64>(pixel.y) / static_cast<f64>(viewport.y) * 2.0 : 0.0;
    const f64 across = x * extent.halfWidth;
    const f64 upward = y * extent.halfHeight;

    ViewRay ray;
    if (extent.orthographic) {
        ray.origin = core::DVec3{cameraFrame.position.x + axes.right.x * across + axes.up.x * upward,
                                 cameraFrame.position.y + axes.right.y * across + axes.up.y * upward,
                                 cameraFrame.position.z + axes.right.z * across + axes.up.z * upward};
        ray.direction = core::Vec3{static_cast<f32>(axes.forward.x), static_cast<f32>(axes.forward.y),
                                   static_cast<f32>(axes.forward.z)};
        return ray;
    }
    ray.origin = cameraFrame.position;
    const core::DVec3 towards{axes.forward.x + axes.right.x * across + axes.up.x * upward,
                              axes.forward.y + axes.right.y * across + axes.up.y * upward,
                              axes.forward.z + axes.right.z * across + axes.up.z * upward};
    const f64 length = std::sqrt(dot(towards, towards));
    ray.direction = core::Vec3{static_cast<f32>(towards.x / length), static_cast<f32>(towards.y / length),
                               static_cast<f32>(towards.z / length)};
    return ray;
}

std::optional<core::Vec2> viewportToPlane(const CameraComponent& camera, const core::CFrameD& cameraFrame,
                                          core::Vec2 viewport, core::Vec2 pixel) noexcept
{
    const ViewRay ray = viewportToRay(camera, cameraFrame, viewport, pixel);
    const f64 along = static_cast<f64>(ray.direction.z);
    if (std::abs(along) < 1.0e-9)
        return std::nullopt;
    const f64 distance = -ray.origin.z / along;
    // An orthographic camera sees the plane from either side of where it
    // stands; a perspective one only in front of its eye.
    if (camera.projection != 1 && distance < 0.0)
        return std::nullopt;
    return core::Vec2{static_cast<f32>(ray.origin.x + static_cast<f64>(ray.direction.x) * distance),
                      static_cast<f32>(ray.origin.y + static_cast<f64>(ray.direction.y) * distance)};
}

} // namespace engine::scene
