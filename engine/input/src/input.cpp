#include "engine/input/input.h"

#include <algorithm>
#include <cmath>

#include "engine/scene/world.h"

namespace engine::input {
namespace {

// `Enum.KeyCode`'s layout, as ranges rather than as ninety-four constants.
//
// The enum is generated from `api/defs/enums.api.luau` in exactly this order and
// numbered sequentially from 0, which is the file's own stated rule. These
// bounds are that rule written down where the resolver can use it, and
// `registerSceneTypes` checks them against the registered descriptor at boot --
// so a KeyCode item inserted in the middle is a boot failure rather than a
// binding that quietly names a different key.
constexpr i32 KeyboardFirst = 1;
constexpr i32 KeyboardCount = 66;

// The keyboard block IS `platform::Key`, item for item and in the same order,
// which is what makes `keyCodeOf` a subtraction rather than a table. Asserted
// rather than assumed: a key added to one enum and not the other would silently
// shift every gamepad code by one.
// The rest of the keyboard (D210), at the END of the enum so no value a game
// already held moved: `platform::Key` goes on past `Delete` with them, and
// `Enum.KeyCode` carries them after the virtual block.
constexpr i32 ExtraKeysCount = 33;
static_assert(static_cast<i32>(platform::Key::Count) == KeyboardCount + ExtraKeysCount + 1,
              "Enum.KeyCode's two keyboard blocks and platform::Key must be the same list");
constexpr i32 MouseButtonFirst = KeyboardFirst + KeyboardCount; // 67
constexpr i32 MouseButtonCount = 5;
constexpr i32 MouseMovement = MouseButtonFirst + MouseButtonCount; // 72
constexpr i32 MouseWheel = MouseMovement + 1;                      // 73
constexpr i32 PadButtonFirst = MouseWheel + 1;                     // 74
constexpr i32 PadButtonCount = 15;
constexpr i32 PadAxisFirst = PadButtonFirst + PadButtonCount; // 89
constexpr i32 PadAxisCount = 6;
constexpr i32 LeftThumbstick = PadAxisFirst + PadAxisCount; // 95
constexpr i32 RightThumbstick = LeftThumbstick + 1;         // 96

// The virtual block (M6): four axes a script writes and two composites over
// them. Not hardware, and deliberately IN the same arrays as hardware -- a
// virtual source that lived somewhere else would be a second input model, and
// the recorded stream would not carry it.
constexpr i32 VirtualFirst = RightThumbstick + 1; // 97
constexpr i32 VirtualCount = 4;
constexpr i32 VirtualStick1 = VirtualFirst + VirtualCount; // 101
constexpr i32 VirtualStick2 = VirtualStick1 + 1;           // 102
constexpr i32 ExtraKeysFirst = VirtualStick2 + 1;          // 103

// The axes, by name, so the two stick composites can find their halves.
constexpr i32 LeftStickX = PadAxisFirst;
constexpr i32 LeftStickY = PadAxisFirst + 1;
constexpr i32 RightStickX = PadAxisFirst + 2;
constexpr i32 RightStickY = PadAxisFirst + 3;

// **The rest of the virtual keys** (D443), `Virtual5` to `Virtual16`, after
// the appended keyboard for the reason that block is where it is: no value a
// game already held moves. Four was "a thumbstick and two or three buttons",
// and the first action game on a phone had a skill bar. They are keys like the
// first four and halves of no stick.
constexpr i32 MoreVirtualFirst = ExtraKeysFirst + ExtraKeysCount; // 136
constexpr i32 MoreVirtualCount = 12;

// **The four swipes** (D462): `SwipeUp`, `SwipeDown`, `SwipeLeft`,
// `SwipeRight`, in `Enum.SwipeDirection`'s order. Codes like any other, so an
// action binds one beside a key; down for the one tick the swipe is
// recognised in, which is a press.
constexpr i32 SwipeFirst = MoreVirtualFirst + MoreVirtualCount; // 148
constexpr i32 SwipeCount = 4;

static_assert(SwipeFirst + SwipeCount == static_cast<i32>(kKeyCodeCount),
              "the KeyCode ranges above must cover the whole enum with no gap");

// How long a finger may stay down and still be a tap, and how long it must
// stay to be a long press, in ticks: a third and a half of a second at sixty.
constexpr u64 TapTicks = 20;
constexpr u64 LongPressTicks = 30;

[[nodiscard]] constexpr bool inRange(i32 value, i32 first, i32 count) noexcept
{
    return value >= first && value < first + count;
}

[[nodiscard]] i32 keyCodeOf(platform::Key key) noexcept
{
    const auto raw = static_cast<i32>(key);
    if (raw > KeyboardCount && raw <= KeyboardCount + ExtraKeysCount)
        return ExtraKeysFirst + raw - KeyboardCount - 1;
    if (raw <= 0 || raw > KeyboardCount)
        return 0;
    // `platform::Key` counts from 1 after `Unknown`, and so does the keyboard
    // block of `Enum.KeyCode`, in the same order. The two tables are generated
    // from one list on purpose (api/defs/enums.api.luau's header says so).
    return KeyboardFirst + raw - 1;
}

[[nodiscard]] i32 keyCodeOf(platform::MouseButton button) noexcept
{
    const auto raw = static_cast<i32>(button);
    if (raw <= 0 || raw > MouseButtonCount)
        return 0;
    return MouseButtonFirst + raw - 1;
}

[[nodiscard]] i32 keyCodeOf(platform::GamepadButton button) noexcept
{
    const auto raw = static_cast<i32>(button);
    if (raw <= 0 || raw > PadButtonCount)
        return 0;
    return PadButtonFirst + raw - 1;
}

[[nodiscard]] i32 keyCodeOf(platform::GamepadAxis axis) noexcept
{
    const auto raw = static_cast<i32>(axis);
    if (raw <= 0 || raw > PadAxisCount)
        return 0;
    return PadAxisFirst + raw - 1;
}

[[nodiscard]] bool valid(i32 keyCode) noexcept
{
    return keyCode > 0 && keyCode < static_cast<i32>(kKeyCodeCount);
}

// Half deflection, which is what an analogue source bound to a `Bool` action
// counts as pressed past. Named rather than written twice, because the trigger
// path and the stick path both need it and a threshold that drifted apart
// between them would be a trigger that fires at a different point than the
// stick that mirrors it.
constexpr f32 AnalogPressThreshold = 0.5f;

} // namespace

namespace {

// One a script may WRITE: a virtual key, and not a stick made of two.
[[nodiscard]] constexpr bool isVirtualKey(i32 keyCode) noexcept
{
    return inRange(keyCode, VirtualFirst, VirtualCount) || inRange(keyCode, MoreVirtualFirst, MoreVirtualCount);
}

} // namespace

bool isVirtual(i32 keyCode) noexcept
{
    return isVirtualKey(keyCode) || keyCode == VirtualStick1 || keyCode == VirtualStick2;
}

DeviceType deviceOf(i32 keyCode) noexcept
{
    if (inRange(keyCode, PadButtonFirst, PadButtonCount) || inRange(keyCode, PadAxisFirst, PadAxisCount) ||
        keyCode == LeftThumbstick || keyCode == RightThumbstick) {
        return DeviceType::Gamepad;
    }
    if (inRange(keyCode, SwipeFirst, SwipeCount))
        return DeviceType::Touch;
    // The roadmap's clause, honoured: an on-screen control is the same thing a
    // touch control will be, so the virtual family reports `Touch` rather than
    // growing a fourth item nobody asked for.
    if (isVirtual(keyCode))
        return DeviceType::Touch;
    // Everything else, `Unknown` included. There is no third answer to give:
    // a binding that names nothing is not a touch binding.
    return DeviceType::KeyboardMouse;
}

bool isAnalog(i32 keyCode) noexcept
{
    // Virtual included: the seam carries a VALUE and not a press, so a HUD
    // button writes 1 and a thumbstick writes a deflection, and both go through
    // the same half-deflection rule when a `Bool` action asks.
    return keyCode == MouseMovement || keyCode == MouseWheel || inRange(keyCode, PadAxisFirst, PadAxisCount) ||
           keyCode == LeftThumbstick || keyCode == RightThumbstick || isVirtual(keyCode);
}

// The four names no device event carries, so no `platform` table has them.
// Indexed by KeyCode minus the block's first value, which is why they are laid
// out as two pairs rather than as a map.
constexpr std::string_view AnalogNames[] = {"MouseMovement", "MouseWheel"};
constexpr std::string_view StickNames[] = {"LeftThumbstick", "RightThumbstick"};
constexpr std::string_view VirtualNames[] = {"Virtual1", "Virtual2", "Virtual3", "Virtual4"};
constexpr std::string_view MoreVirtualNames[] = {"Virtual5",  "Virtual6",  "Virtual7",  "Virtual8",
                                                 "Virtual9",  "Virtual10", "Virtual11", "Virtual12",
                                                 "Virtual13", "Virtual14", "Virtual15", "Virtual16"};
static_assert(std::size(MoreVirtualNames) == static_cast<usize>(MoreVirtualCount));
constexpr std::string_view SwipeNames[] = {"SwipeUp", "SwipeDown", "SwipeLeft", "SwipeRight"};
static_assert(std::size(SwipeNames) == static_cast<usize>(SwipeCount));
constexpr std::string_view VirtualStickNames[] = {"VirtualStick1", "VirtualStick2"};

i32 keyCodeFromName(std::string_view name) noexcept
{
    if (name.empty())
        return 0;

    if (const platform::Key key = platform::keyFromName(name); key != platform::Key::Unknown)
        return keyCodeOf(key);
    if (const platform::MouseButton button = platform::mouseButtonFromName(name);
        button != platform::MouseButton::Unknown) {
        return keyCodeOf(button);
    }
    if (const platform::GamepadButton button = platform::gamepadButtonFromName(name);
        button != platform::GamepadButton::Unknown) {
        return keyCodeOf(button);
    }
    if (const platform::GamepadAxis axis = platform::gamepadAxisFromName(name);
        axis != platform::GamepadAxis::Unknown) {
        return keyCodeOf(axis);
    }

    for (i32 index = 0; index < 2; ++index) {
        if (name == AnalogNames[index])
            return MouseMovement + index;
        if (name == StickNames[index])
            return LeftThumbstick + index;
        if (name == VirtualStickNames[index])
            return VirtualStick1 + index;
    }
    for (i32 index = 0; index < VirtualCount; ++index) {
        if (name == VirtualNames[index])
            return VirtualFirst + index;
    }
    for (i32 index = 0; index < MoreVirtualCount; ++index) {
        if (name == MoreVirtualNames[index])
            return MoreVirtualFirst + index;
    }
    for (i32 index = 0; index < SwipeCount; ++index) {
        if (name == SwipeNames[index])
            return SwipeFirst + index;
    }
    return 0;
}

std::string_view keyCodeName(i32 keyCode) noexcept
{
    if (inRange(keyCode, KeyboardFirst, KeyboardCount))
        return platform::keyName(static_cast<platform::Key>(keyCode - KeyboardFirst + 1));
    if (inRange(keyCode, ExtraKeysFirst, ExtraKeysCount))
        return platform::keyName(static_cast<platform::Key>(KeyboardCount + 1 + keyCode - ExtraKeysFirst));
    if (inRange(keyCode, MouseButtonFirst, MouseButtonCount))
        return platform::mouseButtonName(static_cast<platform::MouseButton>(keyCode - MouseButtonFirst + 1));
    if (inRange(keyCode, PadButtonFirst, PadButtonCount))
        return platform::gamepadButtonName(static_cast<platform::GamepadButton>(keyCode - PadButtonFirst + 1));
    if (inRange(keyCode, PadAxisFirst, PadAxisCount))
        return platform::gamepadAxisName(static_cast<platform::GamepadAxis>(keyCode - PadAxisFirst + 1));
    if (keyCode == MouseMovement || keyCode == MouseWheel)
        return AnalogNames[keyCode - MouseMovement];
    if (inRange(keyCode, VirtualFirst, VirtualCount))
        return VirtualNames[keyCode - VirtualFirst];
    if (inRange(keyCode, MoreVirtualFirst, MoreVirtualCount))
        return MoreVirtualNames[keyCode - MoreVirtualFirst];
    if (inRange(keyCode, SwipeFirst, SwipeCount))
        return SwipeNames[keyCode - SwipeFirst];
    if (keyCode == VirtualStick1 || keyCode == VirtualStick2)
        return VirtualStickNames[keyCode - VirtualStick1];
    if (keyCode == LeftThumbstick || keyCode == RightThumbstick)
        return StickNames[keyCode - LeftThumbstick];
    return {};
}

void InputSystem::setHeld(i32 code, bool down) noexcept
{
    const auto at = static_cast<usize>(code);
    if (down) {
        m_state.held[at] = true;
        m_downUnseen[at] = true;
        m_releaseDeferred[at] = false;
    }
    else if (m_downUnseen[at]) {
        // Pressed and let go before a tick read it: down for that tick, then up.
        m_releaseDeferred[at] = true;
    }
    else {
        m_state.held[at] = false;
    }
}

void InputSystem::pumpFrame(std::span<const platform::Event> events)
{
    for (const platform::Event& event : events) {
        // The mouse the system makes out of a finger: the finger itself is
        // what this follows (see `platform::Event::fromTouch`).
        if (event.fromTouch)
            continue;
        switch (event.type) {
        case platform::EventType::KeyDown:
        case platform::EventType::KeyUp: {
            const i32 code = keyCodeOf(event.key);
            if (!valid(code))
                break;
            // A repeat is a key that is already down. Recording it as a fresh
            // press would make `Pressed` fire again every autorepeat interval,
            // which is a jump per repeat rather than a jump per press.
            setHeld(code, event.type == platform::EventType::KeyDown);
            m_state.lastDevice = DeviceType::KeyboardMouse;
            break;
        }
        case platform::EventType::MouseButtonDown:
        case platform::EventType::MouseButtonUp: {
            const i32 code = keyCodeOf(event.button);
            if (!valid(code))
                break;
            setHeld(code, event.type == platform::EventType::MouseButtonDown);
            m_state.lastDevice = DeviceType::KeyboardMouse;
            break;
        }
        case platform::EventType::MouseMoved:
            m_state.pointer = core::Vec2{event.pointerX, event.pointerY};
            // Accumulated, not sampled: several motion events arrive per frame,
            // and a tick that read only the last one would lose most of a fast
            // flick. Y is negated so that moving the mouse away from the player
            // is +Y, which is the direction the `Up` composite means.
            m_simPointerDelta = m_simPointerDelta + core::Vec2{event.pointerDeltaX, -event.pointerDeltaY};
            m_renderPointerDelta = m_renderPointerDelta + core::Vec2{event.pointerDeltaX, -event.pointerDeltaY};
            // Deliberately does NOT set `lastDevice`: a mouse nudged by a desk
            // bump would otherwise steal every prompt on screen from a gamepad
            // the player is holding.
            break;
        case platform::EventType::MouseWheel:
            m_simWheel = m_simWheel + core::Vec2{event.wheelX, event.wheelY};
            m_renderWheel = m_renderWheel + core::Vec2{event.wheelX, event.wheelY};
            m_state.lastDevice = DeviceType::KeyboardMouse;
            break;
        case platform::EventType::GamepadButtonDown:
        case platform::EventType::GamepadButtonUp: {
            const i32 code = keyCodeOf(event.gamepadButton);
            if (!valid(code))
                break;
            setHeld(code, event.type == platform::EventType::GamepadButtonDown);
            m_state.lastDevice = DeviceType::Gamepad;
            break;
        }
        case platform::EventType::GamepadAxisMoved: {
            const i32 code = keyCodeOf(event.gamepadAxis);
            if (!valid(code))
                break;
            m_state.axis[static_cast<usize>(code)] = event.axisValue;
            // Only a real deflection claims the device. A stick resting inside
            // its dead zone still emits events on most hardware, and letting
            // those set `lastDevice` would flip a HUD's prompts to gamepad
            // while nobody is touching one.
            if (std::abs(event.axisValue) > AnalogPressThreshold)
                m_state.lastDevice = DeviceType::Gamepad;
            break;
        }
        case platform::EventType::GamepadRemoved:
            // Every gamepad input goes to rest. The pad is gone; anything still
            // recorded as held would stay held forever.
            for (i32 code = PadButtonFirst; code < PadButtonFirst + PadButtonCount; ++code)
                m_state.held[static_cast<usize>(code)] = false;
            for (i32 code = PadAxisFirst; code < PadAxisFirst + PadAxisCount; ++code)
                m_state.axis[static_cast<usize>(code)] = 0.0f;
            break;
        case platform::EventType::FingerDown: {
            // The slot this finger already holds, or the first free one.
            Finger* slot = nullptr;
            for (Finger& finger : m_state.fingers) {
                if (finger.down && finger.id == event.fingerId) {
                    slot = &finger;
                    break;
                }
            }
            for (usize index = 0; slot == nullptr && index < m_state.fingers.size(); ++index) {
                // Free, and not still ending in the previous snapshot: reusing
                // it on the same tick would turn an `Ended` into a `Changed`.
                if (!m_state.fingers[index].down && !(m_hasPrevious && m_previous.fingers[index].down))
                    slot = &m_state.fingers[index];
            }
            if (slot == nullptr)
                break;
            *slot = Finger{.down = true,
                           .lifting = false,
                           .id = event.fingerId,
                           .position = core::Vec2{event.pointerX, event.pointerY},
                           .origin = core::Vec2{event.pointerX, event.pointerY}};
            m_state.lastDevice = DeviceType::Touch;
            break;
        }
        case platform::EventType::FingerMoved:
        case platform::EventType::FingerUp: {
            for (usize index = 0; index < m_state.fingers.size(); ++index) {
                Finger& finger = m_state.fingers[index];
                if (!finger.down || finger.id != event.fingerId)
                    continue;
                finger.position = core::Vec2{event.pointerX, event.pointerY};
                if (event.type == platform::EventType::FingerUp) {
                    // Seen down by a tick: it ends now. Not yet: it ends one
                    // tick after it began, so the tap is not lost.
                    if (m_hasPrevious && m_previous.fingers[index].down && m_previous.fingers[index].id == finger.id)
                        finger.down = false;
                    else
                        finger.lifting = true;
                }
                break;
            }
            break;
        }
        case platform::EventType::WindowFocusGained:
            m_state.focused = true;
            break;
        case platform::EventType::WindowFocusLost:
            m_state.focused = false;
            break;
        default:
            break;
        }
    }
}

void InputSystem::setSnapshot(const DeviceState& state) noexcept
{
    m_state = state;
    // The deltas come from the snapshot rather than accumulating on top of it:
    // a replay hands the state a tick should see, and adding the live mouse to
    // it would make the replay depend on whether anybody moved the pointer.
    m_simPointerDelta = state.pointerDelta;
    m_renderPointerDelta = state.pointerDelta;
    m_simWheel = state.wheel;
    m_renderWheel = state.wheel;
}

namespace {

// One binding's contribution to its action, in the action's own currency.
struct Contribution
{
    bool pressed = false;
    core::Vec3 axis;
};

[[nodiscard]] bool digital(const DeviceState& state, const std::array<bool, kKeyCodeCount>& consumed, i32 code) noexcept
{
    if (!valid(code) || consumed[static_cast<usize>(code)])
        return false;
    if (isAnalog(code)) {
        // An analogue source on a digital question. `Enum.KeyCode`'s doc states
        // the answer rather than leaving it to be discovered: past half
        // deflection counts as pressed, because refusing the binding outright
        // would let a rebinding UI hand the player an unusable choice.
        if (code == LeftThumbstick)
            return std::abs(state.axis[LeftStickX]) > AnalogPressThreshold ||
                   std::abs(state.axis[LeftStickY]) > AnalogPressThreshold;
        if (code == RightThumbstick)
            return std::abs(state.axis[RightStickX]) > AnalogPressThreshold ||
                   std::abs(state.axis[RightStickY]) > AnalogPressThreshold;
        if (code == VirtualStick1 || code == VirtualStick2) {
            const usize first = static_cast<usize>(VirtualFirst + (code - VirtualStick1) * 2);
            return std::abs(state.axis[first]) > AnalogPressThreshold ||
                   std::abs(state.axis[first + 1]) > AnalogPressThreshold;
        }
        return std::abs(state.axis[static_cast<usize>(code)]) > AnalogPressThreshold;
    }
    return state.held[static_cast<usize>(code)];
}

// The signed contribution of a pair of composite keys: +1 for the positive one,
// -1 for the negative one, 0 for both or neither. Both-at-once cancelling is
// what makes holding A and D stand still rather than drift by whichever the
// engine happened to read last.
[[nodiscard]] f32 composite(const DeviceState& state, const std::array<bool, kKeyCodeCount>& consumed, i32 positive,
                            i32 negative) noexcept
{
    const f32 up = digital(state, consumed, positive) ? 1.0f : 0.0f;
    const f32 down = digital(state, consumed, negative) ? 1.0f : 0.0f;
    return up - down;
}

[[nodiscard]] core::Vec2 stick(const DeviceState& state, i32 code) noexcept
{
    if (code == LeftThumbstick) {
        // Y negated, because SDL reports a stick's Y positive DOWNWARD and the
        // `Up` composite means +Y. One convention reaches the game, and this is
        // the line that establishes it.
        return core::Vec2{state.axis[LeftStickX], -state.axis[LeftStickY]};
    }
    if (code == VirtualStick1 || code == VirtualStick2) {
        // NOT negated: a virtual axis is written by a script in the engine's own
        // convention, so there is no hardware convention to undo. A script that
        // wants an inverted stick writes a negative number, or sets the
        // binding's `Scale` to -1 like a settings screen does.
        const usize first = static_cast<usize>(VirtualFirst + (code - VirtualStick1) * 2);
        return core::Vec2{state.axis[first], state.axis[first + 1]};
    }
    return core::Vec2{state.axis[RightStickX], -state.axis[RightStickY]};
}

} // namespace

namespace {

// Which `Enum.UserInputType` a `KeyCode` produces. Coarser than `deviceOf`,
// because these are the kinds a raw handler switches on rather than the families
// a prompt draws for.
[[nodiscard]] UserInputType userInputTypeOf(i32 keyCode) noexcept
{
    if (inRange(keyCode, KeyboardFirst, KeyboardCount))
        return UserInputType::Keyboard;
    if (keyCode == MouseMovement)
        return UserInputType::MouseMovement;
    if (keyCode == MouseWheel)
        return UserInputType::MouseWheel;
    if (inRange(keyCode, MouseButtonFirst, MouseButtonCount)) {
        // The first three get their own items because `== MouseButton1` is the
        // overwhelmingly common test; the fourth and fifth have no name in the
        // enum and report as the primary's neighbour rather than as `None`.
        switch (keyCode - MouseButtonFirst) {
        case 0:
            return UserInputType::MouseButton1;
        case 1:
            return UserInputType::MouseButton2;
        case 2:
            return UserInputType::MouseButton3;
        default:
            return UserInputType::MouseButton1;
        }
    }
    if (inRange(keyCode, PadButtonFirst, PadButtonCount) || inRange(keyCode, PadAxisFirst, PadAxisCount) ||
        keyCode == LeftThumbstick || keyCode == RightThumbstick)
        return UserInputType::Gamepad;
    return UserInputType::None;
}

// Whether the interface already took this input. Two claims, one per device
// family, and each one covers exactly the codes that family produces -- which is
// what stops a HUD button under the pointer from also eating the jump key.
[[nodiscard]] bool consumedByUi(i32 keyCode, bool pointerCaptured, bool keyboardCaptured) noexcept
{
    if (pointerCaptured &&
        (inRange(keyCode, MouseButtonFirst, MouseButtonCount) || keyCode == MouseMovement || keyCode == MouseWheel))
        return true;
    return keyboardCaptured && inRange(keyCode, KeyboardFirst, KeyboardCount);
}

} // namespace

void InputSystem::setFingerTakenByUi(u64 fingerId)
{
    if (std::find(m_uiFingers.begin(), m_uiFingers.end(), fingerId) == m_uiFingers.end())
        m_uiFingers.push_back(fingerId);
}

void InputSystem::setVirtualState(i32 keyCode, f32 value) noexcept
{
    if (!isVirtualKey(keyCode))
        return;
    m_state.axis[static_cast<usize>(keyCode)] = value;
    // A virtual press marks the device family too, so a HUD that switches its
    // prompts follows the on-screen control the way it follows a gamepad.
    if (std::abs(value) > AnalogPressThreshold)
        m_state.lastDevice = DeviceType::Touch;
}

void InputSystem::setActionState(std::string_view action, core::Vec3 value, bool pressed)
{
    for (HeldAction& held : m_held) {
        if (held.name == action) {
            held.value = value;
            held.pressed = pressed;
            return;
        }
    }
    m_held.push_back(HeldAction{std::string(action), value, pressed});
}

bool InputSystem::isKeyDown(i32 keyCode) const noexcept
{
    // The same `digital`, with nothing consumed: a poll is about the device --
    // so a tap held down for the tick that has not read it yet is up here.
    static const std::array<bool, kKeyCodeCount> nothingConsumed{};
    if (keyCode >= 0 && static_cast<usize>(keyCode) < m_releaseDeferred.size() &&
        m_releaseDeferred[static_cast<usize>(keyCode)])
        return false;
    return digital(m_state, nothingConsumed, keyCode);
}

std::span<const RawInputEvent> InputSystem::drainRawEvents() noexcept
{
    m_rawDrained.swap(m_rawEvents);
    m_rawEvents.clear();
    return m_rawDrained;
}

void InputSystem::collectRawEvents(core::Vec2 pointerDelta, core::Vec2 wheel)
{
    m_rawEvents.clear();

    const core::Vec3 pointer{m_state.pointer.x, m_state.pointer.y, 0.0f};

    // **Walked by KeyCode, ascending.** The order these fire in is observable --
    // a handler may write to the world -- so it has to come from something that
    // promises one (R10), and an array index is the cheapest promise there is.
    for (i32 code = 1; code < static_cast<i32>(kKeyCodeCount); ++code) {
        // The composites are a way of READING two axes together, not inputs of
        // their own: a stick pushed left is one event about `LeftStickX`, and
        // a second one saying `LeftThumbstick` began would be the same fact
        // twice under a name no device produced.
        if (code == LeftThumbstick || code == RightThumbstick || code == VirtualStick1 || code == VirtualStick2)
            continue;
        // A swipe is said by `TouchSwiped`, with where it began and with how
        // many fingers; as a raw press it would be a key no device has.
        if (inRange(code, SwipeFirst, SwipeCount))
            continue;

        const auto slot = static_cast<usize>(code);
        // `digital` rather than `held`, so a trigger crossing half deflection
        // begins and ends like a button -- which is what `Enum.KeyCode`'s own
        // doc promises and what a `Bool` action already does with one.
        static const std::array<bool, kKeyCodeCount> nothingConsumed{};
        const bool held = digital(m_state, nothingConsumed, code);
        const bool was = m_hasPrevious && digital(m_previous, nothingConsumed, code);
        if (held == was)
            continue;

        const UserInputType kind = userInputTypeOf(code);
        const bool consumed =
            held ? consumedByUi(code, m_uiCapturedPointer, m_uiCapturedKeyboard) : m_beganConsumed[slot];
        if (held)
            m_beganConsumed[slot] = consumed;

        RawInputEvent event;
        event.phase = held ? RawInputEvent::Phase::Began : RawInputEvent::Phase::Ended;
        event.userInputType = kind;
        event.keyCode = code;
        event.position = pointer;
        event.uiConsumed = consumed;
        m_rawEvents.push_back(event);
    }

    // Motion, once per tick, with the delta ACCUMULATED since the last dispatch.
    // A handler that saw only the last device event would lose most of a fast
    // flick, which is the same reason the deltas are accumulated at all.
    if (pointerDelta.x != 0.0f || pointerDelta.y != 0.0f) {
        RawInputEvent event;
        event.phase = RawInputEvent::Phase::Changed;
        event.userInputType = UserInputType::MouseMovement;
        event.position = pointer;
        event.delta = core::Vec3{pointerDelta.x, pointerDelta.y, 0.0f};
        event.uiConsumed = m_uiCapturedPointer;
        m_rawEvents.push_back(event);
    }

    if (wheel.x != 0.0f || wheel.y != 0.0f) {
        RawInputEvent event;
        event.phase = RawInputEvent::Phase::Changed;
        event.userInputType = UserInputType::MouseWheel;
        // `z` is where the wheel lives, in both fields, so a handler reads one
        // component whichever it reached for.
        event.position = core::Vec3{m_state.pointer.x, m_state.pointer.y, wheel.y};
        event.delta = core::Vec3{wheel.x, 0.0f, wheel.y};
        event.uiConsumed = m_uiCapturedPointer;
        m_rawEvents.push_back(event);
    }

    // Gamepad axes, which have no press to begin or end: a stick that moved is
    // `InputChanged` with its deflection, and one resting at the same value
    // produces nothing at all.
    for (i32 code = PadAxisFirst; code < PadAxisFirst + PadAxisCount; ++code) {
        const auto slot = static_cast<usize>(code);
        const f32 value = m_state.axis[slot];
        const f32 was = m_hasPrevious ? m_previous.axis[slot] : 0.0f;
        if (value == was)
            continue;

        RawInputEvent event;
        event.phase = RawInputEvent::Phase::Changed;
        event.userInputType = UserInputType::Gamepad;
        event.keyCode = code;
        event.position = core::Vec3{value, 0.0f, 0.0f};
        event.delta = core::Vec3{value - was, 0.0f, 0.0f};
        m_rawEvents.push_back(event);
    }

    // **Fingers, by slot**: each begins, moves and ends on its own, and its
    // slot is its `TouchId`. After the keys and the pad, in slot order (R10).
    //
    // **One that came down on the interface says so** (D444), from its
    // `Began` to its `Ended`: a game that also aims by tapping the world has
    // to know a button was under the finger, exactly as it does for a click.
    // It is still reported -- a game's own on-screen controls are made of
    // these -- and what the handler does with the flag is the handler's.
    const auto taken = [this](u64 id) {
        return std::find(m_uiFingers.begin(), m_uiFingers.end(), id) != m_uiFingers.end();
    };
    for (usize index = 0; index < m_state.fingers.size(); ++index) {
        const Finger& now = m_state.fingers[index];
        const Finger before = m_hasPrevious ? m_previous.fingers[index] : Finger{};
        const bool sameFinger = before.down && now.down && before.id == now.id;
        RawInputEvent event;
        event.userInputType = UserInputType::Touch;
        event.touchId = static_cast<i32>(index) + 1;
        if (before.down && !sameFinger) {
            // Lifted -- or lifted and replaced by a new finger in one frame,
            // which ends the old one before the new one begins.
            event.phase = RawInputEvent::Phase::Ended;
            event.position = core::Vec3{before.position.x, before.position.y, 0.0f};
            event.uiConsumed = taken(before.id);
            m_rawEvents.push_back(event);
            // Unless the same finger is the one that is down again already (a
            // tap and a second tap inside one tick reuse the id on some
            // devices): then the claim is the new press's.
            if (!(now.down && now.id == before.id))
                std::erase(m_uiFingers, before.id);
        }
        event.uiConsumed = now.down && taken(now.id);
        if (now.down && !sameFinger) {
            event.phase = RawInputEvent::Phase::Began;
            event.position = core::Vec3{now.position.x, now.position.y, 0.0f};
            m_rawEvents.push_back(event);
        }
        else if (sameFinger && (now.position.x != before.position.x || now.position.y != before.position.y)) {
            event.phase = RawInputEvent::Phase::Changed;
            event.position = core::Vec3{now.position.x, now.position.y, 0.0f};
            event.delta = core::Vec3{now.position.x - before.position.x, now.position.y - before.position.y, 0.0f};
            m_rawEvents.push_back(event);
        }
    }

    collectGestures();

    m_previous = m_state;
    m_hasPrevious = true;
    // A claim outlives its finger by nothing: one whose finger never took a
    // slot (a sixth finger), or was swept away with the focus, goes here.
    std::erase_if(m_uiFingers, [this](u64 id) {
        return std::none_of(m_previous.fingers.begin(), m_previous.fingers.end(),
                            [id](const Finger& finger) { return finger.down && finger.id == id; });
    });
    // A tap shorter than a frame has now been seen down; it ends next tick.
    for (Finger& finger : m_state.fingers) {
        if (finger.lifting) {
            finger.down = false;
            finger.lifting = false;
        }
    }
}

void InputSystem::collectGestures()
{
    ++m_gestureTick;
    m_gestures.clear();
    // Last tick's swipes were presses of one tick: they are up again.
    for (i32 code = SwipeFirst; code < SwipeFirst + SwipeCount; ++code)
        m_state.held[static_cast<usize>(code)] = false;

    const f32 threshold = m_swipeThreshold;
    const auto taken = [this](u64 id) {
        return std::find(m_uiFingers.begin(), m_uiFingers.end(), id) != m_uiFingers.end();
    };

    // What is down now, slot by slot, and the mouse last: its left button is a
    // finger of its own, so the same motion is the same gesture on a desk.
    struct Contact
    {
        bool down = false;
        bool ui = false;
        u64 id = 0;
        core::Vec2 position;
        core::Vec2 origin;
    };
    std::array<Contact, kMaxFingers + 1> contacts{};
    for (usize index = 0; index < kMaxFingers; ++index) {
        const Finger& finger = m_state.fingers[index];
        contacts[index] =
            Contact{finger.down, finger.down && taken(finger.id), finger.id, finger.position, finger.origin};
    }
    {
        const auto left = static_cast<usize>(MouseButtonFirst);
        Contact& mouse = contacts[kMaxFingers];
        mouse.down = m_state.held[left];
        // What the interface took when the button went down stays its own
        // until it comes up, wherever the pointer is dragged to.
        mouse.ui = mouse.down && m_beganConsumed[left];
        mouse.id = 1;
        mouse.position = m_state.pointer;
        mouse.origin = m_tracks[kMaxFingers].active ? m_tracks[kMaxFingers].start : m_state.pointer;
    }

    i32 fingersDown = 0;
    for (const Contact& contact : contacts)
        fingersDown += contact.down && !contact.ui ? 1 : 0;

    const auto ended = [this](GestureTrack& track) {
        // Down and up again without going anywhere, and not held: a tap.
        if (!track.ui && !track.moved && !track.longFired && m_gestureTick - track.downTick <= TapTicks) {
            GestureEvent tap;
            tap.kind = GestureEvent::Kind::Tap;
            tap.position = track.last;
            m_gestures.push_back(tap);
        }
        track = GestureTrack{};
    };

    // The one a swipe is read from: the first finger down, or the mouse.
    bool primaryFound = false;
    core::Vec2 panTotal;
    i32 panCount = 0;
    for (usize index = 0; index < contacts.size(); ++index) {
        const Contact& contact = contacts[index];
        GestureTrack& track = m_tracks[index];
        const bool same = track.active && contact.down && track.id == contact.id;
        if (track.active && !same)
            ended(track);
        if (!contact.down)
            continue;
        const bool began = !same;
        if (began) {
            track.active = true;
            track.ui = contact.ui;
            track.id = contact.id;
            track.start = contact.origin;
            track.anchor = contact.origin;
            track.last = contact.origin;
            track.direction = -1;
            track.downTick = m_gestureTick;
        }
        if (track.ui)
            continue;

        const core::Vec2 fromStart{contact.position.x - track.start.x, contact.position.y - track.start.y};
        if (std::max(std::abs(fromStart.x), std::abs(fromStart.y)) >= threshold)
            track.moved = true;

        // **A swipe, while the finger is still down**: once it has travelled
        // the threshold along one axis. One long drag one way is ONE swipe --
        // the anchor follows the finger and nothing more is said -- and a
        // change of direction, measured from where the finger then is, is the
        // next.
        if (!primaryFound) {
            primaryFound = true;
            const core::Vec2 travelled{contact.position.x - track.anchor.x, contact.position.y - track.anchor.y};
            const f32 across = std::abs(travelled.x);
            const f32 along = std::abs(travelled.y);
            if (std::max(across, along) >= threshold) {
                // 0 Up, 1 Down, 2 Left, 3 Right; the window's y grows downwards.
                const i32 direction = across >= along ? (travelled.x > 0.0f ? 3 : 2) : (travelled.y > 0.0f ? 1 : 0);
                if (direction != track.direction) {
                    GestureEvent swipe;
                    swipe.kind = GestureEvent::Kind::Swipe;
                    swipe.direction = direction;
                    swipe.position = track.anchor;
                    swipe.fingers = fingersDown;
                    m_gestures.push_back(swipe);
                    m_state.held[static_cast<usize>(SwipeFirst + direction)] = true;
                    track.direction = direction;
                }
                track.anchor = contact.position;
            }
        }

        // Held where it landed: a long press, once.
        if (!track.moved && !track.longFired && m_gestureTick - track.downTick >= LongPressTicks) {
            track.longFired = true;
            GestureEvent held;
            held.kind = GestureEvent::Kind::LongPress;
            held.position = contact.position;
            m_gestures.push_back(held);
        }

        if (!began) {
            panTotal = panTotal + core::Vec2{contact.position.x - track.last.x, contact.position.y - track.last.y};
            ++panCount;
        }
        track.last = contact.position;
    }

    // **A drag**: how far what is down moved this tick, as one motion.
    if (panCount > 0 && (panTotal.x != 0.0f || panTotal.y != 0.0f)) {
        GestureEvent pan;
        pan.kind = GestureEvent::Kind::Pan;
        pan.delta = panTotal * (1.0f / static_cast<f32>(panCount));
        pan.fingers = panCount;
        m_gestures.push_back(pan);
    }

    // **A pinch**: the first two fingers down, and the distance between them
    // against what it was when the second landed.
    const GestureTrack* first = nullptr;
    const GestureTrack* second = nullptr;
    for (usize index = 0; index < kMaxFingers; ++index) {
        const GestureTrack& track = m_tracks[index];
        if (!track.active || track.ui)
            continue;
        if (first == nullptr)
            first = &track;
        else if (second == nullptr)
            second = &track;
    }
    if (first == nullptr || second == nullptr) {
        m_pinching = false;
        return;
    }
    const core::Vec2 apart{second->last.x - first->last.x, second->last.y - first->last.y};
    const f32 distance = std::max(std::sqrt(apart.x * apart.x + apart.y * apart.y), 1.0f);
    if (!m_pinching || m_pinchFirst != first->id || m_pinchSecond != second->id) {
        m_pinching = true;
        m_pinchFirst = first->id;
        m_pinchSecond = second->id;
        m_pinchStart = distance;
        m_pinchLast = 1.0f;
        return;
    }
    const f32 scale = distance / m_pinchStart;
    if (std::abs(scale - m_pinchLast) > 1.0e-3f) {
        m_pinchLast = scale;
        GestureEvent pinch;
        pinch.kind = GestureEvent::Kind::Pinch;
        pinch.scale = scale;
        pinch.position = core::Vec2{(first->last.x + second->last.x) * 0.5f, (first->last.y + second->last.y) * 0.5f};
        m_gestures.push_back(pinch);
    }
}

void InputSystem::dispatch(scene::World& world, Rate rate)
{
    const core::NameAtom pressedAtom = world.atoms().intern("Pressed");
    const core::NameAtom releasedAtom = world.atoms().intern("Released");
    const core::NameAtom stateChangedAtom = world.atoms().intern("StateChanged");

    const bool simulation = rate == Rate::Simulation;
    const core::Vec2 pointerDelta = simulation ? m_simPointerDelta : m_renderPointerDelta;
    const core::Vec2 wheel = simulation ? m_simWheel : m_renderWheel;

    // Every context on this clock, INCLUDING the disabled ones. A disabled
    // context's actions have to be walked so that anything they were holding is
    // released: skipping them would leave a jump held true for the rest of the
    // session, which is what closing a menu mid-press would otherwise do.
    m_contexts.clear();
    world.inputContexts().forEach([&](core::InstanceId id, const scene::InputContextComponent& context) {
        if (context.rate == static_cast<i32>(rate) && !world.destroyed(id))
            m_contexts.emplace_back(context.priority, id);
    });

    // Stable, so two contexts at one priority keep the pool's order -- which is
    // a pure function of the operation sequence and therefore the same on every
    // run (R10). An unstable sort here would be a replay divergence that only
    // shows up once two contexts happen to tie.
    std::stable_sort(m_contexts.begin(), m_contexts.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    m_consumed.fill(false);

    // The UI's claim on the pointer, applied before any context resolves --
    // which is what makes it behave like the highest-priority sinking context
    // without being one. Mouse codes only: a key pressed while the pointer rests
    // over a HUD is still the game's.
    if (m_uiCapturedPointer) {
        for (i32 code = MouseButtonFirst; code < MouseButtonFirst + MouseButtonCount; ++code)
            m_consumed[static_cast<usize>(code)] = true;
        m_consumed[static_cast<usize>(MouseMovement)] = true;
        m_consumed[static_cast<usize>(MouseWheel)] = true;
    }

    // The keyboard half of the same claim: a focused `TextInput` eats the keys.
    // Without it a player typing `w` into a chat box walks forward, which is the
    // same defect the pointer flag fixes one device over.
    if (m_uiCapturedKeyboard) {
        for (i32 code = KeyboardFirst; code < KeyboardFirst + KeyboardCount; ++code)
            m_consumed[static_cast<usize>(code)] = true;
    }

    // The raw events (ADR 0041), collected HERE: after the UI's claims are known
    // and before any context resolves. They describe what the device did, so a
    // sinking context -- which is a fact about actions -- must not change them,
    // while the UI's claim -- which is a fact about who the input reached --
    // must.
    if (simulation)
        collectRawEvents(pointerDelta, wheel);

    for (const auto& [priority, contextId] : m_contexts) {
        const scene::InputContextComponent* context = world.inputContexts().find(contextId);
        if (context == nullptr)
            continue;

        for (core::InstanceId actionId = world.firstChild(contextId); actionId.valid();
             actionId = world.nextSibling(actionId)) {
            scene::InputActionComponent* action = world.inputActions().find(actionId);
            if (action == nullptr)
                continue;

            const auto type = static_cast<ActionType>(action->type);
            core::Vec3 value;
            bool pressed = false;

            // A disabled action, or one inside a disabled context, resolves to
            // NOTHING rather than being skipped. The distinction matters at the
            // moment of disabling: leaving the last value in place is how a key
            // released while a menu was open never reaches the game, and how
            // `Released` fails to fire for something a handler started.
            const bool live = context->enabled && action->enabled;
            if (!live) {
                // Nothing to compute; the zero value above is the answer.
            }
            else if (type == ActionType::ViewportPosition) {
                // The pointer's POSITION, which no binding names: an action of
                // this type answers where the cursor is, and a binding on it
                // would be a field with nothing to say.
                value = core::Vec3{m_state.pointer.x, m_state.pointer.y, 0.0f};
            }
            else if (type != ActionType::Direction3D) {
                for (core::InstanceId bindingId = world.firstChild(actionId); bindingId.valid();
                     bindingId = world.nextSibling(bindingId)) {
                    const scene::InputBindingComponent* binding = world.inputBindings().find(bindingId);
                    if (binding == nullptr)
                        continue;

                    const f32 scale = binding->scale;
                    switch (type) {
                    case ActionType::Bool:
                        pressed = pressed || digital(m_state, m_consumed, binding->keyCode);
                        break;
                    case ActionType::Direction1D: {
                        f32 amount = composite(m_state, m_consumed, binding->up, binding->down);
                        const i32 code = binding->keyCode;
                        if (valid(code) && !m_consumed[static_cast<usize>(code)]) {
                            if (code == MouseWheel)
                                amount += wheel.y;
                            else if (isAnalog(code))
                                amount += m_state.axis[static_cast<usize>(code)];
                            else if (m_state.held[static_cast<usize>(code)])
                                amount += 1.0f;
                        }
                        value.x += amount * scale;
                        break;
                    }
                    case ActionType::Direction2D: {
                        core::Vec2 amount{composite(m_state, m_consumed, binding->right, binding->left),
                                          composite(m_state, m_consumed, binding->up, binding->down)};
                        const i32 code = binding->keyCode;
                        if (valid(code) && !m_consumed[static_cast<usize>(code)]) {
                            if (code == MouseMovement)
                                amount = amount + pointerDelta;
                            else if (code == LeftThumbstick || code == RightThumbstick || code == VirtualStick1 ||
                                     code == VirtualStick2)
                                amount = amount + stick(m_state, code);
                            else if (code == MouseWheel)
                                amount = amount + wheel;
                        }
                        value.x += amount.x * scale;
                        value.y += amount.y * scale;
                        break;
                    }
                    case ActionType::Direction3D:
                    case ActionType::ViewportPosition:
                        break;
                    }
                }
            }

            // Held from outside (ADR 0107 §3): the value the world running
            // this one gave it, in place of whatever its bindings read.
            if (live && !m_held.empty()) {
                const std::string_view name = world.atoms().text(world.name(actionId));
                for (const HeldAction& held : m_held) {
                    if (held.name != name)
                        continue;
                    pressed = type == ActionType::Bool && held.pressed;
                    value = type == ActionType::Bool ? core::Vec3{} : held.value;
                    break;
                }
            }

            // Deliberately NOT clamped. A key contributes 1, a stick its
            // deflection, and a mouse-motion binding contributes PIXELS -- so a
            // clamp to the unit range would make every look control unusable,
            // and a clamp that skipped mouse bindings would make the rule
            // depend on which key a binding happened to name.
            const bool changed = value != action->axis || pressed != action->pressed;
            action->axis = value;
            action->pressed = pressed;

            if (!changed)
                continue;

            if (type == ActionType::Bool) {
                world.changes().push(scene::Change{
                    scene::ChangeKind::InstanceEventNoArgs, actionId, {}, pressed ? pressedAtom : releasedAtom});
            }
            world.changes().push(scene::Change{scene::ChangeKind::InstanceEventNoArgs, actionId, {}, stateChangedAtom});
        }

        if (!context->sink || !context->enabled)
            continue;

        // Consumed AFTER the context resolved, so an action inside a sinking
        // context still reads its own input. Per key rather than per context: a
        // dialog that sinks Escape leaves W to whatever is underneath it.
        for (core::InstanceId actionId = world.firstChild(contextId); actionId.valid();
             actionId = world.nextSibling(actionId)) {
            const scene::InputActionComponent* action = world.inputActions().find(actionId);
            // A disabled action sinks nothing. The alternative is a dead action
            // silently eating a key for the rest of the session, with no way to
            // tell from the outside which context is doing it.
            if (action == nullptr || !action->enabled)
                continue;
            for (core::InstanceId bindingId = world.firstChild(actionId); bindingId.valid();
                 bindingId = world.nextSibling(bindingId)) {
                const scene::InputBindingComponent* binding = world.inputBindings().find(bindingId);
                if (binding == nullptr)
                    continue;
                for (const i32 code : {binding->keyCode, binding->up, binding->down, binding->left, binding->right}) {
                    if (valid(code))
                        m_consumed[static_cast<usize>(code)] = true;
                }
            }
        }
    }

    world.engineState().pointerPosition = m_state.pointer;
    world.engineState().lastInputDeviceType = static_cast<i32>(m_state.lastDevice);

    if (simulation) {
        m_simPointerDelta = core::Vec2{};
        m_simWheel = core::Vec2{};
    }
    else {
        m_renderPointerDelta = core::Vec2{};
        m_renderWheel = core::Vec2{};
    }
}

void InputSystem::dispatchSimTick(scene::World& world, u64)
{
    dispatch(world, Rate::Simulation);
    // What this tick saw is seen; a tap's release is due now.
    for (usize at = 0; at < m_downUnseen.size(); ++at) {
        m_downUnseen[at] = false;
        if (m_releaseDeferred[at]) {
            m_releaseDeferred[at] = false;
            m_state.held[at] = false;
        }
    }
}

void InputSystem::dispatchRenderRate(scene::World& world)
{
    dispatch(world, Rate::Render);
}

void InputSystem::releaseAll(scene::World& world)
{
    m_state.held.fill(false);
    m_downUnseen.fill(false);
    m_releaseDeferred.fill(false);
    m_state.axis.fill(0.0f);
    m_state.fingers.fill(Finger{});
    // Nothing a lost window was in the middle of is finished as a gesture.
    m_tracks.fill(GestureTrack{});
    m_pinching = false;
    m_simPointerDelta = core::Vec2{};
    m_renderPointerDelta = core::Vec2{};
    m_simWheel = core::Vec2{};
    m_renderWheel = core::Vec2{};
    // Both rates, because a held key belongs to whichever context bound it and
    // losing focus releases it for all of them.
    dispatch(world, Rate::Simulation);
    dispatch(world, Rate::Render);
}

} // namespace engine::input
