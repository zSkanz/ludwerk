#include "engine/app/bots.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <thread>

#include "class_descriptors.gen.h"
#include "engine/app/luau_flags.h"
#include "engine/app/manager_link.h"
#include "engine/audio/scene_types.h"
#include "engine/core/crypto.h"
#include "engine/core/i18n.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/input/scene_types.h"
#include "engine/net/secure_transport.h"
#include "engine/net/transport.h"
#include "engine/platform/platform.h"
#include "engine/platform/stop_signal.h"
#include "engine/render/scene_types.h"
#include "engine/replication/replication.h"
#include "engine/replication/session.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/bot_behavior.h"
#include "engine/ui/scene_types.h"

namespace engine::app {

#if ENG_ENABLE_REPLICATION
namespace {

using core::f64;
using core::u32;
using core::u64;

constexpr f64 TickSeconds = 1.0 / 60.0;
// How far a wanderer goes from where it stood, and how long it walks to one
// place before it picks another.
constexpr f64 WanderReach = 150.0;
constexpr u64 WanderTicks = 20 * 60;
// **Joins spread out**: a server takes twenty connections a second from one
// address and sixteen handshakes at once (`MaxConnectsPerAddressPerSecond`,
// `MaxHandshakesPerAddress`), so a process starts one every tenth of a second
// and tries again, a little later, a join the server let go.
constexpr u64 DialEveryTicks = 6;
constexpr u32 MostAttempts = 8;

// The classes and enums every bot's world is made of: those of the engine,
// registered once and shared, as a host registers them for its one world.
struct Registries
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;

    Registries()
    {
        scene::generated::registerClasses(classes, atoms);
        render::registerSceneTypes(classes, atoms);
        input::registerSceneTypes(classes, atoms);
        audio::registerSceneTypes(classes, atoms);
        ui::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
    }
};

// **A world with the services a server's state names**, made as the script
// runtime makes them (`script::boot`): the services a server sends are found
// by their class here, and nothing else is needed to hold what it sends.
struct BotWorld
{
    scene::World world;
    core::InstanceId dataModel;
    core::InstanceId workspace;
    core::InstanceId network;
    core::InstanceId player;

    BotWorld(Registries& shared, u32 seed) : world(shared.classes, shared.enums, shared.atoms, seed)
    {
        world.engineState().networkTopology = scene::NetworkTopology::Replica;
        const scene::ClassRegistry& classes = world.classes();
        core::AtomTable& atoms = world.atoms();
        dataModel = world.create(classes.findId(atoms.intern("DataModel")));
        std::vector<scene::ClassId> services;
        for (scene::ClassId id = 1; id < static_cast<scene::ClassId>(classes.classCount()); ++id) {
            const scene::ClassDescriptor* descriptor = classes.find(id);
            if (descriptor != nullptr && scene::hasFlag(descriptor->flags, scene::ClassFlags::Service) &&
                !descriptor->lazyService && descriptor->integration.empty())
                services.push_back(id);
        }
        for (const scene::ClassId id : services) {
            const core::InstanceId service = world.create(id);
            world.setName(service, classes.find(id)->defaultName);
            (void)world.setParent(service, dataModel);
            const std::string_view name = atoms.text(classes.find(id)->name);
            if (name == "Workspace")
                workspace = service;
            else if (name == "NetworkService")
                network = service;
        }
        player = scene::createPlayer(world, network, 0, true);
    }
};

struct Bot
{
    u32 index = 0;
    std::unique_ptr<BotWorld> world;
    std::unique_ptr<replication::IReplication> link;
    // **One key for the bot's whole run**, as a player's machine has: a pass
    // a front door or a world's `Travel` hands out is bound to the key that
    // asked for it.
    core::X25519Pair identity;
    std::mt19937 random;
    // Where it walks to, and from when.
    std::optional<core::DVec3> target;
    u64 targetSince = 0;
    std::optional<core::DVec3> home;
    bool welcomed = false;
    bool failed = false;
    // When it dials next, and how many times it has.
    u64 dialAt = 0;
    u32 attempts = 0;
    // Dropped after it was in: a different number from a join that failed.
    bool dropped = false;
    u32 seenRedirects = 0;
    u32 travels = 0;
    u64 bytesIn = 0;
    u64 snapshots = 0;
};

[[nodiscard]] std::unique_ptr<replication::IReplication> dial(const BotOptions& options, const Bot& bot,
                                                              const std::string& address, core::u16 port,
                                                              const std::string& pass, const std::string& serverKey,
                                                              std::optional<core::EngineError>& error)
{
    replication::Config config;
    config.topology = replication::Topology::Replica;
    config.address = address;
    config.port = port;
    config.luauFlags = luauFlagsFingerprint();
    config.joinPass = pass;
    config.redial = false;
    std::unique_ptr<net::ITransport> transport = net::createEnetTransport();
    if (options.encrypt) {
        net::SecureConfig secure;
        secure.identity = bot.identity;
        secure.prologue = options.prologue;
        secure.plainRefusal = replication::refusalMessage(replication::RefusedInsecure);
        // A bot trusts whatever server it was pointed at -- and, sent on,
        // the key it was sent to and no other.
        secure.trust = [serverKey](std::string_view, core::u16, const std::array<core::u8, 32>& key) {
            return serverKey.empty() || core::toHex(key) == serverKey;
        };
        transport = net::createSecureTransport(std::move(transport), std::move(secure));
    }
    return replication::createReplicationOver(std::move(transport), config, error);
}

// What a game's behavior reads of a bot's copy of the world.
class TaggedParts final : public script::BotWorldView
{
public:
    explicit TaggedParts(const scene::World& world) noexcept : m_world(world) {}

