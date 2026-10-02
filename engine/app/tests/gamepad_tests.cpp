// A gamepad, from the device to the game (D476).
//
// For the whole of v1 and v2 the platform library was built without a joystick
// subsystem: the engine opened every gamepad it was told about, and was told
// about none. Nothing failed, because every gamepad test handed the input
// layer an event it had made itself -- which proves the layer above the device
// and says nothing about whether there is a device.
//
// So this goes the whole way, on a machine with no hardware: a VIRTUAL gamepad
// the platform library hosts is plugged in, a button on it is pressed, and what
// is asserted is that an `InputAction` bound to that button says so. Every link
// is the one a real controller goes through -- the library's joystick driver,
// its gamepad mapping, the platform's event pump, the Input Action System --
// and a build in which any of them is missing fails here.
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/input/input.h"
#include "engine/input/scene_types.h"
#include "engine/platform/event.h"
#include "engine/platform/platform.h"
#include "engine/scene/world.h"

namespace {

namespace core = engine::core;
namespace input = engine::input;
namespace platform = engine::platform;
namespace scene = engine::scene;

using core::InstanceId;

struct GamepadRig
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    input::InputSystem system;
    core::u32 pad = 0;
    bool started = false;
    core::u64 tick = 0;

    GamepadRig()
    {
        scene::generated::registerClasses(classes, atoms);
        input::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);

        const auto error = platform::init({.headless = true});
        REQUIRE_MESSAGE(!error.has_value(), (error ? error->detail : std::string{}));
        started = true;
        pad = platform::attachVirtualGamepad();
        // The library announces a device and the platform opens it, as it
        // does one that was plugged in.
        pump();
    }

    ~GamepadRig()
    {
        platform::stopVibration();
        platform::detachVirtualGamepad(pad);
        if (started)
            platform::shutdown();
    }

    GamepadRig(const GamepadRig&) = delete;
    GamepadRig& operator=(const GamepadRig&) = delete;

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

    InstanceId make(const char* className, InstanceId parent = {})
    {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != scene::InvalidClass);
        const InstanceId made = world->create(id);
        if (parent.valid())
            REQUIRE_FALSE(world->setParent(made, parent).has_value());
        return made;
    }

    // An action of `type` bound to one key code, in a context of its own.
    InstanceId action(input::ActionType type, const char* key)
    {
        const InstanceId context = make("InputContext");
        const InstanceId made = make("InputAction", context);
        world->inputActions().find(made)->type = static_cast<core::i32>(type);
        const InstanceId binding = make("InputBinding", made);
        world->inputBindings().find(binding)->keyCode = keyCode(key);
        return made;
    }

    // One frame and one tick: the platform's events, as the engine's own loop
    // hands them over.
    std::vector<platform::Event> pump()
    {
        const std::span<const platform::Event> events = platform::pumpEvents();
        std::vector<platform::Event> seen(events.begin(), events.end());
        system.pumpFrame(events);
        system.dispatchSimTick(*world, ++tick);
        return seen;
    }

    [[nodiscard]] const scene::InputActionComponent& state(InstanceId id) const
    {
        const scene::InputActionComponent* component = world->inputActions().find(id);
        REQUIRE(component != nullptr);
        return *component;
    }
};

[[nodiscard]] bool saw(const std::vector<platform::Event>& events, platform::EventType type)
{
    for (const platform::Event& event : events) {
        if (event.type == type)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("D476: the platform library is built with gamepads, and can be handed one")
{
    GamepadRig rig;
    CHECK(platform::gamepadsAvailable());
    // Zero is what a library with no joystick driver answers.
    CHECK(rig.pad != 0);
}

TEST_CASE("D476: a button pressed on a device reaches the action bound to it")
{
    GamepadRig rig;
    REQUIRE(rig.pad != 0);
    const InstanceId jump = rig.action(input::ActionType::Bool, "ButtonSouth");
    (void)rig.pump();
    CHECK_FALSE(rig.state(jump).pressed);

    platform::setVirtualGamepadButton(rig.pad, platform::GamepadButton::South, true);
    const std::vector<platform::Event> down = rig.pump();
    CHECK(saw(down, platform::EventType::GamepadButtonDown));
    CHECK(rig.state(jump).pressed);
    // The last device used is the gamepad: what a HUD reads to show a button
    // instead of a key.
    CHECK(rig.system.snapshot().lastDevice == input::DeviceType::Gamepad);

    platform::setVirtualGamepadButton(rig.pad, platform::GamepadButton::South, false);
    const std::vector<platform::Event> up = rig.pump();
    CHECK(saw(up, platform::EventType::GamepadButtonUp));
    CHECK_FALSE(rig.state(jump).pressed);

    // And another button is another button.
    platform::setVirtualGamepadButton(rig.pad, platform::GamepadButton::East, true);
    (void)rig.pump();
    CHECK_FALSE(rig.state(jump).pressed);
}

TEST_CASE("D476: a stick pushed on a device reaches the axis bound to it")
{
    GamepadRig rig;
    REQUIRE(rig.pad != 0);
    const InstanceId steer = rig.action(input::ActionType::Direction1D, "LeftStickX");
    (void)rig.pump();
    CHECK(static_cast<double>(rig.state(steer).axis.x) == doctest::Approx(0.0));

    platform::setVirtualGamepadAxis(rig.pad, platform::GamepadAxis::LeftX, 0.75f);
    const std::vector<platform::Event> moved = rig.pump();
    CHECK(saw(moved, platform::EventType::GamepadAxisMoved));
    CHECK(static_cast<double>(rig.state(steer).axis.x) == doctest::Approx(0.75).epsilon(0.02));

    platform::setVirtualGamepadAxis(rig.pad, platform::GamepadAxis::LeftX, -1.0f);
    (void)rig.pump();
    CHECK(static_cast<double>(rig.state(steer).axis.x) == doctest::Approx(-1.0).epsilon(0.02));
}

TEST_CASE("D476: a motor's level reaches the device through the platform library itself")
{
    GamepadRig rig;
    REQUIRE(rig.pad != 0);
    // No stand-in: the hardware's own path, into a device that says what it
    // was told.
    platform::setVibrationSink(nullptr);
    platform::setVibrationFocus(true);
    CHECK(platform::vibrationSupported(true));
    CHECK(platform::vibrationMotorSupported(true, platform::VibrationMotor::Large));
    CHECK(platform::vibrationMotorSupported(true, platform::VibrationMotor::RightTrigger));

    platform::setVibrationMotor(platform::VibrationMotor::Large, 0.5f);
    platform::setVibrationMotor(platform::VibrationMotor::Small, 0.25f);
    (void)rig.pump();
    platform::VirtualRumble told = platform::virtualGamepadRumble();
    CHECK(told.calls > 0);
    CHECK(static_cast<double>(told.heavy) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(static_cast<double>(told.light) == doctest::Approx(0.25).epsilon(0.01));

    platform::setVibrationMotor(platform::VibrationMotor::RightTrigger, 1.0f);
    (void)rig.pump();
    told = platform::virtualGamepadRumble();
    CHECK(static_cast<double>(told.rightTrigger) == doctest::Approx(1.0).epsilon(0.01));

    // The window is no longer in front: still, at once.
    platform::setVibrationFocus(false);
    told = platform::virtualGamepadRumble();
    CHECK(told.heavy == 0.0f);
    CHECK(told.light == 0.0f);
    CHECK(told.rightTrigger == 0.0f);
    platform::setVibrationFocus(true);
}
