#include <array>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/asset/image.h"
#include "engine/asset/texture.h"
#include "engine/core/i18n.h"

using namespace engine::asset;
using engine::core::engineCatalog;
using engine::core::u32;
using engine::core::usize;

namespace {

void seedRealCatalog()
{
    const auto result = engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

// The KTX2 identifier, so the bytes below are the shape of a real file rather
// than obvious noise -- a reader that only checked the first byte would pass a
// test built from zeroes.
constexpr unsigned char Ktx2Identifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

} // namespace

// The round trip -- encode a real image, transcode it back -- lives in
// `tools/assetc`, because encoding needs the basis ENCODER and the engine is
// deliberately linked against only the transcoder. What belongs here is the
// half a shipped game actually runs: what happens to bad bytes.

TEST_CASE("an empty texture blob is refused")
{
    seedRealCatalog();

    TextureAsset texture;
    const auto error = transcodeTexture({}, {}, texture);
    REQUIRE(error.has_value());
    CHECK(error->message.find("asset.texture.err.malformed") != std::string::npos);
    CHECK_FALSE(texture.valid());
}

TEST_CASE("a file that is not KTX2 is refused rather than parsed")
{
    seedRealCatalog();

    std::vector<std::byte> noise(4096);
    for (usize i = 0; i < noise.size(); ++i) {
        noise[i] = static_cast<std::byte>((i * 37u) & 0xFFu);
    }

    TextureAsset texture;
    const auto error = transcodeTexture(noise, {}, texture);
    REQUIRE(error.has_value());
    CHECK(error->message.find("asset.texture.err.malformed") != std::string::npos);
}

TEST_CASE("a truncated KTX2 header is an error at every length")
{
    seedRealCatalog();

    // A plausible-looking header, cut short at every point. The identifier is
    // real, so this exercises the parser rather than the first `memcmp`.
    std::vector<std::byte> header(160);
    std::memcpy(header.data(), Ktx2Identifier, sizeof(Ktx2Identifier));
    for (usize i = sizeof(Ktx2Identifier); i < header.size(); ++i) {
        header[i] = static_cast<std::byte>((i * 13u) & 0xFFu);
    }

    for (usize length = 0; length <= header.size(); ++length) {
        std::vector<std::byte> truncated(header.begin(), header.begin() + static_cast<std::ptrdiff_t>(length));
        TextureAsset texture;
        // The requirement is an answer and not a crash. A malformed file that
        // happened to parse into an empty texture would still be refused by
        // `valid()`; what must never happen is a read past the end.
        const auto error = transcodeTexture(truncated, {}, texture);
        CHECK((error.has_value() || !texture.valid()));
    }
}

TEST_CASE("format names and block sizes agree with each other")
{
    CHECK(std::string(textureFormatName(TextureFormat::Rgba8)) == "rgba8");
    CHECK(std::string(textureFormatName(TextureFormat::Bc7Rgba)) == "bc7");
    CHECK(std::string(textureFormatName(TextureFormat::Astc4x4Rgba)) == "astc4x4");

    CHECK_FALSE(isBlockCompressed(TextureFormat::Rgba8));
    CHECK_FALSE(isBlockCompressed(TextureFormat::Unknown));
    CHECK(isBlockCompressed(TextureFormat::Bc1Rgb));
    CHECK(isBlockCompressed(TextureFormat::Bc3Rgba));
    CHECK(isBlockCompressed(TextureFormat::Bc5Rg));
    CHECK(isBlockCompressed(TextureFormat::Bc7Rgba));
    CHECK(isBlockCompressed(TextureFormat::Astc4x4Rgba));

    // What the device samples is said once and is every caller's default
    // (ADR 0180); nothing has said it here.
    CHECK_FALSE(TranscodeOptions{}.allowAstc);
    setDeviceSamplesAstc(true);
    CHECK(TranscodeOptions{}.allowAstc);
    setDeviceSamplesAstc(false);
    CHECK_FALSE(deviceSamplesAstc());
}

// --- A decoded picture's smaller levels (D578) ----------------------------------

namespace {

[[nodiscard]] Image imageOf(u32 width, u32 height, auto&& texel)
{
    Image image;
    image.width = width;
    image.height = height;
    image.sourceChannels = 4;
    image.pixels.resize(static_cast<usize>(width) * height * 4u);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::array<unsigned, 4> rgba = texel(x, y);
            for (usize channel = 0; channel < 4; ++channel)
                image.pixels[(static_cast<usize>(y) * width + x) * 4u + channel] =
                    static_cast<std::byte>(rgba[channel]);
        }
    }
    return image;
}

