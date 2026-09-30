// Where a picture of how the terrain's shading bends its normal steps between
// two pixels (terrain audit T3).
//
// `--debug-view=bend` draws how far the normal the ground is shaded with is
// bent from its mesh's, four times over, the sky black. The grain's nudge and
// a layer's normal map bend it smoothly, a shade or two from one pixel to the
// next; where it jumps, the shading drew a crease the ground does not have --
// the bright streaks and dark notches a low sun or a lamp showed across flat
// ground, which were the grain's noise stepping at a cell's edge.
#pragma once

#include <cstddef>

#include "engine/imgcmp/image.h"

namespace engine::imgcmp {

struct StepReport
{
    // Pixels of ground: any channel lit -- an unbent normal is a mid grey.
    std::size_t ground = 0;
    // Of those, the ones whose right or lower neighbour, ground too, differs
    // from it by more than the step in some channel.
    std::size_t steps = 0;
};

[[nodiscard]] StepReport findSteps(const Image& shot, int step);

} // namespace engine::imgcmp
