// The views' budget and sizes (ADR 0107), without a device: which camera
// textures a frame draws, at what size, and when a `ViewportFrame`'s picture is
// redrawn. The drawing itself is `views_gate`'s, which needs a GPU.
#include <doctest/doctest.h>
#include <vector>

#include "../../render/generated/class_descriptors.gen.h"
#include "../../scene/generated/class_descriptors.gen.h"
#include "../../ui/generated/class_descriptors.gen.h"
#include "engine/app/view_host.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/enum_registry.h"

using namespace engine;
using app::ViewCandidate;

TEST_CASE("the budget draws four of six views, and the other two next frame")
{
    // Frame 1: six views due, none drawn yet.
    std::vector<ViewCandidate> views(6, ViewCandidate{.due = true});
    const std::vector<core::usize> first = app::chooseViews(views, 4);
    CHECK(first == std::vector<core::usize>{0, 1, 2, 3});

    // Frame 2: the four drawn have a picture from frame 1; the two left are
    // still black, and go first.
    for (const core::usize index : first) {
        views[index].drawn = true;
        views[index].lastDrawn = 1;
    }
    const std::vector<core::usize> second = app::chooseViews(views, 4);
    REQUIRE(second.size() == 4);
    CHECK(second[0] == 4);
    CHECK(second[1] == 5);
}

TEST_CASE("the oldest picture is drawn first")
{
    std::vector<ViewCandidate> views{
        {.due = true, .drawn = true, .lastDrawn = 30},
        {.due = true, .drawn = true, .lastDrawn = 10},
        {.due = true, .drawn = true, .lastDrawn = 20},
        {.due = false, .drawn = true, .lastDrawn = 1},
    };
    CHECK(app::chooseViews(views, 2) == std::vector<core::usize>{1, 2});
    // Not due is not drawn, however old.
    CHECK(app::chooseViews(views, 4) == std::vector<core::usize>{1, 2, 0});
    // A budget of none draws none.
    CHECK(app::chooseViews(views, 0).empty());
}

TEST_CASE("UpdateInterval = 3 draws on one frame in three")
{
    CHECK(app::viewDue(false, 0, 7, 3));
    CHECK_FALSE(app::viewDue(true, 7, 8, 3));
    CHECK_FALSE(app::viewDue(true, 7, 9, 3));
    CHECK(app::viewDue(true, 7, 10, 3));
    // One is every frame, and zero is read as one.
    CHECK(app::viewDue(true, 7, 8, 1));
    CHECK(app::viewDue(true, 7, 8, 0));
}

TEST_CASE("a view's size is capped on its longer side, keeping its shape")
{
    CHECK(app::viewSize(core::Vec2{512.0f, 288.0f}, 1024).x == 512.0f);
    const core::Vec2 capped = app::viewSize(core::Vec2{4096.0f, 2048.0f}, 1024);
    CHECK(capped.x == 1024.0f);
    CHECK(capped.y == 512.0f);
    const core::Vec2 tall = app::viewSize(core::Vec2{300.0f, 3000.0f}, 600);
    CHECK(tall.x == 60.0f);
    CHECK(tall.y == 600.0f);
    // Never below a pixel, whatever it is asked for.
    const core::Vec2 tiny = app::viewSize(core::Vec2{0.0f, -5.0f}, 1024);
    CHECK(tiny.x == 1.0f);
    CHECK(tiny.y == 1.0f);
}

TEST_CASE("a ViewportFrame's picture is redrawn when what is inside it changes, and not otherwise")
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::generated::registerEnums(enums, atoms);
    scene::generated::registerClasses(classes, atoms);
    engine::render::generated::registerClasses(classes, atoms);
    engine::ui::generated::registerClasses(classes, atoms);
    scene::World world(classes, enums, atoms, 7u);

    const core::InstanceId frame = world.create(classes.findId(atoms.intern("ViewportFrame")));
    const core::InstanceId item = world.create(classes.findId(atoms.intern("Part")));
    REQUIRE(frame.valid());
    REQUIRE(item.valid());
    REQUIRE_FALSE(world.setParent(item, frame).has_value());

    // Still: the same number every frame, so forty still items are forty
    // pictures once.
    const core::u64 still = app::frameSignature(world, frame);
    CHECK(app::frameSignature(world, frame) == still);

    // Turned: a new picture.
    world.parts().find(item)->cframe.position.y += 1.0;
    const core::u64 moved = app::frameSignature(world, frame);
    CHECK(moved != still);

    // The frame's own light is part of the picture too.
    world.viewportFrames().find(frame)->lightColor = core::Color3{1.0f, 0.0f, 0.0f};
    CHECK(app::frameSignature(world, frame) != moved);
}

TEST_CASE("a ViewportFrame with nothing inside has nothing to frame")
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::generated::registerEnums(enums, atoms);
    scene::generated::registerClasses(classes, atoms);
    engine::render::generated::registerClasses(classes, atoms);
    engine::ui::generated::registerClasses(classes, atoms);
    scene::World world(classes, enums, atoms, 7u);

    const core::InstanceId frame = world.create(classes.findId(atoms.intern("ViewportFrame")));
    CHECK_FALSE(app::frameLens(world, frame, 1.0f).has_value());

    // With a part inside, a camera in front of it and looking at it.
    const core::InstanceId item = world.create(classes.findId(atoms.intern("Part")));
    REQUIRE_FALSE(world.setParent(item, frame).has_value());
    const std::optional<render::ViewOverride> lens = app::frameLens(world, frame, 1.0f);
    REQUIRE(lens.has_value());
    CHECK(lens->cframe.position.z > 0.0);
    CHECK(lens->cframe.position.y > 0.0);
}
