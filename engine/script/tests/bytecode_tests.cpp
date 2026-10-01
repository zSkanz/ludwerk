// A packaged game's scripts are bytecode (ADR 0112), and the runtime takes it
// where it takes source.

#include <doctest/doctest.h>
#include <string>

#include "engine/script/bytecode.h"
#include "script_fixture.h"

using namespace engine;

TEST_CASE("source and bytecode are told apart by their first byte")
{
    CHECK_FALSE(script::isBytecode(""));
    CHECK_FALSE(script::isBytecode("--!strict\nprint(1)"));
    CHECK_FALSE(script::isBytecode("\xEF\xBB\xBFprint(1)"));
    CHECK_FALSE(script::isBytecode("\tprint(1)"));
    CHECK_FALSE(script::isBytecode("\t\tprint(1)"));
    CHECK_FALSE(script::isBytecode("\n"));

    std::string bytecode;
    std::string error;
    REQUIRE(script::compileForPackage("print(1)", bytecode, error));
    CHECK(script::isBytecode(bytecode));
}

TEST_CASE("a script compiled for a package runs as its source would")
{
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);

    std::string bytecode;
    std::string error;
    REQUIRE(script::compileForPackage("local part = Instance.new('Part') part.Name = 'FromBytecode'", bytecode, error));
    CHECK_FALSE(fixture.runtime->runSource(bytecode, "src/client/init.luau").has_value());

    bool found = false;
    fixture.world->parts().forEach([&](core::InstanceId id, const scene::PartComponent&) {
        if (fixture.world->atoms().text(fixture.world->name(id)) == "FromBytecode")
            found = true;
    });
    CHECK(found);
}

TEST_CASE("a script an editor on Windows marked as UTF-8 compiles as the same script (D415)")
{
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);

    // Run from source, as `ludwerk dev` and the editor do.
    const std::string marked = "\xEF\xBB\xBF"
                               "local held = 1\nerror('boom')\n";
    const auto failed = fixture.runtime->runSource(marked, "src/client/init.luau");
    REQUIRE(failed.has_value());
    // Its own error, on its own line -- not a syntax error on line 1.
    CHECK(failed->message.find("src/client/init.luau:2") != std::string::npos);
    CHECK(failed->message.find("boom") != std::string::npos);

    // And packaged.
    std::string bytecode;
    std::string error;
    CHECK(script::compileForPackage("\xEF\xBB\xBF"
                                    "print(1)",
                                    bytecode, error));
    CHECK(error.empty());
}

TEST_CASE("an error in a packaged script still names the script and the line")
{
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);

    // Debug level 1 keeps lines and drops local names: what a player's log
    // needs to be a bug report.
    std::string bytecode;
    std::string error;
    REQUIRE(script::compileForPackage("local held = 1\nerror('boom')\n", bytecode, error));
    const auto failed = fixture.runtime->runSource(bytecode, "src/client/init.luau");
    REQUIRE(failed.has_value());
    CHECK(failed->message.find("src/client/init.luau:2") != std::string::npos);
}

TEST_CASE("bytecode another engine build wrote is refused by name")
{
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);

    std::string bytecode;
    std::string error;
    REQUIRE(script::compileForPackage("print(1)", bytecode, error));
    // A version below any this VM reads; the types byte after it still says
    // bytecode rather than source.
    bytecode[0] = '\x02';
    const auto refused = fixture.runtime->runSource(bytecode, "src/client/old.luau");
    REQUIRE(refused.has_value());
    CHECK(refused->message.find("script.err.bytecode_version") != std::string::npos);
    CHECK(refused->message.find("src/client/old.luau") != std::string::npos);
}

TEST_CASE("a script that does not compile is not packaged")
{
    std::string bytecode;
    std::string error;
    CHECK_FALSE(script::compileForPackage("local = 1", bytecode, error));
    CHECK_FALSE(error.empty());
    CHECK(bytecode.empty());
}
