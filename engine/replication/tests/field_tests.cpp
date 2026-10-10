// One field's value, its bytes, and the two ways that can be silently wrong
// (ADR 0069).
#include <array>
#include <cmath>
#include <cstddef>
#include <doctest/doctest.h>
#include <iterator>
#include <ostream>
#include <vector>

#include "engine/replication/field.h"
#include "wire_schema.gen.h"

using namespace engine;
using namespace engine::replication;

TEST_CASE("a cell is cleared before it is written")
{
    // **The defect this exists for is invisible and permanent.** A cell is
    // compared as bytes -- that is what makes a diff one `memcmp` per field --
    // so a narrow value written over a wide one leaves the wide one's tail
    // behind. Two equal `Vec3`s would then compare unequal, and the field would
    // be sent every tick for the life of the connection, with nothing anywhere
    // reporting a fault.
    FieldValue wide;
    core::CFrameD frame;
    frame.position = {1234.5, -678.25, 90.125};
    frame.rotation.m[0][1] = 0.5f;
    setCFrame(wide, frame);

    FieldValue narrow = wide;
    setVec3(narrow, core::Vec3{1.0f, 2.0f, 3.0f});

    FieldValue fresh;
    setVec3(fresh, core::Vec3{1.0f, 2.0f, 3.0f});

    CHECK(narrow == fresh);
}

TEST_CASE("every encoding round-trips through the wire")
{
    struct Case
    {
        generated::Encoding encoding;
        FieldValue value;
    };

    std::vector<Case> cases;

    FieldValue flag;
    setBool(flag, true);
    cases.push_back({generated::Encoding::Bool, flag});

    FieldValue number;
    setU32(number, 0xDEADBEEFu);
    cases.push_back({generated::Encoding::U32, number});

    FieldValue single;
    setF32(single, -1234.5f);
    cases.push_back({generated::Encoding::F32, single});

    FieldValue vector;
    setVec3(vector, core::Vec3{1.5f, -2.25f, 3.75f});
    cases.push_back({generated::Encoding::Vector3, vector});

    FieldValue position;
    setPosition(position, core::DVec3{1e6, -2.5e-3, 4096.0});
    cases.push_back({generated::Encoding::Position, position});

    FieldValue frame;
    core::CFrameD cf;
    cf.position = {-4096.5, 12.25, 7.125};
    cf.rotation.m[0][0] = 0.0f;
    cf.rotation.m[0][1] = 1.0f;
    cf.rotation.m[1][0] = -1.0f;
    cf.rotation.m[1][1] = 0.0f;
    setCFrame(frame, cf);
    cases.push_back({generated::Encoding::CFrameD, frame});

    for (const Case& one : cases) {
        std::vector<core::u8> bytes;
        encodeField(bytes, one.encoding, one.value);
        CHECK(bytes.size() == wireBytes(one.encoding));

        core::usize at = 0;
        FieldValue back;
        REQUIRE(decodeField(bytes, at, one.encoding, back));
        CHECK(at == bytes.size());
        // **Byte-identical, not merely equal in value.** The baseline the next
        // tick diffs against is this cell, so a decode that produced the right
        // number in a differently-padded cell would report the field changed on
        // every subsequent tick.
        CHECK(back == one.value);
    }
}

TEST_CASE("a truncated field is refused rather than read past")
{
    FieldValue frame;
    core::CFrameD cf;
    cf.position = {1.0, 2.0, 3.0};
    setCFrame(frame, cf);

    std::vector<core::u8> bytes;
    encodeField(bytes, generated::Encoding::CFrameD, frame);
    REQUIRE(bytes.size() > 8);

    for (core::usize length = 0; length < bytes.size(); ++length) {
        std::vector<core::u8> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
        core::usize at = 0;
        FieldValue back;
        CAPTURE(length);
        CHECK_FALSE(decodeField(cut, at, generated::Encoding::CFrameD, back));
        // And the cursor did not move, so a caller looping over fields cannot
        // be walked off its own buffer by a short one.
        CHECK(at == 0);
    }
}

TEST_CASE("the wire is smaller than the cell it came out of")
{
    // Not a micro-optimisation -- the whole point of a per-tick diff is that it
    // is small, and a cell is padded to sixty-four bytes so it can be compared
    // as bytes. A `Bool` costing sixty-four on the wire would make a snapshot
    // of a hundred parts sixty times what it should be.
    CHECK(wireBytes(generated::Encoding::Bool) == 1);
    CHECK(wireBytes(generated::Encoding::F32) == 4);
    CHECK(wireBytes(generated::Encoding::Vector3) == 12);
    // A position and a rotation in eight bytes (protocol 40): it was sixty.
    CHECK(wireBytes(generated::Encoding::CFrameD) == 32);
    CHECK(wireBytes(generated::Encoding::CFrameD) < FieldValue::Bytes);
}

