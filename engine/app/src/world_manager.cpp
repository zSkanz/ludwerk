#include "engine/app/world_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <utility>

#include "engine/app/data_ops.h"
#include "engine/app/match_keys.h"
#include "engine/app/travel_pass.h"
#include "engine/core/base64.h"
#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/ticket.h"
#include "engine/net/secure_transport.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/platform/process.h"
#include "engine/replication/session.h"

namespace engine::app {

namespace {

// The protocol this build speaks: none without the replication module.
[[nodiscard]] core::u32 ourProtocol()
{
#if ENG_ENABLE_REPLICATION
    return replication::protocolVersion();
#else
    return 0;
#endif
}

} // namespace
namespace {

#if ENG_ENABLE_REPLICATION
// ADR 0232: how often the line is told where it stands, and how far back
// its places freed are counted for the estimate. A build without the
// network has no front door to keep a line at.
constexpr core::u64 QueueTellNs = 5ull * 1'000'000'000ull;
constexpr core::u64 QueueWindowNs = 300ull * 1'000'000'000ull;
#endif
// How long a channel's players are warned before it is merged (ADR 0230 §3).
constexpr core::u64 MergeWarningNs = 60ull * 1'000'000'000ull;

// **A world's name as a file's**: a channel's (`town:2`) has a colon, which
// no Windows file name may.
[[nodiscard]] std::string fileName(std::string_view world)
{
    std::string out(world);
    std::replace(out.begin(), out.end(), ':', '-');
    return out;
}

constexpr core::u64 NsPerSecond = 1'000'000'000ull;
// A world that has said nothing in this long is ended and started again.
constexpr core::u64 HeartbeatLimitNs = 10 * NsPerSecond;
// And one that never says it is ready, in this long.
constexpr core::u64 ReadyLimitNs = 120 * NsPerSecond;
// A world asked to stop that has not, in this long, is ended.
constexpr core::u64 StopLimitNs = 15 * NsPerSecond;
// A joiner the door has redirected is let go after this long.
[[maybe_unused]] constexpr core::u64 DoorHoldNs = 5 * NsPerSecond;
// An instance told it closes stops this long after, and players a fallen one
// sends back are sent for this long (ADR 0222 §2).
constexpr core::u64 ClosingNs = 5 * NsPerSecond;
constexpr core::u64 ReturnNs = 120 * NsPerSecond;
// What a test's kill at a transaction ends the process with: the code
// `--exit-on-line` ends one with, so a driver reads both alike.
constexpr int KilledAtTransaction = 86;

// The engine's own world: its pipes the manager's.
class EngineWorld final : public WorldProcess
{
public:
    explicit EngineWorld(std::unique_ptr<platform::ChildProcess> child) : m_child(std::move(child)) {}
    bool write(std::string_view text) override { return m_child->write(text); }
    [[nodiscard]] std::string read() override { return m_child->readAvailable(); }
    [[nodiscard]] bool running() override { return m_child->running(); }
    void kill() override { m_child->kill(); }
    void closeInput() override { m_child->closeInput(); }

private:
    std::unique_ptr<platform::ChildProcess> m_child;
};

[[nodiscard]] std::string phrase(core::TextKey key, std::span<const core::I18nArg> args = {})
{
    return core::engineCatalog().format(key, args);
}

} // namespace

core::TextKey stateKey(WorldManager::State state)
{
    switch (state) {
    case WorldManager::State::Starting:
        return ENG_TR("manager.state.starting");
    case WorldManager::State::Ready:
        return ENG_TR("manager.state.ready");
    case WorldManager::State::Draining:
        return ENG_TR("manager.state.draining");
    case WorldManager::State::Stopped:
        return ENG_TR("manager.state.stopped");
    case WorldManager::State::Failed:
        break;
    }
    return ENG_TR("manager.state.failed");
}

WorldLauncher engineWorldLauncher(std::filesystem::path engine, std::filesystem::path project)
{
    return [engine = std::move(engine),
            project = std::move(project)](const WorldLaunch& launch) -> std::unique_ptr<WorldProcess> {
        std::vector<std::string> arguments{engine.string(),
                                           project.string(),
                                           "--serve=" + std::to_string(launch.port),
                                           "--managed=" + launch.name,
                                           "--manager-key=" + launch.managerKey,
                                           "--max-players=" + std::to_string(launch.maxPlayers),
                                           "--saves=" + launch.saves.string()};
        if (!launch.scene.empty())
            arguments.push_back("--scene=" + launch.scene);
        if (launch.health)
            arguments.push_back("--health");
        if (launch.timeDilation)
            arguments.push_back("--time-dilation");
        if (!launch.logFile.empty())
            arguments.push_back("--log-file=" + launch.logFile.string());
        platform::ProcessOptions options;
        options.workingDirectory = project;
        options.pipeInput = true;
        std::unique_ptr<platform::ChildProcess> child = platform::ChildProcess::start(arguments, options);
        if (child == nullptr)
            return nullptr;
        return std::make_unique<EngineWorld>(std::move(child));
    };
}

// **The front door** (ADR 0220 §4): it carries no world and no game. A hello
// there is answered with a `Redirect` to a world and a pass for it.
struct WorldManager::Door
{
    std::unique_ptr<net::ITransport> transport;
    platform::FileLock keyHeld;
    // Who is at the door, and since when.
    std::map<core::u32, core::u64> waiting;
    // **The line** (ADR 0232): who waits, in the order they came -- by key,
    // with the ticket they came with, and their connection, or none and
    // until when their place is kept.
    struct Waiting
    {
        core::u32 peer = 0;
        std::string key;
        std::string ticket;
        core::u64 goneUntilNs = 0;
    };
    std::deque<Waiting> line;
    // When each of the last five minutes' places was given, and when the
    // line was last told where it stands.
    std::deque<core::u64> freed;
    core::u64 toldNs = 0;
    // Somebody new in line, or a place given: everybody is told at once.
    bool tellNow = false;
};

WorldManager::WorldManager(Options options) : m_options(std::move(options))
{}

WorldManager::~WorldManager()
{
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world->process != nullptr)
            world->process->kill();
    }
}

void WorldManager::say(std::string_view text) const
{
    if (m_options.print)
        m_options.print(std::string(text));
}

std::optional<core::EngineError> WorldManager::start()
{
    const std::filesystem::path keys = m_options.root / "keys";
    // **The manager's signing key** (ADR 0222 §3): made once, kept readable
    // by its owner only, and never in version control.
    const std::filesystem::path signingFile = keys / "manager.key";
    std::string pem;
    if (const std::optional<std::array<core::u8, 32>> seed =
            platform::readTextFile(signingFile, pem) ? core::ed25519SeedFromPem(pem) : std::nullopt;
        seed.has_value()) {
        m_signing = core::ed25519FromSeed(*seed);
    }
    else {
        std::array<core::u8, 32> fresh{};
        core::secureRandom(fresh);
        m_signing = core::ed25519FromSeed(fresh);
        if (!platform::createDirectories(keys) ||
            !platform::writeTextFile(signingFile, core::ed25519PrivatePem(fresh)) ||
            !platform::restrictToOwner(signingFile)) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"path", signingFile.string()}};
            return core::makeError(ENG_TR("manager.err.key_not_kept"), args);
        }
    }

    // The front door: a match's own handshake, the front door's own key.
    // **A build with no networking has no manager**: what it would answer
    // players with is the replication module's (the player profile).
#if !ENG_ENABLE_REPLICATION
    if (m_options.transports)
        return core::makeError(ENG_TR("engine.cli.err.no_replication"));
#else
    if (m_options.transports) {
        m_door = std::make_unique<Door>();
        std::unique_ptr<net::ITransport> inner = m_options.transports();
        net::SecureConfig secure;
        secure.identity = loadOrMakeMatchKey(keys / "front-door.key", m_door->keyHeld);
        secure.prologue =
            m_options.prologue.has_value() ? *m_options.prologue : replication::matchPrologue(m_options.projectId);
        secure.plainRefusal = replication::refusalMessage(replication::RefusedInsecure);
        m_door->transport = net::createSecureTransport(std::move(inner), std::move(secure));
        net::TransportConfig config;
        config.port = m_options.config.port;
        config.maxPeers = net::EnetPeerCap;
        config.channels = replication::transportChannels();
        if (std::optional<core::EngineError> error = m_door->transport->open(config); error.has_value())
            return error;
        const std::array<core::I18nArg, 2> args{
            core::I18nArg{"port", static_cast<core::i64>(m_options.config.port)},
            core::I18nArg{"key", core::toHex(m_door->transport->localKey().value_or(std::array<core::u8, 32>{}))}};
        core::log(core::LogLevel::Info, ENG_TR("manager.info.front_door"), args);
    }
