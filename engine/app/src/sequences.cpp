#include "engine/app/sequences.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/render/animation.h"
#include "engine/scene/change_queue.h"
#include "engine/scene/localization.h"
#include "engine/scene/morph_host.h"
#include "engine/scene/pivot.h"
#include "engine/scene/scene_file.h"
#include "engine/scene/world.h"

namespace engine::app {
namespace {

using asset::Sequence;
using asset::SequenceKey;
using asset::SequenceSpan;
using asset::SequenceTrack;
using asset::SequenceTrackType;
using asset::SequenceValue;
using core::f32;
using core::f64;
using core::u32;
using core::usize;

constexpr f64 Radians = 3.14159265358979323846 / 180.0;

[[nodiscard]] core::Mat3 turnOf(const f64 degrees[3]) noexcept
{
    return core::fromEulerYxz(core::Vec3{static_cast<f32>(degrees[0] * Radians), static_cast<f32>(degrees[1] * Radians),
                                         static_cast<f32>(degrees[2] * Radians)});
}

// **A keyed value as the kind of value the property holds.** What three
// numbers mean is the property's to say: a place on one, a colour on another.
// False for a value the property cannot be given -- a word on a number.
[[nodiscard]] bool asValue(const scene::World& world, const SequenceValue& from, const scene::Value& like,
                           scene::Value& out)
{
    const bool number = from.kind == SequenceValue::Kind::Number;
    const auto numbers = [&](u32 count) { return from.kind == SequenceValue::Kind::Numbers && from.count == count; };
    switch (scene::valueType(like)) {
    case scene::ValueType::Bool:
        if (from.kind == SequenceValue::Kind::Flag)
            out = from.flag;
        else if (number)
            out = from.numbers[0] != 0.0;
        else
            return false;
        return true;
    case scene::ValueType::Number:
        if (!number)
            return false;
        out = from.numbers[0];
        return true;
    case scene::ValueType::String:
        if (from.kind != SequenceValue::Kind::Text)
            return false;
        out = from.text;
        return true;
    case scene::ValueType::Vector3:
        if (!numbers(3))
            return false;
        out = core::Vec3{static_cast<f32>(from.numbers[0]), static_cast<f32>(from.numbers[1]),
                         static_cast<f32>(from.numbers[2])};
        return true;
    case scene::ValueType::Color3:
        if (!numbers(3))
            return false;
        out = core::Color3{static_cast<f32>(from.numbers[0]), static_cast<f32>(from.numbers[1]),
                           static_cast<f32>(from.numbers[2])};
        return true;
    case scene::ValueType::CFrame: {
        // Three numbers are a place, and the turn it has is kept; six are a
        // place and a turn in degrees.
        core::CFrameD frame = std::get<core::CFrameD>(like);
        if (!numbers(3) && !numbers(6))
            return false;
        frame.position = core::DVec3{from.numbers[0], from.numbers[1], from.numbers[2]};
        if (from.count == 6)
            frame.rotation = turnOf(&from.numbers[3]);
        out = frame;
        return true;
    }
    case scene::ValueType::Vector2:
        if (!numbers(2))
            return false;
        out = core::Vec2{static_cast<f32>(from.numbers[0]), static_cast<f32>(from.numbers[1])};
        return true;
    case scene::ValueType::UDim:
        if (!numbers(2))
            return false;
        out = core::UDim{static_cast<f32>(from.numbers[0]), static_cast<f32>(from.numbers[1])};
        return true;
    case scene::ValueType::UDim2:
        if (!numbers(4))
            return false;
        out = core::UDim2{core::UDim{static_cast<f32>(from.numbers[0]), static_cast<f32>(from.numbers[1])},
                          core::UDim{static_cast<f32>(from.numbers[2]), static_cast<f32>(from.numbers[3])}};
        return true;
    case scene::ValueType::EnumItem: {
        const scene::EnumValue held = std::get<scene::EnumValue>(like);
        if (from.kind == SequenceValue::Kind::Text) {
            const scene::EnumItemDesc* item = world.enums().findItem(held.enumId, world.atoms().lookup(from.text));
            if (item == nullptr)
                return false;
            out = scene::EnumValue{held.enumId, item->value};
            return true;
        }
        if (number && world.enums().findValue(held.enumId, static_cast<core::i32>(from.numbers[0])) != nullptr) {
            out = scene::EnumValue{held.enumId, static_cast<core::i32>(from.numbers[0])};
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

// **A key's value**: its own, or -- written `"$Name"` -- what the player's
// attribute of that name holds at this moment. An attribute that is not
// there, or holds what a key cannot, is no value.
[[nodiscard]] SequenceValue valueOf(const scene::World& world, core::InstanceId player, const SequenceKey& key)
{
    if (key.variable.empty())
        return key.value;
    SequenceValue out;
    const core::NameAtom name = world.atoms().lookup(key.variable);
    if (name.id == 0 || !player.valid())
        return out;
    const scene::Value held = world.getAttribute(player, name);
    const auto numbers = [&](std::initializer_list<f64> from) {
        out.kind = SequenceValue::Kind::Numbers;
        out.count = static_cast<core::u8>(from.size());
        std::copy(from.begin(), from.end(), std::begin(out.numbers));
    };
    switch (scene::valueType(held)) {
    case scene::ValueType::Bool:
        out.kind = SequenceValue::Kind::Flag;
        out.flag = std::get<bool>(held);
        break;
    case scene::ValueType::Number:
        out.kind = SequenceValue::Kind::Number;
        out.count = 1;
        out.numbers[0] = std::get<f64>(held);
        break;
    case scene::ValueType::String:
        out.kind = SequenceValue::Kind::Text;
        out.text = std::get<std::string>(held);
        break;
    case scene::ValueType::Vector3: {
        const core::Vec3 three = std::get<core::Vec3>(held);
        numbers({static_cast<f64>(three.x), static_cast<f64>(three.y), static_cast<f64>(three.z)});
        break;
    }
    case scene::ValueType::Color3: {
        const core::Color3 colour = std::get<core::Color3>(held);
        numbers({static_cast<f64>(colour.r), static_cast<f64>(colour.g), static_cast<f64>(colour.b)});
        break;
    }
    case scene::ValueType::Vector2: {
        const core::Vec2 two = std::get<core::Vec2>(held);
        numbers({static_cast<f64>(two.x), static_cast<f64>(two.y)});
        break;
    }
    default:
        break;
    }
    return out;
}

// Where a keyed track is at `at`: its value there. False before its first
// key, and where the game has not said what a variable is.
[[nodiscard]] bool valueAt(const scene::World& world, core::InstanceId player, const SequenceTrack& track, f64 at,
                           SequenceValue& out)
{
    if (track.keys.empty() || at < static_cast<f64>(track.keys.front().time))
        return false;
    SequenceSpan span;
    if (!asset::sequenceSpanAt(track.keys, static_cast<f32>(at), span))
        return false;
    out = span.from == span.to ? valueOf(world, player, track.keys[span.from])
                               : asset::mixSequenceValues(valueOf(world, player, track.keys[span.from]),
                                                          valueOf(world, player, track.keys[span.to]), span.alpha);
    return out.kind != SequenceValue::Kind::None;
}

// **A hand that is not steady**: a number between minus one and one that
// wanders smoothly as `phase` goes on -- one turn of it a whole number --
// and is the same number for the same phase on every machine, in every run.
// A channel each for what is shaken, so across and up do not move together.
[[nodiscard]] f64 wander(f64 phase, core::u32 channel) noexcept
{
    const auto lattice = [channel](core::i64 at) {
        core::u64 x = static_cast<core::u64>(at) * 0x9E3779B97F4A7C15ull +
                      static_cast<core::u64>(channel + 1) * 0xD6E8FEB86659FD93ull;
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return static_cast<f64>(x >> 11) * (2.0 / 9007199254740992.0) - 1.0;
    };
    const auto smooth = [&](f64 x) {
        const f64 whole = std::floor(x);
        const f64 part = x - whole;
        const f64 eased = part * part * (3.0 - 2.0 * part);
        const f64 from = lattice(static_cast<core::i64>(whole));
        return from + (lattice(static_cast<core::i64>(whole) + 1) - from) * eased;
    };
    // A slow sway with a quicker tremble on it. Two points of it a turn.
    return (smooth(phase * 2.0) + 0.5 * smooth(phase * 4.0 + 17.0)) / 1.5;
}

// The workspace a player is in: where what a scrub brings is put.
[[nodiscard]] core::InstanceId workspaceOver(const scene::World& world, core::InstanceId player)
{
    core::InstanceId top = player;
    for (core::InstanceId at = player; at.valid(); at = world.parentOf(at)) {
        if (world.workspaces().find(at) != nullptr)
            return at;
        top = at;
    }
    for (core::InstanceId child = world.firstChild(top); child.valid(); child = world.nextSibling(child)) {
        if (world.workspaces().find(child) != nullptr)
            return child;
    }
    // A world with none: beside the player.
    return world.parentOf(player);
}

constexpr std::string_view MadeScheme = "memory://";

// The player a clip is played on: the instance itself, or the first one under
// it or under one of its children -- a character's is a child of its model.
[[nodiscard]] core::InstanceId animatorOf(const scene::World& world, core::InstanceId target)
{
    if (!target.valid())
        return {};
    if (world.animationPlayers().find(target) != nullptr)
        return target;
    for (core::InstanceId child = world.firstChild(target); child.valid(); child = world.nextSibling(child)) {
        if (world.animationPlayers().find(child) != nullptr)
            return child;
    }
    for (core::InstanceId child = world.firstChild(target); child.valid(); child = world.nextSibling(child)) {
        for (core::InstanceId inner = world.firstChild(child); inner.valid(); inner = world.nextSibling(inner)) {
            if (world.animationPlayers().find(inner) != nullptr)
                return inner;
        }
    }
    return {};
}

[[nodiscard]] core::i32 categoryOf(std::string_view name) noexcept
{
    if (name == "Music")
        return 1;
    if (name == "Voice")
        return 2;
    return 0;
}

// The first key of `keys` later than `at` -- or, `including`, at it or later.
[[nodiscard]] u32 firstAfter(const std::vector<SequenceKey>& keys, f64 at, bool including) noexcept
{
    u32 index = 0;
    while (index < keys.size() &&
           (including ? static_cast<f64>(keys[index].time) < at : static_cast<f64>(keys[index].time) <= at))
        ++index;
    return index;
}

[[nodiscard]] scene::EventNote noteOf(const SequenceKey& key, bool skipped)
{
    scene::EventNote note;
    note.name = key.name;
    note.skipped = skipped;
    switch (key.value.kind) {
    case SequenceValue::Kind::Number:
        note.kind = scene::EventNote::Kind::Number;
        note.numbers[0] = key.value.numbers[0];
        break;
    case SequenceValue::Kind::Flag:
        note.kind = scene::EventNote::Kind::Flag;
        note.flag = key.value.flag;
        break;
    case SequenceValue::Kind::Numbers:
        note.kind = scene::EventNote::Kind::Numbers;
        note.count = key.value.count;
        std::copy(std::begin(key.value.numbers), std::end(key.value.numbers), std::begin(note.numbers));
        break;
    case SequenceValue::Kind::Text:
        note.kind = scene::EventNote::Kind::Text;
        note.text = key.value.text;
        break;
    case SequenceValue::Kind::None:
        break;
    }
    return note;
}

} // namespace

// --- Files ----------------------------------------------------------------------

core::f32 Sequences::lineSlot(const scene::World& world, std::string_view line) const
{
    const scene::DialogueLines* lines = world.dialogue();
    const scene::DialogueLine* found = lines != nullptr ? lines->find(line) : nullptr;
    if (found == nullptr || !m_sources.lineSeconds)
        return 0.0f;
    return static_cast<f32>(m_sources.lineSeconds(found->sound, found->text, found->seconds));
}

const Sequences::File* Sequences::fileOf(core::NameAtom urn) const noexcept
{
    for (const File& file : m_files) {
        if (file.urn.id == urn.id)
            return file.found ? &file : nullptr;
    }
    return nullptr;
}

Sequences::File* Sequences::fileFor(const scene::World& world, core::NameAtom urn)
{
    if (urn.id == 0)
        return nullptr;
    for (File& file : m_files) {
        if (file.urn.id == urn.id)
            return file.found ? &file : nullptr;
    }
    File read;
    read.urn = urn;
    const std::string name(world.atoms().text(urn));
    // **One a script wrote on another machine** (`LoadSequence`): there is no
    // file of that name to look for, here or anywhere, and nothing to say.
    const bool made = std::string_view{name}.starts_with(MadeScheme);
    const std::optional<std::string> text = m_sources.read && !made ? m_sources.read(name) : std::nullopt;
    if (!text.has_value()) {
        if (!made) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"path", std::string_view{name}}};
            core::log(core::LogLevel::Warn, ENG_TR("app.warn.sequence_missing"), args);
        }
    }
    else {
        std::string where;
        read.there = true;
        read.found = asset::readSequence(*text, read.sequence, &where);
        if (!read.found) {
            const std::array<core::I18nArg, 2> args{core::I18nArg{"path", std::string_view{name}},
                                                    core::I18nArg{"where", std::string_view{where}}};
            core::log(core::LogLevel::Warn, ENG_TR("app.warn.sequence_unreadable"), args);
        }
    }
    if (read.found)
        measure(world, read);
    read.revision = ++m_revisions;
    m_files.push_back(std::move(read));
    return m_files.back().found ? &m_files.back() : nullptr;
}

void Sequences::measure(const scene::World& world, File& file) const
{
    // **A line's slot is part of the length** (ADR 0204): the longest of its
    // languages, so the sequence is as long in every one of them.
    file.length = file.sequence.length;
    file.heard.clear();
    bool after = false;
    for (const SequenceTrack& track : file.sequence.tracks) {
        for (const SequenceKey& key : track.keys) {
            after = after || !key.afterLine.empty();
            if (track.type == SequenceTrackType::Line)
                file.length = std::max(file.length, key.time + lineSlot(world, key.line));
        }
    }
    if (!after)
        return;

    // **A key after a line, as heard** (ADR 0204): at the end of the line's
    // recording in the language this machine's voices are in, with the key's
    // own time counted on from there. The line's SLOT is what the sequence's
    // length is made of -- the longest of its languages, every machine's --
    // so such a key is never past the end here and before it somewhere else.
    const scene::DialogueLines* lines = world.dialogue();
    const auto said = [&](std::string_view line, f32& begins, f32& heard, f32& slot) {
        for (const SequenceTrack& track : file.sequence.tracks) {
            if (track.type != SequenceTrackType::Line)
                continue;
            for (const SequenceKey& key : track.keys) {
                if (key.line != line)
                    continue;
                begins = key.time;
                slot = lineSlot(world, line);
                heard = slot;
                const scene::DialogueLine* found = lines != nullptr ? lines->find(line) : nullptr;
                if (found != nullptr && m_sources.lineHeard)
                    heard = std::min(slot,
                                     static_cast<f32>(m_sources.lineHeard(found->sound, found->text, found->seconds)));
                return true;
            }
        }
        return false;
    };
    file.heard.resize(file.sequence.tracks.size());
    for (usize index = 0; index < file.sequence.tracks.size(); ++index) {
        const SequenceTrack& track = file.sequence.tracks[index];
        if (std::none_of(track.keys.begin(), track.keys.end(),
                         [](const SequenceKey& key) { return !key.afterLine.empty(); }))
            continue;
        SequenceTrack heard = track;
        for (SequenceKey& key : heard.keys) {
            f32 begins = 0.0f;
            f32 length = 0.0f;
            f32 slot = 0.0f;
            if (key.afterLine.empty() || !said(key.afterLine, begins, length, slot))
                continue;
            file.length = std::max(file.length, begins + slot + key.time);
            key.time += begins + length;
        }
        std::stable_sort(heard.keys.begin(), heard.keys.end(),
                         [](const SequenceKey& a, const SequenceKey& b) { return a.time < b.time; });
        file.heard[index] = std::move(heard);
    }
}

const asset::SequenceTrack& Sequences::pictured(const File& file, usize index) noexcept
{
    return index < file.heard.size() && !file.heard[index].keys.empty() ? file.heard[index]
                                                                        : file.sequence.tracks[index];
}

void Sequences::hearAgain(const scene::World& world)
{
    const std::string& voice = world.engineState().voiceLocale;
    if (voice == m_voice)
        return;
    m_voice = voice;
    for (File& file : m_files) {
        if (file.found && !file.heard.empty())
            measure(world, file);
    }
}

bool Sequences::loadSequence(scene::World& world, core::InstanceId player, std::string_view json, std::string& what)
{
    scene::SequencePlayerComponent* self = world.sequencePlayers().find(player);
    if (self == nullptr) {
        what = "player";
        return false;
    }
    asset::Sequence read;
    if (!asset::readSequence(json, read, &what))
        return false;
    // The one it was given before goes: a game that writes a sequence for
    // every blow would otherwise keep every one it ever wrote.
    const core::u32 before = self->sequence.id;
    std::erase_if(m_files, [&](const File& file) { return file.made && file.urn.id == before; });
    // **A name no file has**, and not the name a script on another machine
    // gives one of its own: what a replica is sent as this player's
    // `Sequence` must not find a sequence this machine wrote for itself.
    const bool replica = world.engineState().networkTopology == scene::NetworkTopology::Replica;
    const std::string name = std::string(MadeScheme) + (replica ? "here/" : "sequence/") + std::to_string(++m_made);
    const core::NameAtom urn = world.atoms().intern(name);
    put(world, urn, std::move(read));
    for (File& file : m_files) {
        if (file.urn.id == urn.id)
            file.made = true;
    }
    (void)world.setProperty(player, world.atoms().intern("Sequence"), scene::Value{name});
    return true;
}

void Sequences::put(const scene::World& world, core::NameAtom urn, asset::Sequence sequence)
{
    if (urn.id == 0)
        return;
    File* held = nullptr;
    for (File& file : m_files) {
        if (file.urn.id == urn.id)
            held = &file;
    }
    if (held == nullptr) {
        m_files.push_back(File{});
        held = &m_files.back();
        held->urn = urn;
    }
    held->found = true;
    held->there = true;
    held->revision = ++m_revisions;
    held->sequence = std::move(sequence);
    measure(world, *held);
}

void Sequences::forget(std::span<const core::NameAtom> urns)
{
    std::erase_if(m_files, [&](const File& file) {
        return std::any_of(urns.begin(), urns.end(), [&](core::NameAtom urn) { return urn.id == file.urn.id; });
    });
}

void Sequences::forgetAll()
{
    // What a script wrote is not a file, and cannot be read again.
    std::erase_if(m_files, [](const File& file) { return !file.made; });
}

void Sequences::clear()
{
    m_files.clear();
    m_states.clear();
    m_preview = Preview{};
    m_order = 0;
    m_made = 0;
    m_voice.clear();
    m_workspace = {};
    m_stats = Stats{};
}

const asset::Sequence* Sequences::sequenceOf(const scene::World& world, core::InstanceId player)
{
    const scene::SequencePlayerComponent* self = world.sequencePlayers().find(player);
    const File* file = self != nullptr ? fileFor(world, self->sequence) : nullptr;
    return file != nullptr ? &file->sequence : nullptr;
}

bool Sequences::unreadable(const scene::World& world, core::InstanceId player)
{
    const scene::SequencePlayerComponent* self = world.sequencePlayers().find(player);
    if (self == nullptr || fileFor(world, self->sequence) != nullptr)
        return false;
    for (const File& file : m_files) {
        if (file.urn.id == self->sequence.id)
            return file.there;
    }
    return false;
}

core::u32 Sequences::revisionOf(const scene::World& world, core::InstanceId player)
{
    const scene::SequencePlayerComponent* self = world.sequencePlayers().find(player);
    if (self == nullptr)
        return 0;
    (void)fileFor(world, self->sequence);
    for (const File& file : m_files) {
        if (file.urn.id == self->sequence.id)
            return file.revision;
    }
    return 0;
}

core::f32 Sequences::sequenceLength(const scene::World& world, core::NameAtom file)
{
    const File* read = fileFor(world, file);
    return read != nullptr ? read->length : 0.0f;
}

void Sequences::sequenceMarkers(const scene::World& world, core::NameAtom file, std::vector<scene::SequenceMarker>& out)
{
    out.clear();
    const File* read = fileFor(world, file);
    if (read == nullptr)
        return;
    for (const SequenceTrack& track : read->sequence.tracks) {
        if (track.type != SequenceTrackType::Marker)
            continue;
        for (const SequenceKey& key : track.keys)
            out.push_back(scene::SequenceMarker{key.name, key.time});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const scene::SequenceMarker& a, const scene::SequenceMarker& b) { return a.time < b.time; });
}

std::string Sequences::sequenceHoldAt(const scene::World& world, core::NameAtom file, f64 seconds)
{
    const File* read = fileFor(world, file);
    if (read == nullptr)
        return {};
    for (const SequenceTrack& track : read->sequence.tracks) {
        if (track.type != SequenceTrackType::Hold)
            continue;
        for (const SequenceKey& key : track.keys) {
            if (std::abs(static_cast<f64>(key.time) - seconds) < 1.0e-3)
                return key.name;
        }
    }
    return {};
}

core::f32 Sequences::lengthOf(const scene::World& world, core::InstanceId player)
{
    const scene::SequencePlayerComponent* self = world.sequencePlayers().find(player);
    const File* file = self != nullptr ? fileFor(world, self->sequence) : nullptr;
    return file != nullptr ? file->length : 0.0f;
}

// --- Bindings -------------------------------------------------------------------

core::InstanceId Sequences::resolve(const scene::World& world, core::InstanceId player, std::string_view target) const
{
    std::string_view binding;
    std::string_view path;
    asset::splitSequenceTarget(target, binding, path);
    const core::NameAtom name = world.atoms().lookup(binding);
    if (name.id == 0)
        return {};
    core::InstanceId at;
    for (core::InstanceId child = world.firstChild(player); child.valid(); child = world.nextSibling(child)) {
        const scene::SequenceBindingComponent* bound = world.sequenceBindings().find(child);
        if (bound != nullptr && world.name(child).id == name.id) {
            at = bound->target;
            break;
        }
    }
    // Whoever the scrub's cast tracks have brought: bound by no instance,
    // because a scrub leaves none in the scene.
    if (!at.valid() && player == m_preview.player) {
        for (const Cast& one : m_preview.cast) {
            if (one.name.id == name.id)
                at = one.root;
        }
    }
    if (!at.valid() || !world.alive(at) || world.destroyed(at))
        return {};
    while (!path.empty()) {
        const usize slash = path.find('/');
        const std::string_view step = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        const core::NameAtom child = world.atoms().lookup(step);
        at = child.id != 0 ? world.findFirstChild(at, child) : core::InstanceId{};
        if (!at.valid())
            return {};
    }
    return at;
}

// --- States ---------------------------------------------------------------------

Sequences::State& Sequences::stateFor(core::InstanceId player, bool& fresh)
{
    for (State& state : m_states) {
        if (state.player == player) {
            fresh = false;
            return state;
        }
    }
    fresh = true;
    State made;
    made.player = player;
    m_states.push_back(std::move(made));
    return m_states.back();
}

const Sequences::State* Sequences::stateOf(core::InstanceId player) const noexcept
{
    for (const State& state : m_states) {
        if (state.player == player)
            return &state;
    }
    return nullptr;
}

void Sequences::anchor(const Tick& tick, scene::SequencePlayerComponent& player) const
{
    player.anchorTime = static_cast<f32>(player.timePosition);
    player.anchorTick = static_cast<u32>(tick.serverTick);
}

void Sequences::begin(const Tick& tick, State& state, const File& file)
{
    state.begun = true;
    state.order = ++m_order;
    // **A shape key the sequence keys is the sequence's while it plays**: a
    // weight left held on it -- by the last time this played -- would sit
    // over what the track draws.
    if (tick.morphs == nullptr)
        return;
    for (const SequenceTrack& track : file.sequence.tracks) {
        if (track.type != SequenceTrackType::Morph)
            continue;
        if (const core::InstanceId mesh = resolve(*tick.world, state.player, track.target); mesh.valid())
            tick.morphs->clearMorphWeight(mesh, track.property);
    }
}

void Sequences::endSounds(scene::World& world, State& state)
{
    for (const core::InstanceId id : state.sounds) {
        if (!world.alive(id) || world.destroyed(id))
            continue;
        scene::SoundComponent* sound = world.sounds().find(id);
        if (sound == nullptr || !sound->playing)
            continue;
        // Ended, not finished: no `Ended` fires, and a sound that was made
        // to be played once goes as one that has been.
        sound->playing = false;
        sound->timePosition = 0.0;
        if (sound->ownsItself)
            sound->endedFor = 0;
    }
    state.sounds.clear();
}

void Sequences::setDown(scene::World& world, const Carried& carried)
{
    if (!world.alive(carried.body) || world.destroyed(carried.body))
        return;
    // **On its feet**: where the keys left it, its turn about the vertical
    // kept and any lean dropped, with no speed -- and its own to simulate
    // again, so the ground under it is where it stands a moment later.
    const core::NameAtom frameName = world.atoms().intern("CFrame");
    if (const std::optional<scene::Value> held = world.getProperty(carried.body, frameName); held.has_value()) {
        if (const core::CFrameD* frame = std::get_if<core::CFrameD>(&*held); frame != nullptr) {
            core::CFrameD upright = *frame;
            const core::Vec3 turn = core::toEulerYxz(frame->rotation);
            upright.rotation = core::fromEulerYxz(core::Vec3{0.0f, turn.y, 0.0f});
            (void)world.setProperty(carried.body, frameName, scene::Value{upright});
        }
    }
    if (scene::CharacterBodyComponent* body = world.characterBodies().find(carried.body); body != nullptr) {
        body->verticalVelocity = 0.0f;
        body->push = core::Vec3{};
        body->moveDirection = core::Vec3{};
        body->jumpRequested = false;
    }
    (void)world.setProperty(carried.body, world.atoms().intern("Anchored"), scene::Value{carried.wasAnchored});
}

void Sequences::end(const Tick& tick, State& state, const File* file, scene::SequencePlayerComponent& player,
                    bool owner, End how)
{
    scene::World& world = *tick.world;
    if (tick.animation != nullptr) {
        for (Clip& clip : state.clips) {
            if (clip.on)
                tick.animation->stop(clip.id, clip.fade);
            clip.on = false;
        }
    }
    if (owner) {
        for (const Carried& carried : state.carried)
            setDown(world, carried);
        // What it began is ended with it. One that reached its end leaves
        // its last line to finish being said.
        if (how != End::Finished)
            endSounds(world, state);
        // **Whoever it brought**: stopped, they go with it -- the sequence is
        // back before it brought anybody. At its end, or skipped to it, they
        // are as its last key left them: there, or gone.
        dismiss(world, state.cast, how != End::Stopped);
    }
    state.carried.clear();
    state.sounds.clear();

    // **A shape key is left where the sequence left it**: the frame's layer
    // goes when the sequence does, so what it had got to is held as a script
    // holds one. Each machine's own, as that is.
    if (tick.morphs != nullptr && file != nullptr && state.begun) {
        for (usize index = 0; index < file->sequence.tracks.size(); ++index) {
            const SequenceTrack& track = pictured(*file, index);
            if (track.type != SequenceTrackType::Morph || track.keys.empty())
                continue;
            SequenceValue value;
            if (how == End::Stopped) {
                if (!valueAt(world, state.player, track, player.timePosition, value))
                    continue;
            }
            else {
                value = valueOf(world, state.player, track.keys.back());
            }
            if (const core::InstanceId mesh = resolve(world, state.player, track.target); mesh.valid())
                tick.morphs->setMorphWeight(mesh, track.property,
                                            std::clamp(static_cast<f32>(value.numbers[0]), 0.0f, 1.0f));
        }
    }
    state.begun = false;
}

// --- What a track does ----------------------------------------------------------

void Sequences::writeProperty(scene::World& world, core::InstanceId player, State* state, const SequenceTrack& track,
                              u32 index, core::InstanceId target, f64 at, bool owner, Preview* preview)
{
    // **A track that can write nothing says so, once.** A name that is not
    // a property of what the track is bound to, or a key whose value is not
    // that property's kind -- a colour as one number, an enum's item as a
    // word it has not got -- used to do nothing in silence, on a part and on
    // `Lighting` alike, and read from outside as "a track cannot key this".
    const auto refuse = [&]() {
        ++m_stats.refused;
        if (state == nullptr || state->warnedProperty)
            return;
        state->warnedProperty = true;
        const std::array<core::I18nArg, 3> args{core::I18nArg{"player", world.atoms().text(world.name(state->player))},
                                                core::I18nArg{"target", std::string_view{track.target}},
                                                core::I18nArg{"property", std::string_view{track.property}}};
        core::log(core::LogLevel::Warn, ENG_TR("app.warn.sequence_property"), args);
    };
    const core::NameAtom property = world.atoms().lookup(track.property);
    const std::optional<scene::Value> like =
        property.id != 0 ? world.getProperty(target, property) : std::optional<scene::Value>{};
    SequenceValue keyed;
    scene::Value value;
    if (!valueAt(world, player, track, at, keyed))
        return;
    if (!like.has_value() || !asValue(world, keyed, *like, value)) {
        refuse();
        return;
    }

    // **A character whose place is keyed is carried** (ADR 0204): held where
    // the keys say and not simulated -- as an anchored one is -- until the
    // track has run out, and then set down.
    if (state != nullptr && owner && world.characterBodies().find(target) != nullptr &&
        (track.property == "CFrame" || track.property == "Position")) {
        const bool known = std::any_of(state->carried.begin(), state->carried.end(),
                                       [&](const Carried& one) { return one.body == target && one.track == index; });
        if (!known) {
            const core::NameAtom anchored = world.atoms().intern("Anchored");
            const std::optional<scene::Value> was = world.getProperty(target, anchored);
            const bool* flag = was.has_value() ? std::get_if<bool>(&*was) : nullptr;
            // Carried by another track already: what it was before that one.
            bool before = flag != nullptr && *flag;
            for (const Carried& one : state->carried) {
                if (one.body == target)
                    before = one.wasAnchored;
            }
            state->carried.push_back(Carried{target, index, before});
            (void)world.setProperty(target, anchored, scene::Value{true});
        }
    }
    if (preview != nullptr) {
        const bool kept = std::any_of(preview->kept.begin(), preview->kept.end(), [&](const Kept& one) {
            return one.instance == target && one.property.id == property.id;
        });
        if (!kept)
            preview->kept.push_back(Kept{target, property, *like});
    }
    if (world.setProperty(target, property, value) == scene::World::SetResult::Changed)
        ++m_stats.written;
}

void Sequences::startClip(const Tick& tick, State& state, const SequenceTrack& track, u32 index, u32 key, f64 into)
{
    if (tick.animation == nullptr)
        return;
    scene::World& world = *tick.world;
    const SequenceKey& at = track.keys[key];
    const core::InstanceId animator = animatorOf(world, resolve(world, state.player, track.target));
    if (!animator.valid())
        return;
    const scene::SequencePlayerComponent* player = world.sequencePlayers().find(state.player);
    const bool running = player != nullptr && player->playing;

    Clip* slot = nullptr;
    for (Clip& clip : state.clips) {
        if (clip.track == index && clip.key == key && clip.animator == animator)
            slot = &clip;
    }
    if (slot == nullptr) {
        // As `LoadAnimation` names one: `file#Clip`, or the mesh's own `Clip`.
        std::string_view name = at.clip;
        std::string_view path;
        if (const usize hash = name.rfind('#'); hash != std::string_view::npos) {
            path = name.substr(0, hash);
            name = name.substr(hash + 1);
        }
        if (name.empty())
            return;
        Clip made;
        made.track = index;
        made.key = key;
        made.animator = animator;
        made.id =
            tick.animation->createTrack(animator, path.empty() ? core::NameAtom{} : world.atoms().intern(path), name);
        tick.animation->setOver(made.id, true);
        state.clips.push_back(made);
        slot = &state.clips.back();
    }
    // **The clip before it on this track goes as this one comes**: one fade,
    // from the one into the other.
    for (Clip& other : state.clips) {
        if (other.track == index && other.on && &other != slot) {
            tick.animation->stop(other.id, at.fade);
            other.on = false;
        }
    }
    slot->fade = at.fade;
    slot->hold = at.hold;
    slot->on = true;
    tick.animation->play(slot->id, at.fade, 1.0f, at.speed);
    if (into > tick.fixedDt)
        tick.animation->seek(slot->id, into * static_cast<f64>(at.speed));
    // A sequence that is not running holds its clips still.
    if (!running)
        tick.animation->adjustSpeed(slot->id, 0.0f);
}

void Sequences::sayLine(const Tick& tick, State& state, const SequenceKey& key, f64 into)
{
    scene::World& world = *tick.world;
    const scene::DialogueLines* lines = world.dialogue();
    const scene::DialogueLine* line = lines != nullptr ? lines->find(key.line) : nullptr;
    if (line == nullptr) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"line", std::string_view{key.line}}};
        core::log(core::LogLevel::Warn, ENG_TR("app.warn.sequence_line_unknown"), args);
        return;
    }
    // As `DialogueService:Say` makes one: a voice with its caption, played,
    // and gone when it has been said.
    const core::InstanceId sound = world.create(world.classes().findId(world.atoms().intern("Sound")));
    const core::InstanceId caption = world.create(world.classes().findId(world.atoms().intern("Caption")));
    scene::SoundComponent* voice = world.sounds().find(sound);
    scene::CaptionComponent* said = world.captions().find(caption);
    if (voice == nullptr || said == nullptr)
        return;
    voice->content = line->sound;
    (void)world.atoms().intern(line->sound);
    voice->category = 2;
    voice->ownsItself = true;
    voice->plays += 1;
    voice->playing = true;
    voice->timePosition = std::max(0.0, into);
    said->text = world.atoms().intern(line->text);
    said->speaker = line->speaker.empty() ? core::NameAtom{} : world.atoms().intern(line->speaker);
    said->color = line->color;
    said->seconds = line->seconds;
    if (world.local(state.player)) {
        world.setLocal(sound);
        world.setLocal(caption);
    }
    (void)world.setParent(caption, sound);
    core::InstanceId from = key.at.empty() ? core::InstanceId{} : resolve(world, state.player, key.at);
    if (!from.valid())
        from = tick.workspace;
    (void)world.setParent(sound, from);
    state.sounds.push_back(sound);
}

