#include "engine/app/ui_navigation.h"

#include <cmath>

#include "engine/input/input.h"
#include "engine/scene/world.h"
#include "engine/ui/ui.h"

namespace engine::app {
namespace {

using core::f32;
using core::f64;
using core::i32;
using core::i8;

// How far a stick is pushed before it is a direction, and how far back it
// comes before it is none: two numbers, so a stick resting on the edge of one
// does not chatter.
constexpr f32 StickOn = 0.6f;
constexpr f32 StickOff = 0.4f;
// A held direction steps again after this long, and then this often.
constexpr f64 RepeatDelay = 0.4;
constexpr f64 RepeatEvery = 0.12;

[[nodiscard]] i32 codeOf(platform::Key key) noexcept
{
    return input::keyCodeFromName(platform::keyName(key));
}

[[nodiscard]] i32 codeOf(platform::GamepadButton button) noexcept
{
    return input::keyCodeFromName(platform::gamepadButtonName(button));
}

[[nodiscard]] i32 codeOf(platform::GamepadAxis axis) noexcept
{
    return input::keyCodeFromName(platform::gamepadAxisName(axis));
}

[[nodiscard]] i8 directionOf(f32 value, i8 was) noexcept
{
    const f32 threshold = was != 0 ? StickOff : StickOn;
    if (value > threshold)
        return 1;
    if (value < -threshold)
        return -1;
    return 0;
}

} // namespace

void UiNavigation::beginFrame() noexcept
{
    m_arrows.pressX = m_arrows.pressY = 0;
    m_dpad.pressX = m_dpad.pressY = 0;
    m_stick.pressX = m_stick.pressY = 0;
    m_enter = false;
    m_south = false;
    m_shoulder = 0;
    m_wheel = {};
}

void UiNavigation::letGo() noexcept
{
    *this = UiNavigation{};
}

void UiNavigation::feed(const platform::Event& event) noexcept
{
    switch (event.type) {
    case platform::EventType::KeyDown:
        // A repeat is a step: holding an arrow walks the selection.
        if (event.key == platform::Key::Left)
            m_arrows.pressX = -1;
        else if (event.key == platform::Key::Right)
            m_arrows.pressX = 1;
        else if (event.key == platform::Key::Up)
            m_arrows.pressY = -1;
        else if (event.key == platform::Key::Down)
            m_arrows.pressY = 1;
        else if ((event.key == platform::Key::Return || event.key == platform::Key::KeypadEnter) && !event.repeat)
            m_enter = true;
        break;
    case platform::EventType::GamepadButtonDown:
        if (event.gamepadButton == platform::GamepadButton::DpadLeft)
            m_dpad.pressX = m_dpad.heldX = -1;
        else if (event.gamepadButton == platform::GamepadButton::DpadRight)
            m_dpad.pressX = m_dpad.heldX = 1;
        else if (event.gamepadButton == platform::GamepadButton::DpadUp)
            m_dpad.pressY = m_dpad.heldY = -1;
        else if (event.gamepadButton == platform::GamepadButton::DpadDown)
            m_dpad.pressY = m_dpad.heldY = 1;
        else if (event.gamepadButton == platform::GamepadButton::South)
            m_south = true;
        else if (event.gamepadButton == platform::GamepadButton::LeftShoulder)
            m_shoulder = -1;
        else if (event.gamepadButton == platform::GamepadButton::RightShoulder)
            m_shoulder = 1;
        break;
    case platform::EventType::GamepadButtonUp:
        if (event.gamepadButton == platform::GamepadButton::DpadLeft ||
            event.gamepadButton == platform::GamepadButton::DpadRight)
            m_dpad.heldX = 0;
        else if (event.gamepadButton == platform::GamepadButton::DpadUp ||
                 event.gamepadButton == platform::GamepadButton::DpadDown)
            m_dpad.heldY = 0;
        break;
    case platform::EventType::GamepadAxisMoved: {
        // A stick pushed past the threshold is a press of that direction; y is
        // down on a stick as it is on the screen.
        if (event.gamepadAxis == platform::GamepadAxis::LeftX) {
            m_stickX = event.axisValue;
            const i8 now = directionOf(m_stickX, m_stick.heldX);
            if (now != 0 && now != m_stick.heldX)
                m_stick.pressX = now;
            m_stick.heldX = now;
        }
        else if (event.gamepadAxis == platform::GamepadAxis::LeftY) {
            m_stickY = event.axisValue;
            const i8 now = directionOf(m_stickY, m_stick.heldY);
            if (now != 0 && now != m_stick.heldY)
                m_stick.pressY = now;
            m_stick.heldY = now;
        }
        break;
    }
    case platform::EventType::MouseWheel:
        m_wheel = m_wheel + core::Vec2{event.wheelX, event.wheelY};
        break;
    case platform::EventType::GamepadRemoved:
    case platform::EventType::WindowFocusLost:
        m_dpad = {};
        m_stick = {};
        m_stickX = m_stickY = 0.0f;
        break;
    default:
        break;
    }
}

bool UiNavigation::boundByGame(const scene::World& world, core::i32 keyCode)
{
    if (keyCode == 0)
        return false;
    bool bound = false;
    world.inputBindings().forEach([&](core::InstanceId id, const scene::InputBindingComponent& binding) {
        if (bound)
            return;
        if (binding.keyCode != keyCode && binding.up != keyCode && binding.down != keyCode && binding.left != keyCode &&
            binding.right != keyCode)
            return;
        // A binding counts while its action and its context are on, and both
        // are in the tree: `InputContext` > `InputAction` > `InputBinding`.
        const core::InstanceId action = world.parentOf(id);
        const core::InstanceId context = world.parentOf(action);
        const scene::InputActionComponent* actionState = world.inputActions().find(action);
        const scene::InputContextComponent* contextState = world.inputContexts().find(context);
        bound = actionState != nullptr && actionState->enabled && contextState != nullptr && contextState->enabled &&
                !world.destroyed(context);
    });
    return bound;
}

void UiNavigation::fill(const scene::World& world, ui::InteractionInput& out, core::f64 now) noexcept
{
    const auto free = [&world](i32 code) { return !boundByGame(world, code); };
    // Each way of giving a direction, where the game has not taken it. A stick
    // bound whole -- as `LeftThumbstick` -- is bound on both its axes.
    const bool arrowsX = free(codeOf(platform::Key::Left)) && free(codeOf(platform::Key::Right));
    const bool arrowsY = free(codeOf(platform::Key::Up)) && free(codeOf(platform::Key::Down));
    const bool dpadX =
        free(codeOf(platform::GamepadButton::DpadLeft)) && free(codeOf(platform::GamepadButton::DpadRight));
    const bool dpadY = free(codeOf(platform::GamepadButton::DpadUp)) && free(codeOf(platform::GamepadButton::DpadDown));
    const bool stickWhole = free(input::keyCodeFromName("LeftThumbstick"));
    const bool stickX = stickWhole && free(codeOf(platform::GamepadAxis::LeftX));
    const bool stickY = stickWhole && free(codeOf(platform::GamepadAxis::LeftY));

    i8 stepX = 0;
    i8 stepY = 0;
    if (arrowsX && m_arrows.pressX != 0)
        stepX = m_arrows.pressX;
    if (arrowsY && m_arrows.pressY != 0)
        stepY = m_arrows.pressY;

    // The d-pad and the stick: a press steps at once, and one held steps again
    // after a pause and then steadily, as a held key does.
    const i8 pressX = dpadX && m_dpad.pressX != 0 ? m_dpad.pressX : stickX ? m_stick.pressX : i8{0};
    const i8 pressY = dpadY && m_dpad.pressY != 0 ? m_dpad.pressY : stickY ? m_stick.pressY : i8{0};
    const i8 heldX = dpadX && m_dpad.heldX != 0 ? m_dpad.heldX : stickX ? m_stick.heldX : i8{0};
    const i8 heldY = dpadY && m_dpad.heldY != 0 ? m_dpad.heldY : stickY ? m_stick.heldY : i8{0};
    if (pressX != 0 || pressY != 0) {
        if (stepX == 0)
            stepX = pressX;
        if (stepY == 0)
            stepY = pressY;
        m_repeatAt = now + RepeatDelay;
        m_repeatX = heldX;
        m_repeatY = heldY;
    }
    else if (heldX != 0 || heldY != 0) {
        if (heldX != m_repeatX || heldY != m_repeatY) {
            // Held another way than it was, with no press to say so (one of two
            // held directions let go): the pause starts again.
            m_repeatX = heldX;
            m_repeatY = heldY;
            m_repeatAt = now + RepeatDelay;
        }
        else if (now >= m_repeatAt) {
            if (stepX == 0)
                stepX = heldX;
            if (stepY == 0)
                stepY = heldY;
            m_repeatAt = now + RepeatEvery;
        }
    }
    else {
        m_repeatX = m_repeatY = 0;
    }

    out.navigateX = stepX;
    out.navigateY = stepY;
    out.navigateActivate =
        (m_enter && free(codeOf(platform::Key::Return))) || (m_south && free(codeOf(platform::GamepadButton::South)));
    if (m_shoulder != 0 &&
        free(codeOf(m_shoulder < 0 ? platform::GamepadButton::LeftShoulder : platform::GamepadButton::RightShoulder)))
        out.pageStep = m_shoulder;
    out.wheel = m_wheel;
}

} // namespace engine::app
