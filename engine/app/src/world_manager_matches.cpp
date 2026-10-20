// **Matchmaking and lobbies under the manager** (ADR 0243): what a world
// asks of the match book, the instance a match is played in, its players sent
// there from whatever world each is in, and the ratings kept in the data
// service.
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/app/world_manager.h"
#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/script/save_store.h"

namespace engine::app {
namespace {

constexpr core::f64 StartingRating = 1500.0;
// Where ratings are kept, as a game reads them with `DataService:GetAsync`.
constexpr std::string_view RatingScope = "rating";

[[nodiscard]] std::string ratingKey(std::string_view name, std::string_view member)
{
    return std::string(name) + ":" + std::string(member);
}

// **A value as the data service keeps one**: a saved value's text, so a game
// reads a rating, or the match an instance is for, as a script wrote it.
[[nodiscard]] std::string savedText(script::SaveValue value)
{
    script::SaveSlotData slot;
    slot.values.emplace("data", std::move(value));
    return script::SaveStore::encode(slot, 0.0);
}

[[nodiscard]] std::vector<std::string> everyone(const Match& match)
{
    std::vector<std::string> out;
    for (const std::vector<std::string>& team : match.teams)
        out.insert(out.end(), team.begin(), team.end());
    return out;
}

void answerLobby(core::JsonWriter& answer, const MatchBook::Answer& done)
{
    answer.field("ok", done.ok);
    answer.field("reason", std::string_view{done.reason});
    if (done.lobby.has_value()) {
        answer.key("lobby");
        writeLobby(answer, *done.lobby);
    }
}

} // namespace

std::vector<std::string> WorldManager::keysOf(const std::vector<std::string>& members) const
{
    std::vector<std::string> keys;
    for (const std::unique_ptr<World>& world : m_worlds) {
        for (const auto& [key, account] : world->present) {
            if (std::find(members.begin(), members.end(), memberOf(key, account)) != members.end() &&
                std::find(keys.begin(), keys.end(), key) == keys.end())
                keys.push_back(key);
        }
    }
    return keys;
}

core::f64 WorldManager::rating(std::string_view name, std::string_view member)
{
    if (m_data == nullptr)
        return StartingRating;
    const SharedDatum held = m_data->store().getShared(RatingScope, ratingKey(name, member));
    script::SaveSlotData slot;
    if (held.version == 0 || !script::SaveStore::decode(held.value, slot))
        return StartingRating;
    const auto found = slot.values.find("data");
    if (found == slot.values.end() || !std::holds_alternative<core::f64>(found->second.scalar))
        return StartingRating;
    return std::get<core::f64>(found->second.scalar);
}

void WorldManager::setRating(std::string_view name, std::string_view member, core::f64 value)
{
    if (m_data == nullptr)
        return;
    const std::string key = ratingKey(name, member);
    const std::string text = savedText(script::SaveValue{scene::Value{value}, nullptr});
    // Only this thread writes the manager's store: the version read is the
    // version written over.
    const SharedDatum held = m_data->store().getShared(RatingScope, key);
    (void)m_data->store().setShared(RatingScope, key, text, held.version);
}

void WorldManager::matchOp(World& asker, const ManagerMessage& message, core::u64 nowNs)
{
    const core::JsonValue root = message.root();
    const auto id = static_cast<core::u32>(root["id"].asInteger(0));
    core::JsonDocument asked;
    if (!asked.parse(root["request"].asString(), "match-op"))
        return;
    const core::JsonValue request = asked.root();
    const std::string_view op = request["op"].asString();
    const std::string member(request["member"].asString());
    MatchBook& book = *m_matches;
    core::JsonWriter answer(core::JsonLayout::Compact);
    answer.beginObject();
    if (op == "define") {
        MatchQueue queue;
        queue.name = std::string(request["queue"].asString());
        queue.templateName = std::string(request["template"].asString());
        queue.teams = static_cast<core::u32>(std::clamp<core::f64>(request["teams"].asNumber(2.0), 1.0, 64.0));
        queue.teamSize = static_cast<core::u32>(std::clamp<core::f64>(request["teamSize"].asNumber(1.0), 1.0, 100.0));
        queue.rating = std::string(request["rating"].asString());
        queue.widenAfterSeconds = std::max(0.0, request["after"].asNumber(10.0));
        queue.widenStep = std::max(0.0, request["step"].asNumber(100.0));
        queue.widenMost = std::max(queue.widenStep, request["most"].asNumber(600.0));
        queue.regions = request["regions"].asBool(true);
        book.define(std::move(queue));
    }
    else if (op == "queue") {
        // **A party queues as one**: its members online, wherever they are.
        std::vector<std::string> members;
        bool known = true;
        if (const std::string_view partyId = request["party"].asString(); !partyId.empty()) {
            const std::optional<Party> party = m_parties != nullptr ? m_parties->find(partyId) : std::nullopt;
            known = party.has_value();
            if (known) {
                for (const std::string& one : party->members) {
                    if (!keysOf({one}).empty())
                        members.push_back(one);
                }
            }
        }
        else {
            members.push_back(member);
        }
        const std::string queueName(request["queue"].asString());
        if (!known || members.empty()) {
            answer.field("ok", false);
            answer.field("reason", "NoParty");
        }
        else {
            core::f64 mean = 0.0;
            if (const MatchQueue* queue = book.queue(queueName); queue != nullptr && !queue->rating.empty()) {
                for (const std::string& one : members)
                    mean += rating(queue->rating, one);
                mean /= static_cast<core::f64>(members.size());
            }
            const std::string_view named = request["region"].asString();
            const std::string_view region = named.empty() ? std::string_view{m_options.config.region} : named;
            const MatchBook::Answer done = book.enqueue(queueName, std::move(members), mean, region, nowNs);
            answer.field("ok", done.ok);
            answer.field("reason", std::string_view{done.reason});
            if (done.ticket.has_value())
                answer.field("ticket", std::string_view{done.ticket->id});
        }
    }
    else if (op == "cancel") {
        const MatchBook::Answer done = book.cancel(request["ticket"].asString());
        answer.field("ok", done.ok);
        answer.field("reason", std::string_view{done.reason});
    }
    else if (op == "backfill") {
        const auto team = request["team"].asInteger(-1);
        const MatchBook::Answer done = team < 0
                                           ? MatchBook::Answer{false, "Invalid", std::nullopt, std::nullopt}
                                           : book.backfill(request["match"].asString(), static_cast<core::u32>(team));
        answer.field("ok", done.ok);
        answer.field("reason", std::string_view{done.reason});
        if (done.ticket.has_value())
            answer.field("ticket", std::string_view{done.ticket->id});
    }
    else if (op == "report") {
        const Match* match = book.match(request["match"].asString());
        const MatchQueue* queue = match != nullptr ? book.queue(match->queue) : nullptr;
        const auto team = request["team"].asInteger(-1);
        if (match == nullptr) {
            answer.field("ok", false);
            answer.field("reason", "NoMatch");
        }
        else if (queue == nullptr || queue->rating.empty()) {
            answer.field("ok", false);
            answer.field("reason", "NoRating");
        }
        else if (team < 0 || static_cast<core::usize>(team) >= match->teams.size()) {
            answer.field("ok", false);
            answer.field("reason", "Invalid");
        }
        else {
            // **Elo, from the ratings as they are now**, written back each.
            std::map<std::string, core::f64> ratings;
            for (const std::string& one : everyone(*match))
                ratings[one] = rating(queue->rating, one);
            applyElo(*match, static_cast<core::u32>(team), ratings);
            for (const auto& [one, value] : ratings)
                setRating(queue->rating, one, value);
            answer.field("ok", true);
        }
    }
    else if (op == "getRating") {
        answer.field("ok", true);
        answer.field("rating", rating(request["rating"].asString(), member));
    }
    else if (op == "setRating") {
        setRating(request["rating"].asString(), member, request["value"].asNumber(StartingRating));
        answer.field("ok", true);
    }
    else if (op == "createLobby") {
        const auto size = static_cast<core::u32>(std::clamp<core::f64>(request["size"].asNumber(8.0), 1.0, 100.0));
        answerLobby(answer,
                    book.createLobby(member, request["template"].asString(), size, request["public"].asBool(false)));
    }
    else if (op == "joinLobby") {
        answerLobby(answer, book.joinLobby(member, request["code"].asString()));
    }
    else if (op == "leaveLobby") {
        answerLobby(answer, book.leaveLobby(member));
    }
    else if (op == "ready") {
        answerLobby(answer, book.setReady(member, request["ready"].asBool(false)));
    }
    else if (op == "listLobbies") {
        answer.field("ok", true);
        answer.key("lobbies");
        answer.beginArray();
        for (const Lobby& lobby : book.listLobbies(request["template"].asString()))
            writeLobby(answer, lobby);
        answer.endArray();
    }
    else if (op == "startLobby") {
        std::string why;
        const std::optional<Match> match = book.startLobby(request["lobby"].asString(), member, why);
        const World* instance = match.has_value() ? startMatch(*match) : nullptr;
        if (match.has_value() && instance == nullptr)
            why = "NoInstance";
        answer.field("ok", instance != nullptr);
        answer.field("reason", std::string_view{why});
        if (instance != nullptr) {
            answer.key("match");
            writeMatch(answer, *match);
            answer.field("instance", std::string_view{instance->config.name});
        }
    }
    else {
        answer.field("ok", false);
        answer.field("reason", "Invalid");
    }
    answer.endObject();
    if (id != 0) {
        core::JsonWriter result(core::JsonLayout::Compact);
        result.beginObject();
        result.field("type", "match-result");
        result.field("id", static_cast<core::i64>(id));
        result.field("answer", std::string_view{answer.text()});
        result.endObject();
        send(asker, result);
    }
    matchNotices();
}

WorldManager::World* WorldManager::startMatch(const Match& match)
{
    std::string why;
    World* instance = openInstance(match.templateName, why);
    if (instance == nullptr) {
        // **No room for it**: the match is let go of, its players free to
        // queue again, and the operator told why.
        m_matches->endMatch(match.id);
        const std::array<core::I18nArg, 2> args{core::I18nArg{"match", std::string_view{match.id}},
                                                core::I18nArg{"why", std::string_view{why}}};
        core::log(core::LogLevel::Warn, ENG_TR("manager.warn.match_no_instance"), args);
        return nullptr;
    }
    instance->askedKeys = keysOf(everyone(match));
    core::JsonWriter asked(core::JsonLayout::Compact);
    writeMatch(asked, match);
    instance->askedMatch = asked.text();
    instance->match = match.id;
    if (instance->state == State::Ready)
        allocate(*instance);
    topUpWarm();
    return instance;
}

void WorldManager::sendToMatch(const Match& match, const World& instance, const std::vector<std::string>& members)
{
    // **`Matched` in each world one of them is in, then the hop** -- the
    // travel order a party's is, from that world.
    core::JsonWriter body(core::JsonLayout::Compact);
    body.beginObject();
    body.key("match");
    writeMatch(body, match);
    body.field("instance", std::string_view{instance.config.name});
    body.endObject();
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world->process == nullptr || world.get() == &instance)
            continue;
        std::vector<std::string> keys;
        for (const auto& [key, account] : world->present) {
            if (std::find(members.begin(), members.end(), memberOf(key, account)) != members.end())
                keys.push_back(key);
        }
        if (keys.empty())
            continue;
        core::JsonWriter made(core::JsonLayout::Compact);
        made.beginObject();
        made.field("type", "match-made");
        made.field("body", std::string_view{body.text()});
        made.endObject();
        send(*world, made);
        core::JsonWriter order(core::JsonLayout::Compact);
        order.beginObject();
        order.field("type", "party-travel");
        order.key("keys");
        order.beginArray();
        for (const std::string& key : keys)
            order.value(std::string_view{key});
        order.endArray();
        order.field("target", std::string_view{instance.config.name});
        order.field("data", "");
        order.endObject();
        send(*world, order);
    }
}

