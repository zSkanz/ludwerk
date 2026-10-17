// **An online game's worlds** (ADRs 0220, 0222): `servers.toml`, a manager
// whose worlds are real worlds in this process -- their pipes lists -- a front
// door that sends a joiner to the entry world with a pass, and a player who
// travels to another world carrying data.
#include <chrono>
#include <deque>
#include <doctest/doctest.h>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "engine/app/managed_world.h"
#include "engine/app/network_session.h"
#include "engine/app/server_console.h"
#include "engine/app/servers_config.h"
#include "engine/app/travel_pass.h"
#include "engine/app/world_host.h"
#include "engine/app/world_manager.h"
#include "engine/core/base64.h"
#include "engine/net/memory_transport.h"
#include "engine/replication/replication.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

namespace {

constexpr core::i32 Connected = 2;

[[nodiscard]] std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A world the manager started, run here: its standard input and output are
// two lists.
struct WorldMachine
{
    Project project;
    std::unique_ptr<app::WorldHost> host = std::make_unique<app::WorldHost>();
    std::unique_ptr<app::NetworkSession> network;
    std::unique_ptr<app::ManagedWorld> managed;
    std::deque<std::string> input;
    std::string output;
    bool killed = false;

    void boot(const std::shared_ptr<net::MemoryNetwork>& wire, const app::WorldLaunch& launch, std::string script)
    {
        project.write("src/server/world.luau", script);
        REQUIRE_FALSE(host->boot(bootOptions(project.root)).has_value());
        replication::Config base;
        base.ticksPerSnapshot = 1;
        base.interpolationDelayTicks = 0;
        network = std::make_unique<app::NetworkSession>([this]() { return host.get(); }, base,
                                                        [wire]() { return net::createMemoryTransport(wire); });
        std::array<core::u8, 32> key{};
        const std::optional<std::vector<core::u8>> bytes = core::base64Decode(launch.managerKey);
        REQUIRE(bytes.has_value());
        std::copy(bytes->begin(), bytes->end(), key.begin());
        managed =
            std::make_unique<app::ManagedWorld>(launch.name, key, [this](const std::string& line) { output += line; });
        managed->setKey(core::toHex(network->security().identity.pub));
        network->setJoinGate(
            [this](replication::JoinAsk& ask) { return managed->admit(ask, host->world(), unixNow()); });
        host->world().engineState().pendingNetwork =
            scene::EngineState::NetworkRequest{.kind = scene::EngineState::NetworkRequest::Kind::Host,
                                               .address = {},
                                               .port = launch.port,
                                               .relay = {},
                                               .relayDirect = true,
                                               .ticket = {}};
    }

    // **Ended**, as a process is: its port let go with it.
    void end()
    {
        killed = true;
        network.reset();
    }

    void frame()
    {
        if (killed)
            return;
        while (!input.empty()) {
            const std::string line = std::move(input.front());
            input.pop_front();
            if (const std::optional<app::ManagerMessage> message = app::ManagerMessage::read(line); message.has_value())
                managed->take(*message, host->world(), network->replication());
        }
        network->receive();
        host->tick();
        network->send();
        network->sendMessages();
        network->update();
        managed->frame(host->world(), host->dataModel(), network->replication(),
                       host->world().engineState().networkPort, nowNs += 16'000'000ull, 1.0);
        // Told to stop, it ends, as the process does.
        if (managed->stopping())
            end();
    }

    core::u64 nowNs = 0;
};

// The manager's view of one: what it writes goes in, what it says comes out.
class FakeProcess final : public app::WorldProcess
{
public:
    explicit FakeProcess(WorldMachine& world) : m_world(world) {}
    bool write(std::string_view text) override
    {
        // A line at a time, as a pipe delivers them.
        std::size_t start = 0;
        for (std::size_t end = text.find('\n'); end != std::string_view::npos; end = text.find('\n', start)) {
            m_world.input.emplace_back(text.substr(start, end - start));
            start = end + 1;
        }
        return true;
    }
    [[nodiscard]] std::string read() override
    {
        std::string out = std::move(m_world.output);
        m_world.output.clear();
        return out;
    }
    [[nodiscard]] bool running() override { return !m_world.killed; }
    void kill() override { m_world.end(); }
    void closeInput() override {}

private:
    WorldMachine& m_world;
};

struct Client
{
    Project project;
    std::unique_ptr<app::WorldHost> host = std::make_unique<app::WorldHost>();
    std::unique_ptr<app::NetworkSession> network;

