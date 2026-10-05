// The replication seam's vocabulary (ADR 0069, ADR 0070).
//
// **POD only, and no transport type reaches this header.** `IReplication` is
// what `app` holds and what `scene` learns one fact from, and neither may see an
// ENet handle for the reason R17 gives about Jolt: a backend type in a public
// header is a backend everything above it is compiled against.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::replication {

using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

// **The four postures, decided at process start from arguments and never
// changed** (ADR 0070). A script reads this and branches on it as a gameplay
// question -- "do I decide this" -- rather than as a configuration one.
//
// `Solo` is the default and its answers are the solo truth: authority true, one
// player, no peers. That is what makes `if NetworkService.Authority then` a
// branch that is present AND TAKEN in a game nobody networked.
enum class Topology : u8
{
    Solo = 0,
    // Client and server in one process. Authority true.
    Host = 1,
    // Headless, no client of its own. Authority true.
    Dedicated = 2,
    // A client of somebody else's authority. Authority false.
    Replica = 3,
};

[[nodiscard]] constexpr bool hasAuthority(Topology topology) noexcept
{
    return topology != Topology::Replica;
}

// A peer's identity, which is not an `InstanceId` and must not be confused with
// one.
//
// **Ids are never reused within a session.** A reconnecting player is a new
// peer with a new id; whether the GAME treats them as the same person is the
// game's question and it needs an account, which is the backend's business
// rather than the engine's.
struct PeerId
{
    u32 value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const PeerId& other) const noexcept { return value == other.value; }
};

// **A network id, and never a pointer and never a path.**
//
// A path is a string that goes stale the moment anything is renamed and is
// O(depth) to resolve on a replica that may not hold the ancestors -- which it
// often will not, because interest management gives it a subset by design.
struct NetId
{
    u32 value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const NetId& other) const noexcept { return value == other.value; }
};

// **Who a player is, across connections** (ADR 0085). A peer id is one
// connection and is never reused; this is the player. The authority makes one
// at a player's first welcome and gives it back the same `UserId` whenever it
// is presented again -- a replica that dropped and redialled, without the
// game losing track of whose score, inventory or seat it was.
//
// Opaque and unguessable rather than a counter, because it is a bearer claim:
// whoever presents it is that player. That is exactly as strong as the rest of
// an ENet session, which is a LAN or a trusted link (ADR 0012).
struct PlayerToken
{
    u64 high = 0;
    u64 low = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return high != 0 || low != 0; }
    [[nodiscard]] constexpr auto operator<=>(const PlayerToken&) const noexcept = default;
};

// What `app` hands the module at startup, from arguments and from nowhere else
// (ADR 0070).
struct Config
{
    Topology topology = Topology::Solo;
    // Where to listen, for `Host` and `Dedicated`. Ignored otherwise.
    u16 port = 0;
    // Where to dial, for `Replica`. Ignored otherwise.
    std::string address;
    // How many peers an authority accepts.
    //
    // **Validated against the transport's own cap rather than trusted.** ENet
    // refuses more than 4,095 and returns null from `enet_host_create`, which is
    // a startup failure that reads as "networking is broken" rather than as "you
    // asked for too many".
    u32 maxPeers = 32;
    // Snapshots per second. A COUNT of ticks between sends rather than a
    // millisecond interval, so it is a function of the simulation rather than of
    // the wall clock (R10).
    u32 ticksPerSnapshot = 2;
    // A replica: how many ticks behind the server's clock other players' parts
    // are drawn, so there is a snapshot on each side of the moment shown
    // (ADR 0076). Zero applies each snapshot as it arrives.
    u32 interpolationDelayTicks = 4;
    // A worse network than the one there is, for measuring (netcode ledger A):
    // see `net::TransportConfig::simulatedDelayMs`. Applied to a replica's
    // link, both ways.
    u32 simulatedDelayMs = 0;
    u32 simulatedJitterMs = 0;
    core::f32 simulatedLossPercent = 0.0f;
    // A replica says each correction of its predicted character in the log,
    // with what the authority disagreed about (`--net-log-corrections`).
    bool logCorrections = false;
    // How long the other end may go silent before it is gone, in
    // milliseconds: `[network] timeout` (D208). See `TransportConfig`.
    u32 timeoutMs = 10000;
    // A replica whose connection drops dials again (ADR 0085). False for a
    // join a script made (ADR 0106): the game hears `Disconnected` and decides
    // what next, rather than the engine deciding for it.
    bool redial = true;
    // A replica: **the player it was on this server before** (D207), presented
    // in the hello so a game that joins again after a drop is welcomed back as
    // the same `UserId` rather than as a stranger. Invalid for a first join.
    PlayerToken token;
    // **Reaching a host behind a NAT** (ADR 0178). An authority: the relay it
    // registers with (`host` or `host:port`), or none. A replica: the relay
    // that knows the host, with `joinCode` the host's code -- `address` and
    // `port` are then not read -- and whether a path each to the other is
    // tried before the relay carries the match.
    std::string relay;
    std::string joinCode;
    bool relayDirect = true;
};

