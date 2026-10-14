// Whether what a flat mirror shows is what is in front of it (ADR 0209).
//
// A lake reflects the screen: each pixel of water walks its mirror ray across
// the picture and shows what it lands on. Two ways that goes wrong look alike
// from a distance -- a reflection where there is nothing to reflect, and the
// wrong thing reflected -- and one picture cannot tell either from a right
// answer. Four can:
//
//   reflected   the scene, the water reflecting it;
//   plain       the same frame with the reflection off;
//   mirrored    the scene with NO water, through the camera mirrored in the
//               water's plane. Turned top to bottom, that is what a flat
//               mirror in that plane shows at every pixel;
//   sky         what that mirrored camera sees with nothing in the scene at
//               all: what the water reflects where there is nothing to.
//
// A pixel that `reflected` changed from `plain` is one the reflection wrote.
// With the reflection off the water showed the sky there; so what it wrote
// is, to a share and a tint, the mirror's picture LESS the sky's. Two things
// follow, and they are the check:
//
//   - where the mirror shows the sky, there was nothing to write. A pixel the
//     reflection changed there is **wrong**: a bank's sand laid across open
//     water, a ray that passed behind something and took its colour;
//   - where the mirror shows a thing, the change went the way from the sky to
//     that thing -- redder for a red post, darker for a dark wall. A change
//     the other way is the **wrong** thing reflected.
//
// The way, and not the amount: water takes a share of what it reflects and
// tints it, and neither is this check's business.
//
// A mirrored camera and a ray walked through depth agree to a pixel or two at
// an edge, so the mirror is read a little either side.
#pragma once

#include <cstddef>

#include "engine/imgcmp/image.h"

namespace engine::imgcmp {

struct MirrorReport
{
    // Pixels the reflection changed by more than the tolerance.
    std::size_t reflecting = 0;
    // Of those, the ones where the mirror shows the sky, or a thing the
    // change did not go towards.
    std::size_t wrong = 0;
    // The first of them, or -1.
    int firstX = -1;
    int firstY = -1;
};

// The four pictures are the same size, or the report is empty. `tolerance`
// is the sum over the three channels a pixel must differ by to count as
// changed -- the water from itself, and the mirror from its sky; `reach` how
// many pixels either side the mirror is read.
[[nodiscard]] MirrorReport checkMirror(const Image& reflected, const Image& plain, const Image& mirrored,
                                       const Image& sky, int tolerance, int reach);

} // namespace engine::imgcmp
