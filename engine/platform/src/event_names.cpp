#include "engine/platform/event.h"

namespace engine::platform {
namespace {
struct KeyNaming
{
    Key key;
    std::string_view name;
};

constexpr KeyNaming KeyNames[] = {
    {Key::Escape, "Escape"},
    {Key::F1, "F1"},
    {Key::F2, "F2"},
    {Key::F3, "F3"},
    {Key::F4, "F4"},
    {Key::F5, "F5"},
    {Key::F6, "F6"},
    {Key::F7, "F7"},
    {Key::F8, "F8"},
    {Key::F9, "F9"},
    {Key::F10, "F10"},
    {Key::F11, "F11"},
    {Key::F12, "F12"},
    {Key::A, "A"},
    {Key::B, "B"},
    {Key::C, "C"},
    {Key::D, "D"},
    {Key::E, "E"},
    {Key::F, "F"},
    {Key::G, "G"},
    {Key::H, "H"},
    {Key::I, "I"},
    {Key::J, "J"},
    {Key::K, "K"},
    {Key::L, "L"},
    {Key::M, "M"},
    {Key::N, "N"},
    {Key::O, "O"},
    {Key::P, "P"},
    {Key::Q, "Q"},
    {Key::R, "R"},
    {Key::S, "S"},
    {Key::T, "T"},
    {Key::U, "U"},
    {Key::V, "V"},
    {Key::W, "W"},
    {Key::X, "X"},
    {Key::Y, "Y"},
    {Key::Z, "Z"},
    {Key::Digit0, "Digit0"},
    {Key::Digit1, "Digit1"},
    {Key::Digit2, "Digit2"},
    {Key::Digit3, "Digit3"},
    {Key::Digit4, "Digit4"},
    {Key::Digit5, "Digit5"},
    {Key::Digit6, "Digit6"},
    {Key::Digit7, "Digit7"},
    {Key::Digit8, "Digit8"},
    {Key::Digit9, "Digit9"},
    {Key::Space, "Space"},
    {Key::Return, "Return"},
    {Key::Tab, "Tab"},
    {Key::Backspace, "Backspace"},
    {Key::LeftShift, "LeftShift"},
    {Key::RightShift, "RightShift"},
    {Key::LeftControl, "LeftControl"},
    {Key::RightControl, "RightControl"},
    {Key::LeftAlt, "LeftAlt"},
    {Key::RightAlt, "RightAlt"},
    {Key::Left, "Left"},
    {Key::Right, "Right"},
    {Key::Up, "Up"},
    {Key::Down, "Down"},
    {Key::Home, "Home"},
    {Key::End, "End"},
    {Key::Delete, "Delete"},
    {Key::Minus, "Minus"},
    {Key::Equals, "Equals"},
    {Key::LeftBracket, "LeftBracket"},
    {Key::RightBracket, "RightBracket"},
    {Key::Backslash, "Backslash"},
    {Key::Semicolon, "Semicolon"},
    {Key::Quote, "Quote"},
    {Key::Backquote, "Backquote"},
    {Key::Comma, "Comma"},
    {Key::Period, "Period"},
    {Key::Slash, "Slash"},
    {Key::CapsLock, "CapsLock"},
    {Key::Insert, "Insert"},
    {Key::PageUp, "PageUp"},
    {Key::PageDown, "PageDown"},
    {Key::NumLock, "NumLock"},
    {Key::Keypad0, "Keypad0"},
    {Key::Keypad1, "Keypad1"},
    {Key::Keypad2, "Keypad2"},
    {Key::Keypad3, "Keypad3"},
    {Key::Keypad4, "Keypad4"},
    {Key::Keypad5, "Keypad5"},
    {Key::Keypad6, "Keypad6"},
    {Key::Keypad7, "Keypad7"},
    {Key::Keypad8, "Keypad8"},
    {Key::Keypad9, "Keypad9"},
    {Key::KeypadPeriod, "KeypadPeriod"},
    {Key::KeypadDivide, "KeypadDivide"},
    {Key::KeypadMultiply, "KeypadMultiply"},
    {Key::KeypadMinus, "KeypadMinus"},
    {Key::KeypadPlus, "KeypadPlus"},
    {Key::KeypadEnter, "KeypadEnter"},
    {Key::KeypadEquals, "KeypadEquals"},
};

struct MouseButtonNaming
{
    MouseButton button;
    std::string_view name;
};

constexpr MouseButtonNaming MouseButtonNames[] = {
    {MouseButton::Left, "MouseLeft"}, {MouseButton::Middle, "MouseMiddle"}, {MouseButton::Right, "MouseRight"},
    {MouseButton::X1, "MouseX1"},     {MouseButton::X2, "MouseX2"},
};

struct GamepadButtonNaming
{
    GamepadButton button;
    std::string_view name;
};

// Prefixed, because these names share a namespace with the key legends in the
// recorded input stream and in `Enum.KeyCode`: "Start" alone would be a key on
// some keyboard somewhere, and "South" alone means nothing to a reader.
constexpr GamepadButtonNaming GamepadButtonNames[] = {
    {GamepadButton::South, "ButtonSouth"},
    {GamepadButton::East, "ButtonEast"},
    {GamepadButton::West, "ButtonWest"},
    {GamepadButton::North, "ButtonNorth"},
    {GamepadButton::Back, "ButtonBack"},
    {GamepadButton::Guide, "ButtonGuide"},
    {GamepadButton::Start, "ButtonStart"},
    {GamepadButton::LeftStick, "ButtonLeftStick"},
    {GamepadButton::RightStick, "ButtonRightStick"},
    {GamepadButton::LeftShoulder, "ButtonLeftShoulder"},
    {GamepadButton::RightShoulder, "ButtonRightShoulder"},
    {GamepadButton::DpadUp, "DpadUp"},
    {GamepadButton::DpadDown, "DpadDown"},
    {GamepadButton::DpadLeft, "DpadLeft"},
    {GamepadButton::DpadRight, "DpadRight"},
};

struct GamepadAxisNaming
{
    GamepadAxis axis;
    std::string_view name;
};

constexpr GamepadAxisNaming GamepadAxisNames[] = {
    {GamepadAxis::LeftX, "LeftStickX"},        {GamepadAxis::LeftY, "LeftStickY"},
    {GamepadAxis::RightX, "RightStickX"},      {GamepadAxis::RightY, "RightStickY"},
    {GamepadAxis::LeftTrigger, "LeftTrigger"}, {GamepadAxis::RightTrigger, "RightTrigger"},
};

} // namespace

std::string_view keyName(Key key) noexcept
{
    for (const KeyNaming& naming : KeyNames) {
        if (naming.key == key)
            return naming.name;
    }
    return {};
}

Key keyFromName(std::string_view name) noexcept
{
    // Case-sensitive, because the legend is the name: "w" is not a key on any
    // keyboard, and accepting it would make "Space" and "space" two spellings
    // of one thing in an API that has no other case-insensitive lookup.
    for (const KeyNaming& naming : KeyNames) {
        if (naming.name == name)
            return naming.key;
    }
    return Key::Unknown;
}

std::string_view mouseButtonName(MouseButton button) noexcept
{
    for (const MouseButtonNaming& naming : MouseButtonNames) {
        if (naming.button == button)
            return naming.name;
    }
    return {};
}

MouseButton mouseButtonFromName(std::string_view name) noexcept
{
    for (const MouseButtonNaming& naming : MouseButtonNames) {
        if (naming.name == name)
            return naming.button;
    }
    return MouseButton::Unknown;
}

std::string_view gamepadButtonName(GamepadButton button) noexcept
{
    for (const GamepadButtonNaming& naming : GamepadButtonNames) {
        if (naming.button == button)
            return naming.name;
    }
    return {};
}

GamepadButton gamepadButtonFromName(std::string_view name) noexcept
{
    for (const GamepadButtonNaming& naming : GamepadButtonNames) {
        if (naming.name == name)
            return naming.button;
    }
    return GamepadButton::Unknown;
}

std::string_view gamepadAxisName(GamepadAxis axis) noexcept
{
    for (const GamepadAxisNaming& naming : GamepadAxisNames) {
        if (naming.axis == axis)
            return naming.name;
    }
    return {};
}

GamepadAxis gamepadAxisFromName(std::string_view name) noexcept
{
    for (const GamepadAxisNaming& naming : GamepadAxisNames) {
        if (naming.name == name)
            return naming.axis;
    }
    return GamepadAxis::Unknown;
}

// --- A gamepad that is not there (D476) -------------------------------------------

} // namespace engine::platform