    void boot(const std::shared_ptr<net::MemoryNetwork>& wire, std::string script)
    {
        project.write("src/client/join.luau", script);
        REQUIRE_FALSE(host->boot(bootOptions(project.root)).has_value());
        replication::Config base;
        base.ticksPerSnapshot = 1;
        base.interpolationDelayTicks = 0;
        network = std::make_unique<app::NetworkSession>([this]() { return host.get(); }, base,
                                                        [wire]() { return net::createMemoryTransport(wire); });
        network->setJoinTimeout(2.0);
    }

    void frame()
    {
        network->receive();
        host->tick();
        network->send();
        network->sendMessages();
        network->update();
    }
};

constexpr std::string_view Servers = R"(
[manager]
public_address = "memory"
port = 47981
world_ports = "47982-47989"

[[world]]
name = "town"
max_players = 50
entry = true

[[world]]
name = "fields"
)";

} // namespace

TEST_CASE("servers.toml names its worlds, one entry, and is refused when it is wrong")
{
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(Servers, "servers.toml", error);
    REQUIRE(config.has_value());
    CHECK(config->publicAddress == "memory");
    CHECK(config->firstWorldPort == 47982);
    REQUIRE(config->worlds.size() == 2);
    CHECK(config->worlds[0].name == "town");
    CHECK(config->worlds[0].maxPlayers == 50);
    CHECK(config->entry()->name == "town");
    CHECK(config->world("fields") != nullptr);
    // Administration off unless asked, beside the front door when it is.
    CHECK_FALSE(config->admin);
    std::optional<core::EngineError> adminWhy;
    const std::optional<app::ServersConfig> administered =
        app::readServersConfig("[manager]\nadmin = true\nadmin_allow = [\"10.0.0.0/8\"]\nhealth = true\n"
                               "[[world]]\nname = \"a\"\nentry = true\n",
                               "servers.toml", adminWhy);
    REQUIRE(administered.has_value());
    CHECK(administered->adminPort == 7778);
    CHECK(administered->adminAllow == std::vector<std::string>{"10.0.0.0/8"});
    CHECK(administered->health);

    // No entry; two; a name twice; a name that is not one; a port in the
    // range, the front door's or administration's.
    for (const std::string_view wrong :
         {std::string_view{"[[world]]\nname = \"town\"\n"},
          std::string_view{"[[world]]\nname = \"a\"\nentry = true\n[[world]]\nname = \"b\"\nentry = true\n"},
          std::string_view{"[[world]]\nname = \"a\"\nentry = true\n[[world]]\nname = \"a\"\n"},
          std::string_view{"[[world]]\nname = \"Town Square\"\nentry = true\n"},
          std::string_view{"[manager]\nport = 7800\n[[world]]\nname = \"a\"\nentry = true\n"},
          std::string_view{"[manager]\nport = 7799\nadmin = true\n[[world]]\nname = \"a\"\nentry = true\n"}}) {
        std::optional<core::EngineError> why;
        CHECK_FALSE(app::readServersConfig(wrong, "servers.toml", why).has_value());
        CHECK(why.has_value());
    }
}

