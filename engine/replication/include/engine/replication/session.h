// The two ends of a replicated world, over any transport (ADR 0069).
//
// **The model is the one every shipped action game converges on**: the
// authority keeps a short history of what the world looked like at each send,
// each replica acknowledges the newest state it has fully reconstructed, and
// every snapshot is a diff against the state that peer last acknowledged. A
// lost snapshot costs nothing but bandwidth -- the next one is diffed against
// the same baseline and carries everything the lost one did -- and a replica can
// never apply a diff against a state it does not hold, because it only ever
// acknowledges states it does.
//
// **Spawns and despawns are reliable; snapshots are not.** An instance entering
// or leaving is a fact that cannot be superseded, so it travels on the control
// channel. Its fields travel in the snapshot like everybody else's: an instance
// the baseline does not have is sent whole, which is the one decoding path for
// "new" and "changed" rather than two.
//
// **Checked, and honestly.** Each snapshot carries a checksum of the state it
// describes; the replica reconstructs, compares, and only acknowledges a match.
// That catches an apply bug -- a decoder and an encoder disagreeing -- and
// nothing else: it is not a comparison of two worlds, and a replica's world is
// a subset of the authority's by design (ADR 0069, decision 8).
//
// Both classes are driven from outside, one call per tick each way, and hold no
// thread and no clock: what a peer receives is a function of the calls made and
// the transport's delivery, and with the memory transport that is a function of
// the operation sequence alone (R10).
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/name_atom.h"
#include "engine/net/transport.h"
#include "engine/replication/extract.h"
#include "engine/replication/types.h"
#include "engine/scene/character_replay.h"
#include "engine/scene/components.h"

namespace engine::scene {
class World;
}

