#include "engine/asset/texture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <basisu_transcoder.h>
#include <cmath>
#include <cstring>
#include <mutex>

#include "engine/asset/image.h"
#include "engine/core/i18n.h"
#include "engine/core/text_key.h"

namespace engine::asset {
namespace {

using core::I18nArg;

// Upstream builds several codebook tables on first use and documents the call
// as one-per-process. `call_once` rather than a bare bool because a transcode
// may be issued from a job (`Domain::AssetIo`) and two workers arriving at once
// would race those tables.
void ensureTranscoderReady()
{
    static std::once_flag once;
    std::call_once(once, [] { basist::basisu_transcoder_init(); });
}

[[nodiscard]] u32 blocksAcross(u32 pixels) noexcept
{
    return (pixels + 3u) / 4u;
}

// The bytes one mip level occupies once transcoded. Written out rather than
// asked of the transcoder, because it is also what sizes the buffer handed IN.
[[nodiscard]] usize levelBytes(TextureFormat format, u32 width, u32 height) noexcept
{
    if (!isBlockCompressed(format)) {
        return static_cast<usize>(width) * height * 4u;
    }
    const usize blocks = static_cast<usize>(blocksAcross(width)) * blocksAcross(height);
    // BC1 is eight bytes a block; BC3, BC5, BC7 and ASTC's four by four are
    // sixteen.
    return format == TextureFormat::Bc1Rgb ? blocks * 8u : blocks * 16u;
}

[[nodiscard]] basist::transcoder_texture_format toBasis(TextureFormat format) noexcept
{
    switch (format) {
    case TextureFormat::Bc1Rgb:
        return basist::transcoder_texture_format::cTFBC1_RGB;
    case TextureFormat::Bc3Rgba:
        return basist::transcoder_texture_format::cTFBC3_RGBA;
    case TextureFormat::Bc5Rg:
        return basist::transcoder_texture_format::cTFBC5_RG;
    case TextureFormat::Bc7Rgba:
        return basist::transcoder_texture_format::cTFBC7_RGBA;
    case TextureFormat::Astc4x4Rgba:
        return basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
    case TextureFormat::Rgba8:
    case TextureFormat::Unknown:
        break;
    }
    return basist::transcoder_texture_format::cTFRGBA32;
}

// The best format the caller can sample, given what the texture holds.
//
// BC7 first because it is the only block format that is good at both colour
// and alpha; BC1 for opaque when BC7 is unavailable; BC3 for alpha when BC7 is
// unavailable. RGBA when nothing else is allowed -- which is the headless and
// capture path, and is also what makes a golden comparable across machines.
[[nodiscard]] TextureFormat chooseFormat(const TranscodeOptions& options, bool hasAlpha) noexcept
{
    if (options.forceUncompressed) {
        return TextureFormat::Rgba8;
    }
    if (options.allowBc7) {
        return TextureFormat::Bc7Rgba;
    }
    // A phone's BC7: one format for colour and alpha both, at the same eight
    // bits a pixel.
    if (options.allowAstc) {
        return TextureFormat::Astc4x4Rgba;
    }
    if (hasAlpha) {
        return options.allowBc3 ? TextureFormat::Bc3Rgba : TextureFormat::Rgba8;
    }
    return options.allowBc1 ? TextureFormat::Bc1Rgb : TextureFormat::Rgba8;
}

} // namespace

const char* textureFormatName(TextureFormat format) noexcept
{
    switch (format) {
    case TextureFormat::Rgba8:
        return "rgba8";
    case TextureFormat::Bc1Rgb:
        return "bc1";
    case TextureFormat::Bc3Rgba:
        return "bc3";
    case TextureFormat::Bc5Rg:
        return "bc5";
    case TextureFormat::Bc7Rgba:
        return "bc7";
    case TextureFormat::Astc4x4Rgba:
        return "astc4x4";
    case TextureFormat::Unknown:
        break;
    }
    return "unknown";
}

bool isBlockCompressed(TextureFormat format) noexcept
{
    return format == TextureFormat::Bc1Rgb || format == TextureFormat::Bc3Rgba || format == TextureFormat::Bc5Rg ||
           format == TextureFormat::Bc7Rgba || format == TextureFormat::Astc4x4Rgba;
}

namespace {
std::atomic<bool> g_deviceSamplesAstc{false};
} // namespace

void setDeviceSamplesAstc(bool samples) noexcept
{
    g_deviceSamplesAstc.store(samples, std::memory_order_relaxed);
}

bool deviceSamplesAstc() noexcept
{
    return g_deviceSamplesAstc.load(std::memory_order_relaxed);
}

std::optional<core::EngineError> transcodeTexture(std::span<const std::byte> ktx2, const TranscodeOptions& options,
                                                  TextureAsset& out)
{
    out = TextureAsset{};
    ensureTranscoderReady();

    if (ktx2.empty()) {
        return core::makeError(ENG_TR("asset.texture.err.malformed"));
    }

    basist::ktx2_transcoder transcoder;
    if (!transcoder.init(ktx2.data(), static_cast<u32>(ktx2.size()))) {
        return core::makeError(ENG_TR("asset.texture.err.malformed"));
    }
    if (transcoder.is_hdr()) {
        // The engine has no HDR texture path. Refused by name rather than
        // transcoded into something that looks washed out.
        return core::makeError(ENG_TR("asset.texture.err.hdr_unsupported"));
    }
    if (!transcoder.start_transcoding()) {
        return core::makeError(ENG_TR("asset.texture.err.transcode_failed"));
    }

    out.width = transcoder.get_width();
    out.height = transcoder.get_height();
    out.hasAlpha = transcoder.get_has_alpha() != 0;
    out.srgb = transcoder.is_srgb();
    out.format = chooseFormat(options, out.hasAlpha);

    if (out.width == 0 || out.height == 0) {
        return core::makeError(ENG_TR("asset.texture.err.malformed"));
    }

    const u32 declaredLevels = transcoder.get_levels();
    const u32 levels = options.baseLevelOnly ? 1u : (declaredLevels > 0 ? declaredLevels : 1u);
    const basist::transcoder_texture_format target = toBasis(out.format);
    const bool blocks = isBlockCompressed(out.format);

    usize total = 0;
    out.mips.reserve(levels);
    for (u32 level = 0; level < levels; ++level) {
        basist::ktx2_image_level_info info{};
        if (!transcoder.get_image_level_info(info, level, 0, 0)) {
            return core::makeError(ENG_TR("asset.texture.err.malformed"));
        }
        TextureMip mip;
        mip.width = info.m_orig_width;
        mip.height = info.m_orig_height;
        mip.offset = total;
        mip.size = levelBytes(out.format, mip.width, mip.height);
        total += mip.size;
        out.mips.push_back(mip);
    }

    out.pixels.resize(total);
    for (u32 level = 0; level < levels; ++level) {
        const TextureMip& mip = out.mips[level];
        // The unit of `output_blocks_buf_size_in_blocks_or_pixels` is what its
        // name says and it differs by format, which is the one thing easy to
        // get wrong here: blocks for a block format, PIXELS for RGBA.
        const u32 capacity = blocks ? blocksAcross(mip.width) * blocksAcross(mip.height) : mip.width * mip.height;
        if (!transcoder.transcode_image_level(level, 0, 0, out.pixels.data() + mip.offset, capacity, target)) {
            const I18nArg args[] = {{"format", textureFormatName(out.format)}};
            return core::makeError(ENG_TR("asset.texture.err.transcode_failed"), args);
        }
    }

    return std::nullopt;
}

TextureAsset mipChainOf(const Image& image, bool srgb)
{
    TextureAsset out;
    if (!image.valid())
        return out;
    out.width = image.width;
    out.height = image.height;
    out.format = TextureFormat::Rgba8;
    out.srgb = srgb;
    out.hasAlpha = image.sourceChannels == 2 || image.sourceChannels == 4;

    usize total = 0;
    for (u32 width = image.width, height = image.height;;) {
        const usize size = static_cast<usize>(width) * height * 4u;
        out.mips.push_back(TextureMip{width, height, total, size});
        total += size;
        if (width == 1 && height == 1)
            break;
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
    }
    out.pixels.resize(total);
    std::memcpy(out.pixels.data(), image.pixels.data(), image.pixels.size());

    // A stored value as the light it stands for, and back.
    std::array<float, 256> light{};
    for (usize value = 0; value < light.size(); ++value) {
        const float kept = static_cast<float>(value) / 255.0f;
        if (!srgb)
            light[value] = kept;
        else if (kept <= 0.04045f)
            light[value] = kept / 12.92f;
        else
            light[value] = std::pow((kept + 0.055f) / 1.055f, 2.4f);
    }
    const auto stored = [srgb](float value) {
        float encoded = value;
        if (srgb)
            encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
        return static_cast<unsigned char>(std::clamp(encoded * 255.0f + 0.5f, 0.0f, 255.0f));
    };

    for (usize level = 1; level < out.mips.size(); ++level) {
        const TextureMip& above = out.mips[level - 1];
        const TextureMip& here = out.mips[level];
        const auto* from = reinterpret_cast<const unsigned char*>(out.pixels.data() + above.offset);
        auto* to = reinterpret_cast<unsigned char*>(out.pixels.data() + here.offset);
        for (u32 y = 0; y < here.height; ++y) {
            // The two rows and two columns above; the same one twice where the
            // level above is a single texel across.
            const u32 rows[2]{std::min(y * 2u, above.height - 1u), std::min(y * 2u + 1u, above.height - 1u)};
            for (u32 x = 0; x < here.width; ++x) {
                const u32 columns[2]{std::min(x * 2u, above.width - 1u), std::min(x * 2u + 1u, above.width - 1u)};
                float weighted[3]{};
                float plain[3]{};
                u32 alpha = 0;
                for (const u32 row : rows) {
                    for (const u32 column : columns) {
                        const unsigned char* texel = from + (static_cast<usize>(row) * above.width + column) * 4u;
                        for (usize channel = 0; channel < 3; ++channel) {
                            weighted[channel] += light[texel[channel]] * static_cast<float>(texel[3]);
                            plain[channel] += light[texel[channel]];
                        }
                        alpha += texel[3];
                    }
                }
                unsigned char* made = to + (static_cast<usize>(y) * here.width + x) * 4u;
                for (usize channel = 0; channel < 3; ++channel) {
                    // Four texels of nothing keep their plain mean: a colour
                    // to blend towards, where there is none to weigh.
                    made[channel] =
                        stored(alpha > 0 ? weighted[channel] / static_cast<float>(alpha) : plain[channel] * 0.25f);
                }
                made[3] = static_cast<unsigned char>((alpha + 2u) / 4u);
            }
        }
    }
    return out;
}

} // namespace engine::asset
