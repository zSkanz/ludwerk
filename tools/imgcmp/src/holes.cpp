#include "engine/imgcmp/holes.h"

#include <algorithm>
#include <vector>

namespace engine::imgcmp {

bool skyColoured(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    const int low = std::min<int>(r, b);
    return low >= 96 && static_cast<int>(g) * 2 < low;
}

namespace {

[[nodiscard]] std::vector<std::uint8_t> skyMask(const Image& image)
{
    std::vector<std::uint8_t> mask(image.pixelCount(), 0);
    for (std::size_t at = 0; at < mask.size(); ++at) {
        const std::uint8_t* pixel = &image.rgba[at * 4u];
        mask[at] = skyColoured(pixel[0], pixel[1], pixel[2]) ? 1 : 0;
    }
    return mask;
}

} // namespace

HoleReport findHoles(const Image& shot, const Image* reference, int slack)
{
    HoleReport report;
    if (!shot.wellFormed())
        return report;
    const int width = shot.width;
    const int height = shot.height;
    const std::vector<std::uint8_t> sky = skyMask(shot);

    // The sky that reaches the edge, by a flood from every edge pixel of sky.
    std::vector<std::uint8_t> open(sky.size(), 0);
    std::vector<int> stack;
    const auto seed = [&](int x, int y) {
        const auto at = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        if (sky[at] != 0 && open[at] == 0) {
            open[at] = 1;
            stack.push_back(static_cast<int>(at));
        }
    };
    for (int x = 0; x < width; ++x) {
        seed(x, 0);
        seed(x, height - 1);
    }
    for (int y = 0; y < height; ++y) {
        seed(0, y);
        seed(width - 1, y);
    }
    while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        const int x = at % width;
        const int y = at / width;
        if (x > 0)
            seed(x - 1, y);
        if (x + 1 < width)
            seed(x + 1, y);
        if (y > 0)
            seed(x, y - 1);
        if (y + 1 < height)
            seed(x, y + 1);
    }
    for (std::size_t at = 0; at < sky.size(); ++at) {
        if (sky[at] == 0)
            ++report.ground;
        else if (open[at] == 0)
            ++report.enclosed;
    }

    if (reference == nullptr || !reference->wellFormed() || reference->width != width || reference->height != height)
        return report;
    // Ground in the reference with no sky within `slack` pixels of it: a
    // square neighbourhood, by a running count along rows and then columns.
    const std::vector<std::uint8_t> refSky = skyMask(*reference);
    std::vector<int> rows(refSky.size(), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int count = 0;
            for (int k = std::max(0, x - slack); k <= std::min(width - 1, x + slack); ++k)
                count +=
                    refSky[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(k)];
            rows[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = count;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int count = 0;
            for (int k = std::max(0, y - slack); k <= std::min(height - 1, y + slack); ++k)
                count +=
                    rows[static_cast<std::size_t>(k) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            const auto at = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (count == 0 && sky[at] != 0)
                ++report.vanished;
        }
    }
    return report;
}

} // namespace engine::imgcmp
