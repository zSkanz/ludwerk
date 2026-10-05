// The transport seam (ADR 0012, architecture.md §2 `net_api`).
//
// **This interface is the deliverable; the implementation behind it is not.**
// v1 ships primitives and a loopback echo -- replication is post-v1 (R15) --
// so what M7 owes is a shape future replication can be built on without the
// engine being rebuilt around it. ADR 0012 names GameNetworkingSockets as the
// eventual default with ENet as the LAN option; the manifest row for GNS says
// in full why only ENet is vendored, and the short version is that GNS brings
// protobuf and OpenSSL for a seam with no v1 caller.
//
// **No ENet type appears here** (R17). A caller that can see an `ENetPeer`
// writes code that only works with one transport, which defeats the seam on the
// first line of the first use.
//
// What is deliberately NOT here, so that a later reader does not mistake it for
// an oversight: no replication, no entity ownership, no serialisation, no
// prediction, no clock synchronisation. This moves BYTES between two endpoints,
// with a reliability flag per message. Everything above that is a design nobody
// has approved yet, and a half-built version of it here would be worse than
// none.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/types.h"
#include "engine/net/rendezvous.h"

namespace engine::net {

using core::f32;
using core::u16;
using core::u32;
using core::u8;
using core::usize;

// Which guarantee a message wants, chosen per message rather than per channel.
//
// Per MESSAGE because the mix is the normal case: a position update wants to be
// dropped if it is late and a chat line does not, and they belong to the same
// conversation. A per-connection setting forces two connections for what is one
// relationship.
enum class Delivery : u8
{
    // Arrives, in order, or the connection fails. The expensive one.
    Reliable,

    // May be dropped and may arrive out of order. What a stream of positions
    // wants: a late position is worse than a missing one.
    Unreliable,

    // May be dropped; never delivered out of order. A dropped message is simply
    // superseded by the next. This is what most state replication actually
    // wants, and it is the option people reach for last because it has no name
    // in the sockets API everyone learned first.
    UnreliableSequenced,
};

// A stable handle to a connected endpoint. An integer rather than a pointer
// because a peer can vanish between two lines of caller code, and a stale
// integer is a lookup that fails while a stale pointer is undefined behaviour.
struct PeerId
{
    u32 value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(PeerId, PeerId) noexcept = default;
};

// **Reaching a host behind a NAT** (ADR 0178), in the transport's own words:
// whether a host's relay has it registered, how a peer was reached, and why a
// join by code did not happen.
using RelayState = rendezvous::RelayState;
using PeerPath = rendezvous::Path;
using ConnectFailure = rendezvous::JoinFailure;

// The port a relay listens on where nobody says another.
inline constexpr u16 DefaultRelayPort = 7789;

struct TransportEvent
{
    enum class Kind : u8
    {
        None,
        Connected,
        Disconnected,
        Message,
    };

    Kind kind = Kind::None;
    PeerId peer;

    // Owned by the event and only valid until the next `poll`. Copied by a
    // caller that wants it for longer, which is stated rather than left to be
    // discovered: the alternative is an allocation per datagram.
    std::vector<u8> payload;

    // The channel the message arrived on. Zero for connect and disconnect.
    u8 channel = 0;

    // On a `Disconnected` for a peer that was being joined by its code and
    // never connected: why. `None` for every other event.
    ConnectFailure failure = ConnectFailure::None;
};

struct TransportConfig
{
    // Zero binds nothing and makes this a client. A non-zero port listens.
    //
    // One type for both roles, deliberately: a peer-to-peer session is two
    // hosts that both listen, and a client/server split baked into the type
    // would make that unrepresentable in a seam whose whole job is to outlive
    // the topology chosen above it.
    u16 port = 0;

    // How many peers this host will hold at once. A hard limit rather than a
    // hint: the memory is allocated up front, which is the property that makes
    // a transport's footprint knowable. **At least one, and at most what the
    // transport can address** (`EnetPeerCap` for the one implementation): `open`
    // refuses anything else by name rather than failing as a broken network.
    usize maxPeers = 32;

    // Independent ordered streams. Two channels means a reliable chat message
    // cannot block a reliable state message behind it -- head-of-line blocking
    // is per channel, and having exactly one is how a transport ends up with a
    // stall nobody can explain.
    u8 channels = 2;

    // **How long a peer may go silent before it is gone** (D208), in
    // milliseconds. ENet's own default waits up to thirty seconds, and a player
    // whose server vanished watched a frozen world for all of them. Ten is long
    // enough for a phone changing networks and short enough to say so.
    u32 timeoutMs = 10000;

    // **The largest message this host takes, and so how much one peer may
    // hold half-reassembled** (audit N1's review). Zero keeps the transport's
    // own ceiling -- thirty-two megabytes a message and a peer for ENet, which
    // a replica needs for a large world's first snapshot and an authority
    // never does: what a client sends up is small, and sixty-four clients each
    // allowed thirty-two megabytes in flight is two gigabytes.
    usize maxMessageBytes = 0;

    // Connections one address may hold at once. Zero: no limit.
    usize maxPeersPerAddress = 0;

    // **Serviced by a thread of its own** (N9): acknowledgements and pings
    // answered the moment they arrive, whatever the frame loop is doing.
    // Off, the transport is serviced only inside `poll` -- what a test uses
    // to stand for a process that has stopped answering, which with the
    // thread a frozen game no longer is to the transport.
    bool serviceThread = true;

