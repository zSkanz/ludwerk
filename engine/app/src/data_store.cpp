#include "engine/app/data_store.h"

#include <algorithm>
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

// When a row was written. The operator's clock, never the simulation's: the
// move log is read by a person tracing an item, and kept for so many days.
[[nodiscard]] core::i64 unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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

// **A move, the other way**: what undoing it makes. An item made is
// destroyed and one destroyed made again, which the stores do themselves.
[[nodiscard]] DataMove reversed(const DataMove& move)
{
    DataMove back = move;
    std::swap(back.from, back.to);
    return back;
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
        // Applied to copies, kept only when every move holds.
        Books books{m_items, m_balances};
        for (const DataMove& move : moves) {
            if (!wellFormed(move))
                return {false, "Invalid", 0};
            if (const char* refused = apply(books, move); refused != nullptr)
                return {false, refused, 0};
        }
        m_items = std::move(books.items);
        m_balances = std::move(books.balances);
        const core::i64 id = begin(world, 0);
        for (const DataMove& move : moves)
            log(id, world, move, {}, {});
        return {true, {}, id};
    }

    [[nodiscard]] std::optional<core::i64> createItem(std::string_view world, std::string_view owner,
                                                      std::string_view kind, std::string_view fields) override
    {
        if (owner.empty())
            return std::nullopt;
        const core::i64 id = ++m_lastItem;
        m_items.emplace(id, DataItem{id, std::string{kind}, std::string{owner}, std::string{fields}});
        log(begin(world, 0), world, DataMove{id, {}, 0, {}, std::string{owner}}, kind, fields);
        return id;
    }

    [[nodiscard]] bool destroyItem(std::string_view world, core::i64 id) override
    {
        const auto found = m_items.find(id);
        if (found == m_items.end())
            return false;
        const DataItem gone = found->second;
        m_items.erase(found);
        log(begin(world, 0), world, DataMove{id, {}, 0, gone.owner, {}}, gone.kind, gone.fields);
        return true;
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

    [[nodiscard]] std::vector<DataMoveLine> moves(const DataMoveQuery& query) override
    {
        std::vector<DataMoveLine> found;
        for (auto line = m_log.rbegin(); line != m_log.rend() && found.size() < query.most; ++line) {
            const DataMove& move = line->line.move;
            if (!query.owner.empty() && move.from != query.owner && move.to != query.owner)
                continue;
            if (query.item != 0 && move.item != query.item)
                continue;
            if (query.transaction != 0 && line->line.transaction != query.transaction)
                continue;
            found.push_back(line->line);
        }
        return found;
    }

    [[nodiscard]] DataTransaction undo(std::string_view world, core::i64 transaction) override
    {
        const auto known = m_transactions.find(transaction);
        if (known == m_transactions.end())
            return {false, "NoSuchTransaction", 0};
        if (known->second.undoneBy != 0)
            return {false, "AlreadyUndone", 0};
        std::vector<const Logged*> lines;
        for (const Logged& line : m_log) {
            if (line.line.transaction == transaction)
                lines.push_back(&line);
        }
        if (lines.empty())
            return {false, "NoSuchTransaction", 0};
        Books books{m_items, m_balances};
        // What each undone move left behind, for the log: an item destroyed
        // as it was then.
        std::vector<Logged> undone;
        for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
            const DataMove& move = (*line)->line.move;
            Logged back{DataMoveLine{0, 0, 0, std::string{world}, reversed(move)}, (*line)->kind, (*line)->fields};
            if (move.item != 0 && move.from.empty()) {
                const auto item = books.items.find(move.item);
                if (item == books.items.end() || item->second.owner != move.to)
                    return {false, "ItemNotThere", 0};
                back.kind = item->second.kind;
                back.fields = item->second.fields;
                books.items.erase(item);
            }
            else if (move.item != 0 && move.to.empty()) {
                if (books.items.contains(move.item))
                    return {false, "ItemNotThere", 0};
                books.items.emplace(move.item, DataItem{move.item, (*line)->kind, move.from, (*line)->fields});
            }
            else if (const char* refused = apply(books, back.line.move); refused != nullptr) {
                return {false, refused, 0};
            }
            undone.push_back(std::move(back));
        }
        m_items = std::move(books.items);
        m_balances = std::move(books.balances);
        const core::i64 id = begin(world, transaction);
        for (const Logged& line : undone)
            log(id, world, line.line.move, line.kind, line.fields);
        known->second.undoneBy = id;
        return {true, {}, id};
    }

    core::i64 forgetMovesBefore(core::i64 unixSeconds, core::i64 most) override
    {
        core::i64 forgotten = 0;
        core::i64 transactions = 0;
        for (auto at = m_transactions.begin(); at != m_transactions.end() && transactions < most;) {
            if (at->second.at >= unixSeconds) {
                ++at;
                continue;
            }
            const core::i64 id = at->first;
            forgotten += static_cast<core::i64>(
                std::erase_if(m_log, [id](const Logged& line) { return line.line.transaction == id; }));
            at = m_transactions.erase(at);
            ++transactions;
        }
        return forgotten;
    }

    [[nodiscard]] DataCounts counts() override
    {
        return DataCounts{0,
                          static_cast<core::i64>(m_records.size()),
                          static_cast<core::i64>(m_items.size()),
                          static_cast<core::i64>(m_transactions.size()),
                          static_cast<core::i64>(m_log.size()),
                          0};
    }

    [[nodiscard]] bool startBackup(const std::filesystem::path& file, std::string& error) override
    {
        (void)file;
        error = core::engineCatalog().format(ENG_TR("data.err.backup_in_memory"));
        return false;
    }

    DataBackupProgress stepBackup(int pages) override
    {
        (void)pages;
        return {};
    }

