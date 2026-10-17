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
#if !defined(_WIN32)
#include <cerrno>
#include <poll.h>
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

InputRead readStandardInput(std::string& out, core::u32 timeoutMs)
{
    char buffer[4096];
#if defined(_WIN32)
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE)
        return InputRead::Ended;
    DWORD read = 0;
    switch (GetFileType(input)) {
    case FILE_TYPE_PIPE: {
        // A pipe says how much is waiting without blocking for it.
        const ULONGLONG until = GetTickCount64() + timeoutMs;
        while (true) {
            DWORD waiting = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &waiting, nullptr))
                return InputRead::Ended;
            if (waiting > 0)
                break;
            if (GetTickCount64() >= until)
                return InputRead::Nothing;
            Sleep(10);
        }
        if (!ReadFile(input, buffer, sizeof(buffer), &read, nullptr) || read == 0)
            return InputRead::Ended;
        break;
    }
    case FILE_TYPE_CHAR: {
        // A console: signalled when there is input of any kind, and read a
        // line at a time once there is.
        DWORD mode = 0;
        if (GetConsoleMode(input, &mode)) {
            if (WaitForSingleObject(input, timeoutMs) != WAIT_OBJECT_0)
                return InputRead::Nothing;
            DWORD events = 0;
            INPUT_RECORD record{};
            // Only a key going down starts a line; the rest -- focus, the
            // mouse, a key coming up -- is taken off the queue and waited past.
            if (!PeekConsoleInputW(input, &record, 1, &events) || events == 0)
                return InputRead::Nothing;
            if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
                (void)ReadConsoleInputW(input, &record, 1, &events);
                return InputRead::Nothing;
            }
        }
        if (!ReadFile(input, buffer, sizeof(buffer), &read, nullptr) || read == 0)
            return InputRead::Ended;
        break;
    }
    default:
        // A file, or nothing at all (`NUL`): a read never waits.
        if (!ReadFile(input, buffer, sizeof(buffer), &read, nullptr) || read == 0)
            return InputRead::Ended;
        break;
    }
    out.append(buffer, read);
    return InputRead::Data;
#else
    pollfd waiting{};
    waiting.fd = STDIN_FILENO;
    waiting.events = POLLIN;
    const int ready = ::poll(&waiting, 1, static_cast<int>(timeoutMs));
    if (ready == 0)
        return InputRead::Nothing;
    if (ready < 0)
        return errno == EINTR ? InputRead::Nothing : InputRead::Ended;
    if ((waiting.revents & (POLLIN | POLLHUP)) == 0)
        return InputRead::Ended;
    const ssize_t read = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (read < 0)
        return errno == EINTR || errno == EAGAIN ? InputRead::Nothing : InputRead::Ended;
    if (read == 0)
        return InputRead::Ended;
    out.append(buffer, static_cast<std::size_t>(read));
    return InputRead::Data;
#endif
}

} // namespace engine::platform
