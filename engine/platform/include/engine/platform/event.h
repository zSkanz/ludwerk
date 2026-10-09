#pragma once

#include <span>
#include <string_view>

#include "engine/core/types.h"

namespace engine::platform {

using core::f32;
using core::i32;
using core::u16;
using core::u32;
using core::u64;
using core::u8;

enum class EventType : u8
{
    Quit,
    WindowCloseRequested,
    WindowResized,
    WindowFocusGained,
    WindowFocusLost,
    KeyDown,
    KeyUp,
    // The text a keystroke PRODUCES, which is not the key that produced it: a
    // dead key followed by a vowel is two key events and one text event, and an
    // IME is many key events and one. A text field reads these; an action
    // binding reads the key events. Conflating them is how a text box ends up
    // unable to type an accented character on a layout nobody tested.
    TextInput,
    MouseMoved,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    GamepadAdded,
    GamepadRemoved,
    GamepadButtonDown,
    GamepadButtonUp,
    GamepadAxisMoved,
    // A finger on a touchscreen: `fingerId` says which, `pointerX` / `pointerY`
    // where, in the same window pixels as the mouse. A cancelled touch -- the
    // system took the gesture -- arrives as `FingerUp`.
    FingerDown,
    FingerMoved,
    FingerUp,
    // The app is about to go to the background (a phone's home button, a
    // call). A mobile OS may end a backgrounded process without a close, so
    // this is the last moment a game's saves are sure to be written (ADR 0111).
    WillEnterBackground,
    // An input method's text while it is still being COMPOSED (ADR 0139): a
    // Chinese, Japanese or Korean phrase before it is committed, or the
    // system's emoji picker. `text` is the whole provisional string, and
    // `editStart` / `editLength` its cursor or selection inside it, in
    // characters. An empty `text` ends the composition. Committed text arrives
    // as `TextInput`, as typing does.
    TextEditing,
};

// Physical keys, named by the US-layout legend the way scancodes are.
//
// It was tiny through M4 -- the F-keys and Escape, which was every key the
// engine itself reacted to -- and M5 grows it to a keyboard, because the
// milestone ships a character somebody has to be able to steer.
//
// It is still not a full keycode table, and that is deliberate: mouse buttons
// and gamepad inputs belong to the Input Action System (ADR 0029, M6), which
// maps device-level input to named actions. Growing them here first would build
// the wrong half of that and would have to be re-derived against it anyway.
enum class Key : u16
{
    Unknown = 0,
    Escape,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,

    Space,
    Return,
    Tab,
    Backspace,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,

    Left,
    Right,
    Up,
    Down,
    // The three a caret needs and nothing else did (S6.7). At the end of the
    // KEYBOARD block rather than at the end of `Enum.KeyCode`, because the
    // resolver's `keyCodeOf` is a subtraction over a contiguous block and a
    // `static_assert` holds the two lists to the same length. Renumbering the
    // mouse and gamepad items below is safe: a scene stores an enum item by
    // NAME, which `scene_file.cpp` says it does precisely so that renumbering
    // survives.
    Home,
    End,
    Delete,
    // The rest of the keyboard (D210), which `Enum.KeyCode` carries at its END
    // -- as a second keyboard block, so nothing a game compares by value moved.
    Minus,
    Equals,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Quote,
    Backquote,
    Comma,
    Period,
    Slash,
    CapsLock,
    Insert,
    PageUp,
    PageDown,
    NumLock,
    Keypad0,
    Keypad1,
    Keypad2,
    Keypad3,
    Keypad4,
    Keypad5,
    Keypad6,
    Keypad7,
    Keypad8,
    Keypad9,
    KeypadPeriod,
    KeypadDivide,
    KeypadMultiply,
    KeypadMinus,
    KeypadPlus,
    KeypadEnter,
    KeypadEquals,

    // Not a key. The count is what sizes a keyboard snapshot, and having it
    // here is what stops that array from being a number somebody has to keep in
    // step by hand.
    Count,
};

// The key's name -- its US-layout legend, except for the digits, which are
// `Digit0` rather than `0` because these names share one spelling space with
// the mouse and gamepad tables below and with `Enum.KeyCode`'s items. Empty for
// `Unknown` and for `Count`.
[[nodiscard]] std::string_view keyName(Key key) noexcept;

// The reverse, case-sensitive. `Key::Unknown` for a name no key carries.
[[nodiscard]] Key keyFromName(std::string_view name) noexcept;

// The mouse buttons the Input Action System can bind. SDL numbers them from 1
// and this enum does not: `Unknown` is 0 here so that a default-constructed
// `Event` names no button, which is the same discipline `Key::Unknown` follows.
enum class MouseButton : u8
{
    Unknown = 0,
    Left,
    Middle,
    Right,
    X1,
    X2,