void Sequences::showCaption(const Tick& tick, State& state, const SequenceKey& key)
{
    // **A line with no voice**: a sound of no recording with its caption,
    // shown for as long as the key says -- or as long as it takes to read --
    // by whatever shows a spoken line's, and gone after.
    scene::World& world = *tick.world;
    const core::InstanceId sound = world.create(world.classes().findId(world.atoms().intern("Sound")));
    const core::InstanceId caption = world.create(world.classes().findId(world.atoms().intern("Caption")));
    scene::SoundComponent* voice = world.sounds().find(sound);
    scene::CaptionComponent* said = world.captions().find(caption);
    if (voice == nullptr || said == nullptr)
        return;
    voice->category = 2;
    voice->ownsItself = true;
    voice->plays += 1;
    voice->playing = true;
    said->text = world.atoms().intern(key.caption);
    said->speaker = key.speaker.empty() ? core::NameAtom{} : world.atoms().intern(key.speaker);
    said->seconds = key.seconds;
    if (world.local(state.player)) {
        world.setLocal(sound);
        world.setLocal(caption);
    }
    (void)world.setParent(caption, sound);
    (void)world.setParent(sound, tick.workspace);
    state.sounds.push_back(sound);
}

void Sequences::playSound(const Tick& tick, State& state, const SequenceKey& key, f64 into)
{
    scene::World& world = *tick.world;
    const core::InstanceId sound = world.create(world.classes().findId(world.atoms().intern("Sound")));
    scene::SoundComponent* made = world.sounds().find(sound);
    if (made == nullptr)
        return;
    made->content = key.sound;
    (void)world.atoms().intern(key.sound);
    made->category = categoryOf(key.category);
    made->volume = key.volume;
    made->ownsItself = true;
    made->plays += 1;
    made->playing = true;
    made->timePosition = std::max(0.0, into);
    if (world.local(state.player))
        world.setLocal(sound);
    core::InstanceId from = key.at.empty() ? core::InstanceId{} : resolve(world, state.player, key.at);
    if (!from.valid())
        from = tick.workspace;
    (void)world.setParent(sound, from);
    state.sounds.push_back(sound);
}

