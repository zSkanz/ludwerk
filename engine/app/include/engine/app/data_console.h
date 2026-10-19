// **The game's database, at the operator's console** (ADR 0238 §3, §7):
// `data`, `data backup`, `data undo` and `moves`, and what runs by itself --
// a backup copying a few pages a frame, one every so many hours, and the move
// log's oldest lines let go of. Kept beside the one process that opens the
// database: the manager, or a server with no manager.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "engine/app/data_broker.h"
#include "engine/core/types.h"

namespace engine::app {

class DataConsole
{
public:
    struct Options
    {
        // Where a backup goes when no path is given, and every scheduled one.
        std::filesystem::path backups;
        // What a backup's file is named after: `<game>-<UTC time>.db`.
        std::string game = "game";
        // A backup every so many hours; 0 for never.
        core::f64 backupEveryHours = 0.0;
        // The newest kept in `backups`, the rest deleted after each scheduled one.
        core::u32 backupKeep = 7;
        // The move log's lines kept so many days; 0 for good.
        core::u32 movesKeepDays = 90;
    };

    struct Answer
    {
        std::string text;
        bool failed = false;
    };

    explicit DataConsole(Options options);

    // `data`, `data backup [path]`, `data undo <transaction>` and `moves
    // [<owner> | item <id> | transaction <id>]`, answered; none for a line
    // that is none of them.
    [[nodiscard]] std::optional<Answer> run(std::string_view line, DataBroker& data, std::int64_t nowUnix);
    // Once a frame: a backup's next pages, one that is due, and the move
    // log's next oldest lines let go of.
    void frame(DataBroker& data, std::int64_t nowUnix);

    // A backup's file name, `<game>-YYYYMMDD-HHMMSS.db` in UTC, and the time
    // read back from one; none for a name that is not one.
    [[nodiscard]] static std::string backupName(std::string_view game, std::int64_t unixSeconds);
    [[nodiscard]] static std::optional<std::int64_t> backupTime(std::string_view game, std::string_view name);

private:
    void prune() const;
    [[nodiscard]] std::int64_t newestBackup() const;

    Options m_options;
    // A backup copying, and whether the schedule asked for it.
    bool m_copying = false;
    bool m_scheduled = false;
    std::filesystem::path m_copyingTo;
    core::i64 m_copied = 0;
    core::i64 m_total = 0;
    // When the next scheduled backup is due; none until the folder is read.
    std::optional<std::int64_t> m_nextBackup;
    // When the move log is next read for lines past their days, and how many
    // have gone in the pass under way.
    std::int64_t m_nextForget = 0;
    bool m_forgetting = false;
    core::i64 m_forgotten = 0;
};

} // namespace engine::app
