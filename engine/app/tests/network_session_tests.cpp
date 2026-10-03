// `NetworkService:Join`, `Host` and `Disconnect` from a script (ADR 0106), two
// hosts in one process over the memory transport -- the same session the
// engine runs over ENet.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "engine/app/network_session.h"
#include "engine/app/world_host.h"
#include "engine/app/world_ui.h"
#include "engine/net/memory_transport.h"
#include "engine/platform/file.h"
#include "engine/render/draw_poses.h"
#include "engine/render/transform_history.h"
#include "engine/replication/replication.h"
#include "engine/replication/script_templates.h"
#include "engine/scene/character_replay.h"
#include "engine/scene/components.h"
#include "engine/scene/physics_sync.h"
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

struct Machine
{
    Project project;
    std::unique_ptr<app::WorldHost> host = std::make_unique<app::WorldHost>();
    std::unique_ptr<app::NetworkSession> network;

    void boot(const std::shared_ptr<net::MemoryNetwork>& wire,
              scene::NetworkTopology topology = scene::NetworkTopology::Solo, std::string_view scene = {},
              core::u32 maxPlayers = 32)
    {
        app::WorldHostOptions options = bootOptions(project.root);
        options.networkTopology = topology;
        if (!scene.empty()) {
            options.bootScene = project.root / "content" / std::filesystem::path(scene);
            options.bootScenePath = std::string(scene);
            // The project's stamps, as the engine reads them: `Instance.stamp`
            // and a replica's own scripts both need them (ADR 0138 §6).
            options.bootStamps = [root = project.root](std::string_view stamp) -> std::optional<std::string> {
                std::string text;
                if (!platform::readTextFile(root / "content" / std::filesystem::path(stamp), text))
                    return std::nullopt;
                return text;
            };
        }
        REQUIRE_FALSE(host->boot(options).has_value());
        replication::Config base;
        base.ticksPerSnapshot = 1;
        base.interpolationDelayTicks = 0;
        base.maxPeers = maxPlayers;
        network = std::make_unique<app::NetworkSession>([this]() { return host.get(); }, base,
                                                        [wire]() { return net::createMemoryTransport(wire); });
        network->setJoinTimeout(0.5);
    }

    // The engine's own replication settings, over a transport that loses,
    // holds and reorders what it carries when `loss` says so.
    void bootOver(const std::shared_ptr<net::MemoryNetwork>& wire, const net::LossConfig* loss)
    {
        REQUIRE_FALSE(host->boot(bootOptions(project.root)).has_value());
        const std::optional<net::LossConfig> lossy =
            loss != nullptr ? std::optional<net::LossConfig>(*loss) : std::nullopt;
        network = std::make_unique<app::NetworkSession>(
            [this]() { return host.get(); }, replication::Config{},
            [wire, lossy]() {
                return lossy.has_value() ? net::createLossyTransport(net::createMemoryTransport(wire), *lossy)
                                         : net::createMemoryTransport(wire);
            });
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
    [[nodiscard]] scene::NetworkTopology topology() const { return host->world().engineState().networkTopology; }

    [[nodiscard]] core::usize serverCode() const
    {
        const scene::World& w = host->world();
        const core::InstanceId global = w.findFirstChildOfClass(
            host->runtime().dataModel(), w.classes().findId(w.atoms().lookup("GlobalScriptService")));
        const core::InstanceId server = w.findFirstChild(global, w.atoms().lookup("Server"));
        return server.valid() ? w.childCount(server) : 0;
    }
};

void run(Machine& a, Machine& b, int frames)
{
    for (int at = 0; at < frames; ++at) {
        a.frame();
        b.frame();
    }
}

[[nodiscard]] int occurrences(const Captured& log, std::string_view needle)
{
    int count = 0;
    for (const std::string& line : log.lines)
        count += line.find(needle) != std::string::npos ? 1 : 0;
    return count;
}

} // namespace

TEST_CASE("a solo game hosts, another joins it, and a server that goes puts the client back solo")
{
    Captured log;
    auto wire = net::createMemoryNetwork();

    Machine server;
    server.project.write("src/client/host.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService:Host(47101)
    )");
    server.boot(wire);

    Machine client;
    client.project.write("src/server/rules.luau", "print('client-rules-started')");
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Connected:Connect(function()
            print(`connected authority:{NetworkService.Authority} state:{NetworkService.State.Name}`)
        end)
        NetworkService.Disconnected:Connect(function(reason: string)
            print(`disconnected:{reason} authority:{NetworkService.Authority}`)
        end)
        NetworkService:Join("memory:47101")
    )");
    client.boot(wire);

    // Solo first: the client's rules ran, because solo is the authority.
    run(server, client, 2);
    CHECK(occurrences(log, "client-rules-started") == 1);
    CHECK(server.state() == Hosting);
    CHECK(server.topology() == scene::NetworkTopology::Host);

    run(server, client, 30);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(client.state() == Connected);
    CHECK(client.topology() == scene::NetworkTopology::Replica);
    CHECK(log.contains("connected authority:false state:Connected"));
    // Joining took this machine's server code away: it no longer decides.
    CHECK(client.serverCode() == 0);

    // The host leaves.
    server.host->world().engineState().pendingNetwork =
        scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Disconnect, {}, 0};
    run(server, client, 30);
    CHECK(server.state() == Offline);
    CHECK(client.state() == Offline);
    CHECK(client.topology() == scene::NetworkTopology::Solo);
    CHECK(log.contains("authority:true"));
    CHECK(log.contains("disconnected:"));
    // Solo again, so its server code started again, fresh.
    CHECK(client.serverCode() == 1);
    CHECK(occurrences(log, "client-rules-started") == 2);
}

TEST_CASE("back solo after a join, a machine runs what a solo boot of its scene runs (ADR 0137 §5)")
{
    // Only the file-mounted server code came back: a script inside a part the
    // join had replaced stayed gone, and the solo game was not the game.
    Captured log;
    auto wire = net::createMemoryNetwork();

    Machine server;
    server.project.write("src/client/host.luau", R"(
        game:GetService("NetworkService"):Host(47102)
    )");
    server.boot(wire);

    Machine client;
    client.project.write("content/scenes/main.scene.json",
                         R"json({"format":"scene","version":2,"root":{"children":[{"class":"Part","name":"Crate",)json"
                         R"json("children":[{"class":"Script","name":"Lid","properties":{"Source":)json"
                         R"json("print('crate-lid-started')"}}]}]}})json");
    client.project.write("src/client/join.luau", R"(
        game:GetService("NetworkService"):Join("memory:47102")
    )");
    client.boot(wire, scene::NetworkTopology::Solo, "scenes/main.scene.json");

    run(server, client, 2);
    CHECK(occurrences(log, "crate-lid-started") == 1);
    run(server, client, 30);
    REQUIRE(client.topology() == scene::NetworkTopology::Replica);

    server.host->world().engineState().pendingNetwork =
        scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Disconnect, {}, 0};
    run(server, client, 30);
    REQUIRE(client.topology() == scene::NetworkTopology::Solo);
    // The scene again, as a solo boot of it has it: its crate, and its script.
    CHECK(occurrences(log, "crate-lid-started") == 2);
    const scene::World& world = client.host->world();
    CHECK(world.findFirstChild(client.host->workspace(), world.atoms().lookup("Crate")).valid());
}

