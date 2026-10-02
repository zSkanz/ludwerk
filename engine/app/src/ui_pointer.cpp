#include "engine/app/ui_pointer.h"

#include "engine/ui/ui.h"

namespace engine::app {

void UiPointer::beginFrame() noexcept
{
    m_pressEvent = false;
    m_releaseEvent = false;
    m_releaseFirst = false;
}

void UiPointer::feed(const platform::Event& event) noexcept
{
    const core::Vec2 at{event.pointerX, event.pointerY};
    switch (event.type) {
    case platform::EventType::MouseMoved:
        m_position = at;
        m_known = true;
        break;
    case platform::EventType::MouseButtonDown:
        if (event.button != platform::MouseButton::Left)
            break;
        // **Where the press is**, from the event that is the press: a tap is
        // a button going down at a place no motion led to.
        m_position = at;
        m_known = true;
        m_down = true;
        m_pressEvent = true;
        m_clicks = event.clicks > 0 ? event.clicks : 1;
        m_shiftPress = (event.modifiers & platform::KeyModifier::Shift) != 0;
        break;
    case platform::EventType::MouseButtonUp:
        if (event.button != platform::MouseButton::Left)
            break;
        m_position = at;
        m_known = true;
        m_down = false;
        m_releaseFirst = m_releaseFirst || !m_pressEvent;
        m_releaseEvent = true;
        break;
    case platform::EventType::FingerDown:
        // The first finger is the pointer; a second is the game's own.
        if (!m_fingerHeld) {
            m_fingerHeld = true;
            m_finger = event.fingerId;
        }
        [[fallthrough]];
    case platform::EventType::FingerMoved:
        if (m_fingerHeld && m_finger == event.fingerId) {
            m_position = at;
            m_known = true;
        }
        break;
    case platform::EventType::FingerUp:
        if (m_fingerHeld && m_finger == event.fingerId) {
            m_position = at;
            m_known = true;
            m_fingerHeld = false;
        }
        break;
    default:
        break;
    }
}

void UiPointer::letGo() noexcept
{
    m_down = false;
    m_fingerHeld = false;
}

void UiPointer::fill(ui::InteractionInput& out, core::Vec2 fallback) noexcept
{
    out.pointer = positionOr(fallback);
    out.pressed = m_pressEvent || (m_down && !m_lastDown);
    out.released = m_releaseEvent || (!m_down && m_lastDown);
    out.releasedFirst = m_releaseFirst;
    out.clicks = m_clicks;
    out.shiftPress = m_shiftPress;
    out.pointerHeld = m_down;
    m_lastDown = m_down;
}

} // namespace engine::app
