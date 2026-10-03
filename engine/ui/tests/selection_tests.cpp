// ADR 0128's hands: a selection moved by a gamepad or the arrow keys, an
// element dragged by a pointer, pages turned by a swipe, and a list scrolled by
// the wheel and by a finger (D477) -- thrown, pulled past its end, dragged by
// its bar, nested in another and scrolled to a selection (G40).
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
        // Interaction's memory is the process's.
        ui::resetInteraction();
        service = make("UIService");
        screen = child("ScreenGui", service);
        // What the host says the interface is drawn into.
        world->engineState().viewportSize = Vec2{800.0f, 600.0f};
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

    // An element of `className` at an exact place.
    InstanceId box(const char* className, InstanceId parent, float x, float y, float w, float h)
    {
        const InstanceId id = child(className, parent);
        object(id).position = UDim2{UDim{0.0f, x}, UDim{0.0f, y}};
        object(id).size = UDim2{UDim{0.0f, w}, UDim{0.0f, h}};
        return id;
    }

    [[nodiscard]] scene::UIObjectComponent& object(InstanceId id)
    {
        scene::UIObjectComponent* component = world->uiObjects().find(id);
        REQUIRE(component != nullptr);
        return *component;
    }

    [[nodiscard]] InstanceId& selected() { return world->engineState().uiSelected; }

    void layout()
    {
        world->screenGuis().find(screen)->layoutDirty = true;
        ui::layout(*world, service, Vec2{800.0f, 600.0f});
    }

    // One frame: laid out, then told `input`.
    ui::InteractionResult send(const ui::InteractionInput& input)
    {
        layout();
        return ui::updateInteraction(*world, service, input);
    }

    ui::InteractionResult navigate(int x, int y)
    {
        ui::InteractionInput input;
        input.pointer = Vec2{-100.0f, -100.0f};
        input.navigateX = static_cast<core::i8>(x);
        input.navigateY = static_cast<core::i8>(y);
        return send(input);
    }

    // The pointer at `at`: going down, held, or coming up.
    ui::InteractionResult press(Vec2 at)
    {
        ui::InteractionInput input;
        input.pointer = at;
        input.pressed = true;
        input.pointerHeld = true;
        return send(input);
    }

    ui::InteractionResult hold(Vec2 at)
    {
        ui::InteractionInput input;
        input.pointer = at;
        input.pointerHeld = true;
        return send(input);
    }

    ui::InteractionResult release(Vec2 at)
    {
        ui::InteractionInput input;
        input.pointer = at;
        input.released = true;
        return send(input);
    }

    // The pointer at `at` at `time` seconds: what a fling is measured from.
    ui::InteractionResult touch(Vec2 at, double time, bool pressed, bool held)
    {
        ui::InteractionInput input;
        input.pointer = at;
        input.pressed = pressed;
        input.pointerHeld = held;
        input.released = !pressed && !held;
        input.time = time;
        return send(input);
    }

    ui::InteractionResult wheel(Vec2 at, float notches)
    {
        ui::InteractionInput input;
        input.pointer = at;
        input.wheel = Vec2{0.0f, notches};
        return send(input);
    }

    // Every event since the last call, by name; one that carries a point says
    // where.
    [[nodiscard]] std::vector<std::string> events()
    {
        std::vector<std::string> names;
        for (const scene::Change& change : world->changes().take()) {
            if (change.kind == scene::ChangeKind::InstanceEventNoArgs ||
                change.kind == scene::ChangeKind::InstanceEvent)
                names.emplace_back(atoms.text(change.name));
            if (change.kind == scene::ChangeKind::InstanceEventVector2) {
                const Vec2 point = scene::eventPoint(change.other);
                names.emplace_back(std::string(atoms.text(change.name)) + "@" +
                                   std::to_string(static_cast<int>(point.x)) + "," +
                                   std::to_string(static_cast<int>(point.y)));
            }
        }
        return names;
    }

    [[nodiscard]] bool said(const std::vector<std::string>& names, std::string_view name) const
    {
        for (const std::string& each : names) {
            if (each == name)
                return true;
        }
        return false;
    }
};

// Four buttons: two across the top, one under the first, one down and to the
// right of everything.
//
//   A(100,100)        B(300,100)
//   C(100,200)
//                     D(300,220)
struct Menu
{
    Fixture fixture;
    InstanceId a, b, c, d;

    Menu()
    {
        a = fixture.box("TextButton", fixture.screen, 100.0f, 100.0f, 100.0f, 40.0f);
        b = fixture.box("TextButton", fixture.screen, 300.0f, 100.0f, 100.0f, 40.0f);
        c = fixture.box("TextButton", fixture.screen, 100.0f, 200.0f, 100.0f, 40.0f);
        d = fixture.box("TextButton", fixture.screen, 300.0f, 220.0f, 100.0f, 40.0f);
    }
};

} // namespace

// --- Selection ------------------------------------------------------------------

