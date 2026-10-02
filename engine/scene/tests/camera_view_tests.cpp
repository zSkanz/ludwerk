// Between the world and the picture (D436).
#include <cmath>
#include <doctest/doctest.h>
#include <optional>

#include "engine/scene/camera_view.h"

using namespace engine;
using scene::CameraComponent;

namespace {

constexpr core::Vec2 Screen{1280.0f, 720.0f};

[[nodiscard]] double of(float value)
{
    return static_cast<double>(value);
}

} // namespace

TEST_CASE("D436: a point in front of a perspective camera lands where the picture draws it, and back")
{
    // At the origin, looking down -Z, a 90 degree field: at ten metres the
    // picture is twenty metres tall and 35.6 wide.
    CameraComponent camera;
    camera.fieldOfView = 90.0f;
    const core::CFrameD frame{};

    const scene::ViewportPoint middle = scene::worldToViewport(camera, frame, Screen, core::DVec3{0.0, 0.0, -10.0});
    CHECK(of(middle.pixel.x) == doctest::Approx(640.0));
    CHECK(of(middle.pixel.y) == doctest::Approx(360.0));
    CHECK(middle.depth == doctest::Approx(10.0));
    CHECK(middle.onScreen);

    // Up in the world is up the screen, which is a smaller y; right is right.
    const scene::ViewportPoint top = scene::worldToViewport(camera, frame, Screen, core::DVec3{0.0, 10.0, -10.0});
    CHECK(of(top.pixel.y) == doctest::Approx(0.0).epsilon(0.001));
    const scene::ViewportPoint right = scene::worldToViewport(camera, frame, Screen, core::DVec3{5.0, 0.0, -10.0});
    CHECK(of(right.pixel.x) > 640.0);
    CHECK(right.onScreen);

    // Past the edge is still a place -- an arrow points at it -- and is not
    // on the screen; behind the camera is nowhere.
    const scene::ViewportPoint beside = scene::worldToViewport(camera, frame, Screen, core::DVec3{40.0, 0.0, -10.0});
    CHECK(of(beside.pixel.x) > 1280.0);
    CHECK_FALSE(beside.onScreen);
    const scene::ViewportPoint behind = scene::worldToViewport(camera, frame, Screen, core::DVec3{0.0, 0.0, 10.0});
    CHECK_FALSE(behind.onScreen);
    CHECK(behind.depth < 0.0);

    // And back: the ray through that pixel passes through the point.
    const core::DVec3 point{3.0, -2.0, -12.0};
    const scene::ViewportPoint seen = scene::worldToViewport(camera, frame, Screen, point);
    const scene::ViewRay ray = scene::viewportToRay(camera, frame, Screen, seen.pixel);
    const double along = -12.0 / of(ray.direction.z);
    CHECK(ray.origin.x + of(ray.direction.x) * along == doctest::Approx(3.0).epsilon(0.001));
    CHECK(ray.origin.y + of(ray.direction.y) * along == doctest::Approx(-2.0).epsilon(0.001));
    const double length =
        std::sqrt(of(ray.direction.x) * of(ray.direction.x) + of(ray.direction.y) * of(ray.direction.y) +
                  of(ray.direction.z) * of(ray.direction.z));
    CHECK(length == doctest::Approx(1.0));
}

TEST_CASE("D436: a camera that is somewhere, turned, answers in the world's terms")
{
    CameraComponent camera;
    camera.fieldOfView = 90.0f;
    core::CFrameD frame;
    frame.position = core::DVec3{100.0, 5.0, 50.0};
    // Turned a quarter about Y: it looks along -X.
    frame.rotation = core::rotationY(1.5707964f);

    const scene::ViewportPoint ahead = scene::worldToViewport(camera, frame, Screen, core::DVec3{90.0, 5.0, 50.0});
    CHECK(of(ahead.pixel.x) == doctest::Approx(640.0).epsilon(0.001));
    CHECK(of(ahead.pixel.y) == doctest::Approx(360.0).epsilon(0.001));
    CHECK(ahead.depth == doctest::Approx(10.0).epsilon(0.001));
    const scene::ViewRay ray = scene::viewportToRay(camera, frame, Screen, core::Vec2{640.0f, 360.0f});
    CHECK(of(ray.direction.x) == doctest::Approx(-1.0).epsilon(0.001));
    CHECK(ray.origin.x == doctest::Approx(100.0));
}

TEST_CASE("D436: the 2D layer's camera turns a tap into a place on the plane, and a place into a pixel")
{
    // Orthographic, ten metres above and below the middle, fifty metres in
    // front of the plane, over (20, 4).
    CameraComponent camera;
    camera.projection = 1;
    camera.orthographicSize = 10.0f;
    core::CFrameD frame;
    frame.position = core::DVec3{20.0, 4.0, 50.0};

    // The middle of the screen is what the camera is over.
    const std::optional<core::Vec2> middle = scene::viewportToPlane(camera, frame, Screen, core::Vec2{640.0f, 360.0f});
    REQUIRE(middle.has_value());
    CHECK(of(middle->x) == doctest::Approx(20.0));
    CHECK(of(middle->y) == doctest::Approx(4.0));
    // The top-left corner: ten up, and ten times the aspect to the left.
    const std::optional<core::Vec2> corner = scene::viewportToPlane(camera, frame, Screen, core::Vec2{0.0f, 0.0f});
    REQUIRE(corner.has_value());
    CHECK(of(corner->x) == doctest::Approx(20.0 - 10.0 * 1280.0 / 720.0));
    CHECK(of(corner->y) == doctest::Approx(14.0));

    // And back: a monster at (25, 9) is drawn where a tap on it lands.
    const scene::ViewportPoint monster = scene::worldToViewport(camera, frame, Screen, core::DVec3{25.0, 9.0, 0.0});
    CHECK(monster.onScreen);
    const std::optional<core::Vec2> tapped = scene::viewportToPlane(camera, frame, Screen, monster.pixel);
    REQUIRE(tapped.has_value());
    CHECK(of(tapped->x) == doctest::Approx(25.0));
    CHECK(of(tapped->y) == doctest::Approx(9.0));

    // A perspective camera finds the plane too, where it looks at it; one
    // looking along the plane finds none.
    CameraComponent lens;
    lens.fieldOfView = 90.0f;
    const std::optional<core::Vec2> under = scene::viewportToPlane(lens, frame, Screen, core::Vec2{640.0f, 360.0f});
    REQUIRE(under.has_value());
    CHECK(of(under->x) == doctest::Approx(20.0));
    core::CFrameD sideways;
    sideways.position = core::DVec3{0.0, 0.0, 5.0};
    sideways.rotation = core::rotationY(1.5707964f);
    CHECK_FALSE(scene::viewportToPlane(lens, sideways, Screen, core::Vec2{640.0f, 360.0f}).has_value());
}