void WorldManager::matchReady(World& instance)
{
    if (const Match* match = m_matches->match(instance.match); match != nullptr)
        sendToMatch(*match, instance, everyone(*match));
}

void WorldManager::matchFrame(core::u64 nowNs)
{
    for (const Match& match : m_matches->form(nowNs))
        (void)startMatch(match);
    // **A place filled**: its player let into the running instance, and
    // sent.
    for (const MatchBackfill& filled : m_matches->takeBackfills()) {
        const Match* match = m_matches->match(filled.match);
        const auto instance = std::find_if(m_worlds.begin(), m_worlds.end(),
                                           [&](const auto& world) { return world->match == filled.match; });
        if (match == nullptr || instance == m_worlds.end())
            continue;
        for (const std::string& key : keysOf(filled.members))
            (*instance)->allowed.insert(key);
        if ((*instance)->state == State::Ready)
            sendToMatch(*match, **instance, filled.members);
    }
    // **Who left a match**: in its instance once, and in it no longer.
    for (const std::unique_ptr<World>& world : m_worlds) {
        if (world->match.empty() || world->state != State::Ready)
            continue;
        const Match* match = m_matches->match(world->match);
        if (match == nullptr)
            continue;
        std::set<std::string> here;
        for (const auto& [key, account] : world->present)
            here.insert(memberOf(key, account));
        for (const std::string& one : everyone(*match)) {
            if (here.contains(one))
                world->matchArrived.insert(one);
            else if (world->matchArrived.erase(one) > 0)
                m_matches->leaveMatch(one);
        }
    }
    matchNotices();
}