#endif

    // **The game's data** (ADR 0238 §1): one database, this process's alone.
    // One that cannot be opened is said, and the game runs on with its data
    // kept in memory -- and lost when the manager stops.
    std::unique_ptr<IDataStore> store;
    if (m_options.dataStore) {
        store = m_options.dataStore();
    }
    else {
        const std::filesystem::path file =
            m_options.root / "data" /
            ((m_options.projectId.empty() ? std::string{"game"} : m_options.projectId) + ".db");
        std::string why;
        store = openDataStore(file, why);
        if (store == nullptr) {
            const std::array<core::I18nArg, 2> args{core::I18nArg{"path", file.string()},
                                                    core::I18nArg{"why", std::string_view{why}}};
            core::log(core::LogLevel::Error, ENG_TR("data.err.not_opened"), args);
        }
    }
    m_data = std::make_unique<DataBroker>(store != nullptr ? std::move(store) : makeMemoryDataStore());
    DataConsole::Options dataOptions;
    dataOptions.backups = m_options.root / "data" / "backups";
    dataOptions.game = m_options.projectId.empty() ? std::string{"game"} : m_options.projectId;
    dataOptions.backupEveryHours = m_options.config.backupEveryHours;
    dataOptions.backupKeep = m_options.config.backupKeep;
    dataOptions.movesKeepDays = m_options.config.movesKeepDays;
    m_dataConsole = std::make_unique<DataConsole>(std::move(dataOptions));
    m_parties = std::make_unique<PartyBook>(m_options.config.partySize);
    if (m_options.config.accounts) {
        AccountDoor::Options accounts;
        accounts.secret = m_signing.secret;
        accounts.audience = m_options.projectId;
        accounts.registration = m_options.config.accountsRegister;
        accounts.namePattern = m_options.config.accountsNamePattern;
        accounts.unixClock = m_options.unixClock;
        accounts.clock = m_options.clock;
        m_accounts = std::make_unique<AccountDoor>(m_data->store(), std::move(accounts));
    }

    for (const ServersConfig::World& config : m_options.config.worlds) {
        auto world = std::make_unique<World>();
        world->config = config;
        // Channel 1 of a world with channels is the world itself.
        if (config.channels) {
            world->base = config.name;
            world->channel = 1;
        }
        m_worlds.push_back(std::move(world));
    }
    for (const std::unique_ptr<World>& world : m_worlds)
        launch(*world);
    topUpWarm();
    return std::nullopt;
}

void WorldManager::expect(World& to, std::string_view key)
{
    const core::u64 now = m_options.clock ? m_options.clock() : 0;
    to.expected[std::string(key)] = now + static_cast<core::u64>(PassSeconds) * NsPerSecond;
}

bool WorldManager::roomAt(const World* to) const
{
    if (to == nullptr || to->state != State::Ready || to->held() >= to->config.maxPlayers)
        return false;
    if (m_options.config.maxPlayers == 0)
        return true;
    core::u32 everyone = 0;
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (!world->retired && world->state != State::Failed && world->state != State::Stopped)
            everyone += world->held();
    }
    return everyone < m_options.config.maxPlayers;
}

void WorldManager::line()
{
#if ENG_ENABLE_REPLICATION
    const core::u64 now = m_options.clock ? m_options.clock() : 0;
    std::deque<Door::Waiting>& waiting = m_door->line;
    std::erase_if(waiting, [now](const Door::Waiting& one) { return one.peer == 0 && now > one.goneUntilNs; });
    // **First in, first out, as places free** (§1): the first of the line
    // still at the door; one whose connection dropped keeps their place, and
    // does not hold the rest up.
    const ServersConfig::World* entry = m_options.config.entry();
    while (!m_stopping && entry != nullptr) {
        const auto next =
            std::find_if(waiting.begin(), waiting.end(), [](const Door::Waiting& one) { return one.peer != 0; });
        if (next == waiting.end())
            break;
        World* to = routed(entry->name, 1);
        if (!roomAt(to))
            break;
        const std::string passText = pass(next->key, {}, to->config.name, {}, next->ticket, {});
        expect(*to, next->key);
        (void)m_door->transport->send(
            net::PeerId{next->peer},
            replication::redirectMessage(m_options.config.publicAddress, to->port, passText, to->key, to->config.name),
            net::Delivery::Reliable, 0);
        m_door->waiting[next->peer] = now;
        waiting.erase(next);
        m_door->freed.push_back(now);
        m_door->tellNow = true;
    }
    // **Where each stands, every five seconds** (§1, §2): the place, and the
    // estimate the last five minutes measure -- none before anybody has left
    // the line.
    while (!m_door->freed.empty() && m_door->freed.front() + QueueWindowNs < now)
        m_door->freed.pop_front();
    if (waiting.empty() || (!m_door->tellNow && now < m_door->toldNs + QueueTellNs))
        return;
    m_door->tellNow = false;
    m_door->toldNs = now;
    const core::f64 window =
        m_door->freed.empty() ? 0.0
                              : static_cast<core::f64>(std::max<core::u64>(now - m_door->freed.front(), QueueTellNs)) /
                                    static_cast<core::f64>(NsPerSecond);
    const core::f64 perSecond = window > 0.0 ? static_cast<core::f64>(m_door->freed.size()) / window : 0.0;
    for (std::size_t at = 0; at < waiting.size(); ++at) {
        if (waiting[at].peer == 0)
            continue;
        const core::u32 position = static_cast<core::u32>(at + 1);
        core::u32 estimate = replication::NoEstimate;
        if (perSecond > 0.0)
            estimate = static_cast<core::u32>(std::ceil(static_cast<core::f64>(position) / perSecond / 60.0) * 60.0);
        (void)m_door->transport->send(net::PeerId{waiting[at].peer}, replication::queuedMessage(position, estimate),
                                      net::Delivery::Reliable, 0);
    }
#endif
}

WorldManager::World& WorldManager::startChannel(const ServersConfig::World& config, core::u32 channel)
{
    auto world = std::make_unique<World>();
    world->config = config;
    world->config.name = config.name + ":" + std::to_string(channel);
    world->config.entry = false;
    world->base = config.name;
    world->channel = channel;
    World& added = *world;
    m_worlds.push_back(std::move(world));
    launch(added);
    return added;
}

WorldManager::World* WorldManager::channelFor(std::string_view base, core::u32 travellers, const World* except)
{
    // **The fullest with room** (ADR 0230 §2): a crowd feels alive, and fewer
    // channels stay open.
    World* best = nullptr;
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world.get() == except || world->retired || world->replacing || world->base != base ||
            world->state != State::Ready || world->mergeAtNs != 0)
            continue;
        const core::u32 held = world->held();
        if (held + travellers > world->config.maxPlayers)
            continue;
        if (best == nullptr || held > best->held() || (held == best->held() && world->channel < best->channel))
            best = world.get();
    }
    return best;
}

WorldManager::World* WorldManager::routed(std::string_view target, core::u32 travellers)
{
    // A world with channels: one of them, by room; else the world itself.
    const ServersConfig::World* config = nullptr;
    for (const ServersConfig::World& world : m_options.config.worlds) {
        if (world.name == target)
            config = &world;
    }
    if (config != nullptr && config->channels) {
        if (World* channel = channelFor(target, travellers); channel != nullptr)
            return channel;
    }
    return worldNamed(target);
}

void WorldManager::channels()
{
    const core::u64 now = m_options.clock ? m_options.clock() : 0;
    for (const ServersConfig::World& config : m_options.config.worlds) {
        if (!config.channels || m_stopping)
            continue;
        std::vector<World*> open;
        std::set<core::u32> used;
        bool starting = false;
        for (const std::unique_ptr<World>& world : m_worlds) {
            if (world->retired || world->base != config.name || world->replacing || world->state == State::Stopped ||
                world->state == State::Failed)
                continue;
            used.insert(world->channel);
            if (world->state == State::Starting)
                starting = true;
            else if (world->state == State::Ready && world->mergeAtNs == 0)
                open.push_back(world.get());
        }

        // **Another, when every one open is full enough** (§1): one at a
        // time, and not past the machine's limits.
        const auto fullAt = static_cast<core::u32>(std::ceil(config.channelOpenAt * config.maxPlayers));
        const bool full = !open.empty() && std::all_of(open.begin(), open.end(),
                                                       [&](const World* world) { return world->held() >= fullAt; });
        if (full && !starting && used.size() < config.maxChannels) {
            const core::u64 free = platform::freeMemoryBytes();
            const bool memory =
                free == 0 || free >= static_cast<core::u64>(m_options.config.minFreeMegabytes) * 1024u * 1024u;
            const auto alive =
                static_cast<core::u32>(std::count_if(m_worlds.begin(), m_worlds.end(), [](const auto& w) {
                    return !w->retired && !w->warm && w->state != State::Failed && w->state != State::Stopped;
                }));
            if (memory && (m_options.mostWorlds == 0 || alive < m_options.mostWorlds)) {
                core::u32 next = 2;
                while (used.contains(next))
                    ++next;
                World& opened = startChannel(config, next);
                const std::array<core::I18nArg, 2> args{
                    core::I18nArg{"world", std::string_view{config.name}},
                    core::I18nArg{"channel", static_cast<core::i64>(opened.channel)}};
                core::log(core::LogLevel::Info, ENG_TR("manager.info.channel_opening"), args);
            }
        }

        // **Fewer when the crowd goes** (§3): another than the first stopped
        // once empty for its while; one long under a quarter full, warned,
        // then its players moved to the fullest with room.
        for (World* world : open) {
            if (world->channel <= 1)
                continue;
            const core::u32 held = world->held();
            if (held == 0 && world->present.empty()) {
                if (world->emptySinceChannelNs == 0)
                    world->emptySinceChannelNs = now;
                else if (now >= world->emptySinceChannelNs + config.channelCloseSeconds * NsPerSecond) {
                    world->stopAsked = true;
                    world->retired = true;
                    core::JsonWriter stop(core::JsonLayout::Compact);
                    stop.beginObject();
                    stop.field("type", "stop");
                    stop.endObject();
                    send(*world, stop);
                    const std::array<core::I18nArg, 1> args{
                        core::I18nArg{"world", std::string_view{world->config.name}}};
                    core::log(core::LogLevel::Info, ENG_TR("manager.info.channel_closing"), args);
                }
                continue;
            }
            world->emptySinceChannelNs = 0;
            if (config.mergeMinutes > 0.0 && held * 4 < config.maxPlayers) {
                if (world->lowSinceNs == 0) {
                    world->lowSinceNs = now;
                }
                else if (now >= world->lowSinceNs + static_cast<core::u64>(config.mergeMinutes * 60.0 *
                                                                           static_cast<core::f64>(NsPerSecond))) {
                    world->mergeAtNs = now + MergeWarningNs;
                    core::JsonWriter merging(core::JsonLayout::Compact);
                    merging.beginObject();
                    merging.field("type", "channel-merging");
                    merging.field("seconds",
                                  static_cast<core::f64>(MergeWarningNs) / static_cast<core::f64>(NsPerSecond));
                    merging.endObject();
                    send(*world, merging);
                    const std::array<core::I18nArg, 1> args{
                        core::I18nArg{"world", std::string_view{world->config.name}}};
                    core::log(core::LogLevel::Info, ENG_TR("manager.info.channel_merging"), args);
                }
            }
            else {
                world->lowSinceNs = 0;
            }
        }
        // A merge warned of, due: everybody to the fullest channel with room
        // for them all -- a party stays together.
        for (const std::unique_ptr<World>& world : m_worlds) {
            if (world->base != config.name || world->mergeAtNs == 0 || world->retired || world->state != State::Ready ||
                now < world->mergeAtNs)
                continue;
            World* into = channelFor(config.name, static_cast<core::u32>(world->present.size()), world.get());
            if (into == nullptr) {
                // Nowhere with room: it stays open, and is looked at again.
                world->mergeAtNs = 0;
                world->lowSinceNs = now;
                continue;
            }
            for (const auto& [key, account] : world->present)
                expect(*into, key);
            world->state = State::Draining;
            world->drainedSinceNs = now;
            moveAll(*world, *into);
        }
    }
}

