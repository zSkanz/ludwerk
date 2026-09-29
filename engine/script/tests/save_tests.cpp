// `SaveService`'s store (ADR 0111): what a slot holds comes back as what it
// was, a damaged file is read from its backup, and a name is never a path.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "engine/core/sequence.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/script/save_store.h"
#include "script_fixture.h"

using namespace engine;
using script::SaveSlotData;
using script::SaveStore;
using script::SaveValue;

namespace {

// A folder of its own under the system's temporary one, gone afterwards.
struct TempFolder
{
    std::filesystem::path path;
    TempFolder()
    {
        std::error_code error;
        path = std::filesystem::temp_directory_path(error) / ("engine-save-tests-" + std::to_string(platform::nowNs()));
        std::filesystem::create_directories(path, error);
    }
    ~TempFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

[[nodiscard]] SaveValue scalar(scene::Value value)
{
    return SaveValue{std::move(value), nullptr};
}

} // namespace

TEST_CASE("a slot name is letters, digits, underscore and dash, never a path")
{
    CHECK(SaveStore::validName("profile_1"));
    CHECK(SaveStore::validName("Settings-v2"));
    CHECK_FALSE(SaveStore::validName(""));
    CHECK_FALSE(SaveStore::validName("../escape"));
    CHECK_FALSE(SaveStore::validName("a/b"));
    CHECK_FALSE(SaveStore::validName("with space"));
    CHECK_FALSE(SaveStore::validName(std::string(65, 'a')));
    // Devices on Windows, whatever the case and the extension (audit S15).
    for (const char* device : {"CON", "con", "Nul", "PRN", "aux", "COM1", "lpt9"})
        CHECK_FALSE(SaveStore::validName(device));
    CHECK(SaveStore::validName("COM10"));
    CHECK(SaveStore::validName("console"));
}

TEST_CASE("a slot is one spelling: another case of an open one is refused (audit S15)")
{
    SaveStore store(SaveStore::Options{});
    REQUIRE(store.open("Profile") != nullptr);
    CHECK(store.otherSpelling("profile") == "Profile");
    CHECK(store.open("profile") == nullptr);
    CHECK(store.open("Profile") != nullptr);
    CHECK(store.otherSpelling("Profile").empty());
}

TEST_CASE("every kind of value a slot holds comes back as what it was")
{
    SaveSlotData slot;
    slot.name = "all";
    slot.values["flag"] = scalar(scene::Value{true});
    slot.values["number"] = scalar(scene::Value{core::f64{0.1}});
    slot.values["text"] = scalar(scene::Value{std::string("hello \"there\"")});
    slot.values["vector"] = scalar(scene::Value{core::Vec3{1.0f, 2.0f, 3.0f}});
    slot.values["colour"] = scalar(scene::Value{core::Color3{0.25f, 0.5f, 0.75f}});
    slot.values["plane"] = scalar(scene::Value{core::Vec2{4.0f, 5.0f}});
    slot.values["udim"] = scalar(scene::Value{core::UDim{0.5f, 12.0f}});
    slot.values["udim2"] = scalar(scene::Value{core::UDim2{core::UDim{0.1f, 1.0f}, core::UDim{0.2f, 2.0f}}});
    slot.values["rect"] = scalar(scene::Value{core::Rect{core::Vec2{1.0f, 2.0f}, core::Vec2{3.0f, 4.0f}}});
    core::CFrameD frame;
    frame.position = core::DVec3{10.5, -2.0, 3.25};
    slot.values["frame"] = scalar(scene::Value{frame});
    slot.values["gradient"] = scalar(scene::Value{core::ColorSequence{}});
    slot.values["curve"] = scalar(scene::Value{core::NumberSequence{}});
    auto inner = std::make_shared<script::SaveTable>();
    inner->array.push_back(scalar(scene::Value{core::f64{1.0}}));
    inner->array.push_back(scalar(scene::Value{std::string("two")}));
    inner->fields["deep"] = scalar(scene::Value{core::Color3{1.0f, 0.0f, 0.0f}});
    slot.values["table"] = SaveValue{scene::Value{}, inner};

    const std::string text = SaveStore::encode(slot, 3.0);
    SaveSlotData back;
    REQUIRE(SaveStore::decode(text, back));
    CHECK(back.version == doctest::Approx(3.0));
    REQUIRE(back.values.size() == slot.values.size());
    for (const auto& [key, value] : slot.values) {
        INFO(key);
        const SaveValue& other = back.values.at(key);
        if (value.table != nullptr) {
            REQUIRE(other.table != nullptr);
            CHECK(other.table->array.size() == 2);
            CHECK(std::get<std::string>(other.table->array[1].scalar) == "two");
            // A Color3 inside a table is still a Color3, not a Vector3.
            CHECK(std::holds_alternative<core::Color3>(other.table->fields.at("deep").scalar));
        }
        else {
            CHECK(other.scalar == value.scalar);
        }
    }
}

TEST_CASE("a damaged file is caught by its checksum")
{
    SaveSlotData slot;
    slot.values["gold"] = scalar(scene::Value{core::f64{100.0}});
    std::string text = SaveStore::encode(slot, 1.0);
    text[text.size() - 3] = text[text.size() - 3] == '1' ? '2' : '1';
    SaveSlotData back;
    CHECK_FALSE(SaveStore::decode(text, back));
}

TEST_CASE("a slot is written, read back, and survives damage through its backup")
{
    TempFolder folder;
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        SaveSlotData* slot = store.open("profile");
        REQUIRE(slot != nullptr);
        slot->values["level"] = scalar(scene::Value{core::f64{3.0}});
        ++slot->generation;
        store.flush(1.0);
        // A second save: the first becomes the backup.
        slot->values["level"] = scalar(scene::Value{core::f64{4.0}});
        ++slot->generation;
        store.flush(1.0);
    }
    CHECK(std::filesystem::exists(folder.path / "profile.save"));
    CHECK(std::filesystem::exists(folder.path / "profile.bak"));
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        script::SaveDamage damage = script::SaveDamage::None;
        SaveSlotData* slot = store.open("profile", &damage);
        REQUIRE(slot != nullptr);
        CHECK(damage == script::SaveDamage::None);
        CHECK(std::get<core::f64>(slot->values.at("level").scalar) == doctest::Approx(4.0));
    }
    // The newest file damaged: the backup, one save older, is read.
    {
        std::ofstream broken(folder.path / "profile.save", std::ios::binary | std::ios::trunc);
        broken << "not a save";
    }
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        script::SaveDamage damage = script::SaveDamage::None;
        SaveSlotData* slot = store.open("profile", &damage);
        REQUIRE(slot != nullptr);
        CHECK(damage == script::SaveDamage::FromBackup);
        CHECK(slot->recovered);
        CHECK(std::get<core::f64>(slot->values.at("level").scalar) == doctest::Approx(3.0));
    }
    // Both damaged: empty, and it says so.
    {
        std::ofstream broken(folder.path / "profile.bak", std::ios::binary | std::ios::trunc);
        broken << "not a save either";
    }
    {
        std::ofstream broken(folder.path / "profile.save", std::ios::binary | std::ios::trunc);
        broken << "still not";
    }
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        script::SaveDamage damage = script::SaveDamage::None;
        SaveSlotData* slot = store.open("profile", &damage);
        REQUIRE(slot != nullptr);
        CHECK(damage == script::SaveDamage::Lost);
        CHECK_FALSE(slot->recovered);
        CHECK(slot->values.empty());
    }
}

