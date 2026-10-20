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
#include <unordered_set>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/name_atom.h"
#include "engine/net/transport.h"
#include "engine/replication/extract.h"
#include "engine/replication/types.h"
#include "engine/scene/character_replay.h"
#include "engine/scene/components.h"
#include "engine/scene/tween_clock.h"

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
// **A client's messages are budgeted over time**: what it has in
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
// **How far an owner may move what it owns in a tick**: two metres,
// 120 m/s. A drag, a throw, a vehicle -- all under it; a part put across the
// map by a client that holds it is refused, and the authority's place stands.
inline constexpr core::f64 MaxOwnedMetresPerTick = 2.0;
inline constexpr u16 MaxRemoteHeldSends = 300;
// The script module's own payload ceiling, plus the few bytes of its header.
inline constexpr usize MaxRemoteWirePayload = 64u * 1024u + 16u;
// An `UnreliableRemoteEvent`'s (ADR 0161): what the script module lets one
// carry, and its count byte.
inline constexpr usize MaxUnreliableWirePayload = 16u * 1024u + 16u;

// **A replicated swarm** (ADR 0162).
//
// One message of positions: a packet, so it is never sent in fragments and a
// lost one is only itself.
inline constexpr usize SwarmStateBytes = 1100;
// What one replica is sent of positions, a tick and at most in hand: 29 KB a
// second, and a burst of five ticks.
inline constexpr core::f64 SwarmBytesPerTick = 480.0;
inline constexpr core::f64 SwarmByteBurst = 4000.0;
// How wrong a replica may draw an agent before it is told again, near its
// focus and at the edge of its reach, in metres and in radians of facing; and
// how long an agent goes untold, in ticks.
inline constexpr core::f64 SwarmNearError = 0.2;
inline constexpr core::f64 SwarmFarError = 1.5;
inline constexpr core::f64 SwarmNearTurn = 0.35;
inline constexpr core::f64 SwarmFarTurn = 1.2;
// A lost message is told again when nobody acknowledges it, so the refresh is
// only for what that misses: two seconds near, four far.
inline constexpr core::f64 SwarmNearRefresh = 120.0;
inline constexpr core::f64 SwarmFarRefresh = 240.0;
// How long a message of positions waits for its acknowledgement before its
// agents are told again, in ticks: the round trip it has measured and half
// again, within these.
inline constexpr core::f64 SwarmAckWaitLeast = 8.0;
inline constexpr core::f64 SwarmAckWaitMost = 60.0;
// How many of the last messages a replica names in each acknowledgement.
inline constexpr usize SwarmAcksRepeated = 12;
// The reach a position's twelve bits an axis can say, with the lattice its
// origin sits on.
inline constexpr core::f64 MaxSwarmReach = 220.0;
// How many agents one reliable message of comings and goings carries.
inline constexpr usize SwarmAgentsPerMessage = 512;

// **What one peer may cost an authority in one tick**. A
// client is not trusted to send one intent and one owned state a tick, or a
// sane count in either; what is over these is dropped, and a count over them
// is a refusal of the whole message. The byte budget is what `RemoteEvent`
// payloads may add up to, on top of the message count above.
inline constexpr u32 MaxIntentsPerTick = 8;
// **Intent messages are budgeted over time, not by tick**: a peer has
// `MaxIntentBurst` in hand and gains `IntentBudgetPerTick` a tick. A replica
// sends one a tick, so its budget is always full; a burst after a hitch -- a
// server frame of 200 ms packs twelve of a client's ticks into one of its own --
// is all read, where eight a tick dropped the newest four; and a peer flooding
// is held to two a tick for as long as it floods.
inline constexpr u32 MaxIntentBurst = 32;
inline constexpr u32 IntentBudgetPerTick = 2;
inline constexpr u16 MaxIntentEntries = 256;
// **An action's name crosses once a connection** (protocol 37): an
// `IntentNames` message gives it a number, and every intent after carries the
// number. As many names as a connection may give, how long one may be, and how
// many entries may wait for a name that has not come yet.
inline constexpr u16 MaxIntentNames = 1024;
inline constexpr usize MaxIntentNameBytes = 255;
inline constexpr usize MaxUnnamedIntents = 256;
// The most predicted attributes a snapshot carries for a replica's own
// character.
inline constexpr u16 MaxPredictedAttributes = 64;

// **Intents, buffered and applied one a tick** (protocol 22).
// An `Intent` message carries the newest tick and up to
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
// It is the ceiling, and nothing else drains a queue: a replica whose clock runs
// ahead of this one's is told how far past the delay its queue stands, and
// slows its own clock by `ClockNudge` from `PaceAbove` ticks over until it is
// back within `PaceBelow` -- a skipped intent is a tick of the player's travel
// the replica stepped and the authority did not, and a correction by it.
inline constexpr u32 IntentCatchUp = 8;
inline constexpr u32 PaceAbove = 2;
inline constexpr u32 PaceBelow = 1;
inline constexpr core::f64 ClockNudge = 0.04;
inline constexpr usize MaxQueuedIntents = 64;
// **How long the last intent stands in while the peer says nothing**:
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
// **Still, to the intent queue**: a character that moved less than this
// since the last tick, and a body slower than this, is at rest -- a tick of
// its player's held or skipped then moves nothing a replica stepped.
inline constexpr core::f64 RestMetres = 0.0005;
inline constexpr core::f32 RestSpeed = 0.01f;
inline constexpr u32 DefaultPredictMaxBodies = 32;
inline constexpr u32 DefaultPredictLingerTicks = 30;
// **A part that moves on its own, near a character this machine predicts**
// (ADR 0254): drawn where the authority will have it at the tick predicted,
// carried on by its last motion at most this far ahead of what was heard
// (`[network] predict_moving_lead_ms`), and blended back between snapshots
// over this many ticks once it leaves.
inline constexpr core::f64 DefaultPredictMovingLeadMs = 250.0;
inline constexpr u32 MovingBlendTicks = 15;
// A part moved within this many ticks of the newest snapshot is moving.
inline constexpr u64 MovingRecentTicks = 60;
inline constexpr core::f64 VisualSnapMetres = 2.0;
// **A vehicle glides slower than a character** (ADR 0213): what is left of a
// correction a tick, about a quarter of a second to nothing. A car moves its
// own length in a few ticks, and corrected over a character's tenth of a
// second its step showed as a jolt.
inline constexpr core::f64 VehicleVisualDecayPerTick = 0.8;
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
// Why a replica was refused (`Refused`).
inline constexpr u8 RefusedVersion = 1;
inline constexpr u8 RefusedFull = 2;
// The host removed this player (`Player:Kick`, ADR 0167), with words of the
// game's own, at most this many bytes.
inline constexpr u8 RefusedRemoved = 3;
// The address it came from already has as many players on this authority as
// it allows (`setMaxPeersPerAddress`).
inline constexpr u8 RefusedAddressLimit = 4;
// Its game runs with other Luau flags than the authority's (ADR 0217): other
// syntax, another compiler or another VM, so not the same scripts.
inline constexpr u8 RefusedLuauFlags = 5;
// Its game does not encrypt, and this authority does (ADR 0224): said in the
// clear, the one message that is, by the transport that encrypts.
inline constexpr u8 RefusedInsecure = 6;
// The game's own answer to an ask to join (`JoinAnswer`): a ban, a ticket that
// did not check, a queue -- in its words, which travel with it.
inline constexpr u8 RefusedByGame = 7;
// **Decided by this machine, never sent** (ADR 0224 §3): the server proved a
// key other than the one this machine pinned or was told, or would not finish
// an encrypted handshake. Answers as final as a refusal -- dialling again
// meets the same key -- where they read as a connection lost, dialled again
// for ever.
inline constexpr u8 RefusedServerKey = 8;
inline constexpr u8 RefusedNotEncrypted = 9;
// An ask to join the game did not answer in this many authority receives (a
// minute at sixty): refused, as one nobody admitted.
inline constexpr u32 MaxAskReceives = 3600;
inline constexpr usize MaxRemovedReasonBytes = 512;
// The longest ticket a hello carries (ADR 0226): a signed token of a few
// claims is a few hundred bytes.
inline constexpr usize MaxTicketBytes = 2048;
// And the longest pass (ADR 0222), in base64: its fields, a signature and the
// travel's data, at most 16 KiB of it.
inline constexpr usize MaxPassBytes = 24 * 1024;
// A `Refused` message with no words of the game's: what the encrypting
// transport says, in the clear, to a joiner that does not encrypt.
[[nodiscard]] std::vector<u8> refusalMessage(u8 reason, std::string_view words = {});
// The protocol this build speaks, and the channels a connection to it opens:
// what a front door with no session of its own needs (ADR 0220).
[[nodiscard]] u32 protocolVersion() noexcept;
[[nodiscard]] u8 transportChannels() noexcept;
// **What a match's handshake binds** (ADR 0224): this protocol's version and
// the game's id, so another game or another build fails the handshake.
[[nodiscard]] std::vector<u8> matchPrologue(std::string_view projectId);

