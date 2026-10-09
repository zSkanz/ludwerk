#include <doctest/doctest.h>
#include <ostream>

#include "engine/input/input.h"
#include "engine/input/scene_types.h"
#include "engine/scene/world.h"

// scene's generated header, through the include directory `engine_scene` exports.
// input's own is reached by a relative path from its source; a test does not
// need it, because `registerSceneTypes` is the public way in.
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "class_descriptors.gen.h"

namespace {

namespace core = engine::core;
namespace input = engine::input;
namespace platform = engine::platform;
namespace scene = engine::scene;

using core::InstanceId;

// `Enum.KeyCode` values, by name, so a case reads as the binding a script would
// write. Taken from the generated enum at construction rather than hard-coded:
// the point of these tests is the resolver, and a case that spelled 4 where it
// meant "D" would go wrong silently when the enum moved.
struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    input::InputSystem system;

    Fixture()
    {
        scene::generated::registerClasses(classes, atoms);
        input::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
    }

    [[nodiscard]] core::i32 keyCode(const char* name) const
    {
        const scene::EnumDescriptor* descriptor = enums.find(scene::generated::KeyCodeEnumId);
        REQUIRE(descriptor != nullptr);
        for (const scene::EnumItemDesc& item : descriptor->items) {
            if (atoms.text(item.name) == name)
                return item.value;
        }
        FAIL("no KeyCode item named ", name);
        return 0;
    }

    [[nodiscard]] InstanceId make(const char* className)
    {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != scene::InvalidClass);
        return world->create(id);
    }

    InstanceId context(float priority = 0.0f, bool sink = false)
    {
        const InstanceId id = make("InputContext");
        scene::InputContextComponent* component = world->inputContexts().find(id);
        REQUIRE(component != nullptr);
        component->priority = priority;
        component->sink = sink;
        return id;
    }

    InstanceId action(InstanceId parent, input::ActionType type)
    {
        const InstanceId id = make("InputAction");
        REQUIRE_FALSE(world->setParent(id, parent).has_value());
        scene::InputActionComponent* component = world->inputActions().find(id);
        REQUIRE(component != nullptr);
        component->type = static_cast<core::i32>(type);
        return id;
    }

    InstanceId binding(InstanceId parent)
    {
        const InstanceId id = make("InputBinding");
        REQUIRE_FALSE(world->setParent(id, parent).has_value());
        REQUIRE(world->inputBindings().find(id) != nullptr);
        return id;
    }

    void press(const char* keyName)
    {
        platform::Event event;
        event.type = platform::EventType::KeyDown;
        event.key = platform::keyFromName(keyName);
        REQUIRE(event.key != platform::Key::Unknown);
        const platform::Event events[] = {event};
        system.pumpFrame(events);
    }

    void release(const char* keyName)
    {
        platform::Event event;
        event.type = platform::EventType::KeyUp;
        event.key = platform::keyFromName(keyName);
        REQUIRE(event.key != platform::Key::Unknown);
        const platform::Event events[] = {event};
        system.pumpFrame(events);
    }

    [[nodiscard]] const scene::InputActionComponent& state(InstanceId id) const
    {
        const scene::InputActionComponent* component = world->inputActions().find(id);
        REQUIRE(component != nullptr);
        return *component;
    }

    // The names of the events enqueued since the last call, in raise order.
    [[nodiscard]] std::vector<std::string> drainEvents()
    {
        std::vector<std::string> names;
        for (const scene::Change& change : world->changes().take()) {
            if (change.kind == scene::ChangeKind::InstanceEventNoArgs)
                names.emplace_back(atoms.text(change.name));
        }
        return names;
    }
};

} // namespace

TEST_CASE("a KeyCode belongs to the device its range says")
{
    // The ranges in input.cpp are `Enum.KeyCode`'s layout written where the
    // resolver can use it, so this is the case that goes red if an item is
    // inserted in the middle of the enum without the ranges moving with it.
    Fixture fixture;
    CHECK(input::deviceOf(fixture.keyCode("Unknown")) == input::DeviceType::KeyboardMouse);
    CHECK(input::deviceOf(fixture.keyCode("W")) == input::DeviceType::KeyboardMouse);
    CHECK(input::deviceOf(fixture.keyCode("MouseLeft")) == input::DeviceType::KeyboardMouse);
    CHECK(input::deviceOf(fixture.keyCode("MouseMovement")) == input::DeviceType::KeyboardMouse);
    CHECK(input::deviceOf(fixture.keyCode("ButtonSouth")) == input::DeviceType::Gamepad);
    CHECK(input::deviceOf(fixture.keyCode("LeftTrigger")) == input::DeviceType::Gamepad);
    CHECK(input::deviceOf(fixture.keyCode("RightThumbstick")) == input::DeviceType::Gamepad);

    CHECK_FALSE(input::isAnalog(fixture.keyCode("W")));
    CHECK_FALSE(input::isAnalog(fixture.keyCode("ButtonSouth")));
    CHECK(input::isAnalog(fixture.keyCode("MouseMovement")));
    CHECK(input::isAnalog(fixture.keyCode("LeftTrigger")));
    CHECK(input::isAnalog(fixture.keyCode("LeftThumbstick")));
}

TEST_CASE("a Bool action follows the key its binding names")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId jump = fixture.action(context, input::ActionType::Bool);
    const InstanceId binding = fixture.binding(jump);
    fixture.world->inputBindings().find(binding)->keyCode = fixture.keyCode("Space");

    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK_FALSE(fixture.state(jump).pressed);
    (void)fixture.drainEvents();

    fixture.press("Space");
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(jump).pressed);
    // Two facts, in this order: the specific one and the general one. A handler
    // connected to either is told once.
    CHECK(fixture.drainEvents() == std::vector<std::string>{"Pressed", "StateChanged"});

    // Held rather than pressed again. `Pressed` is an edge, and an autorepeat
    // that re-fired it would be a jump per repeat instead of a jump per press.
    fixture.system.dispatchSimTick(*fixture.world, 3);
    CHECK(fixture.state(jump).pressed);
    CHECK(fixture.drainEvents().empty());

    fixture.release("Space");
    fixture.system.dispatchSimTick(*fixture.world, 4);
    CHECK_FALSE(fixture.state(jump).pressed);
    CHECK(fixture.drainEvents() == std::vector<std::string>{"Released", "StateChanged"});
}

TEST_CASE("a Direction2D action reads its four composite keys")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId move = fixture.action(context, input::ActionType::Direction2D);
    const InstanceId binding = fixture.binding(move);
    scene::InputBindingComponent* keys = fixture.world->inputBindings().find(binding);
    keys->up = fixture.keyCode("W");
    keys->down = fixture.keyCode("S");
    keys->left = fixture.keyCode("A");
    keys->right = fixture.keyCode("D");

    fixture.press("W");
    fixture.press("D");
    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK(fixture.state(move).axis.x == doctest::Approx(1.0));
    CHECK(fixture.state(move).axis.y == doctest::Approx(1.0));

    // Both halves of an axis at once cancel. Reading whichever the engine
    // happened to see last would make holding A and D drift sideways, which is
    // the classic version of this bug.
    fixture.press("A");
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(move).axis.x == doctest::Approx(0.0));
    CHECK(fixture.state(move).axis.y == doctest::Approx(1.0));

    // Scale is a multiplier and a negative one inverts the axis, which is what
    // a settings screen's "invert Y" writes.
    keys->scale = -2.0f;
    fixture.system.dispatchSimTick(*fixture.world, 3);
    CHECK(fixture.state(move).axis.y == doctest::Approx(-2.0));
}

TEST_CASE("a sinking context hides the keys it names and no others")
{
    Fixture fixture;

    // A menu above gameplay. Both bind Escape; only gameplay binds W.
    const InstanceId gameplay = fixture.context(0.0f, false);
    const InstanceId walk = fixture.action(gameplay, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(walk))->keyCode = fixture.keyCode("W");
    const InstanceId cancel = fixture.action(gameplay, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(cancel))->keyCode = fixture.keyCode("Escape");

    const InstanceId menu = fixture.context(10.0f, true);
    const InstanceId close = fixture.action(menu, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(close))->keyCode = fixture.keyCode("Escape");

    fixture.press("Escape");
    fixture.press("W");
    fixture.system.dispatchSimTick(*fixture.world, 1);

    // The menu got the Escape; gameplay did not.
    CHECK(fixture.state(close).pressed);
    CHECK_FALSE(fixture.state(cancel).pressed);
    // Sinking is per input rather than per context: the menu never named W, so
    // it cannot hide it. A context that swallowed everything would make a HUD
    // under a dialog impossible.
    CHECK(fixture.state(walk).pressed);

    // Disabling the menu hands Escape back the very next tick, which is what
    // closing a menu has to do.
    fixture.world->inputContexts().find(menu)->enabled = false;
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(cancel).pressed);
    CHECK_FALSE(fixture.state(close).pressed);
}