void Sequences::bring(scene::World& world, core::InstanceId player, const SequenceTrack& track, u32 index,
                      std::vector<Cast>& cast, bool show, core::InstanceId put, bool scrub, bool* warned)
{
    const auto gone = [&](core::InstanceId id) { return !id.valid() || !world.alive(id) || world.destroyed(id); };
    usize slot = cast.size();
    for (usize held = 0; held < cast.size(); ++held) {
        if (cast[held].track == index)
            slot = held;
    }
    // Destroyed by the game in the meantime: made again if wanted again.
    if (slot < cast.size() && gone(cast[slot].root)) {
        if (!gone(cast[slot].binding))
            (void)world.destroy(cast[slot].binding);
        cast.erase(cast.begin() + static_cast<std::ptrdiff_t>(slot));
        slot = cast.size();
    }
    if (slot == cast.size()) {
        if (!show)
            return;
        const std::string path = scene::normalizeStampPath(track.model);
        const std::optional<std::string> text = m_sources.stamp ? m_sources.stamp(path) : std::nullopt;
        const core::InstanceId root = text.has_value()
                                          ? scene::readStamp(world, *text, core::InstanceId{}, path, nullptr,
                                                             m_sources.stamp ? &m_sources.stamp : nullptr)
                                          : core::InstanceId{};
        if (!root.valid()) {
            if (warned != nullptr && !*warned) {
                *warned = true;
                const std::array<core::I18nArg, 2> args{core::I18nArg{"player", world.atoms().text(world.name(player))},
                                                        core::I18nArg{"model", std::string_view{track.model}}};
                core::log(core::LogLevel::Warn, ENG_TR("app.warn.sequence_cast"), args);
            }
            return;
        }
        Cast made;
        made.track = index;
        made.name = world.atoms().intern(track.target);
        made.root = root;
        if (scrub) {
            // In the scene while the scrub lasts and not of it: never saved,
            // and not a row of the tree.
            world.setGenerated(root, true);
        }
        else {
            // **The name the other tracks know it by is a binding**, as one
            // a person made is: an instance under the player, and so every
            // machine's -- a replica's camera looks at who the server brought.
            made.binding = world.create(world.classes().findId(world.atoms().intern("SequenceBinding")));
            world.setName(made.binding, made.name);
            if (world.local(player)) {
                world.setLocal(root, true);
                world.setLocal(made.binding);
            }
            (void)world.setProperty(made.binding, world.atoms().intern("Target"), scene::Value{root});
            (void)world.setParent(made.binding, player);
        }
        cast.push_back(made);
    }
    Cast& held = cast[slot];
    if (held.shown == show)
        return;
    // Not there is out of the world, and kept: it comes back as it left.
    (void)world.setParent(held.root, show ? put : core::InstanceId{});
    held.shown = show;
}

