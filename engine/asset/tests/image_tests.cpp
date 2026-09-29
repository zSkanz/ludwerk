#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "engine/asset/image.h"
#include "engine/core/i18n.h"

using engine::asset::decodeImage;
using engine::asset::Image;
using engine::asset::writePng;
using engine::core::u32;

namespace {

// The catalog has to be loaded or every error message below is a bare key, and
// a test asserting on message text would then assert on nothing. M2's Finding
// 11 and M3's are both this.
struct CatalogFixture
{
    CatalogFixture()
    {
        const auto result = engine::core::engineCatalog().loadFromFile(ENG_TEST_CATALOG);
        REQUIRE(result.ok);
    }
};

std::filesystem::path scratchFile(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "engine-asset-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

std::vector<std::byte> readAll(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        bytes[i] = static_cast<std::byte>(raw[i]);
    return bytes;
}

// Four pixels, four different colours, no two channels equal -- so a swizzle, a
// dropped channel or a transposed row all show up instead of matching by luck.
// The same reasoning as M1's clear colour.
std::vector<std::byte> distinctRgba()
{
    const unsigned char raw[]{
        0xFF, 0x00, 0x00, 0xFF, // top-left  opaque red
        0x00, 0xFF, 0x00, 0x80, // top-right half-alpha green
        0x00, 0x00, 0xFF, 0xFF, // bottom-left opaque blue
        0x10, 0x20, 0x30, 0x40, // bottom-right, four different values
    };
    std::vector<std::byte> bytes(sizeof(raw));
    for (std::size_t i = 0; i < sizeof(raw); ++i)
        bytes[i] = static_cast<std::byte>(raw[i]);
    return bytes;
}

} // namespace

TEST_CASE_FIXTURE(CatalogFixture, "image: a PNG round trip preserves every channel of every pixel")
{
    const std::vector<std::byte> source = distinctRgba();
    const auto path = scratchFile("roundtrip.png");

    REQUIRE_FALSE(writePng(path, source, 2, 2).has_value());

    Image decoded;
    REQUIRE_FALSE(decodeImage(readAll(path), decoded).has_value());

    CHECK(decoded.valid());
    CHECK(decoded.width == 2u);
    CHECK(decoded.height == 2u);
    CHECK(decoded.sourceChannels == 4u);
    CHECK(decoded.pixels == source);

    std::filesystem::remove(path);
}

TEST_CASE_FIXTURE(CatalogFixture, "image: a source with fewer channels still decodes to RGBA, and says so")
{
    // A three-channel PNG written by stb, decoded back. The point of the
    // assertion is `sourceChannels`: the pixels come back as RGBA either way,
    // and the count is the only way a caller tells an image that meant to be
    // opaque from one that happens to be.
    const unsigned char rgb[]{
        0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, //
        0x00, 0x00, 0xFF, 0xFF, 0x10, 0x20, 0x30, 0xFF,
    };
    std::vector<std::byte> opaque(sizeof(rgb));
    for (std::size_t i = 0; i < sizeof(rgb); ++i)
        opaque[i] = static_cast<std::byte>(rgb[i]);

    const auto path = scratchFile("opaque.png");
    REQUIRE_FALSE(writePng(path, opaque, 2, 2).has_value());

    Image decoded;
    REQUIRE_FALSE(decodeImage(readAll(path), decoded).has_value());
    CHECK(decoded.pixels == opaque);
    // stb writes the four channels it was given, so the round trip reports 4;
    // what this pins is that `sourceChannels` reports the FILE's count rather
    // than the 4 the decode forced.
    CHECK(decoded.sourceChannels == 4u);

    std::filesystem::remove(path);
}

TEST_CASE_FIXTURE(CatalogFixture, "image: garbage is an error, not a zero-sized image")
{
    const std::vector<std::byte> notAnImage(64, std::byte{0x7F});

    Image decoded;
    const auto error = decodeImage(notAnImage, decoded);
    REQUIRE(error.has_value());
    // Engine messages carry their key AND the resolved sentence (api-design.md
    // §6), so the assertion that the catalog was actually loaded is the English
    // half -- asserting the key alone would pass against an unloaded catalog,
    // which is M2's Finding 11.
    CHECK(error->message.find("asset.image.err.decode_failed") != std::string::npos);
    CHECK(error->message.find("Could not decode the image.") != std::string::npos);
    CHECK_FALSE(decoded.valid());

    Image untouched;
    CHECK(decodeImage({}, untouched).has_value());
}

TEST_CASE_FIXTURE(CatalogFixture, "image: a decode failure leaves the output empty rather than half-filled")
{
    Image decoded;
    // Fill it first: the contract is that a failed decode resets, not that it
    // leaves whatever the caller had.
    decoded.width = 7;
    decoded.height = 7;
    decoded.pixels.assign(16, std::byte{0xAB});

    REQUIRE(decodeImage(std::vector<std::byte>(8, std::byte{0}), decoded).has_value());
    CHECK(decoded.width == 0u);
    CHECK(decoded.height == 0u);
    CHECK(decoded.pixels.empty());
}

TEST_CASE_FIXTURE(CatalogFixture, "image: writePng rejects a buffer that does not match the size")
{
    const std::vector<std::byte> tooSmall(4 * 4 * 4 - 1, std::byte{0});
    CHECK(writePng(scratchFile("never.png"), tooSmall, 4, 4).has_value());
    CHECK(writePng(scratchFile("never.png"), tooSmall, 0, 4).has_value());
    CHECK_FALSE(std::filesystem::exists(scratchFile("never.png")));
}

// --- Heightmaps -----------------------------------------------------------------

namespace {

std::vector<std::byte> bytesOf(std::initializer_list<unsigned> values)
{
    std::vector<std::byte> out;
    for (const unsigned value : values)
        out.push_back(static_cast<std::byte>(value));
    return out;
}

} // namespace

TEST_CASE_FIXTURE(CatalogFixture, "heightmap: a sixteen-bit PNG keeps its sixteen bits")
{
    // A 2x1 greyscale PNG at sixteen bits, samples 0x0102 and 0xFFFE: values an
    // eight-bit decode could not tell from 0x0100 and 0xFF00.
    const std::vector<std::byte> png = bytesOf(
        {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
         0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x81, 0xd9, 0xfc, 0x15, 0x00, 0x00, 0x00,
         0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x64, 0xfa, 0xff, 0x0f, 0x00, 0x03, 0x0b, 0x02, 0x01,
         0x84, 0x91, 0xe8, 0x13, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82});
    engine::asset::HeightImage image;
    REQUIRE_FALSE(engine::asset::decodeHeightmap(png, "ridge.png", image).has_value());
    REQUIRE(image.valid());
    CHECK(image.width == 2u);
    CHECK(image.height == 1u);
    CHECK(image.samples[0] == static_cast<float>(0x0102) / 65535.0f);
    CHECK(image.samples[1] == static_cast<float>(0xFFFE) / 65535.0f);
}

TEST_CASE_FIXTURE(CatalogFixture, "heightmap: RAW is square little-endian sixteen-bit samples")
{
    const std::vector<std::byte> raw = bytesOf({0x00, 0x00, 0xff, 0xff, 0x00, 0x80, 0x01, 0x00});
    engine::asset::HeightImage image;
    REQUIRE_FALSE(engine::asset::decodeHeightmap(raw, "island.R16", image).has_value());
    CHECK(image.width == 2u);
    CHECK(image.height == 2u);
    CHECK(image.samples[0] == 0.0f);
    CHECK(image.samples[1] == 1.0f);
    CHECK(image.samples[2] == static_cast<float>(0x8000) / 65535.0f);
    CHECK(image.samples[3] == 1.0f / 65535.0f);

    // Six samples is not a square, and a guessed side would be a sheared map.
    const std::vector<std::byte> ragged(12, std::byte{0});
    CHECK(engine::asset::decodeHeightmap(ragged, "island.raw", image).has_value());
    CHECK_FALSE(image.valid());
}

TEST_CASE_FIXTURE(CatalogFixture, "heightmap: an eight-bit PNG decodes to the same range")
{
    const std::filesystem::path path = scratchFile("heightmap_8bit.png");
    const std::vector<std::byte> pixels = bytesOf({0, 0, 0, 255, 255, 255, 255, 255});
    REQUIRE_FALSE(writePng(path, pixels, 2, 1).has_value());
    const std::vector<std::byte> encoded = readAll(path);

    engine::asset::HeightImage image;
    REQUIRE_FALSE(engine::asset::decodeHeightmap(encoded, "flat.png", image).has_value());
    CHECK(image.samples[0] == 0.0f);
    CHECK(image.samples[1] == 1.0f);
}

TEST_CASE("heightmap: resampling lands the corners on the corners and blends between")
{
    engine::asset::HeightImage image;
    image.width = 2;
    image.height = 2;
    image.samples = {0.0f, 1.0f, 0.0f, 1.0f};

    // At its own size, exact, mapped onto the range.
    const std::vector<float> same = engine::asset::resampleHeights(image, 2, 2, 10.0f, 30.0f);
    REQUIRE(same.size() == 4u);
    CHECK(same[0] == 10.0f);
    CHECK(same[1] == 30.0f);
    CHECK(same[2] == 10.0f);
    CHECK(same[3] == 30.0f);

    // Stretched to three columns, the middle one is halfway.
    const std::vector<float> wide = engine::asset::resampleHeights(image, 3, 2, 0.0f, 8.0f);
    REQUIRE(wide.size() == 6u);
    CHECK(wide[0] == 0.0f);
    CHECK(wide[1] == 4.0f);
    CHECK(wide[2] == 8.0f);

    CHECK(engine::asset::resampleHeights(image, 0, 2, 0.0f, 1.0f).empty());
}

namespace {

// A PNG that is its signature and a header claiming `width` x `height`, and
// nothing else: what a hostile file costs to make.
[[nodiscard]] std::vector<std::byte> pngClaiming(unsigned width, unsigned height)
{
    std::vector<unsigned> bytes{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (const unsigned value : {width, height}) {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back((value >> shift) & 0xFFu);
    }
    for (const unsigned rest : {8u, 6u, 0u, 0u, 0u, 0u, 0u, 0u, 0u})
        bytes.push_back(rest);
    std::vector<std::byte> out;
    for (const unsigned value : bytes)
        out.push_back(static_cast<std::byte>(value));
    return out;
}

} // namespace

TEST_CASE_FIXTURE(CatalogFixture,
                  "image: a header claiming a huge picture is refused before anything is allocated (audit F8)")
{
    // Past stb's own dimension limit, and inside it but past the area an
    // image may have: both refused from the header alone.
    for (const auto& [width, height] : {std::pair{60000u, 60000u}, std::pair{10000u, 10000u}}) {
        Image decoded;
        const auto error = decodeImage(pngClaiming(width, height), decoded);
        REQUIRE(error.has_value());
        CHECK_FALSE(decoded.valid());
    }
}

TEST_CASE_FIXTURE(CatalogFixture, "image: a format this engine does not use is not decoded (audit F8)")
{
    // A 1 x 1 BMP: stb would read it, and nothing in a project is one.
    const std::vector<unsigned> bmp{'B', 'M', 58, 0, 0, 0, 0, 0, 0,  0, 54, 0, 0, 0, 40, 0, 0,   0, 1, 0,
                                    0,   0,   1,  0, 0, 0, 1, 0, 24, 0, 0,  0, 0, 0, 4,  0, 0,   0, 0, 0,
                                    0,   0,   0,  0, 0, 0, 0, 0, 0,  0, 0,  0, 0, 0, 0,  0, 255, 0};
    std::vector<std::byte> bytes;
    for (const unsigned value : bmp)
        bytes.push_back(static_cast<std::byte>(value));
    Image decoded;
    CHECK(decodeImage(bytes, decoded).has_value());
}