TEST_CASE("a pass is the manager's, for one player, one world, thirty seconds and one use")
{
    const core::Ed25519Pair manager = core::ed25519Generate();
    const core::Ed25519Pair stranger = core::ed25519Generate();
    const std::int64_t now = unixNow();
    app::TravelPass made{.player = "aa",
                         .origin = "town",
                         .target = "fields",
                         .data = "d",
                         .ticket = "t",
                         .reason = {},
                         .issued = now,
                         .expires = now + app::PassSeconds,
                         .nonce = "n1"};
    app::PassReplay replay;
    app::PassRefusal why = app::PassRefusal::Missing;
    const std::string signedPass = app::signPass(made, manager.secret);
    const std::optional<app::TravelPass> opened =
        app::openPass(signedPass, manager.pub, "fields", "aa", now, replay, why);
    REQUIRE(opened.has_value());
    CHECK(opened->origin == "town");
    CHECK(opened->data == "d");
    CHECK(opened->ticket == "t");
    // Once.
    CHECK_FALSE(app::openPass(signedPass, manager.pub, "fields", "aa", now, replay, why).has_value());
    CHECK(why == app::PassRefusal::Used);
    made.nonce = "n2";
    // Another world's, another player's, a stranger's, an old one.
    CHECK_FALSE(
        app::openPass(app::signPass(made, manager.secret), manager.pub, "crypt", "aa", now, replay, why).has_value());
    CHECK(why == app::PassRefusal::OtherWorld);
    CHECK_FALSE(
        app::openPass(app::signPass(made, manager.secret), manager.pub, "fields", "bb", now, replay, why).has_value());
    CHECK(why == app::PassRefusal::OtherPlayer);
    CHECK_FALSE(
        app::openPass(app::signPass(made, stranger.secret), manager.pub, "fields", "aa", now, replay, why).has_value());
    CHECK(why == app::PassRefusal::NotTheManagers);
    CHECK_FALSE(app::openPass(app::signPass(made, manager.secret), manager.pub, "fields", "aa", now + 60, replay, why)
                    .has_value());
    CHECK(why == app::PassRefusal::Expired);
    CHECK_FALSE(app::openPass({}, manager.pub, "fields", "aa", now, replay, why).has_value());
    CHECK(why == app::PassRefusal::Missing);
}

TEST_CASE("a joiner at the front door lands in the entry world, and travels to another carrying data")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(Servers, "servers.toml", error);
    REQUIRE(config.has_value());

    Project managerRoot;
    std::map<std::string, std::unique_ptr<WorldMachine>> worlds;
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.TravelArrived:Connect(function(player, data, origin)
            print(`town-arrived:{origin}:{data == nil}`)
            NetworkService:Travel(player, "fields", { gold = 42, gate = "north" })
        end)
    )";
    const std::string fields = R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.TravelArrived:Connect(function(player, data, origin)
            print(`fields-arrived:{origin}:{data.gold}:{data.gate}`)
        end)
    )";
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.projectId = {};
    // The sessions here bind nothing, as every test's does.
    options.prologue = std::vector<core::u8>{};
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        auto& machine = worlds[launch.name];
        machine = std::make_unique<WorldMachine>();
        machine->boot(wire, launch, launch.name == "town" ? town : fields);
        return std::make_unique<FakeProcess>(*machine);
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());

    Client client;
    client.boot(wire, R"(
        local NetworkService = game:GetService("NetworkService")
        local screen = require("@engine/travelscreen").mount()
        NetworkService.TravelStarted:Connect(function(address)
            print(`client-travel:{address}`)
            task.defer(function()
                print(`screen:{screen:IsShowing()}:{screen:GetText()}`)
            end)
        end)
        NetworkService.Connected:Connect(function()
            print("client-connected")
            task.defer(function()
                print(`screen-after:{screen:IsShowing()}`)
            end)
        end)
        NetworkService.JoinFailed:Connect(function(reason)
            print(`client-failed:{reason}`)
        end)
        task.wait(0.2)
        NetworkService:Join("memory:47981")
    )");

    const auto run = [&](int frames) {
        for (int at = 0; at < frames; ++at) {
            now += 16'000'000ull;
            manager.frame();
            for (auto& [name, machine] : worlds)
                machine->frame();
            client.frame();
        }
    };
    run(30);
    // Both worlds ready, on their own ports.
    for (const app::WorldManager::WorldView& view : manager.worlds())
        CHECK_MESSAGE(view.state == app::WorldManager::State::Ready, view.name);

    run(200);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    // The door sent it to the town, which took it with the pass...
    CHECK(log.contains("town-arrived::true"));
    // ...and the town sent it on, with what it carried.
    CHECK(log.contains("client-travel:town"));
    // The loading screen, mounted in one line, covers the hop and goes.
    CHECK(log.contains("screen:true:Going to fields"));
    CHECK(log.contains("screen-after:false"));
    CHECK(log.contains("client-travel:fields"));
    CHECK(log.contains("fields-arrived:town:42:north"));
    CHECK(client.host->world().engineState().networkState == Connected);

    // A world joined directly, without the door's pass, refuses.
    Client sneaking;
    sneaking.boot(wire, R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.JoinFailed:Connect(function(reason)
            print(`sneak-failed:{reason}`)
        end)
        NetworkService:Join("memory:47982")
    )");
    for (int at = 0; at < 120; ++at) {
        now += 16'000'000ull;
        manager.frame();
        for (auto& [name, machine] : worlds)
            machine->frame();
        sneaking.frame();
    }
    CHECK(log.contains("sneak-failed:This world is joined through its game's front door, not directly."));
}