TEST_CASE("a button and a text input are selectable, and nothing else until it is told")
{
    Fixture fixture;
    const InstanceId button = fixture.box("TextButton", fixture.screen, 0.0f, 0.0f, 10.0f, 10.0f);
    const InstanceId field = fixture.box("TextInput", fixture.screen, 0.0f, 0.0f, 10.0f, 10.0f);
    const InstanceId frame = fixture.box("Frame", fixture.screen, 0.0f, 0.0f, 10.0f, 10.0f);
    const InstanceId label = fixture.box("TextLabel", fixture.screen, 0.0f, 0.0f, 10.0f, 10.0f);
    CHECK(ui::isSelectable(*fixture.world, button));
    CHECK(ui::isSelectable(*fixture.world, field));
    CHECK_FALSE(ui::isSelectable(*fixture.world, frame));
    CHECK_FALSE(ui::isSelectable(*fixture.world, label));

    fixture.object(frame).selectable = 1;
    fixture.object(button).selectable = 0;
    CHECK(ui::isSelectable(*fixture.world, frame));
    CHECK_FALSE(ui::isSelectable(*fixture.world, button));
}

TEST_CASE("the first step of a d-pad selects the first selectable object")
{
    Menu menu;
    Fixture& fixture = menu.fixture;
    CHECK_FALSE(fixture.navigate(0, 0).selectionActive);
    CHECK_FALSE(fixture.selected().valid());

    const ui::InteractionResult result = fixture.navigate(1, 0);
    CHECK(fixture.selected() == menu.a);
    CHECK(result.selectionActive);
    const std::vector<std::string> said = fixture.events();
    CHECK(fixture.said(said, "SelectionGained"));
    CHECK(fixture.said(said, "SelectionChanged"));

    SUBCASE("and with AutoSelect off it selects nothing")
    {
        fixture.selected() = {};
        fixture.world->engineState().uiAutoSelect = false;
        (void)fixture.navigate(1, 0);
        CHECK_FALSE(fixture.selected().valid());
    }
}

TEST_CASE("the selection moves to the nearest selectable object in each direction")
{
    Menu menu;
    Fixture& fixture = menu.fixture;
    fixture.selected() = menu.a;

    (void)fixture.navigate(1, 0);
    CHECK(fixture.selected() == menu.b);
    // Straight down from B is D; C is nearer but mostly to the side.
    (void)fixture.navigate(0, 1);
    CHECK(fixture.selected() == menu.d);
    (void)fixture.navigate(-1, 0);
    CHECK(fixture.selected() == menu.c);
    (void)fixture.navigate(0, -1);
    CHECK(fixture.selected() == menu.a);
    (void)fixture.navigate(0, 1);
    CHECK(fixture.selected() == menu.c);

    // Nothing that way: it stays.
    (void)fixture.navigate(-1, 0);
    CHECK(fixture.selected() == menu.c);
    // The only thing below at all, however far to the side, is still below.
    (void)fixture.navigate(0, 1);
    CHECK(fixture.selected() == menu.d);
    (void)fixture.navigate(0, 1);
    CHECK(fixture.selected() == menu.d);

    SUBCASE("what is hidden or not selectable is passed over")
    {
        fixture.selected() = menu.a;
        fixture.object(menu.b).visible = false;
        (void)fixture.navigate(1, 0);
        CHECK(fixture.selected() == menu.d);
        fixture.selected() = menu.a;
        fixture.object(menu.d).selectable = 0;
        (void)fixture.navigate(1, 0);
        CHECK(fixture.selected() == menu.a);
    }
}

TEST_CASE("NextSelection says where the selection goes, and nearest is only the default")
{
    Menu menu;
    Fixture& fixture = menu.fixture;
    fixture.selected() = menu.a;
    fixture.object(menu.a).nextSelectionRight = menu.d;
    (void)fixture.navigate(1, 0);
    CHECK(fixture.selected() == menu.d);

    // Somewhere a selection cannot be falls back to the nearest.
    fixture.selected() = menu.a;
    fixture.object(menu.d).visible = false;
    (void)fixture.navigate(1, 0);
    CHECK(fixture.selected() == menu.b);
}

TEST_CASE("the activating button fires Activated on what is selected")
{
    Menu menu;
    Fixture& fixture = menu.fixture;
    fixture.selected() = menu.b;
    (void)fixture.navigate(0, 0);
    (void)fixture.events();

    ui::InteractionInput input;
    input.pointer = Vec2{-100.0f, -100.0f};
    input.navigateActivate = true;
    (void)fixture.send(input);
    std::vector<std::string> said;
    for (const scene::Change& change : fixture.world->changes().take()) {
        if (change.kind == scene::ChangeKind::InstanceEventNoArgs && change.subject == menu.b)
            said.emplace_back(fixture.atoms.text(change.name));
    }
    REQUIRE(said.size() == 1);
    CHECK(said[0] == "Activated");
}

