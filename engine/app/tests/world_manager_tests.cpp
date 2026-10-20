// **An online game's worlds** (ADRs 0220, 0222): `servers.toml`, a manager
// whose worlds are real worlds in this process -- their pipes lists -- a front
// door that sends a joiner to the entry world with a pass, and a player who
// travels to another world carrying data.
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <doctest/doctest.h>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "engine/app/data_store.h"
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
        app::WorldHostOptions options = bootOptions(project.root);
        options.managedWorld = launch.name;
        REQUIRE_FALSE(host->boot(options).has_value());
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
    // The database's backups and the move log's days (ADR 0238 §7): none
    // and ninety unless asked.
    CHECK(config->backupEveryHours == 0.0);
    CHECK(config->backupKeep == 7);
    CHECK(config->movesKeepDays == 90);
    std::optional<core::EngineError> dataWhy;
    const std::optional<app::ServersConfig> backedUp = app::readServersConfig(
        "[data]\nbackup_every_hours = 6\nbackup_keep = 3\nmoves_keep_days = 0\n[[world]]\nname = \"a\"\nentry = true\n",
        "servers.toml", dataWhy);
    REQUIRE(backedUp.has_value());
    CHECK(backedUp->backupEveryHours == 6.0);
    CHECK(backedUp->backupKeep == 3);
    CHECK(backedUp->movesKeepDays == 0);

    // No entry; two; a name twice; a name that is not one; a port in the
    // range, the front door's or administration's.
    for (const std::string_view wrong :
         {std::string_view{"[[world]]\nname = \"town\"\n"},
          std::string_view{"[[world]]\nname = \"a\"\nentry = true\n[[world]]\nname = \"b\"\nentry = true\n"},
          std::string_view{"[[world]]\nname = \"a\"\nentry = true\n[[world]]\nname = \"a\"\n"},
          std::string_view{"[[world]]\nname = \"Town Square\"\nentry = true\n"},
          std::string_view{"[manager]\nport = 7800\n[[world]]\nname = \"a\"\nentry = true\n"},
          std::string_view{"[manager]\nport = 7799\nadmin = true\n[[world]]\nname = \"a\"\nentry = true\n"},
          std::string_view{"[data]\nbackup_every_hours = 0.1\n[[world]]\nname = \"a\"\nentry = true\n"},
          std::string_view{"[data]\nbackup_keep = 0\n[[world]]\nname = \"a\"\nentry = true\n"}}) {
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
                         .account = {},
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

    // **The account is inside what is signed** (ADR 0237 amendment C): a
    // pass crosses the player's machine, and one whose account was changed
    // on the way is not the manager's.
    made.nonce = "n3";
    made.account = "acc-1";
    const std::string withAccount = app::signPass(made, manager.secret);
    const std::size_t dot = withAccount.find('.');
    REQUIRE(dot != std::string::npos);
    const std::optional<std::vector<core::u8>> body = core::base64UrlDecode(withAccount.substr(0, dot));
    REQUIRE(body.has_value());
    std::string json(body->begin(), body->end());
    const std::size_t at = json.find("acc-1");
    REQUIRE(at != std::string::npos);
    json.replace(at, 5, "acc-2");
    const std::string forged =
        core::base64UrlEncode(std::span<const core::u8>(reinterpret_cast<const core::u8*>(json.data()), json.size())) +
        withAccount.substr(dot);
    CHECK_FALSE(app::openPass(forged, manager.pub, "fields", "aa", now, replay, why).has_value());
    CHECK(why == app::PassRefusal::NotTheManagers);
    const std::optional<app::TravelPass> honest =
        app::openPass(withAccount, manager.pub, "fields", "aa", now, replay, why);
    REQUIRE(honest.has_value());
    CHECK(honest->account == "acc-1");
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

TEST_CASE("ADR 0237: with accounts on, the front door refuses who did not sign in, and a test account is known as such "
          "in every world")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config =
        app::readServersConfig(std::string(Servers) + "\n[accounts]\nenabled = true\n", "servers.toml", error);
    REQUIRE(config.has_value());
    REQUIRE(config->accounts);

    // **Who a world takes a player for**: the account the pass named, and the
    // same after a trip.
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.TravelArrived:Connect(function(player, data, origin)
            print(`town-account:{player.AccountId}`)
            NetworkService:Travel(player, "fields", {})
        end)
    )";
    const std::string fields = R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.TravelArrived:Connect(function(player, data, origin)
            print(`fields-account:{player.AccountId}`)
        end)
    )";
    const auto manage = [&](bool trial, Project& root, std::map<std::string, std::unique_ptr<WorldMachine>>& worlds,
                            core::u64& now) {
        app::WorldManager::Options options;
        options.config = *config;
        options.root = root.root;
        options.projectId = {};
        options.prologue = std::vector<core::u8>{};
        options.dataStore = []() { return app::makeMemoryDataStore(); };
        options.trialAccounts = trial;
        options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
            auto& machine = worlds[launch.name];
            machine = std::make_unique<WorldMachine>();
            machine->boot(wire, launch, launch.name == "town" ? town : fields);
            return std::make_unique<FakeProcess>(*machine);
        };
        options.transports = [wire]() { return net::createMemoryTransport(wire); };
        options.clock = [&now]() { return now; };
        options.unixClock = []() { return unixNow(); };
        return std::make_unique<app::WorldManager>(std::move(options));
    };
    const std::string joiner = R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.JoinFailed:Connect(function(reason)
            print(`joiner-failed:{reason}`)
        end)
        task.wait(0.2)
        NetworkService:Join("memory:47981")
    )";

    // A game's servers: nobody in without signing in.
    {
        Project root;
        std::map<std::string, std::unique_ptr<WorldMachine>> worlds;
        core::u64 now = 1;
        std::unique_ptr<app::WorldManager> manager = manage(false, root, worlds, now);
        REQUIRE_FALSE(manager->start().has_value());
        Client client;
        client.boot(wire, joiner);
        for (int at = 0; at < 240; ++at) {
            now += 16'000'000ull;
            manager->frame();
            for (auto& [name, machine] : worlds)
                machine->frame();
            client.frame();
        }
        CHECK(log.contains("joiner-failed:Sign in first: this game lets in only the players its servers signed in."));
        CHECK_FALSE(log.contains("town-account:"));
    }

    // **Tried in the editor**: a test account of the player's key, in the
    // entry world and after a trip.
    {
        Project root;
        std::map<std::string, std::unique_ptr<WorldMachine>> worlds;
        core::u64 now = 1;
        std::unique_ptr<app::WorldManager> manager = manage(true, root, worlds, now);
        REQUIRE_FALSE(manager->start().has_value());
        Client client;
        client.boot(wire, joiner);
        for (int at = 0; at < 400; ++at) {
            now += 16'000'000ull;
            manager->frame();
            for (auto& [name, machine] : worlds)
                machine->frame();
            client.frame();
        }
        CHECK_MESSAGE(log.firstError().empty(), log.firstError());
        CHECK(log.contains("town-account:trial-"));
        CHECK(log.contains("fields-account:trial-"));
    }
}

