#include "engine/core/log.h"

#include <cstdio>
#ifdef _WIN32
#include <share.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <string>
#include <utility>

namespace engine::core {
namespace {

LogSink& sinkSlot()
{
    static LogSink sink;
    return sink;
}

std::FILE*& fileSlot() noexcept
{
    static std::FILE* file = nullptr;
    return file;
}

// Flushed per line, deliberately. The whole reason this file exists is to
// survive a process that dies without unwinding, and a buffer is exactly what
// such a process does not get to flush. It costs a write syscall per line on a
// path that already pays one for the console.
void writeFile(LogLevel level, std::string_view text)
{
    std::FILE* file = fileSlot();
    if (file == nullptr)
        return;
    const std::string line = formatLogLine(level, text);
    std::fwrite(line.data(), 1, line.size(), file);
    std::fflush(file);
}

void writeDefault(LogLevel level, std::string_view text)
{
    // Warnings and errors go to stderr so a headless CI run can separate them
    // from ordinary output without parsing.
    std::FILE* stream = (level == LogLevel::Warn || level == LogLevel::Error) ? stderr : stdout;
    const std::string line = formatLogLine(level, text);
    std::fwrite(line.data(), 1, line.size(), stream);
    std::fflush(stream);
}

} // namespace

std::string_view logLevelName(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

std::string formatLogLine(LogLevel level, std::string_view text)
{
    const std::string_view name = logLevelName(level);

    std::string line;
    line.reserve(name.size() + text.size() + 4);
    line += '[';
    line += name;
    line += "] ";
    line += text;
    line += '\n';
    return line;
}

LogSink setLogSink(LogSink sink)
{
    LogSink previous = std::move(sinkSlot());
    sinkSlot() = std::move(sink);
    return previous;
}

void resetLogSink()
{
    sinkSlot() = nullptr;
}

#ifndef _WIN32
namespace {
// Whether another writer HOLDS the file -- as opposed to there being no lock
// to take. **A file system without advisory locks answers every `flock` with
// an error, and it is not "would block"** (D593): a phone's shared storage,
// where a game's log has been since D569, is one on the phones that serve it
// through FUSE. Taken for "held", every one of the nine names a log may have
// was somebody else's, and the game ran with no log at all -- nine empty
// files in its folder, and nothing for a tester to send. Where there are no
// locks there is no second writer to tell from the first, and the log is
// written.
[[nodiscard]] bool lockedByAnother(int descriptor) noexcept
{
    if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0)
        return false;
    return errno == EWOULDBLOCK || errno == EAGAIN;
}
} // namespace
#endif

bool openLogFile(const std::filesystem::path& path)
{
    closeLogFile();
#ifdef _WIN32
    // `_wfsopen` with `_SH_DENYWR` rather than `fopen`/`_wfopen_s`, which open
    // for EXCLUSIVE access on this CRT: nothing else could read the file while
    // the engine was running, so tailing a live log -- which is most of why a
    // person wants one -- would fail with a sharing violation. Writers are still
    // denied, because two processes interleaving lines into one log is worse
    // than no log.
    std::FILE* file = ::_wfsopen(path.c_str(), L"wb", _SH_DENYWR);
#else
    std::FILE* file = std::fopen(path.c_str(), "wb");
    // The same rule as Windows' share mode, by an advisory lock: one writer.
    if (file != nullptr && lockedByAnother(::fileno(file))) {
        std::fclose(file);
        file = nullptr;
    }
#endif
    if (file == nullptr)
        return false;
    fileSlot() = file;
    return true;
}

namespace {

// Whether another writer holds `path`, asked without truncating it.
[[nodiscard]] bool heldByAnother(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return false;
#ifdef _WIN32
    std::FILE* probe = ::_wfsopen(path.c_str(), L"ab", _SH_DENYWR);
    if (probe == nullptr)
        return true;
    std::fclose(probe);
    return false;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    if (fd < 0)
        return true;
    const bool held = lockedByAnother(fd);
    ::close(fd);
    return held;
#endif
}

} // namespace

std::optional<std::filesystem::path> openLogFileBeside(const std::filesystem::path& path)
{
    for (int sibling = 1; sibling <= 9; ++sibling) {
        std::filesystem::path candidate = path;
        if (sibling > 1)
            candidate.replace_filename(path.stem().string() + "_" + std::to_string(sibling) +
                                       path.extension().string());
        if (heldByAnother(candidate))
            continue;
        // **The run before keeps its log** (audit A15): one generation, beside it.
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) {
            std::filesystem::path previous = candidate;
            previous.replace_filename(candidate.stem().string() + ".previous" + candidate.extension().string());
            std::filesystem::rename(candidate, previous, error);
        }
        if (openLogFile(candidate))
            return candidate;
    }
    return std::nullopt;
}

void closeLogFile() noexcept
{
    std::FILE*& file = fileSlot();
    if (file != nullptr) {
        std::fclose(file);
        file = nullptr;
    }
}

namespace {
// The detail of the line being handed to the sinks, on the thread handing it.
// Set around the sink's call alone and put back after it, so a line a sink
// logs while it runs carries its own detail and not the outer line's.
thread_local std::string_view t_detail;
} // namespace

void logText(LogLevel level, std::string_view text, std::string_view detail)
{
    // Before the sink rather than after, and outside the branch: a host that
    // installs a sink -- the DebugShell's log pane, a test capturing output --
    // must not be able to take the file away with it.
    writeFile(level, text);

    const std::string_view outer = std::exchange(t_detail, detail);
    if (const LogSink& sink = sinkSlot())
        sink(level, text);
    else
        writeDefault(level, text);
    t_detail = outer;
}

void logText(LogLevel level, std::string_view text)
{
    logText(level, text, {});
}

std::string_view logDetail() noexcept
{
    return t_detail;
}

void log(LogLevel level, TextKey key, std::span<const I18nArg> args)
{
    logText(level, engineCatalog().format(key, args));
}

} // namespace engine::core