TEST_CASE("a selection is announced when it changes, whoever changed it")
{
    Menu menu;
    Fixture& fixture = menu.fixture;
    // A script's write: the next frame says so.
    fixture.selected() = menu.c;
    (void)fixture.navigate(0, 0);
    std::vector<std::string> said = fixture.events();
    CHECK(fixture.said(said, "SelectionGained"));
    CHECK(fixture.said(said, "SelectionChanged"));
    CHECK_FALSE(fixture.said(said, "SelectionLost"));

    // And nothing while it stays.
    (void)fixture.navigate(0, 0);
    CHECK(fixture.events().empty());

    // What is selected goes away: the selection goes with it.
    fixture.object(menu.c).visible = false;
    (void)fixture.navigate(0, 0);
    CHECK_FALSE(fixture.selected().valid());
    said = fixture.events();
    CHECK(fixture.said(said, "SelectionLost"));
    CHECK(fixture.said(said, "SelectionChanged"));
}

TEST_CASE("a press of the pointer clears a selection the engine manages, and not the game's own")
{
    Menu menu;
    Fixture& fixture = menu.fixture;
    fixture.selected() = menu.a;
    (void)fixture.press(Vec2{700.0f, 500.0f});
    CHECK_FALSE(fixture.selected().valid());

    fixture.world->engineState().uiAutoSelect = false;
    fixture.selected() = menu.a;
    (void)fixture.press(Vec2{700.0f, 500.0f});
    CHECK(fixture.selected() == menu.a);
}

TEST_CASE("the arrows are a text field's while one is being typed into")
{
    Menu menu;
    Fixture& fixture = menu.fixture;
    const InstanceId field = fixture.box("TextInput", fixture.screen, 500.0f, 400.0f, 200.0f, 30.0f);
    (void)fixture.press(Vec2{510.0f, 410.0f});
    (void)fixture.release(Vec2{510.0f, 410.0f});
    CHECK(fixture.world->textInputs().find(field)->focused);

    fixture.selected() = menu.a;
    (void)fixture.navigate(1, 0);
    CHECK(fixture.selected() == menu.a);
}

// --- UIDragDetector -------------------------------------------------------------

TEST_CASE("a drag detector makes its parent follow the pointer")
{
    Fixture fixture;
    const InstanceId window = fixture.box("TextButton", fixture.screen, 100.0f, 100.0f, 100.0f, 100.0f);
    const InstanceId detector = fixture.child("UIDragDetector", window);
    scene::UIDragDetectorComponent& settings = *fixture.world->uiDragDetectors().find(detector);

    SUBCASE("on both axes, and a press that became a drag is not a press")
    {
        (void)fixture.press(Vec2{110.0f, 110.0f});
        (void)fixture.hold(Vec2{160.0f, 140.0f});
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(150.0));
        CHECK(fixture.object(window).position.y.offset == doctest::Approx(130.0));
        CHECK(settings.dragUDim2.x.offset == doctest::Approx(50.0));
        (void)fixture.hold(Vec2{120.0f, 190.0f});
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(110.0));
        CHECK(fixture.object(window).position.y.offset == doctest::Approx(180.0));
        (void)fixture.release(Vec2{120.0f, 190.0f});

        const std::vector<std::string> said = fixture.events();
        CHECK(fixture.said(said, "DragStart@110,110"));
        CHECK(fixture.said(said, "DragContinue@160,140"));
        CHECK(fixture.said(said, "DragEnd@120,190"));
        CHECK_FALSE(fixture.said(said, "Activated"));
    }

    SUBCASE("a press that does not move is still a press")
    {
        (void)fixture.press(Vec2{110.0f, 110.0f});
        (void)fixture.hold(Vec2{111.0f, 111.0f});
        (void)fixture.release(Vec2{111.0f, 111.0f});
        const std::vector<std::string> said = fixture.events();
        CHECK(fixture.said(said, "Activated"));
        CHECK_FALSE(fixture.said(said, "DragStart@110,110"));
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(100.0));
    }

    SUBCASE("along a line, the element slides on that line alone")
    {
        settings.dragStyle = 1;
        settings.dragAxis = Vec2{1.0f, 0.0f};
        (void)fixture.press(Vec2{110.0f, 110.0f});
        (void)fixture.hold(Vec2{170.0f, 190.0f});
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(160.0));
        CHECK(fixture.object(window).position.y.offset == doctest::Approx(100.0));
    }

    SUBCASE("the translation limits hold it")
    {
        settings.minDragTranslation = UDim2{UDim{0.0f, -10.0f}, UDim{}};
        settings.maxDragTranslation = UDim2{UDim{0.0f, 30.0f}, UDim{}};
        (void)fixture.press(Vec2{110.0f, 110.0f});
        (void)fixture.hold(Vec2{310.0f, 150.0f});
        // Across it is clamped; down there is no limit, the two being equal.
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(130.0));
        CHECK(fixture.object(window).position.y.offset == doctest::Approx(140.0));
        (void)fixture.hold(Vec2{10.0f, 150.0f});
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(90.0));
    }

    SUBCASE("as a scale, it is written as a fraction of the parent")
    {
        settings.responseStyle = 1;
        (void)fixture.press(Vec2{110.0f, 110.0f});
        (void)fixture.hold(Vec2{190.0f, 170.0f});
        // 80 of 800 and 60 of 600.
        CHECK(fixture.object(window).position.x.scale == doctest::Approx(0.1));
        CHECK(fixture.object(window).position.y.scale == doctest::Approx(0.1));
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(100.0));
    }

    SUBCASE("custom responses move nothing and say how far")
    {
        settings.responseStyle = 2;
        (void)fixture.press(Vec2{110.0f, 110.0f});
        (void)fixture.hold(Vec2{160.0f, 140.0f});
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(100.0));
        CHECK(settings.dragUDim2.x.offset == doctest::Approx(50.0));
        CHECK(settings.dragUDim2.y.offset == doctest::Approx(30.0));
    }

    SUBCASE("Scriptable only says where the pointer is")
    {
        settings.dragStyle = 3;
        (void)fixture.press(Vec2{110.0f, 110.0f});
        (void)fixture.hold(Vec2{160.0f, 140.0f});
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(100.0));
        CHECK(fixture.said(fixture.events(), "DragContinue@160,140"));
    }

    SUBCASE("a detector that is off drags nothing")
    {
        settings.enabled = false;
        (void)fixture.press(Vec2{110.0f, 110.0f});
        (void)fixture.hold(Vec2{160.0f, 140.0f});
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(100.0));
    }

    SUBCASE("turning it, the element follows the pointer round its middle")
    {
        settings.dragStyle = 2;
        // The middle is (150, 150): from due right of it to due below is a
        // quarter turn clockwise.
        (void)fixture.press(Vec2{190.0f, 150.0f});
        (void)fixture.hold(Vec2{150.0f, 190.0f});
        CHECK(fixture.object(window).rotation == doctest::Approx(90.0));
        CHECK(settings.dragRotation == doctest::Approx(90.0));
        CHECK(fixture.object(window).position.x.offset == doctest::Approx(100.0));

        settings.minDragAngle = -45.0f;
        settings.maxDragAngle = 120.0f;
        (void)fixture.hold(Vec2{110.0f, 150.0f});
        CHECK(fixture.object(window).rotation == doctest::Approx(120.0));
    }
}

