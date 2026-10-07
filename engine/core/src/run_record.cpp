// What the run before this one left behind. See run_record.h.
#include "engine/core/run_record.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace engine::core {

namespace {

// How much of the run's log goes into its report: its end, where what went
// wrong is, and little enough to send by any means a player has.
constexpr std::uintmax_t ReportLogBytes = 256 * 1024;

struct State
{
    std::mutex guard;
    bool begun = false;
    bool ended = false;
    std::filesystem::path record;
    RunIdentity who;
    i64 started = 0;
    u32 scriptErrors = 0;
    std::string firstScriptError;
    std::chrono::steady_clock::time_point written{};
    u32 writtenErrors = 0;
    LastRun last;
};

State& state()
{
    static State instance;
    return instance;
}

[[nodiscard]] i64 nowSeconds()
{
    // The wall clock, and on purpose: this is a fact about a machine's
    // afternoon, written to a file a person reads. Nothing of it reaches the
    // simulation (R10).
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::string oneLine(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\n' || c == '\r')
            out.push_back(' ');
        else
            out.push_back(c);
    }
    constexpr usize Most = 2000;
    if (out.size() > Most)
        out.resize(Most);
    return out;
}

// `key=value` a line. Written whole to a file beside it and renamed over, so a
// run that dies while writing leaves the record it had.
void writeRecord(const State& s, bool clean)
{
    if (s.record.empty())
        return;
    std::ostringstream text;
    text << "state=" << (clean ? "clean" : "running") << '\n';
    text << "process=" << s.who.process << '\n';
    text << "started=" << s.started << '\n';
    text << "game=" << oneLine(s.who.game) << '\n';
    text << "engine=" << oneLine(s.who.engine) << '\n';
    text << "platform=" << oneLine(s.who.platform) << '\n';
    text << "scriptErrors=" << s.scriptErrors << '\n';
    text << "firstScriptError=" << oneLine(s.firstScriptError) << '\n';

    std::filesystem::path scratch = s.record;
    scratch += ".new";
    {
        std::ofstream file(scratch, std::ios::binary | std::ios::trunc);
        if (!file)
            return;
        file << text.str();
    }
    std::error_code error;
    std::filesystem::rename(scratch, s.record, error);
    if (error) {
        // Where a rename will not replace: remove, then rename.
        std::filesystem::remove(s.record, error);
        std::filesystem::rename(scratch, s.record, error);
    }
}

struct Record
{
    bool found = false;
    bool clean = false;
    u32 process = 0;
    i64 started = 0;
    std::string game;
    std::string engine;
    std::string platform;
    u32 scriptErrors = 0;
    std::string firstScriptError;
};

[[nodiscard]] Record readRecord(const std::filesystem::path& path)
{
    Record record;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return record;
    record.found = true;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const usize equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        const auto number = [&value]() -> i64 {
            i64 out = 0;
            for (const char c : value) {
                if (c < '0' || c > '9')
                    return 0;
                out = out * 10 + (c - '0');
                if (out > 4'000'000'000'000ll)
                    return 0;
            }
            return out;
        };
        if (key == "state")
            record.clean = value == "clean";
        else if (key == "process")
            record.process = static_cast<u32>(number());
        else if (key == "started")
            record.started = number();
        else if (key == "game")
            record.game = value;
        else if (key == "engine")
            record.engine = value;
        else if (key == "platform")
            record.platform = value;
        else if (key == "scriptErrors")
            record.scriptErrors = static_cast<u32>(number());
        else if (key == "firstScriptError")
            record.firstScriptError = value;
    }
    return record;
}

// A file's text, or the last `most` bytes of it from the start of a line.
[[nodiscard]] std::string tailOf(const std::filesystem::path& path, std::uintmax_t most)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
        return {};
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    const std::uintmax_t from = size > most ? size - most : 0;
    file.seekg(static_cast<std::streamoff>(from));
    std::string text(static_cast<usize>(size - from), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<usize>(file.gcount()));
    if (from > 0) {
        const usize line = text.find('\n');
        if (line != std::string::npos)
            text.erase(0, line + 1);
    }
    return text;
}

// A crash file is the run's own when it was written after the run began: a
// process number is used again, and an old note under it is somebody else's.
[[nodiscard]] bool writtenSince(const std::filesystem::path& path, i64 started)
{
    if (path.empty())
        return false;
    std::error_code error;
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(path, error);
    if (error)
        return false;
    const auto age = std::filesystem::file_time_type::clock::now() - written;
    const i64 writtenAt = nowSeconds() - std::chrono::duration_cast<std::chrono::seconds>(age).count();
    // A second of grace: the two clocks are read apart.
    return writtenAt + 1 >= started;
}

[[nodiscard]] std::string_view outcomeWord(RunOutcome outcome)
{
    switch (outcome) {
    case RunOutcome::Clean:
        return "ended when asked to";
    case RunOutcome::Crashed:
        return "crashed";
    case RunOutcome::Unfinished:
        return "ended without saying so (ended from outside, or the power)";
    case RunOutcome::None:
        break;
    }
    return "unknown";
}

} // namespace

