// `print` of a table (the owner, 2026-09-27): the line says what is in it, and
// the rows beside it are what the console folds open.
#include <doctest/doctest.h>
#include <string>

#include "script_fixture.h"

using engine::script::testing::Fixture;

namespace {

// The last line printed, and the rows it carried -- without the first, which
// names where the print was (`@chunk`, the line).
[[nodiscard]] std::pair<std::string, std::string> lastPrint(const Fixture& fixture)
{
    REQUIRE_FALSE(fixture.logged.empty());
    std::string rows = fixture.details.back();
    if (!rows.empty() && rows.front() == '@')
        rows.erase(0, rows.find('\n') + 1);
    return {fixture.logged.back().second, rows};
}

} // namespace

TEST_CASE("a printed table says what is in it, in a stable order")
{
    Fixture fixture;
    REQUIRE(fixture.booted);
    REQUIRE_FALSE(fixture.run(R"(print({ b = 2, a = "x", [2] = true, [1] = { deep = 1 } }))").has_value());
    const auto [line, rows] = lastPrint(fixture);
    CHECK(line == R"({[1] = {deep = 1}, [2] = true, a = "x", b = 2})");
    CHECK(rows == "0\x1F[1]\x1F{deep = 1}\n"
                  "1\x1F"
                  "deep\x1F"
                  "1\n"
                  "0\x1F[2]\x1Ftrue\n"
                  "0\x1F"
                  "a\x1F\"x\"\n"
                  "0\x1F"
                  "b\x1F"
                  "2\n");
}

TEST_CASE("a plain print carries no rows, and a table that names itself prints its name")
{
    Fixture fixture;
    REQUIRE(fixture.booted);
    REQUIRE_FALSE(fixture.run(R"(print("hello", 1))").has_value());
    CHECK(lastPrint(fixture).first == "hello\t1");
    CHECK(lastPrint(fixture).second.empty());

    REQUIRE_FALSE(
        fixture.run(R"(print(setmetatable({}, { __tostring = function() return "Named" end })))").has_value());
    CHECK(lastPrint(fixture).first == "Named");
    CHECK(lastPrint(fixture).second.empty());
}

TEST_CASE("a print says where it was")
{
    Fixture fixture;
    REQUIRE(fixture.booted);
    REQUIRE_FALSE(fixture.run("local x = 1\nprint(x)").has_value());
    const std::string& detail = fixture.details.back();
    CHECK(detail.rfind("@", 0) == 0);
    // The second line of the chunk.
    CHECK(detail.find(std::string(1, '\x1F') + "2\n") != std::string::npos);
}

TEST_CASE("a table that holds itself is printed once, not forever")
{
    Fixture fixture;
    REQUIRE(fixture.booted);
    REQUIRE_FALSE(fixture.run(R"(local t = { name = "loop" }; t.self = t; print(t))").has_value());
    const auto [line, rows] = lastPrint(fixture);
    CHECK(line.find("name = \"loop\"") != std::string::npos);
    CHECK(rows.find("<the table it is inside>") != std::string::npos);
}