TEST_CASE("a rotation crosses in eight bytes and comes back the rotation it was (protocol 40)")
{
    // The three smallest components of its quaternion at twenty bits each,
    // and which the fourth is: nine floats were thirty-six bytes of every
    // record of everything that moved.
    const auto roundTrip = [](const core::Mat3& rotation) {
        core::CFrameD frame;
        frame.position = {12.5, -3.0, 4096.25};
        frame.rotation = rotation;
        FieldValue cell;
        setCFrame(cell, frame);
        std::vector<core::u8> bytes;
        encodeField(bytes, generated::Encoding::CFrameD, cell);
        CHECK(bytes.size() == 32u);
        core::usize at = 0;
        FieldValue back;
        REQUIRE(decodeField(bytes, at, generated::Encoding::CFrameD, back));
        CHECK(back == cell);
        const core::CFrameD read = asCFrame(back);
        // The position is the position, to the bit.
        CHECK(read.position.x == frame.position.x);
        CHECK(read.position.z == frame.position.z);
        // **What is read back packs to the cell it came from**: an authority
        // that applies a client's part and captures it again says nothing
        // changed.
        FieldValue again;
        setCFrame(again, read);
        CHECK(again == cell);
        return read.rotation;
    };

    // No turn at all is no turn at all, exactly: most of what is in a world.
    const core::Mat3 still = roundTrip(core::Mat3{});
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row)
            CHECK(still.m[column][row] == (column == row ? 1.0f : 0.0f));
    }
    // Anything else, to a few millionths.
    const std::array<core::Mat3, 5> turns{
        core::rotationY(1.5707964f),
        core::rotationX(3.1415927f),
        core::fromEulerYxz(core::Vec3{0.3f, -2.1f, 1.2f}),
        core::fromAxisAngle(core::Vec3{0.57735f, 0.57735f, 0.57735f}, 2.0943952f),
        core::fromEulerYxz(core::Vec3{-1.4f, 0.01f, 3.0f}),
    };
    for (const core::Mat3& turn : turns) {
        const core::Mat3 read = roundTrip(turn);
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                CAPTURE(column);
                CAPTURE(row);
                CHECK(std::abs(read.m[column][row] - turn.m[column][row]) < 6e-6f);
            }
        }
    }
}

TEST_CASE("every encoding is the size the published protocol says it is (ADR 0100)")
{
    // `docs/protocol/wire.md` states these sizes from the same table; a
    // decoder written from the page must read what this encoder writes.
    for (std::size_t at = 0; at < std::size(generated::EncodingBytes); ++at) {
        const auto encoding = static_cast<generated::Encoding>(at);
        CAPTURE(at);
        CHECK(wireBytes(encoding) == generated::EncodingBytes[at]);
    }
}

TEST_CASE("the generated schema is what the module was built against")
{
    // A cheap tripwire on a real hazard: the header is checked in and
    // regenerated by a gate, so a build that picked up a stale copy would
    // compile and then disagree with its peer about what field three is.
    CHECK(generated::ProtocolVersion >= 1);
    CHECK(std::size(generated::CommonFields) == 2);
    // BasePart, CharacterBody, Model, Lighting, Decal, ParticleEmitter, Folder,
    // RemoteEvent, ReplicatedStorage, RemoteFunction and Part2D (protocol 11),
    // ADR 0096's five effects, `Atmosphere` and `Sky` (protocol 13), and
    // `Tilemap2D` (protocol 15), and `Workspace` for its wind (protocol 19),
    // and `ClickDetector` (protocol 21), and `Water`, `WaterWave` and
    // `WaterPoint` (protocol 28), and `Part` for its shape and `MeshPart` for
    // its mesh (protocol 30), and the joints -- `Attachment`, `Constraint` and
    // the seven kinds and six movers that extend it, `Weld`, `WeldConstraint`
    // and `NoCollisionConstraint` (protocol 34, NA34). Teams, prompts and the
    // world's drags left with protocol 35. `UnreliableRemoteEvent`, and the
    // channel its messages ride, came with protocol 38 (ADR 0161) -- and so
    // did `Swarm` and the channel its agents' positions ride (ADR 0162). And
    // what a character carries (protocol 44): `PointLight`, `SpotLight`,
    // `SpringBone`, `SpringCollider`, `Bone`, `Highlight`, `Beam`, `Trail`
    // and `Sound`.
    CHECK(std::size(generated::Classes) == 58);
    CHECK(std::size(generated::Channels) == 6);

    // Channel 3 was claimed from protocol 1 so the numbering could not shift
    // when ownership arrived (ADR 0099).
    CHECK(generated::Channels[3].name == "Ownership");
}

