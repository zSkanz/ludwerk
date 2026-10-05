// **D430**: a tap presses the game's interface.
//
// On a phone no button's `Activated` fired and no field took focus: the
// interface took the press from the events and its place from the mouse's
// pointer, which a finger never moves. A press carries its own place.
#include <doctest/doctest.h>
#include <vector>

#include "engine/app/ui_pointer.h"
#include "engine/app/world_host.h"
#include "engine/platform/event.h"
#include "engine/scene/world.h"
#include "engine/ui/ui.h"
#include "project_fixture.h"

using namespace engine;

namespace {

// What a phone delivers for one tap at `at`: the finger, and the mouse the
// system makes out of it -- marked as made from a touch, which is what the
// input system skips and the interface follows.
[[nodiscard]] std::vector<platform::Event> tapDown(core::Vec2 at)
{
    std::vector<platform::Event> events(3);
    events[0].type = platform::EventType::FingerDown;
    events[0].fingerId = 7;
    events[1].type = platform::EventType::MouseMoved;
    events[1].fromTouch = true;
    events[2].type = platform::EventType::MouseButtonDown;
    events[2].button = platform::MouseButton::Left;
    events[2].clicks = 1;
    events[2].fromTouch = true;
    for (platform::Event& event : events) {
        event.pointerX = at.x;
        event.pointerY = at.y;
    }
    return events;
}

[[nodiscard]] std::vector<platform::Event> tapUp(core::Vec2 at)
{
    std::vector<platform::Event> events(2);
    events[0].type = platform::EventType::MouseButtonUp;
    events[0].button = platform::MouseButton::Left;
    events[0].fromTouch = true;
    events[1].type = platform::EventType::FingerUp;
    events[1].fingerId = 7;
    for (platform::Event& event : events) {
        event.pointerX = at.x;
        event.pointerY = at.y;
    }
    return events;
}

// One frame of the loop's interface half: the events, the layout, the
// interaction, and a tick for the handlers.
ui::InteractionResult frame(app::WorldHost& host, app::UiPointer& pointer, const std::vector<platform::Event>& events)
{
    // The input system first, as the loop does: it skips what was made from a
    // touch, so its pointer stays where the mouse left it.
    host.pumpInput(events);
    pointer.beginFrame();
    for (const platform::Event& event : events)
        pointer.feed(event);
    ui::layout(host.world(), host.uiService(), core::Vec2{800.0f, 600.0f});
    ui::InteractionInput interaction;
    pointer.fill(interaction, host.input().snapshot().pointer);
    const ui::InteractionResult result = ui::updateInteraction(host.world(), host.uiService(), interaction);
    host.tick();
    return result;
}

} // namespace

TEST_CASE("D430: a tap presses a button and focuses a field")
{
    app::testing::Captured log;
    app::testing::Project project;
    project.write("src/client/Main.luau", R"(
        local UIService = game:GetService("UIService")
        local screen = Instance.new("ScreenGui")
        screen.Parent = UIService
        local button = Instance.new("TextButton")
        button.Name = "Play"
        button.Position = UDim2.fromOffset(300, 200)
        button.Size = UDim2.fromOffset(200, 80)
        button.Parent = screen
        button.Activated:Connect(function()
            print("button-activated")
        end)
        local field = Instance.new("TextInput")
        field.Name = "PlayerName"
        field.Position = UDim2.fromOffset(300, 400)
        field.Size = UDim2.fromOffset(200, 60)
        field.Parent = screen
        field.Focused:Connect(function()
            print("field-focused")
        end)
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(app::testing::bootOptions(project.root)).has_value());
    host.tick();
    ui::resetInteraction();

    app::UiPointer pointer;
    // Nothing has touched the screen: the input system's pointer is at its
    // corner, where it began.
    (void)frame(host, pointer, {});
    CHECK(host.input().snapshot().pointer.x == 0.0f);

    // **A tap on the button**: down and up at its middle.
    const core::Vec2 onButton{400.0f, 240.0f};
    (void)frame(host, pointer, tapDown(onButton));
    (void)frame(host, pointer, tapUp(onButton));
    CHECK(log.contains("button-activated"));
    // The mouse's pointer did not move -- the finger is not a mouse -- and the
    // interface's did.
    CHECK(host.input().snapshot().pointer.x == 0.0f);
    CHECK(pointer.positionOr(core::Vec2{}).x == 400.0f);

    // **A tap on the field** focuses it, which is what raises the keyboard.
    const core::Vec2 onField{400.0f, 430.0f};
    (void)frame(host, pointer, tapDown(onField));
    const ui::InteractionResult focused = frame(host, pointer, tapUp(onField));
    CHECK(log.contains("field-focused"));
    CHECK(focused.textInputFocused);

    // A tap whose down and up arrive in ONE frame is still a press and a
    // release, at the tap's place.
    std::vector<platform::Event> quick = tapDown(onButton);
    for (const platform::Event& event : tapUp(onButton))
        quick.push_back(event);
    log.lines.clear();
    (void)frame(host, pointer, quick);
    (void)frame(host, pointer, {});
    CHECK(log.contains("button-activated"));
}

namespace {

// A finger that is not the system's mouse: a phone makes a mouse of the first
// finger down and of no other.
[[nodiscard]] platform::Event finger(platform::EventType type, core::u64 id, core::Vec2 at)
{
    platform::Event event;
    event.type = type;
    event.fingerId = id;
    event.pointerX = at.x;
    event.pointerY = at.y;
    return event;
}

} // namespace

