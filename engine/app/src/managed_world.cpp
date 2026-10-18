#include "engine/app/managed_world.h"

#include <algorithm>
#include <span>
#include <utility>

#include "engine/core/base64.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/platform.h"
#include "engine/replication/replication.h"
#include "engine/replication/session.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"

namespace engine::app {
namespace {

constexpr core::u64 HeartbeatNs = 1'000'000'000ull;

[[nodiscard]] core::TextKey refusalKey(PassRefusal why)
{
    switch (why) {
    case PassRefusal::Missing:
        return ENG_TR("net.err.pass_missing");
    case PassRefusal::NotTheManagers:
        return ENG_TR("net.err.pass_forged");
    case PassRefusal::OtherWorld:
        return ENG_TR("net.err.pass_other_world");
    case PassRefusal::OtherPlayer:
        return ENG_TR("net.err.pass_other_player");
    case PassRefusal::Expired:
        return ENG_TR("net.err.pass_expired");
    case PassRefusal::Used:
        break;
    }
    return ENG_TR("net.err.pass_used");
}

} // namespace

core::u32 channelOf(std::string_view world) noexcept
{
    const std::size_t colon = world.rfind(':');
    if (colon == std::string_view::npos)
        return 1;
    core::u32 channel = 0;
    for (const char digit : world.substr(colon + 1)) {
        if (digit < '0' || digit > '9' || channel > 1000)
            return 1;
        channel = channel * 10 + static_cast<core::u32>(digit - '0');
    }
    return channel == 0 ? 1 : channel;
}

ManagedWorld::ManagedWorld(std::string name, std::array<core::u8, 32> managerKey, Send send)
    : m_name(std::move(name)), m_managerKey(managerKey), m_send(std::move(send))
{}

void ManagedWorld::dropCrossWorld(scene::EngineState& state)
{
    ++state.crossWorldDropped;
    // Said once a minute, however many.
    const std::int64_t now = m_unixClock ? m_unixClock() : 0;
    if (now >= m_crossWorldSaidAt + 60) {
        m_crossWorldSaidAt = now;
        const std::array<core::I18nArg, 1> args{
            core::I18nArg{"dropped", static_cast<core::i64>(state.crossWorldDropped)}};
        core::log(core::LogLevel::Warn, ENG_TR("net.warn.cross_world_dropped"), args);
    }
}

void ManagedWorld::send(const core::JsonWriter& message) const
{
    if (m_send)
        m_send(managerLine(message));
}

void ManagedWorld::take(const ManagerMessage& message, scene::World& world, replication::IReplication* replication)
{
    const std::string_view type = message.type();
    const core::JsonValue root = message.root();
    scene::EngineState& state = world.engineState();
    if (type == "hello") {
        state.managerWorlds.clear();
        for (core::usize at = 0; at < root["worlds"].size(); ++at)
            state.managerWorlds.emplace_back(root["worlds"].at(at).asString());
        return;
    }
    if (type == "passes") {
        // **The manager's passes for a travel**: each traveller is sent on.
        // Id 0 is the manager's own: every player named, moved -- an update
        // (ADR 0220 §7).
        const core::u32 id = static_cast<core::u32>(root["id"].asInteger(0));
        const auto travel = m_travels.find(id);
        if (id != 0 && travel == m_travels.end())
            return;
        const std::string_view address = root["address"].asString();
        const core::u16 port = static_cast<core::u16>(root["port"].asInteger(0));
        const core::JsonValue passes = root["passes"];
        for (core::usize at = 0; at < passes.size(); ++at) {
            const std::string key(passes.at(at)["key"].asString());
            core::u32 userId = 0;
            if (id != 0) {
                const auto who = travel->second.find(key);
                userId = who != travel->second.end() ? who->second : 0;
            }
            else if (m_dataModel.valid()) {
                const core::InstanceId network = scene::networkServiceOf(world, m_dataModel);
                for (core::InstanceId child = network.valid() ? world.firstChild(network) : core::InstanceId{};
                     child.valid(); child = world.nextSibling(child)) {
                    const scene::PlayerComponent* player = world.players().find(child);
                    if (player != nullptr && player->publicKey == key)
                        userId = player->userId;
                }
            }
            if (userId != 0 && replication != nullptr)
                (void)replication->redirect(userId, address, port, passes.at(at)["pass"].asString(),
                                            root["key"].asString(), root["world"].asString());
        }
        if (id != 0)
            m_travels.erase(travel);
        return;
    }
    if (type == "travel-failed") {
        m_travels.erase(static_cast<core::u32>(root["id"].asInteger(0)));
        const std::array<core::I18nArg, 2> args{core::I18nArg{"target", root["target"].asString()},
                                                core::I18nArg{"why", root["why"].asString()}};
        core::log(core::LogLevel::Warn, ENG_TR("net.warn.travel_failed"), args);
        return;
    }
    if (type == "remove") {
        // **The same account came in through another world** (ADR 0226 §4).
        const std::string_view key = root["key"].asString();
        const core::InstanceId network = scene::networkServiceOf(world, m_dataModel);
        for (core::InstanceId child = network.valid() ? world.firstChild(network) : core::InstanceId{}; child.valid();
             child = world.nextSibling(child)) {
            const scene::PlayerComponent* player = world.players().find(child);
            if (player != nullptr && player->publicKey == key)
                state.pendingRemovals.emplace_back(
                    player->userId, core::engineCatalog().format(ENG_TR("net.info.signed_in_elsewhere")));
        }
        return;
    }
    // **Instances** (ADR 0221): the manager's answer to one this world asked
    // for, and -- in an instance -- its allocation and its closing.
    if (type == "instance-ready" || type == "instance-failed") {
        state.instanceAnswers.push_back(scene::EngineState::InstanceAnswer{
            static_cast<core::u32>(root["id"].asInteger(0)), std::string(root["instance"].asString()),
            type == "instance-failed" ? std::string(root["why"].asString()) : std::string{}});
        return;
    }
    if (type == "allocate") {
        // The template's scene again, with the group's `Data`.
        const std::optional<std::vector<core::u8>> data = core::base64Decode(root["data"].asString());
        const std::string scene(root["scene"].asString());
        if (data.has_value() && !data->empty() && !scene.empty())
            state.pendingSceneLoad = scene::EngineState::PendingSceneLoad{scene, *data};
        return;
    }
    if (type == "instance-closed") {
        state.instancesClosed.push_back(scene::EngineState::InstanceClosed{std::string(root["instance"].asString()),
                                                                           std::string(root["reason"].asString())});
        return;
    }
    if (type == "deliver") {
        // **An event from the game's worlds** (ADR 0223), held for the
        // callbacks -- unless it is old news, or more than this world can keep.
        const std::int64_t sent = root["sent"].asInteger(0);
        if (m_unixClock && m_unixClock() > sent + CrossWorldOldSeconds) {
            dropCrossWorld(state);
            return;
        }
        state.crossWorldIn.push_back(scene::EngineState::CrossWorldMessage{std::string(root["topic"].asString()),
                                                                           std::string(root["message"].asString()),
                                                                           std::string(root["origin"].asString())});
        std::size_t bytes = 0;
        for (const scene::EngineState::CrossWorldMessage& held : state.crossWorldIn)
            bytes += held.message.size() + held.topic.size();
        while (state.crossWorldIn.size() > CrossWorldMostHeld || bytes > CrossWorldMostBytes) {
            bytes -= state.crossWorldIn.front().message.size() + state.crossWorldIn.front().topic.size();
            state.crossWorldIn.pop_front();
            dropCrossWorld(state);
        }
        return;
    }
    if (type == "closing") {
        state.instanceClosing = true;
        return;
    }
    // **Channels** (ADR 0230): their list, asked for; and this one to be
    // merged into another, in so many seconds.
    if (type == "channels") {
        scene::EngineState::ChannelAnswer answer;
        answer.id = static_cast<core::u32>(root["id"].asInteger(0));
        const core::JsonValue list = root["channels"];
        for (core::usize at = 0; at < list.size(); ++at)
            answer.channels.push_back(
                scene::EngineState::ChannelInfo{static_cast<core::u32>(list.at(at)["channel"].asInteger(1)),
                                                static_cast<core::u32>(list.at(at)["players"].asInteger(0)),
                                                static_cast<core::u32>(list.at(at)["max"].asInteger(0))});
        state.channelAnswers.push_back(std::move(answer));
        return;
    }
    // **A record the manager's database gave, or refused** (ADR 0238).
    if (type == "data-record") {
        scene::EngineState::DataAnswer answer;
        answer.id = static_cast<core::u32>(root["id"].asInteger(0));
        answer.ok = root["ok"].asBool(false);
        answer.busy = root["busy"].asBool(false);
        answer.found = root["found"].asBool(false);
        answer.version = root["version"].asNumber(1.0);
        if (const std::optional<std::vector<core::u8>> body = core::base64Decode(root["body"].asString());
            body.has_value())
            answer.body.assign(body->begin(), body->end());
        state.dataAnswers.push_back(std::move(answer));
        return;
    }
    if (type == "channel-merging") {
        state.channelMerging.push_back(root["seconds"].asNumber(60.0));
        return;
    }
    if (type == "drain") {
        m_draining = true;
        return;
    }
    if (type == "stop")
        m_stopping = true;
}

std::optional<replication::JoinAnswer> ManagedWorld::admit(replication::JoinAsk& ask, scene::World& world,
                                                           std::int64_t nowUnix)
{
    if (m_draining)
        return replication::JoinAnswer{
            .admit = false, .reason = core::engineCatalog().format(ENG_TR("net.err.world_draining")), .accountId = {}};
    PassRefusal why = PassRefusal::Missing;
    const std::optional<TravelPass> pass =
        openPass(ask.pass, m_managerKey, m_name, ask.publicKey, nowUnix, m_replay, why);
    if (!pass.has_value()) {
        const std::string said = core::engineCatalog().format(refusalKey(why));
        const std::array<core::I18nArg, 2> args{core::I18nArg{"address", std::string_view{ask.address}},
                                                core::I18nArg{"why", std::string_view{said}}};
        core::log(core::LogLevel::Info, ENG_TR("net.info.pass_refused"), args);
        // **What only a forger does is a suspicion** (ADR 0241 §3); a pass
        // missing or out of date is a player who took the wrong door.
        if (why == PassRefusal::NotTheManagers || why == PassRefusal::OtherPlayer || why == PassRefusal::OtherWorld ||
            why == PassRefusal::Used)
            scene::suspect(world, core::InstanceId{}, "pass_refused", said,
                           ask.publicKey.empty() ? std::string{} : "key:" + ask.publicKey);
        return replication::JoinAnswer{.admit = false, .reason = said, .accountId = {}};
    }
    // What the player logged in with at the front door is what the game's
    // `OnJoinRequest` reads here.
    ask.ticket = pass->ticket;
    world.engineState().travelArrivals.push_back(
        scene::EngineState::TravelArrival{ask.publicKey, pass->origin, pass->data, 0, pass->reason});
    return std::nullopt;
}

void ManagedWorld::frame(scene::World& world, core::InstanceId dataModel, replication::IReplication* replication,
                         core::u16 port, core::u64 nowNs, core::f64 tickMs)
{
    scene::EngineState& state = world.engineState();
    state.managedWorld = m_name;
    state.networkChannel = channelOf(m_name);
    m_dataModel = dataModel;
    if (!m_ready && replication != nullptr && port != 0) {
        m_ready = true;
        m_nextHeartbeatNs = nowNs + HeartbeatNs;
        core::JsonWriter ready(core::JsonLayout::Compact);
        ready.beginObject();
        ready.field("type", "ready");
        ready.field("port", static_cast<core::i64>(port));
        ready.field("key", std::string_view{m_key});
#if ENG_ENABLE_REPLICATION
        ready.field("protocol", static_cast<core::i64>(replication::protocolVersion()));
#else
        ready.field("protocol", static_cast<core::i64>(0));
#endif
        ready.endObject();
        send(ready);
    }
    m_tickSum += tickMs;
    m_tickWorst = std::max(m_tickWorst, tickMs);
    ++m_ticks;

    // Who is in now, against who was: told as they come and go.
    std::map<std::string, std::string> present;
    const core::InstanceId network = scene::networkServiceOf(world, dataModel);
    for (core::InstanceId child = network.valid() ? world.firstChild(network) : core::InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        const scene::PlayerComponent* player = world.players().find(child);
        // Who joined: a host's own players are the host's, never moved.
        if (player != nullptr && !player->local && !player->publicKey.empty() && !world.destroyed(child))
            present.emplace(player->publicKey, player->accountId);
    }
    for (const auto& [key, account] : present) {
        if (m_present.contains(key))
            continue;
        core::JsonWriter joined(core::JsonLayout::Compact);
        joined.beginObject();
        joined.field("type", "player-joined");
        joined.field("key", std::string_view{key});
        joined.field("account", std::string_view{account});
        joined.endObject();
        send(joined);
    }
    for (const auto& [key, account] : m_present) {
        if (present.contains(key))
            continue;
        core::JsonWriter left(core::JsonLayout::Compact);
        left.beginObject();
        left.field("type", "player-left");
        left.field("key", std::string_view{key});
        left.endObject();
        send(left);
    }
    m_present = std::move(present);

    // **Who a script sends elsewhere** (ADR 0222): the manager is asked for
    // their passes.
    std::vector<scene::EngineState::TravelRequest> travels;
    travels.swap(state.travelRequests);
    for (const scene::EngineState::TravelRequest& travel : travels) {
        std::map<std::string, core::u32> who;
        core::JsonWriter asked(core::JsonLayout::Compact);
        asked.beginObject();
        asked.field("type", "travel");
        asked.field("id", static_cast<core::i64>(travel.id));
        asked.field("target", std::string_view{travel.target});
        asked.field("data", std::string_view{travel.data});
        if (travel.channel != 0)
            asked.field("channel", static_cast<core::i64>(travel.channel));
        if (!travel.toPlayer.empty())
            asked.field("toPlayer", std::string_view{travel.toPlayer});
        asked.key("players");
        asked.beginArray();
        for (const core::u32 userId : travel.userIds) {
            const core::InstanceId player = scene::playerByUserId(world, userId);
            const scene::PlayerComponent* component = world.players().find(player);
            if (component == nullptr || component->publicKey.empty())
                continue;
            who.emplace(component->publicKey, userId);
            asked.beginObject();
            asked.field("key", std::string_view{component->publicKey});
            asked.field("account", std::string_view{component->accountId});
            asked.endObject();
        }
        asked.endArray();
        asked.endObject();
        if (who.empty())
            continue;
        m_travels[travel.id] = std::move(who);
        send(asked);
    }

    // The channels, asked for (ADR 0230).
    std::vector<core::u32> asks;
    asks.swap(state.channelRequests);
    for (const core::u32 id : asks) {
        core::JsonWriter asked(core::JsonLayout::Compact);
        asked.beginObject();
        asked.field("type", "channels");
        asked.field("id", static_cast<core::i64>(id));
        asked.endObject();
        send(asked);
    }

    sendData(state);

    // **Events for the game's other worlds**, and the topics it hears.
    std::vector<scene::EngineState::CrossWorldMessage> published;
    published.swap(state.crossWorldOut);
    for (const scene::EngineState::CrossWorldMessage& message : published) {
        core::JsonWriter publish(core::JsonLayout::Compact);
        publish.beginObject();
        publish.field("type", "publish");
        publish.field("topic", std::string_view{message.topic});
        publish.field("message", std::string_view{message.message});
        publish.endObject();
        send(publish);
    }
    std::vector<std::pair<std::string, bool>> topics;
    topics.swap(state.crossWorldTopics);
    for (const auto& [topic, on] : topics) {
        core::JsonWriter subscription(core::JsonLayout::Compact);
        subscription.beginObject();
        subscription.field("type", on ? "subscribe" : "unsubscribe");
        subscription.field("topic", std::string_view{topic});
        subscription.endObject();
        send(subscription);
    }
    // An instance asked for, and what an instance says of itself.
    std::vector<scene::EngineState::InstanceRequest> requests;
    requests.swap(state.instanceRequests);
    for (const scene::EngineState::InstanceRequest& request : requests) {
        core::JsonWriter asked(core::JsonLayout::Compact);
        asked.beginObject();
        asked.field("type", "instance-create");
        asked.field("id", static_cast<core::i64>(request.id));
        asked.field("template", std::string_view{request.templateName});
        asked.field("data", std::string_view{core::base64Encode(request.data)});
        asked.key("players");
        asked.beginArray();
        for (const std::string& key : request.keys)
            asked.value(std::string_view{key});
        asked.endArray();
        asked.endObject();
        send(asked);
    }
    std::vector<scene::EngineState::InstanceControl> controls;
    controls.swap(state.instanceControls);
    for (const scene::EngineState::InstanceControl& said : controls) {
        using Kind = scene::EngineState::InstanceControl::Kind;
        core::JsonWriter message(core::JsonLayout::Compact);
        message.beginObject();
        message.field("type", said.kind == Kind::Allow      ? "instance-allow"
                              : said.kind == Kind::Disallow ? "instance-disallow"
                              : said.kind == Kind::Close    ? "instance-close"
                                                            : "instance-keep-open");
        message.field("key", std::string_view{said.key});
        message.field("seconds", said.seconds);
        message.endObject();
        send(message);
    }

    if (m_ready && nowNs >= m_nextHeartbeatNs) {
        m_nextHeartbeatNs = nowNs + HeartbeatNs;
        core::JsonWriter beat(core::JsonLayout::Compact);
        beat.beginObject();
        beat.field("type", "heartbeat");
        beat.field("tickMs", m_ticks == 0 ? 0.0 : m_tickSum / static_cast<core::f64>(m_ticks));
        beat.field("tickWorstMs", m_tickWorst);
        beat.field("players", static_cast<core::i64>(m_present.size()));
        beat.field("memoryMb", static_cast<core::i64>(platform::residentBytes() / (1024u * 1024u)));
        beat.endObject();
        send(beat);
        m_tickSum = 0.0;
        m_tickWorst = 0.0;
        m_ticks = 0;
    }
}

void ManagedWorld::sendData(scene::EngineState& state) const
{
    // **The records scripts asked for, and the writes** (ADR 0238), to the
    // manager's database.
    state.dataAvailable = true;
    std::vector<scene::EngineState::DataLoad> loads;
    loads.swap(state.dataLoads);
    for (const scene::EngineState::DataLoad& load : loads) {
        core::JsonWriter asked(core::JsonLayout::Compact);
        asked.beginObject();
        asked.field("type", "data-load");
        asked.field("id", static_cast<core::i64>(load.id));
        asked.field("account", std::string_view{load.account});
        asked.endObject();
        send(asked);
    }
    std::vector<scene::EngineState::DataWrite> writes;
    writes.swap(state.dataWrites);
    for (const scene::EngineState::DataWrite& write : writes) {
        core::JsonWriter written(core::JsonLayout::Compact);
        written.beginObject();
        written.field("type", "data-write");
        written.field("account", std::string_view{write.account});
        written.field("body", std::string_view{core::base64Encode(std::span<const core::u8>(
                                  reinterpret_cast<const core::u8*>(write.body.data()), write.body.size()))});
        written.field("version", write.version);
        written.field("release", write.release);
        written.endObject();
        send(written);
    }
}

} // namespace engine::app