TEST_CASE("ADR 0240: a party crosses worlds -- invited where its member is, heard in each, and sent together")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(Servers, "servers.toml", error);
    REQUIRE(config.has_value());

    // The town makes a party of its first two players, sends the third to
    // the fields and invites them there by their member id; once the party is
    // three, it sends the party to the fields.
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        local PartyService = game:GetService("PartyService")
        local here = {}
        local sent = false
        local madeId = nil
        PartyService.Changed:Connect(function(id)
            print(`town-changed:{id == madeId}`)
            local party = PartyService:GetPartyAsync(here[1])
            if party and #party.Members == 3 and not sent then
                sent = true
                local went = NetworkService:TravelGroup(party.Id, "fields")
                print(`town-sent:{#went}`)
            end
        end)
        NetworkService.PlayerAdded:Connect(function(player)
            table.insert(here, player)
            if #here ~= 3 then
                return
            end
            local party = PartyService:CreateAsync(here[1])
            madeId = party.Id
            local invited = PartyService:InviteAsync(party.Id, here[1], here[2])
            local joined = PartyService:AcceptAsync(party.Id, here[2])
            print(`town-two:{invited ~= nil}:{#joined.Members}`)
            local _, refused = PartyService:InviteAsync(party.Id, here[3], "nobody-here")
            print(`town-refused:{refused}`)
            local third = PartyService:MemberIdOf(here[3])
            NetworkService:Travel(here[3], "fields", {})
            task.wait(1)
            PartyService:InviteAsync(party.Id, here[1], third)
        end)
    )";
    const std::string fields = R"(
        local NetworkService = game:GetService("NetworkService")
        local PartyService = game:GetService("PartyService")
        local arrived = 0
        PartyService.Changed:Connect(function(id)
            print("fields-changed")
        end)
        PartyService.Invited:Connect(function(player, id, by)
            local party = PartyService:AcceptAsync(id, player)
            print(`fields-accepted:{#party.Members}`)
        end)
        NetworkService.TravelArrived:Connect(function(player)
            arrived += 1
            print(`fields-arrived:{arrived}`)
        end)
    )";
    Project managerRoot;
    std::map<std::string, std::unique_ptr<WorldMachine>> worlds;
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.projectId = {};
    options.prologue = std::vector<core::u8>{};
    options.dataStore = []() { return app::makeMemoryDataStore(); };
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

    const std::string joiner = R"(
        local NetworkService = game:GetService("NetworkService")
        task.wait(0.2)
        NetworkService:Join("memory:47981")
    )";
    std::array<Client, 3> clients;
    for (Client& client : clients)
        client.boot(wire, joiner);
    for (int at = 0; at < 900; ++at) {
        now += 16'000'000ull;
        manager.frame();
        for (auto& [name, machine] : worlds)
            machine->frame();
        for (Client& client : clients)
            client.frame();
    }
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("town-two:true:2"));
    // Only a member invites.
    CHECK(log.contains("town-refused:NotMember"));
    // The third, in the fields, invited there and in.
    CHECK(log.contains("fields-accepted:3"));
    // The change heard in both worlds.
    CHECK(log.contains("town-changed:true"));
    CHECK(log.contains("fields-changed"));
    // The party sent together: the two in the town go, and all three are in
    // the fields.
    CHECK(log.contains("town-sent:2"));
    CHECK(log.contains("fields-arrived:3"));
}

