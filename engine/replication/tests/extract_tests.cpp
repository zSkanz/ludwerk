// Reading a world's replicated state and diffing it (ADR 0069).
//
// **No transport, no peers, no socket.** Everything here is a function of a
// world and a baseline, which is what lets the half that has to be right before
// anything is sent be tested in a process with no network in it.
#include <cstring>
#include <doctest/doctest.h>
#include <ostream>

#include "../../scene/tests/scene_fixture.h"
#include "engine/replication/extract.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"
#include "wire_schema.gen.h"

using namespace engine;
using namespace engine::replication;

namespace {

struct Rig
{
    scene::testing::Fixture fixture;

    [[nodiscard]] scene::World& world() { return fixture.world; }

    [[nodiscard]] core::InstanceId part(core::DVec3 at)
    {
        const core::InstanceId id = fixture.world.create(fixture.schema.partClass);
        REQUIRE(id.valid());
        scene::PartComponent* component = fixture.world.parts().find(id);
        REQUIRE(component != nullptr);
        component->cframe.position = at;
        return id;
    }
};

} // namespace

TEST_CASE("a Part's schema is its own, and carries BasePart's")
{
    // **A leaf needs a row only for what it adds.** `Part` adds its shape
    // (D424: a ball was a block on every replica), and everything else it
    // sends is `BasePart`'s, by `Extends` -- the schema does not repeat the
    // class hierarchy, and a class with nothing of its own to send still finds
    // its ancestor's by walking up.
    Rig rig;
    const core::InstanceId id = rig.part({1.0, 2.0, 3.0});

    const generated::ClassDesc* desc = schemaFor(rig.world(), id);
    REQUIRE(desc != nullptr);
    CHECK(desc->name == "Part");

    bool movement = false;
    bool shape = false;
    for (core::usize index = 0; fieldAt(*desc, index) != nullptr; ++index) {
        const generated::FieldDesc* field = fieldAt(*desc, index);
        movement = movement || field->name == "CFrame";
        shape = shape || field->name == "Shape";
    }
    CHECK(movement);
    CHECK(shape);
}

TEST_CASE("an instance with no schema replicates nothing")
{
    Rig rig;
    const core::InstanceId folder = rig.fixture.world.create(rig.fixture.schema.folderClass);
    REQUIRE(folder.valid());

    // `Folder` IS replicated -- with no fields of its own, which is the point:
    // a `Parent` reference to one has to resolve on the replica.
    const generated::ClassDesc* desc = schemaFor(rig.world(), folder);
    REQUIRE(desc != nullptr);
    CHECK(desc->name == "Folder");
    CHECK(desc->fields.empty());
    // But it still carries the common set.
    CHECK(fieldCount(*desc) == 2);
}

TEST_CASE("extracting a part reads every field it declares")
{
    Rig rig;
    const core::InstanceId id = rig.part({10.0, -4.0, 2.5});
    // `Anchored` and `CanCollide` live in `rigidBodies`, so a part without one
    // is not fully extractable -- which the case below this one is about.
    rig.world().rigidBodies().add(id, scene::RigidBodyComponent{});
    scene::PartComponent* part = rig.world().parts().find(id);
    REQUIRE(part != nullptr);
    part->size = {2.0f, 3.0f, 4.0f};
    asset::MaterialProperties faded;
    faded.transparency = 0.25f;
    (void)asset::setOverride(part->materialParameters, asset::MaterialField::Transparency, faded);

    const generated::ClassDesc* desc = schemaFor(rig.world(), id);
    REQUIRE(desc != nullptr);

    FieldSet fields;
    REQUIRE(extractFields(rig.world(), id, *desc, fields));
    CHECK(fields.size() == fieldCount(*desc));

    // The values are where the schema's order says they are.
    const core::usize common = std::size(generated::CommonFields);
    CHECK(asCFrame(fields[common + 0]).position.x == doctest::Approx(10.0));
    CHECK(static_cast<double>(asVec3(fields[common + 1]).y) == doctest::Approx(3.0));
    // `MaterialParameters` (ADR 0090), after Anchored, CanCollide and Material:
    // a u16 mask, then the values at fixed offsets, Transparency the fourth
    // float after the four-byte head.
    const FieldValue& parameters = fields[common + 5];
    core::u16 set = 0;
    std::memcpy(&set, parameters.raw.data(), sizeof(set));
    CHECK((set & asset::fieldBit(asset::MaterialField::Transparency)) != 0);
    float transparency = 0.0f;
    std::memcpy(&transparency, parameters.raw.data() + 16, sizeof(transparency));
    CHECK(static_cast<double>(transparency) == doctest::Approx(0.25));
}

