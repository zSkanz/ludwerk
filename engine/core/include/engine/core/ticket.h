// **A ticket that names an account** (ADR 0226): a standard JWT, signed with
// Ed25519 by a game's own backend, checked where the server is with no call
// out.
//
// The shape is fixed and the check takes nothing from the ticket that could
// change how it is checked: the algorithm is EdDSA or the ticket is refused
// before its signature is looked at, and the key is the game's, never one the
// ticket names.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/core/types.h"

namespace engine::core {

// The longest a ticket may live, from issue to expiry, and how far two clocks
// may disagree each way.
inline constexpr std::int64_t TicketMostSeconds = 90;
inline constexpr std::int64_t TicketClockSlack = 30;

// What a ticket says: the five the check needs, and every other top-level
// claim that is a string, a number or a boolean, for the game to read.
struct TicketClaims
{
    std::string subject;
    std::string audience;
    std::string id;
    std::int64_t issuedAt = 0;
    std::int64_t expiresAt = 0;
    std::vector<std::pair<std::string, std::variant<std::string, f64, bool>>> all;
};

// **The ids already spent**, each kept until its ticket would have expired:
// a ticket is good for one join.
class TicketReplay
{
public:
    // False when `id` was spent before; spends it otherwise.
    bool spend(const std::string& id, std::int64_t expiresAt, std::int64_t now);

private:
    std::map<std::string, std::int64_t> m_spent;
};

// **Checks `ticket`** against the backend's public key, the game's id and the
// time now. `replay` null checks signature and audience only -- a world
// reached by a pass, whose ticket was spent at the front door. Nothing on
// failure, with `why` saying what was wrong, for the server's log.
[[nodiscard]] std::optional<TicketClaims> verifyTicket(std::string_view ticket, const std::array<u8, 32>& publicKey,
                                                       std::string_view audience, std::int64_t now,
                                                       TicketReplay* replay, std::string& why);

// **A ticket made here**, for `ludwerk ticket sign` and the tests: what a
// backend's JWT library makes, signed with `secret`.
[[nodiscard]] std::string signTicket(const std::array<u8, 64>& secret, std::string_view subject,
                                     std::string_view audience, std::int64_t issuedAt, std::int64_t seconds,
                                     std::string_view id);
// A JWT's own parts, for a test that forges one: `header` and `payload` as
// JSON, signed with `secret`.
[[nodiscard]] std::string signTicketParts(const std::array<u8, 64>& secret, std::string_view header,
                                          std::string_view payload);

// **An Ed25519 private key as a PKCS #8 PEM file**, the form a JWT library
// reads (`importPKCS8`, `load_pem_private_key`), and its seed back.
[[nodiscard]] std::string ed25519PrivatePem(const std::array<u8, 32>& seed);
[[nodiscard]] std::optional<std::array<u8, 32>> ed25519SeedFromPem(std::string_view pem);

// Base64url, with no padding, as a JWT writes it; and back.
[[nodiscard]] std::string base64UrlEncode(std::span<const u8> bytes);
[[nodiscard]] std::optional<std::vector<u8>> base64UrlDecode(std::string_view text);

} // namespace engine::core
