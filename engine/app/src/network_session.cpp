#include "engine/app/network_session.h"

#include "engine/app/luau_flags.h"
#include "engine/app/world_host.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/profile.h"
#include "engine/net/secure_transport.h"
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
// How long the game's `OnJoinRequest` may take before its joiner is refused
// (ADR 0226 §1).
[[maybe_unused]] constexpr core::u64 JoinRequestNs = 10'000'000'000ull;

// `host`, `host:port`, or `[address]:port`. **A port that does not parse is
// refused, not replaced**: `host:77x` used to dial 7777, and the player
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

// **What a player is told**, from an engine error: its words without
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

// The words for why an authority refused this machine.
// **What a join is to**, from what a player typed (ADR 0178): a host's code
// where a relay is known and the text is one, or written whole as
// `relay://host:port/CODE`; an address otherwise. A host whose NAME happens to
// read as a code is written with its port, which no code has.
struct JoinTarget
{
    bool byCode = false;
    std::string relay;
    std::string code;
};

[[nodiscard]] JoinTarget joinTargetOf(std::string_view typed, std::string_view relay)
{
    constexpr std::string_view Scheme = "relay://";
    JoinTarget target;
    if (typed.starts_with(Scheme)) {
        typed.remove_prefix(Scheme.size());
        const std::size_t slash = typed.rfind('/');
        if (slash == std::string_view::npos)
            return target;
        relay = typed.substr(0, slash);
        typed = typed.substr(slash + 1);
        if (relay.empty() || !net::rendezvous::parseCode(typed).has_value())
            return target;
    }
    else if (relay.empty() || typed.find_first_of(".:[") != std::string_view::npos ||
             !net::rendezvous::parseCode(typed).has_value()) {
        return target;
    }
    target.byCode = true;
    target.relay = std::string(relay);
    target.code = net::rendezvous::parseCode(typed)->str();
    return target;
}

// Why a join by code did not reach its host, in the game's player's words.
[[maybe_unused]] [[nodiscard]] std::string joinFailureText(core::u8 failure, const std::string& relay,
                                                           const std::string& code)
{
    const std::array<core::I18nArg, 2> args{core::I18nArg{"relay", std::string_view{relay}},
                                            core::I18nArg{"code", std::string_view{code}}};
    switch (static_cast<net::ConnectFailure>(failure)) {
    case net::ConnectFailure::RelayUnreachable:
        return core::engineCatalog().format(ENG_TR("net.err.join_relay_unreachable"), args);
    case net::ConnectFailure::NoSession:
        return core::engineCatalog().format(ENG_TR("net.err.join_no_session"), args);
    case net::ConnectFailure::Full:
        return core::engineCatalog().format(ENG_TR("net.err.refused_full"));
    case net::ConnectFailure::Refused:
        return core::engineCatalog().format(ENG_TR("net.err.join_kept_away"), args);
    case net::ConnectFailure::Busy:
        return core::engineCatalog().format(ENG_TR("net.err.join_relay_busy"), args);
    case net::ConnectFailure::KeyChanged:
        return core::engineCatalog().format(ENG_TR("net.err.join_key_changed"), args);
    case net::ConnectFailure::Insecure:
        return core::engineCatalog().format(ENG_TR("net.err.join_insecure"), args);
    case net::ConnectFailure::None:
    case net::ConnectFailure::TimedOut:
        break;
    }
    return core::engineCatalog().format(ENG_TR("net.err.join_no_path"), args);
}

[[maybe_unused]] [[nodiscard]] std::string refusalText(core::u8 reason, const std::string& words)
{
#if ENG_ENABLE_REPLICATION
    if (reason == replication::RefusedFull)
        return core::engineCatalog().format(ENG_TR("net.err.refused_full"));
    if (reason == replication::RefusedAddressLimit)
        return core::engineCatalog().format(ENG_TR("net.err.refused_address_limit"));
    if (reason == replication::RefusedLuauFlags)
        return core::engineCatalog().format(ENG_TR("net.err.refused_luau_flags"));
    if (reason == replication::RefusedInsecure)
        return core::engineCatalog().format(ENG_TR("net.err.refused_insecure"));
    if (reason == replication::RefusedServerKey)
        return core::engineCatalog().format(ENG_TR("net.err.join_key_changed"));
    if (reason == replication::RefusedNotEncrypted)
        return core::engineCatalog().format(ENG_TR("net.err.join_insecure"));
    // The game's own words for a refusal, or the engine's where it gave none.
    if (reason == replication::RefusedByGame)
        return words.empty() ? core::engineCatalog().format(ENG_TR("net.err.refused_by_game")) : words;
    // The game's own words for a removal, or the engine's where it gave none.
    if (reason == replication::RefusedRemoved)
        return words.empty() ? core::engineCatalog().format(ENG_TR("net.info.removed")) : words;
#endif
    (void)reason;
    (void)words;
    return core::engineCatalog().format(ENG_TR("net.err.refused_version"));
}

} // namespace