    void tagged(std::string_view tag, std::vector<core::DVec3>& out) const override
    {
        const core::NameAtom atom = m_world.atoms().lookup(tag);
        if (!atom.valid())
            return;
        std::vector<core::InstanceId> found;
        m_world.collectTagged(atom, found);
        for (const core::InstanceId id : found) {
            if (const scene::PartComponent* part = m_world.parts().find(id))
                out.push_back(part->cframe.position);
        }
    }

private:
    const scene::World& m_world;
};

// The part a bot's player has for a body, if it has one yet.
[[nodiscard]] const scene::PartComponent* bodyOf(const Bot& bot)
{
    const scene::World& world = bot.world->world;
    const scene::PlayerComponent* player = world.players().find(bot.world->player);
    if (player == nullptr || !player->character.valid() || !world.alive(player->character))
        return nullptr;
    return world.parts().find(player->character);
}

// **What a bot presses this tick**: towards where it is going, as a stick
// would, on the plane the game walks on.
void steer(const BotOptions& options, script::BotBehaviors* behaviors, Bot& bot, u64 tick)
{
    scene::World& world = bot.world->world;
    scene::PlayerComponent* player = world.players().find(bot.world->player);
    if (player == nullptr)
        return;
    player->intents.clear();
    if (options.behavior == "idle")
        return;
    const scene::PartComponent* body = bodyOf(bot);
    if (behaviors != nullptr) {
        // **A game's behavior** says where to walk; it runs whether or not
        // the bot has a body yet, and begins once it has.
        script::BotSight sight;
        sight.hasBody = body != nullptr;
        if (body != nullptr)
            sight.position = body->cframe.position;
        behaviors->step(bot.index, sight, TaggedParts(world), static_cast<f64>(tick) * TickSeconds);
        bot.target = behaviors->walkTarget(bot.index);
        if (body == nullptr || !bot.target.has_value())
            return;
    }
    if (body == nullptr)
        return;
    const core::DVec3 at = body->cframe.position;
    if (!bot.home.has_value())
        bot.home = at;
    std::uniform_real_distribution<f64> across(-1.0, 1.0);
    const bool crowd = options.behavior == "crowd";
    const auto reached = [&](const core::DVec3& where) {
        const f64 dx = where.x - at.x;
        const f64 dz = where.z - at.z;
        return dx * dx + dz * dz < 4.0;
    };
    // **`travel` walks to a pad**: one of the parts the game tagged `Portal`,
    // where its server sends players on; with none, it wanders.
    std::vector<core::DVec3> pads;
    if (behaviors == nullptr && options.behavior == "travel")
        TaggedParts(world).tagged(BotPortalTag, pads);
    if (behaviors != nullptr) {
        // Where the behavior said.
    }
    else if (!pads.empty()) {
        if (!bot.target.has_value() || reached(*bot.target) || tick > bot.targetSince + WanderTicks) {
            bot.target = pads[bot.random() % pads.size()];
            bot.targetSince = tick;
        }
    }
    else if (!bot.target.has_value() || reached(*bot.target) || tick > bot.targetSince + WanderTicks) {
        // A crowd gathers in a few metres round the world's middle; a
        // wanderer goes anywhere near where it started.
        const core::DVec3 centre = crowd ? core::DVec3{} : *bot.home;
        const f64 reach = crowd ? 8.0 : WanderReach;
        bot.target = core::DVec3{centre.x + across(bot.random) * reach, at.y, centre.z + across(bot.random) * reach};
        bot.targetSince = tick;
    }
    const f64 dx = bot.target->x - at.x;
    const f64 dz = bot.target->z - at.z;
    const f64 length = std::sqrt(dx * dx + dz * dz);
    if (length < 1.0e-6)
        return;
    scene::PlayerIntent move;
    move.action = world.atoms().intern(options.intent);
    move.type = 2;
    // A stick's axes: right is +X, forward is +Y, and forward walks -Z.
    move.axis = core::Vec3{static_cast<float>(dx / length), static_cast<float>(-dz / length), 0.0f};
    move.pressed = true;
    player->intents.push_back(move);
}

} // namespace