private:
    using Balances = std::map<std::pair<std::string, std::string>, core::i64>;

    // What a transaction changes, as copies until it holds.
    struct Books
    {
        std::map<core::i64, DataItem> items;
        Balances balances;
    };

    struct Logged
    {
        DataMoveLine line;
        // An item made or destroyed: what it was, to make it again.
        std::string kind;
        std::string fields;
    };

    struct Transaction
    {
        core::i64 at = 0;
        core::i64 undoneBy = 0;
    };

    // One move of an item between owners, or of money; why not, or null.
    [[nodiscard]] static const char* apply(Books& books, const DataMove& move)
    {
        if (move.item != 0) {
            const auto item = books.items.find(move.item);
            if (item == books.items.end() || item->second.owner != move.from)
                return "ItemNotThere";
            item->second.owner = move.to;
            return nullptr;
        }
        if (!move.from.empty()) {
            core::i64& from = books.balances[{move.from, move.currency}];
            if (from < move.amount)
                return "NotEnough";
            from -= move.amount;
        }
        if (!move.to.empty())
            books.balances[{move.to, move.currency}] += move.amount;
        return nullptr;
    }

    core::i64 begin(std::string_view world, core::i64 undoes)
    {
        (void)world;
        (void)undoes;
        const core::i64 id = ++m_lastTransaction;
        m_transactions.emplace(id, Transaction{unixNow(), 0});
        return id;
    }

    void log(core::i64 transaction, std::string_view world, const DataMove& move, std::string_view kind,
             std::string_view fields)
    {
        m_log.push_back(Logged{DataMoveLine{++m_lastMove, unixNow(), transaction, std::string{world}, move},
                               std::string{kind}, std::string{fields}});
    }

    std::map<std::pair<std::string, std::string>, SharedDatum> m_shared;
    std::map<std::string, StoredRecord, std::less<>> m_records;
    std::map<core::i64, DataItem> m_items;
    Balances m_balances;
    std::map<core::i64, Transaction> m_transactions;
    std::vector<Logged> m_log;
    core::i64 m_lastItem = 0;
    core::i64 m_lastTransaction = 0;
    core::i64 m_lastMove = 0;
};

#if ENG_ENABLE_DATA

// **The schema's version**, in the file's `user_version`: a database written
// by a newer build is refused rather than read wrong. Each version's tables
// are made by the step that reaches it, so a database of any older version
// is brought up to this one where it is opened.
constexpr int SchemaVersion = 4;

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