TEST_CASE("a world that falls is started again, and past its limit is failed until an operator restarts it")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    std::optional<app::ServersConfig> config = app::readServersConfig(
        "[manager]\nrestarts = 2\nrestart_minutes = 10\nworld_ports = \"47990-47995\"\nport = 47996\n"
        "[[world]]\nname = \"town\"\nentry = true\n",
        "servers.toml", error);
    REQUIRE(config.has_value());
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    int started = 0;
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        ++started;
        // Each world its own engine log, beside what the manager keeps of it.
        CHECK(launch.logFile == managerRoot.root / "logs" / "town" / "engine.log");
        machines.push_back(std::make_unique<WorldMachine>());
        machines.back()->boot(wire, launch, "");
        return std::make_unique<FakeProcess>(*machines.back());
    };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    const auto run = [&](int frames) {
        for (int at = 0; at < frames; ++at) {
            now += 16'000'000ull;
            manager.frame();
            machines.back()->frame();
        }
    };
    run(10);
    CHECK(manager.worlds().front().state == app::WorldManager::State::Ready);
    CHECK(started == 1);

    // It falls: started again.
    machines.back()->end();
    run(10);
    CHECK(started == 2);
    CHECK(manager.worlds().front().restarts == 1);
    // A world that goes silent is ended and started again too.
    now += 11'000'000'000ull;
    manager.frame();
    run(10);
    CHECK(started == 3);
    // Past the limit: failed, and left so.
    machines.back()->end();
    run(10);
    CHECK(manager.worlds().front().state == app::WorldManager::State::Failed);
    CHECK(started == 3);
    // Until the operator says so.
    CHECK(manager.run("restart town").find("town") != std::string::npos);
    run(10);
    CHECK(started == 4);
    CHECK(manager.worlds().front().state == app::WorldManager::State::Ready);
    CHECK(manager.run("worlds").find("town") != std::string::npos);
}

