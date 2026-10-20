// **Parties** (ADR 0240 §1): who plays together, kept where every world can
// be asked -- the manager, or a server with no manager for its own world.
//
// A member is who a world knows a player by: their account, or their key
// where the game has no accounts. A party lives while somebody in it is
// online, and five minutes more; a member who drops and comes back within
// that is still in it.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/types.h"

namespace engine::app {

struct Party
{
    std::string id;
    std::string leader;
    // In the order they joined, the leader among them.
    std::vector<std::string> members;
    // Asked, and not yet answered.
    std::set<std::string, std::less<>> invited;
};

class PartyBook
{
public:
    // How long a party with nobody online is kept.
    static constexpr core::u64 KeptOfflineNs = 5ull * 60ull * 1'000'000'000ull;

    // `[parties] size`: the most members a party has.
    explicit PartyBook(core::u32 size = 8);

    // **What a request did**: the party as it is now, or why not -- `InParty`,
    // `NoParty`, `NotMember`, `NotInvited`, `Full`, `Invalid`.
    struct Answer
    {
        bool ok = false;
        std::string reason;
        std::optional<Party> party;
    };

    [[nodiscard]] Answer create(std::string_view leader, core::u64 nowNs);
    // `by` a member of it; `invitee` in no party.
    [[nodiscard]] Answer invite(std::string_view party, std::string_view by, std::string_view invitee);
    [[nodiscard]] Answer accept(std::string_view party, std::string_view invitee);
    [[nodiscard]] Answer decline(std::string_view party, std::string_view invitee);
    // The leader who leaves hands the party to the member who joined first
    // after them; the last one leaving ends it.
    [[nodiscard]] Answer leave(std::string_view member);
    [[nodiscard]] Answer kick(std::string_view party, std::string_view member);
    [[nodiscard]] Answer setLeader(std::string_view party, std::string_view member);
    [[nodiscard]] std::optional<Party> partyOf(std::string_view member) const;
    [[nodiscard]] std::optional<Party> find(std::string_view party) const;

    // **Who is online**, as the worlds say: everyone else is not. Parties
    // whose last member went offline over `KeptOfflineNs` ago are gone.
    void setOnline(const std::set<std::string, std::less<>>& online, core::u64 nowNs);

    // The parties that changed since the last call, and those that are gone.
    [[nodiscard]] std::vector<std::string> takeChanged();
    [[nodiscard]] std::vector<std::string> takeGone();

private:
    [[nodiscard]] Party* partyNamed(std::string_view id);
    void changed(const std::string& id);
    void end(const std::string& id);

    core::u32 m_size;
    std::map<std::string, Party, std::less<>> m_parties;
    std::map<std::string, std::string, std::less<>> m_memberOf;
    // When each party last had a member online; none while one is.
    std::map<std::string, core::u64, std::less<>> m_offlineSince;
    std::set<std::string, std::less<>> m_online;
    std::vector<std::string> m_changed;
    std::vector<std::string> m_gone;
};

// **A party request as text, and its answer as text** (the pipe's and a lone
// server's), like the data service's: `op` one of `create`, `invite`,
// `accept`, `decline`, `leave`, `kick`, `leader`, `get`, with `party`,
// `member` and `by`; the answer `ok`, `reason` and `party`
// (`{ id, leader, members }`), null when there is none.
[[nodiscard]] std::string runPartyOp(PartyBook& book, std::string_view request, core::u64 nowNs);
// A party as the JSON an answer and a world's notice carry, `{ id, leader,
// members }`, and read back; none for what is not one.
void writeParty(core::JsonWriter& out, const Party& party);
[[nodiscard]] std::optional<Party> readParty(const core::JsonValue& value);

} // namespace engine::app
