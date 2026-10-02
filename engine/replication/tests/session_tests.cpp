// Two worlds, one authority (ADR 0069): the sessions over the memory transport.
//
// **Every case here is a function of the operation sequence.** The memory
// transport delivers at the next poll and the lossy one misbehaves from a seed,
// so a failure reproduces exactly -- which is the property a replication bug
// most needs and a socket least provides.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <doctest/doctest.h>
#include <limits>
#include <span>
#include <string>

#include "../../scene/tests/scene_fixture.h"
#include "class_descriptors.gen.h"
#include "engine/asset/material.h"
#include "engine/asset/terrain.h"
#include "engine/asset/terrain_rules.h"
#include "engine/asset/voxel.h"
#include "engine/core/i18n.h"
#include "engine/net/memory_transport.h"
#include "engine/replication/extract.h"
#include "engine/replication/replication.h"
#include "engine/replication/session.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"

using namespace engine;
using namespace engine::replication;

namespace {

constexpr core::u16 Port = 7100;

void seedCatalog()
{
    const auto result = core::engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

// One side: its own registry, its own atoms, its own world, and a folder that
// is the replicated root.
struct Side
{
    scene::testing::Fixture fixture;
    core::InstanceId root;

    Side() { root = fixture.folder("Root"); }

    [[nodiscard]] scene::World& world() { return fixture.world; }

    core::InstanceId part(std::string_view name, core::DVec3 at, core::InstanceId parent)
    {
        const core::InstanceId id = fixture.part(name);
        fixture.world.rigidBodies().add(id, scene::RigidBodyComponent{});
        fixture.world.parts().find(id)->cframe.position = at;
        REQUIRE_FALSE(fixture.world.setParent(id, parent).has_value());
        return id;
    }

    // The child of `parent` called `name`, or nothing.
    [[nodiscard]] core::InstanceId child(core::InstanceId parent, std::string_view name)
    {
        const core::NameAtom atom = fixture.atom(name);
        for (core::InstanceId at = fixture.world.firstChild(parent); at.valid(); at = fixture.world.nextSibling(at)) {
            if (fixture.world.name(at) == atom)
                return at;
        }
        return {};
    }
};

// An authority and one replica on a memory network. `loss` wraps the
// authority's transport, which is the direction snapshots travel.
struct Match
{
    std::shared_ptr<net::MemoryNetwork> network = net::createMemoryNetwork();
    std::unique_ptr<net::ITransport> serverTransport;
    std::unique_ptr<net::ITransport> clientTransport = net::createMemoryTransport(network);
    std::optional<AuthoritySession> authority;
    std::optional<ReplicaSession> replica;
    Side server;
    Side client;
    core::u64 tick = 0;

    explicit Match(const net::LossConfig* loss = nullptr)
    {
        seedCatalog();
        serverTransport = loss != nullptr ? net::createLossyTransport(net::createMemoryTransport(network), *loss)
                                          : net::createMemoryTransport(network);
        REQUIRE_FALSE(
            serverTransport->open(net::TransportConfig{.port = Port, .maxPeers = 4, .channels = 4}).has_value());
        REQUIRE_FALSE(clientTransport->open(net::TransportConfig{.port = 0, .maxPeers = 1, .channels = 4}).has_value());
        net::PeerId toServer;
        REQUIRE_FALSE(clientTransport->connect("memory", Port, toServer).has_value());
        authority.emplace(*serverTransport);
        replica.emplace(*clientTransport, toServer);
        // The transport's own semantics: each snapshot applied as it arrives.
        replica->setInterpolationDelay(0);
    }

    // One tick, in the order the frame runs it: what arrived, the tick, what
    // to send -- on both ends.
    void step()
    {
        tick += 1;
        authority->receive(server.world(), server.root);
        authority->send(server.world(), server.root, tick);
        replica->receive(client.world(), client.root);
    }

    void run(int ticks)
    {
        for (int at = 0; at < ticks; ++at)
            step();
    }
};

} // namespace

TEST_CASE("a replica is welcomed, and holds the authority's instances, named, parented and placed")
{
    Match match;
    const core::InstanceId car = match.server.fixture.model("Car");
    REQUIRE_FALSE(match.server.world().setParent(car, match.server.root).has_value());
    (void)match.server.part("Wheel", {1.0, 2.0, 3.0}, car);
    (void)match.server.part("Body", {0.0, 5.0, 0.0}, car);

    match.run(4);
    CHECK(match.replica->welcomed());
    CHECK(match.authority->peerCount() == 1);
    CHECK(match.replica->checksumFailures() == 0);

    // **By name, in the replica's own atoms.** The two fixtures intern in
    // different orders, so a name that travelled as the authority's atom
    // number would come out as some other word.
    const core::InstanceId replicaCar = match.client.child(match.client.root, "Car");
    REQUIRE(replicaCar.valid());
    CHECK(match.client.world().classOf(replicaCar) == match.client.fixture.schema.modelClass);
    const core::InstanceId wheel = match.client.child(replicaCar, "Wheel");
    REQUIRE(wheel.valid());
    const scene::PartComponent* part = match.client.world().parts().find(wheel);
    REQUIRE(part != nullptr);
    CHECK(part->cframe.position.x == doctest::Approx(1.0));
    CHECK(part->cframe.position.z == doctest::Approx(3.0));
    CHECK(match.client.child(replicaCar, "Body").valid());
}

TEST_CASE("a moved part moves on the replica, and a quiet world sends almost nothing")
{
    Match match;
    const core::InstanceId crate = match.server.part("Crate", {0.0, 0.0, 0.0}, match.server.root);
    match.run(4);

    // Nothing changes: a snapshot is a header and no records.
    const core::u64 before = match.authority->stats().bytesSent;
    match.run(1);
    const core::u64 quiet = match.authority->stats().bytesSent - before;
    CHECK(quiet < 64);

    match.server.world().parts().find(crate)->cframe.position = core::DVec3{4.0, 0.0, -2.0};
    const core::u64 moving = match.authority->stats().bytesSent;
    match.run(2);
    // One field of one instance: a record header and a transform, not a world.
    CHECK(match.authority->stats().bytesSent - moving < 2 * 64 + 128);

    const core::InstanceId replicaCrate = match.client.child(match.client.root, "Crate");
    REQUIRE(replicaCrate.valid());
    CHECK(match.client.world().parts().find(replicaCrate)->cframe.position.x == doctest::Approx(4.0));
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a renamed, reparented and destroyed instance is all three on the replica")
{
    Match match;
    const core::InstanceId shelf = match.server.fixture.folder("Shelf");
    REQUIRE_FALSE(match.server.world().setParent(shelf, match.server.root).has_value());
    const core::InstanceId box = match.server.part("Box", {0.0, 0.0, 0.0}, match.server.root);
    const core::InstanceId doomed = match.server.part("Doomed", {0.0, 0.0, 0.0}, match.server.root);
    match.run(4);
    REQUIRE(match.client.child(match.client.root, "Doomed").valid());

    match.server.world().setName(box, match.server.fixture.atom("Parcel"));
    REQUIRE_FALSE(match.server.world().setParent(box, shelf).has_value());
    REQUIRE(match.server.world().destroy(doomed));
    match.server.world().retireDestroyed();
    match.run(3);
    match.client.world().retireDestroyed();

    CHECK_FALSE(match.client.child(match.client.root, "Doomed").valid());
    CHECK_FALSE(match.client.child(match.client.root, "Box").valid());
    const core::InstanceId replicaShelf = match.client.child(match.client.root, "Shelf");
    REQUIRE(replicaShelf.valid());
    CHECK(match.client.child(replicaShelf, "Parcel").valid());
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("under loss and reordering the replica converges and never fails its checksum")
{
    // **The case `transport_tests.cpp` says is invisible without a decorator.**
    // A third of the snapshots lost and a fifth reordered: the replica must end
    // exactly where the authority is, having acknowledged only states it held.
    const net::LossConfig loss{.seed = 7, .dropPerMille = 330, .reorderPerMille = 200};
    Match match(&loss);
    const core::InstanceId mover = match.server.part("Mover", {0.0, 0.0, 0.0}, match.server.root);
    for (int at = 0; at < 8; ++at)
        (void)match.server.part("Still" + std::to_string(at), {static_cast<double>(at), 1.0, 0.0}, match.server.root);

    for (int at = 0; at < 200; ++at) {
        match.server.world().parts().find(mover)->cframe.position = core::DVec3{static_cast<double>(at), 0.0, 0.0};
        match.step();
    }
    // Held still long enough for a snapshot to get through.
    match.run(30);

    CHECK(match.replica->checksumFailures() == 0);
    const core::InstanceId replicaMover = match.client.child(match.client.root, "Mover");
    REQUIRE(replicaMover.valid());
    CHECK(match.client.world().parts().find(replicaMover)->cframe.position.x == doctest::Approx(199.0));
    CHECK(match.client.child(match.client.root, "Still7").valid());
    // Loss cost snapshots, not correctness: fewer arrived than were sent.
    CHECK(match.replica->stats().snapshotsReceived < match.authority->stats().snapshotsSent);
}

TEST_CASE("a peer speaking another protocol is refused before anything is parsed")
{
    seedCatalog();
    auto network = net::createMemoryNetwork();
    auto serverTransport = net::createMemoryTransport(network);
    auto rogue = net::createMemoryTransport(network);
    REQUIRE_FALSE(serverTransport->open(net::TransportConfig{.port = Port, .maxPeers = 4, .channels = 4}).has_value());
    REQUIRE_FALSE(rogue->open(net::TransportConfig{.port = 0, .maxPeers = 1, .channels = 4}).has_value());
    net::PeerId toServer;
    REQUIRE_FALSE(rogue->connect("memory", Port, toServer).has_value());

    AuthoritySession authority(*serverTransport);
    Side server;
    authority.receive(server.world(), server.root);
    // A hello claiming protocol 999.
    const core::u8 hello[] = {1, 0xE7, 0x03, 0x00, 0x00};
    REQUIRE_FALSE(rogue->send(toServer, hello, net::Delivery::Reliable, 0).has_value());
    authority.receive(server.world(), server.root);
    CHECK(authority.peerCount() == 0);
    CHECK(serverTransport->peerCount() == 0);
}

TEST_CASE("solo is no replication at all, and an impossible peer count is refused by name")
{
    seedCatalog();
    std::optional<core::EngineError> error;
    CHECK(createReplicationOver(net::createMemoryTransport(net::createMemoryNetwork()), Config{}, error) == nullptr);
    CHECK_FALSE(error.has_value());

    Config tooMany;
    tooMany.topology = Topology::Dedicated;
    tooMany.maxPeers = 100000;
    CHECK(createReplicationOver(net::createMemoryTransport(net::createMemoryNetwork()), tooMany, error) == nullptr);
    REQUIRE(error.has_value());
    CHECK(error->message.find("net.err.replication_peer_cap") != std::string::npos);
}

TEST_CASE("a replica whose connection drops dials again by itself, and a restarted server takes it back")
{
    // The redial half of ADR 0085. The server goes away and comes back as a
    // new process: it does not know the player's token, so the player is new
    // to it -- and the replica is in its world again without anybody at
    // either machine doing anything.
    seedCatalog();
    auto network = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    Config hostConfig;
    hostConfig.topology = Topology::Host;
    hostConfig.port = Port;
    auto host = createReplicationOver(net::createMemoryTransport(network), hostConfig, error);
    REQUIRE(host != nullptr);
    Config joinConfig;
    joinConfig.topology = Topology::Replica;
    joinConfig.port = Port;
    joinConfig.address = "memory";
    auto join = createReplicationOver(net::createMemoryTransport(network), joinConfig, error);
    REQUIRE(join != nullptr);

    Side server;
    Side client;
    (void)server.part("Beacon", {9.0, 0.0, 0.0}, server.root);
    core::u64 tick = 0;
    const auto run = [&](int ticks) {
        for (int at = 0; at < ticks; ++at) {
            tick += 1;
            host->receive(server.world(), server.root);
            host->send(server.world(), server.root, tick);
            join->receive(client.world(), client.root);
            join->send(client.world(), client.root, tick);
        }
    };
    run(8);
    REQUIRE(join->status().peerCount == 1);

    // The server process ends, and a new one takes the port.
    host->shutdown();
    host = createReplicationOver(net::createMemoryTransport(network), hostConfig, error);
    REQUIRE(host != nullptr);
    run(2);
    CHECK(join->status().peerCount == 0);

    // A second of ticks later the replica has dialled, been welcomed, and holds
    // the world again -- once, not twice.
    run(80);
    CHECK(join->status().peerCount == 1);
    CHECK(host->status().peerCount == 1);
    int beacons = 0;
    for (core::InstanceId at = client.world().firstChild(client.root); at.valid();
         at = client.world().nextSibling(at)) {
        if (client.world().name(at) == client.fixture.atom("Beacon"))
            ++beacons;
    }
    CHECK(beacons == 1);
}

TEST_CASE("createReplicationOver drives both postures through the seam")
{
    seedCatalog();
    auto network = net::createMemoryNetwork();
    std::optional<core::EngineError> error;
    Config hostConfig;
    hostConfig.topology = Topology::Host;
    hostConfig.port = Port;
    auto host = createReplicationOver(net::createMemoryTransport(network), hostConfig, error);
    REQUIRE(host != nullptr);
    Config joinConfig;
    joinConfig.topology = Topology::Replica;
    joinConfig.port = Port;
    joinConfig.address = "memory";
    auto join = createReplicationOver(net::createMemoryTransport(network), joinConfig, error);
    REQUIRE(join != nullptr);

    Side server;
    Side client;
    (void)server.part("Beacon", {9.0, 0.0, 0.0}, server.root);
    for (core::u64 tick = 1; tick <= 8; ++tick) {
        host->receive(server.world(), server.root);
        host->send(server.world(), server.root, tick);
        join->receive(client.world(), client.root);
        join->send(client.world(), client.root, tick);
    }
    CHECK(host->status().authority);
    CHECK(host->status().peerCount == 1);
    CHECK_FALSE(join->status().authority);
    CHECK(join->status().serverTick > 0);
    CHECK(client.child(client.root, "Beacon").valid());
}

namespace {

// A world with the engine's real classes: a data model, its `NetworkService`
// and a `Workspace`, which is the shape players need and the hand-built
// fixture does not have.
struct RealSide
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::World world{classes, enums, atoms, 99u};
    core::InstanceId dataModel;
    core::InstanceId network;
    core::InstanceId workspace;
    core::InstanceId lighting;

    RealSide()
    {
        scene::generated::registerClasses(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        // `Decal` is the renderer's class, and this module sits beside the
        // renderer rather than above it -- so the one class the wire carries
        // from there is declared here by hand, storing what the real one does.
        (void)classes.registerClass({
            .name = atoms.intern("Decal"),
            .super = classes.findId(atoms.intern("Instance")),
            .defaultName = atoms.intern("Decal"),
            .attachComponents = [](scene::World& world,
                                   core::InstanceId id) { world.decals().add(id, scene::DecalComponent{}); },
            .detachComponents = [](scene::World& world, core::InstanceId id) { world.decals().remove(id); },
        });
        // And `Lighting`, the service whose properties travel, for the same
        // reason.
        (void)classes.registerClass({
            .name = atoms.intern("Lighting"),
            .super = classes.findId(atoms.intern("Instance")),
            .defaultName = atoms.intern("Lighting"),
            .attachComponents = [](scene::World& world,
                                   core::InstanceId id) { world.lighting().add(id, scene::LightingComponent{}); },
            .detachComponents = [](scene::World& world, core::InstanceId id) { world.lighting().remove(id); },
        });
        // And the look's (ADR 0096): the abstract effect, the blur that stands
        // for all five on the wire, and the camera a viewer's effect sits on.
        const scene::ClassId effect = classes.registerClass({
            .name = atoms.intern("PostEffect"),
            .super = classes.findId(atoms.intern("Instance")),
            .defaultName = atoms.intern("PostEffect"),
            .attachComponents = [](scene::World& world,
                                   core::InstanceId id) { world.postEffects().add(id, scene::PostEffectComponent{}); },
            .detachComponents = [](scene::World& world, core::InstanceId id) { world.postEffects().remove(id); },
        });
        (void)classes.registerClass({
            .name = atoms.intern("BlurEffect"),
            .super = effect,
            .defaultName = atoms.intern("BlurEffect"),
            .attachComponents = [](scene::World& world,
                                   core::InstanceId id) { world.blurEffects().add(id, scene::BlurEffectComponent{}); },
            .detachComponents = [](scene::World& world, core::InstanceId id) { world.blurEffects().remove(id); },
        });
        (void)classes.registerClass({
            .name = atoms.intern("Camera"),
            .super = classes.findId(atoms.intern("Instance")),
            .defaultName = atoms.intern("Camera"),
            .attachComponents = [](scene::World& world,
                                   core::InstanceId id) { world.cameras().add(id, scene::CameraComponent{}); },
            .detachComponents = [](scene::World& world, core::InstanceId id) { world.cameras().remove(id); },
        });
        const auto make = [this](std::string_view name) {
            const core::InstanceId id = world.create(classes.findId(atoms.intern(name)));
            REQUIRE(id.valid());
            world.setName(id, atoms.intern(name));
            return id;
        };
        dataModel = make("DataModel");
        network = make("NetworkService");
        workspace = make("Workspace");
        lighting = make("Lighting");
        REQUIRE_FALSE(world.setParent(lighting, dataModel).has_value());
        REQUIRE_FALSE(world.setParent(network, dataModel).has_value());
        REQUIRE_FALSE(world.setParent(workspace, dataModel).has_value());
    }
};

} // namespace

TEST_CASE("a joined replica is a player on the authority, and what it does arrives as intent")
{
    seedCatalog();
    auto network = net::createMemoryNetwork();
    auto serverTransport = net::createMemoryTransport(network);
    auto clientTransport = net::createMemoryTransport(network);
    REQUIRE_FALSE(serverTransport->open(net::TransportConfig{.port = Port, .maxPeers = 4, .channels = 4}).has_value());
    REQUIRE_FALSE(clientTransport->open(net::TransportConfig{.port = 0, .maxPeers = 1, .channels = 4}).has_value());
    net::PeerId toServer;
    REQUIRE_FALSE(clientTransport->connect("memory", Port, toServer).has_value());

    RealSide server;
    RealSide client;
    // What the host does at boot for everybody but a dedicated server.
    const core::InstanceId host = scene::createPlayer(server.world, server.network, 1, true);
    const core::InstanceId me = scene::createPlayer(client.world, client.network, 0, true);
    REQUIRE(host.valid());
    REQUIRE(me.valid());

    AuthoritySession authority(*serverTransport);
    ReplicaSession replica(*clientTransport, toServer);
    const auto step = [&](core::u64 tick) {
        authority.receive(server.world, server.workspace);
        authority.send(server.world, server.workspace, tick);
        replica.receive(client.world, client.workspace);
        replica.sendIntent(client.world, tick);
    };
    for (core::u64 tick = 1; tick <= 3; ++tick)
        step(tick);

    // The authority has two players now, and the replica knows which it is.
    const core::InstanceId remote = scene::playerByUserId(server.world, 2);
    REQUIRE(remote.valid());
    CHECK(server.world.parentOf(remote) == server.network);
    CHECK(client.world.players().find(me)->userId == 2);
    CHECK(scene::localPlayerOf(server.world) == host);

    // **And the replica knows who else is playing**: the host's player is on
    // it too, as a player nobody at that machine drives.
    const core::InstanceId hostSeen = scene::playerByUserId(client.world, 1);
    REQUIRE(hostSeen.valid());
    CHECK_FALSE(client.world.players().find(hostSeen)->local);
    CHECK(client.world.parentOf(hostSeen) == client.network);
    CHECK(scene::localPlayerOf(client.world) == me);

    // The replica's player jumps and walks; the authority's copy of that player
    // reads it, by action NAME, in its own atoms -- names the authority's own
    // code has used (a `GetIntent`, an `InputAction`), never ones a peer
    // invents (audit E3).
    (void)server.atoms.intern("Jump");
    (void)server.atoms.intern("Move");
    client.world.players().find(me)->intents = {
        scene::PlayerIntent{client.atoms.intern("Jump"), 0, core::Vec3{}, true},
        scene::PlayerIntent{client.atoms.intern("Move"), 2, core::Vec3{0.5f, -1.0f, 0.0f}, false},
    };
    // A few ticks: the authority applies a replica's intents a few ticks
    // behind the newest, so arrival jitter never leaves it without one.
    for (core::u64 tick = 4; tick <= 10; ++tick)
        step(tick);
    const scene::PlayerComponent* seen = server.world.players().find(remote);
    REQUIRE(seen != nullptr);
    REQUIRE(seen->intents.size() == 2);
    CHECK(server.atoms.text(seen->intents[0].action) == "Jump");
    CHECK(seen->intents[0].pressed);
    CHECK(server.atoms.text(seen->intents[1].action) == "Move");
    CHECK(static_cast<double>(seen->intents[1].axis.y) == doctest::Approx(-1.0));

    // Leaving is a player removed, not a player left behind.
    clientTransport.reset();
    authority.receive(server.world, server.workspace);
    server.world.retireDestroyed();
    CHECK_FALSE(scene::playerByUserId(server.world, 2).valid());
    CHECK(scene::localPlayerOf(server.world) == host);
}

TEST_CASE("a character's transform replicates, because a CharacterBody carries BasePart's fields too")
{
    // **The defect this pins**: the schema said `CharacterBody` inherited the
    // part's six fields in its doc and not in its data, so a character on a
    // replica held its spawn position for ever.
    seedCatalog();
    auto network = net::createMemoryNetwork();
    auto serverTransport = net::createMemoryTransport(network);
    auto clientTransport = net::createMemoryTransport(network);
    REQUIRE_FALSE(serverTransport->open(net::TransportConfig{.port = Port, .maxPeers = 4, .channels = 4}).has_value());
    REQUIRE_FALSE(clientTransport->open(net::TransportConfig{.port = 0, .maxPeers = 1, .channels = 4}).has_value());
    net::PeerId toServer;
    REQUIRE_FALSE(clientTransport->connect("memory", Port, toServer).has_value());

    RealSide server;
    RealSide client;
    const core::InstanceId body = server.world.create(server.classes.findId(server.atoms.intern("CharacterBody")));
    REQUIRE(body.valid());
    server.world.setName(body, server.atoms.intern("Hero"));
    REQUIRE_FALSE(server.world.setParent(body, server.workspace).has_value());

    AuthoritySession authority(*serverTransport);
    ReplicaSession replica(*clientTransport, toServer);
    // The transport's own semantics: each snapshot applied as it arrives.
    replica.setInterpolationDelay(0);
    for (core::u64 tick = 1; tick <= 6; ++tick) {
        server.world.parts().find(body)->cframe.position = core::DVec3{static_cast<double>(tick), 2.0, -3.0};
        authority.receive(server.world, server.workspace);
        authority.send(server.world, server.workspace, tick);
        replica.receive(client.world, client.workspace);
    }

    const core::InstanceId hero = client.world.findFirstChild(client.workspace, client.atoms.intern("Hero"));
    REQUIRE(hero.valid());
    CHECK(client.world.characterBodies().find(hero) != nullptr);
    CHECK(client.world.parts().find(hero)->cframe.position.x == doctest::Approx(6.0));
    CHECK(client.world.parts().find(hero)->cframe.position.z == doctest::Approx(-3.0));
    CHECK(replica.checksumFailures() == 0);
}

// --- Characters, interest and prediction (N1, ADR 0076) -------------------

namespace {

// Two real worlds over the memory transport, each with the players a host and
// a replica have at boot, stepped in the frame's order on both ends.
struct PlayedMatch
{
    std::shared_ptr<net::MemoryNetwork> network = net::createMemoryNetwork();
    std::unique_ptr<net::ITransport> serverTransport;
    std::unique_ptr<net::ITransport> clientTransport = net::createMemoryTransport(network);
    RealSide server;
    RealSide client;
    core::InstanceId host;
    core::InstanceId me;
    std::optional<AuthoritySession> authority;
    std::optional<ReplicaSession> replica;
    net::PeerId toServer;
    core::u64 tick = 0;

    explicit PlayedMatch(const net::LossConfig* loss = nullptr, const net::LossConfig* clientLoss = nullptr)
    {
        seedCatalog();
        serverTransport = loss != nullptr ? net::createLossyTransport(net::createMemoryTransport(network), *loss)
                                          : net::createMemoryTransport(network);
        if (clientLoss != nullptr)
            clientTransport = net::createLossyTransport(net::createMemoryTransport(network), *clientLoss);
        REQUIRE_FALSE(
            serverTransport->open(net::TransportConfig{.port = Port, .maxPeers = 4, .channels = 4}).has_value());
        REQUIRE_FALSE(clientTransport->open(net::TransportConfig{.port = 0, .maxPeers = 1, .channels = 4}).has_value());
        REQUIRE_FALSE(clientTransport->connect("memory", Port, toServer).has_value());
        host = scene::createPlayer(server.world, server.network, 1, true);
        me = scene::createPlayer(client.world, client.network, 0, true);
        authority.emplace(*serverTransport);
        replica.emplace(*clientTransport, toServer);
        run(3);
    }

    // A part under the authority's workspace, at `at`.
    core::InstanceId part(std::string_view name, core::DVec3 at)
    {
        const core::InstanceId id = server.world.create(server.classes.findId(server.atoms.intern("Part")));
        REQUIRE(id.valid());
        server.world.setName(id, server.atoms.intern(name));
        server.world.parts().find(id)->cframe.position = at;
        REQUIRE_FALSE(server.world.setParent(id, server.workspace).has_value());
        return id;
    }

    // The authority's player for this replica.
    [[nodiscard]] core::InstanceId remote() { return scene::playerByUserId(server.world, 2); }

    // The replica's copy of an authority instance, or nothing.
    [[nodiscard]] core::InstanceId copyOf(core::InstanceId id) { return replica->localOf(authority->netIdOf(id)); }

    void step()
    {
        tick += 1;
        authority->receive(server.world, server.workspace);
        authority->send(server.world, server.workspace, tick);
        authority->sendMessages(server.world);
        replica->receive(client.world, client.workspace);
        replica->sendIntent(client.world, tick);
        replica->sendMessages(client.world);
    }

    void run(int ticks)
    {
        for (int at = 0; at < ticks; ++at)
            step();
    }
};

} // namespace

TEST_CASE("each machine's Player.Character is its own copy of the part the authority named")
{
    PlayedMatch match;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId hostRacer = match.part("HostRacer", core::DVec3{5.0, 1.0, 0.0});
    REQUIRE(match.remote().valid());
    match.server.world.players().find(match.remote())->character = racer;
    match.server.world.players().find(match.host)->character = hostRacer;
    match.run(3);

    const core::InstanceId mine = match.copyOf(racer);
    REQUIRE(mine.valid());
    CHECK(match.client.world.players().find(match.me)->character == mine);
    const core::InstanceId hostSeen = scene::playerByUserId(match.client.world, 1);
    REQUIRE(hostSeen.valid());
    CHECK(match.client.world.players().find(hostSeen)->character == match.copyOf(hostRacer));

    // And a character taken away is taken away everywhere.
    match.server.world.players().find(match.remote())->character = {};
    match.run(2);
    CHECK_FALSE(match.client.world.players().find(match.me)->character.valid());
}

TEST_CASE("a replica is sent what is near its character, and nothing far from it")
{
    PlayedMatch match;
    match.server.world.engineState().streamingLoadRadius = 100.0;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId near = match.part("Near", core::DVec3{40.0, 1.0, 0.0});
    const core::InstanceId far = match.part("Far", core::DVec3{500.0, 1.0, 0.0});
    // A model straddling the boundary: its near part comes, its far part does
    // not, and the model comes because something in it did.
    const core::InstanceId model =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("Model")));
    REQUIRE_FALSE(match.server.world.setParent(model, match.server.workspace).has_value());
    const core::InstanceId inside = match.part("Inside", core::DVec3{10.0, 1.0, 10.0});
    const core::InstanceId outside = match.part("Outside", core::DVec3{900.0, 1.0, 10.0});
    REQUIRE_FALSE(match.server.world.setParent(inside, model).has_value());
    REQUIRE_FALSE(match.server.world.setParent(outside, model).has_value());
    // And whatever is attached to a near part comes with it, wherever it is.
    const core::InstanceId attached = match.part("Attached", core::DVec3{700.0, 1.0, 0.0});
    REQUIRE_FALSE(match.server.world.setParent(attached, near).has_value());

    match.server.world.players().find(match.remote())->character = racer;
    match.run(4);

    CHECK(match.copyOf(racer).valid());
    CHECK(match.copyOf(near).valid());
    CHECK(match.copyOf(model).valid());
    CHECK(match.copyOf(inside).valid());
    CHECK(match.copyOf(attached).valid());
    CHECK_FALSE(match.copyOf(far).valid());
    CHECK_FALSE(match.copyOf(outside).valid());

    // The far part walks in and arrives; walks just past the radius and stays,
    // inside the hysteresis; and leaves past it.
    match.server.world.parts().find(far)->cframe.position = core::DVec3{90.0, 1.0, 0.0};
    match.run(3);
    CHECK(match.copyOf(far).valid());
    match.server.world.parts().find(far)->cframe.position = core::DVec3{115.0, 1.0, 0.0};
    match.run(3);
    CHECK(match.copyOf(far).valid());
    match.server.world.parts().find(far)->cframe.position = core::DVec3{200.0, 1.0, 0.0};
    match.run(3);
    CHECK_FALSE(match.copyOf(far).valid());
    // And back again: a second spawn of the same id, whole.
    match.server.world.parts().find(far)->cframe.position = core::DVec3{20.0, 1.0, 0.0};
    match.run(3);
    REQUIRE(match.copyOf(far).valid());
    match.run(8);
    CHECK(match.client.world.parts().find(match.copyOf(far))->cframe.position.x == doctest::Approx(20.0));
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("interest is a sphere: a player deep under the world is not sent what is far above them")
{
    // **The vertical axis the unresolved list asked for** (the owner's mandate,
    // S3). The streaming grid is columns of chunks, and a player in a cave
    // was feared to be sent the whole column above them. Interest is measured
    // per part in three dimensions, so height counts as much as distance
    // across does.
    PlayedMatch match;
    match.server.world.engineState().streamingLoadRadius = 100.0;
    const core::InstanceId caver = match.part("Caver", core::DVec3{0.0, -400.0, 0.0});
    const core::InstanceId beside = match.part("Beside", core::DVec3{30.0, -390.0, 0.0});
    const core::InstanceId overhead = match.part("Overhead", core::DVec3{0.0, 10.0, 0.0});
    match.server.world.players().find(match.remote())->character = caver;
    match.run(4);

    CHECK(match.copyOf(caver).valid());
    CHECK(match.copyOf(beside).valid());
    CHECK_FALSE(match.copyOf(overhead).valid());

    // Climbing up to it brings it in.
    match.server.world.parts().find(caver)->cframe.position = core::DVec3{0.0, -50.0, 0.0};
    match.run(3);
    CHECK(match.copyOf(overhead).valid());
}

TEST_CASE("a player who drops and comes back is the same player, and gets the world back whole")
{
    // **ADR 0085.** The peer id is the connection and is never reused; the
    // token is the player. A game that keys a score or an inventory on
    // `UserId` finds it again.
    PlayedMatch match;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    match.run(3);
    REQUIRE(match.copyOf(racer).valid());
    const core::u32 who = match.replica->playerId();
    CHECK(who == 2);
    const PlayerToken token = match.replica->playerToken();
    CHECK(token.valid());

    // The link goes.
    match.clientTransport->disconnect(match.toServer);
    match.run(2);
    CHECK(match.replica->lost());
    CHECK_FALSE(scene::playerByUserId(match.server.world, who).valid());

    // And comes back: the same player, and the world arrives again, once.
    net::PeerId again;
    REQUIRE_FALSE(match.clientTransport->connect("memory", Port, again).has_value());
    match.replica->rebind(again);
    match.run(4);
    CHECK(match.replica->welcomed());
    CHECK_FALSE(match.replica->lost());
    CHECK(match.replica->playerId() == who);
    CHECK(match.replica->playerToken() == token);
    CHECK(scene::playerByUserId(match.server.world, who).valid());
    REQUIRE(match.copyOf(racer).valid());
    int racers = 0;
    for (core::InstanceId at = match.client.world.firstChild(match.client.workspace); at.valid();
         at = match.client.world.nextSibling(at)) {
        if (match.client.world.name(at) == match.client.atoms.intern("Racer"))
            ++racers;
    }
    CHECK(racers == 1);
    // The replica's own services are not the rejoined world's to destroy.
    CHECK_FALSE(match.client.world.destroyed(match.client.workspace));
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a connection still holding a player who is back already is let go")
{
    // A link that died without saying so stays open on the authority until it
    // times out. The player redials on another before that: the new one is the
    // player, and the old one is dropped rather than leaving two of them.
    PlayedMatch match;
    const PlayerToken token = match.replica->playerToken();
    REQUIRE(token.valid());

    auto otherTransport = net::createMemoryTransport(match.network);
    REQUIRE_FALSE(otherTransport->open(net::TransportConfig{.port = 0, .maxPeers = 1, .channels = 4}).has_value());
    net::PeerId toServer;
    REQUIRE_FALSE(otherTransport->connect("memory", Port, toServer).has_value());
    RealSide other;
    (void)scene::createPlayer(other.world, other.network, 0, true);
    ReplicaSession second(*otherTransport, toServer);
    second.setPlayerToken(token);
    for (int at = 0; at < 4; ++at) {
        match.step();
        second.receive(other.world, other.workspace);
    }

    CHECK(second.playerId() == 2);
    CHECK(match.replica->lost());
    int copies = 0;
    for (core::InstanceId at = match.server.world.firstChild(match.server.network); at.valid();
         at = match.server.world.nextSibling(at)) {
        if (const scene::PlayerComponent* player = match.server.world.players().find(at);
            player != nullptr && player->userId == 2)
            ++copies;
    }
    CHECK(copies == 1);
}

TEST_CASE("a replica moves its own character at once, and the snapshots only correct it")
{
    PlayedMatch match;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    match.server.world.players().find(match.remote())->character = racer;
    match.run(3);
    const core::InstanceId mine = match.copyOf(racer);
    REQUIRE(mine.valid());

    // The game, on both ends: while "Move" is held, the character goes half a
    // metre along x a tick. The authority runs it from the intent it received;
    // the replica runs it from its own input, at once -- which is prediction.
    const core::NameAtom move = match.client.atoms.intern("Move");
    match.client.world.players().find(match.me)->intents = {scene::PlayerIntent{move, 0, core::Vec3{}, true}};
    const auto serverGame = [&] {
        const scene::PlayerComponent* player = match.server.world.players().find(match.remote());
        for (const scene::PlayerIntent& intent : player->intents) {
            if (match.server.atoms.text(intent.action) == "Move" && intent.pressed)
                match.server.world.parts().find(racer)->cframe.position.x += 0.5;
        }
    };
    const auto clientGame = [&] { match.client.world.parts().find(mine)->cframe.position.x += 0.5; };

    // A round trip of several ticks: the replica reads the network every
    // fourth tick, so an answer is always a few ticks stale when it lands.
    for (int frame = 1; frame <= 60; ++frame) {
        match.tick += 1;
        match.authority->receive(match.server.world, match.server.workspace);
        serverGame();
        match.authority->send(match.server.world, match.server.workspace, match.tick);
        if (frame % 4 == 0)
            match.replica->receive(match.client.world, match.client.workspace);
        clientGame();
        match.replica->sendIntent(match.client.world, match.tick);
    }

    const double server = match.server.world.parts().find(racer)->cframe.position.x;
    const double client = match.client.world.parts().find(mine)->cframe.position.x;
    // **Ahead of the authority, not behind it**: the replica shows the moves it
    // has made and the authority has not answered yet.
    CHECK(client > server);
    CHECK(client - server < 5.0);
    // And the two agree about every move both have seen, so nothing was
    // corrected.
    CHECK(match.replica->stats().corrections == 0);

    // The authority disagrees -- a teleport the replica could not predict --
    // and the replica is corrected by exactly that much.
    match.server.world.parts().find(racer)->cframe.position.z = 30.0;
    for (int frame = 1; frame <= 12; ++frame) {
        match.tick += 1;
        match.authority->receive(match.server.world, match.server.workspace);
        match.authority->send(match.server.world, match.server.workspace, match.tick);
        match.replica->receive(match.client.world, match.client.workspace);
        match.replica->sendIntent(match.client.world, match.tick);
    }
    CHECK(match.client.world.parts().find(mine)->cframe.position.z == doctest::Approx(30.0));
    CHECK(match.replica->stats().corrections >= 1);
}

namespace {

// A world with a wall at x = 5 the prediction does not know about: each
// command walks half a metre along x, and the wall stops it.
class WalledReplay final : public scene::ICharacterReplay
{
public:
    [[nodiscard]] std::optional<scene::CharacterCommand> lastCommand(core::InstanceId) const override
    {
        scene::CharacterCommand command;
        command.moveDirection = core::Vec3{1.0f, 0.0f, 0.0f};
        command.dt = 1.0f / 60.0f;
        return command;
    }

    [[nodiscard]] std::vector<core::CFrameD> replay(core::InstanceId, const scene::CharacterReplayStart& start,
                                                    std::span<const scene::CharacterCommand> commands) override
    {
        starts.push_back(start.transform.position);
        std::vector<core::CFrameD> frames;
        core::CFrameD at = start.transform;
        for (std::size_t step = 0; step < commands.size(); ++step) {
            at.position.x = std::min(at.position.x + 0.5, 5.0);
            frames.push_back(at);
        }
        return frames;
    }

    std::vector<core::DVec3> starts;
};

} // namespace

TEST_CASE("a corrected character is stepped again from where the authority put it, not shifted by the error")
{
    // **What re-simulation buys over shifting** (ADR 0076, as amended): the
    // replica predicts walking on through x = 5, where the authority's world
    // has a wall. Shifted by the error at the answered tick, the prediction
    // stays that far ahead -- inside the wall. Stepped again from the
    // authority's position through the unanswered commands, it stops where
    // the authority will stop it.
    PlayedMatch match;
    WalledReplay walls;
    match.replica->setCharacterReplay(&walls);
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    match.server.world.players().find(match.remote())->character = racer;
    match.run(3);
    const core::InstanceId mine = match.copyOf(racer);
    REQUIRE(mine.valid());

    const core::NameAtom move = match.client.atoms.intern("Move");
    match.client.world.players().find(match.me)->intents = {scene::PlayerIntent{move, 0, core::Vec3{}, true}};
    for (int frame = 1; frame <= 40; ++frame) {
        match.tick += 1;
        match.authority->receive(match.server.world, match.server.workspace);
        // The authority's world: the wall.
        for (const scene::PlayerIntent& intent : match.server.world.players().find(match.remote())->intents) {
            if (intent.pressed) {
                double& x = match.server.world.parts().find(racer)->cframe.position.x;
                x = std::min(x + 0.5, 5.0);
            }
        }
        match.authority->send(match.server.world, match.server.workspace, match.tick);
        if (frame % 4 == 0)
            match.replica->receive(match.client.world, match.client.workspace);
        // The replica's prediction: no wall.
        match.client.world.parts().find(mine)->cframe.position.x += 0.5;
        match.replica->sendIntent(match.client.world, match.tick);
    }

    CHECK(match.replica->stats().replays >= 1);
    REQUIRE_FALSE(walls.starts.empty());
    // Every replay started where the authority said, which is never past it.
    for (const core::DVec3& start : walls.starts)
        CHECK(start.x <= 5.0);
    // The last correction left the character at the wall -- plus the one step
    // predicted since -- not the metres past it the error would have carried
    // it.
    CHECK(match.client.world.parts().find(mine)->cframe.position.x <= 5.5 + 1e-9);
}

namespace {

// A flat world, and a movement model that walks `walkSpeed` a second along x --
// the one the authority steps too, so a replay that used the authority's speed
// lands where the authority does.
class FlatReplay final : public scene::ICharacterReplay
{
public:
    FlatReplay(const scene::World& world, core::InstanceId& character) : m_world(world), m_character(character) {}

    [[nodiscard]] std::optional<scene::CharacterCommand> lastCommand(core::InstanceId) const override
    {
        const scene::CharacterBodyComponent* body = m_world.characterBodies().find(m_character);
        if (body == nullptr)
            return std::nullopt;
        scene::CharacterCommand command;
        command.moveDirection = core::Vec3{1.0f, 0.0f, 0.0f};
        command.walkSpeed = body->walkSpeed;
        command.jumpSpeed = body->jumpSpeed;
        command.dt = 1.0f / 60.0f;
        return command;
    }

    [[nodiscard]] std::vector<core::CFrameD> replay(core::InstanceId, const scene::CharacterReplayStart& start,
                                                    std::span<const scene::CharacterCommand> commands) override
    {
        std::vector<core::CFrameD> frames;
        core::CFrameD at = start.transform;
        for (const scene::CharacterCommand& command : commands) {
            speeds.push_back(command.walkSpeed);
            at.position.x += static_cast<double>(command.walkSpeed) * static_cast<double>(command.dt);
            frames.push_back(at);
        }
        return frames;
    }

    std::vector<core::f32> speeds;

private:
    const scene::World& m_world;
    core::InstanceId& m_character;
};

// Both ends' game: while "Move" is held, a character walks its own
// `WalkSpeed` along x for a tick.
void walk(scene::World& world, core::InstanceId character)
{
    const scene::CharacterBodyComponent* body = world.characterBodies().find(character);
    if (body != nullptr)
        world.parts().find(character)->cframe.position.x += static_cast<double>(body->walkSpeed) / 60.0;
}

} // namespace

TEST_CASE("a character's speeds reach the replica that predicts it, so a faster walk is not a rollback")
{
    // **D205.** A server script set `WalkSpeed = 24`; the replica, which never
    // heard, predicted at 16 and was corrected every snapshot -- the stutter a
    // player felt as the game fighting them.
    PlayedMatch match;
    const core::InstanceId racer =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("CharacterBody")));
    REQUIRE(racer.valid());
    match.server.world.setName(racer, match.server.atoms.intern("Racer"));
    match.server.world.parts().find(racer)->cframe.position = core::DVec3{0.0, 1.0, 0.0};
    REQUIRE_FALSE(match.server.world.setParent(racer, match.server.workspace).has_value());
    scene::CharacterBodyComponent* authoritative = match.server.world.characterBodies().find(racer);
    REQUIRE(authoritative != nullptr);
    authoritative->walkSpeed = 24.0f;
    authoritative->jumpSpeed = 11.0f;
    authoritative->maxSlopeAngle = 30.0f;
    authoritative->autoStepHeight = 0.25f;
    match.server.world.players().find(match.remote())->character = racer;
    match.run(3);
    core::InstanceId mine = match.copyOf(racer);
    REQUIRE(mine.valid());
    FlatReplay replay(match.client.world, mine);
    match.replica->setCharacterReplay(&replay);

    // The four arrive -- on the replica's OWN character, whose motion state
    // stays its own.
    const scene::CharacterBodyComponent* predicted = match.client.world.characterBodies().find(mine);
    REQUIRE(predicted != nullptr);
    CHECK(predicted->walkSpeed == 24.0f);
    CHECK(predicted->jumpSpeed == 11.0f);
    CHECK(predicted->maxSlopeAngle == 30.0f);
    CHECK(predicted->autoStepHeight == 0.25f);

    const core::NameAtom move = match.client.atoms.intern("Move");
    match.client.world.players().find(match.me)->intents = {scene::PlayerIntent{move, 0, core::Vec3{}, true}};
    const auto play = [&](int frames) {
        for (int frame = 1; frame <= frames; ++frame) {
            match.tick += 1;
            match.authority->receive(match.server.world, match.server.workspace);
            for (const scene::PlayerIntent& intent : match.server.world.players().find(match.remote())->intents) {
                if (intent.pressed)
                    walk(match.server.world, racer);
            }
            match.authority->send(match.server.world, match.server.workspace, match.tick);
            if (frame % 4 == 0)
                match.replica->receive(match.client.world, match.client.workspace);
            walk(match.client.world, mine);
            match.replica->sendIntent(match.client.world, match.tick);
        }
    };
    play(60);
    // Within a centimetre every tick: nothing to correct.
    CHECK(match.replica->stats().corrections == 0);

    // **The speed changes while commands are in flight.** The ones the
    // authority has not answered yet were predicted at 24 and will be answered
    // at 30 -- so they are replayed at 30, and the replica lands where the
    // authority will put it.
    match.server.world.characterBodies().find(racer)->walkSpeed = 30.0f;
    play(60);
    REQUIRE_FALSE(replay.speeds.empty());
    CHECK(replay.speeds.back() == 30.0f);
    const core::u64 settled = match.replica->stats().corrections;
    play(60);
    CHECK(match.replica->stats().corrections == settled);
}

TEST_CASE("a replica walking through jitter, reordering and loss is never corrected (multiplayer smoothness)")
{
    // **A real connection, both ways**: each message held 0 to 2 ticks, some
    // delivered out of order, two in a hundred lost. The owner's match over a
    // VPN: every player's character flicked while walking, because the
    // authority applied whatever intent had arrived last -- none on one tick,
    // two on the next -- and acknowledged the last received rather than the
    // one its step came from.
    net::LossConfig serverLoss;
    serverLoss.seed = 11;
    serverLoss.dropPerMille = 20;
    serverLoss.reorderPerMille = 50;
    serverLoss.jitterPolls = 2;
    net::LossConfig clientLoss = serverLoss;
    clientLoss.seed = 23;
    PlayedMatch match(&serverLoss, &clientLoss);
    const core::InstanceId racer =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("CharacterBody")));
    REQUIRE(racer.valid());
    match.server.world.setName(racer, match.server.atoms.intern("Racer"));
    match.server.world.parts().find(racer)->cframe.position = core::DVec3{0.0, 1.0, 0.0};
    REQUIRE_FALSE(match.server.world.setParent(racer, match.server.workspace).has_value());
    match.server.world.characterBodies().find(racer)->walkSpeed = 8.0f;
    match.server.world.players().find(match.remote())->character = racer;
    match.run(10);
    core::InstanceId mine = match.copyOf(racer);
    REQUIRE(mine.valid());
    FlatReplay replay(match.client.world, mine);
    match.replica->setCharacterReplay(&replay);

    // Ten seconds of a held key, every tick read and sent as a game does.
    const core::NameAtom move = match.client.atoms.intern("Move");
    match.client.world.players().find(match.me)->intents = {scene::PlayerIntent{move, 0, core::Vec3{}, true}};
    double last = match.server.world.parts().find(racer)->cframe.position.x;
    int uneven = 0;
    for (int frame = 1; frame <= 600; ++frame) {
        match.tick += 1;
        match.authority->receive(match.server.world, match.server.workspace);
        for (const scene::PlayerIntent& intent : match.server.world.players().find(match.remote())->intents) {
            if (intent.pressed)
                walk(match.server.world, racer);
        }
        // Once walking, the authority steps exactly one tick of walk a tick:
        // what the other players see.
        const double x = match.server.world.parts().find(racer)->cframe.position.x;
        if (x > 0.0 && last > 0.0 && std::abs((x - last) - 8.0 / 60.0) > 1e-6)
            ++uneven;
        last = x;
        match.authority->send(match.server.world, match.server.workspace, match.tick);
        match.replica->receive(match.client.world, match.client.workspace);
        walk(match.client.world, mine);
        match.replica->sendIntent(match.client.world, match.tick);
    }
    CHECK(match.server.world.parts().find(racer)->cframe.position.x > 70.0);
    CHECK(uneven == 0);
    // Not one correction past a centimetre.
    CHECK(match.replica->stats().corrections == 0);
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a correction is drawn sliding over a tenth of a second, and a teleport where it lands")
{
    PlayedMatch match;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    match.server.world.players().find(match.remote())->character = racer;
    match.run(10);
    const core::InstanceId mine = match.copyOf(racer);
    REQUIRE(mine.valid());
    CHECK(match.replica->visualCorrection().offset == core::DVec3{});

    // Half a metre the replica did not predict: the simulation takes it at
    // once, and the drawing starts where the character was.
    match.server.world.parts().find(racer)->cframe.position.x += 0.5;
    const core::u64 before = match.replica->stats().corrections;
    for (int at = 0; at < 10 && match.replica->stats().corrections == before; ++at)
        match.step();
    REQUIRE(match.replica->stats().corrections == before + 1);
    CHECK(match.client.world.parts().find(mine)->cframe.position.x == doctest::Approx(0.5));
    CHECK(match.replica->stats().lastCorrectionMetres == doctest::Approx(0.5));
    const replication::VisualCorrection slide = match.replica->visualCorrection();
    CHECK(slide.character == mine);
    CHECK(slide.offset.x < -0.2);
    CHECK(slide.offset.x >= -0.5);
    // A tenth of a second later it is there.
    match.run(6);
    CHECK(std::abs(match.replica->visualCorrection().offset.x) < 0.02);

    // Thirty metres is a teleport, and drawn as one.
    match.server.world.parts().find(racer)->cframe.position.z = 30.0;
    const core::u64 again = match.replica->stats().corrections;
    for (int at = 0; at < 10 && match.replica->stats().corrections == again; ++at)
        match.step();
    CHECK(match.client.world.parts().find(mine)->cframe.position.z == doctest::Approx(30.0));
    CHECK(match.replica->visualCorrection().offset == core::DVec3{});
}

