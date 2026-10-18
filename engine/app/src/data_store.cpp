#include "engine/app/data_store.h"

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <system_error>
#include <utility>

#include "engine/core/i18n.h"

#if ENG_ENABLE_DATA
#include <sqlite3.h>
#endif

namespace engine::app {
namespace {

#if ENG_ENABLE_DATA
// When a row was written: the database's alone, so a build without one has
// no use for it.
[[nodiscard]] core::i64 unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
#endif

// A move that names an item and nothing else, or an amount and its currency
// and nothing else, from one owner to another.
[[nodiscard]] bool wellFormed(const DataMove& move)
{
    if (move.from == move.to)
        return false;
    if (move.item != 0)
        return move.item > 0 && move.amount == 0 && move.currency.empty() && !move.to.empty() && !move.from.empty();
    return move.amount > 0 && !move.currency.empty();
}

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

    [[nodiscard]] DataTransaction transact(std::string_view world, const std::vector<DataMove>& moves) override
    {
        (void)world;
        // Applied to copies, kept only when every move holds.
        std::map<core::i64, DataItem> items = m_items;
        std::map<std::pair<std::string, std::string>, core::i64> balances = m_balances;
        for (const DataMove& move : moves) {
            if (!wellFormed(move))
                return {false, "Invalid", 0};
            if (move.item != 0) {
                const auto item = items.find(move.item);
                if (item == items.end() || item->second.owner != move.from)
                    return {false, "ItemNotThere", 0};
                item->second.owner = move.to;
                continue;
            }
            if (!move.from.empty()) {
                core::i64& from = balances[{move.from, move.currency}];
                if (from < move.amount)
                    return {false, "NotEnough", 0};
                from -= move.amount;
            }
            if (!move.to.empty())
                balances[{move.to, move.currency}] += move.amount;
        }
        m_items = std::move(items);
        m_balances = std::move(balances);
        return {true, {}, ++m_transactions};
    }

    [[nodiscard]] std::optional<core::i64> createItem(std::string_view world, std::string_view owner,
                                                      std::string_view kind, std::string_view fields) override
    {
        (void)world;
        const core::i64 id = ++m_lastItem;
        m_items.emplace(id, DataItem{id, std::string{kind}, std::string{owner}, std::string{fields}});
        return id;
    }

    [[nodiscard]] bool destroyItem(std::string_view world, core::i64 id) override
    {
        (void)world;
        return m_items.erase(id) > 0;
    }

    [[nodiscard]] std::vector<DataItem> items(std::string_view owner) override
    {
        std::vector<DataItem> owned;
        for (const auto& [id, item] : m_items) {
            if (item.owner == owner)
                owned.push_back(item);
        }
        return owned;
    }

    [[nodiscard]] core::i64 balance(std::string_view owner, std::string_view currency) override
    {
        const auto found = m_balances.find({std::string{owner}, std::string{currency}});
        return found != m_balances.end() ? found->second : 0;
    }

    [[nodiscard]] SharedDatum getShared(std::string_view scope, std::string_view key) override
    {
        const auto found = m_shared.find({std::string{scope}, std::string{key}});
        return found != m_shared.end() ? found->second : SharedDatum{};
    }

    [[nodiscard]] SharedWrite setShared(std::string_view scope, std::string_view key, std::string_view value,
                                        core::i64 version) override
    {
        SharedDatum& held = m_shared[{std::string{scope}, std::string{key}}];
        if (held.version != version)
            return {false, held};
        held = SharedDatum{std::string{value}, version + 1};
        return {true, held};
    }

private:
    std::map<std::pair<std::string, std::string>, SharedDatum> m_shared;
    std::map<std::string, StoredRecord, std::less<>> m_records;
    std::map<core::i64, DataItem> m_items;
    std::map<std::pair<std::string, std::string>, core::i64> m_balances;
    core::i64 m_lastItem = 0;
    core::i64 m_transactions = 0;
};

#if ENG_ENABLE_DATA

// **The schema's version**, in the file's `user_version`: a database written
// by a newer build is refused rather than read wrong. Each version's tables
// are made by the step that reaches it, so a database of any older version
// is brought up to this one where it is opened.
constexpr int SchemaVersion = 3;

// A prepared statement, reset and its bindings cleared when it goes out of use.
class Statement
{
public:
    Statement(sqlite3* database, const char* text)
    {
        if (sqlite3_prepare_v2(database, text, -1, &m_statement, nullptr) != SQLITE_OK)
            m_statement = nullptr;
    }
    ~Statement() { sqlite3_finalize(m_statement); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] bool ready() const noexcept { return m_statement != nullptr; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return m_statement; }