// **What a hello says** (ADR 0220): read by a front door, which answers it
// without a world. Nothing for bytes that are not a hello.
struct HelloFields
{
    u32 protocol = 0;
    u64 luauFlags = 0;
    std::string ticket;
    std::string pass;
};
[[nodiscard]] std::optional<HelloFields> readHello(std::span<const u8> payload);
// A `Redirect` (ADRs 0220, 0222): join there, with that pass.
[[nodiscard]] std::vector<u8> redirectMessage(std::string_view address, u16 port, std::string_view pass,
                                              std::string_view key, std::string_view world);
// **A place in the front door's line** (ADR 0232): from 1, and the seconds
// until its turn, none when nothing has measured it yet.
inline constexpr u32 NoEstimate = 0xFFFFFFFFu;
[[nodiscard]] std::vector<u8> queuedMessage(u32 position, u32 estimateSeconds);
// **Signing in at the front door** (ADR 0237): what a player's machine sends
// on a connection opened for it, and the door's answer. Nothing for bytes
// that are not one.
// What a `SignIn` asks (protocol 62): an account signed in to or made, or a
// session of the game's Nakama checked (ADR 0242).
enum class SignInKind : u8
{
    SignIn = 0,
    Register = 1,
    NakamaSession = 2,
    // A match Nakama's matchmaker made: its id in `name`, the session in
    // `password`, and the front door makes the match's instance (ADR 0242).
    NakamaMatch = 3,
};
struct SignInFields
{
    SignInKind kind = SignInKind::SignIn;
    std::string name;
    // The password, or a session's token.
    std::string password;
};
inline constexpr usize MaxSignInNameBytes = 64;
inline constexpr usize MaxSignInPasswordBytes = 128;
inline constexpr usize MaxSignInSessionBytes = 4096;
[[nodiscard]] std::vector<u8> signInMessage(const SignInFields& fields);
[[nodiscard]] std::optional<SignInFields> readSignIn(std::span<const u8> payload);
struct SignedInFields
{
    // 0 signed in, else an `Enum.SignInFailure`'s value.
    u8 failure = 0;
    std::string ticket;
    u32 retryAfter = 0;
};
[[nodiscard]] std::vector<u8> signedInMessage(const SignedInFields& fields);
[[nodiscard]] std::optional<SignedInFields> readSignedIn(std::span<const u8> payload);
// Players this authority remembers across connections (ADR 0085). Past it,
// the longest-known one not connected now is forgotten: its next visit is a
// new player, which is what an authority that restarted would say too.
inline constexpr usize MaxKnownIdentities = 65536;
// The largest message an authority accepts from a peer: an owned state of
// `MaxOwnedRecords` parts, a `RemoteEvent` at its ceiling, with room.
inline constexpr usize MaxAuthorityMessageBytes = 1024u * 1024u;
// **A snapshot too large to go as one unreliable message**: past this
// it is sent reliably, in parts of `SnapshotPartBytes`, and nothing more is
// sent to that peer until it is acknowledged. An unreliable message ENet
// fragments is lost whole when one fragment is, and a world's first snapshot --
// every record whole, no baseline -- was resent every other tick until one got
// through: 29 seconds to join a world of 1 000 parts on this machine's own
// loopback, and never at 5 000.
inline constexpr usize ReliableSnapshotBytes = 16u * 1024u;
// **How long a connected peer may say nothing before it is gone**, in ticks:
// ten seconds, the transport's own default. The transport is serviced by a
// thread of its own, so a process whose game froze still answers it --
// and a server stuck in a script, or a client whose window hung, was a
// connection that never ended. A replica hears a snapshot every few ticks and
// an authority an intent every tick, so silence this long is a peer that
// stopped.
inline constexpr u32 SilentPeerTicks = 600;
inline constexpr usize SnapshotPartBytes = 32u * 1024u;
// The most a replica will join from snapshot parts: a world of some hundred
// thousand parts, and not an authority's way to fill a player's memory.
inline constexpr usize MaxReplicaSnapshotBytes = 64u * 1024u * 1024u;
// **A move this far in a tick is a teleport, not a motion**: drawn as a
// step at its tick rather than a slide across the map. Two metres a tick is
// 120 m/s -- faster than anything a game walks, drives or flies on purpose.
inline constexpr core::f64 TeleportMetresPerTick = 2.0;
// **The furthest ahead another player's character is expected** (ADR 0163),
// in ticks: a quarter of a second. Past that a guess at where somebody will be
// is worse than where they were last seen going.
inline constexpr u64 MaxCollisionLeadTicks = 15;
// The most the adaptive interpolation delay adds over what it was set to.
inline constexpr u32 MaxAddedInterpolationDelay = 20;
// **Players one address may be on an authority by default**, `[network]
// max_connections_per_address` and `--max-per-address`, nought for no limit.
// Eight refused the ninth player of a household or a LAN house behind one
// router, of a school behind one NAT, and of a carrier's NAT, where phones by
// the thousand share an address -- and the ninth heard "connection lost".
inline constexpr u32 DefaultPeersPerAddress = 64;
// **A flood is told from a crowd by its rate, not its count**: connections
// begun from one address in the last second of receives, and connections of
// it that have not said hello yet. Past either, the connection is let go
// without a word -- a machine dialling in a loop is not owed one.
inline constexpr u32 MaxConnectsPerAddressPerSecond = 20;
inline constexpr u32 MaxHandshakesPerAddress = 16;

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
// The furthest behind a machine is believed to draw the world (ADR 0235): a
// minute at sixty ticks. The cap a shot is rewound by is far less.
inline constexpr u64 MaxViewBehindTicks = 3600;
inline constexpr usize InterpolationSamples = 6;
inline constexpr usize PredictionHistory = 128;

