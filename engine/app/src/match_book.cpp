#include "engine/app/match_book.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace engine::app {
namespace {

[[nodiscard]] MatchBook::Answer refused(std::string_view reason)
{
    MatchBook::Answer answer;
    answer.reason = std::string(reason);
    return answer;
}

// **How far apart two ratings may be**, for a ticket that has waited
// `waitedNs`: one step, and another every `widenAfterSeconds`, up to the
// most. A queue with no rating fits anything.
[[nodiscard]] core::f64 windowOf(const MatchQueue& queue, core::u64 waitedNs)
{
    if (queue.rating.empty())
        return std::numeric_limits<core::f64>::infinity();
    const core::f64 waited = static_cast<core::f64>(waitedNs) / 1e9;
    const core::f64 steps = queue.widenAfterSeconds > 0.0 ? std::floor(waited / queue.widenAfterSeconds) : 0.0;
    return std::min(queue.widenMost, queue.widenStep * (1.0 + steps));
}

[[nodiscard]] bool widest(const MatchQueue& queue, core::u64 waitedNs)
{
    return queue.rating.empty() || windowOf(queue, waitedNs) >= queue.widenMost;
}

// **Teams filled from tickets**, each whole on one team: the first ticket
// always in, the rest tried in order. A search with a bound -- a queue of
// thousands is still answered in a frame, the oldest tickets first.
class TeamFiller
{
public:
    TeamFiller(const std::vector<const MatchTicket*>& tickets, core::u32 teams, core::u32 teamSize)
        : m_tickets(tickets), m_teamSize(teamSize), m_fill(teams, 0), m_placed(tickets.size(), -1)
    {
        m_needed = teams * teamSize;
        m_left.assign(tickets.size() + 1, 0);
        for (core::usize at = tickets.size(); at > 0; --at)
            m_left[at - 1] = m_left[at] + static_cast<core::u32>(tickets[at - 1]->members.size());
    }

    [[nodiscard]] bool fill() { return place(0, 0); }

    // Which team each ticket went on, or -1 for one left out.
    [[nodiscard]] const std::vector<int>& placed() const noexcept { return m_placed; }

private:
    static constexpr core::u32 Budget = 20'000;

    [[nodiscard]] bool place(core::usize at, core::u32 seated)
    {
        if (seated == m_needed)
            return true;
        if (at == m_tickets.size() || seated + m_left[at] < m_needed || ++m_tried > Budget)
            return false;
        const auto size = static_cast<core::u32>(m_tickets[at]->members.size());
        for (core::usize team = 0; team < m_fill.size(); ++team) {
            // Two teams as full as each other are the same choice.
            bool same = false;
            for (core::usize before = 0; before < team && !same; ++before)
                same = m_fill[before] == m_fill[team];
            if (same || m_fill[team] + size > m_teamSize)
                continue;
            m_fill[team] += size;
            m_placed[at] = static_cast<int>(team);
            if (place(at + 1, seated + size))
                return true;
            m_fill[team] -= size;
            m_placed[at] = -1;
        }
        // The first is the one the match is for: never left out.
        return at != 0 && place(at + 1, seated);
    }

    const std::vector<const MatchTicket*>& m_tickets;
    core::u32 m_teamSize;
    core::u32 m_needed = 0;
    std::vector<core::u32> m_fill;
    std::vector<int> m_placed;
    std::vector<core::u32> m_left;
    core::u32 m_tried = 0;
};

void writeStrings(core::JsonWriter& out, const std::vector<std::string>& list)
{
    out.beginArray();
    for (const std::string& one : list)
        out.value(std::string_view{one});
    out.endArray();
}

[[nodiscard]] std::vector<std::string> readStrings(const core::JsonValue& value)
{
    std::vector<std::string> out;
    for (core::usize at = 0; at < value.size(); ++at)
        out.emplace_back(value.at(at).asString());
    return out;
}

} // namespace

MatchBook::MatchBook(Letters letters) : m_letters(std::move(letters))
{}

std::string MatchBook::nextId(std::string_view prefix)
{
    return std::string(prefix) + std::to_string(m_next++) + "-" + m_letters(8);
}

void MatchBook::define(MatchQueue queue)
{
    queue.teams = std::max<core::u32>(queue.teams, 1);
    queue.teamSize = std::max<core::u32>(queue.teamSize, 1);
    const std::string name = queue.name;
    m_queues.insert_or_assign(name, std::move(queue));
}

const MatchQueue* MatchBook::queue(std::string_view name) const
{
    const auto found = m_queues.find(name);
    return found == m_queues.end() ? nullptr : &found->second;
}

