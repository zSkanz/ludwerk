// The ENet implementation of `ITransport` (ADR 0012).
//
// One translation unit, and every ENet type stays inside it. `transport.h`
// exposes a factory returning the interface for exactly that reason (R17): the
// day GameNetworkingSockets or QUIC arrives beside this, nothing above has to
// change, because nothing above ever knew what was underneath.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <enet/enet.h>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <unordered_map>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/net/transport.h"

#if defined(_WIN32)
// `SIO_UDP_CONNRESET` is in <mstcpip.h> on new SDKs and nowhere on old ones:
// named here by its definition, which Windows has not changed since 2000.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

namespace engine::net {

static_assert(EnetPeerCap == static_cast<usize>(ENET_PROTOCOL_MAXIMUM_PEER_ID), "the published cap is ENet's own");
namespace {

using core::f32;
using core::I18nArg;
using core::i64;
using core::LogLevel;
using core::u64;

// What one `poll` hands up at most: far more than a tick of any match, and
// small enough that a flood costs a frame a slice rather than the frame.
constexpr usize MaxEventsPerPoll = 8192;
constexpr usize MaxBytesPerPoll = 64u * 1024u * 1024u;
// What the service thread holds for the frame loop at most. Past it, it stops
// draining and ENet's own queue holds the rest, in order, as it did before.
constexpr usize MaxInboxEvents = 65536;
constexpr usize MaxInboxBytes = 256u * 1024u * 1024u;

// **The datagram size every peer is held to** (NA5): 1200 bytes, what QUIC
// and GameNetworkingSockets settle on. ENet's 1392 does not fit through a VPN
// or a tunnel's smaller path, where a datagram is fragmented by IP or dropped.
constexpr enet_uint32 TransportMtu = 1200;
// How long the service thread waits on the socket for something to arrive
// before it services ENet's timers -- its resends, its pings, its timeouts --
// again.
constexpr enet_uint32 ServiceWaitMs = 1;

// ENet is initialised once per process and deinitialised never.
//
// Never, deliberately. `enet_deinitialize` calls `WSACleanup` on Windows, and
// this process has other things holding sockets -- the dev-server control
// connection (ADR 0035) among them. A transport going away must not take the
// platform's networking down with it, and the cost is one refcount.
[[nodiscard]] bool ensureEnet()
{
    static const bool initialized = [] {
        if (enet_initialize() != 0) {
            core::log(LogLevel::Warn, ENG_TR("net.err.transport_init_failed"), {});
            return false;
        }
        return true;
    }();
    return initialized;
}

// The ENet flags one `Delivery` asks for.
//
// **`UNRELIABLE_FRAGMENT` is on both unreliable modes, and leaving it off was a
// defect that only appeared above one MTU** (D150). `enet_peer_send` tests for
// fragmentation FIRST, before it dispatches on unsequenced-versus-reliable
// (`third_party/enet/peer.c:121`), and its fragment branch reads
// `(flags & (RELIABLE | UNRELIABLE_FRAGMENT)) == UNRELIABLE_FRAGMENT`. With
// neither bit set that test fails, so the `else` sends
// `SEND_FRAGMENT | COMMAND_FLAG_ACKNOWLEDGE` -- **reliable, acknowledged and
// head-of-line blocking**, which is the exact thing `UnreliableSequenced` is
// chosen to avoid. The threshold is `mtu - sizeof(ENetProtocolHeader) -
// sizeof(ENetProtocolSendFragment)`, about 1364 bytes at ENet's default MTU of
// 1392, and a state delta is precisely the payload that crosses it.
[[nodiscard]] constexpr enet_uint32 flagsFor(Delivery delivery) noexcept
{
    switch (delivery) {
    case Delivery::Reliable:
        return ENET_PACKET_FLAG_RELIABLE;
    case Delivery::Unreliable:
        // ENet's UNSEQUENCED is the one that may arrive out of order. Its plain
        // unreliable packet is in fact sequenced, which is a naming trap this
        // switch exists to pay for once rather than at every call site.
        //
        // **A fragmented one is sequenced anyway**, and that is ENet's
        // limitation rather than a choice made here: there is no unsequenced
        // fragment command in the protocol, so a payload over one MTU gets a
        // sequence number. Sequenced-and-droppable is still much nearer this
        // mode's contract -- "may be dropped and may arrive out of order" --
        // than the reliable, ordered delivery it used to silently become.
        return ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
    case Delivery::UnreliableSequenced:
        return ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
    }
    return ENET_PACKET_FLAG_RELIABLE;
}

// **Asserted here rather than in a test, because no test can see it.** ENet is
// linked PRIVATE precisely so that nothing above this file knows its constants
// (the CMakeLists calls that "the R17 rule holding in the build graph"), and a
// loopback round-trip cannot tell the two apart anyway: a packet that wrongly
// became reliable still arrives, with the same bytes, in the same order. What
// went wrong was invisible to every observation available to a test.
//
// A compile-time check costs nothing, runs on every build on every platform,
// and reads as the contract it is guarding.
static_assert((flagsFor(Delivery::Unreliable) & ENET_PACKET_FLAG_RELIABLE) == 0,
              "an unreliable delivery must not be sent reliably");
static_assert((flagsFor(Delivery::UnreliableSequenced) & ENET_PACKET_FLAG_RELIABLE) == 0,
              "an unreliable-sequenced delivery must not be sent reliably");
static_assert((flagsFor(Delivery::Unreliable) & ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT) != 0,
              "an unreliable delivery must stay unreliable above one MTU (D150)");
static_assert((flagsFor(Delivery::UnreliableSequenced) & ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT) != 0,
              "an unreliable-sequenced delivery must stay unreliable above one MTU (D150)");
static_assert((flagsFor(Delivery::Reliable) & ENET_PACKET_FLAG_RELIABLE) != 0,
              "a reliable delivery must be sent reliably");

// **The link conditioner** (netcode ledger A): a relay between this host's
// socket and the server, in this process, that delays, jitters and loses what
// crosses it, both ways. ENet dials the relay's local port instead of the
// server, so everything ENet measures and resends is measured and resent
// through the conditions -- which an in-process wrapper above the transport
// could never give it.
class LinkConditioner
{
public:
    ~LinkConditioner() { stop(); }

