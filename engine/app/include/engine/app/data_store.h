// **Where an online game's data is kept** (ADR 0238): a player's record, the
// items and the money that change hands, in one database the manager opens --
// or a server with no manager opens itself -- and no other process does.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"

namespace engine::app {

// A player's record as the store keeps it: the text a world wrote, in the
// format `SaveService` writes a slot in, and the game's version it was
// written at.
struct StoredRecord
{
    std::string body;
    core::f64 version = 1.0;

    [[nodiscard]] bool operator==(const StoredRecord&) const = default;
};

// **One move of a transaction** (ADR 0238 §3): an item from one owner to
// another, or an amount of a currency. An empty owner is the game itself:
// money it pays comes from nowhere, and money paid to it is gone.
struct DataMove
{
    core::i64 item = 0;
    std::string currency;
    core::i64 amount = 0;
    std::string from;
    std::string to;
};

// An item: its id, never reused; its kind; its owner; and its fields, as a
// saved value's text.
struct DataItem
{
    core::i64 id = 0;
    std::string kind;
    std::string owner;
    std::string fields;
};

// What a transaction did: every move, or none -- and why none.
struct DataTransaction
{
    bool ok = false;
    // `ItemNotThere`, `NotEnough`, `Invalid` or `Failed`.
    std::string reason;
    core::i64 id = 0;
};

// **A value no player owns** (ADR 0238 §4) -- an auction's listings, a
// world event's progress -- as a saved value's text, and its version: every
// write makes the next, and 0 is a value never written.
struct SharedDatum
{
    std::string value;
    core::i64 version = 0;
};

// What a write of a shared value did: written, or refused because another
// world wrote first -- and the value as it is now, either way.
struct SharedWrite
{
    bool ok = false;
    SharedDatum now;
};

// **A line of the move log** (ADR 0238 §3): when, which transaction, which
// world, and the move. An item made comes from the game (an empty `from`) and
// one destroyed goes to it (an empty `to`).
struct DataMoveLine
{
    core::i64 id = 0;
    // Unix seconds.
    core::i64 at = 0;
    core::i64 transaction = 0;
    std::string world;
    DataMove move;
};

// Which lines of the move log to read, newest first: those of one account
// or name, of one item or of one transaction, or with none of them named,
// all of them.
struct DataMoveQuery
{
    std::string owner;
    core::i64 item = 0;
    core::i64 transaction = 0;
    core::usize most = 20;
};

// **An account** (ADR 0237 §3): a random id never reused, the name it signs
// in with, the name's skeleton -- what two names that look alike share, and
// what makes an account's name its own -- and its password's hash.
struct DataAccount
{
    std::string id;
    std::string name;
    std::string skeleton;
    std::string hash;
    // Unix seconds; 0 for never.
    core::i64 made = 0;
    core::i64 signedIn = 0;
};

// What the console's `data` says of the store.
struct DataCounts
{
    core::i64 schema = 0;
    core::i64 accounts = 0;
    core::i64 records = 0;
    core::i64 items = 0;
    core::i64 transactions = 0;
    core::i64 moves = 0;
    // The file and its journal, on the disk; 0 for a store in memory.
    core::u64 bytes = 0;
};

// **How far an online backup has gone** (ADR 0238 §7).
struct DataBackupProgress
{
    enum class State : core::u8
    {
        // None asked for, or the last one already said how it ended.
        Idle,
        Copying,
        Done,
        Failed,
    };
    State state = State::Idle;
    core::i64 copied = 0;
    core::i64 total = 0;
    std::filesystem::path file;
    std::string error;
};

class IDataStore
{
public:
    virtual ~IDataStore() = default;
    // The record kept for `account`; none for an account with no record yet.
    [[nodiscard]] virtual std::optional<StoredRecord> loadRecord(std::string_view account) = 0;
    // Replaces it, durably before it returns; false where the store could not.
    [[nodiscard]] virtual bool writeRecord(std::string_view account, const StoredRecord& record) = 0;

