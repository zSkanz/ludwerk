// **Signing players in** (ADR 0237 and its amendment): a login name's
// skeleton; an account made and signed in to; the refusals, each after the
// same work; an address's failures and a name's delays; the console's
// accounts; and a ticket a world's door takes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <string>
#include <thread>
#include <vector>

#include "engine/app/account_door.h"
#include "engine/app/data_store.h"
#include "engine/core/crypto.h"
#include "engine/core/ticket.h"
#include "engine/platform/platform.h"
#include "engine/platform/process.h"

using engine::app::AccountDoor;
using Failure = engine::app::AccountDoor::Failure;

namespace {

constexpr std::string_view Game = "dev.ludwerk.accounts-test";

[[nodiscard]] std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A door whose clock a test moves, over a store in memory.
struct Door
{
    engine::core::Ed25519Pair key = engine::core::ed25519Generate();
    std::unique_ptr<engine::app::IDataStore> store = engine::app::makeMemoryDataStore();
    std::shared_ptr<std::atomic<engine::core::u64>> clock = std::make_shared<std::atomic<engine::core::u64>>(1);
    std::unique_ptr<AccountDoor> door;

    explicit Door(AccountDoor::Options options = {})
    {
        options.secret = key.secret;
        options.audience = std::string(Game);
        options.unixClock = unixNow;
        options.clock = [clock = clock]() { return clock->load(); };
        if (options.threads == 0)
            options.threads = 2;
        door = std::make_unique<AccountDoor>(*store, std::move(options));
    }

    void advance(double seconds) { clock->fetch_add(static_cast<engine::core::u64>(seconds * 1e9)); }

    // Asked, and answered: the pool given the time its hash takes.
    [[nodiscard]] AccountDoor::Result ask(std::string_view name, std::string password, bool registering,
                                          std::string_view address = "10.0.0.1", engine::core::u32 peer = 1)
    {
        door->ask(peer, address, name, std::move(password), registering);
        return answer();
    }