void Sequences::dismiss(scene::World& world, std::vector<Cast>& cast, bool hiddenOnly)
{
    std::erase_if(cast, [&](const Cast& one) {
        if (hiddenOnly && one.shown)
            return false;
        for (const core::InstanceId id : {one.binding, one.root}) {
            if (id.valid() && world.alive(id) && !world.destroyed(id))
                (void)world.destroy(id);
        }
        return true;
    });
}

void Sequences::advance(const Tick& tick, State& state, const File& file, f64 to, bool owner, bool skipped)
{
    scene::World& world = *tick.world;
    const std::vector<SequenceTrack>& tracks = file.sequence.tracks;
    if (state.cursor.size() != tracks.size())
        state.cursor.assign(tracks.size(), 0);

    // **Who is brought comes first**: a track written above the cast track
    // that plays on whoever it brings finds them there the tick they arrive.
    // Skipped over, they are as the last key passed says.
    for (u32 index = 0; index < tracks.size(); ++index) {
        const SequenceTrack& track = tracks[index];
        if (track.type != SequenceTrackType::Cast)
            continue;
        u32& cursor = state.cursor[index];
        const u32 before = cursor;
        for (; cursor < track.keys.size() && static_cast<f64>(track.keys[cursor].time) <= to; ++cursor)
            ++m_stats.keys;
        if (owner && cursor != before)
            bring(world, state.player, track, index, state.cast, track.keys[cursor - 1].show, tick.workspace, false,
                  &state.warnedCast);
    }

    for (u32 index = 0; index < tracks.size(); ++index) {
        const SequenceTrack& track = tracks[index];
        u32& cursor = state.cursor[index];
        ++m_stats.tracks;
        switch (track.type) {
        case SequenceTrackType::Property: {
            // **From its first key to its last**: before the one the property
            // is not the sequence's yet, and after the other it is not any
            // more -- it stays where it was left, and is the game's again.
            if (!owner || cursor != 0 || track.keys.empty() || to < static_cast<f64>(track.keys.front().time))
                break;
            const f64 last = static_cast<f64>(track.keys.back().time);
            if (const core::InstanceId target = resolve(world, state.player, track.target); target.valid())
                writeProperty(world, state.player, &state, track, index, target, std::min(to, last), owner, nullptr);
            else if (!state.warned) {
                state.warned = true;
                const std::array<core::I18nArg, 2> args{
                    core::I18nArg{"player", world.atoms().text(world.name(state.player))},
                    core::I18nArg{"target", std::string_view{track.target}}};
                core::log(core::LogLevel::Warn, ENG_TR("app.warn.sequence_unbound"), args);
            }
            if (to >= last) {
                cursor = 1;
                ++m_stats.keys;
                std::erase_if(state.carried, [&](const Carried& carried) {
                    if (carried.track != index)
                        return false;
                    setDown(world, carried);
                    return true;
                });
            }
            break;
        }
        case SequenceTrackType::Animation:
            for (; cursor < track.keys.size() && static_cast<f64>(track.keys[cursor].time) <= to; ++cursor) {
                const SequenceKey& key = track.keys[cursor];
                ++m_stats.keys;
                if (!key.clip.empty()) {
                    // Skipped over, a clip is one that was never seen.
                    if (!skipped)
                        startClip(tick, state, track, index, cursor, to - static_cast<f64>(key.time));
                    continue;
                }
                // A parameter is set whether it was watched or not: it is
                // where the graph would have been left.
                if (tick.animation == nullptr)
                    continue;
                const core::InstanceId animator = animatorOf(world, resolve(world, state.player, track.target));
                if (!animator.valid())
                    continue;
                const f32 value = key.value.kind == SequenceValue::Kind::Flag ? (key.value.flag ? 1.0f : 0.0f)
                                                                              : static_cast<f32>(key.value.numbers[0]);
                (void)tick.animation->setGraphParameter(animator, key.parameter, value);
            }
            break;
        case SequenceTrackType::Line:
        case SequenceTrackType::Sound:
            for (; cursor < track.keys.size() && static_cast<f64>(track.keys[cursor].time) <= to; ++cursor) {
                ++m_stats.keys;
                if (!owner || skipped)
                    continue;
                const SequenceKey& key = track.keys[cursor];
                if (track.type == SequenceTrackType::Line)
                    sayLine(tick, state, key, 0.0);
                else
                    playSound(tick, state, key, 0.0);
            }
            break;
        case SequenceTrackType::Event: {
            const core::NameAtom reached = world.atoms().intern("EventReached");
            for (; cursor < track.keys.size() && static_cast<f64>(track.keys[cursor].time) <= to; ++cursor) {
                ++m_stats.keys;
                world.changes().pushNote(state.player, reached, noteOf(track.keys[cursor], skipped));
            }
            break;
        }
        case SequenceTrackType::Marker: {
            const core::NameAtom reached = world.atoms().intern("MarkerReached");
            for (; cursor < track.keys.size() && static_cast<f64>(track.keys[cursor].time) <= to; ++cursor) {
                ++m_stats.keys;
                world.changes().pushText(state.player, reached, track.keys[cursor].name);
            }
            break;
        }
        case SequenceTrackType::Hold:
            // Passed: stopping at one is the tick's to do, before it comes
            // here, and a skip does not stop.
            for (; cursor < track.keys.size() && static_cast<f64>(track.keys[cursor].time) <= to; ++cursor)
                ++m_stats.keys;
            break;
        case SequenceTrackType::Caption:
            for (; cursor < track.keys.size() && static_cast<f64>(track.keys[cursor].time) <= to; ++cursor) {
                ++m_stats.keys;
                if (owner && !skipped)
                    showCaption(tick, state, track.keys[cursor]);
            }
            break;
        case SequenceTrackType::Cast:
            // Done, above.
            break;
        case SequenceTrackType::Camera:
        case SequenceTrackType::Morph:
        case SequenceTrackType::Fade:
        case SequenceTrackType::Bars:
            // Picture: the frame's.
            break;
        }
    }

    // A clip that reached its end and was not told to hold goes, by the fade
    // it came in by.
    if (tick.animation != nullptr) {
        for (Clip& clip : state.clips) {
            if (!clip.on || clip.hold)
                continue;
            const scene::TrackState now = tick.animation->state(clip.id);
            if (!now.playing && now.length > 0.0f) {
                tick.animation->stop(clip.id, clip.fade);
                clip.on = false;
            }
        }
    }
}

