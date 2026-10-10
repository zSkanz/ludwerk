// The Input Action System (ADR 0029, api-design.md §2.4).
//
// Three jobs, and the split between them is the design:
//
//   1. **The device snapshot.** `pumpFrame` folds a frame's platform events
//      into "what is held right now" -- a key set, a button set, axis values, a
//      pointer position, and the motion and wheel deltas ACCUMULATED since the
//      last dispatch. A snapshot rather than an event stream, because two reads
//      inside one tick must agree and because a recorded stream has to be able
//      to hand the same answers back with no hardware attached (M5 settled this
//      for the keyboard; it is the same contract).
//
//   2. **Resolution.** `dispatch*` walks the `InputContext` tree in priority
//      order, computes each enabled action's value from its bindings, and
//      writes it into the action's component. Sinking contexts consume the
//      inputs they name, so a lower-priority context bound to the same key sees
//      nothing.
//
//   3. **Telling `script`.** A value that changed enqueues a `Change` on the
//      world's queue, exactly as a property write does; `script` turns it into
//      a deferred fire. Nothing here knows what a Luau value is (R17,
//      architecture §2 rule 2).
//
// The dispatch split is ADR 0039's: an `InputContext` declares its own rate,
// `Simulation` by default. A `Render`-rate context is dispatched once per
// rendered frame and is NOT part of the recorded input stream -- a render frame
// is not a unit the replay has.
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/platform/event.h"
#include "engine/scene/types.h"

namespace engine::scene {
class World;
}

namespace engine::input {

using core::f32;
using core::f64;
using core::i32;
using core::u32;
using core::u64;
using core::usize;

// `Enum.InputActionType`'s values, spelled once so that neither the resolver nor
// the accessors compare against a literal.
enum class ActionType : i32
{
    Bool = 0,
    Direction1D = 1,
    Direction2D = 2,
    Direction3D = 3,
    ViewportPosition = 4,
};

// `Enum.InputDeviceType`'s values.
enum class DeviceType : i32
{
    KeyboardMouse = 0,
    Gamepad = 1,
    Touch = 2,
};

// `Enum.InputRate`'s values.
enum class Rate : i32
{
    Simulation = 0,
    Render = 1,
};

// `Enum.UserInputType`'s values -- what KIND of input an `InputObject` carries
// (ADR 0041).
enum class UserInputType : i32
{
    None = 0,
    Keyboard = 1,
    MouseButton1 = 2,
    MouseButton2 = 3,
    MouseButton3 = 4,
    MouseMovement = 5,
    MouseWheel = 6,
    Gamepad = 7,
    Touch = 8,
};

// One raw input, as `InputService.InputBegan` / `InputChanged` / `InputEnded`
// carry it.
//
// **Produced by the IAS's own dispatch and never read from the OS** (ADR 0041).
// That is the whole reason this surface is allowed to exist: the events come
// from the same snapshot the actions do, on the same tick, after the UI has
// consumed what it consumed -- so they sink correctly, they replay from the
// recorded stream, and a handler that writes to the world is deterministic.
//
// POD and trivially copyable, because it crosses the same kind of seam a
// `scene::Change` does and for the same reason: `script` turns one into a
// userdata, and nothing here may know what a Luau value is (R17).
struct RawInputEvent
{
    enum class Phase : core::u8
    {
        Began,
        Changed,
        Ended,
    };