TEST_CASE("a dragged element stays inside its BoundingUI")
{
    Fixture fixture;
    const InstanceId track = fixture.box("Frame", fixture.screen, 100.0f, 100.0f, 200.0f, 40.0f);
    const InstanceId thumb = fixture.box("TextButton", track, 0.0f, 0.0f, 50.0f, 40.0f);
    const InstanceId detector = fixture.child("UIDragDetector", thumb);
    fixture.world->uiDragDetectors().find(detector)->boundingUI = track;

    (void)fixture.press(Vec2{110.0f, 110.0f});
    (void)fixture.hold(Vec2{500.0f, 300.0f});
    // The track is 200 wide and the thumb 50: 150 is as far as it goes, and it
    // has nowhere to go down.
    CHECK(fixture.object(thumb).position.x.offset == doctest::Approx(150.0));
    CHECK(fixture.object(thumb).position.y.offset == doctest::Approx(0.0));
    (void)fixture.hold(Vec2{0.0f, 0.0f});
    CHECK(fixture.object(thumb).position.x.offset == doctest::Approx(0.0));
}

TEST_CASE("a press on something inside a dragged element drags the element")
{
    Fixture fixture;
    const InstanceId window = fixture.box("Frame", fixture.screen, 100.0f, 100.0f, 200.0f, 200.0f);
    (void)fixture.child("UIDragDetector", window);
    const InstanceId title = fixture.box("TextButton", window, 0.0f, 0.0f, 200.0f, 30.0f);
    (void)title;

    (void)fixture.press(Vec2{150.0f, 110.0f});
    (void)fixture.hold(Vec2{170.0f, 150.0f});
    CHECK(fixture.object(window).position.x.offset == doctest::Approx(120.0));
    CHECK(fixture.object(window).position.y.offset == doctest::Approx(140.0));
}

// --- A ScrollFrame, by hand (D477) ----------------------------------------------

TEST_CASE("D477: the wheel over a scroll frame moves its canvas")
{
    Fixture fixture;
    const InstanceId list = fixture.box("ScrollFrame", fixture.screen, 0.0f, 0.0f, 200.0f, 100.0f);
    scene::ScrollFrameComponent& scroll = *fixture.world->scrollFrames().find(list);
    scroll.canvasSize = UDim2{UDim{}, UDim{0.0f, 400.0f}};
    const InstanceId row = fixture.box("TextButton", list, 0.0f, 0.0f, 200.0f, 40.0f);
    (void)row;

    // Towards the hand is down the list: three lines a notch.
    CHECK(fixture.wheel(Vec2{50.0f, 50.0f}, -1.0f).wheelTaken);
    CHECK(scroll.canvasPosition.y == doctest::Approx(48.0));
    (void)fixture.wheel(Vec2{50.0f, 50.0f}, -2.0f);
    CHECK(scroll.canvasPosition.y == doctest::Approx(144.0));
    // Never past the end: 400 of canvas in 100 of view is 300 of travel.
    (void)fixture.wheel(Vec2{50.0f, 50.0f}, -10.0f);
    CHECK(scroll.canvasPosition.y == doctest::Approx(300.0));
    // At the end the wheel is nobody's, so what is behind the list may have it.
    CHECK_FALSE(fixture.wheel(Vec2{50.0f, 50.0f}, -1.0f).wheelTaken);
    (void)fixture.wheel(Vec2{50.0f, 50.0f}, 1.0f);
    CHECK(scroll.canvasPosition.y == doctest::Approx(252.0));

    // And nothing when the pointer is somewhere else.
    CHECK_FALSE(fixture.wheel(Vec2{500.0f, 500.0f}, 1.0f).wheelTaken);
    CHECK(scroll.canvasPosition.y == doctest::Approx(252.0));
}