WorldManager::World& WorldManager::startInstance(const ServersConfig::Instance& config, bool warm)
{
    auto world = std::make_unique<World>();
    world->config.name = "i-" + std::to_string(m_nextInstance++);
    world->config.scene = config.scene;
    world->config.maxPlayers = config.maxPlayers;
    world->instance = true;
    world->templateName = config.name;
    world->warm = warm;
    World& added = *world;
    m_worlds.push_back(std::move(world));
    launch(added);
    return added;
}

void WorldManager::topUpWarm()
{
    // **A warm pool** (ADR 0221 §2): each template's `warm` started ahead.
    for (const ServersConfig::Instance& config : m_options.config.instances) {
        const auto warm = static_cast<core::u32>(std::count_if(m_worlds.begin(), m_worlds.end(), [&](const auto& w) {
            return w->instance && w->warm && !w->retired && w->templateName == config.name &&
                   w->state != State::Failed && w->state != State::Stopped;
        }));
        for (core::u32 at = warm; at < config.warm && !m_stopping; ++at)
            (void)startInstance(config, true);
    }
}

void WorldManager::reply(World& asker, core::u32 id, std::string_view instance, std::string_view why)
{
    core::JsonWriter answer(core::JsonLayout::Compact);
    answer.beginObject();
    answer.field("type", why.empty() ? "instance-ready" : "instance-failed");
    answer.field("id", static_cast<core::i64>(id));
    answer.field("instance", instance);
    answer.field("why", why);
    answer.endObject();
    send(asker, answer);
}

void WorldManager::closed(World& instance, std::string_view reason)
{
    // **The world that made it learns it is gone** (ADR 0221).
    if (instance.creator == nullptr || instance.creator->process == nullptr)
        return;
    core::JsonWriter message(core::JsonLayout::Compact);
    message.beginObject();
    message.field("type", "instance-closed");
    message.field("instance", std::string_view{instance.config.name});
    message.field("reason", reason);
    message.endObject();
    send(*instance.creator, message);
    instance.creator = nullptr;
}

void WorldManager::allocate(World& instance)
{
    // Given to its group: its scene with their `Data`, and its door open to
    // them alone.
    instance.warm = false;
    instance.emptySinceNs = m_options.clock ? m_options.clock() : 0;
    instance.allowed.insert(instance.askedKeys.begin(), instance.askedKeys.end());
    core::JsonWriter allocate(core::JsonLayout::Compact);
    allocate.beginObject();
    allocate.field("type", "allocate");
    allocate.field("scene", std::string_view{instance.config.scene});
    allocate.field("data", std::string_view{instance.askedData});
    allocate.endObject();
    send(instance, allocate);
    if (instance.asker != nullptr)
        reply(*instance.asker, instance.askedId, instance.config.name, {});
    instance.asker = nullptr;
    const std::array<core::I18nArg, 2> args{core::I18nArg{"instance", std::string_view{instance.config.name}},
                                            core::I18nArg{"template", std::string_view{instance.templateName}}};
    core::log(core::LogLevel::Info, ENG_TR("manager.info.instance_allocated"), args);
}

void WorldManager::createInstance(World& asker, const ManagerMessage& message)
{
    const core::JsonValue root = message.root();
    const core::u32 id = static_cast<core::u32>(root["id"].asInteger(0));
    const std::string templateName(root["template"].asString());
    const ServersConfig::Instance* config = m_options.config.instance(templateName);
    if (config == nullptr) {
        reply(asker, id, {}, phrase(ENG_TR("manager.err.no_such_template")));
        return;
    }
    // **Bounded**: the template's own limit, and the machine's memory.
    const auto running = static_cast<core::u32>(std::count_if(m_worlds.begin(), m_worlds.end(), [&](const auto& w) {
        return w->instance && !w->warm && w->templateName == templateName && w->state != State::Failed &&
               w->state != State::Stopped;
    }));
    if (running >= config->maxInstances) {
        reply(asker, id, {}, phrase(ENG_TR("manager.err.instances_full")));
        say(phrase(ENG_TR("manager.err.instances_full")));
        return;
    }
    // **And the machine's own limit** where one is set: the editor's.
    if (m_options.mostWorlds != 0) {
        const auto alive = static_cast<core::u32>(std::count_if(m_worlds.begin(), m_worlds.end(), [](const auto& w) {
            return !w->retired && !w->warm && w->state != State::Failed && w->state != State::Stopped;
        }));
        if (alive >= m_options.mostWorlds) {
            const std::array<core::I18nArg, 1> most{
                core::I18nArg{"count", static_cast<core::i64>(m_options.mostWorlds)}};
            reply(asker, id, {}, phrase(ENG_TR("manager.err.most_worlds"), most));
            say(phrase(ENG_TR("manager.err.most_worlds"), most));
            return;
        }
    }
    if (const core::u64 free = platform::freeMemoryBytes();
        free != 0 && free < static_cast<core::u64>(m_options.config.minFreeMegabytes) * 1024u * 1024u) {
        reply(asker, id, {}, phrase(ENG_TR("manager.err.memory_low")));
        return;
    }
    World* instance = nullptr;
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world->instance && world->warm && world->templateName == templateName && world->state == State::Ready &&
            world->asker == nullptr) {
            instance = world.get();
            break;
        }
    }
    if (instance == nullptr)
        instance = &startInstance(*config, false);
    instance->warm = false;
    instance->asker = &asker;
    instance->creator = &asker;
    instance->askedId = id;
    instance->askedData = std::string(root["data"].asString());
    instance->askedKeys.clear();
    for (core::usize at = 0; at < root["players"].size(); ++at)
        instance->askedKeys.emplace_back(root["players"].at(at).asString());
    // **For a party** (ADR 0240 §1): its members online, in any world.
    if (const std::string_view partyId = root["party"].asString(); !partyId.empty() && m_parties != nullptr) {
        if (const std::optional<Party> party = m_parties->find(partyId); party.has_value()) {
            for (const std::unique_ptr<World>& world : m_worlds) {
                for (const auto& [key, account] : world->present) {
                    const std::string who = memberOf(key, account);
                    if (std::find(party->members.begin(), party->members.end(), who) != party->members.end() &&
                        std::find(instance->askedKeys.begin(), instance->askedKeys.end(), key) ==
                            instance->askedKeys.end())
                        instance->askedKeys.push_back(key);
                }
            }
        }
    }
    if (instance->state == State::Ready)
        allocate(*instance);
    topUpWarm();
}

