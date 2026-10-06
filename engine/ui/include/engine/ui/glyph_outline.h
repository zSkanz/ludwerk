// A glyph's outline, made from its coverage (ADR 0110).
#pragma once

#include <vector>

#include "engine/core/types.h"

namespace engine::ui {

// **A text stroke's shape**: every texel takes the largest coverage within
// `radius` of it, through a kernel shaped like the join -- 0, a disc, rounds
// the corners; 1, an octagon, cuts them; 2, a square, keeps them -- and the
// kernel's own edge is soft over one texel, so a stroke of 2.3 pixels is 2.3
// and not 2 or 3. `out` is the glyph grown by `pad` on each side: `width + pad
// * 2` by `height + pad * 2`, row-major.
//
// A function of its arguments and nothing else: the same bytes on every
// machine, and safe on any thread.
void dilateCoverage(const std::vector<core::u8>& coverage, core::u32 width, core::u32 height, core::f32 radius,
                    core::u32 join, core::u32 pad, std::vector<core::u8>& out);

} // namespace engine::ui