void Sequences::seek(const Tick& tick, State& state, const File& file, f64 to, bool owner)
{
    scene::World& world = *tick.world;
    const std::vector<SequenceTrack>& tracks = file.sequence.tracks;
    state.cursor.assign(tracks.size(), 0);
    const scene::SequencePlayerComponent* player = world.sequencePlayers().find(state.player);
    const bool running = player != nullptr && player->playing;

    // What was going on is over, at once: a seek is not a moment that passes.
    if (tick.animation != nullptr) {
        for (Clip& clip : state.clips) {
            if (clip.on)
                tick.animation->stop(clip.id, 0.0f);
            clip.on = false;
        }
    }
    if (owner) {
        endSounds(world, state);
        for (const Carried& carried : state.carried)
            setDown(world, carried);
    }
    state.carried.clear();

    // Whoever the sequence brings, as this moment has them: there, not
    // there, or -- before the track's first key -- not made at all. First,
    // as in a tick, so what plays on them finds them.
    for (u32 index = 0; index < tracks.size(); ++index) {
        const SequenceTrack& track = tracks[index];
        if (track.type != SequenceTrackType::Cast)
            continue;
        state.cursor[index] = firstAfter(track.keys, to, false);
        if (!owner)
            continue;
        if (state.cursor[index] == 0) {
            std::vector<Cast> early;
            std::erase_if(state.cast, [&](const Cast& one) {
                if (one.track != index)
                    return false;
                early.push_back(one);
                return true;
            });
            dismiss(world, early, false);
        }
        else {
            bring(world, state.player, track, index, state.cast, track.keys[state.cursor[index] - 1].show,
                  tick.workspace, false, &state.warnedCast);
        }
    }

    for (u32 index = 0; index < tracks.size(); ++index) {
        const SequenceTrack& track = tracks[index];
        u32& cursor = state.cursor[index];
        switch (track.type) {
        case SequenceTrackType::Property: {
            if (!owner || track.keys.empty() || to < static_cast<f64>(track.keys.front().time))
                break;
            const f64 last = static_cast<f64>(track.keys.back().time);
            const core::InstanceId target = resolve(world, state.player, track.target);
            if (target.valid())
                writeProperty(world, state.player, to >= last ? nullptr : &state, track, index, target,
                              std::min(to, last), owner, nullptr);
            if (to >= last)
                cursor = 1;
            break;
        }
        case SequenceTrackType::Animation: {
            cursor = firstAfter(track.keys, to, false);
            // Every parameter as its last key before this moment left it,
            // and the clip this moment falls in, at the frame it falls on.
            core::i64 clip = -1;
            for (u32 key = 0; key < cursor; ++key) {
                const SequenceKey& at = track.keys[key];
                if (!at.clip.empty()) {
                    clip = key;
                    continue;
                }
                if (tick.animation == nullptr)
                    continue;
                const core::InstanceId animator = animatorOf(world, resolve(world, state.player, track.target));
                if (!animator.valid())
                    continue;
                const f32 value = at.value.kind == SequenceValue::Kind::Flag ? (at.value.flag ? 1.0f : 0.0f)
                                                                             : static_cast<f32>(at.value.numbers[0]);
                (void)tick.animation->setGraphParameter(animator, at.parameter, value);
            }
            if (clip >= 0 && tick.animation != nullptr && (state.begun || running)) {
                const u32 key = static_cast<u32>(clip);
                const f64 into = to - static_cast<f64>(track.keys[key].time);
                startClip(tick, state, track, index, key, into);
                // One that would have ended by now, and does not hold, is not there.
                for (Clip& made : state.clips) {
                    if (made.track != index || made.key != key || !made.on || made.hold)
                        continue;
                    const scene::TrackState now = tick.animation->state(made.id);
                    if (now.length > 0.0f &&
                        into * static_cast<f64>(track.keys[key].speed) >= static_cast<f64>(now.length)) {
                        tick.animation->stop(made.id, 0.0f);
                        made.on = false;
                    }
                }
            }
            break;
        }
        case SequenceTrackType::Line: {
            cursor = firstAfter(track.keys, to, false);
            // A line this moment falls in is being said, from where it has
            // got to -- while the sequence runs; held still, nothing speaks.
            if (!owner || !running)
                break;
            for (u32 key = 0; key < cursor; ++key) {
                const SequenceKey& at = track.keys[key];
                const f64 into = to - static_cast<f64>(at.time);
                if (into < static_cast<f64>(lineSlot(world, at.line)))
                    sayLine(tick, state, at, into);
            }
            break;
        }
        case SequenceTrackType::Sound:
            // A sound is not picked up part way: how long one is is the
            // mixer's to know, and music a seek lands in starts at its key.
            cursor = firstAfter(track.keys, to, false);
            break;
        case SequenceTrackType::Event:
        case SequenceTrackType::Marker:
        case SequenceTrackType::Hold:
            // None fires, and nothing is held; one AT this moment is still
            // to come.
            cursor = firstAfter(track.keys, to, true);
            break;
        case SequenceTrackType::Caption:
            // Not picked up part way, as a sound is not.
            cursor = firstAfter(track.keys, to, false);
            break;
        case SequenceTrackType::Cast:
            // Done, above.
            break;
        case SequenceTrackType::Camera:
        case SequenceTrackType::Morph:
        case SequenceTrackType::Fade:
        case SequenceTrackType::Bars:
            break;
        }
    }
}

// --- The tick -------------------------------------------------------------------