    // Starts relaying to `server`. Fills `local` with the address ENet is to
    // dial instead.
    [[nodiscard]] bool start(const ENetAddress& server, const TransportConfig& config, ENetAddress& local)
    {
        stop();
        m_server = server;
        m_delayMs = config.simulatedDelayMs;
        m_jitterMs = config.simulatedJitterMs;
        m_loss = std::clamp(config.simulatedLossPercent, 0.0f, 100.0f) / 100.0f;
        m_near = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        m_far = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (m_near == ENET_SOCKET_NULL || m_far == ENET_SOCKET_NULL)
            return fail();
        ENetAddress loopback{};
        if (enet_address_set_host_ip(&loopback, "127.0.0.1") != 0)
            return fail();
        loopback.port = 0;
        ENetAddress any{};
        any.host = ENET_HOST_ANY;
        any.port = 0;
        if (enet_socket_bind(m_near, &loopback) != 0 || enet_socket_bind(m_far, &any) != 0 ||
            enet_socket_get_address(m_near, &local) != 0)
            return fail();
        (void)enet_socket_set_option(m_near, ENET_SOCKOPT_NONBLOCK, 1);
        (void)enet_socket_set_option(m_far, ENET_SOCKOPT_NONBLOCK, 1);
        local.host = loopback.host;
        m_stopping.store(false);
        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stop()
    {
        if (m_thread.joinable()) {
            m_stopping.store(true);
            m_thread.join();
        }
        if (m_near != ENET_SOCKET_NULL)
            enet_socket_destroy(m_near);
        if (m_far != ENET_SOCKET_NULL)
            enet_socket_destroy(m_far);
        m_near = ENET_SOCKET_NULL;
        m_far = ENET_SOCKET_NULL;
        m_haveClient = false;
        m_pending = {};
    }

private:
    struct Pending
    {
        u64 dueNs = 0;
        bool toServer = false;
        std::vector<u8> bytes;
        [[nodiscard]] bool operator>(const Pending& other) const noexcept { return dueNs > other.dueNs; }
    };

    [[nodiscard]] bool fail()
    {
        stop();
        return false;
    }

    [[nodiscard]] static u64 nowNs() noexcept
    {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // What crossed, held for its delay -- or lost.
    void hold(bool toServer, const u8* data, usize size)
    {
        if (m_loss > 0.0f && std::uniform_real_distribution<f32>(0.0f, 1.0f)(m_random) < m_loss)
            return;
        i64 delayMs = static_cast<i64>(m_delayMs);
        if (m_jitterMs > 0)
            delayMs += std::uniform_int_distribution<i64>(-static_cast<i64>(m_jitterMs),
                                                          static_cast<i64>(m_jitterMs))(m_random);
        const u64 due = nowNs() + static_cast<u64>((std::max<i64>)(delayMs, 0)) * 1'000'000ull;
        m_pending.push(Pending{due, toServer, std::vector<u8>(data, data + size)});
    }

    void run()
    {
        std::array<u8, 4096> buffer{};
        while (!m_stopping.load()) {
            const u64 now = nowNs();
            while (!m_pending.empty() && m_pending.top().dueNs <= now) {
                const Pending& due = m_pending.top();
                ENetBuffer out{};
                out.data = const_cast<u8*>(due.bytes.data());
                out.dataLength = due.bytes.size();
                if (due.toServer)
                    (void)enet_socket_send(m_far, &m_server, &out, 1);
                else if (m_haveClient)
                    (void)enet_socket_send(m_near, &m_client, &out, 1);
                m_pending.pop();
            }
            ENetSocketSet readable;
            ENET_SOCKETSET_EMPTY(readable);
            ENET_SOCKETSET_ADD(readable, m_near);
            ENET_SOCKETSET_ADD(readable, m_far);
            (void)enet_socketset_select((std::max)(m_near, m_far), &readable, nullptr, 1);
            for (int side = 0; side < 2; ++side) {
                const ENetSocket socket = side == 0 ? m_near : m_far;
                while (true) {
                    ENetAddress from{};
                    ENetBuffer in{};
                    in.data = buffer.data();
                    in.dataLength = buffer.size();
                    const int got = enet_socket_receive(socket, &from, &in, 1);
                    if (got <= 0)
                        break;
                    if (side == 0) {
                        m_client = from;
                        m_haveClient = true;
                    }
                    hold(side == 0, buffer.data(), static_cast<usize>(got));
                }
            }
        }
    }

    ENetSocket m_near = ENET_SOCKET_NULL;
    ENetSocket m_far = ENET_SOCKET_NULL;
    ENetAddress m_server{};
    ENetAddress m_client{};
    bool m_haveClient = false;
    u32 m_delayMs = 0;
    u32 m_jitterMs = 0;
    f32 m_loss = 0.0f;
    std::mt19937 m_random{0x6C696E6Bu};
    std::priority_queue<Pending, std::vector<Pending>, std::greater<>> m_pending;
    std::thread m_thread;
    std::atomic<bool> m_stopping{false};
};

class EnetTransport final : public ITransport
{
public:
    ~EnetTransport() override { close(); }

    std::optional<core::EngineError> open(const TransportConfig& config) override
    {
        if (!ensureEnet()) {
            return core::makeError(ENG_TR("net.err.transport_init_failed"));
        }
        close();

        ENetAddress address{};
        ENetAddress* bindTo = nullptr;
        if (config.port != 0) {
            address.host = ENET_HOST_ANY;
            address.port = config.port;
            bindTo = &address;
        }

        if (config.maxPeers == 0 || config.maxPeers > EnetPeerCap) {
            const I18nArg args[] = {{"count", static_cast<core::i64>(config.maxPeers)},
                                    {"cap", static_cast<core::i64>(EnetPeerCap)}};
            return core::makeError(ENG_TR("net.err.transport_peer_cap"), args);
        }
        m_channels = std::max<u8>(1, config.channels);
        m_timeoutMs = std::max<u32>(1, config.timeoutMs);
        m_config = config;
        m_host = enet_host_create(bindTo, config.maxPeers, m_channels, 0, 0);
        if (m_host == nullptr) {
            // **With no port asked for there is no port to be in use** (D431):
            // the system refused the socket itself -- which is what Android
            // does to an app with no INTERNET permission -- and "the port may
            // already be in use", on port 0, sent the reader the wrong way.
            if (config.port == 0)
                return core::makeError(ENG_TR("net.err.transport_socket_failed"));
            const I18nArg args[] = {{"port", static_cast<core::i64>(config.port)}};
            return core::makeError(ENG_TR("net.err.transport_open_failed"), args);
        }
        // ENet's own ceilings are its defaults, and both are generous: a
        // message it reassembles and a peer's waiting data are each bounded
        // by them, per peer.
        if (config.maxMessageBytes != 0) {
            m_host->maximumPacketSize = config.maxMessageBytes;
            m_host->maximumWaitingData = config.maxMessageBytes * 2;
        }
        if (config.maxPeersPerAddress != 0)
            m_host->duplicatePeers = config.maxPeersPerAddress;
        m_host->mtu = TransportMtu;
#if defined(_WIN32)
        // **A peer that went away is not a broken socket** (NA5). Windows
        // reports the "port unreachable" a vanished peer's machine answers
        // with as an error on the NEXT receive, which ended that service's
        // receiving early and held every other peer's data a tick, until the
        // gone one timed out. Every engine that runs UDP on Windows turns it
        // off.
        {
            BOOL report = FALSE;
            DWORD returned = 0;
            (void)WSAIoctl(m_host->socket, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr,
                           nullptr);
        }
#endif
        // **ENet is serviced on its own clock** (N9, NA4): a thread of its
        // own takes what arrives and answers it -- acknowledgements, pings --
        // the moment it does, whatever the frame loop is doing. Serviced once
        // a tick by the frame, a window in the background (ten frames a
        // second) read a ping of a hundred milliseconds, and every message
        // waited up to a tick at each end. What the thread receives waits in
        // `m_inbox` for `poll`, which the frame calls where it always did:
        // nothing reaches the simulation at any other point.
        m_stopping.store(false);
        if (config.serviceThread)
            m_service = std::thread([this] { serviceLoop(); });
        return std::nullopt;
    }

    void close() override
    {
        if (m_service.joinable()) {
            m_stopping.store(true);
            m_service.join();
        }
        // The relay outlives the goodbye below: what is sent through it on the
        // way out still crosses.
        struct StopAfter
        {
            LinkConditioner& relay;
            ~StopAfter() { relay.stop(); }
        } stopAfter{m_conditioner};
        std::lock_guard lock(m_mutex);
        m_inbox.clear();
        m_inboxBytes = 0;
        if (m_host == nullptr) {
            return;
        }
        // **Every peer is told, and not waited for** (D217). A graceful
        // disconnect needs a round trip and this runs from a destructor, so it
        // used to reset them without a word -- and a player who left a match
        // stayed on the server until its timeout. `enet_peer_disconnect_now`
        // sends the notice and flushes it in one call, then resets: no round
        // trip, and the other end hears it unless the packet itself is lost.
        // **What was sent goes first** (NA9): `disconnect_now` threw away the
        // queues, so `FireServer(save)` then `Disconnect()` lost the save. A
        // disconnect after the queue is out, then a short wait for it to go
        // -- a tenth of a second, then whatever is left is reset.
        for (const auto& entry : m_peers)
            enet_peer_disconnect_later(entry.second, 0);
        ENetEvent event{};
        for (int round = 0; round < 10 && !m_peers.empty(); ++round) {
            while (enet_host_service(m_host, &event, 10) > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE)
                    enet_packet_destroy(event.packet);
                else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                    forget(event.peer);
            }
        }
        for (const auto& entry : m_peers) {
            entry.second->data = nullptr;
            enet_peer_disconnect_now(entry.second, 0);
        }
        m_peers.clear();
        enet_host_destroy(m_host);
        m_host = nullptr;
    }

    std::optional<core::EngineError> connect(std::string_view host, u16 port, PeerId& outPeer) override
    {
        outPeer = PeerId{};
        if (m_host == nullptr) {
            return core::makeError(ENG_TR("net.err.transport_not_open"));
        }

        // Resolved outside the lock: it touches no host state, and a slow
        // name server must not hold the service thread.
        ENetAddress address{};
        address.port = port;
        const std::string hostText(host);
        // **An address is read, not looked up** (NA11): a name server is asked
        // only for a name, and the address a player types -- a VPN's, a LAN's
        // -- costs nothing.
        if (enet_address_set_host_ip(&address, hostText.c_str()) != 0 &&
            enet_address_set_host(&address, hostText.c_str()) != 0) {
            const I18nArg args[] = {{"host", hostText}};
            return core::makeError(ENG_TR("net.err.connect_unresolved"), args);
        }

        // Through the link conditioner when this host was asked for a worse
        // network (netcode ledger A): ENet dials the relay, which dials the
        // server.
        if (m_config.simulatedDelayMs != 0 || m_config.simulatedJitterMs != 0 || m_config.simulatedLossPercent > 0.0f) {
            ENetAddress relay{};
            if (!m_conditioner.start(address, m_config, relay)) {
                const I18nArg args[] = {{"host", hostText}};
                return core::makeError(ENG_TR("net.err.connect_unresolved"), args);
            }
            address = relay;
        }
        std::lock_guard lock(m_mutex);
        ENetPeer* const peer = enet_host_connect(m_host, &address, m_channels, 0);
        if (peer == nullptr) {
            const I18nArg args[] = {{"host", hostText}, {"port", static_cast<core::i64>(port)}};
            return core::makeError(ENG_TR("net.err.transport_no_peer_slot"), args);
        }

        // The throttle is set when the handshake is done (`take`): a command
        // queued on a peer still connecting breaks the handshake.
        enet_peer_timeout(peer, 0, m_timeoutMs, m_timeoutMs);
        outPeer = track(peer);
        return std::nullopt;
    }

    void disconnect(PeerId peer) override
    {
        std::lock_guard lock(m_mutex);
        const auto at = m_peers.find(peer.value);
        if (at == m_peers.end()) {
            return;
        }
        // After what was queued to it (NA9).
        enet_peer_disconnect_later(at->second, 0);
    }

    void drop(PeerId peer) override
    {
        std::lock_guard lock(m_mutex);
        const auto at = m_peers.find(peer.value);
        if (at == m_peers.end()) {
            return;
        }
        ENetPeer* const gone = at->second;
        forget(gone);
        enet_peer_reset(gone);
    }

    std::optional<core::EngineError> send(PeerId peer, std::span<const u8> payload, Delivery delivery,
                                          u8 channel) override
    {
        if (m_host == nullptr) {
            return core::makeError(ENG_TR("net.err.transport_not_open"));
        }
        std::lock_guard lock(m_mutex);
        const auto at = m_peers.find(peer.value);
        if (at == m_peers.end()) {
            return core::makeError(ENG_TR("net.err.transport_unknown_peer"));
        }
        if (channel >= m_channels) {
            const I18nArg args[] = {{"channel", static_cast<core::i64>(channel)},
                                    {"channels", static_cast<core::i64>(m_channels)}};
            return core::makeError(ENG_TR("net.err.transport_bad_channel"), args);
        }

        ENetPacket* const packet = enet_packet_create(payload.data(), payload.size(), flagsFor(delivery));
        if (packet == nullptr) {
            return core::makeError(ENG_TR("net.err.transport_send_failed"));
        }
        if (enet_peer_send(at->second, channel, packet) != 0) {
            // ENet takes ownership on SUCCESS only, so a failed send is ours to
            // free. Getting that backwards is a leak per dropped packet, which
            // is the shape of leak nobody finds.
            enet_packet_destroy(packet);
            return core::makeError(ENG_TR("net.err.transport_send_failed"));
        }
        return std::nullopt;
    }

    void flush() override
    {
        std::lock_guard lock(m_mutex);
        if (m_host != nullptr)
            enet_host_flush(m_host);
    }

    std::optional<core::EngineError> poll(std::vector<TransportEvent>& out, u32 timeoutMs) override
    {
        if (m_host == nullptr) {
            return core::makeError(ENG_TR("net.err.transport_not_open"));
        }
        std::unique_lock lock(m_mutex);
        if (!m_service.joinable()) {
            // No thread: serviced here, as it always was. The FIRST service
            // call carries the timeout and the rest do not.
            ENetEvent event{};
            enet_uint32 wait = timeoutMs;
            while (m_inbox.size() < MaxInboxEvents && m_inboxBytes < MaxInboxBytes &&
                   enet_host_service(m_host, &event, wait) > 0) {
                wait = 0;
                (void)take(event);
            }
        }
        // The wait is for something, not for each thing: one wait, then
        // whatever is there.
        else if (m_inbox.empty() && timeoutMs > 0) {
            m_arrived.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !m_inbox.empty(); });
        }
        // **Bounded a poll** (audit N1's review): a peer sending as fast as
        // the link carries kept this loop draining, and the frame that called
        // it never ended. What is over the bound waits for the next poll, in
        // order.
        usize events = 0;
        usize bytes = 0;
        while (!m_inbox.empty() && events < MaxEventsPerPoll && bytes < MaxBytesPerPoll) {
            TransportEvent& event = m_inbox.front();
            bytes += event.payload.size();
            m_inboxBytes -= event.payload.size();
            out.push_back(std::move(event));
            m_inbox.pop_front();
            events += 1;
        }
        return std::nullopt;
    }