std::filesystem::path runRecordPath(const std::filesystem::path& log)
{
    std::filesystem::path path = log;
    path.replace_extension(".run");
    return path;
}

std::filesystem::path runReportPath(const std::filesystem::path& log)
{
    std::filesystem::path path = log;
    path.replace_filename(log.stem().string() + ".last-run.txt");
    return path;
}

const LastRun& beginRun(const RunFiles& files, const RunIdentity& who)
{
    State& s = state();
    const std::lock_guard lock(s.guard);
    s.last = LastRun{};
    s.record = runRecordPath(files.log);
    s.who = who;
    s.started = nowSeconds();
    s.begun = true;
    s.ended = false;
    s.scriptErrors = 0;
    s.firstScriptError.clear();
    s.writtenErrors = 0;

    const Record before = readRecord(s.record);
    std::filesystem::path note;
    if (before.found) {
        s.last.scriptErrors = before.scriptErrors;
        s.last.firstScriptError = before.firstScriptError;
        if (before.clean) {
            s.last.outcome = RunOutcome::Clean;
        }
        else {
            if (files.crashNoteOf)
                note = files.crashNoteOf(before.process);
            const bool crashed = writtenSince(note, before.started);
            s.last.outcome = crashed ? RunOutcome::Crashed : RunOutcome::Unfinished;
            if (!crashed)
                note.clear();
            if (crashed && files.crashDumpOf) {
                const std::filesystem::path dump = files.crashDumpOf(before.process);
                if (writtenSince(dump, before.started))
                    s.last.dump = dump;
            }
        }
    }

    // **The report**: one file of text a player can send by any means they
    // have. Written for a run that left something to report, and removed for
    // one that did not -- an old report beside a run that ended well would be
    // sent as if it were about it.
    const std::filesystem::path report = runReportPath(files.log);
    std::error_code error;
    const bool worth = before.found && (s.last.outcome != RunOutcome::Clean || s.last.scriptErrors > 0);
    if (!worth) {
        std::filesystem::remove(report, error);
    }
    else {
        std::filesystem::path previous = files.log;
        previous.replace_filename(files.log.stem().string() + ".previous" + files.log.extension().string());
        std::ofstream out(report, std::ios::binary | std::ios::trunc);
        if (out) {
            // English, and not the catalog's: this is read by the game's
            // developer, whatever language its player reads -- as the log is.
            out << "What the last run left\n";
            out << "======================\n";
            out << "game:      " << before.game << '\n';
            out << "engine:    " << before.engine << '\n';
            out << "platform:  " << before.platform << '\n';
            out << "started:   " << before.started << " (seconds since 1970, UTC)\n";
            out << "ended:     " << outcomeWord(s.last.outcome) << '\n';
            out << "script errors nothing caught: " << s.last.scriptErrors << '\n';
            if (!s.last.firstScriptError.empty())
                out << "the first: " << s.last.firstScriptError << '\n';
            if (!s.last.dump.empty())
                out << "dump:      " << s.last.dump.filename().string() << " (beside this file)\n";
            if (!note.empty()) {
                out << "\nThe crash note\n--------------\n";
                out << tailOf(note, ReportLogBytes) << '\n';
            }
            out << "\nThe end of its log\n------------------\n";
            const std::string log = tailOf(previous, ReportLogBytes);
            out << (log.empty() ? std::string("(no log was kept)\n") : log);
            s.last.report = report;
        }
    }

    writeRecord(s, false);
    s.written = std::chrono::steady_clock::now();
    return s.last;
}

const LastRun& lastRun() noexcept
{
    return state().last;
}

void noteScriptError(std::string_view message)
{
    State& s = state();
    const std::lock_guard lock(s.guard);
    if (!s.begun || s.ended)
        return;
    ++s.scriptErrors;
    if (s.firstScriptError.empty())
        s.firstScriptError = oneLine(message);
    const auto now = std::chrono::steady_clock::now();
    if (s.writtenErrors == 0 || now - s.written >= std::chrono::seconds(1)) {
        writeRecord(s, false);
        s.written = now;
        s.writtenErrors = s.scriptErrors;
    }
}

void endRun() noexcept
{
    State& s = state();
    try {
        const std::lock_guard lock(s.guard);
        if (!s.begun || s.ended)
            return;
        s.ended = true;
        writeRecord(s, true);
    } catch (...) {
        // Ending is not where a run may fail: the record then says it did not
        // end well, which is the worst it can say.
    }
}

void resetRunRecordForTest() noexcept
{
    State& s = state();
    const std::lock_guard lock(s.guard);
    s.begun = false;
    s.ended = false;
    s.record.clear();
    s.last = LastRun{};
    s.scriptErrors = 0;
    s.firstScriptError.clear();
    s.writtenErrors = 0;
}

} // namespace engine::core
