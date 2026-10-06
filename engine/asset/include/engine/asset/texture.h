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
    // Transcode only the largest level. The UI wants this: a `ScreenGui` image
    // is drawn at one size and mips would be memory nobody samples.
    bool baseLevelOnly = false;
    // ASTC, taken where BC7 is not allowed: a phone (ADR 0180). The device's
    // word unless the caller says. Last, so the options written by position
    // before it existed mean what they meant.
    bool allowAstc = deviceSamplesAstc();
};

// Reads a KTX2 blob and transcodes it. Bad input is an error rather than a
// crash: this reads bytes that came out of a pack a person may have truncated.
[[nodiscard]] std::optional<core::EngineError> transcodeTexture(std::span<const std::byte> ktx2,
                                                                const TranscodeOptions& options, TextureAsset& out);

} // namespace engine::asset
