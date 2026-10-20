// **Matchmaking and lobbies** (ADR 0243): queues fill teams from tickets
// that fit, a party never split; the window widens as a ticket waits; a
// running match is backfilled; a lobby by code starts once all are ready;
// Elo moves ratings by its own numbers.
#include <doctest/doctest.h>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine/app/match_book.h"

using engine::app::Match;
using engine::app::MatchBook;
using engine::app::MatchQueue;
namespace core = engine::core;

namespace {

constexpr core::u64 Second = 1'000'000'000ull;

// Counted letters: a test's ids and codes are the same every run.
[[nodiscard]] MatchBook counted()
{
    auto next = std::make_shared<core::u32>(0);
    return MatchBook([next](core::usize count) {
        std::string out(count, 'A');
        core::u32 n = (*next)++;
        for (core::usize at = count; at > 0 && n > 0; --at, n /= 26)
            out[at - 1] = static_cast<char>('A' + n % 26);
        return out;
    });
}

[[nodiscard]] MatchQueue queueOf(std::string name, core::u32 teams, core::u32 teamSize, std::string rating = "Arena")
{
    MatchQueue queue;
    queue.name = std::move(name);
    queue.templateName = "arena";
    queue.teams = teams;
    queue.teamSize = teamSize;
    queue.rating = std::move(rating);
    queue.widenAfterSeconds = 10.0;
    queue.widenStep = 100.0;
    queue.widenMost = 600.0;
    return queue;
}

} // namespace

TEST_CASE("match book > 200 players in mixed parties: every match full, no party split, nobody twice, the last "
          "within the widest window")
{
    MatchBook book = counted();
    book.define(queueOf("Arena2v2", 2, 2));
    book.define(queueOf("Arena4v4", 2, 4));
    // **Ratings from 1,200 to 1,800**, scattered the same way every run.
    core::u32 seed = 12345;
    const auto rating = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return 1200.0 + static_cast<core::f64>((seed >> 8) % 601u);
    };
    std::vector<std::vector<std::string>> tickets2;
    std::vector<std::vector<std::string>> tickets4;
    int player = 0;
    const auto group = [&](std::vector<std::vector<std::string>>& into, int size) {
        std::vector<std::string> members;
        for (int at = 0; at < size; ++at)
            members.push_back("p" + std::to_string(player++));
        into.push_back(members);
    };
    // 2v2: 40 solos and 30 pairs; 4v4: 40 solos, 20 pairs and 5 fours.
    for (int at = 0; at < 40; ++at)
        group(tickets2, 1);
    for (int at = 0; at < 30; ++at)
        group(tickets2, 2);
    for (int at = 0; at < 40; ++at)
        group(tickets4, 1);
    for (int at = 0; at < 20; ++at)
        group(tickets4, 2);
    for (int at = 0; at < 5; ++at)
        group(tickets4, 4);
    REQUIRE(player == 200);
    // All at once, interleaved as players would arrive.
    for (core::usize at = 0; at < std::max(tickets2.size(), tickets4.size()); ++at) {
        if (at < tickets2.size())
            REQUIRE(book.enqueue("Arena2v2", tickets2[at], rating(), "eu", 0).ok);
        if (at < tickets4.size())
            REQUIRE(book.enqueue("Arena4v4", tickets4[at], rating(), "eu", 0).ok);
    }

    std::vector<Match> matches;
    core::u64 lastAt = 0;
    for (core::u64 at = 0; at <= 120; ++at) {
        std::vector<Match> made = book.form(at * Second);
        if (!made.empty())
            lastAt = at;
        matches.insert(matches.end(), made.begin(), made.end());
    }
    // The widest window is reached after 50 seconds: no match waits past it.
    CHECK(lastAt <= 50);

    std::map<std::string, std::string> matchOf;
    std::map<std::string, core::usize> teamOf;
    std::map<std::string, int> counts{{"Arena2v2", 0}, {"Arena4v4", 0}};
    for (const Match& match : matches) {
        const core::u32 size = match.queue == "Arena2v2" ? 2u : 4u;
        REQUIRE(match.teams.size() == 2);
        for (core::usize team = 0; team < match.teams.size(); ++team) {
            CHECK(match.teams[team].size() == size);
            for (const std::string& member : match.teams[team]) {
                // Nobody in two matches.
                CHECK_FALSE(matchOf.contains(member));
                matchOf[member] = match.id;
                teamOf[member] = team;
            }
        }
        counts[match.queue] += 1;
    }
    // **No party split**: a ticket's members share a match and a team.
    for (const auto* list : {&tickets2, &tickets4}) {
        for (const std::vector<std::string>& ticket : *list) {
            if (!matchOf.contains(ticket.front()))
                continue;
            for (const std::string& member : ticket) {
                CHECK(matchOf[member] == matchOf[ticket.front()]);
                CHECK(teamOf[member] == teamOf[ticket.front()]);
            }
        }
    }
    // Everyone who could be matched was: 100 is 25 matches of four, and 12
    // of eight with four players over.
    CHECK(counts["Arena2v2"] == 25);
    CHECK(counts["Arena4v4"] == 12);
    CHECK(book.tickets().size() <= 4);
}

