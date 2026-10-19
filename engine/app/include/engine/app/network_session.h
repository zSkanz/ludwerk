// The connection a host holds, and the way a script changes it (ADR 0106).
//
// **One place owns the network, and a script only asks.** `NetworkService`'s
// `Join`, `Host` and `Disconnect` leave a request in the world's engine state;
// `update` carries it out at the safe point after the frame's ticks, starts or
// stops the replication, and says what happened through `State` and the
// `Connected`, `JoinFailed` and `Disconnected` events. The command line's
// `--host`, `--serve` and `--join=` are `start`, the same calls made before the
// first tick.
//
// **What changes with the posture** (ADR 0105 §2):
//
// - joining: this machine stops deciding the world. What the authority is
//   about to send is cleared, server code goes with it, and the server's scene
//   arrives;
// - leaving, or a server that goes away: this machine is solo again in the
//   scene it is in, the other players go, and server code starts again, fresh.
//
// The transport is a factory so a test runs two of these over the memory
// transport, in one process, as the engine runs one over ENet.
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/app/match_keys.h"
#include "engine/core/crypto.h"
#include "engine/core/error.h"
#include "engine/core/types.h"
#include "engine/replication/types.h"

namespace engine::net {
class ITransport;
}

namespace engine::replication {
class IReplication;
}

namespace engine::scene {
class ICharacterReplay;
struct EngineState;
} // namespace engine::scene

namespace engine::render {
class TransformHistory;
}

namespace engine::app {

class WorldHost;

class NetworkSession
{
public:
    using TransportFactory = std::function<std::unique_ptr<net::ITransport>()>;

    // `host` answers the host this session serves -- a function, because a
    // hot reload replaces the host. `base` is the replication's configuration
    // apart from its posture: snapshot rate, interpolation, the peer cap.
    // Absent a factory, ENet.
    NetworkSession(std::function<WorldHost*()> host, replication::Config base, TransportFactory transports = nullptr);
    ~NetworkSession();

    NetworkSession(const NetworkSession&) = delete;
    NetworkSession& operator=(const NetworkSession&) = delete;

    // **How this machine's matches are encrypted** (ADR 0224). On unless a
    // development run says otherwise; with nothing set, a key for this run
    // and every host's key taken -- what a test runs by. The engine gives a
    // kept key, the project's id in the prologue and the hosts it pinned.
    struct MatchSecurity
    {
        bool encrypt = true;
        core::X25519Pair identity = core::x25519Generate();
        std::vector<core::u8> prologue;
        // Where the hosts this machine met are pinned; empty pins none.
        std::filesystem::path pins;
        // **What a ticket is checked against** (ADR 0226): the game's backend's
        // key, `[network] ticket_public_key`, and `[project] id`.
        std::optional<std::array<core::u8, 32>> ticketKey;
        std::string projectId;
        // **The game's own server and its key** (`[network] server` and
        // `server_key`): known before the first join, so not even that one
        // is taken on trust. Empty for a game that names none.
        std::string server;
        std::optional<std::array<core::u8, 32>> serverKey;
    };
    void setSecurity(MatchSecurity security);
    // **Who may join this machine's match** (ADRs 0218, 0226): asked of every
    // joiner that passed the engine's checks. An answer refuses or admits at
    // once; none admits. Kept across matches.
    using JoinGate = std::function<std::optional<replication::JoinAnswer>(replication::JoinAsk&)>;
    void setJoinGate(JoinGate gate) { m_joinGate = std::move(gate); }
    // **And after the game has named an account** (ADR 0226): a refusal for
    // that account -- a ban -- or nothing.
    using AccountGate = std::function<std::optional<replication::JoinAnswer>(const std::string& account)>;
    void setAccountGate(AccountGate gate) { m_accountGate = std::move(gate); }
    // **What every handshake of a game binds**: the protocol's version and the
    // project's id, so another game, or another build of this one, fails the
    // handshake rather than joining.
    [[nodiscard]] static std::vector<core::u8> prologueFor(std::string_view projectId);
    [[nodiscard]] const MatchSecurity& security() const noexcept { return m_security; }

