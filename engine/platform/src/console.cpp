#include "engine/platform/console.h"

#include <cstdio>
#include <string>
#if defined(__ANDROID__)
#include <android/log.h>
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
