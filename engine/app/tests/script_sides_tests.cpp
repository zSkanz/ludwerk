#include <doctest/doctest.h>
#include <string>

#include "engine/app/script_sides.h"
#include "engine/core/text_key.h"
#include "project_fixture.h"

using namespace engine;
using app::testing::Captured;
using app::testing::Project;

namespace {

[[nodiscard]] std::vector<std::string> words(const std::vector<app::SideFinding>& findings)
{
    std::vector<std::string> out;
    for (const app::SideFinding& finding : findings)
        out.push_back(finding.word);
    return out;
}

} // namespace

TEST_CASE("a client script reaching for the server's storage is told it finds nothing on a joined client (ADR 0138 §8)")
{
    const std::string source = "local keys = game:GetService(\"ServerStorage\")\n"
                               "-- ServerScriptService in a comment is not a finding\n"
                               "print(workspace.CurrentCamera)\n";
    const std::vector<app::SideFinding> client = app::lintScriptSide(source, script::ScriptSide::Client, true, true);
    REQUIRE(client.size() == 1);
    CHECK(client[0].word == "ServerStorage");
    CHECK(client[0].line == 0);
    CHECK(client[0].column == 30);
    CHECK(client[0].length == 13);

    // The same text on the server: the camera is what it lacks.
    CHECK(words(app::lintScriptSide(source, script::ScriptSide::Server, true, true)) ==
          std::vector<std::string>{"CurrentCamera"});
    // A solo game has one side, and nothing to say.
    CHECK(app::lintScriptSide(source, script::ScriptSide::Client, true, false).empty());
}

TEST_CASE("a server script that sets a morph weight is told nobody will see it (ADR 0196)")
{
    // A weight is kept by the machine that draws it: not replicated, not in a
    // replay. Set by the server's script it shows on no player's screen, and
    // a face that never moves is found a long way from the line that set it.
    const std::string source = "local face = workspace.Hero.Face\n"
                               "face:SetMorphWeight(\"Blink\", 1)\n"
                               "print(face:GetMorphWeight(\"Blink\"))\n";
    const std::vector<app::SideFinding> server = app::lintScriptSide(source, script::ScriptSide::Server, true, true);
    REQUIRE(server.size() == 1);
    CHECK(server[0].word == "SetMorphWeight");
    CHECK(server[0].line == 1);
    CHECK(server[0].key.hash == ENG_TR("script.warn.side_server_touches_player").hash);

    // Where a player sits it is the right place, and reading one is anybody's.
    CHECK(app::lintScriptSide(source, script::ScriptSide::Client, true, true).empty());
    CHECK(app::lintScriptSide(source, script::ScriptSide::Server, true, false).empty());
}

TEST_CASE("a script for both sides that never asks which one it is on is told so, in a multiplayer project only")
{
    const std::string blind = "print('hello')\n";
    const std::string asks = "if game:GetService('NetworkService').Authority then print('server') end\n";
    CHECK(app::lintScriptSide(blind, script::ScriptSide::Anywhere, false, true).size() == 1);
    CHECK(app::lintScriptSide(asks, script::ScriptSide::Anywhere, false, true).empty());
    CHECK(app::lintScriptSide(blind, script::ScriptSide::Anywhere, false, false).empty());
    // A long comment hides a word too.
    CHECK(app::lintScriptSide("--[[ Authority ]] print(1)", script::ScriptSide::Anywhere, false, true).size() == 1);
}

TEST_CASE("engine-host --check-sides lints the scripts inside scenes, stamps, global.json and src")
{
    Captured log;
    Project project;
    project.write("project.toml", "[project]\nname = \"sides\"\n\n[export]\nmultiplayer = \"dedicated\"\n");
    project.write(
        "content/scenes/main.scene.json",
        R"json({"format":"scene","version":2,"root":{"name":"Workspace","children":[)json"
        R"json({"class":"Part","name":"Door","children":[{"class":"Script","name":"Creak",)json"
        R"json("properties":{"Source":"print(game.ServerStorage)","RunContext":"Client"}}]}]},)json"
        R"json("storage":{"ServerScriptService":{"class":"ServerScriptService","name":"ServerScriptService",)json"
        R"json("children":[{"class":"Script","name":"Hud","properties":{"Source":"print(workspace.CurrentCamera)"}}]}}})json");
    project.write("content/stamps/lamp.stamp.json",
                  R"json({"format":"scene","version":2,"root":{"class":"Script","name":"Glow",)json"
                  R"json("properties":{"Source":"print('glow')"}}})json");
    project.write("src/client/hud.luau", "local s = game:GetService('ServerScriptService')\n");

    CHECK(app::checkProjectSides(project.root) == 4);
    CHECK(log.contains("content/scenes/main.scene.json > Workspace.Door.Creak:1:12: warning: ServerStorage"));
    CHECK(log.contains("content/scenes/main.scene.json > ServerScriptService.Hud:1:17: warning: CurrentCamera"));
    CHECK(log.contains("content/stamps/lamp.stamp.json > Glow:1:1: warning: This script runs on the server and on"));
    CHECK(log.contains("src/client/hud.luau:1:28: warning: ServerScriptService"));
}