TEST_CASE("a joined client runs its own copy of a door's client and shared scripts, and none of its server code "
          "(ADR 0138 §6)")
{
    // Scripts never cross the wire. Before ADR 0138 a script inside a part the
    // authority replicates ran its file scope on the client, died with the
    // join's clear, and never came back; a stamp placed at run time brought
    // none of its code at all.
    Captured log;
    auto wire = net::createMemoryNetwork();
    const auto sided = [](std::string_view name, std::string_view side, std::string_view tag) {
        std::string node = R"json({"class":"Script","name":")json" + std::string(name) +
                           R"json(","properties":{"Source":"print(`)json" + std::string(tag) +
                           R"json(:{game:GetService('NetworkService').Authority}`)")json";
        if (!side.empty())
            node += R"json(,"RunContext":")json" + std::string(side) + "\"";
        return node + "}}";
    };
    const std::string scene = R"json({"format":"scene","version":2,"root":{"children":[)json"
                              R"json({"class":"Part","name":"Door","properties":{"Anchored":true},"children":[)json" +
                              sided("Creak", "Client", "door-client") + "," + sided("Rules", "Server", "door-server") +
                              "," + sided("Both", "", "door-both") + "]}]}}";
    const std::string lamp =
        R"json({"format":"scene","version":2,"root":{"class":"Model","name":"Lamp","children":[)json"
        R"json({"class":"Part","name":"Bulb","properties":{"Anchored":true},"children":[)json" +
        sided("Glow", "Client", "lamp-client") + "]}]}}";

    Machine server;
    server.project.write("content/scenes/main.scene.json", scene);
    server.project.write("content/stamps/lamp.stamp.json", lamp);
    // A stamp placed at run time, and a clone of it: each is the stamp again.
    server.project.write("src/client/host.luau", R"(
        game:GetService("NetworkService"):Host(47103)
        local placed = Instance.stamp("lamp")
        placed.Parent = workspace
        placed:Clone().Parent = workspace
    )");
    server.boot(wire, scene::NetworkTopology::Solo, "scenes/main.scene.json");

    Machine client;
    client.project.write("content/scenes/main.scene.json", scene);
    client.project.write("content/stamps/lamp.stamp.json", lamp);
    client.project.write("src/client/join.luau", R"(
        game:GetService("NetworkService"):Join("memory:47103")
    )");
    client.boot(wire, scene::NetworkTopology::Solo, "scenes/main.scene.json");

    run(server, client, 60);
    REQUIRE(client.topology() == scene::NetworkTopology::Replica);

    // The client's own door scripts, run again under the authority's door.
    CHECK(occurrences(log, "door-client:false") == 1);
    CHECK(occurrences(log, "door-both:false") == 1);
    CHECK(occurrences(log, "door-server:false") == 0);
    // Both lamps, each with the stamp's client script from the client's package.
    CHECK(occurrences(log, "lamp-client:false") == 2);
    // Each machine's solo boot, before the host and the join: once each, and
    // hosting did not start the authority's again.
    CHECK(occurrences(log, "door-server:true") == 2);
    CHECK(occurrences(log, "lamp-client:true") == 2);

    // Under the replicated door, in the client's world.
    const scene::World& world = client.host->world();
    const core::InstanceId door = world.findFirstChild(client.host->workspace(), world.atoms().lookup("Door"));
    REQUIRE(door.valid());
    CHECK(world.findFirstChild(door, world.atoms().lookup("Creak")).valid());
}

TEST_CASE("the audit's network run: two clients join, one leaves and comes back, and the server changes scene "
          "(script sides S3)")
{
    // Every script prints its tag and whether its machine is the authority.
    // Each machine is its own project with the same content, as packages are.
    Captured log;
    auto wire = net::createMemoryNetwork();
    const auto sided = [](std::string_view name, std::string_view side, std::string_view tag) {
        std::string node = R"json({"class":"Script","name":")json" + std::string(name) +
                           R"json(","properties":{"Source":"print(`)json" + std::string(tag) +
                           R"json(:{game:GetService('NetworkService').Authority}`)")json";
        if (!side.empty())
            node += R"json(,"RunContext":")json" + std::string(side) + "\"";
        return node + "}}";
    };
    const auto door = [&](std::string_view scene) {
        const std::string tag = std::string(scene) + "-door";
        return R"json({"format":"scene","version":2,"root":{"children":[)json"
               R"json({"class":"Part","name":"Door","properties":{"Anchored":true},"children":[)json" +
               sided("Creak", "Client", tag + "-client") + "," + sided("Rules", "Server", tag + "-server") + "," +
               sided("Both", "", tag + "-both") + "]}]}}";
    };
    const std::string lamp =
        R"json({"format":"scene","version":2,"root":{"class":"Model","name":"Lamp","children":[)json"
        R"json({"class":"Part","name":"Bulb","properties":{"Anchored":true},"children":[)json" +
        sided("Glow", "Client", "lamp-client") + "]}]}}";
    const auto content = [&](Machine& machine) {
        machine.project.write("content/scenes/a.scene.json", door("a"));
        machine.project.write("content/scenes/b.scene.json", door("b"));
        machine.project.write("content/stamps/lamp.stamp.json", lamp);
    };

    Machine server;
    content(server);
    server.project.write("src/server/host.luau", R"(
        game:GetService("NetworkService"):Host(47104)
        local placed = Instance.stamp("lamp")
        placed.Parent = workspace
        placed:Clone().Parent = workspace
    )");
    server.boot(wire, scene::NetworkTopology::Solo, "scenes/a.scene.json");

    Machine first;
    Machine second;
    for (Machine* client : {&first, &second}) {
        content(*client);
        client->project.write("src/client/join.luau", R"(game:GetService("NetworkService"):Join("memory:47104"))");
        client->boot(wire, scene::NetworkTopology::Solo, "scenes/a.scene.json");
    }
    const auto frames = [&](int count) {
        for (int at = 0; at < count; ++at) {
            server.frame();
            first.frame();
            second.frame();
        }
    };

    frames(90);
    REQUIRE(first.topology() == scene::NetworkTopology::Replica);
    REQUIRE(second.topology() == scene::NetworkTopology::Replica);
    // Each client ran its own copy of the door's client and shared scripts and
    // the lamps' client script -- the server's rules nowhere but the server
    // and the solo boots before the join.
    CHECK(occurrences(log, "a-door-client:false") == 2);
    CHECK(occurrences(log, "a-door-both:false") == 2);
    CHECK(occurrences(log, "a-door-server:false") == 0);
    CHECK(occurrences(log, "lamp-client:false") == 4);
    CHECK(occurrences(log, "a-door-server:true") == 3);

    // The second leaves: solo again, its own scene read again.
    second.host->world().engineState().pendingNetwork =
        scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Disconnect, {}, 0};
    frames(60);
    REQUIRE(second.topology() == scene::NetworkTopology::Solo);
    REQUIRE(first.topology() == scene::NetworkTopology::Replica);
    CHECK(occurrences(log, "a-door-server:true") == 4);

    // And comes back: its own copy again, under the server's door.
    second.host->world().engineState().pendingNetwork =
        scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Join, "memory:47104", 0};
    frames(90);
    REQUIRE(second.topology() == scene::NetworkTopology::Replica);
    CHECK(occurrences(log, "a-door-client:false") == 3);
    CHECK(occurrences(log, "a-door-server:false") == 0);
    CHECK(occurrences(log, "lamp-client:false") == 6);

    // The server changes scene; both clients follow and run scene b's door.
    server.host->world().engineState().pendingNetwork = std::nullopt;
    REQUIRE_FALSE(server.host->loadScene("scenes/b.scene.json", {}).has_value());
    frames(90);
    CHECK(occurrences(log, "b-door-server:true") == 1);
    CHECK(occurrences(log, "b-door-client:false") == 2);
    CHECK(occurrences(log, "b-door-both:false") == 2);
    CHECK(occurrences(log, "b-door-server:false") == 0);
}

TEST_CASE("a server naming stamps a client does not have grows nothing on the client (script sides S3)")
{
    // A spawn's origin names a stamp, and a name a client interned is kept for
    // ever. One the client's package does not hold must be refused, or a
    // hostile server grows every client's name table with each spawn.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/host.luau", R"(game:GetService("NetworkService"):Host(47105))");
    server.boot(wire);
    Machine client;
    client.project.write("src/client/join.luau", R"(game:GetService("NetworkService"):Join("memory:47105"))");
    client.boot(wire, scene::NetworkTopology::Solo, "scenes/none.scene.json");
    run(server, client, 60);
    REQUIRE(client.topology() == scene::NetworkTopology::Replica);

    scene::World& world = server.host->world();
    const core::usize before = client.host->atoms().size();
    for (int index = 0; index < 200; ++index) {
        const core::InstanceId part = world.create(world.classes().findId(world.atoms().lookup("Part")));
        world.setOrigin(part, scene::World::Origin{world.atoms().intern("stamp:made-up-" + std::to_string(index)), 0});
        REQUIRE_FALSE(world.setParent(part, server.host->workspace()).has_value());
    }
    run(server, client, 30);
    const core::InstanceId workspace = client.host->workspace();
    REQUIRE(client.host->world().childCount(workspace) >= 200);
    CHECK(client.host->atoms().size() - before < 10);
}

TEST_CASE("a server naming stamps a client does not have costs the client a bounded number of reads (script sides S3)")
{
    Project project;
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    int reads = 0;
    replication::ScriptTemplates templates;
    templates.setStampSource([&reads](std::string_view) -> std::optional<std::string> {
        ++reads;
        return std::nullopt;
    });
    for (int index = 0; index < 200; ++index)
        CHECK_FALSE(templates.stampAsset(host.world(), "stamp:made-up-" + std::to_string(index % 150)).valid());
    CHECK(reads <= 64);
}

