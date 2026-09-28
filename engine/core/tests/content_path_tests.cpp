// The one check every outside path goes through (audit F2 to F5).
#include <doctest/doctest.h>

#include "engine/core/content_path.h"

using engine::core::resolveUnder;
using engine::core::safeRelativePath;

TEST_CASE("a path under the root is folded and kept")
{
    CHECK(safeRelativePath("scenes/arena.scene.json") == "scenes/arena.scene.json");
    CHECK(safeRelativePath("./models//tree.gltf") == "models/tree.gltf");
    CHECK(safeRelativePath("models/old/../tree.gltf") == "models/tree.gltf");
    CHECK(safeRelativePath("a b/cafe.png") == "a b/cafe.png");
}

TEST_CASE("a path that leaves the root, or names the machine, is refused")
{
    for (const char* bad : {"",
                            "/etc/passwd",
                            "../secret",
                            "a/../../b",
                            "..",
                            "C:/Windows/win.ini",
                            "C:secret",
                            "//host/share/x",
                            "\\\\host\\share\\x",
                            "a\\..\\..\\b",
                            "models\\tree.gltf",
                            "file.txt:stream",
                            "a/CON",
                            "NUL.txt",
                            "com1",
                            "LPT9.log",
                            "trailing./x",
                            "space /x",
                            "tab\there",
                            ".",
                            "./"}) {
        CAPTURE(bad);
        CHECK_FALSE(safeRelativePath(bad).has_value());
    }
    // Names that only look like a device are ordinary.
    CHECK(safeRelativePath("CONSOLE/x").has_value());
    CHECK(safeRelativePath("com/x").has_value());
}

TEST_CASE("resolveUnder joins only what it accepts")
{
    const std::filesystem::path root = std::filesystem::path("project") / "content";
    const auto joined = resolveUnder(root, "models/tree.gltf");
    REQUIRE(joined.has_value());
    CHECK(*joined == root / "models" / "tree.gltf");
    CHECK_FALSE(resolveUnder(root, "../src/init.luau").has_value());
    CHECK_FALSE(resolveUnder(root, "//evil/share").has_value());
}
