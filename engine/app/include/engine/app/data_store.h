// **Where an online game's data is kept** (ADR 0238): a player's record,
// in one database the manager opens -- or a server with no manager opens
// itself -- and no other process does.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

class IDataStore
{
public:
    virtual ~IDataStore() = default;
    // The record kept for `account`; none for an account with no record yet.
    [[nodiscard]] virtual std::optional<StoredRecord> loadRecord(std::string_view account) = 0;
    // Replaces it, durably before it returns; false where the store could not.
    [[nodiscard]] virtual bool writeRecord(std::string_view account, const StoredRecord& record) = 0;
};

// Kept in memory, for a test, and for a server with nowhere to write.
[[nodiscard]] std::unique_ptr<IDataStore> makeMemoryDataStore();

// **The game's database**: SQLite, with its journal ahead of the file and
// every commit on the disk before it returns. Null, and `error` saying why,
// where it cannot be opened, where its schema is newer than this build's, and
// in a build without the data service (`ENG_ENABLE_DATA`).
[[nodiscard]] std::unique_ptr<IDataStore> openDataStore(const std::filesystem::path& file, std::string& error);

} // namespace engine::app