void WorldManager::instances()
{
    const core::u64 now = m_options.clock ? m_options.clock() : 0;
    for (std::size_t at = 0; at < m_worlds.size(); ++at) {
        World* world = m_worlds[at].get();
        if (!world->instance || world->warm || world->state != State::Ready)
            continue;
        // Its group's arrival was what it waited for.
        if (world->asker != nullptr)
            continue;
        if (world->closingStopNs != 0) {
            if (now >= world->closingStopNs && !world->stopAsked) {
                world->stopAsked = true;
                world->retired = true;
                closed(*world, "Closed");
                core::JsonWriter stop(core::JsonLayout::Compact);
                stop.beginObject();
                stop.field("type", "stop");
                stop.endObject();
                send(*world, stop);
            }
            continue;
        }
        // **Closed when it has been empty for its while** (ADR 0221 §4).
        if (!world->present.empty()) {
            world->emptySinceNs = now;
            continue;
        }
        const ServersConfig::Instance* config = m_options.config.instance(world->templateName);
        const core::u64 idle = config != nullptr ? static_cast<core::u64>(config->idleCloseSeconds) * NsPerSecond : 0;
        if (now >= world->emptySinceNs + idle && now >= world->keepOpenUntilNs) {
            world->closingStopNs = now + ClosingNs;
            core::JsonWriter closing(core::JsonLayout::Compact);
            closing.beginObject();
            closing.field("type", "closing");
            closing.endObject();
            send(*world, closing);
            std::erase_if(m_members, [&](const auto& entry) { return entry.second == world->config.name; });
            const std::array<core::I18nArg, 1> args{core::I18nArg{"instance", std::string_view{world->config.name}}};
            core::log(core::LogLevel::Info, ENG_TR("manager.info.instance_closing"), args);
        }
    }
    std::erase_if(m_returns, [now](const auto& entry) { return entry.second.untilNs < now; });
    std::erase_if(m_recent, [now](const auto& entry) { return entry.second.untilNs < now; });
}

core::u16 WorldManager::freePort() const
{
    for (core::u32 port = m_options.config.firstWorldPort; port <= m_options.config.lastWorldPort; ++port) {
        const bool taken = std::any_of(m_worlds.begin(), m_worlds.end(), [&](const std::unique_ptr<World>& world) {
            return world->port == port && world->state != State::Stopped && world->state != State::Failed;
        });
        if (!taken)
            return static_cast<core::u16>(port);
    }
    return 0;
}

std::string WorldManager::dataName(const World& world)
{
    return world.config.name + "#" + std::to_string(world.dataRun);
}

void WorldManager::dataAnswers(core::u64 nowNs)
{
    if (m_data == nullptr)
        return;
    m_data->tick(static_cast<core::i64>(nowNs / 1'000'000u));
    for (const DataBroker::Answer& answer : m_data->takeAnswers()) {
        const auto world = std::find_if(m_worlds.begin(), m_worlds.end(), [&](const std::unique_ptr<World>& one) {
            return one->process != nullptr && dataName(*one) == answer.world;
        });
        if (world == m_worlds.end())
            continue;
        core::JsonWriter record(core::JsonLayout::Compact);
        record.beginObject();
        record.field("type", "data-record");
        record.field("id", static_cast<core::i64>(answer.id));
        record.field("ok", answer.ok);
        record.field("busy", answer.busy);
        record.field("found", answer.record.has_value());
        if (answer.record.has_value()) {
            record.field("version", answer.record->version);
            record.field("body", std::string_view{core::base64Encode(std::span<const core::u8>(
                                     reinterpret_cast<const core::u8*>(answer.record->body.data()),
                                     answer.record->body.size()))});
        }
        record.endObject();
        send(**world, record);
    }
}

WorldManager::World* WorldManager::worldNamed(std::string_view name)
{
    // **The one that takes players now**: during an update, the new build's
    // once it is ready, and the old one's until then.
    World* found = nullptr;
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world->config.name != name || world->retired)
            continue;
        if (world->state == State::Ready && !world->replacing)
            return world.get();
        if (found == nullptr || (world->state != State::Stopped && world->state != State::Draining))
            found = world.get();
    }
    return found;
}

void WorldManager::launch(World& world)
{
    // Its port kept across a restart where nobody took it, so a player who
    // reconnects finds the same world.
    if (world.port == 0)
        world.port = freePort();
    world.dataRun = m_nextDataRun++;
    WorldLaunch launch;
    launch.name = world.config.name;
    launch.scene = world.config.scene;
    launch.port = world.port;
    launch.maxPlayers = world.config.maxPlayers;
    launch.managerKey = core::base64Encode(m_signing.pub);
    // **A channel's saves are its world's** (ADR 0230 §4).
    launch.saves = m_options.root / "saves" / (world.base.empty() ? world.config.name : world.base);
    launch.health = m_options.config.health;
    launch.timeDilation = world.config.timeDilation;
    launch.logFile = m_options.root / m_options.config.logs / fileName(world.config.name) / "engine.log";
    world.state = State::Starting;
    world.partial.clear();
    world.players = 0;
    world.present.clear();
    world.stopAsked = false;
    world.startedNs = m_options.clock ? m_options.clock() : 0;
    world.lastHeartbeatNs = world.startedNs;
    // **Its log's folder, before it opens its log**: a world new to this
    // game -- a channel, an instance -- had none, and wrote no log at all.
    (void)platform::createDirectories(launch.logFile.parent_path());
    world.process = world.port == 0 || !m_options.launcher ? nullptr : m_options.launcher(launch);
    const std::array<core::I18nArg, 2> args{core::I18nArg{"world", std::string_view{world.config.name}},
                                            core::I18nArg{"port", static_cast<core::i64>(world.port)}};
    if (world.process == nullptr) {
        core::log(core::LogLevel::Error, ENG_TR("manager.err.world_not_started"), args);
        world.state = State::Failed;
        return;
    }
    core::log(core::LogLevel::Info, ENG_TR("manager.info.world_starting"), args);
    // The other worlds' names, for its `Travel` to check against.
    core::JsonWriter hello(core::JsonLayout::Compact);
    hello.beginObject();
    hello.field("type", "hello");
    hello.key("worlds");
    hello.beginArray();
    for (const ServersConfig::World& other : m_options.config.worlds)
        hello.value(std::string_view{other.name});
    hello.endArray();
    hello.endObject();
    send(world, hello);
}

void WorldManager::send(World& world, const core::JsonWriter& message)
{
    if (world.process != nullptr)
        (void)world.process->write(managerLine(message));
}

void WorldManager::log(const World& world, std::string_view line)
{
    // **One folder a world, its file started anew past its size** (ADR 0220
    // §6), the last few kept.
    const std::string named = fileName(world.config.name);
    const std::filesystem::path folder = m_options.root / m_options.config.logs / named;
    const std::filesystem::path file = folder / (named + ".log");
    std::error_code ec;
    if (std::filesystem::exists(file, ec) &&
        std::filesystem::file_size(file, ec) >= static_cast<core::u64>(m_options.config.logMegabytes) * 1024u * 1024u) {
        for (core::u32 at = m_options.config.logFiles; at > 1; --at) {
            const std::filesystem::path older = folder / (named + "." + std::to_string(at - 1) + ".log");
            const std::filesystem::path oldest = folder / (named + "." + std::to_string(at) + ".log");
            if (std::filesystem::exists(older, ec))
                std::filesystem::rename(older, oldest, ec);
        }
        std::filesystem::rename(file, folder / (named + ".1.log"), ec);
    }
    if (!platform::createDirectories(folder))
        return;
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.put('\n');
}

void WorldManager::readWorld(World& world)
{
    if (world.process == nullptr)
        return;
    world.partial += world.process->read();
    std::size_t start = 0;
    for (std::size_t end = world.partial.find('\n'); end != std::string::npos; end = world.partial.find('\n', start)) {
        std::string_view line(world.partial.data() + start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (const std::optional<ManagerMessage> message = ManagerMessage::read(line); message.has_value()) {
            handle(world, *message);
        }
        else {
            log(world, line);
            if (m_options.worldLine)
                m_options.worldLine(world.config.name, line);
            core::exitOnRelayedLine(line);
        }
        start = end + 1;
    }
    world.partial.erase(0, start);
}

std::string WorldManager::memberOf(const std::string& key, const std::string& account)
{
    return account.empty() ? key : account;
}

void WorldManager::partyOp(World& asker, const ManagerMessage& message, core::u64 nowNs)
{
    const core::JsonValue root = message.root();
    const std::string request(root["request"].asString());
    const std::string answer = runPartyOp(*m_parties, request, nowNs);
    core::JsonWriter result(core::JsonLayout::Compact);
    result.beginObject();
    result.field("type", "party-result");
    result.field("id", root["id"].asInteger(0));
    result.field("answer", std::string_view{answer});
    result.endObject();
    send(asker, result);
    // **An invitation** goes to the world the invitee is in.
    core::JsonDocument asked;
    core::JsonDocument answered;
    if (!asked.parse(request, "party-op") || !answered.parse(answer, "party-result"))
        return;
    if (asked.root()["op"].asString() == "invite" && answered.root()["ok"].asBool(false)) {
        const std::string invitee(asked.root()["member"].asString());
        for (const std::unique_ptr<World>& world : m_worlds) {
            const bool here = std::any_of(world->present.begin(), world->present.end(), [&](const auto& entry) {
                return memberOf(entry.first, entry.second) == invitee;
            });
            if (!here || world->process == nullptr)
                continue;
            core::JsonWriter invited(core::JsonLayout::Compact);
            invited.beginObject();
            invited.field("type", "party-invited");
            invited.field("member", std::string_view{invitee});
            invited.field("party", asked.root()["party"].asString());
            invited.field("by", asked.root()["by"].asString());
            invited.endObject();
            send(*world, invited);
        }
    }
    partyNotices();
}

void WorldManager::partyNotices()
{
    // **What changed, to every world a member is in**; what ended, to every
    // world, which drops what it knew of it.
    for (const std::string& id : m_parties->takeChanged()) {
        const std::optional<Party> party = m_parties->find(id);
        if (!party.has_value())
            continue;
        for (const std::unique_ptr<World>& world : m_worlds) {
            const bool member = std::any_of(world->present.begin(), world->present.end(), [&](const auto& entry) {
                const std::string who = memberOf(entry.first, entry.second);
                return std::find(party->members.begin(), party->members.end(), who) != party->members.end();
            });
            if (!member || world->process == nullptr)
                continue;
            core::JsonWriter notice(core::JsonLayout::Compact);
            notice.beginObject();
            notice.field("type", "party-changed");
            notice.key("party");
            writeParty(notice, *party);
            notice.endObject();
            send(*world, notice);
        }
    }
    for (const std::string& id : m_parties->takeGone()) {
        for (const std::unique_ptr<World>& world : m_worlds) {
            if (world->process == nullptr)
                continue;
            core::JsonWriter notice(core::JsonLayout::Compact);
            notice.beginObject();
            notice.field("type", "party-gone");
            notice.field("party", std::string_view{id});
            notice.endObject();
            send(*world, notice);
        }
    }
}

void WorldManager::partyTravel(const ManagerMessage& message)
{
    // **A party sent somewhere** (ADR 0240 §1): each world sends the members
    // it has, as its own `TravelGroup` would.
    const core::JsonValue root = message.root();
    const std::optional<Party> party = m_parties->find(root["party"].asString());
    if (!party.has_value())
        return;
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world->process == nullptr)
            continue;
        std::vector<std::string> keys;
        for (const auto& [key, account] : world->present) {
            const std::string who = memberOf(key, account);
            if (std::find(party->members.begin(), party->members.end(), who) != party->members.end())
                keys.push_back(key);
        }
        if (keys.empty())
            continue;
        core::JsonWriter order(core::JsonLayout::Compact);
        order.beginObject();
        order.field("type", "party-travel");
        order.key("keys");
        order.beginArray();
        for (const std::string& key : keys)
            order.value(std::string_view{key});
        order.endArray();
        order.field("target", root["target"].asString());
        order.field("data", root["data"].asString());
        order.endObject();
        send(*world, order);
    }
}