// --- Sequences (protocol 44) -------------------------------------------------------------

namespace {

// A sequence's cells, as a set would hold them: the first, and its further ones.
struct SequenceCells
{
    FieldValue first;
    std::vector<FieldValue> further;

    explicit SequenceCells(generated::Encoding encoding) : further(furtherCellsOf(encoding)) {}

    [[nodiscard]] bool operator==(const SequenceCells& other) const noexcept
    {
        return first == other.first && further == other.further;
    }
};

[[nodiscard]] core::ColorSequence rainbow(core::usize keys)
{
    core::ColorSequence sequence;
    sequence.keypoints.clear();
    for (core::usize key = 0; key < keys; ++key) {
        const auto along = static_cast<core::f32>(key) / static_cast<core::f32>(keys - 1);
        sequence.keypoints.push_back(core::ColorKeypoint{along, core::Color3{along, 1.0f - along, 0.25f}});
    }
    return sequence;
}

} // namespace

TEST_CASE("a sequence crosses as a count and its keys, and comes back the bytes it was (protocol 44)")
{
    // The one value longer than a cell: three cells past the first for
    // numbers, five for colours, which is twenty keys of either.
    CHECK(isSequence(generated::Encoding::ColorSequence));
    CHECK(isSequence(generated::Encoding::NumberSequence));
    CHECK_FALSE(isSequence(generated::Encoding::Color3));
    CHECK(furtherCellsOf(generated::Encoding::ColorSequence) == 5);
    CHECK(furtherCellsOf(generated::Encoding::NumberSequence) == 3);
    CHECK(furtherCellsOf(generated::Encoding::CFrameD) == 0);

    for (const core::usize keys : {core::usize{2}, core::usize{3}, core::usize{4}, core::MaxSequenceKeypoints}) {
        CAPTURE(keys);
        const core::ColorSequence colours = rainbow(keys);
        SequenceCells cells(generated::Encoding::ColorSequence);
        setColorSequence(cells.first, cells.further, colours);

        std::vector<core::u8> bytes;
        encodeSequence(bytes, generated::Encoding::ColorSequence, cells.first, cells.further);
        // A count and that many keys, and nothing for the keys it has not.
        CHECK(bytes.size() == 1 + keys * ColorKeyBytes);
        CHECK(bytes.size() <= wireBytes(generated::Encoding::ColorSequence));
        CHECK(bytes[0] == keys);

        // Into cells that held something else: every cell is cleared first,
        // so two ends that hold one sequence hold the same bytes.
        SequenceCells back(generated::Encoding::ColorSequence);
        back.first.raw.fill(0xAB);
        for (FieldValue& cell : back.further)
            cell.raw.fill(0xCD);
        core::usize at = 0;
        REQUIRE(decodeSequence(bytes, at, generated::Encoding::ColorSequence, back.first, back.further));
        CHECK(at == bytes.size());
        CHECK(back == cells);

        core::ColorSequence read;
        REQUIRE(asColorSequence(back.first, back.further, read));
        CHECK(read == colours);
    }

    const core::NumberSequence numbers{{core::NumberKeypoint{0.0f, 0.5f, 0.0f},
                                        core::NumberKeypoint{0.75f, 2.0f, 0.125f},
                                        core::NumberKeypoint{1.0f, 0.25f, 0.0f}}};
    SequenceCells cells(generated::Encoding::NumberSequence);
    setNumberSequence(cells.first, cells.further, numbers);
    std::vector<core::u8> bytes;
    encodeSequence(bytes, generated::Encoding::NumberSequence, cells.first, cells.further);
    CHECK(bytes.size() == 1 + 3 * NumberKeyBytes);
    SequenceCells back(generated::Encoding::NumberSequence);
    core::usize at = 0;
    REQUIRE(decodeSequence(bytes, at, generated::Encoding::NumberSequence, back.first, back.further));
    CHECK(back == cells);
    core::NumberSequence read;
    REQUIRE(asNumberSequence(back.first, back.further, read));
    CHECK(read == numbers);

    // A value written over a longer one leaves none of it behind.
    setColorSequence(cells.first, cells.further, rainbow(2));
    SequenceCells fresh(generated::Encoding::NumberSequence);
    setColorSequence(fresh.first, fresh.further, rainbow(2));
    CHECK(cells == fresh);
}

