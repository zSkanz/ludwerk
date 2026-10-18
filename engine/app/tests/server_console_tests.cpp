// **A server's console** (ADR 0218): what its operator types, what a game adds
// to it, and who it keeps out -- a server and a client in one process, over
// the memory transport, encrypted as every match is.
#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/app/network_session.h"
#include "engine/app/server_console.h"
#include "engine/app/world_host.h"
#include "engine/net/memory_transport.h"
#include "engine/platform/process.h"
#include "engine/replication/replication.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

namespace {

constexpr core::i32 Offline = 0;
constexpr core::i32 Connected = 2;

struct Machine
{
    Project project;
    std::unique_ptr<app::WorldHost> host = std::make_unique<app::WorldHost>();
    std::unique_ptr<app::NetworkSession> network;

    void boot(const std::shared_ptr<net::MemoryNetwork>& wire)
    {
        REQUIRE_FALSE(host->boot(bootOptions(project.root)).has_value());
        replication::Config base;
        base.ticksPerSnapshot = 1;
        base.interpolationDelayTicks = 0;
        network = std::make_unique<app::NetworkSession>([this]() { return host.get(); }, base,
                                                        [wire]() { return net::createMemoryTransport(wire); });
        network->setJoinTimeout(0.5);
    }

    void frame()
    {
        network->receive();
        host->tick();
        network->send();
        network->sendMessages();
        network->update();
    }

    [[nodiscard]] core::i32 state() const { return host->world().engineState().networkState; }

    // The name this machine's player has in the match.
    [[nodiscard]] std::string playerName() const
    {
        const scene::World& world = host->world();
        const core::InstanceId local = scene::localPlayerOf(world);
        const scene::PlayerComponent* player = world.players().find(local);
        return player != nullptr ? "Player" + std::to_string(player->userId) : std::string{};
    }
};

// The server's console, and what it last answered.
struct Console
{
    app::ServerConsole console;
    std::vector<std::string> answers;
    std::vector<bool> failures;
    std::int64_t now = 1'800'000'000;

    explicit Console(std::filesystem::path bans) : console(std::move(bans)) {}

    [[nodiscard]] app::ServerConsole::Host hostOf(Machine& server) const
    {
        return app::ServerConsole::Host{.world = &server.host->world(),
                                        .dataModel = server.host->dataModel(),
                                        .replication = server.network->replication(),
                                        .flushSaves = [&server]() { server.host->flushSaves(); },
                                        .nowNs = 1'000'000'000ull,
                                        .nowUnix = now};
    }

    void type(Machine& server, std::string_view line)
    {
        console.run(
            line, app::ServerConsole::Caller{.local = true, .name = "local", .key = {}, .permission = 3},
            [this](const std::string& text, bool failed) {
                answers.push_back(text);
                failures.push_back(failed);
            },
            hostOf(server));
    }

    void frame(Machine& server) { console.frame(hostOf(server)); }
};

void run(Machine& server, Machine& client, Console& console, int frames)
{
    for (int at = 0; at < frames; ++at) {
        server.frame();
        client.frame();
        console.frame(server);
    }
}

constexpr std::string_view ServerScript = R"(
    local ConsoleService = game:GetService("ConsoleService")
    local NetworkService = game:GetService("NetworkService")
    ConsoleService:Register("give", {
        Description = "Gives a player something.",
        Parameters = {
            { Name = "player", Type = "Player" },
            { Name = "item", Type = "string" },
            { Name = "count", Type = "number", Optional = true },
        },
        Permission = 2,
    }, function(caller, player: Player, item: string, count: number?)
        return `gave {count or 1} {item} to {player.Name}, asked by {caller.Name}`
    end)
    ConsoleService:Register("slow", { Parameters = {} }, function()
        task.wait(0.05)
        return "slow-done"
    end)
    ConsoleService:Register("broken", {}, function()
        error("broken on purpose")
    end)
    NetworkService:Host(47961)
)";