// What a script can see, and every field is a `HostFact`: it describes the
// machine and the build rather than the world, so it is out of the world hash.
struct Status
{
    Topology topology = Topology::Solo;
    bool authority = true;
    // The authority's tick, which on a replica is the last one it applied and on
    // an authority is its own.
    u64 serverTick = 0;
    // Connected peers, not counting this process.
    u32 peerCount = 0;
    // The link as the transport measures it: on a replica, to its authority;
    // on an authority, to its worst peer. Zero where nothing is measured.
    u32 pingMs = 0;
    u32 jitterMs = 0;
    core::f32 loss = 0.0f;
    // A replica: whether the authority has welcomed it, and whether the
    // connection went and has not come back (ADR 0106).
    bool welcomed = false;
    bool lost = false;
    // A replica: why the authority refused it, or 0 (`Refused`, NA8) -- and
    // the game's own words, when it was removed and the host gave some.
    u8 refused = 0;
    std::string refusedText;
    // A replica: the player the authority welcomed it as, to present again on
    // the next join to the same server (D207). Invalid until a welcome.
    PlayerToken token;
    // A replica: how many welcomes made this machine SOMEBODY NEW -- the first,
    // and every one from an authority that did not know the token it was shown
    // (D432). A server that restarted knows nobody: its welcome is a fresh
    // join into a world that is not the one this machine held.
    u32 freshJoins = 0;
    // **Reaching a host behind a NAT** (ADR 0178), as numbers: no transport
    // type reaches this header. An authority: where it stands with its relay
    // (`net::RelayState`), and the code its joiners type, empty until the relay
    // has registered it. `path` is how the other end was reached
    // (`net::PeerPath`: 1 the same network, 2 across the internet, 3 through
    // the relay) -- on an authority, the longest way any of its peers came. A
    // replica that joined by a code and did not get in: why
    // (`net::ConnectFailure`), or 0.
    u8 relayState = 0;
    std::string joinCode;
    u8 path = 0;
    u8 joinFailure = 0;
};

// One past the largest `MessageType` there will be for a while: the kinds a
// message's bytes are counted under.
inline constexpr core::usize MessageKinds = 64;

