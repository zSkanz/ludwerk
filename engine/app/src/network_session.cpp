#include "engine/app/network_session.h"

#include "engine/app/world_host.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/platform.h"
#include "engine/render/transform_history.h"
#include "engine/replication/extract.h"
#include "engine/replication/replication.h"
#include "engine/replication/script_templates.h"
#include "engine/replication/session.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/modules.h"
#include "engine/script/services.h"

#if ENG_ENABLE_REPLICATION
#include "engine/net/transport.h"
#endif

#include <array>
#include <charconv>
#include <string_view>
#include <vector>

namespace engine::app {
namespace {

// `Enum.NetworkState`'s values (ADR 0106).
[[maybe_unused]] constexpr core::i32 StateOffline = 0;
[[maybe_unused]] constexpr core::i32 StateConnecting = 1;
[[maybe_unused]] constexpr core::i32 StateConnected = 2;
[[maybe_unused]] constexpr core::i32 StateHosting = 3;
[[maybe_unused]] constexpr core::i32 StateServing = 4;
[[maybe_unused]] constexpr core::i32 StateReconnecting = 5;

constexpr core::u16 DefaultPort = 7777;

// `host`, `host:port`, or `[address]:port`. **A port that does not parse is
// refused, not replaced** (NA11): `host:77x` used to dial 7777, and the player
// was told nothing answered at an address they never typed.
[[nodiscard]] bool splitAddress(std::string_view address, std::string& host, core::u16& port)
{
    port = DefaultPort;
    std::string_view rest;
    if (address.starts_with('[')) {
        const std::size_t close = address.find(']');
        if (close == std::string_view::npos)
            return false;
        host.assign(address.substr(1, close - 1));
        rest = address.substr(close + 1);
        if (rest.empty())
            return !host.empty();
        if (!rest.starts_with(':'))
            return false;
        rest = rest.substr(1);
    }
    else {
        const std::size_t colon = address.rfind(':');
        if (colon == std::string_view::npos) {
            host.assign(address);
            return !host.empty();
        }
        host.assign(address.substr(0, colon));
        rest = address.substr(colon + 1);
    }
    unsigned value = 0;
    const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), value);
    if (error != std::errc{} || end != rest.data() + rest.size() || value < 1 || value > 65535 || host.empty())
        return false;
    port = static_cast<core::u16>(value);
    return true;
}

// **What a player is told**, from an engine error (D485): its words without
// the `[key]` in front, which is for a log. `HostFailed` and a join that could
// not start handed a game the key with the sentence.
[[nodiscard]] std::string reasonOf(const core::EngineError& error)
{
    const std::string& message = error.message;
    if (message.starts_with('[')) {
        if (const std::size_t end = message.find("] "); end != std::string::npos)
            return message.substr(end + 2);
    }
    return message;
}

// The words for why an authority refused this machine (NA8).
[[maybe_unused]] [[nodiscard]] std::string refusalText(core::u8 reason)
{
#if ENG_ENABLE_REPLICATION
    if (reason == replication::RefusedFull)
        return core::engineCatalog().format(ENG_TR("net.err.refused_full"));
#endif
    (void)reason;
    return core::engineCatalog().format(ENG_TR("net.err.refused_version"));
}

} // namespace

NetworkSession::NetworkSession(std::function<WorldHost*()> host, replication::Config base, TransportFactory transports)
    : m_host(std::move(host)), m_base(std::move(base)), m_transports(std::move(transports))
{
    // **`[network] timeout` is the join's too** (NA28): it reached only the
    // transport, and a join gave up after its own ten seconds whatever the
    // project said.
    if (m_base.timeoutMs > 0)
        m_joinTimeoutSeconds = static_cast<core::f64>(m_base.timeoutMs) / 1000.0;
}

NetworkSession::~NetworkSession()
{
#if ENG_ENABLE_REPLICATION
    if (m_replication != nullptr)
        m_replication->shutdown();
#endif
}