TEST_CASE("another player's part is drawn between snapshots rather than stepping at their rate")
{
    PlayedMatch match;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId other = match.part("Other", core::DVec3{0.0, 1.0, 5.0});
    match.server.world.players().find(match.remote())->character = racer;
    match.run(3);
    const core::InstanceId seen = match.copyOf(other);
    REQUIRE(seen.valid());

    // It moves a metre a tick, and the authority snapshots every other tick,
    // as the default rate does.
    std::vector<double> positions;
    for (int frame = 1; frame <= 40; ++frame) {
        match.tick += 1;
        match.authority->receive(match.server.world, match.server.workspace);
        match.server.world.parts().find(other)->cframe.position.x += 1.0;
        if (match.tick % 2 == 0)
            match.authority->send(match.server.world, match.server.workspace, match.tick);
        match.replica->receive(match.client.world, match.client.workspace);
        positions.push_back(match.client.world.parts().find(seen)->cframe.position.x);
    }
    // Once the buffer is full, every tick moves it a metre: no stall on the
    // tick with no snapshot, and no two-metre jump on the tick with one.
    for (std::size_t at = 20; at < positions.size(); ++at)
        CHECK(positions[at] - positions[at - 1] == doctest::Approx(1.0));
    // And it is drawn a few ticks behind the authority, not ahead of it.
    const double server = match.server.world.parts().find(other)->cframe.position.x;
    CHECK(positions.back() < server);
    CHECK(server - positions.back() <= 6.0);
}