TEST_CASE("D477: a list dragged by a finger scrolls, and the row under the finger is not pressed")
{
    Fixture fixture;
    const InstanceId list = fixture.box("ScrollFrame", fixture.screen, 0.0f, 0.0f, 200.0f, 100.0f);
    scene::ScrollFrameComponent& scroll = *fixture.world->scrollFrames().find(list);
    scroll.canvasSize = UDim2{UDim{}, UDim{0.0f, 400.0f}};
    (void)fixture.box("TextButton", list, 0.0f, 0.0f, 200.0f, 400.0f);

    (void)fixture.press(Vec2{50.0f, 80.0f});
    (void)fixture.hold(Vec2{50.0f, 50.0f});
    // The content follows the finger up, so the canvas goes down by as much.
    CHECK(scroll.canvasPosition.y == doctest::Approx(30.0));
    (void)fixture.hold(Vec2{50.0f, 20.0f});
    CHECK(scroll.canvasPosition.y == doctest::Approx(60.0));
    (void)fixture.release(Vec2{50.0f, 20.0f});
    CHECK_FALSE(fixture.said(fixture.events(), "Activated"));

    SUBCASE("a list with nowhere to scroll still presses its rows")
    {
        scroll.canvasSize = UDim2{};
        scroll.canvasPosition = Vec2{};
        (void)fixture.press(Vec2{50.0f, 80.0f});
        (void)fixture.hold(Vec2{50.0f, 50.0f});
        (void)fixture.release(Vec2{50.0f, 50.0f});
        CHECK(fixture.said(fixture.events(), "Activated"));
    }
}

// --- A ScrollFrame, as a phone's list (G40) ---------------------------------------

namespace {

// A list of `rows` buttons 40 tall in a frame 200 by 100, its canvas grown to
// hold them.
struct List
{
    Fixture fixture;
    InstanceId frame;
    std::vector<InstanceId> rows;

    explicit List(int count = 50)
    {
        frame = fixture.box("ScrollFrame", fixture.screen, 0.0f, 0.0f, 200.0f, 100.0f);
        scroll().automaticCanvasSize = 2;
        (void)fixture.child("UIListLayout", frame);
        for (int index = 0; index < count; ++index)
            rows.push_back(fixture.box("TextButton", frame, 0.0f, 0.0f, 200.0f, 40.0f));
        fixture.layout();
    }

    [[nodiscard]] scene::ScrollFrameComponent& scroll() { return *fixture.world->scrollFrames().find(frame); }

    // Time passing with no hand on it, a frame at a time.
    void wait(double seconds)
    {
        for (double gone = 0.0; gone < seconds; gone += 1.0 / 60.0) {
            ui::advance(*fixture.world, 1.0f / 60.0f);
            fixture.layout();
        }
    }
};

} // namespace

TEST_CASE("G40: a list thrown by a finger glides on, slows and stops; one stopped first does not")
{
    List list;
    // Seventy pixels up in a fifteenth of a second, and let go while moving.
    (void)list.fixture.touch(Vec2{50.0f, 90.0f}, 0.0, true, true);
    (void)list.fixture.touch(Vec2{50.0f, 70.0f}, 0.016, false, true);
    (void)list.fixture.touch(Vec2{50.0f, 45.0f}, 0.033, false, true);
    (void)list.fixture.touch(Vec2{50.0f, 20.0f}, 0.066, false, false);
    CHECK(list.scroll().canvasPosition.y == doctest::Approx(70.0));
    CHECK(list.scroll().flingVelocity.y > 900.0f);
    (void)list.fixture.events();

    list.wait(0.1);
    const float early = list.scroll().canvasPosition.y;
    CHECK(early > 150.0f);
    list.wait(3.0);
    // About its speed over the friction further on, and then still.
    CHECK(list.scroll().flingVelocity.y == 0.0f);
    const float rest = list.scroll().canvasPosition.y;
    CHECK(rest > 400.0f);
    CHECK(rest < 700.0f);
    list.wait(0.5);
    CHECK(list.scroll().canvasPosition.y == rest);

    SUBCASE("a finger that stopped before it lifted throws nothing")
    {
        (void)list.fixture.touch(Vec2{50.0f, 90.0f}, 10.0, true, true);
        (void)list.fixture.touch(Vec2{50.0f, 40.0f}, 10.05, false, true);
        (void)list.fixture.touch(Vec2{50.0f, 40.0f}, 10.3, false, true);
        (void)list.fixture.touch(Vec2{50.0f, 40.0f}, 10.31, false, false);
        CHECK(list.scroll().flingVelocity.y == 0.0f);
    }
}