TEST_CASE("a write that dies between its two renames leaves the last save readable")
{
    // The writer first makes the previous save the backup, then renames the
    // new file into place. A process killed between the two leaves only the
    // backup -- and that is what is read, with nothing lost but the save that
    // never finished.
    TempFolder folder;
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        SaveSlotData* slot = store.open("killed");
        slot->values["gold"] = scalar(scene::Value{core::f64{7.0}});
        ++slot->generation;
        store.flush(1.0);
    }
    std::error_code error;
    std::filesystem::rename(folder.path / "killed.save", folder.path / "killed.bak", error);
    REQUIRE_FALSE(error);
    SaveStore store(SaveStore::Options{.directory = folder.path});
    script::SaveDamage damage = script::SaveDamage::Lost;
    SaveSlotData* slot = store.open("killed", &damage);
    REQUIRE(slot != nullptr);
    CHECK(slot->recovered);
    CHECK(std::get<core::f64>(slot->values.at("gold").scalar) == doctest::Approx(7.0));
}

TEST_CASE("the slot limit and removal")
{
    TempFolder folder;
    SaveStore store(SaveStore::Options{.directory = folder.path, .maxSlots = 2});
    REQUIRE(store.open("a") != nullptr);
    REQUIRE(store.open("b") != nullptr);
    CHECK(store.open("c") == nullptr);
    // One already open is always answered.
    CHECK(store.open("a") != nullptr);

    SaveSlotData* a = store.open("a");
    a->values["x"] = scalar(scene::Value{true});
    ++a->generation;
    store.flush(1.0);
    CHECK(store.list() == std::vector<std::string>{"a"});
    CHECK(store.remove("a"));
    CHECK(a->values.empty());
    CHECK_FALSE(std::filesystem::exists(folder.path / "a.save"));
    CHECK(store.list().empty());
}