    Phase phase = Phase::Began;
    UserInputType userInputType = UserInputType::None;
    // `Enum.KeyCode`'s value, or 0 (`Unknown`) for motion and the wheel.
    i32 keyCode = 0;
    // Pointer position in window pixels with `z` carrying the accumulated
    // wheel, or a gamepad axis's deflection. `vector` rather than a `Vector2`
    // because `vector` is the native primitive (ADR 0013) and this is produced
    // several times a tick.
    core::Vec3 position;
    core::Vec3 delta;
    // For `Touch`: which finger, from 1, stable from its `Began` to its
    // `Ended` -- the slot it holds in `DeviceState::fingers`, plus one. Zero
    // for everything else.
    i32 touchId = 0;
    // Physical controller that produced this event; zero for other devices
    // and legacy aggregate recordings without controller identities.
    u32 gamepadId = 0;
    // Whether the interface already took this input -- the second argument of
    // every one of the three events, and the one a handler that ignores it
    // regrets: it is what stops a click on a button also firing the gun and a
    // `w` typed into a text box also jumping.
    bool uiConsumed = false;
};

// The number of `Enum.KeyCode` items. It is not derived from `platform`'s enums
// because it is not their union: the KeyCode table adds two composite sticks
// that no device event names. `input.cpp` static_asserts the arithmetic against
// the generated enum descriptor at registration, which is the check that keeps
// this number honest.
inline constexpr usize kKeyCodeCount = 152;

// Whether a `KeyCode` is one of the sixteen virtual keys or the two composites over
// them -- the ones `InputService:SetVirtualState` may write and nothing else
// may. A HUD button writing to `Space` would be a script pretending to be a
// keyboard, and there would then be no way for anything to tell the difference.
[[nodiscard]] bool isVirtual(i32 keyCode) noexcept;

// Which device family a `KeyCode` belongs to. `InputBinding.DeviceType` is this
// function and nothing else (ADR 0039): a settable device type could disagree
// with the key beside it, and the engine would then have to choose which of the
// two to believe.
[[nodiscard]] DeviceType deviceOf(i32 keyCode) noexcept;

// Whether a `KeyCode` reports a continuous value rather than a press. Bound to a
// `Bool` action, an analogue source counts as pressed past half deflection --
// which is a choice `Enum.KeyCode`'s own doc states, because refusing at bind
// time would let a rebinding UI hand the player an unusable option.
[[nodiscard]] bool isAnalog(i32 keyCode) noexcept;

// A `KeyCode` by the name `Enum.KeyCode` gives its item, and back. This is the
// spelling a recorded input stream is written in -- `120 + Space` -- so that a
// recording stays readable, stays reviewable as a diff, and stays valid across
// a renumbering of the enum.
//
// The keyboard, mouse-button, gamepad-button and gamepad-axis names come from
// `platform`'s own tables, which is why those tables and the enum are one
// spelling space. The four this module adds are the ones no device event names:
// `MouseMovement`, `MouseWheel`, `LeftThumbstick` and `RightThumbstick`.
//
// 0 for a name no item carries, and an empty view for `Unknown`.
[[nodiscard]] i32 keyCodeFromName(std::string_view name) noexcept;

// **A controller's own slot for an `Enum.KeyCode`**: the index into
// `GamepadSnapshot::buttons`, or into `::axes`, of the button or the axis the
// code names -- and -1 for a code that is not one. What lets a recording say
// "controller two's `ButtonSouth`" and have it land where a device would have
// put it (ADR 0195).
[[nodiscard]] i32 gamepadButtonSlotOf(i32 keyCode) noexcept;
[[nodiscard]] i32 gamepadAxisSlotOf(i32 keyCode) noexcept;
[[nodiscard]] std::string_view keyCodeName(i32 keyCode) noexcept;

// What the resolver reads. One frame's worth of device state, owned by the
// system below and rebuilt by `pumpFrame`.
//
// The two delta fields are accumulated rather than sampled: several motion
// events arrive per frame and a tick that read only the last one would lose most
// of a fast flick. They are cleared when a dispatch consumes them, which is why
// `Simulation` and `Render` contexts get their own copies -- see `Snapshot`.
// The most fingers a touchscreen is followed with. Ten is two hands; a finger
// past it is ignored rather than stealing a slot one already down is in.
inline constexpr usize kMaxFingers = 10;

// One finger's slot in the snapshot. `id` is the platform's and only matters
// while it is down.
struct Finger
{
    bool down = false;
    // Lifted before any tick saw it down -- a tap shorter than a frame. Kept
    // down for one tick so that a handler still sees it begin and end.
    bool lifting = false;
    u64 id = 0;
    core::Vec2 position;
    // Where it came down. In the snapshot with the rest, so a flick that
    // begins and ends between two ticks still has both of its ends.
    core::Vec2 origin;
};

// **What a finger did, said in the words a game uses** (D462): a swipe, a
// tap, a press held, two fingers closing or parting, a drag. Recognised from
// the snapshot at each `Simulation` tick -- never from a clock -- so the same
// fingers make the same gestures in a replay, and drained beside the raw
// events of the same tick.
struct GestureEvent
{
    enum class Kind : core::u8
    {
        Swipe,
        Tap,
        LongPress,
        Pinch,
        Pan,
    };
    Kind kind = Kind::Tap;
    // `Swipe`: `Enum.SwipeDirection`'s value -- 0 Up, 1 Down, 2 Left, 3 Right.
    i32 direction = 0;
    // Window pixels. `Swipe`: where the stroke began. `Tap`, `LongPress`:
    // where the finger is. `Pinch`: midway between the two.
    core::Vec2 position;
    // `Pan`: how far the fingers moved this tick, in window pixels.
    core::Vec2 delta;
    // `Pinch`: the distance between the two fingers over what it was when
    // the second landed.
    f32 scale = 1.0f;
    // `Swipe`, `Pan`: how many fingers are down.
    i32 fingers = 1;
};

struct DeviceEvent
{
    enum class Kind : core::u8
    {
        InputChanged,
        GamepadTypeChanged,
        GamepadIdChanged,
        Connected,
        Disconnected
    };
    Kind kind = Kind::InputChanged;
    DeviceType device = DeviceType::KeyboardMouse;
    platform::GamepadType family = platform::GamepadType::Unknown;
    u32 id = 0;
};

struct GamepadSnapshot
{
    u32 id = 0;
    platform::GamepadType family = platform::GamepadType::Unknown;
    std::array<bool, static_cast<usize>(platform::GamepadButton::Count)> buttons{};
    std::array<f32, static_cast<usize>(platform::GamepadAxis::Count)> axes{};
};

struct DeviceState
{
    std::array<bool, kKeyCodeCount> held{};
    // -1..1 for a stick, 0..1 for a trigger, indexed by `Enum.KeyCode` value.
    std::array<f32, kKeyCodeCount> axis{};
    core::Vec2 pointer;
    core::Vec2 pointerDelta;
    core::Vec2 wheel;
    // Every finger on a touchscreen, by slot (ADR 0041's snapshot, so a replay
    // hands them over exactly as a device does).
    std::array<Finger, kMaxFingers> fingers{};
    bool focused = true;
    DeviceType lastDevice = DeviceType::KeyboardMouse;
    platform::GamepadType preferredGamepadType = platform::GamepadType::Unknown;
    u32 preferredGamepadId = 0;
    // Sorted by id: each controller's own state, beside the aggregate keys
    // every context read before there was more than one, so a local seat
    // never asks the system about a controller inside a tick.
    //
    // **A replay carries these** (ADR 0195): a recording's line may name the
    // controller it is of (`900 + ButtonSouth @2`), and a context that reads
    // one controller reads in a replay what it read from the device -- which
    // is what makes two players at two controllers a determinism scenario
    // (`tests/determinism/coop`).
    std::vector<GamepadSnapshot> gamepads;
};

// The system's own state. Held by `app`, handed to `script` the way the physics
// mirror is: `scene` cannot own it (it would make L3 depend on `platform`), and
// a process-global would make two worlds in one process share a keyboard.
class InputSystem
{
public:
    // Folds this frame's events into the snapshot. Idempotent per frame and
    // safe to call with an empty span, which is what a headless run does.
    void pumpFrame(std::span<const platform::Event> events);

