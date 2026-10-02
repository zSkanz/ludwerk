// ADR 0128's layouts and constraints, each against positions worked out by
// hand: a grid, pages, flex in a list, a size limit, a shape, a scale. Layout
// is arithmetic (ADR 0040), so every number here is one a reader can redo.
#include <cmath>
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/scene/ui_pages.h"
#include "engine/scene/world.h"
#include "engine/ui/scene_types.h"
#include "engine/ui/ui.h"

namespace {

namespace core = engine::core;
namespace scene = engine::scene;
namespace ui = engine::ui;

using core::InstanceId;
using core::UDim;
using core::UDim2;
using core::Vec2;

struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    InstanceId service;
    InstanceId screen;

    Fixture()
    {
        scene::generated::registerClasses(classes, atoms);
        ui::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
        service = make("UIService");
        screen = child("ScreenGui", service);
    }

    InstanceId make(const char* className)
    {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != scene::InvalidClass);
        return world->create(id);
    }

    InstanceId child(const char* className, InstanceId parent)
    {
        const InstanceId id = make(className);
        REQUIRE_FALSE(world->setParent(id, parent).has_value());
        return id;
    }

    // A frame of an exact size, in pixels, at an exact place.
    InstanceId frame(InstanceId parent, float w, float h, float x = 0.0f, float y = 0.0f)
    {
        const InstanceId id = child("Frame", parent);
        object(id).size = UDim2{UDim{0.0f, w}, UDim{0.0f, h}};
        object(id).position = UDim2{UDim{0.0f, x}, UDim{0.0f, y}};
        return id;
    }

    [[nodiscard]] scene::UIObjectComponent& object(InstanceId id)
    {
        scene::UIObjectComponent* component = world->uiObjects().find(id);
        REQUIRE(component != nullptr);
        return *component;
    }

    [[nodiscard]] Vec2 at(InstanceId id) { return object(id).absolutePosition; }
    [[nodiscard]] Vec2 size(InstanceId id) { return object(id).absoluteSize; }

    void run(Vec2 windowSize = Vec2{1280.0f, 720.0f})
    {
        world->screenGuis().find(screen)->layoutDirty = true;
        ui::layout(*world, service, windowSize);
    }

    // The events on `subject` that carry an instance, as "Name:index".
    [[nodiscard]] std::vector<std::string> events(InstanceId subject)
    {
        std::vector<std::string> names;
        for (const scene::Change& change : world->changes().take()) {
            if (change.kind == scene::ChangeKind::InstanceEvent && change.subject == subject)
                names.emplace_back(std::string(atoms.text(change.name)) + ":" + std::to_string(change.other.index));
        }
        return names;
    }
};

void checkAt(Fixture& fixture, InstanceId id, double x, double y)
{
    CHECK(fixture.at(id).x == doctest::Approx(x));
    CHECK(fixture.at(id).y == doctest::Approx(y));
}

void checkSize(Fixture& fixture, InstanceId id, double w, double h)
{
    CHECK(fixture.size(id).x == doctest::Approx(w));
    CHECK(fixture.size(id).y == doctest::Approx(h));
}

// `count` frames under `parent`, each `w` by `h`.
[[nodiscard]] std::vector<InstanceId> fill(Fixture& fixture, InstanceId parent, int count, float w, float h)
{
    std::vector<InstanceId> made;
    for (int index = 0; index < count; ++index)
        made.push_back(fixture.frame(parent, w, h));
    return made;
}

} // namespace

// --- UIGridLayout ---------------------------------------------------------------