void Sequences::step(const Tick& tick)
{
    m_stats = Stats{};
    if (tick.world == nullptr)
        return;
    scene::World& world = *tick.world;
    // Nothing plays a sequence in nearly every game, nearly always.
    if (world.sequencePlayers().size() == 0 && m_states.empty())
        return;
    m_workspace = tick.workspace;
    hearAgain(world);
    for (State& state : m_states)
        state.seen = false;

    const core::NameAtom finished = world.atoms().intern("Finished");
    // Pool order: a pure function of the operations that made the players.
    std::vector<core::InstanceId> players;
    world.sequencePlayers().forEach(
        [&](core::InstanceId id, scene::SequencePlayerComponent&) { players.push_back(id); });

    for (const core::InstanceId id : players) {
        scene::SequencePlayerComponent* player = world.sequencePlayers().find(id);
        if (player == nullptr || world.destroyed(id))
            continue;
        const bool owner = tick.authority || world.local(id);
        bool fresh = false;
        // Nothing asked of it and nothing going on: no state is made for it.
        if (!player->playing && player->seeks == player->seeksDone && player->stops == player->stopsDone &&
            player->skips == player->skipsDone && !player->anchored && stateOf(id) == nullptr) {
            if (player->length == 0.0f && player->sequence.id != 0) {
                if (const File* file = fileFor(world, player->sequence); file != nullptr)
                    player->length = file->length;
            }
            continue;
        }
        State& state = stateFor(id, fresh);
        state.seen = true;
        const File* file = fileFor(world, player->sequence);
        const f64 length = file != nullptr ? static_cast<f64>(file->length) : 0.0;
        if (file != nullptr)
            player->length = file->length;
        if (state.file.id != player->sequence.id) {
            state.file = player->sequence;
            state.cursor.clear();
        }
        bool moved = false;
        const auto finish = [&](bool skipped) {
            player->timePosition = length;
            player->playing = false;
            if (owner)
                player->held = false;
            end(tick, state, file, *player, owner, skipped ? End::Skipped : End::Finished);
            world.changes().push(
                scene::Change{scene::ChangeKind::InstanceEventBool, id, scene::eventFlag(skipped), finished});
            moved = true;
        };

        // **Met already going** (a machine that joined late): what was asked
        // of it before is not asked of this machine, and where it is is read
        // off the anchor, as a seek.
        bool arrived = false;
        if (!owner && fresh) {
            player->stopsDone = player->stops;
            player->skipsDone = player->skips;
            player->seeksDone = player->seeks;
            player->anchored = true;
            arrived = true;
        }

        if (player->stops != player->stopsDone) {
            player->stopsDone = player->stops;
            player->seeksDone = player->seeks;
            // **Heard of a stop, and not of anything after it**: a
            // replica is at nought, whatever the anchor it holds says -- that
            // anchor may be the one the sequence was playing from, sent with
            // the ask before the authority's tick had done the stop, and
            // read as the present it took the replica through the sequence's
            // first moment a second time. Told to play again in the same
            // snapshot, the anchor is the play's, and is read.
            if (!owner && !player->playing)
                player->anchored = false;
            end(tick, state, file, *player, owner, End::Stopped);
            player->timePosition = 0.0;
            player->playing = false;
            if (owner) {
                player->held = false;
                player->skipTo = -1.0f;
            }
            state.cursor.clear();
            moved = true;
        }
        if (player->skips != player->skipsDone) {
            player->skipsDone = player->skips;
            player->seeksDone = player->seeks;
            if (file != nullptr) {
                // Begun, if it never was: what is left held at its end is
                // then held the same way whether or not it was watched.
                if (!state.begun)
                    begin(tick, state, *file);
                advance(tick, state, *file, length, owner, true);
                finish(true);
            }
            player->anchored = false;
            player->skipTo = -1.0f;
        }
        // **Skipped to a marker** (`SkipToNext`), on the machine that
        // decides: the world is left where the sequence would have left it
        // at that moment -- what lies between is passed as a skip passes it
        // -- and it plays on. A replica is told as it is told of a seek.
        if (owner && player->skipTo >= 0.0f) {
            const f64 target = std::min(static_cast<f64>(player->skipTo), length);
            player->skipTo = -1.0f;
            if (file != nullptr && target > player->timePosition) {
                if (!state.begun)
                    begin(tick, state, *file);
                advance(tick, state, *file, target, owner, true);
                player->timePosition = target;
                player->held = false;
                player->playing = true;
                player->seeks += 1;
                player->seeksDone = player->seeks;
                moved = true;
            }
        }
        if (!owner && player->anchored) {
            player->anchored = false;
            f64 now = static_cast<f64>(player->anchorTime);
            if (player->playing && tick.serverTick > player->anchorTick)
                now += static_cast<f64>(tick.serverTick - player->anchorTick) * tick.fixedDt *
                       static_cast<f64>(player->speed);
            if (length > 0.0)
                now = player->looped && player->playing ? std::fmod(now, length) : std::min(now, length);
            if (arrived) {
                player->seeksDone = player->seeks;
                player->timePosition = now;
                if (file != nullptr)
                    seek(tick, state, *file, now, owner);
            }
            else if (player->seeks != player->seeksDone) {
                // **Sought on the authority, and run on since**: put at the
                // place it was sought to -- the anchor is where it was a
                // tick after that, if it was running -- and then brought to
                // now, so an event AT that place is heard here as it was
                // there. A sequence played again from nought says its first
                // event again.
                player->seeksDone = player->seeks;
                const f64 ran = player->playing ? tick.fixedDt * static_cast<f64>(player->speed) : 0.0;
                const f64 sought = std::max(0.0, static_cast<f64>(player->anchorTime) - ran - 1.0e-4);
                if (file != nullptr) {
                    seek(tick, state, *file, std::min(sought, now), owner);
                    if (now > sought) {
                        if (player->playing && !state.begun)
                            begin(tick, state, *file);
                        advance(tick, state, *file, now, owner, false);
                    }
                }
                player->timePosition = now;
            }
            else {
                // Time that passed on the authority and is heard of here.
                if (now > player->timePosition && file != nullptr)
                    advance(tick, state, *file, now, owner, false);
                player->timePosition = now;
                // **Over, on the authority's word**: a replica that
                // reached the end waits there to be told, and one that had
                // not is brought to it first.
                if (!player->playing && length > 0.0 && now >= length && state.begun && file != nullptr)
                    finish(false);
            }
        }
        else if (player->seeks != player->seeksDone) {
            player->seeksDone = player->seeks;
            if (length > 0.0)
                player->timePosition = std::min(player->timePosition, length);
            if (file != nullptr)
                seek(tick, state, *file, player->timePosition, owner);
            // Sought while it waited at a hold and not set going: it waits
            // at no hold now.
            if (owner && !player->playing)
                player->held = false;
            moved = true;
        }

        if (player->playing) {
            if (file == nullptr) {
                // Nothing to play: said once where the file was looked for.
                player->playing = false;
                moved = true;
            }
            else {
                if (!state.begun)
                    begin(tick, state, *file);
                if (!state.wasPlaying || state.speed != player->speed)
                    moved = true;
                f64 to = player->timePosition + tick.fixedDt * static_cast<f64>(player->speed);
                // **A replica's time is the authority's**: where the
                // anchor says the sequence is at the authority's tick as this
                // machine knows it, and not this machine's own ticks counted
                // from the anchor. Counted by its own, a replica of an
                // authority that ran slow -- a loaded server, a hitch -- ran
                // ahead of it without bound: it reached the end, said `End`
                // and `Finished`, and stopped, while the authority was a third
                // of the way through and about to be skipped. Never back: the
                // clock it is read off is an estimate, put right a tick at a
                // time, and what was said is not unsaid. A looped one runs on
                // by its own ticks, and is anchored again at each turn.
                const bool follows = !owner && !player->looped;
                if (follows) {
                    const f64 ran = tick.serverTick > player->anchorTick
                                        ? static_cast<f64>(tick.serverTick - player->anchorTick) * tick.fixedDt *
                                              static_cast<f64>(player->speed)
                                        : 0.0;
                    to = std::max(player->timePosition, static_cast<f64>(player->anchorTime) + ran);
                }
                // **The next hold, if this tick reaches it** (a `hold` track's
                // key), on the machine that decides: time goes as far as the
                // key -- what is AT it is done -- and stands there until it
                // is released. Everywhere else it stands where the anchor
                // puts it, which is the same place.
                f64 hold = -1.0;
                if (owner) {
                    const std::vector<SequenceTrack>& tracks = file->sequence.tracks;
                    if (state.cursor.size() != tracks.size())
                        state.cursor.assign(tracks.size(), 0);
                    for (u32 index = 0; index < tracks.size(); ++index) {
                        if (tracks[index].type != SequenceTrackType::Hold ||
                            state.cursor[index] >= tracks[index].keys.size())
                            continue;
                        const f64 at = static_cast<f64>(tracks[index].keys[state.cursor[index]].time);
                        if (at <= to && at <= length && (hold < 0.0 || at < hold))
                            hold = at;
                    }
                }
                if (hold >= 0.0) {
                    advance(tick, state, *file, hold, owner, false);
                    player->timePosition = hold;
                    player->playing = false;
                    player->held = true;
                    moved = true;
                }
                else if (to < length) {
                    advance(tick, state, *file, to, owner, false);
                    player->timePosition = to;
                }
                else if (follows) {
                    // At its end, and not over until the authority says so:
                    // it may yet be skipped, stopped or sought there, and a
                    // `Finished` said here first would be said twice.
                    advance(tick, state, *file, length, owner, false);
                    player->timePosition = length;
                }
                else {
                    advance(tick, state, *file, length, owner, false);
                    if (player->looped && length > 0.0) {
                        // Wrapped, and what it overshot by is kept.
                        const f64 over = std::fmod(to - length, length);
                        player->timePosition = 0.0;
                        seek(tick, state, *file, 0.0, owner);
                        advance(tick, state, *file, over, owner, false);
                        player->timePosition = over;
                        moved = true;
                    }
                    else {
                        finish(false);
                    }
                }
            }
        }
        // **Held by its file, it says so**: on the machine that decided it
        // and on one that was told.
        if (player->held && !state.wasHeld && file != nullptr) {
            std::string name;
            for (const SequenceTrack& track : file->sequence.tracks) {
                if (track.type != SequenceTrackType::Hold)
                    continue;
                for (const SequenceKey& key : track.keys) {
                    if (std::abs(static_cast<f64>(key.time) - player->timePosition) < 1.0e-3)
                        name = key.name;
                }
            }
            world.changes().pushText(id, world.atoms().intern("HoldReached"), std::move(name));
        }
        state.wasHeld = player->held;
        // Held still, its clips are; running again, they run. **A hold of the
        // file's is not a pause**: what the sequence began goes on -- a
        // looped idle loops while a choice is made -- and nothing new begins.
        if (state.wasPlaying != player->playing)
            moved = true;
        const bool running = player->playing || player->held;
        if (state.wasRunning != running) {
            if (tick.animation != nullptr && file != nullptr) {
                for (const Clip& clip : state.clips) {
                    if (!clip.on || clip.track >= file->sequence.tracks.size())
                        continue;
                    const std::vector<SequenceKey>& keys = file->sequence.tracks[clip.track].keys;
                    const f32 speed = clip.key < keys.size() ? keys[clip.key].speed : 1.0f;
                    tick.animation->adjustSpeed(clip.id, running ? speed : 0.0f);
                }
            }
        }
        state.wasRunning = running;
        state.wasPlaying = player->playing;
        state.speed = player->speed;
        if (owner && moved)
            anchor(tick, *player);
        if (player->playing)
            ++m_stats.playing;

        // The sounds that have gone by themselves are let go of.
        std::erase_if(state.sounds,
                      [&](core::InstanceId sound) { return !world.alive(sound) || world.destroyed(sound); });
    }

    // **A player that is gone**: what it began is ended as a stop ends it.
    for (State& state : m_states) {
        if (state.seen)
            continue;
        if (tick.animation != nullptr) {
            for (const Clip& clip : state.clips) {
                if (clip.on)
                    tick.animation->stop(clip.id, clip.fade);
            }
        }
        for (const Carried& carried : state.carried)
            setDown(world, carried);
        endSounds(world, state);
        dismiss(world, state.cast, false);
        // And the sequence a script wrote for it is nobody's now.
        const core::u32 played = state.file.id;
        std::erase_if(m_files, [&](const File& file) { return file.made && file.urn.id == played; });
    }
    std::erase_if(m_states, [](const State& state) { return !state.seen; });
}

// --- The frame ------------------------------------------------------------------