TEST_CASE("G40: a press on a gliding list stops it and presses nothing; the next press does")
{
    List list;
    (void)list.fixture.touch(Vec2{50.0f, 90.0f}, 0.0, true, true);
    (void)list.fixture.touch(Vec2{50.0f, 50.0f}, 0.02, false, true);
    (void)list.fixture.touch(Vec2{50.0f, 10.0f}, 0.04, false, false);
    list.wait(0.05);
    REQUIRE(list.scroll().flingVelocity.y > 0.0f);
    (void)list.fixture.events();

    (void)list.fixture.touch(Vec2{50.0f, 50.0f}, 1.0, true, true);
    CHECK(list.scroll().flingVelocity.y == 0.0f);
    const float caught = list.scroll().canvasPosition.y;
    (void)list.fixture.touch(Vec2{50.0f, 50.0f}, 1.1, false, false);
    CHECK_FALSE(list.fixture.said(list.fixture.events(), "Activated"));
    list.wait(0.5);
    CHECK(list.scroll().canvasPosition.y == caught);

    (void)list.fixture.touch(Vec2{50.0f, 50.0f}, 2.0, true, true);
    (void)list.fixture.touch(Vec2{50.0f, 50.0f}, 2.1, false, false);
    CHECK(list.fixture.said(list.fixture.events(), "Activated"));
}

TEST_CASE("G40: pulled past its end a list gives, less the further, and springs back; CanvasPosition never leaves")
{
    List list;
    (void)list.fixture.touch(Vec2{50.0f, 10.0f}, 0.0, true, true);
    (void)list.fixture.touch(Vec2{50.0f, 60.0f}, 0.1, false, true);
    // Fifty pixels down at the top: the canvas stays at the start, and the
    // content shows some of the pull, not all of it.
    CHECK(list.scroll().canvasPosition.y == 0.0f);
    const float half = list.scroll().overscroll.y;
    CHECK(half < -5.0f);
    CHECK(half > -50.0f);
    (void)list.fixture.touch(Vec2{50.0f, 95.0f}, 0.2, false, true);
    CHECK(list.scroll().overscroll.y < half);
    CHECK(list.scroll().overscroll.y - half > -35.0f);
    // The rows are drawn where the pull put them.
    list.fixture.layout();
    CHECK(list.fixture.object(list.rows[0]).absolutePosition.y == doctest::Approx(-static_cast<double>(list.scroll().overscroll.y)));

    // Held, it stays; let go, it comes back.
    list.wait(0.2);
    CHECK(list.scroll().overscroll.y < half);
    (void)list.fixture.touch(Vec2{50.0f, 95.0f}, 3.0, false, false);
    list.wait(1.0);
    CHECK(list.scroll().overscroll.y == 0.0f);
    CHECK(list.fixture.object(list.rows[0]).absolutePosition.y == doctest::Approx(0.0));

    SUBCASE("Never is a hard stop")
    {
        list.scroll().elasticBehavior = 2;
        (void)list.fixture.touch(Vec2{50.0f, 10.0f}, 5.0, true, true);
        (void)list.fixture.touch(Vec2{50.0f, 60.0f}, 5.1, false, true);
        CHECK(list.scroll().overscroll.y == 0.0f);
        (void)list.fixture.touch(Vec2{50.0f, 60.0f}, 5.2, false, false);
    }
    SUBCASE("a list too short to scroll stays put, and its row is pressed")
    {
        List shortList(2);
        (void)shortList.fixture.touch(Vec2{50.0f, 5.0f}, 0.0, true, true);
        (void)shortList.fixture.touch(Vec2{50.0f, 35.0f}, 0.1, false, true);
        CHECK(shortList.scroll().overscroll.y == 0.0f);
        (void)shortList.fixture.touch(Vec2{50.0f, 35.0f}, 0.2, false, false);
        CHECK(shortList.fixture.said(shortList.fixture.events(), "Activated"));
        // Always gives even then.
        shortList.scroll().elasticBehavior = 1;
        (void)shortList.fixture.touch(Vec2{50.0f, 5.0f}, 1.0, true, true);
        (void)shortList.fixture.touch(Vec2{50.0f, 35.0f}, 1.1, false, true);
        CHECK(shortList.scroll().overscroll.y < 0.0f);
        (void)shortList.fixture.touch(Vec2{50.0f, 35.0f}, 1.2, false, false);
        CHECK_FALSE(shortList.fixture.said(shortList.fixture.events(), "Activated"));
    }
}

TEST_CASE("G40: a list thrown at its end stops there and bounces")
{
    List list(5); // 200 of canvas, 100 of travel
    (void)list.fixture.touch(Vec2{50.0f, 90.0f}, 0.0, true, true);
    (void)list.fixture.touch(Vec2{50.0f, 50.0f}, 0.02, false, true);
    (void)list.fixture.touch(Vec2{50.0f, 10.0f}, 0.04, false, false);
    list.wait(0.1);
    CHECK(list.scroll().canvasPosition.y == doctest::Approx(100.0));
    CHECK(list.scroll().flingVelocity.y == 0.0f);
    CHECK(list.scroll().overscroll.y > 0.0f);
    list.wait(1.0);
    CHECK(list.scroll().overscroll.y == 0.0f);
    CHECK(list.scroll().canvasPosition.y == doctest::Approx(100.0));
}