TEST_CASE("priority orders fallthrough and rate does not")
{
    Fixture fixture;

    // A `Render`-rate context is not dispatched by a sim tick at all, however
    // high its priority: the two are different questions (ADR 0039), and a
    // priority that also selected a clock would make this pair inexpressible.
    const InstanceId look = fixture.context(100.0f, true);
    fixture.world->inputContexts().find(look)->rate = static_cast<core::i32>(input::Rate::Render);
    const InstanceId aim = fixture.action(look, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(aim))->keyCode = fixture.keyCode("MouseLeft");

    const InstanceId gameplay = fixture.context(0.0f, false);
    const InstanceId fire = fixture.action(gameplay, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(fire))->keyCode = fixture.keyCode("MouseLeft");

    platform::Event click;
    click.type = platform::EventType::MouseButtonDown;
    click.button = platform::MouseButton::Left;
    const platform::Event events[] = {click};
    fixture.system.pumpFrame(events);

    fixture.system.dispatchSimTick(*fixture.world, 1);
    // The high-priority sinking context did NOT consume it, because it was not
    // dispatched: it is on the other clock.
    CHECK(fixture.state(fire).pressed);
    CHECK_FALSE(fixture.state(aim).pressed);

    fixture.system.dispatchRenderRate(*fixture.world);
    CHECK(fixture.state(aim).pressed);
}

TEST_CASE("a disabled action consumes nothing, even inside a sinking context")
{
    Fixture fixture;
    const InstanceId modal = fixture.context(10.0f, true);
    const InstanceId ignored = fixture.action(modal, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(ignored))->keyCode = fixture.keyCode("Space");
    fixture.world->inputActions().find(ignored)->enabled = false;

    const InstanceId gameplay = fixture.context(0.0f, false);
    const InstanceId jump = fixture.action(gameplay, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(jump))->keyCode = fixture.keyCode("Space");

    fixture.press("Space");
    fixture.system.dispatchSimTick(*fixture.world, 1);

    CHECK_FALSE(fixture.state(ignored).pressed);
    // Disabling an action hands its input back rather than swallowing it. The
    // opposite -- a disabled action that still sinks -- is a dead context that
    // silently eats a key for the rest of the session.
    CHECK(fixture.state(jump).pressed);
}

TEST_CASE("losing focus releases everything that was held")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId walk = fixture.action(context, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(walk))->keyCode = fixture.keyCode("W");

    fixture.press("W");
    fixture.system.dispatchSimTick(*fixture.world, 1);
    REQUIRE(fixture.state(walk).pressed);
    (void)fixture.drainEvents();

    fixture.system.releaseAll(*fixture.world);
    CHECK_FALSE(fixture.state(walk).pressed);
    // And it TELLS the game, rather than going quiet: a handler that started
    // something on `Pressed` needs its `Released` or the something never stops.
    const std::vector<std::string> events = fixture.drainEvents();
    CHECK(std::ranges::find(events, "Released") != events.end());
}

TEST_CASE("an analogue source on a Bool action presses past half deflection")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId shoot = fixture.action(context, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(shoot))->keyCode = fixture.keyCode("RightTrigger");

    platform::Event axis;
    axis.type = platform::EventType::GamepadAxisMoved;
    axis.gamepadAxis = platform::GamepadAxis::RightTrigger;

    axis.axisValue = 0.25f;
    const platform::Event light[] = {axis};
    fixture.system.pumpFrame(light);
    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK_FALSE(fixture.state(shoot).pressed);

    axis.axisValue = 0.75f;
    const platform::Event heavy[] = {axis};
    fixture.system.pumpFrame(heavy);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(shoot).pressed);
}

TEST_CASE("a stick reports up as +Y, the direction the Up composite means")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId move = fixture.action(context, input::ActionType::Direction2D);
    fixture.world->inputBindings().find(fixture.binding(move))->keyCode = fixture.keyCode("LeftThumbstick");

    // SDL reports a stick's Y positive DOWNWARD. One convention reaches the
    // game, and this case is what says which one.
    platform::Event axis;
    axis.type = platform::EventType::GamepadAxisMoved;
    axis.gamepadAxis = platform::GamepadAxis::LeftY;
    axis.axisValue = -1.0f;
    const platform::Event events[] = {axis};
    fixture.system.pumpFrame(events);
    fixture.system.dispatchSimTick(*fixture.world, 1);

    CHECK(fixture.state(move).axis.y == doctest::Approx(1.0));
}

TEST_CASE("unplugging or losing focus releases gamepad actions and both analogue sticks")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId jump = fixture.action(context, input::ActionType::Bool);
    const InstanceId move = fixture.action(context, input::ActionType::Direction2D);
    const InstanceId look = fixture.action(context, input::ActionType::Direction2D);
    fixture.world->inputBindings().find(fixture.binding(jump))->keyCode = fixture.keyCode("ButtonSouth");
    fixture.world->inputBindings().find(fixture.binding(move))->keyCode = fixture.keyCode("LeftThumbstick");
    fixture.world->inputBindings().find(fixture.binding(look))->keyCode = fixture.keyCode("RightThumbstick");
    platform::Event pad;
    pad.gamepadId = 19;
    pad.gamepadFamily = platform::GamepadType::Generic;
    pad.type = platform::EventType::GamepadButtonDown;
    pad.gamepadButton = platform::GamepadButton::South;
    fixture.system.pumpFrame({&pad, 1});
    pad.type = platform::EventType::GamepadAxisMoved;
    pad.gamepadAxis = platform::GamepadAxis::LeftX;
    pad.axisValue = 0.8f;
    fixture.system.pumpFrame({&pad, 1});
    pad.gamepadAxis = platform::GamepadAxis::RightY;
    pad.axisValue = -0.7f;
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 1);
    REQUIRE(fixture.state(jump).pressed);
    REQUIRE(fixture.state(move).axis.x > 0.0f);
    REQUIRE(fixture.state(look).axis.y > 0.0f);
    (void)fixture.drainEvents();

    SUBCASE("disconnect")
    {
        pad.type = platform::EventType::GamepadRemoved;
        fixture.system.pumpFrame({&pad, 1});
        fixture.system.dispatchSimTick(*fixture.world, 2);
    }
    SUBCASE("focus loss")
    {
        fixture.system.releaseAll(*fixture.world);
    }
    CHECK_FALSE(fixture.state(jump).pressed);
    CHECK(fixture.state(move).axis == core::Vec3{});
    CHECK(fixture.state(look).axis == core::Vec3{});
    const auto events = fixture.drainEvents();
    CHECK(std::ranges::find(events, "Released") != events.end());
}

TEST_CASE("a Direction3D action reports zero, which is what v1 promises")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId move = fixture.action(context, input::ActionType::Direction3D);
    scene::InputBindingComponent* keys = fixture.world->inputBindings().find(fixture.binding(move));
    keys->up = fixture.keyCode("W");
    keys->right = fixture.keyCode("D");

    fixture.press("W");
    fixture.press("D");
    fixture.system.dispatchSimTick(*fixture.world, 1);

    // Declared and not driveable: no binding in api-design.md §2.4's list names
    // three axes. The zero vector rather than a guess at what the caller meant,
    // and this case is what stops the guess being added quietly later.
    CHECK(fixture.state(move).axis.x == doctest::Approx(0.0));
    CHECK(fixture.state(move).axis.y == doctest::Approx(0.0));
    CHECK(fixture.state(move).axis.z == doctest::Approx(0.0));
}

TEST_CASE("a resting stick does not steal the prompts from the keyboard")
{
    Fixture fixture;

    platform::Event drift;
    drift.type = platform::EventType::GamepadAxisMoved;
    drift.gamepadAxis = platform::GamepadAxis::LeftX;
    drift.axisValue = 0.05f;
    const platform::Event resting[] = {drift};
    fixture.system.pumpFrame(resting);
    CHECK(fixture.system.snapshot().lastDevice == input::DeviceType::KeyboardMouse);

    drift.axisValue = 0.9f;
    const platform::Event pushed[] = {drift};
    fixture.system.pumpFrame(pushed);
    CHECK(fixture.system.snapshot().lastDevice == input::DeviceType::Gamepad);

    // Actual mouse movement selects keyboard/mouse; a zero delta does not.
    platform::Event moved;
    moved.type = platform::EventType::MouseMoved;
    moved.pointerDeltaX = 3.0f;
    const platform::Event nudge[] = {moved};
    fixture.system.pumpFrame(nudge);
    CHECK(fixture.system.snapshot().lastDevice == input::DeviceType::KeyboardMouse);
}

