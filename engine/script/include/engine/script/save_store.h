// What `SaveService` keeps, and where (ADR 0111).
//
// **The slots live here, not in the VM.** A slot's values are C++ values, so
// they outlive a script runtime being replaced -- the editor's Stop restarts
// the VM, and a hot reload may -- and can still be written after it is gone:
// the host flushes this store on close and before a restart, where a table
// held by the VM would already have been collected.
//
// A slot is one file, `<name>.save`, beside the backup the previous save left,
// `<name>.bak`. The file is a header line -- magic, format version, the game's
// save version, the payload's length and its xxh3 -- and a JSON payload in
// which every value says what type it is: a `Color3` and a `Vector3` are both
// three numbers, and a save that read one back as the other would be wrong in
// the one place a player notices.
#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/core/types.h"
#include "engine/scene/value.h"

namespace engine::script {

struct SaveTable;

// A value a save holds: the attribute domain, or a table of them. A table is
// shared between copies only while nobody changes it -- every `Get` pushes a
// fresh Luau table, and every `Set` builds a fresh one of these.
struct SaveValue
{
    scene::Value scalar;
    std::shared_ptr<const SaveTable> table;

    [[nodiscard]] bool isTable() const noexcept { return table != nullptr; }
};

// A table: its array part, 1..n, and its string keys, sorted so a slot writes
// the same bytes for the same contents.
struct SaveTable
{
    std::vector<SaveValue> array;
    std::map<std::string, SaveValue> fields;
};

struct SaveSlotData
{
    std::string name;
    std::map<std::string, SaveValue> values;
    // The game's `SaveService.Version` the file was written at.
    core::f64 version = 1.0;
    // False when both the file and its backup were damaged.
    bool recovered = true;
    // Bumped on every change; a write records the generation it wrote.
    core::u64 generation = 0;
    core::u64 written = 0;

    [[nodiscard]] bool dirty() const noexcept { return generation != written; }
};

// What reading a slot found wrong with its files.
enum class SaveDamage : core::u8
{
    None,
    // The file was damaged and the backup the previous save left was read.
    FromBackup,
    // Both were damaged: the slot starts empty and `recovered` is false.
    Lost,
};

class SaveStore
{
public:
    struct Options
    {
        // Where the `.save` files go. Empty keeps everything in memory: a
        // build or a run with nowhere to write still plays, and says so once.
        std::filesystem::path directory;
        core::u64 maxSlotBytes = 4u * 1024u * 1024u;
        core::u32 maxSlots = 64;
    };

    explicit SaveStore(Options options);
    ~SaveStore();

    SaveStore(const SaveStore&) = delete;
    SaveStore& operator=(const SaveStore&) = delete;

    [[nodiscard]] const Options& options() const noexcept { return m_options; }

    // 1 to 64 characters of letters, digits, `_` and `-`: a slot is a file and
    // its name is never a path.
    [[nodiscard]] static bool validName(std::string_view name) noexcept;

    // The slot, read from disk the first time it is asked for and kept after.
    // A stable address: slots are never destroyed while the store lives, so a
    // handle a script holds stays valid (`remove` empties one). `damage` says
    // what was wrong with the files. Null when `name` is not valid or the
    // store already holds `maxSlots` slots and this would be another.
    [[nodiscard]] SaveSlotData* open(std::string_view name, SaveDamage* damage = nullptr);

    // The slots on disk and in memory, sorted.
    [[nodiscard]] std::vector<std::string> list() const;
    // Empties a slot and removes its files. False when the name is not valid.
    bool remove(std::string_view name);

    // The file's whole text for `slot`, header and payload.
    [[nodiscard]] static std::string encode(const SaveSlotData& slot, core::f64 version);
    // Reads a file's text back into `slot`; false when the header, the length
    // or the checksum does not hold.
    [[nodiscard]] static bool decode(std::string_view text, SaveSlotData& slot);
    // The payload's size for `slot`, which is what the limit is measured on.
    [[nodiscard]] static core::u64 encodedSize(const SaveSlotData& slot);

    // Queues a write of `slot` as it is now; the ticket says when it is done.
    // Untracked, the write is fire-and-forget and leaves no result behind.
    core::u64 write(SaveSlotData& slot, core::f64 version, bool tracked = true);
    // Whether the write `ticket` is done, and then whether it succeeded.
    [[nodiscard]] bool finished(core::u64 ticket, bool& succeeded);

    // Called every tick with its length: a slot that changed is written at most
    // once a second of play (ADR 0111 section 5).
    void pump(core::f64 dt, core::f64 version);
    // Writes every changed slot now and waits: on close, before a restart, and
    // when the app goes to the background.
    void flush(core::f64 version);

private:
    struct Job
    {
        core::u64 ticket = 0;
        std::string name;
        std::string text;
        bool tracked = true;
    };

    void run();
    [[nodiscard]] bool writeNow(const std::string& name, const std::string& text);

    Options m_options;
    std::map<std::string, std::unique_ptr<SaveSlotData>, std::less<>> m_slots;
    core::f64 m_sinceWrite = 0.0;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::deque<Job> m_jobs;
    std::map<core::u64, bool> m_done;
    core::u64 m_nextTicket = 1;
    bool m_busy = false;
    bool m_stopping = false;
    std::thread m_worker;
};

} // namespace engine::script
