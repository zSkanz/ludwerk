// A match hosted through a relay and joined by its code, from a script (ADR
// 0178): two games and a relay in one process, on real sockets -- the memory
// transport has no relay to speak of, and what is tested here is that
// `Host`'s options, `JoinCode`, `RelayStateChanged`, a `Join` by code and its
// failures reach a script as the manual says they do.
#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <thread>

#include "engine/app/network_session.h"
#include "engine/app/world_host.h"
#include "engine/net/relay_service.h"
#include "engine/replication/replication.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

namespace {

// Enum.NetworkState.
constexpr core::i32 Offline = 0;
constexpr core::i32 Connected = 2;
constexpr core::i32 Hosting = 3;
// The host's port; `relay_transport_tests.cpp` has the one before it.
constexpr core::u16 HostPort = 47926;

struct Machine
{
    Project project;
    std::unique_ptr<app::WorldHost> host = std::make_unique<app::WorldHost>();
    std::unique_ptr<app::NetworkSession> network;

    void boot(const std::string& relay)
    {
        app::WorldHostOptions options = bootOptions(project.root);
        // What `[network] relay` is to a script: the relay a call names none over.
        options.defaultRelay = relay;
        REQUIRE_FALSE(host->boot(options).has_value());
        replication::Config base;
        base.ticksPerSnapshot = 1;
        base.interpolationDelayTicks = 0;
        // The engine's own transport: ENet, on the loopback.
        network = std::make_unique<app::NetworkSession>([this]() { return host.get(); }, base);
        network->setJoinTimeout(15.0);
    }

    void frame()
    {
        network->receive();
        host->tick();
        network->send();
        network->sendMessages();
        network->update();
    }

    [[nodiscard]] scene::EngineState& state() const { return host->world().engineState(); }
};

// Frames of both, a millisecond apart -- the sockets are real, and so is the
// time a handshake takes -- until `done` or the budget is spent.
template <typename Predicate>
bool runUntil(Machine& a, Machine& b, Predicate done, int frames = 3000)
{
    for (int at = 0; at < frames; ++at) {
        if (done())
            return true;
        a.frame();
        b.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

} // namespace

TEST_CASE("a script hosts through a relay, reads its join code, and another joins by typing it (ADR 0178)")
{
    Captured log;
    net::RelayService relay;
    REQUIRE_FALSE(relay.start(0).has_value());
    const std::string relayAt = "127.0.0.1:" + std::to_string(relay.port());

    Machine server;
    server.project.write("src/client/host.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        print(`before relay:{NetworkService.RelayState.Name} code:"{NetworkService.JoinCode}"`)
        NetworkService.RelayStateChanged:Connect(function()
            print(`relay:{NetworkService.RelayState.Name} code-length:{#NetworkService.JoinCode}`)
        end)
        NetworkService.PlayerAdded:Connect(function()
            task.wait(0.2)
            print(`host path:{NetworkService:GetStats().Path}`)
        end)
        NetworkService:Host()" + std::to_string(HostPort) +
                                                     R"(, { Relay = ")" + relayAt + R"(" })
    )");
    server.boot({});

    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Connected:Connect(function()
            print(`joined path:{NetworkService:GetStats().Path} relay:{NetworkService.RelayState.Name}`)
        end)
        NetworkService.JoinFailed:Connect(function(reason: string)
            print(`join failed: {reason}`)
        end)
    )");
    // The relay is the project's (`[network] relay`), as a shipped game has it.
    client.boot(relayAt);

    REQUIRE(runUntil(server, client, [&] { return !server.state().networkJoinCode.empty(); }));
    CHECK(server.state().networkState == Hosting);
    CHECK(log.contains("before relay:None code:\"\""));
    // The signal is deferred, as every signal is: heard a tick on.
    REQUIRE(runUntil(server, client, [&] { return log.contains("relay:Ready code-length:8"); }));
    const std::string code = server.state().networkJoinCode;

    // Nobody has this code: the script is told so, in words, and is still solo.
    client.state().pendingNetwork = scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Join,
                                                                       "2222-2222", 0, relayAt, true};
    REQUIRE(runUntil(server, client, [&] { return log.contains("join failed:"); }));
    CHECK(log.contains("join failed: No match has the code 22222222"));
    CHECK(client.state().networkState == Offline);

