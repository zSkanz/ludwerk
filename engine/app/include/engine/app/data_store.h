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
};

// Kept in memory, for a test, and for a server with nowhere to write.
[[nodiscard]] std::unique_ptr<IDataStore> makeMemoryDataStore();

// **The game's database**: SQLite, with its journal ahead of the file and
// every commit on the disk before it returns. Null, and `error` saying why,
// where it cannot be opened, where its schema is newer than this build's, and
// in a build without the data service (`ENG_ENABLE_DATA`).
[[nodiscard]] std::unique_ptr<IDataStore> openDataStore(const std::filesystem::path& file, std::string& error);

} // namespace engine::app
