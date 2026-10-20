#include "engine/app/party_book.h"

#include <algorithm>
#include <array>
#include <utility>

#include "engine/core/crypto.h"

namespace engine::app {
namespace {

[[nodiscard]] PartyBook::Answer refused(std::string_view reason)
{
    return PartyBook::Answer{false, std::string(reason), std::nullopt};
}

[[nodiscard]] PartyBook::Answer given(const Party& party)
{
    return PartyBook::Answer{true, {}, party};
}

// A party's id: 64 random bits, so one never names another for a script that
// kept it.
[[nodiscard]] std::string newPartyId()
{
    std::array<core::u8, 8> bytes{};
    core::secureRandom(bytes);
    return "p-" + core::toHex(bytes);
}

} // namespace

PartyBook::PartyBook(core::u32 size) : m_size(std::max<core::u32>(size, 2))
{}

Party* PartyBook::partyNamed(std::string_view id)
{
    const auto found = m_parties.find(id);
    return found == m_parties.end() ? nullptr : &found->second;
}

void PartyBook::changed(const std::string& id)
{
    if (std::find(m_changed.begin(), m_changed.end(), id) == m_changed.end())
        m_changed.push_back(id);
}

void PartyBook::end(const std::string& id)
{
    const auto found = m_parties.find(id);
    if (found == m_parties.end())
        return;
    for (const std::string& member : found->second.members)
        m_memberOf.erase(member);
    m_parties.erase(found);
    m_offlineSince.erase(id);
    std::erase(m_changed, id);
    m_gone.push_back(id);
}

PartyBook::Answer PartyBook::create(std::string_view leader, core::u64 nowNs)
{
    (void)nowNs;
    if (leader.empty())
        return refused("Invalid");
    if (m_memberOf.contains(leader))
        return refused("InParty");
    Party party;
    party.id = newPartyId();
    party.leader = std::string(leader);
    party.members.push_back(party.leader);
    m_memberOf.emplace(party.leader, party.id);
    const std::string id = party.id;
    m_parties.emplace(id, std::move(party));
    changed(id);
    return given(m_parties.at(id));
}

PartyBook::Answer PartyBook::invite(std::string_view id, std::string_view by, std::string_view invitee)
{
    Party* party = partyNamed(id);
    if (party == nullptr)
        return refused("NoParty");
    if (invitee.empty() || invitee == by)
        return refused("Invalid");
    if (std::find(party->members.begin(), party->members.end(), by) == party->members.end())
        return refused("NotMember");
    if (m_memberOf.contains(invitee))
        return refused("InParty");
    if (party->members.size() >= m_size)
        return refused("Full");
    party->invited.insert(std::string(invitee));
    return given(*party);
}

PartyBook::Answer PartyBook::accept(std::string_view id, std::string_view invitee)
{
    Party* party = partyNamed(id);
    if (party == nullptr)
        return refused("NoParty");
    const auto asked = party->invited.find(invitee);
    if (asked == party->invited.end())
        return refused("NotInvited");
    if (m_memberOf.contains(invitee))
        return refused("InParty");
    if (party->members.size() >= m_size)
        return refused("Full");
    party->invited.erase(asked);
    party->members.emplace_back(invitee);
    m_memberOf.emplace(std::string(invitee), party->id);
    changed(party->id);
    return given(*party);
}

PartyBook::Answer PartyBook::decline(std::string_view id, std::string_view invitee)
{
    Party* party = partyNamed(id);
    if (party == nullptr)
        return refused("NoParty");
    if (party->invited.erase(std::string(invitee)) == 0)
        return refused("NotInvited");
    return given(*party);
}

PartyBook::Answer PartyBook::leave(std::string_view member)
{
    const auto of = m_memberOf.find(member);
    if (of == m_memberOf.end())
        return refused("NoParty");
    Party& party = m_parties.at(of->second);
    const std::string id = party.id;
    std::erase(party.members, std::string(member));
    m_memberOf.erase(of);
    if (party.members.empty()) {
        end(id);
        return PartyBook::Answer{true, {}, std::nullopt};
    }
    if (party.leader == member)
        party.leader = party.members.front();
    changed(id);
    return given(party);
}

PartyBook::Answer PartyBook::kick(std::string_view id, std::string_view member)
{
    Party* party = partyNamed(id);
    if (party == nullptr)
        return refused("NoParty");
    if (std::find(party->members.begin(), party->members.end(), member) == party->members.end())
        return refused("NotMember");
    return leave(member);
}

PartyBook::Answer PartyBook::setLeader(std::string_view id, std::string_view member)
{
    Party* party = partyNamed(id);
    if (party == nullptr)
        return refused("NoParty");
    if (std::find(party->members.begin(), party->members.end(), member) == party->members.end())
        return refused("NotMember");
    party->leader = std::string(member);
    changed(party->id);
    return given(*party);
}

std::optional<Party> PartyBook::partyOf(std::string_view member) const
{
    const auto of = m_memberOf.find(member);
    if (of == m_memberOf.end())
        return std::nullopt;
    return m_parties.at(of->second);
}

std::optional<Party> PartyBook::find(std::string_view id) const
{
    const auto found = m_parties.find(id);
    if (found == m_parties.end())
        return std::nullopt;
    return found->second;
}

void PartyBook::setOnline(const std::set<std::string, std::less<>>& online, core::u64 nowNs)
{
    m_online = online;
    std::vector<std::string> ended;
    for (const auto& [id, party] : m_parties) {
        const bool anyone = std::any_of(party.members.begin(), party.members.end(),
                                        [&](const std::string& member) { return m_online.contains(member); });
        if (anyone) {
            m_offlineSince.erase(id);
            continue;
        }
        const auto [since, made] = m_offlineSince.try_emplace(id, nowNs);
        if (!made && nowNs >= since->second + KeptOfflineNs)
            ended.push_back(id);
    }
    for (const std::string& id : ended)
        end(id);
}

std::vector<std::string> PartyBook::takeChanged()
{
    std::vector<std::string> out;
    out.swap(m_changed);
    return out;
}

std::vector<std::string> PartyBook::takeGone()
{
    std::vector<std::string> out;
    out.swap(m_gone);
    return out;
}

void writeParty(core::JsonWriter& out, const Party& party)
{
    out.beginObject();
    out.field("id", std::string_view{party.id});
    out.field("leader", std::string_view{party.leader});
    out.key("members");
    out.beginArray();
    for (const std::string& member : party.members)
        out.value(std::string_view{member});
    out.endArray();
    out.endObject();
}

std::optional<Party> readParty(const core::JsonValue& value)
{
    if (value.type() != core::JsonType::Object || value["id"].asString().empty())
        return std::nullopt;
    Party party;
    party.id = std::string(value["id"].asString());
    party.leader = std::string(value["leader"].asString());
    const core::JsonValue members = value["members"];
    for (core::usize at = 0; at < members.size(); ++at)
        party.members.emplace_back(members.at(at).asString());
    return party;
}

std::string runPartyOp(PartyBook& book, std::string_view request, core::u64 nowNs)
{
    core::JsonDocument document;
    PartyBook::Answer answer{false, "Invalid", std::nullopt};
    if (document.parse(request, "party-op")) {
        const core::JsonValue root = document.root();
        const std::string_view op = root["op"].asString();
        const std::string_view party = root["party"].asString();
        const std::string_view member = root["member"].asString();
        const std::string_view by = root["by"].asString();
        if (op == "create")
            answer = book.create(member, nowNs);
        else if (op == "invite")
            answer = book.invite(party, by, member);
        else if (op == "accept")
            answer = book.accept(party, member);
        else if (op == "decline")
            answer = book.decline(party, member);
        else if (op == "leave")
            answer = book.leave(member);
        else if (op == "kick")
            answer = book.kick(party, member);
        else if (op == "leader")
            answer = book.setLeader(party, member);
        else if (op == "get") {
            answer.ok = true;
            answer.reason.clear();
            answer.party = party.empty() ? book.partyOf(member) : book.find(party);
        }
    }
    core::JsonWriter out(core::JsonLayout::Compact);
    out.beginObject();
    out.field("ok", answer.ok);
    out.field("reason", std::string_view{answer.reason});
    out.key("party");
    if (answer.party.has_value())
        writeParty(out, *answer.party);
    else
        out.nullValue();
    out.endObject();
    return out.text();
}

} // namespace engine::app
