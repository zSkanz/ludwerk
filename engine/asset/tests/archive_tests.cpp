// The archives an export ends in (ADR 0104 §7), read back byte by byte: the
// `.tar.gz` a Linux player ships in carries the execute bit whatever made it,
// and the `.zip` beside a Windows folder opens in anything.
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/asset/archive.h"

// stb_image's inflate, compiled in `image.cpp`: the reader this checks against.
extern "C" int stbi_zlib_decode_noheader_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

using namespace engine;

namespace {

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string& text)
{
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

[[nodiscard]] std::string readAll(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

[[nodiscard]] core::u32 u16At(const std::string& bytes, std::size_t at)
{
    return static_cast<core::u32>(static_cast<unsigned char>(bytes[at])) |
           (static_cast<core::u32>(static_cast<unsigned char>(bytes[at + 1])) << 8);
}

[[nodiscard]] core::u32 u32At(const std::string& bytes, std::size_t at)
{
    return u16At(bytes, at) | (u16At(bytes, at + 2) << 16);
}

[[nodiscard]] std::string inflate(const std::string& packed, std::size_t size)
{
    std::string out(size, '\0');
    const int written = stbi_zlib_decode_noheader_buffer(out.data(), static_cast<int>(size), packed.data(),
                                                         static_cast<int>(packed.size()));
    REQUIRE(written == static_cast<int>(size));
    return out;
}

// A repetitive text, so deflate has something to take out of it.
const std::string kScript = [] {
    std::string text;
    for (int line = 0; line < 200; ++line)
        text += "print(\"Hello from the engine\")\n";
    return text;
}();

[[nodiscard]] std::vector<asset::ArchiveEntry> sample()
{
    std::vector<asset::ArchiveEntry> entries;
    entries.push_back({"Game/game",
                       bytesOf("\x7f"
                               "ELF player"),
                       true});
    entries.push_back({"Game/src/client/init.luau", bytesOf(kScript), false});
    entries.push_back({"Game/empty.txt", {}, false});
    return entries;
}

struct Scratch
{
    std::filesystem::path root = std::filesystem::temp_directory_path() / "engine-archive-test";
    Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }
    ~Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("crc32 is the IEEE one both formats carry")
{
    const std::vector<std::byte> text = bytesOf("123456789");
    CHECK(asset::crc32(text.data(), text.size()) == 0xCBF43926u);
}

TEST_CASE("a .tar.gz carries mode 0755 on the player and 0644 on the rest")
{
    Scratch scratch;
    const std::filesystem::path file = scratch.root / "game.tar.gz";
    REQUIRE_FALSE(asset::writeTarGz(sample(), file).has_value());

    const std::string gz = readAll(file);
    REQUIRE(gz.size() > 18);
    CHECK(static_cast<unsigned char>(gz[0]) == 0x1F);
    CHECK(static_cast<unsigned char>(gz[1]) == 0x8B);
    const core::u32 size = u32At(gz, gz.size() - 4);
    const std::string tar = inflate(gz.substr(10, gz.size() - 18), size);
    const std::vector<std::byte> tarBytes = bytesOf(tar);
    CHECK(u32At(gz, gz.size() - 8) == asset::crc32(tarBytes.data(), tarBytes.size()));
    REQUIRE(tar.size() % 512 == 0);

    // The first header: the player, executable.
    CHECK(std::string(tar.c_str()) == "Game/game");
    CHECK(std::string(tar.c_str() + 100) == "0000755");
    CHECK(std::string(tar.c_str() + 257, 5) == "ustar");
    // Its checksum, as a reader recomputes it: the field itself as spaces.
    std::string header = tar.substr(0, 512);
    std::memset(header.data() + 148, ' ', 8);
    core::u32 sum = 0;
    for (const char c : header)
        sum += static_cast<unsigned char>(c);
    CHECK(std::stoul(std::string(tar.c_str() + 148), nullptr, 8) == sum);

    // The second: a script, one block of header then its bytes.
    const std::size_t second = 512 + 512;
    CHECK(std::string(tar.c_str() + second) == "Game/src/client/init.luau");
    CHECK(std::string(tar.c_str() + second + 100) == "0000644");
    CHECK(tar.substr(second + 512, kScript.size()) == kScript);
}

TEST_CASE("a .zip deflates what shrinks, stores what does not, and says each mode")
{
    Scratch scratch;
    const std::filesystem::path file = scratch.root / "game.zip";
    REQUIRE_FALSE(asset::writeZip(sample(), file).has_value());

    const std::string zip = readAll(file);
    const std::size_t end = zip.size() - 22;
    REQUIRE(u32At(zip, end) == 0x06054B50u);
    CHECK(u16At(zip, end + 10) == 3);

    // Walk the central directory, and follow each entry to its local header.
    std::size_t at = u32At(zip, end + 16);
    std::vector<std::string> names;
    for (int entry = 0; entry < 3; ++entry) {
        REQUIRE(u32At(zip, at) == 0x02014B50u);
        const core::u32 method = u16At(zip, at + 10);
        const core::u32 packedSize = u32At(zip, at + 20);
        const core::u32 size = u32At(zip, at + 24);
        const core::u32 nameLength = u16At(zip, at + 28);
        const core::u32 mode = u32At(zip, at + 38) >> 16;
        const core::u32 local = u32At(zip, at + 42);
        const std::string name = zip.substr(at + 46, nameLength);
        names.push_back(name);

        REQUIRE(u32At(zip, local) == 0x04034B50u);
        const std::size_t data = local + 30 + u16At(zip, local + 26) + u16At(zip, local + 28);
        const std::string packed = zip.substr(data, packedSize);
        const std::string contents = method == 8 ? inflate(packed, size) : packed;
        const std::vector<std::byte> contentBytes = bytesOf(contents);
        CHECK(u32At(zip, at + 16) == asset::crc32(contentBytes.data(), contentBytes.size()));

        if (name == "Game/game") {
            CHECK(mode == 0100755u);
            CHECK(method == 0);
        }
        if (name == "Game/src/client/init.luau") {
            CHECK(mode == 0100644u);
            CHECK(method == 8);
            CHECK(packedSize < size);
            CHECK(contents == kScript);
        }
        at += 46 + nameLength + u16At(zip, at + 30) + u16At(zip, at + 32);
    }
    CHECK(names.size() == 3);
}

TEST_CASE("a folder is collected in path order, the same archive for the same tree")
{
    Scratch scratch;
    const std::filesystem::path folder = scratch.root / "dist";
    std::filesystem::create_directories(folder / "game" / "src");
    std::ofstream(folder / "zeta.txt") << "z";
    std::ofstream(folder / "game" / "src" / "a.luau") << "a";
    std::ofstream(folder / "mygame", std::ios::binary) << "player";

    std::vector<asset::ArchiveEntry> entries;
    REQUIRE_FALSE(asset::collectArchiveEntries(folder, "MyGame", {"mygame"}, entries).has_value());
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].path == "MyGame/game/src/a.luau");
    CHECK(entries[1].path == "MyGame/mygame");
    CHECK(entries[1].executable);
    CHECK(entries[2].path == "MyGame/zeta.txt");
    CHECK_FALSE(entries[2].executable);

    const std::filesystem::path first = scratch.root / "one.tar.gz";
    const std::filesystem::path second = scratch.root / "two.tar.gz";
    REQUIRE_FALSE(asset::writeTarGz(entries, first).has_value());
    REQUIRE_FALSE(asset::writeTarGz(entries, second).has_value());
    CHECK(readAll(first) == readAll(second));
}
