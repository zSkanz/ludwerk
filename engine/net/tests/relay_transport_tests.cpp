// Reaching a host through a relay (ADR 0178), on real sockets: a relay, a
// host and a joiner in one process, on the loopback.
//
// What this proves is the wiring -- that the messages cross ENet's own socket
// and never reach ENet, that a host's socket for a relayed joiner carries a
// match both ways, that the script's side will be told the truth about paths
// and failures. What a ROUTER does to all of it is `rendezvous_tests.cpp`'s,
// on a network made up for the purpose: the loopback has no router.
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/net/relay_service.h"
#include "engine/net/transport.h"

using namespace engine;
using namespace engine::net;

namespace {

void seedCatalog()
{
    const auto result = core::engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

// The host's port. Fixed, as `transport_tests.cpp` says of its own.
constexpr u16 HostPort = 47925;

[[nodiscard]] std::vector<u8> bytesOf(std::string_view text)
{
    const auto* const data = reinterpret_cast<const u8*>(text.data());
    return {data, data + text.size()};
}

[[nodiscard]] std::string textOf(const std::vector<u8>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

template <typename Predicate>
bool pumpUntil(ITransport& a, ITransport& b, std::vector<TransportEvent>& eventsA, std::vector<TransportEvent>& eventsB,
               Predicate predicate, int rounds = 600)
{
    for (int i = 0; i < rounds; ++i) {
        if (predicate())
            return true;
        REQUIRE_FALSE(a.poll(eventsA, 5).has_value());
        REQUIRE_FALSE(b.poll(eventsB, 5).has_value());
    }
    return predicate();
}

[[nodiscard]] const TransportEvent* find(const std::vector<TransportEvent>& events, TransportEvent::Kind kind)
{
    for (const TransportEvent& event : events)
        if (event.kind == kind)
            return &event;
    return nullptr;
}

// A relay, a host registered with it, and a joiner with a socket.
struct Table
{
    RelayService relay;
    std::unique_ptr<ITransport> host = createEnetTransport();
    std::unique_ptr<ITransport> joiner = createEnetTransport();
    std::vector<TransportEvent> hostEvents;
    std::vector<TransportEvent> joinerEvents;
    std::string relayAt;

    Table()
    {
        seedCatalog();
        REQUIRE_FALSE(relay.start(0).has_value());
        relayAt = "127.0.0.1:" + std::to_string(relay.port());
        REQUIRE_FALSE(host->open({.port = HostPort, .maxPeers = 8, .channels = 2}).has_value());
        REQUIRE_FALSE(joiner->open({.port = 0, .maxPeers = 4, .channels = 2}).has_value());
        CHECK(host->relayState() == RelayState::None);
        CHECK(host->joinCode().empty());
        REQUIRE_FALSE(host->useRelay(relayAt).has_value());
        REQUIRE(pump([&] { return host->relayState() == RelayState::Ready; }));
    }

    template <typename Predicate>
    bool pump(Predicate predicate, int rounds = 600)
    {
        return pumpUntil(*host, *joiner, hostEvents, joinerEvents, predicate, rounds);
    }

    // Joins by the host's code and waits for both ends to say so.
    PeerId join(bool direct)
    {
        PeerId peer;
        REQUIRE_FALSE(joiner->connectByCode(relayAt, host->joinCode(), direct, peer).has_value());
        REQUIRE(peer.valid());
        REQUIRE(pump([&] {
            return find(hostEvents, TransportEvent::Kind::Connected) != nullptr &&
                   find(joinerEvents, TransportEvent::Kind::Connected) != nullptr;
        }));
        CHECK(find(joinerEvents, TransportEvent::Kind::Connected)->peer == peer);
        return peer;
    }

    // One message each way, and that it arrived as it left.
    void exchange(PeerId toHost, PeerId toJoiner, std::string_view word)
    {
        hostEvents.clear();
        joinerEvents.clear();
        const std::string up = std::string(word) + " up";
        const std::string down = std::string(word) + " down";
        REQUIRE_FALSE(joiner->send(toHost, bytesOf(up), Delivery::Reliable, 0).has_value());
        REQUIRE_FALSE(host->send(toJoiner, bytesOf(down), Delivery::Reliable, 1).has_value());
        REQUIRE(pump([&] {
            return find(hostEvents, TransportEvent::Kind::Message) != nullptr &&
                   find(joinerEvents, TransportEvent::Kind::Message) != nullptr;
        }));
        CHECK(textOf(find(hostEvents, TransportEvent::Kind::Message)->payload) == up);
        CHECK(textOf(find(joinerEvents, TransportEvent::Kind::Message)->payload) == down);
        CHECK(find(joinerEvents, TransportEvent::Kind::Message)->channel == 1);
    }
};

} // namespace

TEST_CASE("a host registers with a relay and is joined by its code, each straight to the other")
{
    Table table;
    const std::string code = table.host->joinCode();
    CHECK(code.size() == 8);

    const PeerId toHost = table.join(true);
    const PeerId toJoiner = find(table.hostEvents, TransportEvent::Kind::Connected)->peer;
    // On one machine they are on one network: by an address of its own, and
    // never by the relay.
    CHECK(table.joiner->path(toHost) != PeerPath::Relayed);
    CHECK(table.joiner->path(toHost) != PeerPath::None);
    CHECK(table.host->path(toJoiner) == PeerPath::Lan);
    CHECK(table.relay.stats().relayed == 0);
    table.exchange(toHost, toJoiner, "direct");
    CHECK(table.relay.stats().packets == 0);
    CHECK(table.relay.stats().sessions == 1);
}

TEST_CASE("through the relay: a match crosses both ways, large messages too, and both ends know the path")
{
    Table table;
    const PeerId toHost = table.join(false);
    const PeerId toJoiner = find(table.hostEvents, TransportEvent::Kind::Connected)->peer;
    CHECK(table.joiner->path(toHost) == PeerPath::Relayed);
    CHECK(table.host->path(toJoiner) == PeerPath::Relayed);
    CHECK(table.relay.stats().relayed == 1);
    table.exchange(toHost, toJoiner, "relayed");

    // Sixty kilobytes, in fragments: every one of them through the relay.
    std::vector<u8> large(60'000);
    for (usize index = 0; index < large.size(); ++index)
        large[index] = static_cast<u8>(index * 7 + index / 251);
    table.hostEvents.clear();
    table.joinerEvents.clear();
    REQUIRE_FALSE(table.host->send(toJoiner, large, Delivery::Reliable, 0).has_value());
    REQUIRE_FALSE(table.joiner->send(toHost, large, Delivery::UnreliableSequenced, 1).has_value());
    REQUIRE(table.pump([&] {
        return find(table.hostEvents, TransportEvent::Kind::Message) != nullptr &&
               find(table.joinerEvents, TransportEvent::Kind::Message) != nullptr;
    }));
    CHECK(find(table.joinerEvents, TransportEvent::Kind::Message)->payload == large);
    CHECK(find(table.hostEvents, TransportEvent::Kind::Message)->payload == large);
    CHECK(table.relay.stats().bytes > 120'000);

    // A second joiner is another peer to the host, with its own way back.
    auto second = createEnetTransport();
    REQUIRE_FALSE(second->open({.port = 0, .maxPeers = 4, .channels = 2}).has_value());
    PeerId secondToHost;
    REQUIRE_FALSE(second->connectByCode(table.relayAt, table.host->joinCode(), false, secondToHost).has_value());
    table.hostEvents.clear();
    std::vector<TransportEvent> secondEvents;
    REQUIRE(pumpUntil(*table.host, *second, table.hostEvents, secondEvents, [&] {
        return find(table.hostEvents, TransportEvent::Kind::Connected) != nullptr &&
               find(secondEvents, TransportEvent::Kind::Connected) != nullptr;
    }));
    const PeerId toSecond = find(table.hostEvents, TransportEvent::Kind::Connected)->peer;
    CHECK(toSecond != toJoiner);
    CHECK(table.host->peerCount() == 2);
    CHECK(table.relay.stats().relayed == 2);
    secondEvents.clear();
    table.joinerEvents.clear();
    REQUIRE_FALSE(table.host->send(toSecond, bytesOf("for the second"), Delivery::Reliable, 0).has_value());
    REQUIRE(pumpUntil(*table.host, *second, table.hostEvents, secondEvents,
                      [&] { return find(secondEvents, TransportEvent::Kind::Message) != nullptr; }));
    REQUIRE_FALSE(table.joiner->poll(table.joinerEvents, 20).has_value());
    CHECK(find(table.joinerEvents, TransportEvent::Kind::Message) == nullptr);

    // The joiner leaves: the host hears it through the relay, and the relay
    // stops carrying it.
    table.hostEvents.clear();
    table.joiner->disconnect(toHost);
    REQUIRE(table.pump([&] { return find(table.hostEvents, TransportEvent::Kind::Disconnected) != nullptr; }));
    CHECK(find(table.hostEvents, TransportEvent::Kind::Disconnected)->peer == toJoiner);
    CHECK(table.host->path(toJoiner) == PeerPath::None);
    REQUIRE(pumpUntil(*table.host, *second, table.hostEvents, secondEvents,
                      [&] { return table.relay.stats().relayed == 1; }));
}

TEST_CASE("a join by code that cannot be says why")
{
    Table table;
    PeerId peer;

    // Not a code at all: an error at once, and nothing begun.
    CHECK(table.joiner->connectByCode(table.relayAt, "HELLO", true, peer).has_value());
    CHECK_FALSE(peer.valid());
    CHECK(table.joiner->connectByCode("not a relay", table.host->joinCode(), true, peer).has_value());
    // A transport that is not a listening host has no code to register.
    CHECK(table.joiner->useRelay(table.relayAt).has_value());

    const auto failureOf = [&](std::string_view code) {
        table.joinerEvents.clear();
        PeerId joining;
        REQUIRE_FALSE(table.joiner->connectByCode(table.relayAt, code, true, joining).has_value());
        REQUIRE(table.pump([&] { return find(table.joinerEvents, TransportEvent::Kind::Disconnected) != nullptr; }));
        const TransportEvent* const event = find(table.joinerEvents, TransportEvent::Kind::Disconnected);
        CHECK(event->peer == joining);
        CHECK(table.joiner->peerCount() == 0);
        return event->failure;
    };
    // A well-formed code nobody holds.
    CHECK(failureOf("22222222") == ConnectFailure::NoSession);

    // A host with no room is not even knocked on.
    table.host->setOccupancy(8, 8);
    REQUIRE_FALSE(table.pump([] { return false; }, 20));
    CHECK(failureOf(table.host->joinCode()) == ConnectFailure::Full);
    table.host->setOccupancy(1, 8);
    REQUIRE_FALSE(table.pump([] { return false; }, 20));

    // A joiner the host keeps away: let go with a ban, it is refused at the
    // relay for as long.
    const std::string code = table.host->joinCode();
    table.hostEvents.clear();
    table.joinerEvents.clear();
    const PeerId toHost = table.join(false);
    const PeerId toJoiner = find(table.hostEvents, TransportEvent::Kind::Connected)->peer;
    table.host->ban(toJoiner, 60);
    table.host->disconnect(toJoiner);
    table.joinerEvents.clear();
    REQUIRE(table.pump([&] { return find(table.joinerEvents, TransportEvent::Kind::Disconnected) != nullptr; }));
    CHECK(find(table.joinerEvents, TransportEvent::Kind::Disconnected)->peer == toHost);
    CHECK(find(table.joinerEvents, TransportEvent::Kind::Disconnected)->failure == ConnectFailure::None);
    // The relay is told once the host has heard the joiner go.
    REQUIRE(table.pump([&] { return table.relay.stats().relayed == 0; }));
    CHECK(failureOf(code) == ConnectFailure::Refused);

    // The host stops hosting: its code is nobody's.
    table.host->leaveRelay();
    CHECK(table.host->relayState() == RelayState::None);
    CHECK(table.host->joinCode().empty());
    REQUIRE_FALSE(table.pump([] { return false; }, 20));
    auto other = createEnetTransport();
    REQUIRE_FALSE(other->open({.port = 0, .maxPeers = 4, .channels = 2}).has_value());
    PeerId late;
    REQUIRE_FALSE(other->connectByCode(table.relayAt, code, true, late).has_value());
    std::vector<TransportEvent> otherEvents;
    REQUIRE(pumpUntil(*table.host, *other, table.hostEvents, otherEvents,
                      [&] { return find(otherEvents, TransportEvent::Kind::Disconnected) != nullptr; }));
    CHECK(find(otherEvents, TransportEvent::Kind::Disconnected)->failure == ConnectFailure::NoSession);
}

TEST_CASE("a relay nobody answers at: the joiner is told in three seconds, and the host goes on hosting")
{
    seedCatalog();
    // A port with nothing behind it: a relay bound and stopped.
    RelayService gone;
    REQUIRE_FALSE(gone.start(0).has_value());
    const std::string nowhere = "127.0.0.1:" + std::to_string(gone.port());
    gone.stop();

    auto host = createEnetTransport();
    auto joiner = createEnetTransport();
    REQUIRE_FALSE(host->open({.port = HostPort, .maxPeers = 8, .channels = 2}).has_value());
    REQUIRE_FALSE(joiner->open({.port = 0, .maxPeers = 4, .channels = 2}).has_value());
    REQUIRE_FALSE(host->useRelay(nowhere).has_value());
    CHECK(host->relayState() == RelayState::Connecting);

    std::vector<TransportEvent> hostEvents;
    std::vector<TransportEvent> joinerEvents;
    PeerId joining;
    REQUIRE_FALSE(joiner->connectByCode(nowhere, "22222222", true, joining).has_value());
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(pumpUntil(
        *host, *joiner, hostEvents, joinerEvents,
        [&] { return find(joinerEvents, TransportEvent::Kind::Disconnected) != nullptr; }, 2000));
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK(find(joinerEvents, TransportEvent::Kind::Disconnected)->failure == ConnectFailure::RelayUnreachable);
    CHECK(took >= std::chrono::milliseconds(2500));
    CHECK(took < std::chrono::milliseconds(6000));

    // The host says the relay is not there, has no code, and is still a host:
    // by its address it is joined as ever.
    REQUIRE(pumpUntil(
        *host, *joiner, hostEvents, joinerEvents, [&] { return host->relayState() == RelayState::Unreachable; }, 2000));
    CHECK(host->joinCode().empty());
    PeerId direct;
    REQUIRE_FALSE(joiner->connect("127.0.0.1", HostPort, direct).has_value());
    REQUIRE(pumpUntil(*host, *joiner, hostEvents, joinerEvents,
                      [&] { return find(joinerEvents, TransportEvent::Kind::Connected) != nullptr; }));
    CHECK(joiner->path(direct) == PeerPath::Lan);
}

TEST_CASE("a relay stopped and started again: the code is the same, and a joiner it carried is carried again")
{
    Table table;
    const std::string code = table.host->joinCode();
    const u16 port = table.relay.port();
    const PeerId toHost = table.join(false);
    const PeerId toJoiner = find(table.hostEvents, TransportEvent::Kind::Connected)->peer;
    table.exchange(toHost, toJoiner, "before");

    table.relay.stop();
    REQUIRE_FALSE(table.relay.start(port).has_value());
    CHECK(table.relay.stats().sessions == 0);

    // The host's socket for the joiner says its slot again within three
    // seconds, is told the relay knows no such session, the host registers
    // again, and the slot is the relay's once more. ENet rides it out: the
    // silence is shorter than its timeout.
    table.hostEvents.clear();
    table.joinerEvents.clear();
    REQUIRE(table.pump([&] { return table.relay.stats().relayed == 1; }, 2000));
    CHECK(table.relay.stats().sessions == 1);
    CHECK(table.host->joinCode() == code);
    CHECK(table.host->relayState() == RelayState::Ready);
    table.exchange(toHost, toJoiner, "after");
    CHECK(find(table.hostEvents, TransportEvent::Kind::Disconnected) == nullptr);
    CHECK(find(table.joinerEvents, TransportEvent::Kind::Disconnected) == nullptr);

    // And the code a friend was given before still finds the host.
    auto friendOf = createEnetTransport();
    REQUIRE_FALSE(friendOf->open({.port = 0, .maxPeers = 4, .channels = 2}).has_value());
    PeerId friendToHost;
    REQUIRE_FALSE(friendOf->connectByCode(table.relayAt, code, true, friendToHost).has_value());
    std::vector<TransportEvent> friendEvents;
    REQUIRE(pumpUntil(*table.host, *friendOf, table.hostEvents, friendEvents,
                      [&] { return find(friendEvents, TransportEvent::Kind::Connected) != nullptr; }));
}

namespace {

// A name server for one name, which knows it only once it has been told to.
std::atomic<bool> g_relayNameKnown{false};
std::atomic<int> g_relayNameAsked{0};

bool lookUpRelayName(std::string_view name, std::string& address)
{
    if (name != "relay.test")
        return false;
    ++g_relayNameAsked;
    if (!g_relayNameKnown.load())
        return false;
    address = "127.0.0.1";
    return true;
}

} // namespace

TEST_CASE("D562: a relay whose name is not found yet is looked up again, and the host registers when it is")
{
    seedCatalog();
    g_relayNameKnown.store(false);
    g_relayNameAsked.store(0);
    setNameLookupForTests(&lookUpRelayName);
    struct PutBack
    {
        ~PutBack() { setNameLookupForTests(nullptr); }
    } putBack;

    RelayService relay;
    REQUIRE_FALSE(relay.start(0).has_value());
    const std::string named = "relay.test:" + std::to_string(relay.port());

    auto host = createEnetTransport();
    auto joiner = createEnetTransport();
    REQUIRE_FALSE(host->open({.port = HostPort, .maxPeers = 8, .channels = 2, .relayLookupEveryMs = 150}).has_value());
    REQUIRE_FALSE(joiner->open({.port = 0, .maxPeers = 4, .channels = 2}).has_value());

    // A record made a minute ago: this machine's resolver does not have it
    // yet. The host is told once, in words that say it is looked for again,
    // and is a host all the same.
    const auto refused = host->useRelay(named);
    REQUIRE(refused.has_value());
    CHECK(refused->message.find("relay.test") != std::string::npos);
    CHECK(refused->message.find("looked up again") != std::string::npos);
    CHECK(host->relayState() == RelayState::Unreachable);
    CHECK(host->joinCode().empty());

    std::vector<TransportEvent> hostEvents;
    std::vector<TransportEvent> joinerEvents;
    // It goes on being asked for while it is not found...
    REQUIRE(pumpUntil(*host, *joiner, hostEvents, joinerEvents, [] { return g_relayNameAsked.load() >= 3; }, 2000));
    CHECK(host->relayState() == RelayState::Unreachable);
    CHECK(relay.stats().sessions == 0);

    // ...and the lookup after the record arrives registers the host: no
    // leaving, no hosting again. It was asked once and never again.
    g_relayNameKnown.store(true);
    REQUIRE(pumpUntil(
        *host, *joiner, hostEvents, joinerEvents, [&] { return host->relayState() == RelayState::Ready; }, 2000));
    CHECK(relay.stats().sessions == 1);
    CHECK(host->joinCode().size() == 8);

    // And it is joined by its code like any other.
    PeerId toHost;
    REQUIRE_FALSE(joiner->connectByCode(named, host->joinCode(), true, toHost).has_value());
    REQUIRE(pumpUntil(*host, *joiner, hostEvents, joinerEvents,
                      [&] { return find(joinerEvents, TransportEvent::Kind::Connected) != nullptr; }));

    // A relay left is not looked for any more.
    const int asked = g_relayNameAsked.load();
    host->leaveRelay();
    g_relayNameKnown.store(false);
    REQUIRE(host->useRelay(named).has_value());
    host->leaveRelay();
    const int afterLeaving = g_relayNameAsked.load();
    CHECK(afterLeaving == asked + 1);
    (void)pumpUntil(*host, *joiner, hostEvents, joinerEvents, [] { return false; }, 100);
    CHECK(g_relayNameAsked.load() == afterLeaving);
    CHECK(host->relayState() == RelayState::None);
}
