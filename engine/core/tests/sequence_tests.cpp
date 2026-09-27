#include <cmath>
#include <doctest/doctest.h>
#include <limits>

#include "engine/core/sequence.h"

using namespace engine::core;

namespace {

bool near(f32 a, f32 b) noexcept
{
    return std::fabs(a - b) <= 1e-5f;
}

bool near(Color3 a, Color3 b) noexcept
{
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b);
}

} // namespace

TEST_CASE("a default sequence changes nothing")
{
    const ColorSequence color;
    const NumberSequence transparency;
    CHECK(validSequence(color.keypoints));
    CHECK(validSequence(transparency.keypoints));
    for (const f32 time : {0.0f, 0.3f, 1.0f}) {
        CHECK(near(evaluate(color, time), Color3{1.0f, 1.0f, 1.0f}));
        CHECK(near(evaluate(transparency, time), 0.0f));
    }
}

TEST_CASE("between two stops the value is their straight mix")
{
    ColorSequence sequence;
    sequence.keypoints = {
        {0.0f, Color3{1.0f, 0.0f, 0.0f}}, {0.5f, Color3{0.0f, 1.0f, 0.0f}}, {1.0f, Color3{0.0f, 0.0f, 1.0f}}};
    CHECK(near(evaluate(sequence, 0.25f), Color3{0.5f, 0.5f, 0.0f}));
    CHECK(near(evaluate(sequence, 0.75f), Color3{0.0f, 0.5f, 0.5f}));
    CHECK(near(evaluate(sequence, 1.0f), Color3{0.0f, 0.0f, 1.0f}));
}

TEST_CASE("a time outside the line is clamped to its ends")
{
    NumberSequence sequence;
    sequence.keypoints = {{0.0f, 2.0f, 0.0f}, {1.0f, 4.0f, 0.0f}};
    CHECK(near(evaluate(sequence, -3.0f), 2.0f));
    CHECK(near(evaluate(sequence, 7.0f), 4.0f));
    CHECK(near(evaluate(sequence, std::numeric_limits<f32>::quiet_NaN()), 2.0f));
}

TEST_CASE("two stops at one time are a hard edge, and the later wins at it")
{
    NumberSequence sequence;
    sequence.keypoints = {{0.0f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
    REQUIRE(validSequence(sequence.keypoints));
    CHECK(near(evaluate(sequence, 0.49f), 0.0f));
    CHECK(near(evaluate(sequence, 0.5f), 1.0f));
    CHECK(near(evaluate(sequence, 0.51f), 1.0f));
}

TEST_CASE("a sequence starts at 0, ends at 1, never goes back, and has two to twenty stops")
{
    const std::vector<NumberKeypoint> good{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
    CHECK(validSequence(good));

    CHECK_FALSE(validSequence(std::vector<NumberKeypoint>{{0.0f, 0.0f, 0.0f}}));
    CHECK_FALSE(validSequence(std::vector<NumberKeypoint>{{0.1f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}));
    CHECK_FALSE(validSequence(std::vector<NumberKeypoint>{{0.0f, 0.0f, 0.0f}, {0.9f, 1.0f, 0.0f}}));
    CHECK_FALSE(validSequence(
        std::vector<NumberKeypoint>{{0.0f, 0.0f, 0.0f}, {0.6f, 1.0f, 0.0f}, {0.4f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}));
    CHECK_FALSE(validSequence(
        std::vector<NumberKeypoint>{{0.0f, std::numeric_limits<f32>::infinity(), 0.0f}, {1.0f, 1.0f, 0.0f}}));

    std::vector<ColorKeypoint> many;
    for (usize index = 0; index <= MaxSequenceKeypoints; ++index)
        many.push_back(ColorKeypoint{static_cast<f32>(index) / static_cast<f32>(MaxSequenceKeypoints), {}});
    CHECK_FALSE(validSequence(many));
    many.pop_back();
    many.back().time = 1.0f;
    CHECK(validSequence(many));
}