NetworkSession::NetworkSession(std::function<WorldHost*()> host, replication::Config base, TransportFactory transports)
    : m_host(std::move(host)), m_base(std::move(base)), m_transports(std::move(transports))
{
    // **`[network] timeout` is the join's too**: it reached only the
    // transport, and a join gave up after its own ten seconds whatever the
    // project said.
    if (m_base.timeoutMs > 0)
        m_joinTimeoutSeconds = static_cast<core::f64>(m_base.timeoutMs) / 1000.0;
}

std::vector<core::u8> NetworkSession::prologueFor(std::string_view projectId)
{
#if ENG_ENABLE_REPLICATION
    return replication::matchPrologue(projectId);
#else
    return std::vector<core::u8>(projectId.begin(), projectId.end());
#endif
}

void NetworkSession::setSecurity(MatchSecurity security)
{
    m_security = std::move(security);
    m_pins = m_security.pins.empty() ? nullptr : std::make_unique<KeyPins>(m_security.pins);
}

// **A sign-in on its way** (ADR 0237): its own connection to the front door,
// the message sent once the handshake is done, and how long it may take --
// a minute, past a name's longest delay and the hash behind it.
struct NetworkSession::SignIn
{
    core::u32 id = 0;
    std::unique_ptr<net::ITransport> transport;
    net::PeerId peer{};
    std::vector<core::u8> message;
    core::u64 deadlineNs = 0;
    bool done = false;
};

namespace {

[[maybe_unused]] constexpr core::u64 SignInWaitNs = 60ull * 1'000'000'000ull;
// `Enum.SignInFailure.Unreachable`: said here, never by a door.
constexpr core::u8 SignInUnreachable = 7;

} // namespace

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

void NetworkSession::noteRelay(core::i32 relayState, const std::string& joinCode)
{
    WorldHost* host = m_host();
    if (host == nullptr)
        return;
    scene::EngineState& state = host->world().engineState();
    state.networkRelayState = relayState;
    state.networkJoinCode = joinCode;
    if (relayState == m_relayState && joinCode == m_joinCode)
        return;
    const core::i32 before = m_relayState;
    m_relayState = relayState;
    m_joinCode = joinCode;
    // Said once each way: the code when the relay has it, and the silence when
    // a relay that was answering stops.
    const std::array<core::I18nArg, 2> args{core::I18nArg{"relay", std::string_view{m_relay.relay}},
                                            core::I18nArg{"code", std::string_view{joinCode}}};
    if (relayState == static_cast<core::i32>(net::RelayState::Ready) && relayState != before)
        core::log(core::LogLevel::Info, ENG_TR("net.info.relay_ready"), args);
    else if (relayState == static_cast<core::i32>(net::RelayState::Unreachable) && relayState != before)
        core::log(core::LogLevel::Warn, ENG_TR("net.warn.relay_unreachable"), args);
    script::fireNetworkEvent(host->runtime().state(), "RelayStateChanged", std::nullopt);
}