[[nodiscard]] core::u64 sizeOf(const std::filesystem::path& file)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    return ec ? 0u : static_cast<core::u64>(size);
}

class SqliteDataStore final : public IDataStore
{
public:
    SqliteDataStore(sqlite3* database, std::filesystem::path file) : m_database(database), m_file(std::move(file)) {}

    ~SqliteDataStore() override
    {
        // A backup still copying is not a backup: what it wrote goes.
        if (m_backup.copy != nullptr)
            endBackup(false);
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
        if (version < 2 && !step(error,
                                 "CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, "
                                 "owner TEXT NOT NULL, fields BLOB NOT NULL);"
                                 "CREATE INDEX items_owner ON items (owner);"
                                 "CREATE TABLE balances (owner TEXT NOT NULL, currency TEXT NOT NULL, "
                                 "amount INTEGER NOT NULL, PRIMARY KEY (owner, currency)) WITHOUT ROWID;"
                                 "CREATE TABLE transactions (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                 "at INTEGER NOT NULL, world TEXT NOT NULL);"
                                 "CREATE TABLE moves (id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, "
                                 "transaction_id INTEGER NOT NULL, world TEXT NOT NULL, item INTEGER, currency TEXT, "
                                 "amount INTEGER, from_owner TEXT NOT NULL, to_owner TEXT NOT NULL);"
                                 "CREATE INDEX moves_transaction ON moves (transaction_id)",
                                 2))
            return false;
        // Version 3 (ADR 0238 §4): values no player owns, each with its
        // version.
        if (version < 3 && !step(error,
                                 "CREATE TABLE shared (scope TEXT NOT NULL, key TEXT NOT NULL, value BLOB NOT NULL, "
                                 "version INTEGER NOT NULL, PRIMARY KEY (scope, key)) WITHOUT ROWID",
                                 3))
            return false;
        // Version 4 (ADR 0238 §3, §7): a transaction says which it undoes and
        // which undid it; an item made or destroyed keeps what it was, so it
        // can be made again; and the log is read by owner and item, and let
        // go of by age.
        if (version < 4 && !step(error,
                                 "ALTER TABLE transactions ADD COLUMN undoes INTEGER;"
                                 "ALTER TABLE transactions ADD COLUMN undone_by INTEGER;"
                                 "CREATE INDEX transactions_at ON transactions (at);"
                                 "ALTER TABLE moves ADD COLUMN kind TEXT;"
                                 "ALTER TABLE moves ADD COLUMN fields BLOB;"
                                 "CREATE INDEX moves_item ON moves (item);"
                                 "CREATE INDEX moves_from ON moves (from_owner);"
                                 "CREATE INDEX moves_to ON moves (to_owner)",
                                 4))
            return false;
        const std::array<const char*, Count> texts{
            "SELECT body, version FROM records WHERE account = ?1",
            "INSERT INTO records (account, body, version, written) VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT (account) DO UPDATE SET body = excluded.body, version = excluded.version, "
            "written = excluded.written",
            "INSERT INTO transactions (at, world, undoes) VALUES (?1, ?2, ?3)",
            "UPDATE items SET owner = ?1 WHERE id = ?2 AND owner = ?3",
            "UPDATE balances SET amount = amount - ?1 WHERE owner = ?2 AND currency = ?3 AND amount >= ?1",
            "INSERT INTO balances (owner, currency, amount) VALUES (?1, ?2, ?3) "
            "ON CONFLICT (owner, currency) DO UPDATE SET amount = amount + excluded.amount",
            "INSERT INTO moves (at, transaction_id, world, item, currency, amount, from_owner, to_owner, kind, "
            "fields) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
            "INSERT INTO items (kind, owner, fields) VALUES (?1, ?2, ?3)",
            "SELECT kind, owner, fields FROM items WHERE id = ?1",
            "DELETE FROM items WHERE id = ?1",
            "SELECT id, kind, owner, fields FROM items WHERE owner = ?1 ORDER BY id",
            "SELECT amount FROM balances WHERE owner = ?1 AND currency = ?2",
            "SELECT value, version FROM shared WHERE scope = ?1 AND key = ?2",
            "INSERT INTO shared (scope, key, value, version) VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT (scope, key) DO UPDATE SET value = excluded.value, version = excluded.version",
            "SELECT undone_by FROM transactions WHERE id = ?1",
            "SELECT item, currency, amount, from_owner, to_owner, kind, fields FROM moves "
            "WHERE transaction_id = ?1 ORDER BY id",
            "UPDATE transactions SET undone_by = ?1 WHERE id = ?2",
            "INSERT INTO items (id, kind, owner, fields) VALUES (?1, ?2, ?3, ?4)",
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
        const std::optional<core::i64> id = begin(world, 0);
        if (!id.has_value())
            return rollback("Failed");
        for (const DataMove& move : moves) {
            if (const char* refused = apply(move); refused != nullptr)
                return rollback(refused);
            if (!logMove(*id, world, move, {}, {}))
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
        const std::optional<core::i64> transaction = made ? begin(world, 0) : std::nullopt;
        if (!transaction.has_value() ||
            !logMove(*transaction, world, DataMove{item, {}, 0, {}, std::string{owner}}, kind, fields) ||
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
        const std::optional<DataItem> had = itemRow(id);
        bool done = had.has_value();
        if (done) {
            Statement& remove = use(DeleteItem);
            remove.integer(1, id);
            done = remove.step() == SQLITE_DONE;
            remove.reset();
        }
        const std::optional<core::i64> transaction = done ? begin(world, 0) : std::nullopt;
        if (!transaction.has_value() ||
            !logMove(*transaction, world, DataMove{id, {}, 0, had->owner, {}}, had->kind, had->fields) ||
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

    [[nodiscard]] std::vector<DataMoveLine> moves(const DataMoveQuery& query) override
    {
        // One of four, by what the query names; each one an index.
        const char* text = "SELECT id, at, transaction_id, world, item, currency, amount, from_owner, to_owner "
                           "FROM moves ORDER BY id DESC LIMIT ?2";
        if (query.transaction != 0)
            text = "SELECT id, at, transaction_id, world, item, currency, amount, from_owner, to_owner "
                   "FROM moves WHERE transaction_id = ?1 ORDER BY id DESC LIMIT ?2";
        else if (query.item != 0)
            text = "SELECT id, at, transaction_id, world, item, currency, amount, from_owner, to_owner "
                   "FROM moves WHERE item = ?1 ORDER BY id DESC LIMIT ?2";
        else if (!query.owner.empty())
            text = "SELECT id, at, transaction_id, world, item, currency, amount, from_owner, to_owner "
                   "FROM moves WHERE from_owner = ?1 OR to_owner = ?1 ORDER BY id DESC LIMIT ?2";
        Statement select(m_database, text);
        std::vector<DataMoveLine> found;
        if (!select.ready())
            return found;
        if (query.transaction != 0)
            select.integer(1, query.transaction);
        else if (query.item != 0)
            select.integer(1, query.item);
        else if (!query.owner.empty())
            select.text(1, query.owner);
        select.integer(2, static_cast<core::i64>(query.most));
        while (select.step() == SQLITE_ROW) {
            sqlite3_stmt* row = select.get();
            DataMoveLine line;
            line.id = sqlite3_column_int64(row, 0);
            line.at = sqlite3_column_int64(row, 1);
            line.transaction = sqlite3_column_int64(row, 2);
            line.world = columnText(row, 3);
            line.move.item = sqlite3_column_int64(row, 4);
            line.move.currency = columnText(row, 5);
            line.move.amount = sqlite3_column_int64(row, 6);
            line.move.from = columnText(row, 7);
            line.move.to = columnText(row, 8);
            found.push_back(std::move(line));
        }
        return found;
    }

    [[nodiscard]] DataTransaction undo(std::string_view world, core::i64 transaction) override
    {
        std::string ignored;
        if (!run("BEGIN IMMEDIATE", ignored))
            return {false, "Failed", 0};
        {
            Statement& state = use(TransactionState);
            state.integer(1, transaction);
            const int stepped = state.step();
            const bool undone = stepped == SQLITE_ROW && sqlite3_column_type(state.get(), 0) != SQLITE_NULL;
            state.reset();
            if (stepped != SQLITE_ROW)
                return rollback("NoSuchTransaction");
            if (undone)
                return rollback("AlreadyUndone");
        }
        struct Line
        {
            DataMove move;
            std::string kind;
            std::string fields;
        };
        std::vector<Line> lines;
        {
            Statement& select = use(MovesOf);
            select.integer(1, transaction);
            while (select.step() == SQLITE_ROW) {
                sqlite3_stmt* row = select.get();
                Line line;
                line.move.item = sqlite3_column_int64(row, 0);
                line.move.currency = columnText(row, 1);
                line.move.amount = sqlite3_column_int64(row, 2);
                line.move.from = columnText(row, 3);
                line.move.to = columnText(row, 4);
                line.kind = columnText(row, 5);
                line.fields = columnText(row, 6);
                lines.push_back(std::move(line));
            }
            select.reset();
        }
        if (lines.empty())
            return rollback("NoSuchTransaction");
        const std::optional<core::i64> id = begin(world, transaction);
        if (!id.has_value())
            return rollback("Failed");
        for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
            const DataMove& move = line->move;
            const DataMove back = reversed(move);
            std::string kind = line->kind;
            std::string fields = line->fields;
            if (move.item != 0 && move.from.empty()) {
                // Made by it: destroyed, as it is now, where it still is.
                const std::optional<DataItem> now = itemRow(move.item);
                if (!now.has_value() || now->owner != move.to)
                    return rollback("ItemNotThere");
                Statement& remove = use(DeleteItem);
                remove.integer(1, move.item);
                const bool removed = remove.step() == SQLITE_DONE;
                remove.reset();
                if (!removed)
                    return rollback("Failed");
                kind = now->kind;
                fields = now->fields;
            }
            else if (move.item != 0 && move.to.empty()) {
                // Destroyed by it: made again, under the id it had.
                Statement& insert = use(InsertItemWithId);
                insert.integer(1, move.item).text(2, line->kind).text(3, move.from).blob(4, line->fields);
                const bool made = insert.step() == SQLITE_DONE;
                insert.reset();
                if (!made)
                    return rollback("ItemNotThere");
            }
            else if (const char* refused = apply(back); refused != nullptr) {
                return rollback(refused);
            }
            if (!logMove(*id, world, back, kind, fields))
                return rollback("Failed");
        }
        Statement& mark = use(MarkUndone);
        mark.integer(1, *id).integer(2, transaction);
        const bool marked = mark.step() == SQLITE_DONE;
        mark.reset();
        if (!marked || !run("COMMIT", ignored))
            return rollback("Failed");
        return {true, {}, *id};
    }

    core::i64 forgetMovesBefore(core::i64 unixSeconds, core::i64 most) override
    {
        // The oldest `most` transactions from before then, with their lines,
        // in one write -- so a long log is let go of a little at a time.
        std::string ignored;
        if (!run("BEGIN IMMEDIATE", ignored))
            return 0;
        core::i64 upTo = 0;
        {
            Statement last(m_database, "SELECT id FROM transactions WHERE at < ?1 ORDER BY id LIMIT 1 OFFSET ?2");
            Statement newest(m_database, "SELECT MAX(id) FROM transactions WHERE at < ?1");
            if (!last.ready() || !newest.ready()) {
                (void)run("ROLLBACK", ignored);
                return 0;
            }
            last.integer(1, unixSeconds).integer(2, std::max<core::i64>(most, 1) - 1);
            if (last.step() == SQLITE_ROW) {
                upTo = sqlite3_column_int64(last.get(), 0);
            }
            else {
                newest.integer(1, unixSeconds);
                if (newest.step() == SQLITE_ROW)
                    upTo = sqlite3_column_int64(newest.get(), 0);
            }
        }
        core::i64 forgotten = 0;
        if (upTo > 0) {
            Statement lines(m_database, "DELETE FROM moves WHERE transaction_id IN "
                                        "(SELECT id FROM transactions WHERE id <= ?1 AND at < ?2)");
            Statement rows(m_database, "DELETE FROM transactions WHERE id <= ?1 AND at < ?2");
            if (!lines.ready() || !rows.ready()) {
                (void)run("ROLLBACK", ignored);
                return 0;
            }
            lines.integer(1, upTo).integer(2, unixSeconds);
            rows.integer(1, upTo).integer(2, unixSeconds);
            if (lines.step() != SQLITE_DONE) {
                (void)run("ROLLBACK", ignored);
                return 0;
            }
            forgotten = sqlite3_changes(m_database);
            if (rows.step() != SQLITE_DONE) {
                (void)run("ROLLBACK", ignored);
                return 0;
            }
        }
        if (!run("COMMIT", ignored)) {
            (void)run("ROLLBACK", ignored);
            return 0;
        }
        return forgotten;
    }

    [[nodiscard]] DataCounts counts() override
    {
        DataCounts counts;
        counts.schema = SchemaVersion;
        const auto count = [this](const char* text) -> core::i64 {
            Statement select(m_database, text);
            if (!select.ready() || select.step() != SQLITE_ROW)
                return 0;
            return sqlite3_column_int64(select.get(), 0);
        };
        counts.records = count("SELECT COUNT(*) FROM records");
        counts.items = count("SELECT COUNT(*) FROM items");
        counts.transactions = count("SELECT COUNT(*) FROM transactions");
        counts.moves = count("SELECT COUNT(*) FROM moves");
        std::filesystem::path journal = m_file;
        journal += "-wal";
        counts.bytes = sizeOf(m_file) + sizeOf(journal);
        return counts;
    }

    [[nodiscard]] bool startBackup(const std::filesystem::path& file, std::string& error) override
    {
        if (m_backup.copy != nullptr) {
            error = core::engineCatalog().format(ENG_TR("data.err.backup_running"));
            return false;
        }
        std::error_code ec;
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), ec);
        std::filesystem::path partial = file;
        partial += ".partial";
        std::filesystem::remove(partial, ec);
        const std::u8string name = partial.u8string();
        sqlite3* target = nullptr;
        if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &target,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            error = target != nullptr ? std::string{sqlite3_errmsg(target)}
                                      : core::engineCatalog().format(ENG_TR("data.err.no_memory"));
            sqlite3_close(target);
            return false;
        }
        sqlite3_backup* copy = sqlite3_backup_init(target, "main", m_database, "main");
        if (copy == nullptr) {
            error = sqlite3_errmsg(target);
            sqlite3_close(target);
            std::filesystem::remove(partial, ec);
            return false;
        }
        m_backup = Backup{target, copy, file, std::move(partial)};
        return true;
    }