namespace engine::replication {

// The root of what is replicated -- the `Workspace` in a game, anything in a
// test. Both ends agree on it without saying so, which is what lets a replica
// mount the authority's world under its own.
inline constexpr NetId RootNetId{1};

// How many sent states the authority remembers, and how many received states a
// replica does. A peer whose acknowledgement is older than this is sent the
// whole world again, which is correct and expensive -- and at the default
// thirty snapshots a second it means a peer that has not acknowledged anything
// for two seconds, which is a peer about to time out anyway.
inline constexpr usize StateHistory = 64;

// `RemoteEvent` limits (ADR 0077). A client is not trusted: an authority takes
// this many messages a tick from one peer and drops the rest. A message waits
// this many sends for its event to reach the network before it is dropped.
inline constexpr u32 MaxRemoteMessagesPerTick = 256;
// **A client's messages are budgeted over time** (NA22, NA23): what it has in
// hand, and what it gains a tick -- remote calls, clicks and drags together,
// and their bytes. A server frame of 200 ms packs twelve of a client's ticks
// into one of its own, and a per-tick limit dropped what a game had sent
// legitimately; a burst of a thousand is read whole. A peer that keeps past
// its budget for `FloodTicks` ticks in a row is let go: limits nobody pays
// for are only a slower flood.
inline constexpr u32 RemoteMessageBurst = 1024;
inline constexpr u32 RemoteMessagesPerTick = 64;
inline constexpr usize RemoteByteBurst = 1024u * 1024u;
inline constexpr usize RemoteBytesPerTick = 64u * 1024u;
inline constexpr u32 FloodTicks = 300;
// **How far an owner may move what it owns in a tick** (NA24): two metres,
// 120 m/s. A drag, a throw, a vehicle -- all under it; a part put across the
// map by a client that holds it is refused, and the authority's place stands.
inline constexpr core::f64 MaxOwnedMetresPerTick = 2.0;
inline constexpr u16 MaxRemoteHeldSends = 300;
// The script module's own payload ceiling, plus the few bytes of its header.
inline constexpr usize MaxRemoteWirePayload = 64u * 1024u + 16u;

// **What one peer may cost an authority in one tick** (audit N1's review). A
// client is not trusted to send one intent and one owned state a tick, or a
// sane count in either; what is over these is dropped, and a count over them
// is a refusal of the whole message. The byte budget is what `RemoteEvent`
// payloads may add up to, on top of the message count above.
inline constexpr u32 MaxIntentsPerTick = 8;
// **Intent messages are budgeted over time, not by tick** (NA3): a peer has
// `MaxIntentBurst` in hand and gains `IntentBudgetPerTick` a tick. A replica
// sends one a tick, so its budget is always full; a burst after a hitch -- a
// server frame of 200 ms packs twelve of a client's ticks into one of its own --
// is all read, where eight a tick dropped the newest four; and a peer flooding
// is held to two a tick for as long as it floods.
inline constexpr u32 MaxIntentBurst = 32;
inline constexpr u32 IntentBudgetPerTick = 2;
inline constexpr u16 MaxIntentEntries = 256;
// **An action's name crosses once a connection** (G38, protocol 37): an
// `IntentNames` message gives it a number, and every intent after carries the
// number. As many names as a connection may give, how long one may be, and how
// many entries may wait for a name that has not come yet.
inline constexpr u16 MaxIntentNames = 1024;
inline constexpr usize MaxIntentNameBytes = 255;
inline constexpr usize MaxUnnamedIntents = 256;
// The most predicted attributes a snapshot carries for a replica's own
// character (G37).
inline constexpr u16 MaxPredictedAttributes = 64;

// **Intents, buffered and applied one a tick** (the multiplayer smoothness
// brief, protocol 22). An `Intent` message carries the newest tick and up to
// three before it, so one lost message loses no input; the authority queues
// them by tick and applies exactly one a tick, in order, a few ticks behind
// the newest so arrival jitter never leaves it without one.
inline constexpr u8 IntentRedundancy = 4;
// Where the delay starts, and the bounds it adapts between: a tick more for
// every tick the queue ran dry, a tick less after `IntentDelayRelaxTicks`
// without one -- changed only while the player is idle, so the change itself
// moves nothing.
inline constexpr u32 InitialIntentDelay = 3;
inline constexpr u32 MaxIntentDelay = 6;
inline constexpr u32 IntentDelayRelaxTicks = 600;
// Queued past the delay by more than this, the queue is caught up at once: a
// replica that stalled and then sent a burst is not replayed a second late.
inline constexpr u32 IntentCatchUp = 8;
inline constexpr usize MaxQueuedIntents = 64;
// **How long the last intent stands in while the peer says nothing** (D480):
// half a second of ticks in which no intent message came at all. Counted by
// silence, not by ticks without a real intent: a client's long frame is
// silence of its own length -- the owner's logs have frames of 400 ms -- and a
// key held through it is still held, where a quarter of a second released it
// in every such frame and corrected the replica each time. Longer than that,
// it is a player nobody is holding a key for, and its character stops.
inline constexpr u32 StandInLifetimeTicks = 30;

// The visual slide of a correction: what is left of it after each tick, and
// the distance past which a correction is a teleport and drawn as one.
inline constexpr core::f64 VisualDecayPerTick = 0.55;

// **What a replica predicts besides its character** (ADR 0133): the loose
// parts within this many metres of it, nearest first, up to a count; one that
// has been out of range this many ticks goes back to being drawn between
// snapshots. `[network] predict_radius`, `predict_max_bodies` and
// `predict_linger_ticks` set them for a project.
inline constexpr core::f64 DefaultPredictRadius = 8.0;
inline constexpr u32 DefaultPredictMaxBodies = 32;
inline constexpr u32 DefaultPredictLingerTicks = 30;
inline constexpr core::f64 VisualSnapMetres = 2.0;
// **A replica's island disagreeing by this much is stepped again** from the
// authority's word, without counting a correction (ADR 0133). Measured: from
// the same state the two machines step it to the bit, so any difference is a
// real one -- and a fifth of a millimetre left alone was a centimetre after
// the next time two crates met.
inline constexpr core::f64 ResyncMetres = 0.00001;
// A loose part this close to a predicted one -- past their bounding spheres --
// is predicted too, one step out from the radius.
inline constexpr core::f64 PredictTouchMetres = 0.25;
inline constexpr u32 MaxOwnedStatesPerTick = 2;
inline constexpr u16 MaxOwnedRecords = 4096;
inline constexpr u16 MaxRemoteRefs = 1024;
inline constexpr usize MaxRemoteBytesPerTick = 1024u * 1024u;
// Messages held for a peer until it knows the event they name, whatever their
// age: past either, the oldest is dropped.
inline constexpr usize MaxHeldMessages = 4096;
inline constexpr usize MaxHeldBytes = 16u * 1024u * 1024u;
// **A connection that never says hello is let go**, in authority receives --
// ten seconds at sixty. A handshake is one reliable round trip.
inline constexpr u32 MaxUnwelcomedReceives = 180;
// Why a replica was refused (`Refused`, NA8).
inline constexpr u8 RefusedVersion = 1;
inline constexpr u8 RefusedFull = 2;
// Players this authority remembers across connections (ADR 0085). Past it,
// the longest-known one not connected now is forgotten: its next visit is a
// new player, which is what an authority that restarted would say too.
inline constexpr usize MaxKnownIdentities = 65536;
// The largest message an authority accepts from a peer: an owned state of
// `MaxOwnedRecords` parts, a `RemoteEvent` at its ceiling, with room.
inline constexpr usize MaxAuthorityMessageBytes = 1024u * 1024u;
// **A snapshot too large to go as one unreliable message** (NA1): past this
// it is sent reliably, in parts of `SnapshotPartBytes`, and nothing more is
// sent to that peer until it is acknowledged. An unreliable message ENet
// fragments is lost whole when one fragment is, and a world's first snapshot --
// every record whole, no baseline -- was resent every other tick until one got
// through: 29 seconds to join a world of 1 000 parts on this machine's own
// loopback, and never at 5 000.
inline constexpr usize ReliableSnapshotBytes = 16u * 1024u;
// **How long a connected peer may say nothing before it is gone**, in ticks:
// ten seconds, the transport's own default. The transport is serviced by a
// thread of its own (N9), so a process whose game froze still answers it --
// and a server stuck in a script, or a client whose window hung, was a
// connection that never ended. A replica hears a snapshot every few ticks and
// an authority an intent every tick, so silence this long is a peer that
// stopped.
inline constexpr u32 SilentPeerTicks = 600;
inline constexpr usize SnapshotPartBytes = 32u * 1024u;
// The most a replica will join from snapshot parts: a world of some hundred
// thousand parts, and not an authority's way to fill a player's memory.
inline constexpr usize MaxReplicaSnapshotBytes = 64u * 1024u * 1024u;
// **A move this far in a tick is a teleport, not a motion** (NA15): drawn as a
// step at its tick rather than a slide across the map. Two metres a tick is
// 120 m/s -- faster than anything a game walks, drives or flies on purpose.
inline constexpr core::f64 TeleportMetresPerTick = 2.0;
// The most the adaptive interpolation delay adds over what it was set to.
inline constexpr u32 MaxAddedInterpolationDelay = 20;
// Connections one address may hold on an authority: a household behind one
// router is a few players, and a flood from one machine is not a crowd.
inline constexpr usize MaxPeersPerAddress = 8;

// **What an authority may cost a replica** -- a server a player joined is not
// trusted with that player's machine either. A message naming more than these
// is refused whole.
inline constexpr u32 MaxReplicaRemotesPerTick = 4096;
inline constexpr usize MaxReplicaNames = 1u << 20;
inline constexpr usize MaxReplicaInstances = 1u << 20;
inline constexpr u32 MaxRosterPlayers = 4096;
// How long, in applied ticks, a departed id is still filtered out of a
// snapshot: longer than any baseline a state history can hold.
inline constexpr u64 DepartedMemoryTicks = 600;

// Where the services whose properties travel are numbered from: one fixed id
// per wire class, far above any instance's, so a service is never mistaken for
// something to spawn.
inline constexpr u32 ServiceNetIdBase = 0x7F000000u;

// Prediction and interpolation (ADR 0076). Two snapshot intervals of delay at
// the default rate -- the smallest that nearly always has a sample on each
// side of the moment drawn -- and enough history for a two-second round trip.
inline constexpr u32 DefaultInterpolationDelay = 4;
inline constexpr usize InterpolationSamples = 6;
inline constexpr usize PredictionHistory = 128;

// One instance as the wire sees it: its network id, which of the schema's
// classes describes it, and its fields in schema order. Values are in the
// AUTHORITY'S terms -- its name atoms, its network ids -- on both ends.
struct EntityState
{
    NetId id;
    u8 schema = 0;
    FieldSet fields;
};

// A world at one tick, entities sorted by network id.
struct WorldState
{
    u64 tick = 0;
    std::vector<EntityState> entities;
};

// What a snapshot carries so a replica can prove it reconstructed the state the
// authority meant. FNV-1a over ids, schema indices and field bytes: a checksum,
// not a signature -- it is there to catch our own bugs.
[[nodiscard]] u64 checksumOf(const WorldState& state) noexcept;

class AuthoritySession
{
public:
    // **How many players this authority takes** (NA8). Zero: as many as the
    // transport holds. Past it a replica is told `Refused` and let go -- the
    // transport keeps one slot over so that there is a connection to say it on.
    void setMaxPlayers(u32 players) noexcept { m_maxPlayers = players; }