namespace {

// Enum.NetworkState.Connecting.
constexpr core::i32 Connecting = 1;
// N13: dialling a server that went, as against dialling one for the first time.
constexpr core::i32 Reconnecting = 5;

// A scene with one part, so a machine has a scene and its scene's scripts.
const char* const kSceneOfOne = R"json({"format":"scene","version":2,"root":{"children":[)json"
                                R"json({"class":"Part","name":"Floor","properties":{"Anchored":true}}]}})json";

// A dedicated server on `port` whose code makes one part a join can see.
void serve(Machine& server, const std::shared_ptr<net::MemoryNetwork>& wire, core::u16 port, std::string_view made)
{
    server.project.write("content/scenes/main.scene.json", kSceneOfOne);
    server.project.write("src/server/world.luau", "local part = Instance.new('Part')\npart.Name = '" +
                                                      std::string(made) +
                                                      "'\npart.Anchored = true\npart.Parent = workspace\n");
    server.boot(wire, scene::NetworkTopology::Dedicated, "scenes/main.scene.json");
    REQUIRE_FALSE(server.network->start(replication::Topology::Dedicated, {}, port).has_value());
}

// A client started to join `port`, as `--join` starts one: a script that lives
// across worlds, and one that is the scene's.
void joining(Machine& client, const std::shared_ptr<net::MemoryNetwork>& wire, core::u16 port)
{
    client.project.write("content/scenes/main.scene.json", kSceneOfOne);
    client.project.write("src/client/across.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        print(`across-boot state:{NetworkService.State.Name}`)
        NetworkService.Connected:Connect(function()
            print(`across-connected state:{NetworkService.State.Name}`)
        end)
        NetworkService.Disconnected:Connect(function()
            print(`across-disconnected state:{NetworkService.State.Name} authority:{NetworkService.Authority}`)
        end)
    )");
    client.project.write("src/scenes/main/client/hud.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        local player = NetworkService.LocalPlayer
        local names = {}
        for _, child in workspace:GetChildren() do
            table.insert(names, child.Name)
        end
        table.sort(names)
        print(`scene-start state:{NetworkService.State.Name} user:{if player then player.UserId else -1} world:{table.concat(names, ",")}`)
    )");
    client.boot(wire, scene::NetworkTopology::Replica, "scenes/main.scene.json");
    REQUIRE_FALSE(client.network->start(replication::Topology::Replica, "memory", port).has_value());
}

} // namespace

TEST_CASE("D433: started to join, a scene's client code starts once, in the server's world")
{
    // It started BEFORE the connection: `State` read `Offline`, `UserId` was 0
    // and later the real number, `Workspace` was empty, and what it parented
    // there was destroyed at connect. Alone and "about to join" looked the same.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    serve(server, wire, 47310, "MadeByTheServer");
    Machine client;
    joining(client, wire, 47310);

    // The server has not answered yet. What lives across worlds runs, and
    // says so; the scene's code waits.
    for (int at = 0; at < 20; ++at)
        client.frame();
    CHECK(client.state() == Connecting);
    CHECK(log.contains("across-boot state:Connecting"));
    CHECK(occurrences(log, "scene-start") == 0);

    run(server, client, 80);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(client.state() == Connected);
    // Once, joined, as the player the server made it, with the server's world
    // under it.
    CHECK(occurrences(log, "scene-start") == 1);
    CHECK(log.contains("scene-start state:Connected user:2"));
    CHECK(log.contains("across-connected state:Connected"));
}

TEST_CASE("D432: a server that goes is Connecting, then Disconnected and solo after the timeout")
{
    // `State` stayed `Connected` for ever, no signal fired, and the world
    // stood still: a game could tell its player nothing.
    Captured log;
    auto wire = net::createMemoryNetwork();
    auto server = std::make_unique<Machine>();
    serve(*server, wire, 47311, "MadeByTheServer");
    Machine client;
    joining(client, wire, 47311);
    core::u64 now = 1;
    client.network->setClock([&now] { return now; });
    client.network->setJoinTimeout(10.0);
    run(*server, client, 80);
    REQUIRE(client.state() == Connected);
    REQUIRE(occurrences(log, "scene-start") == 1);

    // The server is gone.
    server.reset();
    for (int at = 0; at < 200 && client.state() == Connected; ++at)
        client.frame();
    CHECK(client.state() == Reconnecting);
    CHECK(client.topology() == scene::NetworkTopology::Replica);
    CHECK(occurrences(log, "across-disconnected") == 0);

    // Inside the timeout it goes on dialling.
    now += 5'000'000'000ull;
    for (int at = 0; at < 30; ++at)
        client.frame();
    CHECK(client.state() == Reconnecting);

    // Past it: given up. `Disconnected`, and this machine decides its own
    // world again -- with its scene's client code started in it.
    now += 6'000'000'000ull;
    for (int at = 0; at < 30; ++at)
        client.frame();
    CHECK(client.state() == Offline);
    CHECK(client.topology() == scene::NetworkTopology::Solo);
    CHECK(log.contains("across-disconnected state:Offline authority:true"));
    CHECK(occurrences(log, "scene-start") == 2);
    CHECK(log.contains("scene-start state:Offline"));
}

TEST_CASE("D432: a server that restarts is a fresh join -- a new world, and Connected again")
{
    // The replica kept the world of the session that died, and stayed frozen.
    Captured log;
    auto wire = net::createMemoryNetwork();
    auto first = std::make_unique<Machine>();
    serve(*first, wire, 47312, "FromTheFirst");
    Machine client;
    joining(client, wire, 47312);
    core::u64 now = 1;
    client.network->setClock([&now] { return now; });
    client.network->setJoinTimeout(60.0);
    run(*first, client, 80);
    REQUIRE(client.state() == Connected);
    REQUIRE(log.contains("world:Floor,FromTheFirst"));

    first.reset();
    for (int at = 0; at < 200 && client.state() == Connected; ++at)
        client.frame();
    REQUIRE(client.state() == Reconnecting);

    // Another server on the same port, which has never heard of this player.
    Machine second;
    serve(second, wire, 47312, "FromTheSecond");
    for (int at = 0; at < 600 && client.state() != Connected; ++at) {
        now += 100'000'000ull;
        second.frame();
        client.frame();
    }
    run(second, client, 40);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(client.state() == Connected);
    CHECK(occurrences(log, "across-connected") == 2);
    // The scene's code started again, in the second server's world: nothing
    // of the first is left under it.
    CHECK(occurrences(log, "scene-start") == 2);
    CHECK(log.contains("world:Floor,FromTheSecond"));
    const scene::World& world = client.host->world();
    CHECK_FALSE(world.findFirstChild(client.host->workspace(), world.atoms().lookup("FromTheFirst")).valid());
    CHECK(world.findFirstChild(client.host->workspace(), world.atoms().lookup("FromTheSecond")).valid());
}

TEST_CASE("D433: a script's own join starts the scene's client code again, in the server's world")
{
    // The manual said a scene's scripts start after the change. They did not:
    // a line at the top of one printed once, and every reference it held was
    // to the world the join threw away.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    serve(server, wire, 47313, "MadeByTheServer");

    Machine client;
    client.project.write("content/scenes/main.scene.json", kSceneOfOne);
    client.project.write("src/client/menu.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService:Join("memory:47313")
    )");
    client.project.write("src/scenes/main/client/hud.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        print(`scene-start state:{NetworkService.State.Name} made:{workspace:FindFirstChild("MadeByTheServer") ~= nil}`)
    )");
    client.boot(wire, scene::NetworkTopology::Solo, "scenes/main.scene.json");
    run(server, client, 80);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(client.state() == Connected);
    // Once alone, at boot; once more, joined.
    CHECK(log.contains("scene-start state:Offline made:false"));
    CHECK(log.contains("scene-start state:Connected"));
    CHECK(occurrences(log, "scene-start") == 2);
}

TEST_CASE("a join nothing answers is JoinFailed, and the game stays solo")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.JoinFailed:Connect(function(reason: string)
            print(`join-failed:{reason ~= ""} state:{NetworkService.State.Name}`)
        end)
        NetworkService:Join("memory:47999")
    )");
    client.boot(wire);
    for (int at = 0; at < 60; ++at)
        client.frame();
    CHECK(log.contains("join-failed:true state:Offline"));
    CHECK(client.topology() == scene::NetworkTopology::Solo);
    CHECK_FALSE(client.network->active());
}