[[nodiscard]] std::array<unsigned, 4> texelOf(const TextureAsset& texture, usize level, u32 x, u32 y)
{
    const TextureMip& mip = texture.mips[level];
    std::array<unsigned, 4> out{};
    for (usize channel = 0; channel < 4; ++channel)
        out[channel] =
            static_cast<unsigned>(texture.pixels[mip.offset + (static_cast<usize>(y) * mip.width + x) * 4u + channel]);
    return out;
}

} // namespace

TEST_CASE("mip chain: every level down to one texel, each half the one above")
{
    const Image image = imageOf(8, 8, [](u32 x, u32 y) {
        // A one-texel checkerboard: the finest thing a picture can hold.
        const unsigned value = ((x + y) & 1u) != 0 ? 255u : 0u;
        return std::array<unsigned, 4>{value, value, value, 255u};
    });
    // As data: the numbers' own mean.
    const TextureAsset texture = mipChainOf(image, false);
    REQUIRE(texture.valid());
    CHECK(texture.format == TextureFormat::Rgba8);
    CHECK(texture.width == 8);
    CHECK(texture.height == 8);
    REQUIRE(texture.mips.size() == 4);
    CHECK(texture.mips[1].width == 4);
    CHECK(texture.mips[2].width == 2);
    CHECK(texture.mips[3].width == 1);
    usize bytes = 0;
    for (const TextureMip& mip : texture.mips) {
        CHECK(mip.offset == bytes);
        CHECK(mip.size == static_cast<usize>(mip.width) * mip.height * 4u);
        bytes += mip.size;
    }
    CHECK(texture.pixels.size() == bytes);
    // The top level is the picture, byte for byte.
    CHECK(std::memcmp(texture.pixels.data(), image.pixels.data(), image.pixels.size()) == 0);
    // And a checkerboard a level down is its mean, everywhere: what drawing it
    // small should show, where four texels of sixty-four showed whichever four
    // the pixel's centre fell among.
    for (usize level = 1; level < texture.mips.size(); ++level) {
        for (u32 y = 0; y < texture.mips[level].height; ++y) {
            for (u32 x = 0; x < texture.mips[level].width; ++x) {
                const std::array<unsigned, 4> texel = texelOf(texture, level, x, y);
                CHECK(texel[0] >= 127u);
                CHECK(texel[0] <= 128u);
                CHECK(texel[3] == 255u);
            }
        }
    }

    // **As a colour, the mean of its light**: black and white in equal parts
    // are the grey half as bright as white, which is stored as 188 -- and is
    // what the compiler's own chain holds for the same picture.
    const TextureAsset colour = mipChainOf(image, true);
    REQUIRE(colour.mips.size() == 4);
    CHECK(colour.srgb);
    for (usize level = 1; level < colour.mips.size(); ++level) {
        const std::array<unsigned, 4> texel = texelOf(colour, level, 0, 0);
        CHECK(texel[0] >= 187u);
        CHECK(texel[0] <= 189u);
        CHECK(texel[3] == 255u);
    }
}

TEST_CASE("mip chain: a transparent texel's colour is not averaged into its neighbours'")
{
    // The left half opaque white, the right half transparent BLACK -- what a
    // paint program leaves under nothing. A plain mean made the level below
    // grey at half alpha: a dark fringe round every icon drawn small.
    const Image image = imageOf(2, 2, [](u32 x, u32) {
        return x == 0 ? std::array<unsigned, 4>{255u, 255u, 255u, 255u} : std::array<unsigned, 4>{0u, 0u, 0u, 0u};
    });
    const TextureAsset texture = mipChainOf(image, true);
    REQUIRE(texture.mips.size() == 2);
    const std::array<unsigned, 4> texel = texelOf(texture, 1, 0, 0);
    CHECK(texel[0] == 255u);
    CHECK(texel[1] == 255u);
    CHECK(texel[2] == 255u);
    CHECK(texel[3] >= 127u);
    CHECK(texel[3] <= 128u);
}

TEST_CASE("mip chain: a picture that is not a power of two, and one that is no picture")
{
    const Image image = imageOf(5, 3, [](u32, u32) { return std::array<unsigned, 4>{40u, 80u, 120u, 255u}; });
    const TextureAsset texture = mipChainOf(image, true);
    REQUIRE(texture.mips.size() == 3);
    CHECK(texture.mips[1].width == 2);
    CHECK(texture.mips[1].height == 1);
    CHECK(texture.mips[2].width == 1);
    CHECK(texture.mips[2].height == 1);
    CHECK(texelOf(texture, 2, 0, 0) == std::array<unsigned, 4>{40u, 80u, 120u, 255u});

    CHECK_FALSE(mipChainOf(Image{}, true).valid());
}