TEST_CASE("a script keeps a value in a slot and reads it back")
{
    TempFolder folder;
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);
    SaveStore store(SaveStore::Options{.directory = folder.path});
    fixture.runtime->setSaveStore(&store);

    CHECK(fixture.failure(R"(
        local saves = game:GetService("SaveService")
        local slot = saves:GetSlotAsync("progress")
        assert(slot.Name == "progress" and slot.Recovered)
        slot:Set("coins", 12)
        slot:Set("where", Vector3.new(1, 2, 3))
        slot:Set("bag", { "sword", { gems = 2 } })
        assert(slot:Update("coins", function(old) return old + 1 end) == 13)
        assert(slot:Get("coins") == 13)
        assert(slot:Get("bag")[2].gems == 2)
        -- A copy: changing what Get returned changes nothing.
        slot:Get("bag")[1] = "stick"
        assert(slot:Get("bag")[1] == "sword")
        local keys = slot:GetKeys()
        assert(#keys == 3 and keys[1] == "bag")
        slot:SaveAsync()
        print("save-done")
    )") == "");
    fixture.tick(2);
    // The write is a real thread's: waited for, then the tick that resumes.
    store.flush(1.0);
    fixture.tick(2);
    CHECK(fixture.errors() == "");
    CHECK(fixture.logContains("save-done"));
    CHECK(std::filesystem::exists(folder.path / "progress.save"));

    SaveStore reopened(SaveStore::Options{.directory = folder.path});
    SaveSlotData* slot = reopened.open("progress");
    REQUIRE(slot != nullptr);
    CHECK(std::get<core::f64>(slot->values.at("coins").scalar) == doctest::Approx(13.0));
    CHECK(std::holds_alternative<core::Vec3>(slot->values.at("where").scalar));
}

TEST_CASE("what a save cannot hold is an error at Set, and a name is never a path")
{
    TempFolder folder;
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);
    SaveStore store(SaveStore::Options{.directory = folder.path});
    fixture.runtime->setSaveStore(&store);
    CHECK(fixture.failure(R"(
        local saves = game:GetService("SaveService")
        local slot = saves:GetSlotAsync("bad")
        assert(not pcall(function() slot:Set("f", function() end) end))
        local loop = {}
        loop.self = loop
        assert(not pcall(function() slot:Set("loop", loop) end))
        assert(not pcall(function() slot:Set("gap", { [1] = 1, [3] = 3 }) end))
        assert(not pcall(function() slot:Set("part", workspace) end))
        assert(not pcall(function() slot:Set("nan", 0 / 0) end))
        assert(not pcall(function() saves:GetSlotAsync("../x") end))
        assert(#slot:GetKeys() == 0)
        print("refusals-done")
    )") == "");
    fixture.tick(5);
    CHECK(fixture.errors() == "");
    CHECK(fixture.logContains("refusals-done"));
}

TEST_CASE("a slot's size is kept as it changes, and the limit reads it (audit S12)")
{
    TempFolder folder;
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);
    SaveStore store(SaveStore::Options{.directory = folder.path, .maxSlotBytes = 200});
    fixture.runtime->setSaveStore(&store);
    CHECK(fixture.failure(R"(
        local slot = game:GetService("SaveService"):GetSlotAsync("sized")
        slot:Set("a", 1)
        slot:Set("b", { "x", { deep = Vector3.new(1, 2, 3) } })
        slot:Set("a", "longer than a number")
        slot:Remove("b")
        slot:Set("c", true)
        -- Past the limit: refused, and the slot is as it was.
        assert(not pcall(function() slot:Set("big", string.rep("z", 300)) end))
        assert(slot:Get("big") == nil)
        -- Replacing a value with a smaller one fits where adding it would not.
        slot:Set("a", string.rep("y", 150))
        slot:Set("a", 2)
        print("sized-done")
    )") == "");
    fixture.tick(2);
    CHECK(fixture.errors() == "");
    CHECK(fixture.logContains("sized-done"));
    SaveSlotData* slot = store.open("sized");
    REQUIRE(slot != nullptr);
    REQUIRE(slot->entryBytesKnown);
    CHECK(SaveStore::payloadSize(slot->entryBytes, slot->values.size()) == SaveStore::encodedSize(*slot));
}

TEST_CASE("a save file larger than any slot could write is not read (audit F13)")
{
    TempFolder folder;
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        SaveSlotData* slot = store.open("big");
        REQUIRE(slot != nullptr);
        slot->values["text"] = scalar(scene::Value{std::string(4000, 'x')});
        ++slot->generation;
        store.flush(1.0);
    }
    // A store whose slots are held to a kilobyte never wrote this file, and
    // does not load four of them to find out.
    SaveStore store(SaveStore::Options{.directory = folder.path, .maxSlotBytes = 1024});
    script::SaveDamage damage = script::SaveDamage::None;
    SaveSlotData* slot = store.open("big", &damage);
    REQUIRE(slot != nullptr);
    CHECK(slot->values.empty());
    CHECK(damage == script::SaveDamage::Lost);
}

TEST_CASE("an older slot is migrated once before it is handed over")
{
    TempFolder folder;
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        SaveSlotData* slot = store.open("old");
        slot->values["hp"] = scalar(scene::Value{core::f64{10.0}});
        ++slot->generation;
        store.flush(1.0);
    }
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);
    SaveStore store(SaveStore::Options{.directory = folder.path});
    fixture.runtime->setSaveStore(&store);
    CHECK(fixture.failure(R"(
        local saves = game:GetService("SaveService")
        saves.Version = 2
        local runs = 0
        saves.OnMigrate = function(slot, from)
            runs += 1
            assert(from == 1)
            slot:Set("health", slot:Get("hp") * 10)
            slot:Remove("hp")
        end
        local slot = saves:GetSlotAsync("old")
        assert(slot:Get("health") == 100 and slot:Get("hp") == nil)
        local again = saves:GetSlotAsync("old")
        assert(runs == 1, tostring(runs))
        print("migrate-done")
    )") == "");
    fixture.tick(10);
    CHECK(fixture.errors() == "");
    CHECK(fixture.logContains("migrate-done"));
}

