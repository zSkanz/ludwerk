// The pointer the game's interface is pressed with (D430).
//
// **A press carries its own place.** The interface took a press from the
// frame's events and its position from the input system's pointer -- which
// follows the MOUSE, and deliberately not the mouse events the system makes
// out of a finger (`platform::Event::fromTouch`), since the same finger has
// already arrived as a finger. So on a phone the pointer never moved from
// where it began, and every tap was a press on nothing at the top-left corner:
// no button's `Activated`, no field's focus, no keyboard -- and, being "a
// press that lands anywhere else", it took the focus from whatever had it.
//
// This is where the interface's pointer IS: wherever the last event that
// moved or pressed it said -- a mouse's, one made from a finger, or the first
// finger's own. The input system's pointer is the answer only before any
// event has said.
//
// **And every other finger is heard too** (D557). The pointer follows one
// finger, the first down, because the system makes its mouse of that one and
// of no other. A second finger was "the game's own" and the interface did not
// hear it at all -- so with a thumb resting on a game's stick, no button
// anywhere on the screen could be pressed. Each finger that is not the
// pointer's is kept here and handed to the interface as a press of its own
// (`ui::InteractionTouch`).
#pragma once

#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/platform/event.h"
#include "engine/ui/ui.h"

namespace engine::app {

class UiPointer
{
public:
    // The frame's edges are over; what is held stays held.
    void beginFrame() noexcept;
    // One event of the frame, in order. Anything that is not the pointer's is
    // ignored.
    void feed(const platform::Event& event) noexcept;
    // Nothing is held: the interface stopped hearing the pointer (the editor,
    // out of Play).
    void letGo() noexcept;
    // The pointer's part of what the interface is told this frame. `fallback`
    // is the input system's pointer, for a frame before any event said where.
    void fill(ui::InteractionInput& out, core::Vec2 fallback) noexcept;

    [[nodiscard]] core::Vec2 positionOr(core::Vec2 fallback) const noexcept { return m_known ? m_position : fallback; }
    [[nodiscard]] bool down() const noexcept { return m_down; }

private:
    // The finger `finger` among the others, or null.
    [[nodiscard]] ui::InteractionTouch* other(core::u64 finger) noexcept;

    core::Vec2 m_position;
    bool m_known = false;
    bool m_down = false;
    bool m_lastDown = false;
    // **The presses and releases this frame, as events** (D362): a tap whose
    // down and up land between two frames leaves the button's state where it
    // was, and a click read from the state alone was lost.
    bool m_pressEvent = false;
    bool m_releaseEvent = false;
    bool m_releaseFirst = false;
    core::u8 m_clicks = 1;
    bool m_shiftPress = false;
    // The finger the pointer follows: the first one down, until it lifts.
    bool m_fingerHeld = false;
    core::u64 m_finger = 0;
    // Every other finger down, in the order they came, and the ones that
    // lifted this frame -- forgotten at the next.
    std::vector<ui::InteractionTouch> m_touches;
};

} // namespace engine::app
