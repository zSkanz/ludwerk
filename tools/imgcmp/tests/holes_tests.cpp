#include <cstdint>
#include <doctest/doctest.h>

#include "engine/imgcmp/holes.h"

using engine::imgcmp::findHoles;
using engine::imgcmp::HoleReport;
using engine::imgcmp::Image;
using engine::imgcmp::makeImage;

namespace {

// A picture of sky, magenta after a tonemap, with a white rectangle of ground.
[[nodiscard]] Image skyWithGround(int x0, int y0, int x1, int y1)
{
    Image image = makeImage(40, 30, 0);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            std::uint8_t* pixel = &image.rgba[(static_cast<std::size_t>(y) * 40u + static_cast<std::size_t>(x)) * 4u];
            const bool ground = x >= x0 && x < x1 && y >= y0 && y < y1;
            pixel[0] = ground ? 230 : 214;
            pixel[1] = ground ? 230 : 20;
            pixel[2] = ground ? 230 : 200;
            pixel[3] = 255;
        }
    }
    return image;
}

void paintSky(Image& image, int x, int y)
{
    std::uint8_t* pixel = &image.rgba[(static_cast<std::size_t>(y) * 40u + static_cast<std::size_t>(x)) * 4u];
    pixel[0] = 214;
    pixel[1] = 20;
    pixel[2] = 200;
}

} // namespace

TEST_CASE("sky inside the ground is a hole; sky round it is not")
{
    Image shot = skyWithGround(5, 5, 35, 25);
    HoleReport report = findHoles(shot, nullptr, 2);
    CHECK(report.enclosed == 0);
    CHECK(report.ground == 30u * 20u);

    // A crack two pixels long in the middle of the ground.
    paintSky(shot, 20, 15);
    paintSky(shot, 21, 15);
    report = findHoles(shot, nullptr, 2);
    CHECK(report.enclosed == 2);

    // A notch open to the sky outside is the silhouette, not a hole.
    Image notched = skyWithGround(5, 5, 35, 25);
    for (int y = 5; y < 12; ++y)
        paintSky(notched, 20, y);
    CHECK(findHoles(notched, nullptr, 2).enclosed == 0);
}

TEST_CASE("ground the reference has and the shot lost counts, past the slack of a coarser silhouette")
{
    const Image reference = skyWithGround(5, 5, 35, 25);
    // The same ground one pixel smaller all round: a coarser silhouette.
    const Image coarser = skyWithGround(6, 6, 34, 24);
    CHECK(findHoles(coarser, &reference, 2).vanished == 0);
    // The right third gone: ground lost.
    const Image lost = skyWithGround(5, 5, 25, 25);
    CHECK(findHoles(lost, &reference, 2).vanished > 0);
}
