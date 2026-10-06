// A relay asked how far it is and what it carries (ADR 0178, amended), on
// real sockets: a relay on the loopback, and a pinger with a socket of its own.
#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <thread>

#include "engine/core/i18n.h"
#include "engine/net/relay_ping.h"
#include "engine/net/relay_service.h"

using namespace engine;
using namespace engine::net;

namespace {

// Waits for a ticket, and says how long that took. Bounded: a pinger that
// never answers is a failure, not a hung test.
[[nodiscard]] bool waitFor(RelayPinger& pinger, RelayPingTicket ticket, RelayPingResult& out, core::f64& tookMs)
{
    const auto started = std::chrono::steady_clock::now();
    for (int round = 0; round < 5000; ++round) {
        if (pinger.take(ticket, out)) {
            tookMs = std::chrono::duration<core::f64, std::milli>(std::chrono::steady_clock::now() - started).count();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

TEST_CASE("a relay says how far it is and what it carries, to anybody who asks")
{
    RelayService relay;
    REQUIRE_FALSE(relay.start(0).has_value());
    const std::string address = "127.0.0.1:" + std::to_string(relay.port());

    RelayPinger pinger;
    const RelayPingTicket ticket = pinger.submit(address, 2000);
    REQUIRE(ticket.valid());
    CHECK(pinger.outstanding() == 1);
    RelayPingResult result;
    core::f64 took = 0.0;
    REQUIRE(waitFor(pinger, ticket, result, took));
    CHECK(result.answered);
    // The loopback: a round trip is a fraction of a millisecond, and anything
    // a busy machine adds is nowhere near this.
    CHECK(result.pingMs >= 0.0);
    CHECK(result.pingMs < 200.0);
    CHECK(result.matches == 0);
    CHECK(result.relayed == 0);
    // Every ask answered: done as soon as the last one is, well before the
    // time allowed.
    CHECK(took < 1500.0);
    CHECK(pinger.outstanding() == 0);
    // Taken once.
    CHECK_FALSE(pinger.take(ticket, result));
    relay.stop();
}

TEST_CASE("three relays asked at once are answered at once, and one that is not there is said so")
{
    RelayService first;
    RelayService second;
    REQUIRE_FALSE(first.start(0).has_value());
    REQUIRE_FALSE(second.start(0).has_value());

    RelayPinger pinger;
    const auto started = std::chrono::steady_clock::now();
    const RelayPingTicket a = pinger.submit("127.0.0.1:" + std::to_string(first.port()), 2000);
    const RelayPingTicket b = pinger.submit("127.0.0.1:" + std::to_string(second.port()), 2000);
    // Nothing listens here: asked the same, answered by nobody.
    second.stop();
    const RelayPingTicket nobody = pinger.submit("127.0.0.1:" + std::to_string(second.port()), 600);
    // And an address that is nobody's at all is given up on at once.
    const RelayPingTicket nowhere = pinger.submit("127.0.0.1:0", 2000);
    REQUIRE(a.valid());
    REQUIRE(nobody.valid());
    REQUIRE(nowhere.valid());

    RelayPingResult result;
    core::f64 took = 0.0;
    REQUIRE(waitFor(pinger, nowhere, result, took));
    CHECK_FALSE(result.answered);
    CHECK(took < 400.0);

    REQUIRE(waitFor(pinger, a, result, took));
    CHECK(result.answered);
    REQUIRE(waitFor(pinger, nobody, result, took));
    CHECK_FALSE(result.answered);
    // The one that is not there cost its own time allowed, not the others':
    // all of it inside a second and a half.
    const core::f64 all =
        std::chrono::duration<core::f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    CHECK(all < 1500.0);
    // `b` was asked of a relay that stopped under it: answered or not, it ends.
    REQUIRE(waitFor(pinger, b, result, took));
    first.stop();
}