TEST_CASE("a grid fills a line with as many cells as fit and starts another")
{
    Fixture fixture;
    const InstanceId holder = fixture.frame(fixture.screen, 400.0f, 300.0f);
    const InstanceId grid = fixture.child("UIGridLayout", holder);
    scene::UIGridLayoutComponent& settings = *fixture.world->uiGridLayouts().find(grid);
    settings.cellSize = UDim2{UDim{0.0f, 100.0f}, UDim{0.0f, 100.0f}};
    settings.cellPadding = UDim2{UDim{0.0f, 10.0f}, UDim{0.0f, 10.0f}};
    // Any size at all: a cell is the grid's.
    const std::vector<InstanceId> cells = fill(fixture, holder, 7, 33.0f, 7.0f);
    fixture.run();

    // Three fit in 400 (three cells and two gaps are 320; a fourth needs 430).
    const double expected[7][2] = {{0, 0}, {110, 0}, {220, 0}, {0, 110}, {110, 110}, {220, 110}, {0, 220}};
    for (int index = 0; index < 7; ++index) {
        checkAt(fixture, cells[static_cast<size_t>(index)], expected[index][0], expected[index][1]);
        checkSize(fixture, cells[static_cast<size_t>(index)], 100.0, 100.0);
    }
    CHECK(settings.absoluteContentSize.x == doctest::Approx(320.0));
    CHECK(settings.absoluteContentSize.y == doctest::Approx(320.0));

    SUBCASE("FillDirectionMaxCells caps a line below what fits, and never above")
    {
        settings.fillDirectionMaxCells = 2;
        fixture.run();
        checkAt(fixture, cells[1], 110.0, 0.0);
        checkAt(fixture, cells[2], 0.0, 110.0);
        settings.fillDirectionMaxCells = 9;
        fixture.run();
        checkAt(fixture, cells[3], 0.0, 110.0);
    }

    SUBCASE("the alignments place the whole block")
    {
        settings.horizontalAlignment = 1;
        settings.verticalAlignment = 2;
        fixture.run();
        // 400 - 320 over two across; 300 - 320 down, which is above the top.
        checkAt(fixture, cells[0], 40.0, -20.0);
        checkAt(fixture, cells[6], 40.0, 200.0);
    }

    SUBCASE("StartCorner mirrors the lines")
    {
        // BottomRight: the first cell in the last column of the last row.
        settings.startCorner = 3;
        fixture.run();
        checkAt(fixture, cells[0], 220.0, 220.0);
        checkAt(fixture, cells[1], 110.0, 220.0);
        checkAt(fixture, cells[3], 220.0, 110.0);
        checkAt(fixture, cells[6], 220.0, 0.0);
        // TopRight: right to left, top to bottom.
        settings.startCorner = 1;
        fixture.run();
        checkAt(fixture, cells[0], 220.0, 0.0);
        checkAt(fixture, cells[6], 220.0, 220.0);
    }

    SUBCASE("a vertical fill makes columns")
    {
        settings.fillDirection = 1;
        fixture.run();
        // Two fit down 300; seven cells are four columns.
        checkAt(fixture, cells[0], 0.0, 0.0);
        checkAt(fixture, cells[1], 0.0, 110.0);
        checkAt(fixture, cells[2], 110.0, 0.0);
        checkAt(fixture, cells[6], 330.0, 0.0);
        CHECK(settings.absoluteContentSize.x == doctest::Approx(430.0));
        CHECK(settings.absoluteContentSize.y == doctest::Approx(210.0));
    }

    SUBCASE("a cell in scale is a fraction of the parent")
    {
        settings.cellSize = UDim2{UDim{0.25f, 0.0f}, UDim{0.5f, 0.0f}};
        settings.cellPadding = UDim2{};
        fixture.run();
        checkSize(fixture, cells[0], 100.0, 150.0);
        checkAt(fixture, cells[3], 300.0, 0.0);
        checkAt(fixture, cells[4], 0.0, 150.0);
    }

    SUBCASE("a hidden cell leaves no hole (D478)")
    {
        fixture.object(cells[1]).visible = false;
        fixture.run();
        checkAt(fixture, cells[2], 110.0, 0.0);
        checkAt(fixture, cells[3], 220.0, 0.0);
    }

    SUBCASE("LayoutOrder decides who is first")
    {
        fixture.object(cells[6]).layoutOrder = -1.0f;
        fixture.run();
        checkAt(fixture, cells[6], 0.0, 0.0);
        checkAt(fixture, cells[0], 110.0, 0.0);
    }
}

