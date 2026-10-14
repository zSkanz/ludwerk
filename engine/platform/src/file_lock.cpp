#include "engine/platform/file.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace engine::platform {

FileLock::Taken FileLock::tryLock(const std::filesystem::path& path)
{
    unlock();
#ifdef _WIN32
    // Opened to share everything: what keeps a second process out is the lock
    // on the file's first byte, not the handle -- so the second one can open
    // the file, ask, and be told.
#if ENG_PLATFORM_UWP
    HANDLE file = CreateFile2(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_ALWAYS, nullptr);
#else
    HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
    if (file == INVALID_HANDLE_VALUE)
        return Taken::Unavailable;
    OVERLAPPED at{};
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
        const DWORD why = GetLastError();
        CloseHandle(file);
        return why == ERROR_LOCK_VIOLATION || why == ERROR_IO_PENDING ? Taken::Busy : Taken::Unavailable;
    }
    m_handle = reinterpret_cast<std::intptr_t>(file);
    return Taken::Yes;
#else
    const int file = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (file < 0)
        return Taken::Unavailable;
    if (::flock(file, LOCK_EX | LOCK_NB) != 0) {
        const int why = errno;
        ::close(file);
        return why == EWOULDBLOCK || why == EINTR ? Taken::Busy : Taken::Unavailable;
    }
    m_handle = file;
    return Taken::Yes;
#endif
}

void FileLock::unlock() noexcept
{
    if (m_handle == kNone)
        return;
#ifdef _WIN32
    const HANDLE file = reinterpret_cast<HANDLE>(m_handle);
    OVERLAPPED at{};
    UnlockFileEx(file, 0, 1, 0, &at);
    CloseHandle(file);
#else
    ::flock(static_cast<int>(m_handle), LOCK_UN);
    ::close(static_cast<int>(m_handle));
#endif
    m_handle = kNone;
}

} // namespace engine::platform
