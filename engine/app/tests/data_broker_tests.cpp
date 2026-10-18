// The data service's records (ADR 0238 §2): one world holds a record at a
// time, the next waits for it or is refused, and a world that dies lets go
// of what it held. And the game's database keeps what was written.
#include <doctest/doctest.h>
#include <filesystem>
#include <string>

#include "engine/app/data_broker.h"
#include "engine/app/data_ops.h"
#include "engine/app/data_store.h"

using engine::app::DataBroker;
using engine::app::StoredRecord;

TEST_CASE("data broker > a record is given to the world that asks, with what was written")
{
    DataBroker broker(engine::app::makeMemoryDataStore());
    broker.load("town", 1, "ana", 0);
    std::vector<DataBroker::Answer> answers = broker.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].ok);
    CHECK(answers[0].world == "town");
    CHECK_FALSE(answers[0].record.has_value());
    CHECK(broker.holder("ana") == "town");

    CHECK(broker.write("town", "ana", StoredRecord{"level=2", 3.0}, true, 10));
    CHECK(broker.holder("ana").empty());
    broker.load("field", 2, "ana", 20);
    answers = broker.takeAnswers();
    REQUIRE(answers.size() == 1);
    REQUIRE(answers[0].record.has_value());
    CHECK(answers[0].record->body == "level=2");
    CHECK(answers[0].record->version == 3.0);
}

TEST_CASE("data broker > a second world waits for the record, and is given it when the first lets go")
{
    DataBroker broker(engine::app::makeMemoryDataStore());
    broker.load("town:1", 1, "ana", 0);
    (void)broker.takeAnswers();
    broker.load("town:2", 7, "ana", 100);
    broker.tick(5'000);
    CHECK(broker.takeAnswers().empty());
    CHECK(broker.write("town:1", "ana", StoredRecord{"gold=5", 1.0}, true, 6'000));
    const std::vector<DataBroker::Answer> answers = broker.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].world == "town:2");
    CHECK(answers[0].id == 7);
    REQUIRE(answers[0].record.has_value());
    CHECK(answers[0].record->body == "gold=5");
    CHECK(broker.holder("ana") == "town:2");
}

TEST_CASE("data broker > a world that waits ten seconds is refused as busy")
{
    DataBroker broker(engine::app::makeMemoryDataStore());
    broker.load("town:1", 1, "ana", 0);
    (void)broker.takeAnswers();
    broker.load("town:2", 2, "ana", 1'000);
    broker.tick(10'999);
    CHECK(broker.takeAnswers().empty());
    broker.tick(11'000);
    const std::vector<DataBroker::Answer> answers = broker.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].busy);
    CHECK_FALSE(answers[0].ok);
    CHECK(broker.holder("ana") == "town:1");
}

TEST_CASE("data broker > a world that dies lets go of its records, and its late write is refused")
{
    DataBroker broker(engine::app::makeMemoryDataStore());
    broker.load("dungeon", 1, "ana", 0);
    broker.load("dungeon", 2, "bo", 0);
    CHECK(broker.write("dungeon", "ana", StoredRecord{"before", 1.0}, false, 10));
    (void)broker.takeAnswers();
    broker.load("town", 3, "ana", 20);
    broker.worldGone("dungeon", 30);
    CHECK(broker.held() == 1);
    const std::vector<DataBroker::Answer> answers = broker.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].world == "town");
    REQUIRE(answers[0].record.has_value());
    CHECK(answers[0].record->body == "before");
    CHECK_FALSE(broker.write("dungeon", "ana", StoredRecord{"after", 1.0}, true, 40));
    CHECK(broker.holder("ana") == "town");
}