bool MatchBook::busy(std::string_view member) const
{
    return m_inMatch.contains(member) || m_inLobby.contains(member);
}

void MatchBook::dropTicketOf(std::string_view member)
{
    std::erase_if(m_waiting, [&](const Waiting& waiting) {
        return !waiting.place.has_value() && std::find(waiting.ticket.members.begin(), waiting.ticket.members.end(),
                                                       member) != waiting.ticket.members.end();
    });
}

MatchBook::Answer MatchBook::enqueue(std::string_view queueName, std::vector<std::string> members, core::f64 rating,
                                     std::string_view region, core::u64 nowNs)
{
    const MatchQueue* rules = queue(queueName);
    if (rules == nullptr)
        return refused("NoQueue");
    if (members.empty())
        return refused("Invalid");
    if (members.size() > rules->teamSize)
        return refused("TooMany");
    for (const std::string& member : members) {
        if (m_inMatch.contains(member))
            return refused("InMatch");
        if (m_inLobby.contains(member))
            return refused("InLobby");
    }
    // **One queue at a time**: queueing again cancels the ticket before.
    for (const std::string& member : members)
        dropTicketOf(member);
    Waiting waiting;
    waiting.ticket.id = nextId("t-");
    waiting.ticket.queue = std::string(queueName);
    waiting.ticket.members = std::move(members);
    waiting.ticket.rating = rating;
    waiting.ticket.region = std::string(region);
    waiting.ticket.sinceNs = nowNs;
    Answer answer;
    answer.ok = true;
    answer.ticket = waiting.ticket;
    m_waiting.push_back(std::move(waiting));
    return answer;
}

MatchBook::Answer MatchBook::cancel(std::string_view ticket)
{
    const auto found = std::find_if(m_waiting.begin(), m_waiting.end(),
                                    [&](const Waiting& waiting) { return waiting.ticket.id == ticket; });
    if (found == m_waiting.end())
        return refused("NoTicket");
    Answer answer;
    answer.ok = true;
    answer.ticket = found->ticket;
    m_waiting.erase(found);
    return answer;
}

std::vector<MatchTicket> MatchBook::tickets() const
{
    std::vector<MatchTicket> out;
    for (const Waiting& waiting : m_waiting)
        out.push_back(waiting.ticket);
    return out;
}

std::vector<Match> MatchBook::form(core::u64 nowNs)
{
    std::vector<Match> made;
    // **Places first**: a running match short of a player is filled by the
    // first lone ticket that fits it.
    for (auto place = m_waiting.begin(); place != m_waiting.end();) {
        if (!place->place.has_value()) {
            ++place;
            continue;
        }
        const MatchQueue* rules = queue(place->ticket.queue);
        const auto match = m_matches.find(place->place->match);
        if (rules == nullptr || match == m_matches.end()) {
            place = m_waiting.erase(place);
            continue;
        }
        const core::f64 window = windowOf(*rules, nowNs - std::min(nowNs, place->ticket.sinceNs));
        const auto filler = std::find_if(m_waiting.begin(), m_waiting.end(), [&](const Waiting& waiting) {
            return !waiting.place.has_value() && waiting.ticket.queue == place->ticket.queue &&
                   waiting.ticket.members.size() == 1 &&
                   std::fabs(waiting.ticket.rating - match->second.rating) <= window;
        });
        if (filler == m_waiting.end()) {
            ++place;
            continue;
        }
        MatchBackfill filled = *place->place;
        filled.members = filler->ticket.members;
        for (const std::string& member : filled.members) {
            match->second.teams[filled.team].push_back(member);
            m_inMatch.insert_or_assign(member, match->first);
        }
        m_backfilled.push_back(std::move(filled));
        const std::string fillerId = filler->ticket.id;
        const std::string placeId = place->ticket.id;
        std::erase_if(m_waiting, [&](const Waiting& waiting) {
            return waiting.ticket.id == fillerId || waiting.ticket.id == placeId;
        });
        place = m_waiting.begin();
    }

    // **Then matches**, each for the oldest ticket that can be given one.
    for (const auto& [name, rules] : m_queues) {
        bool formed = true;
        while (formed) {
            formed = false;
            std::vector<const Waiting*> line;
            for (const Waiting& waiting : m_waiting) {
                if (!waiting.place.has_value() && waiting.ticket.queue == name)
                    line.push_back(&waiting);
            }
            for (core::usize first = 0; first < line.size() && !formed; ++first) {
                const MatchTicket& anchor = line[first]->ticket;
                const core::u64 waited = nowNs - std::min(nowNs, anchor.sinceNs);
                const core::f64 window = windowOf(rules, waited);
                const bool anyRegion = !rules.regions || widest(rules, waited);
                std::vector<const MatchTicket*> fits{&anchor};
                for (core::usize other = 0; other < line.size(); ++other) {
                    const MatchTicket& ticket = line[other]->ticket;
                    if (other == first || std::fabs(ticket.rating - anchor.rating) > window)
                        continue;
                    if (!anyRegion && ticket.region != anchor.region)
                        continue;
                    fits.push_back(&ticket);
                }
                TeamFiller filler(fits, rules.teams, rules.teamSize);
                if (!filler.fill())
                    continue;
                Match match;
                match.id = nextId("m-");
                match.queue = name;
                match.templateName = rules.templateName;
                match.teams.resize(rules.teams);
                core::f64 total = 0.0;
                core::usize players = 0;
                std::set<std::string> used;
                for (core::usize at = 0; at < fits.size(); ++at) {
                    const int team = filler.placed()[at];
                    if (team < 0)
                        continue;
                    used.insert(fits[at]->id);
                    for (const std::string& member : fits[at]->members) {
                        match.teams[static_cast<core::usize>(team)].push_back(member);
                        m_inMatch.insert_or_assign(member, match.id);
                        total += fits[at]->rating;
                        ++players;
                    }
                }
                match.rating = players > 0 ? total / static_cast<core::f64>(players) : 0.0;
                std::erase_if(m_waiting, [&](const Waiting& waiting) { return used.contains(waiting.ticket.id); });
                m_matches.emplace(match.id, match);
                made.push_back(std::move(match));
                formed = true;
            }
        }
    }
    return made;
}