TEST_CASE("a slot read from its backup keeps that backup through the next save (audit F7)")
{
    // Recovered from `.bak`, the next write made the damaged main the backup
    // -- over the only good copy -- so a second damage lost everything.
    TempFolder folder;
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        SaveSlotData* slot = store.open("hero");
        REQUIRE(slot != nullptr);
        slot->values["level"] = scalar(scene::Value{core::f64{3.0}});
        ++slot->generation;
        store.flush(1.0);
        slot->values["level"] = scalar(scene::Value{core::f64{4.0}});
        ++slot->generation;
        store.flush(1.0);
    }
    {
        std::ofstream broken(folder.path / "hero.save", std::ios::binary | std::ios::trunc);
        broken << "not a save";
    }
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        script::SaveDamage damage = script::SaveDamage::None;
        SaveSlotData* slot = store.open("hero", &damage);
        REQUIRE(slot != nullptr);
        REQUIRE(damage == script::SaveDamage::FromBackup);
        slot->values["level"] = scalar(scene::Value{core::f64{5.0}});
        ++slot->generation;
        store.flush(1.0);
    }
    // The damaged file is kept aside, not made the backup.
    CHECK(std::filesystem::exists(folder.path / "hero.corrupt"));
    // Damaged again: the backup is still a good one.
    {
        std::ofstream broken(folder.path / "hero.save", std::ios::binary | std::ios::trunc);
        broken << "damaged again";
    }
    {
        SaveStore store(SaveStore::Options{.directory = folder.path});
        script::SaveDamage damage = script::SaveDamage::None;
        SaveSlotData* slot = store.open("hero", &damage);
        REQUIRE(slot != nullptr);
        CHECK(damage == script::SaveDamage::FromBackup);
        REQUIRE(slot->values.contains("level"));
        CHECK(std::get<core::f64>(slot->values.at("level").scalar) == doctest::Approx(3.0));
    }
}