    // **Every move, or none**, on the disk before it returns, and each one a
    // line of the move log with the transaction and the world. Refused where
    // an item is not where a move says it is, or a balance would fall below
    // nought.
    [[nodiscard]] virtual DataTransaction transact(std::string_view world, const std::vector<DataMove>& moves) = 0;
    // A new item, and its id; none where the store failed. Logged as a move
    // from the game.
    [[nodiscard]] virtual std::optional<core::i64> createItem(std::string_view world, std::string_view owner,
                                                              std::string_view kind, std::string_view fields) = 0;
    // Gone, logged as a move to the game; false where there is no such item.
    [[nodiscard]] virtual bool destroyItem(std::string_view world, core::i64 id) = 0;
    [[nodiscard]] virtual std::vector<DataItem> items(std::string_view owner) = 0;
    [[nodiscard]] virtual core::i64 balance(std::string_view owner, std::string_view currency) = 0;

    [[nodiscard]] virtual SharedDatum getShared(std::string_view scope, std::string_view key) = 0;
    // Written only while its version is still `version`, on the disk before
    // it returns.
    [[nodiscard]] virtual SharedWrite setShared(std::string_view scope, std::string_view key, std::string_view value,
                                                core::i64 version) = 0;

    // **The move log, read** (ADR 0238 §3): what a lost or duplicated item
    // is traced by.
    [[nodiscard]] virtual std::vector<DataMoveLine> moves(const DataMoveQuery& query) = 0;
    // **A transaction undone**: its moves made the other way, last first, as
    // one new transaction of `world`'s -- an item it made destroyed, one it
    // destroyed made again under its own id. All or nothing, as any
    // transaction: refused with `NoSuchTransaction` where the log no longer
    // has it, `AlreadyUndone`, or `ItemNotThere` and `NotEnough` where what
    // it moved has moved on since.
    [[nodiscard]] virtual DataTransaction undo(std::string_view world, core::i64 transaction) = 0;
    // The move log's lines, and their transactions, from before `unixSeconds`
    // let go of -- the oldest `most` transactions of them, so a long log goes
    // a little at a time; how many lines went.
    virtual core::i64 forgetMovesBefore(core::i64 unixSeconds, core::i64 most) = 0;
    [[nodiscard]] virtual DataCounts counts() = 0;

    // **An online backup** (ADR 0238 §7): the whole database copied to
    // `file` a few pages at a time, between the store's other work, so
    // nothing waits on it; the copy is consistent as of the moment it
    // finished, and it is written beside `file` and renamed to it only then.
    // One at a time: false, and `error` saying why, while one is copying, and
    // for a store in memory.
    [[nodiscard]] virtual bool startBackup(const std::filesystem::path& file, std::string& error) = 0;
    // Copies up to `pages` more of a backup that is copying. Done or Failed
    // is said once; it is Idle after that.
    virtual DataBackupProgress stepBackup(int pages) = 0;

    // **Accounts** (ADR 0237 §3). Found by the skeleton of a name, or by id.
    [[nodiscard]] virtual std::optional<DataAccount> accountNamed(std::string_view skeleton) = 0;
    [[nodiscard]] virtual std::optional<DataAccount> accountWithId(std::string_view id) = 0;
    // False, and nothing made, where another account has the skeleton.
    [[nodiscard]] virtual bool createAccount(const DataAccount& account) = 0;
    // A name, a hash, a sign-in's time: false where there is no such
    // account, or -- for a name -- another has its skeleton.
    [[nodiscard]] virtual bool renameAccount(std::string_view id, std::string_view name, std::string_view skeleton) = 0;
    [[nodiscard]] virtual bool setAccountHash(std::string_view id, std::string_view hash) = 0;
    virtual void noteSignIn(std::string_view id, core::i64 at) = 0;
    // Gone; what it owned stays where it is. An id is 128 random bits, never
    // made twice.
    [[nodiscard]] virtual bool deleteAccount(std::string_view id) = 0;
};

// Kept in memory, for a test, and for a server with nowhere to write.
[[nodiscard]] std::unique_ptr<IDataStore> makeMemoryDataStore();

// **The game's database**: SQLite, with its journal ahead of the file and
// every commit on the disk before it returns. Null, and `error` saying why,
// where it cannot be opened, where its schema is newer than this build's, and
// in a build without the data service (`ENG_ENABLE_DATA`).
[[nodiscard]] std::unique_ptr<IDataStore> openDataStore(const std::filesystem::path& file, std::string& error);

} // namespace engine::app