TEST_CASE("a decal placed on the authority is seen on a replica, image and all")
{
    PlayedMatch match;
    const core::InstanceId crate = match.part("Crate", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId decal =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("Decal")));
    REQUIRE(decal.valid());
    scene::DecalComponent* mark = match.server.world.decals().find(decal);
    REQUIRE(mark != nullptr);
    mark->texture = match.server.atoms.intern("asset://textures/scorch.png");
    mark->size = core::Vec3{3.0f, 3.0f, 1.0f};
    mark->transparency = 0.25f;
    REQUIRE_FALSE(match.server.world.setParent(decal, crate).has_value());
    match.run(4);

    const core::InstanceId seen = match.copyOf(decal);
    REQUIRE(seen.valid());
    const scene::DecalComponent* copy = match.client.world.decals().find(seen);
    REQUIRE(copy != nullptr);
    // **The URN, in the replica's own atoms**: the authority's atom number
    // would name some other string here, or none.
    CHECK(match.client.atoms.text(copy->texture) == "asset://textures/scorch.png");
    CHECK(static_cast<double>(copy->size.x) == doctest::Approx(3.0));
    CHECK(static_cast<double>(copy->transparency) == doctest::Approx(0.25));
    CHECK(match.client.world.parentOf(seen) == match.copyOf(crate));

    // A new image is a new string, and it travels the same way.
    match.server.world.decals().find(decal)->texture = match.server.atoms.intern("asset://textures/footprint.png");
    match.run(3);
    CHECK(match.client.atoms.text(match.client.world.decals().find(seen)->texture) == "asset://textures/footprint.png");
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("D424: a part's shape reaches a replica")
{
    // Every ball and cylinder a script made on the authority was a block on a
    // client: the tree's crown printed `Ball` on the server and `Block` on
    // the replica, and it collided as a block there too.
    PlayedMatch match;
    const core::InstanceId crown = match.part("Crown", core::DVec3{0.0, 4.0, 0.0});
    const core::InstanceId trunk = match.part("Trunk", core::DVec3{0.0, 1.0, 0.0});
    match.server.world.parts().find(crown)->shape = 1;
    match.server.world.parts().find(trunk)->shape = 2;

    match.run(4);

    const scene::PartComponent* seenCrown = match.client.world.parts().find(match.copyOf(crown));
    const scene::PartComponent* seenTrunk = match.client.world.parts().find(match.copyOf(trunk));
    REQUIRE(seenCrown != nullptr);
    REQUIRE(seenTrunk != nullptr);
    CHECK(seenCrown->shape == 1);
    CHECK(seenTrunk->shape == 2);

    // A shape changed while it runs follows too.
    match.server.world.parts().find(crown)->shape = 0;
    match.run(4);
    CHECK(match.client.world.parts().find(match.copyOf(crown))->shape == 0);
}