std::optional<Sequences::Camera> Sequences::camera(const scene::World& world, f64 ahead, const PlaceOf& placeOf,
                                                   const FrameOf& frameOf, const Sight& sight) const
{
    // The one being scrubbed, or the one that began last.
    core::InstanceId chosen;
    f64 at = 0.0;
    if (m_preview.player.valid() && world.alive(m_preview.player)) {
        if (!m_previewLooks)
            return std::nullopt;
        chosen = m_preview.player;
        at = m_preview.seconds;
    }
    else {
        core::u64 latest = 0;
        for (const State& state : m_states) {
            const scene::SequencePlayerComponent* player = world.sequencePlayers().find(state.player);
            if (!state.begun || player == nullptr || !player->takesCamera || state.order <= latest)
                continue;
            latest = state.order;
            chosen = state.player;
            at = player->timePosition + (player->playing ? ahead * static_cast<f64>(player->speed) : 0.0);
        }
    }
    const scene::SequencePlayerComponent* player = chosen.valid() ? world.sequencePlayers().find(chosen) : nullptr;
    const File* file = player != nullptr ? fileOf(player->sequence) : nullptr;
    if (file == nullptr)
        return std::nullopt;
    const SequenceTrack* track = nullptr;
    for (usize index = 0; index < file->sequence.tracks.size(); ++index) {
        const SequenceTrack& one = pictured(*file, index);
        if (one.type == SequenceTrackType::Camera && !one.keys.empty()) {
            track = &one;
            break;
        }
    }
    const f64 moment = std::min(at, static_cast<f64>(file->length));
    SequenceSpan span;
    if (track == nullptr || !asset::sequenceSpanAt(track->keys, static_cast<f32>(moment), span))
        return std::nullopt;

    const SequenceKey& from = track->keys[span.from];
    const SequenceKey& to = track->keys[span.to];
    const f64 alpha = static_cast<f64>(span.alpha);
    // **A key in somebody's frame** (`relativeTo`): where that instance
    // stands and faces THIS frame -- a scene set wherever the character
    // happens to be, behind its shoulder whichever way it turned. Each key's
    // place is worked out in the world first and the two are mixed there, so
    // a move from one character's shoulder to another's is a move.
    const auto frame = [&](const SequenceKey& key) -> std::optional<core::CFrameD> {
        if (key.relativeTo.empty())
            return std::nullopt;
        const core::InstanceId held = resolve(world, chosen, key.relativeTo);
        if (!held.valid())
            return std::nullopt;
        if (frameOf)
            return frameOf(held);
        if (const scene::AttachmentComponent* point = world.attachments().find(held); point != nullptr)
            return point->worldCFrame;
        return scene::pivotOf(world, held);
    };
    const std::optional<core::CFrameD> fromFrame = frame(from);
    const std::optional<core::CFrameD> toFrame = span.from == span.to ? fromFrame : frame(to);
    const auto placed = [](const std::optional<core::CFrameD>& in, const f64(&local)[3]) {
        const core::DVec3 place{local[0], local[1], local[2]};
        return in.has_value() ? core::transformPoint(*in, place) : place;
    };
    const core::DVec3 fromEye = placed(fromFrame, from.position);
    const core::DVec3 toEye = placed(toFrame, to.position);
    core::DVec3 eye{fromEye.x + (toEye.x - fromEye.x) * alpha, fromEye.y + (toEye.y - fromEye.y) * alpha,
                    fromEye.z + (toEye.z - fromEye.z) * alpha};
    // **One smooth curve through its keys** (`"path": "curve"`): each key is
    // passed through at the speed and heading that take the camera from the
    // key before it to the key after, so a move through four places is one
    // move and not three with a corner at each. A cut begins another curve,
    // and the first and last key of one are left and arrived at straight.
    if (track->curve && span.from != span.to) {
        const auto eyeAt = [&](usize index) {
            const SequenceKey& key = track->keys[index];
            return placed(frame(key), key.position);
        };
        const f64 whole = static_cast<f64>(to.time) - static_cast<f64>(from.time);
        const auto rate = [&](const core::DVec3& a, f64 aTime, const core::DVec3& b, f64 bTime) {
            const f64 over = bTime - aTime;
            return over > 1.0e-6
                       ? core::DVec3{(b.x - a.x) / over * whole, (b.y - a.y) / over * whole, (b.z - a.z) / over * whole}
                       : core::DVec3{};
        };
        const bool before = span.from > 0 && !from.cut;
        const bool after = span.to + 1 < track->keys.size() && !track->keys[span.to + 1].cut;
        const core::DVec3 leaving = before
                                        ? rate(eyeAt(span.from - 1), static_cast<f64>(track->keys[span.from - 1].time),
                                               toEye, static_cast<f64>(to.time))
                                        : rate(fromEye, static_cast<f64>(from.time), toEye, static_cast<f64>(to.time));
        const core::DVec3 arriving = after
                                         ? rate(fromEye, static_cast<f64>(from.time), eyeAt(span.to + 1),
                                                static_cast<f64>(track->keys[span.to + 1].time))
                                         : rate(fromEye, static_cast<f64>(from.time), toEye, static_cast<f64>(to.time));
        const f64 s = alpha;
        const f64 s2 = s * s;
        const f64 s3 = s2 * s;
        const f64 h00 = 2.0 * s3 - 3.0 * s2 + 1.0;
        const f64 h10 = s3 - 2.0 * s2 + s;
        const f64 h01 = -2.0 * s3 + 3.0 * s2;
        const f64 h11 = s3 - s2;
        eye = core::DVec3{h00 * fromEye.x + h10 * leaving.x + h01 * toEye.x + h11 * arriving.x,
                          h00 * fromEye.y + h10 * leaving.y + h01 * toEye.y + h11 * arriving.y,
                          h00 * fromEye.z + h10 * leaving.z + h01 * toEye.z + h11 * arriving.z};
    }
    // Where what a key looks at is, this frame.
    const auto aim = [&](const SequenceKey& key, const std::optional<core::CFrameD>& in) -> std::optional<core::DVec3> {
        if (!key.lookAtBinding.empty()) {
            if (const core::InstanceId looked = resolve(world, chosen, key.lookAtBinding); looked.valid()) {
                const scene::AttachmentComponent* point = world.attachments().find(looked);
                return placeOf            ? placeOf(looked)
                       : point != nullptr ? point->worldCFrame.position
                                          : scene::pivotOf(world, looked).position;
            }
            return std::nullopt;
        }
        if (key.hasLookAt)
            return placed(in, key.lookAt);
        return std::nullopt;
    };
    // **Kept in sight** (`keepInSight`): where something that does not move
    // stands between the camera and who it looks at, the camera is brought
    // towards them along its own line of sight until it is this side of it
    // -- a wall behind a shoulder, a pillar a character walked behind. One
    // ray a frame; two for the length of a move between keys that look at
    // different things.
    if (sight) {
        const auto near = [&](const SequenceKey& key,
                              const std::optional<core::CFrameD>& in) -> std::optional<core::DVec3> {
            if (!key.keepInSight)
                return std::nullopt;
            const std::optional<core::DVec3> target = aim(key, in);
            if (!target.has_value())
                return std::nullopt;
            const core::DVec3 away{eye.x - target->x, eye.y - target->y, eye.z - target->z};
            const f64 whole = std::sqrt(away.x * away.x + away.y * away.y + away.z * away.z);
            const std::optional<f64> met =
                whole > 1.0e-3 ? sight(*target, eye, resolve(world, chosen, key.lookAtBinding)) : std::nullopt;
            if (!met.has_value())
                return std::nullopt;
            // A hand's breadth this side of what is in the way.
            const f64 kept = std::max(0.0, *met - 0.2) / whole;
            return core::DVec3{target->x + away.x * kept, target->y + away.y * kept, target->z + away.z * kept};
        };
        const std::optional<core::DVec3> first = near(from, fromFrame);
        const bool same =
            span.from == span.to || (from.keepInSight && to.keepInSight && from.lookAtBinding == to.lookAtBinding);
        const std::optional<core::DVec3> second = same ? first : near(to, toFrame);
        const f64 fromWeight = span.from == span.to ? 1.0 : 1.0 - alpha;
        const f64 toWeight = span.from == span.to ? 0.0 : alpha;
        const core::DVec3 was = eye;
        if (first.has_value()) {
            eye.x += (first->x - was.x) * fromWeight;
            eye.y += (first->y - was.y) * fromWeight;
            eye.z += (first->z - was.z) * fromWeight;
        }
        if (second.has_value()) {
            eye.x += (second->x - was.x) * toWeight;
            eye.y += (second->y - was.y) * toWeight;
            eye.z += (second->z - was.z) * toWeight;
        }
    }
    // **Each key's turn from where the camera is NOW**: a key that looks at
    // something looks at it from here, so a camera that moves while it looks
    // at a character keeps the character in the middle of the picture.
    const auto turn = [&](const SequenceKey& key, const std::optional<core::CFrameD>& in) -> core::Mat3 {
        const std::optional<core::DVec3> target = aim(key, in);
        if (target.has_value()) {
            const f64 dx = target->x - eye.x;
            const f64 dy = target->y - eye.y;
            const f64 dz = target->z - eye.z;
            if (dx * dx + dy * dy + dz * dz > 1.0e-8)
                return core::lookAtCFrame(eye, *target, core::Vec3{0.0f, 1.0f, 0.0f}).rotation;
        }
        const core::Mat3 own = key.hasRotation ? turnOf(key.rotation) : core::Mat3{};
        // A turn in somebody's frame is a turn from the way they face.
        return in.has_value() ? (*in * core::CFrameD{.position = core::DVec3{}, .rotation = own}).rotation : own;
    };
    // A key that says no field of view keeps the one before it.
    const auto lens = [&](usize index) -> f32 {
        for (usize back = index + 1; back-- > 0;) {
            if (track->keys[back].fieldOfView > 0.0f)
                return track->keys[back].fieldOfView;
        }
        return 0.0f;
    };

    Camera out;
    out.player = chosen;
    out.time = moment;
    out.cframe.position = eye;
    out.cframe.rotation =
        span.from == span.to ? turn(to, toFrame) : core::slerp(turn(from, fromFrame), turn(to, toFrame), span.alpha);
    // **A hand that is not steady** (`shake`): moved across and up by as much
    // as the key says, in metres, and turned by the angle that much is seen
    // under from a metre away -- which is what is seen of a shake when what
    // is looked at is far off. **A function of the sequence's time**: the
    // same picture on every machine, in every run, and under a scrub. How
    // fast it goes is counted along the track -- the rate going evenly from
    // each key's to the next's -- so a shake that quickens does not jump.
    const f32 shaken = from.shake[0] + (to.shake[0] - from.shake[0]) * span.alpha;
    if (shaken > 0.0f) {
        f64 turns = 0.0;
        const std::vector<SequenceKey>& keys = track->keys;
        for (usize index = 0; index + 1 < keys.size(); ++index) {
            const f64 begins = static_cast<f64>(keys[index].time);
            const f64 ends = static_cast<f64>(keys[index + 1].time);
            if (moment <= begins)
                break;
            if (!(ends > begins))
                continue;
            const f64 slow = static_cast<f64>(keys[index].shake[1]);
            const f64 quick = static_cast<f64>(keys[index + 1].shake[1]);
            const f64 into = std::min(moment, ends) - begins;
            turns += slow * into + (quick - slow) * into * into / (2.0 * (ends - begins));
        }
        if (moment > static_cast<f64>(keys.back().time))
            turns += static_cast<f64>(keys.back().shake[1]) * (moment - static_cast<f64>(keys.back().time));
        const f64 far = static_cast<f64>(shaken);
        core::CFrameD hand;
        hand.position = core::DVec3{wander(turns, 0) * far, wander(turns, 1) * far, 0.0};
        hand.rotation = core::fromEulerYxz(
            core::Vec3{static_cast<f32>(wander(turns, 2) * far), static_cast<f32>(wander(turns, 3) * far), 0.0f});
        out.cframe = out.cframe * hand;
    }
    const f32 wide = lens(span.from);
    const f32 narrow = lens(span.to);
    out.fieldOfView = wide > 0.0f && narrow > 0.0f ? wide + (narrow - wide) * span.alpha : std::max(wide, narrow);
    return out;
}

