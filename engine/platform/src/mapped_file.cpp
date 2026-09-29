#include "engine/platform/file.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace engine::platform {

bool MappedFile::open(const std::filesystem::path& path)
{
    close();
#ifdef _WIN32
    HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > static_cast<unsigned long long>(SIZE_MAX)) {
        CloseHandle(file);
        return false;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    // The mapping holds the file open; the handle is not needed past here.
    CloseHandle(file);
    if (mapping == nullptr)
        return false;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (view == nullptr)
        return false;
    m_data = static_cast<const std::byte*>(view);
    m_size = static_cast<std::size_t>(size.QuadPart);
    return true;
#else
    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return false;
    struct stat info
    {
    };
    if (::fstat(file, &info) != 0 || info.st_size <= 0 || !S_ISREG(info.st_mode)) {
        ::close(file);
        return false;
    }
    void* view = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, file, 0);
    ::close(file);
    if (view == MAP_FAILED)
        return false;
    m_data = static_cast<const std::byte*>(view);
    m_size = static_cast<std::size_t>(info.st_size);
    return true;
#endif
}

void MappedFile::close() noexcept
{
    if (m_data == nullptr)
        return;
#ifdef _WIN32
    UnmapViewOfFile(m_data);
#else
    ::munmap(const_cast<std::byte*>(m_data), m_size);
#endif
    m_data = nullptr;
    m_size = 0;
}

} // namespace engine::platform