TEST_CASE("a part's material, its overrides and a runtime copy all reach a replica, which draws them")
{
    // **The surface the wire never carried** (ADR 0090): a part's `Color` went
    // out and its `Material` never did, so a replica drew every part bare.
    PlayedMatch match;
    // The file is content both machines have, as a mesh is.
    asset::MaterialLibrary serverMaterials;
    asset::MaterialLibrary clientMaterials;
    asset::MaterialAsset brick;
    brick.properties.color = core::Color3{0.6f, 0.3f, 0.2f};
    brick.properties.roughness = 0.9f;
    brick.instanceParameters = asset::fieldBit(asset::MaterialField::Transparency);
    brick.written = asset::AllMaterialFields;
    serverMaterials.put("asset://materials/brick.material.json", brick);
    clientMaterials.put("asset://materials/brick.material.json", brick);
    match.server.world.setMaterialLibrary(&serverMaterials);
    match.client.world.setMaterialLibrary(&clientMaterials);

    const core::InstanceId wall = match.part("Wall", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId crate = match.part("Crate", core::DVec3{2.0, 1.0, 0.0});
    scene::PartComponent* worn = match.server.world.parts().find(wall);
    worn->material = match.server.atoms.intern("asset://materials/brick.material.json");
    asset::MaterialProperties faded;
    faded.transparency = 0.5f;
    (void)asset::setOverride(worn->materialParameters, asset::MaterialField::Transparency, faded);

    // **The authority clones it, changes its colour and its map, and puts it on
    // the crate** -- a copy is state the run produced, and it travels with
    // what it changed.
    const core::u32 clone = match.server.world.cloneMaterial(worn->material, 0, asset::MaterialProperties{});
    match.server.world.holdMaterialClone(clone);
    scene::MaterialClone* copy = match.server.world.writeMaterialClone(clone);
    REQUIRE(copy != nullptr);
    copy->values.color = core::Color3{0.0f, 1.0f, 0.0f};
    copy->values.colorMap = "asset://textures/moss.png";
    (void)match.server.atoms.intern(copy->values.colorMap);
    copy->set = asset::fieldBit(asset::MaterialField::Color) | asset::fieldBit(asset::MaterialField::ColorMap);
    scene::PartComponent* boxed = match.server.world.parts().find(crate);
    boxed->material = worn->material;
    boxed->materialClone = clone;
    match.run(4);

    const scene::PartComponent* seenWall = match.client.world.parts().find(match.copyOf(wall));
    REQUIRE(seenWall != nullptr);
    CHECK(match.client.atoms.text(seenWall->material) == "asset://materials/brick.material.json");
    CHECK(seenWall->materialParameters.has(asset::MaterialField::Transparency));
    const asset::ResolvedMaterial wallLook = match.client.world.surfaceOf(*seenWall);
    CHECK(wallLook.properties.transparency == 0.5f);
    CHECK(wallLook.properties.roughness == 0.9f);

    // **And the replica draws the copy's colour** -- the ledger's own test.
    const scene::PartComponent* seenCrate = match.client.world.parts().find(match.copyOf(crate));
    REQUIRE(seenCrate != nullptr);
    REQUIRE(seenCrate->materialClone != 0);
    const asset::ResolvedMaterial crateLook = match.client.world.surfaceOf(*seenCrate);
    CHECK(crateLook.properties.color == core::Color3{0.0f, 1.0f, 0.0f});
    CHECK(crateLook.properties.colorMap == "asset://textures/moss.png");
    // What the copy did not change still reads through to the asset.
    CHECK(crateLook.properties.roughness == 0.9f);

    // A change to the copy reaches the replica too.
    match.server.world.writeMaterialClone(clone)->values.color = core::Color3{0.0f, 0.0f, 1.0f};
    match.run(3);
    CHECK(match.client.world.surfaceOf(*match.client.world.parts().find(match.copyOf(crate))).properties.color ==
          core::Color3{0.0f, 0.0f, 1.0f});
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("night on the authority is night on the replica: Lighting's properties travel")
{
    PlayedMatch match;
    scene::LightingComponent* sky = match.server.world.lighting().find(match.server.lighting);
    REQUIRE(sky != nullptr);
    sky->clockTime = 19.5f;
    sky->fogEnd = 300.0f;
    sky->fogColor = core::Color3{0.2f, 0.1f, 0.3f};
    match.run(4);

    const scene::LightingComponent* seen = match.client.world.lighting().find(match.client.lighting);
    REQUIRE(seen != nullptr);
    CHECK(static_cast<double>(seen->clockTime) == doctest::Approx(19.5));
    CHECK(static_cast<double>(seen->fogEnd) == doctest::Approx(300.0));
    CHECK(static_cast<double>(seen->fogColor.b) == doctest::Approx(0.3));
    // A service stays where every world keeps it, and is never a copy.
    CHECK(match.client.world.parentOf(match.client.lighting) == match.client.dataModel);
    CHECK(match.client.world.name(match.client.lighting) == match.client.atoms.intern("Lighting"));

    // And the clock keeps up as the authority's day turns.
    sky->clockTime = 6.0f;
    match.run(3);
    CHECK(static_cast<double>(match.client.world.lighting().find(match.client.lighting)->clockTime) ==
          doctest::Approx(6.0));
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("the wind the authority sets is the wind every replica draws")
{
    // ADR 0115, protocol 19: the workspace's wind travels as a service's
    // properties, so a gust crosses every player's field the same.
    PlayedMatch match;
    scene::WorkspaceComponent* air = match.server.world.workspaces().find(match.server.workspace);
    REQUIRE(air != nullptr);
    air->globalWind = core::Vec3{7.0f, 0.0f, -2.0f};
    air->windGusts = 0.4f;
    air->windTurbulence = 0.25f;
    match.run(4);

    const scene::WorkspaceComponent* seen = match.client.world.workspaces().find(match.client.workspace);
    REQUIRE(seen != nullptr);
    CHECK(static_cast<double>(seen->globalWind.x) == doctest::Approx(7.0));
    CHECK(static_cast<double>(seen->globalWind.z) == doctest::Approx(-2.0));
    CHECK(static_cast<double>(seen->windGusts) == doctest::Approx(0.4));
    CHECK(static_cast<double>(seen->windTurbulence) == doctest::Approx(0.25));
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("an instance leaving interest that a script holds is a husk; one the authority destroyed is gone")
{
    PlayedMatch match;
    match.server.world.engineState().streamingLoadRadius = 100.0;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId held = match.part("Held", core::DVec3{40.0, 1.0, 0.0});
    const core::InstanceId loose = match.part("Loose", core::DVec3{50.0, 1.0, 0.0});
    // Held, under a part nobody holds: it must survive its parent leaving.
    const core::InstanceId heldChild = match.part("HeldChild", core::DVec3{50.0, 2.0, 0.0});
    REQUIRE_FALSE(match.server.world.setParent(heldChild, loose).has_value());
    const core::InstanceId doomed = match.part("Doomed", core::DVec3{30.0, 1.0, 0.0});
    match.server.world.players().find(match.remote())->character = racer;
    match.run(4);

    const core::InstanceId heldCopy = match.copyOf(held);
    const core::InstanceId looseCopy = match.copyOf(loose);
    const core::InstanceId childCopy = match.copyOf(heldChild);
    const core::InstanceId doomedCopy = match.copyOf(doomed);
    REQUIRE(heldCopy.valid());
    REQUIRE(looseCopy.valid());
    REQUIRE(childCopy.valid());
    REQUIRE(doomedCopy.valid());
    const std::vector<core::InstanceId> scriptHolds{heldCopy, childCopy, doomedCopy};
    match.replica->setReferenceProbe([&scriptHolds](core::InstanceId id) {
        return std::find(scriptHolds.begin(), scriptHolds.end(), id) != scriptHolds.end();
    });

    match.server.world.parts().find(held)->cframe.position = core::DVec3{500.0, 1.0, 0.0};
    match.server.world.parts().find(loose)->cframe.position = core::DVec3{600.0, 1.0, 0.0};
    match.server.world.parts().find(heldChild)->cframe.position = core::DVec3{600.0, 2.0, 0.0};
    REQUIRE(match.server.world.destroy(doomed));
    match.server.world.retireDestroyed();
    match.run(4);
    match.client.world.retireDestroyed();

    // **Streamed out, and held: a husk** -- alive, parented to nil, reported.
    CHECK(match.client.world.alive(heldCopy));
    CHECK_FALSE(match.client.world.parentOf(heldCopy).valid());
    CHECK(match.client.world.alive(childCopy));
    CHECK_FALSE(match.client.world.parentOf(childCopy).valid());
    // Streamed out and not held, or destroyed by the authority whoever holds
    // it: gone.
    CHECK_FALSE(match.client.world.alive(looseCopy));
    CHECK_FALSE(match.client.world.alive(doomedCopy));
    std::vector<core::InstanceId> husks = match.replica->drainStreamedOut();
    std::sort(husks.begin(), husks.end(), [](core::InstanceId a, core::InstanceId b) { return a.index < b.index; });
    std::vector<core::InstanceId> expected{heldCopy, childCopy};
    std::sort(expected.begin(), expected.end(),
              [](core::InstanceId a, core::InstanceId b) { return a.index < b.index; });
    CHECK(husks == expected);
    CHECK(match.replica->drainStreamedOut().empty());

    // Walking back in is a fresh copy, whole, and the husk is left to the
    // script that holds it.
    match.server.world.parts().find(held)->cframe.position = core::DVec3{20.0, 1.0, 0.0};
    match.run(4);
    REQUIRE(match.copyOf(held).valid());
    CHECK(match.copyOf(held) != heldCopy);
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a replica's message reaches the authority from its own player, and an answer reaches only it")
{
    PlayedMatch match;
    const core::InstanceId remote =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("RemoteEvent")));
    REQUIRE(remote.valid());
    REQUIRE_FALSE(match.server.world.setParent(remote, match.server.workspace).has_value());
    const core::InstanceId crate = match.part("Crate", core::DVec3{0.0, 1.0, 0.0});
    match.run(3);
    const core::InstanceId remoteHere = match.copyOf(remote);
    const core::InstanceId crateHere = match.copyOf(crate);
    REQUIRE(remoteHere.valid());
    REQUIRE(crateHere.valid());

    // What `FireServer` queues on a replica: the payload is the script
    // module's and the session never reads it, so any bytes will do.
    scene::RemoteMessage up;
    up.remote = remoteHere;
    up.toServer = true;
    up.payload = {1, 7, 9};
    up.refs = {crateHere};
    match.client.world.engineState().remoteOutbox.push_back(up);
    match.run(2);

    std::vector<scene::RemoteMessage>& arrived = match.server.world.engineState().remoteInbox;
    REQUIRE(arrived.size() == 1);
    CHECK(arrived[0].remote == remote);
    CHECK(arrived[0].toServer);
    // **The sender is the connection's player**, not anything it claimed.
    CHECK(arrived[0].player == match.remote());
    CHECK(arrived[0].payload == std::vector<core::u8>{1, 7, 9});
    REQUIRE(arrived[0].refs.size() == 1);
    CHECK(arrived[0].refs[0] == crate);
    arrived.clear();

    // `FireClient` to that player: its replica gets it, the instance it names
    // as its own copy.
    scene::RemoteMessage down;
    down.remote = remote;
    down.userId = match.server.world.players().find(match.remote())->userId;
    down.payload = {1, 0};
    down.refs = {crate};
    match.server.world.engineState().remoteOutbox.push_back(down);
    match.run(2);
    std::vector<scene::RemoteMessage>& received = match.client.world.engineState().remoteInbox;
    REQUIRE(received.size() == 1);
    CHECK(received[0].remote == remoteHere);
    CHECK_FALSE(received[0].toServer);
    REQUIRE(received[0].refs.size() == 1);
    CHECK(received[0].refs[0] == crateHere);
    received.clear();

    // To some other player, nothing arrives here.
    down.userId = 99;
    match.server.world.engineState().remoteOutbox.push_back(down);
    match.run(2);
    CHECK(match.client.world.engineState().remoteInbox.empty());

    // **A client cannot fire into something that is not an event.**
    scene::RemoteMessage stray = up;
    stray.remote = crateHere;
    match.client.world.engineState().remoteOutbox.push_back(stray);
    const core::u64 droppedBefore = match.authority->stats().messagesDropped;
    match.run(2);
    CHECK(match.server.world.engineState().remoteInbox.empty());
    CHECK(match.authority->stats().messagesDropped == droppedBefore + 1);

    // **Nor flood it**: past the per-tick limit, the rest is dropped.
    for (core::u32 at = 0; at < MaxRemoteMessagesPerTick + 44; ++at)
        match.client.world.engineState().remoteOutbox.push_back(up);
    // Sent at the end of one step, taken in at the start of the next.
    match.run(2);
    CHECK(match.server.world.engineState().remoteInbox.size() == MaxRemoteMessagesPerTick);
    CHECK(match.authority->stats().messagesDropped == droppedBefore + 1 + 44);
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("an event created and fired in one tick is never named before the replica has it")
{
    PlayedMatch match;
    const core::InstanceId remote =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("RemoteEvent")));
    REQUIRE_FALSE(match.server.world.setParent(remote, match.server.workspace).has_value());
    scene::RemoteMessage everyone;
    everyone.remote = remote;
    everyone.payload = {1, 2};
    match.server.world.engineState().remoteOutbox.push_back(everyone);
    match.run(2);

    REQUIRE(match.copyOf(remote).valid());
    const std::vector<scene::RemoteMessage>& received = match.client.world.engineState().remoteInbox;
    REQUIRE(received.size() == 1);
    CHECK(received[0].remote == match.copyOf(remote));
}

TEST_CASE("a question reaches the authority with its number, and its answer reaches only the asker")
{
    PlayedMatch match;
    const core::InstanceId function =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("RemoteFunction")));
    const core::InstanceId event =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("RemoteEvent")));
    REQUIRE_FALSE(match.server.world.setParent(function, match.server.workspace).has_value());
    REQUIRE_FALSE(match.server.world.setParent(event, match.server.workspace).has_value());
    match.run(3);
    const core::InstanceId functionHere = match.copyOf(function);
    const core::InstanceId eventHere = match.copyOf(event);
    REQUIRE(functionHere.valid());
    REQUIRE(eventHere.valid());

    // What `InvokeServerAsync` queues on a replica.
    scene::RemoteMessage question;
    question.remote = functionHere;
    question.toServer = true;
    question.call = 41;
    question.payload = {1, 3};
    match.client.world.engineState().remoteOutbox.push_back(question);
    match.run(2);
    std::vector<scene::RemoteMessage>& arrived = match.server.world.engineState().remoteInbox;
    REQUIRE(arrived.size() == 1);
    CHECK(arrived[0].remote == function);
    CHECK(arrived[0].call == 41u);
    CHECK_FALSE(arrived[0].reply);
    CHECK(arrived[0].player == match.remote());
    arrived.clear();

    // The authority's answer, to that player, carrying the number back.
    scene::RemoteMessage answer;
    answer.remote = function;
    answer.call = 41;
    answer.reply = true;
    answer.failed = true;
    answer.userId = match.server.world.players().find(match.remote())->userId;
    answer.payload = {1, 0};
    match.server.world.engineState().remoteOutbox.push_back(answer);
    match.run(2);
    std::vector<scene::RemoteMessage>& received = match.client.world.engineState().remoteInbox;
    REQUIRE(received.size() == 1);
    CHECK(received[0].remote == functionHere);
    CHECK(received[0].call == 41u);
    CHECK(received[0].reply);
    CHECK(received[0].failed);
    received.clear();

    // **Each kind of message names its own kind of instance.** A question
    // aimed at an event, and a plain message aimed at a function, are both
    // dropped -- and so is a client pretending to answer.
    const core::u64 droppedBefore = match.authority->stats().messagesDropped;
    scene::RemoteMessage wrongKind = question;
    wrongKind.remote = eventHere;
    match.client.world.engineState().remoteOutbox.push_back(wrongKind);
    scene::RemoteMessage plain = question;
    plain.call = 0;
    match.client.world.engineState().remoteOutbox.push_back(plain);
    scene::RemoteMessage forged = question;
    forged.reply = true;
    match.client.world.engineState().remoteOutbox.push_back(forged);
    match.run(2);
    CHECK(match.server.world.engineState().remoteInbox.empty());
    CHECK(match.authority->stats().messagesDropped == droppedBefore + 3);
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("what ReplicatedStorage keeps reaches every replica, whatever its distance, into the replica's own")
{
    PlayedMatch match;
    const auto storageOf = [](RealSide& side) {
        const core::InstanceId id = side.world.create(side.classes.findId(side.atoms.intern("ReplicatedStorage")));
        REQUIRE(id.valid());
        side.world.setName(id, side.atoms.intern("ReplicatedStorage"));
        REQUIRE_FALSE(side.world.setParent(id, side.dataModel).has_value());
        return id;
    };
    const core::InstanceId serverStorage = storageOf(match.server);
    const core::InstanceId clientStorage = storageOf(match.client);

    match.server.world.engineState().streamingLoadRadius = 100.0;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId far = match.part("Far", core::DVec3{5000.0, 1.0, 0.0});
    // A template far away, and inside a folder, kept for everybody.
    const core::InstanceId folder =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("Folder")));
    REQUIRE_FALSE(match.server.world.setParent(folder, serverStorage).has_value());
    const core::InstanceId sword = match.part("Sword", core::DVec3{5000.0, 1.0, 0.0});
    REQUIRE_FALSE(match.server.world.setParent(sword, folder).has_value());
    match.server.world.players().find(match.remote())->character = racer;
    match.run(4);

    CHECK(match.copyOf(racer).valid());
    CHECK_FALSE(match.copyOf(far).valid());
    const core::InstanceId swordHere = match.copyOf(sword);
    REQUIRE(swordHere.valid());
    CHECK(match.client.world.parentOf(match.copyOf(folder)) == clientStorage);
    CHECK(match.client.world.parentOf(swordHere) == match.copyOf(folder));

    // It stays while the character moves, and goes when the authority drops it.
    match.server.world.parts().find(racer)->cframe.position = core::DVec3{-3000.0, 1.0, 0.0};
    match.run(4);
    CHECK(match.client.world.alive(swordHere));
    REQUIRE(match.server.world.destroy(sword));
    match.server.world.retireDestroyed();
    match.run(4);
    match.client.world.retireDestroyed();
    CHECK_FALSE(match.client.world.alive(swordHere));
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a replica joining clears the world's copy, ReplicatedStorage's copy and all of ServerStorage")
{
    RealSide side;
    const auto service = [&](std::string_view name) {
        const core::InstanceId id = side.world.create(side.classes.findId(side.atoms.intern(name)));
        REQUIRE(id.valid());
        REQUIRE_FALSE(side.world.setParent(id, side.dataModel).has_value());
        return id;
    };
    const core::InstanceId replicated = service("ReplicatedStorage");
    const core::InstanceId server = service("ServerStorage");
    const auto partIn = [&](core::InstanceId parent) {
        const core::InstanceId id = side.world.create(side.classes.findId(side.atoms.intern("Part")));
        REQUIRE_FALSE(side.world.setParent(id, parent).has_value());
        return id;
    };
    const core::InstanceId inWorld = partIn(side.workspace);
    const core::InstanceId kept = partIn(replicated);
    const core::InstanceId secret = partIn(server);

    CHECK(replication::clearForReplica(side.world, side.workspace) == 3);
    CHECK_FALSE(side.world.alive(inWorld));
    CHECK_FALSE(side.world.alive(kept));
    CHECK_FALSE(side.world.alive(secret));
    CHECK(side.world.alive(replicated));
    CHECK(side.world.alive(server));
}