TEST_CASE("a sequence longer than one may be is refused, and a list that is no sequence is not written")
{
    SequenceCells cells(generated::Encoding::ColorSequence);
    setColorSequence(cells.first, cells.further, rainbow(core::MaxSequenceKeypoints));
    std::vector<core::u8> bytes;
    encodeSequence(bytes, generated::Encoding::ColorSequence, cells.first, cells.further);
    REQUIRE(bytes.size() == wireBytes(generated::Encoding::ColorSequence));

    // **The count is the peer's.** One past what a sequence may hold, with
    // every byte it promises behind it: refused before a key is read, the
    // cursor where it was and the cells as they were.
    std::vector<core::u8> tooMany = bytes;
    tooMany[0] = static_cast<core::u8>(core::MaxSequenceKeypoints + 1);
    tooMany.resize(1 + (core::MaxSequenceKeypoints + 1) * ColorKeyBytes, 0);
    SequenceCells back(generated::Encoding::ColorSequence);
    const SequenceCells untouched = back;
    core::usize at = 0;
    CHECK_FALSE(decodeSequence(tooMany, at, generated::Encoding::ColorSequence, back.first, back.further));
    CHECK(at == 0);
    CHECK(back == untouched);
    tooMany[0] = 255;
    tooMany.resize(1 + 255 * ColorKeyBytes, 0);
    CHECK_FALSE(decodeSequence(tooMany, at, generated::Encoding::ColorSequence, back.first, back.further));
    CHECK(at == 0);

    // Fewer bytes than the count says, at every length.
    for (core::usize length = 0; length < bytes.size(); ++length) {
        const std::vector<core::u8> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
        CAPTURE(length);
        CHECK_FALSE(decodeSequence(cut, at, generated::Encoding::ColorSequence, back.first, back.further));
        CHECK(at == 0);
    }

    // Cells that are not the encoding's -- too few to hold twenty keys -- are
    // never written past.
    std::vector<FieldValue> few(2);
    CHECK_FALSE(decodeSequence(bytes, at, generated::Encoding::ColorSequence, back.first, few));
    CHECK_FALSE(decodeSequence(bytes, at, generated::Encoding::Color3, back.first, back.further));
    CHECK(at == 0);

    // **Bytes that decode and are no sequence**: one key, a first key not at
    // 0, a time that falls, a number that is not one. Kept as sent -- the two
    // ends must hold the same bytes -- and never handed on as a sequence.
    core::ColorSequence held = rainbow(3);
    const core::ColorSequence before = held;
    const auto refused = [&](const std::vector<core::ColorKeypoint>& keys) {
        std::vector<core::u8> wire;
        wire.push_back(static_cast<core::u8>(keys.size()));
        for (const core::ColorKeypoint& key : keys) {
            const auto* raw = reinterpret_cast<const core::u8*>(&key);
            wire.insert(wire.end(), raw, raw + sizeof(key));
        }
        core::usize from = 0;
        SequenceCells into(generated::Encoding::ColorSequence);
        REQUIRE(decodeSequence(wire, from, generated::Encoding::ColorSequence, into.first, into.further));
        return !asColorSequence(into.first, into.further, held) && held == before;
    };
    const core::Color3 white{1.0f, 1.0f, 1.0f};
    CHECK(refused({}));
    CHECK(refused({core::ColorKeypoint{0.0f, white}}));
    CHECK(refused({core::ColorKeypoint{0.25f, white}, core::ColorKeypoint{1.0f, white}}));
    CHECK(refused({core::ColorKeypoint{0.0f, white}, core::ColorKeypoint{0.75f, white}}));
    CHECK(refused({core::ColorKeypoint{0.0f, white}, core::ColorKeypoint{0.75f, white},
                   core::ColorKeypoint{0.5f, white}, core::ColorKeypoint{1.0f, white}}));
    CHECK(refused(
        {core::ColorKeypoint{0.0f, white}, core::ColorKeypoint{1.0f, core::Color3{std::nanf(""), 1.0f, 1.0f}}}));

    // **And one a setter would never have let through**: twenty-one keys in a
    // component written by hand are cut to twenty in its cells.
    core::ColorSequence tooLong = rainbow(core::MaxSequenceKeypoints + 1);
    setColorSequence(cells.first, cells.further, tooLong);
    CHECK(cells.first.raw[0] == core::MaxSequenceKeypoints);

    // A field's own encoder and decoder do not take one: it is not one cell.
    std::vector<core::u8> none;
    encodeField(none, generated::Encoding::ColorSequence, cells.first);
    CHECK(none.empty());
    FieldValue single;
    CHECK_FALSE(decodeField(bytes, at, generated::Encoding::ColorSequence, single));
}
