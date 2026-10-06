// The ENet implementation of `ITransport` (ADR 0012).
//
// One translation unit, and every ENet type stays inside it. `transport.h`
// exposes a factory returning the interface for exactly that reason (R17): the
// day GameNetworkingSockets or QUIC arrives beside this, nothing above has to
// change, because nothing above ever knew what was underneath.
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <enet/enet.h>
#include <mutex>
#include <queue>
#include <random>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "enet_internal.h"
#include "engine/core/crypto.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/net/local_address.h"
#include "engine/net/transport.h"

#if !defined(_WIN32)
#include <poll.h>
#endif

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
std::atomic<NameLookup> g_nameLookup{nullptr};
} // namespace

void setNameLookupForTests(NameLookup lookup) noexcept
{
    g_nameLookup.store(lookup);
}

namespace detail {

// ENet is initialised once per process and deinitialised never.
//
// Never, deliberately. `enet_deinitialize` calls `WSACleanup` on Windows, and
// this process has other things holding sockets -- the dev-server control
// connection (ADR 0035) among them. A transport going away must not take the
// platform's networking down with it, and the cost is one refcount.
bool ensureEnet()
{
    static const bool initialized = [] {
        if (enet_initialize() != 0) {
            core::log(core::LogLevel::Warn, ENG_TR("net.err.transport_init_failed"), {});
            return false;
        }
        return true;
    }();
    return initialized;
}

bool resolve(std::string_view text, core::u16 defaultPort, ENetAddress& out)
{
    std::string host(text);
    core::u16 port = defaultPort;
    if (const usize colon = host.rfind(':'); colon != std::string::npos) {
        core::u32 value = 0;
        const char* const first = host.data() + colon + 1;
        const char* const last = host.data() + host.size();
        const auto [end, error] = std::from_chars(first, last, value);
        if (first == last || error != std::errc{} || end != last || value == 0 || value > 65535)
            return false;
        port = static_cast<core::u16>(value);
        host.resize(colon);
    }
    if (host.empty() || port == 0)
        return false;
    out = ENetAddress{};
    out.port = port;
    // Read as an address first, and looked up only where it is a name (NA11).
    if (enet_address_set_host_ip(&out, host.c_str()) == 0)
        return true;
    if (const NameLookup lookup = g_nameLookup.load(); lookup != nullptr) {
        std::string dotted;
        return lookup(host, dotted) && enet_address_set_host_ip(&out, dotted.c_str()) == 0;
    }
    return enet_address_set_host(&out, host.c_str()) == 0;
}

void ignorePortUnreachable(ENetSocket socket) noexcept
{
#if defined(_WIN32)
    BOOL report = FALSE;
    DWORD returned = 0;
    (void)WSAIoctl(socket, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
    (void)socket;
#endif
}

core::u64 steadyMs() noexcept
{
    static const auto start = std::chrono::steady_clock::now();
    const auto since = std::chrono::steady_clock::now() - start;
    return static_cast<core::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

} // namespace detail

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

using detail::ensureEnet;

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

// Many sockets waited on at once. Not `select`: its set holds sixty-four
// sockets on Windows, and a host's pipes are two apiece.
#if defined(_WIN32)
using PollEntry = WSAPOLLFD;
constexpr short PollReadable = POLLRDNORM;
[[nodiscard]] int pollSockets(PollEntry* entries, usize count, int timeoutMs)
{
    return WSAPoll(entries, static_cast<ULONG>(count), timeoutMs);
}
#else
using PollEntry = pollfd;
constexpr short PollReadable = POLLIN;
[[nodiscard]] int pollSockets(PollEntry* entries, usize count, int timeoutMs)
{
    return poll(entries, static_cast<nfds_t>(count), timeoutMs);
}
#endif

void sendTo(ENetSocket socket, const ENetAddress& to, std::span<const u8> datagram)
{
    ENetBuffer buffer{};
    buffer.data = const_cast<void*>(static_cast<const void*>(datagram.data()));
    buffer.dataLength = datagram.size();
    (void)enet_socket_send(socket, &to, &buffer, 1);
}

// 10/8, 172.16/12, 192.168/16, and a machine's own 127/8 and 169.254/16: an
// address that means something only on the network it is on.
[[nodiscard]] constexpr bool isLocalAddress(u32 address) noexcept
{
    return (address >> 24) == 10 || (address >> 24) == 127 || (address >> 20) == ((172u << 4) | 1u) ||
           (address >> 16) == ((192u << 8) | 168u) || (address >> 16) == ((169u << 8) | 254u);
}

// **A host's sockets for the joiners a relay carries** (ADR 0178 §5). The
// relay forwards by where a datagram came from and adds nothing to it, so on
// the way back it must be told which joiner a datagram is for by the socket it
// comes from: one UDP socket per relayed joiner, which says its slot to the
// relay and from then on is that joiner's address there.
//
// Inside this process each is a pipe to ENet's own socket: what the relay
// hands it goes in to ENet from a loopback address of its own -- so ENet sees
// one peer per joiner, as it would across the internet -- and what ENet
// answers to that address goes out to the relay. ENet is not patched and does
// not know.
class PipeSet
{
public:
    ~PipeSet() { stop(); }

    void start(const ENetAddress& relay, u16 hostPort, const rendezvous::Token& token)
    {
        stop();
        m_relay = relay;
        m_token = token;
        m_inwardTo = ENetAddress{};
        (void)enet_address_set_host_ip(&m_inwardTo, "127.0.0.1");
        m_inwardTo.port = hostPort;
        m_stopping.store(false);
        m_thread = std::thread([this] { run(); });
    }

    void stop()
    {
        if (m_thread.joinable()) {
            m_stopping.store(true);
            m_thread.join();
        }
        std::lock_guard lock(m_mutex);
        for (Pipe& pipe : m_pipes)
            destroy(pipe);
        m_pipes.clear();
        m_lost.store(false);
    }

    // False where the system gave no socket; the joiner's wait for the relay
    // then ends as a path that did not open.
    bool open(const rendezvous::HostRendezvous::PipeRequest& request, u64 nowMs)
    {
        std::lock_guard lock(m_mutex);
        for (const Pipe& pipe : m_pipes)
            if (pipe.state.slot() == request.slot)
                return true;
        Pipe pipe;
        pipe.outward = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        pipe.inward = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        ENetAddress any{};
        any.host = ENET_HOST_ANY;
        // **A loopback address of its own**, 127.a.b.1 with the slot in a and
        // b: a host that limits the connections of one address
        // (`maxPeersPerAddress`) would otherwise count every relayed joiner
        // as 127.0.0.1. Where the system has only 127.0.0.1 -- macOS -- it is
        // that, and the limit is the relay's to keep (`slotsPerAddress`).
        ENetAddress own{};
        own.host = ENET_HOST_TO_NET_32((127u << 24) | (static_cast<u32>(request.slot) << 8) | 1u);
        ENetAddress plain{};
        (void)enet_address_set_host_ip(&plain, "127.0.0.1");
        const bool bound = pipe.outward != ENET_SOCKET_NULL && pipe.inward != ENET_SOCKET_NULL &&
                           enet_socket_bind(pipe.outward, &any) == 0 &&
                           (enet_socket_bind(pipe.inward, &own) == 0 || enet_socket_bind(pipe.inward, &plain) == 0) &&
                           enet_socket_get_address(pipe.inward, &pipe.seenAs) == 0;
        if (!bound) {
            destroy(pipe);
            return false;
        }
        for (const ENetSocket socket : {pipe.outward, pipe.inward}) {
            (void)enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1);
            detail::ignorePortUnreachable(socket);
        }
        pipe.state.start(detail::toEndpoint(m_relay), m_token, request, nowMs);
        pipe.openedMs = nowMs;
        m_pipes.push_back(std::move(pipe));
        return true;
    }

    void close(u16 slot)
    {
        std::lock_guard lock(m_mutex);
        for (auto pipe = m_pipes.begin(); pipe != m_pipes.end(); ++pipe) {
            if (pipe->state.slot() != slot)
                continue;
            destroy(*pipe);
            m_pipes.erase(pipe);
            return;
        }
    }

    // The slot whose joiner ENet sees at `address`; zero for an address that
    // is nobody's pipe.
    [[nodiscard]] u16 slotAt(const ENetAddress& address) const
    {
        std::lock_guard lock(m_mutex);
        for (const Pipe& pipe : m_pipes)
            if (pipe.seenAs.host == address.host && pipe.seenAs.port == address.port)
                return pipe.state.slot();
        return 0;
    }

    void attach(u16 slot, u32 peer)
    {
        std::lock_guard lock(m_mutex);
        for (Pipe& pipe : m_pipes)
            if (pipe.state.slot() == slot)
                pipe.peer = peer;
    }

    [[nodiscard]] u16 slotOf(u32 peer) const
    {
        std::lock_guard lock(m_mutex);
        for (const Pipe& pipe : m_pipes)
            if (pipe.peer == peer && peer != 0)
                return pipe.state.slot();
        return 0;
    }

    // How long the relay is to keep the slot's joiner away once it is gone.
    void setBan(u16 slot, u16 seconds)
    {
        std::lock_guard lock(m_mutex);
        for (Pipe& pipe : m_pipes)
            if (pipe.state.slot() == slot)
                pipe.banSeconds = seconds;
    }

    [[nodiscard]] u16 banOf(u16 slot) const
    {
        std::lock_guard lock(m_mutex);
        for (const Pipe& pipe : m_pipes)
            if (pipe.state.slot() == slot)
                return pipe.banSeconds;
        return 0;
    }

    // Pipes the relay asked for that no joiner then came through: it found a
    // better path, or gave up.
    [[nodiscard]] std::vector<u16> unused(u64 nowMs) const
    {
        constexpr u64 UnusedAfterMs = 20'000;
        std::vector<u16> out;
        std::lock_guard lock(m_mutex);
        for (const Pipe& pipe : m_pipes)
            if (pipe.peer == 0 && nowMs - pipe.openedMs > UnusedAfterMs)
                out.push_back(pipe.state.slot());
        return out;
    }

    // True once after the relay said it knows no such session.
    [[nodiscard]] bool takeLost() noexcept { return m_lost.exchange(false); }

private:
    struct Pipe
    {
        rendezvous::HostPipe state;
        // To the relay, and to this process's ENet.
        ENetSocket outward = ENET_SOCKET_NULL;
        ENetSocket inward = ENET_SOCKET_NULL;
        // The inward socket's own address: who ENet takes the joiner for.
        ENetAddress seenAs{};
        u32 peer = 0;
        u64 openedMs = 0;
        u16 banSeconds = 0;
    };

    static void destroy(Pipe& pipe)
    {
        if (pipe.outward != ENET_SOCKET_NULL)
            enet_socket_destroy(pipe.outward);
        if (pipe.inward != ENET_SOCKET_NULL)
            enet_socket_destroy(pipe.inward);
        pipe.outward = ENET_SOCKET_NULL;
        pipe.inward = ENET_SOCKET_NULL;
    }

    void run()
    {
        constexpr int WaitMs = 10;
        std::array<u8, ENET_PROTOCOL_MAXIMUM_MTU> bytes{};
        std::vector<PollEntry> entries;
        while (!m_stopping.load()) {
            entries.clear();
            {
                std::lock_guard lock(m_mutex);
                for (const Pipe& pipe : m_pipes) {
                    for (const ENetSocket socket : {pipe.outward, pipe.inward}) {
                        PollEntry entry{};
                        entry.fd = socket;
                        entry.events = PollReadable;
                        entries.push_back(entry);
                    }
                }
            }
            if (entries.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(WaitMs));
                continue;
            }
            (void)pollSockets(entries.data(), entries.size(), WaitMs);
            // Every pipe is looked at, whatever the wait said: a pipe may have
            // been opened or closed while it waited, and a look at a socket
            // with nothing on it costs one call.
            std::lock_guard lock(m_mutex);
            const u64 now = detail::steadyMs();
            for (Pipe& pipe : m_pipes) {
                carry(pipe, true, bytes, now);
                carry(pipe, false, bytes, now);
                pipe.state.tick(now, [&](rendezvous::Endpoint, std::span<const u8> datagram) {
                    sendTo(pipe.outward, m_relay, datagram);
                });
                if (pipe.state.takeLost())
                    m_lost.store(true);
            }
        }
    }

