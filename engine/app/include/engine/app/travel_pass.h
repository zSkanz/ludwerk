// **A pass from one world to the next** (ADR 0222): signed by the manager, for
// one player, one place, thirty seconds and one use.
//
// A world under a manager takes nobody without one, and trusts the manager's
// public key -- given when the manager started it -- and nothing else. The
// player is proved by the match's handshake (ADR 0224): the pass names their
// key, and the joiner's must be it. What a traveller carries -- the game's
// `data`, and the ticket they logged in with -- is inside what is signed.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/types.h"

namespace engine::app {

// How long a pass lives, in seconds.
inline constexpr std::int64_t PassSeconds = 30;

struct TravelPass
{
    // The player's `PublicKey`, in hexadecimal.
    std::string player;
    // The world they leave -- empty from the front door -- and the one, or
    // the instance's id, they go to.
    std::string origin;
    std::string target;
    // What the travel carries: the game's `data` as a save writes it, and
    // the ticket the player presented at the front door (ADR 0226).
    std::string data;
    std::string ticket;
    // **The account the manager knows the player by** (ADR 0237 amendment
    // C), inside what is signed: a world takes `Player.AccountId` from it.
    // A field a world of an older build ignores, so the pass's version stays.
    std::string account;
    // Why they are sent, when not of their own accord: `InstanceFailed`.
    std::string reason;
    std::int64_t issued = 0;
    std::int64_t expires = 0;
    std::string nonce;
};

// Signed with the manager's Ed25519 key, as text for a `Redirect`.
[[nodiscard]] std::string signPass(const TravelPass& pass, const std::array<core::u8, 64>& managerSecret);

// **The nonces already taken**, each kept until its pass would have expired.
class PassReplay
{
public:
    bool spend(const std::string& nonce, std::int64_t expires, std::int64_t now);

private:
    std::map<std::string, std::int64_t> m_spent;
};

// Why a pass was not taken: what the player is told, and the log says.
enum class PassRefusal : core::u8
{
    Missing,
    NotTheManagers,
    OtherWorld,
    OtherPlayer,
    Expired,
    Used,
};

// **Opens and checks a pass** for this world (`target`) and this joiner
// (`player`, hexadecimal), at `now`: the manager's signature, the place, the
// player, the time and one use. Nothing on failure, with `why`.
[[nodiscard]] std::optional<TravelPass> openPass(std::string_view text, const std::array<core::u8, 32>& managerKey,
                                                 std::string_view target, std::string_view player, std::int64_t now,
                                                 PassReplay& replay, PassRefusal& why);

} // namespace engine::app