TEST_CASE("a parent that sizes to its content is as tall as its grid")
{
    Fixture fixture;
    const InstanceId holder = fixture.frame(fixture.screen, 400.0f, 0.0f);
    fixture.object(holder).automaticSize = 2;
    const InstanceId grid = fixture.child("UIGridLayout", holder);
    scene::UIGridLayoutComponent& settings = *fixture.world->uiGridLayouts().find(grid);
    settings.cellPadding = UDim2{UDim{0.0f, 10.0f}, UDim{0.0f, 10.0f}};
    (void)fill(fixture, holder, 7, 1.0f, 1.0f);
    fixture.run();
    checkSize(fixture, holder, 400.0, 320.0);

    // And along the line it has all the room it likes: one row.
    fixture.object(holder).automaticSize = 1;
    fixture.object(holder).size = UDim2{UDim{0.0f, 0.0f}, UDim{0.0f, 100.0f}};
    fixture.run();
    checkSize(fixture, holder, 7.0 * 100.0 + 6.0 * 10.0, 100.0);
}

// --- UIPageLayout ---------------------------------------------------------------

TEST_CASE("pages lie side by side and the current one is shown")
{
    Fixture fixture;
    const InstanceId holder = fixture.frame(fixture.screen, 400.0f, 300.0f);
    const InstanceId layout = fixture.child("UIPageLayout", holder);
    scene::UIPageLayoutComponent& settings = *fixture.world->uiPageLayouts().find(layout);
    std::vector<InstanceId> pages;
    for (int index = 0; index < 3; ++index) {
        const InstanceId page = fixture.child("Frame", holder);
        fixture.object(page).size = UDim2{UDim{1.0f, 0.0f}, UDim{1.0f, 0.0f}};
        pages.push_back(page);
    }
    fixture.run();

    checkAt(fixture, pages[0], 0.0, 0.0);
    checkAt(fixture, pages[1], 400.0, 0.0);
    checkAt(fixture, pages[2], 800.0, 0.0);
    CHECK(settings.currentPage == pages[0]);
    (void)fixture.events(layout);

    SUBCASE("a turn changes CurrentPage at once and slides the pages there")
    {
        CHECK(scene::stepPage(*fixture.world, layout, 1));
        CHECK(settings.currentPage == pages[1]);
        const std::vector<std::string> said = fixture.events(layout);
        REQUIRE(said.size() == 2);
        CHECK(said[0] == "PageLeave:" + std::to_string(pages[0].index));
        CHECK(said[1] == "PageEnter:" + std::to_string(pages[1].index));

        // Half of 0.3 seconds, eased out: 1 - (1 - 0.5)^2 is three quarters of
        // the way.
        ui::advance(*fixture.world, 0.15f);
        fixture.run();
        checkAt(fixture, pages[0], -300.0, 0.0);
        checkAt(fixture, pages[1], 100.0, 0.0);
        CHECK(fixture.events(layout).empty());

        ui::advance(*fixture.world, 0.2f);
        fixture.run();
        checkAt(fixture, pages[1], 0.0, 0.0);
        const std::vector<std::string> ended = fixture.events(layout);
        REQUIRE(ended.size() == 1);
        CHECK(ended[0] == "Stopped:" + std::to_string(pages[1].index));
        // Nothing is sliding: nothing more is said, and nothing moves.
        ui::advance(*fixture.world, 1.0f);
        CHECK(fixture.events(layout).empty());
    }

    SUBCASE("not animated, the page is there at once")
    {
        settings.animated = false;
        CHECK(scene::turnPage(*fixture.world, layout, 2));
        const std::vector<std::string> said = fixture.events(layout);
        REQUIRE(said.size() == 3);
        CHECK(said[2] == "Stopped:" + std::to_string(pages[2].index));
        fixture.run();
        checkAt(fixture, pages[2], 0.0, 0.0);
        checkAt(fixture, pages[0], -800.0, 0.0);
    }

    SUBCASE("past an end is that end, unless the pages go round")
    {
        settings.animated = false;
        CHECK_FALSE(scene::stepPage(*fixture.world, layout, -1));
        CHECK(scene::turnPage(*fixture.world, layout, 99));
        CHECK(settings.currentPage == pages[2]);
        CHECK_FALSE(scene::stepPage(*fixture.world, layout, 1));

        settings.circular = true;
        CHECK(scene::stepPage(*fixture.world, layout, 1));
        CHECK(settings.currentPage == pages[0]);
        CHECK(scene::stepPage(*fixture.world, layout, -1));
        CHECK(settings.currentPage == pages[2]);
        // Round the end, the first page is the next one: to the right.
        fixture.run();
        checkAt(fixture, pages[2], 0.0, 0.0);
        checkAt(fixture, pages[0], 400.0, 0.0);
        checkAt(fixture, pages[1], -400.0, 0.0);
    }

    SUBCASE("Next at the last page of a round slides on, not back")
    {
        settings.circular = true;
        settings.animated = false;
        CHECK(scene::turnPage(*fixture.world, layout, 2));
        settings.animated = true;
        CHECK(scene::stepPage(*fixture.world, layout, 1));
        CHECK(settings.slideTo == doctest::Approx(3.0));
        ui::advance(*fixture.world, 1.0f);
        CHECK(settings.position == doctest::Approx(0.0));
    }

    SUBCASE("a page that is gone is not the current one")
    {
        settings.animated = false;
        CHECK(scene::turnPage(*fixture.world, layout, 2));
        fixture.object(pages[2]).visible = false;
        fixture.run();
        CHECK(settings.currentPage == pages[1]);
        checkAt(fixture, pages[1], 0.0, 0.0);
    }
}