std::vector<MatchBackfill> MatchBook::takeBackfills()
{
    std::vector<MatchBackfill> out;
    out.swap(m_backfilled);
    return out;
}

MatchBook::Answer MatchBook::backfill(std::string_view matchId, core::u32 team)
{
    const auto match = m_matches.find(matchId);
    if (match == m_matches.end())
        return refused("NoMatch");
    if (team >= match->second.teams.size())
        return refused("Invalid");
    Waiting waiting;
    waiting.ticket.id = nextId("t-");
    waiting.ticket.queue = match->second.queue;
    waiting.ticket.rating = match->second.rating;
    waiting.place = MatchBackfill{match->first, team, {}};
    // **First in line**: the place goes before every ticket.
    Answer answer;
    answer.ok = true;
    answer.ticket = waiting.ticket;
    m_waiting.insert(m_waiting.begin(), std::move(waiting));
    return answer;
}

const Match* MatchBook::match(std::string_view id) const
{
    const auto found = m_matches.find(id);
    return found == m_matches.end() ? nullptr : &found->second;
}

std::optional<std::string> MatchBook::matchOf(std::string_view member) const
{
    const auto found = m_inMatch.find(member);
    if (found == m_inMatch.end())
        return std::nullopt;
    return found->second;
}

void MatchBook::leaveMatch(std::string_view member)
{
    const auto found = m_inMatch.find(member);
    if (found == m_inMatch.end())
        return;
    if (const auto match = m_matches.find(found->second); match != m_matches.end()) {
        for (std::vector<std::string>& team : match->second.teams)
            std::erase(team, std::string(member));
    }
    m_inMatch.erase(found);
}

void MatchBook::endMatch(std::string_view matchId)
{
    const auto match = m_matches.find(matchId);
    if (match == m_matches.end())
        return;
    for (const std::vector<std::string>& team : match->second.teams) {
        for (const std::string& member : team)
            m_inMatch.erase(member);
    }
    std::erase_if(m_waiting,
                  [&](const Waiting& waiting) { return waiting.place.has_value() && waiting.place->match == matchId; });
    m_matches.erase(match);
}

std::vector<std::string> MatchBook::matches() const
{
    std::vector<std::string> out;
    for (const auto& [id, match] : m_matches)
        out.push_back(id);
    return out;
}

Lobby* MatchBook::lobbyNamed(std::string_view id)
{
    const auto found = m_lobbies.find(id);
    return found == m_lobbies.end() ? nullptr : &found->second;
}

void MatchBook::lobbyChanged(const std::string& id)
{
    if (std::find(m_changedLobbies.begin(), m_changedLobbies.end(), id) == m_changedLobbies.end())
        m_changedLobbies.push_back(id);
}

void MatchBook::endLobby(const std::string& id)
{
    const auto found = m_lobbies.find(id);
    if (found == m_lobbies.end())
        return;
    for (const std::string& member : found->second.members)
        m_inLobby.erase(member);
    m_goneLobbies.push_back(std::move(found->second));
    m_lobbies.erase(found);
    std::erase(m_changedLobbies, id);
}

