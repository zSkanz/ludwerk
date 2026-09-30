#include <cstdint>
#include <doctest/doctest.h>

#include "engine/imgcmp/shadow.h"

using engine::imgcmp::findSelfShadow;
using engine::imgcmp::Image;
using engine::imgcmp::makeImage;
using engine::imgcmp::ShadowReport;

namespace {

void paint(Image& image, int x, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    std::uint8_t* pixel = &image.rgba[static_cast<std::size_t>(x) * 4u];
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = 255;
}

} // namespace

TEST_CASE("a face to the sun either shadow darkens is counted; a face turned away is not")
{
    Image image = makeImage(7, 1, 0);
    paint(image, 0, 172, 172, 172); // faces the sun, lit by both
    paint(image, 1, 40, 172, 172);  // the map darkens it
    paint(image, 2, 172, 40, 172);  // the contact mask darkens it
    paint(image, 3, 150, 150, 172); // both do, by an acne's speckle
    paint(image, 4, 40, 40, 0);     // turned away: dark is right
    paint(image, 5, 0, 0, 0);       // the sky, black
    paint(image, 6, 166, 170, 172); // within a twentieth: lit
    const ShadowReport report = findSelfShadow(image);
    CHECK(report.facing == 5);
    CHECK(report.mapDark == 2);
    CHECK(report.contactDark == 2);
}