constexpr std::string_view ClientScript = R"(
    local NetworkService = game:GetService("NetworkService")
    NetworkService.Announced:Connect(function(text: string)
        print(`heard:{text}`)
    end)
    -- The engine's own banner, mounted in one call.
    local shown = require("@engine/announcements").mount({ Seconds = 1 })
    NetworkService.Announced:Connect(function()
        task.defer(function()
            print(`shown:{table.concat(shown:GetLines(), "|")}`)
        end)
    end)
    NetworkService.Disconnected:Connect(function(reason: string)
        print(`gone:{reason}`)
    end)
    NetworkService.JoinFailed:Connect(function(reason: string)
        print(`refused:{reason}`)
    end)
    NetworkService.Connected:Connect(function()
        print("joined")
        -- A client has no console.
        local ok, err = pcall(function()
            game:GetService("ConsoleService"):Register("mine", {}, function() end)
        end)
        print(`client-console:{ok}:{err}`)
    end)
    NetworkService:Join("memory:47961")
)";

[[nodiscard]] bool said(const Console& console, std::string_view needle)
{
    for (const std::string& answer : console.answers) {
        if (answer.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("a server's console lists its players and runs a game's command, which may take a player and yield")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/console.luau", std::string(ServerScript));
    server.boot(wire);
    Console console(server.project.root / "bans.json");
    server.network->setJoinGate(
        [&console](const replication::JoinAsk& ask) { return console.console.refusal(ask, console.now); });
    Machine client;
    client.project.write("src/client/join.luau", std::string(ClientScript));
    client.boot(wire);
    run(server, client, console, 40);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    REQUIRE(client.state() == Connected);
    // And a client cannot reach the console.
    CHECK(log.contains("client-console:false"));

    // Each player, with a key: the match is encrypted.
    console.type(server, "players");
    REQUIRE(console.answers.size() == 1);
    CHECK(console.answers.back().find("UserId") != std::string::npos);
    CHECK(console.answers.back().find("key -") == std::string::npos);

    // A game's command, its player found by name, run at the next tick.
    const std::string name = client.playerName();
    console.type(server, "give " + name + " sword 3");
    CHECK(console.answers.size() == 1);
    run(server, client, console, 2);
    REQUIRE(console.answers.size() == 2);
    CHECK(console.answers.back() == "gave 3 sword to " + name + ", asked by local");
    CHECK_FALSE(console.failures.back());
    // An optional one left out is nil.
    console.type(server, "give " + name + " shield");
    run(server, client, console, 2);
    CHECK(console.answers.back() == "gave 1 shield to " + name + ", asked by local");

    // A line that does not fit is answered with the usage, and nothing runs.
    console.type(server, "give " + name);
    CHECK(console.failures.back());
    CHECK(console.answers.back().find("give <player> <item> [count]") != std::string::npos);
    console.type(server, "give nobody sword");
    CHECK(console.failures.back());
    CHECK(console.answers.back().find("nobody") != std::string::npos);

    // One that yields answers when it finishes; one that fails says why, and
    // the server goes on.
    console.type(server, "slow");
    run(server, client, console, 20);
    CHECK(said(console, "slow-done"));
    console.type(server, "broken");
    run(server, client, console, 2);
    CHECK(console.failures.back());
    CHECK(console.answers.back().find("broken on purpose") != std::string::npos);

    // An unknown command, and `help` listing the game's beside the engine's.
    console.type(server, "fly");
    CHECK(console.failures.back());
    console.type(server, "help");
    CHECK(console.answers.back().find("give <player> <item> [count] -- Gives a player something.") !=
          std::string::npos);
    CHECK(console.answers.back().find("ban <player|key>") != std::string::npos);
}

TEST_CASE("say reaches every player, and a ban by key holds against the same game coming back")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/console.luau", std::string(ServerScript));
    server.boot(wire);
    const std::filesystem::path bans = server.project.root / "saves" / "bans.json";
    auto console = std::make_unique<Console>(bans);
    server.network->setJoinGate(
        [&console](const replication::JoinAsk& ask) { return console->console.refusal(ask, console->now); });
    Machine client;
    client.project.write("src/client/join.luau", std::string(ClientScript));
    client.boot(wire);
    run(server, client, *console, 40);
    REQUIRE(client.state() == Connected);

    console->type(server, "say The gates open at noon");
    run(server, client, *console, 4);
    CHECK(log.contains("heard:The gates open at noon"));
    CHECK(log.contains("shown:The gates open at noon"));

    const std::string name = client.playerName();
    console->type(server, "ban " + name + " 60 cheating");
    run(server, client, *console, 20);
    CHECK(client.state() == Offline);
    CHECK(log.contains("gone:You are banned from this server: cheating"));
    CHECK(std::filesystem::exists(bans));

    // **The same game, with the same key, comes back**: refused, with words.
    client.host->world().engineState().pendingNetwork = scene::EngineState::NetworkRequest{
        scene::EngineState::NetworkRequest::Kind::Join, "memory:47961", 0, {}, true, {}};
    run(server, client, *console, 40);
    CHECK(client.state() == Offline);
    CHECK(log.contains("You are banned from this server: cheating"));

    // **Across a restart**: a new console reads the same file.
    console = std::make_unique<Console>(bans);
    client.host->world().engineState().pendingNetwork = scene::EngineState::NetworkRequest{
        scene::EngineState::NetworkRequest::Kind::Join, "memory:47961", 0, {}, true, {}};
    run(server, client, *console, 40);
    CHECK(client.state() == Offline);

    // And past its end it holds nobody.
    console->now += 61 * 60;
    client.host->world().engineState().pendingNetwork = scene::EngineState::NetworkRequest{
        scene::EngineState::NetworkRequest::Kind::Join, "memory:47961", 0, {}, true, {}};
    run(server, client, *console, 40);
    CHECK(client.state() == Connected);
}

TEST_CASE("a script runs a console line, and stop tells everybody, lets them go and ends the server")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/console.luau", std::string(ServerScript) + R"(
        task.spawn(function()
            local answer = ConsoleService:ExecuteAsync("help kick")
            print(`script-ran:{answer}`)
            local ok, err = pcall(function()
                return ConsoleService:ExecuteAsync("give nobody x")
            end)
            print(`script-failed:{ok}`)
        end)
    )");
    server.boot(wire);
    Console console(server.project.root / "bans.json");
    server.network->setJoinGate(
        [&console](const replication::JoinAsk& ask) { return console.console.refusal(ask, console.now); });
    Machine client;
    client.project.write("src/client/join.luau", std::string(ClientScript));
    client.boot(wire);
    run(server, client, console, 40);
    REQUIRE(client.state() == Connected);
    CHECK(log.contains("script-ran:kick <player> [reason]"));
    CHECK(log.contains("script-failed:false"));

    console.type(server, "stop");
    CHECK(console.answers.back().find("stopping") != std::string::npos);
    run(server, client, console, 20);
    CHECK(log.contains("heard:The server is stopping now."));
    CHECK(log.contains("gone:The server is stopping."));
    CHECK(client.state() == Offline);
    CHECK(console.console.finished());
}

