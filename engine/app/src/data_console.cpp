#include "engine/app/data_console.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/log.h"

namespace engine::app {
namespace {

// A scheduled backup a frame copies this many pages of (4 KiB each): a
// database of a gigabyte is copied in under two minutes of frames, and no
// frame waits on more than a megabyte of it.
constexpr int PagesAFrame = 256;
// The move log's transactions let go of in one write.
constexpr core::i64 ForgetAtOnce = 2000;
constexpr std::int64_t SecondsADay = 86400;
// How often the move log is read for lines past their days.
constexpr std::int64_t ForgetEverySeconds = 3600;

[[nodiscard]] std::string phrase(core::TextKey key, std::span<const core::I18nArg> args = {})
{
    return core::engineCatalog().format(key, args);
}

[[nodiscard]] std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// The first word of a line, and what follows it.
[[nodiscard]] std::pair<std::string_view, std::string_view> split(std::string_view text)
{
    text = trim(text);
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos)
        return {text, {}};
    return {text.substr(0, space), trim(text.substr(space + 1))};
}

[[nodiscard]] std::optional<core::i64> whole(std::string_view text)
{
    core::i64 value = 0;
    const auto [end, failed] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (failed != std::errc{} || end != text.data() + text.size() || value <= 0)
        return std::nullopt;
    return value;
}

// **Days and dates, in UTC**, without the C library's local time: the
// civil calendar's arithmetic, both ways.
[[nodiscard]] std::int64_t daysFromCivil(std::int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned fromMarch = month > 2 ? month - 3 : month + 9;
    const unsigned dayOfYear = (153 * fromMarch + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

struct Civil
{
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
};

[[nodiscard]] Civil civilFrom(std::int64_t unixSeconds)
{
    std::int64_t days = unixSeconds / SecondsADay;
    std::int64_t rest = unixSeconds % SecondsADay;
    if (rest < 0) {
        rest += SecondsADay;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned shifted = (5 * dayOfYear + 2) / 153;
    Civil civil;
    civil.day = dayOfYear - (153 * shifted + 2) / 5 + 1;
    civil.month = shifted < 10 ? shifted + 3 : shifted - 9;
    civil.year = static_cast<std::int64_t>(yearOfEra) + era * 400 + (civil.month <= 2 ? 1 : 0);
    civil.hour = static_cast<unsigned>(rest / 3600);
    civil.minute = static_cast<unsigned>(rest % 3600 / 60);
    civil.second = static_cast<unsigned>(rest % 60);
    return civil;
}

// What the move log says a time is: `2026-10-19 15:30:00 UTC`.
[[nodiscard]] std::string shownTime(std::int64_t unixSeconds)
{
    const Civil at = civilFrom(unixSeconds);
    std::array<char, 24> date{};
    std::array<char, 16> clock{};
    std::snprintf(date.data(), date.size(), "%04lld-%02u-%02u", static_cast<long long>(at.year), at.month, at.day);
    std::snprintf(clock.data(), clock.size(), "%02u:%02u:%02u", at.hour, at.minute, at.second);
    const std::array<core::I18nArg, 2> args{core::I18nArg{"date", std::string_view{date.data()}},
                                            core::I18nArg{"clock", std::string_view{clock.data()}}};
    return phrase(ENG_TR("data.time_utc"), args);
}

[[nodiscard]] std::string ownerName(const std::string& owner)
{
    return owner.empty() ? phrase(ENG_TR("data.the_game")) : owner;
}

[[nodiscard]] std::string reasonText(std::string_view reason)
{
    if (reason == "NoSuchTransaction")
        return phrase(ENG_TR("data.reason.no_such_transaction"));
    if (reason == "AlreadyUndone")
        return phrase(ENG_TR("data.reason.already_undone"));
    if (reason == "ItemNotThere")
        return phrase(ENG_TR("data.reason.item_not_there"));
    if (reason == "NotEnough")
        return phrase(ENG_TR("data.reason.not_enough"));
    return phrase(ENG_TR("data.reason.failed"));
}

[[nodiscard]] std::string moveRow(const DataMoveLine& line)
{
    const std::string at = shownTime(line.at);
    const std::string from = ownerName(line.move.from);
    const std::string to = ownerName(line.move.to);
    if (line.move.item != 0) {
        const std::array<core::I18nArg, 7> args{core::I18nArg{"id", line.id},
                                                core::I18nArg{"at", std::string_view{at}},
                                                core::I18nArg{"transaction", line.transaction},
                                                core::I18nArg{"world", std::string_view{line.world}},
                                                core::I18nArg{"item", line.move.item},
                                                core::I18nArg{"from", std::string_view{from}},
                                                core::I18nArg{"to", std::string_view{to}}};
        return phrase(ENG_TR("data.moves_item"), args);
    }
    const std::array<core::I18nArg, 8> args{core::I18nArg{"id", line.id},
                                            core::I18nArg{"at", std::string_view{at}},
                                            core::I18nArg{"transaction", line.transaction},
                                            core::I18nArg{"world", std::string_view{line.world}},
                                            core::I18nArg{"amount", line.move.amount},
                                            core::I18nArg{"currency", std::string_view{line.move.currency}},
                                            core::I18nArg{"from", std::string_view{from}},
                                            core::I18nArg{"to", std::string_view{to}}};
    return phrase(ENG_TR("data.moves_money"), args);
}

} // namespace

DataConsole::DataConsole(Options options) : m_options(std::move(options))
{}

std::string DataConsole::backupName(std::string_view game, std::int64_t unixSeconds)
{
    const Civil at = civilFrom(unixSeconds);
    std::array<char, 24> stamp{};
    std::snprintf(stamp.data(), stamp.size(), "%04lld%02u%02u-%02u%02u%02u", static_cast<long long>(at.year), at.month,
                  at.day, at.hour, at.minute, at.second);
    return std::string(game) + "-" + stamp.data() + ".db";
}

std::optional<std::int64_t> DataConsole::backupTime(std::string_view game, std::string_view name)
{
    // `<game>-YYYYMMDD-HHMMSS.db`, and nothing else.
    constexpr std::size_t Stamp = 15;
    if (name.size() != game.size() + 1 + Stamp + 3 || !name.starts_with(game) || name[game.size()] != '-' ||
        !name.ends_with(".db"))
        return std::nullopt;
    const std::string_view stamp = name.substr(game.size() + 1, Stamp);
    if (stamp[8] != '-')
        return std::nullopt;
    const auto digits = [&](std::size_t at, std::size_t count) -> std::optional<unsigned> {
        unsigned value = 0;
        const char* first = stamp.data() + at;
        const auto [end, failed] = std::from_chars(first, first + count, value);
        if (failed != std::errc{} || end != first + count)
            return std::nullopt;
        return value;
    };
    const std::optional<unsigned> year = digits(0, 4);
    const std::optional<unsigned> month = digits(4, 2);
    const std::optional<unsigned> day = digits(6, 2);
    const std::optional<unsigned> hour = digits(9, 2);
    const std::optional<unsigned> minute = digits(11, 2);
    const std::optional<unsigned> second = digits(13, 2);
    if (!year || !month || !day || !hour || !minute || !second || *month < 1 || *month > 12 || *day < 1 || *day > 31 ||
        *hour > 23 || *minute > 59 || *second > 59)
        return std::nullopt;
    return daysFromCivil(*year, *month, *day) * SecondsADay + *hour * 3600 + *minute * 60 + *second;
}

std::optional<DataConsole::Answer> DataConsole::run(std::string_view line, DataBroker& data, std::int64_t nowUnix)
{
    const auto [verb, rest] = split(line);
    IDataStore& store = data.store();
    if (verb == "moves") {
        DataMoveQuery query;
        const auto [what, value] = split(rest);
        if (what == "item" || what == "transaction") {
            const std::optional<core::i64> id = whole(value);
            if (!id.has_value())
                return Answer{phrase(ENG_TR("console.usage.moves")), true};
            (what == "item" ? query.item : query.transaction) = *id;
        }
        else if (!what.empty()) {
            if (!value.empty())
                return Answer{phrase(ENG_TR("console.usage.moves")), true};
            query.owner = std::string(what);
        }
        const std::vector<DataMoveLine> lines = store.moves(query);
        if (lines.empty())
            return Answer{phrase(ENG_TR("data.moves_none")), false};
        std::string text;
        for (const DataMoveLine& found : lines)
            text += (text.empty() ? "" : "\n") + moveRow(found);
        return Answer{std::move(text), false};
    }
    if (verb != "data")
        return std::nullopt;

    const auto [action, argument] = split(rest);
    if (action.empty()) {
        const DataCounts counts = store.counts();
        std::array<char, 24> megabytes{};
        std::snprintf(megabytes.data(), megabytes.size(), "%.1f",
                      static_cast<double>(counts.bytes) / (1024.0 * 1024.0));
        const std::array<core::I18nArg, 7> args{core::I18nArg{"schema", counts.schema},
                                                core::I18nArg{"records", counts.records},
                                                core::I18nArg{"held", static_cast<core::i64>(data.held())},
                                                core::I18nArg{"items", counts.items},
                                                core::I18nArg{"transactions", counts.transactions},
                                                core::I18nArg{"moves", counts.moves},
                                                core::I18nArg{"megabytes", std::string_view{megabytes.data()}}};
        std::string text = phrase(counts.bytes > 0 ? ENG_TR("data.status") : ENG_TR("data.status_memory"), args);
        if (m_copying) {
            const std::string to = m_copyingTo.string();
            const std::array<core::I18nArg, 3> copying{core::I18nArg{"path", std::string_view{to}},
                                                       core::I18nArg{"copied", m_copied},
                                                       core::I18nArg{"total", m_total}};
            text += "\n" + phrase(ENG_TR("data.status_backup"), copying);
        }
        return Answer{std::move(text), false};
    }
    if (action == "backup") {
        const std::filesystem::path to = argument.empty() ? m_options.backups / backupName(m_options.game, nowUnix)
                                                          : std::filesystem::path(std::string(argument));
        std::string why;
        if (!store.startBackup(to, why)) {
            const std::string path = to.string();
            const std::array<core::I18nArg, 2> args{core::I18nArg{"path", std::string_view{path}},
                                                    core::I18nArg{"why", std::string_view{why}}};
            return Answer{phrase(ENG_TR("data.err.backup_failed"), args), true};
        }
        m_copying = true;
        m_scheduled = false;
        m_copyingTo = to;
        m_copied = 0;
        m_total = 0;
        const std::string path = to.string();
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", std::string_view{path}}};
        return Answer{phrase(ENG_TR("data.done.backup_started"), args), false};
    }
    if (action == "undo") {
        const std::optional<core::i64> transaction = whole(argument);
        if (!transaction.has_value())
            return Answer{phrase(ENG_TR("console.usage.data")), true};
        const DataTransaction undone = store.undo("console", *transaction);
        if (!undone.ok) {
            const std::string why = reasonText(undone.reason);
            const std::array<core::I18nArg, 2> args{core::I18nArg{"transaction", *transaction},
                                                    core::I18nArg{"why", std::string_view{why}}};
            return Answer{phrase(ENG_TR("data.err.undo_refused"), args), true};
        }
        const std::array<core::I18nArg, 2> args{core::I18nArg{"transaction", *transaction},
                                                core::I18nArg{"undo", undone.id}};
        core::log(core::LogLevel::Info, ENG_TR("data.done.undone"), args);
        return Answer{phrase(ENG_TR("data.done.undone"), args), false};
    }
    return Answer{phrase(ENG_TR("console.usage.data")), true};
}

void DataConsole::frame(DataBroker& data, std::int64_t nowUnix)
{
    IDataStore& store = data.store();
    if (m_copying) {
        const DataBackupProgress progress = store.stepBackup(PagesAFrame);
        m_copied = progress.copied;
        m_total = progress.total;
        if (progress.state != DataBackupProgress::State::Copying) {
            const std::string path = m_copyingTo.string();
            if (progress.state == DataBackupProgress::State::Done) {
                const std::array<core::I18nArg, 2> args{core::I18nArg{"path", std::string_view{path}},
                                                        core::I18nArg{"pages", progress.total}};
                core::log(core::LogLevel::Info, ENG_TR("data.info.backup_done"), args);
                if (m_scheduled)
                    prune();
            }
            else {
                const std::array<core::I18nArg, 2> args{core::I18nArg{"path", std::string_view{path}},
                                                        core::I18nArg{"why", std::string_view{progress.error}}};
                core::log(core::LogLevel::Error, ENG_TR("data.err.backup_failed"), args);
            }
            m_copying = false;
        }
    }

    // **A backup every so many hours**, counted from the newest one in the
    // folder -- so a manager started again does not take one at once, nor
    // wait a whole period more.
    if (m_options.backupEveryHours > 0.0 && !m_copying) {
        const auto period = static_cast<std::int64_t>(m_options.backupEveryHours * 3600.0);
        if (!m_nextBackup.has_value()) {
            const std::int64_t newest = newestBackup();
            m_nextBackup = newest > 0 ? newest + period : nowUnix;
        }
        if (nowUnix >= *m_nextBackup) {
            m_nextBackup = nowUnix + period;
            const std::filesystem::path to = m_options.backups / backupName(m_options.game, nowUnix);
            std::string why;
            if (store.startBackup(to, why)) {
                m_copying = true;
                m_scheduled = true;
                m_copyingTo = to;
                m_copied = 0;
                m_total = 0;
            }
            else {
                const std::string path = to.string();
                const std::array<core::I18nArg, 2> args{core::I18nArg{"path", std::string_view{path}},
                                                        core::I18nArg{"why", std::string_view{why}}};
                core::log(core::LogLevel::Error, ENG_TR("data.err.backup_failed"), args);
            }
        }
    }

    // **The move log's days** (ADR 0238 §3): read once an hour, and what is
    // past them let go of a little each frame.
    if (m_options.movesKeepDays > 0) {
        if (!m_forgetting && nowUnix >= m_nextForget) {
            m_forgetting = true;
            m_forgotten = 0;
        }
        if (m_forgetting) {
            const std::int64_t before = nowUnix - static_cast<std::int64_t>(m_options.movesKeepDays) * SecondsADay;
            const core::i64 gone = store.forgetMovesBefore(before, ForgetAtOnce);
            m_forgotten += gone;
            if (gone == 0) {
                m_forgetting = false;
                m_nextForget = nowUnix + ForgetEverySeconds;
                if (m_forgotten > 0) {
                    const std::array<core::I18nArg, 2> args{
                        core::I18nArg{"lines", m_forgotten},
                        core::I18nArg{"days", static_cast<core::i64>(m_options.movesKeepDays)}};
                    core::log(core::LogLevel::Info, ENG_TR("data.info.moves_forgotten"), args);
                }
            }
        }
    }
}

std::int64_t DataConsole::newestBackup() const
{
    std::int64_t newest = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator at(m_options.backups, ec), end; !ec && at != end; at.increment(ec)) {
        if (const std::optional<std::int64_t> time = backupTime(m_options.game, at->path().filename().string()))
            newest = std::max(newest, *time);
    }
    return newest;
}

void DataConsole::prune() const
{
    // The newest `backupKeep` of the backups named as ours; nothing else in
    // the folder is touched.
    std::vector<std::pair<std::int64_t, std::filesystem::path>> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator at(m_options.backups, ec), end; !ec && at != end; at.increment(ec)) {
        if (const std::optional<std::int64_t> time = backupTime(m_options.game, at->path().filename().string()))
            found.emplace_back(*time, at->path());
    }
    if (found.size() <= m_options.backupKeep)
        return;
    std::sort(found.begin(), found.end());
    for (std::size_t at = 0; at + m_options.backupKeep < found.size(); ++at)
        std::filesystem::remove(found[at].second, ec);
}

} // namespace engine::app