namespace {

// What every store does with items and money, the database and the memory
// one alike.
void checkTransactions(engine::app::IDataStore& store)
{
    using engine::app::DataMove;
    const std::optional<engine::core::i64> sword = store.createItem("town", "ana", "Sword", "edge=3");
    const std::optional<engine::core::i64> shield = store.createItem("town", "ana", "Shield", "");
    REQUIRE(sword.has_value());
    REQUIRE(shield.has_value());
    CHECK(*shield != *sword);
    // Money from the game itself.
    CHECK(store.transact("town", {DataMove{0, "Gold", 1000, "", "bo"}}).ok);
    CHECK(store.balance("bo", "Gold") == 1000);

    // A trade: all of it.
    const engine::app::DataTransaction traded =
        store.transact("town", {DataMove{*sword, "", 0, "ana", "bo"}, DataMove{0, "Gold", 250, "bo", "ana"}});
    CHECK(traded.ok);
    CHECK(traded.id > 0);
    CHECK(store.balance("ana", "Gold") == 250);
    CHECK(store.balance("bo", "Gold") == 750);
    REQUIRE(store.items("bo").size() == 1);
    CHECK(store.items("bo")[0].kind == "Sword");
    CHECK(store.items("bo")[0].fields == "edge=3");

    // Or none of it: the shield moves, then the payment cannot be made.
    const engine::app::DataTransaction short_ =
        store.transact("town", {DataMove{*shield, "", 0, "ana", "bo"}, DataMove{0, "Gold", 900, "bo", "ana"}});
    CHECK_FALSE(short_.ok);
    CHECK(short_.reason == "NotEnough");
    CHECK(store.items("ana").size() == 1);
    CHECK(store.balance("bo", "Gold") == 750);
    // An item not where the move says.
    const engine::app::DataTransaction wrong = store.transact("town", {DataMove{*sword, "", 0, "ana", "bo"}});
    CHECK(wrong.reason == "ItemNotThere");
    CHECK(store.transact("town", {DataMove{0, "Gold", -5, "bo", "ana"}}).reason == "Invalid");

    // An id is never given twice.
    CHECK(store.destroyItem("town", *shield));
    CHECK_FALSE(store.destroyItem("town", *shield));
    const std::optional<engine::core::i64> next = store.createItem("town", "ana", "Bow", "");
    REQUIRE(next.has_value());
    CHECK(*next > *shield);
}

} // namespace

TEST_CASE("data store > a transaction moves everything or nothing, in memory")
{
    const auto store = engine::app::makeMemoryDataStore();
    checkTransactions(*store);
}

TEST_CASE("data store > an operation as text is answered as text")
{
    const auto store = engine::app::makeMemoryDataStore();
    const std::string made =
        engine::app::runDataOp(*store, "town", R"({"op":"create","owner":"ana","kind":"Sword","fields":""})");
    CHECK(made.find("\"ok\":true") != std::string::npos);
    const std::string paid = engine::app::runDataOp(
        *store, "town", R"({"op":"transact","moves":[{"amount":5,"currency":"Gold","from":"","to":"ana"}]})");
    CHECK(paid.find("\"ok\":true") != std::string::npos);
    CHECK(engine::app::runDataOp(*store, "town", R"({"op":"balance","owner":"ana","currency":"Gold"})")
              .find("\"balance\":5") != std::string::npos);
    CHECK(engine::app::runDataOp(*store, "town", R"({"op":"transact","moves":[{"amount":1.5,"currency":"Gold"}]})")
              .find("Invalid") != std::string::npos);
    CHECK(engine::app::runDataOp(*store, "town", "not json").find("Invalid") != std::string::npos);
}

#if ENG_ENABLE_DATA
TEST_CASE("data store > a transaction moves everything or nothing, in the game's database")
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "engine-data-transact-test";
    std::filesystem::remove_all(folder);
    std::string error;
    {
        const auto store = engine::app::openDataStore(folder / "game.db", error);
        REQUIRE_MESSAGE(store != nullptr, error);
        checkTransactions(*store);
    }
    // And kept: what the trades left, after the file is closed.
    const auto store = engine::app::openDataStore(folder / "game.db", error);
    REQUIRE_MESSAGE(store != nullptr, error);
    CHECK(store->balance("ana", "Gold") == 250);
    CHECK(store->items("bo").size() == 1);
}

TEST_CASE("data store > the game's database keeps a record after it is closed")
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "engine-data-store-test";
    std::filesystem::remove_all(folder);
    const std::filesystem::path file = folder / "game.db";
    {
        std::string error;
        const auto store = engine::app::openDataStore(file, error);
        REQUIRE_MESSAGE(store != nullptr, error);
        CHECK_FALSE(store->loadRecord("ana").has_value());
        CHECK(store->writeRecord("ana", StoredRecord{std::string("a\0b", 3), 2.0}));
        CHECK(store->writeRecord("ana", StoredRecord{std::string("c\0d", 3), 4.0}));
    }
    std::string error;
    const auto store = engine::app::openDataStore(file, error);
    REQUIRE_MESSAGE(store != nullptr, error);
    const std::optional<StoredRecord> read = store->loadRecord("ana");
    REQUIRE(read.has_value());
    CHECK(read->body == std::string("c\0d", 3));
    CHECK(read->version == 4.0);
}
#endif
