#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_init.h>
#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "engine/platform/event.h"
#include "engine/platform/platform.h"
#include "engine/platform/sdl_interop.h"

namespace engine::platform {
namespace {

// Main-thread only, like SDL's own event queue. The buffers are kept between
// frames so a steady-state frame does no allocation.
std::vector<SDL_Event> g_rawEvents;
// Paths dropped onto a window this pump. See `droppedFiles`.
std::vector<std::string> g_droppedFiles;
std::vector<Event> g_events;

// Written as an explicit table rather than arithmetic on the scancode range:
// the F-keys happen to be contiguous today, and a silent reordering upstream
// would turn a pin bump into a wrong-key bug that nothing would catch.
// `KeyModifier` bits from SDL's, either side alike (ADR 0139).
[[nodiscard]] u8 modifiersOf(SDL_Keymod mod) noexcept
{
    u8 out = 0;
    if ((mod & SDL_KMOD_SHIFT) != 0)
        out |= KeyModifier::Shift;
    if ((mod & SDL_KMOD_CTRL) != 0)
        out |= KeyModifier::Ctrl;
    if ((mod & SDL_KMOD_ALT) != 0)
        out |= KeyModifier::Alt;
    if ((mod & SDL_KMOD_GUI) != 0)
        out |= KeyModifier::System;
    return out;
}

// `text` into the event's buffer, cut if it must be at a character boundary --
// never inside a UTF-8 sequence, which would hand a field half a character.
void copyText(Event& event, const char* text) noexcept
{
    std::size_t length = std::strlen(text);
    if (length > kMaxTextInputBytes - 1) {
        length = kMaxTextInputBytes - 1;
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
            --length;
    }
    std::memcpy(event.text, text, length);
    event.text[length] = '\0';
}

Key translateScancode(SDL_Scancode scancode) noexcept
{
    switch (scancode) {
    case SDL_SCANCODE_ESCAPE:
        return Key::Escape;
    case SDL_SCANCODE_F1:
        return Key::F1;
    case SDL_SCANCODE_F2:
        return Key::F2;
    case SDL_SCANCODE_F3:
        return Key::F3;
    case SDL_SCANCODE_F4:
        return Key::F4;
    case SDL_SCANCODE_F5:
        return Key::F5;
    case SDL_SCANCODE_F6:
        return Key::F6;
    case SDL_SCANCODE_F7:
        return Key::F7;
    case SDL_SCANCODE_F8:
        return Key::F8;
    case SDL_SCANCODE_F9:
        return Key::F9;
    case SDL_SCANCODE_F10:
        return Key::F10;
    case SDL_SCANCODE_F11:
        return Key::F11;
    case SDL_SCANCODE_F12:
        return Key::F12;
    case SDL_SCANCODE_A:
        return Key::A;
    case SDL_SCANCODE_B:
        return Key::B;
    case SDL_SCANCODE_C:
        return Key::C;
    case SDL_SCANCODE_D:
        return Key::D;
    case SDL_SCANCODE_E:
        return Key::E;
    case SDL_SCANCODE_F:
        return Key::F;
    case SDL_SCANCODE_G:
        return Key::G;
    case SDL_SCANCODE_H:
        return Key::H;
    case SDL_SCANCODE_I:
        return Key::I;
    case SDL_SCANCODE_J:
        return Key::J;
    case SDL_SCANCODE_K:
        return Key::K;
    case SDL_SCANCODE_L:
        return Key::L;
    case SDL_SCANCODE_M:
        return Key::M;
    case SDL_SCANCODE_N:
        return Key::N;
    case SDL_SCANCODE_O:
        return Key::O;
    case SDL_SCANCODE_P:
        return Key::P;
    case SDL_SCANCODE_Q:
        return Key::Q;
    case SDL_SCANCODE_R:
        return Key::R;
    case SDL_SCANCODE_S:
        return Key::S;
    case SDL_SCANCODE_T:
        return Key::T;
    case SDL_SCANCODE_U:
        return Key::U;
    case SDL_SCANCODE_V:
        return Key::V;
    case SDL_SCANCODE_W:
        return Key::W;
    case SDL_SCANCODE_X:
        return Key::X;
    case SDL_SCANCODE_Y:
        return Key::Y;
    case SDL_SCANCODE_Z:
        return Key::Z;
    case SDL_SCANCODE_0:
        return Key::Digit0;
    case SDL_SCANCODE_1:
        return Key::Digit1;
    case SDL_SCANCODE_2:
        return Key::Digit2;
    case SDL_SCANCODE_3:
        return Key::Digit3;
    case SDL_SCANCODE_4:
        return Key::Digit4;
    case SDL_SCANCODE_5:
        return Key::Digit5;
    case SDL_SCANCODE_6:
        return Key::Digit6;
    case SDL_SCANCODE_7:
        return Key::Digit7;
    case SDL_SCANCODE_8:
        return Key::Digit8;
    case SDL_SCANCODE_9:
        return Key::Digit9;
    case SDL_SCANCODE_SPACE:
        return Key::Space;
    case SDL_SCANCODE_RETURN:
        return Key::Return;
    case SDL_SCANCODE_TAB:
        return Key::Tab;
    case SDL_SCANCODE_BACKSPACE:
        return Key::Backspace;
    case SDL_SCANCODE_HOME:
        return Key::Home;
    case SDL_SCANCODE_END:
        return Key::End;
    case SDL_SCANCODE_DELETE:
        return Key::Delete;
    case SDL_SCANCODE_LSHIFT:
        return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT:
        return Key::RightShift;
    case SDL_SCANCODE_LCTRL:
        return Key::LeftControl;
    case SDL_SCANCODE_RCTRL:
        return Key::RightControl;
    case SDL_SCANCODE_LALT:
        return Key::LeftAlt;
    case SDL_SCANCODE_RALT:
        return Key::RightAlt;
    case SDL_SCANCODE_LEFT:
        return Key::Left;
    case SDL_SCANCODE_RIGHT:
        return Key::Right;
    case SDL_SCANCODE_UP:
        return Key::Up;
    case SDL_SCANCODE_DOWN:
        return Key::Down;
    case SDL_SCANCODE_MINUS:
        return Key::Minus;
    case SDL_SCANCODE_EQUALS:
        return Key::Equals;
    case SDL_SCANCODE_LEFTBRACKET:
        return Key::LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET:
        return Key::RightBracket;
    case SDL_SCANCODE_BACKSLASH:
        return Key::Backslash;
    case SDL_SCANCODE_SEMICOLON:
        return Key::Semicolon;
    case SDL_SCANCODE_APOSTROPHE:
        return Key::Quote;
    case SDL_SCANCODE_GRAVE:
        return Key::Backquote;
    case SDL_SCANCODE_COMMA:
        return Key::Comma;
    case SDL_SCANCODE_PERIOD:
        return Key::Period;
    case SDL_SCANCODE_SLASH:
        return Key::Slash;
    case SDL_SCANCODE_CAPSLOCK:
        return Key::CapsLock;
    case SDL_SCANCODE_INSERT:
        return Key::Insert;
    case SDL_SCANCODE_PAGEUP:
        return Key::PageUp;
    case SDL_SCANCODE_PAGEDOWN:
        return Key::PageDown;
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return Key::NumLock;
    case SDL_SCANCODE_KP_0:
        return Key::Keypad0;
    case SDL_SCANCODE_KP_1:
        return Key::Keypad1;
    case SDL_SCANCODE_KP_2:
        return Key::Keypad2;
    case SDL_SCANCODE_KP_3:
        return Key::Keypad3;
    case SDL_SCANCODE_KP_4:
        return Key::Keypad4;
    case SDL_SCANCODE_KP_5:
        return Key::Keypad5;
    case SDL_SCANCODE_KP_6:
        return Key::Keypad6;
    case SDL_SCANCODE_KP_7:
        return Key::Keypad7;
    case SDL_SCANCODE_KP_8:
        return Key::Keypad8;
    case SDL_SCANCODE_KP_9:
        return Key::Keypad9;
    case SDL_SCANCODE_KP_PERIOD:
        return Key::KeypadPeriod;
    case SDL_SCANCODE_KP_DIVIDE:
        return Key::KeypadDivide;
    case SDL_SCANCODE_KP_MULTIPLY:
        return Key::KeypadMultiply;
    case SDL_SCANCODE_KP_MINUS:
        return Key::KeypadMinus;
    case SDL_SCANCODE_KP_PLUS:
        return Key::KeypadPlus;
    case SDL_SCANCODE_KP_ENTER:
        return Key::KeypadEnter;
    case SDL_SCANCODE_KP_EQUALS:
        return Key::KeypadEquals;
    default:
        return Key::Unknown;
    }
}

// One table, walked in both directions. Written out rather than derived from
// the enumerator names, because a table a compiler cannot check is one a test
// has to: `platform_tests` walks every enumerator and requires a round trip
// through both functions.
//
// The digits are `Digit0` rather than `0`, which is the enumerator's name and
// not the key's legend. M6 changed them: these names, the mouse and gamepad
// names beside them, and `Enum.KeyCode`'s items are ONE spelling space -- a
// recorded input stream is written in it, and `input` resolves a name to a
// KeyCode by walking these tables. A legend that differed from the item name
// for ten of the ninety-four would have meant a second table to keep in step.
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

// Every gamepad SDL reports is opened, because an unopened one produces no
// events at all: SDL only sends button and axis events for a gamepad somebody
// holds a handle to. Kept as a flat list rather than a map -- a machine has a
// handful of pads, and a linear scan over four entries beats a hash of one.
std::vector<SDL_Gamepad*> g_gamepads;

// --- Vibration (ADR 0131) ---------------------------------------------------------

// The hardware: every open gamepad, and the first haptic device SDL names --
// which on a phone is the phone.
class SdlVibration final : public VibrationSink
{
public:
    [[nodiscard]] bool gamepad() const override
    {
        for (SDL_Gamepad* pad : g_gamepads) {
            if (SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
                return true;
        }
        return false;
    }

    [[nodiscard]] bool triggers() const override
    {
        for (SDL_Gamepad* pad : g_gamepads) {
            if (SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN,
                                       false))
                return true;
        }
        return false;
    }

