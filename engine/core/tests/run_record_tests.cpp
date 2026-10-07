// What the run before this one left behind (ADR 0187): `run_record.h`.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "engine/core/run_record.h"

using namespace engine;

namespace {

// A folder of its own for each case, with a log in it as a run's would be.
struct Folder
{
    std::filesystem::path root;
    std::filesystem::path log;

    explicit Folder(const std::string& name)
        : root(std::filesystem::temp_directory_path() / "engine-run-record" / name), log(root / "engine.log")
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
        core::resetRunRecordForTest();
    }
    ~Folder()
    {
        core::resetRunRecordForTest();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void write(const std::string& name, const std::string& text) const
    {
        std::ofstream file(root / name, std::ios::binary | std::ios::trunc);
        file << text;
    }

    [[nodiscard]] std::string read(const std::filesystem::path& path) const
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    // The files a run names: its crash note is `crash-<process>.txt` here.
    [[nodiscard]] core::RunFiles files() const
    {
        const std::filesystem::path folder = root;
        return core::RunFiles{
            .log = log,
            .crashNoteOf =
                [folder](core::u32 process) { return folder / ("crash-" + std::to_string(process) + ".txt"); },
            .crashDumpOf =
                [folder](core::u32 process) { return folder / ("crash-" + std::to_string(process) + ".dmp"); },
        };
    }

    // A "process": begins a run as `process` would, as a second run of the
    // game does when the first is gone.
    const core::LastRun& begin(core::u32 process) const
    {
        core::resetRunRecordForTest();
        return core::beginRun(files(), core::RunIdentity{
                                           .process = process,
                                           .game = "Game 1.2.0",
                                           .engine = "Engine 0.0.1",
                                           .platform = "Test",
                                       });
    }
};

} // namespace

TEST_CASE("the first run has no run before it")
{
    const Folder folder("first");
    const core::LastRun& last = folder.begin(100);
    CHECK(last.outcome == core::RunOutcome::None);
    CHECK(last.scriptErrors == 0);
    CHECK(last.report.empty());
    // And it says it is running, which is what the next run reads.
    CHECK(folder.read(core::runRecordPath(folder.log)).find("state=running") != std::string::npos);
    CHECK(folder.read(core::runRecordPath(folder.log)).find("process=100") != std::string::npos);
}

TEST_CASE("a run that ended when asked to is clean, and leaves no report")
{
    const Folder folder("clean");
    (void)folder.begin(100);
    core::endRun();
    CHECK(folder.read(core::runRecordPath(folder.log)).find("state=clean") != std::string::npos);

    const core::LastRun& last = folder.begin(101);
    CHECK(last.outcome == core::RunOutcome::Clean);
    CHECK(last.report.empty());
    CHECK_FALSE(std::filesystem::exists(core::runReportPath(folder.log)));
}

TEST_CASE("a run that left its crash note crashed, and its report holds the note and the end of its log")
{
    const Folder folder("crashed");
    (void)folder.begin(100);
    // It dies: no `endRun`. The crash handler wrote its note and its dump, and
    // the next run's log rotation kept the log as `.previous`.
    folder.write("crash-100.txt", "engine: access violation reading 0x10\n  frame 0: Game::tick\n");
    folder.write("crash-100.dmp", "MDMP");
    folder.write("engine.previous.log", "[info] booted\n[error] the last line before it died\n");

    const core::LastRun& last = folder.begin(101);
    CHECK(last.outcome == core::RunOutcome::Crashed);
    REQUIRE_FALSE(last.report.empty());
    CHECK(last.report == core::runReportPath(folder.log));
    CHECK(last.dump.filename() == "crash-100.dmp");

    const std::string report = folder.read(last.report);
    CHECK(report.find("Game 1.2.0") != std::string::npos);
    CHECK(report.find("Engine 0.0.1") != std::string::npos);
    CHECK(report.find("crashed") != std::string::npos);
    CHECK(report.find("access violation reading 0x10") != std::string::npos);
    CHECK(report.find("the last line before it died") != std::string::npos);
    CHECK(report.find("crash-100.dmp") != std::string::npos);
}

