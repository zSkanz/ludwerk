#include <cstdint>
#include <doctest/doctest.h>

#include "engine/imgcmp/steps.h"

using engine::imgcmp::findSteps;
using engine::imgcmp::Image;
using engine::imgcmp::makeImage;
using engine::imgcmp::StepReport;

namespace {

void paint(Image& image, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width);
    std::uint8_t* pixel = &image.rgba[(row + static_cast<std::size_t>(x)) * 4u];
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = 255;
}

} // namespace

TEST_CASE("a normal that jumps between two pixels of ground is a step; a gentle turn and the sky's edge are not")
{
    Image image = makeImage(4, 2, 0);
    // Top row: ground turning gently, then a jump, then gently again.
    paint(image, 0, 0, 128, 250, 128);
    paint(image, 1, 0, 132, 249, 126);
    paint(image, 2, 0, 170, 230, 120);
    paint(image, 3, 0, 172, 229, 121);
    // Bottom row: the sky, black, under the first three -- ground over sky is
    // an edge, not a step -- and ground like the pixel above it under the last.
    paint(image, 0, 1, 0, 0, 0);
    paint(image, 1, 1, 0, 0, 0);
    paint(image, 2, 1, 0, 0, 0);
    paint(image, 3, 1, 172, 229, 121);
    const StepReport report = findSteps(image, 24);
    CHECK(report.ground == 5);
    // Only the pixel before the jump: its right neighbour is 38 away in red.
    CHECK(report.steps == 1);
    // Red runs from 128 to 172 over the ground; the sky's black is not ground.
    CHECK(report.spread == 44);
}
