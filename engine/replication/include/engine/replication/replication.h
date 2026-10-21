// The replication seam (ADR 0069).
//
// **Null is a real state and not an error**, which is the pattern the animation
// host and the physics mirror both state about themselves. A solo game has no
// replication object at all: `app` holds a `unique_ptr` that is null, calls
// nothing, and pays one branch a tick. That is what makes
// `ENG_ENABLE_REPLICATION=OFF` a build with no socket code in it rather than a
// build with socket code nobody reaches (ADR 0070, clause 3).
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/id.h"
#include "engine/replication/types.h"

namespace engine::core {
struct EngineError;
}

namespace engine::net {
class ITransport;
}

namespace engine::scene {
class ICharacterReplay;
class World;
} // namespace engine::scene

namespace engine::replication {

class ScriptTemplates;

// What the authority and the replica both are.
//
// **Two calls a tick and they are not symmetric.** `receive` runs before the
// simulation, because what arrived is input to the tick that follows it, and
// `send` runs after, because what is sent is the tick's result. Reversing them
// costs a tick of latency in each direction and looks like nothing at all in a
// loopback test.
class IReplication
{
public:
    virtual ~IReplication() = default;

    // Applies everything that arrived since the last call.
    //
    // On an authority that is intent; on a replica it is spawns, despawns and
    // snapshots. **The world is mutated here and nowhere else in this class**,
    // which is what lets `send` take a const world.
    //
    // `root` is what is replicated -- the `Workspace` -- passed rather than
    // looked up, so the module never has to know how a world is arranged above
    // the part of it that travels.
    //
    // **`ticking`: whether a simulation tick follows this call.** A frame that
    // runs no tick still services the connection, and what happens once a
    // tick -- an authority applying each peer's next intent, a replica's
    // clock and interpolation -- must not happen then too: at a thousand
    // frames a second a server consumed input a thousand times a second, and
    // a replica drawn at 144 Hz ran its clock at more than twice the tick
    // rate and was snapped back at every snapshot.
    virtual void receive(scene::World& world, core::InstanceId root, bool ticking = true) = 0;

    // Extracts, diffs and sends this tick's state.
    //
    // `tick` is the simulation's own count and never a clock: what a peer is
    // told is a function of the operation sequence (R10), and a send rate
    // measured in milliseconds would make the bytes on the wire depend on how
    // fast the machine was.
    virtual void send(const scene::World& world, core::InstanceId root, u64 tick) = 0;

    // **A snapshot held while a server catches up** (ADR 0228 §7): the one
    // exception to the count of ticks above, and the frame loop's to decide.
    // A frame that runs several ticks to catch up sends at most one snapshot
    // per snapshot interval of real time, rather than one every
    // `ticksPerSnapshot` ticks run back to back; a snapshot due while held is
    // owed, and goes with the next tick that is not. What a held snapshot
    // changes is when a peer is told, never what: the next one is against the
    // same acknowledged baseline and carries everything since.
    virtual void holdSnapshots(bool held) { (void)held; }
    // Whether the last `send` sent the world's state.
    [[nodiscard]] virtual bool snapshotSent() const { return false; }

    // Sends the `RemoteEvent` messages scripts queued this tick, and takes them
    // out of the world's outbox (ADR 0077). Right after `send`, so a message
    // naming an event the same send spawned is behind its spawn on the wire.
    virtual void sendMessages(scene::World& world) = 0;

    [[nodiscard]] virtual Status status() const = 0;
    // **How many peers an authority seats from now on** (ADR 0167), never
    // more than it opened its transport for: one already seated stays. And
    // the removal of the peer that is player `userId`, told `reason`. A
    // replica does neither.
    virtual void setMaxPeers(u32 peers) { (void)peers; }
    virtual bool removePlayer(scene::World& world, core::InstanceId root, u32 userId, std::string_view reason)
    {
        (void)world;
        (void)root;
        (void)userId;
        (void)reason;
        return false;
    }
    // **Who may join** (`AuthoritySession::setJoinGate`, ADRs 0218, 0226), the
    // answers to it, and the players an operator lists. A replica has none.
    virtual void setJoinGate(std::function<void(const JoinAsk&)> gate) { (void)gate; }
    virtual void answerJoin(u32 ask, JoinAnswer answer)
    {
        (void)ask;
        (void)answer;
    }
    [[nodiscard]] virtual std::vector<PeerSummary> players() const { return {}; }
    // **Each player's round trip, in milliseconds** (`Player:GetNetworkPing`):
    // on an authority every welcomed player's, a machine's guests sharing its
    // link. A replica has none of its own to say here.
    [[nodiscard]] virtual std::vector<std::pair<u32, u32>> playerPings() const { return {}; }
    // **Said to every player** (ADR 0218, `Announce`): an authority's
    // operator's words. A replica says nothing.
    virtual void announce(std::string_view text) { (void)text; }
    // **Player `userId` sent elsewhere** (`Redirect`, ADR 0222): false where
    // nobody is them, and on a replica.
    virtual bool redirect(u32 userId, std::string_view address, u16 port, std::string_view pass, std::string_view key,
                          std::string_view world)
    {
        (void)userId;
        (void)address;
        (void)port;
        (void)pass;
        (void)key;
        (void)world;
        return false;
    }
    [[nodiscard]] virtual Stats stats() const = 0;

