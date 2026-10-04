#include "engine/replication/field.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "wire_schema.gen.h"

namespace engine::replication {

// **The wire layout is the C++ layout, and these hold that true.**
//
// `encodeField` copies a cell's first `wireBytes` verbatim rather than
// switching on the encoding and writing member by member, which is right only
// while every type it names is tightly packed and starts at offset zero. A
// padding byte appearing in one of them would put garbage on the wire and read
// it back as a number, silently, on one compiler and not another -- so it is a
// build failure here instead.
static_assert(sizeof(core::Vec3) == 12, "Vec3 must be three tightly packed f32");
static_assert(sizeof(core::DVec3) == 24, "DVec3 must be three tightly packed f64");
static_assert(sizeof(core::Mat3) == 36, "Mat3 must be nine tightly packed f32");
static_assert(offsetof(core::CFrameD, position) == 0, "a CFrameD's position must lead");
static_assert(offsetof(core::CFrameD, rotation) == 24, "a CFrameD's rotation must follow its position");

namespace {

using core::u8;
using core::usize;

// **Clear, then write.** A cell is compared as bytes, so a `Vec3` written over a
// `CFrameD` would leave the rest of the old value behind and two equal Vec3s
// would compare unequal -- a field that never changed, sent every tick, for
// ever. It is one `memset` and it is the reason this file has a helper rather
// than eleven `std::memcpy` calls.
template <class T>
void store(FieldValue& out, const T& value) noexcept
{
    static_assert(sizeof(T) <= FieldValue::Bytes);
    out.raw.fill(0);
    std::memcpy(out.raw.data(), &value, sizeof(T));
}

template <class T>
[[nodiscard]] T load(const FieldValue& value) noexcept
{
    static_assert(sizeof(T) <= FieldValue::Bytes);
    T result{};
    std::memcpy(&result, value.raw.data(), sizeof(T));
    return result;
}

void putBytes(std::vector<u8>& out, const void* source, usize count)
{
    const auto* bytes = static_cast<const u8*>(source);
    out.insert(out.end(), bytes, bytes + count);
}

} // namespace

void setBool(FieldValue& out, bool value) noexcept
{
    const u8 byte = value ? 1u : 0u;
    store(out, byte);
}

void setU8(FieldValue& out, core::u8 value) noexcept
{
    store(out, value);
}

void setU16(FieldValue& out, core::u16 value) noexcept
{
    store(out, value);
}

void setU32(FieldValue& out, core::u32 value) noexcept
{
    store(out, value);
}

void setI32(FieldValue& out, core::i32 value) noexcept
{
    store(out, value);
}

void setF32(FieldValue& out, float value) noexcept
{
    store(out, value);
}

void setF64(FieldValue& out, double value) noexcept
{
    store(out, value);
}

void setVec3(FieldValue& out, core::Vec3 value) noexcept
{
    store(out, value);
}

void setPosition(FieldValue& out, core::DVec3 value) noexcept
{
    store(out, value);
}

void setCFrame(FieldValue& out, const core::CFrameD& value) noexcept
{
    // **Written member by member rather than as one struct**, and this is the
    // one type that needs it.
    //
    // `sizeof(CFrameD)` is 64: twenty-four bytes of position, thirty-six of
    // rotation, and four of padding the compiler inserts to align the whole to
    // eight. A `memcpy` of the struct copies that padding out of an
    // uninitialised source, so two CFrames with identical values land in cells
    // that differ in four bytes -- and a cell is compared as bytes, which is
    // what makes the diff cheap. The field would be sent every tick, for the
    // life of the connection, with nothing anywhere reporting a fault.
    //
    // Caught by a round-trip test: the wire carries sixty bytes, the decoder
    // zeroes the rest, and the result did not equal what went in.
    //
    // **And the rotation is held as it crosses** (protocol 40): packed. A
    // cell is what is compared, checksummed and sent, so both ends hold the
    // same eight bytes and each makes its own matrix of them -- no machine's
    // arithmetic is in what the checksum covers.
    out.raw.fill(0);
    std::memcpy(out.raw.data(), &value.position, sizeof(value.position));
    const core::u64 packed = packRotation(value.rotation);
    std::memcpy(out.raw.data() + sizeof(value.position), &packed, sizeof(packed));
}

namespace {

// A quaternion's component left out is the largest; the others are within
// this of zero either way.
constexpr double RotationReach = 0.70710678118654752440;
// An even count of steps, so that zero is a step: no turn is exactly no turn.
constexpr double RotationSteps = 1048574.0;
constexpr core::u64 RotationMask = 0xFFFFFu;

} // namespace

core::u64 packRotation(const core::Mat3& rotation) noexcept
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
    core::toQuaternion(rotation, x, y, z, w);
    double q[4] = {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), static_cast<double>(w)};
    const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    // Not a rotation -- a matrix of zeros, a NaN -- is no turn.
    if (!(length > 1e-12) || !std::isfinite(length)) {
        q[0] = q[1] = q[2] = 0.0;
        q[3] = 1.0;
    }
    else {
        for (double& component : q)
            component /= length;
    }
    // **The largest, and `w` when it is as large as any**: two components the
    // same size to the last bits must not take turns at being left out, or a
    // rotation that did not change would read as one that had.
    int largest = 3;
    for (int at = 0; at < 3; ++at) {
        if (std::fabs(q[at]) > std::fabs(q[largest]) + 1e-4)
            largest = at;
    }
    // A quaternion and its negative are one rotation: the one whose left-out
    // component is positive is the one sent.
    const double sign = q[largest] < 0.0 ? -1.0 : 1.0;
    core::u64 packed = static_cast<core::u64>(largest);
    int shift = 2;
    for (int at = 0; at < 4; ++at) {
        if (at == largest)
            continue;
        const double unit = std::clamp(sign * q[at] / RotationReach * 0.5 + 0.5, 0.0, 1.0);
        packed |= (static_cast<core::u64>(std::llround(unit * RotationSteps)) & RotationMask) << shift;
        shift += 20;
    }
    return packed;
}

