// Every match encrypted, around whatever carries it (ADR 0224): a peer is
// connected once its handshake is done, its messages arrive as they were
// sent, and what the wire carries is not them.
#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/net/memory_transport.h"
#include "engine/net/secure_transport.h"

using namespace engine;
using namespace engine::net;

namespace {

constexpr u16 Port = 47960;

std::vector<u8> bytesOf(std::string_view text)
{
    return {text.begin(), text.end()};
}

SecureConfig configOf(const core::X25519Pair& identity, std::string_view prologue)
{
    SecureConfig config;
    config.identity = identity;
    config.prologue = bytesOf(prologue);
    return config;
}

// A transport under the secure one that keeps a copy of everything sent, so
// a test can look at what the wire carried.
class Tap final : public ITransport
{
public:
    Tap(std::unique_ptr<ITransport> inner, std::vector<std::vector<u8>>& sent) : m_inner(std::move(inner)), m_sent(sent)
    {}
    std::optional<core::EngineError> open(const TransportConfig& config) override { return m_inner->open(config); }
    void close() override { m_inner->close(); }
    std::optional<core::EngineError> connect(std::string_view host, u16 port, PeerId& out) override
    {
        return m_inner->connect(host, port, out);
    }
    void disconnect(PeerId peer) override { m_inner->disconnect(peer); }
    std::optional<core::EngineError> send(PeerId peer, std::span<const u8> payload, Delivery delivery,
                                          u8 channel) override
    {
        m_sent.emplace_back(payload.begin(), payload.end());
        return m_inner->send(peer, payload, delivery, channel);
    }
    std::optional<core::EngineError> poll(std::vector<TransportEvent>& out, u32 timeoutMs) override
    {
        return m_inner->poll(out, timeoutMs);
    }
    [[nodiscard]] usize peerCount() const noexcept override { return m_inner->peerCount(); }

private:
    std::unique_ptr<ITransport> m_inner;
    std::vector<std::vector<u8>>& m_sent;
};

struct Ends
{
    std::shared_ptr<MemoryNetwork> network = createMemoryNetwork();
    core::X25519Pair serverKey = core::x25519Generate();
    core::X25519Pair clientKey = core::x25519Generate();
    std::vector<std::vector<u8>> wire;
    std::unique_ptr<ITransport> server;
    std::unique_ptr<ITransport> client;
    PeerId toServer;
    PeerId toClient;
    std::vector<TransportEvent> atServer;
    std::vector<TransportEvent> atClient;

    Ends(std::string_view serverGame, std::string_view clientGame, bool trustAny)
    {
        SecureConfig serving = configOf(serverKey, serverGame);
        SecureConfig joining = configOf(clientKey, clientGame);
        const std::array<u8, 32> expected = serverKey.pub;
        joining.trust = [trustAny, expected](std::string_view, u16, const std::array<u8, 32>& key) {
            return trustAny || key == expected;
        };
        server = createSecureTransport(std::make_unique<Tap>(createMemoryTransport(network), wire), serving);
        client = createSecureTransport(std::make_unique<Tap>(createMemoryTransport(network), wire), joining);
        REQUIRE_FALSE(server->open(TransportConfig{.port = Port, .maxPeers = 4, .channels = 3}).has_value());
        REQUIRE_FALSE(client->open(TransportConfig{.port = 0, .maxPeers = 1, .channels = 3}).has_value());
        REQUIRE_FALSE(client->connect("memory", Port, toServer).has_value());
    }

    void pump(int rounds = 6)
    {
        for (int round = 0; round < rounds; ++round) {
            std::vector<TransportEvent> events;
            REQUIRE_FALSE(server->poll(events, 0).has_value());
            for (TransportEvent& event : events) {
                if (event.kind == TransportEvent::Kind::Connected)
                    toClient = event.peer;
                atServer.push_back(std::move(event));
            }
            events.clear();
            REQUIRE_FALSE(client->poll(events, 0).has_value());
            for (TransportEvent& event : events)
                atClient.push_back(std::move(event));
        }
    }

    [[nodiscard]] static bool has(const std::vector<TransportEvent>& events, TransportEvent::Kind kind)
    {
        return std::any_of(events.begin(), events.end(), [kind](const TransportEvent& e) { return e.kind == kind; });
    }
};

} // namespace

TEST_CASE("an encrypted match connects after its handshake, and the wire never carries a message as it was sent")
{
    Ends ends("match 53 game", "match 53 game", false);
    ends.pump();
    REQUIRE(Ends::has(ends.atServer, TransportEvent::Kind::Connected));
    REQUIRE(Ends::has(ends.atClient, TransportEvent::Kind::Connected));
    // Each side knows the key the other proved.
    CHECK(ends.server->remoteKey(ends.toClient) == ends.clientKey.pub);
    CHECK(ends.client->remoteKey(ends.toServer) == ends.serverKey.pub);

    const std::string secret = "the hidden treasure is under the mill";
    const std::vector<u8> said = bytesOf(secret);
    REQUIRE_FALSE(ends.client->send(ends.toServer, said, Delivery::Reliable, 1).has_value());
    ends.atServer.clear();
    ends.pump(2);
    const auto message = std::find_if(ends.atServer.begin(), ends.atServer.end(),
                                      [](const TransportEvent& e) { return e.kind == TransportEvent::Kind::Message; });
    REQUIRE(message != ends.atServer.end());
    CHECK(message->payload == said);
    CHECK(message->channel == 1);
    // What crossed the wire was not it.
    for (const std::vector<u8>& packet : ends.wire)
        CHECK(std::search(packet.begin(), packet.end(), said.begin(), said.end()) == packet.end());
}

