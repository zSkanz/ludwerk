#include "engine/replication/session.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include "engine/asset/terrain_cell.h"
#include "engine/core/crypto.h"
#include "engine/core/finite.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/profile.h"
#include "engine/jobs/jobs.h"
#include "engine/replication/script_templates.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/phases.h"
#include "engine/scene/players.h"
#include "engine/scene/swarm.h"
#include "engine/scene/voxel_fluid.h"
#include "engine/scene/world.h"
#include "wire_schema.gen.h"

namespace engine::replication {
namespace {

// **Where an instance is, for interest**: a part's position, or a sprite's on
// the 2D plane (ADR 0088). Anything else has no place of its own and is sent
// or not with what it hangs from.
[[nodiscard]] std::optional<core::DVec3> placeOf(const scene::World& world, core::InstanceId id)
{
    if (const scene::PartComponent* part = world.parts().find(id); part != nullptr)
        return part->cframe.position;
    if (const scene::Part2DComponent* sprite = world.parts2d().find(id); sprite != nullptr)
        return core::DVec3{static_cast<double>(sprite->position.x), static_cast<double>(sprite->position.y), 0.0};
    return std::nullopt;
}

// **How far a thing is from where a player looks from, to its nearest
// edge**, squared -- not to its centre. A 4 km floor centred on the origin is
// under a player standing on it 1 km out, and measured from its centre it was
// never sent: the player stood on nothing and was corrected every tick. A
// part is its box, oriented as it is; a sprite is the circle round its
// rectangle. Nothing else has a place to be far from.
[[nodiscard]] std::optional<double> reachSquared(const scene::World& world, core::InstanceId id,
                                                 const core::DVec3& from)
{
    if (const scene::PartComponent* part = world.parts().find(id); part != nullptr) {
        const core::DVec3 delta = from - part->cframe.position;
        const core::Mat3& axes = part->cframe.rotation;
        const double half[3] = {0.5 * static_cast<double>(part->size.x), 0.5 * static_cast<double>(part->size.y),
                                0.5 * static_cast<double>(part->size.z)};
        double squared = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
            // Along the part's own axis, past its face or nothing.
            const double along = static_cast<double>(axes.m[axis][0]) * delta.x +
                                 static_cast<double>(axes.m[axis][1]) * delta.y +
                                 static_cast<double>(axes.m[axis][2]) * delta.z;
            const double beyond = std::max(std::abs(along) - half[axis], 0.0);
            squared += beyond * beyond;
        }
        return squared;
    }
    if (const scene::Part2DComponent* sprite = world.parts2d().find(id); sprite != nullptr) {
        const double dx = from.x - static_cast<double>(sprite->position.x);
        const double dy = from.y - static_cast<double>(sprite->position.y);
        const double dz = from.z;
        const double reach = 0.5 * std::hypot(static_cast<double>(sprite->size.x), static_cast<double>(sprite->size.y));
        const double beyond = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - reach, 0.0);
        return beyond * beyond;
    }
    return std::nullopt;
}

// A guest at another seat of the same machine (ADR 0205): its machine's key
// and its seat, hashed, so the same seat is the same player again.
[[nodiscard]] std::string guestKeyOf(const std::array<u8, 32>& key, u8 seat)
{
    std::array<u8, 33> joined{};
    std::copy(key.begin(), key.end(), joined.begin());
    joined[32] = seat;
    const core::Blake2b hashed = core::blake2b(joined);
    return core::toHex(std::span<const u8>(hashed.data(), 32));
}

// **A token nobody can guess** (ADR 0085), from the operating system's
// entropy. Not simulation state -- it never reaches the world or its hash --
// so this is not the unseeded randomness R10 forbids; the `UserId` it maps to
// is assigned in welcome order, as it always was.
[[nodiscard]] PlayerToken freshToken()
{
    std::random_device device;
    const auto word = [&device] { return (static_cast<u64>(device()) << 32) | static_cast<u64>(device()); };
    PlayerToken token;
    while (!token.valid()) {
        token.high = word();
        token.low = word();
    }
    return token;
}

using core::f32;
using core::f64;
using core::i32;
using core::i64;
using core::InstanceId;
using generated::MessageType;

// Channels by the schema's numbering. Named here once so a send cannot pick one
// by a bare digit.
constexpr u8 ControlChannel = 0;
constexpr u8 StateChannel = 1;
constexpr u8 IntentChannel = 2;
constexpr u8 OwnershipChannel = 3;
constexpr u8 RemoteChannel = 4;
constexpr u8 SwarmChannel = 5;

// A snapshot record whose fields are the whole set rather than a diff.
constexpr u8 FullRecord = 1;
// **A record of a part that waited for its turn** (ADR 0229, protocol 55):
// the replica draws it further in the past, between the states it was sent.
constexpr u8 HeldBackRecord = 2;
// The furthest further back a part that waits is drawn: a second, the
// longest a changed part waits while its budget can carry it.
constexpr f64 MaxPartLagTicks = 60.0;
// **A player's budget** (ADR 0229): half a second of it may be spent at
// once; a part's turn comes at full rate within this many metres of the
// peer's focus, falling to this fraction at the edge of the load radius;
// and one that has waited three quarters of a second goes before any that has
// not -- so that, while the budget can carry each changed part once a second,
// none waits longer.
constexpr f64 BudgetBurstSeconds = 0.5;
constexpr f64 TicksPerSecond = 60.0;
constexpr f64 BudgetNearMetres = 15.0;
// What is this close to one of a peer's own characters is sent with it,
// whatever the budget: what the character can touch next.
constexpr f64 BudgetReachMetres = 8.0;
constexpr f64 BudgetFarRate = 0.1;
constexpr u64 BudgetStaleTicks = 45;
// **An intent as it crosses** (protocol 40). Its byte: the action's type in
// the low three bits, whether it is pressed, and which of its three axes are
// not zero -- only those follow. And, in place of a tick's count of intents,
// "the same as the tick before it in this message": input held is most input,
// and it went four times whole in every message.
constexpr u8 IntentTypeMask = 0x07;
constexpr u8 IntentPressed = 0x08;
constexpr u8 IntentAxisX = 0x10;
constexpr u16 SameIntents = 0xFFFF;
// **Not an action: whose the entries after it are** (protocol 48, ADR 0205).
// Its `bits` is the seat of one of the machine's other players.
constexpr u16 IntentSeat = 0xFFFE;
// **On a field's id in a record that is not whole** (protocol 40): a `CFrameD`
// that carries its position alone, because the rotation is the baseline's. A
// body that walks without turning is most of what moves.
constexpr u16 PositionAlone = 0x2000;

// **The value a written-field record holds when the write could not happen yet**
// -- a parent the replica has not been told about. No real field is all ones,
// so the next apply sees a difference and tries again.
[[nodiscard]] FieldValue pendingValue() noexcept
{
    FieldValue value;
    value.raw.fill(0xFF);
    return value;
}

[[nodiscard]] u64 packed(InstanceId id) noexcept
{
    return (static_cast<u64>(id.generation) << 32) | id.index;
}

// The component fields of each class that name another instance: a
// joint's ends, a weld's parts. Read as this machine's instance and sent as
// the peer's network id.
[[nodiscard]] const std::vector<usize>& referencesOf(u8 schema)
{
    static const std::vector<std::vector<usize>> table = [] {
        std::vector<std::vector<usize>> out(std::size(generated::Classes));
        for (usize index = 0; index < out.size(); ++index) {
            const generated::ClassDesc& desc = generated::Classes[index];
            const usize count = fieldCount(desc);
            for (usize at = std::size(generated::CommonFields); at < count; ++at) {
                const generated::FieldDesc* field = fieldAt(desc, at);
                if (field != nullptr && field->encoding == generated::Encoding::InstanceRef)
                    out[index].push_back(at);
            }
        }
        return out;
    }();
    return table[schema];
}

// --- Little-endian bytes ----------------------------------------------------

class Writer
{
public:
    std::vector<u8> bytes;

    void u8v(u8 value) { bytes.push_back(value); }
    void u16v(u16 value)
    {
        for (int at = 0; at < 2; ++at)
            bytes.push_back(static_cast<u8>(value >> (8 * at)));
    }
    void u32v(u32 value)
    {
        for (int at = 0; at < 4; ++at)
            bytes.push_back(static_cast<u8>(value >> (8 * at)));
    }
    void u64v(u64 value)
    {
        for (int at = 0; at < 8; ++at)
            bytes.push_back(static_cast<u8>(value >> (8 * at)));
    }
    void text(std::string_view value)
    {
        u16v(static_cast<u16>(std::min<usize>(value.size(), 0xFFFF)));
        bytes.insert(bytes.end(), value.begin(), value.begin() + std::min<usize>(value.size(), 0xFFFF));
    }
};

// Every read is bounds-checked and a failed one poisons the reader, so a
// decoder can read a whole message and ask once at the end whether it was all
// there -- rather than checking after every field and forgetting one.
class Reader
{
public:
    explicit Reader(std::span<const u8> bytes) noexcept : m_bytes(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] bool done() const noexcept { return m_at == m_bytes.size(); }
    [[nodiscard]] usize remaining() const noexcept { return m_bytes.size() - m_at; }
    [[nodiscard]] std::span<const u8> bytes() const noexcept { return m_bytes; }
    [[nodiscard]] usize& at() noexcept { return m_at; }
    void fail() noexcept { m_ok = false; }

    [[nodiscard]] u64 read(usize width) noexcept
    {
        if (!m_ok || m_bytes.size() - m_at < width) {
            m_ok = false;
            return 0;
        }
        u64 value = 0;
        for (usize at = 0; at < width; ++at)
            value |= static_cast<u64>(m_bytes[m_at + at]) << (8 * at);
        m_at += width;
        return value;
    }
    [[nodiscard]] u8 u8v() noexcept { return static_cast<u8>(read(1)); }
    [[nodiscard]] u16 u16v() noexcept { return static_cast<u16>(read(2)); }
    [[nodiscard]] u32 u32v() noexcept { return static_cast<u32>(read(4)); }
    [[nodiscard]] u64 u64v() noexcept { return read(8); }
    [[nodiscard]] std::string_view text() noexcept
    {
        const u16 length = u16v();
        if (!m_ok || m_bytes.size() - m_at < length) {
            m_ok = false;
            return {};
        }
        const std::string_view value{reinterpret_cast<const char*>(m_bytes.data() + m_at), length};
        m_at += length;
        return value;
    }

private:
    std::span<const u8> m_bytes;
    usize m_at = 0;
    bool m_ok = true;
};

// --- Attribute values (ADR 0106) ----------------------------------------------
//
// The tag is the value's `ValueType`, so the wire and the scene agree on what a
// number means without a second table. The layout is `docs/protocol/wire.md`'s
// "Attribute values".

void writeF32(Writer& out, f32 value)
{
    out.u32v(std::bit_cast<u32>(value));
}

void writeF64(Writer& out, f64 value)
{
    out.u64v(std::bit_cast<u64>(value));
}

[[nodiscard]] f32 readF32(Reader& in) noexcept
{
    return std::bit_cast<f32>(in.u32v());
}

[[nodiscard]] f64 readF64(Reader& in) noexcept
{
    return std::bit_cast<f64>(in.u64v());
}

// False for a value an attribute cannot hold, which is left out.
[[nodiscard]] bool writeAttributeValue(Writer& out, const scene::Value& value,
                                       const std::function<u32(InstanceId)>& netOf)
{
    const scene::ValueType type = scene::valueType(value);
    switch (type) {
    case scene::ValueType::Nil:
        out.u8v(static_cast<u8>(type));
        return true;
    case scene::ValueType::Bool:
        out.u8v(static_cast<u8>(type));
        out.u8v(std::get<bool>(value) ? 1 : 0);
        return true;
    case scene::ValueType::Number:
        out.u8v(static_cast<u8>(type));
        writeF64(out, std::get<f64>(value));
        return true;
    case scene::ValueType::String:
        out.u8v(static_cast<u8>(type));
        out.text(std::get<std::string>(value));
        return true;
    case scene::ValueType::Vector3: {
        const core::Vec3& v = std::get<core::Vec3>(value);
        out.u8v(static_cast<u8>(type));
        writeF32(out, v.x);
        writeF32(out, v.y);
        writeF32(out, v.z);
        return true;
    }
    case scene::ValueType::Color3: {
        const core::Color3& c = std::get<core::Color3>(value);
        out.u8v(static_cast<u8>(type));
        writeF32(out, c.r);
        writeF32(out, c.g);
        writeF32(out, c.b);
        return true;
    }
    case scene::ValueType::CFrame: {
        const core::CFrameD& frame = std::get<core::CFrameD>(value);
        out.u8v(static_cast<u8>(type));
        writeF64(out, frame.position.x);
        writeF64(out, frame.position.y);
        writeF64(out, frame.position.z);
        for (const auto& row : frame.rotation.m) {
            for (const f32 cell : row)
                writeF32(out, cell);
        }
        return true;
    }
    case scene::ValueType::Instance:
        out.u8v(static_cast<u8>(type));
        out.u32v(netOf(std::get<InstanceId>(value)));
        return true;
    case scene::ValueType::Vector2: {
        const core::Vec2& v = std::get<core::Vec2>(value);
        out.u8v(static_cast<u8>(type));
        writeF32(out, v.x);
        writeF32(out, v.y);
        return true;
    }
    case scene::ValueType::UDim: {
        const core::UDim& d = std::get<core::UDim>(value);
        out.u8v(static_cast<u8>(type));
        writeF32(out, d.scale);
        writeF32(out, d.offset);
        return true;
    }
    case scene::ValueType::UDim2: {
        const core::UDim2& d = std::get<core::UDim2>(value);
        out.u8v(static_cast<u8>(type));
        writeF32(out, d.x.scale);
        writeF32(out, d.x.offset);
        writeF32(out, d.y.scale);
        writeF32(out, d.y.offset);
        return true;
    }
    case scene::ValueType::Rect: {
        const core::Rect& r = std::get<core::Rect>(value);
        out.u8v(static_cast<u8>(type));
        writeF32(out, r.min.x);
        writeF32(out, r.min.y);
        writeF32(out, r.max.x);
        writeF32(out, r.max.y);
        return true;
    }
    // ADR 0110, protocol 18: a stop count, then each stop's numbers.
    case scene::ValueType::ColorSequence: {
        const core::ColorSequence& sequence = std::get<core::ColorSequence>(value);
        out.u8v(static_cast<u8>(type));
        out.u8v(static_cast<u8>(sequence.keypoints.size()));
        for (const core::ColorKeypoint& stop : sequence.keypoints) {
            writeF32(out, stop.time);
            writeF32(out, stop.value.r);
            writeF32(out, stop.value.g);
            writeF32(out, stop.value.b);
        }
        return true;
    }
    case scene::ValueType::NumberSequence: {
        const core::NumberSequence& sequence = std::get<core::NumberSequence>(value);
        out.u8v(static_cast<u8>(type));
        out.u8v(static_cast<u8>(sequence.keypoints.size()));
        for (const core::NumberKeypoint& stop : sequence.keypoints) {
            writeF32(out, stop.time);
            writeF32(out, stop.value);
            writeF32(out, stop.envelope);
        }
        return true;
    }
    default:
        return false;
    }
}

[[nodiscard]] std::optional<scene::Value> readAttributeValue(Reader& in, const std::function<InstanceId(u32)>& localOf)
{
    switch (static_cast<scene::ValueType>(in.u8v())) {
    case scene::ValueType::Nil:
        return scene::Value{};
    case scene::ValueType::Bool:
        return scene::Value{in.u8v() != 0};
    case scene::ValueType::Number:
        return scene::Value{readF64(in)};
    case scene::ValueType::String:
        return scene::Value{std::string(in.text())};
    case scene::ValueType::Vector3: {
        const f32 x = readF32(in);
        const f32 y = readF32(in);
        const f32 z = readF32(in);
        return scene::Value{core::Vec3{x, y, z}};
    }
    case scene::ValueType::Color3: {
        const f32 r = readF32(in);
        const f32 g = readF32(in);
        const f32 b = readF32(in);
        return scene::Value{core::Color3{r, g, b}};
    }
    case scene::ValueType::CFrame: {
        core::CFrameD frame;
        frame.position.x = readF64(in);
        frame.position.y = readF64(in);
        frame.position.z = readF64(in);
        for (auto& row : frame.rotation.m) {
            for (f32& cell : row)
                cell = readF32(in);
        }
        return scene::Value{frame};
    }
    case scene::ValueType::Instance: {
        const InstanceId local = localOf(in.u32v());
        return local.valid() ? scene::Value{local} : scene::Value{};
    }
    case scene::ValueType::Vector2: {
        const f32 x = readF32(in);
        const f32 y = readF32(in);
        return scene::Value{core::Vec2{x, y}};
    }
    case scene::ValueType::UDim: {
        const f32 scale = readF32(in);
        const f32 offset = readF32(in);
        return scene::Value{core::UDim{scale, offset}};
    }
    case scene::ValueType::UDim2: {
        core::UDim2 d;
        d.x.scale = readF32(in);
        d.x.offset = readF32(in);
        d.y.scale = readF32(in);
        d.y.offset = readF32(in);
        return scene::Value{d};
    }
    case scene::ValueType::Rect: {
        core::Rect r;
        r.min.x = readF32(in);
        r.min.y = readF32(in);
        r.max.x = readF32(in);
        r.max.y = readF32(in);
        return scene::Value{r};
    }
    // A sequence the authority could not have made is a message that lies;
    // refused, as any other malformed value is.
    case scene::ValueType::ColorSequence: {
        core::ColorSequence sequence;
        sequence.keypoints.resize(in.u8v());
        for (core::ColorKeypoint& stop : sequence.keypoints) {
            stop.time = readF32(in);
            stop.value.r = readF32(in);
            stop.value.g = readF32(in);
            stop.value.b = readF32(in);
        }
        if (!core::validSequence(sequence.keypoints)) {
            in.fail();
            return std::nullopt;
        }
        return scene::Value{std::move(sequence)};
    }
    case scene::ValueType::NumberSequence: {
        core::NumberSequence sequence;
        sequence.keypoints.resize(in.u8v());
        for (core::NumberKeypoint& stop : sequence.keypoints) {
            stop.time = readF32(in);
            stop.value = readF32(in);
            stop.envelope = readF32(in);
        }
        if (!core::validSequence(sequence.keypoints)) {
            in.fail();
            return std::nullopt;
        }
        return scene::Value{std::move(sequence)};
    }
    default:
        in.fail();
        return std::nullopt;
    }
}

// The count and the entries of one owner's attributes, in the order the world
// keeps them (insertion order: the same on every run, R10).
// The most tags one instance carries across the wire.
constexpr usize MaxReplicaTags = 1024;

[[nodiscard]] std::vector<u8> encodeAttributes(const scene::World& world, InstanceId id,
                                               const std::function<u32(InstanceId)>& netOf)
{
    scene::AttributeMap attributes;
    world.collectAttributes(id, attributes);
    Writer entries;
    u16 count = 0;
    for (const auto& [name, value] : attributes) {
        Writer entry;
        entry.text(world.atoms().text(name));
        if (!writeAttributeValue(entry, value, netOf))
            continue;
        entries.bytes.insert(entries.bytes.end(), entry.bytes.begin(), entry.bytes.end());
        ++count;
    }
    Writer out;
    out.u16v(count);
    out.bytes.insert(out.bytes.end(), entries.bytes.begin(), entries.bytes.end());
    // **And its tags**, in name order so the same set is the same bytes:
    // a part tagged on the authority is tagged on every machine, and a script
    // that finds things by tag finds them there too.
    scene::TagSet tags;
    world.collectTags(id, tags);
    std::vector<std::string_view> names;
    names.reserve(tags.size());
    for (const core::NameAtom tag : tags)
        names.push_back(world.atoms().text(tag));
    std::sort(names.begin(), names.end());
    out.u16v(static_cast<u16>(std::min<usize>(names.size(), MaxReplicaTags)));
    for (usize at = 0; at < names.size() && at < MaxReplicaTags; ++at)
        out.text(names[at]);
    return out.bytes;
}

// Whether an owner's attributes-and-tags body holds anything: two counts of
// none is an owner with nothing to send.
[[nodiscard]] bool carriesAny(const std::vector<u8>& body) noexcept
{
    return body.size() > 4;
}

// **A name in an `AttributeEdits`** (protocol 40): the number of one the
// connection was told before; or, with the top bit, the telling of the next,
// its text after it; or `LiteralName` and its text, told and not kept, once a
// connection has been told every number there is.
constexpr u16 DefineName = 0x8000;
constexpr u16 LiteralName = 0xFFFF;
constexpr usize MaxConnectionNames = 0x7FFF;

// One owner's attributes-and-tags body, taken apart: each attribute's name
// with the bytes of its value, and the tags, which are in name order.
struct OwnedBody
{
    std::vector<std::pair<std::string_view, std::span<const u8>>> values;
    std::vector<std::string_view> tags;
};

[[nodiscard]] bool splitAttributes(std::span<const u8> body, OwnedBody& out)
{
    Reader reader(body);
    const u16 count = reader.u16v();
    for (u16 at = 0; at < count && reader.ok(); ++at) {
        const std::string_view name = reader.text();
        const usize from = reader.at();
        // Read for its length alone: an instance it names is nobody's here.
        if (!readAttributeValue(reader, [](u32) { return InstanceId{}; }).has_value() || !reader.ok())
            return false;
        out.values.emplace_back(name, body.subspan(from, reader.at() - from));
    }
    const u16 tagCount = reader.u16v();
    for (u16 at = 0; at < tagCount && reader.ok(); ++at)
        out.tags.push_back(reader.text());
    return reader.ok() && reader.done();
}

// `GlobalScriptService`, and its fixed `Shared` folder, under a data model.
[[nodiscard]] InstanceId globalScriptsOf(const scene::World& world, InstanceId dataModel) noexcept
{
    return dataModel.valid() ? world.findFirstChildOfClass(
                                   dataModel, world.classes().findId(world.atoms().lookup("GlobalScriptService")))
                             : InstanceId{};
}

[[nodiscard]] usize schemaCount() noexcept
{
    return std::size(generated::Classes);
}

[[nodiscard]] u8 schemaIndexOf(const generated::ClassDesc* desc) noexcept
{
    // One of the table's own, so its place is its distance from the first.
    const auto at = desc - generated::Classes;
    return at >= 0 && static_cast<usize>(at) < schemaCount() ? static_cast<u8>(at) : u8{0};
}

// The flat index of a common field by name, which is fixed by the schema.
[[nodiscard]] usize commonIndex(std::string_view name) noexcept
{
    for (usize at = 0; at < std::size(generated::CommonFields); ++at) {
        if (generated::CommonFields[at].name == name)
            return at;
    }
    return static_cast<usize>(-1);
}

// The flat index a wire id names in a class, or past the end when it names none.
[[nodiscard]] usize indexOfWireId(const generated::ClassDesc& desc, u16 wireId) noexcept
{
    const usize count = fieldCount(desc);
    for (usize at = 0; at < count; ++at) {
        if (wireIdAt(desc, at) == wireId)
            return at;
    }
    return count;
}

[[nodiscard]] auto findEntity(std::vector<EntityState>& entities, u32 id)
{
    return std::lower_bound(entities.begin(), entities.end(), id,
                            [](const EntityState& entity, u32 probe) { return entity.id.value < probe; });
}

[[nodiscard]] const EntityState* findEntity(const WorldState& state, u32 id) noexcept
{
    const auto at = std::lower_bound(state.entities.begin(), state.entities.end(), id,
                                     [](const EntityState& entity, u32 probe) { return entity.id.value < probe; });
    return at != state.entities.end() && at->id.value == id ? &*at : nullptr;
}

[[nodiscard]] u32 bitsOf(float value) noexcept
{
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

[[nodiscard]] float floatOf(u32 bits) noexcept
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Whether the transport took it. **A refusal is counted, never passed over**:
// a message too large for the transport was dropped in silence and
// counted as sent, and the snapshot it was is the one a joining player waited
// on for ever.
// A message's bytes, under its kind and under what a game calls that kind.
void countBytes(Stats& stats, std::span<const u8> bytes)
{
    if (bytes.empty() || bytes[0] >= MessageKinds)
        return;
    stats.bytesByMessage[bytes[0]] += bytes.size();
    switch (static_cast<MessageType>(bytes[0])) {
    case MessageType::Snapshot:
    case MessageType::SnapshotPart:
        stats.snapshotBytes += bytes.size();
        break;
    case MessageType::Attributes:
    case MessageType::AttributeEdits:
        stats.attributeBytes += bytes.size();
        break;
    case MessageType::RemoteToAuthority:
    case MessageType::RemoteToReplica:
        stats.remoteBytes += bytes.size();
        break;
    case MessageType::UnreliableToAuthority:
    case MessageType::UnreliableToReplica:
        stats.unreliableBytes += bytes.size();
        break;
    case MessageType::Intent:
    case MessageType::IntentNames:
    case MessageType::OwnedState:
    case MessageType::DetectorInput:
    case MessageType::Ack:
        stats.inputBytes += bytes.size();
        break;
    default:
        break;
    }
}

bool sendBytes(net::ITransport& transport, net::PeerId peer, const std::vector<u8>& bytes, net::Delivery delivery,
               u8 channel, Stats& stats)
{
    if (transport.send(peer, bytes, delivery, channel).has_value()) {
        stats.sendFailures += 1;
        // **A reliable message refused is said**: it is not sent again,
        // and what it carried -- an edit to the ground -- was lost without a
        // word while a replica drifted from the authority.
        if (delivery == net::Delivery::Reliable) {
            const core::I18nArg args[] = {{"bytes", static_cast<core::i64>(bytes.size())},
                                          {"type", static_cast<core::i64>(bytes.empty() ? 0 : bytes[0])}};
            core::log(core::LogLevel::Warn, ENG_TR("net.warn.reliable_refused"), args);
        }
        return false;
    }
    stats.bytesSent += bytes.size();
    countBytes(stats, bytes);
    return true;
}

// The flags byte after a message's call number (protocol 8).
constexpr u8 RemoteReplyFlag = 0x1;
constexpr u8 RemoteFailedFlag = 0x2;
// Which of a machine's players a message is from or for (protocol 48): in a
// remote's flags, and in the top of an unreliable one's count of references.
constexpr u8 RemoteSeatShift = 2;
constexpr u8 RemoteSeatMask = 0x0C;
constexpr u16 UnreliableSeatShift = 14;
constexpr u16 UnreliableCountMask = 0x3FFF;

// **A `RemoteEvent` message on the wire** (ADR 0077): the event's network id,
// the call number and flags a `RemoteFunction` uses (ADR 0079), the network ids
// of the instances its arguments name, then the payload exactly as the script
// module wrote it. Nothing here parses the payload.
void writeRemote(Writer& out, MessageType type, u32 remote, const scene::RemoteMessage& message,
                 std::span<const u32> refs, u8 seat = 0)
{
    const std::span<const u8> payload = message.payload;
    out.u8v(static_cast<u8>(type));
    out.u32v(remote);
    out.u32v(message.call);
    out.u8v(static_cast<u8>((message.reply ? RemoteReplyFlag : 0) | (message.failed ? RemoteFailedFlag : 0) |
                            ((seat << RemoteSeatShift) & RemoteSeatMask)));
    out.u16v(static_cast<u16>(refs.size()));
    for (const u32 ref : refs)
        out.u32v(ref);
    out.u32v(static_cast<u32>(payload.size()));
    out.bytes.insert(out.bytes.end(), payload.begin(), payload.end());
}

struct RemoteOnWire
{
    u32 remote = 0;
    u32 call = 0;
    u8 flags = 0;
    std::vector<u32> refs;
    std::span<const u8> payload;
};

// Reads one, the type byte already consumed. False on anything short, long or
// oversized -- a peer's bytes are not trusted to be what they claim.
[[nodiscard]] bool readRemote(Reader& in, RemoteOnWire& out)
{
    out.remote = in.u32v();
    out.call = in.u32v();
    out.flags = in.u8v();
    const u16 count = in.u16v();
    // Four bytes a reference, so a count the message cannot hold is refused
    // before anything is kept for it.
    if (!in.ok() || count > MaxRemoteRefs || in.remaining() < static_cast<usize>(count) * 4u)
        return false;
    out.refs.reserve(count);
    for (u16 at = 0; at < count && in.ok(); ++at)
        out.refs.push_back(in.u32v());
    const u32 size = in.u32v();
    if (!in.ok() || size > MaxRemoteWirePayload || in.bytes().size() - in.at() != size)
        return false;
    out.payload = in.bytes().subspan(in.at(), size);
    in.at() += size;
    return true;
}

// **An `UnreliableRemoteEvent` message on the wire** (ADR 0161): its number
// for the connection, the event's network id, the network ids of the instances
// its arguments name, and the payload as the script module wrote it.
void writeUnreliable(Writer& out, MessageType type, u16 sequence, u32 remote, const scene::RemoteMessage& message,
                     std::span<const u32> refs, u8 seat = 0)
{
    const std::span<const u8> payload = message.payload;
    out.u8v(static_cast<u8>(type));
    out.u16v(sequence);
    out.u32v(remote);
    out.u16v(static_cast<u16>((static_cast<u16>(refs.size()) & UnreliableCountMask) |
                              static_cast<u16>(static_cast<u16>(seat & 3u) << UnreliableSeatShift)));
    for (const u32 ref : refs)
        out.u32v(ref);
    out.u32v(static_cast<u32>(payload.size()));
    out.bytes.insert(out.bytes.end(), payload.begin(), payload.end());
}

struct UnreliableOnWire
{
    u16 sequence = 0;
    u32 remote = 0;
    // Which of the machine's players it is from or for (protocol 48).
    u8 seat = 0;
    std::vector<u32> refs;
    std::span<const u8> payload;
};

// Reads one, the type byte already consumed, as `readRemote` does.
[[nodiscard]] bool readUnreliable(Reader& in, UnreliableOnWire& out)
{
    out.sequence = in.u16v();
    out.remote = in.u32v();
    const u16 packed = in.u16v();
    const u16 count = packed & UnreliableCountMask;
    out.seat = static_cast<u8>(packed >> UnreliableSeatShift);
    if (!in.ok() || count > MaxRemoteRefs || in.remaining() < static_cast<usize>(count) * 4u)
        return false;
    out.refs.reserve(count);
    for (u16 at = 0; at < count && in.ok(); ++at)
        out.refs.push_back(in.u32v());
    const u32 size = in.u32v();
    if (!in.ok() || size > MaxUnreliableWirePayload || in.bytes().size() - in.at() != size)
        return false;
    out.payload = in.bytes().subspan(in.at(), size);
    in.at() += size;
    return true;
}

// Whether a message numbered `sequence` is newer than the last one taken, the
// numbers wrapping: the newest wins, and one that arrives after it is dropped.
[[nodiscard]] bool newerUnreliable(u16 sequence, u16 last, bool heard) noexcept
{
    return !heard || static_cast<core::i16>(static_cast<u16>(sequence - last)) > 0;
}

[[nodiscard]] bool isClass(const scene::World& world, InstanceId id, std::string_view name)
{
    if (!world.alive(id))
        return false;
    const scene::ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    return descriptor != nullptr && world.atoms().text(descriptor->name) == name;
}

// Whether `id` is what a message with this call number may name: a
// `RemoteEvent` for a plain message, a `RemoteFunction` for a question or an
// answer. A peer is not trusted to have picked the right one.
[[nodiscard]] bool isRemoteTarget(const scene::World& world, InstanceId id, u32 call)
{
    if (!world.alive(id))
        return false;
    const scene::ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    if (descriptor == nullptr)
        return false;
    return world.atoms().text(descriptor->name) == (call != 0 ? "RemoteFunction" : "RemoteEvent");
}

} // namespace

u64 hashOf(const EntityState& entity) noexcept
{
    // A word at a time: a field is sixty-four bytes whatever it holds, and a
    // byte at a time was the whole cost of a checksum.
    u64 hash = 0x9E3779B97F4A7C15ull ^ (static_cast<u64>(entity.id.value) << 8) ^ entity.schema;
    for (const FieldValue& field : entity.fields) {
        for (usize at = 0; at < FieldValue::Bytes; at += sizeof(u64)) {
            u64 word = 0;
            std::memcpy(&word, field.raw.data() + at, sizeof(word));
            hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
            hash ^= hash >> 29;
        }
    }
    return hash != 0 ? hash : 1;
}

namespace {

[[nodiscard]] u64 foldChecksum(u64 folded, const EntityState& entity) noexcept
{
    const u64 own = entity.hash != 0 ? entity.hash : hashOf(entity);
    folded = (folded ^ own) * 0x100000001B3ull;
    return folded ^ (folded >> 31);
}

} // namespace

u64 checksumOf(const WorldState& state) noexcept
{
    u64 folded = 0xCBF29CE484222325ull;
    for (const EntityState& entity : state.entities)
        folded = foldChecksum(folded, entity);
    return folded;
}

u64 checksumOf(std::span<const EntityState* const> entities) noexcept
{
    u64 folded = 0xCBF29CE484222325ull;
    for (const EntityState* entity : entities)
        folded = foldChecksum(folded, *entity);
    return folded;
}

// --- Authority ----------------------------------------------------------------

AuthoritySession::Peer* AuthoritySession::peerFor(net::PeerId id) noexcept
{
    for (Peer& peer : m_peers) {
        if (peer.id == id)
            return &peer;
    }
    return nullptr;
}

u32 AuthoritySession::peerCount() const noexcept
{
    return static_cast<u32>(
        std::count_if(m_peers.begin(), m_peers.end(), [](const Peer& peer) { return peer.welcomed; }));
}

net::PeerPath AuthoritySession::worstPath() const noexcept
{
    net::PeerPath worst = net::PeerPath::None;
    for (const Peer& peer : m_peers)
        if (peer.welcomed)
            worst = std::max(worst, m_transport.path(peer.id));
    return worst;
}

net::PeerLink AuthoritySession::worstLink() const noexcept
{
    net::PeerLink worst;
    for (const Peer& peer : m_peers) {
        if (!peer.welcomed)
            continue;
        const net::PeerLink link = m_transport.link(peer.id);
        worst.roundTripMs = std::max(worst.roundTripMs, link.roundTripMs);
        worst.jitterMs = std::max(worst.jitterMs, link.jitterMs);
        worst.loss = std::max(worst.loss, link.loss);
    }
    return worst;
}

NetId AuthoritySession::netIdOf(InstanceId id) const noexcept
{
    const u64 key = packed(id);
    const auto found =
        std::lower_bound(m_netIds.begin(), m_netIds.end(), key,
                         [](const std::pair<u64, u32>& entry, u64 wanted) { return entry.first < wanted; });
    return found != m_netIds.end() && found->first == key ? NetId{found->second} : NetId{};
}

InstanceId AuthoritySession::instanceOfNet(const scene::World& world, u32 netId) const noexcept
{
    // By the index the capture built: a peer names ids by the thousand, and a
    // walk of the world for each was the cost it could make the authority pay.
    const auto found =
        std::lower_bound(m_orderOfNet.begin(), m_orderOfNet.end(), netId,
                         [](const std::pair<u32, u32>& entry, u32 wanted) { return entry.first < wanted; });
    if (found == m_orderOfNet.end() || found->first != netId)
        return {};
    const InstanceId id = m_order[found->second].id;
    return world.alive(id) ? id : InstanceId{};
}

namespace {

// A tilemap's blocks to one peer (ADR 0103), 64 to a message so a large map
// is several messages of a size every transport carries.
constexpr usize TileBlocksPerMessage = 64;
constexpr usize TileBlockCells = static_cast<usize>(scene::TileChunkEdge * scene::TileChunkEdge);

void sendTileBlocks(net::ITransport& transport, net::PeerId peer, u32 netId,
                    const std::vector<std::pair<scene::TileChunkKey, scene::TileChunk>>& blocks, Stats& stats)
{
    for (usize first = 0; first < blocks.size(); first += TileBlocksPerMessage) {
        const usize last = std::min(blocks.size(), first + TileBlocksPerMessage);
        Writer out;
        out.u8v(static_cast<u8>(MessageType::TilemapBlocks));
        out.u32v(netId);
        out.u16v(static_cast<u16>(last - first));
        for (usize at = first; at < last; ++at) {
            const auto& [key, cells] = blocks[at];
            out.u32v(static_cast<u32>(key.x));
            out.u32v(static_cast<u32>(key.y));
            out.u16v(static_cast<u16>(TileBlockCells));
            for (const u16 tile : cells)
                out.u16v(tile);
        }
        sendBytes(transport, peer, out.bytes, net::Delivery::Reliable, ControlChannel, stats);
    }
}

} // namespace

void AuthoritySession::diffTilemaps(const scene::World& world)
{
    m_tilemapEdits.clear();
    std::set<u32> present;
    for (const Captured& entry : m_order) {
        const scene::Tilemap2DComponent* tilemap = world.tilemaps2d().find(entry.id);
        if (tilemap == nullptr)
            continue;
        present.insert(entry.netId);
        const auto found = m_tilemapShadows.find(entry.netId);
        if (found == m_tilemapShadows.end()) {
            // New this send: every peer is sent it whole with its spawn.
            m_tilemapShadows.emplace(entry.netId, TilemapShadow{tilemap->chunks, tilemap->revision, world.restores()});
            continue;
        }
        TilemapShadow& shadow = found->second;
        if (shadow.revision == tilemap->revision && shadow.restores == world.restores())
            continue;
        // Both sorted by block: one merge walk finds every changed, added and
        // emptied block, in block order (R10).
        std::vector<TileBlock> edits;
        auto now = tilemap->chunks.begin();
        auto was = shadow.blocks.begin();
        while (now != tilemap->chunks.end() || was != shadow.blocks.end()) {
            if (was == shadow.blocks.end() || (now != tilemap->chunks.end() && now->first < was->first)) {
                edits.emplace_back(now->first, now->second);
                ++now;
            }
            else if (now == tilemap->chunks.end() || was->first < now->first) {
                edits.emplace_back(was->first, scene::TileChunk{});
                ++was;
            }
            else {
                if (now->second != was->second)
                    edits.emplace_back(now->first, now->second);
                ++now;
                ++was;
            }
        }
        shadow.blocks = tilemap->chunks;
        shadow.revision = tilemap->revision;
        shadow.restores = world.restores();
        if (!edits.empty())
            m_tilemapEdits.emplace(entry.netId, std::move(edits));
    }
    std::erase_if(m_tilemapShadows, [&](const auto& entry) { return !present.contains(entry.first); });
}

void AuthoritySession::diffAttributes(const scene::World& world, InstanceId root)
{
    m_attributeEdits.clear();
    const auto netOf = [this](InstanceId id) { return netIdOf(id).value; };
    std::map<AttributeOwner, std::vector<u8>> now;
    // What `encodeAttributes` makes of an owner with nothing: no attribute,
    // no tag.
    static const std::vector<u8> Nothing(4, u8{0});

    // Every captured instance that carries any, and the owners nothing spawns.
    //
    // **Most of a world carries no attribute and no tag**, and each of those
    // was encoded, put in a tree and compared with its like from the tick
    // before, every tick. They are passed over: an instance known last send
    // and not in the shadows carried nothing then.
    for (const Captured& entry : m_order) {
        if (!world.carriesAttributesOrTags(entry.id))
            continue;
        std::vector<u8> body = encodeAttributes(world, entry.id, netOf);
        m_stats.attributeBodiesEncoded += 1;
        if (carriesAny(body))
            now.emplace(AttributeOwner{u8{0}, entry.netId}, std::move(body));
    }
    // And the instances that carried some and now carry none, which is a
    // change like any other: still captured, they are compared as nothing.
    const auto byNet = [](const std::pair<u32, u32>& entry, u32 wanted) { return entry.first < wanted; };
    for (const auto& [owner, body] : m_attributeShadows) {
        if (owner.first != 0 || now.contains(owner))
            continue;
        if (const auto still = std::lower_bound(m_orderOfNet.begin(), m_orderOfNet.end(), owner.second, byNet);
            still != m_orderOfNet.end() && still->first == owner.second)
            now.emplace(owner, Nothing);
    }
    const InstanceId dataModel = world.parentOf(root);
    const InstanceId network = scene::networkServiceOf(world, dataModel);
    for (InstanceId child = network.valid() ? world.firstChild(network) : InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        if (const scene::PlayerComponent* player = world.players().find(child); player != nullptr)
            now.emplace(AttributeOwner{u8{1}, player->userId}, encodeAttributes(world, child, netOf));
    }
    if (const InstanceId global = globalScriptsOf(world, dataModel); global.valid()) {
        now.emplace(AttributeOwner{u8{2}, 0u}, encodeAttributes(world, global, netOf));
        if (const InstanceId shared = world.findFirstChild(global, world.atoms().lookup("Shared")); shared.valid())
            now.emplace(AttributeOwner{u8{3}, 0u}, encodeAttributes(world, shared, netOf));
    }

    // Changed against the shadow. An instance new this send is sent whole with
    // its spawn, so only an owner that was already known is an edit.
    for (const auto& [owner, body] : now) {
        const auto found = m_attributeShadows.find(owner);
        const std::vector<u8>* shadow = found != m_attributeShadows.end() ? &found->second : nullptr;
        // An instance the last send captured, with nothing on it then.
        if (shadow == nullptr && owner.first == 0 &&
            std::binary_search(m_attributeKnown.begin(), m_attributeKnown.end(), owner.second))
            shadow = &Nothing;
        if (shadow == nullptr) {
            if (owner.first != 0 && carriesAny(body)) {
                AttributeEdit whole;
                whole.owner = owner;
                whole.whole = body;
                m_attributeEdits.push_back(std::move(whole));
            }
            continue;
        }
        if (*shadow == body)
            continue;
        // **What of it changed, and nothing else**. The whole body
        // went again whenever one value did: a hero's eighteen attributes and
        // its tags, by name, thirty times a second, because it turned.
        AttributeEdit edit;
        edit.owner = owner;
        OwnedBody is;
        OwnedBody was;
        if (!splitAttributes(body, is) || !splitAttributes(*shadow, was)) {
            edit.whole = body;
            m_attributeEdits.push_back(std::move(edit));
            continue;
        }
        std::map<std::string_view, std::span<const u8>> before;
        for (const auto& [name, value] : was.values)
            before.emplace(name, value);
        for (const auto& [name, value] : is.values) {
            const auto held = before.find(name);
            if (held == before.end() ||
                !std::equal(value.begin(), value.end(), held->second.begin(), held->second.end()))
                edit.values.emplace_back(std::string(name), std::vector<u8>(value.begin(), value.end()));
            if (held != before.end())
                before.erase(held);
        }
        // What is left was there and is not: in name order.
        for (const auto& [name, value] : before)
            edit.values.emplace_back(std::string(name), std::vector<u8>{});
        // Both in name order: one walk finds what came and what went.
        std::vector<std::string_view> came;
        std::vector<std::string_view> went;
        std::set_difference(is.tags.begin(), is.tags.end(), was.tags.begin(), was.tags.end(), std::back_inserter(came));
        std::set_difference(was.tags.begin(), was.tags.end(), is.tags.begin(), is.tags.end(), std::back_inserter(went));
        edit.tagsAdded.assign(came.begin(), came.end());
        edit.tagsRemoved.assign(went.begin(), went.end());
        m_attributeEdits.push_back(std::move(edit));
    }
    m_attributeShadows = std::move(now);
    // Of the instances, only what carries something is kept.
    std::erase_if(m_attributeShadows,
                  [](const auto& entry) { return entry.first.first == 0 && !carriesAny(entry.second); });
    m_attributeKnown.clear();
    m_attributeKnown.reserve(m_orderOfNet.size());
    for (const std::pair<u32, u32>& entry : m_orderOfNet)
        m_attributeKnown.push_back(entry.first);
}

// --- The ground (ADR 0135) ------------------------------------------------------
//
// The workspace's terrain and the block world, sent as whole chunks: every
// chunk that differs from the scene to a peer that joins, then every chunk an
// edit changed. A chunk received twice is simply the truth.

namespace {

constexpr usize GroundChunksPerMessage = 64;
// **And at most this many bytes**: a whole heightmap rewritten, painted
// a column at a time, made sixty-four chunks of more than a megabyte -- past
// the host's own ceiling on a message (`MaxAuthorityMessageBytes`), which ENet
// holds what it sends to as well as what it takes. The send was refused, the
// edit never reached a replica, and its player walked on ground the host did
// not have. A chunk larger than this alone still goes, in a message of its own.
constexpr usize GroundMessageBytes = 256u * 1024u;
// The longest code a block chunk can have: a run for every block.
constexpr usize MaxVoxelChunkCode = static_cast<usize>(asset::VoxelChunkVolume) * 4;

[[nodiscard]] InstanceId groundTerrainUnder(const scene::World& world, InstanceId root)
{
    for (InstanceId child = root.valid() ? world.firstChild(root) : InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        if (world.terrains().find(child) != nullptr)
            return child;
    }
    return {};
}

[[nodiscard]] InstanceId groundVoxelsOf(const scene::World& world)
{
    InstanceId found;
    world.voxels().forEach([&found](InstanceId id, const scene::VoxelComponent&) {
        if (!found.valid())
            found = id;
    });
    return found;
}

// The chunk `entries` holds at `key`, or null.
template <class Entry, class Key>
[[nodiscard]] const void* chunkAt(std::span<const Entry> entries, const Key& key) noexcept
{
    const auto at = std::lower_bound(entries.begin(), entries.end(), key,
                                     [](const Entry& entry, const Key& probe) { return entry.first < probe; });
    return at != entries.end() && at->first == key ? static_cast<const void*>(at->second.get()) : nullptr;
}

// **What changed between two sends**: every key whose chunk now is not the
// chunk it was, in key order -- less a key that was the package's chunk and
// still is, which is ground a streamed cell brought in or took out on both
// ends alike. An emptied key comes back with no chunk.
template <class Entry>
[[nodiscard]] std::vector<Entry> changedChunks(std::span<const Entry> now, std::span<const Entry> was,
                                               std::span<const Entry> shippedNow, std::span<const Entry> shippedWas)
{
    std::vector<Entry> out;
    const auto consider = [&](const auto& key, const auto& chunk, const void* before) {
        if (chunk.get() == chunkAt(shippedNow, key) && before == chunkAt(shippedWas, key))
            return;
        // **Gone from the ground and from the package's copy at once** is a
        // cell streamed out, which every machine does for itself -- even when
        // the chunk was an edit whose bytes came back to the package's, which
        // the streamer calls untouched by digest where this compares
        // pointers. Sent, it was a hole on every replica.
        if (chunk == nullptr && chunkAt(shippedNow, key) == nullptr && chunkAt(shippedWas, key) != nullptr)
            return;
        out.emplace_back(key, chunk);
    };
    auto a = now.begin();
    auto b = was.begin();
    while (a != now.end() || b != was.end()) {
        if (b == was.end() || (a != now.end() && a->first < b->first)) {
            consider(a->first, a->second, nullptr);
            ++a;
        }
        else if (a == now.end() || b->first < a->first) {
            consider(b->first, decltype(b->second){}, b->second.get());
            ++b;
        }
        else {
            if (a->second != b->second)
                consider(a->first, a->second, b->second.get());
            ++a;
            ++b;
        }
    }
    return out;
}

// Every key whose chunk is not the package's: what a peer that loaded the
// scene lacks.
template <class Entry>
[[nodiscard]] std::vector<Entry> unshippedChunks(std::span<const Entry> now, std::span<const Entry> shipped)
{
    return changedChunks<Entry>(now, shipped, std::span<const Entry>{}, std::span<const Entry>{});
}

void writeKey(Writer& out, core::i32 x, core::i32 y, core::i32 z)
{
    out.u32v(static_cast<u32>(x));
    out.u32v(static_cast<u32>(y));
    out.u32v(static_cast<u32>(z));
}

// The chunks each message holds, as runs of their encoded `sizes`: at most
// `GroundChunksPerMessage` of them and, past the first, at most
// `GroundMessageBytes`.
[[nodiscard]] std::vector<std::pair<usize, usize>> groundRuns(const std::vector<usize>& sizes)
{
    std::vector<std::pair<usize, usize>> runs;
    usize first = 0;
    usize bytes = 0;
    for (usize at = 0; at < sizes.size(); ++at) {
        // Its key and its length, and its code.
        const usize cost = 16u + sizes[at];
        if (at > first && (at - first == GroundChunksPerMessage || bytes + cost > GroundMessageBytes)) {
            runs.emplace_back(first, at);
            first = at;
            bytes = 0;
        }
        bytes += cost;
    }
    if (first < sizes.size())
        runs.emplace_back(first, sizes.size());
    return runs;
}

[[nodiscard]] std::vector<std::vector<u8>> terrainChunkMessages(const asset::FieldSettings& settings,
                                                                const std::vector<asset::TerrainField::Entry>& chunks)
{
    std::vector<std::vector<std::byte>> codes(chunks.size());
    std::vector<usize> sizes(chunks.size(), 0);
    for (usize at = 0; at < chunks.size(); ++at) {
        if (chunks[at].second != nullptr)
            codes[at] = asset::encodeTerrainChunk(*chunks[at].second);
        sizes[at] = codes[at].size();
    }
    std::vector<std::vector<u8>> messages;
    for (const auto& [first, last] : groundRuns(sizes)) {
        Writer out;
        out.u8v(static_cast<u8>(MessageType::TerrainChunks));
        writeF32(out, settings.voxelSize);
        writeF32(out, settings.minHeight);
        writeF32(out, settings.maxHeight);
        out.u16v(static_cast<u16>(last - first));
        for (usize at = first; at < last; ++at) {
            const asset::ChunkKey& key = chunks[at].first;
            writeKey(out, key.x, key.y, key.z);
            out.u32v(static_cast<u32>(codes[at].size()));
            for (const std::byte byte : codes[at])
                out.u8v(static_cast<u8>(byte));
        }
        messages.push_back(std::move(out.bytes));
    }
    return messages;
}

[[nodiscard]] std::vector<std::vector<u8>> voxelChunkMessages(f32 blockSize,
                                                              const std::vector<asset::VoxelGrid::Entry>& chunks)
{
    std::vector<std::vector<core::u8>> codes(chunks.size());
    std::vector<usize> sizes(chunks.size(), 0);
    for (usize at = 0; at < chunks.size(); ++at) {
        if (chunks[at].second != nullptr)
            codes[at] = asset::encodeVoxelChunk(*chunks[at].second);
        sizes[at] = codes[at].size();
    }
    std::vector<std::vector<u8>> messages;
    for (const auto& [first, last] : groundRuns(sizes)) {
        Writer out;
        out.u8v(static_cast<u8>(MessageType::VoxelChunks));
        writeF32(out, blockSize);
        out.u16v(static_cast<u16>(last - first));
        for (usize at = first; at < last; ++at) {
            const auto& key = chunks[at].first;
            writeKey(out, key.x, key.y, key.z);
            out.u32v(static_cast<u32>(codes[at].size()));
            for (const core::u8 byte : codes[at])
                out.u8v(byte);
        }
        messages.push_back(std::move(out.bytes));
    }
    return messages;
}

[[nodiscard]] std::vector<u8> terrainLookMessage(const scene::TerrainComponent& terrain)
{
    Writer out;
    out.u8v(static_cast<u8>(MessageType::TerrainLook));
    // Where it is: a terrain a server's script placed, or
    // moved, stood at the origin on every replica.
    writeF64(out, terrain.origin.x);
    writeF64(out, terrain.origin.y);
    writeF64(out, terrain.origin.z);
    const usize layers = std::min<usize>(terrain.layers.size(), asset::MaxTerrainLayers);
    out.u16v(static_cast<u16>(layers));
    for (usize at = 0; at < layers; ++at)
        out.text(terrain.layers[at]);
    const usize rules = std::min<usize>(terrain.rules.size(), asset::MaxTerrainRules);
    out.u16v(static_cast<u16>(rules));
    for (usize at = 0; at < rules; ++at) {
        const asset::TerrainRule& rule = terrain.rules[at];
        out.u8v(rule.enabled ? 1 : 0);
        out.u8v(rule.material);
        for (const f32 value : {rule.slopeMin, rule.slopeMax, rule.heightMin, rule.heightMax, rule.blend, rule.noise})
            writeF32(out, value);
        const usize applies = std::min<usize>(rule.appliesTo.size(), 255);
        out.u16v(static_cast<u16>(applies));
        for (usize index = 0; index < applies; ++index)
            out.u8v(rule.appliesTo[index]);
    }
    return std::move(out.bytes);
}

// **The collision groups, whole**: the names, and the pairs that do not
// collide. Nothing at all while no game has registered a group and `Default`
// meets itself -- the table every machine starts with.
[[nodiscard]] std::vector<u8> collisionGroupsMessage(const scene::World& world)
{
    const scene::CollisionGroups& groups = world.collisionGroups();
    const u32 count = groups.count();
    if (count <= 1 && groups.collidable(scene::CollisionGroups::kDefault, scene::CollisionGroups::kDefault))
        return {};
    Writer out;
    out.u8v(static_cast<u8>(MessageType::CollisionGroups));
    out.u16v(static_cast<u16>(count));
    for (u32 at = 0; at < count; ++at)
        out.text(world.atoms().text(groups.nameAt(static_cast<u16>(at))));
    std::vector<std::pair<u16, u16>> apart;
    for (u32 a = 0; a < count; ++a) {
        for (u32 b = a; b < count; ++b) {
            if (!groups.collidable(static_cast<u16>(a), static_cast<u16>(b)))
                apart.emplace_back(static_cast<u16>(a), static_cast<u16>(b));
        }
    }
    out.u32v(static_cast<u32>(apart.size()));
    for (const auto& [a, b] : apart) {
        out.u16v(a);
        out.u16v(b);
    }
    return std::move(out.bytes);
}

[[nodiscard]] std::vector<u8> voxelTypesMessage(const scene::World& world, const scene::VoxelComponent& voxels)
{
    Writer out;
    out.u8v(static_cast<u8>(MessageType::VoxelTypes));
    const usize count = std::min<usize>(voxels.types.size(), 0xFFFF);
    out.u16v(static_cast<u16>(count));
    const auto atom = [&world](core::NameAtom name) {
        return name.valid() ? world.atoms().text(name) : std::string_view{};
    };
    for (usize at = 0; at < count; ++at) {
        const scene::VoxelBlockType& type = voxels.types[at];
        out.text(atom(type.name));
        for (const core::Color3& color : {type.color, type.side, type.bottom}) {
            writeF32(out, color.r);
            writeF32(out, color.g);
            writeF32(out, color.b);
        }
        out.text(atom(type.texture));
        out.text(atom(type.sideTexture));
        out.text(atom(type.bottomTexture));
        out.u8v(static_cast<u8>(std::clamp(type.opacity, 0, 2)));
        writeF32(out, type.transparency);
        out.u8v(type.fluidReach);
        out.u32v(type.fluidTicks);
    }
    const usize reactions = std::min<usize>(voxels.fluidReactions.size(), 0xFFFF);
    out.u16v(static_cast<u16>(reactions));
    for (usize at = 0; at < reactions; ++at) {
        const scene::VoxelComponent::FluidReaction& reaction = voxels.fluidReactions[at];
        out.u16v(reaction.from);
        out.u16v(reaction.touching);
        out.u16v(reaction.result);
    }
    return std::move(out.bytes);
}

} // namespace

void AuthoritySession::diffGround(const scene::World& world, InstanceId root)
{
    m_groundEdits.clear();
    // Whatever changes below makes the whole ground a peer is sent another.
    const auto invalidate = [this]() {
        m_groundWholeValid = false;
        // And whatever is being encoded in a job is of a ground that is gone.
        m_groundGeneration += 1;
    };
    const bool restored = m_ground.restores != world.restores();
    m_ground.restores = world.restores();
    // **Whether anybody holds the ground to be told what changed**.
    // The changes are compressed chunk by chunk into messages for the peers
    // that have been sent the ground -- and were, with no such peer: a land a
    // script lays in a match's first second cost its host twelve milliseconds
    // of a frame, for nobody. With none, what changed is only noted: whoever
    // joins is sent the ground whole, as it then is.
    const bool listened = std::any_of(m_peers.begin(), m_peers.end(), [](const Peer& peer) { return peer.groundSent; });

    // **The collision groups**, with the ground because they are sent
    // as it is: whole to a peer that joins, and to every peer when they
    // change. The world's, not a scene's -- a new scene does not forget them.
    // Read once a revision, and after everything below, put FIRST: a part
    // whose group a replica has not heard of collides as `Default` until it
    // has.
    std::vector<u8> groupsChanged;
    if (const u32 revision = world.collisionGroups().revision(); !m_groupsRead || revision != m_groupsRevision) {
        m_groupsRead = true;
        m_groupsRevision = revision;
        std::vector<u8> groups = collisionGroupsMessage(world);
        if (groups != m_groupsSent) {
            m_groupsSent = groups;
            groupsChanged = std::move(groups);
            invalidate();
        }
    }
    const auto withGroups = [&]() {
        if (!groupsChanged.empty())
            m_groundEdits.insert(m_groundEdits.begin(), std::move(groupsChanged));
    };

    // **A new scene is every peer's own ground again**: each loads it.
    if (const std::string& scene = world.engineState().currentScene; scene != m_ground.scene) {
        invalidate();
        m_ground.scene = scene;
        m_ground.terrain = InstanceId{};
        m_ground.terrainChunks.clear();
        m_ground.terrainShipped.clear();
        m_ground.base.clear();
        m_ground.baseSet = false;
        m_ground.terrainRevision = ~u64{0};
        m_ground.terrainLook.clear();
    }

    // The terrain. A scene's -- or one a script made in a scene with none --
    // starts from the package's ground, which is what a peer sent the ground
    // whole before it was here holds: from an empty shadow, a chunk of the
    // package this terrain lacks was never visited, and its removal never
    // sent.
    //
    // **One that replaces another in the same scene is the same ground to
    // every peer**: a script that destroyed the terrain
    // and made a new one left the old scene's ground on every replica, which
    // has no way to know a different instance meant different ground. So the
    // shadow stays what the peers hold, and the package they loaded stays the
    // base the new one is measured against -- and a terrain destroyed takes
    // its ground from every replica with it.
    const InstanceId terrainId = groundTerrainUnder(world, root);
    const scene::TerrainComponent* terrain = terrainId.valid() ? world.terrains().find(terrainId) : nullptr;
    if (terrainId != m_ground.terrain) {
        invalidate();
        if (m_ground.terrain.valid() || m_ground.baseSet) {
            if (!m_ground.baseSet) {
                m_ground.base = m_ground.terrainShipped;
                m_ground.baseSet = true;
            }
        }
        else if (terrain != nullptr) {
            m_ground.terrainChunks.assign(terrain->shipped.chunks().begin(), terrain->shipped.chunks().end());
            m_ground.terrainShipped = m_ground.terrainChunks;
        }
        m_ground.terrain = terrainId;
        m_ground.terrainRevision = ~u64{0};
        m_ground.terrainLook.clear();
    }
    const std::span<const asset::TerrainField::Entry> package =
        m_ground.baseSet     ? std::span<const asset::TerrainField::Entry>(m_ground.base)
        : terrain != nullptr ? terrain->shipped.chunks()
                             : std::span<const asset::TerrainField::Entry>{};
    if (terrain != nullptr) {
        if (restored || terrain->fieldRevision != m_ground.terrainRevision) {
            const std::vector<asset::TerrainField::Entry> changed = changedChunks<asset::TerrainField::Entry>(
                terrain->field.chunks(), m_ground.terrainChunks, package, m_ground.terrainShipped);
            if (!listened) {
                if (!changed.empty())
                    invalidate();
            }
            else {
                for (std::vector<u8>& message : terrainChunkMessages(terrain->field.settings(), changed))
                    m_groundEdits.push_back(std::move(message));
            }
            m_ground.terrainChunks.assign(terrain->field.chunks().begin(), terrain->field.chunks().end());
            m_ground.terrainShipped.assign(package.begin(), package.end());
            m_ground.terrainRevision = terrain->fieldRevision;
            m_ground.settings = terrain->field.settings();
        }
        std::vector<u8> look = terrainLookMessage(*terrain);
        if (look != m_ground.terrainLook) {
            m_groundEdits.push_back(look);
            m_ground.terrainLook = std::move(look);
        }
    }
    else if (m_ground.baseSet && !m_ground.terrainChunks.empty()) {
        const std::vector<asset::TerrainField::Entry> gone = changedChunks<asset::TerrainField::Entry>(
            std::span<const asset::TerrainField::Entry>{}, m_ground.terrainChunks, package, m_ground.terrainShipped);
        if (!listened) {
            if (!gone.empty())
                invalidate();
        }
        else {
            for (std::vector<u8>& message : terrainChunkMessages(m_ground.settings, gone))
                m_groundEdits.push_back(std::move(message));
        }
        m_ground.terrainChunks.clear();
        m_ground.terrainShipped.assign(package.begin(), package.end());
    }

    // The block world, the same way.
    const InstanceId voxelsId = groundVoxelsOf(world);
    const scene::VoxelComponent* voxels = voxelsId.valid() ? world.voxels().find(voxelsId) : nullptr;
    if (!m_groundEdits.empty())
        invalidate();
    if (voxels == nullptr) {
        if (m_ground.voxelRevision != ~u64{0})
            invalidate();
        m_ground.voxelChunks.clear();
        m_ground.voxelShipped.clear();
        m_ground.voxelTypes.clear();
        m_ground.voxelRevision = ~u64{0};
        withGroups();
        // Noted above, and nobody's to be sent.
        if (!listened)
            m_groundEdits.clear();
        return;
    }
    // A block world new here, on the terrain's terms.
    if (m_ground.voxelRevision == ~u64{0}) {
        m_ground.voxelChunks.assign(voxels->shipped.chunks().begin(), voxels->shipped.chunks().end());
        m_ground.voxelShipped = m_ground.voxelChunks;
    }
    if (restored || voxels->revision != m_ground.voxelRevision) {
        const std::vector<asset::VoxelGrid::Entry> changed = changedChunks<asset::VoxelGrid::Entry>(
            voxels->grid.chunks(), m_ground.voxelChunks, voxels->shipped.chunks(), m_ground.voxelShipped);
        if (!listened) {
            if (!changed.empty())
                invalidate();
        }
        else {
            for (std::vector<u8>& message : voxelChunkMessages(voxels->blockSize, changed))
                m_groundEdits.push_back(std::move(message));
        }
        m_ground.voxelChunks.assign(voxels->grid.chunks().begin(), voxels->grid.chunks().end());
        m_ground.voxelShipped.assign(voxels->shipped.chunks().begin(), voxels->shipped.chunks().end());
        m_ground.voxelRevision = voxels->revision;
    }
    std::vector<u8> types = voxelTypesMessage(world, *voxels);
    if (types != m_ground.voxelTypes) {
        // Before the chunks that may use them: a block of a type the replica
        // has not been told of draws as nothing.
        m_groundEdits.insert(m_groundEdits.begin(), types);
        m_ground.voxelTypes = std::move(types);
    }
    withGroups();
    if (!m_groundEdits.empty())
        invalidate();
    if (!listened)
        m_groundEdits.clear();
}

// **A whole ground, from what it is made of to the messages a peer is sent**.
// Reading it off the world is cheap and is done on the frame: the
// groups, the look, the block types, and WHICH chunks differ from what the
// peer loaded -- a chunk is shared and never written to (`terrain.h`), so the
// list is pointers. Compressing every one of them is the cost, and `encode`
// touches nothing but this.
struct AuthoritySession::GroundWork
{
    // Which ground it is of (`m_groundGeneration`), and the job making it.
    u64 generation = 0;
    jobs::JobHandle job;

    // In the order a peer is sent them. The collision groups first,
    // when a game has any.
    std::vector<u8> groups;
    // The terrain's chunks -- against the package the peer loaded: a terrain
    // that replaced the scene's is measured from the scene's;
    // or, the scene's terrain destroyed and none since, its ground going.
    bool terrain = false;
    asset::FieldSettings settings;
    std::vector<asset::TerrainField::Entry> terrainChunks;
    // Its look; empty where there is no terrain to have one.
    std::vector<u8> look;
    bool voxels = false;
    std::vector<u8> voxelTypes;
    f32 blockSize = 0.0f;
    std::vector<asset::VoxelGrid::Entry> voxelChunks;

    std::vector<std::vector<u8>> messages;

    [[nodiscard]] bool compresses() const noexcept { return !terrainChunks.empty() || !voxelChunks.empty(); }

    void encode()
    {
        messages.clear();
        if (!groups.empty())
            messages.push_back(groups);
        if (terrain) {
            for (std::vector<u8>& message : terrainChunkMessages(settings, terrainChunks))
                messages.push_back(std::move(message));
            if (!look.empty())
                messages.push_back(look);
        }
        if (voxels) {
            messages.push_back(voxelTypes);
            for (std::vector<u8>& message : voxelChunkMessages(blockSize, voxelChunks))
                messages.push_back(std::move(message));
        }
    }
};

AuthoritySession::GroundWork AuthoritySession::groundWorkOf(const scene::World& world)
{
    GroundWork work;
    work.generation = m_groundGeneration;
    work.groups = collisionGroupsMessage(world);
    if (const scene::TerrainComponent* terrain =
            m_ground.terrain.valid() ? world.terrains().find(m_ground.terrain) : nullptr;
        terrain != nullptr) {
        work.terrain = true;
        work.settings = terrain->field.settings();
        work.terrainChunks = unshippedChunks<asset::TerrainField::Entry>(
            terrain->field.chunks(),
            m_ground.baseSet ? std::span<const asset::TerrainField::Entry>(m_ground.base) : terrain->shipped.chunks());
        work.look = terrainLookMessage(*terrain);
    }
    else if (m_ground.baseSet) {
        work.terrain = true;
        work.settings = m_ground.settings;
        work.terrainChunks =
            unshippedChunks<asset::TerrainField::Entry>(std::span<const asset::TerrainField::Entry>{}, m_ground.base);
    }
    const InstanceId voxelsId = groundVoxelsOf(world);
    if (const scene::VoxelComponent* voxels = voxelsId.valid() ? world.voxels().find(voxelsId) : nullptr;
        voxels != nullptr) {
        work.voxels = true;
        work.voxelTypes = voxelTypesMessage(world, *voxels);
        work.blockSize = voxels->blockSize;
        work.voxelChunks = unshippedChunks<asset::VoxelGrid::Entry>(voxels->grid.chunks(), voxels->shipped.chunks());
    }
    return work;
}

bool AuthoritySession::groundWholeReady(const scene::World& world)
{
    if (m_groundWholeValid || !jobs::initialized())
        return true;
    if (m_groundWork != nullptr) {
        if (!jobs::finished(m_groundWork->job))
            return false;
        const std::shared_ptr<GroundWork> done = std::move(m_groundWork);
        m_groundWork.reset();
        if (done->generation == m_groundGeneration) {
            m_groundWhole = std::move(done->messages);
            m_groundWholeValid = true;
            m_stats.groundsEncodedInJobs += 1;
        }
        // Of a ground that has changed since, and thrown away: the peer is
        // given the present one, encoded where it is sent, once -- a ground
        // dug at every tick would otherwise keep a peer out for good.
        return true;
    }
    // **The job is told nothing it could outlive**: the work is kept until
    // the job has run, and whoever lets go of it waits for that first.
    std::shared_ptr<GroundWork> work(new GroundWork(groundWorkOf(world)), [](GroundWork* made) {
        if (made->job.valid() && jobs::initialized())
            jobs::wait(made->job);
        delete made;
    });
    // Nothing to compress: made here, which is nothing.
    if (!work->compresses())
        return true;
    GroundWork* raw = work.get();
    work->job = jobs::schedule("ground-encode", jobs::Domain::AssetIo, [raw]() noexcept { raw->encode(); });
    if (!work->job.valid())
        return true;
    m_groundWork = std::move(work);
    return false;
}

void AuthoritySession::sendGroundWhole(Peer& peer, const scene::World& world)
{
    if (!m_groundWholeValid) {
        // Made with the messages each peer is sent, in the order it is sent
        // them. Here, on the frame, for a peer already in the match whose
        // scene changed, for a ground that changed while a job encoded it,
        // and where there are no jobs.
        GroundWork work = groundWorkOf(world);
        work.encode();
        m_groundWhole = std::move(work.messages);
        m_groundWholeValid = true;
        if (work.compresses())
            m_stats.groundsEncodedOnFrame += 1;
    }
    for (const std::vector<u8>& bytes : m_groundWhole)
        sendBytes(m_transport, peer.id, bytes, net::Delivery::Reliable, ControlChannel, m_stats);
}

void AuthoritySession::sendAttributes(Peer& peer, const std::vector<u32>& entering)
{
    const auto send = [&](const AttributeOwner& owner, const std::vector<u8>& body) {
        Writer message;
        message.u8v(static_cast<u8>(MessageType::Attributes));
        message.u8v(owner.first);
        message.u32v(owner.second);
        message.bytes.insert(message.bytes.end(), body.begin(), body.end());
        sendBytes(m_transport, peer.id, message.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
    };
    const auto knows = [&](u32 id) { return std::binary_search(peer.known.begin(), peer.known.end(), id); };
    const auto isEntering = [&](u32 id) { return std::binary_search(entering.begin(), entering.end(), id); };

    // A peer new to an owner is sent what it has; an owner with none is
    // nothing to send.
    for (const u32 id : entering) {
        if (const auto found = m_attributeShadows.find(AttributeOwner{u8{0}, id});
            found != m_attributeShadows.end() && carriesAny(found->second))
            send(found->first, found->second);
    }
    if (!peer.attributesSeeded) {
        peer.attributesSeeded = true;
        for (const auto& [owner, body] : m_attributeShadows) {
            if (!carriesAny(body))
                continue;
            // **And the owners nothing ever spawns**: the workspace
            // and the services are on both ends from boot, so they never
            // "enter" a peer's view -- and what they held when it joined was
            // sent to nobody. A round's number set before a player arrived
            // was nil on that player's machine until it was written again.
            const bool neverSpawned =
                owner.first == 0 && (owner.second == RootNetId.value || owner.second >= ServiceNetIdBase);
            if (owner.first != 0 || (neverSpawned && !isEntering(owner.second)))
                send(owner, body);
        }
    }
    // A name as this peer is told it: its number, and its text the first time.
    const auto name = [&peer](Writer& out, const std::string& text) {
        if (const auto told = peer.names.find(text); told != peer.names.end()) {
            out.u16v(told->second);
            return;
        }
        if (peer.names.size() >= MaxConnectionNames) {
            out.u16v(LiteralName);
            out.text(text);
            return;
        }
        const auto number = static_cast<u16>(peer.names.size());
        peer.names.emplace(text, number);
        out.u16v(static_cast<u16>(DefineName | number));
        out.text(text);
    };
    for (const AttributeEdit& edit : m_attributeEdits) {
        const AttributeOwner& owner = edit.owner;
        if (owner.first == 0 && (!knows(owner.second) || isEntering(owner.second)))
            continue;
        if (!edit.whole.empty()) {
            send(owner, edit.whole);
            continue;
        }
        Writer message;
        message.u8v(static_cast<u8>(MessageType::AttributeEdits));
        message.u8v(owner.first);
        message.u32v(owner.second);
        message.u16v(static_cast<u16>(edit.values.size()));
        for (const auto& [text, value] : edit.values) {
            name(message, text);
            message.u8v(value.empty() ? 0 : 1);
            message.bytes.insert(message.bytes.end(), value.begin(), value.end());
        }
        for (const std::vector<std::string>* tags : {&edit.tagsAdded, &edit.tagsRemoved}) {
            message.u16v(static_cast<u16>(tags->size()));
            for (const std::string& tag : *tags)
                name(message, tag);
        }
        sendBytes(m_transport, peer.id, message.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
    }
}

// --- A replicated swarm (ADR 0162) ------------------------------------------------

[[nodiscard]] static std::optional<core::DVec3> focusOf(const scene::World& world,
                                                        const scene::PlayerComponent* player);

namespace {

constexpr f64 Pi = 3.14159265358979323846;

// A facing as a 256th of a turn, and back.
[[nodiscard]] u8 packYaw(f32 yaw) noexcept
{
    const f64 turns = static_cast<f64>(yaw) / (2.0 * Pi);
    const f64 wrapped = turns - std::floor(turns);
    return static_cast<u8>(static_cast<u32>(wrapped * 256.0 + 0.5) & 0xffu);
}

[[nodiscard]] f32 unpackYaw(u8 packed) noexcept
{
    const f64 turns = static_cast<f64>(packed) / 256.0;
    return static_cast<f32>((turns > 0.5 ? turns - 1.0 : turns) * 2.0 * Pi);
}

// A walk in quarters of a metre a second, six bits.
[[nodiscard]] u8 packWalk(f32 walk) noexcept
{
    return static_cast<u8>(std::clamp(static_cast<f64>(walk) * 4.0 + 0.5, 0.0, 63.0));
}

// How far apart two facings are, in radians.
[[nodiscard]] f64 turnBetween(f32 a, f32 b) noexcept
{
    f64 turn = std::fabs(static_cast<f64>(a) - static_cast<f64>(b));
    while (turn > Pi)
        turn = std::fabs(turn - 2.0 * Pi);
    return turn;
}

// The origin a message's positions are measured from: a 16 m lattice near
// `focus`, in whole metres, so every agent in reach is within twelve bits of
// an eighth of a metre of it.
struct SwarmOrigin
{
    core::i32 x = 0;
    core::i32 y = 0;
    core::i32 z = 0;
};

[[nodiscard]] SwarmOrigin originNear(const core::DVec3& focus) noexcept
{
    const auto lattice = [](f64 value) {
        return static_cast<core::i32>(std::clamp(std::floor(value / 16.0 + 0.5) * 16.0, -2.0e9, 2.0e9));
    };
    return SwarmOrigin{lattice(focus.x), lattice(focus.y), lattice(focus.z)};
}

void writeSwarmF32(Writer& out, f32 value)
{
    out.u32v(std::bit_cast<u32>(value));
}

// A float off the wire: finite, and within `limit` of zero.
[[nodiscard]] f32 readSwarmF32(Reader& in, f32 limit = 1.0e6f) noexcept
{
    const f32 value = std::bit_cast<f32>(in.u32v());
    return std::isfinite(value) ? std::clamp(value, -limit, limit) : 0.0f;
}

// A message of comings and goings carries its number for the peer; one of
// positions carries its own sequence instead.
void writeSwarmHeader(Writer& out, MessageType type, u32 swarm, u64 tick, const SwarmOrigin& origin, f32 floor,
                      u16 number)
{
    out.u8v(static_cast<u8>(type));
    out.u32v(swarm);
    out.u32v(static_cast<u32>(tick));
    if (type == MessageType::SwarmState)
        out.u16v(number);
    out.u32v(static_cast<u32>(origin.x));
    out.u32v(static_cast<u32>(origin.y));
    out.u32v(static_cast<u32>(origin.z));
    if (type == MessageType::SwarmAgents)
        out.u16v(number);
    writeSwarmF32(out, floor);
}

// Where a replica draws an agent it was last told of, at `tick`: its place
// carried along the direction it faced.
[[nodiscard]] core::DVec3 drawnAt(const core::DVec3& position, f32 faceX, f32 faceZ, f32 walk, u64 told, u64 tick,
                                  f64 dt) noexcept
{
    const f64 ticks = std::min(static_cast<f64>(tick - std::min(told, tick)), scene::SwarmCarryTicks);
    const f64 metres = static_cast<f64>(walk) * ticks * dt;
    return core::DVec3{position.x + static_cast<f64>(faceX) * metres, position.y,
                       position.z + static_cast<f64>(faceZ) * metres};
}

} // namespace

void AuthoritySession::sendSwarms(scene::World& world)
{
    if (m_swarmTick == m_tick)
        return;
    m_swarmTick = m_tick;
    ENG_PROFILE_SCOPE("net.swarms");
    // Collected first, in pool order: a send does not change the pool, but
    // what is walked while a swarm's log is cleared should not be the pool.
    std::vector<InstanceId> swarms;
    world.swarms().forEach([&](InstanceId id, scene::SwarmComponent& swarm) {
        if (swarm.replicates)
            swarms.push_back(id);
        else
            swarm.removed.clear();
    });
    std::vector<core::DVec3> truths;
    for (const InstanceId id : swarms) {
        scene::SwarmComponent* swarm = world.swarms().find(id);
        const NetId netId = netIdOf(id);
        if (swarm == nullptr)
            continue;
        if (netId.valid()) {
            // Where every agent is this tick, once for every peer.
            const f64 dt = world.engineState().fixedTimestep;
            const usize count = std::min<usize>(swarm->agents.size(), 65535);
            truths.resize(count);
            for (usize slot = 0; slot < count; ++slot) {
                // A swarm that is not stepped is where it stands: "where
                // its walk carries it" is a place it never went.
                if (swarm->agents[slot].alive)
                    truths[slot] = swarm->enabled
                                       ? scene::swarmAgentAt(swarm->agents[slot], static_cast<f64>(m_tick), dt)
                                       : swarm->agents[slot].position;
            }
            for (Peer& peer : m_peers) {
                // A peer that has not been told of the swarm has nothing to
                // put its agents in: it is told of them the tick it is.
                if (peer.welcomed && std::binary_search(peer.known.begin(), peer.known.end(), netId.value))
                    sendSwarmTo(world, peer, *swarm, netId.value, truths);
            }
        }
        // Every peer has been told who went.
        swarm->removed.clear();
    }
}

void AuthoritySession::sendSwarmTo(scene::World& world, Peer& peer, scene::SwarmComponent& swarm, u32 netId,
                                   std::span<const core::DVec3> truths)
{
    auto view = std::find_if(peer.swarms.begin(), peer.swarms.end(),
                             [netId](const Peer::SwarmView& held) { return held.netId == netId; });
    if (view == peer.swarms.end()) {
        Peer::SwarmView fresh;
        fresh.netId = netId;
        peer.swarms.push_back(std::move(fresh));
        view = peer.swarms.end() - 1;
    }
    // A number is sixteen bits on the wire: a swarm past that many agents
    // replicates its first 65535.
    const usize count = std::min<usize>(swarm.agents.size(), 65535);
    if (view->agents.size() < count)
        view->agents.resize(count);

    const f64 dt = world.engineState().fixedTimestep;
    const u64 tick = m_tick;
    const scene::PlayerComponent* player = peer.player.valid() ? world.players().find(peer.player) : nullptr;
    const std::optional<core::DVec3> focus = focusOf(world, player);
    const f64 reach = std::clamp(static_cast<f64>(swarm.replicationRadius), 1.0, MaxSwarmReach);
    // **What a replica is told of a swarm that stands**: that it does
    // not walk. An agent's own walk is what it will do when the swarm steps
    // again, and told as it is, a replica would carry a standing agent along
    // it.
    const bool standing = !swarm.enabled;
    const auto walkOf = [standing](const scene::SwarmAgent& agent) { return standing ? 0.0f : agent.walk; };
    const f64 keep = reach * 1.25;

    struct Gone
    {
        u16 slot = 0;
        u8 reason = 0;
        u16 tag = 0;
        core::DVec3 position;
    };
    struct Told
    {
        f32 ratio = 0.0f;
        u32 slot = 0;
        core::DVec3 position;
    };
    std::vector<Gone> gone;
    std::vector<u32> added;
    std::vector<Told> wrong;
    // The lowest of the agents this tick tells of: the floor a lift is
    // measured from where no terrain is under the agent.
    f64 lowest = 1.0e300;

    // **A message nobody acknowledged was lost** (its round trip and half
    // again have passed): what it said, the replica does not have, whatever
    // this end took for sent.
    const f64 wait = std::clamp(peer.swarmAckTicks * 1.5 + 2.0, SwarmAckWaitLeast, SwarmAckWaitMost);
    while (!peer.swarmFlights.empty() &&
           static_cast<f64>(tick - std::min(peer.swarmFlights.front().tick, tick)) > wait) {
        const Peer::SwarmFlight& flight = peer.swarmFlights.front();
        const auto owner = std::find_if(peer.swarms.begin(), peer.swarms.end(),
                                        [&flight](const Peer::SwarmView& held) { return held.netId == flight.netId; });
        if (owner != peer.swarms.end()) {
            for (const u32 slot : flight.slots) {
                // Unless it has been told again since: that is another message's.
                if (slot < owner->agents.size() && owner->agents[slot].known && owner->agents[slot].tick == flight.tick)
                    owner->agents[slot].lost = true;
            }
        }
        peer.swarmFlights.pop_front();
    }

    for (usize slot = 0; slot < view->agents.size(); ++slot) {
        Peer::SwarmSent& sent = view->agents[slot];
        const scene::SwarmAgent* agent = slot < count && swarm.agents[slot].alive ? &swarm.agents[slot] : nullptr;
        // The agent this peer has under that number is no more: removed, and
        // perhaps another in its place already.
        if (sent.known && (agent == nullptr || agent->born != sent.born)) {
            Gone record{static_cast<u16>(slot), scene::SwarmRemovalRemoved, sent.tag, sent.position};
            for (const scene::SwarmRemoved& removed : swarm.removed) {
                if (removed.slot == slot && removed.born == sent.born) {
                    record.tag = removed.tag;
                    record.position = removed.position;
                }
            }
            gone.push_back(record);
            sent = Peer::SwarmSent{};
        }
        if (agent == nullptr)
            continue;
        const core::DVec3& truth = truths[slot];
        f64 distance = 0.0;
        if (focus.has_value()) {
            const core::DVec3 away = truth - *focus;
            distance = std::sqrt(away.x * away.x + away.y * away.y + away.z * away.z);
        }
        if (!sent.known) {
            if (distance > reach)
                continue;
            sent = Peer::SwarmSent{.known = true,
                                   .born = agent->born,
                                   .tag = agent->tag,
                                   .yaw = agent->yaw,
                                   .walk = walkOf(*agent),
                                   .lift = 0.0f,
                                   .tick = tick,
                                   .position = truth,
                                   .faceX = agent->faceX,
                                   .faceZ = agent->faceZ};
            added.push_back(static_cast<u32>(slot));
            continue;
        }
        if (distance > keep) {
            gone.push_back(Gone{static_cast<u16>(slot), scene::SwarmRemovalOutOfReach, agent->tag, truth});
            sent = Peer::SwarmSent{};
            continue;
        }
        // **Not until the peer has taken in the message that brought it**: a
        // number is used again, and until then a position for this agent
        // would move the one the peer still knows by that number. The message
        // that brings it says where it is; it waits a round trip for more.
        if (static_cast<core::i16>(static_cast<u16>(view->membershipTaken - sent.addedIn)) < 0)
            continue;
        // **Told again when the replica would be wrong** -- where it draws the
        // agent from what it was last told, against where the agent is -- by
        // more than a threshold that grows with distance; or when it has not
        // been told for a while, which is what repairs a message the network
        // lost, since nobody says one was.
        const core::DVec3 drawn = drawnAt(sent.position, sent.faceX, sent.faceZ, sent.walk, sent.tick, tick, dt);
        const f64 offX = truth.x - drawn.x;
        const f64 offZ = truth.z - drawn.z;
        // Up and down counts half: a pile's heights change every tick, and
        // nobody reads a pile to the decimetre. On terrain a replica follows
        // the ground itself, so what can be wrong is the lift; off it, it
        // stays at the height it was told.
        const f64 rise = sent.onTerrain ? static_cast<f64>(agent->lift - sent.lift) : truth.y - sent.position.y;
        const f64 off = std::sqrt(offX * offX + offZ * offZ) + 0.5 * std::fabs(rise);
        const f64 far = std::clamp(distance / reach, 0.0, 1.0);
        const f64 error = off / (SwarmNearError + (SwarmFarError - SwarmNearError) * far);
        const f64 turn = turnBetween(agent->yaw, sent.yaw) / (SwarmNearTurn + (SwarmFarTurn - SwarmNearTurn) * far);
        const f64 stale =
            static_cast<f64>(tick - sent.tick) / (SwarmNearRefresh + (SwarmFarRefresh - SwarmNearRefresh) * far);
        f64 ratio = std::max({error, turn, stale});
        if (agent->tag != sent.tag)
            ratio = std::max(ratio, 2.0);
        if (sent.lost)
            ratio = std::max(ratio, 3.0);
        // Told once that it stands, the tick the swarm stops: not when the
        // replica has carried it far enough to be called wrong.
        if (standing && sent.walk != 0.0f)
            ratio = std::max(ratio, 2.0);
        if (ratio >= 1.0)
            wrong.push_back(Told{static_cast<f32>(ratio), static_cast<u32>(slot), truth});
    }

    const SwarmOrigin origin = originNear(focus.value_or(core::DVec3{}));
    for (const Told& told : wrong)
        lowest = std::min(lowest, told.position.y);
    for (const u32 slot : added)
        lowest = std::min(lowest, view->agents[slot].position.y);
    // From the origin, and an eighth of a metre under the lowest, so nothing
    // rounds to below it.
    const f32 floor =
        lowest < 1.0e299 ? static_cast<f32>(std::floor((lowest - static_cast<f64>(origin.y)) * 8.0) / 8.0) : 0.0f;
    const f64 floorY = static_cast<f64>(origin.y) + static_cast<f64>(floor);
    // An agent's lift on the wire: above the terrain where there is one under
    // it, above the floor where there is none.
    const auto liftOf = [&](const core::DVec3& at, bool& onTerrain) {
        const std::optional<f64> terrain = scene::swarmTerrainAt(world, at.x, at.z);
        onTerrain = terrain.has_value();
        return static_cast<f32>(std::max(at.y - terrain.value_or(floorY), 0.0));
    };
    const auto relative = [&origin](const core::DVec3& position) {
        return core::Vec3{static_cast<f32>(position.x - static_cast<f64>(origin.x)),
                          static_cast<f32>(position.y - static_cast<f64>(origin.y)),
                          static_cast<f32>(position.z - static_cast<f64>(origin.z))};
    };

    // --- Who came and went: reliable, the goings first.
    usize goneAt = 0;
    usize addedAt = 0;
    while (goneAt < gone.size() || addedAt < added.size()) {
        Writer out;
        view->membership = static_cast<u16>(view->membership + 1);
        writeSwarmHeader(out, MessageType::SwarmAgents, netId, tick, origin, floor, view->membership);
        const usize goneCount = std::min(gone.size() - goneAt, SwarmAgentsPerMessage);
        out.u16v(static_cast<u16>(goneCount));
        for (usize at = goneAt; at < goneAt + goneCount; ++at) {
            const core::Vec3 where = relative(gone[at].position);
            out.u16v(gone[at].slot);
            out.u8v(gone[at].reason);
            out.u16v(gone[at].tag);
            writeSwarmF32(out, where.x);
            writeSwarmF32(out, where.y);
            writeSwarmF32(out, where.z);
        }
        // The comings of this message wait for every going to have gone: a
        // number is used again only after its agent went.
        goneAt += goneCount;
        const usize addedCount = goneAt >= gone.size() ? std::min(added.size() - addedAt, SwarmAgentsPerMessage) : 0;
        out.u16v(static_cast<u16>(addedCount));
        for (usize at = addedAt; at < addedAt + addedCount; ++at) {
            const scene::SwarmAgent& agent = swarm.agents[added[at]];
            Peer::SwarmSent& sent = view->agents[added[at]];
            const core::Vec3 where = relative(sent.position);
            sent.lift = liftOf(sent.position, sent.onTerrain);
            sent.addedIn = view->membership;
            out.u16v(static_cast<u16>(added[at]));
            out.u16v(agent.tag);
            out.u8v(static_cast<u8>(std::clamp(static_cast<f64>(agent.radius) * 16.0 + 0.5, 0.0, 255.0)));
            out.u8v(static_cast<u8>(std::clamp(static_cast<f64>(agent.height) * 8.0 + 0.5, 0.0, 255.0)));
            writeSwarmF32(out, where.x);
            writeSwarmF32(out, where.y);
            writeSwarmF32(out, where.z);
            writeSwarmF32(out, sent.lift);
            out.u8v(packYaw(agent.yaw));
            out.u8v(packWalk(walkOf(agent)));
        }
        addedAt += addedCount;
        (void)sendBytes(m_transport, peer.id, out.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        m_stats.swarmRemoved += goneCount;
        m_stats.swarmAdded += addedCount;
    }

    // --- Where the ones it would draw wrong are: what the budget lets through,
    // the most wrong first, then in rising number for the wire.
    // By the ticks since it was last sent to: an authority that sends every
    // second or third tick has the same budget a second as one that sends
    // every tick.
    const f64 elapsed = std::clamp(static_cast<f64>(tick - std::min(view->budgetTick, tick)), 1.0, 8.0);
    view->budgetTick = tick;
    view->budget = std::min(view->budget + SwarmBytesPerTick * elapsed, SwarmByteBurst);
    m_stats.swarmWanted += wrong.size();
    if (wrong.empty() || view->budget < 32.0)
        return;
    constexpr f64 AgentBytes = 6.5;
    const auto fits = static_cast<usize>(view->budget / AgentBytes);
    if (wrong.size() > fits) {
        std::partial_sort(
            wrong.begin(), wrong.begin() + static_cast<std::ptrdiff_t>(fits), wrong.end(),
            [](const Told& a, const Told& b) { return a.ratio != b.ratio ? a.ratio > b.ratio : a.slot < b.slot; });
        wrong.resize(fits);
    }
    std::sort(wrong.begin(), wrong.end(), [](const Told& a, const Told& b) { return a.slot < b.slot; });

    Writer out;
    std::vector<u8> body;
    std::vector<u32> carried;
    core::i64 last = -1;
    const auto flush = [&] {
        if (body.empty())
            return;
        out.bytes.clear();
        peer.swarmSequence = static_cast<u16>(peer.swarmSequence + 1);
        writeSwarmHeader(out, MessageType::SwarmState, netId, tick, origin, floor, peer.swarmSequence);
        out.u16v(static_cast<u16>(body.size()));
        out.bytes.insert(out.bytes.end(), body.begin(), body.end());
        // **Not sequenced**: every agent in a message is ordered by its own
        // tick where it arrives, and a message that arrives after a later one
        // still says something about the agents the later one did not name.
        // Sequenced, a link whose packets swap places -- twenty milliseconds
        // of jitter at sixty a second -- loses a third of them to the rule.
        if (sendBytes(m_transport, peer.id, out.bytes, net::Delivery::Unreliable, SwarmChannel, m_stats))
            m_stats.swarmBytes += out.bytes.size();
        view->budget -= static_cast<f64>(out.bytes.size());
        // On its way, until the replica says it arrived. Bounded: a replica
        // that acknowledges nothing is told everything again and again, and
        // what is kept of that is the last four seconds.
        if (peer.swarmFlights.size() >= 1024)
            peer.swarmFlights.pop_front();
        peer.swarmFlights.push_back(Peer::SwarmFlight{peer.swarmSequence, netId, tick, std::move(carried)});
        carried.clear();
        body.clear();
        last = -1;
    };
    for (const Told& told : wrong) {
        const scene::SwarmAgent& agent = swarm.agents[told.slot];
        Peer::SwarmSent& sent = view->agents[told.slot];
        // Twelve bits an axis of an eighth of a metre, from the origin less
        // 256 m. An agent past that -- a focus that jumped -- waits a tick.
        const f64 x = (told.position.x - static_cast<f64>(origin.x) + 256.0) * 8.0 + 0.5;
        const f64 z = (told.position.z - static_cast<f64>(origin.z) + 256.0) * 8.0 + 0.5;
        if (!(x >= 0.0 && x < 4096.0 && z >= 0.0 && z < 4096.0))
            continue;
        if (body.size() + 13 > SwarmStateBytes - 29)
            flush();
        carried.push_back(told.slot);
        u32 step = static_cast<u32>(static_cast<core::i64>(told.slot) - last);
        last = static_cast<core::i64>(told.slot);
        while (step >= 0x80u) {
            body.push_back(static_cast<u8>(step | 0x80u));
            step >>= 7;
        }
        body.push_back(static_cast<u8>(step));
        const u32 packed = (static_cast<u32>(x) << 12) | static_cast<u32>(z);
        body.push_back(static_cast<u8>(packed));
        body.push_back(static_cast<u8>(packed >> 8));
        body.push_back(static_cast<u8>(packed >> 16));
        const u8 yaw = packYaw(agent.yaw);
        const u8 walk = packWalk(walkOf(agent));
        bool onTerrain = false;
        const f64 liftEighths =
            std::clamp(static_cast<f64>(liftOf(told.position, onTerrain)) * 8.0 + 0.5, 0.0, 65535.0);
        const auto lift = static_cast<u32>(liftEighths);
        const bool tagged = agent.tag != sent.tag;
        body.push_back(yaw);
        body.push_back(static_cast<u8>(walk | (lift != 0 ? 0x40u : 0u) | (tagged ? 0x80u : 0u)));
        if (lift != 0) {
            if (lift < 255) {
                body.push_back(static_cast<u8>(lift));
            }
            else {
                body.push_back(255);
                body.push_back(static_cast<u8>(lift));
                body.push_back(static_cast<u8>(lift >> 8));
            }
        }
        if (tagged) {
            body.push_back(static_cast<u8>(agent.tag));
            body.push_back(static_cast<u8>(agent.tag >> 8));
        }
        // What the replica now draws it from: what it was sent, as it reads it.
        sent.position = core::DVec3{static_cast<f64>(origin.x) - 256.0 + static_cast<f64>(static_cast<u32>(x)) / 8.0,
                                    told.position.y,
                                    static_cast<f64>(origin.z) - 256.0 + static_cast<f64>(static_cast<u32>(z)) / 8.0};
        sent.yaw = unpackYaw(yaw);
        sent.faceX = -std::sin(sent.yaw);
        sent.faceZ = -std::cos(sent.yaw);
        sent.walk = static_cast<f32>(walk) / 4.0f;
        sent.lift = onTerrain ? agent.lift : static_cast<f32>(lift) / 8.0f;
        sent.onTerrain = onTerrain;
        sent.tag = agent.tag;
        sent.tick = tick;
        sent.lost = false;
        m_stats.swarmStates += 1;
    }
    flush();
}

void AuthoritySession::sendMessages(scene::World& world)
{
    // **The crowd's comings and goings first** (ADR 0162): a game's own
    // messages of this tick name agents by number, and arrive behind the
    // message that says which agent a number is.
    sendSwarms(world);
    std::vector<scene::RemoteMessage> outbox;
    outbox.swap(world.engineState().remoteOutbox);

    // What each peer was already waiting on goes first, in order, as far as
    // it now knows the events named: a message never overtakes an earlier one.
    //
    // **Aged by when each was held, not by how long it waited at the front**:
    // counted at the front, a queue of k messages behind one the peer never
    // learns about waited k times the limit, and grew for as long as a script
    // kept firing. Held in order, so the front is always the oldest.
    const auto dropFront = [&](Peer& peer) {
        peer.heldBytes -= peer.held.front().bytes.size();
        peer.held.pop_front();
        m_stats.messagesDropped += 1;
    };
    // **In order per remote, not across them**: one message to an event
    // this peer does not hold -- inside a model out of its interest, say --
    // held every message to every other event behind it, for up to five
    // seconds. What waits for an event now holds back only what follows it to
    // the same event.
    const auto flush = [&](Peer& peer) {
        peer.flushes += 1;
        std::vector<u32> waiting;
        for (auto at = peer.held.begin(); at != peer.held.end();) {
            const bool known = std::binary_search(peer.known.begin(), peer.known.end(), at->remote);
            const bool behind = std::find(waiting.begin(), waiting.end(), at->remote) != waiting.end();
            if (known && !behind) {
                sendBytes(m_transport, peer.id, at->bytes, net::Delivery::Reliable, ControlChannel, m_stats);
                m_stats.messagesSent += 1;
                peer.heldBytes -= at->bytes.size();
                at = peer.held.erase(at);
                continue;
            }
            if (!known && !behind && peer.flushes - at->heldAt > MaxRemoteHeldSends) {
                peer.heldBytes -= at->bytes.size();
                at = peer.held.erase(at);
                m_stats.messagesDropped += 1;
                continue;
            }
            waiting.push_back(at->remote);
            ++at;
        }
    };
    for (Peer& peer : m_peers) {
        if (peer.welcomed)
            flush(peer);
    }

    for (scene::RemoteMessage& message : outbox) {
        if (message.toServer)
            continue; // an authority's own FireServer never left it
        const NetId remote = netIdOf(message.remote);
        if (message.unreliable) {
            // **Sent once or not at all** (ADR 0161): an event with no
            // network id yet, or one a peer has not been told of, is not
            // waited for -- the next message is on its way.
            if (!remote.valid()) {
                m_stats.unreliableDropped += 1;
                continue;
            }
            std::vector<u32> named;
            named.reserve(message.refs.size());
            for (const InstanceId ref : message.refs)
                named.push_back(netIdOf(ref).value);
            for (Peer& peer : m_peers) {
                if (!peer.welcomed || (message.userId != 0 && !peer.has(message.userId)))
                    continue;
                if (!std::binary_search(peer.known.begin(), peer.known.end(), remote.value)) {
                    m_stats.unreliableDropped += 1;
                    continue;
                }
                Writer out;
                peer.unreliableOut = static_cast<u16>(peer.unreliableOut + 1);
                writeUnreliable(out, MessageType::UnreliableToReplica, peer.unreliableOut, remote.value, message, named,
                                peer.seatOf(message.userId));
                if (sendBytes(m_transport, peer.id, out.bytes, net::Delivery::UnreliableSequenced, RemoteChannel,
                              m_stats))
                    m_stats.unreliableSent += 1;
                else
                    m_stats.unreliableDropped += 1;
            }
            continue;
        }
        if (!remote.valid()) {
            // Created since the last capture: no network id yet. It waits for
            // the next send that captures, a few ticks at most.
            if (world.alive(message.remote) && ++message.held <= MaxRemoteHeldSends)
                world.engineState().remoteOutbox.push_back(std::move(message));
            else
                m_stats.messagesDropped += 1;
            continue;
        }
        std::vector<u32> refs;
        refs.reserve(message.refs.size());
        for (const InstanceId ref : message.refs)
            refs.push_back(netIdOf(ref).value);
        // For one player it goes to the machine that player is at, saying
        // which of its players it is for (protocol 48).
        u8 seat = 0;
        for (const Peer& peer : m_peers) {
            if (peer.has(message.userId))
                seat = peer.seatOf(message.userId);
        }
        Writer out;
        writeRemote(out, MessageType::RemoteToReplica, remote.value, message, refs, seat);
        for (Peer& peer : m_peers) {
            if (!peer.welcomed || (message.userId != 0 && !peer.has(message.userId)))
                continue;
            const bool waiting = std::any_of(peer.held.begin(), peer.held.end(),
                                             [&](const Peer::Held& held) { return held.remote == remote.value; });
            if (!waiting && std::binary_search(peer.known.begin(), peer.known.end(), remote.value)) {
                sendBytes(m_transport, peer.id, out.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
                m_stats.messagesSent += 1;
            }
            else {
                while (!peer.held.empty() &&
                       (peer.held.size() >= MaxHeldMessages || peer.heldBytes + out.bytes.size() > MaxHeldBytes))
                    dropFront(peer);
                peer.heldBytes += out.bytes.size();
                peer.held.push_back(Peer::Held{remote.value, peer.flushes, out.bytes});
            }
        }
    }
}

bool AuthoritySession::removePlayer(scene::World& world, InstanceId root, u32 userId, std::string_view reason)
{
    const auto found =
        std::find_if(m_peers.begin(), m_peers.end(), [userId](const Peer& peer) { return peer.has(userId); });
    if (found == m_peers.end())
        return false;
    const InstanceId players = scene::networkServiceOf(world, world.parentOf(root));
    // **A guest removed leaves its seat, and the machine stays** (protocol
    // 48): the others at that screen are still playing.
    if (const u8 seat = found->seatOf(userId); seat != 0) {
        Peer::Guest& guest = found->guests[seat - 1u];
        if (guest.player.valid())
            scene::removePlayer(world, players, guest.player);
        guest = Peer::Guest{};
        Writer emptied;
        emptied.u8v(static_cast<u8>(MessageType::GuestSeat));
        emptied.u8v(seat);
        emptied.u32v(0);
        (void)sendBytes(m_transport, found->id, emptied.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        (void)reason;
        return true;
    }
    Writer refused;
    refused.u8v(static_cast<u8>(MessageType::Refused));
    refused.u8v(RefusedRemoved);
    refused.text(reason.substr(0, MaxRemovedReasonBytes));
    (void)sendBytes(m_transport, found->id, refused.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
    // As a peer that left is taken out: its player, then the peer -- and the
    // token it would come back with is nobody's.
    const InstanceId network = scene::networkServiceOf(world, world.parentOf(root));
    if (found->player.valid())
        scene::removePlayer(world, network, found->player);
    // And everybody else at its screen goes with it.
    for (const Peer::Guest& guest : found->guests) {
        if (guest.player.valid())
            scene::removePlayer(world, network, guest.player);
    }
    m_transport.disconnect(found->id);
    m_peers.erase(found);
    return true;
}

void AuthoritySession::receive(scene::World& world, InstanceId root, bool ticking)
{
    // Where players live. A world with no `NetworkService` -- a test's bare
    // tree -- still replicates; it just has nobody to name.
    const InstanceId network = scene::networkServiceOf(world, world.parentOf(root));
    // **A host is a player too** (ADR 0224): its own players are its key, as
    // a joiner's are theirs -- the first the machine's, a guest the machine's
    // and its seat.
    if (const std::optional<std::array<u8, 32>> own = m_transport.localKey(); own.has_value()) {
        std::vector<InstanceId> here;
        scene::localPlayers(world, here);
        for (std::size_t seat = 0; seat < here.size(); ++seat) {
            scene::PlayerComponent* player = world.players().find(here[seat]);
            if (player != nullptr && player->publicKey.empty())
                player->publicKey = seat == 0 ? core::toHex(*own) : guestKeyOf(*own, static_cast<u8>(seat));
        }
    }
    // **A connection that never says hello is let go**: it holds a slot, and
    // a slot is what the next player needs.
    // The per-tick limits count from one tick to the next, whatever the
    // frames in between did.
    std::vector<net::PeerId> silent;
    for (Peer& peer : m_peers) {
        if (!ticking)
            continue;
        peer.messagesThisTick = 0;
        peer.intentsThisTick = 0;
        peer.messageBudget = std::min(peer.messageBudget + RemoteMessagesPerTick, RemoteMessageBurst);
        peer.byteBudget = std::min(peer.byteBudget + RemoteBytesPerTick, RemoteByteBurst);
        peer.floodTicks = peer.floodedThisTick ? peer.floodTicks + 1 : 0;
        peer.floodedThisTick = false;
        if (peer.welcomed && peer.floodTicks > FloodTicks) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"user", static_cast<core::i64>(peer.userId)}};
            core::log(core::LogLevel::Warn, ENG_TR("net.warn.peer_flooding"), args);
            silent.push_back(peer.id);
            continue;
        }
        peer.intentBudget = std::min(peer.intentBudget + IntentBudgetPerTick, MaxIntentBurst);
        // A welcomed peer silent for ten seconds is gone, as the transport
        // would have said before its thread kept a frozen process answering.
        if (peer.welcomed && ++peer.quietTicks > SilentPeerTicks)
            silent.push_back(peer.id);
        peer.lateIntentCounted = false;
        peer.ownedThisTick = 0;
        peer.remoteBytesThisTick = 0;
        // One the game is still deciding about waits for its word, a while.
        if (!peer.welcomed && peer.asking == 0 && ++peer.unwelcomedReceives > MaxUnwelcomedReceives)
            silent.push_back(peer.id);
        if (!peer.welcomed && peer.asking != 0 && ++peer.askReceives > MaxAskReceives &&
            !m_answers.contains(peer.asking))
            m_answers[peer.asking] = JoinAnswer{.admit = false, .reason = {}, .accountId = {}};
    }
    settleAsks(world, network);
    for (const net::PeerId gone : silent) {
        // A player whose connection went silent leaves, as one whose
        // connection closed does.
        if (const Peer* leaving = peerFor(gone); leaving != nullptr) {
            if (leaving->player.valid())
                scene::removePlayer(world, network, leaving->player);
            // A machine takes its guests with it (protocol 48).
            for (const Peer::Guest& guest : leaving->guests) {
                if (guest.player.valid())
                    scene::removePlayer(world, network, guest.player);
            }
        }
        m_transport.drop(gone);
        std::erase_if(m_peers, [&](const Peer& peer) { return peer.id == gone; });
    }
    std::vector<net::TransportEvent> events;
    (void)m_transport.poll(events, 0);
    m_receives += 1;
    for (const net::TransportEvent& event : events) {
        switch (event.kind) {
        case net::TransportEvent::Kind::Connected:
            if (peerFor(event.peer) == nullptr) {
                Peer peer;
                peer.id = event.peer;
                peer.address = m_transport.addressOf(event.peer);
                // **A flood, by its rate** (`MaxConnectsPerAddressPerSecond`,
                // `MaxHandshakesPerAddress`): let go before it costs a hello.
                if (peer.address != 0) {
                    std::deque<u64>& begun = m_connectsByAddress[peer.address];
                    while (!begun.empty() && begun.front() + 60 <= m_receives)
                        begun.pop_front();
                    begun.push_back(m_receives);
                    u32 waiting = 0;
                    for (const Peer& other : m_peers)
                        waiting += other.address == peer.address && !other.welcomed ? 1u : 0u;
                    if (begun.size() > MaxConnectsPerAddressPerSecond || waiting >= MaxHandshakesPerAddress) {
                        m_stats.messagesDropped += 1;
                        m_transport.drop(event.peer);
                        break;
                    }
                }
                m_peers.push_back(std::move(peer));
                std::sort(m_peers.begin(), m_peers.end(),
                          [](const Peer& a, const Peer& b) { return a.id.value < b.id.value; });
            }
            break;
        case net::TransportEvent::Kind::Disconnected:
            if (Peer* gone = peerFor(event.peer); gone != nullptr) {
                if (gone->player.valid())
                    scene::removePlayer(world, network, gone->player);
                for (const Peer::Guest& guest : gone->guests) {
                    if (guest.player.valid())
                        scene::removePlayer(world, network, guest.player);
                }
            }
            std::erase_if(m_peers, [&](const Peer& peer) { return peer.id == event.peer; });
            break;
        case net::TransportEvent::Kind::Message: {
            Peer* peer = peerFor(event.peer);
            if (peer == nullptr)
                break;
            peer->quietTicks = 0;
            m_stats.bytesReceived += event.payload.size();
            countBytes(m_stats, event.payload);
            Reader reader(event.payload);
            const auto type = static_cast<MessageType>(reader.u8v());
            if (event.channel == IntentChannel && type == MessageType::Intent && peer->welcomed) {
                scene::PlayerComponent* player = peer->player.valid() ? world.players().find(peer->player) : nullptr;
                if (player == nullptr)
                    break;
                // One a tick is what a replica sends; a few more is a burst
                // after a stall. Past that, and past a count no input map
                // has, it is a peer making the authority parse.
                if (peer->intentBudget == 0) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                peer->intentBudget -= 1;
                peer->intentsThisTick += 1;
                // **Whether its clock moved**: how many times it has
                // dropped simulated time, which changes only when it did.
                const u32 epoch = reader.u32v();
                // What its machine draws everyone else at (ADR 0235).
                const u16 drawn = reader.u16v();
                // **Up to four ticks, oldest first** (protocol 22): read whole,
                // then queued -- a tick already applied or already queued is a
                // redundant copy, and nothing.
                const u8 ticks = reader.u8v();
                if (!reader.ok() || ticks == 0 || ticks > IntentRedundancy) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                std::vector<std::pair<u64, std::vector<scene::PlayerIntent>>> carried;
                std::vector<Peer::UnnamedIntent> unnamed;
                // A tick's intents as they were written: the action's number
                // and what it reads. Kept from one tick of the message to the
                // next, for the one that says "the same".
                std::vector<std::pair<u16, scene::PlayerIntent>> written;
                u64 reached = 0;
                for (u8 slot = 0; slot < ticks && reader.ok(); ++slot) {
                    // The first by its tick, each after by how far on it is.
                    if (slot == 0) {
                        reached = reader.u64v();
                    }
                    else {
                        const u8 after = reader.u8v();
                        if (after == 0)
                            reader.fail();
                        reached += after;
                    }
                    const u64 tick = reached;
                    std::vector<scene::PlayerIntent> intents;
                    const u16 count = reader.u16v();
                    if (!reader.ok() || (count == SameIntents ? slot == 0 : count > MaxIntentEntries)) {
                        reader.fail();
                        break;
                    }
                    if (count != SameIntents) {
                        written.clear();
                        written.reserve(count);
                        // A tick begins with the machine's first player's.
                        u8 seat = 0;
                        for (u16 at = 0; at < count && reader.ok(); ++at) {
                            const u16 id = reader.u16v();
                            const u8 bits = reader.u8v();
                            // **Whose the entries after it are** (protocol
                            // 48): one of the machine's other players.
                            if (id == IntentSeat) {
                                if (bits == 0 || bits > peer->guests.size())
                                    reader.fail();
                                seat = bits;
                                continue;
                            }
                            scene::PlayerIntent intent;
                            intent.seat = seat;
                            intent.type = static_cast<core::i32>(bits & IntentTypeMask);
                            intent.pressed = (bits & IntentPressed) != 0;
                            for (int axis = 0; axis < 3; ++axis) {
                                if ((bits & (IntentAxisX << axis)) != 0)
                                    (&intent.axis.x)[axis] = floatOf(reader.u32v());
                            }
                            written.emplace_back(id, intent);
                        }
                        if (!reader.ok())
                            break;
                    }
                    intents.reserve(written.size());
                    for (const auto& [id, read] : written) {
                        scene::PlayerIntent intent = read;
                        // **A peer's numbers, made safe**: a
                        // non-finite axis is none, and every axis is bounded --
                        // a direction is about 1, a pointer a few thousand pixels.
                        intent.axis = core::sanitize(intent.axis, 1e6f);
                        // `Enum.InputActionType`: Bool to ViewportPosition, 0 to 4.
                        if (intent.type < 0 || intent.type > 4)
                            continue;
                        // A seat nobody sits in: a guest that has just left,
                        // its last intents still on their way.
                        if (intent.seat != 0 && peer->guests[intent.seat - 1u].userId == 0)
                            continue;
                        // **A number this connection's `IntentNames` gave**
                        // (protocol 37). One whose name has not come yet -- the
                        // names travel reliably, on another channel -- waits
                        // for it rather than being lost with its press.
                        if (id >= peer->intentNames.size() || peer->intentNames[id].empty()) {
                            if (id < MaxIntentNames)
                                unnamed.push_back(Peer::UnnamedIntent{tick, id, intent});
                            continue;
                        }
                        intent.action = intentAtom(world, *peer, id);
                        if (!intent.action.valid())
                            continue;
                        intents.push_back(intent);
                    }
                    carried.emplace_back(tick, std::move(intents));
                }
                // Whole or not at all: half an intent is a player whose second
                // key was released by a truncated packet.
                if (!reader.ok() || !reader.done()) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                for (const Peer::UnnamedIntent& each : unnamed) {
                    if (peer->unnamedIntents.size() >= MaxUnnamedIntents)
                        break;
                    peer->unnamedIntents.push_back(each);
                }
                // **How far behind it draws the world** (ADR 0235): from the
                // tick it was drawing to the newest this authority has run,
                // which is when it heard. Every seat at the machine sees one
                // screen's world.
                if (const u64 heard = world.engineState().tick; drawn != 0) {
                    // Its low sixteen bits: the tick at or before this one
                    // that has them.
                    const u64 behind = static_cast<u16>(static_cast<u16>(heard & 0xFFFFu) - drawn);
                    for (u8 seat = 0; seat <= peer->guests.size(); ++seat) {
                        if (scene::PlayerComponent* seated = world.players().find(peer->playerIn(seat));
                            seated != nullptr) {
                            seated->viewBehind = std::min<u64>(behind, MaxViewBehindTicks);
                            seated->viewKnown = true;
                        }
                    }
                }
                // **Intents that are late whole, tick after tick, are a clock
                // that moved and not a packet that was slow**. A replica
                // that dropped simulated time after a long frame numbers its
                // ticks that much behind where this authority has got to, and
                // so does one whose packets now take longer than the queue has
                // room for: every intent it sends from then on is "too late
                // for its tick", for the rest of the session, and the last one
                // applied -- whatever was held at that moment -- stood in for
                // ever. There was a catch-up for a peer that got AHEAD and
                // none for one that fell BEHIND.
                //
                // So the stream is anchored again on the newest tick the peer
                // has sent, exactly as it was on its first: applied
                // `intentDelay` ticks from now, the ticks before it forgotten.
                // What that newest intent holds stands in until then -- it is
                // the newest thing the player is known to be doing.
                //
                // **Two ways to tell.** A replica that dropped simulated time
                // says so: its count of drops has moved, and a message
                // late after that is its clock, and the stream is anchored
                // again at once. **Lateness alone never was** -- a burst held
                // up by a busy machine is late by as many ticks, and anchored
                // on it the authority stepped again ticks it had already stood
                // in for, and the replica was corrected for each. That rule
                // is gone: a late burst of the same clock is dropped, its
                // ticks already stood in for with what they hold. A window in the background runs ten frames a
                // second and sends six ticks together, so waiting for late
                // ticks in a row there was waiting four frames: measured on a
                // real window, 18 to 47 ticks without one real intent after
                // each long frame, and the character stood still for up to 26
                // of them. Late by less -- a replica that runs a little slow,
                // a route a little longer -- is counted by tick, four in a
                // row, so that a burst a slow route delivers at once is one
                // late tick and the packets sent after it are on time again.
                u64 newestCarried = 0;
                for (const auto& [tick, intents] : carried)
                    newestCarried = std::max(newestCarried, tick);
                peer->silentTicks = 0;
                const bool clockMoved = peer->timeEpochKnown && epoch != peer->timeEpoch;
                peer->timeEpoch = epoch;
                peer->timeEpochKnown = true;
                const bool late = peer->intentStarted && newestCarried <= peer->appliedTick;
                if (!late) {
                    peer->lateIntentTicks = 0;
                }
                else if (!peer->lateIntentCounted) {
                    peer->lateIntentCounted = true;
                    peer->lateIntentTicks += 1;
                }
                if (late && (peer->lateIntentTicks >= IntentRedundancy || clockMoved)) {
                    peer->lateIntentTicks = 0;
                    peer->intentQueue.clear();
                    peer->standIns.clear();
                    peer->anchoredAgain = true;
                    // Every tick of this message that was never seen, not only
                    // its newest: a button tapped in one of them and let go by
                    // the newest is a press the start carries forward.
                    peer->anchorFloor = peer->newestIntentSeen;
                    for (auto& [tick, intents] : carried) {
                        if (tick > peer->anchorFloor)
                            peer->intentQueue.emplace(tick, std::move(intents));
                    }
                    peer->intentStarted = false;
                    // The ticks that ran dry were the clock moving: they say
                    // nothing of how unevenly this peer's packets arrive.
                    if (peer->standInRun > 0)
                        peer->intentDelay = peer->delayBeforeRun;
                    peer->standInRun = 0;
                    peer->ticksSinceStarved = 0;
                    peer->newestIntentSeen = newestCarried;
                    m_stats.intentReanchors += 1;
                    break;
                }
                const u64 seenBefore = peer->newestIntentSeen;
                peer->newestIntentSeen = std::max(peer->newestIntentSeen, newestCarried);
                for (auto& [tick, intents] : carried) {
                    if (peer->intentStarted && tick <= peer->appliedTick) {
                        // Too late for its tick. If that tick was stood in
                        // for, a press it held that the stand-in did not is
                        // carried into the next tick rather than lost.
                        //
                        // **And a tick this authority has never seen is one it
                        // never applied**, whether or not it remembers
                        // what stood in for it: after a long frame the tick a
                        // replica is on is older than any stand-in kept, and a
                        // press made in the ticks before the stream is
                        // anchored again was the jump that did not come out.
                        const auto stood = peer->standIns.find(tick);
                        const bool unseen = stood == peer->standIns.end() && tick > seenBefore;
                        if (stood == peer->standIns.end() && !unseen)
                            continue;
                        for (const scene::PlayerIntent& intent : intents) {
                            if (intent.type != 0 || !intent.pressed)
                                continue;
                            const bool had = !unseen && std::any_of(stood->second.begin(), stood->second.end(),
                                                                    [&](const scene::PlayerIntent& other) {
                                                                        return other.action == intent.action &&
                                                                               other.seat == intent.seat &&
                                                                               other.pressed;
                                                                    });
                            if (!had)
                                carryPress(*peer, intent.seat, intent.action);
                        }
                        if (!unseen)
                            peer->standIns.erase(stood);
                        continue;
                    }
                    if (peer->intentQueue.contains(tick))
                        continue;
                    // Waiting to be anchored again: what was seen before the
                    // clock moved has been applied or stood in for already.
                    if (peer->anchoredAgain && tick <= peer->anchorFloor)
                        continue;
                    peer->intentQueue.emplace(tick, std::move(intents));
                }
                // Bounded: a peer that sends far ahead is caught up by
                // `applyIntents`, and one that floods loses its oldest.
                while (peer->intentQueue.size() > MaxQueuedIntents)
                    peer->intentQueue.erase(peer->intentQueue.begin());
                break;
            }
            if (event.channel == OwnershipChannel && type == MessageType::OwnedState && peer->welcomed) {
                const u64 tick = reader.u64v();
                if (!reader.ok() || tick <= peer->ownedTick || peer->userId == 0)
                    break;
                if (++peer->ownedThisTick > MaxOwnedStatesPerTick) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                const u16 count = reader.u16v();
                if (!reader.ok() || count > MaxOwnedRecords) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                // **Read whole, then applied** -- and the tick taken only from
                // a message that read: a truncated one moved half the parts,
                // and still made the next good one look old.
                struct OwnedRecord
                {
                    scene::PartComponent* part = nullptr;
                    scene::RigidBodyComponent* body = nullptr;
                    core::CFrameD frame;
                    core::Vec3 speed;
                    core::Vec3 spin;
                };
                std::vector<OwnedRecord> records;
                records.reserve(count);
                for (u16 at = 0; at < count && reader.ok(); ++at) {
                    const u32 netId = reader.u32v();
                    FieldValue cframe;
                    FieldValue linear;
                    FieldValue angular;
                    if (!reader.ok() ||
                        !decodeField(reader.bytes(), reader.at(), generated::Encoding::CFrameD, cframe) ||
                        !decodeField(reader.bytes(), reader.at(), generated::Encoding::Vector3, linear) ||
                        !decodeField(reader.bytes(), reader.at(), generated::Encoding::Vector3, angular)) {
                        reader.fail();
                        break;
                    }
                    // **Only what it was given**: a part it does not own now --
                    // never did, or was handed back since -- is not its to move.
                    const InstanceId id = instanceOfNet(world, netId);
                    scene::RigidBodyComponent* body = id.valid() ? world.rigidBodies().find(id) : nullptr;
                    scene::PartComponent* part = id.valid() ? world.parts().find(id) : nullptr;
                    if (body == nullptr || part == nullptr || !peer->has(body->networkOwner))
                        continue;
                    // **Where an owner says its part is, only if a world can
                    // hold it**: a non-finite value would corrupt
                    // this simulation and replicate to everyone, and a
                    // rotation that is not one is a body the solver cannot
                    // reason about.
                    const core::CFrameD frame = asCFrame(cframe);
                    const core::Vec3 speed = asVec3(linear);
                    const core::Vec3 spin = asVec3(angular);
                    if (!core::isWorldPosition(frame.position) || !core::isRotation(frame.rotation) ||
                        !core::isFinite(speed) || !core::isFinite(spin))
                        continue;
                    // **Within reach of where it is**: ownership was a
                    // licence to put the part anywhere in the world in one
                    // message, the authority taking it back a tick after.
                    // **Followed at the reach, never refused**: a refusal is
                    // for good -- the owner never hears it, moves on from
                    // where it is, and every place after is further still --
                    // and one owner whose part began in the wrong place had
                    // its kart held at the grid for the whole race.
                    const u64 elapsed = peer->ownedTick != 0 ? std::min<u64>(tick - peer->ownedTick, 30) : 1;
                    const core::DVec3 moved = frame.position - part->cframe.position;
                    const core::f64 reach =
                        MaxOwnedMetresPerTick * static_cast<core::f64>(std::max<u64>(elapsed, 1)) + 1.0;
                    const core::f64 distance = std::sqrt(moved.x * moved.x + moved.y * moved.y + moved.z * moved.z);
                    core::CFrameD reached = frame;
                    if (distance > reach) {
                        const core::f64 scale = reach / distance;
                        reached.position =
                            part->cframe.position + core::DVec3{moved.x * scale, moved.y * scale, moved.z * scale};
                        m_stats.ownedClamped += 1;
                    }
                    records.push_back(OwnedRecord{part, body, reached, speed, spin});
                }
                if (!reader.ok() || !reader.done()) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                peer->ownedTick = tick;
                // And no faster than it may move.
                constexpr core::f32 MaxOwnedSpeed = static_cast<core::f32>(MaxOwnedMetresPerTick * 60.0);
                for (const OwnedRecord& record : records) {
                    record.part->cframe = record.frame;
                    record.body->linearVelocity = core::sanitize(record.speed, MaxOwnedSpeed);
                    record.body->angularVelocity = core::sanitize(record.spin, core::MaxSpeed);
                }
                break;
            }
            if (event.channel == SwarmChannel && type == MessageType::SwarmAck && peer->welcomed) {
                // **Which messages of positions arrived** (ADR 0162): each is
                // forgotten, and how long it took is how long the next is
                // waited for.
                const u8 count = reader.u8v();
                for (u8 at = 0; at < count && reader.ok(); ++at) {
                    const u16 sequence = reader.u16v();
                    const auto flight =
                        std::find_if(peer->swarmFlights.begin(), peer->swarmFlights.end(),
                                     [sequence](const Peer::SwarmFlight& held) { return held.sequence == sequence; });
                    if (flight == peer->swarmFlights.end())
                        continue;
                    const f64 took = static_cast<f64>(m_tick - std::min(flight->tick, m_tick));
                    peer->swarmAckTicks += (took - peer->swarmAckTicks) * 0.1;
                    peer->swarmFlights.erase(flight);
                }
                const u8 swarms = reader.u8v();
                for (u8 at = 0; at < swarms && reader.ok(); ++at) {
                    const u32 swarm = reader.u32v();
                    const u16 taken = reader.u16v();
                    for (Peer::SwarmView& view : peer->swarms) {
                        // Forward only: an acknowledgement that arrives late
                        // says less than one already heard. And never past
                        // what was sent -- a peer is not trusted to count.
                        if (view.netId == swarm && reader.ok() &&
                            static_cast<core::i16>(static_cast<u16>(taken - view.membershipTaken)) > 0 &&
                            static_cast<core::i16>(static_cast<u16>(view.membership - taken)) >= 0)
                            view.membershipTaken = taken;
                    }
                }
                break;
            }
            if (event.channel == RemoteChannel && type == MessageType::UnreliableToAuthority && peer->welcomed &&
                peer->player.valid()) {
                // **From the same budgets a reliable one is** (ADR 0161): a
                // client is not trusted to be polite on either channel.
                if (peer->messageBudget == 0) {
                    m_stats.unreliableDropped += 1;
                    peer->floodedThisTick = true;
                    break;
                }
                peer->messageBudget -= 1;
                peer->messagesThisTick += 1;
                UnreliableOnWire wire;
                if (!readUnreliable(reader, wire)) {
                    m_stats.unreliableDropped += 1;
                    break;
                }
                if (event.payload.size() > peer->byteBudget) {
                    m_stats.unreliableDropped += 1;
                    peer->floodedThisTick = true;
                    break;
                }
                peer->byteBudget -= event.payload.size();
                peer->remoteBytesThisTick += event.payload.size();
                // The newest wins: one that arrives after a later one is dropped.
                if (!newerUnreliable(wire.sequence, peer->unreliableIn, peer->unreliableHeard)) {
                    m_stats.unreliableDropped += 1;
                    break;
                }
                peer->unreliableIn = wire.sequence;
                peer->unreliableHeard = true;
                const InstanceId remote = instanceOfNet(world, wire.remote);
                const InstanceId sender = peer->playerIn(wire.seat);
                if (!isClass(world, remote, "UnreliableRemoteEvent") || !sender.valid()) {
                    m_stats.unreliableDropped += 1;
                    break;
                }
                scene::RemoteMessage message;
                message.remote = remote;
                message.toServer = true;
                message.unreliable = true;
                message.player = sender;
                message.payload.assign(wire.payload.begin(), wire.payload.end());
                for (const u32 ref : wire.refs)
                    message.refs.push_back(ref != 0 ? instanceOfNet(world, ref) : InstanceId{});
                world.engineState().remoteInbox.push_back(std::move(message));
                m_stats.unreliableReceived += 1;
                break;
            }
            // **A replica's word that it holds a snapshot**: on the state
            // channel, where one lost holds nothing up (protocol 43) --
            // and on the control channel, reliably, for the one snapshot
            // nothing is sent after until it is heard: the one sent in parts.
            // The newest heard is kept, so the order they come in is nothing.
            if (type == MessageType::Ack && (event.channel == StateChannel || event.channel == ControlChannel)) {
                const u64 tick = reader.u64v();
                // **Only a tick this peer was sent**: acknowledging one
                // it never was -- the largest number there is, say -- pinned
                // the authority to diffing against nothing.
                const bool sent = tick == peer->pinnedTick || std::find(peer->sentTicks.begin(), peer->sentTicks.end(),
                                                                        tick) != peer->sentTicks.end();
                if (!reader.ok() || !sent) {
                    m_stats.acksRefused += 1;
                }
                else {
                    peer->acked = std::max(peer->acked, tick);
                    if (peer->pinnedPending && peer->acked >= peer->pinnedTick)
                        peer->pinnedPending = false;
                }
                break;
            }
            if (event.channel != ControlChannel)
                break;
            if (type == MessageType::Hello) {
                const u32 version = reader.u32v();
                // **Version first, and a mismatch is a refusal rather than a
                // guess** -- a decoder reading field ids it does not have is a
                // replica confidently wrong about the world.
                PlayerToken token;
                token.high = reader.u64v();
                token.low = reader.u64v();
                const u64 luauFlags = reader.u64v();
                const std::string_view ticket = reader.remaining() >= 2 ? reader.text() : std::string_view{};
                const std::string_view pass = reader.remaining() >= 2 ? reader.text() : std::string_view{};
                // **A player is their key** (ADR 0224): one that proved a key
                // in the handshake is that key's player, whatever token it
                // sent -- the same `UserId` on a reconnect while this server
                // runs, and its `PublicKey` what lasts past that. The token is
                // still this run's own, so a server that restarted is a fresh
                // join to the player's machine (ADR 0085).
                const std::optional<std::array<u8, 32>> key = m_transport.remoteKey(peer->id);
                if (key.has_value()) {
                    const auto known = m_keyTokens.find(*key);
                    token = known != m_keyTokens.end() ? known->second : PlayerToken{};
                }
                // **Other Luau flags are other scripts** (ADR 0217): the same
                // source parsed, compiled or run another way, so a world that
                // parts from this one by the first tick. Said, like a version.
                if (reader.ok() && version == generated::ProtocolVersion && !peer->welcomed &&
                    luauFlags != m_luauFlags) {
                    core::log(core::LogLevel::Info, ENG_TR("net.info.refused_luau_flags"));
                    Writer refused;
                    refused.u8v(static_cast<u8>(MessageType::Refused));
                    refused.u8v(RefusedLuauFlags);
                    refused.text({});
                    (void)sendBytes(m_transport, peer->id, refused.bytes, net::Delivery::Reliable, ControlChannel,
                                    m_stats);
                    m_transport.disconnect(peer->id);
                    break;
                }
                if (!reader.ok() || version != generated::ProtocolVersion || peer->welcomed) {
                    // **Said, not just done**: the replica learns why.
                    if (reader.ok() && !peer->welcomed) {
                        Writer refused;
                        refused.u8v(static_cast<u8>(MessageType::Refused));
                        refused.u8v(RefusedVersion);
                        refused.text({});
                        (void)sendBytes(m_transport, peer->id, refused.bytes, net::Delivery::Reliable, ControlChannel,
                                        m_stats);
                    }
                    m_transport.disconnect(peer->id);
                    break;
                }
                // Asked already, and waiting for the answer: a second hello
                // is nothing.
                if (peer->asking != 0)
                    break;
                if (m_gate) {
                    peer->asking = m_nextAsk++;
                    peer->askedToken = token;
                    JoinAsk ask;
                    ask.id = peer->asking;
                    ask.publicKey = key.has_value() ? core::toHex(*key) : std::string{};
                    ask.address = m_transport.addressText(peer->id);
                    ask.ticket.assign(ticket.substr(0, MaxTicketBytes));
                    ask.pass.assign(pass.substr(0, MaxPassBytes));
                    const net::PeerId asked = peer->id;
                    m_gate(ask);
                    // Answered inside the call -- a ban list -- is settled now,
                    // so a gate costs a joiner no tick.
                    settleAsks(world, network);
                    (void)asked;
                    break;
                }
                admit(world, network, peer->id, token);
            }
            else if ((type == MessageType::GuestJoin || type == MessageType::GuestLeave) && peer->welcomed) {
                // **Somebody else at the same screen** (protocol 48, ADR
                // 0205): a seat asked for, or given back. Against the same
                // budget a remote message is, since each makes the authority
                // work.
                if (peer->messageBudget == 0) {
                    m_stats.messagesDropped += 1;
                    peer->floodedThisTick = true;
                    break;
                }
                peer->messageBudget -= 1;
                const u8 seat = reader.u8v();
                if (!reader.ok() || !reader.done() || seat == 0 || seat > peer->guests.size()) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                Peer::Guest& guest = peer->guests[seat - 1u];
                if (type == MessageType::GuestLeave) {
                    if (guest.player.valid())
                        scene::removePlayer(world, network, guest.player);
                    guest = Peer::Guest{};
                }
                else if (guest.userId == 0 && network.valid()) {
                    // A guest is a player: counted against the match's size
                    // as a connection is.
                    u32 seated = 0;
                    for (const Peer& other : m_peers) {
                        if (!other.welcomed)
                            continue;
                        seated += 1;
                        for (const Peer::Guest& sitting : other.guests)
                            seated += sitting.userId != 0 ? 1u : 0u;
                    }
                    if (m_maxPlayers == 0 || seated < m_maxPlayers) {
                        guest.userId = m_nextUserId++;
                        guest.player = scene::createPlayer(world, network, guest.userId, false);
                        // A guest is its machine's key and its seat: the same
                        // player at that seat next time.
                        if (scene::PlayerComponent* sat = world.players().find(guest.player);
                            sat != nullptr && peer->key.has_value())
                            sat->publicKey = guestKeyOf(*peer->key, seat);
                    }
                }
                // Who sits there now: its number, or nobody.
                Writer answer;
                answer.u8v(static_cast<u8>(MessageType::GuestSeat));
                answer.u8v(seat);
                answer.u32v(guest.userId);
                sendBytes(m_transport, peer->id, answer.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
            }
            else if (type == MessageType::RemoteToAuthority && peer->welcomed && peer->player.valid()) {
                // **The sender is the connection's player**, never anything
                // the message says; and a client is not trusted to be polite.
                if (peer->messageBudget == 0) {
                    m_stats.messagesDropped += 1;
                    peer->floodedThisTick = true;
                    break;
                }
                peer->messageBudget -= 1;
                peer->messagesThisTick += 1;
                RemoteOnWire wire;
                if (!readRemote(reader, wire)) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                if (event.payload.size() > peer->byteBudget) {
                    m_stats.messagesDropped += 1;
                    peer->floodedThisTick = true;
                    break;
                }
                peer->byteBudget -= event.payload.size();
                peer->remoteBytesThisTick += event.payload.size();
                const InstanceId remote = instanceOfNet(world, wire.remote);
                // A client asks and never answers: a reply from one is not a
                // thing, and neither is a failure. **Which of the machine's
                // players** it says, and only that (protocol 48): one of the
                // connection's own seats, or the message is nobody's.
                const u8 seat = static_cast<u8>((wire.flags & RemoteSeatMask) >> RemoteSeatShift);
                const InstanceId sender = peer->playerIn(seat);
                if (!isRemoteTarget(world, remote, wire.call) || (wire.flags & ~RemoteSeatMask) != 0 ||
                    !sender.valid()) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                scene::RemoteMessage message;
                message.remote = remote;
                message.toServer = true;
                message.call = wire.call;
                message.player = sender;
                message.payload.assign(wire.payload.begin(), wire.payload.end());
                for (const u32 ref : wire.refs)
                    message.refs.push_back(ref != 0 ? instanceOfNet(world, ref) : InstanceId{});
                world.engineState().remoteInbox.push_back(std::move(message));
                m_stats.messagesReceived += 1;
            }
            else if (type == MessageType::IntentNames && peer->welcomed) {
                // **The numbers its intents will carry** (protocol 37).
                // A name once given stands: a peer cannot make an action held
                // under one name read as another.
                const u16 count = reader.u16v();
                if (!reader.ok() || count > MaxIntentEntries) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                std::vector<std::pair<u16, std::string_view>> given;
                given.reserve(count);
                for (u16 at = 0; at < count && reader.ok(); ++at) {
                    const u16 id = reader.u16v();
                    const std::string_view name = reader.text();
                    if (reader.ok() && (id >= MaxIntentNames || name.empty() || name.size() > MaxIntentNameBytes))
                        reader.fail();
                    given.emplace_back(id, name);
                }
                if (!reader.ok() || !reader.done()) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                for (const auto& [id, name] : given) {
                    if (id >= peer->intentNames.size()) {
                        peer->intentNames.resize(static_cast<usize>(id) + 1);
                        peer->intentAtoms.resize(static_cast<usize>(id) + 1);
                    }
                    if (peer->intentNames[id].empty())
                        peer->intentNames[id] = std::string(name);
                }
                takeNamedIntents(world, *peer);
                m_stats.messagesReceived += 1;
            }
            else if (type == MessageType::DetectorInput && peer->welcomed && peer->player.valid()) {
                // **The sender is the connection's player** here too; whether
                // it could have pressed the thing is the tick's to check
                // (ADR 0126), against the world as it stands then.
                if (peer->messageBudget == 0) {
                    m_stats.messagesDropped += 1;
                    peer->floodedThisTick = true;
                    break;
                }
                peer->messageBudget -= 1;
                peer->messagesThisTick += 1;
                const u32 netId = reader.u32v();
                const u8 kind = reader.u8v();
                const InstanceId detector = instanceOfNet(world, netId);
                if (!reader.ok() || !reader.done() || !detector.valid() ||
                    kind > static_cast<u8>(scene::DetectorMessage::Kind::RightClick)) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                world.engineState().detectorInbox.push_back(
                    scene::DetectorMessage{detector, peer->player, static_cast<scene::DetectorMessage::Kind>(kind), 0});
                m_stats.messagesReceived += 1;
            }
            break;
        }
        case net::TransportEvent::Kind::None:
            break;
        }
    }
    if (ticking)
        applyIntents(world);
}

// A press to be applied at the next tick: once, however often it is found.
void AuthoritySession::carryPress(Peer& peer, u8 seat, core::NameAtom action)
{
    const bool carried =
        std::any_of(peer.carriedPresses.begin(), peer.carriedPresses.end(),
                    [&](const Peer::CarriedPress& press) { return press.seat == seat && press.action == action; });
    if (!carried)
        peer.carriedPresses.push_back(Peer::CarriedPress{seat, action});
}

core::NameAtom AuthoritySession::intentAtom(scene::World& world, Peer& peer, u16 id)
{
    if (id >= peer.intentNames.size() || peer.intentNames[id].empty())
        return {};
    core::NameAtom& atom = peer.intentAtoms[id];
    // **Looked up, never interned**: the atom table never frees, and a peer
    // naming a new action every packet would grow it for ever. An action
    // nothing on this machine has named -- no `InputAction`, no `GetIntent` --
    // is nothing anybody here reads; looked up again until something does.
    if (!atom.valid())
        atom = world.atoms().lookup(peer.intentNames[id]);
    return atom;
}

void AuthoritySession::takeNamedIntents(scene::World& world, Peer& peer)
{
    std::erase_if(peer.unnamedIntents, [&](Peer::UnnamedIntent& each) {
        if (each.id >= peer.intentNames.size() || peer.intentNames[each.id].empty())
            return peer.intentStarted && each.tick + MaxQueuedIntents < peer.appliedTick;
        each.intent.action = intentAtom(world, peer, each.id);
        if (!each.intent.action.valid())
            return true;
        // Still queued: it is part of its tick, as if it had come named.
        if (const auto queued = peer.intentQueue.find(each.tick); queued != peer.intentQueue.end()) {
            const bool had =
                std::any_of(queued->second.begin(), queued->second.end(), [&](const scene::PlayerIntent& other) {
                    return other.action == each.intent.action && other.seat == each.intent.seat;
                });
            if (!had)
                queued->second.push_back(each.intent);
        }
        // Its tick has gone: a press in it is carried, late and not lost, as a
        // press that came after its tick is.
        else if (each.intent.type == 0 && each.intent.pressed) {
            carryPress(peer, each.intent.seat, each.intent.action);
        }
        return true;
    });
}

void AuthoritySession::applyIntents(scene::World& world)
{
    const auto idle = [](const std::vector<scene::PlayerIntent>& intents) {
        return std::all_of(intents.begin(), intents.end(), [](const scene::PlayerIntent& intent) {
            return !intent.pressed && intent.axis.x == 0.0f && intent.axis.y == 0.0f && intent.axis.z == 0.0f;
        });
    };
    m_stats.intentDepth = 0;
    // A tick's intents, with any press a late intent carried applied on top.
    const auto withCarried = [](Peer& peer) {
        std::vector<scene::PlayerIntent> applied = peer.lastIntents;
        for (const Peer::CarriedPress& press : peer.carriedPresses) {
            auto same = std::find_if(applied.begin(), applied.end(), [&](const scene::PlayerIntent& intent) {
                return intent.action == press.action && intent.seat == press.seat;
            });
            if (same != applied.end())
                same->pressed = true;
            else
                applied.push_back(scene::PlayerIntent{press.action, 0, core::Vec3{}, true, press.seat});
        }
        peer.carriedPresses.clear();
        return applied;
    };
    // **A press in a tick that is passed over is carried, not lost**: the
    // buttons down in `skipped` that the tick taking over from them does not
    // have down, and that were not already down before, go down for one tick.
    // What `target` has down goes down at its own tick, once.
    const auto carryPresses = [](Peer& peer, const std::vector<scene::PlayerIntent>& skipped,
                                 const std::vector<scene::PlayerIntent>* target) {
        const auto down = [](const std::vector<scene::PlayerIntent>& intents, const scene::PlayerIntent& which) {
            return std::any_of(intents.begin(), intents.end(), [&](const scene::PlayerIntent& intent) {
                return intent.action == which.action && intent.seat == which.seat && intent.type == 0 && intent.pressed;
            });
        };
        for (const scene::PlayerIntent& intent : skipped) {
            if (intent.type != 0 || !intent.pressed || down(peer.lastIntents, intent) ||
                (target != nullptr && down(*target, intent)))
                continue;
            carryPress(peer, intent.seat, intent.action);
        }
    };
    // **Each of the machine's players is given its own** (protocol 48, ADR
    // 0205): the stream is one, and what it holds for a tick is dealt by
    // seat. `tick` is the machine's own tick the intents are of, or null on a
    // tick that is held.
    const auto deal = [&world](const Peer& peer, scene::PlayerComponent& first,
                               std::vector<scene::PlayerIntent> applied, const u64* tick) {
        std::array<scene::PlayerComponent*, 3> others{};
        for (usize seat = 0; seat < peer.guests.size(); ++seat) {
            others[seat] = peer.guests[seat].player.valid() ? world.players().find(peer.guests[seat].player) : nullptr;
            if (others[seat] != nullptr) {
                others[seat]->intents.clear();
                if (tick != nullptr)
                    others[seat]->intentTick = *tick;
            }
        }
        first.intents.clear();
        for (scene::PlayerIntent& intent : applied) {
            scene::PlayerComponent* whose = intent.seat == 0 ? &first : others[intent.seat - 1u];
            if (whose != nullptr)
                whose->intents.push_back(intent);
        }
        if (tick != nullptr)
            first.intentTick = *tick;
    };
    for (Peer& peer : m_peers) {
        scene::PlayerComponent* player =
            peer.welcomed && peer.player.valid() ? world.players().find(peer.player) : nullptr;
        if (player == nullptr)
            continue;
        if (peer.intentStarted)
            peer.silentTicks += 1;
        if (!peer.intentStarted) {
            if (peer.intentQueue.empty())
                continue;
            // **Started `intentDelay` ticks behind the first**, so the queue
            // holds that many when the first is applied -- the room arrival
            // jitter needs.
            peer.intentStarted = true;
            if (peer.anchoredAgain) {
                // **Anchored again on the NEWEST tick that has come**: the
                // messages that arrived with the one that showed the clock had
                // moved each carry the three ticks before theirs, and
                // anchoring on the oldest of those would start the stream that
                // much further behind what the player is doing now.
                peer.anchoredAgain = false;
                for (auto at = peer.intentQueue.begin(); at != std::prev(peer.intentQueue.end()); ++at)
                    carryPresses(peer, at->second, &peer.intentQueue.rbegin()->second);
                peer.intentQueue.erase(peer.intentQueue.begin(), std::prev(peer.intentQueue.end()));
                // **What stands in until that tick's own turn is what the
                // player is doing now, less what only just began**: the
                // directions as the newest intent has them, a button still
                // held if it was held before, and a button newly down not yet
                // -- it goes down at its own tick, so one pressed for a tick
                // is pressed for a tick and not for the whole wait.
                std::vector<scene::PlayerIntent> standing = peer.intentQueue.begin()->second;
                for (scene::PlayerIntent& intent : standing) {
                    if (intent.type != 0 || !intent.pressed)
                        continue;
                    intent.pressed = std::any_of(
                        peer.lastIntents.begin(), peer.lastIntents.end(), [&](const scene::PlayerIntent& before) {
                            return before.action == intent.action && before.seat == intent.seat && before.pressed;
                        });
                }
                peer.lastIntents = std::move(standing);
            }
            peer.firstIntentTick = peer.intentQueue.begin()->first;
            peer.appliedTick = peer.firstIntentTick - std::min<u64>(peer.firstIntentTick, 1u + peer.intentDelay);
            // **Anchored again, the answer is in the peer's own ticks again**:
            // while the last intent stood in, the tick a snapshot
            // answered ran ahead of anything the replica had predicted. From
            // here it names the tick the replica was on when the world it is
            // shown was stepped, so the replica corrects from there once and
            // steps its own input again on top.
            if (peer.intentTick != 0)
                peer.intentTick = peer.appliedTick;
        }
        const u64 newest = peer.intentQueue.empty() ? peer.appliedTick : peer.intentQueue.rbegin()->first;
        const u64 depth = newest > peer.appliedTick ? newest - peer.appliedTick : 0;
        m_stats.intentDepth = std::max(m_stats.intentDepth, static_cast<u32>(std::min<u64>(depth, 0xFFFF)));
        const bool resting = idle(peer.lastIntents);
        // **The delay adapts, and only while the player is idle**: holding a
        // tick or skipping one then moves nothing, so the change is never a
        // correction.
        //
        // One held tick for each new intent that arrives: a peer that stops
        // sending does not fill the queue, and holding for it would hold for
        // ever what it had already sent.
        if (resting && depth + 1 < peer.intentDelay && !peer.intentQueue.empty() && newest != peer.pausedAtNewest) {
            peer.pausedAtNewest = newest;
            deal(peer, *player, withCarried(peer), nullptr);
            continue;
        }
        u64 next = peer.appliedTick + 1;
        // **Never skipped while its predicted steps keep state**: the
        // step runs once for the ticks skipped, and a timer it counts down --
        // a cooldown, a dash -- would count one tick for several of the
        // player's own, and be corrected until it ran out. A hold is harmless:
        // neither machine steps the player then.
        const scene::CharacterBodyComponent* stepped =
            player->character.valid() ? world.characterBodies().find(player->character) : nullptr;
        bool predictedState = stepped != nullptr && !stepped->predictedAttributes.empty();
        // Any of the machine's players: they are stepped by the one stream.
        for (const Peer::Guest& guest : peer.guests) {
            const scene::PlayerComponent* other = guest.player.valid() ? world.players().find(guest.player) : nullptr;
            const scene::CharacterBodyComponent* body =
                other != nullptr && other->character.valid() ? world.characterBodies().find(other->character) : nullptr;
            predictedState = predictedState || (body != nullptr && !body->predictedAttributes.empty());
        }
        if (depth > static_cast<u64>(peer.intentDelay) + IntentCatchUp ||
            (resting && !predictedState && depth > static_cast<u64>(peer.intentDelay) + 1)) {
            // Too far behind the newest: caught up, what is skipped dropped --
            // but not a button pressed in it.
            next = newest - peer.intentDelay;
        }
        std::vector<scene::PlayerIntent>* found = nullptr;
        if (const auto at = peer.intentQueue.find(next); at != peer.intentQueue.end())
            found = &at->second;
        for (auto at = peer.intentQueue.upper_bound(peer.appliedTick); at != peer.intentQueue.end() && at->first < next;
             ++at)
            carryPresses(peer, at->second, found);
        if (found != nullptr) {
            peer.lastIntents = std::move(*found);
            peer.standInRun = 0;
            peer.ticksSinceStarved += 1;
            if (peer.ticksSinceStarved > IntentDelayRelaxTicks && peer.intentDelay > 1) {
                peer.intentDelay -= 1;
                peer.ticksSinceStarved = 0;
            }
        }
        else if (next >= peer.firstIntentTick) {
            // **Run dry: the last one stands in for this tick**, and the real
            // one is dropped when it arrives. For a held key the two are the
            // same, and nothing is corrected.
            peer.starvations += 1;
            m_stats.intentStarvations += 1;
            peer.ticksSinceStarved = 0;
            if (peer.standInRun == 0)
                peer.delayBeforeRun = peer.intentDelay;
            peer.standInRun += 1;
            peer.intentDelay = std::min(peer.intentDelay + 1, MaxIntentDelay);
            // **A stand-in has a lifetime**: past it, nobody is known
            // to be holding anything. A player whose connection is up and
            // silent -- a window being dragged, a phone in a pocket -- stops,
            // rather than running on what it held when it went quiet.
            if (peer.silentTicks > StandInLifetimeTicks) {
                for (scene::PlayerIntent& intent : peer.lastIntents) {
                    intent.pressed = false;
                    // `Enum.InputActionType.ViewportPosition`: where the
                    // pointer is, which stays where it was.
                    if (intent.type != 4)
                        intent.axis = core::Vec3{};
                }
            }
            peer.standIns.emplace(next, peer.lastIntents);
            while (peer.standIns.size() > IntentRedundancy * 4u)
                peer.standIns.erase(peer.standIns.begin());
        }
        // The peer's own tick: what its predicted steps are numbered by
        // there, and so here.
        deal(peer, *player, withCarried(peer), &next);
        peer.appliedTick = next;
        // Before the first intent's own tick nothing of the peer's has been
        // applied, and the answer stays "none" -- but a stream anchored AGAIN
        // has been answered before, and its answer keeps step with it.
        if (next >= peer.firstIntentTick || peer.intentTick != 0)
            peer.intentTick = next;
        peer.intentQueue.erase(peer.intentQueue.begin(), peer.intentQueue.upper_bound(next));
    }
}

namespace {

// Says, once, that instances of `classId` under what the authority sends are on
// no replica (protocol 44). **Decided as `schemaFor` decides the class is off
// the wire**: by the first of its own name and its ancestors' the schema
// excludes -- and said only when that exclusion is not a quiet one. A class the
// schema excludes by no name at all is a service, and is under nothing sent.
void warnUncarried(const scene::World& world, scene::ClassId classId)
{
    const scene::ClassDescriptor* own = world.classes().find(classId);
    if (own == nullptr)
        return;
    scene::ClassId at = classId;
    for (int guard = 0; guard < 64 && at != scene::InvalidClass; ++guard) {
        const scene::ClassDescriptor* descriptor = world.classes().find(at);
        if (descriptor == nullptr)
            return;
        const std::string_view name = world.atoms().text(descriptor->name);
        if (std::find(std::begin(generated::ExcludedClasses), std::end(generated::ExcludedClasses), name) !=
            std::end(generated::ExcludedClasses)) {
            if (std::find(std::begin(generated::QuietClasses), std::end(generated::QuietClasses), name) !=
                std::end(generated::QuietClasses))
                return;
            const std::array<core::I18nArg, 1> args{core::I18nArg{"class", world.atoms().text(own->name)}};
            core::log(core::LogLevel::Warn, ENG_TR("net.warn.class_not_replicated"), args);
            return;
        }
        at = descriptor->super;
    }
}

} // namespace

void AuthoritySession::capture(const scene::World& world, InstanceId root, u64 tick)
{
    auto state = std::make_shared<WorldState>();
    state->tick = tick;
    state->entities.reserve(m_order.size() + 16);
    const usize parentField = commonIndex("Parent");

    // **Nothing here allocates an instance** once it has run a few ticks: the
    // ids are gathered in a list and sorted once, a child carries its parent's
    // id and place on the stack with it, and a field set is one a state the
    // history let go was holding. A capture reads every replicated instance
    // every tick, and four allocations each were more than the reading.
    std::vector<std::pair<u64, u32>>& seen = m_seenScratch;
    seen.clear();
    // The walk itself, for interest: which instance each entity is and where
    // its parent is in this list, in pre-order.
    m_order.clear();
    std::vector<Walked>& stack = m_walkScratch;
    const auto byKey = [](const std::pair<u64, u32>& entry, u64 wanted) { return entry.first < wanted; };
    // The capture before, which an instance nothing has touched is kept from
    // -- of this same world: a world changed under the session is read whole.
    // And not of a world that was put back to an earlier state since: its
    // stamps went back with it, and say nothing about what changed.
    const WorldState* before = m_readWorld == &world && m_readRestores == world.restores() && !m_history.empty()
                                   ? m_history.back().get()
                                   : nullptr;
    // **A property a script wrote is read at the next capture** (protocol
    // 45): the world stamps the instance at the write, and one stamped since
    // the capture before is not kept. A write that changes no byte the digest
    // below compares -- a pivot's offset, which is known and not read -- was
    // late by the periodic reading, and the periodic reading was a third of a
    // send for it.
    const u64 writtenBefore = m_readMutations;
    // Another world has classes of its own.
    if (m_readWorld != &world) {
        m_schemaAsked.clear();
        m_schemaOfClass.clear();
    }
    const usize nameField = commonIndex("Name");
    // How often an instance is read whatever its bytes and its stamp say, in
    // captures, each on a capture of its own. **A net, and nothing is known to
    // fall into it**: every field is read from a component the digest
    // compares, from a table whose revision it mixes in, or through a
    // property whose write stamps the instance. It was one in eight while a
    // pivot waited on it, which read an eighth of a world every send -- a
    // third of what a send cost (0.35 ms of 1.07 on 2,700 instances).
    constexpr u64 RereadEvery = 64;
    // **Counted in captures, not ticks**: a game sends one tick in two, and
    // by the tick's own number an instance whose id was of the other parity
    // never had its turn -- what a capture cannot see change was not late by
    // eight, it never arrived.
    const u64 turn = m_captures++;
    // A set to read an instance into: one a state the history let go was
    // holding alone, or a new one.
    const auto fieldSet = [this] {
        std::shared_ptr<FieldSet> fields;
        if (!m_fieldPool.empty()) {
            fields = std::move(m_fieldPool.back());
            m_fieldPool.pop_back();
        }
        else {
            fields = std::make_shared<FieldSet>();
            m_stats.fieldSetsAllocated += 1;
        }
        return fields;
    };

    // One container's subtree -- `Workspace`'s, or a service's whose contents
    // travel (ADR 0080) -- with the container itself standing for the parent
    // of its children. Pre-order, children in sibling order: a parent is
    // always captured before its children, so a new child's parent already
    // has an id. `pinned` is every replica's, whatever its position.
    const auto walk = [&](InstanceId container, u32 containerNetId, i32 containerOrder, bool pinned) {
        stack.clear();
        const auto pushChildren = [&](InstanceId parent, u32 parentNet, i32 parentOrder) {
            const auto from = static_cast<std::ptrdiff_t>(stack.size());
            for (InstanceId child = world.firstChild(parent); child.valid(); child = world.nextSibling(child))
                stack.push_back(Walked{child, parentNet, parentOrder});
            std::reverse(stack.begin() + from, stack.end());
        };
        pushChildren(container, containerNetId, containerOrder);

        while (!stack.empty()) {
            const Walked next = stack.back();
            stack.pop_back();
            const InstanceId id = next.id;
            // **What a client-side script made is this machine's alone** (ADR
            // 0186), and takes its subtree with it as below.
            if (world.local(id))
                continue;
            const scene::ClassId classId = world.classOf(id);
            if (classId >= m_schemaAsked.size()) {
                m_schemaAsked.resize(static_cast<usize>(classId) + 1, u8{0});
                m_schemaOfClass.resize(static_cast<usize>(classId) + 1, nullptr);
            }
            if (m_schemaAsked[classId] == 0) {
                m_schemaAsked[classId] = 1;
                m_schemaOfClass[classId] = schemaFor(world, id);
            }
            const generated::ClassDesc* desc = m_schemaOfClass[classId];
            // **An instance the schema does not describe takes its subtree with
            // it.** A replica could not parent the children to anything.
            if (desc == nullptr) {
                // **And where a person is developing, it is said** (protocol
                // 44), once a class: under an instance the authority sends,
                // this one will be on no replica. Not under a container
                // itself, where a replica keeps what its own scene put there.
                if (m_warnUncarried && m_schemaAsked[classId] == 1 && next.parentNet != containerNetId) {
                    m_schemaAsked[classId] = 2;
                    warnUncarried(world, classId);
                }
                continue;
            }
            const u64 key = packed(id);
            u32 netId = 0;
            bool known = false;
            if (const auto found = std::lower_bound(m_netIds.begin(), m_netIds.end(), key, byKey);
                found != m_netIds.end() && found->first == key) {
                netId = found->second;
                known = true;
            }
            const u8 schema = schemaIndexOf(desc);

            // **Kept from the capture before, when the components it is read
            // from are the same bytes** -- most of a world, every tick: its
            // props, its walls, whatever stands still. Read again when they
            // are not (`sourceDigestOf`), when it was renamed or moved in the
            // tree, and one capture in `RereadEvery` whatever the bytes say: what
            // a field is read from that is neither is late by that much, and
            // no more.
            // Kept, it is the very set the state before holds: nothing is
            // copied, and nothing is read.
            //
            // **One that names another instance is kept too** (protocol 44) --
            // a model names its primary part, and a world's models were read
            // every tick for it. What it names can come or go with no byte of
            // its own changing, so the number each name was given by the
            // capture before is in what is compared: a part that left, or one
            // that arrived, is a tick late in the model that names it, and
            // then read.
            SharedFields fields;
            u64 keptHash = 0;
            u64 source = 0;
            // **Asleep, and asleep at the capture before** (ADR 0234): no
            // player is near, so the step did not move it, and nothing wrote
            // it -- kept without its components even being looked at. Most
            // of a large world's characters, every tick, where reading them to
            // find nothing changed was the largest part of a capture.
            const scene::CharacterBodyComponent* sleeper = world.characterBodies().find(id);
            const bool asleep = sleeper != nullptr && sleeper->dormant;
            if (asleep && known && before != nullptr && world.writtenAt(id) <= writtenBefore &&
                (turn + netId) % RereadEvery != 0 &&
                std::binary_search(m_asleepBefore.begin(), m_asleepBefore.end(), netId)) {
                const EntityState* held = findEntity(*before, netId);
                if (held != nullptr && held->schema == schema && held->hash != 0 &&
                    asU32(held->fields[nameField]) == world.name(id).id &&
                    asU32(held->fields[parentField]) == next.parentNet) {
                    fields = held->fields;
                    keptHash = held->hash;
                    source = held->source;
                }
            }
            if (keptHash == 0)
                source = sourceDigestOf(world, id, *desc);
            if (keptHash == 0 && source != 0) {
                for (const usize at : referencesOf(schema)) {
                    const InstanceId target = referenceAt(world, id, *desc, at);
                    u64 named = 0;
                    if (target.valid()) {
                        const u64 targetKey = packed(target);
                        if (const auto found = std::lower_bound(m_netIds.begin(), m_netIds.end(), targetKey, byKey);
                            found != m_netIds.end() && found->first == targetKey)
                            named = found->second;
                    }
                    source = (source ^ (named + 0x9E3779B97F4A7C15ull)) * 0xFF51AFD7ED558CCDull;
                    source ^= source >> 29;
                }
                if (source == 0)
                    source = 1;
            }
            if (keptHash == 0 && known && before != nullptr && source != 0 && world.writtenAt(id) <= writtenBefore &&
                (turn + netId) % RereadEvery != 0) {
                const EntityState* held = findEntity(*before, netId);
                if (held != nullptr && held->schema == schema && held->hash != 0 && held->source == source &&
                    asU32(held->fields[nameField]) == world.name(id).id &&
                    asU32(held->fields[parentField]) == next.parentNet) {
                    fields = held->fields;
                    keptHash = held->hash;
                }
            }
            if (keptHash != 0) {
                m_stats.entitiesKept += 1;
            }
            else {
                std::shared_ptr<FieldSet> read = fieldSet();
                if (!extractFieldsInto(world, id, *desc, *read)) {
                    m_fieldPool.push_back(std::move(read));
                    continue;
                }
                m_stats.entitiesRead += 1;
                setNetId((*read)[parentField], NetId{next.parentNet});
                fields = SharedFields(std::move(read));
            }

            if (!known) {
                netId = m_nextNetId++;
                m_classNames[netId] = world.classes().find(world.classOf(id))->name;
            }
            seen.emplace_back(key, netId);
            if (asleep)
                m_asleepNow.push_back(netId);

            state->entities.push_back(EntityState{NetId{netId}, schema, std::move(fields), keptHash, source});
            const auto order = static_cast<i32>(m_order.size());
            m_order.push_back(Captured{netId, id, next.parentOrder, pinned});
            pushChildren(id, netId, order);
        }
    };
    m_asleepNow.clear();
    walk(root, RootNetId.value, -1, false);

    // **The services whose properties travel** (`Service = true` in the wire
    // schema), each under an id of its own that never changes: every world has
    // one from boot, so there is nothing to spawn and nothing to number.
    const InstanceId dataModel = world.parentOf(root);
    for (usize index = 0; index < std::size(generated::Classes) && dataModel.valid(); ++index) {
        const generated::ClassDesc& desc = generated::Classes[index];
        if (!desc.service)
            continue;
        for (InstanceId child = world.firstChild(dataModel); child.valid(); child = world.nextSibling(child)) {
            if (world.atoms().text(world.classes().find(world.classOf(child))->name) != desc.name)
                continue;
            std::shared_ptr<FieldSet> read = fieldSet();
            if (!extractFieldsInto(world, child, desc, *read)) {
                m_fieldPool.push_back(std::move(read));
                break;
            }
            setNetId((*read)[parentField], RootNetId);
            const u32 netId = ServiceNetIdBase + static_cast<u32>(index);
            state->entities.push_back(EntityState{NetId{netId}, static_cast<u8>(index), SharedFields(std::move(read))});
            m_order.push_back(Captured{netId, child, -1, desc.contents});
            if (desc.contents)
                walk(child, netId, static_cast<i32>(m_order.size() - 1), true);
            break;
        }
    }

    // In id order from here: what a reference is looked up in, and what the
    // next capture finds its instances' ids in.
    std::sort(seen.begin(), seen.end());
    std::sort(m_asleepNow.begin(), m_asleepNow.end());
    std::swap(m_asleepBefore, m_asleepNow);

    // **What a joint names, as a network id** -- after the walk, since a
    // joint may name what is captured after it. An instance that was not
    // captured -- out of the world, or of a class off the wire -- is none at
    // all to a replica.
    for (EntityState& entity : state->entities) {
        // One kept from the capture before holds the numbers it was given
        // then, in a set other states share: only what was read this tick
        // holds instances still to be numbered.
        if (entity.hash != 0)
            continue;
        for (const usize at : referencesOf(entity.schema)) {
            const InstanceId target = asInstance(entity.fields[at]);
            u32 named = 0;
            if (target.valid()) {
                const u64 key = packed(target);
                if (const auto found = std::lower_bound(seen.begin(), seen.end(), key, byKey);
                    found != seen.end() && found->first == key)
                    named = found->second;
            }
            setNetId(entity.fields.write()[at], NetId{named});
        }
        // What it comes to, once and for every peer: after the references,
        // which are fields like any other. One kept from the capture before
        // comes to what it came to then.
        entity.hash = hashOf(entity);
    }
    m_readWorld = &world;
    m_readMutations = world.mutations();
    m_readRestores = world.restores();

    // Instances gone since the last capture give their ids up for good: both
    // lists are in id order, so one walk of the two finds them.
    {
        auto still = seen.begin();
        for (const std::pair<u64, u32>& old : m_netIds) {
            while (still != seen.end() && still->first < old.first)
                ++still;
            if (still == seen.end() || still->first != old.first)
                m_classNames.erase(old.second);
        }
    }
    m_netIds.swap(seen);
    m_orderOfNet.clear();
    m_orderOfNet.reserve(m_order.size());
    for (usize at = 0; at < m_order.size(); ++at)
        m_orderOfNet.emplace_back(m_order[at].netId, static_cast<u32>(at));
    std::sort(m_orderOfNet.begin(), m_orderOfNet.end());

    std::sort(state->entities.begin(), state->entities.end(),
              [](const EntityState& a, const EntityState& b) { return a.id.value < b.id.value; });
    m_history.push_back(std::move(state));
    while (m_history.size() > StateHistory) {
        // Its field sets are the next capture's, when nobody else holds the
        // state -- a peer being sent it in parts does (`pinnedState`).
        const std::shared_ptr<const WorldState> gone = std::move(m_history.front());
        m_history.pop_front();
        if (gone.use_count() == 1 && m_fieldPool.size() < 2 * m_order.size() + 64) {
            // Made here, by `make_shared<WorldState>()`, and held by nobody
            // else: taking it apart is this function's to do.
            WorldState& spent = const_cast<WorldState&>(*gone);
            for (EntityState& entity : spent.entities) {
                // The sets no later state holds: those are free to read into.
                if (std::shared_ptr<FieldSet> freed = entity.fields.release(); freed != nullptr)
                    m_fieldPool.push_back(std::move(freed));
            }
        }
    }
}

const WorldState* AuthoritySession::historyAt(u64 tick) const noexcept
{
    for (auto at = m_history.rbegin(); at != m_history.rend(); ++at) {
        if ((*at)->tick == tick)
            return at->get();
    }
    return nullptr;
}

void AuthoritySession::send(const scene::World& world, InstanceId root, u64 tick)
{
    m_world = &world;
    m_tick = tick;
    // **The budget's numbers, a second at a time** (ADR 0229).
    if (m_budgetWindowTick == 0 || tick < m_budgetWindowTick)
        m_budgetWindowTick = tick;
    if (tick >= m_budgetWindowTick + static_cast<u64>(TicksPerSecond)) {
        m_stats.deferredLastSecond = m_windowDeferred;
        m_stats.budgetUsedLastSecond = m_windowGiven > 0.0 ? m_windowSpent / m_windowGiven : 0.0;
        m_windowDeferred = 0;
        m_windowSpent = 0.0;
        m_windowGiven = 0.0;
        m_budgetWindowTick = tick;
    }
    // What a send spends, by what it is doing: reading the world, finding
    // what changed off the entities, and telling each peer.
    core::profile::Sections stretch;
    ENG_PROFILE_NEXT(stretch, "net.capture");
    capture(world, root, tick);
    ENG_PROFILE_NEXT(stretch, "net.changes");
    diffTilemaps(world);
    diffAttributes(world, root);
    diffGround(world, root);
    m_stats.groundEditMessages += m_groundEdits.size();
    ENG_PROFILE_NEXT(stretch, "net.peers");
    const WorldState& current = *m_history.back();

    // Everybody taking part, in join order -- the children of `NetworkService`,
    // which is where every path that makes a player puts it -- each as its user
    // id and its character's NetId, zero for none. The pair is what lets a
    // replica know which part is its own (`Player.Character`).
    std::vector<u32> roster;
    const InstanceId network = scene::networkServiceOf(world, world.parentOf(root));
    for (InstanceId child = network.valid() ? world.firstChild(network) : InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        const scene::PlayerComponent* player = world.players().find(child);
        if (player == nullptr || world.destroyed(child))
            continue;
        roster.push_back(player->userId);
        roster.push_back(player->character.valid() ? netIdOf(player->character).value : 0u);
    }

    indexInterest(world);
    for (Peer& peer : m_peers) {
        if (!peer.welcomed)
            continue;
        // **A peer that has been sent nothing yet waits for the ground**.
        // The first thing it is sent includes the whole ground, every
        // chunk of it compressed -- twenty-two milliseconds of this frame for
        // a land four hundred metres square, each time somebody joined. It is
        // compressed in a job instead, and the peer begins on the tick that
        // is done: a tick or two later than it would have, with nothing sent
        // in between for it to have missed.
        if (!peer.begun) {
            if (!groundWholeReady(world))
                continue;
            peer.begun = true;
        }
        // What this peer owns (ADR 0099), in network-id order: from the
        // entries that have an owner at all, not from every entry.
        std::vector<u32> owned;
        for (const auto& [index, owner] : m_interest.owners) {
            if (peer.has(owner))
                owned.push_back(m_order[index].netId);
        }
        std::sort(owned.begin(), owned.end());
        std::vector<u32> relevant;
        {
            ENG_PROFILE_SCOPE("net.peer.interest");
            relevant = interestOf(world, peer);
        }
        ENG_PROFILE_SCOPE("net.peer.send");
        sendTo(peer, current, roster, relevant, owned);
    }
}

// **Where a player looks from** (ADR 0162): `Player.ReplicationFocus` when
// the game named one, their character otherwise, and nothing for a player
// with neither.
static std::optional<core::DVec3> focusOf(const scene::World& world, const scene::PlayerComponent* player)
{
    if (player == nullptr)
        return std::nullopt;
    if (player->replicationFocus.valid() && world.alive(player->replicationFocus)) {
        if (const std::optional<core::DVec3> place = placeOf(world, player->replicationFocus); place.has_value())
            return place;
    }
    if (player->character.valid() && world.alive(player->character))
        return placeOf(world, player->character);
    return std::nullopt;
}

namespace {

// A cell's key: three signed cell coordinates, 21 bits each.
[[nodiscard]] u64 cellKey(i64 x, i64 y, i64 z) noexcept
{
    constexpr i64 Mask = (i64{1} << 21) - 1;
    return (static_cast<u64>(x & Mask) << 42) | (static_cast<u64>(y & Mask) << 21) | static_cast<u64>(z & Mask);
}

// Where an entry is and how far it reaches from there: a part's box, as the
// sphere round it; a sprite's rectangle, the circle round it.
[[nodiscard]] std::optional<std::pair<core::DVec3, f64>> boundsOf(const scene::World& world, core::InstanceId id)
{
    if (const scene::PartComponent* part = world.parts().find(id); part != nullptr) {
        const f64 x = static_cast<f64>(part->size.x);
        const f64 y = static_cast<f64>(part->size.y);
        const f64 z = static_cast<f64>(part->size.z);
        return std::pair{part->cframe.position, 0.5 * std::sqrt(x * x + y * y + z * z)};
    }
    if (const scene::Part2DComponent* sprite = world.parts2d().find(id); sprite != nullptr) {
        return std::pair{core::DVec3{static_cast<f64>(sprite->position.x), static_cast<f64>(sprite->position.y), 0.0},
                         0.5 * std::hypot(static_cast<f64>(sprite->size.x), static_cast<f64>(sprite->size.y))};
    }
    return std::nullopt;
}

// How many cells one entry may be put in: past this it is measured by every
// peer, as before -- a floor of kilometres under a tiny radius.
constexpr i64 MostCellsAnEntry = 4096;

} // namespace

void AuthoritySession::indexInterest(const scene::World& world)
{
    InterestIndex& index = m_interest;
    const usize count = m_order.size();
    index.positioned.assign(count, 0);
    index.cells.clear();
    index.owners.clear();
    const f64 keep = world.engineState().streamingLoadRadius * 1.25;
    // A radius the grid cannot hold -- none, or one beyond the world -- is
    // measured entry by entry, as it always was.
    index.everything = !(keep > 1.0 && keep < 1.0e6);
    index.cell = keep;
    // **Phases** (ADR 0231): each entry's own, read only in a world that has
    // any.
    index.phased = scene::anyPhased(world);
    index.phase.assign(index.phased ? count : 0, 0u);
    if (index.phased) {
        for (usize at = 0; at < count; ++at) {
            if (const scene::PhaseComponent* own = world.phases().find(m_order[at].id); own != nullptr)
                index.phase[at] = own->phase.id;
        }
    }
    for (usize at = 0; at < count; ++at) {
        const core::InstanceId id = m_order[at].id;
        if (const scene::RigidBodyComponent* body = world.rigidBodies().find(id);
            body != nullptr && body->networkOwner != 0)
            index.owners.emplace_back(static_cast<u32>(at), body->networkOwner);
        const std::optional<std::pair<core::DVec3, f64>> bounds = boundsOf(world, id);
        if (!bounds.has_value())
            continue;
        index.positioned[at] = 1;
        if (index.everything)
            continue;
        const auto [centre, reach] = *bounds;
        const auto low = [&](f64 v) { return static_cast<i64>(std::floor((v - reach) / index.cell)); };
        const auto high = [&](f64 v) { return static_cast<i64>(std::floor((v + reach) / index.cell)); };
        const i64 x0 = low(centre.x);
        const i64 x1 = high(centre.x);
        const i64 y0 = low(centre.y);
        const i64 y1 = high(centre.y);
        const i64 z0 = low(centre.z);
        const i64 z1 = high(centre.z);
        if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > MostCellsAnEntry) {
            // Too big to cell: every peer measures it, in the list kept
            // under the key nothing else has.
            index.cells[~u64{0}].push_back(static_cast<u32>(at));
            continue;
        }
        for (i64 x = x0; x <= x1; ++x)
            for (i64 y = y0; y <= y1; ++y)
                for (i64 z = z0; z <= z1; ++z)
                    index.cells[cellKey(x, y, z)].push_back(static_cast<u32>(at));
    }
}

std::vector<u32> AuthoritySession::interestOf(const scene::World& world, const Peer& peer) const
{
    std::vector<u32> relevant;
    relevant.reserve(m_order.size());

    // **Measured from the peer's character** (`Player.Character`). A player
    // with none has nothing to measure from and is sent everything, which is
    // what every session did before interest existed.
    const scene::PlayerComponent* player = peer.player.valid() ? world.players().find(peer.player) : nullptr;
    // **And in phase** (ADR 0231 §3): an entry whose phase, or a phase above
    // it, none of the machine's players is in is not sent at all.
    std::vector<u8> unseen;
    if (m_interest.phased) {
        std::vector<u32> held;
        for (u8 seat = 0; seat <= peer.guests.size(); ++seat) {
            const InstanceId seated = peer.playerIn(seat);
            if (const scene::PlayerComponent* sitting = seated.valid() ? world.players().find(seated) : nullptr;
                sitting != nullptr)
                for (const core::NameAtom phase : sitting->phases)
                    held.push_back(phase.id);
        }
        std::sort(held.begin(), held.end());
        unseen.assign(m_order.size(), 0);
        for (usize at = 0; at < m_order.size(); ++at) {
            const u32 own = at < m_interest.phase.size() ? m_interest.phase[at] : 0u;
            const i32 parent = m_order[at].parent;
            unseen[at] = (own != 0 && !std::binary_search(held.begin(), held.end(), own)) ||
                                 (parent >= 0 && unseen[static_cast<usize>(parent)])
                             ? 1
                             : 0;
        }
    }
    const std::optional<core::DVec3> body = focusOf(world, player);
    if (!body.has_value()) {
        for (usize at = 0; at < m_order.size(); ++at) {
            if (unseen.empty() || !unseen[at])
                relevant.push_back(m_order[at].netId);
        }
        std::sort(relevant.begin(), relevant.end());
        return relevant;
    }
    // **And from each of the machine's other players** (protocol 48, ADR
    // 0205): somebody at the same screen who has walked off has a view of
    // their own, and what is round them is drawn in it.
    std::array<core::DVec3, 4> foci{*body};
    usize fociCount = 1;
    for (const Peer::Guest& guest : peer.guests) {
        const scene::PlayerComponent* other = guest.player.valid() ? world.players().find(guest.player) : nullptr;
        if (const std::optional<core::DVec3> from = focusOf(world, other); other != nullptr && from.has_value())
            foci[fociCount++] = *from;
    }

    // The streaming radius, with the streaming manager's hysteresis: in at the
    // load radius, out only past a quarter more, so a part on the boundary does
    // not spawn and despawn on alternate ticks.
    const f64 radius = world.engineState().streamingLoadRadius;
    const f64 keep = radius * 1.25;

    const usize count = m_order.size();
    std::vector<u8> inRange(count, 0);
    std::vector<u8> positioned = m_interest.positioned;
    positioned.resize(count, 0);
    std::vector<u8> marked(count, 0);
    // **Measured exactly, but only what could be in reach** (ADR 0228 §2):
    // the entries in the cells round each focus, once each.
    const auto measure = [&](usize at) {
        // The nearest of the machine's players, to the nearest edge
        // (`reachSquared`).
        std::optional<f64> nearest;
        for (usize from = 0; from < fociCount; ++from) {
            const std::optional<f64> squared = reachSquared(world, m_order[at].id, foci[from]);
            if (!squared.has_value())
                break;
            nearest = nearest.has_value() ? std::min(*nearest, *squared) : *squared;
        }
        if (!nearest.has_value())
            return;
        const f64 distance = *nearest;
        const f64 reach = std::binary_search(peer.known.begin(), peer.known.end(), m_order[at].netId) ? keep : radius;
        inRange[at] = distance <= reach * reach ? 1 : 0;
    };
    if (m_interest.everything) {
        for (usize at = 0; at < count; ++at)
            measure(at);
    }
    else {
        std::vector<u8> measured(count, 0);
        const auto visit = [&](const std::vector<u32>& entries) {
            for (const u32 at : entries) {
                if (at < count && !measured[at]) {
                    measured[at] = 1;
                    measure(at);
                }
            }
        };
        if (const auto huge = m_interest.cells.find(~u64{0}); huge != m_interest.cells.end())
            visit(huge->second);
        const f64 cell = m_interest.cell;
        for (usize from = 0; from < fociCount; ++from) {
            const core::DVec3& focus = foci[from];
            const auto low = [&](f64 v) { return static_cast<i64>(std::floor((v - keep) / cell)); };
            const auto high = [&](f64 v) { return static_cast<i64>(std::floor((v + keep) / cell)); };
            for (i64 x = low(focus.x); x <= high(focus.x); ++x)
                for (i64 y = low(focus.y); y <= high(focus.y); ++y)
                    for (i64 z = low(focus.z); z <= high(focus.z); ++z)
                        if (const auto found = m_interest.cells.find(cellKey(x, y, z)); found != m_interest.cells.end())
                            visit(found->second);
        }
    }
    // What the peer owns it simulates, so it has it wherever it is.
    for (const auto& [at, owner] : m_interest.owners) {
        if (at < count && peer.has(owner))
            inRange[at] = 1;
    }

    // Down: a part in range brings everything under it -- what is attached to
    // a part goes with it, whatever its own position says. Pre-order, so a
    // parent is decided before its children.
    std::vector<u8> positionedAbove(count, 0);
    for (usize at = 0; at < count; ++at) {
        const i32 parent = m_order[at].parent;
        if (parent >= 0) {
            const auto up = static_cast<usize>(parent);
            positionedAbove[at] = positioned[up] || positionedAbove[up] ? 1 : 0;
            marked[at] = marked[up] && (positioned[up] || positionedAbove[up]) ? 1 : 0;
        }
        if (inRange[at])
            marked[at] = 1;
    }

    // Up: the ancestors of anything sent, so it has somewhere to be parented;
    // and a container with no part anywhere in or above it, which costs nothing
    // and has no position to be far from. Reverse pre-order visits children
    // before their parent.
    std::vector<u8> positionedBelow(count, 0);
    for (usize at = count; at-- > 0;) {
        const i32 parent = m_order[at].parent;
        if (!marked[at] && !positioned[at] && !positionedBelow[at] && !positionedAbove[at])
            marked[at] = 1;
        if (parent >= 0) {
            const auto up = static_cast<usize>(parent);
            if (marked[at] && (positioned[at] || positionedBelow[at] || positionedAbove[at] || inRange[at]))
                marked[up] = 1;
            if (positioned[at] || positionedBelow[at])
                positionedBelow[up] = 1;
        }
    }

    // What a service keeps for everybody is everybody's, near or far.
    for (usize at = 0; at < count; ++at) {
        if ((marked[at] || m_order[at].pinned) && (unseen.empty() || !unseen[at]))
            relevant.push_back(m_order[at].netId);
    }
    std::sort(relevant.begin(), relevant.end());
    return relevant;
}

// **What goes now and what waits** (ADR 0229). What may not wait goes first
// whatever it costs: a record whole (a part new to the peer), its own
// players' characters, what it owns, and what is within reach of one of its
// characters. Then what has waited longest past
// `BudgetStaleTicks`, oldest first; then the rest by a priority that grows each snapshot it waits --
// by how near it is to the peer's focus, times the part's
// `ReplicationPriority`, times what the game said for this player -- until
// the budget is spent. What does not fit waits, its priority kept.
void AuthoritySession::chooseWithinBudget(Peer& peer, const scene::PlayerComponent* player,
                                          std::span<const RecordFacts> records, std::span<const usize> starts,
                                          std::vector<u8>& chosen, std::vector<u32>& heldBack)
{
    const scene::World& world = *m_world;
    const u64 now = m_tick;
    std::vector<u32> own;
    // Where this machine's characters are: what is within reach of one of
    // them may not wait, since what its prediction collides with has to be
    // where the authority has it.
    std::vector<core::DVec3> bodies;
    for (u8 seat = 0; seat <= peer.guests.size(); ++seat) {
        const InstanceId seated = peer.playerIn(seat);
        const scene::PlayerComponent* sitting = seated.valid() ? world.players().find(seated) : nullptr;
        if (sitting == nullptr || !sitting->character.valid())
            continue;
        own.push_back(netIdOf(sitting->character).value);
        if (const std::optional<core::DVec3> place = placeOf(world, sitting->character); place.has_value())
            bodies.push_back(*place);
    }
    const auto withinReach = [&](const std::optional<core::DVec3>& place) {
        if (!place.has_value())
            return false;
        for (const core::DVec3& body : bodies) {
            const core::DVec3 apart = *place - body;
            if (apart.x * apart.x + apart.y * apart.y + apart.z * apart.z <= BudgetReachMetres * BudgetReachMetres)
                return true;
        }
        return false;
    };
    const std::optional<core::DVec3> focus = focusOf(world, player);
    const f64 radius = std::max(world.engineState().streamingLoadRadius, BudgetNearMetres + 1.0);
    // How fast a part's turn comes for this peer.
    const auto rateOf = [&](u32 id) {
        const InstanceId instance = instanceOfNet(world, id);
        f64 rate = 1.0;
        if (!instance.valid())
            return rate;
        if (const scene::PartComponent* part = world.parts().find(instance); part != nullptr)
            rate *= static_cast<f64>(part->replicationPriority);
        if (player != nullptr) {
            for (const auto& [named, priority] : player->replicationPriorities) {
                if (named == instance)
                    rate *= static_cast<f64>(priority);
            }
        }
        if (focus.has_value()) {
            if (const std::optional<core::DVec3> place = placeOf(world, instance); place.has_value()) {
                const core::DVec3 apart = *place - *focus;
                const f64 distance = std::sqrt(apart.x * apart.x + apart.y * apart.y + apart.z * apart.z);
                if (distance > BudgetNearMetres) {
                    const f64 along = std::min(1.0, (distance - BudgetNearMetres) / (radius - BudgetNearMetres));
                    rate *= 1.0 - (1.0 - BudgetFarRate) * along;
                }
            }
        }
        return rate;
    };

    struct Turn
    {
        usize at = 0;
        bool must = false;
        bool waited = false;
        bool stale = false;
        f32 priority = 0.0f;
        u64 since = 0;
        u32 id = 0;
    };
    std::vector<Turn> turns;
    turns.reserve(records.size());
    for (usize at = 0; at < records.size(); ++at) {
        const u32 id = records[at].id;
        Turn turn;
        turn.at = at;
        turn.id = id;
        turn.must = records[at].full || std::find(own.begin(), own.end(), id) != own.end() ||
                    std::binary_search(peer.owned.begin(), peer.owned.end(), id);
        if (!turn.must && !bodies.empty()) {
            const InstanceId instance = instanceOfNet(world, id);
            turn.must = instance.valid() && withinReach(placeOf(world, instance));
        }
        const auto was = std::lower_bound(peer.waiting.begin(), peer.waiting.end(), id,
                                          [](const Peer::Waiting& waiting, u32 probe) { return waiting.id < probe; });
        turn.waited = was != peer.waiting.end() && was->id == id;
        turn.priority = turn.waited ? was->priority : 0.0f;
        turn.since = turn.waited ? was->since : now;
        if (!turn.must)
            turn.priority += static_cast<f32>(rateOf(id));
        turn.stale = now >= turn.since + BudgetStaleTicks;
        turns.push_back(turn);
    }
    // **In three goes.** What may not wait; then what has waited past
    // `BudgetStaleTicks`, oldest first, with at most half of what is left --
    // were it all, a budget that barely carries everything would be spent in
    // order of age and what the game put first would wait its turn like the
    // rest; then everything else, the old that did not fit among it, by
    // priority.
    const auto byPriority = [](const Turn& a, const Turn& b) {
        if (a.priority != b.priority)
            return a.priority > b.priority;
        return a.id < b.id;
    };
    std::vector<Turn> stale;
    std::vector<Turn> rest;
    std::vector<Peer::Waiting> waiting;
    f64 left = peer.budgetBytes;
    const auto take = [&](const Turn& turn) {
        chosen[turn.at] = 1;
        left -= records[turn.at].full ? 0.0 : static_cast<f64>(starts[turn.at + 1] - starts[turn.at]);
        // What may not wait is drawn on time from here: within reach of
        // the peer's character, its prediction meets it where it is.
        if (turn.waited && !turn.must)
            heldBack.push_back(turn.id);
    };
    for (const Turn& turn : turns) {
        if (turn.must)
            take(turn);
        else if (turn.stale)
            stale.push_back(turn);
        else
            rest.push_back(turn);
    }
    std::sort(stale.begin(), stale.end(),
              [](const Turn& a, const Turn& b) { return a.since != b.since ? a.since < b.since : a.id < b.id; });
    const f64 staleShare = left * 0.5;
    f64 staleSpent = 0.0;
    for (const Turn& turn : stale) {
        const f64 size = static_cast<f64>(starts[turn.at + 1] - starts[turn.at]);
        if (staleSpent + size <= staleShare && size <= left) {
            staleSpent += size;
            take(turn);
            continue;
        }
        rest.push_back(turn);
    }
    std::sort(rest.begin(), rest.end(), byPriority);
    for (const Turn& turn : rest) {
        const f64 size = static_cast<f64>(starts[turn.at + 1] - starts[turn.at]);
        if (size <= left) {
            take(turn);
            continue;
        }
        chosen[turn.at] = 0;
        waiting.push_back(Peer::Waiting{turn.id, turn.priority, turn.since});
    }
    std::sort(waiting.begin(), waiting.end(),
              [](const Peer::Waiting& a, const Peer::Waiting& b) { return a.id < b.id; });
    std::sort(heldBack.begin(), heldBack.end());
    peer.waiting = std::move(waiting);
}

void AuthoritySession::sendTo(Peer& peer, const WorldState& everything, const std::vector<u32>& roster,
                              const std::vector<u32>& relevant, const std::vector<u32>& owned)
{
    // **This peer's world is what is in its interest.** Everything below --
    // spawns, despawns, the diff and the checksum -- is over this, so a replica
    // reconstructs and verifies exactly the subset it was sent (ADR 0069
    // decision 8: its hash is a subset by design).
    //
    // **A view of the whole, never a copy of it**: an entity is a kilobyte and
    // more, and copying each one a peer is sent, every tick, was megabytes a
    // peer. Both lists are in id order, so one walk of the two finds them.
    const u64 currentTick = everything.tick;
    std::vector<const EntityState*> current;
    current.reserve(relevant.size());
    {
        // **Looked up, not walked** (ADR 0228 §2): a peer holds tens of what
        // the world has thousands of, and a walk of the whole for each peer
        // was the cost of every peer again. Both are in id order, so each
        // search starts where the last one ended.
        auto from = everything.entities.begin();
        for (const u32 wanted : relevant) {
            from = std::lower_bound(from, everything.entities.end(), wanted,
                                    [](const EntityState& entity, u32 id) { return entity.id.value < id; });
            if (from == everything.entities.end())
                break;
            if (from->id.value == wanted)
                current.push_back(&*from);
        }
    }

    // --- Who is playing, whole, when it changed for this peer.
    if (!peer.rosterSent || peer.roster != roster) {
        Writer players;
        players.u8v(static_cast<u8>(MessageType::Players));
        players.u32v(static_cast<u32>(roster.size() / 2));
        for (const u32 value : roster)
            players.u32v(value);
        sendBytes(m_transport, peer.id, players.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        peer.roster = roster;
        peer.rosterSent = true;
    }

    // --- **The phases its players are in** (ADR 0231), whole, when they
    // changed: what `HasPhase` reads on that machine.
    for (u8 seat = 0; seat <= peer.guests.size(); ++seat) {
        const InstanceId seated = peer.playerIn(seat);
        const scene::PlayerComponent* sitting = seated.valid() ? m_world->players().find(seated) : nullptr;
        if (sitting == nullptr)
            continue;
        std::vector<std::string> names;
        for (const core::NameAtom phase : sitting->phases)
            names.emplace_back(m_world->atoms().text(phase));
        std::sort(names.begin(), names.end());
        if (peer.phasesTold[seat] && peer.phasesSent[seat] == names)
            continue;
        Writer phases;
        phases.u8v(static_cast<u8>(MessageType::Phases));
        phases.u8v(seat);
        phases.u16v(static_cast<u16>(std::min<usize>(names.size(), 0xFFFF)));
        for (const std::string& name : names)
            phases.text(name);
        sendBytes(m_transport, peer.id, phases.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        peer.phasesSent[seat] = std::move(names);
        peer.phasesTold[seat] = true;
    }

    // --- The scene the authority is in (ADR 0106), before anything of it is
    // spawned: once to a peer that joins, and whenever it changes.
    // **And whenever it is loaded again**: the next match in the same
    // scene is a change as much as a move to another is.
    if (const std::string& scene = m_world->engineState().currentScene;
        !peer.sceneSent || peer.scene != scene || peer.sceneLoad != m_world->engineState().sceneLoads) {
        Writer change;
        change.u8v(static_cast<u8>(MessageType::SceneChange));
        change.text(scene);
        const std::vector<u8>& data = m_world->engineState().sceneLoadData;
        change.u32v(static_cast<u32>(data.size()));
        for (const u8 byte : data)
            change.u8v(byte);
        change.u32v(m_world->engineState().sceneLoads);
        sendBytes(m_transport, peer.id, change.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        peer.scene = scene;
        peer.sceneLoad = m_world->engineState().sceneLoads;
        peer.sceneSent = true;
    }

    // --- Spawns and despawns, reliable, before the snapshot that needs them.
    std::vector<u32> now;
    now.reserve(current.size());
    for (const EntityState* entity : current)
        now.push_back(entity->id.value);

    std::vector<u32> entering;
    std::set_difference(now.begin(), now.end(), peer.known.begin(), peer.known.end(), std::back_inserter(entering));
    std::vector<u32> leaving;
    std::set_difference(peer.known.begin(), peer.known.end(), now.begin(), now.end(), std::back_inserter(leaving));
    // A service exists on both ends from boot: its first record arrives whole
    // because no baseline has it, and it is never spawned or despawned.
    std::erase_if(entering, [](u32 id) { return id >= ServiceNetIdBase; });
    std::erase_if(leaving, [](u32 id) { return id >= ServiceNetIdBase; });

    if (!leaving.empty()) {
        // **Two lists: destroyed, then streamed out.** The replica treats them
        // differently -- a destroyed instance is gone, one that only left this
        // peer's interest is kept as a husk for a script that holds it -- and
        // only the authority knows which is which: a streamed-out id is still in
        // the whole world's state and only missing from this peer's.
        std::vector<u32> destroyed;
        std::vector<u32> streamed;
        for (const u32 id : leaving)
            (findEntity(everything, id) != nullptr ? streamed : destroyed).push_back(id);
        Writer despawn;
        despawn.u8v(static_cast<u8>(MessageType::Despawn));
        for (const std::vector<u32>* list : {&destroyed, &streamed}) {
            despawn.u32v(static_cast<u32>(list->size()));
            for (const u32 id : *list)
                despawn.u32v(id);
        }
        sendBytes(m_transport, peer.id, despawn.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        m_stats.despawned += static_cast<u32>(leaving.size());
    }
    if (!entering.empty()) {
        Writer spawn;
        spawn.u8v(static_cast<u8>(MessageType::Spawn));
        spawn.u32v(static_cast<u32>(entering.size()));
        for (const u32 id : entering) {
            spawn.u32v(id);
            spawn.text(m_world->atoms().text(m_classNames.at(id)));
            // Where it was authored (protocol 29, ADR 0138 §6): the replica's
            // own scripts for that place go under it.
            const InstanceId instance = instanceOfNet(*m_world, id);
            const scene::World::Origin origin = instance.valid() ? m_world->originOf(instance) : scene::World::Origin{};
            spawn.text(origin.asset.valid() ? m_world->atoms().text(origin.asset) : std::string_view{});
            spawn.u32v(origin.index);
        }
        sendBytes(m_transport, peer.id, spawn.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        m_stats.spawned += static_cast<u32>(entering.size());
    }

    // --- Tilemaps' cells (ADR 0103), after the spawns that name them: every
    // block of one entering, and what this send's edits changed of the rest.
    for (const u32 id : entering) {
        const InstanceId instance = instanceOfNet(*m_world, id);
        const scene::Tilemap2DComponent* tilemap = instance.valid() ? m_world->tilemaps2d().find(instance) : nullptr;
        if (tilemap == nullptr || tilemap->chunks.empty())
            continue;
        const std::vector<TileBlock> blocks(tilemap->chunks.begin(), tilemap->chunks.end());
        sendTileBlocks(m_transport, peer.id, id, blocks, m_stats);
    }
    for (const auto& [id, edits] : m_tilemapEdits) {
        if (std::binary_search(now.begin(), now.end(), id) && !std::binary_search(entering.begin(), entering.end(), id))
            sendTileBlocks(m_transport, peer.id, id, edits, m_stats);
    }
    // Attributes (ADR 0106), after the spawns and the players they name.
    {
        const std::vector<u32> known = std::move(peer.known);
        peer.known = now;
        sendAttributes(peer, entering);
        peer.known = known;
    }
    peer.known = std::move(now);

    // --- The ground (ADR 0135): whole to a peer that has not got this scene's,
    // and what this send changed to one that has.
    if (const std::string& scene = m_world->engineState().currentScene; !peer.groundSent || peer.groundScene != scene) {
        sendGroundWhole(peer, *m_world);
        peer.groundSent = true;
        peer.groundScene = scene;
    }
    else {
        for (const std::vector<u8>& message : m_groundEdits)
            sendBytes(m_transport, peer.id, message, net::Delivery::Reliable, ControlChannel, m_stats);
    }

    // --- What this peer owns, whole, when it changed -- after the spawns, so
    // it never names a part the peer has not been sent.
    if (peer.owned != owned) {
        Writer ownership;
        ownership.u8v(static_cast<u8>(MessageType::Ownership));
        ownership.u32v(static_cast<u32>(owned.size()));
        for (const u32 id : owned)
            ownership.u32v(id);
        sendBytes(m_transport, peer.id, ownership.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        peer.owned = owned;
    }
    // What this peer holds at this tick, for the baseline a later snapshot is
    // diffed against: the global state then, cut to what this peer had then.
    peer.interest.push_back(PeerInterest{currentTick, peer.known});
    while (peer.interest.size() > StateHistory)
        peer.interest.pop_front();

    // --- A snapshot sent in parts and not yet all in: nothing more until it
    // is. Everything above -- spawns, ground, ownership -- still goes,
    // and the next snapshot after the acknowledgement covers what changed.
    if (peer.pinnedPending)
        return;

    // --- The snapshot, against what this peer last proved it holds.
    // A state its budget held something back at, which an acknowledgement has
    // passed, is a baseline no more (ADR 0229).
    while (!peer.heldStates.empty() && peer.heldStates.front().first < peer.acked)
        peer.heldStates.pop_front();
    const WorldState* baseline = peer.acked != 0 ? historyAt(peer.acked) : nullptr;
    const std::vector<u32>* heldThen = nullptr;
    for (const PeerInterest& held : peer.interest) {
        if (held.tick == peer.acked)
            heldThen = &held.ids;
    }
    // **What it holds there, where its budget held something back**: not
    // the authority's state then, but that with the parts it was not sent
    // as they were before.
    const std::vector<EntityState>* heldState = nullptr;
    for (const auto& [tick, state] : peer.heldStates) {
        if (tick == peer.acked)
            heldState = state.get();
    }
    // The one sent in parts, which the history may have dropped by now.
    if ((baseline == nullptr || heldThen == nullptr) && peer.acked != 0 && peer.acked == peer.pinnedTick &&
        peer.pinnedState != nullptr) {
        baseline = peer.pinnedState.get();
        heldThen = &peer.pinnedHeld;
        if (heldState == nullptr)
            heldState = peer.pinnedHeldState.get();
    }
    if (heldThen == nullptr) {
        baseline = nullptr;
        heldState = nullptr;
    }
    const bool based = heldThen != nullptr && (baseline != nullptr || heldState != nullptr);
    if (!based)
        m_stats.snapshotsWhole += 1;
    // What the peer holds of `id` at the baseline.
    const auto heldOf = [&](u32 id) -> const EntityState* {
        if (heldState != nullptr) {
            const auto at =
                std::lower_bound(heldState->begin(), heldState->end(), id,
                                 [](const EntityState& entity, u32 probe) { return entity.id.value < probe; });
            return at != heldState->end() && at->id.value == id ? &*at : nullptr;
        }
        return findEntity(*baseline, id);
    };

    struct Record
    {
        const EntityState* entity = nullptr;
        bool full = false;
        std::vector<usize> fields;
        // What the peer holds of it, when the record is a diff.
        const EntityState* before = nullptr;
        // The names its fields mention, by the authority's atom.
        std::vector<u32> names;
    };
    std::vector<Record> records;
    const usize nameField = commonIndex("Name");
    for (const EntityState* each : current) {
        const EntityState& entity = *each;
        // Diffed only against what this peer HAD at the baseline -- and whole
        // when it is entering now, whatever the baseline says: the replica
        // scrubbed it from every stored state when it left.
        const bool held = based && std::binary_search(heldThen->begin(), heldThen->end(), entity.id.value);
        const bool entered = std::binary_search(entering.begin(), entering.end(), entity.id.value);
        const EntityState* before = held && !entered ? heldOf(entity.id.value) : nullptr;
        // **What comes to the same number is the same** -- nearly all of a
        // world, every tick -- and is left without a field of it compared.
        if (before != nullptr && before->schema == entity.schema && before->hash != 0 && before->hash == entity.hash)
            continue;
        if (before != nullptr)
            m_stats.entitiesCompared += 1;
        Record record{&entity, before == nullptr || before->schema != entity.schema, {}, before, {}};
        const generated::ClassDesc& described = generated::Classes[entity.schema];
        // **The fields, and not the cells past them** (protocol 44): those
        // are their sequences', compared with them and sent as one value.
        for (usize at = 0, count = std::min(fieldCount(described), entity.fields.size()); at < count; ++at) {
            if (record.full || !sameField(described, at, before->fields.view(), entity.fields.view()))
                record.fields.push_back(at);
        }
        if (record.fields.empty())
            continue;
        // **Every name-shaped field, not `Name` alone**: a decal's image is a
        // content URN, and an atom number means nothing on another machine.
        for (const usize at : record.fields) {
            const generated::FieldDesc* field = fieldAt(described, at);
            if (at == nameField || (field != nullptr && field->encoding == generated::Encoding::NameAtom))
                record.names.push_back(asU32(entity.fields[at]));
        }
        records.push_back(std::move(record));
    }

    // **Each record's bytes, in id order**, where it begins kept: what the
    // budget weighs, and what is copied out of here into the snapshot.
    Writer body;
    std::vector<usize> starts;
    starts.reserve(records.size() + 1);
    for (const Record& record : records) {
        starts.push_back(body.bytes.size());
        const generated::ClassDesc& desc = generated::Classes[record.entity->schema];
        body.u32v(record.entity->id.value);
        body.u8v(record.entity->schema);
        body.u8v(record.full ? FullRecord : 0);
        body.u16v(static_cast<u16>(record.fields.size()));
        for (const usize at : record.fields) {
            const generated::Encoding encoding = fieldAt(desc, at)->encoding;
            if (encoding == generated::Encoding::CFrameD && !record.full &&
                sameRotation(record.before->fields[at], record.entity->fields[at])) {
                body.u16v(static_cast<u16>(wireIdAt(desc, at) | PositionAlone));
                encodePosition(body.bytes, record.entity->fields[at]);
                continue;
            }
            body.u16v(wireIdAt(desc, at));
            if (isSequence(encoding)) {
                encodeSequenceField(body.bytes, desc, at, record.entity->fields.view());
                continue;
            }
            encodeField(body.bytes, encoding, record.entity->fields[at]);
        }
    }
    starts.push_back(body.bytes.size());

    // --- **Within its budget** (ADR 0229): what matters most to this peer
    // first, and the rest when its turn comes.
    std::vector<u8> chosen(records.size(), 1);
    // The records sent now that had waited.
    std::vector<u32> heldBack;
    std::shared_ptr<std::vector<EntityState>> heldNow;
    u32 deferred = 0;
    const scene::PlayerComponent* player = peer.player.valid() ? m_world->players().find(peer.player) : nullptr;
    const f64 perSecond = player != nullptr && player->replicationBudgetKb > 0.0f
                              ? static_cast<f64>(player->replicationBudgetKb) * 1024.0
                              : static_cast<f64>(m_clientBudget);
    if (perSecond <= 0.0) {
        peer.waiting.clear();
    }
    else {
        const f64 most = perSecond * BudgetBurstSeconds;
        const u64 since = peer.budgetTick != 0 && currentTick > peer.budgetTick ? currentTick - peer.budgetTick : 0;
        const f64 given = peer.budgetTick == 0 ? most : perSecond * static_cast<f64>(since) / TicksPerSecond;
        peer.budgetBytes = std::min(most, peer.budgetBytes + given);
        peer.budgetTick = currentTick;
        m_windowGiven += perSecond * static_cast<f64>(since) / TicksPerSecond;
        // **A part new to the peer is not counted** (ADR 0229 §1): sent
        // whole, at once, as the rest of what a newcomer is sent is.
        f64 whole = 0.0;
        for (usize at = 0; at < records.size(); ++at) {
            if (records[at].full)
                whole += static_cast<f64>(starts[at + 1] - starts[at]);
        }
        const f64 total = static_cast<f64>(body.bytes.size()) - whole;
        if (total <= peer.budgetBytes) {
            peer.budgetBytes -= total;
            m_windowSpent += total;
            peer.waiting.clear();
        }
        else {
            std::vector<RecordFacts> facts;
            facts.reserve(records.size());
            for (const Record& record : records)
                facts.push_back(RecordFacts{record.entity->id.value, record.full});
            chooseWithinBudget(peer, player, facts, starts, chosen, heldBack);
            for (usize at = 0; at < records.size(); ++at) {
                if (chosen[at] == 0) {
                    deferred += 1;
                    continue;
                }
                if (records[at].full)
                    continue;
                const f64 size = static_cast<f64>(starts[at + 1] - starts[at]);
                peer.budgetBytes -= size;
                m_windowSpent += size;
            }
            peer.budgetBytes = std::max(peer.budgetBytes, -most);
        }
    }
    // **A record that waited says so** (protocol 55): the replica draws that
    // part further in the past, between the states it was sent.
    for (usize at = 0; at < records.size(); ++at) {
        if (chosen[at] != 0 && std::binary_search(heldBack.begin(), heldBack.end(), records[at].entity->id.value))
            body.bytes[starts[at] + 5] |= HeldBackRecord;
    }
    if (deferred != 0) {
        m_stats.updatesDeferred += deferred;
        m_windowDeferred += deferred;
        // What the peer will hold at this snapshot: what it was sent, and
        // what it was not as it holds it now.
        heldNow = std::make_shared<std::vector<EntityState>>();
        heldNow->reserve(current.size());
        usize at = 0;
        for (const EntityState* entity : current) {
            if (at < records.size() && records[at].entity == entity) {
                heldNow->push_back(chosen[at] != 0 ? *entity : *records[at].before);
                ++at;
                continue;
            }
            heldNow->push_back(*entity);
        }
    }

    std::set<u32> atoms;
    u32 sending = 0;
    for (usize at = 0; at < records.size(); ++at) {
        if (chosen[at] == 0)
            continue;
        sending += 1;
        atoms.insert(records[at].names.begin(), records[at].names.end());
    }

    Writer snapshot;
    snapshot.u8v(static_cast<u8>(MessageType::Snapshot));
    snapshot.u64v(currentTick);
    snapshot.u64v(based ? peer.acked : 0);
    if (heldNow != nullptr) {
        std::vector<const EntityState*> holding;
        holding.reserve(heldNow->size());
        for (const EntityState& entity : *heldNow)
            holding.push_back(&entity);
        snapshot.u64v(checksumOf(std::span<const EntityState* const>(holding)));
    }
    else {
        snapshot.u64v(checksumOf(std::span<const EntityState* const>(current)));
    }
    // The last of this peer's intents the authority applied, for its
    // prediction to reconcile against (ADR 0076).
    snapshot.u64v(peer.intentTick);
    // How this peer's input buffer stands (protocol 22): what its own overlay
    // shows, which only the authority can see.
    {
        const u64 newest = peer.intentQueue.empty() ? peer.appliedTick : peer.intentQueue.rbegin()->first;
        snapshot.u8v(static_cast<u8>(std::min<u64>(newest > peer.appliedTick ? newest - peer.appliedTick : 0, 255)));
        snapshot.u32v(static_cast<u32>(std::min<u64>(peer.starvations, 0xFFFFFFFFu)));
    }
    // **The rate the world runs at** (ADR 0233, protocol 55), in ten
    // thousandths: the replica's clock runs at it too.
    snapshot.u16v(static_cast<u16>(std::lround(std::clamp(m_world->engineState().timeDilation, 0.0, 1.0) * 10000.0)));
    // **Its own character's predicted attributes** (protocol 37): what
    // the steps of this peer's ticks wrote, as they stand after the one this
    // state answers -- the replica's prediction of them is checked against
    // it, and put right by it.
    {
        const auto netOf = [this](InstanceId id) { return netIdOf(id).value; };
        // What one of the peer's players' characters has predicted, and how
        // many of them: the count goes in front of the entries.
        const auto predictedOf = [this, &netOf](InstanceId seated, Writer& entries) -> u16 {
            const scene::PlayerComponent* player = seated.valid() ? m_world->players().find(seated) : nullptr;
            const scene::CharacterBodyComponent* body = player != nullptr && player->character.valid()
                                                            ? m_world->characterBodies().find(player->character)
                                                            : nullptr;
            u16 count = 0;
            if (body == nullptr)
                return count;
            for (const core::NameAtom name : body->predictedAttributes) {
                if (count == MaxPredictedAttributes)
                    break;
                // **Its name as a hash** (protocol 40): the replica's own
                // steps wrote the same attributes and it knows them by the
                // same hash -- where the name itself was most of what every
                // snapshot carried, thirty times a second.
                Writer entry;
                entry.u32v(core::hashTextKey(m_world->atoms().text(name)));
                if (!writeAttributeValue(entry, m_world->getAttribute(player->character, name), netOf))
                    continue;
                entries.bytes.insert(entries.bytes.end(), entry.bytes.begin(), entry.bytes.end());
                ++count;
            }
            return count;
        };
        Writer entries;
        const u16 count = predictedOf(peer.player, entries);
        snapshot.u16v(count);
        snapshot.bytes.insert(snapshot.bytes.end(), entries.bytes.begin(), entries.bytes.end());
        // **And each of its guests' characters'** (ADR 0208, protocol 51): a
        // guest's character is predicted on the machine it plays at as the
        // first player's is, so what its steps wrote is answered the same
        // way. A seat with nothing predicted is not written -- a machine with
        // no guest, which is nearly every one, pays the one byte that says so.
        std::array<Writer, 3> seats;
        std::array<u16, 3> counts{};
        u8 written = 0;
        for (usize seat = 0; seat < peer.guests.size(); ++seat) {
            counts[seat] = predictedOf(peer.guests[seat].player, seats[seat]);
            written += counts[seat] != 0 ? 1 : 0;
        }
        snapshot.u8v(written);
        for (usize seat = 0; seat < peer.guests.size(); ++seat) {
            if (counts[seat] == 0)
                continue;
            snapshot.u8v(static_cast<u8>(seat + 1));
            snapshot.u16v(counts[seat]);
            snapshot.bytes.insert(snapshot.bytes.end(), seats[seat].bytes.begin(), seats[seat].bytes.end());
        }
    }
    // The names this message's fields mention, by the authority's atom. The
    // replica interns each once and keeps the mapping, so a name costs its
    // bytes on the wire when it changes rather than every tick. A count of 32
    // bits (protocol 33): sixteen wrapped past 65 535 names.
    snapshot.u32v(static_cast<u32>(atoms.size()));
    for (const u32 atom : atoms) {
        snapshot.u32v(atom);
        snapshot.text(m_world->atoms().text(core::NameAtom{atom}));
    }
    snapshot.u32v(sending);
    for (usize at = 0; at < records.size(); ++at) {
        if (chosen[at] != 0)
            snapshot.bytes.insert(snapshot.bytes.end(), body.bytes.begin() + static_cast<std::ptrdiff_t>(starts[at]),
                                  body.bytes.begin() + static_cast<std::ptrdiff_t>(starts[at + 1]));
    }
    if (heldNow != nullptr) {
        peer.heldStates.emplace_back(currentTick, heldNow);
        while (peer.heldStates.size() > StateHistory)
            peer.heldStates.pop_front();
    }
    peer.sentTicks.push_back(currentTick);
    while (peer.sentTicks.size() > StateHistory)
        peer.sentTicks.pop_front();
    if (snapshot.bytes.size() <= ReliableSnapshotBytes) {
        if (sendBytes(m_transport, peer.id, snapshot.bytes, net::Delivery::UnreliableSequenced, StateChannel, m_stats))
            m_stats.snapshotsSent += 1;
        return;
    }
    // **Too large for one unreliable message: reliable, in parts, once**.
    // On the control channel, after the spawns it names; kept with
    // what this peer held, since the history may move past it before the
    // last part is in; and nothing more to this peer until it is.
    const usize parts = (snapshot.bytes.size() + SnapshotPartBytes - 1) / SnapshotPartBytes;
    bool whole = parts <= 0xFFFFu;
    for (usize index = 0; index < parts && whole; ++index) {
        const usize from = index * SnapshotPartBytes;
        const usize size = std::min(SnapshotPartBytes, snapshot.bytes.size() - from);
        Writer part;
        part.u8v(static_cast<u8>(MessageType::SnapshotPart));
        part.u64v(currentTick);
        part.u16v(static_cast<u16>(index));
        part.u16v(static_cast<u16>(parts));
        part.u32v(static_cast<u32>(size));
        part.bytes.insert(part.bytes.end(), snapshot.bytes.begin() + static_cast<std::ptrdiff_t>(from),
                          snapshot.bytes.begin() + static_cast<std::ptrdiff_t>(from + size));
        whole = sendBytes(m_transport, peer.id, part.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
    }
    if (!whole)
        return;
    m_stats.snapshotsSent += 1;
    m_stats.snapshotsInParts += 1;
    peer.pinnedState = m_history.back();
    peer.pinnedHeld = peer.known;
    peer.pinnedHeldState = heldNow;
    peer.pinnedTick = currentTick;
    peer.pinnedPending = true;
}

// --- Replica ------------------------------------------------------------------

InstanceId ReplicaSession::localOf(NetId id) const noexcept
{
    const auto found = m_locals.find(id.value);
    return found != m_locals.end() ? found->second : InstanceId{};
}

const WorldState* ReplicaSession::stateAt(u64 tick) const noexcept
{
    for (auto at = m_states.rbegin(); at != m_states.rend(); ++at) {
        if ((*at)->tick == tick)
            return at->get();
    }
    return nullptr;
}

void ReplicaSession::receive(scene::World& world, InstanceId root, bool ticking)
{
    // **An authority silent for ten seconds is gone** (`SilentPeerTicks`): the
    // transport's thread answers for a server whose game froze, and the
    // transport alone would never have said so.
    if (ticking && m_welcomed && ++m_quietTicks > SilentPeerTicks) {
        m_quietTicks = 0;
        m_transport.disconnect(m_authority);
        m_connected = false;
        m_welcomed = false;
        m_lost = true;
    }
    std::vector<net::TransportEvent> events;
    (void)m_transport.poll(events, 0);
    u32 remotesThisTick = 0;
    for (const net::TransportEvent& event : events) {
        if (!(event.peer == m_authority))
            continue;
        if (event.kind == net::TransportEvent::Kind::Connected) {
            m_connected = true;
            m_lost = false;
            Writer hello;
            hello.u8v(static_cast<u8>(MessageType::Hello));
            hello.u32v(generated::ProtocolVersion);
            hello.u64v(m_token.high);
            hello.u64v(m_token.low);
            hello.u64v(m_luauFlags);
            hello.text(std::string_view{m_ticket}.substr(0, MaxTicketBytes));
            hello.text(std::string_view{m_pass}.substr(0, MaxPassBytes));
            sendBytes(m_transport, m_authority, hello.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
            continue;
        }
        if (event.kind == net::TransportEvent::Kind::Disconnected) {
            m_connected = false;
            m_welcomed = false;
            m_lost = true;
            if (event.failure != net::ConnectFailure::None)
                m_joinFailure = event.failure;
            if (event.failure == net::ConnectFailure::KeyChanged)
                m_refused = RefusedServerKey;
            else if (event.failure == net::ConnectFailure::Insecure)
                m_refused = RefusedNotEncrypted;
            continue;
        }
        if (event.kind != net::TransportEvent::Kind::Message || event.payload.empty())
            continue;
        m_quietTicks = 0;
        m_stats.bytesReceived += event.payload.size();
        countBytes(m_stats, event.payload);
        switch (static_cast<MessageType>(event.payload[0])) {
        case MessageType::Welcome: {
            Reader reader(event.payload);
            (void)reader.u8v();
            const u32 version = reader.u32v();
            const u32 player = reader.u32v();
            (void)reader.u64v();
            PlayerToken token;
            token.high = reader.u64v();
            token.low = reader.u64v();
            if (!reader.ok() || version != generated::ProtocolVersion) {
                m_refused = RefusedVersion;
                m_transport.disconnect(m_authority);
                break;
            }
            if (!m_joinedBefore && m_welcomeHandler)
                m_welcomeHandler(world);
            if (m_joinedBefore)
                resetForRejoin(world);
            // The token shown was not known: this authority is not the one
            // that gave it, and this is a new player in a new world.
            if (!m_joinedBefore || !(token == m_token))
                ++m_freshJoins;
            m_joinedBefore = true;
            m_welcomed = true;
            // A new connection knows nothing of this machine's guests: each
            // asks for its seat again, and has no number until it is answered.
            for (GuestSeat& guest : m_guests) {
                guest.asked = false;
                guest.userId = 0;
            }
            // A new connection counts its unreliable messages from the start.
            m_unreliableOut = 0;
            m_unreliableIn = 0;
            m_unreliableHeard = false;
            m_playerId = player;
            m_token = token;
            // The player at this machine takes the number the authority gave
            // it, so `LocalPlayer.UserId` on a replica is the same number the
            // authority's copy of that player has -- and the key it proved,
            // which the authority has too (ADR 0224).
            if (const InstanceId local = scene::localPlayerOf(world); local.valid()) {
                if (scene::PlayerComponent* component = world.players().find(local); component != nullptr) {
                    component->userId = player;
                    const std::optional<std::array<u8, 32>> key = m_transport.localKey();
                    component->publicKey = key.has_value() ? core::toHex(*key) : std::string{};
                }
                world.setName(local, world.atoms().intern("Player" + std::to_string(player)));
            }
            break;
        }
        case MessageType::Spawn:
            onSpawn(world, event.payload);
            break;
        case MessageType::Despawn:
            onDespawn(world, event.payload);
            break;
        case MessageType::Snapshot:
            onSnapshot(world, root, event.payload, false);
            break;
        case MessageType::SnapshotPart:
            onSnapshotPart(world, root, event.payload);
            break;
        case MessageType::Refused: {
            Reader reader(event.payload);
            (void)reader.u8v();
            const u8 reason = reader.u8v();
            // Its words, from protocol 40: an authority of another version
            // says only that it is one.
            const std::string_view text = reader.remaining() >= 2 ? reader.text() : std::string_view{};
            m_refused = reader.ok() && reason != 0 ? reason : RefusedVersion;
            m_refusedText.assign(reader.ok() ? text.substr(0, MaxRemovedReasonBytes) : std::string_view{});
            m_lost = true;
            m_welcomed = false;
            break;
        }
        case MessageType::Redirect: {
            // **Sent elsewhere** (ADRs 0220, 0222): this connection is let go,
            // and the session joins where it was told, with the pass.
            Reader reader(event.payload);
            (void)reader.u8v();
            const std::string_view address = reader.text();
            const u16 port = reader.u16v();
            const std::string_view pass = reader.text();
            const std::string_view key = reader.text();
            const std::string_view where = reader.remaining() >= 2 ? reader.text() : std::string_view{};
            if (!reader.ok() || address.empty() || port == 0)
                break;
            m_redirectWorld.assign(where.substr(0, 64));
            m_redirectAddress.assign(address);
            m_redirectPort = port;
            m_redirectPass.assign(pass.substr(0, MaxPassBytes));
            m_redirectKey.assign(key.substr(0, 64));
            ++m_redirects;
            m_transport.disconnect(m_authority);
            break;
        }
        case MessageType::Phases: {
            // **The phases one of this machine's players is in** (ADR 0231).
            Reader reader(event.payload);
            (void)reader.u8v();
            const u8 seat = reader.u8v();
            const u16 count = reader.u16v();
            std::vector<core::NameAtom> phases;
            for (u16 at = 0; at < count && reader.ok() && at < scene::MostPhases; ++at)
                phases.push_back(world.atoms().intern(reader.text()));
            if (!reader.ok())
                break;
            std::vector<InstanceId> here;
            scene::localPlayers(world, here);
            if (seat < here.size())
                if (scene::PlayerComponent* player = world.players().find(here[seat]); player != nullptr)
                    player->phases = std::move(phases);
            break;
        }
        case MessageType::Queued: {
            // **In line at the front door** (ADR 0232): the game is full; the
            // turn is a `Redirect`.
            Reader reader(event.payload);
            (void)reader.u8v();
            const u32 position = reader.u32v();
            const u32 estimate = reader.u32v();
            if (!reader.ok() || position == 0)
                break;
            m_queuePosition = position;
            world.engineState().queueUpdates.push_back(
                scene::EngineState::QueueUpdate{position, estimate == NoEstimate ? -1 : static_cast<i64>(estimate)});
            break;
        }
        case MessageType::Announce: {
            // **What the server's operator said** (ADR 0218): for this
            // machine's scripts to hear, between ticks.
            Reader reader(event.payload);
            (void)reader.u8v();
            const std::string_view text = reader.text();
            if (reader.ok())
                world.engineState().serverMessages.emplace_back(text.substr(0, MaxRemovedReasonBytes));
            break;
        }
        case MessageType::Players:
            onPlayers(world, root, event.payload);
            break;
        case MessageType::GuestSeat: {
            // **Who sits in one of this machine's seats** (protocol 48): the
            // number the authority gave the guest, which its `Player` here
            // takes as this machine's first took its own on being welcomed --
            // or nobody, and the guest leaves.
            Reader reader(event.payload);
            (void)reader.u8v();
            const u8 seat = reader.u8v();
            const u32 userId = reader.u32v();
            if (!reader.ok() || !reader.done() || seat == 0 || seat > m_guests.size())
                break;
            GuestSeat& guest = m_guests[seat - 1u];
            scene::PlayerComponent* component =
                world.alive(guest.player) ? world.players().find(guest.player) : nullptr;
            if (component == nullptr || !guest.asked)
                break;
            if (userId != 0) {
                guest.userId = userId;
                component->userId = userId;
                const std::optional<std::array<u8, 32>> key = m_transport.localKey();
                component->publicKey = key.has_value() ? guestKeyOf(*key, seat) : std::string{};
                world.setName(guest.player, world.atoms().intern("Player" + std::to_string(userId)));
                world.changes().push(scene::Change{
                    scene::ChangeKind::PropertyChanged, guest.player, {}, world.atoms().intern("UserId")});
            }
            else if (const InstanceId network = scene::networkServiceOf(world, world.parentOf(root)); network.valid()) {
                // Refused, or taken out by the authority: not a player here.
                const InstanceId gone = guest.player;
                guest = GuestSeat{};
                scene::removePlayer(world, network, gone);
            }
            break;
        }
        case MessageType::Ownership:
            onOwnership(world, event.payload);
            break;
        case MessageType::TilemapBlocks:
            onTilemapBlocks(world, event.payload);
            break;
        case MessageType::TerrainChunks:
            onTerrainChunks(world, root, event.payload);
            break;
        case MessageType::TerrainLook:
            onTerrainLook(world, root, event.payload);
            break;
        case MessageType::VoxelChunks:
            onVoxelChunks(world, event.payload);
            break;
        case MessageType::VoxelTypes:
            onVoxelTypes(world, event.payload);
            break;
        case MessageType::SceneChange:
            onSceneChange(world, event.payload);
            break;
        case MessageType::Attributes:
            onAttributes(world, root, event.payload);
            break;
        case MessageType::AttributeEdits:
            onAttributeEdits(world, root, event.payload);
            break;
        case MessageType::CollisionGroups:
            onCollisionGroups(world, event.payload);
            break;
        case MessageType::SwarmAgents:
            onSwarmAgents(world, event.payload);
            break;
        case MessageType::SwarmState:
            onSwarmState(world, event.payload);
            break;
        case MessageType::UnreliableToReplica:
            if (++remotesThisTick > MaxReplicaRemotesPerTick)
                m_stats.unreliableDropped += 1;
            else
                onUnreliable(world, event.payload);
            break;
        case MessageType::RemoteToReplica: {
            Reader reader(event.payload);
            (void)reader.u8v();
            RemoteOnWire wire;
            if (++remotesThisTick > MaxReplicaRemotesPerTick || !readRemote(reader, wire)) {
                m_stats.messagesDropped += 1;
                break;
            }
            const auto local = m_locals.find(wire.remote);
            const bool reply = (wire.flags & RemoteReplyFlag) != 0;
            // The authority answers and never asks: a call number here is an
            // answer or nothing.
            if (local == m_locals.end() || !isRemoteTarget(world, local->second, wire.call) ||
                reply != (wire.call != 0)) {
                m_stats.messagesDropped += 1;
                break;
            }
            scene::RemoteMessage message;
            message.remote = local->second;
            message.call = wire.call;
            message.reply = reply;
            message.failed = (wire.flags & RemoteFailedFlag) != 0;
            // For one of this machine's guests (protocol 48): its scripts
            // hear it as that player's.
            if (const u8 seat = static_cast<u8>((wire.flags & RemoteSeatMask) >> RemoteSeatShift); seat != 0) {
                if (m_guests[seat - 1u].userId == 0 || !world.alive(m_guests[seat - 1u].player)) {
                    m_stats.messagesDropped += 1;
                    break;
                }
                message.player = m_guests[seat - 1u].player;
            }
            message.payload.assign(wire.payload.begin(), wire.payload.end());
            // An instance this machine was never sent arrives as nil.
            for (const u32 ref : wire.refs) {
                const auto found = m_locals.find(ref);
                message.refs.push_back(found != m_locals.end() ? found->second : InstanceId{});
            }
            world.engineState().remoteInbox.push_back(std::move(message));
            m_stats.messagesReceived += 1;
            break;
        }
        default:
            break;
        }
    }
    resolveCharacters(world, root);
    if (!ticking)
        return;
    updatePredicted(world);
    m_serverClock += 1;
    // The delay follows what the link needs: two snapshot intervals and twice
    // the spread of their lateness, never less than it was set to. A tick at
    // a time, every half second, so nothing drawn jumps when it moves.
    if (m_baseDelay > 0 && ++m_delayTicks >= 30) {
        m_delayTicks = 0;
        const u32 wanted =
            std::clamp(static_cast<u32>(std::ceil(2.0 * m_snapshotInterval + 2.0 * m_lateSpread + m_lateAverage)),
                       m_baseDelay, m_baseDelay + MaxAddedInterpolationDelay);
        if (wanted > m_interpolationDelay)
            m_interpolationDelay += 1;
        else if (wanted < m_interpolationDelay)
            m_interpolationDelay -= 1;
    }
    m_stats.interpolationDelayTicks = m_interpolationDelay;
    interpolate(world);
    decayVisualOffset();
}

void ReplicaSession::updatePredicted(scene::World& world)
{
    const auto release = [&](u32 netId) {
        const auto local = m_locals.find(netId);
        if (local != m_locals.end() && world.alive(local->second)) {
            if (scene::RigidBodyComponent* body = world.rigidBodies().find(local->second); body != nullptr)
                body->predicted = false;
        }
    };
    // **Round every character this machine predicts** (ADR 0208): a part in
    // reach of any of its players' is predicted, under the machine's one
    // cap -- what it costs is the island's, not a character's.
    std::vector<core::DVec3> centres;
    for (const Owned& seat : m_seats) {
        const auto own = seat.netId != 0 && seat.synced ? m_locals.find(seat.netId) : m_locals.end();
        const scene::PartComponent* character =
            own != m_locals.end() && world.alive(own->second) ? world.parts().find(own->second) : nullptr;
        // **Only round a character that can be stepped again**. A
        // part is corrected by putting the island back and stepping it once
        // more with the character's commands, and a character that is a part
        // its player's script moves has none: no controller, no step to take
        // again. Parts predicted round one were found wrong at every answer
        // and never put right -- crates falling by a racer as it joined were
        // a round trip of their fall from the authority's, metres, sixteen
        // corrections in half a second. Round such a character everything is
        // drawn from what the authority sends.
        if (character != nullptr && world.characterBodies().find(own->second) != nullptr)
            centres.push_back(character->cframe.position);
    }
    // **And the vehicle each of them drives** (ADR 0213): its chassis is the
    // middle of an island too, and the nearest thing in it -- a car that
    // answered its wheel a round trip late would not be drivable. Only for a
    // driver whose character can be stepped again, for the reason above: a
    // correction steps the vehicle by that character's commands.
    std::vector<core::InstanceId> wheelParts;
    world.vehicleControllers().forEach([&](core::InstanceId id, const scene::VehicleControllerComponent& vehicle) {
        const scene::PlayerComponent* driver = nullptr;
        world.players().forEach([&](core::InstanceId, const scene::PlayerComponent& player) {
            if (vehicle.driver != 0 && player.userId == vehicle.driver && player.local)
                driver = &player;
        });
        if (driver == nullptr || world.characterBodies().find(driver->character) == nullptr)
            return;
        const core::InstanceId chassis = world.parentOf(id);
        const scene::PartComponent* part = world.parts().find(chassis);
        if (part == nullptr)
            return;
        centres.push_back(part->cframe.position);
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
            if (const scene::VehicleWheelComponent* wheel = world.vehicleWheels().find(child);
                wheel != nullptr && wheel->wheelPart.valid())
                wheelParts.push_back(wheel->wheelPart);
        }
    });
    m_drivenWheels.clear();
    for (const auto& [netId, local] : m_locals) {
        if (std::find(wheelParts.begin(), wheelParts.end(), local) != wheelParts.end()) {
            m_drivenWheels.insert(netId);
            m_samples.erase(netId);
        }
    }
    // Only where there is a simulation to predict with: a replica with no
    // physics replay -- a test's bare world -- follows every part.
    if (centres.empty() || m_predictMax == 0 || m_replay == nullptr) {
        for (const auto& [netId, away] : m_predictedParts)
            release(netId);
        m_predictedParts.clear();
        return;
    }

    // In range: loose, nobody's, not a character, nearest first (R10: ties by
    // id) -- and what those touch, one step out, so a crate pushed into
    // another past the radius meets a crate that moves rather than a wall.
    struct Candidate
    {
        f64 distance = 0.0;
        u32 netId = 0;
        core::DVec3 at{};
        f64 reach = 0.0;
    };
    std::vector<Candidate> loose;
    for (const auto& [netId, local] : m_locals) {
        if (ownsNet(netId) || !world.alive(local) || world.characterBodies().find(local) != nullptr)
            continue;
        const scene::RigidBodyComponent* body = world.rigidBodies().find(local);
        const scene::PartComponent* part = world.parts().find(local);
        if (body == nullptr || part == nullptr || body->anchored || body->networkOwner != 0)
            continue;
        // How far from the nearest of them.
        f64 nearest = std::numeric_limits<f64>::infinity();
        for (const core::DVec3& centre : centres) {
            const core::DVec3 d = part->cframe.position - centre;
            nearest = std::min(nearest, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
        }
        const core::Vec3& size = part->size;
        loose.push_back(
            Candidate{.distance = nearest,
                      .netId = netId,
                      .at = part->cframe.position,
                      .reach = 0.5 * std::sqrt(static_cast<f64>(size.x * size.x + size.y * size.y + size.z * size.z))});
    }
    std::vector<std::pair<f64, u32>> near;
    std::vector<const Candidate*> inRange;
    for (const Candidate& candidate : loose) {
        if (candidate.distance <= m_predictRadius) {
            near.emplace_back(candidate.distance, candidate.netId);
            inRange.push_back(&candidate);
        }
    }
    for (const Candidate& candidate : loose) {
        if (candidate.distance <= m_predictRadius)
            continue;
        for (const Candidate* held : inRange) {
            const core::DVec3 d = candidate.at - held->at;
            if (std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) <= candidate.reach + held->reach + PredictTouchMetres) {
                near.emplace_back(candidate.distance, candidate.netId);
                break;
            }
        }
    }
    std::sort(near.begin(), near.end());
    if (near.size() > m_predictMax)
        near.resize(m_predictMax);
    std::set<u32> wanted;
    for (const auto& entry : near)
        wanted.insert(entry.second);

    // Out of range long enough: drawn between snapshots again.
    for (auto at = m_predictedParts.begin(); at != m_predictedParts.end();) {
        if (wanted.contains(at->first)) {
            at->second = 0;
            ++at;
            continue;
        }
        if (++at->second > m_predictLinger) {
            release(at->first);
            at = m_predictedParts.erase(at);
            continue;
        }
        ++at;
    }

    // In: from the newest state the authority sent, never the drawn one,
    // which is the interpolation's, ticks behind.
    const WorldState* newest = m_states.empty() ? nullptr : m_states.back().get();
    for (const u32 netId : wanted) {
        if (m_predictedParts.contains(netId) || m_predictedParts.size() >= m_predictMax)
            continue;
        const InstanceId local = m_locals.at(netId);
        scene::PartComponent* part = world.parts().find(local);
        scene::RigidBodyComponent* body = world.rigidBodies().find(local);
        const EntityState* held = newest != nullptr ? findEntity(*newest, netId) : nullptr;
        if (held != nullptr) {
            const generated::ClassDesc& desc = generated::Classes[held->schema];
            for (usize at = 0; at < held->fields.size(); ++at) {
                const generated::FieldDesc* field = fieldAt(desc, at);
                if (field == nullptr)
                    continue;
                if (field->name == "CFrame" && field->pool == "parts")
                    part->cframe = asCFrame(held->fields[at]);
                else if (field->name == "LinearVelocity" && field->pool == "rigidBodies")
                    body->linearVelocity = asVec3(held->fields[at]);
                else if (field->name == "AngularVelocity" && field->pool == "rigidBodies")
                    body->angularVelocity = asVec3(held->fields[at]);
            }
        }
        body->predicted = true;
        m_samples.erase(netId);
        m_predictedParts.emplace(netId, 0u);
    }
}

bool ReplicaSession::carriesVehicle(const scene::World& world, core::InstanceId part)
{
    for (core::InstanceId child = world.firstChild(part); child.valid(); child = world.nextSibling(child)) {
        if (world.vehicleControllers().find(child) != nullptr)
            return true;
    }
    return false;
}

ReplicaSession::DrivenVehicle ReplicaSession::drivenBy(const scene::World& world, core::InstanceId character)
{
    DrivenVehicle found;
    u32 driver = 0;
    world.players().forEach([&](core::InstanceId, const scene::PlayerComponent& player) {
        if (driver == 0 && player.character == character)
            driver = player.userId;
    });
    if (driver == 0)
        return found;
    world.vehicleControllers().forEach([&](core::InstanceId id, const scene::VehicleControllerComponent& vehicle) {
        if (found.chassis.valid() || vehicle.driver != driver)
            return;
        found.chassis = world.parentOf(id);
        found.root = found.chassis;
        for (core::InstanceId up = world.parentOf(found.chassis); up.valid(); up = world.parentOf(up)) {
            if (world.models().find(up) != nullptr) {
                found.root = up;
                break;
            }
        }
    });
    return found;
}

void ReplicaSession::decayVisualOffset() noexcept
{
    const auto decay = [](core::DVec3& offset, bool& fresh, f64 kept) {
        if (fresh) {
            fresh = false;
            return;
        }
        offset = core::DVec3{offset.x * kept, offset.y * kept, offset.z * kept};
        if (offset.x * offset.x + offset.y * offset.y + offset.z * offset.z < 1e-8)
            offset = core::DVec3{};
    };
    for (Owned& seat : m_seats) {
        decay(seat.visualOffset, seat.visualFresh, VisualDecayPerTick);
        decay(seat.vehicleOffset, seat.vehicleFresh, VehicleVisualDecayPerTick);
    }
}

void ReplicaSession::onSwarmAgents(scene::World& world, std::span<const u8> payload)
{
    Reader reader(payload);
    (void)reader.u8v();
    const u32 netId = reader.u32v();
    const f64 tick = static_cast<f64>(reader.u32v());
    const f64 originX = static_cast<f64>(static_cast<core::i32>(reader.u32v()));
    const f64 originY = static_cast<f64>(static_cast<core::i32>(reader.u32v()));
    const f64 originZ = static_cast<f64>(static_cast<core::i32>(reader.u32v()));
    const u16 membership = reader.u16v();
    const f64 floor = originY + static_cast<f64>(readSwarmF32(reader));
    const auto local = m_locals.find(netId);
    scene::SwarmComponent* swarm = local != m_locals.end() ? world.swarms().find(local->second) : nullptr;
    if (!reader.ok() || swarm == nullptr) {
        m_stats.messagesDropped += 1;
        return;
    }
    swarm->mirrored = true;
    // Reliable and in order: this is the next of them. Said to the authority,
    // which sends an agent's position only once its coming was taken in.
    swarm->mirrorMembership = membership;
    const auto held = std::find_if(m_swarmMemberships.begin(), m_swarmMemberships.end(),
                                   [netId](const std::pair<u32, u16>& entry) { return entry.first == netId; });
    if (held == m_swarmMemberships.end())
        m_swarmMemberships.emplace_back(netId, membership);
    else
        held->second = membership;
    m_swarmAckDue = true;
    const u16 goneCount = reader.u16v();
    // Seventeen bytes a going, twenty-two a coming: a count the message
    // cannot hold is refused before anything is done for it.
    if (!reader.ok() || reader.remaining() < static_cast<usize>(goneCount) * 17u) {
        m_stats.messagesDropped += 1;
        return;
    }
    for (u16 at = 0; at < goneCount; ++at) {
        const u16 slot = reader.u16v();
        const u8 reason = reader.u8v();
        const u16 tag = reader.u16v();
        const f32 x = readSwarmF32(reader);
        const f32 y = readSwarmF32(reader);
        const f32 z = readSwarmF32(reader);
        scene::mirrorSwarmAgentRemoved(
            *swarm, slot, reason == scene::SwarmRemovalOutOfReach ? reason : scene::SwarmRemovalRemoved, tag,
            core::DVec3{originX + static_cast<f64>(x), originY + static_cast<f64>(y), originZ + static_cast<f64>(z)});
        m_stats.swarmRemoved += 1;
    }
    const u16 addedCount = reader.u16v();
    if (!reader.ok() || reader.remaining() < static_cast<usize>(addedCount) * 24u) {
        m_stats.messagesDropped += 1;
        return;
    }
    for (u16 at = 0; at < addedCount; ++at) {
        const u16 slot = reader.u16v();
        const u16 tag = reader.u16v();
        const f32 radius = static_cast<f32>(reader.u8v()) / 16.0f;
        const f32 height = static_cast<f32>(reader.u8v()) / 8.0f;
        const f32 x = readSwarmF32(reader);
        const f32 y = readSwarmF32(reader);
        const f32 z = readSwarmF32(reader);
        const f32 lift = readSwarmF32(reader);
        const f32 yaw = unpackYaw(reader.u8v());
        const f32 walk = static_cast<f32>(reader.u8v() & 0x3fu) / 4.0f;
        scene::SwarmTold told;
        told.position =
            core::DVec3{originX + static_cast<f64>(x), originY + static_cast<f64>(y), originZ + static_cast<f64>(z)};
        told.lift = std::max(lift, 0.0f);
        told.floor = floor;
        told.yaw = yaw;
        told.walk = walk;
        scene::mirrorSwarmAgentAdded(*swarm, slot, tag, std::max(radius, 0.01f), std::max(height, 0.01f), told, tick);
        m_stats.swarmAdded += 1;
    }
}

void ReplicaSession::onSwarmState(scene::World& world, std::span<const u8> payload)
{
    Reader reader(payload);
    (void)reader.u8v();
    const u32 netId = reader.u32v();
    const f64 tick = static_cast<f64>(reader.u32v());
    const u16 sequence = reader.u16v();
    const f64 originX = static_cast<f64>(static_cast<core::i32>(reader.u32v()));
    const f64 originY = static_cast<f64>(static_cast<core::i32>(reader.u32v()));
    const f64 originZ = static_cast<f64>(static_cast<core::i32>(reader.u32v()));
    const f64 floor = originY + static_cast<f64>(readSwarmF32(reader));
    const u16 size = reader.u16v();
    const auto local = m_locals.find(netId);
    scene::SwarmComponent* swarm = local != m_locals.end() ? world.swarms().find(local->second) : nullptr;
    if (!reader.ok() || swarm == nullptr || reader.remaining() != size) {
        m_stats.messagesDropped += 1;
        return;
    }
    swarm->mirrored = true;
    m_stats.swarmBytes += payload.size();

    // It arrived, whatever of it is still news: said to the authority, in
    // the next few acknowledgements.
    if (std::find(m_swarmAcks.begin(), m_swarmAcks.end(), sequence) == m_swarmAcks.end()) {
        if (m_swarmAcks.size() >= SwarmAcksRepeated)
            m_swarmAcks.erase(m_swarmAcks.begin());
        m_swarmAcks.push_back(sequence);
    }
    m_swarmAckDue = true;
    const std::span<const u8> body = reader.bytes().subspan(reader.at(), size);
    const f64 dt = world.engineState().fixedTimestep;
    usize at = 0;
    core::i64 slot = -1;
    const auto byte = [&]() -> u32 { return at < body.size() ? body[at++] : (at = body.size() + 1, 0u); };
    while (at < body.size()) {
        u32 step = 0;
        for (u32 shift = 0; shift < 35; shift += 7) {
            const u32 part = byte();
            step |= (part & 0x7fu) << shift;
            if ((part & 0x80u) == 0)
                break;
        }
        slot += static_cast<core::i64>(step);
        const u32 low = byte();
        const u32 middle = byte();
        const u32 high = byte();
        const u32 packed = low | (middle << 8) | (high << 16);
        const u8 yaw = static_cast<u8>(byte());
        const u32 flags = byte();
        u32 lift = 0;
        if ((flags & 0x40u) != 0) {
            lift = byte();
            if (lift == 255) {
                lift = byte();
                const u32 more = byte();
                lift |= more << 8;
            }
        }
        std::optional<u16> tag;
        if ((flags & 0x80u) != 0) {
            const u32 tagLow = byte();
            const u32 tagHigh = byte();
            tag = static_cast<u16>(tagLow | (tagHigh << 8));
        }
        // Cut short: what was read of this agent is not an agent.
        if (at > body.size() || step == 0 || slot > 65535)
            break;
        scene::SwarmTold told;
        told.position = core::DVec3{originX - 256.0 + static_cast<f64>(packed >> 12) / 8.0, 0.0,
                                    originZ - 256.0 + static_cast<f64>(packed & 0xfffu) / 8.0};
        told.lift = static_cast<f32>(lift) / 8.0f;
        told.floor = floor;
        told.yaw = unpackYaw(yaw);
        told.walk = static_cast<f32>(flags & 0x3fu) / 4.0f;
        if (scene::mirrorSwarmAgentTold(*swarm, static_cast<u32>(slot), told, tick, dt, tag))
            m_stats.swarmStates += 1;
    }
}

void ReplicaSession::onUnreliable(scene::World& world, std::span<const u8> payload)
{
    Reader reader(payload);
    (void)reader.u8v();
    UnreliableOnWire wire;
    if (!readUnreliable(reader, wire) || !newerUnreliable(wire.sequence, m_unreliableIn, m_unreliableHeard)) {
        m_stats.unreliableDropped += 1;
        return;
    }
    m_unreliableIn = wire.sequence;
    m_unreliableHeard = true;
    const auto local = m_locals.find(wire.remote);
    if (local == m_locals.end() || !isClass(world, local->second, "UnreliableRemoteEvent")) {
        m_stats.unreliableDropped += 1;
        return;
    }
    scene::RemoteMessage message;
    message.remote = local->second;
    message.unreliable = true;
    if (wire.seat != 0) {
        if (m_guests[wire.seat - 1u].userId == 0 || !world.alive(m_guests[wire.seat - 1u].player)) {
            m_stats.unreliableDropped += 1;
            return;
        }
        message.player = m_guests[wire.seat - 1u].player;
    }
    message.payload.assign(wire.payload.begin(), wire.payload.end());
    // An instance this machine was never sent arrives as nil.
    for (const u32 ref : wire.refs) {
        const auto found = m_locals.find(ref);
        message.refs.push_back(found != m_locals.end() ? found->second : InstanceId{});
    }
    world.engineState().remoteInbox.push_back(std::move(message));
    m_stats.unreliableReceived += 1;
}

void ReplicaSession::sendMessages(scene::World& world)
{
    // Which messages of a swarm's positions arrived (ADR 0162) -- and said
    // again every so often with nothing new, since the one that said a
    // coming was taken in may itself be lost, and the authority waits on it.
    m_swarmAckQuiet += 1;
    if ((m_swarmAckDue || (!m_swarmMemberships.empty() && m_swarmAckQuiet >= 15)) && m_welcomed) {
        m_swarmAckDue = false;
        m_swarmAckQuiet = 0;
        Writer ack;
        ack.u8v(static_cast<u8>(MessageType::SwarmAck));
        ack.u8v(static_cast<u8>(m_swarmAcks.size()));
        for (const u16 sequence : m_swarmAcks)
            ack.u16v(sequence);
        // And the last message of comings and goings taken in, for each swarm.
        const usize swarms = std::min<usize>(m_swarmMemberships.size(), 255);
        ack.u8v(static_cast<u8>(swarms));
        for (usize at = 0; at < swarms; ++at) {
            ack.u32v(m_swarmMemberships[at].first);
            ack.u16v(m_swarmMemberships[at].second);
        }
        (void)sendBytes(m_transport, m_authority, ack.bytes, net::Delivery::Unreliable, SwarmChannel, m_stats);
    }
    std::vector<scene::RemoteMessage> outbox;
    outbox.swap(world.engineState().remoteOutbox);
    // This machine's instance to the network id the authority knows it by.
    const auto netIdOf = [this](InstanceId id) -> u32 {
        for (const auto& [netId, local] : m_locals) {
            if (local == id)
                return netId;
        }
        return 0;
    };
    for (scene::RemoteMessage& message : outbox) {
        if (!message.toServer)
            continue;
        if (message.unreliable) {
            // Sent once or not at all (ADR 0161): nothing waits for a welcome
            // or for an event the authority does not know.
            const u32 named = m_welcomed ? netIdOf(message.remote) : 0;
            if (named == 0) {
                m_stats.unreliableDropped += 1;
                continue;
            }
            std::vector<u32> refs;
            refs.reserve(message.refs.size());
            for (const InstanceId ref : message.refs)
                refs.push_back(netIdOf(ref));
            Writer out;
            m_unreliableOut = static_cast<u16>(m_unreliableOut + 1);
            writeUnreliable(out, MessageType::UnreliableToAuthority, m_unreliableOut, named, message, refs,
                            seatOfLocal(message.player));
            if (sendBytes(m_transport, m_authority, out.bytes, net::Delivery::UnreliableSequenced, RemoteChannel,
                          m_stats))
                m_stats.unreliableSent += 1;
            else
                m_stats.unreliableDropped += 1;
            continue;
        }
        // Not welcomed yet: nobody to send to, for a moment.
        if (!m_welcomed) {
            if (++message.held <= MaxRemoteHeldSends)
                world.engineState().remoteOutbox.push_back(std::move(message));
            else
                m_stats.messagesDropped += 1;
            continue;
        }
        // An event this replica made itself was never the authority's, and
        // the authority has no idea what it is.
        const u32 remote = netIdOf(message.remote);
        if (remote == 0) {
            m_stats.messagesDropped += 1;
            continue;
        }
        std::vector<u32> refs;
        refs.reserve(message.refs.size());
        for (const InstanceId ref : message.refs)
            refs.push_back(netIdOf(ref));
        Writer out;
        writeRemote(out, MessageType::RemoteToAuthority, remote, message, refs, seatOfLocal(message.player));
        sendBytes(m_transport, m_authority, out.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        m_stats.messagesSent += 1;
    }

    // Clicks (ADR 0126): the detector by the id the authority knows it by, and
    // which button.
    std::vector<scene::DetectorMessage> pressed;
    pressed.swap(world.engineState().detectorOutbox);
    for (scene::DetectorMessage& message : pressed) {
        const u32 detector = m_welcomed ? netIdOf(message.detector) : 0u;
        if (detector == 0) {
            if (++message.held <= MaxRemoteHeldSends)
                world.engineState().detectorOutbox.push_back(message);
            else
                m_stats.messagesDropped += 1;
            continue;
        }
        Writer out;
        out.u8v(static_cast<u8>(MessageType::DetectorInput));
        out.u32v(detector);
        out.u8v(static_cast<u8>(message.kind));
        sendBytes(m_transport, m_authority, out.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
        m_stats.messagesSent += 1;
    }
}

void ReplicaSession::interpolate(scene::World& world)
{
    // **Everyone else, drawn a few ticks in the past, between two snapshots**
    // -- Valve's entity interpolation. A snapshot every other tick applied as it
    // arrived stepped every remote part at thirty hertz; drawn `delay` ticks
    // behind the server's clock, there are nearly always two samples around the
    // moment being drawn, and the motion between them is a line.
    if (m_interpolationDelay == 0)
        return;
    const u64 drawn = m_serverClock > m_interpolationDelay ? m_serverClock - m_interpolationDelay : 0;
    for (auto& [netId, samples] : m_samples) {
        // Its own character is predicted, never drawn from the past.
        if (samples.empty() || ownsNet(netId) || m_ownedParts.contains(netId) || m_drivenWheels.contains(netId))
            continue;
        // A part that waits for its turn is drawn as far behind as it waits.
        const u64 behind = static_cast<u64>(std::lround(samples.back().lag));
        const u64 target = drawn > behind ? drawn - behind : 0;
        const auto local = m_locals.find(netId);
        scene::PartComponent* part =
            local != m_locals.end() && world.alive(local->second) ? world.parts().find(local->second) : nullptr;
        if (part == nullptr)
            continue;
        // **And where this machine's own character is to meet it** (ADR 0163),
        // once it is drawn: a character somebody else plays is drawn in the
        // past, and this machine's own is stepped ahead of the authority by as
        // many ticks as it has intents unanswered. The authority will have
        // applied the tick being predicted now that many ticks after the
        // newest snapshot -- so that is how far past its newest place the
        // other is expected, carried on by the way it was last going. Across
        // the ground only: a jump guessed forward is a head in the ceiling.
        const auto expect = [&]() {
            scene::CharacterBodyComponent* remote = world.characterBodies().find(local->second);
            if (remote == nullptr)
                return;
            const Sample& newest = samples.back();
            core::DVec3 expected = newest.cframe.position;
            const u64 now = world.engineState().tick;
            const u64 lead =
                m_ackedIntent != 0 && now > m_ackedIntent ? std::min(now - m_ackedIntent, MaxCollisionLeadTicks) : 0;
            if (samples.size() >= 2 && lead != 0) {
                const Sample& prior = samples[samples.size() - 2];
                if (newest.tick > prior.tick) {
                    const f64 ticks = static_cast<f64>(newest.tick - prior.tick);
                    const core::DVec3 went = newest.cframe.position - prior.cframe.position;
                    const f64 most = TeleportMetresPerTick * ticks;
                    if (went.x * went.x + went.y * went.y + went.z * went.z <= most * most) {
                        expected.x += went.x / ticks * static_cast<f64>(lead);
                        expected.z += went.z / ticks * static_cast<f64>(lead);
                    }
                }
            }
            const core::DVec3 apart = expected - part->cframe.position;
            remote->collisionLead =
                core::Vec3{static_cast<f32>(apart.x), static_cast<f32>(apart.y), static_cast<f32>(apart.z)};
            // And where it was last told to be, which the guess starts from.
            const core::DVec3 known = newest.cframe.position - part->cframe.position;
            remote->collisionKnown =
                core::Vec3{static_cast<f32>(known.x), static_cast<f32>(known.y), static_cast<f32>(known.z)};
            remote->collisionLeadSet = true;
        };
        // Past the newest sample, the newest: extrapolating a part the authority
        // has stopped talking about would put it where it is not.
        const Sample* before = &samples.front();
        const Sample* after = nullptr;
        for (const Sample& sample : samples) {
            if (sample.tick <= target)
                before = &sample;
            else if (after == nullptr)
                after = &sample;
        }
        if (after == nullptr || target <= before->tick || after->tick <= before->tick) {
            part->cframe = target < samples.front().tick ? samples.front().cframe : before->cframe;
            expect();
            continue;
        }
        // A teleport is a step at its tick, not a slide across the map.
        const core::DVec3 moved = after->cframe.position - before->cframe.position;
        const f64 reach = TeleportMetresPerTick * static_cast<f64>(after->tick - before->tick);
        if (moved.x * moved.x + moved.y * moved.y + moved.z * moved.z > reach * reach) {
            part->cframe = before->cframe;
            expect();
            continue;
        }
        const f64 alpha = static_cast<f64>(target - before->tick) / static_cast<f64>(after->tick - before->tick);
        part->cframe = core::lerp(before->cframe, after->cframe, alpha);
        expect();
    }

    // **Sprites the same way** (ADR 0103), by the same rules: past the newest
    // sample the newest, and nothing this machine owns.
    const u64 target = drawn;
    for (auto& [netId, samples] : m_samples2d) {
        if (samples.empty() || ownsNet(netId) || m_ownedParts.contains(netId))
            continue;
        const auto local = m_locals.find(netId);
        scene::Part2DComponent* sprite =
            local != m_locals.end() && world.alive(local->second) ? world.parts2d().find(local->second) : nullptr;
        if (sprite == nullptr)
            continue;
        const Sample2D* before = &samples.front();
        const Sample2D* after = nullptr;
        for (const Sample2D& sample : samples) {
            if (sample.tick <= target)
                before = &sample;
            else if (after == nullptr)
                after = &sample;
        }
        if (after == nullptr || target <= before->tick || after->tick <= before->tick) {
            const Sample2D& held = target < samples.front().tick ? samples.front() : *before;
            sprite->position = held.position;
            sprite->rotation = held.rotation;
            continue;
        }
        // A teleport is a step at its tick, not a slide.
        const core::Vec2 moved = after->position - before->position;
        const f64 reach = TeleportMetresPerTick * static_cast<f64>(after->tick - before->tick);
        const f64 movedX = static_cast<f64>(moved.x);
        const f64 movedY = static_cast<f64>(moved.y);
        if (movedX * movedX + movedY * movedY > reach * reach) {
            sprite->position = before->position;
            sprite->rotation = before->rotation;
            continue;
        }
        const core::f32 alpha = static_cast<core::f32>(static_cast<f64>(target - before->tick) /
                                                       static_cast<f64>(after->tick - before->tick));
        sprite->position = before->position + (after->position - before->position) * alpha;
        // **The short way round.** The solver answers in (-180, 180], so a
        // sprite turning through 180 goes from 179 to -179; a plain lerp would
        // spin it the long way, through zero.
        core::f32 turn = std::fmod(after->rotation - before->rotation, 360.0f);
        if (turn > 180.0f)
            turn -= 360.0f;
        else if (turn < -180.0f)
            turn += 360.0f;
        sprite->rotation = before->rotation + turn * alpha;
    }
}

void ReplicaSession::resolveCharacters(scene::World& world, InstanceId root)
{
    // Every player's `Character`, on this machine: the NetId the roster named,
    // through this machine's own copy of it -- which may arrive a message after
    // the roster that names it, so this runs after every receive.
    const InstanceId network = scene::networkServiceOf(world, world.parentOf(root));
    for (InstanceId child = network.valid() ? world.firstChild(network) : InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        scene::PlayerComponent* player = world.players().find(child);
        if (player == nullptr)
            continue;
        const auto named = m_characters.find(player->userId);
        const auto local = named != m_characters.end() ? m_locals.find(named->second) : m_locals.end();
        player->character = local != m_locals.end() && world.alive(local->second) ? local->second : InstanceId{};
        // **Every player of this machine is predicted** (ADR 0208): its
        // first, and each guest the authority has given a number -- by seat.
        // A guest was drawn as any other player's is, from what the
        // authority said of it, a round trip behind its own stick.
        //
        // **From the roster that names its character**: that is the tick a
        // guest's prediction begins at, with nothing kept from before it --
        // the first answer is taken, not compared (`Owned::synced`).
        std::optional<usize> seated;
        if (player->local && (player->userId == m_playerId || m_playerId == 0)) {
            seated = 0;
        }
        else if (player->local && player->userId != 0) {
            for (usize seat = 0; seat < m_guests.size(); ++seat) {
                if (m_guests[seat].userId == player->userId)
                    seated = seat + 1;
            }
        }
        if (seated.has_value()) {
            Owned& own = m_seats[*seated];
            const u32 owned = named != m_characters.end() && local != m_locals.end() ? named->second : 0u;
            if (owned != own.netId) {
                own.synced = false;
                own.predicted.clear();
                own.predicted2d.clear();
                own.snapshotAttributes.reset();
                own.visualOffset = core::DVec3{};
                own.visualFresh = false;
                own.displaced = core::DVec3{};
            }
            own.netId = owned;
            // What was buffered for it before this machine knew it was its
            // own: the newest of it is where it starts being predicted from.
            if (const auto buffered = own.netId != 0 ? m_samples.find(own.netId) : m_samples.end();
                buffered != m_samples.end()) {
                if (scene::PartComponent* part = world.parts().find(local->second);
                    part != nullptr && !buffered->second.empty())
                    part->cframe = buffered->second.back().cframe;
                m_samples.erase(buffered);
            }
            // A character on the plane, the same.
            if (const auto buffered = own.netId != 0 ? m_samples2d.find(own.netId) : m_samples2d.end();
                buffered != m_samples2d.end()) {
                if (scene::Part2DComponent* sprite = world.parts2d().find(local->second);
                    sprite != nullptr && !buffered->second.empty()) {
                    sprite->position = buffered->second.back().position;
                    sprite->rotation = buffered->second.back().rotation;
                }
                m_samples2d.erase(buffered);
            }
        }
    }
    // **A seat nobody sits in predicts nobody** (ADR 0208): a guest that
    // left takes its history with it -- the island remembered for the ticks
    // before still holds its character's body, and is not asked for it,
    // because no answer names a character with no seat.
    for (usize seat = 1; seat < m_seats.size(); ++seat) {
        if (m_guests[seat - 1].userId == 0 && m_seats[seat].netId != 0)
            m_seats[seat] = Owned{};
    }
    // Its own parts, which may have spawned after the list that names them.
    for (const u32 netId : m_ownedParts) {
        const auto local = m_locals.find(netId);
        scene::RigidBodyComponent* body =
            local != m_locals.end() && world.alive(local->second) ? world.rigidBodies().find(local->second) : nullptr;
        if (body != nullptr)
            body->networkOwner = m_playerId;
    }
}

InstanceId ReplicaSession::attributeOwner(scene::World& world, InstanceId root, u8 owner, u32 id) const
{
    InstanceId target;
    const InstanceId dataModel = world.parentOf(root);
    switch (owner) {
    case 0:
        target = localOf(NetId{id});
        // **The workspace and the services are here from boot**, and
        // what they hold arrives with the welcome -- before the first state
        // that would have told this machine which of its instances an id
        // names. Found by what they are.
        if (!target.valid() && id == RootNetId.value) {
            target = root;
        }
        else if (!target.valid() && id >= ServiceNetIdBase && id - ServiceNetIdBase < std::size(generated::Classes) &&
                 generated::Classes[id - ServiceNetIdBase].service) {
            const std::string_view wanted = generated::Classes[id - ServiceNetIdBase].name;
            for (InstanceId child = dataModel.valid() ? world.firstChild(dataModel) : InstanceId{}; child.valid();
                 child = world.nextSibling(child)) {
                if (world.atoms().text(world.classes().find(world.classOf(child))->name) == wanted) {
                    target = child;
                    break;
                }
            }
        }
        break;
    case 1:
        target = scene::playerByUserId(world, id);
        break;
    case 2:
        target = globalScriptsOf(world, dataModel);
        break;
    case 3:
        if (const InstanceId global = globalScriptsOf(world, dataModel); global.valid())
            target = world.findFirstChild(global, world.atoms().lookup("Shared"));
        break;
    default:
        break;
    }
    return target;
}

void ReplicaSession::onAttributeEdits(scene::World& world, InstanceId root, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u8 owner = reader.u8v();
    const u32 id = reader.u32v();
    // **Read whole before any of it is applied -- and every name it tells is
    // kept whatever becomes of the rest**: the authority counts a name told
    // once it is sent, and the next message names it by number.
    const auto name = [&]() -> std::string {
        const u16 ref = reader.u16v();
        if (ref == LiteralName)
            return std::string(reader.text());
        if ((ref & DefineName) != 0) {
            std::string text(reader.text());
            // Told in order from zero: any other number is not this protocol.
            if (!reader.ok() || static_cast<usize>(ref & ~DefineName) != m_attributeNames.size()) {
                reader.fail();
                return {};
            }
            m_attributeNames.push_back(text);
            return text;
        }
        if (ref >= m_attributeNames.size()) {
            reader.fail();
            return {};
        }
        return m_attributeNames[ref];
    };
    const auto localOfNet = [this](u32 net) { return localOf(NetId{net}); };
    std::vector<std::pair<std::string, std::optional<scene::Value>>> values;
    const u16 count = reader.u16v();
    for (u16 at = 0; at < count && reader.ok(); ++at) {
        std::string text = name();
        if (reader.u8v() == 0) {
            values.emplace_back(std::move(text), std::nullopt);
            continue;
        }
        std::optional<scene::Value> value = readAttributeValue(reader, localOfNet);
        if (!value.has_value()) {
            reader.fail();
            break;
        }
        values.emplace_back(std::move(text), std::move(value));
    }
    std::array<std::vector<std::string>, 2> tags;
    for (std::vector<std::string>& list : tags) {
        const u16 tagCount = reader.u16v();
        if (tagCount > MaxReplicaTags)
            reader.fail();
        for (u16 at = 0; at < tagCount && reader.ok(); ++at)
            list.push_back(name());
    }
    if (!reader.ok() || !reader.done())
        return;

    const InstanceId target = attributeOwner(world, root, owner, id);
    if (!target.valid() || !world.alive(target))
        return;
    // What this replica predicts of its own character is the snapshot's to
    // say, as in `onAttributes`.
    std::set<std::string_view> predicted;
    if (const u32 targetNet = owner == 0 ? id : 0u; ownsNet(targetNet) && localOf(NetId{targetNet}) == target) {
        if (const scene::CharacterBodyComponent* body = world.characterBodies().find(target); body != nullptr) {
            for (const core::NameAtom predictedName : body->predictedAttributes)
                predicted.insert(world.atoms().text(predictedName));
        }
    }
    for (const auto& [text, value] : values) {
        if (!predicted.contains(text))
            (void)world.setAttribute(target, world.atoms().intern(text), value.has_value() ? *value : scene::Value{});
    }
    for (const std::string& tag : tags[0])
        (void)world.addTag(target, world.atoms().intern(tag));
    for (const std::string& tag : tags[1])
        (void)world.removeTag(target, world.atoms().intern(tag));
}

void ReplicaSession::onAttributes(scene::World& world, InstanceId root, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u8 owner = reader.u8v();
    const u32 id = reader.u32v();
    const u16 count = reader.u16v();
    const auto localOfNet = [this](u32 net) { return localOf(NetId{net}); };
    std::vector<std::pair<std::string, scene::Value>> incoming;
    for (u16 at = 0; at < count && reader.ok(); ++at) {
        std::string name(reader.text());
        std::optional<scene::Value> value = readAttributeValue(reader, localOfNet);
        if (!value.has_value())
            return;
        incoming.emplace_back(std::move(name), std::move(*value));
    }
    // Its tags, exactly these as its attributes are.
    const u16 tagCount = reader.u16v();
    if (!reader.ok() || tagCount > MaxReplicaTags)
        return;
    std::vector<std::string> tags;
    tags.reserve(tagCount);
    for (u16 at = 0; at < tagCount && reader.ok(); ++at)
        tags.emplace_back(reader.text());
    if (!reader.ok() || !reader.done())
        return;

    const InstanceId target = attributeOwner(world, root, owner, id);
    if (!target.valid() || !world.alive(target))
        return;

    // **What this replica predicts is not taken from here**: its own
    // character's predicted attributes come with the snapshot that answers
    // its prediction, tick by tick; this copy, sent when they change, is
    // older than the prediction and would undo it.
    std::set<std::string_view> predicted;
    if (const u32 targetNet = owner == 0 ? id : 0u; ownsNet(targetNet) && localOf(NetId{targetNet}) == target) {
        if (const scene::CharacterBodyComponent* body = world.characterBodies().find(target); body != nullptr) {
            for (const core::NameAtom name : body->predictedAttributes)
                predicted.insert(world.atoms().text(name));
        }
    }
    // **Exactly these**: what the authority has now, and nothing it removed.
    // Looked up in a set: a scan of the message for each attribute held was
    // sixty-five thousand squared for one message.
    std::set<std::string_view> kept;
    for (const auto& entry : incoming)
        kept.insert(entry.first);
    scene::AttributeMap current;
    world.collectAttributes(target, current);
    for (const auto& [name, value] : current) {
        if (!kept.contains(world.atoms().text(name)) && !predicted.contains(world.atoms().text(name)))
            (void)world.setAttribute(target, name, scene::Value{});
    }
    for (const auto& [name, value] : incoming) {
        if (!predicted.contains(name))
            (void)world.setAttribute(target, world.atoms().intern(name), value);
    }

    // **Exactly these tags**, too: what the authority took off is gone here.
    scene::TagSet had;
    world.collectTags(target, had);
    std::set<std::string_view> wanted(tags.begin(), tags.end());
    for (const core::NameAtom tag : had) {
        if (!wanted.contains(world.atoms().text(tag)))
            (void)world.removeTag(target, tag);
    }
    for (const std::string& tag : tags)
        (void)world.addTag(target, world.atoms().intern(tag));
}

void ReplicaSession::onSceneChange(scene::World& world, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    std::string path(reader.text());
    const u32 length = reader.u32v();
    // **The bytes the message has, never the count it claims**: reserved from
    // the claim, seven bytes asked for four gigabytes and the allocation that
    // failed took the process with it.
    if (!reader.ok() || reader.remaining() != static_cast<usize>(length) + 4u)
        return;
    const std::span<const u8> rest = reader.bytes().subspan(reader.at(), length);
    std::vector<u8> data(rest.begin(), rest.end());
    Reader tail(reader.bytes().subspan(reader.at() + length));
    const u32 load = tail.u32v();
    // **Already there** is what a replica that joined into the scene it booted
    // hears, and it is not a reason to load it again -- the first change it
    // hears only. After that a load the authority made is one this replica
    // makes, the same scene again included: it was taken for the join's,
    // and the last match's client code ran on over the next.
    const bool again = m_sceneHeard && load != m_sceneLoad;
    m_sceneHeard = true;
    m_sceneLoad = load;
    if (path.empty() || !m_sceneChanger || (path == world.engineState().currentScene && !again))
        return;
    m_sceneChanger(world, path, std::move(data));
}

void ReplicaSession::onTilemapBlocks(scene::World& world, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u32 netId = reader.u32v();
    const u16 count = reader.u16v();
    // Read whole before any is applied: a message cut short changes nothing.
    std::vector<std::pair<scene::TileChunkKey, scene::TileChunk>> blocks;
    constexpr usize BlockBytes = 4 + 4 + 2 + TileBlockCells * 2;
    if (!reader.ok() || reader.remaining() != static_cast<usize>(count) * BlockBytes)
        return;
    blocks.reserve(count);
    for (u16 at = 0; at < count && reader.ok(); ++at) {
        scene::TileChunkKey key{static_cast<core::i32>(reader.u32v()), static_cast<core::i32>(reader.u32v())};
        if (reader.u16v() != TileBlockCells)
            return;
        scene::TileChunk cells{};
        for (u16& tile : cells)
            tile = reader.u16v();
        blocks.emplace_back(key, cells);
    }
    if (!reader.ok() || !reader.done())
        return;
    const auto local = m_locals.find(netId);
    scene::Tilemap2DComponent* tilemap =
        local != m_locals.end() && world.alive(local->second) ? world.tilemaps2d().find(local->second) : nullptr;
    if (tilemap == nullptr)
        return;
    for (const auto& [key, cells] : blocks) {
        const bool empty = std::all_of(cells.begin(), cells.end(), [](u16 tile) { return tile == 0; });
        if (empty)
            tilemap->chunks.erase(key);
        else
            tilemap->chunks[key] = cells;
    }
    // What rebuilds from the cells -- the colliders and the drawing -- watches
    // this, as it does for an edit made here.
    tilemap->revision += 1;
}

// --- The ground, received (ADR 0135) ----------------------------------------------
//
// Each message is read whole before any of it is applied, so one cut short or
// malformed changes nothing -- and it is the authority's ground either way, so
// a chunk received twice is simply the truth.

namespace {

struct ChunkKeyOnWire
{
    core::i32 x = 0;
    core::i32 y = 0;
    core::i32 z = 0;
};

// What a replica's `shipped` holds for a chunk the authority removed before
// this machine loaded it: any chunk at all, since only its presence is read.
[[nodiscard]] std::shared_ptr<asset::TerrainChunk> knownGone()
{
    static const auto marker = std::make_shared<asset::TerrainChunk>(asset::Voxel{asset::FullOccupancy, 1});
    return marker;
}

[[nodiscard]] const std::vector<asset::BlockId>& knownGoneBlocks()
{
    static const std::vector<asset::BlockId> marker(asset::VoxelChunkVolume, asset::BlockId{1});
    return marker;
}

[[nodiscard]] ChunkKeyOnWire readKey(Reader& in) noexcept
{
    ChunkKeyOnWire key;
    key.x = static_cast<core::i32>(in.u32v());
    key.y = static_cast<core::i32>(in.u32v());
    key.z = static_cast<core::i32>(in.u32v());
    return key;
}

// `length` bytes of the message, or an empty span and a failed reader.
[[nodiscard]] std::span<const u8> readCode(Reader& in, usize length) noexcept
{
    if (!in.ok() || in.remaining() < length) {
        in.fail();
        return {};
    }
    const std::span<const u8> code = in.bytes().subspan(in.at(), length);
    in.at() += length;
    return code;
}

} // namespace

namespace {

// **The workspace's terrain, made when the authority has one and this machine
// none** -- a server that made its terrain in a script. Made with `settings`
// when they are known, and the field's defaults when the look came first.
[[nodiscard]] scene::TerrainComponent* groundTerrainFor(scene::World& world, InstanceId root,
                                                        std::optional<asset::FieldSettings> settings)
{
    InstanceId id = groundTerrainUnder(world, root);
    if (!id.valid()) {
        const scene::ClassId terrainClass = world.classes().findId(world.atoms().intern("Terrain"));
        if (terrainClass == scene::InvalidClass)
            return nullptr;
        id = world.create(terrainClass);
        if (!id.valid())
            return nullptr;
        world.setName(id, world.atoms().intern("Terrain"));
        if (world.setParent(id, root).has_value()) {
            world.destroy(id);
            return nullptr;
        }
        if (scene::TerrainComponent* made = world.terrains().find(id); made != nullptr && settings.has_value()) {
            made->field = asset::TerrainField(*settings);
            made->shipped = asset::TerrainField(*settings);
            made->minHeight = settings->minHeight;
            made->maxHeight = settings->maxHeight;
        }
    }
    return world.terrains().find(id);
}

} // namespace

void ReplicaSession::onTerrainChunks(scene::World& world, InstanceId root, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    asset::FieldSettings settings;
    settings.voxelSize = readF32(reader);
    settings.minHeight = readF32(reader);
    settings.maxHeight = readF32(reader);
    const u16 count = reader.u16v();
    if (!reader.ok() || count > GroundChunksPerMessage || !asset::saneFieldSettings(settings)) {
        m_stats.messagesDropped += 1;
        return;
    }
    std::vector<asset::TerrainField::Entry> chunks;
    chunks.reserve(count);
    for (u16 at = 0; at < count && reader.ok(); ++at) {
        const ChunkKeyOnWire wire = readKey(reader);
        const asset::ChunkKey key{wire.x, wire.y, wire.z};
        const u32 length = reader.u32v();
        if (!asset::chunkKeyInRange(key) || length > asset::MaxTerrainChunkCode) {
            reader.fail();
            break;
        }
        const std::span<const u8> code = readCode(reader, length);
        std::shared_ptr<asset::TerrainChunk> chunk;
        if (length > 0 &&
            !asset::decodeTerrainChunk(
                std::span<const std::byte>{reinterpret_cast<const std::byte*>(code.data()), code.size()}, chunk)) {
            reader.fail();
            break;
        }
        chunks.emplace_back(key, std::move(chunk));
    }
    if (!reader.ok() || !reader.done()) {
        m_stats.messagesDropped += 1;
        return;
    }

    scene::TerrainComponent* terrain = groundTerrainFor(world, root, settings);
    if (terrain == nullptr)
        return;
    // **Another voxel is another ground** -- unless this one has none yet, and
    // then it is the authority's: a server script that set
    // `VoxelSize` on the scene's empty terrain and generated sent every
    // replica chunks it refused.
    if (std::abs(terrain->field.settings().voxelSize - settings.voxelSize) > 1e-6f ||
        terrain->field.settings().minHeight != settings.minHeight ||
        terrain->field.settings().maxHeight != settings.maxHeight) {
        if (!terrain->field.empty() && std::abs(terrain->field.settings().voxelSize - settings.voxelSize) > 1e-6f) {
            m_stats.messagesDropped += 1;
            return;
        }
        if (terrain->field.empty()) {
            terrain->field = asset::TerrainField(settings);
            terrain->shipped = asset::TerrainField(settings);
        }
        else {
            terrain->field.setHeightRange(settings.minHeight, settings.maxHeight);
        }
        terrain->minHeight = settings.minHeight;
        terrain->maxHeight = settings.maxHeight;
    }
    for (auto& [key, chunk] : chunks) {
        // **The authority's word on a chunk this machine has not loaded** is
        // kept as known, so the cell loading here later leaves it standing
        // rather than merging the package's chunk over it -- a removal most of
        // all, which the field alone cannot hold. A
        // replica's `shipped` is read by nothing but that load.
        if (terrain->shipped.findChunk(key) == nullptr)
            terrain->shipped.setChunk(key, chunk != nullptr ? chunk : knownGone());
        if (chunk != nullptr)
            terrain->field.setChunk(key, std::move(chunk));
        else
            terrain->field.removeChunk(key);
    }
    terrain->fieldRevision += 1;
}

void ReplicaSession::onTerrainLook(scene::World& world, InstanceId root, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    core::DVec3 origin;
    origin.x = readF64(reader);
    origin.y = readF64(reader);
    origin.z = readF64(reader);
    if (!reader.ok() || !core::isFinite(origin.x) || !core::isFinite(origin.y) || !core::isFinite(origin.z)) {
        m_stats.messagesDropped += 1;
        return;
    }
    const u16 layerCount = reader.u16v();
    if (!reader.ok() || layerCount > asset::MaxTerrainLayers) {
        m_stats.messagesDropped += 1;
        return;
    }
    std::vector<std::string> layers;
    layers.reserve(layerCount);
    for (u16 at = 0; at < layerCount && reader.ok(); ++at)
        layers.emplace_back(reader.text());
    const u16 ruleCount = reader.u16v();
    if (!reader.ok() || ruleCount > asset::MaxTerrainRules) {
        m_stats.messagesDropped += 1;
        return;
    }
    std::vector<asset::TerrainRule> rules;
    rules.reserve(ruleCount);
    for (u16 at = 0; at < ruleCount && reader.ok(); ++at) {
        asset::TerrainRule rule;
        rule.enabled = reader.u8v() != 0;
        rule.material = reader.u8v();
        rule.slopeMin = readF32(reader);
        rule.slopeMax = readF32(reader);
        rule.heightMin = readF32(reader);
        rule.heightMax = readF32(reader);
        rule.blend = readF32(reader);
        rule.noise = readF32(reader);
        const u16 applies = reader.u16v();
        if (!reader.ok() || applies > 255 || rule.material == 0 || !core::isFinite(rule.slopeMin) ||
            !core::isFinite(rule.slopeMax) || !core::isFinite(rule.heightMin) || !core::isFinite(rule.heightMax) ||
            !core::isFinite(rule.blend) || !core::isFinite(rule.noise)) {
            reader.fail();
            break;
        }
        for (u16 index = 0; index < applies; ++index)
            rule.appliesTo.push_back(reader.u8v());
        rules.push_back(std::move(rule));
    }
    if (!reader.ok() || !reader.done()) {
        m_stats.messagesDropped += 1;
        return;
    }
    // **Made here when the look comes first**: a server
    // script that made its terrain and set its layers before any ground sent
    // the look to a replica with nothing to put it on, and never again.
    scene::TerrainComponent* terrain = groundTerrainFor(world, root, std::nullopt);
    if (terrain == nullptr)
        return;
    if (terrain->origin.x != origin.x || terrain->origin.y != origin.y || terrain->origin.z != origin.z) {
        terrain->origin = origin;
        terrain->fieldRevision += 1;
    }
    if (terrain->layers != layers) {
        terrain->layers = std::move(layers);
        terrain->layersRevision += 1;
    }
    terrain->rules = std::move(rules);
}

void ReplicaSession::onVoxelChunks(scene::World& world, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const f32 blockSize = readF32(reader);
    const u16 count = reader.u16v();
    if (!reader.ok() || count > GroundChunksPerMessage || !core::isFinite(blockSize) || !(blockSize >= 0.01f) ||
        blockSize > 64.0f) {
        m_stats.messagesDropped += 1;
        return;
    }
    std::vector<std::pair<asset::VoxelChunkKey, std::vector<asset::BlockId>>> chunks;
    chunks.reserve(count);
    for (u16 at = 0; at < count && reader.ok(); ++at) {
        const ChunkKeyOnWire wire = readKey(reader);
        const asset::VoxelChunkKey key{wire.x, wire.y, wire.z};
        const u32 length = reader.u32v();
        if (!asset::voxelChunkKeyInRange(key) || length > MaxVoxelChunkCode) {
            reader.fail();
            break;
        }
        const std::span<const u8> code = readCode(reader, length);
        std::vector<asset::BlockId> blocks;
        if (length > 0 && !asset::decodeVoxelChunk(code, blocks)) {
            reader.fail();
            break;
        }
        chunks.emplace_back(key, std::move(blocks));
    }
    if (!reader.ok() || !reader.done()) {
        m_stats.messagesDropped += 1;
        return;
    }
    const InstanceId id = groundVoxelsOf(world);
    scene::VoxelComponent* voxels = id.valid() ? world.voxels().find(id) : nullptr;
    if (voxels == nullptr)
        return;
    // A world with no blocks yet takes the authority's size; one with blocks
    // of another size is another world.
    if (std::abs(voxels->blockSize - blockSize) > 1e-6f) {
        if (voxels->grid.chunkCount() != 0) {
            m_stats.messagesDropped += 1;
            return;
        }
        voxels->blockSize = blockSize;
    }
    for (const auto& [key, blocks] : chunks) {
        // `onTerrainChunks`' reason: a chunk not loaded here stays the
        // authority's when its cell loads.
        if (voxels->shipped.findChunk(key) == nullptr)
            voxels->shipped.setChunk(key, blocks.empty() ? std::span<const asset::BlockId>(knownGoneBlocks())
                                                         : std::span<const asset::BlockId>(blocks));
        if (blocks.empty())
            voxels->grid.removeChunk(key);
        else
            voxels->grid.setChunk(key, blocks);
    }
    voxels->revision += 1;
}

constexpr u16 MaxFluidReactionsOnWire = 4096;

void ReplicaSession::onCollisionGroups(scene::World& world, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u16 count = reader.u16v();
    if (count == 0 || count > scene::CollisionGroups::kMaxGroups) {
        m_stats.messagesDropped += 1;
        return;
    }
    // Read whole before anything is registered: a message cut short must not
    // leave half a table, nor grow the atom table.
    std::vector<std::string_view> names;
    names.reserve(count);
    for (u16 at = 0; at < count && reader.ok(); ++at)
        names.push_back(reader.text());
    const u32 apartCount = reader.u32v();
    if (!reader.ok() || apartCount > static_cast<u32>(count) * count) {
        m_stats.messagesDropped += 1;
        return;
    }
    std::vector<std::pair<u16, u16>> apart;
    apart.reserve(apartCount);
    for (u32 at = 0; at < apartCount && reader.ok(); ++at) {
        const u16 a = reader.u16v();
        const u16 b = reader.u16v();
        if (a >= count || b >= count)
            reader.fail();
        apart.emplace_back(a, b);
    }
    if (!reader.ok()) {
        m_stats.messagesDropped += 1;
        return;
    }
    for (const std::string_view name : names) {
        if (name.empty() || name.size() > 256) {
            m_stats.messagesDropped += 1;
            return;
        }
    }

    // **By name**: this machine numbers its groups as it met them, and its own
    // scripts may have registered some first. Every pair among the groups the
    // authority named is the authority's -- collidable unless it says apart --
    // and a group only this machine has is left as this machine set it.
    scene::CollisionGroups& groups = world.collisionGroups();
    std::vector<u16> local;
    local.reserve(count);
    for (const std::string_view name : names)
        local.push_back(groups.add(world.atoms().intern(name)));
    for (u16 a = 0; a < count; ++a) {
        for (u16 b = a; b < count; ++b) {
            if (local[a] != scene::CollisionGroups::kInvalid && local[b] != scene::CollisionGroups::kInvalid)
                groups.setCollidable(local[a], local[b], true);
        }
    }
    for (const auto& [a, b] : apart) {
        if (local[a] != scene::CollisionGroups::kInvalid && local[b] != scene::CollisionGroups::kInvalid)
            groups.setCollidable(local[a], local[b], false);
    }
    groups.bumpRevision();
}

void ReplicaSession::onVoxelTypes(scene::World& world, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u16 count = reader.u16v();
    std::vector<scene::VoxelBlockType> types;
    types.reserve(count);
    const auto atom = [&world](std::string_view text) {
        return text.empty() ? core::NameAtom{} : world.atoms().intern(text);
    };
    // Read first, interned only once the whole message is known good: a
    // hostile message must not grow the atom table.
    struct Read
    {
        std::string_view name, texture, sideTexture, bottomTexture;
        scene::VoxelBlockType type;
    };
    std::vector<Read> read;
    read.reserve(count);
    for (u16 at = 0; at < count && reader.ok(); ++at) {
        Read entry;
        entry.name = reader.text();
        for (core::Color3* color : {&entry.type.color, &entry.type.side, &entry.type.bottom}) {
            color->r = readF32(reader);
            color->g = readF32(reader);
            color->b = readF32(reader);
        }
        entry.texture = reader.text();
        entry.sideTexture = reader.text();
        entry.bottomTexture = reader.text();
        entry.type.opacity = std::min<core::i32>(reader.u8v(), 2);
        entry.type.transparency = readF32(reader);
        entry.type.fluidReach = std::min<core::u8>(reader.u8v(), static_cast<core::u8>(asset::MaxFluidReach));
        entry.type.fluidTicks = std::clamp<u32>(reader.u32v(), 1, 1'000'000);
        // Colours and a transparency a renderer can use.
        bool finite = core::isFinite(entry.type.transparency);
        for (const core::Color3* color : {&entry.type.color, &entry.type.side, &entry.type.bottom})
            finite = finite && core::isFinite(color->r) && core::isFinite(color->g) && core::isFinite(color->b);
        if (!finite) {
            reader.fail();
            break;
        }
        read.push_back(entry);
    }
    const u16 reactionCount = reader.u16v();
    // Each is an insertion into a sorted list: tens of thousands from a
    // hostile server were a quadratic stall. No world has
    // a fraction of this many.
    if (reactionCount > MaxFluidReactionsOnWire)
        reader.fail();
    std::vector<scene::VoxelComponent::FluidReaction> reactions;
    for (u16 at = 0; at < reactionCount && reader.ok(); ++at) {
        scene::VoxelComponent::FluidReaction reaction;
        reaction.from = reader.u16v();
        reaction.touching = reader.u16v();
        reaction.result = reader.u16v();
        reactions.push_back(reaction);
    }
    if (!reader.ok() || !reader.done()) {
        m_stats.messagesDropped += 1;
        return;
    }
    const InstanceId id = groundVoxelsOf(world);
    scene::VoxelComponent* voxels = id.valid() ? world.voxels().find(id) : nullptr;
    if (voxels == nullptr)
        return;
    for (Read& entry : read) {
        entry.type.name = atom(entry.name);
        entry.type.texture = atom(entry.texture);
        entry.type.sideTexture = atom(entry.sideTexture);
        entry.type.bottomTexture = atom(entry.bottomTexture);
        types.push_back(entry.type);
    }
    voxels->types = std::move(types);
    voxels->fluidReactions.clear();
    for (const scene::VoxelComponent::FluidReaction& reaction : reactions)
        scene::setFluidReaction(*voxels, reaction.from, reaction.touching, reaction.result);
    voxels->revision += 1;
}

void ReplicaSession::onOwnership(scene::World& world, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u32 count = reader.u32v();
    std::set<u32> owned;
    for (u32 at = 0; at < count && reader.ok(); ++at)
        owned.insert(reader.u32v());
    if (!reader.ok() || !reader.done())
        return;
    // Handed back: a follower of the snapshots again, from the next one.
    for (const u32 netId : m_ownedParts) {
        if (owned.contains(netId))
            continue;
        const auto local = m_locals.find(netId);
        if (local == m_locals.end() || !world.alive(local->second))
            continue;
        if (scene::RigidBodyComponent* body = world.rigidBodies().find(local->second); body != nullptr)
            body->networkOwner = 0;
        // **Back to where the authority has it**, from the newest state this
        // replica holds: the snapshots only send what changed, and the
        // authority's copy may never change again.
        const EntityState* held = m_states.empty() ? nullptr : findEntity(*m_states.back(), netId);
        scene::PartComponent* part = world.parts().find(local->second);
        if (held == nullptr || part == nullptr)
            continue;
        const generated::ClassDesc& desc = generated::Classes[held->schema];
        for (usize at = 0; at < held->fields.size(); ++at) {
            const generated::FieldDesc* field = fieldAt(desc, at);
            if (field != nullptr && field->name == "CFrame" && field->pool == "parts")
                part->cframe = asCFrame(held->fields[at]);
        }
    }
    // **Taken from where the authority has it now**, from the newest state
    // this replica holds -- not from where it was last drawn, which is the
    // interpolation delay behind and, for a part handed over in the snapshot
    // that made it, nowhere at all: it was simulated from the origin, and the
    // authority refused every place it was sent as out of reach.
    for (const u32 netId : owned) {
        if (m_ownedParts.contains(netId))
            continue;
        const auto local = m_locals.find(netId);
        const EntityState* held = m_states.empty() ? nullptr : findEntity(*m_states.back(), netId);
        if (local == m_locals.end() || !world.alive(local->second) || held == nullptr)
            continue;
        scene::PartComponent* part = world.parts().find(local->second);
        scene::RigidBodyComponent* body = world.rigidBodies().find(local->second);
        const generated::ClassDesc& desc = generated::Classes[held->schema];
        for (usize at = 0; at < held->fields.size(); ++at) {
            const generated::FieldDesc* field = fieldAt(desc, at);
            if (field == nullptr)
                continue;
            if (part != nullptr && field->name == "CFrame" && field->pool == "parts")
                part->cframe = asCFrame(held->fields[at]);
            else if (body != nullptr && field->name == "LinearVelocity" && field->pool == "rigidBodies")
                body->linearVelocity = asVec3(held->fields[at]);
            else if (body != nullptr && field->name == "AngularVelocity" && field->pool == "rigidBodies")
                body->angularVelocity = asVec3(held->fields[at]);
        }
    }
    for (const u32 netId : owned) {
        m_samples.erase(netId);
        m_samples2d.erase(netId);
    }
    m_ownedParts = std::move(owned);
}

void ReplicaSession::sendOwned(const scene::World& world, u64 tick)
{
    if (m_ownedParts.empty())
        return;
    Writer state;
    state.u8v(static_cast<u8>(MessageType::OwnedState));
    state.u64v(tick);
    std::vector<std::pair<u32, InstanceId>> alive;
    for (const u32 netId : m_ownedParts) {
        const auto local = m_locals.find(netId);
        if (local != m_locals.end() && world.alive(local->second) && world.parts().find(local->second) != nullptr &&
            world.rigidBodies().find(local->second) != nullptr)
            alive.emplace_back(netId, local->second);
    }
    state.u16v(static_cast<u16>(std::min<usize>(alive.size(), 0xFFFF)));
    for (usize at = 0; at < alive.size() && at < 0xFFFF; ++at) {
        const scene::PartComponent& part = *world.parts().find(alive[at].second);
        const scene::RigidBodyComponent& body = *world.rigidBodies().find(alive[at].second);
        state.u32v(alive[at].first);
        FieldValue value;
        setCFrame(value, part.cframe);
        encodeField(state.bytes, generated::Encoding::CFrameD, value);
        setVec3(value, body.linearVelocity);
        encodeField(state.bytes, generated::Encoding::Vector3, value);
        setVec3(value, body.angularVelocity);
        encodeField(state.bytes, generated::Encoding::Vector3, value);
    }
    sendBytes(m_transport, m_authority, state.bytes, net::Delivery::UnreliableSequenced, OwnershipChannel, m_stats);
}

u8 ReplicaSession::seatOfLocal(InstanceId player) const noexcept
{
    for (u8 seat = 0; seat < m_guests.size(); ++seat) {
        if (player.valid() && m_guests[seat].player == player && m_guests[seat].userId != 0)
            return static_cast<u8>(seat + 1);
    }
    return 0;
}

ReplicaSession::Owned* ReplicaSession::seatOfNet(u32 netId) noexcept
{
    if (netId == 0)
        return nullptr;
    for (Owned& seat : m_seats) {
        if (seat.netId == netId)
            return &seat;
    }
    return nullptr;
}

void ReplicaSession::syncGuests(const scene::World& world)
{
    std::vector<InstanceId> seated;
    scene::localPlayers(world, seated);
    const auto say = [this](MessageType type, u8 seat) {
        Writer message;
        message.u8v(static_cast<u8>(type));
        message.u8v(seat);
        sendBytes(m_transport, m_authority, message.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
    };
    // Gone: the seat is given back.
    for (u8 seat = 0; seat < m_guests.size(); ++seat) {
        GuestSeat& guest = m_guests[seat];
        if (!guest.player.valid())
            continue;
        // The first of the machine is no guest, whoever it was before.
        const auto here = std::find(seated.begin(), seated.end(), guest.player);
        if (here != seated.end() && here != seated.begin())
            continue;
        if (guest.asked && m_welcomed)
            say(MessageType::GuestLeave, static_cast<u8>(seat + 1));
        guest = GuestSeat{};
    }
    // New: the first free seat, in the order they joined.
    for (usize at = 1; at < seated.size(); ++at) {
        const bool sitting = std::any_of(m_guests.begin(), m_guests.end(),
                                         [&](const GuestSeat& guest) { return guest.player == seated[at]; });
        if (sitting)
            continue;
        const auto free = std::find_if(m_guests.begin(), m_guests.end(),
                                       [](const GuestSeat& guest) { return !guest.player.valid(); });
        // More than a machine seats: a fifth player has nobody to be here.
        if (free == m_guests.end())
            break;
        free->player = seated[at];
    }
    // And the authority asked, on this connection, for each that has not been.
    if (!m_welcomed)
        return;
    for (u8 seat = 0; seat < m_guests.size(); ++seat) {
        GuestSeat& guest = m_guests[seat];
        if (guest.player.valid() && !guest.asked) {
            guest.asked = true;
            say(MessageType::GuestJoin, static_cast<u8>(seat + 1));
        }
    }
}

void ReplicaSession::sendIntent(const scene::World& world, u64 tick)
{
    if (!m_welcomed)
        return;
    // What this replica predicted for each of its own characters at this
    // tick, for the snapshot that answers this intent to be compared against.
    bool stepped = false;
    for (Owned& seat : m_seats) {
        if (seat.netId == 0)
            continue;
        const auto local = m_locals.find(seat.netId);
        const scene::PartComponent* part =
            local != m_locals.end() && world.alive(local->second) ? world.parts().find(local->second) : nullptr;
        if (part != nullptr) {
            seat.predicted.push_back(
                Sample{tick, part->cframe, m_replay != nullptr ? m_replay->lastCommand(local->second) : std::nullopt});
            while (seat.predicted.size() > PredictionHistory)
                seat.predicted.pop_front();
            stepped = true;
        }
        // A character on the plane: where its scripts left it.
        else if (const scene::Part2DComponent* sprite = local != m_locals.end() && world.alive(local->second)
                                                            ? world.parts2d().find(local->second)
                                                            : nullptr;
                 sprite != nullptr) {
            seat.predicted2d.push_back(Sample2D{tick, sprite->position, sprite->rotation});
            while (seat.predicted2d.size() > PredictionHistory)
                seat.predicted2d.pop_front();
        }
    }
    // And what the simulation holds after this tick, for a correction to
    // restore and step again for real (ADR 0133): once, whoever was stepped.
    if (stepped && m_replay != nullptr)
        m_replay->remember(tick);
    const InstanceId local = scene::localPlayerOf(world);
    const scene::PlayerComponent* player = local.valid() ? world.players().find(local) : nullptr;
    if (player == nullptr)
        return;
    // **By number** (protocol 37): the authority's atom numbers are not
    // this world's, and an action is what both ends' scripts call it -- so its
    // name crosses once, reliably, and the number every tick after. Sent
    // before the intent that first uses it; one that overtakes it waits.
    Writer names;
    u16 named = 0;
    Writer entries;
    u16 count = 0;
    // **The machine's other players after its first** (protocol 48, ADR
    // 0205): an entry that is no action says whose the ones after it are.
    // The whole machine's input is one stream, a tick of it whole or lost.
    std::array<const scene::PlayerComponent*, 4> seats{player, nullptr, nullptr, nullptr};
    for (usize seat = 0; seat < m_guests.size(); ++seat) {
        if (m_guests[seat].userId != 0 && world.alive(m_guests[seat].player))
            seats[seat + 1] = world.players().find(m_guests[seat].player);
    }
    for (u8 seat = 0; seat < seats.size(); ++seat) {
        if (seats[seat] == nullptr)
            continue;
        if (seat != 0) {
            if (count == MaxIntentEntries)
                break;
            entries.u16v(IntentSeat);
            entries.u8v(seat);
            count += 1;
        }
        for (const scene::PlayerIntent& each : seats[seat]->intents) {
            if (count == MaxIntentEntries)
                break;
            // A button not held is what an action nobody sent reads as.
            if (each.type == 0 && !each.pressed)
                continue;
            auto id = m_intentIds.find(each.action.id);
            if (id == m_intentIds.end()) {
                const std::string_view name = world.atoms().text(each.action);
                if (m_intentIds.size() >= MaxIntentNames || name.empty() || name.size() > MaxIntentNameBytes)
                    continue;
                id = m_intentIds.emplace(each.action.id, static_cast<u16>(m_intentIds.size())).first;
                names.u16v(id->second);
                names.text(name);
                named += 1;
            }
            entries.u16v(id->second);
            u8 bits = static_cast<u8>(static_cast<u8>(each.type) & IntentTypeMask);
            if (each.pressed)
                bits |= IntentPressed;
            for (int axis = 0; axis < 3; ++axis) {
                if (bitsOf((&each.axis.x)[axis]) != 0)
                    bits |= static_cast<u8>(IntentAxisX << axis);
            }
            entries.u8v(bits);
            for (int axis = 0; axis < 3; ++axis) {
                if (bitsOf((&each.axis.x)[axis]) != 0)
                    entries.u32v(bitsOf((&each.axis.x)[axis]));
            }
            count += 1;
        }
    }
    if (named > 0) {
        Writer message;
        message.u8v(static_cast<u8>(MessageType::IntentNames));
        message.u16v(named);
        message.bytes.insert(message.bytes.end(), names.bytes.begin(), names.bytes.end());
        sendBytes(m_transport, m_authority, message.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
    }
    Writer one;
    one.u16v(count);
    one.bytes.insert(one.bytes.end(), entries.bytes.begin(), entries.bytes.end());
    m_sentIntents.emplace_back(tick, std::move(one.bytes));
    while (m_sentIntents.size() > IntentRedundancy)
        m_sentIntents.pop_front();
    // A tick says how far it is past the one before in a byte: after a gap
    // no byte holds -- this machine stood still for seconds -- what is older
    // than the gap is not carried.
    for (usize at = m_sentIntents.size(); at-- > 1;) {
        if (m_sentIntents[at].first <= m_sentIntents[at - 1].first ||
            m_sentIntents[at].first - m_sentIntents[at - 1].first > 255) {
            m_sentIntents.erase(m_sentIntents.begin(), m_sentIntents.begin() + static_cast<std::ptrdiff_t>(at));
            break;
        }
    }
    // **This tick and the three before it** (protocol 22): a lost message is
    // a tick of input the next one still carries. **Each said once**
    // (protocol 40): a tick whose intents are the tick before's says so in
    // two bytes, where it said them again whole -- input held is most input.
    Writer intent;
    intent.u8v(static_cast<u8>(MessageType::Intent));
    intent.u32v(m_timeEpoch);
    // **The tick it draws everyone else at** (protocol 55, ADR 0235): what
    // `interpolate` draws, or with no delay, the newest snapshot as applied.
    const u64 drawn = m_interpolationDelay == 0              ? m_lastSnapshotTick
                      : m_serverClock > m_interpolationDelay ? m_serverClock - m_interpolationDelay
                                                             : 0;
    intent.u16v(static_cast<u16>(drawn & 0xFFFFu));
    intent.u8v(static_cast<u8>(m_sentIntents.size()));
    for (usize at = 0; at < m_sentIntents.size(); ++at) {
        const auto& [sentTick, bytes] = m_sentIntents[at];
        if (at == 0) {
            intent.u64v(sentTick);
            intent.bytes.insert(intent.bytes.end(), bytes.begin(), bytes.end());
            continue;
        }
        intent.u8v(static_cast<u8>(sentTick - m_sentIntents[at - 1].first));
        if (bytes == m_sentIntents[at - 1].second)
            intent.u16v(SameIntents);
        else
            intent.bytes.insert(intent.bytes.end(), bytes.begin(), bytes.end());
    }
    sendBytes(m_transport, m_authority, intent.bytes, net::Delivery::UnreliableSequenced, IntentChannel, m_stats);
    sendOwned(world, tick);
}

void ReplicaSession::onPlayers(scene::World& world, InstanceId root, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u32 count = reader.u32v();
    if (!reader.ok() || count > MaxRosterPlayers)
        return;
    std::vector<u32> roster;
    std::map<u32, u32> characters;
    for (u32 at = 0; at < count && reader.ok(); ++at) {
        const u32 userId = reader.u32v();
        const u32 character = reader.u32v();
        roster.push_back(userId);
        characters[userId] = character;
    }
    if (!reader.ok() || !reader.done())
        return;
    m_characters = std::move(characters);
    const InstanceId network = scene::networkServiceOf(world, world.parentOf(root));
    if (!network.valid())
        return;

    // The others, as ordinary `Player`s nobody at this machine drives: gone
    // first, then new, so a script's `PlayerRemoving` sees the list shrink
    // before `PlayerAdded` sees it grow.
    std::vector<InstanceId> leaving;
    for (InstanceId child = world.firstChild(network); child.valid(); child = world.nextSibling(child)) {
        const scene::PlayerComponent* player = world.players().find(child);
        if (player == nullptr || player->local || world.destroyed(child))
            continue;
        if (std::find(roster.begin(), roster.end(), player->userId) == roster.end())
            leaving.push_back(child);
    }
    for (const InstanceId gone : leaving)
        scene::removePlayer(world, network, gone);
    for (const u32 userId : roster) {
        if (userId == m_playerId || scene::playerByUserId(world, userId).valid())
            continue;
        (void)scene::createPlayer(world, network, userId, false);
    }
}

void ReplicaSession::onSpawn(scene::World& world, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u32 count = reader.u32v();
    for (u32 at = 0; at < count && reader.ok(); ++at) {
        const u32 id = reader.u32v();
        const std::string_view className = reader.text();
        const std::string_view originText = reader.text();
        const u32 originIndex = reader.u32v();
        if (!reader.ok() || m_locals.contains(id))
            continue;
        if (m_locals.size() >= MaxReplicaInstances)
            break;
        // An id that left this replica's interest and has come back into it.
        m_departed.erase(id);
        // **By name, not by the authority's class number**: the two ends build
        // their registries from the same generated tables, but a name is the
        // fact the protocol version vouches for and a number is not. Looked
        // up, never interned: every class this build has is named already,
        // and a name it does not have is nothing to intern for ever.
        const scene::ClassId classId = world.classes().findId(world.atoms().lookup(className));
        if (classId == scene::InvalidClass)
            continue;
        const InstanceId local = world.create(classId);
        if (scene::RigidBodyComponent* body = world.rigidBodies().find(local); body != nullptr)
            body->fromAuthority = true;
        if (!local.valid())
            continue;
        m_locals[id] = local;
        // Made here now: whatever the last state said of this id, this one
        // has been written nothing.
        m_unsettled.insert(id);
        // **This machine's own scripts for where it was authored** (ADR 0138
        // §6). Looked up, never interned, for the reason the class is: an
        // origin this replica never read is not one it holds scripts for --
        // except a stamp its own package holds, read the first time
        // (`ScriptTemplates::stampAsset` says why no other name is interned).
        ScriptTemplates* templates = m_templates ? m_templates() : nullptr;
        if (!originText.empty() && templates != nullptr) {
            const core::NameAtom asset = originText.starts_with("stamp:") ? templates->stampAsset(world, originText)
                                                                          : world.atoms().lookup(originText);
            if (asset.valid()) {
                world.setOrigin(local, scene::World::Origin{asset, originIndex});
                (void)templates->attach(world, local, scene::World::Origin{asset, originIndex});
            }
        }
    }
}

void ReplicaSession::onDespawn(scene::World& world, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    std::vector<u32> destroyed;
    std::vector<u32> streamed;
    for (std::vector<u32>* list : {&destroyed, &streamed}) {
        const u32 count = reader.u32v();
        for (u32 at = 0; at < count && reader.ok(); ++at) {
            const u32 id = reader.u32v();
            if (reader.ok())
                list->push_back(id);
        }
    }
    if (!reader.ok())
        return;

    // **Held ones first, then the rest.** A husk is taken out of the tree
    // before anything is destroyed, so a part a script holds survives its
    // parent leaving with it -- `destroy` takes the whole subtree.
    std::vector<InstanceId> removed;
    for (const u32 id : streamed) {
        const auto found = m_locals.find(id);
        if (found == m_locals.end() || !world.alive(found->second))
            continue;
        if (m_probe && m_probe(found->second)) {
            (void)world.setParent(found->second, InstanceId{});
            m_streamedOut.push_back(found->second);
        }
        else {
            removed.push_back(found->second);
        }
    }
    for (const u32 id : destroyed) {
        if (const auto found = m_locals.find(id); found != m_locals.end() && world.alive(found->second))
            removed.push_back(found->second);
    }
    for (const InstanceId gone : removed) {
        if (world.alive(gone))
            (void)world.destroy(gone);
    }

    std::vector<u32> gone = std::move(destroyed);
    gone.insert(gone.end(), streamed.begin(), streamed.end());
    forget(gone);
}

void ReplicaSession::resetForRejoin(scene::World& world)
{
    // Held ones become husks first, as `onDespawn` does it: `destroy` takes a
    // whole subtree, and a part a script holds must survive its parent going.
    std::vector<InstanceId> removed;
    for (const auto& [id, local] : m_locals) {
        if (!world.alive(local))
            continue;
        // **A service is this machine's own**, mapped to the authority's and
        // never spawned: the `Workspace` has been on the wire since protocol
        // 19, and destroying it here left every instance of the rejoined world
        // parented under a destroyed one (found by a refusal).
        if (const scene::ClassDescriptor* descriptor = world.classes().find(world.classOf(local));
            descriptor != nullptr && scene::hasFlag(descriptor->flags, scene::ClassFlags::Service))
            continue;
        if (m_probe && m_probe(local)) {
            (void)world.setParent(local, InstanceId{});
            m_streamedOut.push_back(local);
        }
        else {
            removed.push_back(local);
        }
    }
    for (const InstanceId gone : removed) {
        if (world.alive(gone))
            (void)world.destroy(gone);
    }
    m_locals.clear();
    m_written.clear();
    m_appliedState.reset();
    m_unsettled.clear();
    m_departed.clear();
    m_names.clear();
    m_states.clear();
    // A rejoin's first scene is the join's.
    m_sceneHeard = false;
    m_samples.clear();
    m_samples2d.clear();
    m_characters.clear();
    m_ownedParts.clear();
    for (Owned& seat : m_seats)
        seat = Owned{};
    m_sentIntents.clear();
    m_intentIds.clear();
    m_predictedParts.clear();
    m_ackedIntent = 0;
    m_reconciledAck = 0;
    m_applied = 0;
    // Names are a connection's: the next one tells them from zero.
    m_attributeNames.clear();
}

void ReplicaSession::forget(std::span<const u32> ids)
{
    if (ids.empty())
        return;
    for (const u32 id : ids) {
        m_departed[id] = m_applied;
        m_samples.erase(id);
        m_samples2d.erase(id);
        m_ownedParts.erase(id);
        m_locals.erase(id);
        m_written.erase(id);
        m_unsettled.erase(id);
    }
    // Out of every remembered state too, so a diff against one of them
    // reconstructs what the authority has -- which no longer includes them.
    // **One copy a state for the whole despawn**: a copy per id was a
    // despawn of n ids costing n whole states each.
    std::vector<u32> sorted(ids.begin(), ids.end());
    std::sort(sorted.begin(), sorted.end());
    const auto leaving = [&sorted](const EntityState& entity) {
        return std::binary_search(sorted.begin(), sorted.end(), entity.id.value);
    };
    for (std::shared_ptr<const WorldState>& state : m_states) {
        if (std::none_of(state->entities.begin(), state->entities.end(), leaving))
            continue;
        auto copy = std::make_shared<WorldState>(*state);
        std::erase_if(copy->entities, leaving);
        state = std::move(copy);
    }
}

void ReplicaSession::onSnapshot(scene::World& world, InstanceId root, std::span<const u8> bytes, bool inParts)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u64 tick = reader.u64v();
    const u64 baseTick = reader.u64v();
    const u64 checksum = reader.u64v();
    const u64 intentTick = reader.u64v();
    const u8 intentDepth = reader.u8v();
    const u32 intentStarvations = reader.u32v();
    const u16 dilation = reader.u16v();
    // Its own characters' predicted attributes, a seat at a time: the
    // first player's always, then each guest's that has any (protocol 51).
    std::array<scene::PredictedAttributes, 4> predicted;
    const auto readPredicted = [this, &world, &reader](const Owned& seat, scene::PredictedAttributes& into) {
        const u16 predictedCount = reader.u16v();
        if (predictedCount > MaxPredictedAttributes)
            reader.fail();
        // **Named by hash** (protocol 40): the attributes this machine's own
        // steps have written on that character, which are the ones the
        // authority's steps wrote. One it has not written yet has no name
        // here and is passed over -- and, not being predicted here, is taken
        // from `Attributes` like any other.
        std::map<u32, core::NameAtom> known;
        if (const auto local = seat.netId != 0 ? m_locals.find(seat.netId) : m_locals.end(); local != m_locals.end()) {
            if (const scene::CharacterBodyComponent* body = world.characterBodies().find(local->second);
                body != nullptr) {
                for (const core::NameAtom name : body->predictedAttributes)
                    known.emplace(core::hashTextKey(world.atoms().text(name)), name);
            }
        }
        const auto localOfNet = [this](u32 net) { return localOf(NetId{net}); };
        for (u16 at = 0; at < predictedCount && reader.ok(); ++at) {
            const u32 hash = reader.u32v();
            std::optional<scene::Value> value = readAttributeValue(reader, localOfNet);
            if (!reader.ok() || !value.has_value()) {
                reader.fail();
                break;
            }
            if (const auto named = known.find(hash); named != known.end())
                into.emplace_back(named->second, std::move(*value));
        }
    };
    readPredicted(m_seats[0], predicted[0]);
    {
        const u8 guests = reader.u8v();
        if (guests >= m_seats.size())
            reader.fail();
        for (u8 at = 0; at < guests && reader.ok(); ++at) {
            const u8 seat = reader.u8v();
            if (seat == 0 || seat >= m_seats.size()) {
                reader.fail();
                break;
            }
            readPredicted(m_seats[seat], predicted[seat]);
        }
    }
    // Older than what the world already shows: a reordered straggler, and
    // applying it would move the world backwards.
    if (!reader.ok() || tick <= m_applied)
        return;
    m_stats.intentDepth = intentDepth;
    m_stats.intentStarvations = intentStarvations;
    if (dilation > 0 && dilation <= 10000)
        m_timeDilation = static_cast<f64>(dilation) / 10000.0;

    const WorldState* base = baseTick != 0 ? stateAt(baseTick) : nullptr;
    if (baseTick != 0 && base == nullptr)
        return;

    const u32 atomCount = reader.u32v();
    if (atomCount > MaxReplicaNames)
        reader.fail();
    for (u32 at = 0; at < atomCount && reader.ok(); ++at) {
        const u32 atom = reader.u32v();
        const std::string_view text = reader.text();
        // The atom table never frees: past the bound, a server naming new
        // strings every snapshot is refused rather than kept for ever.
        if (!m_names.contains(atom) && m_names.size() >= MaxReplicaNames)
            reader.fail();
        if (reader.ok())
            m_names[atom] = world.atoms().intern(text);
    }

    auto state = std::make_shared<WorldState>();
    state->tick = tick;
    if (base != nullptr)
        state->entities = base->entities;
    m_heldBackNow.clear();

    // **Records in ascending id order, as the authority writes them**, and the
    // new ones kept aside and merged once: inserted one by one into the sorted
    // state, a snapshot of new ids cost the square of its size.
    std::vector<EntityState> added;
    u32 previous = 0;
    const u32 recordCount = reader.u32v();
    for (u32 record = 0; record < recordCount && reader.ok(); ++record) {
        const u32 id = reader.u32v();
        const u8 schema = reader.u8v();
        const u8 flags = reader.u8v();
        const u16 fields = reader.u16v();
        if (!reader.ok() || schema >= schemaCount() || (record != 0 && id <= previous)) {
            reader.fail();
            break;
        }
        previous = id;
        if ((flags & HeldBackRecord) != 0)
            m_heldBackNow.push_back(id);
        const generated::ClassDesc& desc = generated::Classes[schema];
        EntityState* at = nullptr;
        if (const auto held = findEntity(state->entities, id); held != state->entities.end() && held->id.value == id) {
            at = &*held;
            if ((flags & FullRecord) != 0)
                *at = EntityState{NetId{id}, schema, SharedFields(FieldSet(cellCount(desc)))};
            // Changed by this record: what it comes to is taken again below.
            at->hash = 0;
        }
        else {
            if ((flags & FullRecord) == 0 || state->entities.size() + added.size() >= MaxReplicaInstances) {
                // A diff for an instance the baseline does not have: the two
                // ends disagree about the baseline, which the checksum would
                // catch anyway -- refused here, where the cause is plain.
                reader.fail();
                break;
            }
            added.push_back(EntityState{NetId{id}, schema, SharedFields(FieldSet(cellCount(desc)))});
            at = &added.back();
        }
        for (u16 field = 0; field < fields && reader.ok(); ++field) {
            const u16 named = reader.u16v();
            const bool positionAlone = (named & PositionAlone) != 0;
            const usize index = indexOfWireId(desc, static_cast<u16>(named & ~PositionAlone));
            if (!reader.ok() || index >= fieldCount(desc) || at->fields.size() != cellCount(desc)) {
                reader.fail();
                break;
            }
            const generated::Encoding encoding = fieldAt(desc, index)->encoding;
            // **A sequence says its own length** (protocol 44), and is read
            // into its cells or not at all: a count past what one may hold
            // refuses the message.
            if (isSequence(encoding)) {
                if (positionAlone ||
                    !decodeSequenceField(reader.bytes(), reader.at(), desc, index, at->fields.write())) {
                    reader.fail();
                    break;
                }
                continue;
            }
            if (positionAlone) {
                // Over the cell the baseline left there, whose rotation
                // stands: only a diff of a frame can say so.
                if ((flags & FullRecord) != 0 || encoding != generated::Encoding::CFrameD ||
                    !decodePosition(reader.bytes(), reader.at(), at->fields.write()[index])) {
                    reader.fail();
                    break;
                }
                continue;
            }
            FieldValue value;
            if (!decodeField(reader.bytes(), reader.at(), encoding, value)) {
                reader.fail();
                break;
            }
            at->fields.write()[index] = value;
        }
    }
    if (!reader.ok() || !reader.done())
        return;
    if (!added.empty()) {
        const auto middle = static_cast<std::ptrdiff_t>(state->entities.size());
        state->entities.insert(state->entities.end(), std::make_move_iterator(added.begin()),
                               std::make_move_iterator(added.end()));
        std::inplace_merge(state->entities.begin(), state->entities.begin() + middle, state->entities.end(),
                           [](const EntityState& a, const EntityState& b) { return a.id.value < b.id.value; });
    }

    // Departed ids leave the filter once no baseline can still hold them.
    std::erase_if(m_departed, [&](const auto& entry) { return tick > entry.second + DepartedMemoryTicks; });
    // Asked of every instance only when something has departed: with nothing
    // in the filter it was a lookup an instance a snapshot, for nothing.
    if (!m_departed.empty())
        std::erase_if(state->entities,
                      [this](const EntityState& entity) { return m_departed.contains(entity.id.value); });
    // What each entity this snapshot made or changed comes to; the rest keep
    // the number the state before had for them.
    for (EntityState& entity : state->entities) {
        if (entity.hash == 0)
            entity.hash = hashOf(entity);
    }
    if (checksumOf(*state) != checksum) {
        m_checksumFailures += 1;
        return;
    }

    m_states.push_back(state);
    while (m_states.size() > StateHistory)
        m_states.pop_front();
    m_applied = tick;
    m_stats.snapshotsReceived += 1;
    // **How this snapshot came, against the clock**: late by how
    // many ticks, how unevenly, and how many ticks since the one before --
    // what the interpolation delay is sized from.
    {
        const f64 late = m_serverClock > tick ? static_cast<f64>(m_serverClock - tick) : 0.0;
        m_lateSpread = 0.9 * m_lateSpread + 0.1 * std::abs(late - m_lateAverage);
        m_lateAverage = 0.9 * m_lateAverage + 0.1 * late;
        if (m_lastSnapshotTick != 0 && tick > m_lastSnapshotTick)
            m_snapshotInterval =
                0.9 * m_snapshotInterval + 0.1 * static_cast<f64>(std::min<u64>(tick - m_lastSnapshotTick, 30));
        m_lastSnapshotTick = tick;
    }
    // The server's clock, as far as this replica can tell: the newest tick it
    // has heard of, advanced one a tick between snapshots (`receive`). Pulled
    // forward by a snapshot from further ahead, and snapped when it has drifted
    // more than a handful of ticks behind -- and **nudged back a tick** when a
    // snapshot is later than this link's own spread explains: a clock
    // that only ever moved forward ate the delay for the rest of the session
    // after a route grew slower.
    if (tick > m_serverClock || m_serverClock - tick > 8)
        m_serverClock = tick;
    else if (static_cast<f64>(m_serverClock - tick) > m_lateAverage + 2.0 * m_lateSpread + 1.0)
        m_serverClock -= 1;
    m_ackedIntent = intentTick;
    for (usize seat = 0; seat < m_seats.size(); ++seat)
        m_seats[seat].snapshotAttributes = std::move(predicted[seat]);
    applyToWorld(world, root, state);
    for (Owned& seat : m_seats)
        seat.snapshotAttributes.reset();

    // **Said without a guarantee, and not on the control channel**
    // (protocol 43). It went reliably there, one a snapshot, in front of every
    // message of the game's: on a link losing two packets in a hundred one was
    // lost every second or so, and until it was sent again -- a third of a
    // second at 300 ms -- every `RemoteEvent` behind it waited. One call in
    // five was late by a tenth to a third of a second. And a guarantee bought
    // nothing: the next says more, the authority keeps the newest it heard,
    // and both ends keep the same sixty-four states, so the snapshot after a
    // lost one is diffed against a state this replica still holds.
    //
    // **But the one that came in parts is said reliably**: the authority sends
    // nothing more until it hears of that one (`pinnedPending`), so nothing
    // later could say it instead.
    Writer ack;
    ack.u8v(static_cast<u8>(MessageType::Ack));
    ack.u64v(tick);
    if (inParts)
        sendBytes(m_transport, m_authority, ack.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
    else
        sendBytes(m_transport, m_authority, ack.bytes, net::Delivery::UnreliableSequenced, StateChannel, m_stats);
}

void ReplicaSession::onSnapshotPart(scene::World& world, InstanceId root, std::span<const u8> bytes)
{
    Reader reader(bytes);
    (void)reader.u8v();
    const u64 tick = reader.u64v();
    const u16 index = reader.u16v();
    const u16 count = reader.u16v();
    const u32 size = reader.u32v();
    if (!reader.ok() || count == 0 || index >= count || size > bytes.size())
        return;
    // A first part starts a snapshot; any other must follow the one before.
    // The channel is reliable and in order, so a gap is a new snapshot that
    // replaced this one, and what was joined so far goes.
    if (index == 0) {
        m_partTick = tick;
        m_partNext = 0;
        m_partBytes.clear();
    }
    if (tick != m_partTick || index != m_partNext || m_partBytes.size() + size > MaxReplicaSnapshotBytes) {
        m_partBytes.clear();
        m_partNext = 0;
        return;
    }
    const std::span<const u8> payload = bytes.subspan(bytes.size() - size);
    m_partBytes.insert(m_partBytes.end(), payload.begin(), payload.end());
    m_partNext += 1;
    if (m_partNext < count)
        return;
    std::vector<u8> joined = std::move(m_partBytes);
    m_partBytes.clear();
    m_partNext = 0;
    onSnapshot(world, root, joined, true);
}

// **A hello that passed every check is a player**: the same one again where
// the token is known (ADR 0085), else a new one -- unless the address or the
// match is full, which is said.
void AuthoritySession::admit(scene::World& world, InstanceId network, net::PeerId id, PlayerToken token)
{
    // **A token this authority gave is the same player again**
    // (ADR 0085): the same `UserId`, so a game keyed on it finds
    // the score, the seat and the inventory it left. Anything else
    // -- the first time, or a token from an authority that has
    // since restarted -- is a new player with a new token.
    Peer* peer = peerFor(id);
    if (peer == nullptr)
        return;
    const std::optional<std::array<u8, 32>> key = m_transport.remoteKey(id);
    u32 userId = 0;
    if (const auto known = m_identities.find(token); token.valid() && known != m_identities.end())
        userId = known->second;
    if (userId != 0) {
        // A connection that still holds this player is one that
        // died without saying so and has not timed out yet: the
        // player is here now, so that one is let go.
        std::vector<net::PeerId> stale;
        for (const Peer& other : m_peers) {
            if (!(other.id == id) && other.welcomed && other.token == token)
                stale.push_back(other.id);
        }
        for (const net::PeerId gone : stale) {
            if (Peer* old = peerFor(gone); old != nullptr && old->player.valid())
                scene::removePlayer(world, network, old->player);
            m_transport.disconnect(gone);
            std::erase_if(m_peers, [&](const Peer& candidate) { return candidate.id == gone; });
        }
        peer = peerFor(id);
        if (peer == nullptr)
            return;
    }
    // **An address with as many players as it may be says so**
    // -- a household, a school, a carrier's NAT -- rather than
    // being dropped and heard as a lost connection.
    if (m_maxPerAddress != 0 && peer->address != 0) {
        u32 fromThere = 0;
        for (const Peer& other : m_peers)
            fromThere += other.welcomed && other.address == peer->address ? 1u : 0u;
        if (fromThere >= m_maxPerAddress) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"most", static_cast<core::i64>(m_maxPerAddress)}};
            core::log(core::LogLevel::Info, ENG_TR("net.info.refused_address_limit"), args);
            Writer refused;
            refused.u8v(static_cast<u8>(MessageType::Refused));
            refused.u8v(RefusedAddressLimit);
            refused.text({});
            (void)sendBytes(m_transport, peer->id, refused.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
            m_transport.disconnect(peer->id);
            return;
        }
    }
    // **A full server says so**, after a returning player's
    // old connection was let go -- that seat is theirs.
    if (m_maxPlayers != 0) {
        u32 seated = 0;
        for (const Peer& other : m_peers) {
            seated += other.welcomed ? 1u : 0u;
            // Somebody's guest is a player the match holds.
            for (const Peer::Guest& sitting : other.guests)
                seated += other.welcomed && sitting.userId != 0 ? 1u : 0u;
        }
        if (seated >= m_maxPlayers) {
            Writer refused;
            refused.u8v(static_cast<u8>(MessageType::Refused));
            refused.u8v(RefusedFull);
            refused.text({});
            (void)sendBytes(m_transport, peer->id, refused.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
            m_transport.disconnect(peer->id);
            return;
        }
    }
    if (userId == 0) {
        userId = m_nextUserId++;
        token = freshToken();
        if (key.has_value())
            m_keyTokens[*key] = token;
        // **Bounded**: a peer that throws its token away and
        // dials again is a new player each time. Past the limit
        // the longest-known one not connected now is forgotten.
        if (m_identities.size() >= MaxKnownIdentities) {
            auto oldest = m_identities.end();
            for (auto at = m_identities.begin(); at != m_identities.end(); ++at) {
                const bool connected = std::any_of(m_peers.begin(), m_peers.end(), [&](const Peer& other) {
                    return other.welcomed && other.token == at->first;
                });
                if (!connected && (oldest == m_identities.end() || at->second < oldest->second))
                    oldest = at;
            }
            if (oldest != m_identities.end()) {
                const PlayerToken forgotten = oldest->first;
                std::erase_if(m_keyTokens, [&](const auto& entry) { return entry.second == forgotten; });
                m_identities.erase(oldest);
            }
        }
        m_identities[token] = userId;
    }
    peer->welcomed = true;
    peer->joinedTick = m_tick;
    peer->userId = userId;
    peer->token = token;
    if (network.valid())
        peer->player = scene::createPlayer(world, network, peer->userId, false);
    if (key.has_value()) {
        peer->key = *key;
        if (scene::PlayerComponent* player = world.players().find(peer->player); player != nullptr)
            player->publicKey = core::toHex(*key);
    }
    // The account the game named (ADR 0226), on its player.
    if (scene::PlayerComponent* player = world.players().find(peer->player); player != nullptr)
        player->accountId = peer->accountId;
    Writer welcome;
    welcome.u8v(static_cast<u8>(MessageType::Welcome));
    welcome.u32v(generated::ProtocolVersion);
    welcome.u32v(peer->userId);
    welcome.u64v(m_tick);
    welcome.u64v(token.high);
    welcome.u64v(token.low);
    sendBytes(m_transport, peer->id, welcome.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
}

// **The game's answers, settled** (`setJoinGate`): a refusal is said in the
// game's words and let go; an admission is welcomed as any hello is.
void AuthoritySession::settleAsks(scene::World& world, InstanceId network)
{
    for (auto answer = m_answers.begin(); answer != m_answers.end();) {
        const auto asking = std::find_if(m_peers.begin(), m_peers.end(), [&](const Peer& candidate) {
            return candidate.asking == answer->first && !candidate.welcomed;
        });
        if (asking == m_peers.end()) {
            // Not asked yet, or the joiner has gone: an answer for an ask that
            // is not made yet waits; one for a joiner gone is forgotten.
            if (answer->first >= m_nextAsk)
                ++answer;
            else
                answer = m_answers.erase(answer);
            continue;
        }
        const net::PeerId id = asking->id;
        const PlayerToken token = asking->askedToken;
        JoinAnswer said = std::move(answer->second);
        answer = m_answers.erase(answer);
        asking->asking = 0;
        if (!said.admit) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"reason", std::string_view{said.reason}}};
            core::log(core::LogLevel::Info, ENG_TR("net.info.refused_by_game"), args);
            Writer refused;
            refused.u8v(static_cast<u8>(MessageType::Refused));
            refused.u8v(RefusedByGame);
            refused.text(std::string_view{said.reason}.substr(0, MaxRemovedReasonBytes));
            (void)sendBytes(m_transport, id, refused.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
            m_transport.disconnect(id);
            continue;
        }
        asking->accountId = std::move(said.accountId);
        admit(world, network, id, token);
    }
}

bool AuthoritySession::redirect(u32 userId, std::string_view address, u16 port, std::string_view pass,
                                std::string_view key, std::string_view world)
{
    const auto found = std::find_if(m_peers.begin(), m_peers.end(),
                                    [&](const Peer& peer) { return peer.welcomed && peer.userId == userId; });
    if (found == m_peers.end())
        return false;
    const std::vector<u8> said = redirectMessage(address, port, pass, key, world);
    (void)sendBytes(m_transport, found->id, said, net::Delivery::Reliable, ControlChannel, m_stats);
    return true;
}

void AuthoritySession::announce(std::string_view text)
{
    Writer said;
    said.u8v(static_cast<u8>(MessageType::Announce));
    said.text(text.substr(0, MaxRemovedReasonBytes));
    for (const Peer& peer : m_peers) {
        if (peer.welcomed)
            (void)sendBytes(m_transport, peer.id, said.bytes, net::Delivery::Reliable, ControlChannel, m_stats);
    }
}

std::vector<PeerSummary> AuthoritySession::players() const
{
    std::vector<PeerSummary> out;
    for (const Peer& peer : m_peers) {
        if (!peer.welcomed)
            continue;
        PeerSummary one;
        one.userId = peer.userId;
        one.publicKey = peer.key.has_value() ? core::toHex(*peer.key) : std::string{};
        one.address = m_transport.addressText(peer.id);
        one.pingMs = m_transport.link(peer.id).roundTripMs;
        one.joinedTick = peer.joinedTick;
        one.accountId = peer.accountId;
        out.push_back(std::move(one));
    }
    return out;
}

std::vector<std::pair<u32, u32>> AuthoritySession::playerPings() const
{
    std::vector<std::pair<u32, u32>> out;
    for (const Peer& peer : m_peers) {
        if (!peer.welcomed)
            continue;
        const u32 ping = m_transport.link(peer.id).roundTripMs;
        if (peer.userId != 0)
            out.emplace_back(peer.userId, ping);
        for (const Peer::Guest& guest : peer.guests) {
            if (guest.userId != 0)
                out.emplace_back(guest.userId, ping);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<u8> refusalMessage(u8 reason, std::string_view words)
{
    Writer refused;
    refused.u8v(static_cast<u8>(MessageType::Refused));
    refused.u8v(reason);
    refused.text(words.substr(0, MaxRemovedReasonBytes));
    return std::move(refused.bytes);
}

u32 protocolVersion() noexcept
{
    return generated::ProtocolVersion;
}

u8 transportChannels() noexcept
{
    return static_cast<u8>(std::size(generated::Channels));
}

std::optional<HelloFields> readHello(std::span<const u8> payload)
{
    if (payload.empty() || payload[0] != static_cast<u8>(MessageType::Hello))
        return std::nullopt;
    Reader reader(payload);
    (void)reader.u8v();
    HelloFields hello;
    hello.protocol = reader.u32v();
    (void)reader.u64v();
    (void)reader.u64v();
    hello.luauFlags = reader.u64v();
    if (reader.remaining() >= 2)
        hello.ticket.assign(reader.text().substr(0, MaxTicketBytes));
    if (reader.remaining() >= 2)
        hello.pass.assign(reader.text().substr(0, MaxPassBytes));
    if (!reader.ok())
        return std::nullopt;
    return hello;
}

std::vector<u8> redirectMessage(std::string_view address, u16 port, std::string_view pass, std::string_view key,
                                std::string_view world)
{
    Writer said;
    said.u8v(static_cast<u8>(MessageType::Redirect));
    said.text(address);
    said.u16v(port);
    said.text(pass.substr(0, MaxPassBytes));
    said.text(key.substr(0, 64));
    said.text(world.substr(0, 64));
    return std::move(said.bytes);
}

std::vector<u8> queuedMessage(u32 position, u32 estimateSeconds)
{
    Writer said;
    said.u8v(static_cast<u8>(MessageType::Queued));
    said.u32v(position);
    said.u32v(estimateSeconds);
    return std::move(said.bytes);
}

std::vector<u8> matchPrologue(std::string_view projectId)
{
    std::string text = "match " + std::to_string(generated::ProtocolVersion) + " ";
    text += projectId;
    return std::vector<u8>(text.begin(), text.end());
}

void ReplicaSession::reconcileSeats(scene::World& world, std::span<Answer> answers)
{
    // One character of the machine's, as this answer finds it.
    struct Work
    {
        Answer* answer = nullptr;
        Owned* seat = nullptr;
        scene::PartComponent* part = nullptr;
        const Sample* predicted = nullptr;
        bool comparing = false;
        bool corrected = false;
        core::DVec3 error{};
        f64 distance = 0.0;
        core::Mat3 turn;
        bool turned = false;
        bool attributesOff = false;
        std::vector<core::NameAtom> attributesWrong;
        std::vector<scene::CharacterCommand> commands;
        // How many of those are ticks it has no prediction of: it was not
        // this machine's yet.
        usize before = 0;
    };
    std::vector<Work> work;
    for (Answer& answer : answers) {
        scene::PartComponent* part = world.parts().find(answer.character);
        if (part == nullptr || answer.seat >= m_seats.size())
            continue;
        Work one;
        one.answer = &answer;
        one.seat = &m_seats[answer.seat];
        one.part = part;
        work.push_back(std::move(one));
    }
    if (work.empty())
        return;
    if (m_ackedIntent == 0) {
        // None of this replica's intents applied yet: the authority is right,
        // and the prediction starts from it.
        for (Work& one : work) {
            one.part->cframe = one.answer->start.transform;
            one.seat->predicted.clear();
            one.seat->predicted2d.clear();
        }
        return;
    }
    // What this replica predicted for each at the intent the authority last
    // applied. No such prediction -- it answered one this replica no longer
    // remembers -- is nothing to reconcile against, so the authority is
    // simply right.
    //
    // **An answer with no prediction is not a reason to throw the whole
    // history away**: the authority applies intents a few ticks behind the
    // newest, so it answers ticks older
    // than the one this replica is on. Cleared whole, the history was one
    // every later answer found empty -- snapped, and cleared again, for ever.
    //
    // **Taken, not compared** -- a character that has just become this
    // machine's, or an answer naming a tick with no prediction: the authority
    // is right, at the tick it answered, which is behind the one this
    // replica is on. What it has not answered is stepped again from there
    // below, as a correction's is. Taken as it stood, a character that joined
    // while falling was that many ticks behind its own fall, and the first
    // comparison corrected it by 27 cm (ADR 0133's measurement).
    bool anyComparing = false;
    for (Work& one : work) {
        for (const Sample& sample : one.seat->predicted) {
            if (sample.tick == m_ackedIntent)
                one.predicted = &sample;
        }
        one.comparing = one.seat->synced && one.predicted != nullptr;
        one.seat->synced = true;
        anyComparing = anyComparing || one.comparing;
    }
    // **And every part they predict** (ADR 0133): a crate a character pushed
    // that the authority has somewhere else is a correction of the whole
    // island, even with every character where it should be. The machine's,
    // not a character's: said once, with the first answer.
    m_lastBodyCorrection = 0.0;
    f64 drift = 0.0;
    InstanceId driftPart;
    bool pulled = false;
    if (anyComparing && m_replay != nullptr) {
        for (const scene::CharacterReplayStart::Body& body : work.front().answer->start.bodies) {
            const std::optional<core::CFrameD> mine = m_replay->remembered(m_ackedIntent, body.id);
            if (!mine.has_value())
                continue;
            const core::DVec3 apart = body.cframe.position - mine->position;
            const f64 off = std::sqrt(apart.x * apart.x + apart.y * apart.y + apart.z * apart.z);
            if (off > drift || !driftPart.valid()) {
                drift = std::max(drift, off);
                driftPart = body.id;
            }
            // **A vehicle's chassis under its own threshold is not
            // corrected, and not left either** (ADR 0213): it is stepped
            // again pulled part of the way to the authority's, so two
            // machines never settle a few centimetres apart about where a
            // car is -- which is what decides what it hits.
            if (off < scene::VehicleCorrectionMetres && carriesVehicle(world, body.id)) {
                pulled = pulled || off >= ResyncMetres;
                continue;
            }
            m_lastBodyCorrection = std::max(m_lastBodyCorrection, off);
        }
    }
    bool agreed = true;
    for (Work& one : work) {
        if (!one.comparing) {
            agreed = false;
            continue;
        }
        const core::CFrameD& authority = one.answer->start.transform;
        one.error = authority.position - one.predicted->cframe.position;
        one.distance = std::sqrt(one.error.x * one.error.x + one.error.y * one.error.y + one.error.z * one.error.z);
        // The turn the authority disagrees by, the same way.
        one.turn = authority.rotation * core::transpose(one.predicted->cframe.rotation);
        for (int column = 0; column < 3 && !one.turned; ++column) {
            for (int row = 0; row < 3; ++row) {
                const core::f32 identity = column == row ? 1.0f : 0.0f;
                if (std::abs(one.turn.m[column][row] - identity) > 1e-3f) {
                    one.turned = true;
                    break;
                }
            }
        }
        // **And what its predicted steps wrote**: an attribute the
        // authority has otherwise at that tick -- a dash it never began, a
        // value this replica's own code wrote over -- is a correction too.
        if (one.answer->start.attributes.has_value() && m_replay != nullptr) {
            const std::optional<scene::PredictedAttributes> mine =
                m_replay->rememberedAttributes(m_ackedIntent, one.answer->character);
            if (mine.has_value()) {
                for (const auto& [name, value] : *one.answer->start.attributes) {
                    const auto held = std::find_if(mine->begin(), mine->end(),
                                                   [&](const auto& entry) { return entry.first == name; });
                    const scene::Value remembered = held != mine->end() ? held->second : scene::Value{};
                    if (!(remembered == value)) {
                        one.attributesOff = true;
                        if (m_logCorrections)
                            one.attributesWrong.push_back(name);
                    }
                }
            }
        }
        agreed = agreed && one.distance < ResyncMetres && !one.turned && !one.attributesOff;
    }
    // **Agreed is agreed**: every one of them, and what they push. Nothing to do.
    if (anyComparing) {
        m_stats.predictedDriftMetres = drift;
        m_stats.predictedDriftPart = driftPart;
    }
    if (agreed && m_lastBodyCorrection < ResyncMetres && !pulled)
        return;
    // **Under a centimetre, stepped again without a word** (ADR 0133). From
    // the same state the two machines step the island to the bit, but a
    // replay does not always land there -- the solver warm-starts from
    // contacts it cached at later ticks -- and a fifth of a millimetre left
    // alone was a centimetre the next time two crates met. Caught while it is
    // that small it never grows: the same replay as a correction, not counted
    // as one.
    //
    // **A correction is counted for each character that was wrong** (ADR
    // 0208), and once for the machine where only what they push was.
    bool anyCorrected = false;
    for (Work& one : work) {
        one.corrected = one.comparing && (one.distance >= 0.01 || one.turned || one.attributesOff);
        anyCorrected = anyCorrected || one.corrected;
    }
    const bool bodiesCorrected = anyComparing && m_lastBodyCorrection >= 0.01;
    // **What they push alone is the machine's, and no character's**.
    // It was given to the first of them -- counted as that player's, with the
    // part's distance as how far that player's character had been moved --
    // and the first time the rows a player were looked at, a crate a quarter
    // of a metre off read as "player one: last correction 21 cm" beside a
    // player two with none: the answer to "which of us is the link treating
    // badly", and wrong. The machine's count has it; the log says it was a
    // part's.
    if (!anyCorrected && bodiesCorrected) {
        m_stats.corrections += 1;
        if (m_logCorrections) {
            const std::array<core::I18nArg, 3> args{
                core::I18nArg{"tick", static_cast<core::i64>(world.engineState().tick)},
                core::I18nArg{"intent", static_cast<core::i64>(m_ackedIntent)},
                core::I18nArg{"parts", m_lastBodyCorrection}};
            core::log(core::LogLevel::Info, ENG_TR("net.info.correction_parts"), args);
        }
    }
    for (const Work& one : work) {
        if (!one.corrected)
            continue;
        m_stats.corrections += 1;
        m_stats.seatCorrections[one.answer->seat] += 1;
        if (!m_logCorrections)
            continue;
        // **What it disagreed about, and whose it was** (ADR 0208): a
        // count says a prediction was wrong and nothing of why, and the why
        // is the game's to act on -- a state the authority decided, a body it
        // ran into that is somewhere else there, a long frame.
        std::string names;
        for (const core::NameAtom name : one.attributesWrong) {
            if (!names.empty())
                names += ", ";
            names += world.atoms().text(name);
        }
        const std::array<core::I18nArg, 10> args{
            core::I18nArg{"tick", static_cast<core::i64>(world.engineState().tick)},
            core::I18nArg{"intent", static_cast<core::i64>(m_ackedIntent)},
            core::I18nArg{"metres", one.distance},
            core::I18nArg{"x", one.error.x},
            core::I18nArg{"y", one.error.y},
            core::I18nArg{"z", one.error.z},
            core::I18nArg{"turned", std::string_view{one.turned ? "yes" : "no"}},
            core::I18nArg{"parts", m_lastBodyCorrection},
            core::I18nArg{"attributes", std::string_view{names.empty() ? std::string_view{"-"} : names}},
            core::I18nArg{"seat", static_cast<core::i64>(one.answer->seat + 1)}};
        core::log(core::LogLevel::Info, ENG_TR("net.info.correction"), args);
    }

    // **Stepped again from where the authority put them** -- what every
    // engine that predicts a character does. The commands this replica has
    // not had answered yet are replayed through the same movement step the
    // simulation uses, in the world as it now is, so a prediction that walked
    // into a wall the authority's did not, or landed where the authority's
    // was still falling, comes out where the authority will put it next
    // rather than where the error at one tick implied.
    //
    // Only when every unanswered tick kept its command, for every one of
    // them: a gap -- a character was not stepped here that tick -- is a
    // history that cannot be replayed, and the error is what is left.
    bool complete = m_replay != nullptr;
    usize count = 0;
    const Work* fullest = nullptr;
    for (usize index = 0; index < work.size() && complete; ++index) {
        Work& one = work[index];
        for (const Sample& sample : one.seat->predicted) {
            if (sample.tick <= m_ackedIntent)
                continue;
            if (!sample.command.has_value()) {
                complete = false;
                break;
            }
            one.commands.push_back(*sample.command);
            // Answered at the authority's speeds, so replayed at them.
            if (one.answer->start.walkSpeed.has_value())
                one.commands.back().walkSpeed = *one.answer->start.walkSpeed;
            if (one.answer->start.jumpSpeed.has_value())
                one.commands.back().jumpSpeed = *one.answer->start.jumpSpeed;
        }
        // **A taken character's history can begin after the tick answered**:
        // its intents were answered from the first one sent, and this
        // machine predicted it only from the tick its character arrived. The
        // ticks between were stepped by the authority all the same -- a fall
        // goes on -- so they are stepped here, standing, as the ticks a guest
        // lacks are below. Left out, a character that joined in the air was
        // replayed a tick short of its fall and corrected by that tick at the
        // first comparison.
        if (complete && !one.comparing && !one.commands.empty() && one.commands.front().tick > m_ackedIntent + 1) {
            const usize lacking = static_cast<usize>(one.commands.front().tick - m_ackedIntent - 1);
            scene::CharacterCommand standing = one.commands.front();
            standing.moveDirection = core::Vec3{};
            standing.jump = false;
            standing.intents.clear();
            standing.presses.clear();
            std::vector<scene::CharacterCommand> whole;
            whole.reserve(lacking + one.commands.size());
            for (usize at = 0; at < lacking; ++at) {
                standing.tick = m_ackedIntent + 1 + at;
                whole.push_back(standing);
            }
            whole.insert(whole.end(), std::make_move_iterator(one.commands.begin()),
                         std::make_move_iterator(one.commands.end()));
            one.commands = std::move(whole);
            one.before = lacking;
        }
        if (fullest == nullptr || one.commands.size() > count) {
            count = one.commands.size();
            fullest = &one;
        }
    }
    // **One step a tick for all of them: as many ticks each** -- and a
    // character that has only just become this machine's has fewer (ADR 0208):
    // a guest who sat down in the middle of a match was sent no intent before
    // the tick its number arrived, so this machine has no command of its for
    // the ticks before. The authority had none either: it stepped the
    // character those ticks with nobody at its stick. So the ticks it lacks
    // are stepped here as the authority stepped them, standing, and its own
    // from where they begin. Left out of the step instead, it was taken where
    // the authority had it a round trip ago, walked on from there, and was
    // corrected by that round trip's walk at the first answer that could be
    // compared -- the guest's first sight of its own character, a jump back.
    //
    // Only for one whose history begins after the answered tick. One that
    // had the answered tick and lacks a later one has a hole, and a history
    // with a hole cannot be stepped again.
    for (usize index = 0; index < work.size() && complete; ++index) {
        Work& one = work[index];
        if (one.commands.size() == count)
            continue;
        if (one.comparing) {
            complete = false;
            break;
        }
        const usize lacking = count - one.commands.size();
        one.before += lacking;
        scene::CharacterCommand standing;
        if (!one.commands.empty()) {
            standing = one.commands.front();
        }
        else if (const scene::CharacterBodyComponent* body = world.characterBodies().find(one.answer->character);
                 body != nullptr) {
            standing.walkSpeed = one.answer->start.walkSpeed.value_or(body->walkSpeed);
            standing.jumpSpeed = one.answer->start.jumpSpeed.value_or(body->jumpSpeed);
        }
        standing.moveDirection = core::Vec3{};
        standing.jump = false;
        standing.intents.clear();
        standing.presses.clear();
        std::vector<scene::CharacterCommand> whole;
        whole.reserve(count);
        for (usize at = 0; at < lacking; ++at) {
            standing.tick = fullest->commands[at].tick;
            standing.dt = fullest->commands[at].dt;
            whole.push_back(standing);
        }
        whole.insert(whole.end(), std::make_move_iterator(one.commands.begin()),
                     std::make_move_iterator(one.commands.end()));
        one.commands = std::move(whole);
    }
    if (complete) {
        std::vector<scene::CharacterReplayStep> steps;
        steps.reserve(work.size());
        for (Work& one : work) {
            scene::CharacterReplayStart from = one.answer->start;
            from.tick = m_ackedIntent;
            steps.push_back(scene::CharacterReplayStep{one.answer->character, std::move(from), one.commands});
        }
        // Timed for the overlay alone: the clock never reaches the result.
        const auto began = std::chrono::steady_clock::now();
        const std::vector<std::vector<core::CFrameD>> frames = m_replay->replayAll(steps);
        m_stats.resimulationMicros += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
        bool whole = frames.size() == work.size();
        for (usize index = 0; index < frames.size() && whole; ++index)
            whole = frames[index].size() == count;
        if (whole) {
            m_stats.resimulations += 1;
            m_stats.resimulatedTicks += count;
            if (anyCorrected || bodiesCorrected)
                m_stats.replays += 1;
            for (usize index = 0; index < work.size(); ++index) {
                Work& one = work[index];
                const core::CFrameD& authority = one.answer->start.transform;
                // Its own predictions begin where its own ticks do.
                usize at = one.before;
                for (Sample& sample : one.seat->predicted) {
                    if (sample.tick <= m_ackedIntent)
                        continue;
                    sample.cframe.position = frames[index][at].position;
                    if (one.turned)
                        sample.cframe.rotation = one.turn * sample.cframe.rotation;
                    else if (!one.comparing)
                        sample.cframe.rotation = authority.rotation;
                    ++at;
                }
                // The rotation is the scripts', which a controller does not
                // turn: corrected by the turn at that moment, as before --
                // or, taken, the authority's.
                core::CFrameD now = one.part->cframe;
                now.position = frames[index].empty() ? authority.position : frames[index].back().position;
                if (one.turned)
                    now.rotation = one.turn * now.rotation;
                else if (!one.comparing)
                    now.rotation = authority.rotation;
                one.part->cframe = now;
                std::erase_if(one.seat->predicted,
                              [this](const Sample& sample) { return sample.tick < m_ackedIntent; });
            }
            return;
        }
    }
    for (Work& one : work) {
        const core::CFrameD& authority = one.answer->start.transform;
        // Not stepped again: its predicted attributes are the authority's, as
        // its place is.
        if (one.answer->start.attributes.has_value()) {
            for (const auto& [name, value] : *one.answer->start.attributes) {
                if (!(world.getAttribute(one.answer->character, name) == value))
                    (void)world.setAttribute(one.answer->character, name, value);
            }
        }
        if (!one.comparing) {
            // Nothing to step again: taken as it stands -- and **so is every
            // prediction made after the tick it answers**. Left at what
            // they were, the next answer found one, read the whole distance
            // the take had just moved the character as an error, and added it
            // to a character already holding the authority's place: a body
            // put at (0.5, 27.9, 0.5) by the server stood at twice that.
            one.part->cframe = authority;
            std::erase_if(one.seat->predicted, [this](const Sample& sample) { return sample.tick <= m_ackedIntent; });
            for (Sample& sample : one.seat->predicted)
                sample.cframe = authority;
            continue;
        }
        // **The error at that moment, carried forward to now**, where there
        // is no history to replay: the correction the error implies wherever
        // the ground does not change underfoot, and the next snapshot
        // corrects what it could not.
        const auto correct = [&](core::CFrameD& frame) {
            frame.position = frame.position + one.error;
            if (one.turned)
                frame.rotation = one.turn * frame.rotation;
        };
        correct(one.part->cframe);
        for (Sample& sample : one.seat->predicted) {
            if (sample.tick > m_ackedIntent)
                correct(sample.cframe);
        }
    }
}

void ReplicaSession::reconcile2d(scene::World& world, Owned& seat, InstanceId character, core::Vec2 position,
                                 core::f32 rotation)
{
    scene::Part2DComponent* sprite = world.parts2d().find(character);
    if (sprite == nullptr)
        return;
    // **The rule a 3D character a script moves is corrected by, on the
    // plane**. The sprite is this machine's to move: whatever its
    // scripts did to it each tick is kept, by tick, and the authority's place
    // for it at the intent it last applied is compared with what this machine
    // had THEN. The difference is carried forward to now.
    //
    // So a game whose client steps its hero with the code the server steps it
    // with sees no correction at all and no delay; and a game whose client
    // does nothing -- the server moves the hero from `GetIntent` -- has
    // "what it had then" standing still, so the difference is the whole of
    // the authority's motion and the hero is where the authority's newest
    // word puts it. Either way it is not drawn a few ticks in the past, which
    // is what every other sprite is.
    const Sample2D* predicted = nullptr;
    for (const Sample2D& sample : seat.predicted2d) {
        if (sample.tick == m_ackedIntent)
            predicted = &sample;
    }
    // None of this machine's intents applied yet, a sprite that has only now
    // become its own, or an answer to a tick no longer remembered: the
    // authority is right.
    const bool comparing = m_ackedIntent != 0 && seat.synced && predicted != nullptr;
    seat.synced = true;
    if (!comparing) {
        sprite->position = position;
        sprite->rotation = rotation;
        std::erase_if(seat.predicted2d, [this](const Sample2D& sample) { return sample.tick <= m_ackedIntent; });
        // What was predicted after the answered tick is taken too.
        for (Sample2D& sample : seat.predicted2d) {
            sample.position = position;
            sample.rotation = rotation;
        }
        return;
    }
    const core::Vec2 error = position - predicted->position;
    // The short way round, as the interpolation turns.
    core::f32 turn = std::fmod(rotation - predicted->rotation, 360.0f);
    if (turn > 180.0f)
        turn -= 360.0f;
    else if (turn < -180.0f)
        turn += 360.0f;
    const f64 distance = std::sqrt(static_cast<f64>(error.x) * static_cast<f64>(error.x) +
                                   static_cast<f64>(error.y) * static_cast<f64>(error.y));
    std::erase_if(seat.predicted2d, [this](const Sample2D& sample) { return sample.tick < m_ackedIntent; });
    // Agreed is agreed.
    if (distance < ResyncMetres && std::abs(turn) < 1.0e-3f)
        return;
    const usize seatIndex = static_cast<usize>(&seat - m_seats.data());
    m_stats.corrections += 1;
    m_stats.lastCorrectionMetres = distance;
    m_stats.seatCorrections[seatIndex] += 1;
    m_stats.seatLastCorrectionMetres[seatIndex] = distance;
    if (m_logCorrections) {
        const std::array<core::I18nArg, 10> args{
            core::I18nArg{"tick", static_cast<core::i64>(world.engineState().tick)},
            core::I18nArg{"intent", static_cast<core::i64>(m_ackedIntent)},
            core::I18nArg{"metres", distance},
            core::I18nArg{"x", static_cast<f64>(error.x)},
            core::I18nArg{"y", static_cast<f64>(error.y)},
            core::I18nArg{"z", 0.0},
            core::I18nArg{"turned", std::string_view{std::abs(turn) < 1.0e-3f ? "no" : "yes"}},
            core::I18nArg{"parts", 0.0},
            core::I18nArg{"attributes", std::string_view{"-"}},
            core::I18nArg{"seat", static_cast<core::i64>(seatIndex + 1)}};
        core::log(core::LogLevel::Info, ENG_TR("net.info.correction"), args);
    }
    sprite->position = sprite->position + error;
    sprite->rotation = sprite->rotation + turn;
    for (Sample2D& sample : seat.predicted2d) {
        sample.position = sample.position + error;
        sample.rotation = sample.rotation + turn;
    }
}

void ReplicaSession::applyToWorld(scene::World& world, InstanceId root,
                                  const std::shared_ptr<const WorldState>& applied)
{
    const WorldState& state = *applied;
    // Whichever way this returns, the world is as this state says from here.
    struct Settle
    {
        std::shared_ptr<const WorldState>& held;
        const std::shared_ptr<const WorldState>& now;
        ~Settle() { held = now; }
    };
    // Each own character's answer, by seat, kept until every entity is read
    // (ADRs 0133, 0208).
    std::array<std::optional<scene::CharacterReplayStart>, 4> answers;
    std::array<InstanceId, 4> answeredCharacters{};
    // The same for a character on the plane.
    std::array<std::optional<Sample2D>, 4> answers2d;
    const usize nameField = commonIndex("Name");
    const usize parentField = commonIndex("Parent");
    // **This world's own copy of each service, by its class name, first.**
    // Services sort after every instance, and an instance kept in one
    // (ADR 0080) names it as its parent in this same state.
    for (const EntityState& entity : state.entities) {
        if (entity.id.value < ServiceNetIdBase || m_locals.contains(entity.id.value))
            continue;
        const InstanceId dataModel = world.parentOf(root);
        const std::string_view wanted = generated::Classes[entity.schema].name;
        for (InstanceId child = dataModel.valid() ? world.firstChild(dataModel) : InstanceId{}; child.valid();
             child = world.nextSibling(child)) {
            if (world.atoms().text(world.classes().find(world.classOf(child))->name) == wanted) {
                m_locals[entity.id.value] = child;
                break;
            }
        }
    }
    // The state last written from, walked beside this one: both in id order.
    const std::shared_ptr<const WorldState> last = m_appliedState;
    const Settle settle{m_appliedState, applied};
    auto lastAt = last != nullptr ? last->entities.begin() : state.entities.end();
    const auto lastEnd = last != nullptr ? last->entities.end() : state.entities.end();
    for (const EntityState& entity : state.entities) {
        const bool service = entity.id.value >= ServiceNetIdBase;
        // **The very set it was last written from: nothing to write**.
        // Not the own character, which is an answer to compare whether or
        // not it moved; and not what is unsettled.
        Owned* const seat = seatOfNet(entity.id.value);
        const bool own = seat != nullptr;
        const usize seatIndex = own ? static_cast<usize>(seat - m_seats.data()) : 0;
        bool asWritten = false;
        if (last != nullptr) {
            while (lastAt != lastEnd && lastAt->id.value < entity.id.value)
                ++lastAt;
            asWritten =
                lastAt != lastEnd && lastAt->id.value == entity.id.value && lastAt->fields.sameSetAs(entity.fields);
        }
        if (asWritten && !own && (m_unsettled.empty() || !m_unsettled.contains(entity.id.value)))
            continue;
        const auto local = m_locals.find(entity.id.value);
        if (local == m_locals.end() || !world.alive(local->second)) {
            // Its spawn has not arrived yet; the next apply writes it whole.
            m_unsettled.insert(entity.id.value);
            continue;
        }
        const generated::ClassDesc& desc = generated::Classes[entity.schema];
        const auto written = m_written.find(entity.id.value);
        m_stats.entitiesApplied += 1;
        FieldSet next = entity.fields.copy();
        // A field left waiting for the instance it names: read again next time.
        bool waiting = false;
        // **The own character is checked against every answer, moved or not.**
        // Only what changed is written, and an authority that stopped the
        // character -- against a wall the prediction walked through -- sends
        // the same transform tick after tick: compared only on a change, the
        // prediction was never corrected and walked on through the wall.
        const bool answered = own && m_ackedIntent != m_reconciledAck;
        bool sampled2d = false;

        // The fields; the cells past them are their sequences', written with them.
        for (usize at = 0, count = std::min(fieldCount(desc), entity.fields.size()); at < count; ++at) {
            const FieldValue& value = entity.fields[at];
            // **A sequence is all its cells** (protocol 44): written whole
            // when any of them is other than what was last written. One that
            // is no sequence -- a peer is not trusted to send one -- is not
            // written, and what the instance had stands.
            if (const generated::FieldDesc* keys = fieldAt(desc, at); keys != nullptr && isSequence(keys->encoding)) {
                if (written == m_written.end() || !sameField(desc, at, written->second, entity.fields.view()))
                    (void)applySequence(world, local->second, desc, at, entity.fields.view());
                continue;
            }
            if (written != m_written.end() && at < written->second.size() && written->second[at] == value) {
                const generated::FieldDesc* same = answered ? fieldAt(desc, at) : nullptr;
                if (same == nullptr || same->name != "CFrame" || same->pool != "parts")
                    continue;
            }
            // A service keeps its own name and its place under the data model.
            if (service && (at == nameField || at == parentField))
                continue;
            if (at == nameField) {
                const auto name = m_names.find(asU32(value));
                if (name != m_names.end())
                    world.setName(local->second, name->second);
                continue;
            }
            if (at == parentField) {
                const u32 parentId = asNetId(value).value;
                const InstanceId parent = parentId == RootNetId.value ? root : localOf(NetId{parentId});
                if (!parent.valid()) {
                    next[at] = pendingValue();
                    waiting = true;
                    continue;
                }
                if (!(world.parentOf(local->second) == parent))
                    (void)world.setParent(local->second, parent);
                continue;
            }
            const generated::FieldDesc* field = fieldAt(desc, at);
            const bool cframe = field != nullptr && field->name == "CFrame" && field->pool == "parts";
            const bool placed2d = field != nullptr && field->pool == "parts2d" &&
                                  (field->name == "Position" || field->name == "Rotation") &&
                                  !m_ownedParts.contains(entity.id.value);
            // **An instance a joint names**: the authority's network id,
            // this machine's own copy by the time it is written -- and, until
            // that copy has arrived, nothing written and the next apply trying
            // again, as a parent is.
            if (field != nullptr && field->encoding == generated::Encoding::InstanceRef) {
                const u32 named = asNetId(value).value;
                const InstanceId target = named == 0 ? InstanceId{} : localOf(NetId{named});
                if (named != 0 && !target.valid()) {
                    next[at] = pendingValue();
                    waiting = true;
                    continue;
                }
                FieldValue translated;
                setInstance(translated, target);
                (void)applyField(world, local->second, desc, FieldDelta{wireIdAt(desc, at), translated});
                continue;
            }
            // A name-shaped component field arrives as the authority's atom,
            // and is this machine's own atom by the time it is written.
            if (field != nullptr && field->encoding == generated::Encoding::NameAtom) {
                const auto name = m_names.find(asU32(value));
                if (name == m_names.end())
                    continue;
                FieldValue translated;
                setU32(translated, name->second.id);
                (void)applyField(world, local->second, desc, FieldDelta{wireIdAt(desc, at), translated});
                continue;
            }
            if (own) {
                // **This machine's own character is predicted** (ADR 0076): the
                // local scripts already moved it, so the authority's value
                // corrects it rather than replacing it. Its motion state is the
                // local simulation's for the same reason.
                if (cframe) {
                    // With the authority's motion state beside its transform:
                    // what a replay starts from that a position does not say.
                    scene::CharacterReplayStart start;
                    start.transform = asCFrame(value);
                    for (usize other = 0; other < entity.fields.size(); ++other) {
                        const generated::FieldDesc* motion = fieldAt(desc, other);
                        if (motion == nullptr || motion->pool != "characterBodies")
                            continue;
                        if (motion->name == "VerticalVelocity")
                            start.verticalVelocity = asF32(entity.fields[other]);
                        else if (motion->name == "PushVelocity")
                            start.push = asVec3(entity.fields[other]);
                        else if (motion->name == "Grounded")
                            start.grounded = asBool(entity.fields[other]);
                        else if (motion->name == "WalkSpeed")
                            start.walkSpeed = asF32(entity.fields[other]);
                        else if (motion->name == "JumpSpeed")
                            start.jumpSpeed = asF32(entity.fields[other]);
                    }
                    start.attributes = seat->snapshotAttributes;
                    answers[seatIndex] = start;
                    answeredCharacters[seatIndex] = local->second;
                }
                // The MOTION state is the local simulation's; the settings it
                // steps with -- `WalkSpeed` and the rest -- are the authority's,
                // and a replica that kept its own predicted at the wrong speed
                // and was corrected every snapshot.
                if (field != nullptr && field->pool == "characterBodies" &&
                    (field->name == "VerticalVelocity" || field->name == "Grounded" || field->name == "State" ||
                     field->name == "PushVelocity"))
                    continue;
                // **And its body's velocity is its motion too**: the
                // authority's, a round trip older than the prediction, was
                // written over it, and a predicted step that read the body's
                // speed -- a fall's, for a landing -- read a past one and
                // differed from the authority at every snapshot of a fall.
                if (field != nullptr && field->pool == "rigidBodies" &&
                    (field->name == "LinearVelocity" || field->name == "AngularVelocity"))
                    continue;
                if (cframe)
                    continue;
                // **A character on the plane, the same**: where the
                // authority has it is an answer to compare, read whole below,
                // and neither written over what the local scripts did nor
                // kept to be drawn from the past.
                if (field != nullptr && field->pool == "parts2d" &&
                    (field->name == "Position" || field->name == "Rotation"))
                    continue;
            }
            else if (cframe && m_ownedParts.contains(entity.id.value) && written != m_written.end()) {
                // **Its own part is simulated here** (ADR 0099): the authority's
                // copy is this machine's, a round trip old. **Once it has been
                // placed**: handed over before its first state arrived -- a
                // part a server made and gave away in the same tick, a first
                // snapshot that came in parts -- it was never put anywhere,
                // simulated from the origin and reported from there.
                continue;
            }
            else if (m_predictedParts.contains(entity.id.value) &&
                     (cframe || (field != nullptr && field->pool == "rigidBodies" &&
                                 (field->name == "LinearVelocity" || field->name == "AngularVelocity")))) {
                // **Predicted here** (ADR 0133): simulated beside the
                // character and corrected with it, from the answer below.
                continue;
            }
            else if (cframe && m_drivenWheels.contains(entity.id.value)) {
                // A wheel of the vehicle this machine drives, placed here by
                // it (ADR 0213).
                continue;
            }
            else if (cframe && m_interpolationDelay > 0) {
                std::deque<Sample>& samples = m_samples[entity.id.value];
                // **Further in the past for a part that waited** (ADR 0229
                // §3): by how much later than a snapshot's interval its
                // states come, eased in and out.
                core::f32 lag = 0.0f;
                if (!samples.empty()) {
                    const bool waited = std::binary_search(m_heldBackNow.begin(), m_heldBackNow.end(), entity.id.value);
                    const f64 gap =
                        state.tick > samples.back().tick ? static_cast<f64>(state.tick - samples.back().tick) : 0.0;
                    const f64 late = waited ? std::clamp(gap - m_snapshotInterval, 0.0, MaxPartLagTicks) : 0.0;
                    // Eased in as it waits; back on time twice as fast
                    // once it is sent on time again.
                    lag = static_cast<core::f32>(waited ? 0.7 * static_cast<f64>(samples.back().lag) + 0.3 * late
                                                        : 0.5 * static_cast<f64>(samples.back().lag));
                    if (!waited && lag < 0.05f)
                        lag = 0.0f;
                }
                samples.push_back(Sample{state.tick, asCFrame(value), std::nullopt, lag});
                while (samples.size() > InterpolationSamples)
                    samples.pop_front();
                continue;
            }
            else if (placed2d && m_interpolationDelay > 0) {
                // Both halves of where a sprite is, from the whole state, once
                // per snapshot however many of the two changed (ADR 0103).
                if (!sampled2d) {
                    sampled2d = true;
                    Sample2D sample{state.tick, {}, 0.0f};
                    for (usize other = 0; other < entity.fields.size(); ++other) {
                        const generated::FieldDesc* half = fieldAt(desc, other);
                        if (half == nullptr || half->pool != "parts2d")
                            continue;
                        if (half->name == "Position") {
                            const core::Vec3 place = asVec3(entity.fields[other]);
                            sample.position = core::Vec2{place.x, place.y};
                        }
                        else if (half->name == "Rotation") {
                            sample.rotation = asF32(entity.fields[other]);
                        }
                    }
                    std::deque<Sample2D>& samples = m_samples2d[entity.id.value];
                    samples.push_back(sample);
                    while (samples.size() > InterpolationSamples)
                        samples.pop_front();
                }
                continue;
            }
            (void)applyField(world, local->second, desc, FieldDelta{wireIdAt(desc, at), value});
        }
        m_written[entity.id.value] = std::move(next);
        if (waiting)
            m_unsettled.insert(entity.id.value);
        else if (!m_unsettled.empty())
            m_unsettled.erase(entity.id.value);

        // The own sprite's place in this state, changed or not: an authority
        // that stopped it sends the same place tick after tick, and that is
        // an answer too.
        if (own && world.parts2d().find(local->second) != nullptr) {
            Sample2D place{state.tick, {}, 0.0f};
            for (usize at = 0; at < entity.fields.size(); ++at) {
                const generated::FieldDesc* half = fieldAt(desc, at);
                if (half == nullptr || half->pool != "parts2d")
                    continue;
                if (half->name == "Position") {
                    const core::Vec3 at2 = asVec3(entity.fields[at]);
                    place.position = core::Vec2{at2.x, at2.y};
                }
                else if (half->name == "Rotation") {
                    place.rotation = asF32(entity.fields[at]);
                }
            }
            answers2d[seatIndex] = place;
            answeredCharacters[seatIndex] = local->second;
        }
    }

    // A character on the plane, a seat at a time: it has no island to
    // step again.
    bool any3d = false;
    bool any2d = false;
    for (usize index = 0; index < m_seats.size(); ++index) {
        any3d = any3d || answers[index].has_value();
        if (!answers2d[index].has_value() || answers[index].has_value())
            continue;
        any2d = true;
        if (m_ackedIntent != m_reconciledAck || !m_seats[index].synced)
            reconcile2d(world, m_seats[index], answeredCharacters[index], answers2d[index]->position,
                        answers2d[index]->rotation);
    }
    if (!any3d) {
        if (any2d)
            m_reconciledAck = m_ackedIntent;
        return;
    }
    std::vector<Answer> all;
    for (usize index = 0; index < m_seats.size(); ++index) {
        if (answers[index].has_value())
            all.push_back(Answer{static_cast<u8>(index), answeredCharacters[index], std::move(*answers[index])});
    }
    // The predicted parts at the answered tick, from this whole state: the
    // machine's, said with the first answer.
    for (const auto& [netId, away] : m_predictedParts) {
        const EntityState* held = findEntity(state, netId);
        const auto local = m_locals.find(netId);
        if (held == nullptr || local == m_locals.end())
            continue;
        scene::CharacterReplayStart::Body body;
        body.id = local->second;
        const generated::ClassDesc& desc = generated::Classes[held->schema];
        for (usize at = 0; at < held->fields.size(); ++at) {
            const generated::FieldDesc* field = fieldAt(desc, at);
            if (field == nullptr)
                continue;
            if (field->name == "CFrame" && field->pool == "parts")
                body.cframe = asCFrame(held->fields[at]);
            else if (field->name == "LinearVelocity" && field->pool == "rigidBodies")
                body.linear = asVec3(held->fields[at]);
            else if (field->name == "AngularVelocity" && field->pool == "rigidBodies")
                body.angular = asVec3(held->fields[at]);
        }
        all.front().start.bodies.push_back(body);
    }
    // **And each vehicle on a predicted chassis, at the answered tick** (ADR
    // 0213), from the same state.
    if (world.vehicleControllers().size() != 0) {
        std::map<u64, u32> netOf;
        for (const auto& [netId, local] : m_locals)
            netOf.emplace(static_cast<u64>(local.index) | (static_cast<u64>(local.generation) << 32), netId);
        const auto heldOf = [&](core::InstanceId id) -> const EntityState* {
            const auto found = netOf.find(static_cast<u64>(id.index) | (static_cast<u64>(id.generation) << 32));
            return found != netOf.end() ? findEntity(state, found->second) : nullptr;
        };
        world.vehicleControllers().forEach([&](core::InstanceId id, const scene::VehicleControllerComponent&) {
            const scene::RigidBodyComponent* chassis = world.rigidBodies().find(world.parentOf(id));
            const EntityState* held = chassis != nullptr && chassis->predicted ? heldOf(id) : nullptr;
            if (held == nullptr)
                return;
            scene::CharacterReplayStart::Vehicle vehicle;
            vehicle.controller = id;
            const generated::ClassDesc& desc = generated::Classes[held->schema];
            for (usize at = 0; at < held->fields.size(); ++at) {
                const generated::FieldDesc* field = fieldAt(desc, at);
                if (field == nullptr || field->pool != "vehicleControllers")
                    continue;
                if (field->name == "RPM")
                    vehicle.rpm = asF32(held->fields[at]);
                else if (field->name == "CurrentGear")
                    vehicle.gear = asI32(held->fields[at]);
                else if (field->name == "Clutch")
                    vehicle.clutch = asF32(held->fields[at]);
                else if (field->name == "GearSwitchTimeLeft")
                    vehicle.gearSwitchTimeLeft = asF32(held->fields[at]);
                else if (field->name == "ClutchReleaseTimeLeft")
                    vehicle.clutchReleaseTimeLeft = asF32(held->fields[at]);
                else if (field->name == "GearSwitchLatencyTimeLeft")
                    vehicle.gearSwitchLatencyTimeLeft = asF32(held->fields[at]);
            }
            for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
                const EntityState* wheelHeld = world.vehicleWheels().find(child) != nullptr ? heldOf(child) : nullptr;
                if (wheelHeld == nullptr)
                    continue;
                scene::CharacterReplayStart::Vehicle::Wheel wheel;
                wheel.id = child;
                const generated::ClassDesc& wheelDesc = generated::Classes[wheelHeld->schema];
                for (usize at = 0; at < wheelHeld->fields.size(); ++at) {
                    const generated::FieldDesc* field = fieldAt(wheelDesc, at);
                    if (field == nullptr || field->pool != "vehicleWheels")
                        continue;
                    if (field->name == "AngularVelocity")
                        wheel.angularVelocity = asF32(wheelHeld->fields[at]);
                    else if (field->name == "Rotation")
                        wheel.rotation = asF32(wheelHeld->fields[at]);
                    else if (field->name == "SteerAngle")
                        wheel.steerAngle = asF32(wheelHeld->fields[at]);
                }
                vehicle.wheels.push_back(wheel);
            }
            all.front().start.vehicles.push_back(std::move(vehicle));
        });
    }
    // **Corrected at once, drawn sliding**:
    // what the correction moved is kept as an offset the drawing decays over a
    // tenth of a second, unless it is a teleport. **A character each** (ADR
    // 0208): the one that was right was put back where it was, and nothing
    // of it slides.
    std::array<core::DVec3, 4> before{};
    for (const Answer& each : all) {
        if (const scene::PartComponent* drawn = world.parts().find(each.character); drawn != nullptr)
            before[each.seat] = drawn->cframe.position;
    }
    // **And the vehicle each drives** (ADR 0213): its chassis is stepped
    // again with the island, and a correction moves it as it moves the
    // character -- drawn sliding the same way, the model it is in with it,
    // so a cabin welded on and its wheels go where the chassis goes.
    std::array<DrivenVehicle, 4> driven{};
    std::array<core::DVec3, 4> vehicleBefore{};
    for (const Answer& each : all) {
        driven[each.seat] = drivenBy(world, each.character);
        if (const scene::PartComponent* chassis = world.parts().find(driven[each.seat].chassis); chassis != nullptr)
            vehicleBefore[each.seat] = chassis->cframe.position;
    }
    const u64 counted = m_stats.corrections;
    const std::array<u64, 4> seatCounted = m_stats.seatCorrections;
    reconcileSeats(world, all);
    f64 furthest = 0.0;
    for (const Answer& each : all) {
        const scene::PartComponent* drawn = world.parts().find(each.character);
        if (drawn == nullptr)
            continue;
        Owned& seat = m_seats[each.seat];
        const core::DVec3 moved = before[each.seat] - drawn->cframe.position;
        const f64 distance = std::sqrt(moved.x * moved.x + moved.y * moved.y + moved.z * moved.z);
        furthest = std::max(furthest, distance);
        if (seat.visualCharacter != each.character)
            seat.visualOffset = core::DVec3{};
        seat.visualCharacter = each.character;
        seat.visualOffset = seat.visualOffset + moved;
        seat.visualFresh = distance > 0.0;
        const core::DVec3& o = seat.visualOffset;
        if (std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z) > VisualSnapMetres)
            seat.visualOffset = core::DVec3{};
        else
            seat.displaced = seat.displaced - moved;
        // How far this character was moved, and nothing of what it pushes:
        // that is the machine's, below.
        if (m_stats.seatCorrections[each.seat] != seatCounted[each.seat])
            m_stats.seatLastCorrectionMetres[each.seat] = distance;

        const DrivenVehicle& vehicle = driven[each.seat];
        const scene::PartComponent* chassis = world.parts().find(vehicle.chassis);
        if (chassis == nullptr) {
            seat.visualVehicle = core::InstanceId{};
            seat.vehicleOffset = core::DVec3{};
            continue;
        }
        const core::DVec3 carried = vehicleBefore[each.seat] - chassis->cframe.position;
        if (seat.visualVehicle != vehicle.root)
            seat.vehicleOffset = core::DVec3{};
        seat.visualVehicle = vehicle.root;
        seat.vehicleOffset = seat.vehicleOffset + carried;
        seat.vehicleFresh = carried.x != 0.0 || carried.y != 0.0 || carried.z != 0.0;
        const core::DVec3& v = seat.vehicleOffset;
        if (std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) > VisualSnapMetres)
            seat.vehicleOffset = core::DVec3{};
        else
            seat.vehicleDisplaced = seat.vehicleDisplaced - carried;
    }
    if (m_stats.corrections != counted)
        m_stats.lastCorrectionMetres = std::max(furthest, m_lastBodyCorrection);
    m_reconciledAck = m_ackedIntent;
}

} // namespace engine::replication