    // **The command line's posture**, before the first tick: `--host`,
    // `--serve` or `--join=`. A join here keeps the dial-again behaviour a
    // launched client always had (ADR 0085).
    [[nodiscard]] std::optional<core::EngineError> start(replication::Topology topology, const std::string& address,
                                                         core::u16 port);

    // The frame's traffic, as `IReplication` has it. No-ops while solo.
    // `ticking`: whether a simulation tick follows (`IReplication::receive`).
    void receive(bool ticking = true);
    void send();
    void sendMessages();

    // After the frame's ticks: carries out what a script asked for, notices a
    // join that succeeded or failed and a connection that ended, and writes
    // what `NetworkService` reports.
    void update();

    [[nodiscard]] bool active() const noexcept { return m_replication != nullptr; }
    // This machine dropped simulated time this frame: a replica tells
    // its authority.
    void noteTimeDropped();
    // **Snapshots held while a server catches up** (ADR 0228 §7), and whether
    // the last send sent one: the frame loop paces them by real time.
    void holdSnapshots(bool held);
    [[nodiscard]] bool snapshotSent() const;
    // The own character's drawn offset after a correction (a replica's), for
    // the renderer's transform history.
    [[nodiscard]] replication::VisualCorrection visualCorrection(core::usize seat = 0) const;
    [[nodiscard]] replication::IReplication* replication() noexcept { return m_replication.get(); }

    // Handed to every replication this session starts.
    void setReferenceProbe(std::function<bool(core::InstanceId)> probe);
    void setCharacterReplay(scene::ICharacterReplay* replay);

    // **How long a join a script made may wait for the server's welcome**, in
    // seconds of wall time, before it is `JoinFailed`. Counted in
    // updates it was ten seconds at sixty frames, four at 144, and moments in
    // a minimised window. A join from the command line has no limit: it dials
    // until the server answers, as it does when a server goes away (ADR 0085).
    void setJoinTimeout(core::f64 seconds) noexcept { m_joinTimeoutSeconds = seconds; }
    // The code this machine's join is by (ADR 0178); empty for one by address.
    [[nodiscard]] const std::string& joinedByCode() const noexcept { return m_relay.code; }
    // The clock the timeout is read from; `platform::nowNs` unless a test
    // gives another.
    void setClock(std::function<core::u64()> nowNs) { m_clock = std::move(nowNs); }

private:
    // The relay a match is started with (ADR 0178): where a host registers,
    // or, with `code`, where a join asks for the host that holds it.
    struct RelayUse
    {
        std::string relay;
        std::string code;
        bool direct = true;
        // The ticket a join presents (ADR 0226).
        std::string ticket;
        // And the pass a `Redirect` handed over (ADR 0222), in base64, with the
        // key of the world it is for: that one, and no other.
        std::string pass;
        std::string serverKey;
    };
    [[nodiscard]] std::optional<core::EngineError> begin(replication::Topology topology, const std::string& address,
                                                         core::u16 port, bool redial, const RelayUse& relay);
    // What `RelayState` and `JoinCode` read, and `RelayStateChanged` when
    // either is another.
    void noteRelay(core::i32 relayState, const std::string& joinCode);
    // **Who this machine takes a host's key from** (ADR 0224): the one a
    // world named, the game's own server's, or the pinned one.
    [[nodiscard]] std::function<bool(std::string_view, core::u16, const std::array<core::u8, 32>&)>
    trustedHosts(std::string named) const;
    // `SignInAsync` and `RegisterAsync` (ADR 0237): each a connection of its
    // own to a front door, open until it answers.
    struct SignIn;
    void signIns(scene::EngineState& state);
    std::vector<std::unique_ptr<SignIn>> m_signIns;
    void wire();
    void goSolo(std::string_view event, std::string_view reason, bool wasAuthority);
    void setState(core::i32 state);