    [[nodiscard]] bool device() const override { return const_cast<SdlVibration*>(this)->open() != nullptr; }

    void rumble(f32 heavy, f32 light) override
    {
        for (SDL_Gamepad* pad : g_gamepads)
            (void)SDL_RumbleGamepad(pad, level(heavy), level(light), kLifeMs);
    }

    void rumbleTriggers(f32 left, f32 right) override
    {
        for (SDL_Gamepad* pad : g_gamepads)
            (void)SDL_RumbleGamepadTriggers(pad, level(left), level(right), kLifeMs);
    }

    void vibrate(f32 strength, f32 seconds) override
    {
        if (SDL_Haptic* haptic = open(); haptic != nullptr)
            (void)SDL_PlayHapticRumble(haptic, strength, static_cast<Uint32>(seconds * 1000.0f));
    }

    void stop() override
    {
        for (SDL_Gamepad* pad : g_gamepads) {
            (void)SDL_RumbleGamepad(pad, 0, 0, 0);
            (void)SDL_RumbleGamepadTriggers(pad, 0, 0, 0);
        }
        if (m_haptic != nullptr)
            (void)SDL_StopHapticRumble(m_haptic);
    }

private:
    // How long one send lasts. Re-sent every frame, so this is how long a game
    // that has stopped pumping goes on shaking.
    static constexpr Uint32 kLifeMs = 500;

