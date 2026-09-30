#include "engine/imgcmp/tile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace engine::imgcmp {

namespace {

// The picture's detail: luminance less its mean over a box `window` wide,
// through a summed-area table.
std::vector<double> detail(const std::vector<double>& light, int width, int height, int window)
{
    const auto stride = static_cast<std::size_t>(width) + 1u;
    std::vector<double> sums(stride * (static_cast<std::size_t>(height) + 1u), 0.0);
    for (int y = 0; y < height; ++y) {
        double row = 0.0;
        for (int x = 0; x < width; ++x) {
            row += light[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            sums[(static_cast<std::size_t>(y) + 1u) * stride + static_cast<std::size_t>(x) + 1u] =
                sums[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) + 1u] + row;
        }
    }
    const int half = window / 2;
    std::vector<double> out(light.size());
    for (int y = 0; y < height; ++y) {
        const int y0 = std::max(0, y - half);
        const int y1 = std::min(height, y + half + 1);
        for (int x = 0; x < width; ++x) {
            const int x0 = std::max(0, x - half);
            const int x1 = std::min(width, x + half + 1);
            const auto at = [&](int ax, int ay) {
                return sums[static_cast<std::size_t>(ay) * stride + static_cast<std::size_t>(ax)];
            };
            const double total = at(x1, y1) - at(x0, y1) - at(x1, y0) + at(x0, y0);
            const double mean = total / static_cast<double>((x1 - x0) * (y1 - y0));
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            out[index] = light[index] - mean;
        }
    }
    return out;
}

// The normalised correlation of the detail with itself shifted by (dx, dy),
// over where the two overlap.
double correlation(const std::vector<double>& d, int width, int height, int dx, int dy)
{
    double both = 0.0;
    double first = 0.0;
    double second = 0.0;
    for (int y = 0; y + dy < height; ++y) {
        for (int x = 0; x + dx < width; ++x) {
            const double a =
                d[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            const double b = d[static_cast<std::size_t>(y + dy) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x + dx)];
            both += a * b;
            first += a * a;
            second += b * b;
        }
    }
    const double scale = std::sqrt(first * second);
    return scale > 0.0 ? both / scale : 0.0;
}

// `correlation` for a shift left and down.
double mirrored(const std::vector<double>& d, int width, int height, int dx, int dy)
{
    double both = 0.0;
    double first = 0.0;
    double second = 0.0;
    for (int y = 0; y + dy < height; ++y) {
        for (int x = dx; x < width; ++x) {
            const double a =
                d[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            const double b = d[static_cast<std::size_t>(y + dy) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x - dx)];
            both += a * b;
            first += a * a;
            second += b * b;
        }
    }
    const double scale = std::sqrt(first * second);
    return scale > 0.0 ? both / scale : 0.0;
}

} // namespace

TileReport measureTiling(const Image& shot, double period)
{
    TileReport report;
    if (!shot.wellFormed() || !(period >= 4.0) ||
        period * 3.0 >= static_cast<double>(std::min(shot.width, shot.height)))
        return report;
    const int width = shot.width;
    const int height = shot.height;
    std::vector<double> light(shot.pixelCount());
    for (std::size_t index = 0; index < light.size(); ++index) {
        const std::uint8_t* pixel = &shot.rgba[index * 4u];
        light[index] = 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
    }

    const std::vector<double> d = detail(light, width, height, static_cast<int>(std::lround(period)) | 1);
    const int low = static_cast<int>(std::floor(period * 0.95));
    const int high = static_cast<int>(std::ceil(period * 1.05));
    report.across = -1.0;
    report.down = -1.0;
    for (int shift = low; shift <= high; ++shift) {
        report.across = std::max(report.across, correlation(d, width, height, shift, 0));
        report.down = std::max(report.down, correlation(d, width, height, 0, shift));
    }

    // The ring, on the detail shrunk by whole boxes.
    const int shrink = std::max(1, static_cast<int>(period / 16.0));
    const int smallWidth = width / shrink;
    const int smallHeight = height / shrink;
    std::vector<double> small(static_cast<std::size_t>(smallWidth) * static_cast<std::size_t>(smallHeight), 0.0);
    for (int y = 0; y < smallHeight * shrink; ++y) {
        for (int x = 0; x < smallWidth * shrink; ++x)
            small[static_cast<std::size_t>(y / shrink) * static_cast<std::size_t>(smallWidth) +
                  static_cast<std::size_t>(x / shrink)] +=
                d[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
    const double smallPeriod = period / static_cast<double>(shrink);
    const int reach = static_cast<int>(std::ceil(smallPeriod * 1.05));
    report.ring = -1.0;
    // Half the ring: a shift and its opposite correlate alike.
    for (int dy = 0; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            const double length = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            if (length < smallPeriod * 0.95 || length > smallPeriod * 1.05 || (dy == 0 && dx < 0))
                continue;
            report.ring = std::max(report.ring, dx >= 0 ? correlation(small, smallWidth, smallHeight, dx, dy)
                                                        : mirrored(small, smallWidth, smallHeight, -dx, dy));
        }
    }

    // Blocks a period across, whole ones only.
    const int block = static_cast<int>(std::lround(period));
    std::vector<double> means;
    for (int by = 0; by + block <= height; by += block) {
        for (int bx = 0; bx + block <= width; bx += block) {
            double total = 0.0;
            for (int y = by; y < by + block; ++y) {
                for (int x = bx; x < bx + block; ++x)
                    total += light[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)];
            }
            means.push_back(total / static_cast<double>(block * block));
        }
    }
    double mean = 0.0;
    for (const double m : means)
        mean += m;
    mean /= static_cast<double>(means.size());
    double variance = 0.0;
    for (const double m : means)
        variance += (m - mean) * (m - mean);
    variance /= static_cast<double>(means.size());
    report.blocks = mean > 0.0 ? std::sqrt(variance) / mean : 0.0;
    return report;
}

} // namespace engine::imgcmp
