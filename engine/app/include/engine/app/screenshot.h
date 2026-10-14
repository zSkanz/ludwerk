// Writing what the engine rendered to a file a tool can look at.
//
// This is the second half of "eyes before features".
// rhi::IDevice::readTexture gets the pixels off the GPU; this
// gets them onto disk in a format `tools/imgcmp` can compare against a
// reference. Everything from the first renderer onward gates on that comparison.
//
// The encoding and the file moved to `asset::writePng` with the first renderer, as this note used
// to promise they would. What is left here is the name the host calls, kept
// because `--screenshot` is the host's feature and its callers should not have
// to know which module owns a PNG encoder.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::app {

// Writes tightly packed 8-bit RGBA as PNG. `pixels` must hold exactly
// width * height * 4 bytes, top row first.
//
// Returns the error rather than logging it: a screenshot that silently failed
// to write is indistinguishable from one that matched, and the whole point of
// the file is to be evidence.
[[nodiscard]] std::optional<core::EngineError>
writePng(const std::filesystem::path& path, std::span<const std::byte> pixels, core::u32 width, core::u32 height);

} // namespace engine::app
