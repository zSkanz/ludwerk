// **Matchmaking and lobbies** (ADR 0243): the queues, the tickets in them,
// the matches they make and the lobbies players gather in, kept by the
// manager so a player in any world is matched with players in any other.
//
// This is the book alone -- who waits, who fits, who is in what -- with no
// world, no instance and no database: the manager makes the instance a match
// is played in, sends its players there and keeps their ratings.
#pragma once

#include <functional>
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

// What `DefineQueue` says of a queue.
struct MatchQueue
{
    std::string name;
    // The instance template a match is played in (ADR 0221).
    std::string templateName;
    core::u32 teams = 2;
    core::u32 teamSize = 1;
    // The rating kept per member under this name; empty for none, and then
    // any ticket fits any other.
    std::string rating;
    // **The window**: tickets within `widenStep` rating points of the one
    // that has waited longest fit, and it widens by `widenStep` every
    // `widenAfterSeconds` it waits, up to `widenMost`.
    core::f64 widenAfterSeconds = 10.0;
    core::f64 widenStep = 100.0;
    core::f64 widenMost = 600.0;
    // Matched within their region; past the widest window, with any.
    bool regions = true;
};

// A ticket: one player, or a party's members online, who play together.
struct MatchTicket
{
    std::string id;
    std::string queue;
    std::vector<std::string> members;
    // The members' ratings' mean.
    core::f64 rating = 0.0;
    std::string region;
    core::u64 sinceNs = 0;
};

struct Match
{
    std::string id;
    std::string queue;
    std::string templateName;
    // Each team's members; a ticket's are always on one team.
    std::vector<std::vector<std::string>> teams;
    // Its players' ratings' mean, for a place it asks filled.
    core::f64 rating = 0.0;
};

// A place a running match asks to be filled, and who fills it.
struct MatchBackfill
{
    std::string match;
    core::u32 team = 0;
    std::vector<std::string> members;
};

struct Lobby
{
    std::string id;
    // Six letters, for a player to give their friends.
    std::string code;
    std::string owner;
    std::string templateName;
    core::u32 size = 8;
    bool isPublic = false;
    // In the order they joined, its owner first.
    std::vector<std::string> members;
    std::set<std::string, std::less<>> ready;
};

class MatchBook
{
public:
    // Where the book's ids and lobby codes come from: random in the
    // manager, counted in a test.
    using Letters = std::function<std::string(core::usize count)>;
    explicit MatchBook(Letters letters);

    // **What a request did**, or why not: `NoQueue`, `InMatch`, `TooMany`,
    // `NoTicket`, `NoMatch`, `NoLobby`, `InLobby`, `Full`, `NotOwner`,
    // `NotReady`, `NotMember`, `Invalid`.
    struct Answer
    {
        bool ok = false;
        std::string reason;
        std::optional<MatchTicket> ticket;
        std::optional<Lobby> lobby;
    };

    // A queue said again replaces the one before: every world of a game
    // says the same.
    void define(MatchQueue queue);
    [[nodiscard]] const MatchQueue* queue(std::string_view name) const;

    // **Queueing**: every member of a party at once, on one ticket. A member
    // already queued leaves that ticket -- their party with them -- and one
    // in a match or a lobby is refused.
    [[nodiscard]] Answer enqueue(std::string_view queue, std::vector<std::string> members, core::f64 rating,
                                 std::string_view region, core::u64 nowNs);
    [[nodiscard]] Answer cancel(std::string_view ticket);
    [[nodiscard]] std::vector<MatchTicket> tickets() const;

    // **The matches that can be made now**, oldest ticket first, and the
    // places running matches asked filled. Their members are in them until
    // `leaveMatch` or `endMatch`.
    [[nodiscard]] std::vector<Match> form(core::u64 nowNs);
    [[nodiscard]] std::vector<MatchBackfill> takeBackfills();

    // A running match asks for one player for `team`, first in line.
    [[nodiscard]] Answer backfill(std::string_view match, core::u32 team);
    [[nodiscard]] const Match* match(std::string_view id) const;
    [[nodiscard]] std::optional<std::string> matchOf(std::string_view member) const;
    void leaveMatch(std::string_view member);
    void endMatch(std::string_view match);
    // The matches being played, by id.
    [[nodiscard]] std::vector<std::string> matches() const;

    // **Lobbies**: made, joined by code, readied; started by its owner once
    // everyone is ready, which makes them a match of one team.
    [[nodiscard]] Answer createLobby(std::string_view owner, std::string_view templateName, core::u32 size,
                                     bool isPublic);
    [[nodiscard]] Answer joinLobby(std::string_view member, std::string_view code);
    [[nodiscard]] Answer leaveLobby(std::string_view member);
    [[nodiscard]] Answer setReady(std::string_view member, bool ready);
    [[nodiscard]] std::vector<Lobby> listLobbies(std::string_view templateName) const;
    [[nodiscard]] std::optional<Lobby> lobbyOf(std::string_view member) const;
    [[nodiscard]] std::optional<Lobby> findLobby(std::string_view id) const;
    [[nodiscard]] std::optional<Match> startLobby(std::string_view lobby, std::string_view by, std::string& reason);

    // The lobbies that changed since the last call, and those that are gone
    // -- emptied, or started -- as they were last.
    [[nodiscard]] std::vector<std::string> takeChangedLobbies();
    [[nodiscard]] std::vector<Lobby> takeGoneLobbies();

private:
    struct Waiting
    {
        MatchTicket ticket;
        // A place a running match asked filled, first in line.
        std::optional<MatchBackfill> place;
    };

    [[nodiscard]] std::string nextId(std::string_view prefix);
    void dropTicketOf(std::string_view member);
    [[nodiscard]] bool busy(std::string_view member) const;
    void lobbyChanged(const std::string& id);
    void endLobby(const std::string& id);
    [[nodiscard]] Lobby* lobbyNamed(std::string_view id);

    Letters m_letters;
    core::u64 m_next = 1;
    std::map<std::string, MatchQueue, std::less<>> m_queues;
    // In the order they were made, so the oldest is matched first.
    std::vector<Waiting> m_waiting;
    std::map<std::string, Match, std::less<>> m_matches;
    std::map<std::string, std::string, std::less<>> m_inMatch;
    std::vector<MatchBackfill> m_backfilled;
    std::map<std::string, Lobby, std::less<>> m_lobbies;
    std::map<std::string, std::string, std::less<>> m_inLobby;
    std::vector<std::string> m_changedLobbies;
    std::vector<Lobby> m_goneLobbies;
};

// A match and a lobby as the JSON the pipe carries, and read back.
void writeMatch(core::JsonWriter& out, const Match& match);
[[nodiscard]] std::optional<Match> readMatch(const core::JsonValue& value);
void writeLobby(core::JsonWriter& out, const Lobby& lobby);
[[nodiscard]] std::optional<Lobby> readLobby(const core::JsonValue& value);

// **Elo** (K = 32) for one match: each team's members against the other
// teams' mean, the winners scoring 1. `ratings` holds each member's, and is
// changed in place.
void applyElo(const Match& match, core::u32 winningTeam, std::map<std::string, core::f64>& ratings);

} // namespace engine::app