// One tick's worth of what the module did, for the overlay and for a test that
// wants to assert a send happened rather than infer it.
struct Stats
{
    u64 snapshotsSent = 0;
    u64 snapshotsReceived = 0;
    u64 bytesSent = 0;
    u64 bytesReceived = 0;
    // **Those bytes by the kind of message they were in** -- a message's first
    // byte, `MessageType` -- sent and taken in together, since a kind travels
    // one way: what a game reads to see where its traffic goes
    // (`NetworkService:GetStats`), where before it had the total and had to
    // find the rest by turning things off.
    std::array<u64, MessageKinds> bytesByMessage{};
    // And summed as a game is told them: the world's state, attributes, its
    // reliable remotes, its unreliable ones, and a client's own input and
    // what it owns. The rest of the total is instances coming and going, the
    // ground and the handshake.
    u64 snapshotBytes = 0;
    u64 attributeBytes = 0;
    u64 remoteBytes = 0;
    u64 unreliableBytes = 0;
    u64 inputBytes = 0;
    // Instances that entered and left interest this tick, summed over peers.
    // A replica's own character, corrected by the authority because the
    // prediction had drifted more than a centimetre (ADR 0076).
    u64 corrections = 0;
    // Of those, the ones corrected by stepping the unanswered commands again
    // from where the authority put the character, rather than by the error.
    u64 replays = 0;
    // How far the last correction moved the own character, in metres.
    core::f64 lastCorrectionMetres = 0.0;
    // **The input buffer** (the multiplayer smoothness brief): on an
    // authority, the deepest peer's queue this tick and every tick any peer's
    // ran dry, summed; on a replica, its own, as the authority's snapshots say.
    u32 intentDepth = 0;
    u64 intentStarvations = 0;
    // How many times a peer's intent stream was anchored again because the
    // peer's clock had moved against this one's (D480).
    u64 intentReanchors = 0;
    // **What a replica simulates itself, and what stepping it again costs**
    // (ADR 0133): the loose parts it predicts now; the replays that stepped
    // its island again, and how many ticks they stepped in all; and the time
    // they took, in microseconds -- measured, never simulated with.
    u32 predictedBodies = 0;
    // A replica: how many ticks behind the server's clock others are drawn,
    // as the link has made it (NA14).
    u32 interpolationDelayTicks = 0;
    u64 resimulations = 0;
    u64 resimulatedTicks = 0;
    u64 resimulationMicros = 0;
    u32 spawned = 0;
    u32 despawned = 0;
    // `RemoteEvent` messages (ADR 0077): sent, taken in, and refused -- a flood
    // past the per-tick limit, or a message naming something that is not an
    // event.
    u64 messagesSent = 0;
    u64 messagesReceived = 0;
    u64 messagesDropped = 0;
    // `UnreliableRemoteEvent` messages (ADR 0161), counted apart: sent, taken
    // in, and what this machine refused or dropped itself -- an event the
    // peer has not got, a flood, a message older than the last. What the
    // network lost is not known to either end.
    u64 unreliableSent = 0;
    u64 unreliableReceived = 0;
    u64 unreliableDropped = 0;
    // A replicated `Swarm`'s agents (ADR 0162): the positions sent or taken
    // in, the bytes of them, and the agents that came and went.
    u64 swarmStates = 0;
    // How many an authority WANTED to send: past the budget, the rest waited.
    u64 swarmWanted = 0;
    u64 swarmBytes = 0;
    u64 swarmAdded = 0;
    u64 swarmRemoved = 0;
    // Messages the transport refused to send -- too large, its queue full, a
    // peer gone (NA1). Before, they were counted as sent.
    u64 sendFailures = 0;
    // Snapshots sent reliably in parts because they were too large to go
    // unreliable (NA1), and the acknowledgements refused because they named a
    // tick this peer was never sent.
    u64 snapshotsInParts = 0;
    u64 acksRefused = 0;
    // An owner's place for its part further than it could have moved, taken
    // only as far as it could (NA24).
    u64 ownedClamped = 0;
};

// **Where the own character is drawn, against where it is** (the multiplayer
// smoothness brief): a correction moves the simulation at once, and the
// drawing slides there over about a tenth of a second -- a pop is what a
// player sees. `offset` is added to the character's drawn position.
struct VisualCorrection
{
    core::InstanceId character;
    core::DVec3 offset{};
    // **Every move a correction made to the character's simulated place,
    // summed** (ADR 0134): what the drawing's history moves the character's
    // last place by, so it slides from where it was drawn rather than being
    // counted twice -- once by the offset, once by the step between ticks. A
    // teleport past `VisualSnapMetres` is not in it: that is drawn where it
    // lands.
    core::DVec3 displaced{};
};

} // namespace engine::replication