    Count,
};

// The standard gamepad layout SDL maps every controller onto
// (`SDL_gamepad.h:152`). Named by POSITION rather than by legend -- `South`
// rather than `A` -- because the legend depends on the pad: the bottom face
// button is A on an Xbox pad, B on a Nintendo one and Cross on a PlayStation
// one, and a binding stored as "A" would move when somebody changed hardware.
// What the button is CALLED is a display concern, and it belongs to the prompt
// glyph rather than to the binding.
//
// The paddles, touchpad button and the `MISC` range are deliberately absent:
// they exist on a minority of hardware, and an enum item nothing can produce on
// the machine in front of you is a binding that silently never fires.
// Physical controller family, independent of button labels and the host OS.
enum class GamepadType : u8
{
    Unknown = 0,
    Xbox = 1,
    PlayStation = 2,
    Nintendo = 3,
    Generic = 4
};
[[nodiscard]] GamepadType gamepadType(u32 id) noexcept;

enum class GamepadButton : u8
{
    Unknown = 0,
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,

    Count,
};

// The six analogue axes of that same standard layout. Sticks report -1 to 1;
// triggers report 0 to 1, because SDL reports them over the positive half of
// its range and a trigger at rest reading -1 would be a resting input that
// looks like a held one.
enum class GamepadAxis : u8
{
    Unknown = 0,
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,

    Count,
};

// The longest UTF-8 sequence a single `TextInput` event carries, plus room for
// a terminator. SDL hands text as a pointer into memory it frees once the event
// is handled, so the bytes are COPIED here: an `Event` outlives the SDL event
// it came from by a whole frame, and a dangling `const char*` in a public
// struct is a use-after-free waiting for a slow frame.
// An input method commits a whole phrase in one event, so this is a phrase's
// room and not a keystroke's (ADR 0139); a longer one is cut at a character
// boundary, never inside one.
inline constexpr u32 kMaxTextInputBytes = 256;

// The keys held with a key or a press (ADR 0139): what makes Ctrl+C a copy and
// Shift+Left a selection. Bits, either side of the keyboard alike.
struct KeyModifier
{
    static constexpr u8 Shift = 1;
    static constexpr u8 Ctrl = 2;
    static constexpr u8 Alt = 4;
    // The Windows key, or Command on a Mac.
    static constexpr u8 System = 8;
};

// One flat record rather than a tagged union: the union machinery would cost
// more to read than the unused fields cost to carry, and at sixteen event types
// that trade has not changed -- only the number of fields nobody reads on any
// given event has.
struct Event
{
    EventType type = EventType::Quit;

    // The window the event belongs to; 0 when it is not window-scoped.
    u32 windowId = 0;

    // KeyDown / KeyUp.
    Key key = Key::Unknown;
    bool repeat = false;
    // KeyDown / KeyUp / MouseButtonDown / MouseButtonUp: `KeyModifier` bits.
    u8 modifiers = 0;
    // MouseButtonDown / MouseButtonUp: 1 for a single press, 2 for a double,
    // 3 for a triple -- the system's own count, on its own double-click time.
    u8 clicks = 0;

    // TextEditing: the composition's cursor, in characters of `text`.
    i32 editStart = 0;
    i32 editLength = 0;

    // WindowResized -- the new drawable size in pixels.
    i32 width = 0;
    i32 height = 0;

    // MouseMoved, MouseButtonDown / MouseButtonUp -- the pointer in window
    // pixels, top-left origin, y growing downward.
    f32 pointerX = 0.0f;
    f32 pointerY = 0.0f;
    // MouseMoved -- the motion since the previous event, which is NOT the
    // difference of two positions once the pointer is locked: relative mode
    // keeps reporting motion the position cannot express.
    f32 pointerDeltaX = 0.0f;
    f32 pointerDeltaY = 0.0f;

    // MouseButtonDown / MouseButtonUp.
    MouseButton button = MouseButton::Unknown;

    // FingerDown / FingerMoved / FingerUp -- which finger, stable while it is
    // down and meaningless after.
    u64 fingerId = 0;
    // A mouse event the system made out of a finger. The interface follows it
    // -- that is how a tap presses a button -- and the input system does not,
    // since the same finger already arrived as a `Finger*` event.
    bool fromTouch = false;

