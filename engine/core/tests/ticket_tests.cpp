// A ticket that names an account (ADR 0226): a standard EdDSA JWT, checked
// with no call out -- and refused for every way it can be wrong.
#include <doctest/doctest.h>
#include <ostream>
#include <string>

#include "engine/core/base64.h"
#include "engine/core/crypto.h"
#include "engine/core/ticket.h"

using namespace engine;
using namespace engine::core;

namespace {

constexpr std::int64_t Now = 1'800'000'010;
constexpr std::string_view Game = "com.example.realms";

// A key pair whose seed is the bytes 0 to 31, and a ticket PyJWT 2.13 signed
// with it -- what a backend's ordinary JWT library makes.
constexpr std::string_view LibraryPem = "-----BEGIN PRIVATE KEY-----\n"
                                        "MC4CAQAwBQYDK2VwBCIEIAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4f\n"
                                        "-----END PRIVATE KEY-----\n";
constexpr std::string_view LibraryPublic = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
constexpr std::string_view LibraryTicket =
    "eyJhbGciOiJFZERTQSIsInR5cCI6IkpXVCJ9."
    "eyJzdWIiOiJhY2NvdW50LTQyIiwiYXVkIjoiY29tLmV4YW1wbGUucmVhbG1zIiwiaWF0IjoxODAwMDAwMDAwLCJleHAiOjE4MDAwMDAwOTAs"
    "Imp0aSI6IjViMWYwYzJlLTdhNGQtNGMxYi05YjY1LTJmMGU2YTFkM2M3NyIsIm5hbWUiOiJBZGEifQ."
    "-KpXTqnUkP_A8V27T9gy4LbzaIjPgmECvEM2EEM71dnUmBL53fRh8Z_0zgxZhyTFmWEaB6lFCgx5QGaoH0ZRDA";

[[nodiscard]] std::array<u8, 32> publicOf(std::string_view base64)
{
    const std::optional<std::vector<u8>> bytes = base64Decode(base64);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == 32);
    std::array<u8, 32> out{};
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return out;
}

[[nodiscard]] bool accepted(std::string_view ticket, const std::array<u8, 32>& key, TicketReplay& replay,
                            std::string_view game = Game, std::int64_t now = Now)
{
    std::string why;
    return verifyTicket(ticket, key, game, now, &replay, why).has_value();
}

} // namespace

TEST_CASE("a ticket a stock JWT library signed is accepted, its claims read, and its key file is ours")
{
    const std::array<u8, 32> key = publicOf(LibraryPublic);
    TicketReplay replay;
    std::string why;
    const std::optional<TicketClaims> claims = verifyTicket(LibraryTicket, key, Game, Now, &replay, why);
    REQUIRE_MESSAGE(claims.has_value(), why);
    CHECK(claims->subject == "account-42");
    CHECK(claims->audience == Game);
    CHECK(claims->expiresAt - claims->issuedAt == 90);
    // A claim of the backend's own is there for the game.
    bool named = false;
    for (const auto& [name, value] : claims->all)
        named = named || (name == "name" && std::get<std::string>(value) == "Ada");
    CHECK(named);

    // The library's PEM is the seed this engine reads, and writes back the same.
    const std::optional<std::array<u8, 32>> seed = ed25519SeedFromPem(LibraryPem);
    REQUIRE(seed.has_value());
    CHECK(ed25519FromSeed(*seed).pub == key);
    CHECK(ed25519PrivatePem(*seed) == LibraryPem);

    // And a ticket this engine signs is the same shape, accepted the same way.
    const Ed25519Pair pair = ed25519FromSeed(*seed);
    const std::string ours = signTicket(pair.secret, "account-7", Game, Now, 60, "one");
    CHECK(accepted(ours, key, replay));
}