std::optional<core::EngineError> NetworkSession::begin(replication::Topology topology, const std::string& address,
                                                       core::u16 port, bool redial, const RelayUse& relay)
{
    m_redial = redial;
    m_port = port;
    m_relay = relay;
    // **Where this machine first joined**, with no pass: what a dropped
    // connection to a world it was sent to goes back to (ADR 0220 §5).
    if (topology == replication::Topology::Replica && relay.pass.empty()) {
        m_home = Home{address, port, redial, relay};
        m_awayFromHome = false;
    }
#if ENG_ENABLE_REPLICATION
    replication::Config config = m_base;
    config.topology = topology;
    config.address = address;
    config.port = port;
    config.redial = redial;
    config.relay = relay.relay;
    config.joinCode = relay.code;
    config.relayDirect = relay.direct;
    config.joinTicket = relay.ticket;
    config.joinPass = relay.pass;
    m_seenRedirects = 0;
    config.luauFlags = luauFlagsFingerprint();
    m_tokenKey.clear();
    if (topology == replication::Topology::Replica) {
        // A host is the same host by its code as by its address, to the
        // player it welcomed before.
        m_tokenKey = relay.code.empty() ? address + ":" + std::to_string(port) : relay.relay + "/" + relay.code;
        if (const auto known = m_tokens.find(m_tokenKey); known != m_tokens.end())
            config.token = known->second;
    }
    std::optional<core::EngineError> error;
    std::unique_ptr<net::ITransport> transport = m_transports ? m_transports() : net::createEnetTransport();
    // **Every match encrypted** (ADR 0224), unless a development run said
    // otherwise: around whatever carries it, so the relay carries what it
    // cannot read.
    if (m_security.encrypt) {
        net::SecureConfig secure;
        secure.identity = m_security.identity;
        secure.prologue = m_security.prologue;
        secure.plainRefusal = replication::refusalMessage(replication::RefusedInsecure);
        secure.trust = trustedHosts(relay.serverKey);
        // **A host says its key**, for its players to be given and its
        // operator to check (ADR 0224 §3).
        if (topology != replication::Topology::Replica) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"key", core::toHex(m_security.identity.pub)}};
            core::log(core::LogLevel::Info, ENG_TR("net.info.match_key"), args);
        }
        transport = net::createSecureTransport(std::move(transport), std::move(secure));
    }
    m_replication = replication::createReplicationOver(std::move(transport), config, error);
    if (m_replication == nullptr)
        return error.has_value() ? error : core::makeError(ENG_TR("net.err.transport_init_failed"));
    if (topology != replication::Topology::Replica) {
        // **Who may join** (ADRs 0218, 0226): the engine's own refusals -- a
        // ban, a server stopping -- at once; then the game's `OnJoinRequest`
        // where it set one, between ticks; else everybody.
        m_validating.clear();
        replication::IReplication* replica = m_replication.get();
        m_replication->setJoinGate([this, replica](const replication::JoinAsk& asked) {
            replication::JoinAsk ask = asked;
            if (m_joinGate) {
                if (std::optional<replication::JoinAnswer> refusal = m_joinGate(ask); refusal.has_value()) {
                    replica->answerJoin(ask.id, std::move(*refusal));
                    return;
                }
            }
            WorldHost* host = m_host();
            if (host == nullptr || (!host->world().engineState().validateJoinSet && ask.accountId.empty())) {
                replica->answerJoin(ask.id, replication::JoinAnswer{});
                return;
            }
            // **An account a manager's pass names**, with no function of the
            // game's to ask: admitted as one a game named, through the same
            // ban and the same one session an account.
            if (!host->world().engineState().validateJoinSet) {
                host->world().engineState().joinVerdicts.push_back(scene::EngineState::JoinVerdict{
                    .id = ask.id, .admit = true, .reason = {}, .accountId = ask.accountId});
                m_validating[ask.id] = (m_clock ? m_clock() : platform::nowNs()) + JoinRequestNs;
                return;
            }
            host->world().engineState().joinValidations.push_back(
                scene::EngineState::JoinValidation{.id = ask.id,
                                                   .publicKey = ask.publicKey,
                                                   .address = ask.address,
                                                   .ticket = ask.ticket,
                                                   .accountId = ask.accountId});
            m_validating[ask.id] = (m_clock ? m_clock() : platform::nowNs()) + JoinRequestNs;
        });
    }
    wire();
    return std::nullopt;
#else
    (void)topology;
    (void)address;
    (void)port;
    (void)relay;
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
    // not when it asks**: until the welcome a join is only a question,
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
        // now.
        host->cancelSceneChanges();
        // **Joining replaces this machine's scene with the server's**: what
        // the authority replicates is cleared, and server code with it.
        (void)replication::clearForReplica(host->world(), host->workspace(), host->scriptTemplates());
        // And every script is checked against "live" for a replica (ADR
        // 0137 §5): what does not run here stops -- the scene's client code
        // with it, until the server's world is here.
        state.sceneClientHeld = true;
        script::reconcileAllScripts(host->runtime().state());
    });
    m_replication->setSceneChanger([hostOf](scene::World&, const std::string& path, std::vector<core::u8> data) {
        WorldHost* host = hostOf();
        if (host == nullptr)
            return;
        // **Said here as `LoadScene` says it on the authority**: a
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
    // `--join=CODE` with a relay known is a join by code, as it is from a
    // script; and `--host` registers with the relay there is.
    RelayUse relay{.relay = m_base.relay, .code = {}, .direct = true, .ticket = {}, .pass = {}, .serverKey = {}};
    if (topology == replication::Topology::Replica) {
        const JoinTarget target = joinTargetOf(address, m_base.relay);
        relay = RelayUse{
            .relay = target.relay, .code = target.code, .direct = true, .ticket = {}, .pass = {}, .serverKey = {}};
    }
    if (std::optional<core::EngineError> error = begin(topology, address, port, true, relay); error.has_value())
        return error;
    if (topology != replication::Topology::Replica && !relay.relay.empty()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"relay", std::string_view{relay.relay}}};
        core::log(core::LogLevel::Info, ENG_TR("net.info.relay_asked"), args);
    }
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