TEST_CASE("a page smaller than its holder sits where the alignments put it")
{
    Fixture fixture;
    const InstanceId holder = fixture.frame(fixture.screen, 400.0f, 300.0f);
    const InstanceId layout = fixture.child("UIPageLayout", holder);
    scene::UIPageLayoutComponent& settings = *fixture.world->uiPageLayouts().find(layout);
    settings.padding = UDim{0.0f, 20.0f};
    const std::vector<InstanceId> pages = fill(fixture, holder, 2, 200.0f, 100.0f);
    fixture.run();
    // Centred: (400 - 200) / 2 across and (300 - 100) / 2 down.
    checkAt(fixture, pages[0], 100.0, 100.0);
    checkAt(fixture, pages[1], 100.0 + 200.0 + 20.0, 100.0);

    settings.horizontalAlignment = 0;
    settings.verticalAlignment = 2;
    fixture.run();
    checkAt(fixture, pages[0], 0.0, 200.0);
}

// --- Flex in UIListLayout -------------------------------------------------------

namespace {

struct Row
{
    Fixture fixture;
    InstanceId holder;
    InstanceId list;
    std::vector<InstanceId> items;

    // A horizontal list of three 50 by 20 children, 10 apart, in 400 by 100:
    // 170 used, 230 left over.
    Row()
    {
        holder = fixture.frame(fixture.screen, 400.0f, 100.0f);
        list = fixture.child("UIListLayout", holder);
        settings().fillDirection = 0;
        settings().padding = UDim{0.0f, 10.0f};
        items = fill(fixture, holder, 3, 50.0f, 20.0f);
    }

    [[nodiscard]] scene::UIListLayoutComponent& settings() { return *fixture.world->listLayouts().find(list); }