TEST_CASE("gamepad connection metadata does not change the deterministic world hash")
{
    Fixture fixture;
    (void)fixture.make("InputService");
    const auto before = fixture.world->worldHash();
    fixture.world->engineState().preferredGamepadType = static_cast<core::i32>(platform::GamepadType::PlayStation);
    fixture.world->engineState().preferredGamepadId = 42;
    CHECK(fixture.world->worldHash() == before);

    // The category remains simulation input through the existing property.
    fixture.world->engineState().lastInputDeviceType = static_cast<core::i32>(input::DeviceType::Gamepad);
    CHECK(fixture.world->worldHash() != before);
}

TEST_CASE("connecting a pad leaves preference alone; usage selects physical family and id")
{
    Fixture fixture;
    platform::Event pad;
    pad.type = platform::EventType::GamepadAdded;
    pad.gamepadId = 41;
    pad.gamepadFamily = platform::GamepadType::PlayStation;
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK(fixture.system.snapshot().lastDevice == input::DeviceType::KeyboardMouse);
    CHECK(fixture.system.snapshot().preferredGamepadId == 0);
    auto events = fixture.system.drainDeviceEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == input::DeviceEvent::Kind::Connected);
    CHECK(events[0].family == platform::GamepadType::PlayStation);

    pad.type = platform::EventType::GamepadButtonDown;
    pad.gamepadButton = platform::GamepadButton::South;
    pad.gamepadFamily = platform::GamepadType::Unknown; // Retain connection metadata.
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.world->engineState().preferredGamepadId == 41);
    CHECK(fixture.system.snapshot().preferredGamepadType == platform::GamepadType::PlayStation);
    CHECK(fixture.system.drainDeviceEvents().size() == 3);
    fixture.system.dispatchSimTick(*fixture.world, 3);
    CHECK(fixture.system.drainDeviceEvents().empty());

    fixture.press("Space");
    fixture.system.dispatchSimTick(*fixture.world, 4);
    events = fixture.system.drainDeviceEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == input::DeviceEvent::Kind::InputChanged);
    CHECK(fixture.system.snapshot().preferredGamepadId == 41);
    CHECK(fixture.system.snapshot().preferredGamepadType == platform::GamepadType::PlayStation);

    pad.gamepadId = 42;
    pad.gamepadFamily = platform::GamepadType::PlayStation;
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 5);
    (void)fixture.system.drainDeviceEvents();
    pad.gamepadId = 41;
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 6);
    events = fixture.system.drainDeviceEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == input::DeviceEvent::Kind::GamepadIdChanged);
    CHECK(events[0].id == 41);
}

TEST_CASE("unplugging one pad preserves another pad's held input")
{
    Fixture fixture;
    platform::Event pad;
    pad.type = platform::EventType::GamepadButtonDown;
    pad.gamepadButton = platform::GamepadButton::South;
    pad.gamepadId = 1;
    pad.gamepadFamily = platform::GamepadType::Xbox;
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 1);
    pad.gamepadId = 2;
    pad.gamepadFamily = platform::GamepadType::Nintendo;
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 2);
    (void)fixture.system.drainDeviceEvents();
    pad.type = platform::EventType::GamepadRemoved;
    pad.gamepadId = 1;
    pad.gamepadFamily = platform::GamepadType::Unknown;
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 3);
    CHECK(fixture.system.snapshot().held[static_cast<core::usize>(fixture.keyCode("ButtonSouth"))]);
    CHECK(fixture.system.snapshot().preferredGamepadId == 2);
    const auto events = fixture.system.drainDeviceEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].family == platform::GamepadType::Xbox);
    pad.gamepadId = 2;
    fixture.system.pumpFrame({&pad, 1});
    fixture.system.dispatchSimTick(*fixture.world, 4);
    CHECK_FALSE(fixture.system.snapshot().held[static_cast<core::usize>(fixture.keyCode("ButtonSouth"))]);
    CHECK(fixture.system.snapshot().preferredGamepadId == 0);
    CHECK(fixture.system.snapshot().preferredGamepadType == platform::GamepadType::Unknown);
}

TEST_CASE("unknown pads work, drift and idle motion do not select them, focus loss clears their buttons")
{
    Fixture fixture;
    platform::Event pad;
    pad.type = platform::EventType::GamepadAxisMoved;
    pad.gamepadAxis = platform::GamepadAxis::LeftX;
    pad.gamepadId = 9;
    pad.axisValue = 0.1f;
    fixture.system.pumpFrame({&pad, 1});
    CHECK(fixture.system.snapshot().preferredGamepadId == 0);
    pad.type = platform::EventType::GamepadButtonDown;
    pad.gamepadButton = platform::GamepadButton::South;
    fixture.system.pumpFrame({&pad, 1});
    CHECK(fixture.system.snapshot().preferredGamepadId == 9);
    CHECK(fixture.system.snapshot().preferredGamepadType == platform::GamepadType::Unknown);
    platform::Event mouse;
    mouse.type = platform::EventType::MouseMoved;
    fixture.system.pumpFrame({&mouse, 1});
    CHECK(fixture.system.snapshot().lastDevice == input::DeviceType::Gamepad);
    fixture.system.releaseAll(*fixture.world);
    pad.type = platform::EventType::GamepadAxisMoved;
    fixture.system.pumpFrame({&pad, 1});
    CHECK_FALSE(fixture.system.snapshot().held[static_cast<core::usize>(fixture.keyCode("ButtonSouth"))]);
}

// --- The raw event surface (ADR 0041) ----------------------------------------
//
// Every case below is about the one property that made this surface allowable at
// all: these events come out of the IAS's own dispatch. Not from the OS, not on
// the wall clock, and not before the UI has said what it took.

namespace {

// The raw events one `Simulation` dispatch produced, drained.
[[nodiscard]] std::vector<input::RawInputEvent> rawOf(Fixture& fixture)
{
    const std::span<const input::RawInputEvent> events = fixture.system.drainRawEvents();
    return std::vector<input::RawInputEvent>(events.begin(), events.end());
}

[[nodiscard]] platform::Event mouseButton(platform::EventType type)
{
    platform::Event event;
    event.type = type;
    event.button = platform::MouseButton::Left;
    return event;
}

} // namespace

TEST_CASE("a key press and release produce InputBegan and InputEnded, once each")
{
    Fixture fixture;
    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK(rawOf(fixture).empty());

    fixture.press("Space");
    fixture.system.dispatchSimTick(*fixture.world, 2);
    std::vector<input::RawInputEvent> began = rawOf(fixture);
    REQUIRE(began.size() == 1);
    CHECK(began[0].phase == input::RawInputEvent::Phase::Began);
    CHECK(began[0].userInputType == input::UserInputType::Keyboard);
    CHECK(began[0].keyCode == fixture.keyCode("Space"));
    CHECK_FALSE(began[0].uiConsumed);

    // Held is not begun. A surface that fired every tick a key was down would be
    // a polling loop wearing an event's clothes, and every handler would have to
    // filter it back out.
    fixture.system.dispatchSimTick(*fixture.world, 3);
    CHECK(rawOf(fixture).empty());

    fixture.release("Space");
    fixture.system.dispatchSimTick(*fixture.world, 4);
    std::vector<input::RawInputEvent> ended = rawOf(fixture);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].phase == input::RawInputEvent::Phase::Ended);
    CHECK(ended[0].keyCode == fixture.keyCode("Space"));
}

TEST_CASE("nothing is produced on the Render clock")
{
    // ADR 0041 puts these on `Simulation` so a handler that writes to the world
    // replays by construction. A render dispatch that also produced them would
    // fire one press twice, on two clocks, one of which a replay does not have.
    Fixture fixture;
    fixture.press("Space");
    fixture.system.dispatchRenderRate(*fixture.world);
    CHECK(rawOf(fixture).empty());

    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK(rawOf(fixture).size() == 1);
}

TEST_CASE("the UI's claim reaches the event's second argument")
{
    Fixture fixture;
    fixture.system.setPointerCapturedByUi(true);

    const platform::Event events[] = {mouseButton(platform::EventType::MouseButtonDown)};
    fixture.system.pumpFrame(events);
    fixture.system.dispatchSimTick(*fixture.world, 1);

    std::vector<input::RawInputEvent> raw = rawOf(fixture);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].userInputType == input::UserInputType::MouseButton1);
    // The whole reason the flag is the SECOND argument rather than something a
    // handler has to go and ask for: a click on a button must not also fire the
    // gun, and the only way a handler can know is if it is told.
    CHECK(raw[0].uiConsumed);
}

