// SMAA's two precomputed tables (ADR 0158): the area each edge shape covers,
// and where a search along an edge stops -- as its authors publish them with
// the shader, `third_party/smaa/Textures`, uploaded once as two textures.
#pragma once

#include <cstddef>
#include <span>

#include "engine/core/types.h"

namespace engine::render {

inline constexpr core::u32 kSmaaAreaWidth = 160;
inline constexpr core::u32 kSmaaAreaHeight = 560;
inline constexpr core::u32 kSmaaSearchWidth = 64;
inline constexpr core::u32 kSmaaSearchHeight = 16;

// Two bytes a texel, row by row.
[[nodiscard]] std::span<const std::byte> smaaAreaTable() noexcept;
// One byte a texel, row by row.
[[nodiscard]] std::span<const std::byte> smaaSearchTable() noexcept;

} // namespace engine::render