    // Replaces the device snapshot wholesale. This is the seam a recorded input
    // stream drives: the replay hands the state a tick should see instead of
    // reading a device, so what is replayed is a keystroke's whole path to the
    // game rather than a bot calling the API underneath it.
    void setSnapshot(const DeviceState& state) noexcept;

    [[nodiscard]] std::span<const DeviceEvent> drainDeviceEvents() noexcept;

    [[nodiscard]] const DeviceState& snapshot() const noexcept { return m_state; }
    // Whether the last dispatch gave this `Enum.KeyCode` to something above the
    // engine's own uses of it -- the interface, or a context that sinks it. An
    // engine use of a key below every context a game makes asks this before
    // it acts.
    [[nodiscard]] bool consumed(i32 keyCode) const noexcept
    {
        return keyCode >= 0 && static_cast<usize>(keyCode) < kKeyCodeCount && m_consumed[static_cast<usize>(keyCode)];
    }

    // Whether one `Enum.KeyCode` is held, by the same rule a `Bool` action
    // applies -- an analogue source counts as down past half deflection. What
    // `InputService:IsKeyDown` answers, and it deliberately ignores what the UI
    // consumed: a poll asks what the HARDWARE is doing, and an event asks what
    // happened to the game.
    [[nodiscard]] bool isKeyDown(i32 keyCode) const noexcept;
    [[nodiscard]] bool isGamepadKeyDown(u32 id, i32 keyCode) const noexcept;

