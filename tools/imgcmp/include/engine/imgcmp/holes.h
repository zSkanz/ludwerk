// Where a picture of the ground shows through it (terrain audit T0).
//
// The terrain gallery draws every terrain flat white over a magenta sky
// (`--debug-view=holes`), so any magenta inside the ground is a place the
// ground is not: a hole, an open seam between two levels of detail, a
// triangle missing. And against the same frame drawn at full detail, magenta
// where the full-detail picture had ground is ground a coarse level lost.
#pragma once

#include <cstddef>

#include "engine/imgcmp/image.h"

namespace engine::imgcmp {

// The sky's colour after the post chain: red and blue high, green low. What a
// shaded edge between ground and sky blends to still reads as sky -- a crack a
// pixel wide is a partly covered pixel, and it counts.
[[nodiscard]] bool skyColoured(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

struct HoleReport
{
    // Sky pixels no path of sky pixels joins to the picture's edge: seen
    // THROUGH the ground.
    std::size_t enclosed = 0;
    // Pixels that are ground in the reference, deep enough inside it that a
    // coarser silhouette cannot account for them, and sky here.
    std::size_t vanished = 0;
    // Ground pixels, for scale.
    std::size_t ground = 0;
};

// `reference` may be null: then `vanished` is zero. `slack` is how many pixels
// a silhouette may move between the two pictures before a lost pixel counts --
// a coarse level is a coarser shape, not a missing one.
[[nodiscard]] HoleReport findHoles(const Image& shot, const Image* reference, int slack);

} // namespace engine::imgcmp
