#pragma once

// What the run before this one left behind: how it ended, and its report.
//
// **Why it exists.** A game that shipped is run by people who are not its
// developer, and when it dies on one of them the developer learns nothing --
// the log and the crash note are on that person's machine, under a name they
// have never heard, and the game that could ask for them does not know there
// is anything to ask for. So each run keeps one small file beside its log
// saying that it is running; ending well says so in it; and the next run reads
// it. A record that still says "running" is a run that did not end well, and
// the crash handler's note beside it says whether it was a fault.
//
// It is `core` rather than `platform` because a script error is counted in it
// and the script module may not reach `platform`; like the log file, the
// folder is INJECTED -- `app` decides where at boot.
//
// **Nothing is uploaded and nothing is shown.** A game reads this through
// `RunService:GetLastRun` and decides what to say to its player.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "engine/core/types.h"

namespace engine::core {

// How a run ended. The numbers are `Enum.RunOutcome`'s.
enum class RunOutcome : u8
{
    // No run before this one left a record: the first run, or its folder was
    // cleared.
    None = 0,
    // It was asked to end and did.
    Clean = 1,
    // A fault, or an exception nothing caught: the crash handler ran.
    Crashed = 2,
    // It ended without saying so and without a fault of its own: ended from
    // outside -- by the system, by its player, by the power. On a phone this
    // is ordinary: a game in the background is ended when memory is wanted.
    Unfinished = 3,
};

struct LastRun
{
    RunOutcome outcome = RunOutcome::None;
    // Errors a script raised and nothing caught, in that run.
    u32 scriptErrors = 0;
    // The first of them, on one line; empty when there were none.
    std::string firstScriptError;
    // The report written for it at this run's start -- what it was, its crash
    // note, the end of its log -- or empty when it ended clean with no script
    // error, which leaves nothing to report.
    std::filesystem::path report;
    // The dump the crash handler wrote for it, where there is one.
    std::filesystem::path dump;
};

struct RunIdentity
{
    // This process, which is what the crash handler names its files by.
    u32 process = 0;
    // What is running, for the report's first lines: the game and its
    // version, the engine's, the platform. Free text, one line each.
    std::string game;
    std::string engine;
    std::string platform;
};

struct RunFiles
{
    // The log this run writes: the record and the report are named after it
    // and kept beside it, so a second run from the same folder has its own.
    std::filesystem::path log;
    // The note and the dump the crash handler writes for a process, which is
    // `platform`'s to name. Either may answer empty.
    std::function<std::filesystem::path(u32 process)> crashNoteOf;
    std::function<std::filesystem::path(u32 process)> crashDumpOf;
};

// Reads what the run before left, writes its report when it left something to
// report, and begins this run's record. Called once, at boot, after the log is
// open. What it found is kept for `lastRun`.
const LastRun& beginRun(const RunFiles& files, const RunIdentity& who);

// What `beginRun` found; `None` before it is called.
[[nodiscard]] const LastRun& lastRun() noexcept;

// A script raised an error and nothing caught it. Counted, the first kept,
// and the record written at the first and no more than once a second after:
// a script that fails every frame must not write a file every frame.
void noteScriptError(std::string_view message);

// This run is ending because it was asked to. Idempotent.
void endRun() noexcept;

// --- For tests: the record's own text ---------------------------------------

// The record and the report of a log at `log`: `engine.log` keeps
// `engine.run` and `engine.last-run.txt`.
[[nodiscard]] std::filesystem::path runRecordPath(const std::filesystem::path& log);
[[nodiscard]] std::filesystem::path runReportPath(const std::filesystem::path& log);

// Forgets everything, so a test can begin a second "process" in one.
void resetRunRecordForTest() noexcept;

} // namespace engine::core