replication::VisualCorrection NetworkSession::visualCorrection(core::usize seat) const
{
#if ENG_ENABLE_REPLICATION
    if (m_replication != nullptr)
        return m_replication->visualCorrection(seat);
#endif
    (void)seat;
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

void NetworkSession::holdSnapshots(bool held)
{
    if (m_replication != nullptr)
        m_replication->holdSnapshots(held);
}

bool NetworkSession::snapshotSent() const
{
    return m_replication != nullptr && m_replication->snapshotSent();
}

void NetworkSession::noteTimeDropped()
{
    if (m_replication != nullptr)
        m_replication->noteTimeDropped();
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
    // Out of a match, nobody proves a key (ADR 0224).
    if (host != nullptr) {
        std::vector<core::InstanceId> here;
        scene::localPlayers(host->world(), here);
        for (const core::InstanceId id : here) {
            if (scene::PlayerComponent* player = host->world().players().find(id); player != nullptr)
                player->publicKey.clear();
        }
    }
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
    // Its own clock again, at full speed (ADR 0233).
    world.engineState().timeDilation = 1.0;
    world.engineState().clockPace = 1.0;
    world.engineState().networkPort = 0;
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
    // **The player at this machine is player 1 again**: the number the
    // server gave it was the server's.
    if (const core::InstanceId local = scene::localPlayerOf(world); local.valid()) {
        if (scene::PlayerComponent* component = world.players().find(local); component != nullptr)
            component->userId = 1;
        world.setName(local, world.atoms().intern("Player1"));
    }

    // This machine decides the world again: what a solo boot of its scene runs,
    // starts (ADR 0137 §5) -- **the scene it joined from**, not the
    // server's run alone with its server code: a client that lost its server
    // became the authority of a world it had only ever been a guest in.
    if (!wasAuthority)
        host->returnToSolo(m_sceneBeforeJoin);
    m_sceneBeforeJoin.clear();
    script::fireNetworkEvent(host->runtime().state(), event, reason);
}

std::function<bool(std::string_view, core::u16, const std::array<core::u8, 32>&)>
NetworkSession::trustedHosts(std::string named) const
{
    KeyPins* pins = m_pins.get();
    // The game's own server, where it names one with its key.
    std::string ownServer;
    if (std::string host; m_security.serverKey.has_value() && !m_security.server.empty()) {
        core::u16 at = DefaultPort;
        if (splitAddress(m_security.server, host, at))
            ownServer = host + ":" + std::to_string(at);
    }
    // **A host is pinned by the address it was joined at**. A code names
    // a match the relay found, not a machine, and is handed out again to
    // another host later: pinned, it refused that one as a changed key.
    return [pins, ownServer, ownKey = m_security.serverKey,
            named = std::move(named)](std::string_view host, core::u16 port, const std::array<core::u8, 32>& key) {
        // **Sent here by a world that named this one's key** (ADR 0224
        // §3): that key and no other, pinned or not.
        if (!named.empty())
            return core::toHex(key) == named;
        if (port == 0)
            return true;
        const std::string where = std::string(host) + ":" + std::to_string(port);
        if (!ownServer.empty() && where == ownServer)
            return key == *ownKey;
        return pins == nullptr || pins->trust(where, key);
    };
}

void NetworkSession::signIns(scene::EngineState& state)
{
    // When a sign-in gives up: a build with no network answers each at once.
    [[maybe_unused]] const core::u64 now = m_clock ? m_clock() : platform::nowNs();
    const auto answer = [&state](core::u32 id, core::u8 failure, std::string ticket, core::u32 retryAfter) {
        state.signInAnswers.push_back(scene::EngineState::SignInAnswer{id, failure, std::move(ticket), retryAfter});
    };
    for (scene::EngineState::SignInAsk& ask : state.signInAsks) {
#if ENG_ENABLE_REPLICATION
        auto one = std::make_unique<SignIn>();
        one->id = ask.id;
        one->deadlineNs = now + SignInWaitNs;
        std::string host;
        core::u16 port = DefaultPort;
        bool opened = splitAddress(ask.address, host, port);
        one->transport = m_transports ? m_transports() : net::createEnetTransport();
        // **The front door is met as a match's host is** (ADR 0224): its
        // key pinned at that address, so the password goes to it alone.
        if (m_security.encrypt) {
            net::SecureConfig secure;
            secure.identity = m_security.identity;
            secure.prologue = m_security.prologue;
            secure.plainRefusal = replication::refusalMessage(replication::RefusedInsecure);
            secure.trust = trustedHosts({});
            one->transport = net::createSecureTransport(std::move(one->transport), std::move(secure));
        }
        net::TransportConfig config;
        config.port = 0;
        config.maxPeers = 1;
        config.channels = replication::transportChannels();
        opened = opened && !one->transport->open(config).has_value() &&
                 !one->transport->connect(host, port, one->peer).has_value();
        one->message = replication::signInMessage(
            replication::SignInFields{.registering = ask.registering, .name = ask.name, .password = ask.password});
        core::wipe(ask.password);
        if (!opened) {
            core::wipe(ask.password);
            answer(ask.id, SignInUnreachable, {}, 0);
            continue;
        }
        m_signIns.push_back(std::move(one));
#else
        core::wipe(ask.password);
        answer(ask.id, SignInUnreachable, {}, 0);
#endif
    }
    state.signInAsks.clear();
#if ENG_ENABLE_REPLICATION
    std::vector<net::TransportEvent> events;
    for (const std::unique_ptr<SignIn>& one : m_signIns) {
        events.clear();
        (void)one->transport->poll(events, 0);
        for (const net::TransportEvent& event : events) {
            if (one->done)
                break;
            if (event.kind == net::TransportEvent::Kind::Connected && !one->message.empty()) {
                (void)one->transport->send(one->peer, one->message, net::Delivery::Reliable, 0);
                one->transport->flush();
                // The password's bytes, gone once they left.
                std::fill(one->message.begin(), one->message.end(), core::u8{0});
                one->message.clear();
            }
            else if (event.kind == net::TransportEvent::Kind::Message) {
                if (const std::optional<replication::SignedInFields> signedIn =
                        replication::readSignedIn(event.payload)) {
                    answer(one->id, signedIn->failure, signedIn->ticket, signedIn->retryAfter);
                    one->done = true;
                }
            }
            else if (event.kind == net::TransportEvent::Kind::Disconnected) {
                answer(one->id, SignInUnreachable, {}, 0);
                one->done = true;
            }
        }
        if (!one->done && now > one->deadlineNs) {
            answer(one->id, SignInUnreachable, {}, 0);
            one->done = true;
        }
        if (one->done) {
            std::fill(one->message.begin(), one->message.end(), core::u8{0});
            one->transport->disconnect(one->peer);
            one->transport->flush();
            one->transport->close();
        }
    }
    std::erase_if(m_signIns, [](const std::unique_ptr<SignIn>& one) { return one->done; });
#endif
}

void NetworkSession::update()
{
    WorldHost* host = m_host();
    if (host == nullptr)
        return;
    scene::EngineState& state = host->world().engineState();
    state.networkState = m_state;

    // --- What a script asked for, at the safe point after the frame's ticks.
    // **A host forgotten** (ADR 0224): its next key is taken as a new one.
    for (const std::string& address : state.forgottenServerKeys) {
        std::string where;
        core::u16 port = DefaultPort;
        if (m_pins != nullptr && splitAddress(address, where, port))
            m_pins->forget(where + ":" + std::to_string(port));
    }
    state.forgottenServerKeys.clear();
    if (!state.signInAsks.empty() || !m_signIns.empty())
        signIns(state);
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
            const JoinTarget target = joinTargetOf(request.address, request.relay);
            if (!target.byCode && !splitAddress(request.address, address, port)) {
                const std::array<core::I18nArg, 1> args{core::I18nArg{"address", std::string_view{request.address}}};
                script::fireNetworkEvent(host->runtime().state(), "JoinFailed",
                                         core::engineCatalog().format(ENG_TR("net.err.bad_address"), args));
                break;
            }
            const RelayUse relay{.relay = target.relay,
                                 .code = target.code,
                                 .direct = request.relayDirect,
                                 .ticket = request.ticket,
                                 .pass = {},
                                 .serverKey = {}};
            if (std::optional<core::EngineError> error =
                    begin(replication::Topology::Replica, address, port, false, relay);
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
            if (target.byCode) {
                const std::array<core::I18nArg, 2> args{core::I18nArg{"relay", std::string_view{target.relay}},
                                                        core::I18nArg{"code", std::string_view{target.code}}};
                core::log(core::LogLevel::Info, ENG_TR("net.info.joining_by_code"), args);
                break;
            }
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
            const RelayUse relay{
                .relay = request.relay, .code = {}, .direct = true, .ticket = {}, .pass = {}, .serverKey = {}};
            if (std::optional<core::EngineError> error =
                    begin(replication::Topology::Host, {}, request.port, false, relay);
                error.has_value()) {
                // **Said to the game, not only to the log**: a port in
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
            if (!request.relay.empty()) {
                const std::array<core::I18nArg, 1> relayArgs{core::I18nArg{"relay", std::string_view{request.relay}}};
                core::log(core::LogLevel::Info, ENG_TR("net.info.relay_asked"), relayArgs);
            }
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
        noteRelay(0, {});
        state.networkStats = {};
        state.networkPort = 0;
        state.networkMaxPlayers = 0;
        state.pendingRemovals.clear();
        m_rateStartedNs = 0;
        return;
    }
    // --- Who plays (ADR 0167): the seats, and the players a script removed.
    if (state.networkTopology != scene::NetworkTopology::Replica) {
        // A limit is of players, and the machine's own is one of them; the
        // session counts the peers it seats.
        // **Every player at this machine** (ADR 0205): a host's guests are
        // players of the match as its own is.
        std::vector<core::InstanceId> seated;
        scene::localPlayers(host->world(), seated);
        const auto own = static_cast<core::u32>(seated.size());
        const core::u32 most = m_base.maxPeers + own;
        const core::u32 players = state.networkMaxPlayersWanted == 0
                                      ? most
                                      : std::clamp<core::u32>(state.networkMaxPlayersWanted, std::max(own, 1u), most);
        if (players != state.networkMaxPlayers) {
            state.networkMaxPlayers = players;
            m_replication->setMaxPeers(players - std::min(players, own));
        }
        for (const auto& [userId, reason] : state.pendingRemovals)
            (void)m_replication->removePlayer(host->world(), host->workspace(), userId, reason);
    }
    else {
        state.networkMaxPlayers = 0;
    }
    state.pendingRemovals.clear();
    // **The game's word on who joins** (ADR 0226), and a refusal for one it
    // did not give in time.
    state.ticketPublicKey = m_security.ticketKey;
    state.projectId = m_security.projectId;
    if (!m_validating.empty() || !state.joinVerdicts.empty()) {
        std::vector<scene::EngineState::JoinVerdict> verdicts;
        verdicts.swap(state.joinVerdicts);
        for (scene::EngineState::JoinVerdict& verdict : verdicts) {
            if (m_validating.erase(verdict.id) == 0)
                continue;
            replication::JoinAnswer answer{
                .admit = verdict.admit, .reason = std::move(verdict.reason), .accountId = std::move(verdict.accountId)};
            if (answer.admit && !answer.accountId.empty()) {
                // A banned account is refused, whatever key it came with.
                if (m_accountGate) {
                    if (std::optional<replication::JoinAnswer> refusal = m_accountGate(answer.accountId);
                        refusal.has_value())
                        answer = std::move(*refusal);
                }
                // **One session an account**: the one already in goes.
                if (answer.admit) {
                    std::vector<core::u32> already;
                    const scene::World& world = host->world();
                    const core::InstanceId network = scene::networkServiceOf(world, host->dataModel());
                    for (core::InstanceId id = network.valid() ? world.firstChild(network) : core::InstanceId{};
                         id.valid(); id = world.nextSibling(id)) {
                        const scene::PlayerComponent* player = world.players().find(id);
                        if (player != nullptr && player->accountId == answer.accountId)
                            already.push_back(player->userId);
                    }
                    for (const core::u32 userId : already)
                        (void)m_replication->removePlayer(
                            host->world(), host->workspace(), userId,
                            core::engineCatalog().format(ENG_TR("net.info.signed_in_elsewhere")));
                }
            }
            m_replication->answerJoin(verdict.id, std::move(answer));
        }
        const core::u64 now = m_clock ? m_clock() : platform::nowNs();
        for (auto waiting = m_validating.begin(); waiting != m_validating.end();) {
            if (now < waiting->second) {
                ++waiting;
                continue;
            }
            core::log(core::LogLevel::Warn, ENG_TR("net.warn.join_request_timed_out"));
            m_replication->answerJoin(waiting->first,
                                      replication::JoinAnswer{.admit = false, .reason = {}, .accountId = {}});
            waiting = m_validating.erase(waiting);
        }
    }
    const replication::Status status = m_replication->status();
    state.networkServerTick = status.serverTick;
    state.networkServerClock = status.serverClock;
    // **A replica runs at its authority's rate** (ADR 0233): its prediction
    // steps as often as the authority's ticks do.
    if (state.networkTopology == scene::NetworkTopology::Replica) {
        state.timeDilation = status.timeDilation;
        state.clockPace = status.clockPace;
    }
    state.networkPeerCount = status.peerCount;
    state.networkPort = m_port;
    noteRelay(status.relayState, status.joinCode);

    // --- How the connection is doing.
    {
        const replication::Stats stats = m_replication->stats();
        scene::EngineState::NetworkStats& shown = state.networkStats;
        shown.encrypted = m_security.encrypt;
        shown.wireBytesSent = status.wireBytesSent;
        shown.wireBytesReceived = status.wireBytesReceived;
        shown.deferredPerSecond = stats.deferredLastSecond;
        shown.budgetUsed = stats.budgetUsedLastSecond;
        shown.pingMs = static_cast<core::f64>(status.pingMs);
        // **Each player's own** (`Player:GetNetworkPing`): the worst link
        // above is what a host's overlay reads, and no player's figure.
        shown.playerPings = m_replication->playerPings();
        if (state.networkTopology == scene::NetworkTopology::Replica) {
            host->world().players().forEach([&](core::InstanceId, const scene::PlayerComponent& player) {
                if (player.local && player.userId != 0)
                    shown.playerPings.emplace_back(player.userId, status.pingMs);
            });
            std::sort(shown.playerPings.begin(), shown.playerPings.end());
        }
        shown.jitterMs = static_cast<core::f64>(status.jitterMs);
        shown.lossPercent = static_cast<core::f64>(status.loss) * 100.0;
        shown.lastCorrectionMetres = stats.lastCorrectionMetres;
        shown.predictedDriftMetres = stats.predictedDriftMetres;
        shown.predictedDriftPart = stats.predictedDriftPart;
        shown.inputBufferDepth = stats.intentDepth;
        shown.inputStarvations = stats.intentStarvations;
        shown.inputReanchors = stats.intentReanchors;
        shown.corrections = stats.corrections;
        for (core::usize seat = 0; seat < shown.localPlayers.size(); ++seat) {
            shown.localPlayers[seat].predicted = stats.seatPredicted[seat];
            shown.localPlayers[seat].userId = stats.seatUser[seat];
            shown.localPlayers[seat].corrections = stats.seatCorrections[seat];
            shown.localPlayers[seat].lastCorrectionMetres = stats.seatLastCorrectionMetres[seat];
        }
        shown.interpolationDelayMs = static_cast<core::f64>(stats.interpolationDelayTicks) * 1000.0 / 60.0;
        shown.predictedParts = stats.predictedBodies;
        shown.unreliableSent = stats.unreliableSent;
        shown.unreliableReceived = stats.unreliableReceived;
        shown.unreliableDropped = stats.unreliableDropped;
        shown.bytesSent = stats.bytesSent;
        shown.bytesReceived = stats.bytesReceived;
        shown.swarmBytes = stats.swarmBytes;
        shown.snapshotBytes = stats.snapshotBytes;
        shown.attributeBytes = stats.attributeBytes;
        shown.remoteBytes = stats.remoteBytes;
        shown.unreliableBytes = stats.unreliableBytes;
        shown.inputBytes = stats.inputBytes;
        shown.path = status.path;
        const core::u64 now = m_clock ? m_clock() : platform::nowNs();
        const core::u64 snapshots = status.authority ? stats.snapshotsSent : stats.snapshotsReceived;
        if (m_rateStartedNs == 0 || now < m_rateStartedNs) {
            m_rateStartedNs = now;
            m_rateSnapshots = snapshots;
            m_rateCorrections = stats.corrections;
            m_rateSeatCorrections = stats.seatCorrections;
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
            for (core::usize seat = 0; seat < shown.localPlayers.size(); ++seat) {
                const core::u64 counted = stats.seatCorrections[seat];
                shown.localPlayers[seat].correctionsPerSecond =
                    static_cast<core::f64>(counted - std::min(counted, m_rateSeatCorrections[seat])) / seconds;
            }
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
            m_rateSeatCorrections = stats.seatCorrections;
            m_rateResimulations = stats.resimulations;
            m_rateResimulatedTicks = stats.resimulatedTicks;
            m_rateResimulationMicros = stats.resimulationMicros;
        }
    }

    // --- **Sent elsewhere** (`Redirect`, ADRs 0220 and 0222): a front door
    // naming a world, or a world a traveller's next. The join goes on there,
    // with the pass -- the game hears `Connected` when that world takes it,
    // and nothing in between.
    if (status.redirects != m_seenRedirects && !status.redirectAddress.empty() && status.redirectPort != 0) {
        m_seenRedirects = status.redirects;
        host->world().engineState().travelStarted.push_back(status.redirectWorld);
        const std::string address = status.redirectAddress;
        const core::u16 port = status.redirectPort;
        const RelayUse onward{.relay = {},
                              .code = {},
                              .direct = true,
                              .ticket = {},
                              .pass = status.redirectPass,
                              .serverKey = status.redirectKey};
        m_replication->shutdown();
        m_replication.reset();
        if (std::optional<core::EngineError> error =
                begin(replication::Topology::Replica, address, port, false, onward);
            error.has_value()) {
            goSolo("Disconnected", reasonOf(*error), false);
            return;
        }
        m_connecting = true;
        m_awayFromHome = true;
        m_joinStartedNs = m_clock ? m_clock() : platform::nowNs();
        m_address = address + ":" + std::to_string(port);
        setState(StateConnecting);
        return;
    }

    // --- A replica: did the join take, and is the server still there? A
    // script's join is still the machine's own world until the welcome, so
    // this is asked whatever the topology says.
    if (m_connecting && status.refused != 0) {
        const std::string why = refusalText(status.refused, status.refusedText);
        // **A join from the command line has no script to tell**: the log
        // is where its player reads why.
        if (m_redial)
            core::logText(core::LogLevel::Warn, why);
        goSolo("JoinFailed", why, state.networkTopology != scene::NetworkTopology::Replica);
        return;
    }
    if (!m_connecting && state.networkTopology != scene::NetworkTopology::Replica)
        return;
    if (m_connecting) {
        // **In line at a full game's door** (ADR 0232): waiting is not a
        // join that failed -- the clock starts again with each place told.
        if (status.queuePosition != 0 && !status.welcomed && status.queuePosition != m_queuedAt) {
            m_queuedAt = status.queuePosition;
            m_joinStartedNs = m_clock ? m_clock() : platform::nowNs();
        }
        if (status.welcomed) {
            m_connecting = false;
            m_lostSinceNs = 0;
            m_seenFreshJoins = status.freshJoins;
            setState(StateConnected);
            // **The world is the server's now, and the scene's client code
            // starts in it** -- before `Connected` is raised, so what
            // lives across worlds hears it with the scene already running.
            host->holdSceneClientCode(false);
            script::fireNetworkEvent(host->runtime().state(), "Connected", std::nullopt);
        }
        // A command-line join dials until the server answers; a script's
        // gives up after its timeout, by the clock.
        else if (!m_redial &&
                 (status.lost || static_cast<core::f64>((m_clock ? m_clock() : platform::nowNs()) - m_joinStartedNs) >
                                     m_joinTimeoutSeconds * 1'000'000'000.0)) {
            // By a code, the transport knows why: no such match, no room, no
            // path. By an address there is one thing to say.
            const std::array<core::I18nArg, 1> args{core::I18nArg{"address", std::string_view{m_address}}};
            goSolo("JoinFailed",
                   m_relay.code.empty() || (status.joinFailure == 0 && !status.lost)
                       ? core::engineCatalog().format(ENG_TR("net.err.join_failed"), args)
                       : joinFailureText(status.joinFailure, m_relay.relay, m_relay.code),
                   state.networkTopology != scene::NetworkTopology::Replica);
        }
        return;
    }
    // A join from the command line dials again, as it always has (ADR 0085);
    // one a script made hands the decision back to the game.
    if (status.refused != 0) {
        const std::string why = refusalText(status.refused, status.refusedText);
        if (m_redial)
            core::logText(core::LogLevel::Warn, why);
        goSolo("Disconnected", why, false);
        return;
    }
    // **Dropped by a world it was sent to**, on a join that dials until it is
    // answered: back to where it first joined -- a front door, which knows
    // where it belongs now. A pass is not tried twice.
    if (status.lost && m_awayFromHome && m_home.has_value() && m_home->redial) {
        const Home home = *m_home;
        m_replication->shutdown();
        m_replication.reset();
        if (std::optional<core::EngineError> error =
                begin(replication::Topology::Replica, home.address, home.port, true, home.relay);
            error.has_value()) {
            goSolo("Disconnected", reasonOf(*error), false);
            return;
        }
        m_lostSinceNs = m_clock ? m_clock() : platform::nowNs();
        setState(StateReconnecting);
        return;
    }
    if (status.lost && !m_redial) {
        goSolo("Disconnected", core::engineCatalog().format(ENG_TR("net.info.server_gone")), false);
        return;
    }
    // **A redial is a state, and it has an end**. While the server is
    // gone `State` said `Connected`, nothing fired and `ServerTick` stood
    // still, for ever: a game could tell its player nothing. It is
    // `Connecting` while this machine dials, and after `[network] timeout`
    // it gives up -- `Disconnected`, and solo, as a script's own join does.
    if (!status.welcomed) {
        const core::u64 now = m_clock ? m_clock() : platform::nowNs();
        if (m_lostSinceNs == 0) {
            m_lostSinceNs = now != 0 ? now : 1;
            // **Its own state**: a game can say "reconnecting" and stop
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
    // Each of the machine's own characters by what its own corrections
    // moved it (ADR 0208).
    constexpr core::usize Seats = render::TransformHistory::VisualSeats;
    core::DVec3 before[Seats];
    core::DVec3 vehicleBefore[Seats];
    for (core::usize seat = 0; seat < Seats; ++seat) {
        before[seat] = network.visualCorrection(seat).displaced;
        vehicleBefore[seat] = network.visualCorrection(seat).vehicleDisplaced;
    }
    network.receive(ticking);
    for (core::usize seat = 0; seat < Seats; ++seat) {
        const replication::VisualCorrection after = network.visualCorrection(seat);
        const core::DVec3 by{after.displaced.x - before[seat].x, after.displaced.y - before[seat].y,
                             after.displaced.z - before[seat].z};
        if (after.character.valid() && (by.x != 0.0 || by.y != 0.0 || by.z != 0.0))
            history.shift(host.world(), after.character, by);
        // And the vehicle it drives, the same way (ADR 0213).
        const core::DVec3 carried{after.vehicleDisplaced.x - vehicleBefore[seat].x,
                                  after.vehicleDisplaced.y - vehicleBefore[seat].y,
                                  after.vehicleDisplaced.z - vehicleBefore[seat].z};
        if (after.vehicle.valid() && (carried.x != 0.0 || carried.y != 0.0 || carried.z != 0.0))
            history.shift(host.world(), after.vehicle, carried);
    }
}

void runDrawnTick(WorldHost& host, NetworkSession& network, render::TransformHistory& history)
{
    {
        ENG_PROFILE_SCOPE("transforms.history");
        history.capture(host.world());
    }
    {
        ENG_PROFILE_SCOPE("net.receive");
        receiveDrawn(host, network, history, true);
    }
    {
        ENG_PROFILE_SCOPE("tick");
        host.tick();
    }
    {
        ENG_PROFILE_SCOPE("net.send");
        network.send();
        network.sendMessages();
    }
}

} // namespace engine::app
