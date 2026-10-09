// What a gamepad, the arrow keys and the wheel say to the game's interface
// (ADR 0128).
//
// The selection is moved by the d-pad, the left stick and the arrows, and
// activated by the south button and Enter; a gamepad's shoulder buttons turn a
// page; the wheel scrolls what is under the pointer. This reads those out of
// the frame's events, as `UiPointer` reads the pointer out of them.
//
// **A game's own bindings come first.** The ADR's "engine-owned `InputContext`
// a game's own contexts can override", without the Instance (for the reason
// `ui::InteractionResult` gives for the pointer's): a source the game has bound
// in an enabled context is the game's, and the interface is not told about it.
// So a character walked with the arrows keeps walking with a button on the
// screen, and a menu -- whose game has turned its play context off, or has
// none -- is driven by the same keys.
#pragma once

#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/platform/event.h"

namespace engine::scene {
class World;
}

namespace engine::ui {
struct InteractionInput;
}

namespace engine::app {

class UiNavigation
{
public:
    // The frame's edges are over; what is held stays held.
    void beginFrame() noexcept;
    // Zero keeps aggregate navigation; a positive id reserves it for one pad.
    void setGamepadId(core::u32 id) noexcept;
    // One event of the frame, in order. Anything that is none of the above is
    // ignored.
    void feed(const platform::Event& event) noexcept;
    // Nothing is held: the interface stopped hearing the devices.
    void letGo() noexcept;
    // The navigation's part of what the interface is told this frame. `now`
    // is in seconds, for a held direction's repeat.
    void fill(const scene::World& world, ui::InteractionInput& out, core::f64 now) noexcept;

    // Whether an enabled `InputContext` of the game binds `keyCode`.
    [[nodiscard]] static bool boundByGame(const scene::World& world, core::i32 keyCode, core::u32 gamepadId = 0);

private:
    // One way a direction is given: its presses this frame, and what is held.
    struct Source
    {
        core::i8 pressX = 0;
        core::i8 pressY = 0;
        core::i8 heldX = 0;
        core::i8 heldY = 0;
    };
    // The arrows repeat by themselves -- the system's key repeat is a press --
    // and the other two are repeated here.
    core::u32 m_gamepadId = 0;
    Source m_arrows;
    Source m_dpad;
    Source m_stick;
    core::f32 m_stickX = 0.0f;
    core::f32 m_stickY = 0.0f;
    // When a held d-pad or stick next steps again, and which way it was held.
    core::f64 m_repeatAt = 0.0;
    core::i8 m_repeatX = 0;
    core::i8 m_repeatY = 0;

    bool m_enter = false;
    bool m_south = false;
    core::i8 m_shoulder = 0;
    core::Vec2 m_wheel;
};

} // namespace engine::app