    void carry(Pipe& pipe, bool fromRelay, std::array<u8, ENET_PROTOCOL_MAXIMUM_MTU>& bytes, u64 now)
    {
        constexpr int MostPerLook = 256;
        for (int taken = 0; taken < MostPerLook; ++taken) {
            ENetAddress from{};
            ENetBuffer buffer{};
            buffer.data = bytes.data();
            buffer.dataLength = bytes.size();
            const int got = enet_socket_receive(fromRelay ? pipe.outward : pipe.inward, &from, &buffer, 1);
            if (got <= 0)
                return;
            const std::span<const u8> datagram(bytes.data(), static_cast<usize>(got));
            if (fromRelay) {
                // Only the relay speaks to this socket; anyone else who found
                // its port is not carried in to the match.
                if (from.host != m_relay.host || from.port != m_relay.port)
                    continue;
                if (!pipe.state.receive(detail::toEndpoint(from), datagram, now))
                    sendTo(pipe.inward, m_inwardTo, datagram);
            }
            else if (from.port == m_inwardTo.port && (ENET_NET_TO_HOST_32(from.host) >> 24) == 127) {
                sendTo(pipe.outward, m_relay, datagram);
            }
        }
    }

    mutable std::mutex m_mutex;
    std::vector<Pipe> m_pipes;
    std::thread m_thread;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_lost{false};
    ENetAddress m_relay{};
    ENetAddress m_inwardTo{};
    rendezvous::Token m_token{};
};

