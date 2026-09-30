// Where a picture of the sun's shadow on terrain darkens ground that faces the
// sun (terrain audit T3).
//
// `--debug-view=shadow` draws the shadow map's answer in red, the contact
// mask's in green, and blue where the ground faces the sun by more than a
// grazing angle. On a scene where nothing stands between the sun and any face
// turned to it, every blue pixel should be lit by both: red or green missing
// from one is the ground shadowing itself -- acne, a bias too small for the
// slope.
#pragma once

#include <cstddef>

#include "engine/imgcmp/image.h"

namespace engine::imgcmp {

struct ShadowReport
{
    // Pixels facing the sun. The sky is black, so it is none of them.
    std::size_t facing = 0;
    // Of those, the ones the shadow map darkens by more than a twentieth.
    std::size_t mapDark = 0;
    // And the ones the contact mask does.
    std::size_t contactDark = 0;
};

[[nodiscard]] ShadowReport findSelfShadow(const Image& shot);

} // namespace engine::imgcmp