TEST_CASE("a ticket is refused forged, expired, too long, spent, for another game, or not EdDSA")
{
    const Ed25519Pair backend = ed25519Generate();
    const Ed25519Pair forger = ed25519Generate();
    TicketReplay replay;

    // Forged: signed by somebody else's key.
    CHECK_FALSE(accepted(signTicket(forger.secret, "a", Game, Now, 60, "f1"), backend.pub, replay));
    // A payload changed after signing.
    std::string changed = signTicket(backend.secret, "a", Game, Now, 60, "f2");
    changed[changed.find('.') + 5] = changed[changed.find('.') + 5] == 'A' ? 'B' : 'A';
    CHECK_FALSE(accepted(changed, backend.pub, replay));

    // Expired, past the clocks' slack -- and not yet, inside it.
    CHECK_FALSE(accepted(signTicket(backend.secret, "a", Game, Now - 200, 60, "e1"), backend.pub, replay));
    CHECK(accepted(signTicket(backend.secret, "a", Game, Now - 80, 60, "e2"), backend.pub, replay));
    // Issued in the future.
    CHECK_FALSE(accepted(signTicket(backend.secret, "a", Game, Now + 120, 60, "e3"), backend.pub, replay));
    // **Too long a life**: a backend cannot issue one for a day.
    CHECK_FALSE(accepted(signTicket(backend.secret, "a", Game, Now, 91, "l1"), backend.pub, replay));
    CHECK_FALSE(accepted(signTicket(backend.secret, "a", Game, Now, 86400, "l2"), backend.pub, replay));

    // **Spent once**.
    const std::string once = signTicket(backend.secret, "a", Game, Now, 60, "s1");
    CHECK(accepted(once, backend.pub, replay));
    CHECK_FALSE(accepted(once, backend.pub, replay));
    // No id: a ticket must be good for one join.
    CHECK_FALSE(accepted(signTicket(backend.secret, "a", Game, Now, 60, ""), backend.pub, replay));

    // Another game's.
    CHECK_FALSE(accepted(signTicket(backend.secret, "a", "com.other.game", Now, 60, "g1"), backend.pub, replay));

    // **The algorithm is not the ticket's to choose**: `none`, `HS256`, and
    // none at all, each refused -- even signed correctly underneath.
    const std::string payload = R"({"sub":"a","aud":"com.example.realms","iat":1800000010,"exp":1800000070,"jti":"x"})";
    CHECK_FALSE(
        accepted(signTicketParts(backend.secret, R"({"alg":"none","typ":"JWT"})", payload), backend.pub, replay));
    CHECK_FALSE(
        accepted(signTicketParts(backend.secret, R"({"alg":"HS256","typ":"JWT"})", payload), backend.pub, replay));
    CHECK_FALSE(accepted(signTicketParts(backend.secret, R"({"typ":"JWT"})", payload), backend.pub, replay));
    // A `none` ticket the way a forger writes one: no signature at all.
    const std::string unsigned_ =
        base64UrlEncode(std::span<const u8>(reinterpret_cast<const u8*>(R"({"alg":"none"})"), 14)) + "." +
        base64UrlEncode(std::span<const u8>(reinterpret_cast<const u8*>(payload.data()), payload.size())) + ".";
    CHECK_FALSE(accepted(unsigned_, backend.pub, replay));
    // A key the ticket names is never the one checked against.
    const std::string withKey =
        signTicketParts(forger.secret, R"({"alg":"EdDSA","typ":"JWT","kid":"backend","jwk":{"kty":"OKP"}})", payload);
    CHECK_FALSE(accepted(withKey, backend.pub, replay));

    // Not a JWT at all.
    CHECK_FALSE(accepted("", backend.pub, replay));
    CHECK_FALSE(accepted("a.b", backend.pub, replay));
    CHECK_FALSE(accepted("a.b.c.d", backend.pub, replay));

    // **Through a pass** (no replay list): signature and game only.
    std::string why;
    const std::string old = signTicket(backend.secret, "a", Game, Now - 3600, 60, "p1");
    CHECK(verifyTicket(old, backend.pub, Game, Now, nullptr, why).has_value());
    CHECK_FALSE(verifyTicket(old, backend.pub, "com.other.game", Now, nullptr, why).has_value());
}