    std::function<WorldHost*()> m_host;
    replication::Config m_base;
    TransportFactory m_transports;
    MatchSecurity m_security;
    JoinGate m_joinGate;
    // The `Redirect`s this match's replica has been sent, followed.
    core::u32 m_seenRedirects = 0;
    // Where this machine first joined, and whether it is somewhere it was
    // sent since.
    struct Home
    {
        std::string address;
        core::u16 port = 0;
        bool redial = false;
        RelayUse relay;
    };
    std::optional<Home> m_home;
    bool m_awayFromHome = false;
    AccountGate m_accountGate;
    // Joiners the game's `OnJoinRequest` is deciding about, by ask, and when
    // each is a refusal for want of an answer.
    std::map<core::u32, core::u64> m_validating;
    std::unique_ptr<KeyPins> m_pins;
    std::unique_ptr<replication::IReplication> m_replication;
    std::function<bool(core::InstanceId)> m_probe;
    scene::ICharacterReplay* m_replay = nullptr;
    // A join waiting for its welcome, and when it began.
    bool m_connecting = false;
    core::u64 m_joinStartedNs = 0;
    // The last place in a front door's line this join was told (ADR 0232).
    core::u32 m_queuedAt = 0;
    // **A connection that went, being dialled again**: since when, by
    // the clock, or zero while the server is there. And how many fresh joins
    // the replica had counted when this machine last heard `Connected`.
    core::u64 m_lostSinceNs = 0;
    core::u32 m_seenFreshJoins = 0;
    core::f64 m_joinTimeoutSeconds = 10.0;
    std::function<core::u64()> m_clock;
    // The per-second figures of `EngineState::NetworkStats`, counted over the
    // last whole second.
    core::u64 m_rateStartedNs = 0;
    core::u64 m_rateSnapshots = 0;
    core::u64 m_rateCorrections = 0;
    std::array<core::u64, 4> m_rateSeatCorrections{};
    core::u64 m_rateResimulations = 0;
    core::u64 m_rateResimulatedTicks = 0;
    core::u64 m_rateResimulationMicros = 0;
    std::string m_address;
    // The relay this match was started with, and the code it was joined by.
    RelayUse m_relay;
    core::i32 m_relayState = 0;
    std::string m_joinCode;
    // The scene this machine was in when a script joined: where it goes
    // back to when the match ends.
    std::string m_sceneBeforeJoin;
    // **Who this machine was on each server it joined**, by
    // `address:port`: a script's `Join` after a drop presents the same token,
    // and the authority welcomes the same player back -- the `UserId` a game
    // keyed a score on. Kept for the life of the process, never on disk.
    std::map<std::string, replication::PlayerToken> m_tokens;
    std::string m_tokenKey;
    // Whether this connection dials again when it drops: a command-line join
    // does, a script's does not.
    bool m_redial = true;
    // The port `begin` was given: the match this machine hosts or joined.
    core::u16 m_port = 0;
    // `Enum.NetworkState`'s value, kept here and written into the world every
    // update: a hot reload replaces the world, and the connection outlives it.
    core::i32 m_state = 0;
};

// **One tick of a drawn frame, in the order a frame runs it** (ADR 0134):
// where everything was, captured BEFORE the snapshot moves anything -- so
// another player's character is drawn getting where the snapshot put it --
// then the receive (`receiveDrawn`), the tick, and what it produced, sent. The
// engine's loop and the tests that hold its drawing down both run this.
void runDrawnTick(WorldHost& host, NetworkSession& network, render::TransformHistory& history);

// **What arrived, with the drawing kept whole**: a correction of the own
// character moves its last captured place -- and everything under it -- by as
// much as it moved the character, on a frame with a tick or without one.
void receiveDrawn(WorldHost& host, NetworkSession& network, render::TransformHistory& history, bool ticking);

} // namespace engine::app