core::Mat3 unpackRotation(core::u64 packed) noexcept
{
    const int largest = static_cast<int>(packed & 3u);
    double q[4] = {0.0, 0.0, 0.0, 0.0};
    double sum = 0.0;
    int shift = 2;
    for (int at = 0; at < 4; ++at) {
        if (at == largest)
            continue;
        const double unit = static_cast<double>((packed >> shift) & RotationMask) / RotationSteps;
        q[at] = (unit - 0.5) * 2.0 * RotationReach;
        sum += q[at] * q[at];
        shift += 20;
    }
    q[largest] = std::sqrt(std::max(0.0, 1.0 - sum));
    return core::fromQuaternion(static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]),
                                static_cast<float>(q[3]));
}

bool sameRotation(const FieldValue& a, const FieldValue& b) noexcept
{
    return std::memcmp(a.raw.data() + CFramePositionBytes, b.raw.data() + CFramePositionBytes,
                       FieldValue::Bytes - CFramePositionBytes) == 0;
}

void encodePosition(std::vector<core::u8>& out, const FieldValue& value)
{
    out.insert(out.end(), value.raw.begin(), value.raw.begin() + static_cast<std::ptrdiff_t>(CFramePositionBytes));
}

bool decodePosition(std::span<const core::u8> bytes, core::usize& at, FieldValue& out) noexcept
{
    if (at + CFramePositionBytes > bytes.size())
        return false;
    std::memcpy(out.raw.data(), bytes.data() + at, CFramePositionBytes);
    at += CFramePositionBytes;
    return true;
}

void setNetId(FieldValue& out, NetId value) noexcept
{
    store(out, value.value);
}

void setInstance(FieldValue& out, core::InstanceId value) noexcept
{
    store(out, value);
}

bool asBool(const FieldValue& value) noexcept
{
    return load<u8>(value) != 0;
}

core::u32 asU32(const FieldValue& value) noexcept
{
    return load<core::u32>(value);
}

core::i32 asI32(const FieldValue& value) noexcept
{
    return load<core::i32>(value);
}

float asF32(const FieldValue& value) noexcept
{
    return load<float>(value);
}