TEST_CASE("G40: ScrollingEnabled and ScrollingDirection say what a hand may do")
{
    List list;
    SUBCASE("not enabled: neither the wheel nor a finger, and the row is pressed")
    {
        list.scroll().scrollingEnabled = false;
        CHECK_FALSE(list.fixture.wheel(Vec2{50.0f, 50.0f}, -1.0f).wheelTaken);
        // Up within the first row, so the press and the release are on it.
        (void)list.fixture.touch(Vec2{50.0f, 35.0f}, 0.0, true, true);
        (void)list.fixture.touch(Vec2{50.0f, 5.0f}, 0.1, false, true);
        (void)list.fixture.touch(Vec2{50.0f, 5.0f}, 0.2, false, false);
        CHECK(list.scroll().canvasPosition.y == 0.0f);
        CHECK(list.fixture.said(list.fixture.events(), "Activated"));
    }
    SUBCASE("across only: the wheel turns it across")
    {
        const InstanceId strip = list.fixture.box("ScrollFrame", list.fixture.screen, 300.0f, 0.0f, 200.0f, 100.0f);
        scene::ScrollFrameComponent& across = *list.fixture.world->scrollFrames().find(strip);
        across.canvasSize = UDim2{UDim{0.0f, 800.0f}, UDim{}};
        across.scrollingDirection = 1;
        CHECK(list.fixture.wheel(Vec2{350.0f, 50.0f}, -1.0f).wheelTaken);
        CHECK(across.canvasPosition.x == doctest::Approx(48.0));
        CHECK(across.canvasPosition.y == 0.0f);
    }
}

TEST_CASE("G40: the thumb drags the canvas as far along as it goes along its track, and the track pages")
{
    Fixture fixture;
    const InstanceId frame = fixture.box("ScrollFrame", fixture.screen, 0.0f, 0.0f, 100.0f, 100.0f);
    scene::ScrollFrameComponent& scroll = *fixture.world->scrollFrames().find(frame);
    scroll.canvasSize = UDim2{UDim{}, UDim{0.0f, 400.0f}};
    scroll.scrollBarThickness = 10.0f;
    (void)fixture.box("TextButton", frame, 0.0f, 0.0f, 100.0f, 400.0f);
    fixture.layout();

    // The thumb is the top quarter of the track along the right edge: 75 of
    // travel for 300 of canvas, four to one.
    (void)fixture.press(Vec2{95.0f, 10.0f});
    (void)fixture.hold(Vec2{95.0f, 40.0f});
    CHECK(scroll.canvasPosition.y == doctest::Approx(120.0));
    (void)fixture.hold(Vec2{95.0f, 500.0f});
    CHECK(scroll.canvasPosition.y == doctest::Approx(300.0));
    (void)fixture.release(Vec2{95.0f, 500.0f});
    // The button under the bar was not pressed by it.
    CHECK_FALSE(fixture.said(fixture.events(), "Activated"));

    // On the track above the thumb: a view back.
    (void)fixture.press(Vec2{95.0f, 10.0f});
    (void)fixture.release(Vec2{95.0f, 10.0f});
    CHECK(scroll.canvasPosition.y == doctest::Approx(200.0));
    CHECK_FALSE(fixture.said(fixture.events(), "Activated"));
}

TEST_CASE("G40: a drag in a list inside a carousel goes to the one that scrolls the way it started")
{
    Fixture fixture;
    const InstanceId carousel = fixture.box("ScrollFrame", fixture.screen, 0.0f, 0.0f, 200.0f, 100.0f);
    const InstanceId list = fixture.box("ScrollFrame", carousel, 0.0f, 0.0f, 200.0f, 100.0f);
    // Both made before either is held: a pool that grows moves what is in it.
    scene::ScrollFrameComponent& outer = *fixture.world->scrollFrames().find(carousel);
    outer.canvasSize = UDim2{UDim{0.0f, 600.0f}, UDim{}};
    outer.scrollingDirection = 1;
    scene::ScrollFrameComponent& inner = *fixture.world->scrollFrames().find(list);
    inner.canvasSize = UDim2{UDim{}, UDim{0.0f, 400.0f}};
    (void)fixture.box("TextButton", list, 0.0f, 0.0f, 200.0f, 400.0f);
    fixture.layout();

    // Down first: the list, and only down, however sideways it wanders after.
    (void)fixture.touch(Vec2{100.0f, 80.0f}, 0.0, true, true);
    (void)fixture.touch(Vec2{98.0f, 60.0f}, 0.1, false, true);
    (void)fixture.touch(Vec2{40.0f, 40.0f}, 0.2, false, true);
    CHECK(inner.canvasPosition.y == doctest::Approx(40.0));
    CHECK(inner.canvasPosition.x == 0.0f);
    CHECK(outer.canvasPosition.x == 0.0f);
    (void)fixture.touch(Vec2{40.0f, 40.0f}, 1.0, false, false);

    // Sideways first: the carousel, which the list cannot do.
    (void)fixture.touch(Vec2{150.0f, 50.0f}, 2.0, true, true);
    (void)fixture.touch(Vec2{130.0f, 52.0f}, 2.1, false, true);
    (void)fixture.touch(Vec2{90.0f, 80.0f}, 2.2, false, true);
    CHECK(outer.canvasPosition.x == doctest::Approx(60.0));
    CHECK(inner.canvasPosition.y == doctest::Approx(40.0));
    (void)fixture.touch(Vec2{90.0f, 80.0f}, 3.0, false, false);
    CHECK_FALSE(fixture.said(fixture.events(), "Activated"));
}