void NetworkSession::setReferenceProbe(std::function<bool(core::InstanceId)> probe)
{
    m_probe = std::move(probe);
#if ENG_ENABLE_REPLICATION
    if (m_replication != nullptr)
        m_replication->setReferenceProbe(m_probe);
#endif
}

void NetworkSession::setCharacterReplay(scene::ICharacterReplay* replay)
{
    m_replay = replay;
#if ENG_ENABLE_REPLICATION
    if (m_replication != nullptr)
        m_replication->setCharacterReplay(replay);
#endif
}

void NetworkSession::setState(core::i32 state)
{
    m_state = state;
    if (WorldHost* host = m_host(); host != nullptr)
        host->world().engineState().networkState = state;
}

std::optional<core::EngineError> NetworkSession::begin(replication::Topology topology, const std::string& address,
                                                       core::u16 port, bool redial)
{
    m_redial = redial;
#if ENG_ENABLE_REPLICATION
    replication::Config config = m_base;
    config.topology = topology;
    config.address = address;
    config.port = port;
    config.redial = redial;
    m_tokenKey.clear();
    if (topology == replication::Topology::Replica) {
        m_tokenKey = address + ":" + std::to_string(port);
        if (const auto known = m_tokens.find(m_tokenKey); known != m_tokens.end())
            config.token = known->second;
    }
    std::optional<core::EngineError> error;
    std::unique_ptr<net::ITransport> transport = m_transports ? m_transports() : net::createEnetTransport();
    m_replication = replication::createReplicationOver(std::move(transport), config, error);
    if (m_replication == nullptr)
        return error.has_value() ? error : core::makeError(ENG_TR("net.err.transport_init_failed"));
    wire();
    return std::nullopt;
#else
    (void)topology;
    (void)address;
    (void)port;
    (void)m_base;
    (void)m_transports;
    return core::makeError(ENG_TR("engine.cli.err.no_replication"));
#endif
}

void NetworkSession::wire()
{
#if ENG_ENABLE_REPLICATION
    if (m_probe)
        m_replication->setReferenceProbe(m_probe);
    m_replication->setCharacterReplay(m_replay);
    // **A replica follows its authority to another scene** (ADR 0106): the same
    // scene, from this machine's own package, cleared of what the authority is
    // about to send, as a join clears it.
    const std::function<WorldHost*()> hostOf = m_host;
    // **Where this machine's own scripts wait for the authority's instances**
    // (ADR 0138 §6): the host's, asked at each spawn, since a reload makes a
    // new host.
    m_replication->setScriptTemplates([hostOf]() -> replication::ScriptTemplates* {
        WorldHost* host = hostOf();
        return host != nullptr ? host->scriptTemplates() : nullptr;
    });
    // **The world becomes the server's when the server takes this machine,
    // not when it asks** (N1): until the welcome a join is only a question,
    // and a join nobody answers leaves the game -- its menu, the address
    // typed in it, the handler waiting for `JoinFailed` -- as it was.
    m_replication->setWelcomeHandler([hostOf](scene::World&) {
        WorldHost* host = hostOf();
        if (host == nullptr)
            return;
        scene::EngineState& state = host->world().engineState();
        if (state.networkTopology == scene::NetworkTopology::Replica)
            return;
        state.networkTopology = scene::NetworkTopology::Replica;
        // Any scene change this machine was making is the server's to make
        // now (audit A10).
        host->cancelSceneChanges();
        // **Joining replaces this machine's scene with the server's**: what
        // the authority replicates is cleared, and server code with it.
        (void)replication::clearForReplica(host->world(), host->workspace(), host->scriptTemplates());
        // And every script is checked against "live" for a replica (ADR
        // 0137 §5): what does not run here stops -- the scene's client code
        // with it, until the server's world is here (D433).
        state.sceneClientHeld = true;
        script::reconcileAllScripts(host->runtime().state());
    });
    m_replication->setSceneChanger([hostOf](scene::World&, const std::string& path, std::vector<core::u8> data) {
        WorldHost* host = hostOf();
        if (host == nullptr)
            return;
        // **Said here as `LoadScene` says it on the authority** (G13): a
        // loading screen hung on `SceneLoading` never showed on a replica.
        script::fireSceneLoading(host->runtime().state(), path);
        if (const std::optional<core::EngineError> error = host->loadScene(path, std::move(data)); error.has_value()) {
            core::logText(core::LogLevel::Error, error->message);
            return;
        }
        // The scene before's templates are no scene's now.
        if (replication::ScriptTemplates* templates = host->scriptTemplates(); templates != nullptr)
            templates->dropSceneTemplates(host->world());
        (void)replication::clearForReplica(host->world(), host->workspace(), host->scriptTemplates());
    });
#endif
}

