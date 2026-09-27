#include "engine/asset/archive.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "engine/core/i18n.h"

// `image.cpp` compiles stb_image_write, and this is its deflate: declared here
// rather than by including the header a second time.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace engine::asset {
namespace {

using core::u32;

[[nodiscard]] core::EngineError writeFailed(const std::filesystem::path& path)
{
    const std::array<core::I18nArg, 1> args{core::I18nArg{"path", path.string()}};
    return core::makeError(ENG_TR("asset.image.err.write_failed"), args);
}

// Raw deflate: stb writes a zlib stream -- a two-byte header, the deflate, a
// four-byte Adler-32 -- and both archives want only the middle.
[[nodiscard]] bool deflate(const std::vector<std::byte>& input, std::vector<std::byte>& out)
{
    out.clear();
    if (input.empty())
        return true;
    int length = 0;
    unsigned char* zlib =
        stbi_zlib_compress(const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(input.data())),
                           static_cast<int>(input.size()), &length, 8);
    if (zlib == nullptr || length < 6) {
        std::free(zlib);
        return false;
    }
    out.assign(reinterpret_cast<const std::byte*>(zlib) + 2, reinterpret_cast<const std::byte*>(zlib) + length - 4);
    std::free(zlib);
    return true;
}

void put16(std::vector<std::byte>& out, u32 value)
{
    out.push_back(static_cast<std::byte>(value & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

void put32(std::vector<std::byte>& out, u32 value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
}

void putText(std::vector<std::byte>& out, const std::string& text)
{
    for (const char c : text)
        out.push_back(static_cast<std::byte>(c));
}

[[nodiscard]] bool writeFile(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

// One 512-byte ustar header. A name longer than 100 bytes is split at a '/'
// into the 155-byte prefix, which is how ustar spells a long path.
[[nodiscard]] bool tarHeader(std::vector<std::byte>& out, const std::string& path, std::size_t size, bool executable)
{
    std::array<char, 512> header{};
    std::string name = path;
    std::string prefix;
    if (name.size() > 100) {
        const std::size_t cut = name.rfind('/', 155);
        if (cut == std::string::npos || name.size() - cut - 1 > 100)
            return false;
        prefix = name.substr(0, cut);
        name = name.substr(cut + 1);
    }
    std::memcpy(header.data(), name.data(), name.size());
    std::snprintf(header.data() + 100, 8, "%07o", executable ? 0755u : 0644u);
    std::snprintf(header.data() + 108, 8, "%07o", 0u);
    std::snprintf(header.data() + 116, 8, "%07o", 0u);
    std::snprintf(header.data() + 124, 12, "%011llo", static_cast<unsigned long long>(size));
    // No time: the same tree makes the same archive.
    std::snprintf(header.data() + 136, 12, "%011o", 0u);
    header[156] = '0';
    std::memcpy(header.data() + 257, "ustar", 6);
    std::memcpy(header.data() + 263, "00", 2);
    std::memcpy(header.data() + 345, prefix.data(), prefix.size());
    // The checksum is computed with its own field as spaces.
    std::memset(header.data() + 148, ' ', 8);
    u32 sum = 0;
    for (const char c : header)
        sum += static_cast<unsigned char>(c);
    std::snprintf(header.data() + 148, 8, "%06o", sum);
    header[155] = ' ';
    for (const char c : header)
        out.push_back(static_cast<std::byte>(c));
    return true;
}

} // namespace

u32 crc32(const std::byte* data, std::size_t size) noexcept
{
    u32 crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= std::to_integer<u32>(data[index]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::optional<core::EngineError> collectArchiveEntries(const std::filesystem::path& root, const std::string& prefix,
                                                       const std::vector<std::string>& executables,
                                                       std::vector<ArchiveEntry>& out)
{
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec))
            files.push_back(it->path());
    }
    if (ec)
        return writeFailed(root);
    std::vector<std::pair<std::string, std::filesystem::path>> named;
    for (const std::filesystem::path& file : files)
        named.emplace_back(std::filesystem::relative(file, root, ec).generic_string(), file);
    std::sort(named.begin(), named.end());
    for (const auto& [relative, file] : named) {
        ArchiveEntry entry;
        entry.path = prefix.empty() ? relative : prefix + "/" + relative;
        entry.executable = std::find(executables.begin(), executables.end(), relative) != executables.end();
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            return writeFailed(file);
        stream.seekg(0, std::ios::end);
        entry.bytes.resize(static_cast<std::size_t>(stream.tellg()));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(entry.bytes.data()), static_cast<std::streamsize>(entry.bytes.size()));
        out.push_back(std::move(entry));
    }
    return std::nullopt;
}

std::optional<core::EngineError> writeZip(const std::vector<ArchiveEntry>& entries, const std::filesystem::path& path)
{
    std::vector<std::byte> out;
    std::vector<std::byte> central;
    for (const ArchiveEntry& entry : entries) {
        std::vector<std::byte> packed;
        if (!deflate(entry.bytes, packed))
            return writeFailed(path);
        // Deflate only when it helps: an already compressed pack is stored.
        const bool stored = packed.size() >= entry.bytes.size();
        const std::vector<std::byte>& data = stored ? entry.bytes : packed;
        const u32 crc = crc32(entry.bytes.data(), entry.bytes.size());
        const u32 offset = static_cast<u32>(out.size());
        const u32 method = stored ? 0 : 8;

        put32(out, 0x04034B50u);
        put16(out, 20);
        put16(out, 0x0800); // UTF-8 names.
        put16(out, method);
        put16(out, 0);
        put16(out, 0x21); // 1980-01-01: no time, so the same tree is the same zip.
        put32(out, crc);
        put32(out, static_cast<u32>(data.size()));
        put32(out, static_cast<u32>(entry.bytes.size()));
        put16(out, static_cast<u32>(entry.path.size()));
        put16(out, 0);
        putText(out, entry.path);
        out.insert(out.end(), data.begin(), data.end());

        put32(central, 0x02014B50u);
        put16(central, 0x0314); // Made by Unix, so the external attributes carry a mode.
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, method);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, static_cast<u32>(data.size()));
        put32(central, static_cast<u32>(entry.bytes.size()));
        put16(central, static_cast<u32>(entry.path.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, (entry.executable ? 0100755u : 0100644u) << 16);
        put32(central, offset);
        putText(central, entry.path);
    }
    const u32 centralOffset = static_cast<u32>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054B50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<u32>(entries.size()));
    put16(out, static_cast<u32>(entries.size()));
    put32(out, static_cast<u32>(central.size()));
    put32(out, centralOffset);
    put16(out, 0);
    if (!writeFile(path, out))
        return writeFailed(path);
    return std::nullopt;
}

