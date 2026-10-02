// What a gamepad, the arrow keys and the wheel say to the interface (ADR 0128):
// a step on the press, again while held, and nothing at all for a source the
// game has bound in an enabled context.
#include <doctest/doctest.h>
#include <optional>

#include "class_descriptors.gen.h"
#include "engine/app/ui_navigation.h"
#include "engine/input/input.h"
#include "engine/input/scene_types.h"
#include "engine/platform/event.h"
#include "engine/scene/world.h"
#include "engine/ui/ui.h"

using namespace engine;

namespace {

struct NavigationRig
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    app::UiNavigation navigation;

    NavigationRig()
    {
        scene::generated::registerClasses(classes, atoms);
        input::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
    }

    core::InstanceId make(const char* className, core::InstanceId parent = {})
    {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != scene::InvalidClass);
        const core::InstanceId made = world->create(id);
        if (parent.valid())
            REQUIRE_FALSE(world->setParent(made, parent).has_value());
        return made;
    }

    // An action of the game's bound to one key code, in a context of its own.
    core::InstanceId bind(std::string_view key)
    {
        const core::InstanceId context = make("InputContext");
        const core::InstanceId action = make("InputAction", context);
        const core::InstanceId binding = make("InputBinding", action);
        const core::i32 code = input::keyCodeFromName(key);
        REQUIRE(code != 0);
        world->inputBindings().find(binding)->keyCode = code;
        return context;
    }

    // One frame holding `events`, at `now` seconds.
    [[nodiscard]] ui::InteractionInput frame(std::initializer_list<platform::Event> events, core::f64 now = 0.0)
    {
        navigation.beginFrame();
        for (const platform::Event& event : events)
            navigation.feed(event);
        ui::InteractionInput out;
        navigation.fill(*world, out, now);
        return out;
    }
};

[[nodiscard]] platform::Event key(platform::Key which, bool repeat = false)
{
    platform::Event event;
    event.type = platform::EventType::KeyDown;
    event.key = which;
    event.repeat = repeat;
    return event;
}

[[nodiscard]] platform::Event button(platform::GamepadButton which, bool down = true)
{
    platform::Event event;
    event.type = down ? platform::EventType::GamepadButtonDown : platform::EventType::GamepadButtonUp;
    event.gamepadButton = which;
    return event;
}

[[nodiscard]] platform::Event axis(platform::GamepadAxis which, core::f32 value)
{
    platform::Event event;
    event.type = platform::EventType::GamepadAxisMoved;
    event.gamepadAxis = which;
    event.axisValue = value;
    return event;
}

} // namespace

TEST_CASE("an arrow key is one step of the selection, and its repeat another")
{
    NavigationRig rig;
    const ui::InteractionInput down = rig.frame({key(platform::Key::Down)});
    CHECK(down.navigateX == 0);
    CHECK(down.navigateY == 1);

    // Nothing pressed: nothing stepped, though the key is still held.
    CHECK(rig.frame({}).navigateY == 0);
    CHECK(rig.frame({key(platform::Key::Down, true)}).navigateY == 1);
    CHECK(rig.frame({key(platform::Key::Left)}).navigateX == -1);
}

TEST_CASE("Enter and the south button activate, and a held Enter does not again")
{
    NavigationRig rig;
    CHECK(rig.frame({key(platform::Key::Return)}).navigateActivate);
    CHECK_FALSE(rig.frame({key(platform::Key::Return, true)}).navigateActivate);
    CHECK(rig.frame({button(platform::GamepadButton::South)}).navigateActivate);
    CHECK_FALSE(rig.frame({}).navigateActivate);
}