int runBots(const BotOptions& options)
{
    if (options.count == 0 || options.count > MostBotsAProcess || options.address.empty() || options.port == 0) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"most", static_cast<core::i64>(MostBotsAProcess)}};
        core::log(core::LogLevel::Error, ENG_TR("bots.err.count"), args);
        return 2;
    }
    // **A game's own behavior**, compiled once for every bot of the process.
    std::unique_ptr<script::BotBehaviors> behaviors;
    if (!options.script.empty()) {
        std::string error;
        behaviors = script::BotBehaviors::create(options.script, options.behavior, error);
        if (behaviors == nullptr) {
            core::logText(core::LogLevel::Error, error);
            return 2;
        }
    }
    Registries shared;
    std::vector<Bot> bots(options.count);
    for (u32 at = 0; at < options.count; ++at) {
        Bot& bot = bots[at];
        bot.index = at;
        bot.random.seed(0x5eed0000u + at);
        bot.dialAt = 1 + static_cast<u64>(at) * DialEveryTicks;
        bot.identity = core::x25519Generate();
    }
    // The reasons joins failed, each said once.
    std::vector<std::string> said;
    const auto sayOnce = [&said](const std::string& text) {
        if (text.empty() || std::find(said.begin(), said.end(), text) != said.end())
            return;
        said.push_back(text);
        core::logText(core::LogLevel::Warn, text);
    };

    platform::installStopSignals();
    const u64 started = platform::nowNs();
    const auto tickNs = static_cast<u64>(TickSeconds * 1.0e9);
    u64 nextTickNs = started;
    u64 nextReportNs = started + 1'000'000'000ull;
    u64 tick = 0;
    while (!platform::stopRequested()) {
        const u64 now = platform::nowNs();
        if (options.seconds > 0.0 && now - started >= static_cast<u64>(options.seconds * 1.0e9))
            break;
        if (now < nextTickNs) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<u64>(nextTickNs - now, 2'000'000ull)));
            continue;
        }
        nextTickNs += tickNs;
        // A process behind by more than a quarter second drops the rest.
        if (now > nextTickNs + 15 * tickNs)
            nextTickNs = now + tickNs;
        tick += 1;
        for (Bot& bot : bots) {
            if (bot.failed || bot.dropped)
                continue;
            if (bot.link == nullptr) {
                if (tick < bot.dialAt)
                    continue;
                bot.attempts += 1;
                bot.world = std::make_unique<BotWorld>(shared, 1000u + bot.index);
                std::optional<core::EngineError> error;
                bot.link = dial(options, bot, options.address, options.port, {}, {}, error);
                if (bot.link == nullptr) {
                    sayOnce(error.has_value() ? error->message : std::string{});
                    bot.failed = true;
                }
                continue;
            }
            scene::World& world = bot.world->world;
            world.engineState().tick = tick;
            bot.link->receive(world, bot.world->workspace, true);
            const replication::Status status = bot.link->status();
            if (status.refused != 0 || status.lost) {
                if (bot.welcomed) {
                    bot.dropped = true;
                    continue;
                }
                sayOnce(status.refusedText);
                bot.link->shutdown();
                bot.link.reset();
                if (bot.attempts >= MostAttempts) {
                    bot.failed = true;
                    continue;
                }
                // Again in one to three seconds.
                bot.dialAt = tick + 60 + static_cast<u64>(bot.random() % 120u);
                continue;
            }
            // **Sent on** (a front door, or a world's `Travel`): a new world,
            // joined with the pass it was given.
            if (status.redirects != bot.seenRedirects && !status.redirectAddress.empty() && status.redirectPort != 0) {
                bot.seenRedirects = status.redirects;
                const std::string address = status.redirectAddress;
                const core::u16 port = status.redirectPort;
                const std::string pass = status.redirectPass;
                const std::string key = status.redirectKey;
                const replication::Stats spent = bot.link->stats();
                bot.bytesIn += spent.bytesReceived;
                bot.snapshots += spent.snapshotsReceived;
                bot.link->shutdown();
                bot.link.reset();
                bot.world = std::make_unique<BotWorld>(shared, 1000u + bot.index);
                bot.target.reset();
                bot.home.reset();
                bot.welcomed = false;
                bot.seenRedirects = 0;
                bot.travels += 1;
                std::optional<core::EngineError> error;
                bot.link = dial(options, bot, address, port, pass, key, error);
                if (bot.link == nullptr)
                    bot.failed = true;
                continue;
            }
            if (status.welcomed && !bot.welcomed && behaviors != nullptr)
                behaviors->restart(bot.index);
            bot.welcomed = bot.welcomed || status.welcomed;
            if (bot.welcomed)
                steer(options, behaviors.get(), bot, tick);
            bot.link->send(world, bot.world->workspace, tick);
            bot.link->sendMessages(world);
        }

        // **One line a second, for whoever started this process.**
        if (now >= nextReportNs) {
            nextReportNs += 1'000'000'000ull;
            u32 connected = 0;
            u32 failed = 0;
            u32 dropped = 0;
            u32 retried = 0;
            // In line at a full game's door (ADR 0232).
            u32 queued = 0;
            u64 bytesIn = 0;
            u64 snapshots = 0;
            u32 travels = 0;
            for (const Bot& bot : bots) {
                if (bot.failed)
                    failed += 1;
                else if (bot.dropped)
                    dropped += 1;
                else if (bot.welcomed)
                    connected += 1;
                retried += bot.attempts > 1 ? bot.attempts - 1 : 0;
                if (!bot.welcomed && bot.link != nullptr && bot.link->status().queuePosition != 0)
                    queued += 1;
                bytesIn += bot.bytesIn;
                snapshots += bot.snapshots;
                travels += bot.travels;
                if (bot.link != nullptr) {
                    const replication::Stats stats = bot.link->stats();
                    bytesIn += stats.bytesReceived;
                    snapshots += stats.snapshotsReceived;
                }
            }
            core::JsonWriter line(core::JsonLayout::Compact);
            line.beginObject();
            line.field("type", "bots");
            line.field("seconds", static_cast<f64>(now - started) / 1.0e9);
            line.field("bots", static_cast<u64>(bots.size()));
            line.field("connected", static_cast<u64>(connected));
            line.field("failed", static_cast<u64>(failed));
            line.field("dropped", static_cast<u64>(dropped));
            line.field("retries", static_cast<u64>(retried));
            line.field("queued", static_cast<u64>(queued));
            line.field("bytes_in", bytesIn);
            line.field("snapshots", snapshots);
            line.field("travels", static_cast<u64>(travels));
            if (behaviors != nullptr) {
                line.field("behaviors_failed", static_cast<u64>(behaviors->failed()));
                line.field("behaviors_finished", static_cast<u64>(behaviors->finished()));
            }
            line.endObject();
            const std::string written = managerLine(line);
            std::fwrite(written.data(), 1, written.size(), stdout);
            std::fflush(stdout);
        }
    }
    for (Bot& bot : bots) {
        if (bot.link != nullptr)
            bot.link->shutdown();
    }
    return 0;
}

#else

// A build without the network has no bot to dial with.
int runBots(const BotOptions&)
{
    core::log(core::LogLevel::Error, ENG_TR("engine.cli.err.no_replication"));
    return 2;
}

#endif

} // namespace engine::app
