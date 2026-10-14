#include "engine/app/timeline.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

#include "engine/app/sequences.h"
#include "engine/scene/world.h"

namespace engine::app {
namespace {

using asset::SequenceKey;
using asset::SequenceTrack;
using asset::SequenceTrackType;
using asset::SequenceValue;
using core::f32;
using core::f64;
using core::u32;

constexpr f64 Degrees = 180.0 / 3.14159265358979323846;
// Two keys nearer than this are one moment: a key set there replaces.
constexpr f32 SameMoment = 1.0e-4f;

void numbers(SequenceValue& out, std::initializer_list<f64> values)
{
    out.kind = SequenceValue::Kind::Numbers;
    out.count = static_cast<core::u8>(values.size());
    std::copy(values.begin(), values.end(), std::begin(out.numbers));
}

} // namespace

bool sequenceValueOf(const scene::World& world, const scene::Value& value, const SequenceValue* like,
                     SequenceValue& out)
{
    out = SequenceValue{};
    switch (scene::valueType(value)) {
    case scene::ValueType::Bool:
        out.kind = SequenceValue::Kind::Flag;
        out.flag = std::get<bool>(value);
        return true;
    case scene::ValueType::Number:
        out.kind = SequenceValue::Kind::Number;
        out.count = 1;
        out.numbers[0] = std::get<f64>(value);
        return true;
    case scene::ValueType::String:
        out.kind = SequenceValue::Kind::Text;
        out.text = std::get<std::string>(value);
        return true;
    case scene::ValueType::Vector3: {
        const core::Vec3 v = std::get<core::Vec3>(value);
        numbers(out, {static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)});
        return true;
    }
    case scene::ValueType::Color3: {
        const core::Color3 c = std::get<core::Color3>(value);
        numbers(out, {static_cast<f64>(c.r), static_cast<f64>(c.g), static_cast<f64>(c.b)});
        return true;
    }
    case scene::ValueType::CFrame: {
        const core::CFrameD& frame = std::get<core::CFrameD>(value);
        // A track keyed by place alone stays one: its turn is not the
        // sequence's.
        if (like != nullptr && like->kind == SequenceValue::Kind::Numbers && like->count == 3) {
            numbers(out, {frame.position.x, frame.position.y, frame.position.z});
            return true;
        }
        const core::Vec3 turn = core::toEulerYxz(frame.rotation);
        numbers(out, {frame.position.x, frame.position.y, frame.position.z, static_cast<f64>(turn.x) * Degrees,
                      static_cast<f64>(turn.y) * Degrees, static_cast<f64>(turn.z) * Degrees});
        return true;
    }
    case scene::ValueType::Vector2: {
        const core::Vec2 v = std::get<core::Vec2>(value);
        numbers(out, {static_cast<f64>(v.x), static_cast<f64>(v.y)});
        return true;
    }
    case scene::ValueType::UDim: {
        const core::UDim d = std::get<core::UDim>(value);
        numbers(out, {static_cast<f64>(d.scale), static_cast<f64>(d.offset)});
        return true;
    }
    case scene::ValueType::UDim2: {
        const core::UDim2 d = std::get<core::UDim2>(value);
        numbers(out, {static_cast<f64>(d.x.scale), static_cast<f64>(d.x.offset), static_cast<f64>(d.y.scale),
                      static_cast<f64>(d.y.offset)});
        return true;
    }
    case scene::ValueType::EnumItem: {
        const scene::EnumValue item = std::get<scene::EnumValue>(value);
        const scene::EnumItemDesc* named = world.enums().findValue(item.enumId, item.value);
        if (named == nullptr)
            return false;
        out.kind = SequenceValue::Kind::Text;
        out.text = std::string(world.atoms().text(named->name));
        return true;
    }
    default:
        return false;
    }
}

