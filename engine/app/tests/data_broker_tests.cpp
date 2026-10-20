// The data service's records (ADR 0238 §2): one world holds a record at a
// time, the next waits for it or is refused, and a world that dies lets go
// of what it held. And the game's database keeps what was written.
#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "engine/app/data_broker.h"
#include "engine/app/data_console.h"
#include "engine/app/data_ops.h"
#include "engine/app/data_store.h"

using engine::app::DataBroker;
using engine::app::StoredRecord;

namespace {

// **The store is the worker's**: what was asked is done once the queue is
// drained to here, and its answers are taken by `pump`.
[[nodiscard]] std::vector<DataBroker::Answer> settled(DataBroker& broker)
{
    broker.wait([](engine::app::IDataStore&) {});
    broker.pump();
    return broker.takeAnswers();
}

// Something of the store's, done on its thread, and what it returned.
template <typename F>
[[nodiscard]] auto onStore(DataBroker& data, F&& f)
{
    decltype(f(std::declval<engine::app::IDataStore&>())) out{};
    data.wait([&](engine::app::IDataStore& store) { out = f(store); });
    return out;
}

// A console's frame, and the answer of the job it asked for.
void frameOf(engine::app::DataConsole& console, DataBroker& data, std::int64_t now)
{
    console.frame(data, now);
    data.wait([](engine::app::IDataStore&) {});
    data.pump();
}

} // namespace

TEST_CASE("data broker > a record is given to the world that asks, with what was written")
{
    DataBroker broker(engine::app::makeMemoryDataStore());
    broker.load("town", 1, "ana", 0);
    std::vector<DataBroker::Answer> answers = settled(broker);
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].ok);
    CHECK(answers[0].world == "town");
    CHECK_FALSE(answers[0].record.has_value());
    CHECK(broker.holder("ana") == "town");

    CHECK(broker.write("town", "ana", StoredRecord{"level=2", 3.0}, true, 10));
    CHECK(broker.holder("ana").empty());
    broker.load("field", 2, "ana", 20);
    answers = settled(broker);
    REQUIRE(answers.size() == 1);
    REQUIRE(answers[0].record.has_value());
    CHECK(answers[0].record->body == "level=2");
    CHECK(answers[0].record->version == 3.0);
}

