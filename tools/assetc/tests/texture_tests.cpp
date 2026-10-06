#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/asset/image.h"
#include "engine/asset/texture.h"
#include "engine/assetc/compiler.h"
#include "engine/core/i18n.h"

using engine::core::engineCatalog;
using engine::core::f32;
using engine::core::u32;
using engine::core::usize;

namespace {

void seedRealCatalog()
{
    const auto result = engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

// A picture with real structure -- gradients and a hard edge -- because a
// texture codec is trivially lossless on flat colour and this needs to be a
// test of the codec rather than of a constant.
[[nodiscard]] engine::asset::Image testImage(u32 size, bool withAlpha)
{
    engine::asset::Image image;
    image.width = size;
    image.height = size;
    image.sourceChannels = withAlpha ? 4u : 3u;
    image.pixels.resize(static_cast<usize>(size) * size * 4u);

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const usize at = (static_cast<usize>(y) * size + x) * 4u;
            const bool leftHalf = x < size / 2;
            image.pixels[at + 0] = static_cast<std::byte>(x * 255u / (size - 1u));
            image.pixels[at + 1] = static_cast<std::byte>(y * 255u / (size - 1u));
            image.pixels[at + 2] = static_cast<std::byte>(leftHalf ? 240u : 16u);
            image.pixels[at + 3] = static_cast<std::byte>(withAlpha ? (leftHalf ? 255u : 64u) : 255u);
        }
    }
    return image;
}

} // namespace

TEST_CASE("a texture survives the encode and comes back as pixels")
{
    seedRealCatalog();

    const engine::asset::Image source = testImage(64, false);
    std::vector<std::byte> ktx2;
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, true, ktx2).has_value());
    REQUIRE_FALSE(ktx2.empty());

    engine::asset::TranscodeOptions options;
    // Uncompressed, so the comparison below is against the encoder's loss and
    // not also against BC7's.
    options.forceUncompressed = true;

    engine::asset::TextureAsset decoded;
    REQUIRE_FALSE(engine::asset::transcodeTexture(ktx2, options, decoded).has_value());

    CHECK(decoded.width == 64);
    CHECK(decoded.height == 64);
    CHECK(decoded.format == engine::asset::TextureFormat::Rgba8);
    CHECK(decoded.srgb);
    REQUIRE(decoded.valid());
    // Mips down to 1x1: 64, 32, 16, 8, 4, 2, 1.
    CHECK(decoded.mips.size() == 7);
    CHECK(decoded.mips[0].width == 64);
    CHECK(decoded.mips.back().width == 1);

    // UASTC is lossy, so this is a similarity check rather than an equality
    // one -- but a LOOSE one would pass on a black image, which is the failure
    // this case exists to catch. Mean absolute error per channel, over the RGB
    // of the base level.
    const engine::asset::TextureMip& base = decoded.mips[0];
    REQUIRE(base.size == source.pixels.size());
    double totalError = 0.0;
    usize samples = 0;
    for (usize i = 0; i < base.size; i += 4) {
        for (usize channel = 0; channel < 3; ++channel) {
            const auto expected = static_cast<int>(static_cast<unsigned char>(source.pixels[i + channel]));
            const auto actual = static_cast<int>(static_cast<unsigned char>(decoded.pixels[base.offset + i + channel]));
            totalError += std::abs(expected - actual);
            samples += 1;
        }
    }
    const double meanError = totalError / static_cast<double>(samples);
    CHECK(meanError < 4.0);

    // And the picture is still the picture: the hard edge in the blue channel
    // is still where it was put. A codec that returned a flat average would
    // pass the mean-error check on this image and fail here.
    const usize leftAt = base.offset + (static_cast<usize>(32) * 64 + 8) * 4 + 2;
    const usize rightAt = base.offset + (static_cast<usize>(32) * 64 + 56) * 4 + 2;
    CHECK(static_cast<int>(static_cast<unsigned char>(decoded.pixels[leftAt])) > 180);
    CHECK(static_cast<int>(static_cast<unsigned char>(decoded.pixels[rightAt])) < 70);
}

TEST_CASE("alpha survives the round trip and is reported")
{
    seedRealCatalog();

    const engine::asset::Image source = testImage(32, true);
    std::vector<std::byte> ktx2;
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, true, ktx2).has_value());

    engine::asset::TranscodeOptions options;
    options.forceUncompressed = true;
    engine::asset::TextureAsset decoded;
    REQUIRE_FALSE(engine::asset::transcodeTexture(ktx2, options, decoded).has_value());

    CHECK(decoded.hasAlpha);
    const engine::asset::TextureMip& base = decoded.mips[0];
    const usize opaqueAt = base.offset + (static_cast<usize>(16) * 32 + 4) * 4 + 3;
    const usize fadedAt = base.offset + (static_cast<usize>(16) * 32 + 28) * 4 + 3;
    CHECK(static_cast<int>(static_cast<unsigned char>(decoded.pixels[opaqueAt])) > 200);
    CHECK(static_cast<int>(static_cast<unsigned char>(decoded.pixels[fadedAt])) < 120);
}

