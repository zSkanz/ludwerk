// **Parties** (ADR 0240 §1): made, joined by invitation, led, left; one party
// a player; at most eight; kept five minutes after its last member goes
// offline, and a member who comes back within them still in it.
#include <doctest/doctest.h>
#include <set>
#include <string>

#include "engine/app/party_book.h"

using engine::app::PartyBook;

namespace {

constexpr engine::core::u64 Minute = 60ull * 1'000'000'000ull;

[[nodiscard]] std::set<std::string, std::less<>> online(std::initializer_list<const char*> who)
{
    std::set<std::string, std::less<>> out;
    for (const char* one : who)
        out.insert(one);
    return out;
}

} // namespace

TEST_CASE("party book > a party is made, joined by invitation, and led")
{
    PartyBook book;
    const PartyBook::Answer made = book.create("ana", 0);
    REQUIRE(made.ok);
    const std::string id = made.party->id;
    CHECK(id.starts_with("p-"));
    CHECK(made.party->leader == "ana");
    CHECK(book.takeChanged() == std::vector<std::string>{id});

    // Only who is in it invites, and only who was invited joins.
    CHECK(book.invite(id, "cy", "bo").reason == "NotMember");
    CHECK(book.accept(id, "bo").reason == "NotInvited");
    REQUIRE(book.invite(id, "ana", "bo").ok);
    REQUIRE(book.accept(id, "bo").ok);
    CHECK(book.partyOf("bo")->id == id);
    CHECK(book.find(id)->members == std::vector<std::string>{"ana", "bo"});
    // One party a player.
    CHECK(book.create("bo", 0).reason == "InParty");
    REQUIRE(book.create("cy", 0).ok);
    CHECK(book.invite(id, "ana", "cy").reason == "InParty");
    // A declined invitation is gone.
    REQUIRE(book.invite(id, "bo", "dee").ok);
    REQUIRE(book.decline(id, "dee").ok);
    CHECK(book.accept(id, "dee").reason == "NotInvited");

    REQUIRE(book.setLeader(id, "bo").ok);
    CHECK(book.find(id)->leader == "bo");
    CHECK(book.setLeader(id, "cy").reason == "NotMember");
    // The leader who leaves hands it on; the last one ends it.
    REQUIRE(book.leave("bo").ok);
    CHECK(book.find(id)->leader == "ana");
    (void)book.takeGone();
    REQUIRE(book.leave("ana").ok);
    CHECK_FALSE(book.find(id).has_value());
    CHECK(book.takeGone() == std::vector<std::string>{id});
}

TEST_CASE("party book > at most the size, and a kick is a leave")
{
    PartyBook book(3);
    const std::string id = book.create("ana", 0).party->id;
    for (const char* who : {"bo", "cy"}) {
        REQUIRE(book.invite(id, "ana", who).ok);
        REQUIRE(book.accept(id, who).ok);
    }
    CHECK(book.invite(id, "ana", "dee").reason == "Full");
    REQUIRE(book.kick(id, "cy").ok);
    CHECK_FALSE(book.partyOf("cy").has_value());
    REQUIRE(book.invite(id, "ana", "dee").ok);
    REQUIRE(book.accept(id, "dee").ok);
    CHECK(book.find(id)->members.size() == 3);
}

TEST_CASE("party book > kept five minutes with nobody online, and a member back within them still in it")
{
    PartyBook book;
    const std::string id = book.create("ana", 0).party->id;
    REQUIRE(book.invite(id, "ana", "bo").ok);
    REQUIRE(book.accept(id, "bo").ok);
    book.setOnline(online({"ana", "bo"}), 0);
    // Both drop; back within four minutes: still a party.
    book.setOnline(online({}), 1 * Minute);
    book.setOnline(online({}), 4 * Minute);
    CHECK(book.find(id).has_value());
    book.setOnline(online({"bo"}), 5 * Minute);
    CHECK(book.partyOf("bo")->id == id);
    // Gone for good past five.
    book.setOnline(online({}), 6 * Minute);
    book.setOnline(online({}), 10 * Minute);
    CHECK(book.find(id).has_value());
    book.setOnline(online({}), 12 * Minute);
    CHECK_FALSE(book.find(id).has_value());
    CHECK_FALSE(book.partyOf("ana").has_value());
}

TEST_CASE("party book > a request as text is answered as text")
{
    PartyBook book;
    const std::string made = engine::app::runPartyOp(book, R"({"op":"create","member":"ana"})", 0);
    CHECK(made.find("\"ok\":true") != std::string::npos);
    CHECK(made.find("\"leader\":\"ana\"") != std::string::npos);
    const std::string id = book.partyOf("ana")->id;
    CHECK(engine::app::runPartyOp(book, R"({"op":"get","member":"ana"})", 0).find(id) != std::string::npos);
    CHECK(engine::app::runPartyOp(book, R"({"op":"get","member":"nobody"})", 0).find("\"party\":null") !=
          std::string::npos);
    CHECK(engine::app::runPartyOp(book, "not json", 0).find("Invalid") != std::string::npos);
    CHECK(engine::app::runPartyOp(book, R"({"op":"juggle"})", 0).find("Invalid") != std::string::npos);
}