TEST_CASE("an update starts each world again from the new build and moves its players across, dropping nobody")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(
        R"([manager]
public_address = "memory"
port = 47960
world_ports = "47961-47969"
[[world]]
name = "town"
entry = true
[[instance]]
template = "crypt"
warm = 1
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    const std::string script = R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.TravelArrived:Connect(function(player, data, origin)
            print(`arrived:{origin}`)
        end)
    )";
    const auto launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        machines.push_back(std::make_unique<WorldMachine>());
        machines.back()->boot(wire, launch, script);
        return std::make_unique<FakeProcess>(*machines.back());
    };
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.launcher = launcher;
    // `broken` holds a build that will not start; `nowhere` holds no game.
    const auto broken = [](const app::WorldLaunch&) -> std::unique_ptr<app::WorldProcess> { return nullptr; };
    options.launcherFor = [&](const std::filesystem::path& folder) {
        if (folder == "nowhere")
            return app::WorldLauncher{};
        return folder == "broken" ? app::WorldLauncher(broken) : app::WorldLauncher(launcher);
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    Client client;
    client.boot(wire, R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Connected:Connect(function()
            print("client-connected")
        end)
        NetworkService.Disconnected:Connect(function(reason)
            print(`client-dropped:{reason}`)
        end)
        task.wait(0.2)
        NetworkService:Join("memory:47960")
    )");
    const auto run = [&](int frames) {
        for (int at = 0; at < frames; ++at) {
            now += 16'000'000ull;
            manager.frame();
            for (const std::unique_ptr<WorldMachine>& machine : machines)
                machine->frame();
            client.frame();
        }
    };
    run(150);
    REQUIRE(log.contains("arrived:"));
    // The town and a warm crypt.
    REQUIRE(machines.size() == 2);

    CHECK_FALSE(manager.run("update new-build").empty());
    run(300);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    // The new town and a new warm crypt; the player in the town, the old
    // town and the old spare stopped. **No instance became a world**: one
    // updated is an instance still, or it is not there.
    REQUIRE(machines.size() == 4);
    CHECK(log.contains("arrived:town"));
    CHECK(machines[0]->killed);
    CHECK(machines[1]->killed);
    CHECK_FALSE(machines[2]->killed);
    CHECK_FALSE(machines[3]->killed);
    for (const app::WorldManager::WorldView& view : manager.worlds())
        CHECK((view.name == "town") != view.instance);
    CHECK_FALSE(log.contains("client-dropped"));
    CHECK(client.host->world().engineState().networkState == Connected);
    bool readyAgain = false;
    for (const app::WorldManager::WorldView& view : manager.worlds())
        readyAgain =
            readyAgain || (view.name == "town" && view.state == app::WorldManager::State::Ready && view.port != 47961);
    CHECK(readyAgain);

    // **A folder with no game is refused** before anything moves.
    CHECK(manager.run("update nowhere").find("nowhere") != std::string::npos);
    run(30);
    CHECK(machines.size() == 4);

    // **A build that will not start leaves the old one in charge** -- and what
    // starts next is the old build's, not the broken one's.
    (void)manager.run("update broken");
    run(30);
    CHECK(log.contains("did not start"));
    CHECK(client.host->world().engineState().networkState == Connected);
    (void)manager.run("restart town");
    run(200);
    REQUIRE(machines.size() == 5);
    CHECK_FALSE(machines.back()->killed);
}

TEST_CASE("an instance made for a player takes them alone, closes when empty, and a fallen one sends them back")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(
        R"([manager]
public_address = "memory"
port = 47940
world_ports = "47941-47959"
[[world]]
name = "town"
entry = true
[[instance]]
template = "crypt"
warm = 1
max_instances = 3
idle_close_seconds = 2
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    std::map<std::string, WorldMachine*> byName;
    // The town makes a crypt for a new player, and another when they come
    // back from the first.
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        local InstanceService = game:GetService("InstanceService")
        InstanceService.InstanceClosed:Connect(function(id, reason)
            print(`closed:{id}:{reason}`)
        end)
        NetworkService.TravelArrived:Connect(function(player, data, origin, reason)
            print(`town-arrived:{origin}:{reason}`)
            if reason == "" and (origin == "" or origin == "i-1") then
                local id = InstanceService:CreateInstanceAsync("crypt", { Players = { player }, Data = { level = 3 } })
                print(`created:{id}`)
                NetworkService:Travel(player, id)
            end
        end)
    )";
    // The first crypt sends its player home; the second keeps them.
    const std::string crypt = R"(
        local NetworkService = game:GetService("NetworkService")
        local InstanceService = game:GetService("InstanceService")
        InstanceService.Closing:Connect(function()
            print(`closing:{InstanceService.InstanceId}`)
        end)
        NetworkService.TravelArrived:Connect(function(player, data, origin)
            print(`crypt-arrived:{origin}:{InstanceService.InstanceId}`)
            if InstanceService.InstanceId == "i-1" then
                task.wait(0.3)
                NetworkService:Travel(player, "town")
            end
        end)
    )";
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        machines.push_back(std::make_unique<WorldMachine>());
        machines.back()->boot(wire, launch, launch.name == "town" ? town : crypt);
        byName[launch.name] = machines.back().get();
        return std::make_unique<FakeProcess>(*machines.back());
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    // The town and one warm crypt.
    CHECK(machines.size() == 2);
    Client client;
    client.boot(wire, R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Disconnected:Connect(function(reason)
            print("client-dropped")
            task.wait(0.2)
            NetworkService:Join("memory:47940")
        end)
        task.wait(0.2)
        NetworkService:Join("memory:47940")
    )");
    const auto run = [&](int frames) {
        for (int at = 0; at < frames; ++at) {
            now += 16'000'000ull;
            manager.frame();
            for (std::size_t index = 0; index < machines.size(); ++index)
                machines[index]->frame();
            client.frame();
        }
    };
    run(300);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    // The warm one was given to the player, who went in and came back...
    CHECK(log.contains("created:i-1"));
    CHECK(log.contains("crypt-arrived:town:i-1"));
    CHECK(log.contains("town-arrived:i-1:"));
    // ...and into the next, which was warm in the first one's place.
    CHECK(log.contains("created:i-2"));
    CHECK(log.contains("crypt-arrived:town:i-2"));
    // The first, empty for its while, closes.
    run(250);
    CHECK(log.contains("closing:i-1"));
    CHECK(log.contains("closed:i-1:Closed"));
    REQUIRE(byName.contains("i-1"));
    CHECK(byName["i-1"]->killed);
    CHECK_FALSE(manager.run("instances").empty());

    // **The second falls**: its player comes back to the town, told why.
    REQUIRE(byName.contains("i-2"));
    byName["i-2"]->end();
    run(300);
    CHECK(log.contains("client-dropped"));
    CHECK(log.contains("town-arrived::InstanceFailed"));
    CHECK(log.contains("closed:i-2:InstanceFailed"));
    CHECK(client.host->world().engineState().networkState == Connected);
}

