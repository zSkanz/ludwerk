#include <array>
#include <cstdint>
#include <doctest/doctest.h>

#include "engine/imgcmp/flicker.h"

using engine::imgcmp::Image;

namespace {

// A one-row picture of grey levels.
[[nodiscard]] Image row(std::initializer_list<std::uint8_t> levels)
{
    Image image;
    image.width = static_cast<int>(levels.size());
    image.height = 1;
    for (const std::uint8_t level : levels) {
        image.rgba.push_back(level);
        image.rgba.push_back(level);
        image.rgba.push_back(level);
        image.rgba.push_back(255);
    }
    return image;
}

} // namespace

TEST_CASE("a pop is one frame out of line and back, of a size that is asked for")
{
    using engine::imgcmp::countPops;
    // Five pixels: still; a speckle of 12 that comes back; a step of 12 that
    // stays; a thing crossing, 150 and back; a level of rounding.
    const Image before = row({100, 100, 100, 100, 100});
    const Image now = row({100, 112, 112, 250, 101});
    const Image after = row({100, 100, 112, 100, 100});
    // The speckle alone is a small pop.
    CHECK(countPops(before, now, after, 6.0, 24.0) == 1);
    // With no upper bound the crossing is one as well.
    CHECK(countPops(before, now, after, 6.0, 255.0) == 2);
    // Frames of two sizes are no measure.
    CHECK(countPops(before, row({100, 100}), after, 6.0, 24.0) == -1);
}

TEST_CASE("only one thing's pops are counted when its colour is asked for")
{
    // Two pixels that pop by the same twelve levels of luma: one red, one grey.
    const auto pixels = [](std::initializer_list<std::array<std::uint8_t, 3>> colours) {
        Image image;
        image.width = static_cast<int>(colours.size());
        image.height = 1;
        for (const auto& colour : colours)
            image.rgba.insert(image.rgba.end(), {colour[0], colour[1], colour[2], 255});
        return image;
    };
    const Image before = pixels({{180, 40, 40}, {100, 100, 100}});
    const Image now = pixels({{180, 57, 57}, {112, 112, 112}});
    CHECK(engine::imgcmp::countPops(before, now, before, 6.0, 24.0) == 2);
    // Red by more than forty: the red one alone.
    CHECK(engine::imgcmp::countPops(before, now, before, 6.0, 24.0, engine::imgcmp::PopColour{0, 40.0}) == 1);
    // Green: neither.
    CHECK(engine::imgcmp::countPops(before, now, before, 6.0, 24.0, engine::imgcmp::PopColour{1, 40.0}) == 0);
}

TEST_CASE("a pixel that changes steadily does not flicker; one that pops does")
{
    // An edge sweeping across: each pixel brightens by the same step a frame.
    const std::array<Image, 4> steady{row({0, 40}), row({40, 80}), row({80, 120}), row({120, 160})};
    const auto smooth = engine::imgcmp::measureFlicker(steady, 0.0);
    CHECK(smooth.error.empty());
    CHECK(smooth.frames == 4);
    CHECK(smooth.flicker == doctest::Approx(0.0));

    // A thin line that is there, gone, there: every inner frame pops.
    const std::array<Image, 4> crawling{row({255, 0}), row({0, 255}), row({255, 0}), row({0, 255})};
    const auto popping = engine::imgcmp::measureFlicker(crawling);
    CHECK(popping.flicker > 400.0);

    // A level or two of rounding is no flicker; a pop past the band is.
    const std::array<Image, 3> rounding{row({100}), row({102}), row({100})};
    CHECK(engine::imgcmp::measureFlicker(rounding).flicker == doctest::Approx(0.0));
    CHECK(engine::imgcmp::measureFlicker(rounding, 0.0).flicker == doctest::Approx(4.0));
}

TEST_CASE("fewer than three frames, or frames of two sizes, are refused")
{
    const std::array<Image, 2> two{row({1}), row({2})};
    CHECK_FALSE(engine::imgcmp::measureFlicker(two).error.empty());
    const std::array<Image, 3> mixed{row({1}), row({2, 3}), row({4})};
    CHECK_FALSE(engine::imgcmp::measureFlicker(mixed).error.empty());
}