TEST_CASE("ADR 0240: an instance made for a party takes its members, and nobody else")
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
max_instances = 2
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    // The town makes a party of its first two players, a crypt for the
    // party, and sends all three there: the third is no member.
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        local InstanceService = game:GetService("InstanceService")
        local PartyService = game:GetService("PartyService")
        local here = {}
        NetworkService.PlayerAdded:Connect(function(player)
            table.insert(here, player)
            if #here ~= 3 then
                return
            end
            local party = PartyService:CreateAsync(here[1])
            PartyService:InviteAsync(party.Id, here[1], here[2])
            PartyService:AcceptAsync(party.Id, here[2])
            local id = InstanceService:CreateInstanceAsync("crypt", { Party = party.Id })
            for _, player in here do
                NetworkService:Travel(player, id)
            end
            task.wait(5)
            local stayed = {}
            for _, player in here do
                table.insert(stayed, tostring(table.find(NetworkService:GetPlayers(), player) ~= nil))
            end
            print(`town-stayed:{table.concat(stayed, ":")}`)
        end)
    )";
    const std::string crypt = R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.TravelArrived:Connect(function()
            print("crypt-arrived")
        end)
    )";
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        machines.push_back(std::make_unique<WorldMachine>());
        machines.back()->boot(wire, launch, launch.name == "town" ? town : crypt);
        return std::make_unique<FakeProcess>(*machines.back());
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    std::array<Client, 3> clients;
    for (Client& client : clients)
        client.boot(wire, R"(
            local NetworkService = game:GetService("NetworkService")
            task.wait(0.2)
            NetworkService:Join("memory:47940")
        )");
    for (int at = 0; at < 720; ++at) {
        now += 16'000'000ull;
        manager.frame();
        for (std::size_t index = 0; index < machines.size(); ++index)
            machines[index]->frame();
        for (Client& client : clients)
            client.frame();
    }
    const auto count = [&](std::string_view needle) {
        return std::count_if(log.lines.begin(), log.lines.end(),
                             [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    };
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    // The two members went in; the third was refused, and is still in the
    // town.
    CHECK(count("crypt-arrived") == 2);
    CHECK(log.contains("Players could not be sent to i-1"));
    CHECK(log.contains("town-stayed:false:false:true"));
}

// An instance's scene, empty: what a match's is loaded again with is the
// match.
constexpr std::string_view ArenaScene = R"json({"format":"scene","version":2,"root":{"children":[]}})json";

TEST_CASE("ADR 0243: a queue matches a party and two players into an instance, rates the winners, and fills "
          "a place a player left")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(
        R"([manager]
public_address = "memory"
port = 47940
world_ports = "47941-47959"
region = "eu"
[[world]]
name = "town"
entry = true
[[instance]]
template = "arena"
scene = "scenes/arena.scene.json"
max_instances = 2
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    REQUIRE(config->region == "eu");
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    // **The town**: a party of its first two players and the next three on
    // their own queue for two against two; the fifth waits.
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        local MatchmakingService = game:GetService("MatchmakingService")
        local PartyService = game:GetService("PartyService")
        MatchmakingService:DefineQueue("Duel", { Template = "arena", Teams = 2, TeamSize = 2, Rating = "Arena" })
        MatchmakingService.Matched:Connect(function(match)
            print(`town-matched:{#match.Teams[1]}:{#match.Teams[2]}:{match.Instance}`)
        end)
        local here = {}
        NetworkService.PlayerAdded:Connect(function(player)
            table.insert(here, player)
            if #here ~= 5 then
                return
            end
            local party = PartyService:CreateAsync(here[1])
            PartyService:InviteAsync(party.Id, here[1], here[2])
            PartyService:AcceptAsync(party.Id, here[2])
            print(`town-no-queue:{select(2, MatchmakingService:QueueAsync(here[3], "Nothing"))}`)
            local ticket = MatchmakingService:QueueAsync(party.Id, "Duel")
            print(`town-party-ticket:{ticket ~= nil}`)
            for at = 3, 5 do
                MatchmakingService:QueueAsync(here[at], "Duel")
            end
        end)
    )";
    // **The arena**: the match from its load data; two wins for the first
    // team once all four are in; then one of the second team sent home, and
    // their place asked filled.
    const std::string arena = R"(
        local NetworkService = game:GetService("NetworkService")
        local MatchmakingService = game:GetService("MatchmakingService")
        local PartyService = game:GetService("PartyService")
        local DataService = game:GetService("DataService")
        local SceneService = game:GetService("SceneService")
        local arrived = 0
        NetworkService.TravelArrived:Connect(function(player)
            arrived += 1
            print(`arena-arrived:{arrived}`)
            if arrived ~= 4 then
                return
            end
            local match = SceneService:GetLoadData().Match
            print(`arena-match:{#match.Teams}:{typeof(match.Id)}`)
            MatchmakingService:ReportResultAsync(match.Id, 1)
            MatchmakingService:ReportResultAsync(match.Id, 1)
            local winner = match.Teams[1][1]
            local loser = match.Teams[2][1]
            local rated = MatchmakingService:GetRatingAsync(winner, "Arena")
            print(`arena-winner:{string.format("%.2f", rated)}`)
            print(`arena-loser:{string.format("%.2f", MatchmakingService:GetRatingAsync(loser, "Arena"))}`)
            print(`arena-kept:{string.format("%.2f", DataService:GetAsync("rating", "Arena:" .. winner))}`)
            for _, one in NetworkService:GetPlayers() do
                if PartyService:MemberIdOf(one) == loser then
                    NetworkService:Travel(one, "town")
                end
            end
            task.wait(1)
            local place = MatchmakingService:BackfillAsync(match.Id, 2)
            print(`arena-place:{place ~= nil}`)
        end)
    )";
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.dataStore = []() { return app::makeMemoryDataStore(); };
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        machines.push_back(std::make_unique<WorldMachine>());
        machines.back()->project.write("content/scenes/arena.scene.json", ArenaScene);
        machines.back()->boot(wire, launch, launch.name == "town" ? town : arena);
        return std::make_unique<FakeProcess>(*machines.back());
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    std::array<Client, 5> clients;
    for (std::size_t index = 0; index < clients.size(); ++index) {
        // A player's copy of the game has the scene they travel into.
        clients[index].project.write("content/scenes/arena.scene.json", ArenaScene);
        clients[index].boot(wire, R"(
            local NetworkService = game:GetService("NetworkService")
            task.wait()" + std::to_string(0.2 + 0.2 * static_cast<double>(index)) +
                                      R"()
            NetworkService:Join("memory:47940")
        )");
    }
    for (int at = 0; at < 20 * 60; ++at) {
        now += 16'000'000ull;
        manager.frame();
        for (std::size_t index = 0; index < machines.size(); ++index)
            machines[index]->frame();
        for (Client& client : clients)
            client.frame();
    }
    const auto count = [&](std::string_view needle) {
        return std::count_if(log.lines.begin(), log.lines.end(),
                             [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    };
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("town-no-queue:NoQueue"));
    CHECK(log.contains("town-party-ticket:true"));
    // **Matched**: two teams of two, heard in the town where the four were,
    // and all four in the arena.
    CHECK(log.contains("town-matched:2:2:i-1"));
    CHECK(log.contains("arena-match:2:string"));
    // Two wins from 1,500, by Elo's numbers, kept in the data service.
    CHECK(log.contains("arena-winner:1530.53"));
    CHECK(log.contains("arena-loser:1469.47"));
    CHECK(log.contains("arena-kept:1530.53"));
    // **The place a player left**, filled by the one who waited.
    CHECK(log.contains("arena-place:true"));
    CHECK(count("town-matched:2:2:i-1") == 2);
    CHECK(log.contains("arena-arrived:5"));
}

