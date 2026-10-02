// The facts `scene` produces and `script` turns into signal fires
// (api-design.md §3.1, and the seam described in the M2 brief's Decision 3).
//
// This is the whole of the scene↔script contract for events, and its shape is
// the point: every entry is POD. Signal *arguments* can be arbitrary Luau
// values and connections hold Luau functions, so if `scene` owned the signal
// queue it would hold references into the VM -- and L3 would depend on L5
// (architecture.md §2 rule 2), while the ECS would stop being snapshottable.
//
// So the queue is split by what it carries. `scene` enqueues facts that mean
// something in a headless world with no VM at all; `script` consumes them,
// resolves connections, and runs handlers. `Signal.new()` fires, which do carry
// Luau values, live entirely on the `script` side of the same drain.
#pragma once

#include <bit>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/core/types.h"
#include "engine/scene/types.h"

namespace engine::scene {

enum class ChangeKind : u8
{
    // `subject` changed the property named by `name`. Only enqueued when the
    // value actually changed (api-design.md §3.1): the signal is a past-tense
    // fact about a change, and an unconditional enqueue would make the
    // 10k-parts benchmark pathological for nothing.
    PropertyChanged,
    AttributeChanged,

    // `subject` gained or lost the child `other`.
    ChildAdded,
    ChildRemoved,

    // `subject` is the ancestor; `other` is the descendant.
    DescendantAdded,
    DescendantRemoving,

    // `subject` moved; `other` is its new parent, invalid when unparented.
    AncestryChanged,

    // `subject` is being destroyed. Its handle still resolves until the end of
    // the drain that processes this (divergence #25).
    Destroying,

    TagAdded,
    TagRemoved,

    // `subject`'s event named by `name` fired, carrying `other` as its single
    // Instance argument -- nil when `other` is invalid.
    //
    // Generic rather than one kind per signal, because the three physics
    // signals M5 adds (`Touched`, `TouchEnded`, `Landed`) all have exactly this
    // shape, and a kind per event would grow this enum with every milestone
    // that has one. The event's name is data here rather than an enumerator for
    // the same reason a property's name is.
    InstanceEvent,

    // The same, carrying NOTHING. `Destroying` is the shape, and M6's
    // `InputAction.Pressed`, `Released` and `StateChanged` are three more of it.
    //
    // A separate kind rather than `InstanceEvent` with an invalid `other`,
    // because those are different arities and not a value: `Landed` with an
    // invalid `other` fires with one nil argument, which is right for a
    // character that landed on nothing and wrong for a button that was pressed.
    InstanceEventNoArgs,

    // The same, carrying one boolean: `TextInput.FocusLost`'s `submitted`
    // (D215). The value rides in `other.index` -- 1 or 0 -- with `other`'s
    // generation zero, so it can never resolve as an instance.
    InstanceEventBool,

    // The same, carrying one string: `TextInput.Submitted`, `TextChanged` and
    // `InputRejected` (ADR 0139). The text is in the queue's own arena, its
    // index in `other.index` -- never an atom, which would keep every chat
    // message a player ever typed for the life of the process.
    InstanceEventText,

    // `TextInput.FocusLost(submitted, reason)` (ADR 0139): `submitted` in
    // `other.index`, the `Enum.FocusLossReason` value in `other.generation`.
    FocusLost,

    // `BasePart.Collided` (ADR 0127, N2): the other part, where, which way and
    // how fast. The four are in the queue's own list, its index in
    // `other.index` -- a change is sixteen bytes and this is forty.
    InstanceEventContact,