TEST_CASE("one asset transcodes to whatever the device can sample")
{
    seedRealCatalog();

    const engine::asset::Image source = testImage(32, true);
    std::vector<std::byte> ktx2;
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, true, ktx2).has_value());

    // The whole point of the container: the pack carries UASTC once and the
    // device decides what it becomes.
    struct Case
    {
        engine::asset::TranscodeOptions options;
        engine::asset::TextureFormat expected;
    };
    const Case cases[] = {
        {{}, engine::asset::TextureFormat::Bc7Rgba},
        {{false, true, true, false, false}, engine::asset::TextureFormat::Bc3Rgba},
        {{false, false, false, false, false}, engine::asset::TextureFormat::Rgba8},
        {{true, true, true, true, false}, engine::asset::TextureFormat::Rgba8},
        // A phone: no BC format, and ASTC (ADR 0180) -- the same sixteen
        // bytes a block BC7 is.
        {{false, false, false, false, false, true}, engine::asset::TextureFormat::Astc4x4Rgba},
        // And BC7 first where a device has both.
        {{true, true, true, false, false, true}, engine::asset::TextureFormat::Bc7Rgba},
    };

    for (const Case& entry : cases) {
        engine::asset::TextureAsset decoded;
        REQUIRE_FALSE(engine::asset::transcodeTexture(ktx2, entry.options, decoded).has_value());
        CHECK(decoded.format == entry.expected);
        CHECK(decoded.width == 32);
        // Block formats pack sixteen texels into eight or sixteen bytes, so the
        // base level is smaller than the pixels it represents -- which is the
        // whole reason to want one.
        if (engine::asset::isBlockCompressed(decoded.format)) {
            CHECK(decoded.mips[0].size < static_cast<usize>(32) * 32 * 4);
        }
        if (decoded.format == engine::asset::TextureFormat::Astc4x4Rgba) {
            // Eight blocks by eight of sixteen bytes, and every level there.
            CHECK(decoded.mips[0].size == static_cast<usize>(8) * 8 * 16);
            CHECK(decoded.mips.size() > 1);
            // Not sixteen bytes of nothing a block: the transcoder wrote them.
            bool written = false;
            for (usize at = 0; at < decoded.mips[0].size; ++at)
                written = written || decoded.pixels[decoded.mips[0].offset + at] != std::byte{0};
            CHECK(written);
        }
    }
}

TEST_CASE("baseLevelOnly stops at the level the UI draws")
{
    seedRealCatalog();

    const engine::asset::Image source = testImage(32, false);
    std::vector<std::byte> ktx2;
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, true, ktx2).has_value());

    engine::asset::TranscodeOptions options;
    options.baseLevelOnly = true;
    engine::asset::TextureAsset decoded;
    REQUIRE_FALSE(engine::asset::transcodeTexture(ktx2, options, decoded).has_value());

    // A `ScreenGui` image is drawn at one size; mips would be memory nobody
    // samples.
    CHECK(decoded.mips.size() == 1);
    CHECK(decoded.mips[0].width == 32);
}

TEST_CASE("encoding the same image twice produces the same bytes")
{
    seedRealCatalog();

    const engine::asset::Image source = testImage(64, true);
    std::vector<std::byte> first;
    std::vector<std::byte> second;
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, true, first).has_value());
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, true, second).has_value());

    // The property the content hash rests on. The encoder is deliberately
    // single-threaded and built without SSE for exactly this.
    CHECK(first == second);
}

TEST_CASE("the transfer function reaches the bytes")
{
    seedRealCatalog();

    // **The whole point of `srgb` being a parameter.** The same pixels encoded
    // as colour and as data are different bytes, because the encoder bends the
    // values through the transfer curve before compressing them. If these came
    // back equal, the flag would be reaching nothing -- which is exactly the
    // state this compiler was in, with every normal and ORM map marked sRGB.
    const engine::asset::Image source = testImage(64, true);
    std::vector<std::byte> colour;
    std::vector<std::byte> data;
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, true, colour).has_value());
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, false, data).has_value());

    CHECK_FALSE(colour.empty());
    CHECK_FALSE(data.empty());
    CHECK(colour != data);

    // And each is still deterministic on its own, which is the property the
    // content hash rests on.
    std::vector<std::byte> again;
    REQUIRE_FALSE(engine::assetc::encodeTexture(source, false, again).has_value());
    CHECK(again == data);
}

TEST_CASE("an image with no pixels is refused rather than encoded")
{
    seedRealCatalog();

    engine::asset::Image empty;
    std::vector<std::byte> out;
    const auto error = engine::assetc::encodeTexture(empty, true, out);
    REQUIRE(error.has_value());
    CHECK(error->message.find("asset.texture.err.encode_failed") != std::string::npos);
    CHECK(out.empty());
}