    [[nodiscard]] scene::UIFlexItemComponent& flex(size_t index)
    {
        const InstanceId item = fixture.child("UIFlexItem", items[index]);
        return *fixture.world->uiFlexItems().find(item);
    }
};

} // namespace

TEST_CASE("room left over in a list goes where its flex says")
{
    Row row;
    Fixture& fixture = row.fixture;

    SUBCASE("None leaves them where the alignment puts them")
    {
        fixture.run();
        checkAt(fixture, row.items[2], 120.0, 0.0);
        CHECK(row.settings().absoluteContentSize.x == doctest::Approx(170.0));
        CHECK(row.settings().absoluteContentSize.y == doctest::Approx(20.0));
    }

    SUBCASE("Fill grows every child by the same amount")
    {
        row.settings().horizontalFlex = 1;
        fixture.run();
        const double each = 50.0 + 230.0 / 3.0;
        checkSize(fixture, row.items[0], each, 20.0);
        checkAt(fixture, row.items[1], each + 10.0, 0.0);
        checkAt(fixture, row.items[2], 2.0 * (each + 10.0), 0.0);
        // What they asked for, not what they were given.
        CHECK(row.settings().absoluteContentSize.x == doctest::Approx(170.0));
    }

    SUBCASE("SpaceBetween puts it between them and none at the ends")
    {
        row.settings().horizontalFlex = 3;
        fixture.run();
        checkAt(fixture, row.items[0], 0.0, 0.0);
        checkAt(fixture, row.items[1], 175.0, 0.0);
        checkAt(fixture, row.items[2], 350.0, 0.0);
        checkSize(fixture, row.items[1], 50.0, 20.0);
    }

    SUBCASE("SpaceAround gives each a share, half of it on either side")
    {
        row.settings().horizontalFlex = 2;
        fixture.run();
        const double share = 230.0 / 3.0;
        checkAt(fixture, row.items[0], share / 2.0, 0.0);
        checkAt(fixture, row.items[1], 175.0, 0.0);
        checkAt(fixture, row.items[2], 400.0 - 50.0 - share / 2.0, 0.0);
    }

    SUBCASE("SpaceEvenly makes every gap the same, the ends included")
    {
        row.settings().horizontalFlex = 4;
        fixture.run();
        checkAt(fixture, row.items[0], 57.5, 0.0);
        checkAt(fixture, row.items[1], 175.0, 0.0);
        checkAt(fixture, row.items[2], 292.5, 0.0);
    }

    SUBCASE("a child that grows takes all of it")
    {
        row.flex(1).flexMode = 1;
        fixture.run();
        checkAt(fixture, row.items[1], 60.0, 0.0);
        checkSize(fixture, row.items[1], 280.0, 20.0);
        checkAt(fixture, row.items[2], 350.0, 0.0);
    }

    SUBCASE("custom ratios share it out")
    {
        scene::UIFlexItemComponent& first = row.flex(0);
        first.flexMode = 4;
        first.growRatio = 1.0f;
        scene::UIFlexItemComponent& third = row.flex(2);
        third.flexMode = 4;
        third.growRatio = 3.0f;
        fixture.run();
        checkSize(fixture, row.items[0], 107.5, 20.0);
        checkAt(fixture, row.items[1], 117.5, 0.0);
        checkAt(fixture, row.items[2], 177.5, 0.0);
        checkSize(fixture, row.items[2], 222.5, 20.0);
    }
}

TEST_CASE("a line that is too long comes out of the children that shrink")
{
    Row row;
    Fixture& fixture = row.fixture;
    // Three of 200 and two gaps are 620 in 400: 220 short.
    for (const InstanceId item : row.items)
        fixture.object(item).size = UDim2{UDim{0.0f, 200.0f}, UDim{0.0f, 20.0f}};

    fixture.run();
    checkAt(fixture, row.items[2], 420.0, 0.0);

    row.flex(0).flexMode = 2;
    row.flex(1).flexMode = 2;
    fixture.run();
    checkSize(fixture, row.items[0], 90.0, 20.0);
    checkSize(fixture, row.items[1], 90.0, 20.0);
    checkSize(fixture, row.items[2], 200.0, 20.0);
    checkAt(fixture, row.items[2], 200.0, 0.0);
}

