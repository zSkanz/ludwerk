// GPU-ready textures out of the KTX2 blobs the pipeline produces (ADR 0010).
//
// The engine READS textures and never writes them. basis_universal ships an
// encoder and a transcoder from one tree; only the transcoder is linked here,
// and the encoder only into the offline `assetc` tool. That split is a rule
// rather than a convenience -- an encoder in a shipped game is megabytes of
// attack surface nothing calls.
//
// **One asset transcodes to whatever the device wants.** That is the whole
// point of the format: the pack carries UASTC or ETC1S once, and this function
// turns it into BC7, BC1 or plain RGBA depending on what the caller says the
// GPU supports. No `rhi` type appears here -- `render` maps `TextureFormat`
// onto its own enum, exactly as it does for meshes.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::asset {

using core::u32;
using core::usize;

// What a transcoded texture is, in terms `render` can map onto `rhi`. A subset
// of the RHI's own list on purpose: these are the four a transcode can produce
// and the one it falls back to.
enum class TextureFormat : core::u8
{
    Unknown = 0,
    // Uncompressed, four channels, eight bits each. The fallback, and what a
    // capture or a headless run gets.
    Rgba8,
    Bc1Rgb,
    Bc3Rgba,
    Bc5Rg,
    Bc7Rgba,
    // ASTC in blocks of four by four, eight bits a pixel: what a phone's GPU
    // samples where a desktop's samples BC7 (ADR 0180). Colour or data, with
    // or without alpha, as BC7 is.
    Astc4x4Rgba,
};

[[nodiscard]] const char* textureFormatName(TextureFormat format) noexcept;

// True when the format stores 4x4 blocks rather than pixels, which is what
// decides how a row pitch is computed and how an upload is sized.
[[nodiscard]] bool isBlockCompressed(TextureFormat format) noexcept;

struct TextureMip
{
    u32 width = 0;
    u32 height = 0;
    // Into `TextureAsset::pixels`.
    usize offset = 0;
    usize size = 0;
};

struct TextureAsset
{
    u32 width = 0;
    u32 height = 0;
    TextureFormat format = TextureFormat::Unknown;
    // Whether the stored values are sRGB-encoded. Read from the KTX2 data
    // format descriptor rather than guessed from how the texture is used.
    bool srgb = false;
    bool hasAlpha = false;

    // Every mip level, tightly packed, largest first.
    std::vector<TextureMip> mips;
    std::vector<std::byte> pixels;
    // How many larger levels the file had that were left out
    // (`TranscodeOptions::skipLevels`); nought for a texture taken whole.
    u32 skippedLevels = 0;

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0 && !mips.empty(); }
};

// What the target device can sample. Named after capabilities rather than
// after a platform, because that is what `rhi::Capabilities` answers.
// **Whether this platform's GPUs sample the BC formats.** Desktop GPUs do;
// a phone's (Adreno, Mali) do not, and a BC texture there is a texture that
// fails to create. A phone takes ASTC instead (ADR 0180, `allowAstc`), and
// uncompressed RGBA only where its GPU says it samples neither.
#if defined(__ANDROID__)
inline constexpr bool BlockCompressedByDefault = false;
#else
inline constexpr bool BlockCompressedByDefault = true;
#endif

// **Whether the device this process draws on samples ASTC** (ADR 0180): said
// once, when the device is made, by whoever made it; false until then and on
// every machine whose GPU samples the BC formats, which come first. A fact
// about the process rather than a thing each caller passes down, because
// every texture of every kind -- a mesh's, the interface's, the sky's -- goes
// to the one device.
void setDeviceSamplesAstc(bool samples) noexcept;
[[nodiscard]] bool deviceSamplesAstc() noexcept;

struct TranscodeOptions
{
    bool allowBc7 = BlockCompressedByDefault;
    bool allowBc1 = BlockCompressedByDefault;
    bool allowBc3 = BlockCompressedByDefault;
    // Ignores every block format and produces `Rgba8`. What a headless run and
    // the capture backend want, and what makes a golden comparable.
    bool forceUncompressed = false;
    // Transcode only the largest level: for what is sampled at its own size
    // and no other -- a sky's faces, read by the CPU. Not the interface's
    // pictures, which it was written for: an icon is drawn at a fifteenth of
    // its size as often as at its own (D578).
    bool baseLevelOnly = false;
    // ASTC, taken where BC7 is not allowed: a phone (ADR 0180). The device's
    // word unless the caller says. Last, so the options written by position
    // before it existed mean what they meant.
    bool allowAstc = deviceSamplesAstc();
    // **How many of the largest levels to leave out** (`GraphicsService.
    // TextureQuality`, D609): one is half the size and a quarter of the
    // memory, two a quarter and a sixteenth. The texture that comes out IS
    // the smaller one -- its width, its height, its first level -- so
    // nothing after this knows a larger one existed. Never past the last
    // level, and never to a texture smaller than `SkipFloor` on its longer
    // side: an icon of 64 is not what fills a phone's memory.
    u32 skipLevels = 0;
    static constexpr u32 SkipFloor = 64;
};

// Reads a KTX2 blob and transcodes it. Bad input is an error rather than a
// crash: this reads bytes that came out of a pack a person may have truncated.
[[nodiscard]] std::optional<core::EngineError> transcodeTexture(std::span<const std::byte> ktx2,
                                                                const TranscodeOptions& options, TextureAsset& out);

struct Image;

// **A decoded picture with its smaller levels**, as `Rgba8`: the picture
// itself, then each level half the one above down to one texel. What a
// picture the compiler has not seen -- a project run out of its source tree --
// is uploaded as wherever it may be drawn smaller than it is (D578); a
// compiled one carries its chain already.
//
// Each level is the mean of the four texels above it, **weighted by their
// alpha**: the colour under a transparent texel is whatever a paint program
// left there, and averaged in plainly it is a dark fringe round everything
// drawn small. **And of their light, for a colour** (`srgb`): the mean of a
// black texel and a white one is the grey that is half as bright, 188 and not
// 128 -- which is also what the compiler's chain holds, so a picture looks
// the same small whether the compiler has seen it or not. Data -- `srgb`
// false -- is averaged as the numbers it is. An invalid image gives an
// invalid texture.
[[nodiscard]] TextureAsset mipChainOf(const Image& image, bool srgb);

} // namespace engine::asset