std::optional<core::EngineError> NetworkSession::start(replication::Topology topology, const std::string& address,
                                                       core::u16 port)
{
    if (topology == replication::Topology::Solo)
        return std::nullopt;
    if (std::optional<core::EngineError> error = begin(topology, address, port, true); error.has_value())
        return error;
    WorldHost* host = m_host();
    if (host == nullptr)
        return std::nullopt;
    switch (topology) {
    case replication::Topology::Replica:
#if ENG_ENABLE_REPLICATION
        // What the authority is about to send, removed from this copy of the
        // scene so it is not everything twice. See `clearReplicated`.
        (void)replication::clearForReplica(host->world(), host->workspace(), host->scriptTemplates());
#endif
        m_connecting = true;
        m_joinStartedNs = m_clock ? m_clock() : platform::nowNs();
        m_address = address;
        setState(StateConnecting);
        break;
    case replication::Topology::Dedicated:
        setState(StateServing);
        break;
    default:
        setState(StateHosting);
        break;
    }
    return std::nullopt;
}

replication::VisualCorrection NetworkSession::visualCorrection() const
{
#if ENG_ENABLE_REPLICATION
    if (m_replication != nullptr)
        return m_replication->visualCorrection();
#endif
    return {};
}

void NetworkSession::receive([[maybe_unused]] bool ticking)
{
#if ENG_ENABLE_REPLICATION
    if (WorldHost* host = m_host(); host != nullptr && m_replication != nullptr)
        m_replication->receive(host->world(), host->workspace(), ticking);
#endif
}

void NetworkSession::send()
{
#if ENG_ENABLE_REPLICATION
    if (WorldHost* host = m_host(); host != nullptr && m_replication != nullptr)
        m_replication->send(host->world(), host->workspace(), host->world().engineState().tick);
#endif
}

void NetworkSession::sendMessages()
{
#if ENG_ENABLE_REPLICATION
    if (WorldHost* host = m_host(); host != nullptr && m_replication != nullptr)
        m_replication->sendMessages(host->world());
#endif
}

void NetworkSession::goSolo(std::string_view event, std::string_view reason, bool wasAuthority)
{
    WorldHost* host = m_host();
#if ENG_ENABLE_REPLICATION
    if (m_replication != nullptr) {
        if (const replication::Status status = m_replication->status(); status.token.valid() && !m_tokenKey.empty())
            m_tokens[m_tokenKey] = status.token;
        m_replication->shutdown();
        m_replication.reset();
    }
#endif
    m_connecting = false;
    m_lostSinceNs = 0;
    if (host == nullptr)
        return;
    scene::World& world = host->world();
    world.engineState().networkTopology = scene::NetworkTopology::Solo;
    world.engineState().networkPeerCount = 0;
    // The world is this machine's own again: its scene's client code runs.
    world.engineState().sceneClientHeld = false;
    setState(StateOffline);

    // **The others go**: a solo game has one player, the one at this machine.
    const core::InstanceId network = scene::networkServiceOf(world, host->runtime().dataModel());
    std::vector<core::InstanceId> others;
    for (core::InstanceId child = network.valid() ? world.firstChild(network) : core::InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        const scene::PlayerComponent* player = world.players().find(child);
        if (player != nullptr && !player->local)
            others.push_back(child);
    }
    for (const core::InstanceId player : others)
        (void)world.destroy(player);
    // **The player at this machine is player 1 again** (N2): the number the
    // server gave it was the server's.
    if (const core::InstanceId local = scene::localPlayerOf(world); local.valid()) {
        if (scene::PlayerComponent* component = world.players().find(local); component != nullptr)
            component->userId = 1;
        world.setName(local, world.atoms().intern("Player1"));
    }

    // This machine decides the world again: what a solo boot of its scene runs,
    // starts (ADR 0137 §5) -- **the scene it joined from** (N2), not the
    // server's run alone with its server code: a client that lost its server
    // became the authority of a world it had only ever been a guest in.
    if (!wasAuthority)
        host->returnToSolo(m_sceneBeforeJoin);
    m_sceneBeforeJoin.clear();
    script::fireNetworkEvent(host->runtime().state(), event, reason);
}

