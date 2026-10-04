// One field's value, its bytes, and the two ways that can be silently wrong
// (ADR 0069).
#include <array>
#include <cmath>
#include <cstddef>
#include <doctest/doctest.h>
#include <iterator>
#include <ostream>

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
    // did `Swarm` and the channel its agents' positions ride (ADR 0162).
    CHECK(std::size(generated::Classes) == 46);
    CHECK(std::size(generated::Channels) == 6);

    // Channel 3 was claimed from protocol 1 so the numbering could not shift
    // when ownership arrived (ADR 0099).
    CHECK(generated::Channels[3].name == "Ownership");
}