TEST_CASE("ADR 0243: eight players gather in a lobby by its code, and start once all are ready, in one instance")
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
template = "arena"
max_instances = 2
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        local MatchmakingService = game:GetService("MatchmakingService")
        local changes = 0
        MatchmakingService.LobbyChanged:Connect(function(lobby)
            changes += 1
            if #lobby.Members == 0 then
                print(`town-lobby-gone:{changes > 8}`)
            end
        end)
        local here = {}
        NetworkService.PlayerAdded:Connect(function(player)
            table.insert(here, player)
            if #here ~= 8 then
                return
            end
            local lobby = MatchmakingService:CreateLobbyAsync(here[1], { Template = "arena", Size = 8, Public = true })
            print(`town-code:{#lobby.Code}`)
            for at = 2, 8 do
                MatchmakingService:JoinLobbyAsync(here[at], string.lower(lobby.Code))
            end
            local listed = MatchmakingService:ListLobbiesAsync({ Template = "arena" })
            print(`town-listed:{#listed}:{#listed[1].Members}`)
            for at = 1, 7 do
                MatchmakingService:SetReadyAsync(here[at], true)
            end
            print(`town-early:{select(2, MatchmakingService:StartLobbyAsync(lobby.Id, here[1]))}`)
            print(`town-not-owner:{select(2, MatchmakingService:StartLobbyAsync(lobby.Id, here[2]))}`)
            MatchmakingService:SetReadyAsync(here[8], true)
            local match = MatchmakingService:StartLobbyAsync(lobby.Id, here[1])
            print(`town-started:{#match.Teams[1]}:{match.Instance}`)
        end)
    )";
    const std::string arena = R"(
        local NetworkService = game:GetService("NetworkService")
        local arrived = 0
        NetworkService.TravelArrived:Connect(function()
            arrived += 1
            print(`arena-arrived:{arrived}`)
        end)
    )";
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        machines.push_back(std::make_unique<WorldMachine>());
        machines.back()->boot(wire, launch, launch.name == "town" ? town : arena);
        return std::make_unique<FakeProcess>(*machines.back());
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    std::array<Client, 8> clients;
    for (std::size_t index = 0; index < clients.size(); ++index)
        clients[index].boot(wire, R"(
            local NetworkService = game:GetService("NetworkService")
            task.wait()" + std::to_string(0.2 + 0.1 * static_cast<double>(index)) +
                                      R"()
            NetworkService:Join("memory:47940")
        )");
    for (int at = 0; at < 12 * 60; ++at) {
        now += 16'000'000ull;
        manager.frame();
        for (std::size_t index = 0; index < machines.size(); ++index)
            machines[index]->frame();
        for (Client& client : clients)
            client.frame();
    }
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("town-code:6"));
    CHECK(log.contains("town-listed:1:8"));
    CHECK(log.contains("town-early:NotReady"));
    CHECK(log.contains("town-not-owner:NotOwner"));
    CHECK(log.contains("town-started:8:i-1"));
    CHECK(log.contains("town-lobby-gone:true"));
    CHECK(log.contains("arena-arrived:8"));
}