Sequences::Screen Sequences::screen(const scene::World& world, f64 ahead) const
{
    Screen out;
    const auto read = [&](core::InstanceId id, f64 at) {
        const scene::SequencePlayerComponent* player = world.sequencePlayers().find(id);
        const File* file = player != nullptr ? fileOf(player->sequence) : nullptr;
        if (file == nullptr)
            return;
        for (usize index = 0; index < file->sequence.tracks.size(); ++index) {
            const SequenceTrack& track = pictured(*file, index);
            SequenceValue value;
            if ((track.type != SequenceTrackType::Fade && track.type != SequenceTrackType::Bars) ||
                !valueAt(world, id, track, at, value))
                continue;
            const f32 amount = std::clamp(static_cast<f32>(value.numbers[0]), 0.0f, 1.0f);
            // Of two sequences that both cover the picture, the one that
            // covers more of it is the one seen.
            if (track.type == SequenceTrackType::Fade && amount > out.fade) {
                out.fade = amount;
                out.fadeColor = core::Color3{track.color[0], track.color[1], track.color[2]};
            }
            else if (track.type == SequenceTrackType::Bars && amount > out.bars) {
                out.bars = amount;
                out.aspect = track.aspect;
            }
        }
    };
    if (m_preview.player.valid() && world.alive(m_preview.player))
        read(m_preview.player, m_preview.seconds);
    for (const State& state : m_states) {
        const scene::SequencePlayerComponent* player = world.sequencePlayers().find(state.player);
        if (!state.begun || player == nullptr || state.player == m_preview.player)
            continue;
        read(state.player,
             std::min(player->timePosition + (player->playing ? ahead * static_cast<f64>(player->speed) : 0.0),
                      static_cast<f64>(player->length)));
    }
    return out;
}

void Sequences::present(const scene::World& world, render::AnimationSystem& animation, f64 ahead)
{
    const auto draw = [&](core::InstanceId id, f64 at) {
        const scene::SequencePlayerComponent* player = world.sequencePlayers().find(id);
        const File* file = player != nullptr ? fileOf(player->sequence) : nullptr;
        if (file == nullptr)
            return;
        for (usize index = 0; index < file->sequence.tracks.size(); ++index) {
            const SequenceTrack& track = pictured(*file, index);
            SequenceValue value;
            if (track.type != SequenceTrackType::Morph || !valueAt(world, id, track, at, value))
                continue;
            const core::InstanceId mesh = resolve(world, id, track.target);
            if (!mesh.valid())
                continue;
            m_weights.clear();
            m_weights.emplace_back(track.property, std::clamp(static_cast<f32>(value.numbers[0]), 0.0f, 1.0f));
            animation.presentMorphWeights(mesh, m_weights);
        }
    };
    if (m_preview.player.valid() && world.alive(m_preview.player))
        draw(m_preview.player, m_preview.seconds);
    for (const State& state : m_states) {
        const scene::SequencePlayerComponent* player = world.sequencePlayers().find(state.player);
        if (!state.begun || player == nullptr || state.player == m_preview.player)
            continue;
        draw(state.player,
             std::min(player->timePosition + (player->playing ? ahead * static_cast<f64>(player->speed) : 0.0),
                      static_cast<f64>(player->length)));
    }
}

bool Sequences::suspendsControls(const scene::World& world) const noexcept
{
    for (const State& state : m_states) {
        const scene::SequencePlayerComponent* player = world.sequencePlayers().find(state.player);
        if (state.begun && player != nullptr && player->controls == 1)
            return true;
    }
    return false;
}

// --- The editor's scrub ---------------------------------------------------------

void Sequences::preview(scene::World& world, core::InstanceId player, f64 seconds)
{
    const scene::SequencePlayerComponent* self = world.sequencePlayers().find(player);
    const File* file = self != nullptr ? fileFor(world, self->sequence) : nullptr;
    if (file == nullptr) {
        endPreview(world);
        return;
    }
    // What the last moment wrote is put back first: a track this moment is
    // before the first key of must not be left where a later one had it.
    for (auto kept = m_preview.kept.rbegin(); kept != m_preview.kept.rend(); ++kept) {
        if (world.alive(kept->instance) && !world.destroyed(kept->instance))
            (void)world.setProperty(kept->instance, kept->property, kept->value);
    }
    m_preview.kept.clear();
    hearAgain(world);
    if (m_preview.player != player)
        dismiss(world, m_preview.cast, false);
    m_preview.player = player;
    m_preview.seconds = std::clamp(seconds, 0.0, static_cast<f64>(file->length));
    // **Whoever the sequence brings is there under the scrub**, between the
    // keys that have them there: made for the scrub, and gone with it.
    for (u32 index = 0; index < file->sequence.tracks.size(); ++index) {
        const SequenceTrack& track = file->sequence.tracks[index];
        if (track.type != SequenceTrackType::Cast)
            continue;
        const u32 reached = firstAfter(track.keys, m_preview.seconds, false);
        const bool there = reached != 0 && track.keys[reached - 1].show;
        if (there) {
            const core::InstanceId put =
                m_workspace.valid() && world.alive(m_workspace) ? m_workspace : workspaceOver(world, player);
            bring(world, player, track, index, m_preview.cast, true, put, true, nullptr);
        }
        else {
            // Not kept out of the world, as a playing sequence keeps one:
            // nothing of a scrub is left lying in the scene.
            std::vector<Cast> away;
            std::erase_if(m_preview.cast, [&](const Cast& one) {
                if (one.track != index)
                    return false;
                away.push_back(one);
                return true;
            });
            dismiss(world, away, false);
        }
    }
    for (u32 index = 0; index < file->sequence.tracks.size(); ++index) {
        const SequenceTrack& track = file->sequence.tracks[index];
        if (track.type != SequenceTrackType::Property || track.keys.empty())
            continue;
        if (const core::InstanceId target = resolve(world, player, track.target); target.valid())
            writeProperty(world, player, nullptr, track, index, target,
                          std::min(m_preview.seconds, static_cast<f64>(track.keys.back().time)), true, &m_preview);
    }
    posePreview(world, *file);
}

void Sequences::posePreview(scene::World& world, const File& file)
{
    if (m_poser == nullptr)
        return;
    if (m_sources.prepareRigs)
        m_sources.prepareRigs();
    const f64 at = m_preview.seconds;
    m_preview.unsettled = false;
    // What the last moment set is handed back first: a parameter this moment
    // is before the key of must not stay as a later one had it.
    for (const SetParameter& set : m_preview.parameters)
        (void)m_poser->clearGraphParameter(set.animator, set.name);
    m_preview.parameters.clear();
    std::vector<bool> wanted(m_preview.clips.size(), false);

    for (u32 index = 0; index < file.sequence.tracks.size(); ++index) {
        const SequenceTrack& track = file.sequence.tracks[index];
        if (track.type != SequenceTrackType::Animation || track.keys.empty())
            continue;
        const core::InstanceId animator = animatorOf(world, resolve(world, m_preview.player, track.target));
        if (!animator.valid())
            continue;
        core::i64 clip = -1;
        for (u32 key = 0; key < track.keys.size() && static_cast<f64>(track.keys[key].time) <= at; ++key) {
            const SequenceKey& reached = track.keys[key];
            if (!reached.clip.empty()) {
                clip = key;
                continue;
            }
            const f32 value = reached.value.kind == SequenceValue::Kind::Flag
                                  ? (reached.value.flag ? 1.0f : 0.0f)
                                  : static_cast<f32>(reached.value.numbers[0]);
            (void)m_poser->setGraphParameter(animator, reached.parameter, value);
            m_preview.parameters.push_back(SetParameter{animator, reached.parameter});
        }
        if (clip < 0)
            continue;
        const u32 key = static_cast<u32>(clip);
        const SequenceKey& playing = track.keys[key];
        usize slot = m_preview.clips.size();
        for (usize held = 0; held < m_preview.clips.size(); ++held) {
            const Clip& one = m_preview.clips[held];
            if (one.track == index && one.key == key && one.animator == animator)
                slot = held;
        }
        if (slot == m_preview.clips.size()) {
            std::string_view name = playing.clip;
            std::string_view path;
            if (const usize hash = name.rfind('#'); hash != std::string_view::npos) {
                path = name.substr(0, hash);
                name = name.substr(hash + 1);
            }
            if (name.empty())
                continue;
            Clip made;
            made.track = index;
            made.key = key;
            made.animator = animator;
            made.id =
                m_poser->createTrack(animator, path.empty() ? core::NameAtom{} : world.atoms().intern(path), name);
            m_poser->setOver(made.id, true);
            m_preview.clips.push_back(made);
            wanted.push_back(false);
        }
        Clip& held = m_preview.clips[slot];
        const f64 since = at - static_cast<f64>(playing.time);
        const f64 into = since * static_cast<f64>(playing.speed);
        const scene::TrackState known = m_poser->state(held.id);
        // Its length is not known until its file has been read: asked again.
        if (known.length <= 0.0f)
            m_preview.unsettled = true;
        // Run out, and not held: it is not there at this moment.
        if (known.length > 0.0f && into >= static_cast<f64>(known.length) && !playing.hold)
            continue;
        // As far faded in as it would be, at the frame the moment falls on,
        // and still: nothing is playing.
        const f32 weight = playing.fade > 0.0f ? std::clamp(static_cast<f32>(since) / playing.fade, 0.0f, 1.0f) : 1.0f;
        m_poser->play(held.id, 0.0f, weight, playing.speed);
        m_poser->seek(held.id, into);
        m_poser->adjustSpeed(held.id, 0.0f);
        held.on = true;
        wanted[slot] = true;
    }
    for (usize slot = 0; slot < m_preview.clips.size(); ++slot) {
        if (m_preview.clips[slot].on && !wanted[slot]) {
            m_poser->stop(m_preview.clips[slot].id, 0.0f);
            m_preview.clips[slot].on = false;
        }
    }
    // The poses made, with no time passing: a millionth of a second, because
    // a graph reads a body's speed as how far it went over how long.
    m_poser->sample(1.0e-6);
    m_preview.posed = true;
}

void Sequences::endPreview(scene::World& world)
{
    for (auto kept = m_preview.kept.rbegin(); kept != m_preview.kept.rend(); ++kept) {
        if (world.alive(kept->instance) && !world.destroyed(kept->instance))
            (void)world.setProperty(kept->instance, kept->property, kept->value);
    }
    dismiss(world, m_preview.cast, false);
    // And every body as its file rests it: the pose is put back as a
    // property is.
    if (m_poser != nullptr && m_preview.posed) {
        for (const SetParameter& set : m_preview.parameters)
            (void)m_poser->clearGraphParameter(set.animator, set.name);
        m_poser->rest();
    }
    m_preview = Preview{};
}

} // namespace engine::app