class EnetTransport;
// The transport whose host is being serviced on this thread: ENet's intercept
// hook is handed the host and nothing of ours.
thread_local EnetTransport* t_servicing = nullptr;

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
        detail::ignorePortUnreachable(m_host->socket);
        // What a relay, a host and a joiner say to find each other arrives on
        // this same socket, marked (ADR 0178): taken before ENet reads it.
        m_host->intercept = &EnetTransport::intercept;
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
        m_joining.clear();
        for (const auto& entry : m_peers)
            enet_peer_disconnect_later(entry.second, 0);
        ENetEvent event{};
        for (int round = 0; round < 10 && !m_peers.empty(); ++round) {
            while (service(&event, 10) > 0) {
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
        m_paths.clear();
        m_dialling.clear();
        // The relay is told last: its joiners' goodbyes crossed it first.
        if (m_hosting.active())
            m_hosting.stop(sender());
        m_pipes.stop();
        m_codeKnown = false;
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
        m_paths[outPeer.value] = isLocalAddress(ENET_NET_TO_HOST_32(address.host)) ? PeerPath::Lan : PeerPath::Direct;
        return std::nullopt;
    }

    void disconnect(PeerId peer) override
    {
        std::lock_guard lock(m_mutex);
        // A join by code that is given up before it found its host.
        std::erase_if(m_joining, [peer](const Joining& joining) { return joining.id == peer.value; });
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
        std::erase_if(m_joining, [peer](const Joining& joining) { return joining.id == peer.value; });
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
            while (m_inbox.size() < MaxInboxEvents && m_inboxBytes < MaxInboxBytes && service(&event, wait) > 0) {
                wait = 0;
                (void)take(event);
            }
            (void)attend();
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

    // --- Reaching a host behind a NAT (ADR 0178) -----------------------------

    std::optional<core::EngineError> useRelay(std::string_view relay) override
    {
        if (m_host == nullptr)
            return core::makeError(ENG_TR("net.err.transport_not_open"));
        if (m_config.port == 0)
            return core::makeError(ENG_TR("net.err.relay_needs_port"));
        // Outside the lock: a name server is not waited for with ENet held.
        ENetAddress address{};
        if (!detail::resolve(relay, DefaultRelayPort, address)) {
            // **Not found now is not "not there"** (D562): a record made a
            // minute ago, a network that comes up after the game. The name is
            // kept and looked up again; it was asked once and never again,
            // and the host had to be hosted anew to be found.
            std::lock_guard lock(m_mutex);
            if (m_hosting.active())
                m_hosting.stop(sender());
            m_pipes.stop();
            m_codeKnown = false;
            m_relayName = std::string(relay);
            m_relayLookup.reset();
            m_relayLookupAtMs = detail::steadyMs() + m_config.relayLookupEveryMs;
            return relayUnresolved(relay, m_config.relayLookupEveryMs);
        }
        const rendezvous::Locals locals = localsOf(m_config.port);
        std::lock_guard lock(m_mutex);
        m_relayName.clear();
        m_relayLookup.reset();
        registerWith(address, locals);
        return std::nullopt;
    }

    void leaveRelay() override
    {
        std::lock_guard lock(m_mutex);
        if (m_host != nullptr && m_hosting.active())
            m_hosting.stop(sender());
        m_pipes.stop();
        m_codeKnown = false;
        m_relayName.clear();
        m_relayLookup.reset();
    }

    [[nodiscard]] RelayState relayState() const noexcept override
    {
        std::lock_guard lock(m_mutex);
        // A relay still being looked for is one that cannot be reached yet.
        return m_relayName.empty() ? m_hosting.state() : RelayState::Unreachable;
    }

    [[nodiscard]] std::string joinCode() const override
    {
        std::lock_guard lock(m_mutex);
        // Once the relay has said it, it is the session's for good: a relay
        // that goes quiet for a while has not made the code another.
        return m_codeKnown ? m_hosting.code().str() : std::string();
    }

    void setOccupancy(usize players, usize maxPlayers) override
    {
        // A byte each on the wire, and what the relay asks of them is one
        // thing: is there room. Past 255 that answer is kept and the counts
        // are not.
        if (maxPlayers > 255) {
            players = players >= maxPlayers ? 255 : std::min<usize>(players, 254);
            maxPlayers = 255;
        }
        std::lock_guard lock(m_mutex);
        m_hosting.setPlayers(static_cast<u8>(std::min<usize>(players, 255)), static_cast<u8>(maxPlayers));
    }

    std::optional<core::EngineError> connectByCode(std::string_view relay, std::string_view code, bool direct,
                                                   PeerId& outPeer) override
    {
        outPeer = PeerId{};
        if (m_host == nullptr)
            return core::makeError(ENG_TR("net.err.transport_not_open"));
        const std::optional<rendezvous::Code> parsed = rendezvous::parseCode(code);
        if (!parsed) {
            const I18nArg args[] = {{"code", std::string(code)}};
            return core::makeError(ENG_TR("net.err.join_code_malformed"), args);
        }
        ENetAddress address{};
        if (!detail::resolve(relay, DefaultRelayPort, address))
            return relayUnresolved(relay);
        std::array<u8, 8> random{};
        core::secureRandom(random);
        u64 nonce = 0;
        for (const u8 byte : random)
            nonce = (nonce << 8) | byte;

        std::lock_guard lock(m_mutex);
        // This socket's port, where the system has given it one yet: what a
        // host on the same network knocks on.
        ENetAddress bound{};
        const u16 port = enet_socket_get_address(m_host->socket, &bound) == 0 ? bound.port : u16{0};
        Joining& joining = m_joining.emplace_back();
        joining.id = m_nextId++;
        joining.rendezvous.start(detail::toEndpoint(address), *parsed, localsOf(port), nonce, detail::steadyMs(),
                                 rendezvous::JoinRendezvous::Options{.direct = direct});
        outPeer = PeerId{joining.id};
        return std::nullopt;
    }

    [[nodiscard]] PeerPath path(PeerId peer) const noexcept override
    {
        std::lock_guard lock(m_mutex);
        const auto at = m_paths.find(peer.value);
        return at == m_paths.end() ? PeerPath::None : at->second;
    }

    void ban(PeerId peer, u32 seconds) override
    {
        std::lock_guard lock(m_mutex);
        // Said to the relay when the peer is gone, not now: the goodbye the
        // host is about to send it crosses the relay too.
        if (const u16 slot = m_pipes.slotOf(peer.value); slot != 0)
            m_pipes.setBan(slot, static_cast<u16>(std::min<u32>(seconds, 65535)));
    }

private:
    struct Joining
    {
        u32 id = 0;
        rendezvous::JoinRendezvous rendezvous;
    };

    [[nodiscard]] static std::optional<core::EngineError> relayUnresolved(std::string_view relay)
    {
        const I18nArg args[] = {{"relay", std::string(relay)}, {"port", static_cast<core::i64>(DefaultRelayPort)}};
        return core::makeError(ENG_TR("net.err.relay_unresolved"), args);
    }

    // The same, of a host's relay: it goes on being looked for.
    [[nodiscard]] static std::optional<core::EngineError> relayUnresolved(std::string_view relay, u32 everyMs)
    {
        const I18nArg args[] = {{"relay", std::string(relay)},
                                {"port", static_cast<core::i64>(DefaultRelayPort)},
                                {"seconds", static_cast<core::i64>((everyMs + 999) / 1000)}};
        return core::makeError(ENG_TR("net.err.relay_unresolved_host"), args);
    }

    // A relay found: this host registers with it. Called with the lock held.
    void registerWith(const ENetAddress& address, const rendezvous::Locals& locals)
    {
        // Drawn once a registration: its hash is the code, and nobody without
        // it can register that code.
        rendezvous::Token token{};
        core::secureRandom(token);
        if (m_hosting.active())
            m_hosting.stop(sender());
        m_codeKnown = false;
        m_pipes.start(address, m_config.port, token);
        m_hosting.start(detail::toEndpoint(address), token, locals, detail::steadyMs());
    }

    // **A relay's name that was not found, looked up again** (D562). On a
    // thread of its own, which is handed what it needs and answers into a
    // record both hold: a transport closed while a name server is silent does
    // not wait for it. Called with the lock held, wherever ENet is serviced.
    void lookRelayUp()
    {
        if (m_relayName.empty())
            return;
        if (m_relayLookup != nullptr) {
            const int state = m_relayLookup->state.load();
            if (state == RelayLookup::Looking)
                return;
            if (state == RelayLookup::Found) {
                registerWith(m_relayLookup->address, m_relayLookup->locals);
                m_relayName.clear();
            }
            else {
                m_relayLookupAtMs = detail::steadyMs() + m_config.relayLookupEveryMs;
            }
            m_relayLookup.reset();
            return;
        }
        if (detail::steadyMs() < m_relayLookupAtMs)
            return;
        m_relayLookup = std::make_shared<RelayLookup>();
        std::thread([lookup = m_relayLookup, name = m_relayName, port = m_config.port] {
            const bool found = detail::resolve(name, DefaultRelayPort, lookup->address);
            if (found)
                lookup->locals = localsOf(port);
            lookup->state.store(found ? RelayLookup::Found : RelayLookup::NotFound);
        }).detach();
    }

    // This machine's own addresses, for a peer on the same network to try.
    [[nodiscard]] static rendezvous::Locals localsOf(u16 port)
    {
        rendezvous::Locals locals;
        locals.port = port;
        for (const std::string& dotted : localAddresses()) {
            const std::optional<u32> address = rendezvous::parseAddress(dotted);
            if (address && locals.count < rendezvous::MostLocal)
                locals.address[locals.count++] = *address;
        }
        return locals;
    }

    // Every call that lets ENet read its socket goes through here, so the
    // intercept hook knows whose host it was handed.
    int service(ENetEvent* event, enet_uint32 waitMs)
    {
        t_servicing = this;
        const int result = enet_host_service(m_host, event, waitMs);
        t_servicing = nullptr;
        return result;
    }

    static int ENET_CALLBACK intercept(ENetHost* host, ENetEvent* event)
    {
        (void)event;
        EnetTransport* const self = t_servicing;
        if (self == nullptr || self->m_host != host)
            return 0;
        const std::span<const u8> datagram(host->receivedData, host->receivedDataLength);
        if (!rendezvous::marked(datagram))
            return 0;
        // Marked: never the match's, whoever it was for.
        const rendezvous::Endpoint from = detail::toEndpoint(host->receivedAddress);
        const u64 now = detail::steadyMs();
        const rendezvous::Send send = self->sender();
        if (self->m_hosting.active())
            (void)self->m_hosting.receive(from, datagram, now, send);
        for (Joining& joining : self->m_joining)
            (void)joining.rendezvous.receive(from, datagram, now, send);
        return 1;
    }

    // Out of ENet's own socket: the mapping a knock opens in a router is then
    // the one the match uses. Called with the lock held.
    [[nodiscard]] rendezvous::Send sender()
    {
        return [this](rendezvous::Endpoint to, std::span<const u8> datagram) {
            sendTo(m_host->socket, detail::toAddress(to), datagram);
        };
    }

    // The rendezvous's own clockwork: registrations and knocks sent, pipes
    // opened, a join that found its host turned into ENet's connection and one
    // that did not into the event that says why. Called with the lock held,
    // wherever ENet is serviced. True when something was put in the inbox.
    bool attend()
    {
        lookRelayUp();
        if (!m_hosting.active() && m_joining.empty())
            return false;
        const u64 now = detail::steadyMs();
        const rendezvous::Send send = sender();
        if (m_hosting.active()) {
            if (m_pipes.takeLost())
                m_hosting.refresh();
            m_hosting.tick(now, send);
            m_codeKnown = m_codeKnown || m_hosting.state() == RelayState::Ready;
            for (const rendezvous::HostRendezvous::PipeRequest& request : m_hosting.takePipeRequests())
                (void)m_pipes.open(request, now);
            for (const u16 slot : m_pipes.unused(now)) {
                m_pipes.close(slot);
                m_hosting.close(slot, 0, send);
            }
        }
        bool arrived = false;
        for (auto joining = m_joining.begin(); joining != m_joining.end();) {
            joining->rendezvous.tick(now, send);
            ConnectFailure failure = joining->rendezvous.failure();
            if (joining->rendezvous.found()) {
                const ENetAddress address = detail::toAddress(joining->rendezvous.target());
                ENetPeer* const peer = enet_host_connect(m_host, &address, m_channels, 0);
                if (peer != nullptr) {
                    enet_peer_timeout(peer, 0, m_timeoutMs, m_timeoutMs);
                    peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(joining->id));
                    m_peers.emplace(joining->id, peer);
                    m_paths[joining->id] = joining->rendezvous.path();
                    m_dialling.insert(joining->id);
                    joining = m_joining.erase(joining);
                    continue;
                }
                failure = ConnectFailure::Busy;
            }
            else if (!joining->rendezvous.failed()) {
                ++joining;
                continue;
            }
            m_inbox.push_back(TransportEvent{.kind = TransportEvent::Kind::Disconnected,
                                             .peer = PeerId{joining->id},
                                             .payload = {},
                                             .channel = 0,
                                             .failure = failure});
            arrived = true;
            joining = m_joining.erase(joining);
        }
        return arrived;
    }

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
                while (m_inbox.size() < MaxInboxEvents && m_inboxBytes < MaxInboxBytes && service(&event, 0) > 0) {
                    arrived = take(event) || arrived;
                }
                arrived = attend() || arrived;
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
        case ENET_EVENT_TYPE_CONNECT: {
            configurePeer(event.peer);
            const PeerId id = track(event.peer);
            m_dialling.erase(id.value);
            // Through one of this host's pipes: a joiner the relay carries.
            // Otherwise as it was dialled, or -- for one that dialled us --
            // by what its address says.
            if (const u16 slot = m_pipes.slotAt(event.peer->address); slot != 0) {
                m_pipes.attach(slot, id.value);
                m_paths[id.value] = PeerPath::Relayed;
            }
            else if (!m_paths.contains(id.value)) {
                m_paths[id.value] =
                    isLocalAddress(ENET_NET_TO_HOST_32(event.peer->address.host)) ? PeerPath::Lan : PeerPath::Direct;
            }
            // Every field named, empty ones included. Clang counts a skipped
            // designator as a missing initializer under `-Werror`, and the
            // Tier-2 build is where that is discovered.
            m_inbox.push_back(TransportEvent{.kind = TransportEvent::Kind::Connected,
                                             .peer = id,
                                             .payload = {},
                                             .channel = 0,
                                             .failure = ConnectFailure::None});
            return true;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const PeerId id = idOf(event.peer);
            // A join by code that found a path and no host at the end of it:
            // the path did not open in time.
            const bool never = m_dialling.erase(id.value) != 0;
            forget(event.peer);
            m_inbox.push_back(TransportEvent{.kind = TransportEvent::Kind::Disconnected,
                                             .peer = id,
                                             .payload = {},
                                             .channel = 0,
                                             .failure = never ? ConnectFailure::TimedOut : ConnectFailure::None});
            return true;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            TransportEvent message{.kind = TransportEvent::Kind::Message,
                                   .peer = idOf(event.peer),
                                   .payload = {},
                                   .channel = 0,
                                   .failure = ConnectFailure::None};
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
        m_paths.erase(id.value);
        m_dialling.erase(id.value);
        // A relayed joiner that is gone: its socket is closed and the relay
        // told to stop carrying it.
        if (const u16 slot = m_pipes.slotOf(id.value); slot != 0) {
            const u16 banSeconds = m_pipes.banOf(slot);
            m_pipes.close(slot);
            if (m_host != nullptr && m_hosting.active())
                m_hosting.close(slot, banSeconds, sender());
        }
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

    // Reaching a host behind a NAT (ADR 0178). All under `m_mutex`, the pipes'
    // own sockets apart, which have a lock and a thread of their own.
    rendezvous::HostRendezvous m_hosting;
    bool m_codeKnown = false;
    // The relay's name while it has not been found, when it is next looked
    // up, and the lookup in flight if there is one (D562).
    struct RelayLookup
    {
        enum : int
        {
            Looking,
            Found,
            NotFound
        };
        std::atomic<int> state{Looking};
        ENetAddress address{};
        rendezvous::Locals locals{};
    };
    std::string m_relayName;
    u64 m_relayLookupAtMs = 0;
    std::shared_ptr<RelayLookup> m_relayLookup;
    std::vector<Joining> m_joining;
    std::unordered_map<u32, PeerPath> m_paths;
    // Joins by code that found a path and are waiting for ENet's handshake
    // along it.
    std::unordered_set<u32> m_dialling;
    PipeSet m_pipes;
};

} // namespace

std::unique_ptr<ITransport> createEnetTransport()
{
    return std::make_unique<EnetTransport>();
}

} // namespace engine::net