TEST_CASE("ADR 0239 and 0240: a guild across worlds, its chat heard once in each, whispers, the rate, the filter, a "
          "mute and the history")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(Servers, "servers.toml", error);
    REQUIRE(config.has_value());

    // **The town**: names its first three players, makes a guild of them --
    // the third invited by member id and sent to the fields -- and changes
    // it from two threads at once.
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        local PartyService = game:GetService("PartyService")
        local chat = require("@engine/chat")
        local guild = require("@engine/guild")
        chat.start({
            Filter = function(text: string, from: Player): string?
                return if string.find(text, "forbidden") then nil else text
            end,
        })
        local names = { "Ana", "Bo", "Cy", "Dee" }
        local here = {}
        NetworkService.PlayerAdded:Connect(function(player)
            table.insert(here, player)
            player.Name = names[#here] or player.Name
            if #here ~= 3 then
                return
            end
            local g = guild.create(here[1], "Ironhold") :: string
            print(`town-guild:{g ~= nil}`)
            print(`town-taken:{select(2, guild.create(here[2], "IRONHOLD"))}`)
            guild.addRank(g, "Officer", { "invite", "bank.withdraw" })
            print(`town-can-before:{guild.can(g, here[2], "invite")}`)
            guild.invite(g, here[1], here[2])
            guild.join(g, here[2])
            local _, refused = guild.invite(g, here[2], "nobody")
            print(`town-refused:{refused}`)
            guild.invite(g, here[1], PartyService:MemberIdOf(here[3]))
            NetworkService:Travel(here[3], "fields", { Name = "Cy" })
            -- **Two changes at once**: both kept.
            local done = 0
            task.spawn(function()
                guild.setRank(g, here[1], here[2], "Officer")
                done += 1
            end)
            task.spawn(function()
                guild.addRank(g, "Banker", { "bank.withdraw" })
                done += 1
            end)
            while done < 2 do
                task.wait()
            end
            local info = guild.get(g) :: guild.Info
            print(`town-both:{info.Members[PartyService:MemberIdOf(here[2])]}:{info.Ranks.Banker ~= nil}`)
            print(`town-can-after:{guild.can(g, here[2], "invite")}:{guild.can(g, here[2], "kick")}`)
        end)
    )";
    // **The fields**: a traveller keeps their name, and joins the guild they
    // were invited into.
    const std::string fields = R"(
        local NetworkService = game:GetService("NetworkService")
        local PartyService = game:GetService("PartyService")
        local chat = require("@engine/chat")
        local guild = require("@engine/guild")
        chat.start({
            Filter = function(text: string, from: Player): string?
                return if string.find(text, "forbidden") then nil else text
            end,
        })
        local known: { [string]: boolean } = {}
        guild.changed:Connect(function(id)
            known[id] = true
        end)
        NetworkService.TravelArrived:Connect(function(player, data)
            if typeof(data) == "table" and typeof(data.Name) == "string" then
                player.Name = data.Name
            end
            task.wait(0.5)
            for id in known do
                local info = guild.get(id)
                if info and info.Invited[PartyService:MemberIdOf(player)] then
                    print(`fields-joined:{guild.join(id, player)}`)
                end
            end
        end)
    )";
    Project managerRoot;
    std::map<std::string, std::unique_ptr<WorldMachine>> worlds;
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.projectId = {};
    options.prologue = std::vector<core::u8>{};
    options.dataStore = []() { return app::makeMemoryDataStore(); };
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

    // **The players' machines**, each saying what it hears; what each says
    // is after the guild is made, in the order the lines are listed.
    const auto joiner = [](int index, double joinAt, std::string_view says) {
        return std::string(R"(
            local NetworkService = game:GetService("NetworkService")
            local chat = require("@engine/chat")
            chat.messageReceived:Connect(function(m)
                print(`c)") +
               std::to_string(index) + R"(:{m.Channel}:{m.FromName}:{m.Text}`)
            end)
            chat.noticeReceived:Connect(function(n)
                print(`c)" +
               std::to_string(index) + R"(-notice:{n}`)
            end)
            -- In each world it is in, the machine holds that world's remote
            -- alone.
            NetworkService.Connected:Connect(function()
                task.wait(1)
                local remotes = 0
                for _, child in game:GetService("ReplicatedStorage"):GetChildren() do
                    if child.Name == "EngineChat" then
                        remotes += 1
                    end
                end
                print(`c)" +
               std::to_string(index) + R"(-remotes:{remotes}`)
            end)
            task.wait()" +
               std::to_string(joinAt) + R"()
            NetworkService:Join("memory:47981")
            task.spawn(function()
                chat.listen()
            end)
            )" +
               std::string(says);
    };
    const std::string first = joiner(1, 0.2, R"(
            task.wait(10)
            chat.send("guild", "the gates are open")
            chat.whisper("Cy", "meet at the well")
            chat.whisper("Nobody", "hello?")
            task.wait(7)
            for _, text in { "forbidden here", "one", "two", "three", "four", "five" } do
                chat.send("world", text)
            end
    )");
    const std::string second = joiner(2, 0.4, R"(
            task.wait(24)
            chat.send("world", "muted line")
    )");
    const std::string third = joiner(3, 0.6, "");
    const std::string fourth = joiner(4, 28.0, "");
    std::array<Client, 4> clients;
    clients[0].boot(wire, first);
    clients[1].boot(wire, second);
    clients[2].boot(wire, third);
    clients[3].boot(wire, fourth);
    const auto step = [&]() {
        now += 16'000'000ull;
        manager.frame();
        for (auto& [name, machine] : worlds)
            machine->frame();
        for (Client& client : clients)
            client.frame();
    };
    // Twenty-two seconds, then the second player muted, as the console
    // would, in every world.
    for (int at = 0; at < 22 * 60; ++at)
        step();
    const std::string muted = "key:" + core::toHex(clients[1].network->security().identity.pub);
    for (auto& [name, machine] : worlds)
        machine->host->world().engineState().muted.push_back(muted);
    for (int at = 0; at < 14 * 60; ++at)
        step();

    const auto count = [&](std::string_view needle) {
        return std::count_if(log.lines.begin(), log.lines.end(),
                             [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    };
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    // The guild: made once, its name taken in any case, a rank's permissions
    // checked, both of two changes at once kept, and its member in the
    // fields in it.
    CHECK(log.contains("town-guild:true"));
    CHECK(log.contains("town-taken:Taken"));
    CHECK(log.contains("town-can-before:false"));
    CHECK(log.contains("town-refused:NotAllowed"));
    CHECK(log.contains("town-both:Officer:true"));
    CHECK(log.contains("town-can-after:true:false"));
    CHECK(log.contains("fields-joined:true"));
    // **A traveller's machine** keeps nothing of the world it left: one
    // remote after the hop, as before it.
    CHECK(count("c3-remotes:1") == 2);
    CHECK(count("-remotes:2") == 0);
    // **Guild chat**: once to each member, in either world.
    CHECK(count("c2:guild:Ana:the gates are open") == 1);
    CHECK(count("c3:guild:Ana:the gates are open") == 1);
    CHECK(count("c1:guild:Ana:the gates are open") == 1);
    // A whisper across worlds, and one to nobody online.
    CHECK(count("c3:whisper:Ana:meet at the well") == 1);
    CHECK(log.contains("c1-notice:NotOnline"));
    // The filter's nil drops a line everywhere; the sixth in five seconds is
    // dropped and its speaker told.
    CHECK(count("forbidden here") == 0);
    CHECK(log.contains("c2:world:Ana:four"));
    CHECK(count(":world:Ana:five") == 0);
    CHECK(log.contains("c1-notice:TooFast"));
    // A muted player's line reaches nobody, and they are told.
    CHECK(count("muted line") == 0);
    CHECK(log.contains("c2-notice:Muted"));
    // **Joining**: the world's last lines.
    CHECK(log.contains("c4:world:Ana:one"));
    CHECK(log.contains("c4:world:Ana:four"));
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

namespace {

// **A world that says what a test tells it to** (ADR 0230): what the manager
// writes it is kept, line by line; what it says is queued by the test.
struct Scripted
{
    std::vector<std::string> heard;
    std::string said;
    bool killed = false;

    void say(const core::JsonWriter& message) { said += app::managerLine(message); }
    void beat(core::u32 players)
    {
        core::JsonWriter beat(core::JsonLayout::Compact);
        beat.beginObject();
        beat.field("type", "heartbeat");
        beat.field("players", static_cast<core::i64>(players));
        beat.field("tickMs", 1.0);
        beat.endObject();
        say(beat);
    }
    void joined(std::string_view key)
    {
        core::JsonWriter joined(core::JsonLayout::Compact);
        joined.beginObject();
        joined.field("type", "player-joined");
        joined.field("key", key);
        joined.field("account", "");
        joined.endObject();
        say(joined);
    }
    // The last message of `type` the manager wrote it, as text.
    [[nodiscard]] std::string last(std::string_view type) const
    {
        const std::string marker = std::string("\"type\":\"") + std::string(type) + "\"";
        for (auto at = heard.rbegin(); at != heard.rend(); ++at) {
            if (at->find(marker) != std::string::npos)
                return *at;
        }
        return {};
    }
};

class ScriptedProcess final : public app::WorldProcess
{
public:
    explicit ScriptedProcess(Scripted& world) : m_world(world) {}
    bool write(std::string_view text) override
    {
        m_world.heard.emplace_back(text);
        return true;
    }
    [[nodiscard]] std::string read() override
    {
        std::string out = std::move(m_world.said);
        m_world.said.clear();
        return out;
    }
    [[nodiscard]] bool running() override { return !m_world.killed; }
    void kill() override { m_world.killed = true; }
    void closeInput() override {}

private:
    Scripted& m_world;
};

} // namespace

TEST_CASE("ADR 0230: a full world opens another channel, players go to the fullest with room, a party together, "
          "and an emptied channel closes or is merged")
{
    Captured log;
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(
        R"([manager]
public_address = "memory"
port = 47961
world_ports = "47962-47969"
[[world]]
name = "town"
entry = true
max_players = 10
channels = true
max_channels = 3
channel_open_at = 0.9
channel_close_seconds = 5
merge_minutes = 0.05
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    CHECK(config->worlds[0].channels);
    CHECK(config->worlds[0].maxChannels == 3);
    Project managerRoot;
    // Every world started, kept for the whole case; and how many players
    // each says it holds, a heartbeat a second.
    std::vector<std::unique_ptr<Scripted>> started;
    std::map<std::string, Scripted*> worlds;
    std::map<std::string, core::u32> holding;
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        started.push_back(std::make_unique<Scripted>());
        Scripted* world = started.back().get();
        worlds[launch.name] = world;
        core::JsonWriter ready(core::JsonLayout::Compact);
        ready.beginObject();
        ready.field("type", "ready");
        ready.field("port", static_cast<core::i64>(launch.port));
        ready.field("key", "k-" + launch.name);
        ready.field("protocol", static_cast<core::i64>(0));
        ready.endObject();
        world->say(ready);
        // A channel's saves are its world's; its log, a name a file may have.
        CHECK(launch.saves.filename().string() == "town");
        CHECK(launch.logFile.string().find(':', 3) == std::string::npos);
        return std::make_unique<ScriptedProcess>(*world);
    };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    const auto run = [&](core::u64 seconds) {
        for (core::u64 at = 0; at < seconds * 4; ++at) {
            now += 250'000'000ull;
            if (at % 4 == 0) {
                for (const auto& [name, world] : worlds)
                    world->beat(holding[name]);
            }
            manager.frame();
        }
    };
    run(1);
    REQUIRE(worlds.contains("town"));
    Scripted& town = *worlds["town"];

    // **Nearly full: channel 2 opens.**
    holding["town"] = 9;
    run(1);
    REQUIRE(worlds.contains("town:2"));
    run(1);
    Scripted& second = *worlds["town:2"];
    CHECK(log.contains("Opening channel 2 of town"));
    run(1);
    // Not another while one has room.
    CHECK_FALSE(worlds.contains("town:3"));

    // **The fullest with room**: one more fits in the town.
    const auto travel = [&](Scripted& from, core::u32 id, std::vector<std::string> keys, std::string extra) {
        std::string players;
        for (const std::string& key : keys)
            players += (players.empty() ? "" : ",") + std::string("{\"key\":\"") + key + "\",\"account\":\"\"}";
        from.said += std::string("\x1e") + "{\"type\":\"travel\",\"id\":" + std::to_string(id) +
                     ",\"target\":\"town\",\"data\":\"\",\"players\":[" + players + "]" + extra + "}\n";
        run(1);
        return from.last("passes");
    };
    CHECK(travel(town, 1, {"a"}, "").find("\"world\":\"town\"") != std::string::npos);
    // A party of two no longer does, and goes together to channel 2.
    CHECK(travel(town, 2, {"b", "c"}, "").find("\"world\":\"town:2\"") != std::string::npos);
    // To a friend, wherever they are.
    second.joined("friend");
    holding["town:2"] = 3;
    run(1);
    CHECK(travel(town, 3, {"d"}, ",\"toPlayer\":\"friend\"").find("\"world\":\"town:2\"") != std::string::npos);
    // A channel by its number.
    CHECK(travel(second, 4, {"friend"}, ",\"channel\":1").find("\"world\":\"town\"") != std::string::npos);

    // **The list**, for a menu.
    second.said += std::string("\x1e") + "{\"type\":\"channels\",\"id\":9}\n";
    run(1);
    const std::string list = second.last("channels");
    CHECK(list.find("\"channel\":1") != std::string::npos);
    CHECK(list.find("\"channel\":2") != std::string::npos);

    // **Under a quarter full for its while: warned, then merged**, everybody
    // to the fullest channel with room. The travellers above never arrived:
    // their passes lapse first, and nobody is on the way.
    run(31);
    holding["town"] = 5;
    holding["town:2"] = 1;
    run(4);
    CHECK(second.last("channel-merging").find("\"seconds\":60") != std::string::npos);
    CHECK(log.contains("has been under a quarter full"));
    run(61);
    CHECK_FALSE(second.last("drain").empty());
    CHECK(second.last("passes").find("\"world\":\"town\"") != std::string::npos);
    CHECK(second.last("passes").find("\"key\":\"friend\"") != std::string::npos);

    // **Empty for its while: closed.** Channel 3, opened and left empty.
    holding["town"] = 10;
    holding["town:2"] = 0;
    run(1);
    REQUIRE(worlds.contains("town:3"));
    run(1);
    Scripted& third = *worlds["town:3"];
    holding["town"] = 3;
    run(7);
    CHECK_FALSE(third.last("stop").empty());
    CHECK(log.contains("Channel town:3 has been empty"));
}

TEST_CASE("ADR 0230: a channel knows which it is, lists its world's channels, and hears that it will be merged")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    WorldMachine machine;
    app::WorldLaunch launch;
    launch.name = "town:2";
    launch.port = 47971;
    std::array<core::u8, 32> managerKey{};
    launch.managerKey = core::base64Encode(managerKey);
    machine.boot(wire, launch, R"(
        local NetworkService = game:GetService("NetworkService")
        print(`channel:{NetworkService.Channel}`)
        NetworkService.ChannelMerging:Connect(function(seconds: number)
            print(`merging:{seconds}`)
        end)
        task.wait(0.1)
        for _, channel in NetworkService:GetChannelsAsync() do
            print(`listed:{channel.Channel}:{channel.Players}:{channel.MaxPlayers}`)
        end
    )");
    for (int at = 0; at < 30; ++at)
        machine.frame();
    CHECK(log.contains("channel:2"));
    // It asked the manager.
    CHECK(machine.output.find("\"type\":\"channels\"") != std::string::npos);
    machine.input.push_back(std::string("\x1e") +
                            "{\"type\":\"channels\",\"id\":1,\"channels\":[{\"channel\":1,\"players\":40,\"max\":50},"
                            "{\"channel\":2,\"players\":7,\"max\":50}]}");
    machine.input.push_back(std::string("\x1e") + "{\"type\":\"channel-merging\",\"seconds\":60}");
    for (int at = 0; at < 10; ++at)
        machine.frame();
    CHECK(log.contains("listed:1:40:50"));
    CHECK(log.contains("listed:2:7:50"));
    CHECK(log.contains("merging:60"));
}

TEST_CASE("a player who leaves while the server waits is not sent, and is no error (ADR 0230)")
{
    // The trap every game meets: a party member leaves while
    // `CreateInstanceAsync` waits, and the travel after raised on them.
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(
        R"([manager]
public_address = "memory"
port = 47975
world_ports = "47976-47979"
[[world]]
name = "town"
entry = true
[[world]]
name = "fields"
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    const std::string town = R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.TravelArrived:Connect(function(player)
            task.wait(1)
            local sent = NetworkService:TravelGroup({ player }, "fields")
            print(`group-sent:{#sent}`)
            print(`one-sent:{NetworkService:Travel(player, "fields")}`)
            print(`switched:{NetworkService:SwitchChannel(player, 1)}`)
        end)
    )";
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        machines.push_back(std::make_unique<WorldMachine>());
        machines.back()->boot(wire, launch, launch.name == "town" ? town : std::string{});
        return std::make_unique<FakeProcess>(*machines.back());
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
        task.wait(0.2)
        NetworkService:Join("memory:47975")
        NetworkService.Connected:Connect(function()
            task.wait(0.3)
            NetworkService:Disconnect()
        end)
    )");
    for (int at = 0; at < 300; ++at) {
        now += 16'000'000ull;
        manager.frame();
        for (std::size_t index = 0; index < machines.size(); ++index)
            machines[index]->frame();
        client.frame();
    }
    CHECK(log.contains("group-sent:0"));
    CHECK(log.contains("one-sent:false"));
    CHECK(log.contains("switched:false"));
    CHECK_FALSE(log.contains("instance_dead"));
}

