// What the files of this module that speak ENet share, and nothing outside
// it sees (R17): the transport and the relay's socket loop.
#pragma once

#include <enet/enet.h>
#include <string_view>

#include "engine/core/types.h"
#include "engine/net/rendezvous.h"

namespace engine::net::detail {

// ENet is initialised once a process and never deinitialised: see the note at
// its definition.
[[nodiscard]] bool ensureEnet();

// An ENet address keeps its host in network order; an `Endpoint` is `a.b.c.d`
// as `0xAABBCCDD`.
[[nodiscard]] inline rendezvous::Endpoint toEndpoint(const ENetAddress& address) noexcept
{
    return rendezvous::Endpoint{ENET_NET_TO_HOST_32(address.host), address.port};
}
[[nodiscard]] inline ENetAddress toAddress(rendezvous::Endpoint endpoint) noexcept
{
    ENetAddress address{};
    address.host = ENET_HOST_TO_NET_32(endpoint.address);
    address.port = endpoint.port;
    return address;
}

// `host` or `host:port`, a name looked up where it is one. False where it is
// nobody's.
[[nodiscard]] bool resolve(std::string_view text, core::u16 defaultPort, ENetAddress& out);

// **A peer that went away is not a broken socket** (NA5): Windows reports the
// "port unreachable" a vanished peer's machine answers with as an error on the
// next receive. Turned off, as every engine that runs UDP on Windows does.
void ignorePortUnreachable(ENetSocket socket) noexcept;

// Milliseconds on a clock that only goes forward, never zero.
[[nodiscard]] core::u64 steadyMs() noexcept;

} // namespace engine::net::detail
