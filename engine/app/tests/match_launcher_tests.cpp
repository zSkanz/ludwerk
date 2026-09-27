// The editor's Play with players (ADR 0106 §5): the commands a plan makes, the
// windows it tiles, and a match of real processes started, read and stopped --
// headless, which is the part a test can watch. The tiling on a real screen is
// checked by hand.
#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <thread>

#include "engine/app/match_launcher.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::Project;

namespace {

[[nodiscard]] bool has(const std::vector<std::string>& arguments, std::string_view wanted)
{
    for (const std::string& argument : arguments) {
        if (argument == wanted)
            return true;
    }
    return false;
}

[[nodiscard]] bool hasPrefix(const std::vector<std::string>& arguments, std::string_view prefix)
{
    for (const std::string& argument : arguments) {
        if (argument.starts_with(prefix))
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("a match of three players is a host and two clients, each with a window of its own")
{
    app::MatchPlan plan;
    plan.host = "engine-host";
    plan.project = "game";
    plan.players = 3;
    plan.port = 7780;
    plan.area = platform::WindowPlacement{0, 0, 1600, 900, false};
    const std::vector<app::MatchCommand> commands = app::MatchLauncher::commands(plan);
    REQUIRE(commands.size() == 3);
    CHECK(commands[0].name == "Host");
    CHECK(has(commands[0].arguments, "--host=7780"));
    CHECK(commands[1].name == "Client 1");
    CHECK(has(commands[1].arguments, "--join=127.0.0.1:7780"));
    CHECK(commands[2].name == "Client 2");
    // Three windows on a two-by-two grid: every one placed, none the same.
    CHECK(has(commands[0].arguments, "--window=0,0,800,450"));
    CHECK(has(commands[1].arguments, "--window=800,0,800,450"));
    CHECK(has(commands[2].arguments, "--window=0,450,800,450"));
}

TEST_CASE("a dedicated match is a server with no window and a client per player")
{
    app::MatchPlan plan;
    plan.host = "engine-host";
    plan.project = "game";
    plan.players = 2;
    plan.dedicated = true;
    plan.area = platform::WindowPlacement{0, 0, 1000, 500, false};
    const std::vector<app::MatchCommand> commands = app::MatchLauncher::commands(plan);
    REQUIRE(commands.size() == 3);
    CHECK(commands[0].name == "Server");
    CHECK(has(commands[0].arguments, "--serve=7777"));
    CHECK_FALSE(hasPrefix(commands[0].arguments, "--window="));
    CHECK(has(commands[1].arguments, "--window=0,0,500,500"));
    CHECK(has(commands[2].arguments, "--window=500,0,500,500"));
}

TEST_CASE("the windows tile one, two, and four ways")
{
    const platform::WindowPlacement area{10, 20, 1200, 800, false};
    CHECK(app::MatchLauncher::tiles(area, 1).size() == 1);
    CHECK(app::MatchLauncher::tiles(area, 1)[0].width == 1200);
    const auto two = app::MatchLauncher::tiles(area, 2);
    REQUIRE(two.size() == 2);
    CHECK(two[1].x == 610);
    CHECK(two[1].height == 800);
    const auto four = app::MatchLauncher::tiles(area, 4);
    REQUIRE(four.size() == 4);
    CHECK(four[3].x == 610);
    CHECK(four[3].y == 420);
    CHECK(app::MatchLauncher::tiles(platform::WindowPlacement{}, 2).empty());
}

TEST_CASE("a match starts real processes, names what each says, and Stop ends them all")
{
    Project project;
    project.write("src/client/hello.luau", "print('match-hello')");

    app::MatchPlan plan;
    plan.host = ENG_TEST_HOST;
    plan.project = project.root;
    plan.players = 2;
    plan.port = 47201;
    plan.logDirectory = project.root / "logs";
    plan.extraArguments = {"--headless", "--frames=100000", "--rhi=null"};

    app::MatchLauncher launcher;
    REQUIRE(launcher.start(plan));
    CHECK(launcher.size() == 2);

    // Both say hello, each under its own name.
    bool host = false;
    bool client = false;
    for (int wait = 0; wait < 300 && !(host && client); ++wait) {
        for (const app::MatchLauncher::Line& line : launcher.poll()) {
            if (line.text.find("match-hello") == std::string::npos)
                continue;
            host = host || line.process == "Host";
            client = client || line.process == "Client 1";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(host);
    CHECK(client);
    CHECK(launcher.running());

    launcher.stop();
    CHECK_FALSE(launcher.running());
    CHECK(launcher.size() == 0);
}