TEST_CASE("ADR 0232: a full game keeps a line at its door, says where each stands, and lets them in in order")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(
        R"([manager]
public_address = "memory"
port = 47985
world_ports = "47986-47989"
queue_grace_seconds = 30
[[world]]
name = "town"
entry = true
max_players = 2
)",
        "servers.toml", error);
    REQUIRE(config.has_value());
    CHECK(config->queueGraceSeconds == 30);
    Project managerRoot;
    std::vector<std::unique_ptr<WorldMachine>> machines;
    app::WorldManager::Options options;
    options.config = *config;
    options.root = managerRoot.root;
    options.prologue = std::vector<core::u8>{};
    options.launcher = [&](const app::WorldLaunch& launch) -> std::unique_ptr<app::WorldProcess> {
        machines.push_back(std::make_unique<WorldMachine>());
        machines.back()->boot(wire, launch, std::string{});
        return std::make_unique<FakeProcess>(*machines.back());
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    // Four players, a quarter of a second apart; the first leaves after a
    // while, freeing a place.
    std::vector<std::unique_ptr<Client>> clients;
    for (int at = 0; at < 4; ++at) {
        clients.push_back(std::make_unique<Client>());
        clients.back()->boot(wire, "local NetworkService = game:GetService(\"NetworkService\")\n"
                                   "local me = " +
                                       std::to_string(at + 1) +
                                       "\n"
                                       "NetworkService.Queued:Connect(function(position, estimate)\n"
                                       "    print(`queued:{me}:{position}:{estimate ~= nil}`)\n"
                                       "end)\n"
                                       "NetworkService.Connected:Connect(function()\n"
                                       "    print(`in:{me}`)\n"
                                       "    if me == 1 then\n"
                                       "        task.wait(8)\n"
                                       "        NetworkService:Disconnect()\n"
                                       "    end\n"
                                       "end)\n"
                                       "task.wait(" +
                                       std::to_string(0.2 + 0.25 * at) +
                                       ")\n"
                                       "NetworkService:Join(\"memory:47985\")\n");
        clients.back()->network->setJoinTimeout(5.0);
    }
    // **A fifth, who drops while waiting and comes back inside the grace**,
    // and a sixth who came after them: the fifth keeps their place.
    clients.push_back(std::make_unique<Client>());
    clients.back()->boot(wire, R"(
        local NetworkService = game:GetService("NetworkService")
        local back = false
        NetworkService.Queued:Connect(function(position)
            print(`queued:{if back then "5back" else "5"}:{position}`)
        end)
        task.wait(1.2)
        NetworkService:Join("memory:47985")
        task.wait(2)
        NetworkService:Disconnect()
        task.wait(0.5)
        back = true
        NetworkService:Join("memory:47985")
    )");
    clients.back()->network->setJoinTimeout(5.0);
    clients.push_back(std::make_unique<Client>());
    clients.back()->boot(wire, R"(
        local NetworkService = game:GetService("NetworkService")
        NetworkService.Queued:Connect(function(position)
            print(`queued:6:{position}`)
        end)
        task.wait(1.5)
        NetworkService:Join("memory:47985")
    )");
    clients.back()->network->setJoinTimeout(5.0);
    const auto run = [&](int frames) {
        for (int at = 0; at < frames; ++at) {
            now += 16'000'000ull;
            manager.frame();
            for (std::size_t index = 0; index < machines.size(); ++index)
                machines[index]->frame();
            for (const auto& client : clients)
                client->frame();
        }
    };
    run(400);
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("in:1"));
    CHECK(log.contains("in:2"));
    // The two after them wait, in the order they came, with no estimate yet:
    // nobody has left the line.
    CHECK(log.contains("queued:3:1:false"));
    CHECK(log.contains("queued:4:2:false"));
    CHECK_FALSE(log.contains("in:3"));
    CHECK(log.contains("queued:5:3"));
    CHECK(log.contains("queued:5back:3"));
    CHECK(log.contains("queued:6:4"));
    CHECK_FALSE(log.contains("queued:6:3"));
    // One key, one place: coming back did not add a second entry behind.
    CHECK_FALSE(log.contains("queued:6:5"));
    // Waiting longer than the join's own timeout is not a failed join.
    CHECK_FALSE(log.contains("JoinFailed"));
    // The first leaves: the first in line goes in, and the next moves up.
    run(500);
    CHECK(log.contains("in:3"));
    CHECK_FALSE(log.contains("in:4"));
    CHECK(log.contains("queued:4:1:true"));
}