    // The next answer, the pool given the time its hash takes.
    [[nodiscard]] AccountDoor::Result answer()
    {
        for (int at = 0; at < 2000; ++at) {
            door->frame();
            std::vector<AccountDoor::Result> results = door->take();
            if (!results.empty())
                return results.front();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("no answer in ten seconds");
        return {};
    }
};

} // namespace

TEST_CASE("account door > a login name's skeleton folds its case and the letters that look alike")
{
    CHECK(AccountDoor::skeletonOf("Admin") == AccountDoor::skeletonOf("admin"));
    CHECK(AccountDoor::skeletonOf("Adm1n") == AccountDoor::skeletonOf("admin"));
    CHECK(AccountDoor::skeletonOf("AdmIn") == AccountDoor::skeletonOf("admln"));
    CHECK(AccountDoor::skeletonOf("b0b") == AccountDoor::skeletonOf("BOB"));
    CHECK(AccountDoor::skeletonOf("ana.b-c_d").has_value());
    CHECK_FALSE(AccountDoor::skeletonOf("ab").has_value());
    CHECK_FALSE(AccountDoor::skeletonOf("abcdefghijklmnopqrstu").has_value());
    CHECK_FALSE(AccountDoor::skeletonOf("ana!").has_value());
    CHECK_FALSE(AccountDoor::skeletonOf("an a").has_value());
    // Accents and other scripts are a display name's, never a login's.
    CHECK_FALSE(AccountDoor::skeletonOf("jos\xc3\xa9").has_value());
}

TEST_CASE("account door > an account is made, signed in to whatever its case, and its ticket is a world's to take")
{
    Door door;
    const AccountDoor::Result made = door.ask("Ana_1", "correct horse battery", true);
    REQUIRE(made.failure == Failure::None);
    CHECK(made.account.size() == 32);
    // The ticket the worlds take: the manager's key, the game, the account.
    std::string why;
    engine::core::TicketReplay replay;
    const std::optional<engine::core::TicketClaims> claims =
        engine::core::verifyTicket(made.ticket, door.key.pub, Game, unixNow(), &replay, why);
    REQUIRE_MESSAGE(claims.has_value(), why);
    CHECK(claims->subject == made.account);
    CHECK(claims->expiresAt - claims->issuedAt <= AccountDoor::TicketSeconds);

    const AccountDoor::Result signedIn = door.ask("ana_1", "correct horse battery", false);
    CHECK(signedIn.failure == Failure::None);
    CHECK(signedIn.account == made.account);
    // A name that only looks like it is another name, with no account.
    CHECK(door.ask("ana_l", "correct horse battery", false).failure == Failure::WrongNameOrPassword);
    CHECK(door.ask("Ana_1", "not the password", false).failure == Failure::WrongNameOrPassword);
    CHECK(door.ask("nobody", "correct horse battery", false).failure == Failure::WrongNameOrPassword);
    // A register of a name that looks like one taken.
    CHECK(door.ask("ana_l", "another password", true).failure == Failure::Taken);
    CHECK(door.ask("bo!", "another password", true).failure == Failure::InvalidName);
    CHECK(door.ask("bo_2", "short", true).failure == Failure::InvalidPassword);
    // The hash is kept, never the password.
    const std::optional<engine::app::DataAccount> kept = door.store->accountWithId(made.account);
    REQUIRE(kept.has_value());
    CHECK(kept->hash.find("correct horse battery") == std::string::npos);
    CHECK(kept->hash.starts_with("$argon2id$"));
    CHECK(kept->signedIn > 0);
}

TEST_CASE("account door > a door that makes no accounts refuses a register, and a pattern narrows the names")
{
    AccountDoor::Options options;
    options.registration = false;
    Door closed(options);
    CHECK(closed.ask("ana_1", "correct horse battery", true).failure == Failure::Refused);

    AccountDoor::Options narrowed;
    narrowed.namePattern = "^[a-z]+$";
    Door door(narrowed);
    CHECK(door.ask("ana_1", "correct horse battery", true).failure == Failure::InvalidName);
    CHECK(door.ask("ana", "correct horse battery", true).failure == Failure::None);
}

TEST_CASE("account door > five failures from an address in a minute, and the sixth is refused without a hash")
{
    Door door;
    REQUIRE(door.ask("ana_1", "correct horse battery", true).failure == Failure::None);
    for (int at = 0; at < 5; ++at)
        CHECK(door.ask("ana_1", "wrong password " + std::to_string(at), false, "10.0.0.9").failure ==
              Failure::WrongNameOrPassword);
    // Answered at once, not by the pool: no frame is needed.
    door.door->ask(1, "10.0.0.9", "ana_1", "correct horse battery", false);
    std::vector<AccountDoor::Result> refused = door.door->take();
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].failure == Failure::Refused);
    CHECK(refused[0].retryAfter > 0);
    CHECK(refused[0].retryAfter <= 60);
    CHECK(door.door->idle());
    // Another address is not refused -- once the name's own wait is over:
    // the five failures were the name's too, so its next try waits a second.
    door.door->ask(1, "10.0.0.10", "ana_1", "correct horse battery", false);
    door.advance(1.0);
    CHECK(door.answer().failure == Failure::None);
    // And a minute on, this address is not refused either.
    door.advance(61.0);
    CHECK(door.ask("ana_1", "correct horse battery", false, "10.0.0.9").failure == Failure::None);
}