    explicit AuthoritySession(net::ITransport& transport) noexcept : m_transport(transport) {}

    // Handshakes, acknowledgements, departures and intent. A welcomed peer
    // becomes a `Player` under the world's `NetworkService` and a departed one
    // stops being one, so this is where the authority's world gains and loses
    // players -- and where what they did lands, before the tick that reads it.
    // `root`'s parent is the data model the players are found under.
    void receive(scene::World& world, core::InstanceId root, bool ticking = true);

    // Captures `root`'s subtree as it stands at `tick` and sends every
    // welcomed peer what it lacks: spawns, despawns, then the snapshot.
    void send(const scene::World& world, core::InstanceId root, u64 tick);

    // The world's outbox of `RemoteEvent` messages, to the players they name.
    // A peer that has not yet been told the event exists keeps its copy until
    // it has, so a message never arrives before the spawn it names.
    void sendMessages(scene::World& world);

    [[nodiscard]] Stats stats() const noexcept { return m_stats; }
    [[nodiscard]] u32 peerCount() const noexcept;
    // The worst of the welcomed peers' links: what an authority's overlay shows.
    [[nodiscard]] net::PeerLink worstLink() const noexcept;
    // The network id an instance travels under, or an invalid id when it has
    // never been captured. For tests and for `Player` mapping.
    [[nodiscard]] NetId netIdOf(core::InstanceId id) const noexcept;

private:
    // What one peer held at one tick: the ids its snapshot then carried.
    struct PeerInterest
    {
        u64 tick = 0;
        std::vector<u32> ids;
    };