bool Timeline::open(scene::World& world, Sequences& sequences, core::InstanceId player)
{
    if (isOpen())
        close(world, sequences);
    const scene::SequencePlayerComponent* self = world.sequencePlayers().find(player);
    if (self == nullptr || self->sequence.id == 0 || sequences.unreadable(world, player))
        return false;
    const asset::Sequence* held = sequences.sequenceOf(world, player);
    m_sequence = held != nullptr ? *held : asset::Sequence{};
    m_player = player;
    m_file = self->sequence;
    m_time = 0.0;
    m_shownTime = -1.0;
    m_dirty = false;
    // A file that is not there yet is given to the player as this empty copy,
    // so it has something to show a key on.
    m_stale = held == nullptr;
    m_revision = sequences.revisionOf(world, player);
    m_recording.assign(m_sequence.tracks.size(), false);
    m_shown.assign(m_sequence.tracks.size(), std::nullopt);
    return true;
}

void Timeline::close(scene::World& world, Sequences& sequences)
{
    if (!isOpen())
        return;
    sequences.endPreview(world);
    // What was not saved is not what the player plays.
    sequences.forget(std::span<const core::NameAtom>{&m_file, 1});
    *this = Timeline{};
}

void Timeline::settle(scene::World& world, Sequences& sequences)
{
    if (!isOpen())
        return;
    sequences.endPreview(world);
    m_shownTime = -1.0;
    // And the copy is given again when it is next shown: a world put back,
    // or made anew by a play, may not have it.
    m_stale = true;
}

void Timeline::scrub(f64 seconds) noexcept
{
    m_time = std::max(0.0, seconds);
}

void Timeline::touched() noexcept
{
    m_dirty = true;
    m_stale = true;
    // The length is never less than the last key.
    for (const SequenceTrack& track : m_sequence.tracks) {
        if (!track.keys.empty())
            m_sequence.length = std::max(m_sequence.length, track.keys.back().time);
    }
}

u32 Timeline::update(scene::World& world, Sequences& sequences)
{
    if (!isOpen())
        return 0;
    if (!world.alive(m_player) || world.destroyed(m_player) || world.sequencePlayers().find(m_player) == nullptr) {
        close(world, sequences);
        return 0;
    }
    // **The file was read again under the copy**: written by another tool,
    // or by this one's own save. A copy with nothing unsaved in it becomes
    // the file; one with changes stays what it is and is given again.
    if (const u32 reading = sequences.revisionOf(world, m_player); reading != m_revision) {
        if (m_dirty) {
            m_stale = true;
        }
        else if (const asset::Sequence* held = sequences.sequenceOf(world, m_player); held != nullptr) {
            m_sequence = *held;
            m_recording.assign(m_sequence.tracks.size(), false);
            m_shown.assign(m_sequence.tracks.size(), std::nullopt);
            m_shownTime = -1.0;
        }
        m_revision = reading;
    }
    // **What changed under a recording track is keyed**, before the moment is
    // shown again -- showing it is what would put the old value back.
    u32 keyed = 0;
    if (sequences.previewed() == m_player) {
        for (u32 track = 0; track < m_sequence.tracks.size(); ++track) {
            if (!m_recording[track] || !m_shown[track].has_value() ||
                m_sequence.tracks[track].type != SequenceTrackType::Property)
                continue;
            const core::InstanceId target = sequences.resolve(world, m_player, m_sequence.tracks[track].target);
            const core::NameAtom property = world.atoms().lookup(m_sequence.tracks[track].property);
            const std::optional<scene::Value> now =
                target.valid() && property.id != 0 ? world.getProperty(target, property) : std::nullopt;
            if (now.has_value() && !(*now == *m_shown[track]) && keyFromScene(world, sequences, track))
                ++keyed;
        }
    }
    if (m_stale) {
        sequences.put(world, m_file, m_sequence);
        m_revision = sequences.revisionOf(world, m_player);
        m_stale = false;
        m_shownTime = -1.0;
    }
    if (m_shownTime != m_time || sequences.previewed() != m_player) {
        sequences.preview(world, m_player, m_time);
        // A clip still being read is posed when it has been: the moment is
        // shown again, for a few seconds at most -- a clip that is not in
        // its file never arrives.
        m_shownTime = !sequences.previewSettled() && ++m_waited < 240 ? -1.0 : m_time;
        if (sequences.previewSettled())
            m_waited = 0;
        // What each property is now that the moment is shown: what a change
        // is measured against.
        for (u32 track = 0; track < m_sequence.tracks.size(); ++track) {
            m_shown[track].reset();
            if (m_sequence.tracks[track].type != SequenceTrackType::Property)
                continue;
            const core::InstanceId target = sequences.resolve(world, m_player, m_sequence.tracks[track].target);
            const core::NameAtom property = world.atoms().lookup(m_sequence.tracks[track].property);
            if (target.valid() && property.id != 0)
                m_shown[track] = world.getProperty(target, property);
        }
    }
    return keyed;
}