TEST_CASE("ItemLineAlignment places a child across its line")
{
    Row row;
    Fixture& fixture = row.fixture;

    row.settings().itemLineAlignment = 2;
    fixture.run();
    checkAt(fixture, row.items[0], 0.0, 40.0);

    row.settings().itemLineAlignment = 4;
    fixture.run();
    checkAt(fixture, row.items[0], 0.0, 0.0);
    checkSize(fixture, row.items[0], 50.0, 100.0);

    // One child says otherwise.
    row.flex(1).itemLineAlignment = 3;
    fixture.run();
    checkAt(fixture, row.items[1], 60.0, 80.0);
    checkSize(fixture, row.items[1], 50.0, 20.0);
    checkSize(fixture, row.items[2], 50.0, 100.0);
}

TEST_CASE("across the lines of a wrapped list, flex shares what the lines leave")
{
    Fixture fixture;
    const InstanceId holder = fixture.frame(fixture.screen, 100.0f, 200.0f);
    const InstanceId list = fixture.child("UIListLayout", holder);
    scene::UIListLayoutComponent& settings = *fixture.world->listLayouts().find(list);
    settings.fillDirection = 0;
    settings.padding = UDim{0.0f, 10.0f};
    settings.wraps = true;
    // Two of 60 do not fit in 100: three lines of 20, 80 with their gaps, 120 over.
    const std::vector<InstanceId> items = fill(fixture, holder, 3, 60.0f, 20.0f);

    fixture.run();
    checkAt(fixture, items[2], 0.0, 60.0);

    settings.verticalFlex = 3;
    fixture.run();
    checkAt(fixture, items[1], 0.0, 90.0);
    checkAt(fixture, items[2], 0.0, 180.0);

    // `Fill` makes each line 40 deeper; a child fills its line when it is
    // told to stretch.
    settings.verticalFlex = 1;
    settings.itemLineAlignment = 4;
    fixture.run();
    checkAt(fixture, items[1], 0.0, 70.0);
    checkSize(fixture, items[1], 60.0, 60.0);
    checkAt(fixture, items[2], 0.0, 140.0);
}

TEST_CASE("a hidden child of a list keeps no room (D478)")
{
    Row row;
    Fixture& fixture = row.fixture;
    fixture.object(row.items[1]).visible = false;
    fixture.run();
    checkAt(fixture, row.items[2], 60.0, 0.0);

    // And a parent that sizes to its content is not as large as what is hidden.
    fixture.object(row.holder).automaticSize = 1;
    fixture.run();
    checkSize(fixture, row.holder, 110.0, 100.0);
}

// --- Constraints ----------------------------------------------------------------

TEST_CASE("a size constraint clamps, and zero is no maximum")
{
    Fixture fixture;
    const InstanceId panel = fixture.child("Frame", fixture.screen);
    fixture.object(panel).size = UDim2{UDim{0.5f, 0.0f}, UDim{0.5f, 0.0f}};
    const InstanceId limit = fixture.child("UISizeConstraint", panel);
    scene::UISizeConstraintComponent& settings = *fixture.world->uiSizeConstraints().find(limit);

    // A new one changes nothing.
    fixture.run();
    checkSize(fixture, panel, 640.0, 360.0);

    settings.maxSize = Vec2{300.0f, 0.0f};
    fixture.run();
    checkSize(fixture, panel, 300.0, 360.0);

    settings.minSize = Vec2{0.0f, 400.0f};
    fixture.run();
    checkSize(fixture, panel, 300.0, 400.0);

    // The floor wins over the ceiling.
    settings.minSize = Vec2{500.0f, 400.0f};
    fixture.run();
    checkSize(fixture, panel, 500.0, 400.0);

    // Its children are laid out in the clamped box.
    const InstanceId inside = fixture.child("Frame", panel);
    fixture.object(inside).size = UDim2{UDim{0.5f, 0.0f}, UDim{0.5f, 0.0f}};
    fixture.run();
    checkSize(fixture, inside, 250.0, 200.0);
}