TEST_CASE("a keyboard press is the game's even while the pointer is over the UI")
{
    // The pointer's claim covers the mouse codes and nothing else. A health bar
    // under the cursor eating the jump key is the defect this asserts against.
    Fixture fixture;
    fixture.system.setPointerCapturedByUi(true);
    fixture.press("Space");
    fixture.system.dispatchSimTick(*fixture.world, 1);

    std::vector<input::RawInputEvent> raw = rawOf(fixture);
    REQUIRE(raw.size() == 1);
    CHECK_FALSE(raw[0].uiConsumed);
}

TEST_CASE("a focused TextInput takes the keyboard, and the action stops seeing it")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId jump = fixture.action(context, input::ActionType::Bool);
    const InstanceId binding = fixture.binding(jump);
    fixture.world->inputBindings().find(binding)->keyCode = fixture.keyCode("W");

    fixture.press("W");
    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK(fixture.state(jump).pressed);

    // Focus moves into a text field. The key is still physically down, and the
    // action has to stop seeing it or a player typing `w` into a chat box walks
    // forward.
    fixture.system.setKeyboardCapturedByUi(true);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK_FALSE(fixture.state(jump).pressed);
}

TEST_CASE("InputEnded reports what InputBegan reported, even if the UI let go first")
{
    // A press that started on a button is still that press when it is released,
    // and a handler pairing the two must not be told the release was the game's
    // when the press was not. Otherwise a drag off a button is half-handled.
    Fixture fixture;
    fixture.system.setPointerCapturedByUi(true);

    const platform::Event downEvents[] = {mouseButton(platform::EventType::MouseButtonDown)};
    fixture.system.pumpFrame(downEvents);
    fixture.system.dispatchSimTick(*fixture.world, 1);
    REQUIRE(rawOf(fixture).size() == 1);

    fixture.system.setPointerCapturedByUi(false);
    const platform::Event upEvents[] = {mouseButton(platform::EventType::MouseButtonUp)};
    fixture.system.pumpFrame(upEvents);
    fixture.system.dispatchSimTick(*fixture.world, 2);

    std::vector<input::RawInputEvent> raw = rawOf(fixture);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].phase == input::RawInputEvent::Phase::Ended);
    CHECK(raw[0].uiConsumed);
}

TEST_CASE("pointer motion is one InputChanged a tick, with the delta accumulated")
{
    Fixture fixture;
    platform::Event first;
    first.type = platform::EventType::MouseMoved;
    first.pointerX = 10.0f;
    first.pointerY = 20.0f;
    first.pointerDeltaX = 3.0f;
    platform::Event second = first;
    second.pointerX = 14.0f;
    second.pointerDeltaX = 4.0f;
    const platform::Event events[] = {first, second};
    fixture.system.pumpFrame(events);
    fixture.system.dispatchSimTick(*fixture.world, 1);

    std::vector<input::RawInputEvent> raw = rawOf(fixture);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].phase == input::RawInputEvent::Phase::Changed);
    CHECK(raw[0].userInputType == input::UserInputType::MouseMovement);
    // Seven and not four: a handler that saw only the last device event would
    // lose most of a fast flick, which is the same reason the deltas are
    // accumulated for actions.
    CHECK(static_cast<double>(raw[0].delta.x) == doctest::Approx(7.0));
    CHECK(static_cast<double>(raw[0].position.x) == doctest::Approx(14.0));
    // Motion has no beginning and no end, so it is never `Began` or `Ended`.
    CHECK(raw[0].keyCode == 0);
}

TEST_CASE("a resting pointer produces nothing at all")
{
    // Sixty ticks a second into a handler that has nothing to react to is the
    // cost a naive implementation pays forever.
    Fixture fixture;
    fixture.system.dispatchSimTick(*fixture.world, 1);
    (void)rawOf(fixture);
    for (core::u64 tick = 2; tick < 10; ++tick) {
        fixture.system.dispatchSimTick(*fixture.world, tick);
        CHECK(rawOf(fixture).empty());
    }
}

TEST_CASE("losing focus ends everything that was held")
{
    // A handler that pairs `InputBegan` with `InputEnded` must never leak a
    // press. An alt-tab that left W down is how a character keeps walking into a
    // wall with the window in the background.
    Fixture fixture;
    fixture.press("W");
    fixture.press("A");
    fixture.system.dispatchSimTick(*fixture.world, 1);
    REQUIRE(rawOf(fixture).size() == 2);

    fixture.system.releaseAll(*fixture.world);
    std::vector<input::RawInputEvent> raw = rawOf(fixture);
    REQUIRE(raw.size() == 2);
    CHECK(raw[0].phase == input::RawInputEvent::Phase::Ended);
    CHECK(raw[1].phase == input::RawInputEvent::Phase::Ended);
}

TEST_CASE("events come out in KeyCode order, which is an order something promises")
{
    // R10: an observable order has to come from a container that has one. Two
    // keys pressed in the same frame arrive from the OS in whatever order the
    // driver produced, and a replay must not depend on it.
    Fixture fixture;
    fixture.press("Z");
    fixture.press("A");
    fixture.system.dispatchSimTick(*fixture.world, 1);

    std::vector<input::RawInputEvent> raw = rawOf(fixture);
    REQUIRE(raw.size() == 2);
    CHECK(raw[0].keyCode < raw[1].keyCode);
    CHECK(raw[0].keyCode == fixture.keyCode("A"));
}

TEST_CASE("IsKeyDown reads the device and ignores what the UI took")
{
    // The opposite of the events' second argument, deliberately: a poll asks
    // what the hardware is doing. A caller who wants the UI-aware answer wants
    // an `InputAction`, which is also the one that can be rebound.
    Fixture fixture;
    CHECK_FALSE(fixture.system.isKeyDown(fixture.keyCode("Space")));

    fixture.press("Space");
    CHECK(fixture.system.isKeyDown(fixture.keyCode("Space")));
    fixture.system.setKeyboardCapturedByUi(true);
    CHECK(fixture.system.isKeyDown(fixture.keyCode("Space")));

    fixture.release("Space");
    CHECK_FALSE(fixture.system.isKeyDown(fixture.keyCode("Space")));
}

// --- The non-device seam (roadmap M6's design constraint) ---------------------
//
// "An action must be drivable by something that is not a physical device." The
// four virtual channels are that seam, and every case here is about the sentence
// that follows it in the roadmap: it goes through the SAME dispatch, or it is the
// second input model that was declined an hour earlier.

TEST_CASE("a virtual channel drives a Bool action through an ordinary binding")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId jump = fixture.action(context, input::ActionType::Bool);
    const InstanceId binding = fixture.binding(jump);
    fixture.world->inputBindings().find(binding)->keyCode = fixture.keyCode("Virtual1");

    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK_FALSE(fixture.state(jump).pressed);

    // A HUD button pressed. Nothing about the action, the binding or the context
    // is different from a keyboard's -- which is the whole claim.
    fixture.system.setVirtualState(fixture.keyCode("Virtual1"), 1.0f);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(jump).pressed);

    fixture.system.setVirtualState(fixture.keyCode("Virtual1"), 0.0f);
    fixture.system.dispatchSimTick(*fixture.world, 3);
    CHECK_FALSE(fixture.state(jump).pressed);
}

TEST_CASE("a virtual stick drives a Direction2D action, which is why the seam carries a value")
{
    // The easy mistake the roadmap names: design an on-screen BUTTON and the
    // thumbstick -- the other half of any touch scheme -- does not fit later.
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId move = fixture.action(context, input::ActionType::Direction2D);
    const InstanceId binding = fixture.binding(move);
    fixture.world->inputBindings().find(binding)->keyCode = fixture.keyCode("VirtualStick1");

    fixture.system.setVirtualState(fixture.keyCode("Virtual1"), 0.5f);
    fixture.system.setVirtualState(fixture.keyCode("Virtual2"), -0.25f);
    fixture.system.dispatchSimTick(*fixture.world, 1);

    CHECK(static_cast<double>(fixture.state(move).axis.x) == doctest::Approx(0.5));
    // NOT negated: a virtual axis is written in the engine's own convention, so
    // there is no hardware convention to undo.
    CHECK(static_cast<double>(fixture.state(move).axis.y) == doctest::Approx(-0.25));
}

