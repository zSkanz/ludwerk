// **Who may join, as the game says** (ADR 0226): `OnJoinRequest`, a ticket a
// backend signed, one session an account, and a ban that holds an account
// whatever key it comes back with.
#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <ostream>
#include <string>

#include "engine/app/network_session.h"
#include "engine/app/server_console.h"
#include "engine/app/world_host.h"
#include "engine/core/crypto.h"
#include "engine/core/ticket.h"
#include "engine/net/memory_transport.h"
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
constexpr std::string_view Game = "com.example.realms";

struct Machine
{
    Project project;
    std::unique_ptr<app::WorldHost> host = std::make_unique<app::WorldHost>();
    std::unique_ptr<app::NetworkSession> network;
    core::u64 now = 1'000'000'000ull;

    void boot(const std::shared_ptr<net::MemoryNetwork>& wire)
    {
        REQUIRE_FALSE(host->boot(bootOptions(project.root)).has_value());
        replication::Config base;
        base.ticksPerSnapshot = 1;
        base.interpolationDelayTicks = 0;
        network = std::make_unique<app::NetworkSession>([this]() { return host.get(); }, base,
                                                        [wire]() { return net::createMemoryTransport(wire); });
        network->setJoinTimeout(0.5);
        network->setClock([this]() { return now; });
    }

    void frame()
    {
        now += 16'000'000ull;
        network->receive();
        host->tick();
        network->send();
        network->sendMessages();
        network->update();
    }

    [[nodiscard]] core::i32 state() const { return host->world().engineState().networkState; }

    void join(std::string ticket)
    {
        host->world().engineState().pendingNetwork =
            scene::EngineState::NetworkRequest{.kind = scene::EngineState::NetworkRequest::Kind::Join,
                                               .address = "memory:47971",
                                               .port = 0,
                                               .relay = {},
                                               .relayDirect = true,
                                               .ticket = std::move(ticket)};
    }
};

void run(Machine& server, std::initializer_list<Machine*> clients, int frames)
{
    for (int at = 0; at < frames; ++at) {
        server.frame();
        for (Machine* client : clients)
            client->frame();
    }
}

[[nodiscard]] std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

constexpr std::string_view ServerScript = R"(
    local NetworkService = game:GetService("NetworkService")
    NetworkService.OnJoinRequest = function(joiner)
        -- A backend asked, as a game would: the answer comes a while later.
        task.wait(0.05)
        local claims = NetworkService:VerifyTicket(joiner.Ticket)
        if claims == nil then
            return false, "Please log in again."
        end
        joiner.AccountId = claims.sub
        return true
    end
    NetworkService.PlayerAdded:Connect(function(player: Player)
        print(`added:{player.AccountId}`)
    end)
    NetworkService:Host(47971)
)";

constexpr std::string_view ClientScript = R"(
    local NetworkService = game:GetService("NetworkService")
    NetworkService.JoinFailed:Connect(function(reason: string)
        print(`refused:{reason}`)
    end)
    NetworkService.Disconnected:Connect(function(reason: string)
        print(`gone:{reason}`)
    end)
)";

} // namespace

TEST_CASE("a joiner with a backend's ticket is their account, one without is refused in the game's words")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    const core::Ed25519Pair backend = core::ed25519Generate();

    Machine server;
    server.project.write("src/server/join.luau", std::string(ServerScript));
    server.boot(wire);
    app::NetworkSession::MatchSecurity security;
    security.ticketKey = backend.pub;
    security.projectId = std::string(Game);
    server.network->setSecurity(security);
    app::ServerConsole console(server.project.root / "bans.json");
    server.network->setAccountGate(
        [&console](const std::string& account) { return console.accountRefusal(account, unixNow()); });

    Machine first;
    first.project.write("src/client/watch.luau", std::string(ClientScript));
    first.boot(wire);
    Machine second;
    second.project.write("src/client/watch.luau", std::string(ClientScript));
    second.boot(wire);
    run(server, {&first, &second}, 4);

    // No ticket: refused, in the game's own words, and never a player.
    first.join({});
    run(server, {&first, &second}, 60);
    CHECK(first.state() == Offline);
    CHECK(log.contains("refused:Please log in again."));
    CHECK_FALSE(log.contains("added:"));

    // A ticket the backend signed: the account, on the server's `Player`.
    first.join(core::signTicket(backend.secret, "account-1", Game, unixNow(), 60, "t1"));
    run(server, {&first, &second}, 60);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    REQUIRE(first.state() == Connected);
    CHECK(log.contains("added:account-1"));

    // **Spent once**: the same ticket again is refused.
    second.join(core::signTicket(backend.secret, "account-2", Game, unixNow(), 60, "t1"));
    run(server, {&first, &second}, 60);
    CHECK(second.state() == Offline);

    // **One session an account**: the same account from another machine
    // drops the first.
    second.join(core::signTicket(backend.secret, "account-1", Game, unixNow(), 60, "t2"));
    run(server, {&first, &second}, 60);
    CHECK(second.state() == Connected);
    CHECK(first.state() == Offline);
    CHECK(log.contains("gone:This account joined again from somewhere else."));

    // **A ban by account holds**, whatever key comes with it.
    console.run("ban account:account-1 cheating",
                app::ServerConsole::Caller{.local = true, .name = "local", .key = {}, .permission = 3}, nullptr,
                app::ServerConsole::Host{.world = &server.host->world(),
                                         .dataModel = server.host->dataModel(),
                                         .replication = server.network->replication(),
                                         .flushSaves = nullptr,
                                         .nowNs = 1,
                                         .nowUnix = unixNow()});
    first.join(core::signTicket(backend.secret, "account-1", Game, unixNow(), 60, "t3"));
    run(server, {&first, &second}, 60);
    CHECK(first.state() == Offline);
    CHECK(log.contains("refused:You are banned from this server: cheating"));
}

TEST_CASE("an OnJoinRequest that never answers is a refusal after ten seconds, and the server goes on")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.OnJoinRequest = function(joiner)
            while true do
                task.wait(1)
            end
        end
        NetworkService:Host(47971)
    )");
    server.boot(wire);
    Machine client;
    client.project.write("src/client/watch.luau", std::string(ClientScript));
    client.boot(wire);
    run(server, {&client}, 4);
    client.join({});
    run(server, {&client}, 30);
    CHECK(client.state() != Connected);
    // Ten seconds of the session's clock.
    server.now += 11'000'000'000ull;
    run(server, {&client}, 30);
    CHECK(client.state() == Offline);
    CHECK(log.contains("did not answer within 10 seconds"));
}