std::optional<std::string> WorldManager::admittedAccount(std::string_view key, std::string_view ticket)
{
    const std::string hex(key);
    if (!ticket.empty()) {
        const std::int64_t now = m_options.unixClock ? m_options.unixClock() : 0;
        std::string why;
        if (const std::optional<core::TicketClaims> claims =
                core::verifyTicket(ticket, m_signing.pub, m_options.projectId, now, &m_tickets, why);
            claims.has_value() && !claims->subject.empty()) {
            m_accountOf[hex] = claims->subject;
            return claims->subject;
        }
    }
    // **Tried in the editor**: a test account, the same for a key each time.
    if (ticket.empty() && m_options.trialAccounts) {
        const std::string trial = "trial-" + hex.substr(0, 16);
        m_accountOf[hex] = trial;
        return trial;
    }
    // **Coming back** with a ticket spent long ago: to a world they dropped
    // from, an instance they are a member of, a place in line, or from an
    // instance that fell -- as the account the manager knows them by.
    const auto known = m_accountOf.find(hex);
    if (known == m_accountOf.end())
        return std::nullopt;
    const bool inLine =
        m_door != nullptr && std::any_of(m_door->line.begin(), m_door->line.end(),
                                         [&](const Door::Waiting& waiting) { return waiting.key == hex; });
    if (m_recent.contains(hex) || m_members.contains(hex) || m_returns.contains(hex) || inLine)
        return known->second;
    return std::nullopt;
}

void WorldManager::forgetAccounts(core::u64 nowNs)
{
    // **Who the manager no longer has anywhere** is forgotten: in no world,
    // expected by none, with no place to come back to.
    if (nowNs < m_accountsForgottenNs)
        return;
    m_accountsForgottenNs = nowNs + 10 * NsPerSecond;
    std::erase_if(m_accountOf, [this](const auto& entry) {
        const std::string& key = entry.first;
        if (m_recent.contains(key) || m_members.contains(key) || m_returns.contains(key))
            return false;
        if (m_door != nullptr && std::any_of(m_door->line.begin(), m_door->line.end(),
                                             [&](const Door::Waiting& waiting) { return waiting.key == key; }))
            return false;
        return std::none_of(m_worlds.begin(), m_worlds.end(), [&](const std::unique_ptr<World>& world) {
            return world->present.contains(key) || world->expected.contains(key);
        });
    });
}

std::string WorldManager::pass(std::string_view player, std::string_view origin, std::string_view target,
                               std::string_view data, std::string_view ticket, std::string_view reason)
{
    TravelPass made;
    made.player = std::string(player);
    made.origin = std::string(origin);
    made.target = std::string(target);
    made.data = std::string(data);
    made.ticket = std::string(ticket);
    if (const auto known = m_accountOf.find(std::string(player)); known != m_accountOf.end())
        made.account = known->second;
    made.reason = std::string(reason);
    made.issued = m_options.unixClock ? m_options.unixClock() : 0;
    made.expires = made.issued + PassSeconds;
    made.nonce = core::uniqueId();
    return signPass(made, m_signing.secret);
}