TEST_CASE("a run ended from outside is unfinished, not crashed")
{
    // The system ending a game in the background, its player ending the task,
    // the power: no note, because no fault. A phone does this every day, and a
    // game that called it a crash would ask for a report every morning.
    const Folder folder("unfinished");
    (void)folder.begin(100);
    folder.write("engine.previous.log", "[info] booted\n");

    const core::LastRun& last = folder.begin(101);
    CHECK(last.outcome == core::RunOutcome::Unfinished);
    CHECK(last.dump.empty());
    REQUIRE_FALSE(last.report.empty());
    CHECK(folder.read(last.report).find("ended without saying so") != std::string::npos);
}

TEST_CASE("an old note under the same process number is not this run's crash")
{
    // A process number is used again. A note written BEFORE the run began is
    // some other run's.
    const Folder folder("stale");
    folder.write("crash-100.txt", "an old crash\n");
    std::error_code error;
    std::filesystem::last_write_time(folder.root / "crash-100.txt",
                                     std::filesystem::file_time_type::clock::now() - std::chrono::hours(2), error);
    REQUIRE_FALSE(error);
    (void)folder.begin(100);

    const core::LastRun& last = folder.begin(101);
    CHECK(last.outcome == core::RunOutcome::Unfinished);
    CHECK(folder.read(last.report).find("an old crash") == std::string::npos);
}

TEST_CASE("script errors nothing caught are counted, the first kept, and a clean run with them has a report")
{
    const Folder folder("errors");
    (void)folder.begin(100);
    core::noteScriptError("src/client/Hud.luau:12: attempt to index nil with 'Name'\nstack traceback:\n  Hud:12");
    core::noteScriptError("a second one");
    core::noteScriptError("a third");
    // The first is written at once -- a run that dies a moment later still has
    // it -- and the count as the run ends.
    CHECK(folder.read(core::runRecordPath(folder.log)).find("scriptErrors=1") != std::string::npos);
    core::endRun();
    CHECK(folder.read(core::runRecordPath(folder.log)).find("scriptErrors=3") != std::string::npos);
    folder.write("engine.previous.log", "[error] src/client/Hud.luau:12: attempt to index nil with 'Name'\n");

    const core::LastRun& last = folder.begin(101);
    CHECK(last.outcome == core::RunOutcome::Clean);
    CHECK(last.scriptErrors == 3);
    // One line: the record is a line a field.
    CHECK(last.firstScriptError ==
          "src/client/Hud.luau:12: attempt to index nil with 'Name' stack traceback:   Hud:12");
    REQUIRE_FALSE(last.report.empty());
    const std::string report = folder.read(last.report);
    CHECK(report.find("script errors nothing caught: 3") != std::string::npos);
    CHECK(report.find("ended when asked to") != std::string::npos);
}

TEST_CASE("a report is one run's: the next clean run takes it away")
{
    const Folder folder("replaced");
    (void)folder.begin(100);
    folder.write("crash-100.txt", "a fault\n");
    (void)folder.begin(101);
    REQUIRE(std::filesystem::exists(core::runReportPath(folder.log)));
    core::endRun();

    const core::LastRun& last = folder.begin(102);
    CHECK(last.outcome == core::RunOutcome::Clean);
    CHECK(last.report.empty());
    CHECK_FALSE(std::filesystem::exists(core::runReportPath(folder.log)));
}

TEST_CASE("a second run from the same folder keeps a record of its own")
{
    // The log of a second run is `engine_2.log` (`openLogFileBeside`), and its
    // record is named after it: the first run's is not read as its own.
    const Folder folder("beside");
    (void)folder.begin(100);
    CHECK(core::runRecordPath(folder.root / "engine_2.log") == folder.root / "engine_2.run");
    CHECK(core::runReportPath(folder.root / "engine_2.log") == folder.root / "engine_2.last-run.txt");
    CHECK(core::runRecordPath(folder.log) == folder.root / "engine.run");
}