TEST_CASE("N1: a join nothing answers leaves the game exactly as it was")
{
    // A menu that offers "join by address": its parts, the address typed into
    // it and the handler for `JoinFailed` were cleared away by the join, and
    // the failure then reloaded the scene under them.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine client;
    client.project.write("src/server/rules.luau", "print('client-rules-started')");
    client.project.write("src/client/menu.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        local marker = Instance.new("Part")
        marker.Name = "MenuMarker"
        marker.Parent = workspace
        NetworkService.JoinFailed:Connect(function(reason: string)
            print(`join-failed marker:{workspace:FindFirstChild("MenuMarker") ~= nil} state:{NetworkService.State.Name}`)
        end)
        NetworkService:Join("memory:47998")
    )");
    client.boot(wire);
    for (int at = 0; at < 60; ++at)
        client.frame();
    CHECK(log.contains("join-failed marker:true state:Offline"));
    // Its own server code ran once, and was never stopped and started again.
    CHECK(occurrences(log, "client-rules-started") == 1);
    CHECK(client.serverCode() == 1);
    CHECK(client.topology() == scene::NetworkTopology::Solo);
}

TEST_CASE("NA8: a full server says so, and the player who asked hears why")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/client/host.luau", R"(game:GetService("NetworkService"):Host(47102))");
    server.boot(wire, scene::NetworkTopology::Solo, {}, 1);
    Machine first;
    first.project.write("src/client/join.luau", R"(game:GetService("NetworkService"):Join("memory:47102"))");
    first.boot(wire);
    Machine second;
    second.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.JoinFailed:Connect(function(reason: string)
            print(`second-refused:{reason}`)
        end)
        task.wait(0.5)
        NetworkService:Join("memory:47102")
    )");
    second.boot(wire);
    for (int at = 0; at < 90; ++at) {
        server.frame();
        first.frame();
        second.frame();
    }
    CHECK(first.state() == Connected);
    CHECK(second.state() == Offline);
    CHECK(log.contains("second-refused:"));
    CHECK(log.contains("full"));
}

TEST_CASE("NA7: a host that cannot open its port says so to the game")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine first;
    first.project.write("src/client/host.luau", R"(game:GetService("NetworkService"):Host(47103))");
    first.boot(wire);
    Machine second;
    second.project.write("src/client/host.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.HostFailed:Connect(function(reason: string)
            -- The words a game shows its player, with no catalog key in front
            -- of them (D485), as `JoinFailed`'s are.
            print(`host-failed:{reason ~= ""} state:{NetworkService.State.Name} keyed:{string.sub(reason, 1, 1) == "["}`)
        end)
        task.wait(0.2)
        NetworkService:Host(47103)
    )");
    second.boot(wire);
    for (int at = 0; at < 40; ++at) {
        first.frame();
        second.frame();
    }
    CHECK(first.state() == Hosting);
    CHECK(log.contains("host-failed:true state:Offline keyed:false"));
}

TEST_CASE("N2: a client whose server goes is player 1 again, in its own world")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/client/host.luau", R"(game:GetService("NetworkService"):Host(47104))");
    server.boot(wire);
    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Connected:Connect(function()
            print(`joined-as:{(NetworkService.LocalPlayer :: Player).UserId}`)
        end)
        NetworkService.Disconnected:Connect(function()
            print(`solo-as:{(NetworkService.LocalPlayer :: Player).UserId}`)
        end)
        NetworkService:Join("memory:47104")
    )");
    client.boot(wire);
    run(server, client, 30);
    REQUIRE(client.state() == Connected);
    CHECK(log.contains("joined-as:2"));
    server.host->world().engineState().pendingNetwork =
        scene::EngineState::NetworkRequest{scene::EngineState::NetworkRequest::Kind::Disconnect, {}, 0};
    run(server, client, 30);
    CHECK(client.state() == Offline);
    CHECK(log.contains("solo-as:1"));
}

TEST_CASE("NA17: a part a client's own script makes falls on that client")
{
    // Debris, a shell, a projectile only this machine sees: every loose body
    // on a replica was treated as the authority's to move, and one the
    // authority never heard of hung in the air.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/client/host.luau", R"(game:GetService("NetworkService"):Host(47105))");
    server.boot(wire);
    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Connected:Connect(function()
            local debris = Instance.new("Part")
            debris.Size = Vector3.new(1, 1, 1)
            debris.Position = Vector3.new(0, 50, 0)
            debris.Parent = workspace
            task.wait(1)
            print(`debris-fell:{debris.Position.y < 45}`)
        end)
        NetworkService:Join("memory:47105")
    )");
    client.boot(wire);
    run(server, client, 120);
    REQUIRE(client.state() == Connected);
    CHECK(log.contains("debris-fell:true"));
}

TEST_CASE("a join waits by the clock, not by frames, and one from the command line keeps dialling (audit A3)")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    // Something listening that never welcomes: a server still starting.
    auto silent = net::createMemoryTransport(wire);
    REQUIRE_FALSE(silent->open(net::TransportConfig{.port = 47990, .maxPeers = 4, .channels = 4}).has_value());

    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.JoinFailed:Connect(function()
            print("join-failed")
        end)
        NetworkService:Join("memory:47990")
    )");
    client.boot(wire);
    core::u64 now = 0;
    client.network->setClock([&now] { return now; });
    client.network->setJoinTimeout(10.0);
    // A thousand frames in no time at all -- a minimised window's: counted in
    // frames, the ten seconds were over after 600, in two milliseconds.
    for (int at = 0; at < 1000; ++at)
        client.frame();
    CHECK_FALSE(log.contains("join-failed"));
    now += 11'000'000'000ull;
    // One frame to give up, and the next drains the deferred `JoinFailed`.
    client.frame();
    client.frame();
    CHECK(log.contains("join-failed"));
    CHECK(client.topology() == scene::NetworkTopology::Solo);

    // From the command line it dials until the server answers.
    Machine dialler;
    dialler.boot(wire, scene::NetworkTopology::Replica);
    core::u64 later = 0;
    dialler.network->setClock([&later] { return later; });
    dialler.network->setJoinTimeout(10.0);
    REQUIRE_FALSE(dialler.network->start(replication::Topology::Replica, "memory", 47990).has_value());
    for (int at = 0; at < 10; ++at) {
        later += 5'000'000'000ull;
        dialler.frame();
    }
    CHECK(dialler.topology() == scene::NetworkTopology::Replica);
}

TEST_CASE("a script reads how the connection is doing (multiplayer smoothness)")
{
    Captured log;
    auto wire = net::createMemoryNetwork();

    Machine server;
    server.project.write("src/client/host.luau", R"(
        game:GetService("NetworkService"):Host(47102)
    )");
    server.boot(wire);

    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService:Join("memory:47102")
        game:GetService("RunService").Heartbeat:Connect(function()
            local stats = NetworkService:GetStats()
            if stats.SnapshotsPerSecond > 0 then
                print(`stats:{typeof(stats.Ping)} {stats.CorrectionsPerSecond} {stats.InputBufferDepth >= 0}`)
            end
        end)
    )");
    client.boot(wire);
    // A second of the match by the session's clock, a tick at a time.
    core::u64 now = 1;
    client.network->setClock([&now] { return now; });
    for (int at = 0; at < 90; ++at) {
        now += 16'666'667ull;
        run(server, client, 1);
    }
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(client.state() == Connected);
    CHECK(log.contains("stats:number 0 true"));
}

TEST_CASE("a dedicated server cannot join, host or disconnect")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/try.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        local joined = pcall(function() NetworkService:Join("memory:1") end)
        local hosted = pcall(function() NetworkService:Host(1) end)
        local left = pcall(function() NetworkService:Disconnect() end)
        print(`dedicated join:{joined} host:{hosted} leave:{left}`)
    )");
    server.boot(wire, scene::NetworkTopology::Dedicated);
    server.frame();
    CHECK(log.contains("dedicated join:false host:false leave:false"));
}

TEST_CASE("a script that joins again after leaving is welcomed back as the same player")
{
    // **D207.** The command line's redial kept the player's token; a script's
    // `Join` began a new session with none, and the server made a stranger of
    // somebody it had just seen -- a new `UserId`, and a game that keyed a score
    // on it lost the score.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/client/host.luau", R"(
        game:GetService("NetworkService"):Host(47102)
    )");
    server.boot(wire);

    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        local joins = 0
        NetworkService.Connected:Connect(function()
            joins += 1
            print(`joined:{joins} user:{NetworkService.LocalPlayer.UserId}`)
            if joins == 1 then
                NetworkService:Disconnect()
            end
        end)
        NetworkService.Disconnected:Connect(function()
            if joins == 1 then
                NetworkService:Join("memory:47102")
            end
        end)
        NetworkService:Join("memory:47102")
    )");
    client.boot(wire);

    run(server, client, 90);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("joined:1 user:2"));
    CHECK(log.contains("joined:2 user:2"));
    CHECK_FALSE(log.contains("joined:2 user:3"));
}