TEST_CASE("the worlds of one game share their bans: a ban in one holds in every other, and so does an unban")
{
    // Two worlds' lists over one file, as a manager's worlds have.
    Project folder;
    const std::filesystem::path file = folder.root / "bans.json";
    app::BanList town(file);
    app::BanList fields(file);
    CHECK(fields.find("account:ana", 100) == nullptr);

    town.add(app::BanList::Ban{"account:ana", "griefing", 0, "local", 100});
    const app::BanList::Ban* seen = fields.find("account:ana", 100);
    REQUIRE(seen != nullptr);
    CHECK(seen->reason == "griefing");

    // A ban made in the second while the first already holds one keeps both.
    fields.add(app::BanList::Ban{"key:abcd", "", 0, "local", 101});
    CHECK(town.find("account:ana", 102) != nullptr);
    CHECK(town.find("key:abcd", 102) != nullptr);

    CHECK(fields.remove("account:ana"));
    CHECK(town.find("account:ana", 103) == nullptr);
    CHECK(town.find("key:abcd", 103) != nullptr);
}

TEST_CASE("a mute is kept beside the bans and read by every world; suspects and suspect list what went past")
{
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/console.luau", std::string(ServerScript));
    server.boot(wire);
    const std::filesystem::path bans = server.project.root / "saves" / "bans.json";
    Console console(bans);
    Machine client;
    client.project.write("src/client/join.luau", std::string(ClientScript));
    client.boot(wire);
    run(server, client, console, 40);
    REQUIRE(client.state() == Connected);
    const std::string name = client.playerName();
    const scene::World& clientWorld = client.host->world();
    const core::u32 userId = clientWorld.players().find(scene::localPlayerOf(clientWorld))->userId;
    scene::World& world = server.host->world();
    const core::InstanceId player = scene::playerByUserId(world, userId);
    REQUIRE(player.valid());

    // Muted for ten minutes: the file beside the bans, and the world told.
    console.type(server, "mute " + name + " 10 spam");
    REQUIRE(!console.failures.back());
    CHECK(std::filesystem::exists(bans.parent_path() / "mutes.json"));
    console.frame(server);
    REQUIRE(world.engineState().muted.size() == 1);
    const std::string who = world.engineState().muted[0];
    CHECK(who.starts_with("key:"));

    // **Another world of the same game** reads the same list.
    Console other(bans);
    world.engineState().muted.clear();
    other.frame(server);
    CHECK(world.engineState().muted == std::vector<std::string>{who});

    // What went past, named as the mute names the player.
    scene::suspect(world, player, "impossible_score", "9999");
    scene::suspect(world, player, "impossible_score", "10000");
    console.type(server, "suspects");
    CHECK(console.answers.back().find(who + ": 2") != std::string::npos);
    console.type(server, "suspect " + name);
    CHECK(console.answers.back().find("impossible_score  10000") != std::string::npos);

    // Past its end, nobody; and an unmute lifts one before it.
    Console later(bans);
    later.now = console.now + 11 * 60;
    later.frame(server);
    CHECK(world.engineState().muted.empty());
    console.type(server, "unmute " + who);
    REQUIRE(!console.failures.back());
    console.frame(server);
    CHECK(world.engineState().muted.empty());
    console.type(server, "unmute " + who);
    CHECK(console.failures.back());
}

TEST_CASE("a server whose input is an open pipe nobody writes still ends when its frames are done")
{
    // **What held a packaged server open for an hour**: told to run a few
    // frames and end, with its standard input a pipe its parent never wrote
    // to and never closed. Its console was waiting for a line, holding the C
    // library's lock on that input, and the end of the process waited for the
    // lock.
    Project project;
    project.write("src/server/quiet.luau", "print('quiet-server')");
    platform::ProcessOptions options;
    options.pipeInput = true;
    std::unique_ptr<platform::ChildProcess> server = platform::ChildProcess::start(
        {ENG_TEST_HOST, project.root.string(), "--serve=47977", "--headless", "--rhi=null", "--frames=30", "--exit"},
        options);
    REQUIRE(server != nullptr);
    bool ended = false;
    for (int waited = 0; waited < 600 && !ended; ++waited) {
        ended = !server->running();
        if (!ended)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK_MESSAGE(ended, "the server was still running 30 seconds after its last frame");
    if (ended)
        CHECK(server->exitCode() == 0);
}