// --- Tracks ---------------------------------------------------------------------

u32 Timeline::addTrack(SequenceTrackType type, std::string_view target, std::string_view property)
{
    SequenceTrack track;
    track.type = type;
    track.target = std::string(target);
    track.property = std::string(property);
    std::string_view binding;
    std::string_view path;
    asset::splitSequenceTarget(target, binding, path);
    // **Who a cast track brings is not a binding the file expects**: the
    // track makes them, under the name it is given, of the stamp it names.
    if (type == SequenceTrackType::Cast) {
        track.model = std::string(property);
        track.property.clear();
    }
    else if (!binding.empty() &&
             std::find(m_sequence.bindings.begin(), m_sequence.bindings.end(), binding) == m_sequence.bindings.end())
        m_sequence.bindings.emplace_back(binding);
    m_sequence.tracks.push_back(std::move(track));
    m_recording.push_back(false);
    m_shown.emplace_back();
    touched();
    return static_cast<u32>(m_sequence.tracks.size() - 1);
}

void Timeline::removeTrack(u32 track)
{
    if (track >= m_sequence.tracks.size())
        return;
    m_sequence.tracks.erase(m_sequence.tracks.begin() + track);
    m_recording.erase(m_recording.begin() + track);
    m_shown.erase(m_shown.begin() + track);
    touched();
}

void Timeline::setRecording(u32 track, bool on)
{
    if (track < m_recording.size())
        m_recording[track] = on;
}

bool Timeline::recording(u32 track) const noexcept
{
    return track < m_recording.size() && m_recording[track];
}

void Timeline::setLength(f32 seconds)
{
    m_sequence.length = std::max(0.0f, seconds);
    touched();
}

// --- Keys -----------------------------------------------------------------------

u32 Timeline::place(std::vector<SequenceKey>& keys, SequenceKey key)
{
    // A key at this moment already is replaced: one moment, one key -- but
    // for a cut, which is the second key of its moment by design.
    for (u32 index = 0; index < keys.size(); ++index) {
        if (std::abs(keys[index].time - key.time) <= SameMoment && keys[index].cut == key.cut) {
            keys[index] = std::move(key);
            return index;
        }
    }
    u32 at = 0;
    while (at < keys.size() && keys[at].time <= key.time)
        ++at;
    keys.insert(keys.begin() + at, std::move(key));
    return at;
}

bool Timeline::keyFromScene(const scene::World& world, const Sequences& sequences, u32 track)
{
    if (track >= m_sequence.tracks.size() || m_sequence.tracks[track].type != SequenceTrackType::Property)
        return false;
    SequenceTrack& edited = m_sequence.tracks[track];
    const core::InstanceId target = sequences.resolve(world, m_player, edited.target);
    const core::NameAtom property = world.atoms().lookup(edited.property);
    const std::optional<scene::Value> now =
        target.valid() && property.id != 0 ? world.getProperty(target, property) : std::nullopt;
    SequenceKey key;
    key.time = static_cast<f32>(m_time);
    if (!now.has_value() ||
        !sequenceValueOf(world, *now, edited.keys.empty() ? nullptr : &edited.keys.front().value, key.value))
        return false;
    // Of the kind its keys are: a number is not eased into a word.
    if (!edited.keys.empty() &&
        (edited.keys.front().value.kind != key.value.kind || edited.keys.front().value.count != key.value.count))
        return false;
    (void)place(edited.keys, std::move(key));
    touched();
    return true;
}