void WorldManager::handle(World& world, const ManagerMessage& message)
{
    const std::string_view type = message.type();
    const core::JsonValue root = message.root();
    const core::u64 now = m_options.clock ? m_options.clock() : 0;
    if (type == "ready") {
        world.state = State::Ready;
        world.lastHeartbeatNs = now;
        world.key = std::string(root["key"].asString());
        world.protocol = static_cast<core::u32>(root["protocol"].asInteger(0));
        const std::array<core::I18nArg, 2> args{core::I18nArg{"world", std::string_view{world.config.name}},
                                                core::I18nArg{"port", static_cast<core::i64>(world.port)}};
        core::log(core::LogLevel::Info, ENG_TR("manager.info.world_ready"), args);
        if (world.instance && world.asker != nullptr)
            allocate(world);
        return;
    }
    if (type == "heartbeat") {
        world.lastHeartbeatNs = now;
        world.players = static_cast<core::u32>(root["players"].asInteger(0));
        std::erase_if(world.expected, [now](const auto& entry) { return entry.second < now; });
        world.tickMs = root["tickMs"].asNumber(0.0);
        return;
    }
    if (type == "player-joined") {
        const std::string key(root["key"].asString());
        const std::string account(root["account"].asString());
        // **One session an account, across every world** (ADR 0226 §4).
        if (!account.empty()) {
            for (const std::unique_ptr<World>& other : m_worlds) {
                if (other.get() == &world)
                    continue;
                for (const auto& [otherKey, otherAccount] : other->present) {
                    if (otherAccount != account)
                        continue;
                    core::JsonWriter remove(core::JsonLayout::Compact);
                    remove.beginObject();
                    remove.field("type", "remove");
                    remove.field("key", std::string_view{otherKey});
                    remove.endObject();
                    send(*other, remove);
                }
            }
        }
        world.present[key] = account;
        if (!account.empty())
            m_accountOf[key] = account;
        world.expected.erase(key);
        // In a world again: nothing to come back to.
        m_recent.erase(key);
        if (world.instance)
            m_members[key] = world.config.name;
        else
            m_members.erase(key);
        return;
    }
    if (type == "player-left") {
        const std::string key(root["key"].asString());
        world.present.erase(key);
        // **Free to come back** without waiting in line, a while (ADR 0232
        // §3): a dropped connection, not one who chose another world.
        if (!world.instance && !world.retired)
            m_recent[key] = Return{world.config.name,
                                   now + static_cast<core::u64>(m_options.config.queueGraceSeconds) * NsPerSecond};
        return;
    }
    // **Events between worlds** (ADR 0223): kept nowhere, delivered to every
    // world that hears the topic, the publisher included, in the order they
    // came.
    if (type == "subscribe") {
        world.topics.insert(std::string(root["topic"].asString()));
        return;
    }
    if (type == "unsubscribe") {
        world.topics.erase(std::string(root["topic"].asString()));
        return;
    }
    if (type == "publish") {
        const std::string topic(root["topic"].asString());
        core::JsonWriter deliver(core::JsonLayout::Compact);
        deliver.beginObject();
        deliver.field("type", "deliver");
        deliver.field("topic", std::string_view{topic});
        deliver.field("message", root["message"].asString());
        deliver.field("origin", std::string_view{world.config.name});
        deliver.field("sent", static_cast<core::i64>(m_options.unixClock ? m_options.unixClock() : 0));
        deliver.endObject();
        for (const std::unique_ptr<World>& other : m_worlds) {
            if (other->topics.contains(topic) && other->process != nullptr && other->state == State::Ready)
                send(*other, deliver);
        }
        return;
    }
    if (type == "instance-create") {
        createInstance(world, message);
        return;
    }
    if (type == "instance-allow" && world.instance) {
        world.allowed.insert(std::string(root["key"].asString()));
        return;
    }
    if (type == "instance-disallow" && world.instance) {
        world.allowed.erase(std::string(root["key"].asString()));
        return;
    }
    if (type == "instance-close" && world.instance) {
        world.keepOpenUntilNs = 0;
        world.emptySinceNs = 0;
        world.present.clear();
        return;
    }
    if (type == "instance-keep-open" && world.instance) {
        world.keepOpenUntilNs = now + static_cast<core::u64>(std::max(0.0, root["seconds"].asNumber(0.0)) *
                                                             static_cast<core::f64>(NsPerSecond));
        return;
    }
    // **A player's record** (ADR 0238 §2): asked for, and written.
    if (type == "party-op" && m_parties != nullptr) {
        partyOp(world, message, now);
        return;
    }
    if (type == "party-travel" && m_parties != nullptr) {
        partyTravel(message);
        return;
    }
    if (type == "data-load" && m_data != nullptr) {
        m_data->load(dataName(world), static_cast<core::u32>(root["id"].asInteger(0)), root["account"].asString(),
                     static_cast<core::i64>(now / 1'000'000u));
        return;
    }
    if (type == "data-op" && m_data != nullptr) {
        // The test's kill, by the count of transactions alone: what else a
        // world asks (items, balances) is not a moment a trade can be cut at.
        bool exitAfter = false;
        if (m_options.exitAtTransaction > 0) {
            core::JsonDocument request;
            if (request.parse(root["request"].asString(), "data-op") && request.root()["op"].asString() == "transact" &&
                ++m_transactionsAsked == m_options.exitAtTransaction) {
                if (!m_options.exitAfterCommit)
                    std::_Exit(KilledAtTransaction);
                exitAfter = true;
            }
        }
        const std::string answer = runDataOp(m_data->store(), dataName(world), root["request"].asString());
        if (exitAfter)
            std::_Exit(KilledAtTransaction);
        core::JsonWriter result(core::JsonLayout::Compact);
        result.beginObject();
        result.field("type", "data-result");
        result.field("id", root["id"].asInteger(0));
        result.field("answer", std::string_view{answer});
        result.endObject();
        send(world, result);
        return;
    }
    if (type == "data-write" && m_data != nullptr) {
        StoredRecord record;
        record.version = root["version"].asNumber(1.0);
        if (const std::optional<std::vector<core::u8>> body = core::base64Decode(root["body"].asString());
            body.has_value())
            record.body.assign(body->begin(), body->end());
        (void)m_data->write(dataName(world), root["account"].asString(), record, root["release"].asBool(false),
                            static_cast<core::i64>(now / 1'000'000u));
        return;
    }
    if (type == "channels") {
        // **The channels of the world this one is** (ADR 0230 §4), for a menu.
        const std::string base = world.base.empty() ? world.config.name : world.base;
        core::JsonWriter answer(core::JsonLayout::Compact);
        answer.beginObject();
        answer.field("type", "channels");
        answer.field("id", root["id"].asInteger(0));
        answer.key("channels");
        answer.beginArray();
        for (const std::unique_ptr<World>& other : m_worlds) {
            const bool same = world.base.empty() ? other.get() == &world : other->base == base;
            if (!same || other->retired || other->replacing || other->state != State::Ready)
                continue;
            answer.beginObject();
            answer.field("channel", static_cast<core::i64>(std::max<core::u32>(other->channel, 1)));
            answer.field("players", static_cast<core::i64>(other->players));
            answer.field("max", static_cast<core::i64>(other->config.maxPlayers));
            answer.endObject();
        }
        answer.endArray();
        answer.endObject();
        send(world, answer);
        return;
    }
    if (type == "travel") {
        // **A world sends players on** (ADR 0222): a pass each, for the
        // world named -- or, for a world with channels, the one of them the
        // players go to (ADR 0230 §2): the one asked for, the one a player
        // is in, or the fullest with room for them all.
        const std::string target(root["target"].asString());
        const auto travellers = static_cast<core::u32>(root["players"].size());
        World* to = nullptr;
        if (const std::string toPlayer(root["toPlayer"].asString()); !toPlayer.empty()) {
            for (const std::unique_ptr<World>& other : m_worlds) {
                if (!other->retired && other->state == State::Ready && other->present.contains(toPlayer) &&
                    other->held() + travellers <= other->config.maxPlayers)
                    to = other.get();
            }
        }
        else if (const auto channel = static_cast<core::u32>(root["channel"].asInteger(0)); channel > 0) {
            to = worldNamed(channel == 1 ? target : target + ":" + std::to_string(channel));
        }
        else {
            to = routed(target, travellers);
        }
        if (to != nullptr && to->instance) {
            bool allowed = !to->warm;
            for (core::usize at = 0; at < root["players"].size() && allowed; ++at)
                allowed = to->allowed.contains(std::string(root["players"].at(at)["key"].asString()));
            if (!allowed)
                to = nullptr;
            else
                for (core::usize at = 0; at < root["players"].size(); ++at)
                    to->origins[std::string(root["players"].at(at)["key"].asString())] = world.config.name;
        }
        // Out of an instance by its own travel: no longer one of it.
        if (world.instance) {
            for (core::usize at = 0; at < root["players"].size(); ++at)
                m_members.erase(std::string(root["players"].at(at)["key"].asString()));
        }
        core::JsonWriter answer(core::JsonLayout::Compact);
        answer.beginObject();
        answer.field("id", root["id"].asInteger(0));
        if (to == nullptr || to->state != State::Ready) {
            answer.field("type", "travel-failed");
            answer.field("target", std::string_view{target});
            answer.field("why", std::string_view{to == nullptr ? phrase(ENG_TR("manager.err.no_such_world"))
                                                               : phrase(ENG_TR("manager.err.world_not_ready"))});
            answer.endObject();
            send(world, answer);
            return;
        }
        for (core::usize at = 0; at < root["players"].size(); ++at)
            expect(*to, root["players"].at(at)["key"].asString());
        answer.field("type", "passes");
        answer.field("address", std::string_view{m_options.config.publicAddress});
        answer.field("port", static_cast<core::i64>(to->port));
        answer.field("key", std::string_view{to->key});
        answer.field("world", std::string_view{to->config.name});
        answer.key("passes");
        answer.beginArray();
        const core::JsonValue players = root["players"];
        for (core::usize at = 0; at < players.size(); ++at) {
            const std::string_view key = players.at(at)["key"].asString();
            answer.beginObject();
            answer.field("key", key);
            answer.field("pass",
                         std::string_view{pass(key, world.config.name, to->config.name, root["data"].asString(), {})});
            answer.endObject();
        }
        answer.endArray();
        answer.endObject();
        send(world, answer);
        return;
    }
    if (type == "answer") {
        // A console line sent to this world, answered.
        say(world.config.name + ": " + std::string(root["text"].asString()));
    }
}

void WorldManager::health(World& world)
{
    const core::u64 now = m_options.clock ? m_options.clock() : 0;
    if (world.state == State::Failed || world.state == State::Stopped)
        return;
    const bool exited = world.process == nullptr || !world.process->running();
    if (exited) {
        if (world.process != nullptr)
            readWorld(world);
        world.process.reset();
        // What it wrote as it stopped is written; what it held is let go.
        if (m_data != nullptr)
            m_data->worldGone(dataName(world), static_cast<core::i64>(now / 1'000'000u));
        if (world.stopAsked || m_stopping) {
            world.state = State::Stopped;
            return;
        }
        if (world.instance) {
            // **A crash sends the group back** (ADR 0221 §5), and the
            // instance is not started again: what was in it is gone.
            world.state = State::Failed;
            world.retired = true;
            for (const auto& [key, account] : world.present) {
                const auto origin = world.origins.find(key);
                m_returns[key] = Return{origin != world.origins.end() ? origin->second : std::string{}, now + ReturnNs};
                m_members.erase(key);
            }
            if (world.asker != nullptr)
                reply(*world.asker, world.askedId, {}, phrase(ENG_TR("manager.err.instance_did_not_start")));
            world.asker = nullptr;
            closed(world, "InstanceFailed");
            const std::array<core::I18nArg, 1> args{core::I18nArg{"instance", std::string_view{world.config.name}}};
            core::log(core::LogLevel::Warn, ENG_TR("manager.warn.instance_failed"), args);
            return;
        }
        // **A crash is restarted, within a limit** (ADR 0220 §5).
        const core::u64 window = static_cast<core::u64>(m_options.config.restartMinutes) * 60u * NsPerSecond;
        while (!world.restartsNs.empty() && world.restartsNs.front() + window < now)
            world.restartsNs.pop_front();
        const std::array<core::I18nArg, 1> args{core::I18nArg{"world", std::string_view{world.config.name}}};
        if (world.restartsNs.size() >= m_options.config.restarts) {
            world.state = State::Failed;
            core::log(core::LogLevel::Error, ENG_TR("manager.err.world_failed"), args);
            return;
        }
        core::log(core::LogLevel::Warn, ENG_TR("manager.warn.world_restarting"), args);
        world.restartsNs.push_back(now);
        ++world.restarts;
        launch(world);
        return;
    }
    // **A heartbeat a second, or it is ended and started again.**
    const core::u64 limit = world.state == State::Starting ? ReadyLimitNs : HeartbeatLimitNs;
    if (now > world.lastHeartbeatNs + limit) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"world", std::string_view{world.config.name}}};
        core::log(core::LogLevel::Warn, ENG_TR("manager.warn.world_silent"), args);
        world.process->kill();
        return;
    }
    if (world.stopAsked && now > world.lastHeartbeatNs + StopLimitNs)
        world.process->kill();
}