TEST_CASE("a sprite on the authority is a sprite on the replica, frame and facing included")
{
    // The 2D layer on the wire (ADR 0088, protocol 11): a `Vector2` travels as
    // a `Vector3` with a zero z, and the animation state -- the sheet's frame
    // and which way it faces -- travels with the position.
    PlayedMatch match;
    const core::InstanceId hero =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("Part2D")));
    REQUIRE(hero.valid());
    scene::Part2DComponent* sprite = match.server.world.parts2d().find(hero);
    REQUIRE(sprite != nullptr);
    sprite->position = core::Vec2{3.5f, -2.25f};
    sprite->rotation = 30.0f;
    sprite->size = core::Vec2{0.8f, 0.95f};
    sprite->image = match.server.atoms.intern("asset://sprites/hero.png");
    sprite->imageRectOffset = core::Vec2{16.0f, 0.0f};
    sprite->flipX = true;
    sprite->zIndex = 5;
    sprite->shape = 2;
    REQUIRE_FALSE(match.server.world.setParent(hero, match.server.workspace).has_value());
    match.run(4);

    const core::InstanceId seen = match.copyOf(hero);
    REQUIRE(seen.valid());
    const scene::Part2DComponent* copy = match.client.world.parts2d().find(seen);
    REQUIRE(copy != nullptr);
    CHECK((copy->position == core::Vec2{3.5f, -2.25f}));
    CHECK(static_cast<double>(copy->rotation) == doctest::Approx(30.0));
    CHECK((copy->size == core::Vec2{0.8f, 0.95f}));
    CHECK(match.client.atoms.text(copy->image) == "asset://sprites/hero.png");
    CHECK((copy->imageRectOffset == core::Vec2{16.0f, 0.0f}));
    CHECK(copy->flipX);
    CHECK(copy->zIndex == 5);
    CHECK(copy->shape == 2);

    // A step of the walk cycle, turned round.
    sprite = match.server.world.parts2d().find(hero);
    sprite->position = core::Vec2{4.0f, -2.25f};
    sprite->imageRectOffset = core::Vec2{0.0f, 0.0f};
    sprite->flipX = false;
    // Long enough for the interpolation buffer to reach the newest sample
    // (ADR 0103): a remote sprite is drawn a few ticks behind.
    match.run(8);
    copy = match.client.world.parts2d().find(seen);
    CHECK((copy->position == core::Vec2{4.0f, -2.25f}));
    CHECK((copy->imageRectOffset == core::Vec2{0.0f, 0.0f}));
    CHECK_FALSE(copy->flipX);
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a remote sprite is drawn between snapshots, and turns through 180 the short way")
{
    PlayedMatch match;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    match.server.world.players().find(match.remote())->character = racer;
    const core::InstanceId kite =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("Part2D")));
    match.server.world.parts2d().find(kite)->rotation = 170.0f;
    REQUIRE_FALSE(match.server.world.setParent(kite, match.server.workspace).has_value());
    match.run(3);
    const core::InstanceId seen = match.copyOf(kite);
    REQUIRE(seen.valid());

    // A metre and two degrees a tick, counter-clockwise through 180, as the
    // solver reports it: 170, 172 ... 178, then -180, -178.
    std::vector<float> xs;
    std::vector<float> turns;
    for (int frame = 1; frame <= 40; ++frame) {
        match.tick += 1;
        match.authority->receive(match.server.world, match.server.workspace);
        scene::Part2DComponent& sprite = *match.server.world.parts2d().find(kite);
        sprite.position.x += 1.0f;
        sprite.rotation += 2.0f;
        if (sprite.rotation > 180.0f)
            sprite.rotation -= 360.0f;
        if (match.tick % 2 == 0)
            match.authority->send(match.server.world, match.server.workspace, match.tick);
        match.replica->receive(match.client.world, match.client.workspace);
        const scene::Part2DComponent& copy = *match.client.world.parts2d().find(seen);
        xs.push_back(copy.position.x);
        turns.push_back(copy.rotation);
    }
    for (std::size_t at = 20; at < xs.size(); ++at) {
        CHECK(std::fabs(xs[at] - xs[at - 1] - 1.0f) < 1.0e-4f);
        // Two degrees a tick, however the angle wrapped: never the long way.
        float turned = std::fmod(turns[at] - turns[at - 1] + 540.0f, 360.0f) - 180.0f;
        CHECK(std::fabs(turned - 2.0f) < 1.0e-3f);
    }
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a tilemap reaches a replica whole, and an edit on the authority reaches it by blocks")
{
    PlayedMatch match;
    const core::InstanceId level =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("Tilemap2D")));
    REQUIRE(level.valid());
    scene::Tilemap2DComponent* tilemap = match.server.world.tilemaps2d().find(level);
    tilemap->cellSize = 0.5f;
    for (core::i32 x = -20; x < 20; ++x)
        (void)tilemap->setCell(x, 0, 3);
    (void)tilemap->setCell(100, -40, 7);
    // Four blocks of floor, and one far off for the lone tile.
    REQUIRE(tilemap->chunks.size() == 5);
    REQUIRE_FALSE(match.server.world.setParent(level, match.server.workspace).has_value());
    match.run(4);

    const core::InstanceId seen = match.copyOf(level);
    REQUIRE(seen.valid());
    const scene::Tilemap2DComponent* copy = match.client.world.tilemaps2d().find(seen);
    REQUIRE(copy != nullptr);
    CHECK(copy->cellSize == 0.5f);
    CHECK(copy->chunks == match.server.world.tilemaps2d().find(level)->chunks);
    const core::u64 before = copy->revision;

    // A wall broken, a floor built, and a lone tile's block emptied.
    tilemap = match.server.world.tilemaps2d().find(level);
    (void)tilemap->setCell(-20, 0, 0);
    (void)tilemap->setCell(5, 5, 9);
    (void)tilemap->setCell(100, -40, 0);
    match.run(2);
    copy = match.client.world.tilemaps2d().find(seen);
    CHECK(copy->cell(-20, 0) == 0);
    CHECK(copy->cell(5, 5) == 9);
    CHECK(copy->chunks.size() == 4);
    CHECK(copy->chunks == tilemap->chunks);
    CHECK(copy->revision > before);

    // Quiet, it sends no blocks at all.
    const core::u64 revision = copy->revision;
    match.run(4);
    CHECK(match.client.world.tilemaps2d().find(seen)->revision == revision);
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a replica follows the authority to another scene without reconnecting (ADR 0106)")
{
    PlayedMatch match;
    std::vector<std::string> followed;
    std::vector<std::vector<core::u8>> carried;
    match.replica->setSceneChanger([&](scene::World& world, const std::string& path, std::vector<core::u8> data) {
        followed.push_back(path);
        carried.push_back(data);
        // What the host does: the scene is this world's now.
        world.engineState().currentScene = path;
    });
    const net::PeerId before = match.toServer;

    // The authority changes scene, with something for the new one.
    match.server.world.engineState().currentScene = "scenes/arena.scene.json";
    match.server.world.engineState().sceneLoadData = {1, 2, 3};
    match.run(3);
    REQUIRE(followed.size() == 1);
    CHECK(followed.front() == "scenes/arena.scene.json");
    CHECK(carried.front() == std::vector<core::u8>{1, 2, 3});
    CHECK(match.replica->welcomed());
    CHECK(match.toServer == before);

    // Once: a scene the replica is already in is not loaded again.
    match.run(3);
    CHECK(followed.size() == 1);

    // And again when it changes again.
    match.server.world.engineState().currentScene = "scenes/lobby.scene.json";
    match.run(3);
    REQUIRE(followed.size() == 2);
    CHECK(followed.back() == "scenes/lobby.scene.json");
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("attributes replicate: set, changed and removed on the authority, seen on the replica (ADR 0106)")
{
    PlayedMatch match;
    const core::InstanceId crate = match.part("Crate", core::DVec3{0.0, 1.0, 0.0});
    scene::World& server = match.server.world;
    const core::NameAtom health = server.atoms().intern("Health");
    const core::NameAtom owner = server.atoms().intern("Owner");
    REQUIRE(server.setAttribute(crate, health, scene::Value{100.0}));
    REQUIRE(server.setAttribute(crate, owner, scene::Value{std::string("red")}));
    match.run(4);

    const core::InstanceId seen = match.copyOf(crate);
    REQUIRE(seen.valid());
    scene::World& client = match.client.world;
    CHECK(client.getAttribute(seen, client.atoms().intern("Health")) == scene::Value{100.0});
    CHECK(client.getAttribute(seen, client.atoms().intern("Owner")) == scene::Value{std::string("red")});

    // Changed, and one removed.
    REQUIRE(server.setAttribute(crate, health, scene::Value{40.0}));
    REQUIRE(server.setAttribute(crate, owner, scene::Value{}));
    match.run(3);
    CHECK(client.getAttribute(seen, client.atoms().intern("Health")) == scene::Value{40.0});
    CHECK(client.getAttribute(seen, client.atoms().intern("Owner")) == scene::Value{});

    // A player's attribute reaches the replica's copy of that player: a lobby's
    // "Ready" with no RemoteEvent.
    const core::InstanceId remote = match.remote();
    REQUIRE(remote.valid());
    REQUIRE(server.setAttribute(remote, server.atoms().intern("Ready"), scene::Value{true}));
    match.run(3);
    CHECK(client.getAttribute(match.me, client.atoms().intern("Ready")) == scene::Value{true});

    // A quiet world sends no attributes again: the replica's own write stands.
    REQUIRE(client.setAttribute(seen, client.atoms().intern("Health"), scene::Value{7.0}));
    match.run(3);
    CHECK(client.getAttribute(seen, client.atoms().intern("Health")) == scene::Value{7.0});
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("the world's blur travels under Lighting, and a camera's stays on its own screen (ADR 0096)")
{
    PlayedMatch match;
    scene::World& world = match.server.world;
    const auto make = [&](std::string_view className, std::string_view name, core::InstanceId parent) {
        const core::InstanceId id = world.create(match.server.classes.findId(match.server.atoms.intern(className)));
        REQUIRE(id.valid());
        world.setName(id, match.server.atoms.intern(name));
        REQUIRE_FALSE(world.setParent(id, parent).has_value());
        return id;
    };

    // The authority lays a blur over its world, and its own camera wears one
    // of its own -- a pause menu nobody else is looking at.
    const core::InstanceId worlds = make("BlurEffect", "WorldBlur", match.server.lighting);
    world.blurEffects().find(worlds)->size = 12.0f;
    const core::InstanceId camera = make("Camera", "Camera", match.server.workspace);
    (void)make("BlurEffect", "PauseBlur", camera);
    match.run(4);

    // The world's arrives under the replica's own Lighting, with its size.
    core::InstanceId seen;
    for (core::InstanceId child = match.client.world.firstChild(match.client.lighting); child.valid();
         child = match.client.world.nextSibling(child)) {
        if (match.client.world.name(child) == match.client.atoms.intern("WorldBlur"))
            seen = child;
    }
    REQUIRE(seen.valid());
    REQUIRE(match.client.world.blurEffects().find(seen) != nullptr);
    CHECK(static_cast<double>(match.client.world.blurEffects().find(seen)->size) == doctest::Approx(12.0));

    // Switched off on the authority, off on the replica.
    world.postEffects().find(worlds)->enabled = false;
    match.run(3);
    CHECK_FALSE(match.client.world.postEffects().find(seen)->enabled);

    // And the camera's never crossed: the replica holds one blur, the world's.
    core::usize blurs = 0;
    match.client.world.blurEffects().forEach([&](core::InstanceId, const scene::BlurEffectComponent&) { ++blurs; });
    CHECK(blurs == 1);
    CHECK(match.replica->checksumFailures() == 0);
}

// --- ADR 0099 -------------------------------------------------------------------

namespace {

// A `TeamService` on one side, with its teams made by `team`.
core::InstanceId teamServiceOf(RealSide& side)
{
    const core::InstanceId id = side.world.create(side.classes.findId(side.atoms.intern("TeamService")));
    REQUIRE(id.valid());
    side.world.setName(id, side.atoms.intern("TeamService"));
    REQUIRE_FALSE(side.world.setParent(id, side.dataModel).has_value());
    return id;
}

core::InstanceId team(RealSide& side, core::InstanceId service, std::string_view name, core::Color3 color)
{
    const core::InstanceId id = side.world.create(side.classes.findId(side.atoms.intern("Team")));
    REQUIRE(id.valid());
    side.world.setName(id, side.atoms.intern(name));
    side.world.teams().find(id)->color = color;
    REQUIRE_FALSE(side.world.setParent(id, service).has_value());
    return id;
}

} // namespace

TEST_CASE("a player who joins is put on the open team with the fewest players, and a team gone is none")
{
    RealSide side;
    const core::InstanceId service = teamServiceOf(side);
    const core::InstanceId red = team(side, service, "Red", {1.0f, 0.0f, 0.0f});
    const core::InstanceId closed = team(side, service, "Referees", {0.0f, 0.0f, 0.0f});
    side.world.teams().find(closed)->autoAssign = false;
    const core::InstanceId blue = team(side, service, "Blue", {0.0f, 0.0f, 1.0f});

    const core::InstanceId first = scene::createPlayer(side.world, side.network, 1, true);
    const core::InstanceId second = scene::createPlayer(side.world, side.network, 2, false);
    const core::InstanceId third = scene::createPlayer(side.world, side.network, 3, false);
    // Ties to the first in child order; never the closed one.
    CHECK(side.world.players().find(first)->team == red);
    CHECK(side.world.players().find(second)->team == blue);
    CHECK(side.world.players().find(third)->team == red);

    REQUIRE(side.world.destroy(red));
    side.world.retireDestroyed();
    CHECK_FALSE(side.world.players().find(first)->team.valid());
}

TEST_CASE("each machine's Player.Team is its own copy of the team the authority named, colour and all")
{
    PlayedMatch match;
    const core::InstanceId serverTeams = teamServiceOf(match.server);
    const core::InstanceId clientTeams = teamServiceOf(match.client);
    const core::InstanceId red = team(match.server, serverTeams, "Red", {1.0f, 0.2f, 0.2f});
    match.server.world.engineState().streamingLoadRadius = 50.0;
    const core::InstanceId racer = match.part("Racer", core::DVec3{0.0, 1.0, 0.0});
    match.server.world.players().find(match.remote())->character = racer;
    match.server.world.players().find(match.remote())->team = red;
    match.run(4);

    // Whatever the distance -- a team has none -- into the replica's own service.
    const core::InstanceId redHere = match.copyOf(red);
    REQUIRE(redHere.valid());
    CHECK(match.client.world.parentOf(redHere) == clientTeams);
    CHECK(static_cast<double>(match.client.world.teams().find(redHere)->color.g) == doctest::Approx(0.2));
    CHECK(match.client.world.players().find(match.me)->team == redHere);

    // A side taken away is taken away everywhere.
    match.server.world.players().find(match.remote())->team = {};
    match.run(2);
    CHECK_FALSE(match.client.world.players().find(match.me)->team.valid());
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a part handed to a replica is simulated there, followed by the authority, and handed back")
{
    PlayedMatch match;
    const core::InstanceId ball = match.part("Ball", core::DVec3{0.0, 1.0, 0.0});
    match.run(3);
    const core::InstanceId mine = match.copyOf(ball);
    REQUIRE(mine.valid());

    match.server.world.rigidBodies().find(ball)->networkOwner = 2;
    match.run(3);
    CHECK(match.client.world.rigidBodies().find(mine)->networkOwner == 2);

    // The owner kicks it: the authority takes where it went, and the replica's
    // own copy is not pulled back by a snapshot a round trip old.
    match.client.world.parts().find(mine)->cframe.position = core::DVec3{7.0, 1.0, 0.0};
    match.client.world.rigidBodies().find(mine)->linearVelocity = core::Vec3{3.0f, 0.0f, 0.0f};
    match.run(3);
    CHECK(match.server.world.parts().find(ball)->cframe.position.x == doctest::Approx(7.0));
    CHECK(static_cast<double>(match.server.world.rigidBodies().find(ball)->linearVelocity.x) == doctest::Approx(3.0));
    match.client.world.parts().find(mine)->cframe.position = core::DVec3{8.0, 1.0, 0.0};
    match.run(1);
    CHECK(match.client.world.parts().find(mine)->cframe.position.x == doctest::Approx(8.0));

    // Handed back: the state still in flight -- the 8 -- is dropped, since the
    // authority takes only what it gave and it has taken this back; the
    // replica's copy goes back to where the authority has it, not where it had
    // rolled to since; and what the replica does to it no longer reaches the
    // authority.
    match.server.world.rigidBodies().find(ball)->networkOwner = 0;
    match.client.world.parts().find(mine)->cframe.position = core::DVec3{12.0, 1.0, 0.0};
    match.run(1);
    CHECK(match.client.world.rigidBodies().find(mine)->networkOwner == 0);
    const double settled = match.server.world.parts().find(ball)->cframe.position.x;
    CHECK(settled == doctest::Approx(7.0));
    CHECK(match.client.world.parts().find(mine)->cframe.position.x == doctest::Approx(settled));
    match.client.world.parts().find(mine)->cframe.position = core::DVec3{-40.0, 1.0, 0.0};
    match.run(8);
    CHECK(match.server.world.parts().find(ball)->cframe.position.x == doctest::Approx(settled));
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("a player who leaves gives back every part they owned")
{
    PlayedMatch match;
    const core::InstanceId ball = match.part("Ball", core::DVec3{0.0, 1.0, 0.0});
    match.server.world.rigidBodies().find(ball)->networkOwner = 2;
    scene::removePlayer(match.server.world, match.server.network, match.remote());
    CHECK(match.server.world.rigidBodies().find(ball)->networkOwner == 0);
}

TEST_CASE("a detector replicates, and a replica's click reaches the authority from its own player")
{
    PlayedMatch match;
    const core::InstanceId crate = match.part("Crate", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId detector =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("ClickDetector")));
    REQUIRE(detector.valid());
    REQUIRE_FALSE(match.server.world.setParent(detector, crate).has_value());
    match.server.world.clickDetectors().find(detector)->maxActivationDistance = 12.0;
    const core::InstanceId prompt =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("ProximityPrompt")));
    REQUIRE_FALSE(match.server.world.setParent(prompt, crate).has_value());
    match.server.world.proximityPrompts().find(prompt)->actionText = match.server.atoms.intern("Open");
    match.run(3);

    // The replica has both, with what the authority set.
    const core::InstanceId detectorHere = match.copyOf(detector);
    const core::InstanceId promptHere = match.copyOf(prompt);
    REQUIRE(detectorHere.valid());
    REQUIRE(promptHere.valid());
    CHECK(match.client.world.clickDetectors().find(detectorHere)->maxActivationDistance == 12.0);
    CHECK(match.client.atoms.text(match.client.world.proximityPrompts().find(promptHere)->actionText) == "Open");

    match.client.world.engineState().detectorOutbox.push_back(
        scene::DetectorMessage{detectorHere, {}, scene::DetectorMessage::Kind::Click, 0});
    match.client.world.engineState().detectorOutbox.push_back(
        scene::DetectorMessage{promptHere, {}, scene::DetectorMessage::Kind::Triggered, 0});
    match.run(2);

    const std::vector<scene::DetectorMessage>& arrived = match.server.world.engineState().detectorInbox;
    REQUIRE(arrived.size() == 2);
    CHECK(arrived[0].detector == detector);
    CHECK(arrived[0].kind == scene::DetectorMessage::Kind::Click);
    // **The sender is the connection's player**, never one the message names.
    CHECK(arrived[0].player == match.remote());
    CHECK(arrived[1].detector == prompt);
    CHECK(arrived[1].kind == scene::DetectorMessage::Kind::Triggered);
}

TEST_CASE("a drag detector replicates, and a replica's drag reaches the authority with its ray (ADR 0126 §3)")
{
    PlayedMatch match;
    const core::InstanceId drawer = match.part("Drawer", core::DVec3{0.0, 1.0, 0.0});
    const core::InstanceId detector =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("DragDetector")));
    REQUIRE(detector.valid());
    REQUIRE_FALSE(match.server.world.setParent(detector, drawer).has_value());
    scene::DragDetectorComponent* component = match.server.world.dragDetectors().find(detector);
    component->dragStyle = 0;
    component->axis = core::Vec3{1.0f, 0.0f, 0.0f};
    component->maxDragTranslation = 0.5;
    match.run(3);

    const core::InstanceId here = match.copyOf(detector);
    REQUIRE(here.valid());
    const scene::DragDetectorComponent* copy = match.client.world.dragDetectors().find(here);
    REQUIRE(copy != nullptr);
    CHECK(copy->dragStyle == 0);
    CHECK(copy->axis.x == doctest::Approx(1.0));
    CHECK(copy->maxDragTranslation == doctest::Approx(0.5));

    scene::DetectorMessage began{here, {}, scene::DetectorMessage::Kind::DragStart, 0};
    began.origin = core::DVec3{0.0, 5.0, 0.0};
    began.direction = core::Vec3{0.0f, -1.0f, 0.0f};
    began.hit = core::DVec3{0.0, 1.5, 0.0};
    match.client.world.engineState().detectorOutbox.push_back(began);
    // A ray that is not one is dropped on the way in.
    scene::DetectorMessage broken = began;
    broken.kind = scene::DetectorMessage::Kind::DragContinue;
    broken.direction = core::Vec3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    match.client.world.engineState().detectorOutbox.push_back(broken);
    match.client.world.engineState().detectorOutbox.push_back(
        scene::DetectorMessage{here, {}, scene::DetectorMessage::Kind::DragEnd, 0});
    match.run(2);

    const std::vector<scene::DetectorMessage>& arrived = match.server.world.engineState().detectorInbox;
    REQUIRE(arrived.size() == 2);
    CHECK(arrived[0].detector == detector);
    CHECK(arrived[0].kind == scene::DetectorMessage::Kind::DragStart);
    CHECK(arrived[0].player == match.remote());
    CHECK(arrived[0].origin.y == doctest::Approx(5.0));
    CHECK(arrived[0].direction.y == doctest::Approx(-1.0));
    CHECK(arrived[0].hit.y == doctest::Approx(1.5));
    CHECK(arrived[1].kind == scene::DetectorMessage::Kind::DragEnd);
}

