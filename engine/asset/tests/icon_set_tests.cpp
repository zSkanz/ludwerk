// One square PNG in, every platform's icons out (ADR 0104 §2): the sizes, the
// `.ico`'s directory, the Android resource tree, and the refusals.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/asset/icon_set.h"
#include "engine/asset/image.h"

using namespace engine;

namespace {

// A square of one colour, fully opaque, with a transparent corner.
[[nodiscard]] asset::Image square(core::u32 side)
{
    asset::Image image;
    image.width = side;
    image.height = side;
    image.sourceChannels = 4;
    image.pixels.resize(static_cast<std::size_t>(side) * side * 4u);
    for (core::u32 y = 0; y < side; ++y) {
        for (core::u32 x = 0; x < side; ++x) {
            std::byte* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * side + x) * 4u;
            const bool corner = x < side / 4 && y < side / 4;
            pixel[0] = std::byte{200};
            pixel[1] = std::byte{40};
            pixel[2] = std::byte{60};
            pixel[3] = corner ? std::byte{0} : std::byte{255};
        }
    }
    return image;
}

[[nodiscard]] std::vector<std::byte> readAll(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index)
        out[index] = static_cast<std::byte>(bytes[index]);
    return out;
}

[[nodiscard]] core::u32 u16At(const std::vector<std::byte>& bytes, std::size_t at)
{
    return std::to_integer<core::u32>(bytes[at]) | (std::to_integer<core::u32>(bytes[at + 1]) << 8);
}

[[nodiscard]] asset::Image decoded(const std::filesystem::path& path)
{
    asset::Image image;
    REQUIRE_FALSE(asset::decodeImage(readAll(path), image).has_value());
    return image;
}

struct Scratch
{
    std::filesystem::path root = std::filesystem::temp_directory_path() / "engine-icon-set-test";
    Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    ~Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("a resize is the size asked for, and keeps a transparent corner transparent")
{
    const asset::Image small = asset::resizeImage(square(1024), 64);
    REQUIRE(small.valid());
    CHECK(small.width == 64);
    CHECK(small.height == 64);
    // The corner is a quarter of the picture: still clear.
    CHECK(std::to_integer<int>(small.pixels[3]) == 0);
    // And the rest keeps its colour, with no dark fringe from the clear corner.
    const std::byte* middle = small.pixels.data() + (32 * 64 + 32) * 4;
    CHECK(std::to_integer<int>(middle[0]) == 200);
    CHECK(std::to_integer<int>(middle[3]) == 255);
}

TEST_CASE("one picture makes Windows', Linux's and Android's icons")
{
    Scratch scratch;
    asset::IconSetReport report;
    asset::IconSetOptions options;
    options.background = 0x1E90FF;
    REQUIRE_FALSE(asset::writeIconSet(square(1024), scratch.root, options, report).has_value());
    CHECK(report.warnings.empty());

    // The `.ico`: seven entries, 16 to 256, the last written as 0.
    const std::vector<std::byte> ico = readAll(scratch.root / "windows" / "icon.ico");
    REQUIRE(ico.size() > 6 + 7 * 16);
    CHECK(u16At(ico, 2) == 1);
    CHECK(u16At(ico, 4) == 7);
    const core::u32 expected[] = {16, 24, 32, 48, 64, 128, 0};
    for (std::size_t entry = 0; entry < 7; ++entry)
        CHECK(std::to_integer<core::u32>(ico[6 + entry * 16]) == expected[entry]);

    CHECK(decoded(scratch.root / "linux" / "icon.png").width == 512);

    const std::filesystem::path res = scratch.root / "android" / "res";
    CHECK(decoded(res / "mipmap-mdpi" / "ic_launcher.png").width == 48);
    CHECK(decoded(res / "mipmap-xxxhdpi" / "ic_launcher.png").width == 192);
    CHECK(decoded(res / "mipmap-xxxhdpi" / "ic_launcher_foreground.png").width == 432);
    CHECK(std::filesystem::exists(res / "mipmap-anydpi-v26" / "ic_launcher.xml"));
    std::ifstream colour(res / "values" / "ic_launcher_background.xml");
    const std::string text((std::istreambuf_iterator<char>(colour)), std::istreambuf_iterator<char>());
    CHECK(text.find("#1E90FF") != std::string::npos);
    CHECK(report.written.size() == 1 + 1 + 5 * 2 + 2);
}

TEST_CASE("a picture that is not square is refused, and a small one warns")
{
    Scratch scratch;
    asset::Image wide = square(512);
    wide.width = 256;
    wide.height = 1024;
    asset::IconSetReport report;
    CHECK(asset::writeIconSet(wide, scratch.root, {}, report).has_value());

    asset::IconSetReport small;
    asset::IconSetOptions windowsOnly;
    windowsOnly.linuxDesktop = false;
    windowsOnly.android = false;
    REQUIRE_FALSE(asset::writeIconSet(square(128), scratch.root, windowsOnly, small).has_value());
    CHECK(small.warnings.size() == 1);
    CHECK(small.written.size() == 1);
}
