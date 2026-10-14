#include "engine/imgcmp/mirror.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace engine::imgcmp {

MirrorReport checkMirror(const Image& reflected, const Image& plain, const Image& mirrored, const Image& sky,
                         int tolerance, int reach)
{
    MirrorReport report;
    if (!reflected.wellFormed() || !plain.wellFormed() || !mirrored.wellFormed() || !sky.wellFormed())
        return report;
    for (const Image* other : {&plain, &mirrored, &sky}) {
        if (other->width != reflected.width || other->height != reflected.height)
            return report;
    }

    const int width = reflected.width;
    const int height = reflected.height;
    const auto at = [width](const Image& image, int x, int y) {
        return &image.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x)) *
                           4u];
    };
    const auto apart = [](const std::uint8_t* a, const std::uint8_t* b) {
        int sum = 0;
        for (int channel = 0; channel < 3; ++channel)
            sum += std::abs(static_cast<int>(a[channel]) - static_cast<int>(b[channel]));
        return sum;
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* now = at(reflected, x, y);
            const std::uint8_t* before = at(plain, x, y);
            if (apart(now, before) <= tolerance)
                continue;
            ++report.reflecting;
            // The mirror's picture, turned top to bottom, a little either side.
            bool towards = false;
            for (int dy = -reach; dy <= reach && !towards; ++dy) {
                for (int dx = -reach; dx <= reach && !towards; ++dx) {
                    const int mx = x + dx;
                    const int my = height - 1 - (y + dy);
                    if (mx < 0 || mx >= width || my < 0 || my >= height)
                        continue;
                    const std::uint8_t* shown = at(mirrored, mx, my);
                    const std::uint8_t* empty = at(sky, mx, my);
                    // The sky there: nothing to reflect.
                    if (apart(shown, empty) <= tolerance)
                        continue;
                    // A thing there: the water went the way from the sky to it.
                    long long along = 0;
                    for (int channel = 0; channel < 3; ++channel) {
                        along += (static_cast<long long>(now[channel]) - before[channel]) *
                                 (static_cast<long long>(shown[channel]) - empty[channel]);
                    }
                    towards = along > 0;
                }
            }
            if (!towards) {
                if (report.wrong == 0) {
                    report.firstX = x;
                    report.firstY = y;
                }
                ++report.wrong;
            }
        }
    }
    return report;
}

} // namespace engine::imgcmp