TEST_CASE("a virtual value is eaten by a sinking context, like every other input")
{
    // A menu that stops the character stops the HUD button too. If it did not,
    // the button would be a second input model wearing a different hat.
    Fixture fixture;
    const InstanceId menu = fixture.context(10.0f, true);
    const InstanceId confirm = fixture.action(menu, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(confirm))->keyCode = fixture.keyCode("Virtual1");

    const InstanceId game = fixture.context(0.0f, false);
    const InstanceId jump = fixture.action(game, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(jump))->keyCode = fixture.keyCode("Virtual1");

    fixture.system.setVirtualState(fixture.keyCode("Virtual1"), 1.0f);
    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK(fixture.state(confirm).pressed);
    CHECK_FALSE(fixture.state(jump).pressed);

    // Menu closed: the same value falls through to the game on the next tick.
    fixture.world->inputContexts().find(menu)->enabled = false;
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(jump).pressed);
}

TEST_CASE("a virtual value counts as pressed past half deflection")
{
    // The rule every analogue source here follows, applied to this one so that a
    // slider bound to a `Bool` action behaves like a trigger does.
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId action = fixture.action(context, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(action))->keyCode = fixture.keyCode("Virtual2");

    fixture.system.setVirtualState(fixture.keyCode("Virtual2"), 0.4f);
    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK_FALSE(fixture.state(action).pressed);

    fixture.system.setVirtualState(fixture.keyCode("Virtual2"), 0.6f);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(action).pressed);
}

TEST_CASE("the seam is one-way: nothing else may be written")
{
    // A script writing to `Space` would be pretending to be a keyboard, and
    // nothing downstream could tell the two apart afterwards.
    Fixture fixture;
    CHECK(input::isVirtual(fixture.keyCode("Virtual1")));
    CHECK(input::isVirtual(fixture.keyCode("VirtualStick2")));
    CHECK_FALSE(input::isVirtual(fixture.keyCode("Space")));
    CHECK_FALSE(input::isVirtual(fixture.keyCode("LeftThumbstick")));

    fixture.system.setVirtualState(fixture.keyCode("Space"), 1.0f);
    CHECK_FALSE(fixture.system.isKeyDown(fixture.keyCode("Space")));
}

TEST_CASE("a virtual binding reports the Touch device family")
{
    // The roadmap's clause: when something eventually produces `Touch` it should
    // produce it through this seam rather than growing a new one.
    Fixture fixture;
    CHECK(input::deviceOf(fixture.keyCode("Virtual1")) == input::DeviceType::Touch);
    CHECK(input::deviceOf(fixture.keyCode("VirtualStick1")) == input::DeviceType::Touch);

    fixture.system.setVirtualState(fixture.keyCode("Virtual1"), 1.0f);
    // And a HUD follows the on-screen control the way it follows a gamepad.
    CHECK(fixture.system.snapshot().lastDevice == input::DeviceType::Touch);
}

TEST_CASE("a virtual press produces a raw event, so the replay gate can see it")
{
    // M6's own gate is an obby run replayed headless to the finish flag. A HUD
    // button that reached an action without touching the device snapshot would
    // be an input that gate could not see.
    Fixture fixture;
    fixture.system.dispatchSimTick(*fixture.world, 1);
    (void)fixture.system.drainRawEvents();

    fixture.system.setVirtualState(fixture.keyCode("Virtual1"), 1.0f);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    const std::span<const input::RawInputEvent> raw = fixture.system.drainRawEvents();
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].phase == input::RawInputEvent::Phase::Began);
    CHECK(raw[0].keyCode == fixture.keyCode("Virtual1"));
}

TEST_CASE("losing focus clears a virtual press")
{
    // A HUD button held when the window went away is a button nobody is holding.
    Fixture fixture;
    fixture.system.setVirtualState(fixture.keyCode("Virtual1"), 1.0f);
    CHECK(fixture.system.isKeyDown(fixture.keyCode("Virtual1")));

    fixture.system.releaseAll(*fixture.world);
    CHECK_FALSE(fixture.system.isKeyDown(fixture.keyCode("Virtual1")));
}

TEST_CASE("D443: there are sixteen virtual keys, and the twelve after the fourth are keys like the first")
{
    // An action game's HUD on a phone: a stick, a jump, and a skill bar. Four
    // channels were the stick and two buttons, and the fifth button had
    // nothing to write to.
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId skill = fixture.action(context, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(skill))->keyCode = fixture.keyCode("Virtual16");
    const InstanceId dodge = fixture.action(context, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(dodge))->keyCode = fixture.keyCode("Virtual5");

    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK_FALSE(fixture.state(skill).pressed);

    fixture.system.setVirtualState(fixture.keyCode("Virtual16"), 1.0f);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(skill).pressed);
    // Its own key: the one beside it is not down.
    CHECK_FALSE(fixture.state(dodge).pressed);

    fixture.system.setVirtualState(fixture.keyCode("Virtual16"), 0.0f);
    fixture.system.setVirtualState(fixture.keyCode("Virtual5"), 1.0f);
    fixture.system.dispatchSimTick(*fixture.world, 3);
    CHECK_FALSE(fixture.state(skill).pressed);
    CHECK(fixture.state(dodge).pressed);

    // Named, so a recorded stream carries them; virtual, so a script may
    // write them and they report the touch family; and where a game that
    // compares codes by value left the old ones.
    int count = 0;
    for (int number = 1; number <= 16; ++number) {
        const std::string name = "Virtual" + std::to_string(number);
        CAPTURE(name);
        const core::i32 code = fixture.keyCode(name.c_str());
        CHECK(input::keyCodeFromName(name) == code);
        CHECK(input::keyCodeName(code) == name);
        CHECK(input::isVirtual(code));
        CHECK(input::deviceOf(code) == input::DeviceType::Touch);
        ++count;
    }
    CHECK(count == 16);
    CHECK(fixture.keyCode("Virtual1") == 97);
    CHECK(fixture.keyCode("Virtual4") == 100);
    CHECK(fixture.keyCode("Minus") == 103);
    CHECK(fixture.keyCode("Virtual5") == 136);
}

TEST_CASE("the virtual codes round-trip through their names")
{
    // The recorded stream is written in names, so a code with no name is a code
    // a replay cannot carry -- which would be exactly the gap this seam exists
    // to close.
    Fixture fixture;
    for (const char* name : {"Virtual1", "Virtual2", "Virtual3", "Virtual4", "VirtualStick1", "VirtualStick2"}) {
        CAPTURE(name);
        const core::i32 code = fixture.keyCode(name);
        CHECK(input::keyCodeFromName(name) == code);
        CHECK(input::keyCodeName(code) == name);
    }
}

namespace {

[[nodiscard]] platform::Event finger(platform::EventType type, core::u64 id, float x, float y)
{
    platform::Event event;
    event.type = type;
    event.fingerId = id;
    event.pointerX = x;
    event.pointerY = y;
    return event;
}

} // namespace

TEST_CASE("two fingers begin, move and end on their own, each with its TouchId")
{
    Fixture fixture;
    fixture.system.dispatchSimTick(*fixture.world, 1);
    (void)rawOf(fixture);

    const platform::Event down[] = {finger(platform::EventType::FingerDown, 70, 100.0f, 900.0f),
                                    finger(platform::EventType::FingerDown, 71, 2000.0f, 900.0f)};
    fixture.system.pumpFrame(down);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    std::vector<input::RawInputEvent> began = rawOf(fixture);
    REQUIRE(began.size() == 2);
    CHECK(began[0].phase == input::RawInputEvent::Phase::Began);
    CHECK(began[0].userInputType == input::UserInputType::Touch);
    CHECK(began[0].touchId == 1);
    CHECK(began[0].position.x == 100.0f);
    CHECK(began[1].touchId == 2);
    CHECK(fixture.system.snapshot().lastDevice == input::DeviceType::Touch);

    // The first slides; the second is held still and says nothing.
    const platform::Event slide[] = {finger(platform::EventType::FingerMoved, 70, 160.0f, 900.0f)};
    fixture.system.pumpFrame(slide);
    fixture.system.dispatchSimTick(*fixture.world, 3);
    std::vector<input::RawInputEvent> moved = rawOf(fixture);
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].phase == input::RawInputEvent::Phase::Changed);
    CHECK(moved[0].touchId == 1);
    CHECK(moved[0].delta.x == 60.0f);

    const platform::Event lift[] = {finger(platform::EventType::FingerUp, 71, 2000.0f, 900.0f)};
    fixture.system.pumpFrame(lift);
    fixture.system.dispatchSimTick(*fixture.world, 4);
    std::vector<input::RawInputEvent> ended = rawOf(fixture);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].phase == input::RawInputEvent::Phase::Ended);
    CHECK(ended[0].touchId == 2);
}