u32 Timeline::keyCamera(const core::CFrameD& view, f32 fieldOfView, bool cut)
{
    u32 track = 0;
    while (track < m_sequence.tracks.size() && m_sequence.tracks[track].type != SequenceTrackType::Camera)
        ++track;
    if (track == m_sequence.tracks.size())
        track = addTrack(SequenceTrackType::Camera, {}, {});
    SequenceKey key;
    key.time = static_cast<f32>(m_time);
    key.cut = cut;
    key.hasPosition = true;
    key.position[0] = view.position.x;
    key.position[1] = view.position.y;
    key.position[2] = view.position.z;
    const core::Vec3 turn = core::toEulerYxz(view.rotation);
    key.hasRotation = true;
    key.rotation[0] = static_cast<f64>(turn.x) * Degrees;
    key.rotation[1] = static_cast<f64>(turn.y) * Degrees;
    key.rotation[2] = static_cast<f64>(turn.z) * Degrees;
    key.fieldOfView = fieldOfView;
    const u32 at = place(m_sequence.tracks[track].keys, std::move(key));
    touched();
    return at;
}

bool Timeline::keyNamed(u32 track, std::string_view name)
{
    if (track >= m_sequence.tracks.size() || name.empty())
        return false;
    SequenceKey key;
    key.time = static_cast<f32>(m_time);
    switch (m_sequence.tracks[track].type) {
    case SequenceTrackType::Event:
    case SequenceTrackType::Hold:
    case SequenceTrackType::Marker:
        key.name = std::string(name);
        break;
    case SequenceTrackType::Caption:
        key.caption = std::string(name);
        break;
    case SequenceTrackType::Line:
        key.line = std::string(name);
        break;
    case SequenceTrackType::Sound:
        key.sound = std::string(name);
        break;
    case SequenceTrackType::Animation:
        key.clip = std::string(name);
        break;
    default:
        return false;
    }
    (void)place(m_sequence.tracks[track].keys, std::move(key));
    touched();
    return true;
}

bool Timeline::keyShown(u32 track, bool shown)
{
    if (track >= m_sequence.tracks.size() || m_sequence.tracks[track].type != SequenceTrackType::Cast)
        return false;
    SequenceKey key;
    key.time = static_cast<f32>(m_time);
    key.show = shown;
    (void)place(m_sequence.tracks[track].keys, std::move(key));
    touched();
    return true;
}

bool Timeline::keyNumber(u32 track, f64 value)
{
    if (track >= m_sequence.tracks.size())
        return false;
    const SequenceTrackType type = m_sequence.tracks[track].type;
    if (type != SequenceTrackType::Morph && type != SequenceTrackType::Fade && type != SequenceTrackType::Bars)
        return false;
    SequenceKey key;
    key.time = static_cast<f32>(m_time);
    key.value.kind = SequenceValue::Kind::Number;
    key.value.count = 1;
    key.value.numbers[0] = value;
    (void)place(m_sequence.tracks[track].keys, std::move(key));
    touched();
    return true;
}

u32 Timeline::moveKey(u32 track, u32 key, f32 seconds)
{
    if (track >= m_sequence.tracks.size() || key >= m_sequence.tracks[track].keys.size())
        return key;
    std::vector<SequenceKey>& keys = m_sequence.tracks[track].keys;
    SequenceKey moved = std::move(keys[key]);
    keys.erase(keys.begin() + key);
    moved.time = std::max(0.0f, seconds);
    u32 at = 0;
    while (at < keys.size() && keys[at].time <= moved.time)
        ++at;
    keys.insert(keys.begin() + at, std::move(moved));
    touched();
    return at;
}

void Timeline::removeKey(u32 track, u32 key)
{
    if (track >= m_sequence.tracks.size() || key >= m_sequence.tracks[track].keys.size())
        return;
    m_sequence.tracks[track].keys.erase(m_sequence.tracks[track].keys.begin() + key);
    touched();
}

void Timeline::setCut(u32 track, u32 key, bool cut)
{
    if (track >= m_sequence.tracks.size() || key >= m_sequence.tracks[track].keys.size())
        return;
    m_sequence.tracks[track].keys[key].cut = cut;
    touched();
}

void Timeline::setEase(u32 track, u32 key, core::EasingStyle style, core::EasingDirection direction)
{
    if (track >= m_sequence.tracks.size() || key >= m_sequence.tracks[track].keys.size())
        return;
    m_sequence.tracks[track].keys[key].style = style;
    m_sequence.tracks[track].keys[key].direction = direction;
    touched();
}

std::string Timeline::text() const
{
    return asset::writeSequence(m_sequence);
}

} // namespace engine::app
