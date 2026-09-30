#include "engine/imgcmp/steps.h"

#include <algorithm>
#include <cstdlib>

namespace engine::imgcmp {

StepReport findSteps(const Image& shot, int step)
{
    StepReport report;
    if (!shot.wellFormed())
        return report;
    // The sky is black; a bend never is -- the view draws none as a mid grey,
    // and four times a unit vector's change is not a half in every channel.
    constexpr int Lit = 16;
    const auto at = [&](int x, int y) {
        return &shot.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(shot.width) +
                           static_cast<std::size_t>(x)) *
                          4u];
    };
    const auto ground = [](const std::uint8_t* pixel) { return pixel[0] >= Lit || pixel[1] >= Lit || pixel[2] >= Lit; };
    const auto differs = [step](const std::uint8_t* a, const std::uint8_t* b) {
        for (int channel = 0; channel < 3; ++channel) {
            if (std::abs(static_cast<int>(a[channel]) - static_cast<int>(b[channel])) > step)
                return true;
        }
        return false;
    };
    int lowest[3] = {255, 255, 255};
    int highest[3] = {0, 0, 0};
    for (int y = 0; y < shot.height; ++y) {
        for (int x = 0; x < shot.width; ++x) {
            const std::uint8_t* pixel = at(x, y);
            if (!ground(pixel))
                continue;
            ++report.ground;
            for (int channel = 0; channel < 3; ++channel) {
                lowest[channel] = std::min(lowest[channel], static_cast<int>(pixel[channel]));
                highest[channel] = std::max(highest[channel], static_cast<int>(pixel[channel]));
            }
            const bool right = x + 1 < shot.width && ground(at(x + 1, y)) && differs(pixel, at(x + 1, y));
            const bool down = y + 1 < shot.height && ground(at(x, y + 1)) && differs(pixel, at(x, y + 1));
            if (right || down)
                ++report.steps;
        }
    }
    for (int channel = 0; channel < 3; ++channel)
        report.spread = std::max(report.spread, highest[channel] - lowest[channel]);
    return report;
}

} // namespace engine::imgcmp