// --- Stage 2 (ADR 0133): what a jump onto a corner costs ------------------------------

namespace {

// The engine's own adapter, as `engine.cpp` builds it: a replica's corrections
// replay through the host's physics.
class HostReplay final : public scene::ICharacterReplay
{
public:
    explicit HostReplay(app::WorldHost& host) noexcept : m_host(host) {}

    [[nodiscard]] std::optional<scene::CharacterCommand> lastCommand(core::InstanceId character) const override
    {
        const scene::PhysicsSync* physics = m_host.physics();
        return physics != nullptr ? physics->lastCommand(character) : std::nullopt;
    }

    [[nodiscard]] std::vector<core::CFrameD> replay(core::InstanceId character,
                                                    const scene::CharacterReplayStart& start,
                                                    std::span<const scene::CharacterCommand> commands) override
    {
        scene::PhysicsSync* physics = m_host.physics();
        return physics != nullptr ? physics->replay(character, start, commands) : std::vector<core::CFrameD>{};
    }

    void remember(core::u64 tick) override
    {
        // Timed: what keeping the island costs every tick, for the baseline.
        const auto began = std::chrono::steady_clock::now();
        if (scene::PhysicsSync* physics = m_host.physics(); physics != nullptr)
            physics->remember(tick);
        remembers += 1;
        rememberMicros += static_cast<core::u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
    }

    [[nodiscard]] std::optional<core::CFrameD> remembered(core::u64 tick, core::InstanceId id) const override
    {
        const scene::PhysicsSync* physics = m_host.physics();
        return physics != nullptr ? physics->remembered(tick, id) : std::nullopt;
    }

    core::u64 remembers = 0;
    core::u64 rememberMicros = 0;

private:
    app::WorldHost& m_host;
};

// Both ends' game: a floor, a block 1.5 m tall, and every player's body walked
// and jumped by their intents -- on the authority for all, on a replica for its
// own (the owner's test game's `Movement.drive`).
constexpr std::string_view CornerShared = R"(
local Corner = {}
function Corner.drive(player: Player, body: CharacterBody)
    local move = player:GetIntent("Move")
    if typeof(move) == "Vector2" then
        local direction = vector.create(move.X, 0, -move.Y)
        if vector.magnitude(direction) > 0.01 then
            body:Move(vector.normalize(direction))
        end
    end
    if player:GetIntent("Jump") == true and body.Grounded then
        body:Jump()
    end
end
return Corner
)";

constexpr std::string_view CornerServer = R"(
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Corner = require("@shared/corner")
NetworkService:Host(47120)
local function block(size: vector, position: vector)
    local part = Instance.new("Part")
    part.Anchored = true
    part.Size = size
    part.Position = position
    part.Parent = workspace
end
block(vector.create(80, 1, 80), vector.create(0, -0.5, 0))
block(vector.create(12, 1.5, 12), vector.create(6, 0.75, 0))
local bodies: { [Player]: CharacterBody } = {}
RunService.Heartbeat:Connect(function()
    for _, player in NetworkService:GetPlayers() do
        if player.UserId ~= 2 then
            continue
        end
        local body = bodies[player]
        if not body then
            body = Instance.new("CharacterBody")
            body.Size = vector.create(2, 4, 2)
            body.Position = vector.create(-6, 3, 0)
            body.WalkSpeed = 8
            body.JumpSpeed = 6
            body.Parent = workspace
            bodies[player] = body
            player.Character = body
        end
        Corner.drive(player, body :: CharacterBody)
    end
end)
)";

constexpr std::string_view CornerClient = R"(
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local InputService = game:GetService("InputService")
local Corner = require("@shared/corner")
local context = Instance.new("InputContext")
context.Parent = workspace
local move = Instance.new("InputAction")
move.Name = "Move"
move.Type = Enum.InputActionType.Direction2D
move.Parent = context
local stick = Instance.new("InputBinding")
stick.KeyCode = Enum.KeyCode.VirtualStick1
stick.Parent = move
local jump = Instance.new("InputAction")
jump.Name = "Jump"
jump.Parent = context
local key = Instance.new("InputBinding")
key.KeyCode = Enum.KeyCode.Virtual3
key.Parent = jump
NetworkService:Join("memory:47120")

-- A hundred runs at the block, each jumping from a little further along, so
-- the landings fall at every offset across its edge.
local attempt = 0
local phase = "back"
local held = 0
RunService.Heartbeat:Connect(function()
    local me = NetworkService.LocalPlayer
    local body = if me then me.Character else nil
    if NetworkService.State ~= Enum.NetworkState.Connected or not me or not body then
        return
    end
    local x = body.Position.X
    local run, press = 0, 0
    if phase == "back" then
        run = -1
        if x < -6 then
            phase = "run"
            attempt += 1
            workspace:SetAttribute("Attempt", attempt)
        end
    elseif phase == "run" then
        run = 1
        -- Takeoff between 3.5 m and 0.3 m before the edge -- the capsule's
        -- front is a metre ahead of its centre -- so the feet meet the edge
        -- rising, at the top, and falling.
        if x >= -1.3 - 3.2 * ((attempt * 37) % 100) / 100 then
            press = 1
            phase = "air"
            held = 0
        end
    elseif phase == "air" then
        run = 1
        held += 1
        if held > 60 then
            phase = "back"
        end
    end
    workspace:SetAttribute("Phase", phase)
    InputService:SetVirtualState(Enum.KeyCode.Virtual1, run)
    InputService:SetVirtualState(Enum.KeyCode.Virtual2, 0)
    InputService:SetVirtualState(Enum.KeyCode.Virtual3, press)
    Corner.drive(me, body :: CharacterBody)
    if attempt > 100 then
        print("corner-done")
    end
end)
)";

} // namespace

namespace {

// What a hundred corner jumps cost: every correction, and those past a
// centimetre once the jumps begin -- the join's own take is not a corner's.
struct CornerRun
{
    int corrections = 0;
    int overDuringJumps = 0;
    double largest = 0.0;
};

[[nodiscard]] CornerRun measureCorner(bool lossy, bool withReplay)
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    net::LossConfig loss;
    loss.seed = 5;
    loss.dropPerMille = 20;
    loss.reorderPerMille = 50;
    loss.jitterPolls = 2;

    Machine server;
    server.project.write(".luaurc", R"({"aliases": {"shared": "src/shared"}})");
    server.project.write("src/shared/corner.luau", std::string(CornerShared));
    server.project.write("src/server/init.luau", std::string(CornerServer));
    server.bootOver(wire, lossy ? &loss : nullptr);
    Machine client;
    client.project.write(".luaurc", R"({"aliases": {"shared": "src/shared"}})");
    client.project.write("src/shared/corner.luau", std::string(CornerShared));
    client.project.write("src/client/init.luau", std::string(CornerClient));
    client.bootOver(wire, lossy ? &loss : nullptr);
    HostReplay replay(*client.host);
    if (withReplay)
        client.network->setCharacterReplay(&replay);

    core::u64 seen = 0;
    std::map<std::string, std::pair<int, double>> byPhase;
    double largest = 0.0;
    int over = 0;
    int overDuringJumps = 0;
    for (int frame = 0; frame < 30000 && !log.contains("corner-done"); ++frame) {
        server.frame();
        client.frame();
        const replication::IReplication* replica = client.network->replication();
        if (replica == nullptr)
            continue;
        const replication::Stats stats = replica->stats();
        if (stats.corrections != seen) {
            seen = stats.corrections;
            const scene::Value phase = client.host->world().getAttribute(client.host->workspace(),
                                                                         client.host->world().atoms().lookup("Phase"));
            const std::string name = std::holds_alternative<std::string>(phase) ? std::get<std::string>(phase) : "?";
            byPhase[name].first += 1;
            const scene::Value tried = client.host->world().getAttribute(
                client.host->workspace(), client.host->world().atoms().lookup("Attempt"));
            MESSAGE("MEASURE correction attempt="
                    << (std::holds_alternative<double>(tried) ? std::get<double>(tried) : -1.0) << " phase=" << name
                    << " size=" << stats.lastCorrectionMetres << " starved=" << stats.intentStarvations);
            byPhase[name].second = std::max(byPhase[name].second, stats.lastCorrectionMetres);
            largest = std::max(largest, stats.lastCorrectionMetres);
            if (stats.lastCorrectionMetres > 0.01)
                ++over;
            if (stats.lastCorrectionMetres > 0.01 && std::holds_alternative<double>(tried) &&
                std::get<double>(tried) >= 2.0)
                ++overDuringJumps;
        }
    }
    CHECK(log.contains("corner-done"));
    MESSAGE("MEASURE lossy=" << lossy << " replay=" << withReplay << " corrections=" << seen << " over1cm=" << over
                             << " largest=" << largest);
    for (const auto& [phase, count] : byPhase)
        MESSAGE("MEASURE phase " << phase << ": " << count.first << " corrections, largest " << count.second);
    return CornerRun{static_cast<int>(seen), overDuringJumps, largest};
}

} // namespace

