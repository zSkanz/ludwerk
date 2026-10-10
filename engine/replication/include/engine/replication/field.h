// One replicated field's value, and the bytes it becomes (ADR 0069).
//
// **A fixed-size cell rather than a variant**, because a baseline is one of
// these per field per instance per peer, and the thing it is compared against a
// hundred times a second is its bytes. A variant would put a discriminant beside
// a value the schema already names the type of, and a heap allocation behind
// anything that did not fit inline.
#pragma once

#include <array>
#include <span>
#include <vector>

#include "engine/core/math.h"
#include "engine/core/sequence.h"
#include "engine/core/types.h"
#include "engine/replication/types.h"

namespace engine::replication {

namespace generated {
enum class Encoding : core::u8;
}

// The widest thing a field can hold is a `CFrameD`: three f64 of position and a
// 3x3 of rotation. Everything else fits in less and the cell is padded.
//
// **Compared as bytes and never as a type**, which is what makes `diff` one
// `memcmp` per field rather than a switch. The corollary is that every write
// into a cell has to clear it first: two `CFrameD`s that are equal must have
// equal padding, or a field that did not change would be sent every tick.
struct FieldValue
{
    static constexpr core::usize Bytes = 64;

    std::array<core::u8, Bytes> raw{};

    [[nodiscard]] bool operator==(const FieldValue& other) const noexcept { return raw == other.raw; }
};

// ADR 0090's two: a part's overrides (a u16 mask, two bytes of padding, then
// Color, Transparency, Emissive, Metalness, Roughness, NormalScale and
// AlphaCutoff as f32), and what a runtime clone changed (the same, with the
// mask covering every field and the alpha mode and sidedness as a byte each).
inline constexpr core::usize MaterialOverridesBytes = 4 + 4 * 11;
inline constexpr core::usize MaterialValuesBytes = MaterialOverridesBytes + 4;
static_assert(MaterialValuesBytes <= FieldValue::Bytes, "a clone's values must fit in a field cell");

static_assert(sizeof(core::DVec3) + sizeof(core::Mat3) <= FieldValue::Bytes,
              "a CFrameD must fit in a field cell, or the widest encoding has outgrown it");

// **Every setter clears the cell first.** See `FieldValue`: padding is compared.
void setBool(FieldValue& out, bool value) noexcept;
void setU8(FieldValue& out, core::u8 value) noexcept;
void setU16(FieldValue& out, core::u16 value) noexcept;
void setU32(FieldValue& out, core::u32 value) noexcept;
void setI32(FieldValue& out, core::i32 value) noexcept;
void setF32(FieldValue& out, float value) noexcept;
void setF64(FieldValue& out, double value) noexcept;
void setVec3(FieldValue& out, core::Vec3 value) noexcept;
void setPosition(FieldValue& out, core::DVec3 value) noexcept;
void setCFrame(FieldValue& out, const core::CFrameD& value) noexcept;
// **A rotation as it crosses** (protocol 40): the three smallest components
// of its quaternion at twenty bits each and which the fourth is, in a `u64`.
// What a `CFrameD` cell holds after its position, so two cells are the same
// rotation exactly when they are the same bytes.
[[nodiscard]] core::u64 packRotation(const core::Mat3& rotation) noexcept;
[[nodiscard]] core::Mat3 unpackRotation(core::u64 packed) noexcept;
// A `CFrameD` cell's position alone: how many bytes it is, whether two cells
// differ in nothing else, and its bytes onto a message and back into a cell
// whose rotation is kept.
inline constexpr core::usize CFramePositionBytes = 24;
[[nodiscard]] bool sameRotation(const FieldValue& a, const FieldValue& b) noexcept;
void encodePosition(std::vector<core::u8>& out, const FieldValue& value);
[[nodiscard]] bool decodePosition(std::span<const core::u8> bytes, core::usize& at, FieldValue& out) noexcept;
void setNetId(FieldValue& out, NetId value) noexcept;
// **An instance reference before it is a network id** (NA34): what a component
// holds, read whole. Only the session sees one -- it turns it into the peer's
// network id at capture, and a network id back into one at apply -- so it never
// reaches the wire.
void setInstance(FieldValue& out, core::InstanceId value) noexcept;

// --- Sequences (protocol 44) ------------------------------------------------------
//
// **The one value longer than a cell.** A sequence's bytes as they cross -- a
// count, then that many keys -- lie zero-padded over several cells: the first
// in the field's own place and the rest after the instance's last field
// (`furtherCells`, in `extract.h`). Cells, so that a diff, a checksum and a
// baseline go on reading bytes and know nothing of it; several, because
// widening every cell for the three classes that hold one would be paid by
// every field of every instance in every state.
//
// A colour key is its time and r, g, b; a number key its time, value and
// envelope: what an attribute of either type already is on the wire.
inline constexpr core::usize MaxSequenceKeys = core::MaxSequenceKeypoints;
inline constexpr core::usize ColorKeyBytes = 16;
inline constexpr core::usize NumberKeyBytes = 12;
inline constexpr core::usize ColorSequenceBytes = 1 + MaxSequenceKeys * ColorKeyBytes;
inline constexpr core::usize NumberSequenceBytes = 1 + MaxSequenceKeys * NumberKeyBytes;

// Whether an encoding is one of the two, and how many cells it takes beyond
// the first: none for every other.
[[nodiscard]] bool isSequence(generated::Encoding encoding) noexcept;
[[nodiscard]] core::usize furtherCellsOf(generated::Encoding encoding) noexcept;

// A sequence into its cells, every one cleared first. **No more than twenty
// keys are written**: a list longer than a sequence may be -- which no setter
// lets through -- is cut there, and reads back as no sequence on the other end.
void setColorSequence(FieldValue& first, std::span<FieldValue> further, const core::ColorSequence& value) noexcept;
void setNumberSequence(FieldValue& first, std::span<FieldValue> further, const core::NumberSequence& value) noexcept;

// And back. False, with `out` untouched, when the cells do not hold a
// sequence (`core::validSequence`): a peer is not trusted to have sent one.
[[nodiscard]] bool asColorSequence(const FieldValue& first, std::span<const FieldValue> further,
                                   core::ColorSequence& out);
[[nodiscard]] bool asNumberSequence(const FieldValue& first, std::span<const FieldValue> further,
                                    core::NumberSequence& out);

// A sequence's wire bytes: its count and that many keys, and nothing after.
void encodeSequence(std::vector<core::u8>& out, generated::Encoding encoding, const FieldValue& first,
                    std::span<const FieldValue> further);

// Reads one back into cleared cells. False, with the cursor where it was, on
// a count past twenty, on too few bytes, and on cells that are not the
// encoding's -- never a write past them.
[[nodiscard]] bool decodeSequence(std::span<const core::u8> bytes, core::usize& at, generated::Encoding encoding,
                                  FieldValue& first, std::span<FieldValue> further) noexcept;

[[nodiscard]] bool asBool(const FieldValue& value) noexcept;
[[nodiscard]] core::u32 asU32(const FieldValue& value) noexcept;
[[nodiscard]] core::i32 asI32(const FieldValue& value) noexcept;
[[nodiscard]] float asF32(const FieldValue& value) noexcept;
[[nodiscard]] core::Vec3 asVec3(const FieldValue& value) noexcept;
[[nodiscard]] core::CFrameD asCFrame(const FieldValue& value) noexcept;
[[nodiscard]] NetId asNetId(const FieldValue& value) noexcept;
[[nodiscard]] core::InstanceId asInstance(const FieldValue& value) noexcept;
[[nodiscard]] core::DVec3 asPosition(const FieldValue& value) noexcept;

// How many bytes an encoding occupies on the wire.
//
// **Not `sizeof(FieldValue)`.** The cell is padded so it can be compared as
// bytes; the wire carries exactly what the encoding needs, because the whole
// point of a per-tick diff is that it is small. For a sequence, the most it
// can be: it says its own length.
[[nodiscard]] core::usize wireBytes(generated::Encoding encoding) noexcept;

// Appends one field's wire bytes. Little-endian, fixed width, no framing of its
// own -- the message around it says which field this is. **Not a sequence**,
// which is more than one cell: `encodeSequence` writes it, and this writes
// nothing for one.
void encodeField(std::vector<core::u8>& out, generated::Encoding encoding, const FieldValue& value);

// Reads one back. Returns false when there are not enough bytes left, which is
// the only way a decoder may fail here: everything else about the layout is
// fixed by the encoding. False for a sequence too (`decodeSequence`).
[[nodiscard]] bool decodeField(std::span<const core::u8> bytes, core::usize& at, generated::Encoding encoding,
                               FieldValue& out) noexcept;

} // namespace engine::replication
