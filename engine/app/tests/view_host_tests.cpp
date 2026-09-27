// The views' budget and sizes (ADR 0107), without a device: which camera
// textures a frame draws, and at what size. The drawing itself is
// `views_gate`'s, which needs a GPU.
#include <doctest/doctest.h>
#include <vector>

#include "engine/app/view_host.h"

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