TEST_CASE("a hundred jumps onto a block's corner over jitter and loss are not corrected (ADR 0133)")
{
    // Over a lossy, jittering link: the one take at join aside, no jump is
    // corrected past a centimetre.
    const CornerRun lossy = measureCorner(true, true);
    CHECK(lossy.overDuringJumps == 0);
    // And over a clean one, nothing at all.
    const CornerRun clean = measureCorner(false, true);
    CHECK(clean.corrections == 0);
}

// The same game with three loose crates in the walk: the client runs at them
// and back for ten seconds, pushing them into each other.
constexpr std::string_view CrateServer = R"(
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Corner = require("@shared/corner")
NetworkService:Host(47121)
local function part(name: string, size: vector, position: vector, anchored: boolean)
    local made = Instance.new("Part")
    made.Name = name
    made.Anchored = anchored
    made.Size = size
    made.Position = position
    made.Parent = workspace
end
part("Floor", vector.create(80, 1, 80), vector.create(0, -0.5, 0), true)
part("Crate1", vector.create(2, 2, 2), vector.create(-1, 1, 0), false)
part("Crate2", vector.create(2, 2, 2), vector.create(2, 1, 0.6), false)
part("Crate3", vector.create(2, 2, 2), vector.create(5, 1, -0.8), false)
local bodies: { [Player]: CharacterBody } = {}
RunService.Heartbeat:Connect(function()
    for _, player in NetworkService:GetPlayers() do
        if player.UserId ~= 2 then
            continue
        end
        local body = bodies[player]
        if not body then
            body = Instance.new("CharacterBody")
            body.Name = "Hero"
            body.Size = vector.create(2, 4, 2)
            body.Position = vector.create(-6, 3, 0)
            body.WalkSpeed = 8
            body.JumpSpeed = 6
            body.Parent = workspace
            bodies[player] = body
            player.Character = body
        end
        Corner.drive(player, body :: CharacterBody)
    end
end)
)";

constexpr std::string_view CrateClient = R"(
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local InputService = game:GetService("InputService")
local Corner = require("@shared/corner")
local context = Instance.new("InputContext")
context.Parent = workspace
local move = Instance.new("InputAction")
move.Name = "Move"
move.Type = Enum.InputActionType.Direction2D
move.Parent = context
local stick = Instance.new("InputBinding")
stick.KeyCode = Enum.KeyCode.VirtualStick1
stick.Parent = move
NetworkService:Join("memory:47121")

-- A second to settle, then ten seconds at the crates: a second and a half
-- pushing, a second back, a little sideways each time so they turn.
local ticks = 0
RunService.Heartbeat:Connect(function()
    local me = NetworkService.LocalPlayer
    local body = if me then me.Character else nil
    if NetworkService.State ~= Enum.NetworkState.Connected or not me or not body then
        return
    end
    ticks += 1
    local run, side = 0, 0
    if ticks > 60 then
        local at = (ticks - 60) % 150
        if at < 90 then
            run = 1
            side = if (ticks // 150) % 2 == 0 then 0.25 else -0.25
        else
            run = -1
        end
        workspace:SetAttribute("Pushing", true)
    end
    InputService:SetVirtualState(Enum.KeyCode.Virtual1, run)
    InputService:SetVirtualState(Enum.KeyCode.Virtual2, side)
    Corner.drive(me, body :: CharacterBody)
    if ticks > 660 then
        print("crates-done")
    end
end)
)";

namespace {

// How far the segment `from`..`to` -- a capsule's axis -- comes to a box.
[[nodiscard]] double segmentToBox(const core::DVec3& from, const core::DVec3& to, const core::CFrameD& box,
                                  const core::Vec3& size)
{
    const core::Mat3 back = core::transpose(box.rotation);
    const core::Vec3 half = size * 0.5f;
    double nearest = 1e9;
    for (int step = 0; step <= 8; ++step) {
        const double t = step / 8.0;
        const core::DVec3 point{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                                from.z + (to.z - from.z) * t};
        const core::DVec3 offset = point - box.position;
        const core::Vec3 local = back * core::Vec3{static_cast<core::f32>(offset.x), static_cast<core::f32>(offset.y),
                                                   static_cast<core::f32>(offset.z)};
        const core::Vec3 clamped{std::clamp(local.x, -half.x, half.x), std::clamp(local.y, -half.y, half.y),
                                 std::clamp(local.z, -half.z, half.z)};
        const core::Vec3 apart{local.x - clamped.x, local.y - clamped.y, local.z - clamped.z};
        nearest = std::min(nearest,
                           std::sqrt(static_cast<double>(apart.x * apart.x + apart.y * apart.y + apart.z * apart.z)));
    }
    return nearest;
}

struct CrateRun
{
    int overDuringPush = 0;
    double largest = 0.0;
    double deepest = 0.0;
    double nearest = 1e9;
    int looked = 0;
    core::u32 mostPredicted = 0;
    replication::Stats last;
};

[[nodiscard]] CrateRun measureCrates(bool lossy, core::u32 seed)
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    net::LossConfig loss;
    loss.seed = seed;
    loss.dropPerMille = 20;
    loss.reorderPerMille = 50;
    loss.jitterPolls = 2;

    Machine server;
    server.project.write(".luaurc", R"({"aliases": {"shared": "src/shared"}})");
    server.project.write("src/shared/corner.luau", std::string(CornerShared));
    server.project.write("src/server/init.luau", std::string(CrateServer));
    server.bootOver(wire, lossy ? &loss : nullptr);
    Machine client;
    client.project.write(".luaurc", R"({"aliases": {"shared": "src/shared"}})");
    client.project.write("src/shared/corner.luau", std::string(CornerShared));
    client.project.write("src/client/init.luau", std::string(CrateClient));
    client.bootOver(wire, lossy ? &loss : nullptr);
    HostReplay replay(*client.host);
    client.network->setCharacterReplay(&replay);

    CrateRun run;
    core::u64 seen = 0;
    scene::World& world = client.host->world();
    for (int frame = 0; frame < 20000 && !log.contains("crates-done"); ++frame) {
        server.frame();
        client.frame();
        const replication::IReplication* replica = client.network->replication();
        if (replica == nullptr)
            continue;
        const replication::Stats stats = replica->stats();
        run.last = stats;
        const bool pushing =
            std::holds_alternative<bool>(world.getAttribute(client.host->workspace(), world.atoms().lookup("Pushing")));
        if (stats.corrections != seen) {
            seen = stats.corrections;
            if (pushing) {
                MESSAGE("MEASURE crate correction size=" << stats.lastCorrectionMetres
                                                         << " predicted=" << stats.predictedBodies);
                run.largest = std::max(run.largest, stats.lastCorrectionMetres);
                if (stats.lastCorrectionMetres > 0.01)
                    ++run.overDuringPush;
            }
        }
        if (!pushing)
            continue;
        run.mostPredicted = std::max(run.mostPredicted, stats.predictedBodies);
        const core::InstanceId hero = world.findFirstChild(client.host->workspace(), world.atoms().lookup("Hero"));
        const scene::PartComponent* body = hero.valid() ? world.parts().find(hero) : nullptr;
        if (body == nullptr)
            continue;
        ++run.looked;
        const core::DVec3 feet = body->cframe.position + core::DVec3{0.0, -1.0, 0.0};
        const core::DVec3 head = body->cframe.position + core::DVec3{0.0, 1.0, 0.0};
        for (const char* name : {"Crate1", "Crate2", "Crate3"}) {
            const core::InstanceId crate = world.findFirstChild(client.host->workspace(), world.atoms().lookup(name));
            const scene::PartComponent* box = crate.valid() ? world.parts().find(crate) : nullptr;
            if (box == nullptr)
                continue;
            // A capsule of radius 1: inside by what the axis is closer than that.
            const double gap = segmentToBox(feet, head, box->cframe, box->size) - 1.0;
            run.deepest = std::max(run.deepest, -gap);
            run.nearest = std::min(run.nearest, gap);
        }
    }
    CHECK(log.contains("crates-done"));
    MESSAGE("MEASURE crates lossy=" << lossy << " seed=" << seed << " corrections=" << seen
                                    << " overDuringPush=" << run.overDuringPush << " largest=" << run.largest
                                    << " deepest=" << run.deepest << " nearest=" << run.nearest
                                    << " looked=" << run.looked << " predicted=" << run.mostPredicted
                                    << " resims=" << run.last.resimulations << " ticks=" << run.last.resimulatedTicks
                                    << " micros=" << run.last.resimulationMicros << " remembers=" << replay.remembers
                                    << " rememberMicros=" << replay.rememberMicros);
    return run;
}

} // namespace