TEST_CASE("a peer's non-finite state and invented intents do not reach the authority (audit E3)")
{
    PlayedMatch match;
    const core::InstanceId ball = match.part("Ball", core::DVec3{0.0, 1.0, 0.0});
    match.run(3);
    const core::InstanceId mine = match.copyOf(ball);
    REQUIRE(mine.valid());
    match.server.world.rigidBodies().find(ball)->networkOwner = 2;
    match.run(3);

    // The owner reports a NaN position, then an infinite speed: neither is
    // taken, and the ball stays a ball in a world.
    const double before = match.server.world.parts().find(ball)->cframe.position.x;
    match.client.world.parts().find(mine)->cframe.position.x = std::numeric_limits<double>::quiet_NaN();
    match.run(2);
    CHECK(std::isfinite(match.server.world.parts().find(ball)->cframe.position.x));
    CHECK(match.server.world.parts().find(ball)->cframe.position.x == doctest::Approx(before));
    match.client.world.parts().find(mine)->cframe.position.x = 2.0;
    match.client.world.rigidBodies().find(mine)->linearVelocity =
        core::Vec3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
    match.run(2);
    CHECK(std::isfinite(match.server.world.rigidBodies().find(ball)->linearVelocity.x));

    // An action the authority never named is dropped, not interned; a known
    // one with a NaN axis arrives with the NaN made 0.
    (void)match.server.atoms.intern("Move");
    const core::usize atomsBefore = match.server.atoms.size();
    match.client.world.players().find(match.me)->intents = {
        scene::PlayerIntent{match.client.atoms.intern("Invented1234"), 0, core::Vec3{}, true},
        scene::PlayerIntent{match.client.atoms.intern("Move"), 2,
                            core::Vec3{std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f}, false},
    };
    match.run(3);
    CHECK_FALSE(match.server.atoms.lookup("Invented1234").valid());
    CHECK(match.server.atoms.size() == atomsBefore);
    const scene::PlayerComponent* seen = match.server.world.players().find(match.remote());
    REQUIRE(seen != nullptr);
    REQUIRE(seen->intents.size() == 1);
    CHECK(seen->intents[0].axis.x == 0.0f);
    CHECK(seen->intents[0].axis.y == 1.0f);
}

