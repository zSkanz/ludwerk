#include <cstdint>
#include <doctest/doctest.h>

#include "engine/imgcmp/mirror.h"

using engine::imgcmp::checkMirror;
using engine::imgcmp::Image;
using engine::imgcmp::makeImage;
using engine::imgcmp::MirrorReport;

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

void fill(Image& image, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x)
            paint(image, x, y, r, g, b);
    }
}

// Water one blue everywhere, a sky, and the mirror's picture of that sky with
// a red post in column 2 of its TOP two rows -- which, turned top to bottom,
// is column 2 of the water's bottom two rows.
struct Lake
{
    Image plain = makeImage(8, 8, 0);
    Image sky = makeImage(8, 8, 0);
    Image mirrored = makeImage(8, 8, 0);

    Lake()
    {
        fill(plain, 20, 40, 90);
        fill(sky, 120, 160, 220);
        fill(mirrored, 120, 160, 220);
        paint(mirrored, 2, 0, 220, 30, 30);
        paint(mirrored, 2, 1, 220, 30, 30);
    }
};

} // namespace

TEST_CASE("a thing in the water where the mirror has it is true; a streak where the mirror shows the sky is wrong")
{
    const Lake lake;
    Image reflected = lake.plain;
    // The post, tinted and dimmed as water does: redder, not red.
    paint(reflected, 2, 7, 80, 30, 60);
    paint(reflected, 2, 6, 80, 30, 60);
    MirrorReport report = checkMirror(reflected, lake.plain, lake.mirrored, lake.sky, 24, 0);
    CHECK(report.reflecting == 2);
    CHECK(report.wrong == 0);

    // A bank's sand across open water: brighter, as the sky is brighter than
    // the water -- which is why "nearer the mirror's colour" is no test.
    paint(reflected, 6, 3, 190, 180, 100);
    report = checkMirror(reflected, lake.plain, lake.mirrored, lake.sky, 24, 0);
    CHECK(report.reflecting == 3);
    CHECK(report.wrong == 1);
    CHECK(report.firstX == 6);
    CHECK(report.firstY == 3);
}

TEST_CASE("the wrong thing reflected is wrong: a change away from what the mirror shows there")
{
    const Lake lake;
    Image reflected = lake.plain;
    // Where the mirror has a red post, the water went blue-white instead.
    paint(reflected, 2, 7, 60, 110, 200);
    const MirrorReport report = checkMirror(reflected, lake.plain, lake.mirrored, lake.sky, 24, 0);
    CHECK(report.reflecting == 1);
    CHECK(report.wrong == 1);
}

TEST_CASE("the mirror is read a little either side, and a pixel that did not change is not counted")
{
    const Lake lake;
    Image reflected = lake.plain;
    // A change under the tolerance is the water as it was.
    paint(reflected, 0, 0, 22, 42, 92);
    // The post one pixel beside where the mirror has it: an edge's difference.
    paint(reflected, 3, 7, 80, 30, 60);
    CHECK(checkMirror(reflected, lake.plain, lake.mirrored, lake.sky, 24, 0).wrong == 1);
    const MirrorReport report = checkMirror(reflected, lake.plain, lake.mirrored, lake.sky, 24, 1);
    CHECK(report.reflecting == 1);
    CHECK(report.wrong == 0);
}

TEST_CASE("pictures of different sizes are no report at all")
{
    const Image small = makeImage(4, 4, 0);
    const Image large = makeImage(8, 8, 0);
    const MirrorReport report = checkMirror(small, small, large, small, 24, 2);
    CHECK(report.reflecting == 0);
    CHECK(report.wrong == 0);
}