    struct Peer
    {
        net::PeerId id;
        bool welcomed = false;
        // The newest state this peer has proved it holds. Zero: none yet.
        u64 acked = 0;
        // The snapshot ticks this peer was sent, newest last: an
        // acknowledgement names one of them or is not taken (NA1) -- a
        // client acknowledging a tick it was never sent would have bought
        // full snapshots for ever.
        std::deque<u64> sentTicks;
        // **The last snapshot sent reliably, in parts** (NA1), with what this
        // peer held then: kept here because the history may have moved past
        // it by the time the peer has it all. While `pinnedPending`, nothing
        // more is sent to the peer -- each snapshot after would be as large.
        std::shared_ptr<const WorldState> pinnedState;
        std::vector<u32> pinnedHeld;
        u64 pinnedTick = 0;
        bool pinnedPending = false;
        // Network ids this peer has been told exist, sorted.
        std::vector<u32> known;
        // Its player, and the newest intent applied from it -- older ones
        // arriving late are dropped, because what a player did last tick is
        // superseded by what they did this one.
        u32 userId = 0;
        // Who that player is, across connections (ADR 0085).
        PlayerToken token;
        core::InstanceId player;
        // **The intent tick whose step the world now holds** -- what a
        // snapshot acknowledges, so the replica compares the position with the
        // prediction that produced it (the multiplayer smoothness brief). Zero
        // until the first of the replica's own is applied.
        u64 intentTick = 0;
        // Received and not yet applied, by tick; the last applied, which
        // stands in for a tick that has not arrived; and the delay the queue
        // is kept at.
        std::map<u64, std::vector<scene::PlayerIntent>> intentQueue;
        std::vector<scene::PlayerIntent> lastIntents;
        bool intentStarted = false;
        u64 appliedTick = 0;
        u64 firstIntentTick = 0;
        u32 intentDelay = InitialIntentDelay;
        u32 ticksSinceStarved = 0;
        u64 starvations = 0;
        // **What stood in for a tick, and the presses it missed** (ADR 0133's
        // measurement): a jump held for one tick whose intent came after the
        // tick was stood in for was never applied at all. A press the stand-in
        // lacked is carried into the next tick instead -- late, not lost.
        std::map<u64, std::vector<scene::PlayerIntent>> standIns;
        std::vector<core::NameAtom> carriedPresses;
        // **The names behind its intent numbers** (G38, protocol 37), as its
        // `IntentNames` gave them -- empty for a number not given -- and the
        // atom each is here, once anything here has named it.
        std::vector<std::string> intentNames;
        std::vector<core::NameAtom> intentAtoms;
        // Entries whose number named nothing yet: an intent that overtook its
        // name, which travels on another channel. Taken when the name comes.
        struct UnnamedIntent
        {
            u64 tick = 0;
            u16 id = 0;
            scene::PlayerIntent intent;
        };
        std::vector<UnnamedIntent> unnamedIntents;
        // The newest tick queued when the delay last held a tick to grow.
        u64 pausedAtNewest = 0;
        // **A peer whose clock moved against this one's** (D480). How many
        // ticks in a row brought intents that were all after their ticks, and
        // whether this tick is counted; how many ticks in a row the last
        // intent has stood in; and the delay as it was before that run began,
        // which is put back when the run turns out to have been the peer's
        // clock and not its packets.
        u32 lateIntentTicks = 0;
        u32 intentBudget = MaxIntentBurst;
        bool lateIntentCounted = false;
        // The stream was anchored again and has not been started since: the
        // next start is on the newest tick queued, not the oldest, and a tick
        // at or below `anchorFloor` -- seen before the clock moved -- is not
        // queued again by the messages that arrive with it.
        bool anchoredAgain = false;
        u64 anchorFloor = 0;
        // Ticks since an intent message last came from this peer, and since
        // any message did.
        u32 silentTicks = 0;
        u32 quietTicks = 0;
        // The newest tick this peer has sent: a late tick past it is one the
        // authority has never seen, and a press in it has never been applied.
        u64 newestIntentSeen = 0;
        // **How many times this peer has dropped simulated time** (D498), as
        // its newest intent says, and whether one has said it yet. A change is
        // its clock having moved; anything else late is a packet that was.
        u32 timeEpoch = 0;
        bool timeEpochKnown = false;
        u32 standInRun = 0;
        u32 delayBeforeRun = InitialIntentDelay;
        // The roster this peer was last sent, so it is sent again only when it
        // changes.
        std::vector<u32> roster;
        bool rosterSent = false;
        // The scene it was last told the authority is in (ADR 0106), and at
        // which of the authority's loads (G18).
        std::string scene;
        u32 sceneLoad = 0;
        bool sceneSent = false;
        // Whether it has been sent the attributes of the owners that are not
        // spawned -- players, `GlobalScriptService` -- since it was welcomed.
        bool attributesSeeded = false;
        // **The ground it holds** (ADR 0135): sent every chunk that differs
        // from the scene, for the scene named here; a scene change sends it
        // again, since the peer then loads that scene's ground.
        bool groundSent = false;
        std::string groundScene;
        // The parts it owns as it was last told (ADR 0099), and the newest
        // tick of theirs it has taken a state from.
        std::vector<u32> owned;
        u64 ownedTick = 0;
        // What it held at each of the last `StateHistory` sends.
        std::deque<PeerInterest> interest;
        // `RemoteEvent` messages this tick, against the flood limit, and what
        // this tick's intents, owned states and payload bytes came to.
        u32 messagesThisTick = 0;
        u32 messageBudget = RemoteMessageBurst;
        usize byteBudget = RemoteByteBurst;
        bool floodedThisTick = false;
        u32 floodTicks = 0;
        u32 intentsThisTick = 0;
        u32 ownedThisTick = 0;
        usize remoteBytesThisTick = 0;
        // Receives since it connected without a hello.
        u32 unwelcomedReceives = 0;
        // Messages for it naming an event it has not been told about yet, in
        // the order they were sent: the event's network id, the flush it was
        // held at, and the bytes. Held in order, so the front is the oldest.
        struct Held
        {
            u32 remote = 0;
            u64 heldAt = 0;
            std::vector<u8> bytes;
        };
        std::deque<Held> held;
        usize heldBytes = 0;
        u64 flushes = 0;
    };

