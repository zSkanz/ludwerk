// The file sink `architecture.md` §app names, and one property that is the
// whole point of it: a host installs a sink to capture output for its own log
// pane, and the file must still receive every line. A file that went quiet
// exactly when something was watching would be a file nobody could rely on.
#include <cstdio>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "engine/core/log.h"

#ifdef _WIN32
#include <share.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

using namespace engine;

namespace {

struct TempFile
{
    std::filesystem::path path;

    TempFile()
    {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() / ("engine-log-" + std::to_string(++counter) + ".txt");
        std::filesystem::remove(path);
    }

    ~TempFile()
    {
        core::closeLogFile();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] std::string read() const
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

} // namespace

TEST_CASE("the log file receives lines even while a sink is installed")
{
    TempFile file;
    REQUIRE(core::openLogFile(file.path));

    std::vector<std::string> captured;
    core::LogSink previous =
        core::setLogSink([&captured](core::LogLevel, std::string_view text) { captured.emplace_back(text); });

    core::logText(core::LogLevel::Info, "first");
    core::logText(core::LogLevel::Error, "second");

    core::setLogSink(std::move(previous));
    core::closeLogFile();

    // Both, not either: the sink is where a host's log pane reads from, and the
    // file is what survives a process that dies without unwinding.
    REQUIRE(captured.size() == 2);
    const std::string contents = file.read();
    CHECK(contents.find("[info] first") != std::string::npos);
    CHECK(contents.find("[error] second") != std::string::npos);
}

TEST_CASE("each line is flushed, so a process that dies leaves what it wrote")
{
    // Read while the file is still open. Without the per-line flush this comes
    // back empty, which is precisely the failure the file exists to prevent --
    // and precisely what the human's captured crash log looked like.
    TempFile file;
    REQUIRE(core::openLogFile(file.path));
    core::logText(core::LogLevel::Warn, "written before anything closed it");
    CHECK(file.read().find("written before anything closed it") != std::string::npos);
    core::closeLogFile();
}

TEST_CASE("opening a second file replaces the first, and closing twice is safe")
{
    TempFile first;
    TempFile second;
    REQUIRE(core::openLogFile(first.path));
    core::logText(core::LogLevel::Info, "to the first");
    REQUIRE(core::openLogFile(second.path));
    core::logText(core::LogLevel::Info, "to the second");
    core::closeLogFile();
    core::closeLogFile();

    CHECK(first.read().find("to the first") != std::string::npos);
    CHECK(first.read().find("to the second") == std::string::npos);
    CHECK(second.read().find("to the second") != std::string::npos);
}

TEST_CASE("no file open is the ordinary case and costs nothing")
{
    core::closeLogFile();
    core::logText(core::LogLevel::Info, "nowhere in particular");
    CHECK(true);
}

namespace {

// Another process writing a log: a handle that denies other writers, as the
// engine's own does.
struct OtherWriter
{
#ifdef _WIN32
    std::FILE* file = nullptr;
    explicit OtherWriter(const std::filesystem::path& path) : file(::_wfsopen(path.c_str(), L"ab", _SH_DENYWR)) {}
    ~OtherWriter()
    {
        if (file != nullptr)
            std::fclose(file);
    }
    [[nodiscard]] bool holding() const noexcept { return file != nullptr; }
#else
    int fd = -1;
    explicit OtherWriter(const std::filesystem::path& path)
        : fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644))
    {
        if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fd = -1;
        }
    }
    ~OtherWriter()
    {
        if (fd >= 0)
            ::close(fd);
    }
    [[nodiscard]] bool holding() const noexcept { return fd >= 0; }
#endif
    OtherWriter(const OtherWriter&) = delete;
    OtherWriter& operator=(const OtherWriter&) = delete;
};

} // namespace

TEST_CASE("a log another run is writing is left to it, and this run writes the one beside it")
{
    // **Two runs from one folder** (the owner's queue, Q0): a match tested
    // from one folder, or the gate's tests run side by side, started a second
    // engine whose log was the first one's. It rotated it out from under the
    // first, or could not open it and logged to the console alone -- and a
    // test that refuses warnings failed for it. As the major engines do, the
    // second run writes `engine_2.log`, and the first keeps `engine.log`.
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "engine-log-beside";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder);
    const std::filesystem::path first = folder / "engine.log";
    {
        OtherWriter other(first);
        REQUIRE(other.holding());
        const std::optional<std::filesystem::path> opened = core::openLogFileBeside(first);
        REQUIRE(opened.has_value());
        CHECK(opened->filename() == "engine_2.log");
        core::logText(core::LogLevel::Info, "the second run");
        core::closeLogFile();
        // Not rotated away from the run that holds it.
        CHECK(std::filesystem::exists(first));
        CHECK_FALSE(std::filesystem::exists(folder / "engine.previous.log"));
    }
    // With nobody holding it, it is this run's again, the last one's kept.
    const std::optional<std::filesystem::path> again = core::openLogFileBeside(first);
    REQUIRE(again.has_value());
    CHECK(again->filename() == "engine.log");
    core::closeLogFile();
    CHECK(std::filesystem::exists(folder / "engine.previous.log"));
    std::filesystem::remove_all(folder, ec);
}
