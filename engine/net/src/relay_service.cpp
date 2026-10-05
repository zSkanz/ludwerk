#include "engine/net/relay_service.h"

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

#include "enet_internal.h"
#include "engine/core/i18n.h"

namespace engine::net {

namespace {

using core::I18nArg;
using core::u16;
using core::u64;
using core::u8;

// The largest datagram ENet makes, which is the largest a relay is handed.
constexpr core::usize LargestDatagram = ENET_PROTOCOL_MAXIMUM_MTU;
// What the system holds for the socket between two looks at it: a relay's
// datagrams come in bursts, a tick of every match at once.
constexpr int SocketBufferBytes = 4 * 1024 * 1024;
// How long the thread waits on a silent socket before it lets go of what has
// gone quiet.
constexpr enet_uint32 WaitMs = 50;
constexpr u64 TickEveryMs = 250;
// Datagrams taken in one look, so that a flood does not keep `stats` and
// `stop` waiting for the lock.
constexpr int MostPerLook = 4096;

} // namespace

struct RelayService::Impl
{
    ENetSocket socket = ENET_SOCKET_NULL;
    u16 port = 0;
    std::thread thread;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    rendezvous::RelayServer server;

    void run()
    {
        std::array<u8, LargestDatagram> bytes{};
        const rendezvous::Send send = [this](rendezvous::Endpoint to, std::span<const u8> datagram) {
            const ENetAddress address = detail::toAddress(to);
            ENetBuffer buffer{};
            buffer.data = const_cast<void*>(static_cast<const void*>(datagram.data()));
            buffer.dataLength = datagram.size();
            (void)enet_socket_send(socket, &address, &buffer, 1);
        };
        u64 tickedMs = detail::steadyMs();
        while (!stopping.load()) {
            enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
            (void)enet_socket_wait(socket, &condition, WaitMs);
            std::lock_guard lock(mutex);
            const u64 now = detail::steadyMs();
            for (int taken = 0; taken < MostPerLook; ++taken) {
                ENetAddress from{};
                ENetBuffer buffer{};
                buffer.data = bytes.data();
                buffer.dataLength = bytes.size();
                const int got = enet_socket_receive(socket, &from, &buffer, 1);
                if (got <= 0)
                    break;
                server.receive(detail::toEndpoint(from),
                               std::span<const u8>(bytes.data(), static_cast<core::usize>(got)), now, send);
            }
            if (now - tickedMs >= TickEveryMs) {
                tickedMs = now;
                server.tick(now);
            }
        }
    }
};

RelayService::RelayService() : m_impl(std::make_unique<Impl>())
{}

RelayService::~RelayService()
{
    stop();
}

std::optional<core::EngineError> RelayService::start(u16 port, const rendezvous::RelayLimits& limits)
{
    stop();
    if (!detail::ensureEnet())
        return core::makeError(ENG_TR("net.err.transport_init_failed"));
    Impl& impl = *m_impl;
    impl.socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    ENetAddress bound{};
    if (impl.socket == ENET_SOCKET_NULL || enet_socket_bind(impl.socket, &address) != 0 ||
        enet_socket_get_address(impl.socket, &bound) != 0) {
        if (impl.socket != ENET_SOCKET_NULL)
            enet_socket_destroy(impl.socket);
        impl.socket = ENET_SOCKET_NULL;
        const I18nArg args[] = {{"port", static_cast<core::i64>(port)}};
        return core::makeError(ENG_TR("net.err.transport_open_failed"), args);
    }
    (void)enet_socket_set_option(impl.socket, ENET_SOCKOPT_NONBLOCK, 1);
    (void)enet_socket_set_option(impl.socket, ENET_SOCKOPT_RCVBUF, SocketBufferBytes);
    (void)enet_socket_set_option(impl.socket, ENET_SOCKOPT_SNDBUF, SocketBufferBytes);
    detail::ignorePortUnreachable(impl.socket);
    impl.port = bound.port;
    impl.server = rendezvous::RelayServer(limits);
    impl.stopping.store(false);
    impl.thread = std::thread([&impl] { impl.run(); });
    return std::nullopt;
}

void RelayService::stop()
{
    Impl& impl = *m_impl;
    if (impl.thread.joinable()) {
        impl.stopping.store(true);
        impl.thread.join();
    }
    if (impl.socket != ENET_SOCKET_NULL)
        enet_socket_destroy(impl.socket);
    impl.socket = ENET_SOCKET_NULL;
    impl.port = 0;
}

bool RelayService::running() const noexcept
{
    return m_impl->thread.joinable();
}

u16 RelayService::port() const noexcept
{
    return m_impl->port;
}

rendezvous::RelayStats RelayService::stats() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->server.stats(detail::steadyMs());
}

} // namespace engine::net