TEST_CASE("D444: a finger that came down on the interface says so, from its Began to its Ended")
{
    // A game that aims by tapping the world and has buttons on the screen:
    // a click on a button said `processed`, and a tap on the same button did
    // not -- so every tap on the HUD also fired at whatever was behind it.
    Fixture fixture;
    fixture.system.dispatchSimTick(*fixture.world, 1);
    (void)rawOf(fixture);

    // Two fingers; the host says the first landed on the interface.
    const platform::Event down[] = {finger(platform::EventType::FingerDown, 70, 100.0f, 900.0f),
                                    finger(platform::EventType::FingerDown, 71, 2000.0f, 900.0f)};
    fixture.system.pumpFrame(down);
    fixture.system.setFingerTakenByUi(70);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    std::vector<input::RawInputEvent> began = rawOf(fixture);
    REQUIRE(began.size() == 2);
    CHECK(began[0].touchId == 1);
    CHECK(began[0].uiConsumed);
    CHECK_FALSE(began[1].uiConsumed);

    // Its drag is the interface's too -- a slider under a thumb.
    const platform::Event slide[] = {finger(platform::EventType::FingerMoved, 70, 160.0f, 900.0f),
                                     finger(platform::EventType::FingerMoved, 71, 2010.0f, 900.0f)};
    fixture.system.pumpFrame(slide);
    fixture.system.dispatchSimTick(*fixture.world, 3);
    std::vector<input::RawInputEvent> moved = rawOf(fixture);
    REQUIRE(moved.size() == 2);
    CHECK(moved[0].uiConsumed);
    CHECK_FALSE(moved[1].uiConsumed);

    const platform::Event lift[] = {finger(platform::EventType::FingerUp, 70, 160.0f, 900.0f)};
    fixture.system.pumpFrame(lift);
    fixture.system.dispatchSimTick(*fixture.world, 4);
    std::vector<input::RawInputEvent> ended = rawOf(fixture);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].phase == input::RawInputEvent::Phase::Ended);
    CHECK(ended[0].uiConsumed);

    // **The claim was the press's, not the finger's**: the same finger down
    // again, on the world this time, is the game's.
    const platform::Event again[] = {finger(platform::EventType::FingerDown, 70, 500.0f, 500.0f)};
    fixture.system.pumpFrame(again);
    fixture.system.dispatchSimTick(*fixture.world, 5);
    std::vector<input::RawInputEvent> second = rawOf(fixture);
    REQUIRE(second.size() == 1);
    CHECK(second[0].phase == input::RawInputEvent::Phase::Began);
    CHECK_FALSE(second[0].uiConsumed);

    // A tap shorter than a frame, on a button: both of its events say so.
    const platform::Event tap[] = {finger(platform::EventType::FingerDown, 9, 50.0f, 50.0f),
                                   finger(platform::EventType::FingerUp, 9, 50.0f, 50.0f)};
    fixture.system.pumpFrame(tap);
    fixture.system.setFingerTakenByUi(9);
    fixture.system.dispatchSimTick(*fixture.world, 6);
    std::vector<input::RawInputEvent> tapBegan = rawOf(fixture);
    REQUIRE(tapBegan.size() == 1);
    CHECK(tapBegan[0].uiConsumed);
    fixture.system.pumpFrame({});
    fixture.system.dispatchSimTick(*fixture.world, 7);
    std::vector<input::RawInputEvent> tapEnded = rawOf(fixture);
    REQUIRE(tapEnded.size() == 1);
    CHECK(tapEnded[0].phase == input::RawInputEvent::Phase::Ended);
    CHECK(tapEnded[0].uiConsumed);
}

TEST_CASE("a tap shorter than a frame still begins and ends")
{
    Fixture fixture;
    fixture.system.dispatchSimTick(*fixture.world, 1);
    (void)rawOf(fixture);

    const platform::Event tap[] = {finger(platform::EventType::FingerDown, 9, 50.0f, 50.0f),
                                   finger(platform::EventType::FingerUp, 9, 50.0f, 50.0f)};
    fixture.system.pumpFrame(tap);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    std::vector<input::RawInputEvent> began = rawOf(fixture);
    REQUIRE(began.size() == 1);
    CHECK(began[0].phase == input::RawInputEvent::Phase::Began);
    fixture.system.dispatchSimTick(*fixture.world, 3);
    std::vector<input::RawInputEvent> ended = rawOf(fixture);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].phase == input::RawInputEvent::Phase::Ended);
}

TEST_CASE("the mouse a system makes out of a finger is not a second input")
{
    Fixture fixture;
    fixture.system.dispatchSimTick(*fixture.world, 1);
    (void)rawOf(fixture);
    platform::Event synthetic = mouseButton(platform::EventType::MouseButtonDown);
    synthetic.fromTouch = true;
    const platform::Event events[] = {finger(platform::EventType::FingerDown, 3, 10.0f, 10.0f), synthetic};
    fixture.system.pumpFrame(events);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    std::vector<input::RawInputEvent> raw = rawOf(fixture);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].userInputType == input::UserInputType::Touch);
    CHECK(fixture.system.snapshot().lastDevice == input::DeviceType::Touch);
}

TEST_CASE("punctuation and the keypad are KeyCodes, appended after everything a game already held")
{
    // **D210.** A game could not bind a comma, a slash or the keypad at all.
    Fixture fixture;
    for (const char* name :
         {"Minus",       "Equals",     "LeftBracket", "RightBracket", "Backslash",    "Semicolon",    "Quote",
          "Backquote",   "Comma",      "Period",      "Slash",        "CapsLock",     "Insert",       "PageUp",
          "PageDown",    "NumLock",    "Keypad0",     "Keypad9",      "KeypadPeriod", "KeypadDivide", "KeypadMultiply",
          "KeypadMinus", "KeypadPlus", "KeypadEnter", "KeypadEquals"}) {
        CAPTURE(name);
        const core::i32 code = fixture.keyCode(name);
        CHECK(code > fixture.keyCode("VirtualStick2"));
        CHECK(input::keyCodeFromName(name) == code);
        CHECK(input::keyCodeName(code) == name);
        CHECK(input::deviceOf(code) == input::DeviceType::KeyboardMouse);
    }
    // Nothing that was there moved.
    CHECK(fixture.keyCode("Delete") == 66);
    CHECK(fixture.keyCode("VirtualStick2") == 102);
    CHECK(fixture.keyCode("Minus") == 103);
}

// --- Gestures (D462) -------------------------------------------------------------
//
// "I swiped right, up, down, left -- the engine has to support this." Every
// game on a phone had written its own recogniser. These feed fingers and read
// what the dispatch says they did.

namespace {

using Gesture = input::GestureEvent;

// One tick with these events in it, and the gestures it recognised.
[[nodiscard]] std::vector<Gesture> gesturesOf(Fixture& fixture, std::span<const platform::Event> events, core::u64 tick)
{
    fixture.system.pumpFrame(events);
    fixture.system.dispatchSimTick(*fixture.world, tick);
    const std::span<const Gesture> made = fixture.system.drainGestures();
    return {made.begin(), made.end()};
}

[[nodiscard]] const Gesture* firstOf(const std::vector<Gesture>& gestures, Gesture::Kind kind)
{
    for (const Gesture& gesture : gestures) {
        if (gesture.kind == kind)
            return &gesture;
    }
    return nullptr;
}

[[nodiscard]] int countOf(const std::vector<Gesture>& gestures, Gesture::Kind kind)
{
    int count = 0;
    for (const Gesture& gesture : gestures)
        count += gesture.kind == kind ? 1 : 0;
    return count;
}

constexpr core::i32 Up = 0;
constexpr core::i32 Left = 2;
constexpr core::i32 Right = 3;

} // namespace

