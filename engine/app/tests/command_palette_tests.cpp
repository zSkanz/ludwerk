// The palette's matching, without a window: which commands a few typed letters
// find, and in what order. The order is the feature -- a palette whose first
// row is not the one meant is a palette people stop typing into.
#include <doctest/doctest.h>
#include <string_view>
#include <vector>

#include "engine/app/command_palette.h"

using namespace engine::app;

TEST_CASE("fuzzyScore: every query character, in order, case ignored")
{
    CHECK(fuzzyScore("save", "File: Save Scene") >= 0);
    CHECK(fuzzyScore("SAVE", "File: Save Scene") >= 0);
    CHECK(fuzzyScore("fss", "File: Save Scene") >= 0);
    CHECK(fuzzyScore("evas", "File: Save Scene") < 0);
    CHECK(fuzzyScore("x", "File: Save Scene") < 0);
    CHECK(fuzzyScore("", "anything") >= 0);
}

TEST_CASE("fuzzyScore: spaces separate words rather than match a space")
{
    CHECK(fuzzyScore("ins part", "Insert: Part") >= 0);
    CHECK(fuzzyScore("ins  part", "Insert: Part") >= 0);
    CHECK(fuzzyScore("part ins", "Insert: Part") < 0);
}

TEST_CASE("fuzzyScore: word starts and runs outrank scattered letters")
{
    // "sv" at two word starts beats the same letters in the middle of words.
    CHECK(fuzzyScore("ss", "File: Save Scene") > fuzzyScore("ss", "Edit: Paste Session"));
    // A consecutive run beats the same letters spread out.
    CHECK(fuzzyScore("undo", "Edit: Undo") > fuzzyScore("undo", "View: Hide Unused Nodes Overlay"));
    // A camel-case hump is a word start: "ljm" finds LineJoinMode's words.
    CHECK(fuzzyScore("ljm", "LineJoinMode") > fuzzyScore("ljm", "Lollipop jam"));
}

TEST_CASE("fuzzyScore: the shorter title wins a tie")
{
    CHECK(fuzzyScore("save", "Save") > fuzzyScore("save", "Save Scene As"));
}

TEST_CASE("fuzzyScore: the matched offsets are reported for highlighting")
{
    std::vector<int> positions;
    REQUIRE(fuzzyScore("ip", "Insert: Part", &positions) >= 0);
    CHECK(positions == std::vector<int>{0, 8});

    // And cleared on a miss, so a stale highlight is never drawn.
    CHECK(fuzzyScore("zz", "Insert: Part", &positions) < 0);
    CHECK(positions.empty());
}