core::Vec3 asVec3(const FieldValue& value) noexcept
{
    return load<core::Vec3>(value);
}

core::CFrameD asCFrame(const FieldValue& value) noexcept
{
    // The mirror of `setCFrame`: read the two members, never the struct, so a
    // cell whose padding bytes are zero produces the same object either way.
    core::CFrameD result;
    std::memcpy(&result.position, value.raw.data(), sizeof(result.position));
    core::u64 packed = 0;
    std::memcpy(&packed, value.raw.data() + sizeof(result.position), sizeof(packed));
    result.rotation = unpackRotation(packed);
    return result;
}

NetId asNetId(const FieldValue& value) noexcept
{
    return NetId{load<core::u32>(value)};
}

core::InstanceId asInstance(const FieldValue& value) noexcept
{
    return load<core::InstanceId>(value);
}

core::DVec3 asPosition(const FieldValue& value) noexcept
{
    return load<core::DVec3>(value);
}

usize wireBytes(generated::Encoding encoding) noexcept
{
    switch (encoding) {
    case generated::Encoding::Bool:
    case generated::Encoding::U8:
        return 1;
    case generated::Encoding::U16:
        return 2;
    case generated::Encoding::U32:
    case generated::Encoding::I32:
    case generated::Encoding::F32:
    case generated::Encoding::NameAtom:
    case generated::Encoding::InstanceRef:
        return 4;
    case generated::Encoding::F64:
        return 8;
    case generated::Encoding::Vector3:
    case generated::Encoding::Color3:
        return 12;
    case generated::Encoding::Position:
        return 24;
    // ADR 0090. Fixed-size like every other encoding here, laid out by
    // `extract.cpp`'s packers: a mask, then the values it may select.
    case generated::Encoding::MaterialOverrides:
        return MaterialOverridesBytes;
    case generated::Encoding::MaterialValues:
        return MaterialValuesBytes;
    case generated::Encoding::CFrameD:
        // Three f64 of position, and the rotation packed in a u64.
        //
        // **The rotation is quantised from protocol 40, and the position is
        // not.** Nine floats were thirty-six of a moving body's sixty bytes
        // in every snapshot; the three smallest components of its quaternion
        // at twenty bits each are eight, and right to about three millionths
        // of a radian -- a third of a millimetre at the end of a hundred
        // metre beam. What was held against it at protocol 1 was an error
        // budget nobody had measured against a physics mirror that reads the
        // result back: it is measured now (`netcode_acceptance`, and the
        // predicted-parts tests, which pass with it), and a replica's own
        // simulation is corrected by position at a centimetre. A position
        // quantised to the millimetre would be inside what a stopped
        // character is held to, so it is not.
        return 24 + 8;
    }
    return 0;
}

// **The wire is little-endian, and the copy below is the host's bytes.** A
// big-endian target would speak another protocol, and must fail here rather
// than at a peer.
static_assert(std::endian::native == std::endian::little, "the wire's field encoding assumes a little-endian host");

void encodeField(std::vector<u8>& out, generated::Encoding encoding, const FieldValue& value)
{
    // **The cell's first `wireBytes` are the value**, because every setter
    // stores at offset zero and every encoding is its own C++ type laid out
    // little-endian. This is one `insert` rather than a switch with eleven arms
    // that would each say the same thing.
    const usize count = wireBytes(encoding);
    if (count == 0 || count > FieldValue::Bytes) {
        return;
    }
    putBytes(out, value.raw.data(), count);
}

bool decodeField(std::span<const u8> bytes, usize& at, generated::Encoding encoding, FieldValue& out) noexcept
{
    const usize count = wireBytes(encoding);
    if (count == 0 || count > FieldValue::Bytes || at + count > bytes.size()) {
        return false;
    }
    // Cleared first, for the reason `store` is: a decoded cell is compared
    // against a baseline cell, and leftover bytes would make two equal values
    // differ.
    out.raw.fill(0);
    std::memcpy(out.raw.data(), bytes.data() + at, count);
    at += count;
    return true;
}

} // namespace engine::replication