    // The code as a player types it -- lower case, a dash in the middle -- and
    // straight through the relay.
    std::string typed;
    for (std::size_t index = 0; index < code.size(); ++index) {
        if (index == 4)
            typed += '-';
        typed += static_cast<char>(code[index] >= 'A' && code[index] <= 'Z' ? code[index] - 'A' + 'a' : code[index]);
    }
    client.state().pendingNetwork =
        scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Join, typed, 0, relayAt, false};
    REQUIRE(runUntil(server, client, [&] { return client.state().networkState == Connected; }));
    REQUIRE(runUntil(server, client, [&] { return log.contains("host path:"); }));
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("joined path:relayed relay:None"));
    CHECK(log.contains("host path:relayed"));
    CHECK(relay.stats().relayed == 1);
    CHECK(relay.stats().bytes > 0);

    // The client leaves and comes back the short way: on one machine that is
    // its own network, and the relay carries nothing of it.
    client.state().pendingNetwork =
        scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Disconnect, {}, 0, {}, true};
    REQUIRE(runUntil(server, client, [&] { return client.state().networkState == Offline; }));
    REQUIRE(runUntil(server, client, [&] { return relay.stats().relayed == 0; }));
    client.state().pendingNetwork = scene::EngineState::NetworkRequest{
        scene::EngineState::NetworkRequest::Kind::Join, "relay://" + relayAt + "/" + code, 0, {}, true};
    REQUIRE(runUntil(server, client, [&] { return client.state().networkState == Connected; }));
    REQUIRE(runUntil(server, client,
                     [&] { return log.contains("joined path:lan") || log.contains("joined path:direct"); }));
    CHECK(relay.stats().relayed == 0);

    // The host stops: the code is nobody's, and its `JoinCode` is empty again.
    server.state().pendingNetwork =
        scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Disconnect, {}, 0, {}, true};
    REQUIRE(runUntil(server, client, [&] { return server.state().networkState == Offline; }));
    CHECK(server.state().networkJoinCode.empty());
    REQUIRE(runUntil(server, client, [&] { return log.contains("relay:None code-length:0"); }));
    REQUIRE(runUntil(server, client, [&] { return relay.stats().sessions == 0; }));
}

TEST_CASE("a script asks a relay how far it is before it has a match, and is told nothing by one that is not there")
{
    // What a game with relays in several regions does on its first screen: no
    // match yet, each relay asked, the nearest chosen (ADR 0178, amended).
    Captured log;
    net::RelayService relay;
    REQUIRE_FALSE(relay.start(0).has_value());
    const std::string relayAt = "127.0.0.1:" + std::to_string(relay.port());
    // A relay that was there and is not: its port is nobody's now.
    net::RelayService gone;
    REQUIRE_FALSE(gone.start(0).has_value());
    const std::string goneAt = "127.0.0.1:" + std::to_string(gone.port());
    gone.stop();

    Machine machine;
    machine.project.write("src/client/regions.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        task.spawn(function()
            -- The project's own relay, named by no argument.
            local near = NetworkService:PingRelayAsync()
            print(`near answered:{near ~= nil}`)
            if near then
                print(`near ping-sane:{near.Ping >= 0 and near.Ping < 500} matches:{near.Matches} relayed:{near.Relayed}`)
            end
        end)
        task.spawn(function()
            -- Asked at the same time, of nobody: nil, after its own wait.
            local far = NetworkService:PingRelayAsync(")" +
                                                         goneAt + R"(", 0.4)
            print(`far answered:{far ~= nil}`)
        end)
        print(`state:{NetworkService.State.Name}`)
    )");
    machine.boot(relayAt);
    for (int frame = 0; frame < 4000 && !(log.contains("near answered:") && log.contains("far answered:")); ++frame) {
        // Where the engine's frame hands a thread what a socket brought it.
        machine.host->publishNetworkResults();
        machine.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("near answered:true"));
    CHECK(log.contains("near ping-sane:true matches:0 relayed:0"));
    CHECK(log.contains("far answered:false"));
    // And it took no match to ask.
    CHECK(log.contains("state:Offline"));
    CHECK(machine.state().networkState == Offline);
    relay.stop();
}