    [[nodiscard]] usize peerCount() const noexcept override
    {
        std::lock_guard lock(m_mutex);
        return m_peers.size();
    }

    [[nodiscard]] PeerLink link(PeerId peer) const noexcept override
    {
        std::lock_guard lock(m_mutex);
        const auto at = m_peers.find(peer.value);
        if (at == m_peers.end())
            return {};
        const ENetPeer* const found = at->second;
        return PeerLink{.roundTripMs = found->roundTripTime,
                        .jitterMs = found->roundTripTimeVariance,
                        .loss = static_cast<f32>(found->packetLoss) / static_cast<f32>(ENET_PEER_PACKET_LOSS_SCALE)};
    }

private:
    // The service thread: everything ENet has, into the inbox, then a wait on
    // the socket -- outside the lock, so a send is never held behind it.
    void serviceLoop()
    {
        const ENetSocket socket = m_host->socket;
        while (!m_stopping.load()) {
            bool arrived = false;
            {
                std::lock_guard lock(m_mutex);
                ENetEvent event{};
                while (m_inbox.size() < MaxInboxEvents && m_inboxBytes < MaxInboxBytes &&
                       enet_host_service(m_host, &event, 0) > 0) {
                    arrived = take(event) || arrived;
                }
            }
            if (arrived)
                m_arrived.notify_all();
            enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
            (void)enet_socket_wait(socket, &condition, ServiceWaitMs);
        }
    }

