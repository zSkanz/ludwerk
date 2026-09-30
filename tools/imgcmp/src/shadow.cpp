#include "engine/imgcmp/shadow.h"

#include <algorithm>

namespace engine::imgcmp {

ShadowReport findSelfShadow(const Image& shot)
{
    ShadowReport report;
    if (!shot.wellFormed())
        return report;
    // Blue is a whole 1 through the same tonemap, so it is what lit reads as
    // in this picture: a channel more than a twentieth under it is darkened.
    // Acne is rarely black -- a few of a filter's taps shadowed is a speckle of
    // eighty-five per cent, and it shows.
    constexpr int Facing = 64;
    const auto darkened = [](int channel, int lit) { return channel * 20 < lit * 19; };
    const std::size_t count = shot.pixelCount();
    for (std::size_t at = 0; at < count; ++at) {
        const std::uint8_t* pixel = &shot.rgba[at * 4u];
        if (pixel[2] < Facing)
            continue;
        ++report.facing;
        if (darkened(pixel[0], pixel[2]))
            ++report.mapDark;
        report.mapMass += 1.0 - std::min(1.0, static_cast<double>(pixel[0]) / static_cast<double>(pixel[2]));
        if (darkened(pixel[1], pixel[2]))
            ++report.contactDark;
    }
    return report;
}

} // namespace engine::imgcmp