// One instance as the wire sees it: its network id, which of the schema's
// classes describes it, and its fields in schema order. Values are in the
// AUTHORITY'S terms -- its name atoms, its network ids -- on both ends.
// **An entity's fields, shared by every state that has the same ones.** A
// field is sixty-four bytes and an entity a kilobyte and more; a state was a
// copy of all of them, a history sixty-four states deep, and each tick copied
// every entity nothing had happened to -- on the authority into its capture,
// and on a replica into each snapshot it applied. A state holds the set by
// reference, and takes a set of its own only to write to it (`write`).
class SharedFields
{
public:
    SharedFields() = default;
    explicit SharedFields(FieldSet set) : m_set(std::make_shared<FieldSet>(std::move(set))) {}
    // A set somebody is done with, given again: its storage with it.
    explicit SharedFields(std::shared_ptr<FieldSet> set) noexcept : m_set(std::move(set)) {}

    [[nodiscard]] const FieldValue& operator[](usize at) const noexcept { return (*m_set)[at]; }
    [[nodiscard]] usize size() const noexcept { return m_set != nullptr ? m_set->size() : 0; }
    // Every cell, to read: a sequence's are not next to each other (protocol 44).
    [[nodiscard]] std::span<const FieldValue> view() const noexcept
    {
        return m_set != nullptr ? std::span<const FieldValue>(*m_set) : std::span<const FieldValue>{};
    }
    // The whole set, to copy: what a caller that changes a copy starts from.
    [[nodiscard]] FieldSet copy() const { return m_set != nullptr ? *m_set : FieldSet{}; }
    // Whether the two hold the very same set -- not sets that are equal: one a
    // state kept from the state before it, which is how an instance nothing
    // changed is told without reading a field of it.
    [[nodiscard]] bool sameSetAs(const SharedFields& other) const noexcept { return m_set == other.m_set; }
    [[nodiscard]] FieldSet::const_iterator begin() const noexcept
    {
        return m_set != nullptr ? m_set->cbegin() : FieldSet::const_iterator{};
    }
    [[nodiscard]] FieldSet::const_iterator end() const noexcept
    {
        return m_set != nullptr ? m_set->cend() : FieldSet::const_iterator{};
    }

    // **The set to write to**: this state's own from here on. Copied first
    // when another state holds the same one, so no state is changed under its
    // holder.
    [[nodiscard]] FieldSet& write()
    {
        if (m_set == nullptr)
            m_set = std::make_shared<FieldSet>();
        else if (m_set.use_count() > 1)
            m_set = std::make_shared<FieldSet>(*m_set);
        return *m_set;
    }

    // The set itself, for whoever takes it back when its state goes -- only
    // when no other state holds it, and empty otherwise.
    [[nodiscard]] std::shared_ptr<FieldSet> release() noexcept
    {
        std::shared_ptr<FieldSet> mine = std::move(m_set);
        m_set = nullptr;
        return mine.use_count() == 1 ? mine : std::shared_ptr<FieldSet>{};
    }

private:
    std::shared_ptr<FieldSet> m_set;
};

struct EntityState
{
    NetId id;
    u8 schema = 0;
    SharedFields fields;
    // **What its id, schema and field bytes come to** (`hashOf`), kept beside
    // them by whoever makes or changes the entity; 0 is "not taken yet". A
    // state's checksum is read from these, and so is a diff's "nothing of it
    // changed" -- an entity is a kilobyte and more of field bytes, and both
    // read every byte of every entity for every peer, every tick.
    u64 hash = 0;
    // **What the components it was read from came to** (`sourceDigestOf`), on
    // the authority: the same next tick and the instance is kept, not read
    // again. 0 is "always read". Not on the wire, and nothing to a replica.
    u64 source = 0;
};

// A world at one tick, entities sorted by network id.
struct WorldState
{
    u64 tick = 0;
    std::vector<EntityState> entities;
};

// One entity's id, schema and field bytes as a number, never 0. The same on
// every machine a game runs on: the bytes are read a little-endian word at a
// time, and every platform the engine builds for is little-endian.
[[nodiscard]] u64 hashOf(const EntityState& entity) noexcept;

// What a snapshot carries so a replica can prove it reconstructed the state the
// authority meant: every entity's `hash`, folded in id order (protocol 42 --
// it was FNV-1a over every field byte, four megabytes a peer a tick in a world
// of three thousand instances, and as much again on the replica). A checksum,
// not a signature -- it is there to catch our own bugs. An entity whose hash
// has not been taken is hashed here, and not kept.
[[nodiscard]] u64 checksumOf(const WorldState& state) noexcept;
// The same, of the entities a peer is sent: a view, in id order.
[[nodiscard]] u64 checksumOf(std::span<const EntityState* const> entities) noexcept;

class AuthoritySession
{
public:
    // **How many players this authority takes**. Zero: as many as the
    // transport holds. Past it a replica is told `Refused` and let go -- the
    // transport keeps one slot over so that there is a connection to say it on.
    void setMaxPlayers(u32 players) noexcept { m_maxPlayers = players; }
    // Players one address may be, nought for any number
    // (`DefaultPeersPerAddress`).
    void setMaxPeersPerAddress(u32 players) noexcept { m_maxPerAddress = players; }
    // What this machine's Luau flags come to (`Config::luauFlags`): a
    // replica's that differs is refused.
    void setLuauFlags(u64 fingerprint) noexcept { m_luauFlags = fingerprint; }
    // **Bytes of the world's state a second, each player at most** (ADR
    // 0229): `[network] client_budget_kb_per_second`, which a player's
    // `ReplicationBudget` overrides. Nought sends every change at once.
    void setClientBudget(u32 bytesPerSecond) noexcept { m_clientBudget = bytesPerSecond; }
    // **Says what will be missing on every replica** (protocol 44), where a
    // person is developing: an instance of a class the wire does not carry,
    // under one it does -- the name tag a server script hung on a character.
    // Once a class, in the log. Not the classes that are each machine's own by
    // nature (`QuietClasses`), and never in a player's game.
    void setUncarriedWarnings(bool on) noexcept { m_warnUncarried = on; }
    // **Removes the peer that is player `userId`** (`Player:Kick`, ADR 0167):
    // told why, let go, and its player taken out of the world as one who
    // left is. False when no peer is that player -- the host's own is not.
    bool removePlayer(scene::World& world, core::InstanceId root, u32 userId, std::string_view reason);
    // **Who may join is the game's to say, where it says** (ADRs 0218, 0226):
    // every hello that passed the engine's own checks is asked here before it
    // is a player. The answer may come at once, inside the call -- a ban list
    // -- or later through `answerJoin`, when a backend has been asked; the
    // joiner waits, at most `MaxAskReceives`. Unset, everybody is admitted.
    void setJoinGate(std::function<void(const JoinAsk&)> gate) { m_gate = std::move(gate); }
    void answerJoin(u32 ask, JoinAnswer answer) { m_answers[ask] = std::move(answer); }
    // Every welcomed peer's player, as an operator lists them.
    [[nodiscard]] std::vector<PeerSummary> players() const;
    // Each welcomed player's round trip, by `UserId`, guests with their
    // machine's: sorted by `UserId`.
    [[nodiscard]] std::vector<std::pair<u32, u32>> playerPings() const;
    // Words for every welcomed peer (`Announce`), at most
    // `MaxRemovedReasonBytes` of them.
    void announce(std::string_view text);
    // **Sends player `userId` elsewhere** (`Redirect`, ADR 0222): its machine
    // leaves this connection and joins there. False when no peer is it.
    bool redirect(u32 userId, std::string_view address, u16 port, std::string_view pass, std::string_view key,
                  std::string_view world);

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
    // The most players this session seats now (`setMaxPlayers`).
    [[nodiscard]] u32 maxPlayers() const noexcept { return m_maxPlayers; }
    // The longest way any welcomed peer came: the relay over the internet over
    // the same network (ADR 0178).
    [[nodiscard]] net::PeerPath worstPath() const noexcept;
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

