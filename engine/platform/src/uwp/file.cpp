#include "engine/platform/file.h"

#include <algorithm>
#include <atomic>
#include <limits>
// fileapifromapp requires Windows architecture declarations first.
// clang-format off
#include <windows.h>
#include <fileapifromapp.h>
// clang-format on

#include "engine/platform/platform.h"

namespace engine::platform {
namespace {
struct File
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
bool replace(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (ReplaceFileFromAppW(to.c_str(), from.c_str(), nullptr, 0, nullptr, nullptr))
        return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND && MoveFileFromAppW(from.c_str(), to.c_str());
}
bool write(const std::filesystem::path& path, std::span<const std::byte> bytes, bool durable)
{
    if (path.empty())
        return false;
    static std::atomic<u64> sequence{0};
    auto temporary = path;
    temporary +=
        L".engine-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence.fetch_add(1)) + L".tmp";
    bool okay = true;
    {
        File file{CreateFile2(temporary.c_str(), GENERIC_WRITE, 0, CREATE_NEW, nullptr)};
        if (file.handle == INVALID_HANDLE_VALUE)
            return false;
        std::size_t position = 0;
        while (position < bytes.size()) {
            const DWORD count = static_cast<DWORD>(
                (std::min)(bytes.size() - position, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(file.handle, bytes.data() + position, count, &written, nullptr) || !written) {
                okay = false;
                break;
            }
            position += written;
        }
        if (okay && durable)
            okay = FlushFileBuffers(file.handle) != FALSE;
    }
    if (okay)
        okay = replace(temporary, path);
    if (!okay)
        (void)DeleteFileFromAppW(temporary.c_str());
    return okay;
}
} // namespace
bool readFile(const std::filesystem::path& path, std::vector<std::byte>& out)
{
    File file{CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr)};
    if (file.handle == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < 0 ||
        static_cast<u64>(size.QuadPart) > (std::numeric_limits<std::size_t>::max)())
        return false;
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t position = 0;
    while (position < bytes.size()) {
        DWORD count = static_cast<DWORD>(
                  (std::min)(bytes.size() - position, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))),
              read = 0;
        if (!ReadFile(file.handle, bytes.data() + position, count, &read, nullptr) || !read)
            return false;
        position += read;
    }
    out = std::move(bytes);
    return true;
}
bool readTextFile(const std::filesystem::path& path, std::string& out)
{
    std::vector<std::byte> bytes;
    if (!readFile(path, bytes))
        return false;
    if (bytes.empty())
        out.clear();
    else
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}
bool fileExists(const std::filesystem::path& path)
{
    File f{CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr)};
    return f.handle != INVALID_HANDLE_VALUE;
}
bool writeFile(const std::filesystem::path& p, std::span<const std::byte> b)
{
    return write(p, b, false);
}
bool writeFileDurable(const std::filesystem::path& p, std::span<const std::byte> b)
{
    return write(p, b, true);
}
bool writeTextFile(const std::filesystem::path& p, std::string_view s)
{
    return writeFile(p, std::as_bytes(std::span(s)));
}
bool writeTextFileDurable(const std::filesystem::path& p, std::string_view s)
{
    return writeFileDurable(p, std::as_bytes(std::span(s)));
}
bool createDirectories(const std::filesystem::path& p)
{
    if (p.empty())
        return false;
    std::error_code e;
    std::filesystem::create_directories(p, e);
    return !e;
}
bool renameFile(const std::filesystem::path& a, const std::filesystem::path& b)
{
    return replace(a, b);
}
bool removeFile(const std::filesystem::path& p)
{
    std::error_code e;
    std::filesystem::remove(p, e);
    return !e;
}
std::filesystem::path preferencePath(std::string_view company, std::string_view name)
{
    const auto p = paths().userDir / pathComponent(company) / pathComponent(name);
    return createDirectories(p) ? p : std::filesystem::path{};
}
} // namespace engine::platform