TEST_CASE("an aspect ratio constraint keeps a shape")
{
    Fixture fixture;
    const InstanceId panel = fixture.child("Frame", fixture.screen);
    fixture.object(panel).size = UDim2{UDim{0.5f, 0.0f}, UDim{0.5f, 0.0f}};
    const InstanceId shape = fixture.child("UIAspectRatioConstraint", panel);
    scene::UIAspectRatioConstraintComponent& settings = *fixture.world->uiAspectRatioConstraints().find(shape);

    SUBCASE("FitWithinMaxSize is the largest box of the shape inside its own")
    {
        // 640 by 360, as a square: the height is the short side.
        fixture.run();
        checkSize(fixture, panel, 360.0, 360.0);
        settings.aspectRatio = 2.0f;
        fixture.run();
        checkSize(fixture, panel, 640.0, 320.0);
        settings.aspectRatio = 0.5f;
        fixture.run();
        checkSize(fixture, panel, 180.0, 360.0);
    }

    SUBCASE("ScaleWithParentSize takes the parent's extent on the dominant axis")
    {
        settings.aspectType = 1;
        settings.aspectRatio = 4.0f;
        fixture.run();
        checkSize(fixture, panel, 1280.0, 320.0);
        settings.dominantAxis = 1;
        settings.aspectRatio = 0.5f;
        fixture.run();
        checkSize(fixture, panel, 360.0, 720.0);
    }

    SUBCASE("the anchor places the box it ends up with")
    {
        fixture.object(panel).anchorPoint = Vec2{0.5f, 0.5f};
        fixture.object(panel).position = UDim2{UDim{0.5f, 0.0f}, UDim{0.5f, 0.0f}};
        fixture.run();
        checkAt(fixture, panel, 640.0 - 180.0, 360.0 - 180.0);
    }

    SUBCASE("a size limit has the last word")
    {
        const InstanceId limit = fixture.child("UISizeConstraint", panel);
        fixture.world->uiSizeConstraints().find(limit)->minSize = Vec2{500.0f, 0.0f};
        fixture.run();
        checkSize(fixture, panel, 500.0, 360.0);
    }
}

