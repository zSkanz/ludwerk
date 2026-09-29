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
inline constexpr u16 MaxRemoteHeldSends = 300;
// The script module's own payload ceiling, plus the few bytes of its header.
inline constexpr usize MaxRemoteWirePayload = 64u * 1024u + 16u;

// **What one peer may cost an authority in one tick** (audit N1's review). A
// client is not trusted to send one intent and one owned state a tick, or a
// sane count in either; what is over these is dropped, and a count over them
// is a refusal of the whole message. The byte budget is what `RemoteEvent`
// payloads may add up to, on top of the message count above.
inline constexpr u32 MaxIntentsPerTick = 8;
inline constexpr u16 MaxIntentEntries = 256;

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

// The visual slide of a correction: what is left of it after each tick, and
// the distance past which a correction is a teleport and drawn as one.
inline constexpr core::f64 VisualDecayPerTick = 0.6;
inline constexpr core::f64 VisualSnapMetres = 2.0;
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
inline constexpr u32 MaxUnwelcomedReceives = 600;
// Players this authority remembers across connections (ADR 0085). Past it,
// the longest-known one not connected now is forgotten: its next visit is a
// new player, which is what an authority that restarted would say too.
inline constexpr usize MaxKnownIdentities = 65536;
// The largest message an authority accepts from a peer: an owned state of
// `MaxOwnedRecords` parts, a `RemoteEvent` at its ceiling, with room.
inline constexpr usize MaxAuthorityMessageBytes = 1024u * 1024u;
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
        // The newest tick queued when the delay last held a tick to grow.
        u64 pausedAtNewest = 0;
        // The roster this peer was last sent, so it is sent again only when it
        // changes.
        std::vector<u32> roster;
        bool rosterSent = false;
        // The scene it was last told the authority is in (ADR 0106).
        std::string scene;
        bool sceneSent = false;
        // Whether it has been sent the attributes of the owners that are not
        // spawned -- players, `GlobalScriptService` -- since it was welcomed.
        bool attributesSeeded = false;
        // The parts it owns as it was last told (ADR 0099), and the newest
        // tick of theirs it has taken a state from.
        std::vector<u32> owned;
        u64 ownedTick = 0;
        // What it held at each of the last `StateHistory` sends.
        std::deque<PeerInterest> interest;
        // `RemoteEvent` messages this tick, against the flood limit, and what
        // this tick's intents, owned states and payload bytes came to.
        u32 messagesThisTick = 0;
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
    // One intent a peer, the next in tick order, as this tick's.
    void applyIntents(scene::World& world);
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
    void setPlayerToken(const PlayerToken& token) noexcept { m_token = token; }
    // **How the own character is stepped again when the authority corrects
    // it** (ADR 0076, as amended). Each tick's prediction keeps the command
    // the physics step consumed; a correction puts the character where the
    // authority said and replays the commands it has not answered yet. Unset,
    // a correction shifts the prediction by the error, as before.
    void setCharacterReplay(scene::ICharacterReplay* replay) noexcept { m_replay = replay; }
    // How this replica follows the authority to another scene (ADR 0106).
    // Unset, a scene change is ignored, which is what a test with no host
    // wants.
    void setSceneChanger(std::function<void(scene::World&, const std::string&, std::vector<core::u8>)> changer)
    {
        m_sceneChanger = std::move(changer);
    }
    // The newest state applied to the world. Zero before the first.
    [[nodiscard]] u64 appliedTick() const noexcept { return m_applied; }
    // This replica's player number, as the authority's welcome named it.
    [[nodiscard]] u32 playerId() const noexcept { return m_playerId; }
    [[nodiscard]] core::InstanceId localOf(NetId id) const noexcept;
    [[nodiscard]] Stats stats() const noexcept { return m_stats; }
    // Snapshots refused because the reconstruction did not match what the
    // authority described. Zero in a correct build; a test asserts it.
    [[nodiscard]] u64 checksumFailures() const noexcept { return m_checksumFailures; }
    // The own character's drawn offset (`VisualCorrection`).
    [[nodiscard]] VisualCorrection visualCorrection() const noexcept
    {
        return VisualCorrection{m_visualCharacter, m_visualOffset};
    }

private:
    void decayVisualOffset() noexcept;
    void onSnapshot(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
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
    void onSceneChange(scene::World& world, std::span<const u8> bytes);
    void onAttributes(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    void sendOwned(const scene::World& world, u64 tick);
    void reconcile(scene::World& world, core::InstanceId character, const scene::CharacterReplayStart& authority);
    void interpolate(scene::World& world);
    [[nodiscard]] const WorldState* stateAt(u64 tick) const noexcept;

    net::ITransport& m_transport;
    net::PeerId m_authority;
    bool m_connected = false;
    bool m_welcomed = false;
    bool m_lost = false;
    // Welcomed at least once on any connection, so the next welcome is a rejoin.
    bool m_joinedBefore = false;
    PlayerToken m_token;
    u32 m_playerId = 0;
    u64 m_applied = 0;
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
    // Husks made since the host last drained them.
    std::vector<core::InstanceId> m_streamedOut;
    std::deque<std::shared_ptr<const WorldState>> m_states;
    u64 m_checksumFailures = 0;
    Stats m_stats;
    // Every player's character by user id, as the last roster named it.
    std::map<u32, u32> m_characters;
    // Each player's team, by the network id the roster named (ADR 0099).
    std::map<u32, u32> m_teams;
    // The NetId of this machine's own player's character, or zero.
    u32 m_owned = 0;
    // Whether the own character has taken the authority's state since it
    // became this machine's: until it has, nothing is predicted from it. The
    // roster that names it is reliable and the snapshots that place it are
    // not, so a character could be predicted from where it was spawned --
    // the origin -- and corrected the moment the first answer came.
    bool m_ownedSynced = false;
    // What is left to slide of the corrections so far, and whose.
    core::InstanceId m_visualCharacter;
    core::DVec3 m_visualOffset{};
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
    u64 m_serverClock = 0;
    u32 m_interpolationDelay = DefaultInterpolationDelay;

public:
    // How many ticks behind the server's clock remote parts are drawn. Zero
    // applies each snapshot as it arrives, which is what a test comparing two
    // worlds pixel for pixel wants.
    void setInterpolationDelay(u32 ticks) noexcept { m_interpolationDelay = ticks; }

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