void NetworkSession::update()
{
    WorldHost* host = m_host();
    if (host == nullptr)
        return;
    scene::EngineState& state = host->world().engineState();
    state.networkState = m_state;

    // --- What a script asked for, at the safe point after the frame's ticks.
    if (state.pendingNetwork.has_value()) {
        const scene::EngineState::NetworkRequest request = std::move(*state.pendingNetwork);
        state.pendingNetwork.reset();
        const bool wasAuthority = state.networkTopology != scene::NetworkTopology::Replica;
        using Kind = scene::EngineState::NetworkRequest::Kind;
        switch (request.kind) {
        case Kind::Join: {
            // Whatever this machine was doing on the network, it stops first.
            if (active())
                goSolo("Disconnected", core::engineCatalog().format(ENG_TR("net.info.left")), wasAuthority);
            std::string address;
            core::u16 port = DefaultPort;
            if (!splitAddress(request.address, address, port)) {
                const std::array<core::I18nArg, 1> args{core::I18nArg{"address", std::string_view{request.address}}};
                script::fireNetworkEvent(host->runtime().state(), "JoinFailed",
                                         core::engineCatalog().format(ENG_TR("net.err.bad_address"), args));
                break;
            }
            if (std::optional<core::EngineError> error = begin(replication::Topology::Replica, address, port, false);
                error.has_value()) {
                script::fireNetworkEvent(host->runtime().state(), "JoinFailed", reasonOf(*error));
                break;
            }
            // The world stays this machine's until the server takes it: the
            // welcome handler (`wire`) makes it the server's then.
            m_sceneBeforeJoin = state.currentScene;
            m_connecting = true;
            m_joinStartedNs = m_clock ? m_clock() : platform::nowNs();
            m_address = request.address;
            setState(StateConnecting);
            const std::array<core::I18nArg, 2> args{core::I18nArg{"address", std::string_view{address}},
                                                    core::I18nArg{"port", static_cast<core::i64>(port)}};
            core::log(core::LogLevel::Info, ENG_TR("net.info.joining"), args);
            break;
        }
        case Kind::Host: {
            if (active() && state.networkTopology != scene::NetworkTopology::Replica)
                break; // Hosting already.
            if (active())
                goSolo("Disconnected", core::engineCatalog().format(ENG_TR("net.info.left")), wasAuthority);
            if (std::optional<core::EngineError> error = begin(replication::Topology::Host, {}, request.port, false);
                error.has_value()) {
                // **Said to the game, not only to the log** (NA7): a port in
                // use left `State` at Offline and the script waiting.
                core::logText(core::LogLevel::Error, error->message);
                script::fireNetworkEvent(host->runtime().state(), "HostFailed", reasonOf(*error));
                break;
            }
            state.networkTopology = scene::NetworkTopology::Host;
            script::reconcileAllScripts(host->runtime().state());
            setState(StateHosting);
            const std::array<core::I18nArg, 1> args{core::I18nArg{"port", static_cast<core::i64>(request.port)}};
            core::log(core::LogLevel::Info, ENG_TR("net.info.hosting"), args);
            break;
        }
        case Kind::Disconnect:
            if (active())
                goSolo("Disconnected", core::engineCatalog().format(ENG_TR("net.info.left")), wasAuthority);
            break;
        }
    }

#if ENG_ENABLE_REPLICATION
    if (m_replication == nullptr) {
        state.networkStats = {};
        m_rateStartedNs = 0;
        return;
    }
    const replication::Status status = m_replication->status();
    state.networkServerTick = status.serverTick;
    state.networkPeerCount = status.peerCount;

    // --- How the connection is doing (the multiplayer smoothness brief).
    {
        const replication::Stats stats = m_replication->stats();
        scene::EngineState::NetworkStats& shown = state.networkStats;
        shown.pingMs = static_cast<core::f64>(status.pingMs);
        shown.jitterMs = static_cast<core::f64>(status.jitterMs);
        shown.lossPercent = static_cast<core::f64>(status.loss) * 100.0;
        shown.lastCorrectionMetres = stats.lastCorrectionMetres;
        shown.inputBufferDepth = stats.intentDepth;
        shown.inputStarvations = stats.intentStarvations;
        shown.inputReanchors = stats.intentReanchors;
        shown.corrections = stats.corrections;
        shown.interpolationDelayMs = static_cast<core::f64>(stats.interpolationDelayTicks) * 1000.0 / 60.0;
        shown.predictedParts = stats.predictedBodies;
        const core::u64 now = m_clock ? m_clock() : platform::nowNs();
        const core::u64 snapshots = status.authority ? stats.snapshotsSent : stats.snapshotsReceived;
        if (m_rateStartedNs == 0 || now < m_rateStartedNs) {
            m_rateStartedNs = now;
            m_rateSnapshots = snapshots;
            m_rateCorrections = stats.corrections;
            m_rateResimulations = stats.resimulations;
            m_rateResimulatedTicks = stats.resimulatedTicks;
            m_rateResimulationMicros = stats.resimulationMicros;
        }
        else if (now - m_rateStartedNs >= 1'000'000'000ull) {
            const core::f64 seconds = static_cast<core::f64>(now - m_rateStartedNs) / 1e9;
            shown.snapshotsPerSecond =
                static_cast<core::f64>(snapshots - std::min(snapshots, m_rateSnapshots)) / seconds;
            shown.correctionsPerSecond =
                static_cast<core::f64>(stats.corrections - std::min(stats.corrections, m_rateCorrections)) / seconds;
            const core::u64 resimulations = stats.resimulations - std::min(stats.resimulations, m_rateResimulations);
            shown.resimulationsPerSecond = static_cast<core::f64>(resimulations) / seconds;
            shown.resimulatedTicksPerSecond =
                static_cast<core::f64>(stats.resimulatedTicks -
                                       std::min(stats.resimulatedTicks, m_rateResimulatedTicks)) /
                seconds;
            const core::u64 micros =
                stats.resimulationMicros - std::min(stats.resimulationMicros, m_rateResimulationMicros);
            shown.resimulationMs = resimulations > 0
                                       ? static_cast<core::f64>(micros) / 1000.0 / static_cast<core::f64>(resimulations)
                                       : 0.0;
            m_rateStartedNs = now;
            m_rateSnapshots = snapshots;
            m_rateCorrections = stats.corrections;
            m_rateResimulations = stats.resimulations;
            m_rateResimulatedTicks = stats.resimulatedTicks;
            m_rateResimulationMicros = stats.resimulationMicros;
        }
    }

    // --- A replica: did the join take, and is the server still there? A
    // script's join is still the machine's own world until the welcome, so
    // this is asked whatever the topology says.
    if (m_connecting && status.refused != 0) {
        goSolo("JoinFailed", refusalText(status.refused), state.networkTopology != scene::NetworkTopology::Replica);
        return;
    }
    if (!m_connecting && state.networkTopology != scene::NetworkTopology::Replica)
        return;
    if (m_connecting) {
        if (status.welcomed) {
            m_connecting = false;
            m_lostSinceNs = 0;
            m_seenFreshJoins = status.freshJoins;
            setState(StateConnected);
            // **The world is the server's now, and the scene's client code
            // starts in it** (D433) -- before `Connected` is raised, so what
            // lives across worlds hears it with the scene already running.
            host->holdSceneClientCode(false);
            script::fireNetworkEvent(host->runtime().state(), "Connected", std::nullopt);
        }
        // A command-line join dials until the server answers; a script's
        // gives up after its timeout, by the clock (audit A3).
        else if (!m_redial &&
                 (status.lost || static_cast<core::f64>((m_clock ? m_clock() : platform::nowNs()) - m_joinStartedNs) >
                                     m_joinTimeoutSeconds * 1'000'000'000.0)) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"address", std::string_view{m_address}}};
            goSolo("JoinFailed", core::engineCatalog().format(ENG_TR("net.err.join_failed"), args),
                   state.networkTopology != scene::NetworkTopology::Replica);
        }
        return;
    }
    // A join from the command line dials again, as it always has (ADR 0085);
    // one a script made hands the decision back to the game.
    if (status.refused != 0) {
        goSolo("Disconnected", refusalText(status.refused), false);
        return;
    }
    if (status.lost && !m_redial) {
        goSolo("Disconnected", core::engineCatalog().format(ENG_TR("net.info.server_gone")), false);
        return;
    }
    // **A redial is a state, and it has an end** (D432). While the server is
    // gone `State` said `Connected`, nothing fired and `ServerTick` stood
    // still, for ever: a game could tell its player nothing. It is
    // `Connecting` while this machine dials, and after `[network] timeout`
    // it gives up -- `Disconnected`, and solo, as a script's own join does.
    if (!status.welcomed) {
        const core::u64 now = m_clock ? m_clock() : platform::nowNs();
        if (m_lostSinceNs == 0) {
            m_lostSinceNs = now != 0 ? now : 1;
            // **Its own state** (N13): a game can say "reconnecting" and stop
            // predicting, where `Connecting` was what a first join says.
            setState(StateReconnecting);
        }
        else if (static_cast<core::f64>(now - std::min(now, m_lostSinceNs)) > m_joinTimeoutSeconds * 1'000'000'000.0) {
            m_lostSinceNs = 0;
            goSolo("Disconnected", core::engineCatalog().format(ENG_TR("net.info.server_gone")), false);
        }
        return;
    }
    if (m_lostSinceNs != 0) {
        // **It answered.** As the same player, the world this machine held is
        // the one it goes on in. As somebody new -- a server that restarted
        // knows nobody -- it is a fresh join: the world was replaced, so the
        // scene's client code starts again in it.
        m_lostSinceNs = 0;
        const bool fresh = status.freshJoins != m_seenFreshJoins;
        m_seenFreshJoins = status.freshJoins;
        setState(StateConnected);
        if (fresh) {
            host->holdSceneClientCode(true);
            host->holdSceneClientCode(false);
        }
        script::fireNetworkEvent(host->runtime().state(), "Connected", std::nullopt);
    }
#endif
}

void receiveDrawn(WorldHost& host, NetworkSession& network, render::TransformHistory& history, bool ticking)
{
    const core::DVec3 before = network.visualCorrection().displaced;
    network.receive(ticking);
    const replication::VisualCorrection after = network.visualCorrection();
    const core::DVec3 by{after.displaced.x - before.x, after.displaced.y - before.y, after.displaced.z - before.z};
    if (after.character.valid() && (by.x != 0.0 || by.y != 0.0 || by.z != 0.0))
        history.shift(host.world(), after.character, by);
}

void runDrawnTick(WorldHost& host, NetworkSession& network, render::TransformHistory& history)
{
    history.capture(host.world());
    receiveDrawn(host, network, history, true);
    host.tick();
    network.send();
    network.sendMessages();
}

} // namespace engine::app
