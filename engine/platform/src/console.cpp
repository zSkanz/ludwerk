#include "engine/platform/console.h"

#include <cstdio>
#include <string>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <string>
#include <windows.h>
#endif

namespace engine::platform {
namespace {

void writeBytes(ConsoleStream stream, std::string_view utf8)
{
    std::FILE* file = stream == ConsoleStream::Err ? stderr : stdout;
    std::fwrite(utf8.data(), 1, utf8.size(), file);
    std::fflush(file);
}

#ifdef _WIN32

// GetConsoleMode succeeds only for a real console handle, which makes it the
// standard test for "is this redirected?".
bool consoleHandle(ConsoleStream stream, HANDLE& out)
{
    const HANDLE handle = GetStdHandle(stream == ConsoleStream::Err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return false;

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode))
        return false;

    out = handle;
    return true;
}

#endif

} // namespace

bool consoleClosesWithProcess() noexcept
{
#ifdef _WIN32
    // One process on the console is this one: nobody started it from a shell.
    DWORD processes[2] = {};
    return GetConsoleProcessList(processes, 2) == 1;
#else
    return false;
#endif
}

bool outputGoesUnread() noexcept
{
#if defined(__ANDROID__)
    return true;
#elif defined(_WIN32)
    DWORD processes[2] = {};
    const DWORD attached = GetConsoleProcessList(processes, 2);
    if (attached >= 2)
        return false;
    if (attached == 1)
        return true;
    // No console at all: read by somebody only where it was sent somewhere.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE)
        return true;
    const DWORD kind = GetFileType(out);
    return kind != FILE_TYPE_PIPE && kind != FILE_TYPE_DISK;
#else
    if (isatty(STDOUT_FILENO) != 0 || isatty(STDERR_FILENO) != 0)
        return false;
    struct stat info
    {
    };
    if (fstat(STDOUT_FILENO, &info) != 0)
        return true;
    return !S_ISFIFO(info.st_mode) && !S_ISREG(info.st_mode);
#endif
}

void writeConsole(ConsoleStream stream, std::string_view utf8)
{
    if (utf8.empty())
        return;

#if defined(__ANDROID__)
    // **An Android app has no console**: its stdout and stderr go nowhere a
    // person can read. The system log is where `adb logcat -s ludwerk` finds
    // everything the engine says, errors as errors.
    std::string line(utf8);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    __android_log_write(stream == ConsoleStream::Err ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "engine", line.c_str());
    return;
#endif

#ifdef _WIN32
    HANDLE handle = nullptr;
    if (consoleHandle(stream, handle)) {
        const int utf8Length = static_cast<int>(utf8.size());
        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), utf8Length, nullptr, 0);
        if (wideLength > 0) {
            std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
            if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), utf8Length, wide.data(), wideLength) == wideLength) {
                WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wideLength), nullptr, nullptr);
                return;
            }
        }

        // Malformed UTF-8 is a bug in the catalog or in a script's output, and
        // the raw bytes are more useful for finding it than nothing at all.
    }
#endif

    writeBytes(stream, utf8);
}

} // namespace engine::platform
