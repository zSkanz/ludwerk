// How strongly a picture of ground repeats at a terrain layer's tile period,
// and how much its colour varies from one tile to the next (ADR 0113's
// amendment).
//
// A layer is one small patch of texture laid every `tileSize` metres. Seen
// from above, that is a picture equal to itself shifted by the period: its
// autocorrelation at that shift is one, less what filtering takes. Broken up,
// the same shift finds a different picture. And far off, where the patch's
// detail is below a pixel, every tile averages to the same colour -- a flat
// blur -- unless something varies over tens of metres.
//
// Both are measured on luminance. The autocorrelation is taken after taking
// away the mean over a period-wide box round each pixel, so a slow drift
// across the picture -- which correlates with itself at any shift -- does not
// count as the repeat.
#pragma once

#include "engine/imgcmp/image.h"

namespace engine::imgcmp {

struct TileReport
{
    // The autocorrelation of the picture's detail at the period, across and
    // down, each the largest over shifts within 5% of it (a period is rarely
    // a whole number of pixels): one repeats exactly, zero not at all.
    double across = 0.0;
    double down = 0.0;
    // The same in any direction: the largest over every shift whose length
    // is within 5% of the period, on the picture shrunk until the period is
    // about sixteen pixels. A lattice turned from the picture's axes -- a
    // second sample of a layer, turned so its repeat does not line up with
    // the first's -- repeats along its own axes, not the picture's.
    double ring = 0.0;
    // How far the mean luminance of period-sized blocks spreads, as a share
    // of the mean: zero is every tile the same colour.
    double blocks = 0.0;
};

// `period` in pixels, at least 4 and under a third of the picture's smaller
// side; an ill-formed picture or period reports zeros.
[[nodiscard]] TileReport measureTiling(const Image& shot, double period);

} // namespace engine::imgcmp