TEST_CASE("match book > the window widens with the wait, a party is one ticket, and one queue at a time")
{
    MatchBook book = counted();
    book.define(queueOf("Duel", 2, 1));
    REQUIRE(book.enqueue("Duel", {"ana"}, 1500.0, "eu", 0).ok);
    REQUIRE(book.enqueue("Duel", {"bo"}, 1750.0, "eu", 0).ok);
    // 250 apart: no match within 100, nor 200...
    CHECK(book.form(0).empty());
    CHECK(book.form(10 * Second).empty());
    // ...and one within 300, after twenty seconds.
    const std::vector<Match> made = book.form(20 * Second);
    REQUIRE(made.size() == 1);
    CHECK(book.matchOf("ana") == made.front().id);

    // In a match: refused; a party bigger than a team: refused.
    CHECK(book.enqueue("Duel", {"ana"}, 1500.0, "eu", 0).reason == "InMatch");
    CHECK(book.enqueue("Duel", {"cy", "dee"}, 1500.0, "eu", 0).reason == "TooMany");
    CHECK(book.enqueue("Nothing", {"cy"}, 1500.0, "eu", 0).reason == "NoQueue");

    // Queueing again cancels the ticket before.
    book.define(queueOf("Other", 2, 1));
    REQUIRE(book.enqueue("Duel", {"cy"}, 1500.0, "eu", 0).ok);
    REQUIRE(book.enqueue("Other", {"cy"}, 1500.0, "eu", 0).ok);
    REQUIRE(book.tickets().size() == 1);
    CHECK(book.tickets().front().queue == "Other");

    // Left, a player may queue again; ended, nobody is in it.
    book.leaveMatch("ana");
    CHECK_FALSE(book.matchOf("ana").has_value());
    book.endMatch(made.front().id);
    CHECK_FALSE(book.matchOf("bo").has_value());
}

TEST_CASE("match book > regions first, then any once the window is widest")
{
    MatchBook book = counted();
    book.define(queueOf("Duel", 2, 1));
    REQUIRE(book.enqueue("Duel", {"ana"}, 1500.0, "eu", 0).ok);
    REQUIRE(book.enqueue("Duel", {"bo"}, 1500.0, "us", 0).ok);
    CHECK(book.form(49 * Second).empty());
    CHECK(book.form(50 * Second).size() == 1);
}