    struct Peer;
    // What the budget weighs of one record (ADR 0229).
    struct RecordFacts
    {
        u32 id = 0;
        bool full = false;
    };
    void chooseWithinBudget(Peer& peer, const scene::PlayerComponent* player, std::span<const RecordFacts> records,
                            std::span<const usize> starts, std::vector<u8>& chosen, std::vector<u32>& heldBack);

    struct Peer
    {
        net::PeerId id;
        // The address it came from (`ITransport::addressOf`), nought unknown.
        u64 address = 0;
        // The key it proved in the handshake (ADR 0224), when the transport
        // encrypts.
        std::optional<std::array<u8, 32>> key;
        bool welcomed = false;
        // The newest state this peer has proved it holds. Zero: none yet.
        u64 acked = 0;
        // The snapshot ticks this peer was sent, newest last: an
        // acknowledgement names one of them or is not taken -- a
        // client acknowledging a tick it was never sent would have bought
        // full snapshots for ever.
        std::deque<u64> sentTicks;
        // **The last snapshot sent reliably, in parts**, with what this
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
        // **The machine's other players** (ADR 0205, protocol 48): somebody
        // else at the same screen, in seats 1 to 3. Each is a `Player` of the
        // world like the connection's own, with a number the authority gave;
        // none has a token -- a machine that comes back asks for its seats
        // again. Their intents travel with the first player's, in the one
        // stream, so everything below about that stream is the machine's.
        struct Guest
        {
            u32 userId = 0;
            core::InstanceId player;
        };
        std::array<Guest, 3> guests{};
        // Where each of the machine's characters was at the last tick, the
        // first player's then each seat's: a character that moved since was
        // not at rest, input or none.
        std::array<core::DVec3, 4> lastPlaces{};
        std::array<bool, 4> placed{};
        // Whether `id` is one of this machine's players.
        [[nodiscard]] bool has(u32 user) const noexcept
        {
            if (!welcomed || user == 0)
                return false;
            return userId == user || guests[0].userId == user || guests[1].userId == user || guests[2].userId == user;
        }
        // The seat `user` sits in: 0 the machine's first player, 1 to 3 a
        // guest. 0 for a number that is nobody's here, too: ask `has` first.
        [[nodiscard]] u8 seatOf(u32 user) const noexcept
        {
            for (u8 seat = 0; seat < guests.size(); ++seat) {
                if (user != 0 && guests[seat].userId == user)
                    return static_cast<u8>(seat + 1);
            }
            return 0;
        }
        // The player in a seat, or nothing.
        [[nodiscard]] core::InstanceId playerIn(u8 seat) const noexcept
        {
            if (seat == 0)
                return player;
            return seat <= guests.size() ? guests[seat - 1u].player : core::InstanceId{};
        }
        // **The intent tick whose step the world now holds** -- what a
        // snapshot acknowledges, so the replica compares the position with the
        // prediction that produced it. Zero
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
        // Whose press, and of what (protocol 48: a machine's players each
        // have their own buttons).
        struct CarriedPress
        {
            u8 seat = 0;
            core::NameAtom action;
        };
        std::vector<CarriedPress> carriedPresses;
        // **The names behind its intent numbers** (protocol 37), as its
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
        // **A peer whose clock moved against this one's**. How many
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
        // **How many times this peer has dropped simulated time**, as
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
        // The phases each of its seats was last told it is in (ADR 0231).
        std::array<std::vector<std::string>, 4> phasesSent{};
        std::array<bool, 4> phasesTold{};
        // The scene it was last told the authority is in (ADR 0106), and at
        // which of the authority's loads.
        std::string scene;
        u32 sceneLoad = 0;
        bool sceneSent = false;
        // Whether it has been sent the attributes of the owners that are not
        // spawned -- players, `GlobalScriptService` -- since it was welcomed.
        bool attributesSeeded = false;
        // **The names this peer has been told, by number**: an
        // attribute's or a tag's name travels as text once a connection and
        // as two bytes after.
        std::map<std::string, u16, std::less<>> names;
        // **The ground it holds** (ADR 0135): sent every chunk that differs
        // from the scene, for the scene named here; a scene change sends it
        // again, since the peer then loads that scene's ground.
        bool groundSent = false;
        std::string groundScene;
        // Whether it has been sent anything yet. Until it has, it can be made
        // to wait a tick or two with nothing lost -- which is what lets the
        // ground it is first sent be encoded off the frame.
        bool begun = false;
        // The parts it owns as it was last told (ADR 0099), and the newest
        // tick of theirs it has taken a state from.
        std::vector<u32> owned;
        u64 ownedTick = 0;
        // What it held at each of the last `StateHistory` sends.
        std::deque<PeerInterest> interest;
        // **What its budget held back** (ADR 0229): each changed part it
        // has not been sent, with how much its turn has grown and the
        // snapshot it has waited since. A part sent, or no longer changed
        // from what the peer holds, is not here.
        struct Waiting
        {
            u32 id = 0;
            core::f32 priority = 0.0f;
            u64 since = 0;
        };
        std::vector<Waiting> waiting;
        // **What it holds at a snapshot that held something back**: the
        // state then with those parts as they were before. At any other
        // snapshot it holds the authority's own state then, which the
        // history keeps -- so only these are kept, and only until an
        // acknowledgement passes them.
        std::deque<std::pair<u64, std::shared_ptr<const std::vector<EntityState>>>> heldStates;
        std::shared_ptr<const std::vector<EntityState>> pinnedHeldState;
        // Its budget's bytes not yet spent, and the tick they were last
        // added to; nothing added yet while `budgetTick` is nought.
        core::f64 budgetBytes = 0.0;
        u64 budgetTick = 0;
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
        // **Waiting for the game's word** (`setJoinGate`): the ask it is, the
        // token it would be welcomed with, and for how long it has waited.
        u32 asking = 0;
        PlayerToken askedToken;
        u32 askReceives = 0;
        u64 joinedTick = 0;
        std::string accountId;
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
        // Unreliable messages (ADR 0161): the number the next one to this
        // peer carries, and the last one taken from it.
        u16 unreliableOut = 0;
        u16 unreliableIn = 0;
        bool unreliableHeard = false;