TEST_CASE("an extraction that cannot read a field fails whole")
{
    // **Never partially fills**, and this is the reason: a half-read set diffed
    // against a baseline reports its unread half as changed, every tick, for
    // ever -- and nothing reports a fault while it does.
    //
    // A `Part` in this fixture has a `PartComponent` and no `RigidBodyComponent`
    // unless one is added, and `Anchored` lives in the latter. So the extraction
    // has to refuse rather than write a zero nobody notices.
    Rig rig;
    const core::InstanceId id = rig.part({0.0, 0.0, 0.0});
    const generated::ClassDesc* desc = schemaFor(rig.world(), id);
    REQUIRE(desc != nullptr);

    FieldSet fields;
    const bool complete = rig.world().rigidBodies().find(id) != nullptr;
    CHECK(extractFields(rig.world(), id, *desc, fields) == complete);
    if (!complete) {
        // And it left `out` alone rather than half-writing it.
        CHECK(fields.empty());
    }
}

TEST_CASE("a diff reports what changed and nothing else")
{
    Rig rig;
    const core::InstanceId id = rig.part({0.0, 0.0, 0.0});
    scene::RigidBodyComponent body;
    rig.world().rigidBodies().add(id, body);

    const generated::ClassDesc* desc = schemaFor(rig.world(), id);
    REQUIRE(desc != nullptr);

    FieldSet baseline;
    REQUIRE(extractFields(rig.world(), id, *desc, baseline));

    std::vector<FieldDelta> deltas;
    diffFields(*desc, baseline, baseline, deltas);
    CHECK(deltas.empty());

    // Move it, and exactly one field is different.
    scene::PartComponent* part = rig.world().parts().find(id);
    REQUIRE(part != nullptr);
    part->cframe.position = {5.0, 0.0, 0.0};

    FieldSet current;
    REQUIRE(extractFields(rig.world(), id, *desc, current));
    diffFields(*desc, baseline, current, deltas);
    REQUIRE(deltas.size() == 1);
    CHECK(asCFrame(deltas[0].value).position.x == doctest::Approx(5.0));
}

TEST_CASE("a wire id is unique within a class, and the raw ids are not")
{
    // **The defect this pins.** `api/wire/schema.luau` numbers the common fields
    // from 1 and each class's fields from 1 independently, so `Name` (common id
    // 1) and `CFrame` (`BasePart` id 1) are the same number. A decoder matching
    // on the raw id would write a name into a transform.
    const generated::ClassDesc* basePart = nullptr;
    for (const generated::ClassDesc& desc : generated::Classes) {
        if (desc.name == "BasePart") {
            basePart = &desc;
        }
    }
    REQUIRE(basePart != nullptr);

    const generated::FieldDesc* name = fieldAt(*basePart, 0);
    const generated::FieldDesc* cframe = fieldAt(*basePart, std::size(generated::CommonFields));
    REQUIRE(name != nullptr);
    REQUIRE(cframe != nullptr);
    CHECK(name->name == "Name");
    CHECK(cframe->name == "CFrame");
    // The raw ids collide, which is legal and is why the wire ids do not.
    CHECK(name->id == cframe->id);
    CHECK(wireIdAt(*basePart, 0) != wireIdAt(*basePart, std::size(generated::CommonFields)));

    // Every wire id in the class is distinct.
    std::vector<core::u16> seen;
    for (core::usize at = 0; at < fieldCount(*basePart); ++at) {
        const core::u16 id = wireIdAt(*basePart, at);
        for (const core::u16 other : seen) {
            CAPTURE(at);
            CHECK(id != other);
        }
        seen.push_back(id);
    }
}