    DataBackupProgress stepBackup(int pages) override
    {
        DataBackupProgress progress;
        if (m_backup.copy == nullptr)
            return progress;
        // **The store's own connection writes through to the copy**: a
        // change made between two steps is in it, and nothing starts over.
        const int stepped = sqlite3_backup_step(m_backup.copy, std::max(pages, 1));
        progress.total = sqlite3_backup_pagecount(m_backup.copy);
        progress.copied = progress.total - sqlite3_backup_remaining(m_backup.copy);
        progress.file = m_backup.file;
        if (stepped == SQLITE_OK || stepped == SQLITE_BUSY || stepped == SQLITE_LOCKED) {
            progress.state = DataBackupProgress::State::Copying;
            return progress;
        }
        if (stepped != SQLITE_DONE)
            progress.error = sqlite3_errstr(stepped);
        progress.state = endBackup(stepped == SQLITE_DONE, &progress.error) ? DataBackupProgress::State::Done
                                                                            : DataBackupProgress::State::Failed;
        return progress;
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
        ItemRow,
        DeleteItem,
        ItemsOf,
        BalanceOf,
        SharedOf,
        WriteShared,
        TransactionState,
        MovesOf,
        MarkUndone,
        InsertItemWithId,
        Count,
    };

    struct Backup
    {
        sqlite3* target = nullptr;
        sqlite3_backup* copy = nullptr;
        std::filesystem::path file;
        std::filesystem::path partial;
    };