    // Writes one of the virtual axes. Ignores a code that is not virtual, which
    // is what keeps the seam one-way: a script drives the four channels the
    // engine set aside for it and nothing else.
    //
    // The value STICKS until it is written again, because a HUD button that is
    // held is a value nobody has cleared -- and `releaseAll` clears it with
    // everything else, so a press cannot survive losing focus.
    void setVirtualState(i32 keyCode, f32 value) noexcept;

    // **Holds the action named `action` at a value, whatever its bindings say**
    // (ADR 0107 §3): how the world running a `SubWorld` drives the sub-world's
    // input, by name -- the two worlds share no instance to name it by. It
    // sticks until it is set again.
    void setActionState(std::string_view action, core::Vec3 value, bool pressed);

    // Resolves every `Simulation`-rate context and writes the result into the
    // world. Enqueues a `Change` per action whose value moved.
    void dispatchSimTick(scene::World& world, u64 tick);

    // The same for `Render`-rate contexts. Called once per rendered frame and
    // never in a headless run, because there is no render frame to dispatch on.
    void dispatchRenderRate(scene::World& world);

    // Whether the UI took the pointer this frame.
    //
    // This is architecture.md §2's "engine-owned high-priority InputContext with
    // Sink", without the Instance: a context in the tree would be an object a
    // game can see, reparent and destroy, and destroying it would silently turn
    // off every button. A flag has the same effect and nothing to break.
    //
    // It consumes the MOUSE codes only. A key pressed while the pointer happens
    // to rest over a HUD is still a key the game gets, which is what stops a
    // health bar from eating the jump button.
    void setPointerCapturedByUi(bool captured) noexcept { m_uiCapturedPointer = captured; }

    // Whether a `TextInput` has focus. The keyboard half of the same claim, and
    // it consumes the KEYBOARD codes for that frame -- so a player typing `w`
    // into a chat box does not also walk forward, which is the same defect the
    // pointer flag fixes one device over.
    void setKeyboardCapturedByUi(bool captured) noexcept { m_uiCapturedKeyboard = captured; }

    // **A finger that came down on the interface** (D444): its `Began`, every
    // `Changed` and its `Ended` say `uiConsumed`, as a click on a button does.
    // Said by the host for the finger's WHOLE press, at the event that put it
    // down -- a finger has no hover before it, so the frame-old "the pointer
    // is over the interface" a mouse is judged by knows nothing of where a
    // tap is about to land. Forgotten when the finger ends.
    void setFingerTakenByUi(u64 fingerId);

    // How far a finger travels along one axis before it is a swipe, in window
    // pixels. The host's to say, from a physical length and the display's
    // density (`InputService.SwipeThreshold`): six millimetres is 38 pixels
    // on a desktop monitor and four times that on a phone.
    void setSwipeThreshold(f32 pixels) noexcept { m_swipeThreshold = pixels > 1.0f ? pixels : 1.0f; }

    // The gestures this tick's `Simulation` dispatch recognised, in the order
    // they happened. Valid until the next dispatch.
    [[nodiscard]] std::span<const GestureEvent> drainGestures() const noexcept { return m_gestures; }

    // The raw events this tick's `Simulation` dispatch produced, in a stable
    // order: keys first by `KeyCode`, then the pointer, then the wheel, then the
    // gamepad axes. Drained rather than pushed as a `scene::Change`, because a
    // `Change` is sixteen POD bytes about an Instance and an `InputObject` is
    // neither -- the same reasoning `AnimationHost::drainEnded` carries.
    //
    // Empty for a `Render` dispatch: ADR 0041 puts these on the `Simulation`
    // clock so that a handler which writes to the world replays by
    // construction.
    [[nodiscard]] std::span<const RawInputEvent> drainRawEvents() noexcept;

    // Clears every held input and dispatches, so that anything down is released.
    // Called when the window loses focus: an alt-tab that left a key held is how
    // a character keeps walking into a wall while its window is in the
    // background.
    void releaseAll(scene::World& world);

private:
    void dispatch(scene::World& world, Rate rate);

    // Fills `m_rawEvents` from the difference between `m_previous` and the
    // current snapshot. Called at the top of a `Simulation` dispatch, after the
    // UI's claims are known and before any context resolves -- these events
    // describe what the DEVICE did, and a sinking context is a fact about
    // actions rather than about hardware.
    void collectRawEvents(core::Vec2 pointerDelta, core::Vec2 wheel);