TEST_CASE("D462: a finger that travels the threshold is a swipe while it is still down, and a key for one tick")
{
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId slide = fixture.action(context, input::ActionType::Bool);
    fixture.world->inputBindings().find(fixture.binding(slide))->keyCode = fixture.keyCode("SwipeRight");
    core::u64 tick = 0;
    (void)gesturesOf(fixture, {}, ++tick);

    // Down, and a little way: nothing yet. The threshold is 38 pixels here.
    const platform::Event down[] = {finger(platform::EventType::FingerDown, 5, 100.0f, 500.0f)};
    CHECK(countOf(gesturesOf(fixture, down, ++tick), Gesture::Kind::Swipe) == 0);
    const platform::Event nudge[] = {finger(platform::EventType::FingerMoved, 5, 120.0f, 500.0f)};
    CHECK(countOf(gesturesOf(fixture, nudge, ++tick), Gesture::Kind::Swipe) == 0);
    CHECK_FALSE(fixture.state(slide).pressed);

    // Past it, still down: a swipe to the right, from where the stroke began.
    const platform::Event across[] = {finger(platform::EventType::FingerMoved, 5, 150.0f, 505.0f)};
    const std::vector<Gesture> first = gesturesOf(fixture, across, ++tick);
    REQUIRE(countOf(first, Gesture::Kind::Swipe) == 1);
    const Gesture* swipe = firstOf(first, Gesture::Kind::Swipe);
    CHECK(swipe->direction == Right);
    CHECK(swipe->position.x == 100.0f);
    CHECK(swipe->position.y == 500.0f);
    CHECK(swipe->fingers == 1);
    // And the action bound to it is pressed -- for this tick.
    CHECK(fixture.state(slide).pressed);
    CHECK(gesturesOf(fixture, {}, ++tick).empty());
    CHECK_FALSE(fixture.state(slide).pressed);

    // **One long drag one way is one swipe**, however far it goes.
    for (float x = 190.0f; x <= 600.0f; x += 41.0f) {
        const platform::Event further[] = {finger(platform::EventType::FingerMoved, 5, x, 505.0f)};
        CHECK(countOf(gesturesOf(fixture, further, ++tick), Gesture::Kind::Swipe) == 0);
    }
    CHECK_FALSE(fixture.state(slide).pressed);

    // A change of direction is the next, measured from where the finger is.
    const platform::Event turn[] = {finger(platform::EventType::FingerMoved, 5, 600.0f, 460.0f)};
    const std::vector<Gesture> second = gesturesOf(fixture, turn, ++tick);
    REQUIRE(countOf(second, Gesture::Kind::Swipe) == 1);
    CHECK(firstOf(second, Gesture::Kind::Swipe)->direction == Up);
    CHECK(firstOf(second, Gesture::Kind::Swipe)->position.x == 600.0f);
    // And back the way it came: left.
    const platform::Event back[] = {finger(platform::EventType::FingerMoved, 5, 550.0f, 460.0f)};
    const std::vector<Gesture> third = gesturesOf(fixture, back, ++tick);
    REQUIRE(countOf(third, Gesture::Kind::Swipe) == 1);
    CHECK(firstOf(third, Gesture::Kind::Swipe)->direction == Left);

    // Lifted after all that: not a tap.
    const platform::Event lift[] = {finger(platform::EventType::FingerUp, 5, 550.0f, 460.0f)};
    CHECK(countOf(gesturesOf(fixture, lift, ++tick), Gesture::Kind::Tap) == 0);
    CHECK(countOf(gesturesOf(fixture, {}, ++tick), Gesture::Kind::Tap) == 0);

    // The names, for a recorded stream and a rebinding screen.
    CHECK(input::keyCodeName(fixture.keyCode("SwipeLeft")) == "SwipeLeft");
    CHECK(input::keyCodeFromName("SwipeUp") == fixture.keyCode("SwipeUp"));
    CHECK(input::deviceOf(fixture.keyCode("SwipeDown")) == input::DeviceType::Touch);
}

TEST_CASE(
    "D462: a flick shorter than a frame is a swipe, a touch that goes nowhere is a tap, and one held is a long press")
{
    Fixture fixture;
    core::u64 tick = 0;
    (void)gesturesOf(fixture, {}, ++tick);

    // Down, across and up between two ticks: both of its ends are known.
    const platform::Event flick[] = {finger(platform::EventType::FingerDown, 3, 400.0f, 300.0f),
                                     finger(platform::EventType::FingerMoved, 3, 330.0f, 300.0f),
                                     finger(platform::EventType::FingerUp, 3, 300.0f, 300.0f)};
    const std::vector<Gesture> flicked = gesturesOf(fixture, flick, ++tick);
    REQUIRE(countOf(flicked, Gesture::Kind::Swipe) == 1);
    CHECK(firstOf(flicked, Gesture::Kind::Swipe)->direction == Left);
    CHECK(firstOf(flicked, Gesture::Kind::Swipe)->position.x == 400.0f);
    CHECK(countOf(gesturesOf(fixture, {}, ++tick), Gesture::Kind::Tap) == 0);

    // A tap: down and up where it landed.
    const platform::Event tap[] = {finger(platform::EventType::FingerDown, 4, 50.0f, 60.0f),
                                   finger(platform::EventType::FingerUp, 4, 52.0f, 61.0f)};
    CHECK(gesturesOf(fixture, tap, ++tick).empty());
    const std::vector<Gesture> tapped = gesturesOf(fixture, {}, ++tick);
    REQUIRE(countOf(tapped, Gesture::Kind::Tap) == 1);
    CHECK(firstOf(tapped, Gesture::Kind::Tap)->position.x == 52.0f);

    // Held for half a second: a long press, once, and lifting it is no tap.
    const platform::Event press[] = {finger(platform::EventType::FingerDown, 6, 700.0f, 200.0f)};
    int longPresses = countOf(gesturesOf(fixture, press, ++tick), Gesture::Kind::LongPress);
    for (int held = 0; held < 60; ++held)
        longPresses += countOf(gesturesOf(fixture, {}, ++tick), Gesture::Kind::LongPress);
    CHECK(longPresses == 1);
    const platform::Event release[] = {finger(platform::EventType::FingerUp, 6, 700.0f, 200.0f)};
    CHECK(countOf(gesturesOf(fixture, release, ++tick), Gesture::Kind::Tap) == 0);
    CHECK(countOf(gesturesOf(fixture, {}, ++tick), Gesture::Kind::Tap) == 0);
}

TEST_CASE("D462: a press the interface took starts no gesture, and the mouse swipes as a finger does")
{
    Fixture fixture;
    core::u64 tick = 0;
    (void)gesturesOf(fixture, {}, ++tick);

    // On a button: dragged right across the screen, and nothing is said.
    const platform::Event down[] = {finger(platform::EventType::FingerDown, 8, 100.0f, 100.0f)};
    fixture.system.pumpFrame(down);
    fixture.system.setFingerTakenByUi(8);
    fixture.system.dispatchSimTick(*fixture.world, ++tick);
    const platform::Event drag[] = {finger(platform::EventType::FingerMoved, 8, 400.0f, 100.0f)};
    CHECK(gesturesOf(fixture, drag, ++tick).empty());
    const platform::Event up[] = {finger(platform::EventType::FingerUp, 8, 400.0f, 100.0f)};
    CHECK(gesturesOf(fixture, up, ++tick).empty());
    CHECK(gesturesOf(fixture, {}, ++tick).empty());

    // The mouse, with its left button down, on the world.
    platform::Event place;
    place.type = platform::EventType::MouseMoved;
    place.pointerX = 200.0f;
    place.pointerY = 200.0f;
    const platform::Event press[] = {place, mouseButton(platform::EventType::MouseButtonDown)};
    CHECK(countOf(gesturesOf(fixture, press, ++tick), Gesture::Kind::Swipe) == 0);
    platform::Event moved = place;
    moved.pointerY = 260.0f;
    moved.pointerDeltaY = 60.0f;
    const platform::Event pull[] = {moved};
    const std::vector<Gesture> dragged = gesturesOf(fixture, pull, ++tick);
    REQUIRE(countOf(dragged, Gesture::Kind::Swipe) == 1);
    CHECK(firstOf(dragged, Gesture::Kind::Swipe)->direction == 1);
    // And a drag is a pan: sixty pixels down the window.
    const Gesture* pan = firstOf(dragged, Gesture::Kind::Pan);
    REQUIRE(pan != nullptr);
    CHECK(pan->delta.y == 60.0f);
    CHECK(pan->fingers == 1);
    // Moved with no button down: nothing.
    const platform::Event release[] = {mouseButton(platform::EventType::MouseButtonUp)};
    (void)gesturesOf(fixture, release, ++tick);
    moved.pointerY = 400.0f;
    const platform::Event hover[] = {moved};
    CHECK(gesturesOf(fixture, hover, ++tick).empty());
}