    // The same as `InstanceEventNoArgs`, carrying one point: a
    // `UIDragDetector`'s three signals (ADR 0128). The two floats ride in
    // `other`, bit for bit -- see `eventPoint`.
    InstanceEventVector2,
};

// What an `InstanceEventContact` carries.
struct ContactNote
{
    core::InstanceId other;
    core::DVec3 point;
    core::Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 speed = 0.0f;
};

// A boolean in the shape `InstanceEventBool` carries it.
[[nodiscard]] constexpr core::InstanceId eventFlag(bool value) noexcept
{
    return core::InstanceId{value ? 1u : 0u, 0u};
}

// A point in the shape `InstanceEventVector2` carries it, and back.
[[nodiscard]] inline core::InstanceId eventPoint(core::Vec2 point) noexcept
{
    return core::InstanceId{std::bit_cast<u32>(point.x), std::bit_cast<u32>(point.y)};
}
[[nodiscard]] inline core::Vec2 eventPoint(core::InstanceId carried) noexcept
{
    return core::Vec2{std::bit_cast<f32>(carried.index), std::bit_cast<f32>(carried.generation)};
}

// 16 bytes, trivially copyable, and deliberately not a variant: one shape means
// the queue is a flat vector rather than a discriminated allocation per entry,
// and a property storm is the load case this whole design is measured against.
struct Change
{
    ChangeKind kind = ChangeKind::PropertyChanged;
    core::InstanceId subject;
    core::InstanceId other;
    core::NameAtom name;
};

// Append-only within a frame; `drain` hands the whole run to the consumer and
// starts a fresh one. Order is raise order and nothing reorders it -- that
// single total order is what makes a script's own `Fire` and the `ChildAdded`
// caused by its `part.Parent = x` land in the order the script wrote them
// (api-design.md §3.1).
class ChangeQueue
{
public:
    void push(const Change& change) { m_entries.push_back(change); }

    // Pushes `InstanceEventText` for `subject`'s event `name`, carrying `text`.
    void pushText(core::InstanceId subject, core::NameAtom name, std::string text)
    {
        m_texts.push_back(std::move(text));
        m_entries.push_back(Change{ChangeKind::InstanceEventText, subject,
                                   core::InstanceId{static_cast<u32>(m_texts.size() - 1), 0}, name});
    }

    // Pushes `InstanceEventContact` for `subject`'s event `name`.
    void pushContact(core::InstanceId subject, core::NameAtom name, const ContactNote& note)
    {
        m_contacts.push_back(note);
        m_entries.push_back(Change{ChangeKind::InstanceEventContact, subject,
                                   core::InstanceId{static_cast<u32>(m_contacts.size() - 1), 0}, name});
    }

    // The contact an `InstanceEventContact` of the last `take` carries.
    [[nodiscard]] ContactNote drainedContact(const Change& change) const noexcept
    {
        return change.other.index < m_drainedContacts.size() ? m_drainedContacts[change.other.index] : ContactNote{};
    }

    // The text an `InstanceEventText` of the last `take` carries.
    [[nodiscard]] std::string_view drainedText(const Change& change) const noexcept
    {
        return change.other.index < m_drainedTexts.size() ? std::string_view(m_drainedTexts[change.other.index])
                                                          : std::string_view{};
    }

    // The consumer must finish with the span before the next `push`, which is
    // the natural shape of a drain: `script` copies what it needs into its own
    // structures as it resolves connections.
    [[nodiscard]] std::span<const Change> take() noexcept
    {
        m_drained.swap(m_entries);
        m_entries.clear();
        m_drainedTexts.swap(m_texts);
        m_texts.clear();
        m_drainedContacts.swap(m_contacts);
        m_contacts.clear();
        return m_drained;
    }

    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] usize size() const noexcept { return m_entries.size(); }

    void clear() noexcept
    {
        m_entries.clear();
        m_drained.clear();
        m_texts.clear();
        m_drainedTexts.clear();
        m_contacts.clear();
        m_drainedContacts.clear();
    }

private:
    std::vector<Change> m_entries;
    // Swapped rather than copied, so a drain costs no allocation and the
    // storage is reused frame after frame.
    std::vector<Change> m_drained;
    // `InstanceEventText`'s strings, swapped with the entries.
    std::vector<std::string> m_texts;
    std::vector<std::string> m_drainedTexts;
    // `InstanceEventContact`'s notes, the same way.
    std::vector<ContactNote> m_contacts;
    std::vector<ContactNote> m_drainedContacts;
};

} // namespace engine::scene