    // One ENet event, as the transport's. Called with the lock held.
    bool take(ENetEvent& event)
    {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            configurePeer(event.peer);
            // Every field named, empty ones included. Clang counts a skipped
            // designator as a missing initializer under `-Werror`, and the
            // Tier-2 build is where that is discovered.
            m_inbox.push_back(TransportEvent{
                .kind = TransportEvent::Kind::Connected, .peer = track(event.peer), .payload = {}, .channel = 0});
            return true;
        case ENET_EVENT_TYPE_DISCONNECT: {
            const PeerId id = idOf(event.peer);
            forget(event.peer);
            m_inbox.push_back(
                TransportEvent{.kind = TransportEvent::Kind::Disconnected, .peer = id, .payload = {}, .channel = 0});
            return true;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            TransportEvent message{
                .kind = TransportEvent::Kind::Message, .peer = idOf(event.peer), .payload = {}, .channel = 0};
            message.channel = event.channelID;
            const auto* const data = reinterpret_cast<const u8*>(event.packet->data);
            message.payload.assign(data, data + event.packet->dataLength);
            m_inboxBytes += event.packet->dataLength;
            enet_packet_destroy(event.packet);
            m_inbox.push_back(std::move(message));
            return true;
        }
        case ENET_EVENT_TYPE_NONE:
            break;
        }
        return false;
    }