std::optional<core::EngineError> writeTarGz(const std::vector<ArchiveEntry>& entries, const std::filesystem::path& path)
{
    std::vector<std::byte> tar;
    for (const ArchiveEntry& entry : entries) {
        if (!tarHeader(tar, entry.path, entry.bytes.size(), entry.executable))
            return writeFailed(path);
        tar.insert(tar.end(), entry.bytes.begin(), entry.bytes.end());
        tar.resize((tar.size() + 511) / 512 * 512, std::byte{0});
    }
    // Two empty blocks end an archive.
    tar.resize(tar.size() + 1024, std::byte{0});

    std::vector<std::byte> packed;
    if (!deflate(tar, packed))
        return writeFailed(path);
    std::vector<std::byte> out;
    // gzip: magic, deflate, no flags, no time, no extra flags, an unknown OS.
    for (const u32 byte : {0x1Fu, 0x8Bu, 8u, 0u, 0u, 0u, 0u, 0u, 0u, 0xFFu})
        out.push_back(static_cast<std::byte>(byte));
    out.insert(out.end(), packed.begin(), packed.end());
    put32(out, crc32(tar.data(), tar.size()));
    put32(out, static_cast<u32>(tar.size()));
    if (!writeFile(path, out))
        return writeFailed(path);
    return std::nullopt;
}

} // namespace engine::asset