TEST_CASE("a d-pad held steps at once, then after a pause, then steadily")
{
    NavigationRig rig;
    CHECK(rig.frame({button(platform::GamepadButton::DpadRight)}, 10.0).navigateX == 1);
    // Held, inside the pause.
    CHECK(rig.frame({}, 10.2).navigateX == 0);
    CHECK(rig.frame({}, 10.39).navigateX == 0);
    // The pause is over.
    CHECK(rig.frame({}, 10.41).navigateX == 1);
    CHECK(rig.frame({}, 10.45).navigateX == 0);
    CHECK(rig.frame({}, 10.54).navigateX == 1);
    // Let go: no more.
    CHECK(rig.frame({button(platform::GamepadButton::DpadRight, false)}, 10.6).navigateX == 0);
    CHECK(rig.frame({}, 11.5).navigateX == 0);
}

TEST_CASE("a stick pushed is a direction once, and not again until it comes back")
{
    NavigationRig rig;
    CHECK(rig.frame({axis(platform::GamepadAxis::LeftY, 0.3f)}).navigateY == 0);
    CHECK(rig.frame({axis(platform::GamepadAxis::LeftY, 0.9f)}).navigateY == 1);
    // Wobbling near the edge is still the same push.
    CHECK(rig.frame({axis(platform::GamepadAxis::LeftY, 0.55f)}).navigateY == 0);
    CHECK(rig.frame({axis(platform::GamepadAxis::LeftY, 0.95f)}).navigateY == 0);
    // Back to rest and out again: another.
    CHECK(rig.frame({axis(platform::GamepadAxis::LeftY, 0.0f)}).navigateY == 0);
    CHECK(rig.frame({axis(platform::GamepadAxis::LeftY, -0.9f)}).navigateY == -1);
}

TEST_CASE("the shoulder buttons turn a page and the wheel is passed on")
{
    NavigationRig rig;
    CHECK(rig.frame({button(platform::GamepadButton::RightShoulder)}).pageStep == 1);
    CHECK(rig.frame({button(platform::GamepadButton::LeftShoulder)}).pageStep == -1);

    platform::Event wheel;
    wheel.type = platform::EventType::MouseWheel;
    wheel.wheelY = -1.0f;
    const ui::InteractionInput turned = rig.frame({wheel, wheel});
    CHECK(turned.wheel.y == doctest::Approx(-2.0));
    CHECK(rig.frame({}).wheel.y == doctest::Approx(0.0));
}

TEST_CASE("a source the game has bound is the game's, and the interface is not told")
{
    NavigationRig rig;
    // A character walked with the arrows and jumped with the south button.
    const core::InstanceId walking = rig.bind(platform::keyName(platform::Key::Down));
    (void)rig.bind(platform::gamepadButtonName(platform::GamepadButton::South));

    CHECK(app::UiNavigation::boundByGame(*rig.world, input::keyCodeFromName(platform::keyName(platform::Key::Down))));
    CHECK(rig.frame({key(platform::Key::Down)}).navigateY == 0);
    CHECK_FALSE(rig.frame({button(platform::GamepadButton::South)}).navigateActivate);
    // What it did not bind is still the interface's.
    CHECK(rig.frame({key(platform::Key::Right)}).navigateX == 1);
    CHECK(rig.frame({key(platform::Key::Return)}).navigateActivate);
    CHECK(rig.frame({button(platform::GamepadButton::DpadDown)}).navigateY == 1);
    (void)rig.frame({button(platform::GamepadButton::DpadDown, false)});

    // The game opens its menu and turns its play context off: the arrows are
    // the menu's.
    rig.world->inputContexts().find(walking)->enabled = false;
    CHECK(rig.frame({key(platform::Key::Down)}).navigateY == 1);
}

TEST_CASE("a stick bound whole is bound on both its axes")
{
    NavigationRig rig;
    (void)rig.bind("LeftThumbstick");
    CHECK(rig.frame({axis(platform::GamepadAxis::LeftX, 1.0f)}).navigateX == 0);
    CHECK(rig.frame({axis(platform::GamepadAxis::LeftY, 1.0f)}).navigateY == 0);
    // The d-pad is another thing.
    CHECK(rig.frame({button(platform::GamepadButton::DpadLeft)}).navigateX == -1);
}
