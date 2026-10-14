// The window shared between the players at one machine (ADR 0205): who has a
// view, and where in the window it is.
#include <doctest/doctest.h>
#include <optional>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/render/scene_types.h"
#include "engine/scene/players.h"
#include "engine/scene/screen_split.h"
#include "engine/scene/world.h"

using namespace engine;

namespace {

[[nodiscard]] double of(float value)
{
    return static_cast<double>(value);
}

struct Sofa
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> made;
    core::InstanceId workspace;
    core::InstanceId network;
    std::vector<core::InstanceId> players;

    Sofa()
    {
        scene::generated::registerClasses(classes, atoms);
        render::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        made.emplace(classes, enums, atoms, 1u);
        scene::World& world = *made;
        workspace = world.create(world.classes().findId(world.atoms().intern("Workspace")));
        network = world.create(world.classes().findId(world.atoms().intern("NetworkService")));
    }

    // A local player; with a camera of its own when `seeing`.
    core::InstanceId seat(bool seeing = true)
    {
        scene::World& world = *made;
        const core::InstanceId player =
            scene::createPlayer(world, network, 1 + static_cast<core::u32>(players.size()), true);
        if (seeing) {
            const core::InstanceId camera = world.create(world.classes().findId(world.atoms().intern("Camera")));
            world.players().find(player)->camera = camera;
        }
        players.push_back(player);
        return player;
    }

    scene::WorkspaceComponent& space() { return *made->workspaces().find(workspace); }
};

} // namespace

TEST_CASE("ADR 0205: the rectangles of two, three and four views meet on whole pixels and leave none out")
{
    // A width three does not divide.
    const auto columns = scene::screenSplitRects(1, 3, 1280, 720);
    CHECK(of(columns[0].min.x) == 0.0);
    CHECK(of(columns[0].max.x) == of(columns[1].min.x));
    CHECK(of(columns[1].max.x) == of(columns[2].min.x));
    CHECK(of(columns[2].max.x) == 1280.0);
    CHECK(of(columns[1].max.y) == 720.0);

    const auto rows = scene::screenSplitRects(2, 2, 1280, 721);
    CHECK(of(rows[0].max.y) == 360.0);
    CHECK(of(rows[1].min.y) == 360.0);
    CHECK(of(rows[1].max.y) == 721.0);
    CHECK(of(rows[1].max.x) == 1280.0);

    // Two in a grid are side by side; three and four are the quarters, and
    // three leave the last to nobody.
    const auto pair = scene::screenSplitRects(3, 2, 1920, 1080);
    CHECK(of(pair[0].max.x) == 960.0);
    CHECK(of(pair[0].max.y) == 1080.0);
    const auto three = scene::screenSplitRects(3, 3, 1920, 1080);
    CHECK(of(three[1].min.x) == 960.0);
    CHECK(of(three[1].max.y) == 540.0);
    CHECK(of(three[2].min.y) == 540.0);
    CHECK(of(three[2].max.x) == 960.0);
    CHECK(of(three[3].max.x) == 0.0);
    const auto four = scene::screenSplitRects(3, 4, 1920, 1080);
    CHECK(of(four[3].min.x) == 960.0);
    CHECK(of(four[3].min.y) == 540.0);
    CHECK(of(four[3].max.x) == 1920.0);
    CHECK(of(four[3].max.y) == 1080.0);
}