TEST_CASE("an event between worlds reaches every world that hears its topic, and a world alone hears its own")
{
    Captured log;
    // **Alone**: a world no manager runs hears its own.
    {
        Project project;
        project.write("src/server/chat.luau", R"(
            local CrossWorldService = game:GetService("CrossWorldService")
            CrossWorldService:Subscribe("chat", function(message, origin)
                print(`alone-heard:{message.text}:{origin}`)
            end)
            CrossWorldService:Publish("chat", { text = "hello" })
            local ok = pcall(function()
                CrossWorldService:Publish(string.rep("x", 81), 1)
            end)
            print(`long-topic:{ok}`)
        )");
        app::WorldHost host;
        REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
        for (int at = 0; at < 4; ++at)
            host.tick();
        CHECK(log.contains("alone-heard:hello:"));
        CHECK(log.contains("long-topic:false"));
    }

    // **Under a manager**: every world that hears the topic, the publisher
    // included; one that does not hear nothing.
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(
        R"([manager]
public_address = "memory"
port = 47920
world_ports = "47921-47929"
[[world]]
name = "town"
entry = true
[[world]]
name = "fields"
[[world]]
name = "crypt"
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        machines.push_back(std::make_unique<WorldMachine>());
        const std::string hears = launch.name == "crypt" ? "other" : "guild:42";
        machines.back()->boot(wire, launch,
                              R"(
            local CrossWorldService = game:GetService("CrossWorldService")
            CrossWorldService:Subscribe(")" +
                                  hears + R"(", function(message, origin)
                print(`)" + launch.name +
                                  R"(-heard:{message.from}:{origin}`)
            end)
        )" +
                                  (launch.name == "town" ? std::string(R"(
            task.wait(0.5)
            CrossWorldService:Publish("guild:42", { from = "ada" })
        )")
                                                         : std::string{}));
        return std::make_unique<FakeProcess>(*machines.back());
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    for (int at = 0; at < 120; ++at) {
        now += 16'000'000ull;
        manager.frame();
        for (const std::unique_ptr<WorldMachine>& machine : machines)
            machine->frame();
    }
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("town-heard:ada:town"));
    CHECK(log.contains("fields-heard:ada:town"));
    CHECK_FALSE(log.contains("crypt-heard"));
}