    // One captured instance, in the walk's pre-order: its id, which instance
    // it is, and where its parent is in the walk (-1 for the root's children).
    struct Captured
    {
        u32 netId = 0;
        core::InstanceId id;
        core::i32 parent = -1;
        // Kept in a service whose contents travel (ADR 0080): in every peer's
        // interest, whatever its position.
        bool pinned = false;
    };

    // The subtree as it stands, and the class name each new id is spawned as.
    void capture(const scene::World& world, core::InstanceId root, u64 tick);
    void sendTo(Peer& peer, const WorldState& everything, const std::vector<u32>& roster,
                const std::vector<u32>& relevant, const std::vector<u32>& owned);
    // The ids this peer should hold now, sorted (ADR 0076).
    [[nodiscard]] std::vector<u32> interestOf(const scene::World& world, const Peer& peer) const;
    [[nodiscard]] const WorldState* historyAt(u64 tick) const noexcept;
    [[nodiscard]] Peer* peerFor(net::PeerId id) noexcept;
    // The instance a network id names in the last capture, or an invalid id.
    [[nodiscard]] core::InstanceId instanceOfNet(const scene::World& world, u32 netId) const noexcept;
    // Every tilemap's blocks against its shadow, into `m_tilemapEdits`.
    void diffTilemaps(const scene::World& world);
    // Every owner's attributes against what was last sent (ADR 0106), into
    // `m_attributeEdits`.
    void diffAttributes(const scene::World& world, core::InstanceId root);
    // The ground -- the workspace's terrain and the block world -- against
    // what was last sent (ADR 0135), into `m_groundEdits`.
    void diffGround(const scene::World& world, core::InstanceId root);
    // Everything of the ground that differs from the scene, to a peer that
    // has not been sent it.
    void sendGroundWhole(Peer& peer, const scene::World& world);
    template <typename Send>
    void encodeGroundWhole(const scene::World& world, Send send);
    // One intent a peer, the next in tick order, as this tick's.
    void applyIntents(scene::World& world);
    // The atom a peer's intent number stands for here, or none: a number it
    // has not named, or a name nothing here has.
    static core::NameAtom intentAtom(scene::World& world, Peer& peer, u16 id);
    // The entries that waited for names this peer has now given.
    static void takeNamedIntents(scene::World& world, Peer& peer);
    void sendAttributes(Peer& peer, const std::vector<u32>& entering);

    using TileBlock = std::pair<scene::TileChunkKey, scene::TileChunk>;
    // One tilemap's cells as last sent to every peer (ADR 0103): one copy
    // per tilemap and not per peer, so the cost does not grow with players.
    struct TilemapShadow
    {
        std::map<scene::TileChunkKey, scene::TileChunk> blocks;
        u64 revision = 0;
        // `World::restores` when it was last compared: a restore rewinds
        // revisions, so after one a matching revision proves nothing.
        u64 restores = 0;
    };

    net::ITransport& m_transport;
    std::vector<Peer> m_peers;
    // Keyed by the instance's packed id, ordered so a capture's retirement pass
    // walks in id order (R10). A network id is never reused: an instance that
    // leaves and a new one in its slot are two ids.
    std::map<u64, u32> m_netIds;
    u32 m_nextNetId = RootNetId.value + 1;
    // Player numbers. 1 is whoever sits at a solo or hosting machine, so peers
    // start at 2 -- on a dedicated server too, so a number means the same kind
    // of player whichever way the match is served.
    u32 m_nextUserId = 2;
    // Every player this authority has welcomed, by the token it gave them, for
    // as long as it runs (ADR 0085). One entry per player, not per connection.
    std::map<PlayerToken, u32> m_identities;
    // What each id is spawned as, by the authority's class name atom.
    std::map<u32, core::NameAtom> m_classNames;
    std::deque<std::shared_ptr<const WorldState>> m_history;
    u32 m_maxPlayers = 0;
    // The world's atom table, for the strings a message carries. Captured by
    // `send`, which is the only caller that can need it.
    // The last capture's walk, in pre-order, and each network id's place in
    // it -- a peer names ids, and a lookup must not walk the world.
    std::vector<Captured> m_order;
    std::unordered_map<u32, u32> m_orderOfNet;
    std::map<u32, TilemapShadow> m_tilemapShadows;
    // This send's changed blocks, by tilemap network id; an emptied block
    // is all zeros.
    std::map<u32, std::vector<TileBlock>> m_tilemapEdits;
    // **Attributes** (ADR 0106): each owner's, encoded, as last sent to every
    // peer -- one copy per owner, not per peer -- and this send's changes. An
    // owner is (0, network id), (1, user id) for a `Player`, (2, 0) for
    // `GlobalScriptService` and (3, 0) for its `Shared` folder.
    using AttributeOwner = std::pair<u8, u32>;
    std::map<AttributeOwner, std::vector<u8>> m_attributeShadows;
    std::vector<std::pair<AttributeOwner, std::vector<u8>>> m_attributeEdits;
    // **The ground as last sent to every peer** (ADR 0135): its chunks,
    // SHARED -- an edit clones a chunk, so a chunk that is not the same one
    // here changed -- its look and its block types, encoded. One copy for all
    // peers, as a tilemap's is.
    struct GroundShadow
    {
        core::InstanceId terrain;
        std::vector<asset::TerrainField::Entry> terrainChunks;
        // The package's chunks at that send: a key that was the package's
        // and still is changed only by streaming, on every machine alike.
        std::vector<asset::TerrainField::Entry> terrainShipped;
        u64 terrainRevision = 0;
        std::vector<u8> terrainLook;
        // The scene the shadow is of, and -- once a terrain of it was replaced
        // or destroyed -- the package its peers loaded, which every terrain
        // after is measured against; with the last terrain's settings, to
        // send its removal in (terrain audit R4).
        std::string scene;
        std::vector<asset::TerrainField::Entry> base;
        bool baseSet = false;
        asset::FieldSettings settings;
        std::vector<asset::VoxelGrid::Entry> voxelChunks;
        std::vector<asset::VoxelGrid::Entry> voxelShipped;
        u64 voxelRevision = ~u64{0};
        std::vector<u8> voxelTypes;
        u64 restores = 0;
    };
    GroundShadow m_ground;
    // This send's changes, as whole messages for every peer already holding
    // the ground.
    std::vector<std::vector<u8>> m_groundEdits;
    // **The whole ground, encoded once for every peer that needs it** (NA2):
    // a scene change with thirty peers encoded the world thirty times in one
    // tick. Made again when the ground changes (`diffGround` clears it).
    std::vector<std::vector<u8>> m_groundWhole;
    bool m_groundWholeValid = false;
    const scene::World* m_world = nullptr;
    u64 m_tick = 0;
    Stats m_stats;
};

class ReplicaSession
{
public:
    // `authority` is the peer `connect` returned. The handshake starts when the
    // transport reports the connection, not before.
    ReplicaSession(net::ITransport& transport, net::PeerId authority) noexcept
        : m_transport(transport), m_authority(authority)
    {}

