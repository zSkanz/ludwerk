// One square PNG in, every icon a platform wants out (ADR 0104 §2).
//
// **Nobody opens an icon editor.** A project names one picture, 1024 pixels
// recommended, and the export makes the sizes each target needs:
//
// - Windows: a multi-size `.ico` (16 to 256), PNG-compressed entries;
// - Linux: a 512-pixel PNG beside the binary, named in its `.desktop` file;
// - Android: the legacy launcher icons from mdpi to xxxhdpi, and an adaptive
//   icon -- the picture as its foreground, inside the safe zone, on a
//   background colour.
//
// The downscale is the engine's own box filter over premultiplied colour: a
// picture made smaller keeps its edges clean against a transparent corner,
// and it takes no dependency.
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/asset/image.h"
#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::asset {

struct IconSetOptions
{
    bool windows = true;
    bool linuxDesktop = true;
    bool android = true;
    // The adaptive icon's background, 0xRRGGBB.
    core::u32 background = 0xFFFFFF;
    // An adaptive foreground drawn for it, full-bleed at 108 by 108 dp with the
    // safe zone in mind. Absent, the icon itself is shrunk into the safe zone.
    std::optional<Image> foreground;
};

struct IconSetReport
{
    // Relative to the output directory, in the order they were written.
    std::vector<std::string> written;
    // Said, not refused: a picture under 256 pixels is upscaled for the large
    // sizes and looks it.
    std::vector<std::string> warnings;
};

// The picture at `size` by `size`, box-filtered down (or nearest up).
[[nodiscard]] Image resizeImage(const Image& source, core::u32 size);

// An `.ico` holding each image as a PNG entry, in the order given.
[[nodiscard]] std::optional<core::EngineError> encodeIco(const std::vector<Image>& images, std::vector<std::byte>& out);

// Writes the icons `options` asks for under `directory`:
// `windows/icon.ico`, `linux/icon.png`, and `android/res/...` as an Android
// resource tree. A picture that is not square is refused with a keyed error
// naming both sides.
[[nodiscard]] std::optional<core::EngineError> writeIconSet(const Image& source, const std::filesystem::path& directory,
                                                            const IconSetOptions& options, IconSetReport& report);

} // namespace engine::asset