TEST_CASE("ADR 0205: every local player with a camera has a view, in the order they joined")
{
    Sofa sofa;
    const core::InstanceId first = sofa.seat();
    (void)sofa.seat(false);
    const core::InstanceId third = sofa.seat();

    // No split asked for: one picture, through the workspace's camera.
    scene::ScreenSplitFrame frame = scene::resolveScreenSplit((*sofa.made), sofa.workspace, 1920, 1080);
    CHECK(frame.form == scene::ScreenForm::One);
    CHECK(frame.count == 0);
    CHECK_FALSE(frame.camera.valid());

    sofa.space().screenSplit = 1;
    frame = scene::resolveScreenSplit((*sofa.made), sofa.workspace, 1920, 1080);
    REQUIRE(frame.form == scene::ScreenForm::Rects);
    REQUIRE(frame.count == 2);
    // The one with no camera has none, and the third player has the second.
    CHECK(frame.views[0].player == first);
    CHECK(frame.views[1].player == third);
    CHECK(of(frame.views[1].rect.min.x) == 960.0);
    // The line is as thick to the eye at any size of window.
    CHECK(of(frame.lineThickness) == doctest::Approx(4.0));
    CHECK(of(scene::resolveScreenSplit((*sofa.made), sofa.workspace, 960, 540).lineThickness) == doctest::Approx(2.0));

    // A camera destroyed is a view gone, and one view is the window through
    // that player's camera.
    const core::InstanceId kept = (*sofa.made).players().find(first)->camera;
    (void)(*sofa.made).destroy((*sofa.made).players().find(third)->camera);
    frame = scene::resolveScreenSplit((*sofa.made), sofa.workspace, 1920, 1080);
    CHECK(frame.form == scene::ScreenForm::One);
    CHECK(frame.camera == kept);
}

TEST_CASE("ADR 0205: a dynamic split is one picture together, two across a line apart, and a grid for more")
{
    Sofa sofa;
    const core::InstanceId first = sofa.seat();
    const core::InstanceId second = sofa.seat();
    sofa.space().screenSplit = 4;

    // Together: one picture, through the camera that frames them.
    scene::ScreenSplitFrame frame = scene::resolveScreenSplit((*sofa.made), sofa.workspace, 1920, 1080);
    CHECK(frame.form == scene::ScreenForm::One);
    CHECK_FALSE(frame.camera.valid());

    // Apart, the second to the right of the first: the line is upright, the
    // first player's view is the left of the window and the second's the
    // right, each the size of the window.
    sofa.space().splitBlend = 0.5f;
    frame = scene::resolveScreenSplit((*sofa.made), sofa.workspace, 1920, 1080);
    REQUIRE(frame.form == scene::ScreenForm::Line);
    REQUIRE(frame.count == 2);
    CHECK(of(frame.views[0].rect.max.x) == 1920.0);
    CHECK(of(frame.views[0].sideNormal.x) == doctest::Approx(1.0));
    CHECK(of(frame.views[1].sideNormal.x) == doctest::Approx(-1.0));
    // Half way apart, half the line.
    CHECK(of(frame.lineThickness) == doctest::Approx(2.0));
    const scene::ScreenView* left = scene::screenViewAt(frame.shown(), core::Vec2{200.0f, 500.0f});
    const scene::ScreenView* right = scene::screenViewAt(frame.shown(), core::Vec2{1700.0f, 500.0f});
    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);
    CHECK(left->player == first);
    CHECK(right->player == second);

    // The second ABOVE the first: the line lies flat, and the first has the
    // bottom of the window.
    sofa.space().splitAngle = 90.0f;
    frame = scene::resolveScreenSplit((*sofa.made), sofa.workspace, 1920, 1080);
    const scene::ScreenView* bottom = scene::screenViewAt(frame.shown(), core::Vec2{960.0f, 900.0f});
    const scene::ScreenView* top = scene::screenViewAt(frame.shown(), core::Vec2{960.0f, 100.0f});
    REQUIRE(bottom != nullptr);
    REQUIRE(top != nullptr);
    CHECK(bottom->player == first);
    CHECK(top->player == second);

    // Three: the grid, faded in over the one picture on the way apart.
    (void)sofa.seat();
    sofa.space().splitBlend = 0.25f;
    frame = scene::resolveScreenSplit((*sofa.made), sofa.workspace, 1920, 1080);
    REQUIRE(frame.form == scene::ScreenForm::Rects);
    CHECK(frame.count == 3);
    CHECK(of(frame.wholeWeight) == doctest::Approx(0.75));
    CHECK(of(frame.views[2].rect.min.y) == 540.0);
    sofa.space().splitBlend = 1.0f;
    CHECK(of(scene::resolveScreenSplit((*sofa.made), sofa.workspace, 1920, 1080).wholeWeight) == 0.0);
    // And a point of the quarter nobody has is in nobody's view.
    CHECK(scene::screenViewAt(frame.shown(), core::Vec2{1500.0f, 800.0f}) == nullptr);
}