    [[nodiscard]] Statement& use(Which which) { return *m_statements[which]; }

    // A transaction's row: its id, the move log's key.
    [[nodiscard]] std::optional<core::i64> begin(std::string_view world, core::i64 undoes)
    {
        Statement& insert = use(BeginTransaction);
        insert.integer(1, unixNow()).text(2, world);
        if (undoes != 0)
            insert.integer(3, undoes);
        else
            insert.null(3);
        const bool done = insert.step() == SQLITE_DONE;
        insert.reset();
        if (!done)
            return std::nullopt;
        return sqlite3_last_insert_rowid(m_database);
    }

    // One move of an item between owners, or of money; why not, or null.
    [[nodiscard]] const char* apply(const DataMove& move)
    {
        if (move.item != 0) {
            Statement& give = use(MoveItem);
            give.text(1, move.to).integer(2, move.item).text(3, move.from);
            const bool moved = give.step() == SQLITE_DONE && sqlite3_changes(m_database) == 1;
            give.reset();
            return moved ? nullptr : "ItemNotThere";
        }
        if (!move.from.empty()) {
            Statement& take = use(TakeMoney);
            take.integer(1, move.amount).text(2, move.from).text(3, move.currency);
            const bool taken = take.step() == SQLITE_DONE && sqlite3_changes(m_database) == 1;
            take.reset();
            if (!taken)
                return "NotEnough";
        }
        if (!move.to.empty()) {
            Statement& give = use(GiveMoney);
            give.text(1, move.to).text(2, move.currency).integer(3, move.amount);
            const bool given = give.step() == SQLITE_DONE;
            give.reset();
            if (!given)
                return "Failed";
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<DataItem> itemRow(core::i64 id)
    {
        Statement& select = use(ItemRow);
        select.integer(1, id);
        std::optional<DataItem> item;
        if (select.step() == SQLITE_ROW)
            item = DataItem{id, columnText(select.get(), 0), columnText(select.get(), 1), columnText(select.get(), 2)};
        select.reset();
        return item;
    }

    [[nodiscard]] bool logMove(core::i64 transaction, std::string_view world, const DataMove& move,
                               std::string_view kind, std::string_view fields)
    {
        Statement& insert = use(LogMove);
        insert.integer(1, unixNow()).integer(2, transaction).text(3, world);
        if (move.item != 0)
            insert.integer(4, move.item).null(5).null(6);
        else
            insert.null(4).text(5, move.currency).integer(6, move.amount);
        insert.text(7, move.from).text(8, move.to);
        // What an item made or destroyed was, to make it again.
        if (move.item != 0 && (move.from.empty() || move.to.empty()))
            insert.text(9, kind).blob(10, fields);
        else
            insert.null(9).null(10);
        const bool done = insert.step() == SQLITE_DONE;
        insert.reset();
        return done;
    }

    // The copy closed, and kept under its name where it finished.
    bool endBackup(bool finished, std::string* error = nullptr)
    {
        const int ended = sqlite3_backup_finish(m_backup.copy);
        const int closed = sqlite3_close(m_backup.target);
        std::error_code ec;
        bool kept = finished && ended == SQLITE_OK && closed == SQLITE_OK;
        if (kept) {
            std::filesystem::rename(m_backup.partial, m_backup.file, ec);
            kept = !ec;
            if (!kept && error != nullptr)
                *error = ec.message();
        }
        else if (error != nullptr && error->empty()) {
            *error = sqlite3_errstr(ended != SQLITE_OK ? ended : closed);
        }
        if (!kept)
            std::filesystem::remove(m_backup.partial, ec);
        m_backup = Backup{};
        return kept;
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
    std::filesystem::path m_file;
    std::vector<std::unique_ptr<Statement>> m_statements;
    Backup m_backup;
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
    auto store = std::make_unique<SqliteDataStore>(database, file);
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
