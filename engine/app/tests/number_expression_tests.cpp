// Arithmetic typed into a number field (the owner, 2026-09-27: `0+2` must be
// 2 and `1/2` must be 0.5, and nothing may break).
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <string_view>

#include "engine/app/number_expression.h"

using engine::app::evaluateNumberExpression;

namespace {

[[nodiscard]] double value(std::string_view text)
{
    const std::optional<double> result = evaluateNumberExpression(text);
    REQUIRE_MESSAGE(result.has_value(), std::string(text));
    return *result;
}

} // namespace

TEST_CASE("a number field works out what is typed into it")
{
    CHECK(value("0+2") == doctest::Approx(2.0));
    CHECK(value("1/2") == doctest::Approx(0.5));
    CHECK(value("2*3+1") == doctest::Approx(7.0));
    CHECK(value("2*(3+1)") == doctest::Approx(8.0));
    CHECK(value("10 - 4 - 3") == doctest::Approx(3.0));
    CHECK(value("-5") == doctest::Approx(-5.0));
    CHECK(value("-(2+3)") == doctest::Approx(-5.0));
    CHECK(value("  12.5  ") == doctest::Approx(12.5));
    CHECK(value("1e3/4") == doctest::Approx(250.0));
}

TEST_CASE("a plain number reads as itself, with its unit or a comma for a point")
{
    CHECK(value("0.000 m") == doctest::Approx(0.0));
    CHECK(value("4.000 m + 1") == doctest::Approx(5.0));
    CHECK(value("-10.0\xC2\xB0") == doctest::Approx(-10.0));
    CHECK(value("90 deg / 2") == doctest::Approx(45.0));
    CHECK(value("0,5") == doctest::Approx(0.5));
}

TEST_CASE("what cannot be worked out changes nothing")
{
    CHECK_FALSE(evaluateNumberExpression("").has_value());
    CHECK_FALSE(evaluateNumberExpression("1/0").has_value());
    CHECK_FALSE(evaluateNumberExpression("2+").has_value());
    CHECK_FALSE(evaluateNumberExpression("(1+2").has_value());
    CHECK_FALSE(evaluateNumberExpression("abc").has_value());
    CHECK_FALSE(evaluateNumberExpression("1 2").has_value());
    CHECK_FALSE(evaluateNumberExpression("--").has_value());
}