    // Applies everything that arrived, in arrival order, under `root`, and
    // acknowledges what it reconstructed.
    void receive(scene::World& world, core::InstanceId root, bool ticking = true);

    // Sends what the player at this machine did this tick: their intents, as
    // this world's `captureLocalIntents` left them. Unreliable and sequenced,
    // because a late intent is worse than a missing one -- the next tick's
    // says what the player is doing now.
    void sendIntent(const scene::World& world, u64 tick);
    // This machine dropped simulated time (D498): the intents sent after say so.
    void noteTimeDropped() noexcept { m_timeEpoch += 1; }

    // The world's outbox of `FireServer` messages, to the authority.
    void sendMessages(scene::World& world);

    [[nodiscard]] bool welcomed() const noexcept { return m_welcomed; }
    // The link to the authority, as the transport measures it.
    [[nodiscard]] net::PeerLink link() const noexcept { return m_transport.link(m_authority); }
    [[nodiscard]] bool connected() const noexcept { return m_connected; }
    // Whether the connection went and has not come back: the host's cue to
    // dial again (`rebind`).
    [[nodiscard]] bool lost() const noexcept { return m_lost; }
    // A new connection to the authority, dialled after the last one was lost.
    // The handshake starts when it connects, and presents the token, so the
    // authority welcomes the same player back (ADR 0085).
    void rebind(net::PeerId authority) noexcept
    {
        m_authority = authority;
        m_connected = false;
        m_welcomed = false;
    }
    // Who this machine's player is to the authority: invalid before the first
    // welcome. Settable, so an identity can outlive the process that got it.
    [[nodiscard]] const PlayerToken& playerToken() const noexcept { return m_token; }
    // Welcomes that made this machine a new player (`Status::freshJoins`).
    [[nodiscard]] u32 freshJoins() const noexcept { return m_freshJoins; }
    void setPlayerToken(const PlayerToken& token) noexcept { m_token = token; }
    // **How the own character is stepped again when the authority corrects
    // it** (ADR 0076, as amended). Each tick's prediction keeps the command
    // the physics step consumed; a correction puts the character where the
    // authority said and replays the commands it has not answered yet. Unset,
    // a correction shifts the prediction by the error, as before.
    void setCharacterReplay(scene::ICharacterReplay* replay) noexcept { m_replay = replay; }
    // **Where this replica's own scripts are kept** (ADR 0138 §6): asked at
    // each spawn, because a hot reload makes a new host and new templates.
    // Unset, nothing is attached, which is what a test with no host wants.
    void setScriptTemplates(std::function<ScriptTemplates*()> templates) { m_templates = std::move(templates); }
    // How this replica follows the authority to another scene (ADR 0106).
    // Unset, a scene change is ignored, which is what a test with no host
    // wants.
    void setSceneChanger(std::function<void(scene::World&, const std::string&, std::vector<core::u8>)> changer)
    {
        m_sceneChanger = std::move(changer);
    }
    // **Called when the authority first takes this replica** (N1), before
    // anything it sends is applied: where a host clears its world for the
    // server's. Until then the machine is what it was -- a join that fails
    // leaves its menu standing.
    void setWelcomeHandler(std::function<void(scene::World&)> handler) { m_welcomeHandler = std::move(handler); }
    // Why the authority refused this replica, or 0 (`RefusedVersion`,
    // `RefusedFull`).
    [[nodiscard]] u8 refused() const noexcept { return m_refused; }
    // The newest state applied to the world. Zero before the first.
    [[nodiscard]] u64 appliedTick() const noexcept { return m_applied; }
    // This replica's player number, as the authority's welcome named it.
    [[nodiscard]] u32 playerId() const noexcept { return m_playerId; }
    [[nodiscard]] core::InstanceId localOf(NetId id) const noexcept;
    [[nodiscard]] Stats stats() const noexcept
    {
        Stats now = m_stats;
        now.predictedBodies = static_cast<u32>(m_predictedParts.size());
        return now;
    }
    // Snapshots refused because the reconstruction did not match what the
    // authority described. Zero in a correct build; a test asserts it.
    [[nodiscard]] u64 checksumFailures() const noexcept { return m_checksumFailures; }
    // The loose parts this replica predicts (ADR 0133), and how far, how many
    // and how long.
    void setPrediction(core::f64 radius, u32 maxBodies, u32 lingerTicks) noexcept
    {
        m_predictRadius = radius;
        m_predictMax = maxBodies;
        m_predictLinger = lingerTicks;
    }
    [[nodiscard]] usize predictedCount() const noexcept { return m_predictedParts.size(); }
    // The own character's drawn offset (`VisualCorrection`).
    [[nodiscard]] VisualCorrection visualCorrection() const noexcept
    {
        return VisualCorrection{m_visualCharacter, m_visualOffset, m_displaced};
    }

private:
    void decayVisualOffset() noexcept;
    // Which loose parts are predicted this tick: in, out, and what each
    // starts from.
    void updatePredicted(scene::World& world);
    void onSnapshot(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    // A part of a snapshot sent reliably (NA1): joined, and applied whole
    // when the last part is in.
    void onSnapshotPart(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    void onSpawn(scene::World& world, std::span<const u8> bytes);
    void onDespawn(scene::World& world, std::span<const u8> bytes);
    // Every record of these ids this session keeps, gone: the local mapping,
    // what was written, the samples, and the ids in every remembered state --
    // each state copied once for all of them, not once an id.
    void forget(std::span<const u32> ids);
    // **A welcome after a lost connection starts the world again.** The new
    // connection's baseline is empty on the authority's side, so everything
    // is sent afresh; what this replica held from the old one leaves as a
    // chunk streaming out does -- a husk if a script holds it -- and nothing
    // stale survives to be updated by nobody.
    void resetForRejoin(scene::World& world);
    void onPlayers(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    void applyToWorld(scene::World& world, core::InstanceId root, const WorldState& state);
    void resolveCharacters(scene::World& world, core::InstanceId root);
    void onOwnership(scene::World& world, std::span<const u8> bytes);
    void onTilemapBlocks(scene::World& world, std::span<const u8> bytes);
    // The ground (ADR 0135): the workspace's terrain, made if there is none,
    // and the block world, their chunks and their looks.
    void onTerrainChunks(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    void onTerrainLook(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    void onVoxelChunks(scene::World& world, std::span<const u8> bytes);
    void onVoxelTypes(scene::World& world, std::span<const u8> bytes);
    void onSceneChange(scene::World& world, std::span<const u8> bytes);
    void onAttributes(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    void sendOwned(const scene::World& world, u64 tick);
    void reconcile(scene::World& world, core::InstanceId character, const scene::CharacterReplayStart& authority);
    // The same for a character on the plane (D434): where the authority has
    // the sprite at the intent it last applied, against where this machine
    // had it then.
    void reconcile2d(scene::World& world, core::InstanceId character, core::Vec2 position, core::f32 rotation);
    void interpolate(scene::World& world);
    [[nodiscard]] const WorldState* stateAt(u64 tick) const noexcept;

    net::ITransport& m_transport;
    net::PeerId m_authority;
    bool m_connected = false;
    bool m_welcomed = false;
    bool m_lost = false;
    // Welcomed at least once on any connection, so the next welcome is a rejoin.
    bool m_joinedBefore = false;
    // The authority's scene load this replica last followed (G18), and whether
    // it has heard one since it joined: the first is the join's.
    u32 m_sceneLoad = 0;
    bool m_sceneHeard = false;
    u32 m_freshJoins = 0;
    PlayerToken m_token;
    u32 m_playerId = 0;
    u64 m_applied = 0;
    // Ticks since anything came from the authority (`SilentPeerTicks`).
    u32 m_quietTicks = 0;
    // The snapshot whose parts are arriving: its tick, the next part's index,
    // and its bytes so far.
    u64 m_partTick = 0;
    u32 m_partNext = 0;
    std::vector<u8> m_partBytes;
    // The authority's name atoms, as this world's.
    std::map<u32, core::NameAtom> m_names;
    // Network id to local instance, for everything spawned and not despawned.
    std::map<u32, core::InstanceId> m_locals;
    // What the world was last given, per id, in the authority's terms -- so an
    // apply writes only what changed.
    std::map<u32, FieldSet> m_written;
    // Ids that have left, and the applied tick they left at: a filter a
    // reconstructed state must pass, for as long as a baseline could still
    // hold them (`DepartedMemoryTicks`), and then forgotten.
    std::map<u32, u64> m_departed;
    std::function<bool(core::InstanceId)> m_probe;
    std::function<void(scene::World&, const std::string&, std::vector<core::u8>)> m_sceneChanger;
    std::function<void(scene::World&)> m_welcomeHandler;
    u8 m_refused = 0;
    std::function<ScriptTemplates*()> m_templates;
    // Husks made since the host last drained them.
    std::vector<core::InstanceId> m_streamedOut;
    std::deque<std::shared_ptr<const WorldState>> m_states;
    u64 m_checksumFailures = 0;
    Stats m_stats;
    // Every player's character by user id, as the last roster named it.
    std::map<u32, u32> m_characters;
    // The NetId of this machine's own player's character, or zero.
    u32 m_owned = 0;
    // Whether the own character has taken the authority's state since it
    // became this machine's: until it has, nothing is predicted from it. The
    // roster that names it is reliable and the snapshots that place it are
    // not, so a character could be predicted from where it was spawned --
    // the origin -- and corrected the moment the first answer came.
    bool m_ownedSynced = false;
    // Predicted parts, by network id, and how many ticks each has been out of
    // range.
    std::map<u32, u32> m_predictedParts;
    // How far the last comparison found a predicted part from the authority's.
    core::f64 m_lastBodyCorrection = 0.0;
    core::f64 m_predictRadius = DefaultPredictRadius;
    u32 m_predictMax = DefaultPredictMaxBodies;
    u32 m_predictLinger = DefaultPredictLingerTicks;
    // What is left to slide of the corrections so far, and whose.
    core::InstanceId m_visualCharacter;
    core::DVec3 m_visualOffset{};
    // An offset was added this tick: it is drawn whole once before it decays
    // (NA16). Decayed in the receive that added it, 40% of a correction was a
    // pop on its first frame.
    bool m_visualFresh = false;
    core::DVec3 m_displaced{};
    // The parts this replica owns (ADR 0099): simulated here, sent up, and
    // never overwritten by a snapshot.
    std::set<u32> m_ownedParts;

    // One remembered transform at one tick, and for the own character the
    // command that step consumed.
    struct Sample
    {
        u64 tick = 0;
        core::CFrameD cframe;
        std::optional<scene::CharacterCommand> command;
    };
    scene::ICharacterReplay* m_replay = nullptr;
    // Prediction (ADR 0076): the own character after each local tick, by the
    // intent tick that produced it, and the last intent the authority applied.
    std::deque<Sample> m_predicted;
    u64 m_ackedIntent = 0;
    // The last `IntentRedundancy` intents sent, encoded, oldest first: each
    // message carries them all (protocol 22).
    std::deque<std::pair<u64, std::vector<u8>>> m_sentIntents;
    // The number each action's name was given on this connection (G38), by
    // atom: given in the order first sent, and sent once, reliably.
    std::map<u32, u16> m_intentIds;
    // **The own character's predicted attributes, as the snapshot being
    // applied has them** (G37): the authority's word, compared with what this
    // replica remembers at the tick it answers. Nothing: it said nothing.
    std::optional<scene::PredictedAttributes> m_snapshotAttributes;
    // How many times this machine has dropped simulated time (D498), sent
    // with every intent.
    u32 m_timeEpoch = 0;
    // The answer the own character was last compared against.
    u64 m_reconciledAck = 0;
    // Interpolation: every remote part's last few snapshot transforms, by
    // server tick, and the server clock they are drawn against.
    std::map<u32, std::deque<Sample>> m_samples;
    // The same for every remote `Part2D` (ADR 0103): where it was and how it
    // was turned, in degrees, at one snapshot tick.
    struct Sample2D
    {
        u64 tick = 0;
        core::Vec2 position{0.0f, 0.0f};
        core::f32 rotation = 0.0f;
    };
    std::map<u32, std::deque<Sample2D>> m_samples2d;
    // Prediction, for an own character that is a `Part2D` (D434): where the
    // sprite was after each local tick, by the intent tick that produced it.
    std::deque<Sample2D> m_predicted2d;
    u64 m_serverClock = 0;
    u32 m_interpolationDelay = DefaultInterpolationDelay;
    // **The delay adapts to the link** (NA14): what it was set to is the
    // least it is; how late snapshots arrive against the clock, and how
    // unevenly, and the ticks between them, say how much more it needs.
    u32 m_baseDelay = DefaultInterpolationDelay;
    core::f64 m_lateAverage = 0.0;
    core::f64 m_lateSpread = 0.0;
    core::f64 m_snapshotInterval = 1.0;
    u64 m_lastSnapshotTick = 0;
    u32 m_delayTicks = 0;

public:
    // How many ticks behind the server's clock remote parts are drawn, at
    // least: the link may make it more (NA14). Zero applies each snapshot as
    // it arrives, which is what a test comparing two worlds pixel for pixel
    // wants, and does not adapt.
    void setInterpolationDelay(u32 ticks) noexcept
    {
        m_interpolationDelay = ticks;
        m_baseDelay = ticks;
    }
    [[nodiscard]] u32 interpolationDelay() const noexcept { return m_interpolationDelay; }

    // Whether a script holds an instance, asked before one that left interest
    // is removed: held, it becomes a husk (reparented to nil) and is reported
    // through `drainStreamedOut`; not held, it is destroyed. Unset, everything
    // is destroyed, which is what a test with no VM wants.
    void setReferenceProbe(std::function<bool(core::InstanceId)> probe) { m_probe = std::move(probe); }
    [[nodiscard]] std::vector<core::InstanceId> drainStreamedOut()
    {
        std::vector<core::InstanceId> drained;
        drained.swap(m_streamedOut);
        return drained;
    }
};

} // namespace engine::replication
