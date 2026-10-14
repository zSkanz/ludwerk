#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>

#include "engine/imgcmp/tile.h"

using engine::imgcmp::Image;
using engine::imgcmp::makeImage;
using engine::imgcmp::measureTiling;
using engine::imgcmp::TileReport;

namespace {

void grey(Image& image, int x, int y, std::uint8_t value)
{
    const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width);
    std::uint8_t* pixel = &image.rgba[(row + static_cast<std::size_t>(x)) * 4u];
    pixel[0] = value;
    pixel[1] = value;
    pixel[2] = value;
    pixel[3] = 255;
}

// A fixed pseudo-random byte for a pixel: a patch with no pattern of its own.
std::uint8_t noise(int x, int y)
{
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return static_cast<std::uint8_t>(64u + ((h ^ (h >> 16u)) & 127u));
}

} // namespace

TEST_CASE("one patch laid every period repeats; the same noise unrepeated does not")
{
    constexpr int Period = 16;
    Image repeating = makeImage(128, 96, 0);
    Image unrepeated = makeImage(128, 96, 0);
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 128; ++x) {
            grey(repeating, x, y, noise(x % Period, y % Period));
            grey(unrepeated, x, y, noise(x, y));
        }
    }
    const TileReport tiled = measureTiling(repeating, Period);
    CHECK(tiled.across > 0.99);
    CHECK(tiled.down > 0.99);
    // Every block is the same patch, so the same mean.
    CHECK(tiled.blocks < 1e-9);
    const TileReport broken = measureTiling(unrepeated, Period);
    CHECK(broken.across < 0.2);
    CHECK(broken.down < 0.2);
    CHECK(broken.ring < 0.2);
    CHECK(broken.blocks > 0.0);
}

TEST_CASE("a patch laid on a turned lattice repeats any way, though not across or down")
{
    // The lattice's steps are (16, 12) and (-12, 16): twenty pixels long,
    // turned 37 degrees from the picture's axes.
    Image image = makeImage(160, 160, 0);
    for (int y = 0; y < 160; ++y) {
        for (int x = 0; x < 160; ++x) {
            // Coordinates in the lattice, then back to a cell of the patch.
            const double u = (16.0 * x + 12.0 * y) / 400.0;
            const double v = (-12.0 * x + 16.0 * y) / 400.0;
            const double fu = u - std::floor(u);
            const double fv = v - std::floor(v);
            grey(image, x, y, noise(static_cast<int>(fu * 8.0), static_cast<int>(fv * 8.0)));
        }
    }
    const TileReport report = measureTiling(image, 20.0);
    CHECK(report.across < 0.5);
    CHECK(report.down < 0.5);
    CHECK(report.ring > 0.8);
}

TEST_CASE("a slow drift across the picture is not a repeat, but it does vary the blocks")
{
    constexpr int Period = 16;
    Image image = makeImage(128, 96, 0);
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 128; ++x)
            grey(image, x, y, static_cast<std::uint8_t>(noise(x, y) / 2 + x));
    }
    const TileReport report = measureTiling(image, Period);
    CHECK(report.across < 0.2);
    CHECK(report.blocks > 0.2);
}

TEST_CASE("a period the picture cannot hold three of measures nothing")
{
    const TileReport report = measureTiling(makeImage(40, 40, 128), 16);
    CHECK(report.across == 0.0);
    CHECK(report.blocks == 0.0);
}
