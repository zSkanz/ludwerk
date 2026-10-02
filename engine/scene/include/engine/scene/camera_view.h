// Between the world and the picture, for a camera (D436).
//
// **Every game needs both directions** -- a click on a monster, a name over a
// head, a number that floats up -- and `Camera` had `ViewportSize` and nothing
// that used it: each game wrote the projection by hand, for one projection.
// These are the camera's own arithmetic, perspective or orthographic, the same
// the picture is drawn with (`render::extract`): vertical field of view, the
// aspect of what is drawn into, looking down -Z.
//
// A place on the screen is in pixels from the top-left of what the world is
// drawn into, which is the unit `UIObject.AbsolutePosition` and
// `InputService:GetPointerPosition` use.
#pragma once

#include <optional>

#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/scene/components.h"

namespace engine::scene {

struct ViewportPoint
{
    // Where the point is on the screen, in pixels. Meaningful past the edges
    // too: an arrow at the screen's border points at it.
    core::Vec2 pixel;
    // How far in front of the camera, along where it looks, in metres; zero
    // or less is beside or behind it, where `pixel` means nothing.
    core::f64 depth = 0.0;
    // In front of the near plane, short of the far one, and inside the picture.
    bool onScreen = false;
};

[[nodiscard]] ViewportPoint worldToViewport(const CameraComponent& camera, const core::CFrameD& cameraFrame,
                                            core::Vec2 viewport, core::DVec3 point) noexcept;

struct ViewRay
{
    core::DVec3 origin;
    // Of length one.
    core::Vec3 direction{0.0f, 0.0f, -1.0f};
};

// The ray a pixel looks along: from the camera through the pixel for a
// perspective one, and straight ahead from the pixel's own place for an
// orthographic one.
[[nodiscard]] ViewRay viewportToRay(const CameraComponent& camera, const core::CFrameD& cameraFrame,
                                    core::Vec2 viewport, core::Vec2 pixel) noexcept;

// Where a pixel is on the 2D plane (z = 0), or nothing when the camera looks
// along the plane or away from it.
[[nodiscard]] std::optional<core::Vec2> viewportToPlane(const CameraComponent& camera, const core::CFrameD& cameraFrame,
                                                        core::Vec2 viewport, core::Vec2 pixel) noexcept;

} // namespace engine::scene