TEST_CASE("ten seconds pushing crates over jitter and loss are not corrected, and nothing overlaps (ADR 0133)")
{
    // Over three different runs of loss and jitter.
    for (const core::u32 seed : {9u, 5u, 21u}) {
        const CrateRun lossy = measureCrates(true, seed);
        // The crates were the replica's to simulate, not only to draw.
        CHECK(lossy.mostPredicted >= 2);
        CHECK(lossy.overDuringPush == 0);
        // Pushing -- in touch, looked at every tick -- and never inside a
        // crate past the solver's own skin.
        CHECK(lossy.looked > 500);
        CHECK(lossy.nearest < 0.05);
        CHECK(lossy.deepest < 0.03);
    }
    const CrateRun clean = measureCrates(false, 0);
    CHECK(clean.overDuringPush == 0);
    CHECK(clean.deepest < 0.03);
}

TEST_CASE("a replica predicts the loose parts near its character and what they touch, and no further (ADR 0133)")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write(".luaurc", R"({"aliases": {"shared": "src/shared"}})");
    server.project.write("src/shared/corner.luau", std::string(CornerShared));
    // The character stands at x = -6: a crate 7.5 m away, one touching it past
    // the 8 m radius, and one far off.
    std::string script(CrateServer);
    const std::string placed = R"(part("Crate1", vector.create(2, 2, 2), vector.create(-1, 1, 0), false)
part("Crate2", vector.create(2, 2, 2), vector.create(2, 1, 0.6), false)
part("Crate3", vector.create(2, 2, 2), vector.create(5, 1, -0.8), false))";
    const std::string instead = R"(part("Crate1", vector.create(2, 2, 2), vector.create(1.5, 1, 0), false)
part("Crate2", vector.create(2, 2, 2), vector.create(3.6, 1, 0), false)
part("Crate3", vector.create(2, 2, 2), vector.create(14, 1, 0), false))";
    const std::size_t at = script.find(placed);
    REQUIRE(at != std::string::npos);
    script.replace(at, placed.size(), instead);
    server.project.write("src/server/init.luau", script);
    server.bootOver(wire, nullptr);
    Machine client;
    client.project.write("src/client/init.luau", R"(game:GetService("NetworkService"):Join("memory:47121"))");
    client.bootOver(wire, nullptr);
    HostReplay replay(*client.host);
    client.network->setCharacterReplay(&replay);

    core::u32 predicted = 0;
    for (int frame = 0; frame < 240; ++frame) {
        server.frame();
        client.frame();
        if (const replication::IReplication* replica = client.network->replication(); replica != nullptr)
            predicted = replica->stats().predictedBodies;
    }
    CHECK(predicted == 2);
}

TEST_CASE("another machine's moving part is drawn between ticks, with what hangs on it (ADR 0134)")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/init.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        local RunService = game:GetService("RunService")
        NetworkService:Host(47122)
        local cart = Instance.new("Part")
        cart.Name = "Cart"
        cart.Anchored = true
        cart.Size = vector.create(2, 2, 2)
        cart.Position = vector.create(0, 3, 0)
        cart.Parent = workspace
        RunService.Heartbeat:Connect(function()
            cart.Position += vector.create(0.1, 0, 0)
        end)
    )");
    server.bootOver(wire, nullptr);
    Machine client;
    client.project.write("src/client/init.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        local RunService = game:GetService("RunService")
        NetworkService:Join("memory:47122")
        -- A name over the other machine's cart, as a player's is.
        RunService.Heartbeat:Connect(function()
            local cart = workspace:FindFirstChild("Cart")
            if cart and not cart:FindFirstChild("Tag") then
                local tag = Instance.new("BillboardGui")
                tag.Name = "Tag"
                tag.WorldOffset = vector.create(0, 2, 0)
                tag.Parent = cart
            end
        end)
    )");
    client.bootOver(wire, nullptr);

    // Joined, and the cart has been moving a while.
    for (int frame = 0; frame < 240; ++frame) {
        server.frame();
        client.frame();
    }
    scene::World& world = client.host->world();
    const core::InstanceId cart = world.findFirstChild(client.host->workspace(), world.atoms().lookup("Cart"));
    REQUIRE(cart.valid());
    const core::InstanceId tag = world.findFirstChild(cart, world.atoms().lookup("Tag"));
    REQUIRE(tag.valid());

    // The frame's own order, at 144 Hz: the server a tick for each of the
    // client's, and the client drawn between.
    render::TransformHistory history;
    render::DrawPoses poses;
    render::RenderCamera camera;
    camera.valid = true;
    camera.view = core::Mat4{};
    camera.projection = core::perspective(1.2f, 16.0f / 9.0f, 0.1f, 1000.0f);
    std::vector<double> xs;
    std::vector<core::DVec3> tagOffsets;
    double nextTick = 0.0;
    for (int frame = 0; frame < 288; ++frame) {
        const double now = static_cast<double>(frame) / 144.0;
        while (nextTick <= now) {
            server.frame();
            app::runDrawnTick(*client.host, *client.network, history);
            client.network->update();
            nextTick += 1.0 / 60.0;
        }
        const auto alpha = static_cast<core::f32>((now - (nextTick - 1.0 / 60.0)) * 60.0);
        poses.begin(world, &history, alpha);
        const core::DVec3 at = poses.part(cart).position;
        xs.push_back(at.x);
        camera.origin = core::DVec3{at.x, at.y, at.z + 15.0};
        for (const app::PlacedCanvas& placed : app::placeWorldCanvases(world, client.host->workspace(), {},
                                                                       core::Vec2{1280.0f, 720.0f}, camera, &poses)) {
            if (placed.canvas != tag)
                continue;
            const app::CanvasPlacement& p = placed.placement;
            const core::Vec3 middle = p.topLeft + p.right * (p.canvas.x * 0.5f) + p.down * (p.canvas.y * 0.5f);
            tagOffsets.push_back(core::DVec3{camera.origin.x + static_cast<double>(middle.x) - at.x,
                                             camera.origin.y + static_cast<double>(middle.y) - at.y,
                                             camera.origin.z + static_cast<double>(middle.z) - at.z});
        }
    }

    // Six metres a second: about 4.2 cm every frame at 144 Hz, never a whole
    // tick's 10 cm and then nothing. Snapshots arrive when they arrive, so a
    // frame may differ from the next by a little; none by a tick.
    REQUIRE(xs.size() == 288);
    int still = 0;
    double largest = 0.0;
    for (std::size_t at = 1; at < xs.size(); ++at) {
        const double step = xs[at] - xs[at - 1];
        still += step < 0.01 ? 1 : 0;
        largest = std::max(largest, step);
    }
    MESSAGE("MEASURE remote cart: still frames " << still << ", largest step " << largest);
    CHECK(still == 0);
    CHECK(largest < 0.07);
    // And the name over it rides on it.
    REQUIRE(tagOffsets.size() == 288);
    double drift = 0.0;
    for (const core::DVec3& offset : tagOffsets)
        drift = std::max(drift, std::hypot(offset.x - tagOffsets.front().x, offset.y - tagOffsets.front().y,
                                           offset.z - tagOffsets.front().z));
    CHECK(drift < 1e-3);
}

TEST_CASE("a Join during a scene change cancels the change, and no scene of its own opens on the replica (audit A10)")
{
    // The scene's close waits a second for its handler; the Join lands inside
    // that wait. Before, the change went on after the Join and opened scene B
    // on a replica -- mounting B's server code there.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/server/init.luau", R"(game:GetService("NetworkService"):Host(47123))");
    server.boot(wire);

    Machine client;
    client.project.write("content/scenes/a.scene.json",
                         R"json({"format":"scene","version":2,"root":{},"storage":{}})json");
    client.project.write("content/scenes/b.scene.json",
                         R"json({"format":"scene","version":2,"root":{},"storage":{}})json");
    client.project.write("src/scenes/b/server/code.luau", "print('b-server started')");
    client.project.write("src/scenes/b/client/code.luau", "print('b-client started')");
    client.project.write("src/client/init.luau", R"(
        local SceneService = game:GetService("SceneService")
        local NetworkService = game:GetService("NetworkService")
        SceneService.CurrentScene:BindToClose(function()
            task.wait(1)
        end)
        SceneService.SceneLoaded:Connect(function(path: string)
            print(`loaded:{path}`)
        end)
        SceneService:LoadScene("scenes/b.scene.json")
        task.delay(0.2, function()
            NetworkService:Join("memory:47123")
        end)
    )");
    app::WorldHostOptions options = bootOptions(client.project.root);
    options.bootScene = client.project.root / "content" / "scenes" / "a.scene.json";
    options.bootScenePath = "scenes/a.scene.json";
    REQUIRE_FALSE(client.host->boot(options).has_value());
    replication::Config base;
    base.ticksPerSnapshot = 1;
    base.interpolationDelayTicks = 0;
    client.network = std::make_unique<app::NetworkSession>([&client]() { return client.host.get(); }, base,
                                                           [wire]() { return net::createMemoryTransport(wire); });

    run(server, client, 240);
    CHECK(client.state() == Connected);
    CHECK_FALSE(log.contains("loaded:scenes/b.scene.json"));
    CHECK_FALSE(log.contains("b-server started"));
    CHECK(log.contains("cancelled"));
}