    // MouseWheel -- in wheel notches, positive right and positive away from the
    // user, with SDL's FLIPPED direction already undone.
    f32 wheelX = 0.0f;
    f32 wheelY = 0.0f;

    // Every gamepad event. SDL's instance id, which is unique for the life of
    // the connection and is NOT a player index -- unplugging and replugging one
    // pad produces a new id, which is why a binding is never stored against it.
    u32 gamepadId = 0;
    GamepadType gamepadFamily = GamepadType::Unknown;
    GamepadButton gamepadButton = GamepadButton::Unknown;
    GamepadAxis gamepadAxis = GamepadAxis::Unknown;
    // GamepadAxisMoved. -1..1 for a stick, 0..1 for a trigger. No dead zone is
    // applied here: a dead zone is a per-action processor (ADR 0029) and
    // applying one at the device would make it unremovable.
    f32 axisValue = 0.0f;

    // TextInput and TextEditing -- NUL-terminated UTF-8, copied rather than
    // referenced.
    char text[kMaxTextInputBytes] = {};
};

// The US-layout legend for a mouse button, a gamepad button and a gamepad axis.
// Empty for `Unknown` and for `Count`, exactly as `keyName` is -- these three
// exist for the same reason it does: the recorded input stream the determinism
// gate replays is written in names, not in numbers, so that a trace stays
// readable and stays valid across a reordering of these enums.
[[nodiscard]] std::string_view mouseButtonName(MouseButton button) noexcept;
[[nodiscard]] MouseButton mouseButtonFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view gamepadButtonName(GamepadButton button) noexcept;
[[nodiscard]] GamepadButton gamepadButtonFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view gamepadAxisName(GamepadAxis axis) noexcept;
[[nodiscard]] GamepadAxis gamepadAxisFromName(std::string_view name) noexcept;

// --- Vibration (ADR 0131 section 2) ---------------------------------------------
//
// A gamepad's motors and a phone's own vibrator. **A motor is a level**: it
// runs at what it was last told until it is told otherwise, and it is still
// while the window is not in front -- nobody is holding the pad of a game they
// are not playing. `pumpVibration` re-sends the levels each frame with a short
// life, so a game that stops pumping -- closed, crashed, frozen -- stops
// shaking within half a second by itself.

enum class VibrationMotor : u8
{
    Large,
    Small,
    LeftTrigger,
    RightTrigger,
    // Two controllers, one a hand: nothing this layer drives has them yet.
    LeftHand,
    RightHand,
};

// Where the calls end up. The hardware's is the default; a test puts its own
// here and reads what arrived.
class VibrationSink
{
public:
    virtual ~VibrationSink() = default;
    // Whether a gamepad with motors is connected, whether one has motors in
    // its triggers, and whether the device itself can vibrate.
    [[nodiscard]] virtual bool gamepad() const = 0;
    [[nodiscard]] virtual bool triggers() const = 0;
    [[nodiscard]] virtual bool device() const = 0;
    // Every gamepad's two body motors, and its two trigger motors, 0 to 1,
    // for the next half second.
    virtual void rumble(f32 heavy, f32 light) = 0;
    virtual void rumbleTriggers(f32 left, f32 right) = 0;
    // The device itself, for `seconds`.
    virtual void vibrate(f32 strength, f32 seconds) = 0;
    // Everything still, now.
    virtual void stop() = 0;
};

// Null puts the hardware back.
void setVibrationSink(VibrationSink* sink) noexcept;

[[nodiscard]] bool vibrationSupported(bool gamepad) noexcept;
[[nodiscard]] bool vibrationMotorSupported(bool gamepad, VibrationMotor motor) noexcept;
// A gamepad motor's level, 0 to 1, until it is set again.
void setVibrationMotor(VibrationMotor motor, f32 value) noexcept;
// The device itself: `strength` 0 to 1 for `seconds`, at most five.
void vibrateDevice(f32 strength, f32 seconds) noexcept;
// Whether the game's window is the one in front. Losing it stills everything;
// the levels are kept and return with it.
void setVibrationFocus(bool focused) noexcept;
// Once a frame: sends the levels on, while focused.
void pumpVibration() noexcept;
// Every level to zero and everything still: what closing a game does.
void stopVibration() noexcept;

// --- A gamepad that is not there (D476) -------------------------------------------
//
// A virtual gamepad, hosted by the platform library itself: to everything
// above this layer -- and to the library's own gamepad code below it -- it is
// a device that was plugged in. It exists so that "a gamepad works" is a fact
// a test can state on a machine with none: a button pressed here arrives as
// the same event a real one sends, through the same code, and a rumble sent
// to it is what the device was told.
//
// For tests and for automation. The id is what `detachVirtualGamepad` and the
// setters take; zero means the platform could not make one -- which is what a
// build with no joystick support answers, and what the test is there to catch.

[[nodiscard]] u32 attachVirtualGamepad() noexcept;
void detachVirtualGamepad(u32 id) noexcept;
// Takes effect at the next `pumpEvents`, as a real device's does.
void setVirtualGamepadButton(u32 id, GamepadButton button, bool down) noexcept;
// -1 to 1 for a stick, 0 to 1 for a trigger.
void setVirtualGamepadAxis(u32 id, GamepadAxis axis, f32 value) noexcept;

// What the virtual gamepad was last told to do with its motors, 0 to 1.
struct VirtualRumble
{
    f32 heavy = 0.0f;
    f32 light = 0.0f;
    f32 leftTrigger = 0.0f;
    f32 rightTrigger = 0.0f;
    // How many times it was told anything.
    u32 calls = 0;
};
[[nodiscard]] VirtualRumble virtualGamepadRumble() noexcept;

// Whether the platform library was built with a joystick subsystem at all.
// Starts it if nothing has.
[[nodiscard]] bool gamepadsAvailable() noexcept;

// Whether this run looks for gamepads (`InitOptions::gamepads`), and whether
// it has begun to: the pump starts the subsystem the second time it runs.
void setGamepadsWanted(bool wanted) noexcept;
[[nodiscard]] bool gamepadsStarted() noexcept;

// Whether the platform layer delivers `TextInput` events for this window.
//
// Off by default, and that is SDL's rule rather than ours: a game that never
// asks pays no IME cost and, on a phone, gets no on-screen keyboard. `ui` turns
// it on while a `TextInput` element holds focus and off again when it does not
// (`app::TextInputFocus`, D204).
void setTextInputEnabled(u32 windowId, bool enabled) noexcept;
[[nodiscard]] bool textInputEnabled(u32 windowId) noexcept;

// **What the text being typed is** (ADR 0139): the on-screen keyboard a phone
// raises, and the hints a desktop input method reads.
enum class TextInputKind : u8
{
    Text,
    Number,
    Decimal,
    Phone,
    Email,
    Url,
    // Masked: no suggestions, nothing remembered.
    Password,
};

struct TextInputOptions
{
    TextInputKind kind = TextInputKind::Text;
    // Return makes a new line rather than finishing.
    bool multiLine = false;
};

// Turns text input on with `options`, or off. Turning it on again with other
// options restarts it, which is how a phone changes its keyboard.
void setTextInputEnabled(u32 windowId, bool enabled, const TextInputOptions& options) noexcept;

// **Where the caret is, for the input method** (ADR 0139): the field's
// rectangle in window pixels and the caret's x inside it, so a composition's
// candidate list opens beside the text rather than in a corner.
void setTextInputArea(u32 windowId, i32 x, i32 y, i32 width, i32 height, i32 cursor) noexcept;

// Drains the OS queue and returns this frame's translated events. The span is
// owned by the module and stays valid until the next call, so a caller that
// wants to keep an event past the frame must copy it.
//
// Events SDL reports that the engine does not model yet are dropped here but
// remain visible through sdl_interop.h, which is how the ImGui backend gets
// the full stream without the engine pretending to model input it does not.
//
// **It comes back while the system holds the window too** (D535). On Windows a
// title bar held, a border dragged or a window's menu open is a loop the system
// runs itself, inside the call that asks it for events, until the hand lets go.
// The pump returns from inside it about sixty times a second, with what the
// window did meanwhile, so the loop that calls it goes on simulating, sending
// and drawing; `pumpHeld` says when that is where it came back from.
[[nodiscard]] std::span<const Event> pumpEvents();

// Whether the last `pumpEvents` came back from inside a loop the system is
// still running for a held window. False everywhere but Windows: nowhere else
// does moving a window stop the thread that owns it.
[[nodiscard]] bool pumpHeld() noexcept;

// **For the tests of that**: holds the window as the system does while its
// title bar is held -- the same notices to the window, the same loop inside the
// next `pumpEvents` -- for `milliseconds`, with no hand and no pointer. False
// where there is no such loop (everywhere but Windows) or no such window.
bool simulateWindowHold(u32 windowId, u32 milliseconds) noexcept;

} // namespace engine::platform