TEST_CASE("two games do not connect: the handshake binds the game and the protocol")
{
    {
        Ends ends("match 53 one.game", "match 53 two.game", true);
        ends.pump();
        CHECK_FALSE(Ends::has(ends.atServer, TransportEvent::Kind::Connected));
        CHECK_FALSE(Ends::has(ends.atClient, TransportEvent::Kind::Connected));
        const auto gone = std::find_if(ends.atClient.begin(), ends.atClient.end(), [](const TransportEvent& e) {
            return e.kind == TransportEvent::Kind::Disconnected;
        });
        REQUIRE(gone != ends.atClient.end());
        CHECK(gone->failure == ConnectFailure::Insecure);
    }
}

TEST_CASE("a pinned key that changed is said as KeyChanged, and nothing is sent to that server")
{
    Ends ends("match 53 game", "match 53 game", false);
    // Re-made with a trust that expects a key the server does not hold.
    const std::array<u8, 32> pinned = core::x25519Generate().pub;
    SecureConfig joining = configOf(ends.clientKey, "match 53 game");
    joining.trust = [pinned](std::string_view, u16, const std::array<u8, 32>& key) { return key == pinned; };
    std::vector<std::vector<u8>> wire;
    ends.client = createSecureTransport(std::make_unique<Tap>(createMemoryTransport(ends.network), wire), joining);
    REQUIRE_FALSE(ends.client->open(TransportConfig{.port = 0, .maxPeers = 1, .channels = 3}).has_value());
    REQUIRE_FALSE(ends.client->connect("memory", Port, ends.toServer).has_value());
    ends.pump();
    CHECK_FALSE(Ends::has(ends.atClient, TransportEvent::Kind::Connected));
    const auto gone = std::find_if(ends.atClient.begin(), ends.atClient.end(), [](const TransportEvent& e) {
        return e.kind == TransportEvent::Kind::Disconnected;
    });
    REQUIRE(gone != ends.atClient.end());
    CHECK(gone->failure == ConnectFailure::KeyChanged);
    // The client never sent its third message: it proved nothing to a server
    // it did not trust.
    CHECK(std::none_of(wire.begin(), wire.end(), [](const std::vector<u8>& p) { return !p.empty() && p[0] == 0x03; }));
}

TEST_CASE("a joiner that does not encrypt is told so in the clear, and is never a peer of the match")
{
    auto network = createMemoryNetwork();
    const std::vector<u8> refusal{23, 6, 0, 0};
    SecureConfig serving = configOf(core::x25519Generate(), "match 53 game");
    serving.plainRefusal = refusal;
    std::unique_ptr<ITransport> server = createSecureTransport(createMemoryTransport(network), serving);
    std::unique_ptr<ITransport> client = createMemoryTransport(network);
    REQUIRE_FALSE(server->open(TransportConfig{.port = Port, .maxPeers = 4, .channels = 3}).has_value());
    REQUIRE_FALSE(client->open(TransportConfig{.port = 0, .maxPeers = 1, .channels = 3}).has_value());
    // The server says the key it proves; a plain transport has none.
    CHECK(server->localKey() == serving.identity.pub);
    CHECK_FALSE(client->localKey().has_value());
    PeerId toServer;
    REQUIRE_FALSE(client->connect("memory", Port, toServer).has_value());

    std::vector<TransportEvent> atServer;
    std::vector<TransportEvent> atClient;
    bool said = false;
    for (int round = 0; round < 8; ++round) {
        REQUIRE_FALSE(server->poll(atServer, 0).has_value());
        std::vector<TransportEvent> events;
        REQUIRE_FALSE(client->poll(events, 0).has_value());
        for (TransportEvent& event : events) {
            // Its hello, in the clear, the moment it is connected.
            if (event.kind == TransportEvent::Kind::Connected && !said) {
                std::vector<u8> hello(29, 0);
                hello[0] = 1;
                REQUIRE_FALSE(client->send(toServer, hello, Delivery::Reliable, 0).has_value());
                said = true;
            }
            atClient.push_back(std::move(event));
        }
    }
    REQUIRE(said);
    // Told why, in words it can read.
    const auto told = std::find_if(atClient.begin(), atClient.end(),
                                   [](const TransportEvent& e) { return e.kind == TransportEvent::Kind::Message; });
    REQUIRE(told != atClient.end());
    CHECK(told->payload == refusal);
    // And the match never heard of it.
    CHECK(std::none_of(atServer.begin(), atServer.end(), [](const TransportEvent& e) {
        return e.kind == TransportEvent::Kind::Connected || e.kind == TransportEvent::Kind::Message ||
               e.kind == TransportEvent::Kind::Disconnected;
    }));
}