void WorldManager::matchNotices()
{
    // **A lobby's change, to every world one of its members is in**; one
    // gone, to the worlds its members were in, with nobody in it.
    const auto tell = [this](const Lobby& lobby, const std::vector<std::string>& members) {
        core::JsonWriter body(core::JsonLayout::Compact);
        writeLobby(body, lobby);
        for (const std::unique_ptr<World>& world : m_worlds) {
            const bool member = std::any_of(world->present.begin(), world->present.end(), [&](const auto& entry) {
                return std::find(members.begin(), members.end(), memberOf(entry.first, entry.second)) != members.end();
            });
            if (!member || world->process == nullptr)
                continue;
            core::JsonWriter notice(core::JsonLayout::Compact);
            notice.beginObject();
            notice.field("type", "lobby-changed");
            notice.field("body", std::string_view{body.text()});
            notice.endObject();
            send(*world, notice);
        }
    };
    for (const std::string& id : m_matches->takeChangedLobbies()) {
        if (const std::optional<Lobby> lobby = m_matches->findLobby(id); lobby.has_value())
            tell(*lobby, lobby->members);
    }
    for (Lobby gone : m_matches->takeGoneLobbies()) {
        const std::vector<std::string> members = std::move(gone.members);
        gone.members.clear();
        gone.ready.clear();
        tell(gone, members);
    }
}

} // namespace engine::app
