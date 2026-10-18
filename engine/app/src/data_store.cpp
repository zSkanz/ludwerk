#include "engine/app/data_store.h"

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <system_error>

#include "engine/core/i18n.h"

#if ENG_ENABLE_DATA
#include <sqlite3.h>
#endif

namespace engine::app {
namespace {

class MemoryDataStore final : public IDataStore
{
public:
    [[nodiscard]] std::optional<StoredRecord> loadRecord(std::string_view account) override
    {
        const auto found = m_records.find(account);
        if (found == m_records.end())
            return std::nullopt;
        return found->second;
    }

    [[nodiscard]] bool writeRecord(std::string_view account, const StoredRecord& record) override
    {
        m_records.insert_or_assign(std::string{account}, record);
        return true;
    }

private:
    std::map<std::string, StoredRecord, std::less<>> m_records;
};

#if ENG_ENABLE_DATA

// **The schema's version**, in the file's `user_version`: a database written
// by a newer build is refused rather than read wrong. Each version's tables
// are made by the step that reaches it, so a database of any older version
// is brought up to this one where it is opened.
constexpr int SchemaVersion = 1;

class SqliteDataStore final : public IDataStore
{
public:
    explicit SqliteDataStore(sqlite3* database) : m_database(database) {}

    ~SqliteDataStore() override
    {
        sqlite3_finalize(m_load);
        sqlite3_finalize(m_write);
        sqlite3_close(m_database);
    }

    SqliteDataStore(const SqliteDataStore&) = delete;
    SqliteDataStore& operator=(const SqliteDataStore&) = delete;

    [[nodiscard]] bool open(std::string& error)
    {
        // One process opens the file, so the journal ahead of it costs no
        // locking; and a commit is on the disk before it returns.
        if (!run("PRAGMA journal_mode=WAL", error) || !run("PRAGMA synchronous=FULL", error))
            return false;
        int version = 0;
        {
            sqlite3_stmt* read = nullptr;
            if (sqlite3_prepare_v2(m_database, "PRAGMA user_version", -1, &read, nullptr) != SQLITE_OK)
                return said(error);
            if (sqlite3_step(read) == SQLITE_ROW)
                version = sqlite3_column_int(read, 0);
            sqlite3_finalize(read);
        }
        if (version > SchemaVersion) {
            const std::array<core::I18nArg, 2> args{core::I18nArg{"schema", static_cast<core::i64>(version)},
                                                    core::I18nArg{"ours", static_cast<core::i64>(SchemaVersion)}};
            error = core::engineCatalog().format(ENG_TR("data.err.newer_schema"), args);
            return false;
        }
        if (version < 1) {
            if (!run("BEGIN IMMEDIATE", error) ||
                !run("CREATE TABLE IF NOT EXISTS records (account TEXT PRIMARY KEY, body BLOB NOT NULL, "
                     "version REAL NOT NULL, written INTEGER NOT NULL) WITHOUT ROWID",
                     error) ||
                !run("PRAGMA user_version=1", error) || !run("COMMIT", error))
                return false;
        }
        if (sqlite3_prepare_v2(m_database, "SELECT body, version FROM records WHERE account = ?1", -1, &m_load,
                               nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(m_database,
                               "INSERT INTO records (account, body, version, written) VALUES (?1, ?2, ?3, ?4) "
                               "ON CONFLICT (account) DO UPDATE SET body = excluded.body, "
                               "version = excluded.version, written = excluded.written",
                               -1, &m_write, nullptr) != SQLITE_OK)
            return said(error);
        return true;
    }

    [[nodiscard]] std::optional<StoredRecord> loadRecord(std::string_view account) override
    {
        sqlite3_reset(m_load);
        sqlite3_bind_text(m_load, 1, account.data(), static_cast<int>(account.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(m_load) != SQLITE_ROW)
            return std::nullopt;
        StoredRecord record;
        const auto* body = static_cast<const char*>(sqlite3_column_blob(m_load, 0));
        record.body.assign(body != nullptr ? body : "", static_cast<std::size_t>(sqlite3_column_bytes(m_load, 0)));
        record.version = sqlite3_column_double(m_load, 1);
        sqlite3_reset(m_load);
        return record;
    }

    [[nodiscard]] bool writeRecord(std::string_view account, const StoredRecord& record) override
    {
        const auto written =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        sqlite3_reset(m_write);
        sqlite3_bind_text(m_write, 1, account.data(), static_cast<int>(account.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(m_write, 2, record.body.data(), static_cast<int>(record.body.size()), SQLITE_TRANSIENT);
        sqlite3_bind_double(m_write, 3, record.version);
        sqlite3_bind_int64(m_write, 4, static_cast<sqlite3_int64>(written));
        const bool done = sqlite3_step(m_write) == SQLITE_DONE;
        sqlite3_reset(m_write);
        return done;
    }

private:
    [[nodiscard]] bool said(std::string& error) const
    {
        error = sqlite3_errmsg(m_database);
        return false;
    }

    [[nodiscard]] bool run(const char* statement, std::string& error) const
    {
        char* message = nullptr;
        if (sqlite3_exec(m_database, statement, nullptr, nullptr, &message) == SQLITE_OK)
            return true;
        error = message != nullptr ? message : sqlite3_errmsg(m_database);
        sqlite3_free(message);
        return false;
    }

    sqlite3* m_database = nullptr;
    sqlite3_stmt* m_load = nullptr;
    sqlite3_stmt* m_write = nullptr;
};

#endif

} // namespace

std::unique_ptr<IDataStore> makeMemoryDataStore()
{
    return std::make_unique<MemoryDataStore>();
}

std::unique_ptr<IDataStore> openDataStore(const std::filesystem::path& file, std::string& error)
{
#if ENG_ENABLE_DATA
    std::error_code ignored;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ignored);
    const std::u8string name = file.u8string();
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        error = database != nullptr ? std::string{sqlite3_errmsg(database)}
                                    : core::engineCatalog().format(ENG_TR("data.err.no_memory"));
        sqlite3_close(database);
        return nullptr;
    }
    auto store = std::make_unique<SqliteDataStore>(database);
    if (!store->open(error))
        return nullptr;
    return store;
#else
    (void)file;
    error = core::engineCatalog().format(ENG_TR("data.err.not_in_build"));
    return nullptr;
#endif
}

} // namespace engine::app
