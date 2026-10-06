// **A relay asked how far it is and what it carries** (ADR 0178, amended):
// the call a game with relays in several regions makes before it joins or
// hosts, to choose one. A relay answers a ping with no session and no
// registration (`rendezvous::Ping`), so this needs no match -- and has a
// socket of its own, because before a match there is no transport to borrow
// one from.
//
// A thread of its own, started with the first ask: looking a name up blocks,
// and a script that asks three regions must not wait for one after another.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "engine/core/types.h"

namespace engine::net {

struct RelayPingTicket
{
    core::u32 id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    [[nodiscard]] constexpr bool operator==(const RelayPingTicket&) const noexcept = default;
};

// What a relay said, or that it said nothing.
struct RelayPingResult
{
    // False: no answer in the time allowed -- the name is nobody's, nothing
    // listens there, or the way to it is shut. Nothing below is set.
    bool answered = false;
    // The round trip, the least of the asks answered: what a match through
    // this relay adds to each of its ends.
    core::f64 pingMs = 0.0;
    // What it carries: matches registered, players it relays, the bytes a
    // second through it, and how long it has been up.
    core::u32 matches = 0;
    core::u32 relayed = 0;
    core::u32 bytesPerSecond = 0;
    core::u32 uptimeSeconds = 0;
};

class RelayPinger
{
public:
    RelayPinger();
    ~RelayPinger();
    RelayPinger(const RelayPinger&) = delete;
    RelayPinger& operator=(const RelayPinger&) = delete;

    // How many times a relay is asked, how far apart, and how long after the
    // last ask an answer is still waited for once one has come.
    static constexpr core::u32 Asks = 3;
    static constexpr core::u32 AskEveryMs = 120;
    static constexpr core::u32 SettleMs = 250;

    // Asks `relay` -- `host` or `host:port`, the relay's default port without
    // one -- and never blocks. An invalid ticket when no socket could be made.
    [[nodiscard]] RelayPingTicket submit(std::string_view relay, core::u32 timeoutMs);

    // True once for each ticket, when it is answered or given up on.
    [[nodiscard]] bool take(RelayPingTicket ticket, RelayPingResult& out);

    // Asks not yet taken.
    [[nodiscard]] core::usize outstanding() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::net