    // **This machine dropped simulated time** -- a frame owed more ticks than
    // it may catch up, and the rest were not stepped. A replica tells
    // its authority with the intents it sends after, so the authority knows
    // its tick numbers fell behind because its clock moved, not because its
    // packets were slow. An authority has nothing to tell.
    virtual void noteTimeDropped() {}

    // **Voice** (ADR 0256): this machine's player speaks a frame, which the
    // authority sends on to who hears them; the frames this machine is to
    // play; and the game's answer to who hears whom, on the authority.
    virtual void speak(scene::World& world, u8 codec, u16 sequence, bool speaking, std::span<const u8> frame)
    {
        (void)world;
        (void)codec;
        (void)sequence;
        (void)speaking;
        (void)frame;
    }
    virtual void takeVoice(std::vector<HeardVoice>& out) { (void)out; }
    virtual void setVoiceFilter(std::function<bool(core::InstanceId listener, core::InstanceId speaker)> filter)
    {
        (void)filter;
    }
    virtual void setVoiceCodec(u8 codec) { (void)codec; }

    // **Losing interest is streaming out** (ADR 0069 decision 6). On a replica,
    // an instance the authority stopped sending is kept as a husk, reparented to
    // nil, when `probe` says a script holds it, and destroyed when not. The
    // husks are drained here for the host to fire `InstanceStreamedOut` for,
    // exactly as it does for an evicted chunk. An authority has none.
    virtual void setReferenceProbe(std::function<bool(core::InstanceId)> probe) = 0;

    // How a replica steps its own character again when the authority corrects
    // it (`ReplicaSession::setCharacterReplay`). Ignored by an authority.
    virtual void setCharacterReplay(scene::ICharacterReplay* replay) = 0;

    // **How a replica follows the authority to another scene** (ADR 0106):
    // called, during `receive`, with the scene's content-relative path and its
    // load data, before the new scene's instances are spawned. The host loads
    // the scene from its own package; the authority never calls it.
    using SceneChanger = std::function<void(scene::World&, const std::string&, std::vector<core::u8>)>;
    virtual void setSceneChanger(SceneChanger changer) = 0;

    // **Called when the authority first takes a replica**, before
    // anything it sends is applied: where a host makes its world the server's.
    // Ignored by an authority.
    virtual void setWelcomeHandler(std::function<void(scene::World&)> handler) = 0;

    // **Where a replica keeps its own scripts** (ADR 0138 §6), asked at each
    // spawn (`ReplicaSession::setScriptTemplates`). Ignored by an authority.
    virtual void setScriptTemplates(std::function<ScriptTemplates*()> templates) = 0;
    [[nodiscard]] virtual std::vector<core::InstanceId> drainStreamedOut() = 0;

    // A replica's own character's drawn offset (`VisualCorrection`); nothing
    // on an authority.
    // By seat (ADR 0208): 0 the machine's first player, 1 to 3 its guests.
    [[nodiscard]] virtual VisualCorrection visualCorrection(core::usize seat = 0) const
    {
        (void)seat;
        return {};
    }

    // Closes every connection and stops listening. Idempotent.
    virtual void shutdown() = 0;
};

// Builds one, or says why not.
//
// **This is the single caller of anything that binds a socket** (ADR 0070,
// clause 2), and `app` reaches it from argument parsing. There is no other path:
// no service method, no property, no script. The rule is checkable rather than
// promised precisely because this function is the only door.
//
// Returns null with no error for `Topology::Solo` -- a solo game is not a
// failure to network, it is a game that is not networked.
[[nodiscard]] std::unique_ptr<IReplication> createReplication(const Config& config,
                                                              std::optional<core::EngineError>& error);

// The same, over a transport the caller made -- the memory transport in a test
// and in the two-worlds gate, where what arrives must be a function of the
// operation sequence rather than of a socket. `createReplication` is this over
// ENet, and it stays the only caller that reaches a real one.
[[nodiscard]] std::unique_ptr<IReplication> createReplicationOver(std::unique_ptr<net::ITransport> transport,
                                                                  const Config& config,
                                                                  std::optional<core::EngineError>& error);

} // namespace engine::replication
