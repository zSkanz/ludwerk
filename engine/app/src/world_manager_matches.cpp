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

// **Ratings, on the store's thread** (the data worker's): what the manager's
// loop asks of them is a job, and the answer comes back to it.
namespace {

core::f64 ratingIn(IDataStore& store, std::string_view name, std::string_view member)
{
    const SharedDatum held = store.getShared(RatingScope, ratingKey(name, member));
    script::SaveSlotData slot;
    if (held.version == 0 || !script::SaveStore::decode(held.value, slot))
        return StartingRating;
    const auto found = slot.values.find("data");
    if (found == slot.values.end() || !std::holds_alternative<core::f64>(found->second.scalar))
        return StartingRating;
    return std::get<core::f64>(found->second.scalar);
}

void setRatingIn(IDataStore& store, std::string_view name, std::string_view member, core::f64 value)
{
    const std::string key = ratingKey(name, member);
    const std::string text = savedText(script::SaveValue{scene::Value{value}, nullptr});
    // Only the store's thread writes it: the version read is the version
    // written over.
    const SharedDatum held = store.getShared(RatingScope, key);
    (void)store.setShared(RatingScope, key, text, held.version);
}

[[nodiscard]] std::string refusal(std::string_view reason)
{
    core::JsonWriter answer(core::JsonLayout::Compact);
    answer.beginObject();
    answer.field("ok", false);
    answer.field("reason", reason);
    answer.endObject();
    return answer.text();
}

} // namespace

void WorldManager::matchAnswer(const std::string& world, core::u32 id, const std::string& answer)
{
    if (id == 0)
        return;
    const auto found = std::find_if(m_worlds.begin(), m_worlds.end(), [&](const std::unique_ptr<World>& one) {
        return one->config.name == world && one->process != nullptr;
    });
    if (found == m_worlds.end())
        return;
    core::JsonWriter result(core::JsonLayout::Compact);
    result.beginObject();
    result.field("type", "match-result");
    result.field("id", static_cast<core::i64>(id));
    result.field("answer", std::string_view{answer});
    result.endObject();
    send(**found, result);
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
    const std::string askerName = asker.config.name;
    // **Asked of the store**: answered when the job is back, not here.
    bool deferred = false;
    const auto onStore = [&](DataWorker::Job job) {
        deferred = true;
        if (m_data == nullptr || !m_data->post(std::move(job)))
            matchAnswer(askerName, id, refusal("Unavailable"));
    };
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
            const MatchQueue* queue = book.queue(queueName);
            const std::string ratingName = queue != nullptr ? queue->rating : std::string{};
            const std::string_view named = request["region"].asString();
            const std::string region(named.empty() ? std::string_view{m_options.config.region} : named);
            // **The ticket's rating is its members' mean**, read on the
            // store's thread; the ticket is made here once it is known.
            onStore([this, askerName, id, queueName, ratingName, region, members,
                     nowNs](IDataStore& store) mutable -> DataWorker::Then {
                core::f64 mean = 0.0;
                if (!ratingName.empty()) {
                    for (const std::string& one : members)
                        mean += ratingIn(store, ratingName, one);
                    mean /= static_cast<core::f64>(members.size());
                }
                return [this, askerName, id, queueName, region, members = std::move(members), mean, nowNs]() mutable {
                    const MatchBook::Answer done =
                        m_matches->enqueue(queueName, std::move(members), mean, region, nowNs);
                    core::JsonWriter queued(core::JsonLayout::Compact);
                    queued.beginObject();
                    queued.field("ok", done.ok);
                    queued.field("reason", std::string_view{done.reason});
                    if (done.ticket.has_value())
                        queued.field("ticket", std::string_view{done.ticket->id});
                    queued.endObject();
                    matchAnswer(askerName, id, queued.text());
                };
            });
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
            // **Elo, from the ratings as they are now**, written back each --
            // read and written on the store's thread, one job, so no other
            // write of theirs comes between.
            onStore([this, askerName, id, played = *match, ratingName = queue->rating,
                     winner = static_cast<core::u32>(team)](IDataStore& store) -> DataWorker::Then {
                std::map<std::string, core::f64> ratings;
                for (const std::string& one : everyone(played))
                    ratings[one] = ratingIn(store, ratingName, one);
                applyElo(played, winner, ratings);
                for (const auto& [one, value] : ratings)
                    setRatingIn(store, ratingName, one, value);
                return [this, askerName, id]() { matchAnswer(askerName, id, R"({"ok":true})"); };
            });
        }
    }
    else if (op == "getRating") {
        onStore([this, askerName, id, name = std::string(request["rating"].asString()),
                 member](IDataStore& store) -> DataWorker::Then {
            const core::f64 value = ratingIn(store, name, member);
            return [this, askerName, id, value]() {
                core::JsonWriter rated(core::JsonLayout::Compact);
                rated.beginObject();
                rated.field("ok", true);
                rated.field("rating", value);
                rated.endObject();
                matchAnswer(askerName, id, rated.text());
            };
        });
    }
    else if (op == "setRating") {
        onStore([this, askerName, id, name = std::string(request["rating"].asString()), member,
                 value = request["value"].asNumber(StartingRating)](IDataStore& store) -> DataWorker::Then {
            setRatingIn(store, name, member, value);
            return [this, askerName, id]() { matchAnswer(askerName, id, R"({"ok":true})"); };
        });
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
    if (!deferred)
        matchAnswer(askerName, id, answer.text());
    matchNotices();
}

void WorldManager::nakamaMatched(const AccountDoor::MatchFound& result)
{
    // **Once a match**: the first of its players at the door is enough, as
    // Nakama's record of it lists them all.
    if (!m_nakamaMatches.insert(result.match).second)
        return;
    const MatchQueue* queue = m_matches->queue(result.queue);
    if (queue == nullptr) {
        const std::array<core::I18nArg, 2> args{core::I18nArg{"match", std::string_view{result.match}},
                                                core::I18nArg{"queue", std::string_view{result.queue}}};
        core::log(core::LogLevel::Warn, ENG_TR("manager.warn.nakama_no_queue"), args);
        return;
    }
    // Its players dealt into the queue's teams in Nakama's order, and played
    // as a match of the manager's own (ADR 0243).
    Match match;
    match.id = "nakama-" + result.match;
    match.queue = queue->name;
    match.templateName = queue->templateName;
    match.teams.resize(queue->teams);
    for (core::usize at = 0; at < result.accounts.size(); ++at)
        match.teams[at % match.teams.size()].push_back(result.accounts[at]);
    m_matches->adopt(match);
    (void)startMatch(match);
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