// --- Hostile peers (audit N1's review) -----------------------------------------------------

namespace {

// A message as a hostile peer writes it: little-endian, whatever it likes.
struct Bytes
{
    std::vector<core::u8> data;

    Bytes& u8v(core::u8 value)
    {
        data.push_back(value);
        return *this;
    }
    Bytes& u16v(core::u16 value)
    {
        for (int at = 0; at < 2; ++at)
            data.push_back(static_cast<core::u8>(value >> (8 * at)));
        return *this;
    }
    Bytes& u32v(core::u32 value)
    {
        for (int at = 0; at < 4; ++at)
            data.push_back(static_cast<core::u8>(value >> (8 * at)));
        return *this;
    }
    Bytes& u64v(core::u64 value)
    {
        for (int at = 0; at < 8; ++at)
            data.push_back(static_cast<core::u8>(value >> (8 * at)));
        return *this;
    }
    Bytes& f32v(float value)
    {
        core::u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32v(bits);
    }
    Bytes& f64v(double value)
    {
        core::u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u64v(bits);
    }
    Bytes& text(std::string_view value)
    {
        u16v(static_cast<core::u16>(value.size()));
        data.insert(data.end(), value.begin(), value.end());
        return *this;
    }
    // A `CFrameD` on the wire: three f64 of position, nine f32 of rotation.
    Bytes& cframe(core::DVec3 at, float scale = 1.0f)
    {
        f64v(at.x).f64v(at.y).f64v(at.z);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column)
                f32v(row == column ? scale : 0.0f);
        }
        return *this;
    }
};

// A replica joined to a fake authority: the test is the server, and writes
// whatever bytes it likes to the replica.
struct FakeAuthority
{
    std::shared_ptr<net::MemoryNetwork> network = net::createMemoryNetwork();
    std::unique_ptr<net::ITransport> server = net::createMemoryTransport(network);
    std::unique_ptr<net::ITransport> clientTransport = net::createMemoryTransport(network);
    std::optional<ReplicaSession> replica;
    Side client;
    net::PeerId peer;

    FakeAuthority()
    {
        seedCatalog();
        REQUIRE_FALSE(server->open(net::TransportConfig{.port = Port, .maxPeers = 4, .channels = 4}).has_value());
        REQUIRE_FALSE(clientTransport->open(net::TransportConfig{.port = 0, .maxPeers = 1, .channels = 4}).has_value());
        net::PeerId toServer;
        REQUIRE_FALSE(clientTransport->connect("memory", Port, toServer).has_value());
        replica.emplace(*clientTransport, toServer);
        replica->receive(client.world(), client.root);
        std::vector<net::TransportEvent> events;
        (void)server->poll(events, 0);
        for (const net::TransportEvent& event : events) {
            if (event.kind == net::TransportEvent::Kind::Connected)
                peer = event.peer;
        }
        REQUIRE(peer.valid());
    }

    void deliver(const Bytes& message)
    {
        REQUIRE_FALSE(server->send(peer, message.data, net::Delivery::Reliable, 0).has_value());
        replica->receive(client.world(), client.root);
    }
};

} // namespace

TEST_CASE("a hostile authority's message is refused whole, and never sizes the replica's memory")
{
    FakeAuthority fake;
    bool changed = false;
    fake.replica->setSceneChanger([&](scene::World&, const std::string&, std::vector<core::u8>) { changed = true; });

    // Seven bytes claiming four gigabytes of scene data: reserved from the
    // claim, the allocation failed and took the process with it.
    fake.deliver(Bytes{}.u8v(14).text("x").u32v(0xFFFFFFFFu));
    CHECK_FALSE(changed);
    // The same message telling the truth still changes the scene.
    fake.deliver(Bytes{}.u8v(14).text("x").u32v(2).u8v(7).u8v(9));
    CHECK(changed);

    // Sixty-five thousand tile blocks, none of them there.
    fake.deliver(Bytes{}.u8v(13).u32v(5).u16v(0xFFFF));

    // A class this build has never heard of is not interned for ever.
    fake.deliver(Bytes{}.u8v(3).u32v(1).u32v(5000).text("NoSuchClassFromAPeer"));
    CHECK_FALSE(fake.client.world().atoms().lookup("NoSuchClassFromAPeer").valid());
    CHECK_FALSE(fake.replica->localOf(NetId{5000}).valid());

    // Records out of order are refused before anything is built from them:
    // inserted one at a time, a descending snapshot cost its size squared.
    Bytes snapshot;
    snapshot.u8v(5).u64v(1).u64v(0).u64v(0).u64v(0).u8v(0).u32v(0).u16v(0).u32v(2);
    snapshot.u32v(9).u8v(0).u8v(1).u16v(0);
    snapshot.u32v(8).u8v(0).u8v(1).u16v(0);
    fake.deliver(snapshot);
    CHECK(fake.replica->appliedTick() == 0);
    CHECK(fake.replica->checksumFailures() == 0);
}

TEST_CASE("a peer's intents and owned states are bounded a tick, and an owned state is taken whole or not at all")
{
    PlayedMatch match;
    const core::InstanceId ball = match.part("Ball", core::DVec3{0.0, 1.0, 0.0});
    match.run(3);
    match.server.world.rigidBodies().find(ball)->networkOwner = 2;
    match.run(3);
    const NetId ballNet = match.authority->netIdOf(ball);
    REQUIRE(ballNet.valid());
    (void)match.server.atoms.intern("Move");

    // Twenty intent messages in one tick: eight are read -- the replica's own
    // from the last step among them -- and the rest are a peer making the
    // authority parse.
    const core::u64 droppedBefore = match.authority->stats().messagesDropped;
    for (core::u32 at = 1; at <= 20; ++at) {
        Bytes intent;
        intent.u8v(7)
            .u8v(1)
            .u64v(100000 + at)
            .u16v(1)
            .text("Move")
            .u8v(2)
            .f32v(static_cast<float>(at))
            .f32v(0.0f)
            .f32v(0.0f)
            .u8v(1);
        REQUIRE_FALSE(
            match.clientTransport->send(match.toServer, intent.data, net::Delivery::Unreliable, 2).has_value());
    }
    match.authority->receive(match.server.world, match.server.workspace);
    CHECK(match.authority->stats().messagesDropped == droppedBefore + 13);

    // More ticks than a message carries, or more entries than an input map
    // has, is refused whole.
    Bytes five;
    five.u8v(7).u8v(5);
    for (core::u64 at = 0; at < 5; ++at)
        five.u64v(200000 + at).u16v(0);
    Bytes many;
    many.u8v(7).u8v(1).u64v(300000).u16v(300);
    for (int at = 0; at < 300; ++at)
        many.text("Move").u8v(2).f32v(9.0f).f32v(0.0f).f32v(0.0f).u8v(1);
    REQUIRE_FALSE(match.clientTransport->send(match.toServer, five.data, net::Delivery::Unreliable, 2).has_value());
    REQUIRE_FALSE(match.clientTransport->send(match.toServer, many.data, net::Delivery::Unreliable, 2).has_value());
    match.authority->receive(match.server.world, match.server.workspace);
    CHECK(match.authority->stats().messagesDropped == droppedBefore + 15);

    // An owned state whose second record is cut short moves nothing -- and
    // does not make the whole one after it, at the same tick, look old.
    Bytes cut;
    cut.u8v(12).u64v(500000).u16v(2);
    cut.u32v(ballNet.value).cframe({5.0, 1.0, 0.0}).f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
    cut.u32v(ballNet.value).u8v(1).u8v(2);
    Bytes whole;
    whole.u8v(12).u64v(500000).u16v(1);
    whole.u32v(ballNet.value).cframe({6.0, 1.0, 0.0}).f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
    REQUIRE_FALSE(match.clientTransport->send(match.toServer, cut.data, net::Delivery::Unreliable, 3).has_value());
    match.authority->receive(match.server.world, match.server.workspace);
    CHECK(match.server.world.parts().find(ball)->cframe.position.x == doctest::Approx(0.0));
    REQUIRE_FALSE(match.clientTransport->send(match.toServer, whole.data, net::Delivery::Unreliable, 3).has_value());
    match.authority->receive(match.server.world, match.server.workspace);
    CHECK(match.server.world.parts().find(ball)->cframe.position.x == doctest::Approx(6.0));

    // A rotation that is a scale is not a rotation.
    Bytes scaled;
    scaled.u8v(12).u64v(500001).u16v(1);
    scaled.u32v(ballNet.value)
        .cframe({9.0, 1.0, 0.0}, 2.0f)
        .f32v(0.0f)
        .f32v(0.0f)
        .f32v(0.0f)
        .f32v(0.0f)
        .f32v(0.0f)
        .f32v(0.0f);
    REQUIRE_FALSE(match.clientTransport->send(match.toServer, scaled.data, net::Delivery::Unreliable, 3).has_value());
    match.authority->receive(match.server.world, match.server.workspace);
    CHECK(match.server.world.parts().find(ball)->cframe.position.x == doctest::Approx(6.0));
}

TEST_CASE("a connection that never says hello is let go, and a message naming too many instances is refused")
{
    PlayedMatch match;
    auto silent = net::createMemoryTransport(match.network);
    REQUIRE_FALSE(silent->open(net::TransportConfig{.port = 0, .maxPeers = 1, .channels = 4}).has_value());
    net::PeerId toServer;
    REQUIRE_FALSE(silent->connect("memory", Port, toServer).has_value());
    match.authority->receive(match.server.world, match.server.workspace);
    CHECK(match.serverTransport->peerCount() == 2);
    for (core::u32 at = 0; at <= MaxUnwelcomedReceives; ++at)
        match.authority->receive(match.server.world, match.server.workspace);
    CHECK(match.serverTransport->peerCount() == 1);
    CHECK(match.authority->peerCount() == 1);

    // A `RemoteEvent` message naming more instances than any call passes.
    const core::InstanceId remote =
        match.server.world.create(match.server.classes.findId(match.server.atoms.intern("RemoteEvent")));
    REQUIRE_FALSE(match.server.world.setParent(remote, match.server.workspace).has_value());
    match.run(3);
    const NetId remoteNet = match.authority->netIdOf(remote);
    REQUIRE(remoteNet.valid());
    Bytes flood;
    flood.u8v(9).u32v(remoteNet.value).u32v(0).u8v(0).u16v(static_cast<core::u16>(MaxRemoteRefs + 1));
    for (core::u32 at = 0; at <= MaxRemoteRefs; ++at)
        flood.u32v(remoteNet.value);
    flood.u32v(0);
    REQUIRE_FALSE(match.clientTransport->send(match.toServer, flood.data, net::Delivery::Reliable, 0).has_value());
    match.authority->receive(match.server.world, match.server.workspace);
    CHECK(match.server.world.engineState().remoteInbox.empty());
}