TEST_CASE("data broker > a second world waits for the record, and is given it when the first lets go")
{
    DataBroker broker(engine::app::makeMemoryDataStore());
    broker.load("town:1", 1, "ana", 0);
    (void)settled(broker);
    broker.load("town:2", 7, "ana", 100);
    broker.tick(5'000);
    CHECK(settled(broker).empty());
    CHECK(broker.write("town:1", "ana", StoredRecord{"gold=5", 1.0}, true, 6'000));
    const std::vector<DataBroker::Answer> answers = settled(broker);
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
    (void)settled(broker);
    broker.load("town:2", 2, "ana", 1'000);
    broker.tick(10'999);
    CHECK(settled(broker).empty());
    broker.tick(11'000);
    const std::vector<DataBroker::Answer> answers = settled(broker);
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
    (void)settled(broker);
    broker.load("town", 3, "ana", 20);
    broker.worldGone("dungeon", 30);
    CHECK(broker.held() == 1);
    const std::vector<DataBroker::Answer> answers = settled(broker);
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

namespace {

// A shared value is written only at the version it was read at.
void checkShared(engine::app::IDataStore& store)
{
    CHECK(store.getShared("auction", "sword").version == 0);
    const engine::app::SharedWrite first = store.setShared("auction", "sword", "bid=10", 0);
    CHECK(first.ok);
    CHECK(first.now.version == 1);
    // Another world read version 0 too, and loses.
    const engine::app::SharedWrite late = store.setShared("auction", "sword", "bid=12", 0);
    CHECK_FALSE(late.ok);
    CHECK(late.now.value == "bid=10");
    CHECK(late.now.version == 1);
    CHECK(store.setShared("auction", "sword", "bid=12", 1).ok);
    CHECK(store.getShared("auction", "sword").value == "bid=12");
    CHECK(store.getShared("auction", "shield").value.empty());
}

} // namespace

TEST_CASE("data store > a shared value is written only at the version it was read at")
{
    const auto memory = engine::app::makeMemoryDataStore();
    checkShared(*memory);
#if ENG_ENABLE_DATA
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "engine-data-shared-test";
    std::filesystem::remove_all(folder);
    std::string error;
    const auto store = engine::app::openDataStore(folder / "game.db", error);
    REQUIRE_MESSAGE(store != nullptr, error);
    checkShared(*store);
#endif
}

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

namespace {

[[nodiscard]] engine::core::i64 secondsNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[maybe_unused]] [[nodiscard]] bool holds(const std::vector<engine::app::DataItem>& items, engine::core::i64 id)
{
    return std::any_of(items.begin(), items.end(), [id](const engine::app::DataItem& item) { return item.id == id; });
}

// **A query of the move log**, its fields named here: one built field by
// field leaves none unsaid.
[[nodiscard]] engine::app::DataMoveQuery logOf(std::string owner, engine::core::i64 item = 0,
                                               engine::core::i64 transaction = 0, engine::core::usize most = 20)
{
    engine::app::DataMoveQuery query;
    query.owner = std::move(owner);
    query.item = item;
    query.transaction = transaction;
    query.most = most;
    return query;
}

// **The move log, read and undone** (ADR 0238 §3), the database and the
// memory store alike.
void checkUndo(engine::app::IDataStore& store)
{
    using engine::app::DataMove;
    const std::optional<engine::core::i64> sword = store.createItem("town", "ana", "Sword", "edge=3");
    REQUIRE(sword.has_value());
    REQUIRE(store.transact("town", {DataMove{0, "Gold", 100, "", "bo"}}).ok);
    const engine::app::DataTransaction trade =
        store.transact("town", {DataMove{*sword, "", 0, "ana", "bo"}, DataMove{0, "Gold", 40, "bo", "ana"}});
    REQUIRE(trade.ok);

    // Newest first: the payment, then the sword.
    const std::vector<engine::app::DataMoveLine> traded = store.moves(logOf({}, 0, trade.id));
    REQUIRE(traded.size() == 2);
    CHECK(traded[0].move.currency == "Gold");
    CHECK(traded[0].world == "town");
    CHECK(traded[1].move.item == *sword);
    // The sword made, then traded; and Ana in all three of her lines.
    CHECK(store.moves(logOf({}, *sword)).size() == 2);
    CHECK(store.moves(logOf("ana")).size() == 3);
    CHECK(store.moves(logOf({}, 0, 0, 1)).size() == 1);

    // Undone: the sword back with Ana, the gold back with Bo -- once.
    const engine::app::DataTransaction undone = store.undo("console", trade.id);
    REQUIRE(undone.ok);
    CHECK(undone.id > trade.id);
    CHECK(store.items("ana").size() == 1);
    CHECK(store.items("bo").empty());
    CHECK(store.balance("bo", "Gold") == 100);
    CHECK(store.balance("ana", "Gold") == 0);
    CHECK(store.undo("console", trade.id).reason == "AlreadyUndone");
    CHECK(store.undo("console", 99999).reason == "NoSuchTransaction");
    CHECK(store.moves(logOf({}, 0, undone.id)).size() == 2);

    // An item destroyed is made again under its own id, as it was.
    REQUIRE(store.destroyItem("town", *sword));
    const engine::core::i64 destroyed = store.moves(logOf({}, *sword)).front().transaction;
    REQUIRE(store.undo("console", destroyed).ok);
    REQUIRE(store.items("ana").size() == 1);
    CHECK(store.items("ana")[0].id == *sword);
    CHECK(store.items("ana")[0].kind == "Sword");
    CHECK(store.items("ana")[0].fields == "edge=3");

    // What has moved on since is not undone, and nothing of it is.
    const std::optional<engine::core::i64> bow = store.createItem("town", "bo", "Bow", "");
    REQUIRE(bow.has_value());
    const engine::core::i64 made = store.moves(logOf({}, *bow)).front().transaction;
    REQUIRE(store.transact("town", {DataMove{*bow, "", 0, "bo", "ana"}}).ok);
    CHECK(store.undo("console", made).reason == "ItemNotThere");
    CHECK(store.items("ana").size() == 2);
    const engine::app::DataTransaction paid = store.transact("town", {DataMove{0, "Gold", 50, "", "cy"}});
    REQUIRE(store.transact("town", {DataMove{0, "Gold", 30, "cy", "ana"}}).ok);
    CHECK(store.undo("console", paid.id).reason == "NotEnough");
    CHECK(store.balance("cy", "Gold") == 20);

    const engine::app::DataCounts counts = store.counts();
    CHECK(counts.items == 2);
    CHECK(counts.transactions > 0);
    CHECK(counts.moves >= counts.transactions);

    // **The log's days**: nothing is from before the past, and everything
    // is from before a moment to come -- let go of a few at a time.
    CHECK(store.forgetMovesBefore(1, 100) == 0);
    engine::core::i64 forgotten = 0;
    for (engine::core::i64 gone = 1; gone > 0;) {
        gone = store.forgetMovesBefore(secondsNow() + 10, 2);
        forgotten += gone;
    }
    CHECK(forgotten == counts.moves);
    CHECK(store.moves(logOf({})).empty());
    CHECK(store.counts().transactions == 0);
    CHECK(store.undo("console", paid.id).reason == "NoSuchTransaction");
    // What the log said is gone; what it moved is not.
    CHECK(store.items("ana").size() == 2);
    CHECK(store.balance("cy", "Gold") == 20);
}

[[nodiscard]] engine::app::DataConsole::Options consoleOptions(std::filesystem::path backups, std::string game,
                                                               double everyHours, engine::core::u32 keep,
                                                               engine::core::u32 days)
{
    engine::app::DataConsole::Options options;
    options.backups = std::move(backups);
    options.game = std::move(game);
    options.backupEveryHours = everyHours;
    options.backupKeep = keep;
    options.movesKeepDays = days;
    return options;
}

} // namespace

TEST_CASE("data store > a transaction is traced in the move log and undone whole, in memory")
{
    const auto store = engine::app::makeMemoryDataStore();
    checkUndo(*store);
    std::string why;
    CHECK_FALSE(store->startBackup(std::filesystem::temp_directory_path() / "never.db", why));
    CHECK_FALSE(why.empty());
}

#if ENG_ENABLE_DATA
TEST_CASE("data store > a transaction is traced in the move log and undone whole, in the game's database")
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "engine-data-undo-test";
    std::filesystem::remove_all(folder);
    std::string error;
    const auto store = engine::app::openDataStore(folder / "game.db", error);
    REQUIRE_MESSAGE(store != nullptr, error);
    checkUndo(*store);
    CHECK(store->counts().schema == 5);
    CHECK(store->counts().bytes > 0);
}

TEST_CASE("data store > a backup taken while trades run is a database whose owners and log agree")
{
    using engine::app::DataBackupProgress;
    using engine::app::DataMove;
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "engine-data-backup-test";
    std::filesystem::remove_all(folder);
    std::string error;
    const auto store = engine::app::openDataStore(folder / "game.db", error);
    REQUIRE_MESSAGE(store != nullptr, error);
    // Enough pages that the copy is still under way after hundreds of trades.
    const std::string filler(400, 'x');
    for (int at = 0; at < 300; ++at)
        REQUIRE(store->createItem("town", "chest", "Stone", filler).has_value());
    std::vector<engine::core::i64> swords;
    for (int at = 0; at < 5; ++at) {
        const std::optional<engine::core::i64> sword = store->createItem("town", "ana", "Sword", "");
        REQUIRE(sword.has_value());
        swords.push_back(*sword);
    }
    REQUIRE(store->transact("town", {DataMove{0, "Gold", 1000, "", "bo"}}).ok);

    const std::filesystem::path copy = folder / "backups" / "game-copy.db";
    REQUIRE_MESSAGE(store->startBackup(copy, error), error);
    std::string busy;
    CHECK_FALSE(store->startBackup(folder / "second.db", busy));
    // **A thousand trades while it copies** (ADR 0238's proof), a page of
    // the copy between every ten of them.
    int trades = 0;
    int whileCopying = 0;
    // The database as it was when the copy finished, to hold the copy to.
    engine::app::DataCounts atDone;
    engine::core::i64 anaAtDone = 0;
    const auto step = [&](int pages) {
        DataBackupProgress next = store->stepBackup(pages);
        if (next.state == DataBackupProgress::State::Done) {
            atDone = store->counts();
            anaAtDone = store->balance("ana", "Gold");
        }
        return next;
    };
    DataBackupProgress progress = step(1);
    for (int at = 0; at < 1000; ++at) {
        // A sword one way and its price the other, whichever way can.
        const engine::core::i64 sword = swords[static_cast<std::size_t>(at) % swords.size()];
        const bool withAna = holds(store->items("ana"), sword);
        const std::string seller = withAna ? "ana" : "bo";
        const std::string buyer = withAna ? "bo" : "ana";
        if (store->transact("town", {DataMove{sword, "", 0, seller, buyer}, DataMove{0, "Gold", 1, buyer, seller}}).ok)
            ++trades;
        if (progress.state == DataBackupProgress::State::Copying) {
            ++whileCopying;
            if (at % 10 == 0)
                progress = step(1);
        }
    }
    while (progress.state == DataBackupProgress::State::Copying)
        progress = step(8);
    REQUIRE_MESSAGE(progress.state == DataBackupProgress::State::Done, progress.error);
    CHECK(trades == 1000);
    CHECK(whileCopying >= 300);
    CHECK(progress.copied == progress.total);
    CHECK(store->stepBackup(1).state == DataBackupProgress::State::Idle);
    CHECK(std::filesystem::exists(copy));
    CHECK_FALSE(std::filesystem::exists(folder / "backups" / "game-copy.db.partial"));

    // **The copy is the database as it was when it finished**: the same
    // owners, the same money, the same log -- and each sword's last line
    // names who holds it.
    const auto backup = engine::app::openDataStore(copy, error);
    REQUIRE_MESSAGE(backup != nullptr, error);
    CHECK(backup->counts().transactions == atDone.transactions);
    CHECK(backup->counts().moves == atDone.moves);
    CHECK(backup->items("ana").size() + backup->items("bo").size() == 5);
    CHECK(backup->items("chest").size() == 300);
    CHECK(backup->balance("ana", "Gold") + backup->balance("bo", "Gold") == 1000);
    CHECK(backup->balance("ana", "Gold") == anaAtDone);
    for (const engine::core::i64 sword : swords) {
        const std::vector<engine::app::DataMoveLine> last = backup->moves(logOf({}, sword, 0, 1));
        REQUIRE(last.size() == 1);
        CHECK(holds(backup->items(last[0].move.to), sword));
    }
}
#endif

TEST_CASE("data console > a backup's name is its time in UTC, and read back")
{
    using engine::app::DataConsole;
    CHECK(DataConsole::backupName("dev.game", 1760886000) == "dev.game-20251019-150000.db");
    CHECK(DataConsole::backupName("g", 0) == "g-19700101-000000.db");
    for (const std::int64_t at : {std::int64_t{0}, std::int64_t{1760886000}, std::int64_t{1835481599}}) {
        const std::optional<std::int64_t> back =
            DataConsole::backupTime("dev.game", DataConsole::backupName("dev.game", at));
        REQUIRE(back.has_value());
        CHECK(*back == at);
    }
    CHECK_FALSE(DataConsole::backupTime("dev.game", "other-20251019-150000.db").has_value());
    CHECK_FALSE(DataConsole::backupTime("dev.game", "dev.game-20251319-150000.db").has_value());
    CHECK_FALSE(DataConsole::backupTime("dev.game", "dev.game-20251019-150000.db.partial").has_value());
}

TEST_CASE("data console > data and moves are answered, and an undo refused says so")
{
    using engine::app::DataMove;
    DataBroker data(engine::app::makeMemoryDataStore());
    engine::app::DataConsole console(consoleOptions({}, "game", 0.0, 7, 90));
    const std::int64_t now = secondsNow();
    CHECK_FALSE(console.run("status", data, now).has_value());
    REQUIRE(console.run("data", data, now).has_value());
    CHECK_FALSE(console.run("data", data, now)->failed);
    CHECK_FALSE(console.run("moves", data, now)->failed);

    const std::optional<engine::core::i64> sword =
        onStore(data, [&](engine::app::IDataStore& store) { return store.createItem("town", "ana", "Sword", ""); });
    REQUIRE(sword.has_value());
    const engine::app::DataTransaction gift = onStore(data, [&](engine::app::IDataStore& store) {
        return store.transact("town", {DataMove{*sword, "", 0, "ana", "bo"}});
    });
    REQUIRE(gift.ok);
    const std::optional<engine::app::DataConsole::Answer> lines = console.run("moves bo", data, now);
    REQUIRE(lines.has_value());
    CHECK(lines->text.find("bo") != std::string::npos);
    // The sword made, then given: two lines.
    CHECK(console.run("moves item " + std::to_string(*sword), data, now)->text.find('\n') != std::string::npos);
    CHECK(console.run("moves item nine", data, now)->failed);
    CHECK(console.run("moves ana bo", data, now)->failed);

    CHECK(console.run("data undo", data, now)->failed);
    CHECK_FALSE(console.run("data undo " + std::to_string(gift.id), data, now)->failed);
    CHECK(onStore(data, [&](engine::app::IDataStore& store) { return store.items("ana"); }).size() == 1);
    CHECK(console.run("data undo " + std::to_string(gift.id), data, now)->failed);
    // A store in memory has no file to back up.
    CHECK(console.run("data backup", data, now)->failed);
    CHECK(console.run("data frobnicate", data, now)->failed);
}

TEST_CASE("data console > the move log's lines past their days are let go of by the frame")
{
    using engine::app::DataMove;
    DataBroker data(engine::app::makeMemoryDataStore());
    engine::app::DataConsole console(consoleOptions({}, "game", 0.0, 7, 1));
    REQUIRE(onStore(data, [&](engine::app::IDataStore& store) {
                return store.transact("town", {DataMove{0, "Gold", 5, "", "ana"}});
            }).ok);
    const std::int64_t now = secondsNow();
    // Within its day: kept.
    frameOf(console, data, now);
    frameOf(console, data, now);
    CHECK(onStore(data, [&](engine::app::IDataStore& store) { return store.counts(); }).moves == 1);
    // Two days on -- past the next hourly read -- it goes.
    for (int frame = 0; frame < 3; ++frame)
        frameOf(console, data, now + 2 * 86400);
    CHECK(onStore(data, [&](engine::app::IDataStore& store) { return store.counts(); }).moves == 0);
    CHECK(onStore(data, [&](engine::app::IDataStore& store) { return store.balance("ana", "Gold"); }) == 5);
}

#if ENG_ENABLE_DATA
TEST_CASE("data console > a scheduled backup is taken, counted from the newest, and the oldest pruned")
{
    using engine::app::DataConsole;
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "engine-data-schedule-test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "backups");
    std::string error;
    auto store = engine::app::openDataStore(folder / "game.db", error);
    REQUIRE_MESSAGE(store != nullptr, error);
    DataBroker data(std::move(store));
    const std::int64_t now = secondsNow();
    // Three older backups, and a file that is not one of them.
    for (const std::int64_t ago : {std::int64_t{30000}, std::int64_t{20000}, std::int64_t{10000}})
        std::ofstream(folder / "backups" / DataConsole::backupName("g", now - ago)) << "old";
    std::ofstream(folder / "backups" / "notes.txt") << "mine";

    DataConsole console(consoleOptions(folder / "backups", "g", 2.0, 2, 90));
    // The newest is under three hours old, two hours a period: one is due.
    for (int frame = 0; frame < 50; ++frame)
        frameOf(console, data, now);
    const auto names = [&]() {
        std::vector<std::string> found;
        for (const auto& entry : std::filesystem::directory_iterator(folder / "backups"))
            found.push_back(entry.path().filename().string());
        std::sort(found.begin(), found.end());
        return found;
    };
    const std::vector<std::string> kept = names();
    REQUIRE(kept.size() == 3);
    CHECK(kept[0] == DataConsole::backupName("g", now - 10000));
    CHECK(kept[1] == DataConsole::backupName("g", now));
    CHECK(kept[2] == "notes.txt");
    // The next is two hours after this one, not before.
    for (int frame = 0; frame < 5; ++frame)
        frameOf(console, data, now + 3600);
    CHECK(names().size() == 3);
}
#endif