TEST_CASE("D557: a second finger presses a button while the first is held somewhere else")
{
    // On a phone the left thumb rests on the stick for the whole run: it is
    // the first finger down, the one the interface's pointer follows. The
    // right thumb then taps a button -- the cross of a sheet, a card, RESUME
    // -- and the tap was not heard at all: the interface heard one finger. A
    // player could not close a sheet over a fight and died behind it.
    app::testing::Captured log;
    app::testing::Project project;
    project.write("src/client/Main.luau", R"(
        local UIService = game:GetService("UIService")
        local screen = Instance.new("ScreenGui")
        screen.Parent = UIService
        local close = Instance.new("TextButton")
        close.Name = "Close"
        close.Position = UDim2.fromOffset(600, 40)
        close.Size = UDim2.fromOffset(120, 80)
        close.Parent = screen
        local count = 0
        close.Activated:Connect(function()
            count += 1
            print(`close-activated {count}`)
        end)
        local other = Instance.new("TextButton")
        other.Name = "Other"
        other.Position = UDim2.fromOffset(600, 300)
        other.Size = UDim2.fromOffset(120, 80)
        other.Parent = screen
        other.Activated:Connect(function()
            print("other-activated")
        end)
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(app::testing::bootOptions(project.root)).has_value());
    host.tick();
    ui::resetInteraction();
    app::UiPointer pointer;
    (void)frame(host, pointer, {});

    // The first finger comes down on nothing -- the stick's corner of the
    // glass -- and stays.
    const core::Vec2 onStick{100.0f, 500.0f};
    (void)frame(host, pointer, tapDown(onStick));
    (void)frame(host, pointer, {});

    // The second taps the button: down and up on it, a frame apart.
    const core::Vec2 onClose{660.0f, 80.0f};
    (void)frame(host, pointer, {finger(platform::EventType::FingerDown, 9, onClose)});
    CHECK_FALSE(log.contains("close-activated"));
    (void)frame(host, pointer, {finger(platform::EventType::FingerUp, 9, onClose)});
    CHECK(log.contains("close-activated 1"));
    CHECK_FALSE(log.contains("close-activated 2"));

    // Down and up inside one frame is a tap too.
    (void)frame(
        host, pointer,
        {finger(platform::EventType::FingerDown, 9, onClose), finger(platform::EventType::FingerUp, 9, onClose)});
    CHECK(log.contains("close-activated 2"));

    // Both ends on the same element, as for the first finger: a second finger
    // that slides off what it pressed has changed its mind, and one that
    // slides ONTO a button did not press it.
    (void)frame(host, pointer, {finger(platform::EventType::FingerDown, 9, onClose)});
    (void)frame(host, pointer, {finger(platform::EventType::FingerMoved, 9, core::Vec2{660.0f, 340.0f})});
    (void)frame(host, pointer, {finger(platform::EventType::FingerUp, 9, core::Vec2{660.0f, 340.0f})});
    CHECK_FALSE(log.contains("close-activated 3"));
    CHECK_FALSE(log.contains("other-activated"));

    // Two more fingers on two buttons at once: each its own press.
    (void)frame(host, pointer,
                {finger(platform::EventType::FingerDown, 9, onClose),
                 finger(platform::EventType::FingerDown, 11, core::Vec2{660.0f, 340.0f})});
    (void)frame(host, pointer,
                {finger(platform::EventType::FingerUp, 11, core::Vec2{660.0f, 340.0f}),
                 finger(platform::EventType::FingerUp, 9, onClose)});
    CHECK(log.contains("close-activated 3"));
    CHECK(log.contains("other-activated"));

    // And the first finger is still the pointer: it lifts, and nothing it was
    // never on is pressed by that.
    (void)frame(host, pointer, tapUp(onStick));
    CHECK_FALSE(log.contains("close-activated 4"));
}

TEST_CASE("D430: the interface's pointer follows the mouse, a mouse made of a finger, and the first finger")
{
    app::UiPointer pointer;
    // Before anything has said where: the input system's.
    CHECK(pointer.positionOr(core::Vec2{12.0f, 34.0f}).x == 12.0f);

    platform::Event moved;
    moved.type = platform::EventType::MouseMoved;
    moved.pointerX = 100.0f;
    moved.pointerY = 50.0f;
    pointer.feed(moved);
    CHECK(pointer.positionOr(core::Vec2{}).x == 100.0f);

    // A finger with no mouse made of it still moves the pointer; a second
    // finger does not take it over.
    platform::Event first;
    first.type = platform::EventType::FingerDown;
    first.fingerId = 1;
    first.pointerX = 300.0f;
    first.pointerY = 200.0f;
    pointer.feed(first);
    platform::Event second = first;
    second.fingerId = 2;
    second.pointerX = 700.0f;
    pointer.feed(second);
    CHECK(pointer.positionOr(core::Vec2{}).x == 300.0f);
    first.type = platform::EventType::FingerMoved;
    first.pointerX = 320.0f;
    pointer.feed(first);
    CHECK(pointer.positionOr(core::Vec2{}).x == 320.0f);

    // A right button is not the interface's press.
    platform::Event right;
    right.type = platform::EventType::MouseButtonDown;
    right.button = platform::MouseButton::Right;
    pointer.feed(right);
    CHECK_FALSE(pointer.down());
}