TEST_CASE("applying a delta writes the field its wire id names")
{
    Rig rig;
    const core::InstanceId id = rig.part({0.0, 0.0, 0.0});
    scene::RigidBodyComponent body;
    rig.world().rigidBodies().add(id, body);

    const generated::ClassDesc* desc = schemaFor(rig.world(), id);
    REQUIRE(desc != nullptr);

    FieldValue moved;
    core::CFrameD target;
    target.position = {7.0, 8.0, 9.0};
    setCFrame(moved, target);

    const core::usize common = std::size(generated::CommonFields);
    FieldDelta delta{wireIdAt(*desc, common), moved};
    REQUIRE(applyField(rig.world(), id, *desc, delta));

    const scene::PartComponent* part = rig.world().parts().find(id);
    REQUIRE(part != nullptr);
    CHECK(part->cframe.position.y == doctest::Approx(8.0));
    // And the name was not touched, which is what the wire id's top bit buys.
    CHECK(rig.world().name(id).valid());
}

TEST_CASE("a delta naming a field this class does not have is refused")
{
    // What a peer speaking a newer protocol looks like. A refusal rather than a
    // guess: a decoder that fell through to the nearest field would corrupt a
    // world rather than report a mismatch.
    Rig rig;
    const core::InstanceId id = rig.part({0.0, 0.0, 0.0});
    const generated::ClassDesc* desc = schemaFor(rig.world(), id);
    REQUIRE(desc != nullptr);

    FieldValue value;
    setU32(value, 42);
    CHECK_FALSE(applyField(rig.world(), id, *desc, FieldDelta{9999, value}));
}

TEST_CASE("every pool the wire reads is one a capture can tell has not changed")
{
    // A capture keeps an instance from the tick before when the components
    // its fields are read from are the same bytes (`sourceDigestOf`). A field
    // of a pool the digest does not know makes its whole class one that is
    // read every tick: never wrong, and exactly the cost this was written to
    // take away -- so a pool added to the wire is added to the digest's list.
    for (const generated::ClassDesc& desc : generated::Classes) {
        CAPTURE(desc.name);
        CHECK(digestKnowsPoolsOf(desc));
    }
}

// --- Sequences (protocol 44) -------------------------------------------------------------

namespace {

[[nodiscard]] const generated::ClassDesc* classNamed(std::string_view name)
{
    for (const generated::ClassDesc& desc : generated::Classes) {
        if (desc.name == name)
            return &desc;
    }
    return nullptr;
}

[[nodiscard]] core::usize indexNamed(const generated::ClassDesc& desc, std::string_view name)
{
    for (core::usize at = 0; at < fieldCount(desc); ++at) {
        if (fieldAt(desc, at)->name == name)
            return at;
    }
    return fieldCount(desc);
}

// An emitter in a rig's world: the renderer's class, declared by hand as the
// session's tests declare it, storing what the real one does.
[[nodiscard]] core::InstanceId emitterIn(Rig& rig)
{
    scene::ClassRegistry& classes = rig.fixture.schema.classes;
    scene::ClassId made = classes.findId(rig.fixture.atom("ParticleEmitter"));
    if (made == scene::InvalidClass) {
        made = classes.registerClass({
            .name = rig.fixture.atom("ParticleEmitter"),
            .super = classes.findId(rig.fixture.atom("Instance")),
            .defaultName = rig.fixture.atom("ParticleEmitter"),
            .attachComponents =
                [](scene::World& world, core::InstanceId id) {
                    world.particleEmitters().add(id, scene::ParticleEmitterComponent{});
                },
            .detachComponents = [](scene::World& world, core::InstanceId id) { world.particleEmitters().remove(id); },
        });
    }
    REQUIRE(made != scene::InvalidClass);
    const core::InstanceId id = rig.world().create(made);
    REQUIRE(id.valid());
    return id;
}

} // namespace