TEST_CASE("G40: a selection moved out of sight scrolls into view, down and back up")
{
    List list(10);
    list.fixture.selected() = list.rows[5];
    (void)list.fixture.navigate(0, 0);
    // Row five is 200 to 240: its bottom at the bottom of the view.
    CHECK(list.scroll().canvasPosition.y == doctest::Approx(140.0));
    (void)list.fixture.navigate(0, 1);
    CHECK(list.fixture.selected() == list.rows[6]);
    CHECK(list.scroll().canvasPosition.y == doctest::Approx(180.0));
    list.fixture.selected() = list.rows[1];
    (void)list.fixture.navigate(0, 0);
    CHECK(list.scroll().canvasPosition.y == doctest::Approx(40.0));
}

// --- Pages, by hand -------------------------------------------------------------

namespace {

struct Pages
{
    Fixture fixture;
    InstanceId holder;
    InstanceId layout;
    std::vector<InstanceId> pages;

    Pages()
    {
        holder = fixture.box("Frame", fixture.screen, 0.0f, 0.0f, 400.0f, 300.0f);
        layout = fixture.child("UIPageLayout", holder);
        settings().animated = false;
        for (int index = 0; index < 3; ++index) {
            const InstanceId page = fixture.child("Frame", holder);
            fixture.object(page).size = UDim2{UDim{1.0f, 0.0f}, UDim{1.0f, 0.0f}};
            pages.push_back(page);
        }
        fixture.layout();
    }

    [[nodiscard]] scene::UIPageLayoutComponent& settings() { return *fixture.world->uiPageLayouts().find(layout); }
};

} // namespace

TEST_CASE("a swipe across the pages turns one, and a small one does not")
{
    Pages rig;
    Fixture& fixture = rig.fixture;

    // To the left is forwards: the next page comes in from the right.
    (void)fixture.press(Vec2{300.0f, 150.0f});
    (void)fixture.hold(Vec2{200.0f, 150.0f});
    (void)fixture.release(Vec2{200.0f, 150.0f});
    CHECK(rig.settings().currentPage == rig.pages[1]);

    (void)fixture.press(Vec2{100.0f, 150.0f});
    (void)fixture.release(Vec2{250.0f, 150.0f});
    CHECK(rig.settings().currentPage == rig.pages[0]);

    // A sixth of 400 is the least: 30 pixels is a tap that wandered.
    (void)fixture.press(Vec2{300.0f, 150.0f});
    (void)fixture.release(Vec2{270.0f, 150.0f});
    CHECK(rig.settings().currentPage == rig.pages[0]);

    rig.settings().touchInputEnabled = false;
    (void)fixture.press(Vec2{300.0f, 150.0f});
    (void)fixture.release(Vec2{100.0f, 150.0f});
    CHECK(rig.settings().currentPage == rig.pages[0]);
}

TEST_CASE("the wheel and a shoulder button turn a page")
{
    Pages rig;
    Fixture& fixture = rig.fixture;

    CHECK(fixture.wheel(Vec2{200.0f, 150.0f}, -1.0f).wheelTaken);
    CHECK(rig.settings().currentPage == rig.pages[1]);
    (void)fixture.wheel(Vec2{200.0f, 150.0f}, 1.0f);
    CHECK(rig.settings().currentPage == rig.pages[0]);
    // Off the pages, nothing.
    CHECK_FALSE(fixture.wheel(Vec2{700.0f, 500.0f}, -1.0f).wheelTaken);
    rig.settings().scrollWheelInputEnabled = false;
    (void)fixture.wheel(Vec2{200.0f, 150.0f}, -1.0f);
    CHECK(rig.settings().currentPage == rig.pages[0]);

    ui::InteractionInput input;
    input.pointer = Vec2{-100.0f, -100.0f};
    input.pageStep = 1;
    (void)fixture.send(input);
    CHECK(rig.settings().currentPage == rig.pages[1]);
    input.pageStep = -1;
    (void)fixture.send(input);
    CHECK(rig.settings().currentPage == rig.pages[0]);
    rig.settings().gamepadInputEnabled = false;
    input.pageStep = 1;
    (void)fixture.send(input);
    CHECK(rig.settings().currentPage == rig.pages[0]);
}