MatchBook::Answer MatchBook::createLobby(std::string_view owner, std::string_view templateName, core::u32 size,
                                         bool isPublic)
{
    if (owner.empty() || templateName.empty() || size == 0)
        return refused("Invalid");
    if (m_inMatch.contains(owner))
        return refused("InMatch");
    if (m_inLobby.contains(owner))
        return refused("InLobby");
    dropTicketOf(owner);
    Lobby lobby;
    lobby.id = nextId("l-");
    // **A code nobody else's lobby has**, in letters a player reads aloud.
    do {
        lobby.code = m_letters(6);
    } while (std::any_of(m_lobbies.begin(), m_lobbies.end(),
                         [&](const auto& entry) { return entry.second.code == lobby.code; }));
    lobby.owner = std::string(owner);
    lobby.templateName = std::string(templateName);
    lobby.size = size;
    lobby.isPublic = isPublic;
    lobby.members.push_back(lobby.owner);
    m_inLobby.emplace(lobby.owner, lobby.id);
    const std::string id = lobby.id;
    m_lobbies.emplace(id, std::move(lobby));
    lobbyChanged(id);
    Answer answer;
    answer.ok = true;
    answer.lobby = m_lobbies.at(id);
    return answer;
}

MatchBook::Answer MatchBook::joinLobby(std::string_view member, std::string_view code)
{
    std::string upper(code);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
    const auto found =
        std::find_if(m_lobbies.begin(), m_lobbies.end(), [&](const auto& entry) { return entry.second.code == upper; });
    if (found == m_lobbies.end())
        return refused("NoLobby");
    Lobby& lobby = found->second;
    if (m_inMatch.contains(member))
        return refused("InMatch");
    if (m_inLobby.contains(member))
        return refused("InLobby");
    if (lobby.members.size() >= lobby.size)
        return refused("Full");
    dropTicketOf(member);
    lobby.members.emplace_back(member);
    m_inLobby.emplace(std::string(member), lobby.id);
    lobbyChanged(lobby.id);
    Answer answer;
    answer.ok = true;
    answer.lobby = lobby;
    return answer;
}

MatchBook::Answer MatchBook::leaveLobby(std::string_view member)
{
    const auto in = m_inLobby.find(member);
    if (in == m_inLobby.end())
        return refused("NotMember");
    const std::string id = in->second;
    Lobby& lobby = m_lobbies.at(id);
    std::erase(lobby.members, std::string(member));
    lobby.ready.erase(std::string(member));
    m_inLobby.erase(in);
    Answer answer;
    answer.ok = true;
    // **An empty lobby is gone**; one its owner left is the next member's.
    if (lobby.members.empty()) {
        endLobby(id);
        return answer;
    }
    if (lobby.owner == member)
        lobby.owner = lobby.members.front();
    lobbyChanged(id);
    answer.lobby = lobby;
    return answer;
}

MatchBook::Answer MatchBook::setReady(std::string_view member, bool ready)
{
    const auto in = m_inLobby.find(member);
    if (in == m_inLobby.end())
        return refused("NotMember");
    Lobby& lobby = m_lobbies.at(in->second);
    if (ready)
        lobby.ready.emplace(member);
    else
        lobby.ready.erase(std::string(member));
    lobbyChanged(lobby.id);
    Answer answer;
    answer.ok = true;
    answer.lobby = lobby;
    return answer;
}

std::vector<Lobby> MatchBook::listLobbies(std::string_view templateName) const
{
    std::vector<Lobby> out;
    for (const auto& [id, lobby] : m_lobbies) {
        if (lobby.isPublic && (templateName.empty() || lobby.templateName == templateName))
            out.push_back(lobby);
    }
    return out;
}

std::optional<Lobby> MatchBook::lobbyOf(std::string_view member) const
{
    const auto in = m_inLobby.find(member);
    if (in == m_inLobby.end())
        return std::nullopt;
    return m_lobbies.at(in->second);
}

std::optional<Lobby> MatchBook::findLobby(std::string_view id) const
{
    const auto found = m_lobbies.find(id);
    if (found == m_lobbies.end())
        return std::nullopt;
    return found->second;
}