    struct GamepadState : GamepadSnapshot
    {
        std::array<bool, static_cast<usize>(platform::GamepadButton::Count)> unseen{};
        std::array<bool, static_cast<usize>(platform::GamepadButton::Count)> releasing{};
    };
    std::vector<GamepadState> m_gamepads;
    std::vector<DeviceEvent> m_deviceEvents;
    std::vector<DeviceEvent> m_devicesDrained;
    DeviceType m_reportedDevice = DeviceType::KeyboardMouse;
    platform::GamepadType m_reportedGamepadType = platform::GamepadType::Unknown;
    u32 m_reportedGamepadId = 0;
    GamepadState& gamepad(const platform::Event& event);
    void preferGamepad(const GamepadState& pad) noexcept;
    void rebuildGamepadState();
    DeviceState m_state;
    // **A press a tick has not seen yet, and a release that waits for it**
    // (NA25). Buttons were recorded as held or not when the frame's events
    // were folded in, so a key pressed and let go between two of them was
    // never down at any tick: at thirty frames a second, or ten in the
    // background, a quick tap vanished. A button that goes down stays down
    // until a simulation tick has read it, and its release is applied after.
    std::array<bool, kKeyCodeCount> m_downUnseen{};
    std::array<bool, kKeyCodeCount> m_releaseDeferred{};
    void setHeld(i32 code, bool down) noexcept;
    // Accumulated since the last `Simulation` dispatch and since the last
    // `Render` one, separately: a frame may carry several ticks or none, and a
    // delta consumed by one rate must still be there for the other.
    core::Vec2 m_simPointerDelta;
    core::Vec2 m_simWheel;
    core::Vec2 m_renderPointerDelta;
    core::Vec2 m_renderWheel;
    bool m_uiCapturedPointer = false;
    // The fingers now down that the interface took, by their device id.
    std::vector<u64> m_uiFingers;

    // Fills `m_gestures` and the four swipe codes from the snapshot. Called
    // from `collectRawEvents`, before the snapshot becomes the previous one.
    void collectGestures();

    // One finger's stroke -- or the mouse's, with the left button down, which
    // is the last of them: a swipe is the same motion on a desk.
    struct GestureTrack
    {
        bool active = false;
        // The interface took the press: it is nobody's gesture.
        bool ui = false;
        u64 id = 0;
        // Where the stroke began, and where the swipe now being measured did:
        // the second moves to the finger each time a swipe is recognised.
        core::Vec2 start;
        core::Vec2 anchor;
        core::Vec2 last;
        // The direction of the last swipe of this stroke, or -1.
        i32 direction = -1;
        u64 downTick = 0;
        bool moved = false;
        bool longFired = false;
    };
    std::array<GestureTrack, kMaxFingers + 1> m_tracks{};
    std::vector<GestureEvent> m_gestures;
    f32 m_swipeThreshold = 38.0f;
    u64 m_gestureTick = 0;
    // The two fingers a pinch is between, and how far apart they began.
    u64 m_pinchFirst = 0;
    u64 m_pinchSecond = 0;
    f32 m_pinchStart = 0.0f;
    f32 m_pinchLast = 1.0f;
    bool m_pinching = false;
    bool m_uiCapturedKeyboard = false;

    // What the last `Simulation` dispatch saw, so the next one can tell a press
    // from a hold. Held here rather than derived from the event stream, because
    // the snapshot IS the contract: a replay hands over a state and this has to
    // produce the same events from it as a live device would.
    DeviceState m_previous;
    bool m_hasPrevious = false;
    // Whether each held input was UI-consumed when it began, so that
    // `InputEnded` reports what `InputBegan` reported -- a press that started on
    // a button is still consumed when it is released off one.
    std::array<bool, kKeyCodeCount> m_beganConsumed{};
    std::vector<RawInputEvent> m_rawEvents;
    std::vector<RawInputEvent> m_rawDrained;

    // Reused across dispatches so a steady-state frame allocates nothing.
    // A context and its priority, sorted highest first by a STABLE sort, so
    // that two contexts at one priority resolve in the order the pool reports
    // them -- which is a pure function of the operation sequence and therefore
    // the same on every run (R10).
    std::vector<std::pair<f32, core::InstanceId>> m_contexts;
    std::array<bool, kKeyCodeCount> m_consumed{};

    // What `setActionState` holds, in the order the names were first set.
    struct HeldAction
    {
        std::string name;
        core::Vec3 value;
        bool pressed = false;
    };
    std::vector<HeldAction> m_held;
};

} // namespace engine::input