void WorldManager::frame()
{
    const core::u64 now = m_options.clock ? m_options.clock() : 0;
    // The door: each hello sent to the entry world with a pass.
#if ENG_ENABLE_REPLICATION
    if (m_door != nullptr) {
        std::vector<net::TransportEvent> events;
        (void)m_door->transport->poll(events, 0);
        for (net::TransportEvent& event : events) {
            if (event.kind == net::TransportEvent::Kind::Connected) {
                m_door->waiting[event.peer.value] = now;
                continue;
            }
            if (event.kind == net::TransportEvent::Kind::Disconnected) {
                m_door->waiting.erase(event.peer.value);
                // **Gone from the line, their place kept a while** (§3).
                for (Door::Waiting& waiting : m_door->line) {
                    if (waiting.peer == event.peer.value) {
                        waiting.peer = 0;
                        waiting.goneUntilNs =
                            now + static_cast<core::u64>(m_options.config.queueGraceSeconds) * NsPerSecond;
                    }
                }
                continue;
            }
            if (event.kind != net::TransportEvent::Kind::Message)
                continue;
            // **A sign-in** (ADR 0237): held at the door while its hash runs,
            // and answered when it is done.
            if (std::optional<replication::SignInFields> signIn = replication::readSignIn(event.payload);
                signIn.has_value()) {
                if (m_accounts == nullptr) {
                    core::wipe(signIn->password);
                    (void)m_door->transport->send(event.peer,
                                                  replication::signedInMessage(replication::SignedInFields{
                                                      static_cast<core::u8>(AccountDoor::Failure::Refused), {}, 0}),
                                                  net::Delivery::Reliable, 0);
                    continue;
                }
                std::string address = m_door->transport->addressText(event.peer);
                if (const std::size_t colon = address.rfind(':'); colon != std::string::npos)
                    address.resize(colon);
                m_door->waiting.erase(event.peer.value);
                m_accounts->ask(event.peer.value, address, signIn->name, std::move(signIn->password),
                                signIn->registering);
                continue;
            }
            const std::optional<replication::HelloFields> hello = replication::readHello(event.payload);
            if (!hello.has_value())
                continue;
            if (hello->protocol != replication::protocolVersion()) {
                (void)m_door->transport->send(event.peer, replication::refusalMessage(replication::RefusedVersion),
                                              net::Delivery::Reliable, 0);
                continue;
            }
            const ServersConfig::World* entry = m_options.config.entry();
            World* to = entry != nullptr ? routed(entry->name, 1) : nullptr;
            const std::optional<std::array<core::u8, 32>> key = m_door->transport->remoteKey(event.peer);
            // **Signed in, or not in** (amendment C): the door checks the
            // ticket itself, and every pass after it names the account.
            if (m_accounts != nullptr && (!key.has_value() || !admittedAccount(core::toHex(*key), hello->ticket))) {
                (void)m_door->transport->send(event.peer,
                                              replication::refusalMessage(replication::RefusedByGame,
                                                                          phrase(ENG_TR("manager.err.sign_in_first"))),
                                              net::Delivery::Reliable, 0);
                continue;
            }
            std::string reason;
            if (key.has_value()) {
                const std::string hex = core::toHex(*key);
                // Sent back by an instance that fell, to where they came from.
                if (const auto back = m_returns.find(hex); back != m_returns.end()) {
                    if (World* origin = worldNamed(back->second.origin);
                        origin != nullptr && origin->state == State::Ready)
                        to = origin;
                    reason = "InstanceFailed";
                    m_returns.erase(back);
                }
                // Dropped from an instance that lives: back into it.
                else if (const auto member = m_members.find(hex); member != m_members.end()) {
                    if (World* instance = worldNamed(member->second);
                        instance != nullptr && instance->state == State::Ready && instance->closingStopNs == 0)
                        to = instance;
                }
            }
            // **Back to where they were**, within the grace (ADR 0232 §3).
            if (key.has_value() && reason.empty()) {
                if (const auto recent = m_recent.find(core::toHex(*key)); recent != m_recent.end()) {
                    if (World* back = worldNamed(recent->second.origin); back != nullptr && roomAt(back))
                        to = back;
                    m_recent.erase(recent);
                }
            }
            // **In line** (ADR 0232): nobody has room, or others wait before
            // them. One already in line has come back to their place.
            if (!m_stopping && key.has_value() && to != nullptr && reason.empty() &&
                m_members.find(core::toHex(*key)) == m_members.end() && (!roomAt(to) || !m_door->line.empty())) {
                const std::string hex = core::toHex(*key);
                const auto mine = std::find_if(m_door->line.begin(), m_door->line.end(),
                                               [&](const Door::Waiting& waiting) { return waiting.key == hex; });
                if (mine == m_door->line.end() && m_door->line.size() >= m_options.config.queueMax) {
                    (void)m_door->transport->send(event.peer,
                                                  replication::refusalMessage(replication::RefusedByGame,
                                                                              phrase(ENG_TR("manager.err.queue_full"))),
                                                  net::Delivery::Reliable, 0);
                    continue;
                }
                if (mine != m_door->line.end()) {
                    mine->peer = event.peer.value;
                    mine->goneUntilNs = 0;
                    mine->ticket = hello->ticket;
                }
                else {
                    m_door->line.push_back(Door::Waiting{event.peer.value, hex, hello->ticket, 0});
                }
                // Held, and not let go after the door's five seconds.
                m_door->waiting.erase(event.peer.value);
                m_door->tellNow = true;
                continue;
            }
            if (m_stopping || to == nullptr || to->state != State::Ready || !key.has_value()) {
                (void)m_door->transport->send(
                    event.peer,
                    replication::refusalMessage(replication::RefusedByGame, phrase(ENG_TR("manager.err.not_open"))),
                    net::Delivery::Reliable, 0);
                continue;
            }
            const std::string passText = pass(core::toHex(*key), {}, to->config.name, {}, hello->ticket, reason);
            expect(*to, core::toHex(*key));
            (void)m_door->transport->send(event.peer,
                                          replication::redirectMessage(m_options.config.publicAddress, to->port,
                                                                       passText, to->key, to->config.name),
                                          net::Delivery::Reliable, 0);
        }
        if (m_accounts != nullptr) {
            m_accounts->frame();
            for (const AccountDoor::Result& result : m_accounts->take()) {
                (void)m_door->transport->send(
                    net::PeerId{result.peer},
                    replication::signedInMessage(replication::SignedInFields{static_cast<core::u8>(result.failure),
                                                                             result.ticket, result.retryAfter}),
                    net::Delivery::Reliable, 0);
                // Answered: let go after the door's five seconds, as any.
                m_door->waiting[result.peer] = now;
            }
            forgetAccounts(now);
        }
        line();
        // Who was answered and has not gone is let go.
        for (auto at = m_door->waiting.begin(); at != m_door->waiting.end();) {
            if (now > at->second + DoorHoldNs) {
                m_door->transport->disconnect(net::PeerId{at->first});
                at = m_door->waiting.erase(at);
            }
            else {
                ++at;
            }
        }
        m_door->transport->flush();
    }
#endif
    // By index: a world's message may start another (an instance), which
    // grows the list under the loop.
    for (std::size_t at = 0; at < m_worlds.size(); ++at) {
        World& world = *m_worlds[at];
        readWorld(world);
        health(world);
    }
    instances();
    channels();
    // **An update, a world at a time** (ADR 0220 §7).
    for (std::size_t at = 0; at < m_worlds.size(); ++at) {
        World& old = *m_worlds[at];
        if (old.successor == nullptr)
            continue;
        World& next = *old.successor;
        if (next.state == State::Failed) {
            // The new build did not start: the old one goes on.
            old.successor = nullptr;
            next.retired = true;
            updateFailed();
            const std::array<core::I18nArg, 1> args{core::I18nArg{"world", std::string_view{old.config.name}}};
            core::log(core::LogLevel::Error, ENG_TR("manager.err.update_failed"), args);
            continue;
        }
        if (next.state != State::Ready || old.state == State::Draining)
            continue;
        // **Another protocol is refused by its first world**, and the update
        // stops with the old build running.
        if (next.protocol != ourProtocol()) {
            next.stopAsked = true;
            next.retired = true;
            if (next.process != nullptr)
                next.process->kill();
            old.successor = nullptr;
            updateFailed();
            const std::array<core::I18nArg, 1> args{core::I18nArg{"world", std::string_view{old.config.name}}};
            core::log(core::LogLevel::Error, ENG_TR("manager.err.update_protocol"), args);
            continue;
        }
        next.replacing = false;
        old.state = State::Draining;
        old.drainedSinceNs = now;
        moveAll(old, next);
    }
    dataAnswers(now);
    // **Who is online**, for the parties' five minutes, and what that changed.
    if (m_parties != nullptr) {
        std::set<std::string, std::less<>> online;
        for (const std::unique_ptr<World>& world : m_worlds) {
            for (const auto& [key, account] : world->present)
                online.insert(memberOf(key, account));
        }
        m_parties->setOnline(online, now);
        partyNotices();
    }
    if (m_dataConsole != nullptr && m_data != nullptr && m_options.unixClock)
        m_dataConsole->frame(*m_data, m_options.unixClock());
    // Drained: stopped once empty, or after a while whatever it holds.
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world->state != State::Draining || world->stopAsked)
            continue;
        if (world->players == 0 || now > world->drainedSinceNs + StopLimitNs) {
            world->stopAsked = true;
            world->retired = true;
            core::JsonWriter stop(core::JsonLayout::Compact);
            stop.beginObject();
            stop.field("type", "stop");
            stop.endObject();
            send(*world, stop);
        }
    }
    if (m_stopping) {
        const bool allGone = std::all_of(m_worlds.begin(), m_worlds.end(), [](const std::unique_ptr<World>& world) {
            return world->state == State::Stopped || world->state == State::Failed;
        });
        if (allGone || now > m_stopDeadlineNs) {
            for (const std::unique_ptr<World>& world : m_worlds) {
                if (world->process != nullptr)
                    world->process->kill();
            }
            m_finished = true;
        }
    }
}