    // **A worse network than the one there is** (netcode ledger A): what a
    // connection this host makes goes through a link conditioner -- a relay
    // in this process, below the transport -- that holds each datagram, each
    // way, for `simulatedDelayMs` give or take `simulatedJitterMs`, and loses
    // `simulatedLossPercent` of them. Below ENet, so ENet's own round trip,
    // resends and timeouts see it as they would see the real thing. For
    // measuring and testing only; `engine-host --net-delay` refuses it in a
    // shipping build. Zero for all three: no relay.
    u32 simulatedDelayMs = 0;
    u32 simulatedJitterMs = 0;
    f32 simulatedLossPercent = 0.0f;
};

// **What a connection is like, as the transport measured it** (the multiplayer
// smoothness brief): the round trip, how much it varies, and the share of
// packets lost. Zero where the transport measures nothing -- the memory one.
struct PeerLink
{
    u32 roundTripMs = 0;
    u32 jitterMs = 0;
    f32 loss = 0.0f;
};

// **Poll-driven, never callback-driven**, and this is the R10 decision in the
// interface rather than a note about it. A callback fires whenever a packet
// arrives, which is a wall-clock-driven entry into game code and therefore a
// source of divergence between two runs of the same simulation. `poll` is
// called at a point the frame loop chooses, and everything it returns is
// applied at that point.
class ITransport
{
public:
    virtual ~ITransport() = default;

    // Binds if `config.port` is non-zero. An unbound host can still `connect`.
    [[nodiscard]] virtual std::optional<core::EngineError> open(const TransportConfig& config) = 0;
    virtual void close() = 0;

    // Begins a connection. The peer is NOT usable until a `Connected` event for
    // it comes out of `poll` -- returning a `PeerId` that is not yet connected
    // is deliberate, so a caller can key its own bookkeeping before the
    // handshake finishes rather than after.
    [[nodiscard]] virtual std::optional<core::EngineError> connect(std::string_view host, u16 port,
                                                                   PeerId& outPeer) = 0;
    virtual void disconnect(PeerId peer) = 0;
    // **Lets a peer go at once** (NA29), freeing its slot without the round
    // trip a disconnect waits for: a connection that never said who it was
    // held a slot for ten seconds, and thirty-two of them filled a server.
    virtual void drop(PeerId peer) { disconnect(peer); }

    [[nodiscard]] virtual std::optional<core::EngineError> send(PeerId peer, std::span<const u8> payload,
                                                                Delivery delivery, u8 channel) = 0;

    // **Puts what was sent on the wire now** (N9): called after a tick's
    // sends, so they leave together and at once rather than when the
    // transport next services itself. Nothing to do for a transport that
    // sends as it is asked.
    virtual void flush() {}

    // Drains up to `timeoutMs` of waiting events into `out`, appending. Returns
    // with whatever it has when the time is spent; zero means "whatever is
    // ready right now".
    [[nodiscard]] virtual std::optional<core::EngineError> poll(std::vector<TransportEvent>& out, u32 timeoutMs) = 0;

    [[nodiscard]] virtual usize peerCount() const noexcept = 0;

    // How the link to `peer` is doing; nothing measured by default.
    [[nodiscard]] virtual PeerLink link(PeerId peer) const noexcept
    {
        (void)peer;
        return {};
    }

    // --- Reaching a host behind a NAT (ADR 0178) -----------------------------
    //
    // A transport that has no such thing says so by the defaults below: no
    // relay, no code, and a join by code that is an error.

    // **A listening host registers with a relay** (`host` or `host:port`), and
    // keeps registered: its `joinCode` is what a joiner anywhere types. The
    // host goes on taking connections by address whether or not the relay
    // ever answers.
    [[nodiscard]] virtual std::optional<core::EngineError> useRelay(std::string_view relay);
    virtual void leaveRelay() {}
    [[nodiscard]] virtual RelayState relayState() const noexcept { return RelayState::None; }
    // Empty until the relay has registered this host.
    [[nodiscard]] virtual std::string joinCode() const { return {}; }
    // What the relay tells a joiner before it tries: a host with no room is
    // not knocked on.
    virtual void setOccupancy(usize players, usize maxPlayers)
    {
        (void)players;
        (void)maxPlayers;
    }

    // **Begins a connection to the host a code names**, by the best path
    // there is: its own network, across the internet each to the other, or
    // through the relay -- `direct` off goes straight to the relay. As with
    // `connect`, the peer is usable from its `Connected`; a join that does not
    // happen is a `Disconnected` whose `failure` says why.
    [[nodiscard]] virtual std::optional<core::EngineError> connectByCode(std::string_view relay, std::string_view code,
                                                                         bool direct, PeerId& outPeer);
    // How `peer` was reached. `None` for a peer this transport does not know.
    [[nodiscard]] virtual PeerPath path(PeerId peer) const noexcept
    {
        (void)peer;
        return PeerPath::None;
    }
    // **Keeps a peer's address away for a while**, where the transport is what
    // stands between it and the host: a relayed joiner's address is the
    // relay's to refuse. Nothing for a peer that came by itself -- its address
    // is the host's own to refuse.
    virtual void ban(PeerId peer, u32 seconds)
    {
        (void)peer;
        (void)seconds;
    }
};

// The one implementation (ADR 0012). Named as a factory rather than a class so
// that no header outside `net_enet` ever sees an ENet type.
//
// Its properties are worth stating where a caller will read them: UDP, optional
// reliability and sequencing, **no encryption and no authentication**. An ENet
// channel is a LAN or an otherwise trusted link. Anything else waits for the
// GameNetworkingSockets row in the manifest to be filled in.
[[nodiscard]] std::unique_ptr<ITransport> createEnetTransport();

// **The most peers `createEnetTransport` can hold**: its protocol addresses a
// peer in twelve bits. Asked for more, ENet's host constructor answers null,
// which read as "the port is in use" until `open` checked this first. A number
// rather than an ENet constant, so no header outside `net_enet` sees ENet; the
// implementation asserts the two agree.
inline constexpr usize EnetPeerCap = 4095;

} // namespace engine::net