TEST_CASE("ADR 0232: past queue_max a newcomer is refused with words, and the line does not grow")
{
    Captured log;
    auto wire = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    const std::optional<app::ServersConfig> config = app::readServersConfig(
        R"([manager]
public_address = "memory"
port = 47990
world_ports = "47991-47993"
queue_max = 1
[[world]]
name = "town"
entry = true
max_players = 1
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
        machines.back()->boot(wire, launch, std::string{});
        return std::make_unique<FakeProcess>(*machines.back());
    };
    options.transports = [wire]() { return net::createMemoryTransport(wire); };
    core::u64 now = 1;
    options.clock = [&now]() { return now; };
    options.unixClock = []() { return unixNow(); };
    app::WorldManager manager(std::move(options));
    REQUIRE_FALSE(manager.start().has_value());
    std::vector<std::unique_ptr<Client>> clients;
    for (int at = 0; at < 3; ++at) {
        std::string script = R"(
            local NetworkService = game:GetService("NetworkService")
            NetworkService.Queued:Connect(function(position)
                print(`queued:ME:{position}`)
            end)
            NetworkService.Connected:Connect(function()
                print("in:ME")
            end)
            NetworkService.JoinFailed:Connect(function(why)
                print(`refused:ME:{why}`)
            end)
            task.wait(DELAY)
            NetworkService:Join("memory:47990")
        )";
        script.replace(script.find("ME"), 2, std::to_string(at + 1));
        script.replace(script.find("ME"), 2, std::to_string(at + 1));
        script.replace(script.find("ME"), 2, std::to_string(at + 1));
        script.replace(script.find("DELAY"), 5, std::to_string(0.2 + 0.3 * at));
        clients.push_back(std::make_unique<Client>());
        clients.back()->boot(wire, script);
        clients.back()->network->setJoinTimeout(5.0);
    }
    for (int at = 0; at < 300; ++at) {
        now += 16'000'000ull;
        manager.frame();
        for (std::size_t index = 0; index < machines.size(); ++index)
            machines[index]->frame();
        for (const auto& client : clients)
            client->frame();
    }
    CHECK(log.contains("in:1"));
    CHECK(log.contains("queued:2:1"));
    CHECK(log.contains("refused:3:The game is full, and so is its line."));
    CHECK_FALSE(log.contains("queued:3:"));
}
