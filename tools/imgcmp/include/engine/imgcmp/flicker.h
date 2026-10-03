// How much a sequence of frames flickers (ADR 0158): what a temporal
// anti-aliasing pass is for, measured.
//
// A thin line or a fence under a slowly moving camera crawls: a pixel the line
// covers one frame is clear the next and covered again after, where a smooth
// picture would brighten or darken it steadily. So the measure is the second
// difference of each pixel's luma over time, |L(t+1) - 2 L(t) + L(t-1)|: zero
// for a pixel that changes at a steady rate, the edge moving across it, and
// large for one that pops. Averaged over every pixel and every inner frame,
// less a dead band: a level or two of change is the eighth bit rounding a
// picture that moved by a hair, which no eye sees, and a spatial pass on a
// still pixel changes it by nothing at all -- so only what is past `ignore`
// counts.
#pragma once

#include <span>
#include <string>

#include "engine/imgcmp/image.h"

namespace engine::imgcmp {

struct FlickerReport
{
    // The mean second difference, in luma steps of 0 to 255.
    double flicker = 0.0;
    // The frames measured; empty when there were too few or they disagree.
    std::size_t frames = 0;
    std::string error;
};

// At least three frames of one size.
[[nodiscard]] FlickerReport measureFlicker(std::span<const Image> frames, double ignore = 4.0);

} // namespace engine::imgcmp