    [[nodiscard]] static Uint16 level(f32 value) noexcept
    {
        return static_cast<Uint16>(std::clamp(value, 0.0f, 1.0f) * 65535.0f);
    }

    // The device's own vibrator, opened the first time it is asked for: a
    // desktop has none, and never pays for the subsystem.
    [[nodiscard]] SDL_Haptic* open()
    {
        if (m_tried)
            return m_haptic;
        m_tried = true;
        if (!SDL_InitSubSystem(SDL_INIT_HAPTIC))
            return nullptr;
        int count = 0;
        SDL_HapticID* ids = SDL_GetHaptics(&count);
        if (ids != nullptr && count > 0) {
            m_haptic = SDL_OpenHaptic(ids[0]);
            if (m_haptic != nullptr && !SDL_InitHapticRumble(m_haptic)) {
                SDL_CloseHaptic(m_haptic);
                m_haptic = nullptr;
            }
        }
        SDL_free(ids);
        return m_haptic;
    }

    SDL_Haptic* m_haptic = nullptr;
    bool m_tried = false;
};

struct VibrationState
{
    SdlVibration hardware;
    VibrationSink* sink = nullptr;
    f32 motors[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool focused = true;
    // Whether the last pump sent anything, so going quiet says so once.
    bool running = false;

    [[nodiscard]] VibrationSink& out() noexcept { return sink != nullptr ? *sink : hardware; }
};

VibrationState g_vibration;

[[nodiscard]] MouseButton translateMouseButton(Uint8 button) noexcept
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
    case SDL_BUTTON_X1:
        return MouseButton::X1;
    case SDL_BUTTON_X2:
        return MouseButton::X2;
    default:
        return MouseButton::Unknown;
    }
}

// An explicit table for the same reason `translateScancode` is one: the two
// enumerations happen to agree in order today, and a silent reordering upstream
// would turn a pin bump into a wrong-button bug nothing would catch.
[[nodiscard]] GamepadButton translateGamepadButton(Uint8 button) noexcept
{
    switch (static_cast<SDL_GamepadButton>(button)) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return GamepadButton::South;
    case SDL_GAMEPAD_BUTTON_EAST:
        return GamepadButton::East;
    case SDL_GAMEPAD_BUTTON_WEST:
        return GamepadButton::West;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return GamepadButton::North;
    case SDL_GAMEPAD_BUTTON_BACK:
        return GamepadButton::Back;
    case SDL_GAMEPAD_BUTTON_GUIDE:
        return GamepadButton::Guide;
    case SDL_GAMEPAD_BUTTON_START:
        return GamepadButton::Start;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return GamepadButton::LeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return GamepadButton::RightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return GamepadButton::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return GamepadButton::RightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return GamepadButton::DpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return GamepadButton::DpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return GamepadButton::DpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return GamepadButton::DpadRight;
    default:
        // Paddles, the touchpad button and the MISC range: real buttons on a
        // minority of hardware, and deliberately not in our enum (event.h).
        return GamepadButton::Unknown;
    }
}