TEST_CASE("match book > a running match short of a player is filled first, by a lone ticket")
{
    MatchBook book = counted();
    book.define(queueOf("Arena2v2", 2, 2, ""));
    REQUIRE(book.enqueue("Arena2v2", {"a1", "a2"}, 0.0, "", 0).ok);
    REQUIRE(book.enqueue("Arena2v2", {"b1", "b2"}, 0.0, "", 0).ok);
    const std::vector<Match> made = book.form(0);
    REQUIRE(made.size() == 1);
    const std::string id = made.front().id;
    book.leaveMatch("a2");
    const core::u32 team = book.match(id)->teams[0].size() == 1 ? 0u : 1u;
    REQUIRE(book.backfill(id, team).ok);
    // A pair waits; the solo behind it takes the place.
    REQUIRE(book.enqueue("Arena2v2", {"c1", "c2"}, 0.0, "", Second).ok);
    REQUIRE(book.enqueue("Arena2v2", {"d1"}, 0.0, "", Second).ok);
    CHECK(book.form(2 * Second).empty());
    const std::vector<engine::app::MatchBackfill> filled = book.takeBackfills();
    REQUIRE(filled.size() == 1);
    CHECK(filled.front().match == id);
    CHECK(filled.front().members == std::vector<std::string>{"d1"});
    CHECK(book.matchOf("d1") == id);
    CHECK(book.match(id)->teams[team].size() == 2);
    CHECK(book.backfill("m-none", 0).reason == "NoMatch");
}

TEST_CASE("match book > a lobby by code: joined, full, started only by its owner once all are ready")
{
    MatchBook book = counted();
    const MatchBook::Answer made = book.createLobby("ana", "arena", 8, true);
    REQUIRE(made.ok);
    const std::string code = made.lobby->code;
    CHECK(code.size() == 6);
    const std::string id = made.lobby->id;
    for (int at = 1; at < 8; ++at)
        REQUIRE(book.joinLobby("p" + std::to_string(at), code).ok);
    CHECK(book.joinLobby("late", code).reason == "Full");
    CHECK(book.joinLobby("late", "ZZZZZZ").reason == "NoLobby");
    CHECK(book.joinLobby("p1", code).reason == "InLobby");
    CHECK(book.listLobbies("arena").size() == 1);
    CHECK(book.listLobbies("other").empty());

    std::string why;
    CHECK_FALSE(book.startLobby(id, "p1", why).has_value());
    CHECK(why == "NotOwner");
    for (int at = 1; at < 8; ++at)
        REQUIRE(book.setReady("p" + std::to_string(at), true).ok);
    CHECK_FALSE(book.startLobby(id, "ana", why).has_value());
    CHECK(why == "NotReady");
    REQUIRE(book.setReady("ana", true).ok);
    const std::optional<Match> started = book.startLobby(id, "ana", why);
    REQUIRE(started.has_value());
    CHECK(started->teams.front().size() == 8);
    CHECK(book.matchOf("p7") == started->id);
    CHECK_FALSE(book.findLobby(id).has_value());
    const std::vector<engine::app::Lobby> gone = book.takeGoneLobbies();
    REQUIRE(gone.size() == 1);
    CHECK(gone.front().id == id);
}

TEST_CASE("match book > a lobby's owner leaving hands it on, and the last one leaving ends it")
{
    MatchBook book = counted();
    const MatchBook::Answer made = book.createLobby("ana", "arena", 4, false);
    REQUIRE(made.ok);
    REQUIRE(book.joinLobby("bo", made.lobby->code).ok);
    CHECK(book.listLobbies("").empty());
    const MatchBook::Answer left = book.leaveLobby("ana");
    REQUIRE(left.ok);
    CHECK(left.lobby->owner == "bo");
    REQUIRE(book.leaveLobby("bo").ok);
    CHECK_FALSE(book.findLobby(made.lobby->id).has_value());
}

TEST_CASE("match book > Elo, K = 32: two wins from 1,500")
{
    Match match;
    match.teams = {{"ana"}, {"bo"}};
    std::map<std::string, core::f64> ratings{{"ana", 1500.0}, {"bo", 1500.0}};
    engine::app::applyElo(match, 0, ratings);
    CHECK(ratings["ana"] == doctest::Approx(1516.0));
    CHECK(ratings["bo"] == doctest::Approx(1484.0));
    engine::app::applyElo(match, 0, ratings);
    // 1516 against 1484: expected 0.5459, so 32 times 0.4541 more.
    CHECK(ratings["ana"] == doctest::Approx(1530.53).epsilon(0.0001));
    CHECK(ratings["bo"] == doctest::Approx(1469.47).epsilon(0.0001));
}