TEST_CASE("a UIScale scales its element and everything in it")
{
    Fixture fixture;
    const InstanceId panel = fixture.frame(fixture.screen, 100.0f, 50.0f, 10.0f, 20.0f);
    const InstanceId inside = fixture.frame(panel, 20.0f, 10.0f, 5.0f, 5.0f);
    const InstanceId deeper = fixture.frame(inside, 4.0f, 4.0f, 1.0f, 1.0f);
    const InstanceId scale = fixture.child("UIScale", panel);
    fixture.world->uiScales().find(scale)->scale = 2.0f;
    fixture.run();

    checkAt(fixture, panel, 10.0, 20.0);
    checkSize(fixture, panel, 200.0, 100.0);
    checkAt(fixture, inside, 20.0, 30.0);
    checkSize(fixture, inside, 40.0, 20.0);
    checkAt(fixture, deeper, 22.0, 32.0);
    checkSize(fixture, deeper, 8.0, 8.0);
    // What is measured in units and read at drawing time is told.
    CHECK(fixture.object(panel).unitScale == doctest::Approx(2.0));
    CHECK(fixture.object(deeper).unitScale == doctest::Approx(2.0));

    SUBCASE("a fraction of the parent inside it is a fraction of the scaled box")
    {
        fixture.object(inside).size = UDim2{UDim{0.5f, 0.0f}, UDim{1.0f, 0.0f}};
        fixture.object(inside).position = UDim2{};
        fixture.run();
        checkSize(fixture, inside, 100.0, 100.0);
    }

    SUBCASE("the anchor places the scaled box")
    {
        fixture.object(panel).anchorPoint = Vec2{0.5f, 0.5f};
        fixture.object(panel).position = UDim2{UDim{0.0f, 300.0f}, UDim{0.0f, 200.0f}};
        fixture.run();
        checkAt(fixture, panel, 200.0, 150.0);
        checkAt(fixture, inside, 210.0, 160.0);
    }

    SUBCASE("a scale inside a scale multiplies")
    {
        const InstanceId inner = fixture.child("UIScale", inside);
        fixture.world->uiScales().find(inner)->scale = 0.5f;
        fixture.run();
        // 20 by 10 at half is 10 by 5 in the panel's units, 20 by 10 in pixels.
        checkSize(fixture, inside, 20.0, 10.0);
        checkSize(fixture, deeper, 4.0, 4.0);
        checkAt(fixture, deeper, 21.0, 31.0);
        CHECK(fixture.object(deeper).unitScale == doctest::Approx(1.0));
    }

    SUBCASE("a scaled screen multiplies it again")
    {
        fixture.world->screenGuis().find(fixture.screen)->referenceHeight = 360.0f;
        fixture.run();
        checkAt(fixture, panel, 20.0, 40.0);
        checkSize(fixture, panel, 400.0, 200.0);
        CHECK(fixture.object(inside).unitScale == doctest::Approx(4.0));
    }
}

TEST_CASE("a list makes room for a scaled child")
{
    Row row;
    Fixture& fixture = row.fixture;
    const InstanceId scale = fixture.child("UIScale", row.items[0]);
    fixture.world->uiScales().find(scale)->scale = 3.0f;
    fixture.run();
    checkSize(fixture, row.items[0], 150.0, 60.0);
    checkAt(fixture, row.items[1], 160.0, 0.0);
}

// --- A ScrollFrame's canvas (D479) ----------------------------------------------

TEST_CASE("D479: a canvas axis left at zero is as wide as the frame, not nothing")
{
    Fixture fixture;
    const InstanceId list = fixture.child("ScrollFrame", fixture.screen);
    fixture.object(list).size = UDim2{UDim{0.0f, 200.0f}, UDim{0.0f, 100.0f}};
    // It scrolls down and not across: the usual list.
    fixture.world->scrollFrames().find(list)->canvasSize = UDim2{UDim{}, UDim{0.0f, 400.0f}};
    const InstanceId row = fixture.child("Frame", list);
    fixture.object(row).size = UDim2{UDim{1.0f, 0.0f}, UDim{0.5f, 0.0f}};
    fixture.run();

    // As wide as the frame, and half as tall as the canvas.
    checkSize(fixture, row, 200.0, 200.0);

    // A grid in it has the frame's width to fill.
    fixture.object(row).visible = false;
    const InstanceId grid = fixture.child("UIGridLayout", list);
    fixture.world->uiGridLayouts().find(grid)->cellPadding = UDim2{};
    const std::vector<InstanceId> cells = fill(fixture, list, 3, 1.0f, 1.0f);
    fixture.run();
    checkAt(fixture, cells[1], 100.0, 0.0);
    checkAt(fixture, cells[2], 0.0, 100.0);

    // A canvas smaller than the frame is the frame: there is nothing to scroll.
    fixture.world->scrollFrames().find(list)->canvasSize = UDim2{UDim{0.0f, 50.0f}, UDim{0.0f, 20.0f}};
    fixture.run();
    checkAt(fixture, cells[1], 100.0, 0.0);
}