TEST_CASE("a press whose intent came after its tick was stood in for is applied late, not lost (ADR 0133)")
{
    PlayedMatch match;
    match.run(10);
    (void)match.server.atoms.intern("Jump");
    const scene::PlayerComponent* player = match.server.world.players().find(match.remote());
    REQUIRE(player != nullptr);
    const auto send = [&](core::u64 tick, bool jump) {
        Bytes intent;
        intent.u8v(7).u8v(1).u64v(tick);
        if (jump)
            intent.u16v(1).text("Jump").u8v(0).f32v(0.0f).f32v(0.0f).f32v(0.0f).u8v(1);
        else
            intent.u16v(0);
        REQUIRE_FALSE(
            match.clientTransport->send(match.toServer, intent.data, net::Delivery::Unreliable, 2).has_value());
    };
    const auto jumping = [&] {
        return std::any_of(player->intents.begin(), player->intents.end(),
                           [&](const scene::PlayerIntent& intent) { return intent.pressed; });
    };
    // The next two ticks arrive; the third does not, and the authority stands
    // the second in for it -- run until it has.
    const core::u64 first = match.tick + 1;
    send(first, false);
    send(first + 1, false);
    const core::u64 starvedBefore = match.authority->stats().intentStarvations;
    for (int at = 0; at < 30 && match.authority->stats().intentStarvations < starvedBefore + 2; ++at)
        match.authority->receive(match.server.world, match.server.workspace);
    REQUIRE(match.authority->stats().intentStarvations >= starvedBefore + 2);
    CHECK_FALSE(jumping());
    // Then the third arrives -- a jump, pressed for that one tick -- with the
    // fourth, both too late for their ticks: the jump is applied once, late.
    send(first + 2, true);
    send(first + 3, false);
    int jumped = 0;
    for (int at = 0; at < 10; ++at) {
        match.authority->receive(match.server.world, match.server.workspace);
        jumped += jumping() ? 1 : 0;
    }
    CHECK(jumped == 1);
}

// --- The ground (ADR 0135) ------------------------------------------------------

namespace {

[[nodiscard]] core::InstanceId terrainIn(scene::World& world, core::InstanceId workspace)
{
    for (core::InstanceId child = world.firstChild(workspace); child.valid(); child = world.nextSibling(child)) {
        if (world.terrains().find(child) != nullptr)
            return child;
    }
    return {};
}

[[nodiscard]] core::InstanceId makeTerrain(RealSide& side)
{
    const core::InstanceId id = side.world.create(side.classes.findId(side.atoms.intern("Terrain")));
    REQUIRE(id.valid());
    side.world.setName(id, side.atoms.intern("Terrain"));
    REQUIRE_FALSE(side.world.setParent(id, side.workspace).has_value());
    return id;
}

[[nodiscard]] core::InstanceId makeVoxels(RealSide& side)
{
    const core::InstanceId id = side.world.create(side.classes.findId(side.atoms.intern("VoxelService")));
    REQUIRE(id.valid());
    REQUIRE(side.world.voxels().find(id) != nullptr);
    REQUIRE_FALSE(side.world.setParent(id, side.dataModel).has_value());
    return id;
}

} // namespace

TEST_CASE("ground a server makes in a script reaches a replica whole, and each edit after it by chunks (ADR 0135)")
{
    // **The owner, 2026-09-29**: a digging game, and a world generated while
    // the match runs. Before, the ground was each machine's own scene, and what
    // a script did to it stayed where it was done.
    PlayedMatch match;
    const core::InstanceId ground = makeTerrain(match.server);
    scene::TerrainComponent* terrain = match.server.world.terrains().find(ground);
    terrain->field.setHeightRange(-32.0f, 32.0f);
    (void)asset::fillFlat(terrain->field, core::DVec3{0.0, 0.0, 0.0}, 64.0f, 2.0f, 1);
    terrain->layers = {"asset://materials/terrain/grass.material.json", "asset://materials/terrain/rock.material.json"};
    terrain->fieldRevision += 1;
    match.run(4);

    const core::InstanceId copy = terrainIn(match.client.world, match.client.workspace);
    REQUIRE(copy.valid());
    const scene::TerrainComponent* seen = match.client.world.terrains().find(copy);
    REQUIRE(seen != nullptr);
    CHECK(seen->field.digest() == terrain->field.digest());
    CHECK(seen->layers == terrain->layers);

    // A crater dug on the server: the chunks it touched, and no others.
    terrain = match.server.world.terrains().find(ground);
    (void)asset::fillBall(terrain->field, core::DVec3{3.0, 2.0, 3.0}, 5.0, 0);
    terrain->fieldRevision += 1;
    match.run(2);
    seen = match.client.world.terrains().find(copy);
    CHECK(seen->field.digest() == terrain->field.digest());
    CHECK(asset::sampleField(seen->field, core::DVec3{3.0, 1.0, 3.0}).distance > 0.0f);

    // A rule added: the look travels whole.
    terrain->rules = asset::defaultTerrainRules();
    match.run(2);
    CHECK(match.client.world.terrains().find(copy)->rules == terrain->rules);

    // Quiet, it sends nothing more.
    const core::u64 revision = match.client.world.terrains().find(copy)->fieldRevision;
    match.run(4);
    CHECK(match.client.world.terrains().find(copy)->fieldRevision == revision);
    CHECK(match.replica->checksumFailures() == 0);
}

TEST_CASE("ground both ends loaded from the scene is not sent; what a script changed of it is (ADR 0135)")
{
    PlayedMatch match;
    // The same scene's ground on both machines, as a scene read leaves it: the
    // field, and the package's copy of it sharing every chunk.
    for (RealSide* side : {&match.server, &match.client}) {
        scene::TerrainComponent* terrain = side->world.terrains().find(makeTerrain(*side));
        terrain->field.setHeightRange(-32.0f, 32.0f);
        (void)asset::fillFlat(terrain->field, core::DVec3{0.0, 0.0, 0.0}, 64.0f, 2.0f, 1);
        terrain->shipped = terrain->field;
        terrain->fieldRevision += 1;
    }
    match.run(3);
    const core::InstanceId serverGround = terrainIn(match.server.world, match.server.workspace);
    const core::InstanceId clientGround = terrainIn(match.client.world, match.client.workspace);
    scene::TerrainComponent* server = match.server.world.terrains().find(serverGround);
    const scene::TerrainComponent* client = match.client.world.terrains().find(clientGround);
    // Nothing differed, so the replica's chunks are still its own.
    const asset::ChunkKey far{0, 0, 0};
    const asset::TerrainChunk* untouched = nullptr;
    for (const auto& [key, chunk] : client->field.chunks()) {
        if (key == far)
            untouched = chunk.get();
    }
    REQUIRE(untouched != nullptr);

    // A hole dug near the origin: the replica gets it, and keeps the rest.
    (void)asset::fillBall(server->field, core::DVec3{-4.0, 2.0, -4.0}, 3.0, 0);
    server->fieldRevision += 1;
    match.run(2);
    client = match.client.world.terrains().find(clientGround);
    CHECK(client->field.digest() == server->field.digest());
    const asset::TerrainChunk* still = nullptr;
    for (const auto& [key, chunk] : client->field.chunks()) {
        if (key == far)
            still = chunk.get();
    }
    CHECK(still == untouched);
}

TEST_CASE("a chunk removed before a replica loaded its cell stays removed when it does (terrain audit G4)")
{
    // The replica's streamer loads the cell after the removal arrived: the
    // field held nothing there either way, so the load merged the package's
    // chunk back over the authority's word.
    PlayedMatch match;
    const asset::FieldSettings settings{.voxelSize = 1.0f, .minHeight = -32.0f, .maxHeight = 32.0f};
    asset::TerrainField package(settings);
    (void)asset::fillFlat(package, core::DVec3{0.0, 0.0, 0.0}, 64.0f, 2.0f, 1);
    const asset::ChunkKey gone{0, 0, 0};
    REQUIRE(package.findChunk(gone) != nullptr);

    // The server loaded the cell and dug the chunk away; the replica has not
    // loaded it yet.
    scene::TerrainComponent* server = match.server.world.terrains().find(makeTerrain(match.server));
    server->field = package;
    server->shipped = package;
    server->field.removeChunk(gone);
    server->fieldRevision += 1;
    scene::TerrainComponent* client = match.client.world.terrains().find(makeTerrain(match.client));
    client->field = asset::TerrainField(settings);
    client->shipped = asset::TerrainField(settings);
    match.run(3);

    // Now the cell loads there, the way the streamer loads one.
    client = match.client.world.terrains().find(terrainIn(match.client.world, match.client.workspace));
    client->field.shareFrom(package, client->shipped);
    client->shipped.shareFrom(package);
    CHECK(client->field.findChunk(gone) == nullptr);
    CHECK(client->field.findChunk(asset::ChunkKey{-1, 0, -1}) != nullptr);
}

TEST_CASE("an edit that came back to the package's bytes is not sent as a hole when the cell streams out (terrain "
          "audit G5)")
{
    PlayedMatch match;
    for (RealSide* side : {&match.server, &match.client}) {
        scene::TerrainComponent* terrain = side->world.terrains().find(makeTerrain(*side));
        terrain->field.setHeightRange(-32.0f, 32.0f);
        (void)asset::fillFlat(terrain->field, core::DVec3{0.0, 0.0, 0.0}, 64.0f, 2.0f, 1);
        terrain->shipped = terrain->field;
        terrain->fieldRevision += 1;
    }
    match.run(3);
    scene::TerrainComponent* server =
        match.server.world.terrains().find(terrainIn(match.server.world, match.server.workspace));
    // Painted and painted back: the same bytes in a chunk of its own.
    const asset::Voxel was = server->field.voxel(3, 1, 3);
    (void)server->field.setVoxel(3, 1, 3, asset::Voxel{asset::FullOccupancy, 7});
    (void)server->field.setVoxel(3, 1, 3, was);
    server->fieldRevision += 1;
    match.run(2);

    // The server's streamer lets the cell go as untouched -- its digest is the
    // package's -- from the ground and the package's copy together.
    const asset::ChunkKey key{0, 0, 0};
    REQUIRE(server->field.findChunk(key) != nullptr);
    server->field.removeChunk(key);
    server->shipped.removeChunk(key);
    server->fieldRevision += 1;
    match.run(2);

    const scene::TerrainComponent* client =
        match.client.world.terrains().find(terrainIn(match.client.world, match.client.workspace));
    CHECK(client->field.findChunk(key) != nullptr);
}

TEST_CASE("a terrain placed and given its look before any ground reaches a replica as it is (terrain audit R1, "
          "R3)")
{
    PlayedMatch match;
    const core::InstanceId ground = makeTerrain(match.server);
    scene::TerrainComponent* terrain = match.server.world.terrains().find(ground);
    terrain->origin = core::DVec3{100.0, 5.0, -40.0};
    terrain->layers = {"asset://materials/terrain/grass.material.json"};
    terrain->rules = asset::defaultTerrainRules();
    match.run(3);

    const core::InstanceId copy = terrainIn(match.client.world, match.client.workspace);
    REQUIRE(copy.valid());
    const scene::TerrainComponent* seen = match.client.world.terrains().find(copy);
    CHECK(seen->layers == terrain->layers);
    CHECK(seen->rules == terrain->rules);
    CHECK(seen->origin.x == doctest::Approx(100.0));
    CHECK(seen->origin.z == doctest::Approx(-40.0));

    // Its ground after, at a voxel of its own.
    terrain = match.server.world.terrains().find(ground);
    terrain->field =
        asset::TerrainField(asset::FieldSettings{.voxelSize = 0.5f, .minHeight = -16.0f, .maxHeight = 16.0f});
    (void)asset::fillFlat(terrain->field, core::DVec3{0.0, 0.0, 0.0}, 32.0f, 2.0f, 1);
    terrain->fieldRevision += 1;
    match.run(3);
    seen = match.client.world.terrains().find(copy);
    CHECK(seen->field.digest() == terrain->field.digest());
}

TEST_CASE("a scene's empty terrain given another voxel by the server takes it (terrain audit R2)")
{
    PlayedMatch match;
    for (RealSide* side : {&match.server, &match.client})
        (void)makeTerrain(*side);
    match.run(2);
    scene::TerrainComponent* server =
        match.server.world.terrains().find(terrainIn(match.server.world, match.server.workspace));
    server->field =
        asset::TerrainField(asset::FieldSettings{.voxelSize = 0.5f, .minHeight = -16.0f, .maxHeight = 16.0f});
    (void)asset::fillFlat(server->field, core::DVec3{0.0, 0.0, 0.0}, 32.0f, 2.0f, 1);
    server->fieldRevision += 1;
    match.run(3);
    const scene::TerrainComponent* client =
        match.client.world.terrains().find(terrainIn(match.client.world, match.client.workspace));
    CHECK(client->field.settings().voxelSize == doctest::Approx(0.5));
    CHECK(client->field.digest() == server->field.digest());
}

TEST_CASE("a terrain destroyed and made again on the server takes the old ground from every replica (terrain audit "
          "R4)")
{
    PlayedMatch match;
    for (RealSide* side : {&match.server, &match.client}) {
        scene::TerrainComponent* terrain = side->world.terrains().find(makeTerrain(*side));
        terrain->field.setHeightRange(-32.0f, 32.0f);
        (void)asset::fillFlat(terrain->field, core::DVec3{0.0, 0.0, 0.0}, 64.0f, 2.0f, 1);
        terrain->shipped = terrain->field;
        terrain->fieldRevision += 1;
    }
    match.run(3);

    // The scene's terrain destroyed, and a new one generated somewhere else.
    match.server.world.destroy(terrainIn(match.server.world, match.server.workspace));
    match.run(2);
    const scene::TerrainComponent* client =
        match.client.world.terrains().find(terrainIn(match.client.world, match.client.workspace));
    CHECK(client->field.empty());

    scene::TerrainComponent* fresh = match.server.world.terrains().find(makeTerrain(match.server));
    fresh->field.setHeightRange(-32.0f, 32.0f);
    (void)asset::fillFlat(fresh->field, core::DVec3{200.0, 0.0, 0.0}, 32.0f, 4.0f, 2);
    fresh->fieldRevision += 1;
    match.run(3);
    client = match.client.world.terrains().find(terrainIn(match.client.world, match.client.workspace));
    CHECK(client->field.digest() == fresh->field.digest());
}

TEST_CASE("a block world's types and blocks reach a replica, and a block broken after (ADR 0135)")
{
    PlayedMatch match;
    const core::InstanceId service = makeVoxels(match.server);
    (void)makeVoxels(match.client);
    scene::VoxelComponent* voxels = match.server.world.voxels().find(service);
    scene::VoxelBlockType stone;
    stone.name = match.server.atoms.intern("Stone");
    stone.color = core::Color3{0.5f, 0.5f, 0.5f};
    voxels->types.push_back(stone);
    (void)voxels->grid.fill(0, 0, 0, 20, 2, 20, 1);
    voxels->revision += 1;
    match.run(4);

    const scene::VoxelComponent* seen = nullptr;
    match.client.world.voxels().forEach([&seen](core::InstanceId, const scene::VoxelComponent& found) {
        if (seen == nullptr)
            seen = &found;
    });
    REQUIRE(seen != nullptr);
    REQUIRE(seen->types.size() == 1);
    CHECK(match.client.atoms.text(seen->types[0].name) == "Stone");
    CHECK(seen->grid.digest() == voxels->grid.digest());

    (void)voxels->grid.set(5, 2, 5, asset::AirBlock);
    voxels->revision += 1;
    match.run(2);
    CHECK(seen->grid.get(5, 2, 5) == asset::AirBlock);
    CHECK(seen->grid.digest() == voxels->grid.digest());
}
