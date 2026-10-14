// What the editor shows under the pointer of a description written for the
// reference.
#include <doctest/doctest.h>
#include <string_view>

#include "engine/app/doc_summary.h"

using engine::app::docSummary;

TEST_CASE("a tooltip shows the first paragraph of a description")
{
    CHECK(docSummary("The size of the part, in meters.\n\nIt is saved with the scene.") ==
          "The size of the part, in meters.");
    CHECK(docSummary("Whether the light shines.") == "Whether the light shines.");
    CHECK(docSummary("").empty());
}

TEST_CASE("a tooltip shows no marks of the reference's markup")
{
    CHECK(docSummary("Uses `Size` and `Enum.PartType.Ball`.") == "Uses Size and Enum.PartType.Ball.");
    CHECK(docSummary("**Visual only.** Not replicated.") == "Visual only. Not replicated.");
    // One star is arithmetic, and stays: `2 * math.sqrt(Stiffness * m)`.
    CHECK(docSummary("Critical damping is `2 * math.sqrt(k * m)`.") == "Critical damping is 2 * math.sqrt(k * m).");
}

TEST_CASE("a tooltip's line breaks only at a space, never inside 0.25 or .irradiance")
{
    // One unit a character: a line holds `width` characters.
    const auto measure = [](std::string_view run) { return static_cast<float>(run.size()); };
    using engine::app::wrapAtSpaces;

    CHECK(wrapAtSpaces("a path ending in .irradiance here", 18.0f, measure) == "a path ending in\n.irradiance here");
    CHECK(wrapAtSpaces("from 0.25 to 16", 7.0f, measure) == "from\n0.25 to\n16");
    CHECK(wrapAtSpaces("short", 80.0f, measure) == "short");
    // A word wider than the line is not cut: it has a line to itself.
    CHECK(wrapAtSpaces("see Enum.IrradianceMode.BounceOnly now", 10.0f, measure) ==
          "see\nEnum.IrradianceMode.BounceOnly\nnow");
    CHECK(wrapAtSpaces("", 10.0f, measure).empty());
    // A break the words came with is kept, and each line is cut on its own.
    CHECK(wrapAtSpaces("Grass\nasset://terrain/grass here\nlayer 2", 22.0f, measure) ==
          "Grass\nasset://terrain/grass\nhere\nlayer 2");
    CHECK(wrapAtSpaces("one\n\ntwo", 10.0f, measure) == "one\n\ntwo");
}

TEST_CASE("a line break inside the first paragraph reads as a space")
{
    CHECK(docSummary("One of:\n- a part\n- a model\n\nMore.") == "One of: - a part - a model");
}