TEST_CASE("account door > a name tried from many addresses waits longer each time, and a right password clears it")
{
    Door door;
    REQUIRE(door.ask("ana_1", "correct horse battery", true).failure == Failure::None);
    // Five failures, each from an address of its own.
    for (int at = 0; at < 5; ++at)
        CHECK(door.ask("ana_1", "wrong", false, "10.1.0." + std::to_string(at)).failure ==
              Failure::WrongNameOrPassword);
    // The sixth waits a second: nothing comes while the clock stands still.
    door.door->ask(1, "10.1.0.99", "ana_1", "wrong", false);
    for (int at = 0; at < 40; ++at) {
        door.door->frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(door.door->take().empty());
    CHECK_FALSE(door.door->idle());
    door.advance(1.0);
    AccountDoor::Result late;
    for (int at = 0; at < 2000 && late.peer == 0; ++at) {
        door.door->frame();
        if (std::vector<AccountDoor::Result> results = door.door->take(); !results.empty())
            late = results.front();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(late.failure == Failure::WrongNameOrPassword);
    // The seventh waits two: one second is not enough.
    door.door->ask(1, "10.1.0.98", "ana_1", "correct horse battery", false);
    door.advance(1.0);
    for (int at = 0; at < 40; ++at) {
        door.door->frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(door.door->take().empty());
    door.advance(1.0);
    AccountDoor::Result right;
    for (int at = 0; at < 2000 && right.peer == 0; ++at) {
        door.door->frame();
        if (std::vector<AccountDoor::Result> results = door.door->take(); !results.empty())
            right = results.front();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // **Never locked**: the account's player gets in, and the count is cleared -- a
    // failure after it is answered with the clock standing still.
    CHECK(right.failure == Failure::None);
    CHECK(door.ask("ana_1", "wrong", false, "10.1.0.97").failure == Failure::WrongNameOrPassword);
    // A name with no account waits the same, so the delay says nothing.
    for (int at = 0; at < 5; ++at)
        (void)door.ask("nobody", "wrong", false, "10.2.0." + std::to_string(at));
    door.door->ask(2, "10.2.0.99", "nobody", "wrong", false);
    door.door->frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    door.door->frame();
    bool early = false;
    for (const AccountDoor::Result& result : door.door->take())
        early = early || result.peer == 2;
    CHECK_FALSE(early);
}

TEST_CASE("account door > past the most waiting, an ask is answered busy at once")
{
    AccountDoor::Options options;
    options.most = 2;
    options.threads = 1;
    Door door(options);
    door.door->ask(1, "10.0.0.1", "ana_1", "correct horse battery", true);
    door.door->ask(2, "10.0.0.2", "bo_22", "correct horse battery", true);
    door.door->ask(3, "10.0.0.3", "cy_33", "correct horse battery", true);
    const std::vector<AccountDoor::Result> busy = door.door->take();
    REQUIRE(busy.size() == 1);
    CHECK(busy[0].peer == 3);
    CHECK(busy[0].failure == Failure::Busy);
    CHECK(busy[0].retryAfter >= 1);
}

TEST_CASE("account door > the console makes accounts and passwords, and takes none")
{
    Door door;
    const std::string made = door.door->run("account create Ana_1");
    const std::size_t colon = made.rfind(": ");
    REQUIRE(colon != std::string::npos);
    const std::string password = made.substr(colon + 2);
    CHECK(password.size() == 16);
    CHECK(door.ask("ana_1", password, false).failure == Failure::None);
    // A name that looks like one taken.
    CHECK(door.door->run("account create ana_l").find(password) == std::string::npos);
    CHECK_FALSE(door.store->accountNamed(*AccountDoor::skeletonOf("bo_22")).has_value());
    // A password is never typed: one given on the line is not taken.
    CHECK(door.door->run("account create bo_22 hunter22") == door.door->run("account"));
    CHECK_FALSE(door.store->accountNamed(*AccountDoor::skeletonOf("bo_22")).has_value());

    const std::string renewed = door.door->run("account password ana_1");
    const std::string second = renewed.substr(renewed.rfind(": ") + 2);
    CHECK(second != password);
    CHECK(door.ask("ana_1", password, false).failure == Failure::WrongNameOrPassword);
    CHECK(door.ask("ana_1", second, false).failure == Failure::None);
    (void)door.door->run("account rename ana_1 ana_9");
    CHECK(door.ask("ana_9", second, false).failure == Failure::None);
    (void)door.door->run("account delete ana_9");
    CHECK(door.ask("ana_9", second, false).failure == Failure::WrongNameOrPassword);
}

TEST_CASE("account door > a wrong name, a wrong password and a taken name take a right one's time" *
          doctest::skip(!engine::platform::environmentFlag("ENG_TIMED_TESTS")))
{
    // **Timed**: run where nothing else measures time (the gate's perf stage).
    Door door;
    REQUIRE(door.ask("ana_1", "correct horse battery", true).failure == Failure::None);
    REQUIRE(door.ask("bo_22", "correct horse battery", true).failure == Failure::None);
    // How long one ask takes to be answered, waited on a millisecond at a
    // time: a coarser wait is a tenth of a hash's time.
    int asked = 0;
    const auto timed = [&](std::string_view name, std::string password, bool registering) {
        asked += 1;
        const auto start = std::chrono::steady_clock::now();
        door.door->ask(static_cast<engine::core::u32>(asked),
                       "10.3." + std::to_string(asked / 250) + "." + std::to_string(asked % 250), name,
                       std::move(password), registering);
        for (int at = 0; at < 10000; ++at) {
            door.door->frame();
            if (!door.door->take().empty())
                return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FAIL("no answer in ten seconds");
        return 0.0;
    };
    // **Warmed first**: a pool's first hashes touch memory none has yet.
    for (int at = 0; at < 4; ++at)
        (void)timed("ana_1", "correct horse battery", false);
    // **Each round all four, one after the other**, so whatever the machine
    // does meanwhile falls on all of them alike; a right password each round
    // clears the name's failures (amendment G), and each name with no
    // account is its own.
    std::vector<double> right;
    std::vector<double> wrongPassword;
    std::vector<double> noName;
    std::vector<double> taken;
    for (int round = 0; round < 7; ++round) {
        right.push_back(timed("ana_1", "correct horse battery", false));
        wrongPassword.push_back(timed("ana_1", "not the password", false));
        noName.push_back(timed("nobody" + std::to_string(round), "correct horse battery", false));
        taken.push_back(timed("BO_22", "correct horse battery", true));
    }
    const auto median = [](std::vector<double> samples) {
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };
    const double r = median(right);
    const double w = median(wrongPassword);
    const double n = median(noName);
    const double t = median(taken);
    MESSAGE("right ", r, " s, wrong password ", w, " s, no such name ", n, " s, taken ", t, " s");
    CHECK(std::abs(w - r) < r * 0.10);
    CHECK(std::abs(n - r) < r * 0.10);
    CHECK(std::abs(t - r) < r * 0.10);
}

TEST_CASE("account door > twenty sign-ins a second on four cores, and the memory it takes" *
          doctest::skip(!engine::platform::environmentFlag("ENG_TIMED_TESTS")))
{
    // **Measured** (amendment A): the pool of four a machine of eight cores
    // or more gives, signing in 200 players asked at once.
    AccountDoor::Options options;
    options.threads = 4;
    options.most = 256;
    Door door(options);
    REQUIRE(door.ask("ana_1", "correct horse battery", true).failure == Failure::None);
    const engine::core::u64 before = engine::platform::residentBytes();
    engine::core::u64 peak = before;
    const auto start = std::chrono::steady_clock::now();
    for (engine::core::u32 at = 0; at < 200; ++at)
        door.door->ask(at + 1, "10.4." + std::to_string(at / 250) + "." + std::to_string(at % 250), "ana_1",
                       "correct horse battery", false);
    std::size_t answered = 0;
    std::size_t signedIn = 0;
    while (answered < 200) {
        door.door->frame();
        for (const AccountDoor::Result& result : door.door->take()) {
            answered += 1;
            signedIn += result.failure == Failure::None ? 1u : 0u;
        }
        peak = std::max(peak, engine::platform::residentBytes());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double rate = 200.0 / seconds;
    MESSAGE("[accounts] ", rate, " sign-ins a second on 4 threads; resident memory ", before / (1024 * 1024),
            " MB before, ", peak / (1024 * 1024), " MB at the most");
    CHECK(signedIn == 200);
    CHECK(rate >= 20.0);
}
