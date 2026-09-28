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

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

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
    // The own character's drawn offset after a correction (a replica's), for
    // the renderer's transform history.
    [[nodiscard]] replication::VisualCorrection visualCorrection() const;
    [[nodiscard]] replication::IReplication* replication() noexcept { return m_replication.get(); }

    // Handed to every replication this session starts.
    void setReferenceProbe(std::function<bool(core::InstanceId)> probe);
    void setCharacterReplay(scene::ICharacterReplay* replay);

    // **How long a join a script made may wait for the server's welcome**, in
    // seconds of wall time, before it is `JoinFailed` (audit A3). Counted in
    // updates it was ten seconds at sixty frames, four at 144, and moments in
    // a minimised window. A join from the command line has no limit: it dials
    // until the server answers, as it does when a server goes away (ADR 0085).
    void setJoinTimeout(core::f64 seconds) noexcept { m_joinTimeoutSeconds = seconds; }
    // The clock the timeout is read from; `platform::nowNs` unless a test
    // gives another.
    void setClock(std::function<core::u64()> nowNs) { m_clock = std::move(nowNs); }

private:
    [[nodiscard]] std::optional<core::EngineError> begin(replication::Topology topology, const std::string& address,
                                                         core::u16 port, bool redial);
    void wire();
    void goSolo(std::string_view event, std::string_view reason, bool wasAuthority);
    void setState(core::i32 state);

    std::function<WorldHost*()> m_host;
    replication::Config m_base;
    TransportFactory m_transports;
    std::unique_ptr<replication::IReplication> m_replication;
    std::function<bool(core::InstanceId)> m_probe;
    scene::ICharacterReplay* m_replay = nullptr;
    // A join waiting for its welcome, and when it began.
    bool m_connecting = false;
    core::u64 m_joinStartedNs = 0;
    core::f64 m_joinTimeoutSeconds = 10.0;
    std::function<core::u64()> m_clock;
    // The per-second figures of `EngineState::NetworkStats`, counted over the
    // last whole second.
    core::u64 m_rateStartedNs = 0;
    core::u64 m_rateSnapshots = 0;
    core::u64 m_rateCorrections = 0;
    std::string m_address;
    // **Who this machine was on each server it joined** (D207), by
    // `address:port`: a script's `Join` after a drop presents the same token,
    // and the authority welcomes the same player back -- the `UserId` a game
    // keyed a score on. Kept for the life of the process, never on disk.
    std::map<std::string, replication::PlayerToken> m_tokens;
    std::string m_tokenKey;
    // Whether this connection dials again when it drops: a command-line join
    // does, a script's does not.
    bool m_redial = true;
    // `Enum.NetworkState`'s value, kept here and written into the world every
    // update: a hot reload replaces the world, and the connection outlives it.
    core::i32 m_state = 0;
};

} // namespace engine::app