        // **What this peer was last told of a swarm's agents** (ADR 0162), by
        // slot: whether it has the agent, which agent that was, and the
        // position, facing, walk, lift and tag it draws it from.
        struct SwarmSent
        {
            bool known = false;
            // The message that last told it was not acknowledged: told again.
            bool lost = false;
            u32 born = 0;
            u16 tag = 0;
            core::f32 yaw = 0.0f;
            core::f32 walk = 0.0f;
            core::f32 lift = 0.0f;
            u64 tick = 0;
            core::DVec3 position;
            // The direction `yaw` is the angle of.
            core::f32 faceX = 0.0f;
            core::f32 faceZ = -1.0f;
            // Whether a terrain was under it then: its lift is from that, and
            // a replica follows the terrain as it walks. Otherwise it stays at
            // the height it was told.
            bool onTerrain = false;
            // Which message of comings and goings brought it to this peer:
            // its position is not sent until the peer says it took that in.
            u16 addedIn = 0;
        };
        struct SwarmView
        {
            u32 netId = 0;
            // Bytes of positions it may be sent now, and the tick that was
            // last added to.
            core::f64 budget = SwarmByteBurst;
            u64 budgetTick = 0;
            // How many messages of comings and goings it has been sent, and
            // the last of them it said it took in.
            u16 membership = 0;
            u16 membershipTaken = 0;
            std::vector<SwarmSent> agents;
        };
        std::vector<SwarmView> swarms;
        // The messages of positions on their way: which swarm, the tick they
        // were sent, and the agents in each. Acknowledged, one is forgotten;
        // not acknowledged in time, its agents are told again.
        struct SwarmFlight
        {
            u16 sequence = 0;
            u32 netId = 0;
            u64 tick = 0;
            std::vector<u32> slots;
        };
        std::deque<SwarmFlight> swarmFlights;
        u16 swarmSequence = 0;
        // How long an acknowledgement takes to come back, in ticks, smoothed.
        core::f64 swarmAckTicks = 12.0;
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
    // **Who is near what, once a send** (ADR 0228 §2): every entry with a place
    // in a grid of cells the size of the radius it is kept within, and every
    // entry with an owner, so a peer is measured against the cells round it
    // and not against the world.
    void indexInterest(const scene::World& world);
    // Every replicated swarm's agents to every peer (ADR 0162): who came and
    // went, reliably, and where the ones it would draw wrong are.
    void sendSwarms(scene::World& world);
    void sendSwarmTo(scene::World& world, Peer& peer, scene::SwarmComponent& swarm, u32 netId,
                     std::span<const core::DVec3> truths);
    // The tick `sendSwarms` last ran for: once a tick, however often it is asked.
    u64 m_swarmTick = 0;
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
    // What a whole ground is made from, read off the world -- cheap -- and
    // what it is made into, which is the cost: every chunk compressed.
    struct GroundWork;
    [[nodiscard]] GroundWork groundWorkOf(const scene::World& world);
    // **Whether the whole ground is ready to be sent**: at once where
    // it is already encoded, or there is nothing of it to compress, or there
    // are no jobs to compress it in. Otherwise it is being encoded in a job
    // and the answer is no until that has finished.
    [[nodiscard]] bool groundWholeReady(const scene::World& world);
    // One intent a peer, the next in tick order, as this tick's.
    void applyIntents(scene::World& world);
    // Whether nothing the peer's players predict moved since the last tick:
    // their characters, a car one of them drives, a loose body near them.
    // Keeps each character's place for the next tick's question.
    [[nodiscard]] bool stillAround(const scene::World& world, Peer& peer);
    // The atom a peer's intent number stands for here, or none: a number it
    // has not named, or a name nothing here has.
    static void carryPress(Peer& peer, u8 seat, core::NameAtom action);
    static core::NameAtom intentAtom(scene::World& world, Peer& peer, u16 id);
    // The entries that waited for names this peer has now given.
    static void takeNamedIntents(scene::World& world, Peer& peer);
    void sendAttributes(Peer& peer, const std::vector<u32>& entering);
    // The tweens on parts' places a peer is to know (ADR 0254 §2.1): those
    // changed this send, of parts it has, and those in force on parts
    // entering its view.
    void sendCurves(Peer& peer, const std::vector<u32>& entering);

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
    // By the instance's packed id, in id order -- a capture's retirement pass
    // walks it so (R10) -- and a sorted array rather than a tree: it is made
    // again every tick, and a node an instance was most of a capture's cost.
    // A network id is never reused: an instance that leaves and a new one in
    // its slot are two ids.
    std::vector<std::pair<u64, u32>> m_netIds;
    // What a capture works in, kept between ticks so it allocates nothing an
    // instance: this capture's ids before they are sorted, the walk's stack,
    // and the field sets of states the history has let go.
    struct Walked
    {
        core::InstanceId id;
        u32 parentNet = 0;
        core::i32 parentOrder = -1;
    };
    std::vector<std::pair<u64, u32>> m_seenScratch;
    std::vector<Walked> m_walkScratch;
    std::vector<std::shared_ptr<FieldSet>> m_fieldPool;
    // The world the last capture read: another one is read whole.
    const scene::World* m_readWorld = nullptr;
    // Where that world's counts of writes and of restores stood at the
    // capture before: an instance written since is read whatever its bytes
    // say, and a world put back to an earlier state is read whole.
    core::u64 m_readMutations = 0;
    core::u64 m_readRestores = 0;
    // How many captures there have been: whose turn it is to be read again
    // whatever its bytes say, one capture in eight.
    u64 m_captures = 0;
    // **Each class's schema, found once a world** (`schemaFor` walks up the
    // class's ancestors comparing names, and a capture asked it of every
    // instance every tick): by class id, with whether it has been asked.
    std::vector<const generated::ClassDesc*> m_schemaOfClass;
    // 1 once asked; 2 once a class with no schema has been said to be missing
    // on every replica (`setUncarriedWarnings`), or is one not to say it of.
    std::vector<u8> m_schemaAsked;
    bool m_warnUncarried = false;
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
    u32 m_maxPerAddress = DefaultPeersPerAddress;
    u64 m_luauFlags = 0;
    // ADR 0234: the characters asleep at the capture before, by network id
    // in order, and those of the capture being made.
    std::vector<u32> m_asleepBefore;
    std::vector<u32> m_asleepNow;
    // ADR 0229: the budget, and the second its numbers are counted over --
    // when it began, and what it has held back, spent and given so far.
    u32 m_clientBudget = 0;
    u64 m_budgetWindowTick = 0;
    u32 m_windowDeferred = 0;
    core::f64 m_windowSpent = 0.0;
    core::f64 m_windowGiven = 0.0;
    // **Which token each proved key was given** in this run (ADR 0224): the
    // same player again while the server runs, a fresh join after it restarts.
    std::map<std::array<u8, 32>, PlayerToken> m_keyTokens;
    void admit(scene::World& world, core::InstanceId network, net::PeerId id, PlayerToken token);
    void settleAsks(scene::World& world, core::InstanceId network);
    std::function<void(const JoinAsk&)> m_gate;
    std::map<u32, JoinAnswer> m_answers;
    u32 m_nextAsk = 1;
    // Receives so far, and when each address last began connections: the
    // second a flood is counted over, in the authority's own receives.
    u64 m_receives = 0;
    std::map<u64, std::deque<u64>> m_connectsByAddress;
    // The world's atom table, for the strings a message carries. Captured by
    // `send`, which is the only caller that can need it.
    // The last capture's walk, in pre-order, and each network id's place in
    // it -- a peer names ids, and a lookup must not walk the world.
    std::vector<Captured> m_order;
    struct InterestIndex
    {
        // Every entry measured, as before the grid: a radius no grid holds.
        bool everything = true;
        core::f64 cell = 0.0;
        // Whether each entry of `m_order` has a place of its own.
        std::vector<u8> positioned;
        // Entries by cell; a part larger than a cell is in each it covers.
        std::unordered_map<u64, std::vector<u32>> cells;
        // Entries a peer may own: their index, and the owner's user id.
        std::vector<std::pair<u32, u32>> owners;
        // **Each entry's own phase** (ADR 0231), as an atom's id, 0 for
        // none; and whether any has one.
        std::vector<u32> phase;
        bool phased = false;
    };
    InterestIndex m_interest;
    // (network id, place in `m_order`), in id order.
    std::vector<std::pair<u32, u32>> m_orderOfNet;
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
    // **The tweens on parts' places** (ADR 0254 §2.1): each in force, by the
    // part's network id, as its `MovingCurve` message; and this send's news,
    // stops among them.
    std::map<u32, std::vector<u8>> m_curves;
    std::vector<std::pair<u32, std::vector<u8>>> m_curveNews;
    // The tick of the newest curve taken, so each is taken once.
    u64 m_curvesTaken = 0;
    // The instances the last send captured, by network id in order: an
    // instance in here and not in the shadows was known and carried nothing
    // (the shadows keep only what carries something, of the instances).
    std::vector<u32> m_attributeKnown;
    // **One owner's changes in a send**: the attributes whose value is
    // another -- a name and the value's bytes, none for one removed -- and
    // the tags put on and taken off. `whole` instead, for an owner nothing
    // spawns that the session had not seen: everything it has.
    struct AttributeEdit
    {
        AttributeOwner owner;
        std::vector<u8> whole;
        std::vector<std::pair<std::string, std::vector<u8>>> values;
        std::vector<std::string> tagsAdded;
        std::vector<std::string> tagsRemoved;
    };
    std::vector<AttributeEdit> m_attributeEdits;
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
        // send its removal in.
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
    // The collision groups as last sent, and the revision they were
    // read at: the world's, kept through a change of scene.
    std::vector<u8> m_groupsSent;
    u32 m_groupsRevision = 0;
    bool m_groupsRead = false;
    // This send's changes, as whole messages for every peer already holding
    // the ground.
    std::vector<std::vector<u8>> m_groundEdits;
    // **The whole ground, encoded once for every peer that needs it**:
    // a scene change with thirty peers encoded the world thirty times in one
    // tick. Made again when the ground changes (`diffGround` clears it).
    std::vector<std::vector<u8>> m_groundWhole;
    bool m_groundWholeValid = false;
    // The one being encoded in a job, and which ground it is of: the count
    // moves each time the ground changes, and one made of a ground that has
    // changed since is thrown away.
    std::shared_ptr<GroundWork> m_groundWork;
    u64 m_groundGeneration = 0;
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
    // **This machine's other players, seated** (ADR 0205, protocol 48): every
    // local player after the first asks the authority for a seat, and one
    // that is gone gives its seat back. Once a tick, before `sendIntent`;
    // nothing is sent on a tick nobody sat down or stood up.
    void syncGuests(const scene::World& world);
    // This machine dropped simulated time: the intents sent after say so.
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
    // Why a join by code did not reach its host (ADR 0178), or none.
    [[nodiscard]] net::ConnectFailure joinFailure() const noexcept { return m_joinFailure; }
    [[nodiscard]] net::PeerPath path() const noexcept { return m_transport.path(m_authority); }
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
    // What this machine's Luau flags come to (`Config::luauFlags`), said in
    // the hello.
    void setLuauFlags(u64 fingerprint) noexcept { m_luauFlags = fingerprint; }
    // What this machine presents in its hello (`Config::joinTicket`).
    void setTicket(std::string ticket) { m_ticket = std::move(ticket); }
    // And the pass a manager signed for this join (`Config::joinPass`).
    void setPass(std::string pass) { m_pass = std::move(pass); }
    // **Where this replica was sent** (`Redirect`), and how many times: the
    // session joins there.
    [[nodiscard]] u32 redirects() const noexcept { return m_redirects; }
    [[nodiscard]] const std::string& redirectAddress() const noexcept { return m_redirectAddress; }
    [[nodiscard]] u16 redirectPort() const noexcept { return m_redirectPort; }
    [[nodiscard]] const std::string& redirectPass() const noexcept { return m_redirectPass; }
    [[nodiscard]] const std::string& redirectKey() const noexcept { return m_redirectKey; }
    [[nodiscard]] const std::string& redirectWorld() const noexcept { return m_redirectWorld; }
    // **How the own character is stepped again when the authority corrects
    // it** (ADR 0076, as amended). Each tick's prediction keeps the command
    // the physics step consumed; a correction puts the character where the
    // authority said and replays the commands it has not answered yet. Unset,
    // a correction shifts the prediction by the error, as before.
    void setCharacterReplay(scene::ICharacterReplay* replay) noexcept { m_replay = replay; }
    // Whether each correction is said in the log, with what it disagreed about
    // (`Config::logCorrections`).
    void setCorrectionLog(bool log) noexcept { m_logCorrections = log; }
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
    // **Called when the authority first takes this replica**, before
    // anything it sends is applied: where a host clears its world for the
    // server's. Until then the machine is what it was -- a join that fails
    // leaves its menu standing.
    void setWelcomeHandler(std::function<void(scene::World&)> handler) { m_welcomeHandler = std::move(handler); }
    // Why the authority refused this replica, or 0 (`RefusedVersion`,
    // `RefusedFull`).
    [[nodiscard]] u8 refused() const noexcept { return m_refused; }
    // The words that came with it: a removal's, when the host gave some.
    [[nodiscard]] const std::string& refusedText() const noexcept { return m_refusedText; }
    // The newest state applied to the world. Zero before the first.
    [[nodiscard]] u64 appliedTick() const noexcept { return m_applied; }
    // The authority's tick now, as this replica estimates it: never
    // behind the last one applied.
    [[nodiscard]] u64 serverClock() const noexcept { return std::max(m_serverClock, m_applied); }
    // This replica's player number, as the authority's welcome named it.
    [[nodiscard]] u32 playerId() const noexcept { return m_playerId; }
    [[nodiscard]] core::InstanceId localOf(NetId id) const noexcept;
    [[nodiscard]] Stats stats() const noexcept
    {
        Stats now = m_stats;
        now.predictedBodies = static_cast<u32>(m_predictedParts.size());
        now.movingParts = static_cast<u32>(m_movingParts.size());
        for (usize seat = 0; seat < m_seats.size(); ++seat) {
            now.seatPredicted[seat] = m_seats[seat].netId != 0 && m_seats[seat].synced;
            now.seatUser[seat] = seat == 0 ? m_playerId : m_guests[seat - 1].userId;
        }
        return now;
    }
    // Snapshots refused because the reconstruction did not match what the
    // authority described. Zero in a correct build; a test asserts it.
    [[nodiscard]] u64 checksumFailures() const noexcept { return m_checksumFailures; }
    // The rate the authority's world runs at, as its last snapshot said
    // (ADR 0233): 1 at full speed.
    [[nodiscard]] core::f64 timeDilation() const noexcept { return m_timeDilation; }
    // **How fast its own clock should run against its authority's
    // rate**: 1, or a few percent less while its intents queue up over there
    // faster than they are applied -- a clock that runs ahead.
    [[nodiscard]] core::f64 clockPace() const noexcept { return m_slowing ? 1.0 - ClockNudge : 1.0; }
    // Its place in a front door's line, 0 when it is in none (ADR 0232).
    [[nodiscard]] u32 queuePosition() const noexcept { return m_queuePosition; }
    // The loose parts this replica predicts (ADR 0133), and how far, how many
    // and how long.
    void setPrediction(core::f64 radius, u32 maxBodies, u32 lingerTicks) noexcept
    {
        m_predictRadius = radius;
        m_predictMax = maxBodies;
        m_predictLinger = lingerTicks;
    }
    [[nodiscard]] usize predictedCount() const noexcept { return m_predictedParts.size(); }
    // How far ahead a moving part is carried on by its last motion (ADR 0254).
    void setMovingLead(core::f64 milliseconds) noexcept { m_predictLeadMs = milliseconds; }
    // The moving parts drawn at the predicted tick (ADR 0254).
    [[nodiscard]] usize movingCount() const noexcept { return m_movingParts.size(); }
    // An own character's drawn offset (`VisualCorrection`), by seat: 0 the
    // machine's first player, 1 to 3 its guests (ADR 0208).
    [[nodiscard]] VisualCorrection visualCorrection(usize seat = 0) const noexcept
    {
        if (seat >= m_seats.size())
            return {};
        const Owned& one = m_seats[seat];
        return VisualCorrection{one.visualCharacter, one.visualOffset,  one.displaced,
                                one.visualVehicle,   one.vehicleOffset, one.vehicleDisplaced};
    }

private:
    void decayVisualOffset() noexcept;
    // The chassis of the vehicle a character's player drives, and what is
    // drawn sliding with it: the nearest model above it, or the chassis.
    struct DrivenVehicle
    {
        core::InstanceId chassis;
        core::InstanceId root;
    };
    [[nodiscard]] static DrivenVehicle drivenBy(const scene::World& world, core::InstanceId character);
    // Whether a part is a vehicle's chassis.
    [[nodiscard]] static bool carriesVehicle(const scene::World& world, core::InstanceId part);
    // Whether a part carries a vehicle nobody drives that is asked to move --
    // a throttle, a wheel turned -- which is a script's doing.
    [[nodiscard]] static bool drivenByScript(const scene::World& world, core::InstanceId part);
    // Which loose parts are predicted this tick: in, out, and what each
    // starts from.
    void updatePredicted(scene::World& world);
    // `inParts`: it came as `SnapshotPart`s, and is acknowledged reliably --
    // the authority sends nothing after such a one until it hears.
    void onSnapshot(scene::World& world, core::InstanceId root, std::span<const u8> bytes, bool inParts);
    // A part of a snapshot sent reliably: joined, and applied whole
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
    void applyToWorld(scene::World& world, core::InstanceId root, const std::shared_ptr<const WorldState>& applied);
    void resolveCharacters(scene::World& world, core::InstanceId root);
    void onOwnership(scene::World& world, std::span<const u8> bytes);
    void onTilemapBlocks(scene::World& world, std::span<const u8> bytes);
    // The ground (ADR 0135): the workspace's terrain, made if there is none,
    // and the block world, their chunks and their looks.
    void onTerrainChunks(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    void onTerrainLook(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    void onVoxelChunks(scene::World& world, std::span<const u8> bytes);
    void onVoxelTypes(scene::World& world, std::span<const u8> bytes);
    void onCollisionGroups(scene::World& world, std::span<const u8> bytes);
    void onSceneChange(scene::World& world, std::span<const u8> bytes);
    void onAttributes(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    // What changed of one owner's attributes and tags.
    void onAttributeEdits(scene::World& world, core::InstanceId root, std::span<const u8> bytes);
    // Whose an `Attributes` or an `AttributeEdits` is, on this machine.
    [[nodiscard]] core::InstanceId attributeOwner(scene::World& world, core::InstanceId root, u8 owner, u32 id) const;
    // The names the authority has told this connection, by number.
    std::vector<std::string> m_attributeNames;
    // An `UnreliableRemoteEvent` message (ADR 0161), into the world's inbox.
    void onUnreliable(scene::World& world, std::span<const u8> payload);
    // A replicated swarm's agents (ADR 0162): who came and went, and where
    // some of them are.
    void onSwarmAgents(scene::World& world, std::span<const u8> payload);
    void onSwarmState(scene::World& world, std::span<const u8> payload);
    // The last `SwarmState` messages taken in, newest last, and whether one
    // has arrived since they were last said.
    std::vector<u16> m_swarmAcks;
    bool m_swarmAckDue = false;
    // For each swarm this replica mirrors, by network id: the last message
    // of comings and goings it took in.
    std::vector<std::pair<u32, u16>> m_swarmMemberships;
    // Sends since the last acknowledgement.
    u32 m_swarmAckQuiet = 0;
    void sendOwned(const scene::World& world, u64 tick);
    // **The authority's word on one of this machine's characters**, at the
    // intent it last applied: whose, which, and what a replay starts from.
    struct Answer
    {
        u8 seat = 0;
        core::InstanceId character;
        scene::CharacterReplayStart start;
    };
    // **Every own character against its answer** (ADRs 0076, 0133, 0208).
    // Each is compared with what was predicted for it at the answered tick;
    // if any one differs -- or a predicted part, or an attribute -- ALL are
    // put at the authority's word and stepped again together, each by its
    // own player's commands: they are one island. The one that was right
    // comes out where it was.
    void reconcileSeats(scene::World& world, std::span<Answer> answers);
    // The same for a character on the plane: where the authority has
    // the sprite at the intent it last applied, against where this machine
    // had it then.
    struct Owned;
    void reconcile2d(scene::World& world, Owned& seat, core::InstanceId character, core::Vec2 position,
                     core::f32 rotation);
    void interpolate(scene::World& world);
    // **Where a moving part is at the authority's `tick`** (ADR 0254): between
    // the states it was sent, or past the newest carried on by its last motion
    // for at most `leadTicks` -- held where it was if it has stopped or leapt.
    [[nodiscard]] std::optional<core::CFrameD> movingPoseAt(u32 netId, u64 tick, u64 leadTicks) const;
    // **Where a tweened part is at the authority's `tick`**, by the tween's own
    // clock (ADR 0254 §2.1): none for a part with no tween in force, or a tick
    // before the one it was told at.
    [[nodiscard]] std::optional<core::CFrameD> curvePoseAt(u32 netId, u64 tick, core::f64 dt);
    void onMovingCurve(std::span<const u8> bytes);
    [[nodiscard]] const WorldState* stateAt(u64 tick) const noexcept;

    net::ITransport& m_transport;
    net::PeerId m_authority;
    bool m_connected = false;
    bool m_welcomed = false;
    // Unreliable messages (ADR 0161): the number the next one sent carries,
    // and the last one taken.
    u16 m_unreliableOut = 0;
    u16 m_unreliableIn = 0;
    bool m_unreliableHeard = false;
    bool m_lost = false;
    net::ConnectFailure m_joinFailure = net::ConnectFailure::None;
    // Welcomed at least once on any connection, so the next welcome is a rejoin.
    bool m_joinedBefore = false;
    // The authority's scene load this replica last followed, and whether
    // it has heard one since it joined: the first is the join's.
    u32 m_sceneLoad = 0;
    bool m_sceneHeard = false;
    u32 m_freshJoins = 0;
    PlayerToken m_token;
    u64 m_luauFlags = 0;
    std::string m_ticket;
    std::string m_pass;
    u32 m_redirects = 0;
    std::string m_redirectAddress;
    u16 m_redirectPort = 0;
    std::string m_redirectPass;
    std::string m_redirectKey;
    std::string m_redirectWorld;
    u32 m_playerId = 0;
    // **This machine's guests, by seat** (protocol 48): the local player in
    // each of seats 1 to 3, whether the authority has been asked for it on
    // this connection, and the number the authority gave -- 0 until it has.
    // A seat is a player's for as long as that player is here: the others do
    // not move up when one leaves, so nobody is given a new number.
    struct GuestSeat
    {
        core::InstanceId player;
        bool asked = false;
        u32 userId = 0;
    };
    std::array<GuestSeat, 3> m_guests{};
    // The seat a local player sits in: 0 the first, 1 to 3 a seated guest.
    [[nodiscard]] u8 seatOfLocal(core::InstanceId player) const noexcept;
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
    // **The wheels' parts of a vehicle this machine's player drives** (ADR
    // 0213): placed here by the predicted vehicle each step, so neither
    // written from a snapshot nor drawn from the past.
    std::set<u32> m_drivenWheels;
    // What the world was last given, per id, in the authority's terms -- so an
    // apply writes only what changed.
    std::map<u32, FieldSet> m_written;
    // **The state the world was last written from**. A state keeps the
    // field set of an instance a snapshot did not change, so an instance that
    // has in the new state the very set it had in this one has nothing to
    // write -- told by walking the two side by side, both in id order, where
    // every field of every instance was copied and compared every snapshot.
    // Held, so no set of it can be let go and another made in its place.
    //
    // And the instances that are NOT as this state says, whatever their sets:
    // one whose spawn had not arrived, one made again, one with a field still
    // waiting for the instance it names. Each is read until it is settled.
    std::shared_ptr<const WorldState> m_appliedState;
    std::unordered_set<u32> m_unsettled;
    // Ids that have left, and the applied tick they left at: a filter a
    // reconstructed state must pass, for as long as a baseline could still
    // hold them (`DepartedMemoryTicks`), and then forgotten.
    std::map<u32, u64> m_departed;
    std::function<bool(core::InstanceId)> m_probe;
    std::function<void(scene::World&, const std::string&, std::vector<core::u8>)> m_sceneChanger;
    std::function<void(scene::World&)> m_welcomeHandler;
    u8 m_refused = 0;
    std::string m_refusedText;
    std::function<ScriptTemplates*()> m_templates;
    // Husks made since the host last drained them.
    std::vector<core::InstanceId> m_streamedOut;
    std::deque<std::shared_ptr<const WorldState>> m_states;
    u64 m_checksumFailures = 0;
    core::f64 m_timeDilation = 1.0;
    bool m_slowing = false;
    u32 m_queuePosition = 0;
    Stats m_stats;
    // Every player's character by user id, as the last roster named it.
    std::map<u32, u32> m_characters;
    // Predicted parts, by network id, and how many ticks each has been out of
    // range.
    std::map<u32, u32> m_predictedParts;
    // **Moving parts** (ADR 0254), by network id, and how many ticks each
    // has been out of range; and those blended back, by the ticks left.
    std::map<u32, u32> m_movingParts;
    std::map<u32, u32> m_movingLeaving;
    // **The tweens in force on parts' places** (ADR 0254 §2.1), by network id:
    // as the authority told them, and where stepping on from there has come.
    struct Curve
    {
        u8 kind = 0;
        u64 tick = 0;
        scene::TweenTiming timing;
        scene::TweenClock clock;
        core::CFrameD start;
        core::CFrameD goal;
        core::CFrameD now;
        u64 atTick = 0;
        scene::TweenClock atClock;
        core::CFrameD atStart;
        core::CFrameD atNow;
    };
    std::map<u32, Curve> m_curves;
    // How far the last comparison found a predicted part from the authority's.
    core::f64 m_lastBodyCorrection = 0.0;
    // The vehicles the last answer carried, by controller: one not among them is
    // newly predicted, and is stepped again from the authority's whole state.
    std::vector<core::u64> m_seededVehicles;
    bool m_logCorrections = false;
    core::f64 m_predictRadius = DefaultPredictRadius;
    u32 m_predictMax = DefaultPredictMaxBodies;
    u32 m_predictLinger = DefaultPredictLingerTicks;
    core::f64 m_predictLeadMs = DefaultPredictMovingLeadMs;
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
        // **How many ticks further in the past the part is drawn** (ADR
        // 0229 §3), as of this sample: grown by the gaps between the states
        // of a part its authority's budget held back, so it moves between
        // them rather than holding and jumping; shrinking as it is sent on
        // time again.
        core::f32 lag = 0.0f;
    };
    scene::ICharacterReplay* m_replay = nullptr;
    // The last intent the authority applied: one for the machine, whose
    // input is one stream -- every seat's character is answered at it.
    u64 m_ackedIntent = 0;
    // The authority's tick of the snapshot that answered it: predicted intent
    // `N` is stepped there at `m_ackedServerTick + N - m_ackedIntent`.
    u64 m_ackedServerTick = 0;
    // The last `IntentRedundancy` intents sent, encoded, oldest first: each
    // message carries them all (protocol 22).
    std::deque<std::pair<u64, std::vector<u8>>> m_sentIntents;
    // The number each action's name was given on this connection, by
    // atom: given in the order first sent, and sent once, reliably.
    std::map<u32, u16> m_intentIds;
    // How many times this machine has dropped simulated time, sent
    // with every intent.
    u32 m_timeEpoch = 0;
    // The answer the own character was last compared against.
    u64 m_reconciledAck = 0;
    // Interpolation: every remote part's last few snapshot transforms, by
    // server tick, and the server clock they are drawn against.
    std::map<u32, std::deque<Sample>> m_samples;
    // The parts the snapshot being applied says waited for their turn.
    std::vector<u32> m_heldBackNow;
    // The same for every remote `Part2D` (ADR 0103): where it was and how it
    // was turned, in degrees, at one snapshot tick.
    struct Sample2D
    {
        u64 tick = 0;
        core::Vec2 position{0.0f, 0.0f};
        core::f32 rotation = 0.0f;
    };
    std::map<u32, std::deque<Sample2D>> m_samples2d;
    // **One of this machine's own characters** (ADRs 0076, 0208): the first
    // player's, and each seated guest's. Each is stepped here by its own
    // player's input and corrected by the authority's answers; what is kept
    // for that is kept a character.
    struct Owned
    {
        // Its NetId, or zero: no character, or a guest with no number yet.
        u32 netId = 0;
        // Whether it has taken the authority's state since it became this
        // machine's: until it has, nothing is predicted from it. The roster
        // that names it is reliable and the snapshots that place it are not,
        // so a character could be predicted from where it was spawned -- the
        // origin -- and corrected the moment the first answer came.
        bool synced = false;
        // Prediction (ADR 0076): the character after each local tick, by the
        // intent tick that produced it.
        std::deque<Sample> predicted;
        // The same for one that is a `Part2D`: where the sprite was
        // after each local tick.
        std::deque<Sample2D> predicted2d;
        // **Its predicted attributes, as the snapshot being applied has
        // them**: the authority's word, compared with what this
        // replica remembers at the tick it answers. Nothing: it said nothing.
        std::optional<scene::PredictedAttributes> snapshotAttributes;
        // What is left to slide of the corrections so far, and whose.
        core::InstanceId visualCharacter;
        core::DVec3 visualOffset{};
        // An offset was added this tick: it is drawn whole once before it
        // decays. Decayed in the receive that added it, 40% of a
        // correction was a pop on its first frame.
        bool visualFresh = false;
        core::DVec3 displaced{};
        // The same for the vehicle it drives (ADR 0213).
        core::InstanceId visualVehicle;
        core::DVec3 vehicleOffset{};
        bool vehicleFresh = false;
        core::DVec3 vehicleDisplaced{};
    };
    // By seat: 0 the machine's first player, 1 to 3 its guests (`m_guests`).
    std::array<Owned, 4> m_seats{};
    // The seat whose character has this NetId, or null.
    [[nodiscard]] Owned* seatOfNet(u32 netId) noexcept;
    // Whether this NetId is one of this machine's own characters.
    [[nodiscard]] bool ownsNet(u32 netId) const noexcept
    {
        return netId != 0 &&
               std::any_of(m_seats.begin(), m_seats.end(), [netId](const Owned& seat) { return seat.netId == netId; });
    }
    u64 m_serverClock = 0;
    u32 m_interpolationDelay = DefaultInterpolationDelay;
    // **The delay adapts to the link**: what it was set to is the
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
    // least: the link may make it more. Zero applies each snapshot as
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