TEST_CASE("NA34: a jointed assembly handed to a replica holds together there, and its motor turns it")
{
    // A vehicle fell apart on the machine that drove it: the owner received
    // the parts and none of what held them -- no attachment, no joint, no
    // motor -- and simulated each part on its own. The world has no gravity
    // here, so nothing but the joint keeps the two together.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/client/host.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService:Host(47134)
        workspace.Gravity = vector.zero
        local AlongZ = CFrame.fromEuler(0, -math.pi / 2, 0)
        local function part(name: string, x: number): Part
            local made = Instance.new("Part")
            made.Name = name
            made.Size = vector.create(2, 2, 2)
            made.Position = vector.create(x, 20, 0)
            made.Parent = workspace
            return made
        end
        local post = part("Post", 0)
        local arm = part("Arm", 3)
        arm.Size = vector.create(1, 1, 1)
        local pivot = Instance.new("Attachment")
        pivot.CFrame = CFrame.new(1.5, 0, 0) * AlongZ
        pivot.Parent = post
        local elbow = Instance.new("Attachment")
        elbow.CFrame = CFrame.new(-1.5, 0, 0) * AlongZ
        elbow.Parent = arm
        local hinge = Instance.new("HingeConstraint")
        hinge.Name = "Hinge"
        hinge.Attachment0 = pivot
        hinge.Attachment1 = elbow
        hinge.CollideConnected = false
        hinge.ActuatorType = Enum.ActuatorType.Motor
        hinge.AngularVelocity = 2
        hinge.MotorMaxTorque = 100000
        hinge.Parent = workspace
        NetworkService.PlayerAdded:Connect(function(player)
            if player == NetworkService.LocalPlayer then
                return
            end
            post:SetNetworkOwner(player)
            arm:SetNetworkOwner(player)
            task.wait(2)
            -- **What the owner simulates, the authority sees** (ADR 0099):
            -- its own copies are kinematic and move only by what is sent.
            local before = arm.CFrame.RightVector
            task.wait(1)
            local after = arm.CFrame.RightVector
            local span = vector.magnitude(pivot.WorldCFrame.Position - elbow.WorldCFrame.Position)
            print(`server-turned:{vector.dot(before, after) < 0.95}`)
            print(`server-span:{span < 0.25}`)
        end)
    )");
    server.boot(wire);

    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Connected:Connect(function()
            task.wait(2)
            local hinge = workspace:FindFirstChild("Hinge") :: any
            local post = workspace:FindFirstChild("Post") :: any
            local arm = workspace:FindFirstChild("Arm") :: any
            print(`client-hinge:{hinge ~= nil and hinge.Attachment0 ~= nil and hinge.Attachment1 ~= nil}`)
            print(`client-gravity:{vector.magnitude(workspace.Gravity) == 0}`)
            if hinge == nil or hinge.Attachment0 == nil or hinge.Attachment1 == nil then
                return
            end
            print(`client-owner:{arm:GetNetworkOwner() == NetworkService.LocalPlayer}`)
            local function span(): number
                return vector.magnitude(hinge.Attachment0.WorldCFrame.Position - hinge.Attachment1.WorldCFrame.Position)
            end
            local function spin(): number
                return arm.AngularVelocity.z - post.AngularVelocity.z
            end
            print(`client-span:{span() < 0.25}`)
            print(`client-motor:{math.abs(spin() - 2) < 0.3}`)
            -- **The owner's own change stays its own**: the authority changed
            -- nothing, so no snapshot puts the old speed back.
            hinge.AngularVelocity = -2
            task.wait(1)
            print(`client-local-change:{math.abs(spin() + 2) < 0.3}`)
            print(`client-span-after:{span() < 0.25}`)
        end)
        NetworkService:Join("memory:47134")
    )");
    client.boot(wire);
    run(server, client, 360);
    REQUIRE(client.state() == Connected);
    CHECK(log.contains("client-hinge:true"));
    CHECK(log.contains("client-gravity:true"));
    CHECK(log.contains("client-owner:true"));
    CHECK(log.contains("client-span:true"));
    CHECK(log.contains("client-motor:true"));
    CHECK(log.contains("client-local-change:true"));
    CHECK(log.contains("client-span-after:true"));
    CHECK(log.contains("server-turned:true"));
    CHECK(log.contains("server-span:true"));
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}

TEST_CASE("D482: an assembly handed over the moment it is made is where the authority made it, and the authority "
          "follows it 20 m")
{
    // The kart game, measured on 43a63075: a server that builds a player's
    // kart and hands it over at once had the owner simulate it from the
    // origin -- the part was given away before its first state arrived, and
    // was never put anywhere -- and the authority refused every place it was
    // sent as out of reach, for the whole race.
    Captured log;
    auto wire = net::createMemoryNetwork();
    Machine server;
    server.project.write("src/client/host.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService:Host(47135)
        workspace.Gravity = vector.zero
        NetworkService.PlayerAdded:Connect(function(player)
            if player == NetworkService.LocalPlayer then
                return
            end
            local AlongZ = CFrame.fromEuler(0, -math.pi / 2, 0)
            local chassis = Instance.new("Part")
            chassis.Name = "Chassis"
            chassis.Size = vector.create(2, 2, 2)
            chassis.Position = vector.create(0, 20, 0)
            chassis.Parent = workspace
            local wheel = Instance.new("Part")
            wheel.Name = "Wheel"
            wheel.Size = vector.create(1, 1, 1)
            wheel.Position = vector.create(3, 20, 0)
            wheel.Parent = workspace
            local axle = Instance.new("Attachment")
            axle.CFrame = CFrame.new(1.5, 0, 0) * AlongZ
            axle.Parent = chassis
            local hub = Instance.new("Attachment")
            hub.CFrame = CFrame.new(-1.5, 0, 0) * AlongZ
            hub.Parent = wheel
            local hinge = Instance.new("HingeConstraint")
            hinge.Attachment0 = axle
            hinge.Attachment1 = hub
            hinge.CollideConnected = false
            hinge.Parent = workspace
            chassis:SetNetworkOwner(player)
            wheel:SetNetworkOwner(player)
            task.wait(3.5)
            print(`server-followed:{wheel.Position.x > 21 and wheel.Position.x < 25}`)
            print(`server-chassis:{chassis.Position.x > 18 and chassis.Position.x < 22}`)
        end)
    )");
    server.boot(wire);

    Machine client;
    client.project.write("src/client/join.luau", R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Connected:Connect(function()
            task.wait(0.5)
            local chassis = workspace:FindFirstChild("Chassis") :: any
            local wheel = workspace:FindFirstChild("Wheel") :: any
            if chassis == nil or wheel == nil then
                print("client-missing")
                return
            end
            print(`client-placed:{math.abs(chassis.Position.y - 20) < 0.5 and math.abs(wheel.Position.x - 3) < 0.5}`)
            -- Twenty metres in two seconds, on this machine alone.
            for _ = 1, 120 do
                chassis.LinearVelocity = vector.create(10, 0, 0)
                wheel.LinearVelocity = vector.create(10, 0, 0)
                task.wait()
            end
            chassis.LinearVelocity = vector.zero
            wheel.LinearVelocity = vector.zero
            print(`client-moved:{chassis.Position.x > 18}`)
        end)
        NetworkService:Join("memory:47135")
    )");
    client.boot(wire);
    run(server, client, 300);
    REQUIRE(client.state() == Connected);
    CHECK(log.contains("client-placed:true"));
    CHECK(log.contains("client-moved:true"));
    CHECK(log.contains("server-followed:true"));
    CHECK(log.contains("server-chassis:true"));
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
}
