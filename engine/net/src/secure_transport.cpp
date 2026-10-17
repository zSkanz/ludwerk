#include "engine/net/secure_transport.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "engine/net/noise.h"

namespace engine::net {
namespace {

// The first byte of every packet this transport sends.
enum class Tag : u8
{
    Handshake1 = 0x01,
    Handshake2 = 0x02,
    Handshake3 = 0x03,
    Sealed = 0x10,
};

// Handshake messages travel reliably on the first channel, before anything
// else does.
constexpr u8 HandshakeChannel = 0;

[[nodiscard]] u64 nowMs()
{
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

class SecureTransport final : public ITransport
{
public:
    SecureTransport(std::unique_ptr<ITransport> inner, SecureConfig config)
        : m_inner(std::move(inner)), m_config(std::move(config))
    {}

    std::optional<core::EngineError> open(const TransportConfig& config) override { return m_inner->open(config); }
    void close() override
    {
        m_peers.clear();
        m_inner->close();
    }

    std::optional<core::EngineError> connect(std::string_view host, u16 port, PeerId& outPeer) override
    {
        std::optional<core::EngineError> error = m_inner->connect(host, port, outPeer);
        if (!error.has_value())
            dialled(outPeer, std::string(host), port);
        return error;
    }
    std::optional<core::EngineError> connectByCode(std::string_view relay, std::string_view code, bool direct,
                                                   PeerId& outPeer) override
    {
        std::optional<core::EngineError> error = m_inner->connectByCode(relay, code, direct, outPeer);
        if (!error.has_value())
            dialled(outPeer, std::string(code), 0);
        return error;
    }

    void disconnect(PeerId peer) override { m_inner->disconnect(peer); }
    void drop(PeerId peer) override { m_inner->drop(peer); }
    void flush() override { m_inner->flush(); }

    std::optional<core::EngineError> send(PeerId peer, std::span<const u8> payload, Delivery delivery,
                                          u8 channel) override
    {
        const auto found = m_peers.find(peer.value);
        // Nothing is sent to a peer before its handshake is done: the layers
        // above are told of it only then.
        if (found == m_peers.end() || !found->second.sender.has_value())
            return std::nullopt;
        std::vector<u8> sealed;
        found->second.sender->seal(payload, sealed);
        std::vector<u8> packet;
        packet.reserve(sealed.size() + 1);
        packet.push_back(static_cast<u8>(Tag::Sealed));
        packet.insert(packet.end(), sealed.begin(), sealed.end());
        return m_inner->send(peer, packet, delivery, channel);
    }

    std::optional<core::EngineError> poll(std::vector<TransportEvent>& out, u32 timeoutMs) override
    {
        std::vector<TransportEvent> events;
        std::optional<core::EngineError> error = m_inner->poll(events, timeoutMs);
        for (TransportEvent& event : events)
            take(std::move(event), out);
        expire(out);
        return error;
    }

    [[nodiscard]] usize peerCount() const noexcept override { return m_inner->peerCount(); }
    [[nodiscard]] PeerLink link(PeerId peer) const noexcept override { return m_inner->link(peer); }
    std::optional<core::EngineError> useRelay(std::string_view relay) override { return m_inner->useRelay(relay); }
    void leaveRelay() override { m_inner->leaveRelay(); }
    [[nodiscard]] RelayState relayState() const noexcept override { return m_inner->relayState(); }
    [[nodiscard]] std::string joinCode() const override { return m_inner->joinCode(); }
    void setOccupancy(usize players, usize maxPlayers) override { m_inner->setOccupancy(players, maxPlayers); }
    [[nodiscard]] PeerPath path(PeerId peer) const noexcept override { return m_inner->path(peer); }
    [[nodiscard]] u64 addressOf(PeerId peer) const noexcept override { return m_inner->addressOf(peer); }
    void ban(PeerId peer, u32 seconds) override { m_inner->ban(peer, seconds); }

    [[nodiscard]] WireTotals wireTotals() const noexcept override { return m_inner->wireTotals(); }
    [[nodiscard]] std::string addressText(PeerId peer) const override { return m_inner->addressText(peer); }

    [[nodiscard]] std::optional<std::array<u8, 32>> localKey() const noexcept override { return m_config.identity.pub; }

    [[nodiscard]] std::optional<std::array<u8, 32>> remoteKey(PeerId peer) const noexcept override
    {
        const auto found = m_peers.find(peer.value);
        if (found == m_peers.end() || !found->second.sender.has_value())
            return std::nullopt;
        return found->second.handshake->remoteStatic();
    }

private:
    struct Peer
    {
        std::optional<NoiseHandshake> handshake;
        std::optional<NoiseSender> sender;
        std::optional<NoiseReceiver> receiver;
        // Where it was dialled, for `trust`; empty for a peer that dialled us.
        std::string host;
        u16 port = 0;
        bool dialled = false;
        u64 began = 0;
        // Whether the layers above have been told it is connected.
        bool surfaced = false;
        // Told in the clear that this side encrypts, and being let go: what
        // it says now is not read.
        bool closing = false;
        // Whether a handshake message from it has been read.
        bool heard = false;
    };

    void dialled(PeerId peer, std::string host, u16 port)
    {
        Peer& state = m_peers[peer.value];
        state.dialled = true;
        state.host = std::move(host);
        state.port = port;
    }

    void sendHandshake(PeerId peer, Tag tag, const std::vector<u8>& message)
    {
        std::vector<u8> packet;
        packet.reserve(message.size() + 1);
        packet.push_back(static_cast<u8>(tag));
        packet.insert(packet.end(), message.begin(), message.end());
        (void)m_inner->send(peer, packet, Delivery::Reliable, HandshakeChannel);
    }

    // A connection let go before it was ever a match: said to the layers above
    // as a failed join when this side dialled, and not at all when it did not.
    void refuse(PeerId peer, ConnectFailure why, std::vector<TransportEvent>& out)
    {
        const auto found = m_peers.find(peer.value);
        const bool dialledHere = found != m_peers.end() && found->second.dialled;
        const bool surfaced = found != m_peers.end() && found->second.surfaced;
        m_peers.erase(peer.value);
        m_inner->drop(peer);
        if (dialledHere || surfaced) {
            TransportEvent gone;
            gone.kind = TransportEvent::Kind::Disconnected;
            gone.peer = peer;
            gone.failure = surfaced ? ConnectFailure::None : why;
            out.push_back(std::move(gone));
        }
    }

    void finish(PeerId peer, Peer& state, std::vector<TransportEvent>& out)
    {
        const NoiseHandshake::Keys keys = state.handshake->split();
        state.sender.emplace(keys.send);
        state.receiver.emplace(keys.receive);
        state.surfaced = true;
        TransportEvent connected;
        connected.kind = TransportEvent::Kind::Connected;
        connected.peer = peer;
        out.push_back(std::move(connected));
    }

    void take(TransportEvent event, std::vector<TransportEvent>& out)
    {
        const PeerId peer = event.peer;
        if (event.kind == TransportEvent::Kind::Connected) {
            Peer& state = m_peers[peer.value];
            state.began = nowMs();
            state.handshake.emplace(state.dialled ? NoiseHandshake::Role::Initiator : NoiseHandshake::Role::Responder,
                                    m_config.identity, m_config.prologue);
            if (state.dialled) {
                if (const std::optional<std::vector<u8>> first = state.handshake->write(); first.has_value())
                    sendHandshake(peer, Tag::Handshake1, *first);
            }
            return;
        }
        if (event.kind == TransportEvent::Kind::Disconnected) {
            const auto found = m_peers.find(peer.value);
            const bool tell = found == m_peers.end()  ? true
                              : found->second.closing ? false
                                                      : found->second.surfaced || found->second.dialled;
            m_peers.erase(peer.value);
            if (tell)
                out.push_back(std::move(event));
            return;
        }
        if (event.kind != TransportEvent::Kind::Message || event.payload.empty())
            return;
        const auto found = m_peers.find(peer.value);
        if (found == m_peers.end() || !found->second.handshake.has_value())
            return;
        Peer& state = found->second;
        if (state.closing)
            return;
        const auto tag = static_cast<Tag>(event.payload.front());
        const std::span<const u8> body = std::span<const u8>(event.payload).subspan(1);

        if (tag == Tag::Sealed) {
            if (!state.receiver.has_value())
                return;
            std::vector<u8> plain;
            // Forged, damaged, played back or too old: dropped, as a lost
            // packet is.
            if (!state.receiver->open(body, plain))
                return;
            event.payload = std::move(plain);
            out.push_back(std::move(event));
            return;
        }

        // The handshake. Anything out of its order is the end of it.
        const bool expected = (tag == Tag::Handshake1 && !state.dialled) || (tag == Tag::Handshake2 && state.dialled) ||
                              (tag == Tag::Handshake3 && !state.dialled);
        if (!expected || !state.handshake->read(body)) {
            // **A joiner that does not encrypt**: its first word is not a
            // handshake. Told so in its own terms, and let go once that has
            // gone out.
            if (!state.dialled && !state.surfaced && !m_config.plainRefusal.empty() && !state.heard) {
                state.closing = true;
                state.began = nowMs();
                (void)m_inner->send(peer, m_config.plainRefusal, Delivery::Reliable, HandshakeChannel);
                m_inner->disconnect(peer);
                return;
            }
            refuse(peer, ConnectFailure::Insecure, out);
            return;
        }
        state.heard = true;
        if (tag == Tag::Handshake2) {
            // The other side's key, proved: the one this machine expected?
            if (m_config.trust && !m_config.trust(state.host, state.port, state.handshake->remoteStatic())) {
                refuse(peer, ConnectFailure::KeyChanged, out);
                return;
            }
            const std::optional<std::vector<u8>> third = state.handshake->write();
            if (!third.has_value()) {
                refuse(peer, ConnectFailure::Insecure, out);
                return;
            }
            sendHandshake(peer, Tag::Handshake3, *third);
            finish(peer, state, out);
            return;
        }
        if (tag == Tag::Handshake1) {
            const std::optional<std::vector<u8>> second = state.handshake->write();
            if (!second.has_value()) {
                refuse(peer, ConnectFailure::Insecure, out);
                return;
            }
            sendHandshake(peer, Tag::Handshake2, *second);
            return;
        }
        // The third: the joiner's key is known, and it is the player.
        finish(peer, state, out);
    }

    void expire(std::vector<TransportEvent>& out)
    {
        const u64 now = nowMs();
        std::vector<PeerId> late;
        std::vector<u32> closed;
        for (const auto& [id, state] : m_peers) {
            if (state.closing) {
                // Let go and never heard from again: forgotten.
                if (now - state.began > m_config.handshakeTimeoutMs)
                    closed.push_back(id);
                continue;
            }
            if (!state.surfaced && state.handshake.has_value() && now - state.began > m_config.handshakeTimeoutMs)
                late.push_back(PeerId{id});
        }
        for (const u32 id : closed) {
            m_inner->drop(PeerId{id});
            m_peers.erase(id);
        }
        for (const PeerId peer : late)
            refuse(peer, ConnectFailure::TimedOut, out);
    }

    std::unique_ptr<ITransport> m_inner;
    SecureConfig m_config;
    std::map<u32, Peer> m_peers;
};

} // namespace

std::unique_ptr<ITransport> createSecureTransport(std::unique_ptr<ITransport> inner, SecureConfig config)
{
    return std::make_unique<SecureTransport>(std::move(inner), std::move(config));
}

} // namespace engine::net