    // What every peer is held to, either side of the handshake: the silence
    // limit, and **a throttle that never throttles** (NA5). ENet's packet
    // throttle answers a rising round trip by dropping a share of unreliable
    // sends -- snapshots and intents -- for up to its five-second interval,
    // and narrows the reliable window to a packet a round trip: after one
    // hitch, a window going to the back, a Wi-Fi blip, play stuttered for
    // seconds. A game's traffic is the rate the game sends, and pacing it is
    // the game's business.
    void configurePeer(ENetPeer* peer)
    {
        enet_peer_timeout(peer, 0, m_timeoutMs, m_timeoutMs);
        enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, 0, 0);
        peer->packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE;
    }

    // The id lives in ENet's own per-peer `data` pointer rather than in a map
    // keyed by peer address. That is what keeps `idOf` correct after a peer
    // slot is REUSED: ENet clears the field on reset, so a recycled slot cannot
    // answer with the previous connection's id.
    [[nodiscard]] PeerId track(ENetPeer* peer)
    {
        if (peer->data != nullptr) {
            return PeerId{static_cast<u32>(reinterpret_cast<std::uintptr_t>(peer->data))};
        }
        const u32 id = m_nextId++;
        peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
        m_peers.emplace(id, peer);
        return PeerId{id};
    }

    [[nodiscard]] static PeerId idOf(ENetPeer* peer) noexcept
    {
        if (peer == nullptr || peer->data == nullptr) {
            return PeerId{};
        }
        return PeerId{static_cast<u32>(reinterpret_cast<std::uintptr_t>(peer->data))};
    }

    void forget(ENetPeer* peer)
    {
        const PeerId id = idOf(peer);
        peer->data = nullptr;
        m_peers.erase(id.value);
    }

    ENetHost* m_host = nullptr;
    u8 m_channels = 2;
    u32 m_timeoutMs = 10000;
    // Ids start at one, so a default-constructed `PeerId` is never a peer.
    u32 m_nextId = 1;
    std::unordered_map<u32, ENetPeer*> m_peers;

    // **Every ENet call is under this lock**: ENet is not thread-safe, and the
    // service thread and the frame both reach it.
    mutable std::mutex m_mutex;
    std::condition_variable m_arrived;
    std::deque<TransportEvent> m_inbox;
    usize m_inboxBytes = 0;
    std::thread m_service;
    std::atomic<bool> m_stopping{false};
    TransportConfig m_config;
    LinkConditioner m_conditioner;
};

} // namespace

std::unique_ptr<ITransport> createEnetTransport()
{
    return std::make_unique<EnetTransport>();
}

} // namespace engine::net