TEST_CASE("D462: two fingers apart are a pinch by how far, and a swipe's length is the host's to say")
{
    Fixture fixture;
    core::u64 tick = 0;
    (void)gesturesOf(fixture, {}, ++tick);

    const platform::Event down[] = {finger(platform::EventType::FingerDown, 1, 300.0f, 300.0f),
                                    finger(platform::EventType::FingerDown, 2, 400.0f, 300.0f)};
    CHECK(countOf(gesturesOf(fixture, down, ++tick), Gesture::Kind::Pinch) == 0);
    // The second twice as far from the first: a scale of two, about the middle.
    const platform::Event apart[] = {finger(platform::EventType::FingerMoved, 2, 500.0f, 300.0f)};
    const std::vector<Gesture> opened = gesturesOf(fixture, apart, ++tick);
    const Gesture* pinch = firstOf(opened, Gesture::Kind::Pinch);
    REQUIRE(pinch != nullptr);
    CHECK(static_cast<double>(pinch->scale) == doctest::Approx(2.0));
    CHECK(pinch->position.x == 400.0f);
    // Held still: nothing more is said.
    CHECK(countOf(gesturesOf(fixture, {}, ++tick), Gesture::Kind::Pinch) == 0);
    // Together again, to half of where they began.
    const platform::Event closed[] = {finger(platform::EventType::FingerMoved, 2, 350.0f, 300.0f)};
    const Gesture* closing = nullptr;
    const std::vector<Gesture> shut = gesturesOf(fixture, closed, ++tick);
    closing = firstOf(shut, Gesture::Kind::Pinch);
    REQUIRE(closing != nullptr);
    CHECK(static_cast<double>(closing->scale) == doctest::Approx(0.5));
    const platform::Event lift[] = {finger(platform::EventType::FingerUp, 1, 300.0f, 300.0f),
                                    finger(platform::EventType::FingerUp, 2, 350.0f, 300.0f)};
    (void)gesturesOf(fixture, lift, ++tick);
    (void)gesturesOf(fixture, {}, ++tick);

    // **A longer swipe**: on a phone six millimetres is four times the pixels,
    // and the same forty that were a swipe are a nudge.
    fixture.system.setSwipeThreshold(150.0f);
    const platform::Event again[] = {finger(platform::EventType::FingerDown, 9, 100.0f, 100.0f)};
    (void)gesturesOf(fixture, again, ++tick);
    const platform::Event forty[] = {finger(platform::EventType::FingerMoved, 9, 140.0f, 100.0f)};
    CHECK(countOf(gesturesOf(fixture, forty, ++tick), Gesture::Kind::Swipe) == 0);
    const platform::Event far[] = {finger(platform::EventType::FingerMoved, 9, 260.0f, 100.0f)};
    CHECK(countOf(gesturesOf(fixture, far, ++tick), Gesture::Kind::Swipe) == 1);
}

TEST_CASE("NA25: a key pressed and let go between two ticks is down for one of them")
{
    // At thirty frames a second -- ten in the background -- a quick tap fell
    // between two ticks and was never down at either.
    Fixture fixture;
    const InstanceId context = fixture.context();
    const InstanceId jump = fixture.action(context, input::ActionType::Bool);
    const InstanceId binding = fixture.binding(jump);
    fixture.world->inputBindings().find(binding)->keyCode = fixture.keyCode("Space");
    fixture.system.dispatchSimTick(*fixture.world, 1);
    (void)fixture.drainEvents();

    fixture.press("Space");
    fixture.release("Space");
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(jump).pressed);
    CHECK(fixture.drainEvents() == std::vector<std::string>{"Pressed", "StateChanged"});
    fixture.system.dispatchSimTick(*fixture.world, 3);
    CHECK_FALSE(fixture.state(jump).pressed);
    CHECK(fixture.drainEvents() == std::vector<std::string>{"Released", "StateChanged"});

    // A key held across ticks is let go when it is let go, not a tick later.
    fixture.press("Space");
    fixture.system.dispatchSimTick(*fixture.world, 4);
    (void)fixture.drainEvents();
    fixture.release("Space");
    fixture.system.dispatchSimTick(*fixture.world, 5);
    CHECK_FALSE(fixture.state(jump).pressed);
}

TEST_CASE("local gamepad contexts isolate sticks and sinking while aggregate input remains compatible")
{
    Fixture fixture;
    const auto high = fixture.context(10, true);
    const auto second = fixture.context(0, false);
    const auto aggregate = fixture.context(-1, false);
    fixture.world->inputContexts().find(high)->gamepadId = 11;
    fixture.world->inputContexts().find(second)->gamepadId = 22;
    const auto actionFor = [&](InstanceId context) {
        const auto action = fixture.action(context, input::ActionType::Direction2D);
        const auto binding = fixture.binding(action);
        fixture.world->inputBindings().find(binding)->keyCode = fixture.keyCode("LeftThumbstick");
        return action;
    };
    const auto one = actionFor(high);
    const auto two = actionFor(second);
    const auto all = actionFor(aggregate);
    input::DeviceState snapshot;
    input::GamepadSnapshot a;
    a.id = 11;
    a.axes[static_cast<core::usize>(platform::GamepadAxis::LeftX)] = 0.9f;
    input::GamepadSnapshot b;
    b.id = 22;
    b.axes[static_cast<core::usize>(platform::GamepadAxis::LeftX)] = -0.5f;
    snapshot.gamepads = {a, b};
    fixture.system.setSnapshot(snapshot);
    fixture.system.dispatchSimTick(*fixture.world, 1);
    CHECK(fixture.state(one).axis.x == doctest::Approx(0.9));
    CHECK(fixture.state(two).axis.x == doctest::Approx(-0.5));
    CHECK(fixture.state(all).axis.x == doctest::Approx(-0.5));
    fixture.world->inputContexts().find(high)->gamepadId = 99;
    fixture.system.dispatchSimTick(*fixture.world, 2);
    CHECK(fixture.state(one).axis.x == 0.0f);
    CHECK(fixture.state(two).axis.x == doctest::Approx(-0.5));
    CHECK(fixture.state(all).axis.x == doctest::Approx(0.9));
}

TEST_CASE("four controllers preserve simultaneous presses short taps disconnects and snapshot replay")
{
    Fixture fixture;
    std::vector<platform::Event> events;
    for (core::u32 id : {40u, 10u, 30u, 20u}) {
        platform::Event event;
        event.type = platform::EventType::GamepadButtonDown;
        event.gamepadId = id;
        event.gamepadButton = platform::GamepadButton::South;
        events.push_back(event);
    }
    fixture.system.pumpFrame(events);
    fixture.system.dispatchSimTick(*fixture.world, 1);
    auto raw = fixture.system.drainRawEvents();
    REQUIRE(raw.size() == 4);
    for (core::usize at = 0; at < raw.size(); ++at) {
        CHECK(raw[at].gamepadId == (at + 1) * 10);
        CHECK(raw[at].phase == input::RawInputEvent::Phase::Began);
    }
    const auto recorded = fixture.system.snapshot();
    Fixture replay;
    replay.system.setSnapshot(recorded);
    replay.system.dispatchSimTick(*replay.world, 1);
    const auto replayed = replay.system.drainRawEvents();
    REQUIRE(replayed.size() == raw.size());
    for (core::usize at = 0; at < raw.size(); ++at) {
        CHECK(replayed[at].gamepadId == raw[at].gamepadId);
        CHECK(replayed[at].keyCode == raw[at].keyCode);
    }
    events.resize(1);
    events[0].type = platform::EventType::GamepadRemoved;
    events[0].gamepadId = 20;
    fixture.system.pumpFrame(events);
    fixture.system.dispatchSimTick(*fixture.world, 2);
    raw = fixture.system.drainRawEvents();
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].gamepadId == 20);
    CHECK(raw[0].phase == input::RawInputEvent::Phase::Ended);
    CHECK(fixture.system.isGamepadKeyDown(10, fixture.keyCode("ButtonSouth")));
    CHECK_FALSE(fixture.system.isGamepadKeyDown(20, fixture.keyCode("ButtonSouth")));
    CHECK_FALSE(fixture.system.isGamepadKeyDown(10, fixture.keyCode("Space")));
    events[0].gamepadId = 50;
    events[0].type = platform::EventType::GamepadButtonDown;
    events.push_back(events[0]);
    events[1].type = platform::EventType::GamepadButtonUp;
    fixture.system.pumpFrame(events);
    CHECK_FALSE(fixture.system.isGamepadKeyDown(50, fixture.keyCode("ButtonSouth")));
    fixture.system.dispatchSimTick(*fixture.world, 3);
    raw = fixture.system.drainRawEvents();
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].gamepadId == 50);
    CHECK(raw[0].phase == input::RawInputEvent::Phase::Began);
    fixture.system.dispatchSimTick(*fixture.world, 4);
    raw = fixture.system.drainRawEvents();
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].gamepadId == 50);
    CHECK(raw[0].phase == input::RawInputEvent::Phase::Ended);
    fixture.system.releaseAll(*fixture.world);
    CHECK_FALSE(fixture.system.isGamepadKeyDown(10, fixture.keyCode("ButtonSouth")));
}