TEST_CASE("a class with a sequence keeps its further cells after its fields (protocol 44)")
{
    // Every class: the further cells of its sequences lie past its fields,
    // one range a sequence and none shared, and a class with none is exactly
    // its fields -- which is every class there was before protocol 44.
    for (const generated::ClassDesc& desc : generated::Classes) {
        CAPTURE(desc.name);
        core::usize next = fieldCount(desc);
        for (core::usize at = 0; at < fieldCount(desc); ++at) {
            const CellRange range = furtherCells(desc, at);
            CHECK(range.count == furtherCellsOf(fieldAt(desc, at)->encoding));
            if (range.count == 0)
                continue;
            CHECK(range.first == next);
            next += range.count;
        }
        CHECK(cellCount(desc) == next);
        // Past the fields a cell has no field and no id of its own.
        if (cellCount(desc) > fieldCount(desc)) {
            CHECK(fieldAt(desc, fieldCount(desc)) == nullptr);
            CHECK(wireIdAt(desc, fieldCount(desc)) == 0);
        }
    }

    const generated::ClassDesc* emitter = classNamed("ParticleEmitter");
    REQUIRE(emitter != nullptr);
    // A colour sequence's five and two number sequences' three each.
    CHECK(cellCount(*emitter) == fieldCount(*emitter) + 5 + 3 + 3);
    const generated::ClassDesc* part = classNamed("Part");
    REQUIRE(part != nullptr);
    CHECK(cellCount(*part) == fieldCount(*part));
}

TEST_CASE("a sequence is read, compared and written as one value, whichever of its cells moved (protocol 44)")
{
    Rig rig;
    const generated::ClassDesc* desc = classNamed("ParticleEmitter");
    REQUIRE(desc != nullptr);
    const core::InstanceId id = emitterIn(rig);
    REQUIRE(schemaFor(rig.world(), id) == desc);

    core::ColorSequence twenty;
    twenty.keypoints.clear();
    for (core::usize key = 0; key < core::MaxSequenceKeypoints; ++key) {
        const auto along = static_cast<core::f32>(key) / static_cast<core::f32>(core::MaxSequenceKeypoints - 1);
        twenty.keypoints.push_back(core::ColorKeypoint{along, core::Color3{along, along, along}});
    }
    rig.world().particleEmitters().find(id)->colorOverLife = twenty;

    FieldSet baseline;
    REQUIRE(extractFields(rig.world(), id, *desc, baseline));
    CHECK(baseline.size() == cellCount(*desc));

    // **One key far along it**, which is in a further cell and in no byte of
    // the field's own: the diff still names the sequence, once, and nothing
    // else.
    const core::usize colours = indexNamed(*desc, "ColorOverLife");
    REQUIRE(colours < fieldCount(*desc));
    rig.world().particleEmitters().find(id)->colorOverLife.keypoints[17].value.g = 0.125f;
    FieldSet current;
    REQUIRE(extractFields(rig.world(), id, *desc, current));
    CHECK(baseline[colours] == current[colours]);
    CHECK_FALSE(sameField(*desc, colours, baseline, current));
    CHECK(sameField(*desc, indexNamed(*desc, "SizeOverLife"), baseline, current));
    std::vector<FieldDelta> deltas;
    diffFields(*desc, baseline, current, deltas);
    REQUIRE(deltas.size() == 1);
    CHECK(deltas[0].id == wireIdAt(*desc, colours));

    // Onto a message and back into another set, and from there to another
    // emitter: the same sequence, to the bit.
    std::vector<core::u8> bytes;
    encodeSequenceField(bytes, *desc, colours, current);
    CHECK(bytes.size() == 1 + core::MaxSequenceKeypoints * 16);
    FieldSet received(cellCount(*desc));
    core::usize at = 0;
    REQUIRE(decodeSequenceField(bytes, at, *desc, colours, received));
    CHECK(at == bytes.size());
    CHECK(sameField(*desc, colours, received, current));
    const core::InstanceId other = emitterIn(rig);
    REQUIRE(applySequence(rig.world(), other, *desc, colours, received));
    CHECK(rig.world().particleEmitters().find(other)->colorOverLife ==
          rig.world().particleEmitters().find(id)->colorOverLife);

    // A field that is no sequence is not one to these, and one cell is not a
    // sequence to `applyField`.
    const core::usize rate = indexNamed(*desc, "Rate");
    CHECK_FALSE(decodeSequenceField(bytes, at, *desc, rate, received));
    CHECK_FALSE(applySequence(rig.world(), other, *desc, rate, received));
    CHECK_FALSE(applyField(rig.world(), other, *desc, FieldDelta{wireIdAt(*desc, colours), received[colours]}));

    // Cells that hold no sequence leave the emitter's as it was.
    FieldSet empty(cellCount(*desc));
    CHECK_FALSE(applySequence(rig.world(), other, *desc, colours, empty));
    CHECK(rig.world().particleEmitters().find(other)->colorOverLife ==
          rig.world().particleEmitters().find(id)->colorOverLife);
}
