#include "engine/imgcmp/flicker.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::imgcmp {

namespace {

// Rec. 709 luma of one pixel, 0 to 255.
[[nodiscard]] double lumaAt(const Image& image, std::size_t pixel) noexcept
{
    const std::uint8_t* rgba = image.rgba.data() + pixel * 4;
    return 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2];
}

} // namespace

FlickerReport measureFlicker(std::span<const Image> frames, double ignore)
{
    FlickerReport report;
    if (frames.size() < 3) {
        report.error = "at least three frames are needed";
        return report;
    }
    for (const Image& frame : frames) {
        if (frame.empty() || !frame.wellFormed() || frame.width != frames[0].width ||
            frame.height != frames[0].height) {
            report.error = "the frames are not all one size";
            return report;
        }
    }
    const std::size_t pixels = frames[0].pixelCount();
    double sum = 0.0;
    for (std::size_t t = 1; t + 1 < frames.size(); ++t) {
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            const double before = lumaAt(frames[t - 1], pixel);
            const double now = lumaAt(frames[t], pixel);
            const double after = lumaAt(frames[t + 1], pixel);
            sum += std::max(0.0, std::abs(after - 2.0 * now + before) - ignore);
        }
    }
    report.frames = frames.size();
    report.flicker = sum / (static_cast<double>(pixels) * static_cast<double>(frames.size() - 2));
    return report;
}

} // namespace engine::imgcmp