[[nodiscard]] GamepadAxis translateGamepadAxis(Uint8 axis) noexcept
{
    switch (static_cast<SDL_GamepadAxis>(axis)) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        return GamepadAxis::LeftX;
    case SDL_GAMEPAD_AXIS_LEFTY:
        return GamepadAxis::LeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX:
        return GamepadAxis::RightX;
    case SDL_GAMEPAD_AXIS_RIGHTY:
        return GamepadAxis::RightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        return GamepadAxis::LeftTrigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        return GamepadAxis::RightTrigger;
    default:
        return GamepadAxis::Unknown;
    }
}

// SDL reports a stick over the whole signed range and a trigger over the
// positive half of it. Dividing both by 32767 would make a released trigger
// read -1, so the two are normalized differently -- and the asymmetry lives
// here, once, rather than in every caller that reads an axis.
[[nodiscard]] float normalizeAxis(GamepadAxis axis, Sint16 raw) noexcept
{
    constexpr float PositiveRange = 32767.0f;
    if (axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger)
        return std::clamp(static_cast<float>(raw) / PositiveRange, 0.0f, 1.0f);
    // -32768 divided by 32767 is slightly past -1, which is why this clamps
    // rather than trusting the division: an axis that can read -1.00003 makes
    // every "is this exactly -1" comparison downstream wrong once in a while.
    return std::clamp(static_cast<float>(raw) / PositiveRange, -1.0f, 1.0f);
}

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

void openGamepad(SDL_JoystickID which)
{
    if (SDL_Gamepad* pad = SDL_OpenGamepad(which); pad != nullptr)
        g_gamepads.push_back(pad);
}

void closeGamepad(SDL_JoystickID which)
{
    const auto found = std::find_if(g_gamepads.begin(), g_gamepads.end(),
                                    [which](SDL_Gamepad* pad) { return SDL_GetGamepadID(pad) == which; });
    if (found == g_gamepads.end())
        return;
    SDL_CloseGamepad(*found);
    g_gamepads.erase(found);
}