    Statement& text(int at, std::string_view value)
    {
        sqlite3_bind_text(m_statement, at, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& blob(int at, std::string_view value)
    {
        sqlite3_bind_blob(m_statement, at, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& integer(int at, core::i64 value)
    {
        sqlite3_bind_int64(m_statement, at, static_cast<sqlite3_int64>(value));
        return *this;
    }
    Statement& null(int at)
    {
        sqlite3_bind_null(m_statement, at);
        return *this;
    }
    // One step: `SQLITE_ROW`, `SQLITE_DONE` or an error.
    [[nodiscard]] int step() { return sqlite3_step(m_statement); }
    void reset()
    {
        sqlite3_reset(m_statement);
        sqlite3_clear_bindings(m_statement);
    }

private:
    sqlite3_stmt* m_statement = nullptr;
};

[[nodiscard]] std::string columnText(sqlite3_stmt* statement, int at)
{
    const auto* text = static_cast<const char*>(sqlite3_column_blob(statement, at));
    return std::string(text != nullptr ? text : "", static_cast<std::size_t>(sqlite3_column_bytes(statement, at)));
}

class SqliteDataStore final : public IDataStore
{
public:
    explicit SqliteDataStore(sqlite3* database) : m_database(database) {}

    ~SqliteDataStore() override
    {
        m_statements.clear();
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
            Statement read(m_database, "PRAGMA user_version");
            if (!read.ready())
                return said(error);
            if (read.step() == SQLITE_ROW)
                version = sqlite3_column_int(read.get(), 0);
        }
        if (version > SchemaVersion) {
            const std::array<core::I18nArg, 2> args{core::I18nArg{"schema", static_cast<core::i64>(version)},
                                                    core::I18nArg{"ours", static_cast<core::i64>(SchemaVersion)}};
            error = core::engineCatalog().format(ENG_TR("data.err.newer_schema"), args);
            return false;
        }
        // **Each version's tables, in order**, each step one transaction.
        if (version < 1 && !step(error,
                                 "CREATE TABLE IF NOT EXISTS records (account TEXT PRIMARY KEY, body BLOB NOT NULL, "
                                 "version REAL NOT NULL, written INTEGER NOT NULL) WITHOUT ROWID",
                                 1))
            return false;
        // Version 2 (ADR 0238 §3): items, never reusing an id; balances;
        // and every move, by its transaction.
        if (version < 2) {
            if (!run("BEGIN IMMEDIATE", error) ||
                !run("CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, "
                     "owner TEXT NOT NULL, fields BLOB NOT NULL)",
                     error) ||
                !run("CREATE INDEX items_owner ON items (owner)", error) ||
                !run("CREATE TABLE balances (owner TEXT NOT NULL, currency TEXT NOT NULL, amount INTEGER NOT NULL, "
                     "PRIMARY KEY (owner, currency)) WITHOUT ROWID",
                     error) ||
                !run("CREATE TABLE transactions (id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, "
                     "world TEXT NOT NULL)",
                     error) ||
                !run("CREATE TABLE moves (id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, "
                     "transaction_id INTEGER NOT NULL, world TEXT NOT NULL, item INTEGER, currency TEXT, "
                     "amount INTEGER, from_owner TEXT NOT NULL, to_owner TEXT NOT NULL)",
                     error) ||
                !run("CREATE INDEX moves_transaction ON moves (transaction_id)", error) ||
                !run("PRAGMA user_version=2", error) || !run("COMMIT", error)) {
                std::string ignored;
                (void)run("ROLLBACK", ignored);
                return false;
            }
        }
        // Version 3 (ADR 0238 §4): values no player owns, each with its
        // version.
        if (version < 3 && !step(error,
                                 "CREATE TABLE shared (scope TEXT NOT NULL, key TEXT NOT NULL, value BLOB NOT NULL, "
                                 "version INTEGER NOT NULL, PRIMARY KEY (scope, key)) WITHOUT ROWID",
                                 3))
            return false;
        const std::array<const char*, Count> texts{
            "SELECT body, version FROM records WHERE account = ?1",
            "INSERT INTO records (account, body, version, written) VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT (account) DO UPDATE SET body = excluded.body, version = excluded.version, "
            "written = excluded.written",
            "INSERT INTO transactions (at, world) VALUES (?1, ?2)",
            "UPDATE items SET owner = ?1 WHERE id = ?2 AND owner = ?3",
            "UPDATE balances SET amount = amount - ?1 WHERE owner = ?2 AND currency = ?3 AND amount >= ?1",
            "INSERT INTO balances (owner, currency, amount) VALUES (?1, ?2, ?3) "
            "ON CONFLICT (owner, currency) DO UPDATE SET amount = amount + excluded.amount",
            "INSERT INTO moves (at, transaction_id, world, item, currency, amount, from_owner, to_owner) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
            "INSERT INTO items (kind, owner, fields) VALUES (?1, ?2, ?3)",
            "SELECT owner FROM items WHERE id = ?1",
            "DELETE FROM items WHERE id = ?1",
            "SELECT id, kind, owner, fields FROM items WHERE owner = ?1 ORDER BY id",
            "SELECT amount FROM balances WHERE owner = ?1 AND currency = ?2",
            "SELECT value, version FROM shared WHERE scope = ?1 AND key = ?2",
            "INSERT INTO shared (scope, key, value, version) VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT (scope, key) DO UPDATE SET value = excluded.value, version = excluded.version",
        };
        for (const char* text : texts) {
            auto statement = std::make_unique<Statement>(m_database, text);
            if (!statement->ready())
                return said(error);
            m_statements.push_back(std::move(statement));
        }
        return true;
    }

    [[nodiscard]] std::optional<StoredRecord> loadRecord(std::string_view account) override
    {
        Statement& load = use(LoadRecord);
        load.text(1, account);
        std::optional<StoredRecord> record;
        if (load.step() == SQLITE_ROW)
            record = StoredRecord{columnText(load.get(), 0), sqlite3_column_double(load.get(), 1)};
        load.reset();
        return record;
    }

    [[nodiscard]] bool writeRecord(std::string_view account, const StoredRecord& record) override
    {
        Statement& write = use(WriteRecord);
        write.text(1, account).blob(2, record.body).integer(4, unixNow());
        sqlite3_bind_double(write.get(), 3, record.version);
        const bool done = write.step() == SQLITE_DONE;
        write.reset();
        return done;
    }

    [[nodiscard]] DataTransaction transact(std::string_view world, const std::vector<DataMove>& moves) override
    {
        for (const DataMove& move : moves) {
            if (!wellFormed(move))
                return {false, "Invalid", 0};
        }
        std::string ignored;
        if (!run("BEGIN IMMEDIATE", ignored))
            return {false, "Failed", 0};
        const std::optional<core::i64> id = begin(world);
        if (!id.has_value())
            return rollback("Failed");
        for (const DataMove& move : moves) {
            if (move.item != 0) {
                Statement& give = use(MoveItem);
                give.text(1, move.to).integer(2, move.item).text(3, move.from);
                const bool moved = give.step() == SQLITE_DONE && sqlite3_changes(m_database) == 1;
                give.reset();
                if (!moved)
                    return rollback("ItemNotThere");
            }
            else {
                if (!move.from.empty()) {
                    Statement& take = use(TakeMoney);
                    take.integer(1, move.amount).text(2, move.from).text(3, move.currency);
                    const bool taken = take.step() == SQLITE_DONE && sqlite3_changes(m_database) == 1;
                    take.reset();
                    if (!taken)
                        return rollback("NotEnough");
                }
                if (!move.to.empty()) {
                    Statement& give = use(GiveMoney);
                    give.text(1, move.to).text(2, move.currency).integer(3, move.amount);
                    const bool given = give.step() == SQLITE_DONE;
                    give.reset();
                    if (!given)
                        return rollback("Failed");
                }
            }
            if (!logMove(*id, world, move))
                return rollback("Failed");
        }
        if (!run("COMMIT", ignored))
            return rollback("Failed");
        return {true, {}, *id};
    }

    [[nodiscard]] std::optional<core::i64> createItem(std::string_view world, std::string_view owner,
                                                      std::string_view kind, std::string_view fields) override
    {
        std::string ignored;
        if (owner.empty() || !run("BEGIN IMMEDIATE", ignored))
            return std::nullopt;
        Statement& insert = use(InsertItem);
        insert.text(1, kind).text(2, owner).blob(3, fields);
        const bool made = insert.step() == SQLITE_DONE;
        insert.reset();
        const core::i64 item = sqlite3_last_insert_rowid(m_database);
        const std::optional<core::i64> transaction = made ? begin(world) : std::nullopt;
        if (!transaction.has_value() || !logMove(*transaction, world, DataMove{item, {}, 0, {}, std::string{owner}}) ||
            !run("COMMIT", ignored)) {
            (void)run("ROLLBACK", ignored);
            return std::nullopt;
        }
        return item;
    }

    [[nodiscard]] bool destroyItem(std::string_view world, core::i64 id) override
    {
        std::string ignored;
        if (!run("BEGIN IMMEDIATE", ignored))
            return false;
        Statement& owner = use(ItemOwner);
        owner.integer(1, id);
        std::optional<std::string> had;
        if (owner.step() == SQLITE_ROW)
            had = columnText(owner.get(), 0);
        owner.reset();
        bool done = had.has_value();
        if (done) {
            Statement& remove = use(DeleteItem);
            remove.integer(1, id);
            done = remove.step() == SQLITE_DONE;
            remove.reset();
        }
        const std::optional<core::i64> transaction = done ? begin(world) : std::nullopt;
        if (!transaction.has_value() || !logMove(*transaction, world, DataMove{id, {}, 0, *had, {}}) ||
            !run("COMMIT", ignored)) {
            (void)run("ROLLBACK", ignored);
            return false;
        }
        return true;
    }

    [[nodiscard]] std::vector<DataItem> items(std::string_view owner) override
    {
        Statement& select = use(ItemsOf);
        select.text(1, owner);
        std::vector<DataItem> owned;
        while (select.step() == SQLITE_ROW) {
            owned.push_back(DataItem{sqlite3_column_int64(select.get(), 0), columnText(select.get(), 1),
                                     columnText(select.get(), 2), columnText(select.get(), 3)});
        }
        select.reset();
        return owned;
    }

    [[nodiscard]] core::i64 balance(std::string_view owner, std::string_view currency) override
    {
        Statement& select = use(BalanceOf);
        select.text(1, owner).text(2, currency);
        const core::i64 amount = select.step() == SQLITE_ROW ? sqlite3_column_int64(select.get(), 0) : 0;
        select.reset();
        return amount;
    }

    [[nodiscard]] SharedDatum getShared(std::string_view scope, std::string_view key) override
    {
        Statement& select = use(SharedOf);
        select.text(1, scope).text(2, key);
        SharedDatum held;
        if (select.step() == SQLITE_ROW)
            held = SharedDatum{columnText(select.get(), 0), sqlite3_column_int64(select.get(), 1)};
        select.reset();
        return held;
    }

    [[nodiscard]] SharedWrite setShared(std::string_view scope, std::string_view key, std::string_view value,
                                        core::i64 version) override
    {
        // Read and written in one transaction: no other write comes between.
        std::string ignored;
        if (!run("BEGIN IMMEDIATE", ignored))
            return {false, getShared(scope, key)};
        const SharedDatum now = getShared(scope, key);
        if (now.version != version) {
            (void)run("ROLLBACK", ignored);
            return {false, now};
        }
        Statement& write = use(WriteShared);
        write.text(1, scope).text(2, key).blob(3, value).integer(4, version + 1);
        const bool written = write.step() == SQLITE_DONE;
        write.reset();
        if (!written || !run("COMMIT", ignored)) {
            (void)run("ROLLBACK", ignored);
            return {false, now};
        }
        return {true, SharedDatum{std::string{value}, version + 1}};
    }

private:
    enum Which : std::size_t
    {
        LoadRecord,
        WriteRecord,
        BeginTransaction,
        MoveItem,
        TakeMoney,
        GiveMoney,
        LogMove,
        InsertItem,
        ItemOwner,
        DeleteItem,
        ItemsOf,
        BalanceOf,
        SharedOf,
        WriteShared,
        Count,
    };

    [[nodiscard]] Statement& use(Which which) { return *m_statements[which]; }

    // A transaction's row: its id, the move log's key.
    [[nodiscard]] std::optional<core::i64> begin(std::string_view world)
    {
        Statement& insert = use(BeginTransaction);
        insert.integer(1, unixNow()).text(2, world);
        const bool done = insert.step() == SQLITE_DONE;
        insert.reset();
        if (!done)
            return std::nullopt;
        return sqlite3_last_insert_rowid(m_database);
    }

    [[nodiscard]] bool logMove(core::i64 transaction, std::string_view world, const DataMove& move)
    {
        Statement& insert = use(LogMove);
        insert.integer(1, unixNow()).integer(2, transaction).text(3, world);
        if (move.item != 0)
            insert.integer(4, move.item).null(5).null(6);
        else
            insert.null(4).text(5, move.currency).integer(6, move.amount);
        insert.text(7, move.from).text(8, move.to);
        const bool done = insert.step() == SQLITE_DONE;
        insert.reset();
        return done;
    }

    [[nodiscard]] DataTransaction rollback(std::string reason)
    {
        std::string ignored;
        (void)run("ROLLBACK", ignored);
        return {false, std::move(reason), 0};
    }

    [[nodiscard]] bool step(std::string& error, const char* create, int version)
    {
        if (!run("BEGIN IMMEDIATE", error) || !run(create, error) ||
            !run(("PRAGMA user_version=" + std::to_string(version)).c_str(), error) || !run("COMMIT", error)) {
            std::string ignored;
            (void)run("ROLLBACK", ignored);
            return false;
        }
        return true;
    }

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
    std::vector<std::unique_ptr<Statement>> m_statements;
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