void WorldManager::moveAll(World& from, World& to)
{
    core::JsonWriter drain(core::JsonLayout::Compact);
    drain.beginObject();
    drain.field("type", "drain");
    drain.endObject();
    send(from, drain);
    core::JsonWriter move(core::JsonLayout::Compact);
    move.beginObject();
    move.field("type", "passes");
    move.field("id", static_cast<core::i64>(0));
    move.field("address", std::string_view{m_options.config.publicAddress});
    move.field("port", static_cast<core::i64>(to.port));
    move.field("key", std::string_view{to.key});
    move.field("world", std::string_view{to.config.name});
    move.key("passes");
    move.beginArray();
    for (const auto& [key, account] : from.present) {
        move.beginObject();
        move.field("key", std::string_view{key});
        move.field("pass", std::string_view{pass(key, from.config.name, to.config.name, {}, {})});
        move.endObject();
    }
    move.endArray();
    move.endObject();
    send(from, move);
    const std::array<core::I18nArg, 2> args{core::I18nArg{"world", std::string_view{from.config.name}},
                                            core::I18nArg{"players", static_cast<core::i64>(from.present.size())}};
    core::log(core::LogLevel::Info, ENG_TR("manager.info.update_moving"), args);
}

void WorldManager::update(const std::filesystem::path& folder)
{
    m_launcherBefore = m_options.launcher;
    m_options.launcher = m_options.launcherFor(folder);
    // Each fixed world gets its successor, started on a port of its own.
    const std::size_t before = m_worlds.size();
    for (std::size_t at = 0; at < before; ++at) {
        World& old = *m_worlds[at];
        // **An instance finishes on the build it began on** (§7): its group
        // stays where it is. A warm one nobody has been given is the old
        // build's spare, let go, and the pool fills again from the new one.
        if (old.instance) {
            if (old.warm && old.asker == nullptr && !old.retired && old.state == State::Ready) {
                old.stopAsked = true;
                old.retired = true;
                core::JsonWriter stop(core::JsonLayout::Compact);
                stop.beginObject();
                stop.field("type", "stop");
                stop.endObject();
                send(old, stop);
            }
            continue;
        }
        if (old.retired || old.successor != nullptr || old.state != State::Ready)
            continue;
        auto next = std::make_unique<World>();
        next->config = old.config;
        next->base = old.base;
        next->channel = old.channel;
        next->replacing = true;
        old.successor = next.get();
        World& added = *next;
        m_worlds.push_back(std::move(next));
        launch(added);
    }
    topUpWarm();
}

void WorldManager::updateFailed()
{
    if (m_launcherBefore) {
        m_options.launcher = m_launcherBefore;
        m_launcherBefore = nullptr;
    }
}

void WorldManager::stop()
{
    if (m_stopping)
        return;
    m_stopping = true;
    const core::u64 now = m_options.clock ? m_options.clock() : 0;
    m_stopDeadlineNs = now + StopLimitNs;
    for (const std::unique_ptr<World>& world : m_worlds) {
        world->stopAsked = true;
        core::JsonWriter stop(core::JsonLayout::Compact);
        stop.beginObject();
        stop.field("type", "stop");
        stop.endObject();
        send(*world, stop);
    }
    core::log(core::LogLevel::Info, ENG_TR("manager.info.stopping"));
}

std::vector<WorldManager::WorldView> WorldManager::worlds() const
{
    std::vector<WorldView> out;
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world->retired)
            continue;
        out.push_back(WorldView{world->config.name, world->state, world->port, world->players, world->tickMs,
                                world->restarts, world->instance, world->templateName});
    }
    return out;
}

std::string WorldManager::run(std::string_view line)
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
        line.remove_suffix(1);
    if (line.empty())
        return {};
    // `<world>: <command>` -- a line for one world's console.
    if (const std::size_t colon = line.find(':'); colon != std::string_view::npos && line.find(' ') > colon) {
        World* world = worldNamed(line.substr(0, colon));
        if (world == nullptr || world->process == nullptr) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"world", line.substr(0, colon)}};
            return phrase(ENG_TR("manager.err.no_world"), args);
        }
        std::string_view command = line.substr(colon + 1);
        while (!command.empty() && command.front() == ' ')
            command.remove_prefix(1);
        (void)world->process->write(std::string(command) + "\n");
        return {};
    }
    const std::size_t space = line.find(' ');
    const std::string_view verb = line.substr(0, space);
    const std::string_view rest = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
    if (verb == "worlds") {
        std::string table;
        for (const WorldView& view : worlds()) {
            const std::string state = phrase(stateKey(view.state));
            const std::array<core::I18nArg, 6> args{core::I18nArg{"world", std::string_view{view.name}},
                                                    core::I18nArg{"state", std::string_view{state}},
                                                    core::I18nArg{"port", static_cast<core::i64>(view.port)},
                                                    core::I18nArg{"players", static_cast<core::i64>(view.players)},
                                                    core::I18nArg{"tick", view.tickMs},
                                                    core::I18nArg{"restarts", static_cast<core::i64>(view.restarts)}};
            table += (table.empty() ? "" : "\n") + phrase(ENG_TR("manager.worlds_row"), args);
        }
        return table;
    }
    if (verb == "restart") {
        World* world = worldNamed(rest);
        if (world == nullptr) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"world", rest}};
            return phrase(ENG_TR("manager.err.no_world"), args);
        }
        // Started again whatever it was, a failed one included: the operator
        // said so.
        if (world->process != nullptr)
            world->process->kill();
        world->process.reset();
        world->restartsNs.clear();
        launch(*world);
        const std::array<core::I18nArg, 1> args{core::I18nArg{"world", rest}};
        return phrase(ENG_TR("manager.done.restarting"), args);
    }
    // **A fall, on purpose** (ADR 0225 §2): the process ended as a crash
    // ends it, so what a game does about one -- a restart, `InstanceFailed`
    // -- is tried as it will happen.
    if (verb == "end") {
        World* world = worldNamed(rest);
        if (world == nullptr || world->process == nullptr) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"world", rest}};
            return phrase(ENG_TR("manager.err.no_world"), args);
        }
        world->process->kill();
        const std::array<core::I18nArg, 1> args{core::I18nArg{"world", rest}};
        return phrase(ENG_TR("manager.done.ended"), args);
    }
    if (verb == "stop") {
        // Each world's own `stop`, with the minutes: it counts down, saves and
        // lets its players go; the manager ends when they have.
        for (const std::unique_ptr<World>& world : m_worlds) {
            if (world->process != nullptr)
                (void)world->process->write(rest.empty() ? std::string("stop\n") : "stop " + std::string(rest) + "\n");
            world->stopAsked = true;
        }
        m_stopping = true;
        const core::u64 now = m_options.clock ? m_options.clock() : 0;
        core::f64 minutes = 0.0;
        if (!rest.empty())
            minutes = std::strtod(std::string(rest).c_str(), nullptr);
        m_stopDeadlineNs = now + static_cast<core::u64>(minutes * 60.0 * 1e9) + StopLimitNs;
        return phrase(ENG_TR("manager.done.stopping"));
    }
    if (verb == "instances") {
        std::string table;
        for (const std::unique_ptr<World>& world : m_worlds) {
            if (!world->instance || world->retired)
                continue;
            const std::string state = phrase(world->warm ? ENG_TR("manager.state.warm") : stateKey(world->state));
            const std::array<core::I18nArg, 4> args{
                core::I18nArg{"instance", std::string_view{world->config.name}},
                core::I18nArg{"template", std::string_view{world->templateName}},
                core::I18nArg{"state", std::string_view{state}},
                core::I18nArg{"players", static_cast<core::i64>(world->present.size())}};
            table += (table.empty() ? "" : "\n") + phrase(ENG_TR("manager.instances_row"), args);
        }
        return table.empty() ? phrase(ENG_TR("manager.instances_none")) : table;
    }
    if (verb == "update") {
        if (!m_options.launcherFor || rest.empty())
            return phrase(ENG_TR("manager.usage"));
        // **A folder with no game in it is refused** before anything moves:
        // every world and instance would otherwise start from nothing.
        if (!m_options.launcherFor(std::filesystem::path(std::string(rest)))) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"path", rest}};
            return phrase(ENG_TR("manager.err.update_folder"), args);
        }
        update(std::filesystem::path(std::string(rest)));
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", rest}};
        return phrase(ENG_TR("manager.done.updating"), args);
    }
    // **The game's database** (ADR 0238 §7): here, in the one process that
    // opens it.
    if ((verb == "data" || verb == "moves") && m_dataConsole != nullptr && m_data != nullptr) {
        const std::int64_t nowUnix = m_options.unixClock ? m_options.unixClock() : 0;
        if (std::optional<DataConsole::Answer> answer = m_dataConsole->run(line, *m_data, nowUnix); answer.has_value())
            return std::move(answer->text);
    }
    if (verb == "account")
        return m_accounts != nullptr ? m_accounts->run(line) : phrase(ENG_TR("manager.err.accounts_off"));
    if (verb == "help")
        return phrase(ENG_TR("manager.usage"));
    const std::array<core::I18nArg, 1> args{core::I18nArg{"name", verb}};
    return phrase(ENG_TR("manager.err.unknown_command"), args);
}

} // namespace engine::app