void translate(const SDL_Event& raw, std::vector<Event>& out)
{
    switch (raw.type) {
    case SDL_EVENT_QUIT: {
        Event event;
        event.type = EventType::Quit;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_WILL_ENTER_BACKGROUND: {
        Event event;
        event.type = EventType::WillEnterBackground;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
        Event event;
        event.type = EventType::WindowCloseRequested;
        event.windowId = raw.window.windowID;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
        // Pixels rather than the logical size, because the consumer that
        // matters is the swapchain.
        Event event;
        event.type = EventType::WindowResized;
        event.windowId = raw.window.windowID;
        event.width = raw.window.data1;
        event.height = raw.window.data2;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const Key key = translateScancode(raw.key.scancode);
        if (key == Key::Unknown)
            break;

        Event event;
        event.type = raw.type == SDL_EVENT_KEY_DOWN ? EventType::KeyDown : EventType::KeyUp;
        event.windowId = raw.key.windowID;
        event.key = key;
        event.repeat = raw.key.repeat;
        event.modifiers = modifiersOf(raw.key.mod);
        out.push_back(event);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST: {
        // Nobody holds the pad of a game they are not playing (ADR 0131).
        setVibrationFocus(raw.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        Event event;
        event.type =
            raw.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? EventType::WindowFocusGained : EventType::WindowFocusLost;
        event.windowId = raw.window.windowID;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_TEXT_INPUT: {
        if (raw.text.text == nullptr)
            break;
        Event event;
        event.type = EventType::TextInput;
        event.windowId = raw.text.windowID;
        copyText(event, raw.text.text);
        out.push_back(event);
        break;
    }
    case SDL_EVENT_TEXT_EDITING: {
        Event event;
        event.type = EventType::TextEditing;
        event.windowId = raw.edit.windowID;
        copyText(event, raw.edit.text != nullptr ? raw.edit.text : "");
        event.editStart = raw.edit.start;
        event.editLength = raw.edit.length;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        Event event;
        event.type = EventType::MouseMoved;
        event.windowId = raw.motion.windowID;
        event.pointerX = raw.motion.x;
        event.pointerY = raw.motion.y;
        event.pointerDeltaX = raw.motion.xrel;
        event.pointerDeltaY = raw.motion.yrel;
        event.fromTouch = raw.motion.which == SDL_TOUCH_MOUSEID;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const MouseButton button = translateMouseButton(raw.button.button);
        if (button == MouseButton::Unknown)
            break;
        Event event;
        event.type = raw.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? EventType::MouseButtonDown : EventType::MouseButtonUp;
        event.windowId = raw.button.windowID;
        event.button = button;
        event.pointerX = raw.button.x;
        event.pointerY = raw.button.y;
        event.fromTouch = raw.button.which == SDL_TOUCH_MOUSEID;
        event.clicks = raw.button.clicks;
        // Shift+press extends a selection (ADR 0139): the keys held now.
        event.modifiers = modifiersOf(SDL_GetModState());
        out.push_back(event);
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        // SDL gives a finger in 0..1 of the window; the engine speaks window
        // pixels, the space the mouse and the interface are in.
        int width = 0;
        int height = 0;
        if (SDL_Window* window = SDL_GetWindowFromID(raw.tfinger.windowID); window != nullptr)
            SDL_GetWindowSize(window, &width, &height);
        Event event;
        event.type = raw.type == SDL_EVENT_FINGER_DOWN     ? EventType::FingerDown
                     : raw.type == SDL_EVENT_FINGER_MOTION ? EventType::FingerMoved
                                                           : EventType::FingerUp;
        event.windowId = raw.tfinger.windowID;
        event.fingerId = static_cast<u64>(raw.tfinger.fingerID);
        event.pointerX = raw.tfinger.x * static_cast<f32>(width);
        event.pointerY = raw.tfinger.y * static_cast<f32>(height);
        event.pointerDeltaX = raw.tfinger.dx * static_cast<f32>(width);
        event.pointerDeltaY = raw.tfinger.dy * static_cast<f32>(height);
        out.push_back(event);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        Event event;
        event.type = EventType::MouseWheel;
        event.windowId = raw.wheel.windowID;
        // SDL reports FLIPPED for a natural-scrolling trackpad and documents
        // the fix as multiplying by -1. Undone here so that every consumer sees
        // one convention, which is what a binding stored in a save file needs.
        const float sign = raw.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        event.wheelX = raw.wheel.x * sign;
        event.wheelY = raw.wheel.y * sign;
        event.pointerX = raw.wheel.mouse_x;
        event.pointerY = raw.wheel.mouse_y;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED: {
        openGamepad(raw.gdevice.which);
        Event event;
        event.type = EventType::GamepadAdded;
        event.gamepadId = static_cast<u32>(raw.gdevice.which);
        out.push_back(event);
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        closeGamepad(raw.gdevice.which);
        Event event;
        event.type = EventType::GamepadRemoved;
        event.gamepadId = static_cast<u32>(raw.gdevice.which);
        out.push_back(event);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const GamepadButton button = translateGamepadButton(raw.gbutton.button);
        if (button == GamepadButton::Unknown)
            break;
        Event event;
        event.type =
            raw.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? EventType::GamepadButtonDown : EventType::GamepadButtonUp;
        event.gamepadId = static_cast<u32>(raw.gbutton.which);
        event.gamepadButton = button;
        out.push_back(event);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const GamepadAxis axis = translateGamepadAxis(raw.gaxis.axis);
        if (axis == GamepadAxis::Unknown)
            break;
        Event event;
        event.type = EventType::GamepadAxisMoved;
        event.gamepadId = static_cast<u32>(raw.gaxis.which);
        event.gamepadAxis = axis;
        event.axisValue = normalizeAxis(axis, raw.gaxis.value);
        out.push_back(event);
        break;
    }
    default:
        // Everything else stays in the raw stream only. Dropping it here is
        // not a loss: the module models what the engine reacts to, and the
        // SDL-facing consumers read rawEvents().
        break;
    }
}

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

void setVibrationSink(VibrationSink* sink) noexcept
{
    g_vibration.sink = sink;
}

bool vibrationSupported(bool gamepad) noexcept
{
    return gamepad ? g_vibration.out().gamepad() : g_vibration.out().device();
}

bool vibrationMotorSupported(bool gamepad, VibrationMotor motor) noexcept
{
    // A phone has one vibrator and no named motors: it has `vibrateDevice`.
    if (!gamepad)
        return false;
    switch (motor) {
    case VibrationMotor::Large:
    case VibrationMotor::Small:
        return g_vibration.out().gamepad();
    case VibrationMotor::LeftTrigger:
    case VibrationMotor::RightTrigger:
        return g_vibration.out().triggers();
    case VibrationMotor::LeftHand:
    case VibrationMotor::RightHand:
        return false;
    }
    return false;
}

void setVibrationMotor(VibrationMotor motor, f32 value) noexcept
{
    const auto index = static_cast<std::size_t>(motor);
    if (index >= std::size(g_vibration.motors))
        return;
    g_vibration.motors[index] = value == value ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

void vibrateDevice(f32 strength, f32 seconds) noexcept
{
    if (!g_vibration.focused || !(strength > 0.0f) || !(seconds > 0.0f))
        return;
    g_vibration.out().vibrate(std::clamp(strength, 0.0f, 1.0f), std::clamp(seconds, 0.0f, 5.0f));
}

void setVibrationFocus(bool focused) noexcept
{
    if (g_vibration.focused == focused)
        return;
    g_vibration.focused = focused;
    // Still at once, not when the last send runs out.
    if (!focused) {
        g_vibration.out().stop();
        g_vibration.running = false;
    }
}

void pumpVibration() noexcept
{
    const f32* motors = g_vibration.motors;
    const bool any = motors[0] > 0.0f || motors[1] > 0.0f || motors[2] > 0.0f || motors[3] > 0.0f;
    if (!g_vibration.focused || !any) {
        // Told to stop once, when it goes quiet -- not every frame after.
        if (g_vibration.running) {
            g_vibration.out().stop();
            g_vibration.running = false;
        }
        return;
    }
    g_vibration.out().rumble(motors[0], motors[1]);
    if (motors[2] > 0.0f || motors[3] > 0.0f)
        g_vibration.out().rumbleTriggers(motors[2], motors[3]);
    g_vibration.running = true;
}

void stopVibration() noexcept
{
    for (f32& motor : g_vibration.motors)
        motor = 0.0f;
    g_vibration.out().stop();
    g_vibration.running = false;
}

void setTextInputEnabled(u32 windowId, bool enabled, const TextInputOptions& options) noexcept
{
    SDL_Window* window = SDL_GetWindowFromID(static_cast<SDL_WindowID>(windowId));
    if (window == nullptr)
        return;
    if (!enabled) {
        (void)SDL_StopTextInput(window);
        return;
    }
    // Android's own input types for what SDL does not name (a phone number, a
    // decimal, an address): `InputType.TYPE_CLASS_*` and its flags.
    constexpr Sint64 AndroidPhone = 3;
    constexpr Sint64 AndroidDecimal = 2 | 8192;
    constexpr Sint64 AndroidUrl = 1 | 16;
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_TextInputType type = SDL_TEXTINPUT_TYPE_TEXT;
    switch (options.kind) {
    case TextInputKind::Text:
        break;
    case TextInputKind::Number:
        type = SDL_TEXTINPUT_TYPE_NUMBER;
        break;
    case TextInputKind::Decimal:
        type = SDL_TEXTINPUT_TYPE_NUMBER;
        (void)SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_ANDROID_INPUTTYPE_NUMBER, AndroidDecimal);
        break;
    case TextInputKind::Phone:
        type = SDL_TEXTINPUT_TYPE_NUMBER;
        (void)SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_ANDROID_INPUTTYPE_NUMBER, AndroidPhone);
        break;
    case TextInputKind::Email:
        type = SDL_TEXTINPUT_TYPE_TEXT_EMAIL;
        break;
    case TextInputKind::Url:
        (void)SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_ANDROID_INPUTTYPE_NUMBER, AndroidUrl);
        break;
    case TextInputKind::Password:
        type = SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN;
        break;
    }
    (void)SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_TYPE_NUMBER, type);
    (void)SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, options.multiLine);
    // A restart is what applies new options to a keyboard already up.
    if (SDL_TextInputActive(window))
        (void)SDL_StopTextInput(window);
    (void)SDL_StartTextInputWithProperties(window, properties);
    SDL_DestroyProperties(properties);
}

void setTextInputArea(u32 windowId, i32 x, i32 y, i32 width, i32 height, i32 cursor) noexcept
{
    SDL_Window* window = SDL_GetWindowFromID(static_cast<SDL_WindowID>(windowId));
    if (window == nullptr)
        return;
    const SDL_Rect area{x, y, width, height};
    (void)SDL_SetTextInputArea(window, &area, cursor);
}

void setTextInputEnabled(u32 windowId, bool enabled) noexcept
{
    SDL_Window* window = SDL_GetWindowFromID(static_cast<SDL_WindowID>(windowId));
    if (window == nullptr)
        return;
    if (enabled)
        (void)SDL_StartTextInput(window);
    else
        (void)SDL_StopTextInput(window);
}

bool textInputEnabled(u32 windowId) noexcept
{
    SDL_Window* window = SDL_GetWindowFromID(static_cast<SDL_WindowID>(windowId));
    return window != nullptr && SDL_TextInputActive(window);
}

std::span<const Event> pumpEvents()
{
    g_rawEvents.clear();
    g_events.clear();
    g_droppedFiles.clear();
    // The motors' levels, sent on for another half second (ADR 0131).
    pumpVibration();

    SDL_Event raw;
    while (SDL_PollEvent(&raw)) {
        g_rawEvents.push_back(raw);
        // **Dropped paths go in a list of their own**, not on an `Event`. An
        // `Event` is a POD copied for every mouse motion and a string on it
        // would be an allocation per frame paid for a thing that happens twice
        // a session. SDL owns `drop.data` until the event is consumed, so the
        // copy happens here rather than being deferred to whoever reads it.
        if (raw.type == SDL_EVENT_DROP_FILE && raw.drop.data != nullptr)
            g_droppedFiles.emplace_back(raw.drop.data);
        translate(raw, g_events);
    }

    return g_events;
}

std::span<const SDL_Event> rawEvents() noexcept
{
    return g_rawEvents;
}

std::span<const std::string> droppedFiles() noexcept
{
    return g_droppedFiles;
}

} // namespace engine::platform
