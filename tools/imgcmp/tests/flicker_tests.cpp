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