std::optional<Match> MatchBook::startLobby(std::string_view id, std::string_view by, std::string& reason)
{
    Lobby* lobby = lobbyNamed(id);
    if (lobby == nullptr) {
        reason = "NoLobby";
        return std::nullopt;
    }
    if (lobby->owner != by) {
        reason = "NotOwner";
        return std::nullopt;
    }
    if (lobby->ready.size() != lobby->members.size()) {
        reason = "NotReady";
        return std::nullopt;
    }
    // **Started, it is a match of one team**, and the lobby is gone.
    Match match;
    match.id = nextId("m-");
    match.templateName = lobby->templateName;
    match.teams.push_back(lobby->members);
    const std::string lobbyId = lobby->id;
    endLobby(lobbyId);
    for (const std::string& member : match.teams.front())
        m_inMatch.insert_or_assign(member, match.id);
    m_matches.emplace(match.id, match);
    return match;
}

std::vector<std::string> MatchBook::takeChangedLobbies()
{
    std::vector<std::string> out;
    out.swap(m_changedLobbies);
    return out;
}

std::vector<Lobby> MatchBook::takeGoneLobbies()
{
    std::vector<Lobby> out;
    out.swap(m_goneLobbies);
    return out;
}

void writeMatch(core::JsonWriter& out, const Match& match)
{
    out.beginObject();
    out.field("id", std::string_view{match.id});
    out.field("queue", std::string_view{match.queue});
    out.field("template", std::string_view{match.templateName});
    out.field("rating", match.rating);
    out.key("teams");
    out.beginArray();
    for (const std::vector<std::string>& team : match.teams)
        writeStrings(out, team);
    out.endArray();
    out.endObject();
}

std::optional<Match> readMatch(const core::JsonValue& value)
{
    if (value.type() != core::JsonType::Object || value["id"].asString().empty())
        return std::nullopt;
    Match match;
    match.id = std::string(value["id"].asString());
    match.queue = std::string(value["queue"].asString());
    match.templateName = std::string(value["template"].asString());
    match.rating = value["rating"].asNumber();
    const core::JsonValue teams = value["teams"];
    for (core::usize at = 0; at < teams.size(); ++at)
        match.teams.push_back(readStrings(teams.at(at)));
    return match;
}

void writeLobby(core::JsonWriter& out, const Lobby& lobby)
{
    out.beginObject();
    out.field("id", std::string_view{lobby.id});
    out.field("code", std::string_view{lobby.code});
    out.field("owner", std::string_view{lobby.owner});
    out.field("template", std::string_view{lobby.templateName});
    out.field("size", static_cast<core::i64>(lobby.size));
    out.field("public", lobby.isPublic);
    out.key("members");
    writeStrings(out, lobby.members);
    out.key("ready");
    writeStrings(out, std::vector<std::string>(lobby.ready.begin(), lobby.ready.end()));
    out.endObject();
}

std::optional<Lobby> readLobby(const core::JsonValue& value)
{
    if (value.type() != core::JsonType::Object || value["id"].asString().empty())
        return std::nullopt;
    Lobby lobby;
    lobby.id = std::string(value["id"].asString());
    lobby.code = std::string(value["code"].asString());
    lobby.owner = std::string(value["owner"].asString());
    lobby.templateName = std::string(value["template"].asString());
    lobby.size = static_cast<core::u32>(value["size"].asInteger(8));
    lobby.isPublic = value["public"].asBool(false);
    lobby.members = readStrings(value["members"]);
    for (const std::string& member : readStrings(value["ready"]))
        lobby.ready.insert(member);
    return lobby;
}

void applyElo(const Match& match, core::u32 winningTeam, std::map<std::string, core::f64>& ratings)
{
    constexpr core::f64 K = 32.0;
    std::vector<core::f64> means;
    for (const std::vector<std::string>& team : match.teams) {
        core::f64 total = 0.0;
        for (const std::string& member : team)
            total += ratings[member];
        means.push_back(team.empty() ? 0.0 : total / static_cast<core::f64>(team.size()));
    }
    // **Each team against the others' mean**, all from the ratings before
    // the match.
    std::map<std::string, core::f64> after = ratings;
    for (core::usize team = 0; team < match.teams.size(); ++team) {
        core::f64 others = 0.0;
        core::usize counted = 0;
        for (core::usize other = 0; other < match.teams.size(); ++other) {
            if (other != team && !match.teams[other].empty()) {
                others += means[other];
                ++counted;
            }
        }
        if (counted == 0)
            continue;
        const core::f64 opponent = others / static_cast<core::f64>(counted);
        const core::f64 expected = 1.0 / (1.0 + std::pow(10.0, (opponent - means[team]) / 400.0));
        const core::f64 score = team == winningTeam ? 1.0 : 0.0;
        for (const std::string& member : match.teams[team])
            after[member] = ratings[member] + K * (score - expected);
    }
    ratings = std::move(after);
}

} // namespace engine::app
