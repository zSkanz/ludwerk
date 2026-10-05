#include "engine/app/ui_pointer.h"

#include "engine/ui/ui.h"

namespace engine::app {

void UiPointer::beginFrame() noexcept
{
    m_pressEvent = false;
    m_releaseEvent = false;
    m_releaseFirst = false;
    // The fingers that lifted last frame have been heard; the rest are held.
    std::erase_if(m_touches, [](const ui::InteractionTouch& touch) { return touch.released; });
    for (ui::InteractionTouch& touch : m_touches)
        touch.pressed = false;
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
        // The first finger is the pointer -- the system's mouse is made of it
        // and of no other -- and each one after it is a press of its own
        // (D557).
        if (!m_fingerHeld) {
            m_fingerHeld = true;
            m_finger = event.fingerId;
        }
        else if (m_finger != event.fingerId) {
            // A finger heard twice without lifting is the same finger.
            std::erase_if(m_touches,
                          [&event](const ui::InteractionTouch& touch) { return touch.finger == event.fingerId; });
            // More fingers than a pair of hands is a palm on the glass.
            constexpr core::usize MostTouches = 10;
            if (m_touches.size() < MostTouches)
                m_touches.push_back({.finger = event.fingerId, .position = at, .pressed = true, .released = false});
            break;
        }
        [[fallthrough]];
    case platform::EventType::FingerMoved:
        if (m_fingerHeld && m_finger == event.fingerId) {
            m_position = at;
            m_known = true;
        }
        else if (ui::InteractionTouch* touch = other(event.fingerId); touch != nullptr && !touch->released) {
            touch->position = at;
        }
        break;
    case platform::EventType::FingerUp:
        if (m_fingerHeld && m_finger == event.fingerId) {
            m_position = at;
            m_known = true;
            m_fingerHeld = false;
        }
        else if (ui::InteractionTouch* touch = other(event.fingerId); touch != nullptr) {
            touch->position = at;
            touch->released = true;
        }
        break;
    default:
        break;
    }
}

ui::InteractionTouch* UiPointer::other(core::u64 finger) noexcept
{
    // The last of that id: one that lifted and came down again in a frame is
    // two entries, and what moves is the one still on the glass.
    for (auto touch = m_touches.rbegin(); touch != m_touches.rend(); ++touch) {
        if (touch->finger == finger)
            return &*touch;
    }
    return nullptr;
}

void UiPointer::letGo() noexcept
{
    m_down = false;
    m_fingerHeld = false;
    m_touches.clear();
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
    out.touches = m_touches;
    m_lastDown = m_down;
}

} // namespace engine::app
