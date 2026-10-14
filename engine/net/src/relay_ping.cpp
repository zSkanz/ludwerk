#include "engine/net/relay_ping.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "enet_internal.h"
#include "engine/net/rendezvous.h"
#include "engine/net/transport.h"

namespace engine::net {

namespace {

using core::f64;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

// How long the thread sleeps on a silent socket between two looks at what is
// asked of it.
constexpr enet_uint32 WaitMs = 20;

struct Ask
{
    u32 id = 0;
    std::string relay;
    u32 timeoutMs = 0;

    // Filled by the thread.
    bool resolved = false;
    ENetAddress address{};
    u64 beganMs = 0;
    u32 sent = 0;
    u64 lastSentMs = 0;
    // Each ask's number and when it left, on a clock finer than a millisecond.
    std::array<u64, RelayPinger::Asks> nonces{};
    std::array<std::chrono::steady_clock::time_point, RelayPinger::Asks> left{};
    u32 answers = 0;
    RelayPingResult result;
};

} // namespace

struct RelayPinger::Impl
{
    mutable std::mutex mutex;
    std::vector<Ask> asked;
    std::vector<std::pair<u32, RelayPingResult>> done;
    usize outstanding = 0;
    u32 nextId = 1;
    std::thread thread;
    std::atomic<bool> stopping{false};
    ENetSocket socket = ENET_SOCKET_NULL;
    // Numbers no other ask of this process has: a relay's answer is matched
    // by its number alone.
    u64 nextNonce = 0x52656C6179506E67ull;

    void finish(Ask& ask)
    {
        std::lock_guard lock(mutex);
        done.emplace_back(ask.id, ask.result);
    }

    void run()
    {
        std::vector<Ask> active;
        std::array<u8, 512> bytes{};
        while (!stopping.load()) {
            {
                std::lock_guard lock(mutex);
                for (Ask& ask : asked)
                    active.push_back(std::move(ask));
                asked.clear();
            }
            // A name is looked up here, off every frame: it blocks for as
            // long as the system's resolver takes.
            for (Ask& ask : active) {
                if (ask.resolved)
                    continue;
                ask.resolved = true;
                ask.beganMs = detail::steadyMs();
                if (!detail::resolve(ask.relay, DefaultRelayPort, ask.address))
                    ask.timeoutMs = 0; // nobody's: given up on at once
            }
            const u64 now = detail::steadyMs();
            for (Ask& ask : active) {
                if (ask.timeoutMs == 0 || ask.sent >= RelayPinger::Asks)
                    continue;
                if (ask.sent != 0 && now - ask.lastSentMs < RelayPinger::AskEveryMs)
                    continue;
                const u64 nonce = nextNonce++;
                const std::vector<u8> datagram = rendezvous::encode(rendezvous::Ping{nonce});
                ENetBuffer buffer{};
                buffer.data = const_cast<void*>(static_cast<const void*>(datagram.data()));
                buffer.dataLength = datagram.size();
                ask.nonces[ask.sent] = nonce;
                ask.left[ask.sent] = std::chrono::steady_clock::now();
                (void)enet_socket_send(socket, &ask.address, &buffer, 1);
                ask.sent += 1;
                ask.lastSentMs = now;
            }

            enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
            (void)enet_socket_wait(socket, &condition, active.empty() ? 50u : WaitMs);
            while (true) {
                ENetAddress from{};
                ENetBuffer buffer{};
                buffer.data = bytes.data();
                buffer.dataLength = bytes.size();
                const int got = enet_socket_receive(socket, &from, &buffer, 1);
                if (got <= 0)
                    break;
                const auto arrived = std::chrono::steady_clock::now();
                rendezvous::Pong pong;
                if (!rendezvous::decode(std::span<const u8>(bytes.data(), static_cast<usize>(got)), pong))
                    continue;
                for (Ask& ask : active) {
                    for (u32 at = 0; at < ask.sent; ++at) {
                        if (ask.nonces[at] != pong.nonce || ask.nonces[at] == 0)
                            continue;
                        // Each number answered once: a datagram that came twice
                        // is not two answers.
                        ask.nonces[at] = 0;
                        const f64 took = std::chrono::duration<f64, std::milli>(arrived - ask.left[at]).count();
                        if (!ask.result.answered || took < ask.result.pingMs)
                            ask.result.pingMs = took;
                        ask.result.answered = true;
                        ask.result.matches = pong.sessions;
                        ask.result.relayed = pong.relayed;
                        ask.result.bytesPerSecond = pong.bytesPerSecond;
                        ask.result.uptimeSeconds = pong.uptimeSeconds;
                        ask.answers += 1;
                    }
                }
            }

            const u64 after = detail::steadyMs();
            std::erase_if(active, [&](Ask& ask) {
                const bool all = ask.answers >= RelayPinger::Asks;
                // One answer and the last ask long enough gone: the others are
                // lost, and the least of what came is the answer.
                const bool settled = ask.result.answered && ask.sent >= RelayPinger::Asks &&
                                     after - ask.lastSentMs >= RelayPinger::SettleMs;
                const bool late = after - ask.beganMs >= ask.timeoutMs;
                if (!all && !settled && !late)
                    return false;
                finish(ask);
                return true;
            });
        }
    }
};

RelayPinger::RelayPinger() : m_impl(std::make_unique<Impl>())
{}

RelayPinger::~RelayPinger()
{
    m_impl->stopping.store(true);
    if (m_impl->thread.joinable())
        m_impl->thread.join();
    if (m_impl->socket != ENET_SOCKET_NULL)
        enet_socket_destroy(m_impl->socket);
}

RelayPingTicket RelayPinger::submit(std::string_view relay, u32 timeoutMs)
{
    std::lock_guard lock(m_impl->mutex);
    if (m_impl->socket == ENET_SOCKET_NULL) {
        if (!detail::ensureEnet())
            return {};
        m_impl->socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (m_impl->socket == ENET_SOCKET_NULL)
            return {};
        ENetAddress any{};
        any.host = ENET_HOST_ANY;
        any.port = 0;
        if (enet_socket_bind(m_impl->socket, &any) != 0) {
            enet_socket_destroy(m_impl->socket);
            m_impl->socket = ENET_SOCKET_NULL;
            return {};
        }
        (void)enet_socket_set_option(m_impl->socket, ENET_SOCKOPT_NONBLOCK, 1);
        detail::ignorePortUnreachable(m_impl->socket);
        m_impl->thread = std::thread([impl = m_impl.get()] { impl->run(); });
    }
    Ask ask;
    ask.id = m_impl->nextId++;
    ask.relay = std::string(relay);
    // At least long enough for every ask to leave and one to come back.
    ask.timeoutMs = std::max<u32>(timeoutMs, 100u);
    m_impl->asked.push_back(std::move(ask));
    m_impl->outstanding += 1;
    return RelayPingTicket{m_impl->nextId - 1};
}

bool RelayPinger::take(RelayPingTicket ticket, RelayPingResult& out)
{
    std::lock_guard lock(m_impl->mutex);
    for (auto at = m_impl->done.begin(); at != m_impl->done.end(); ++at) {
        if (at->first != ticket.id)
            continue;
        out = at->second;
        m_impl->done.erase(at);
        m_impl->outstanding -= 1;
        return true;
    }
    return false;
}

usize RelayPinger::outstanding() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->outstanding;
}

} // namespace engine::net
