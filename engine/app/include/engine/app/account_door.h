// **Signing players in at the front door** (ADR 0237 and its amendment): an
// account's name and password checked, or an account made, and a ticket
// signed for it -- the ticket a world's door, and the manager's own, take.
//
// The hashes are slow on purpose and run in a pool of their own; whoever
// keeps the front door asks and takes answers once a pass, and never waits.
// The accounts are the data store's, read and written on the asker's thread
// alone: the pool sees passwords and hashes, never the database.
#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/app/data_store.h"
#include "engine/core/types.h"

namespace engine::app {

class AccountDoor
{
public:
    // `Enum.SignInFailure`, in its order (amendment B).
    enum class Failure : core::u8
    {
        None,
        WrongNameOrPassword,
        Taken,
        InvalidName,
        InvalidPassword,
        Busy,
        Refused,
    };

    struct Options
    {
        // The manager's signing key: the tickets' and the passes'.
        std::array<core::u8, 64> secret{};
        // The game's id, a ticket's audience.
        std::string audience;
        // Whether a player may make an account, or only the console.
        bool registration = true;
        // `[accounts] name_pattern`: a name must match it too. Empty for none.
        std::string namePattern;
        // The pool's threads: none for min(cores / 2, 4). And the most asks
        // waiting, in the pool and delayed, before one is answered `Busy`.
        unsigned threads = 0;
        core::usize most = 64;
        // Seconds, for a ticket; nanoseconds that only go forward, for the
        // limits.
        std::function<std::int64_t()> unixClock;
        std::function<core::u64()> clock;
        // **What each answer cost the pool**, kept for `takeWork`: off, so a
        // door nobody asks keeps nothing.
        bool keepWork = false;
        // **Who a session of the game's Nakama is** (ADR 0242, as amended):
        // its user's id, or none for a session Nakama does not know. Asked
        // on the pool's threads; none here, and a session is refused.
        std::function<std::optional<std::string>(const std::string& session)> whoami;
        // **A match Nakama's matchmaker made**, as Nakama kept it: its
        // players' user ids and the queue they asked for; none for a match
        // it does not know. Asked with the server's key, on the pool.
        struct NakamaMatch
        {
            std::vector<std::string> users;
            std::string queue;
        };
        std::function<std::optional<NakamaMatch>(const std::string& match)> matchUsers;
    };

    // What is asked: an account signed in to or made, or a session checked.
    enum class Kind : core::u8
    {
        SignIn,
        Register,
        Session,
        // A match of Nakama's matchmaker: the session in `password`, the
        // match's id in `name`.
        Match,
    };

    struct Result
    {
        core::u32 peer = 0;
        Failure failure = Failure::None;
        std::string ticket;
        std::string account;
        // Seconds before another try is worth it: `Busy`, and `Refused` past
        // an address's failures.
        core::u32 retryAfter = 0;
    };

    // **A match Nakama made, a player in it at the door** (`Kind::Match`):
    // its id, its players' accounts as the engine knows them, and its queue.
    struct MatchFound
    {
        std::string match;
        std::vector<std::string> accounts;
        std::string queue;
    };

    // The limits (amendment G and §2): an address's failures in a minute,
    // and a name's in fifteen, before the delays begin; and the longest one.
    static constexpr core::u32 AddressFailures = 5;
    static constexpr core::u32 NameFailures = 5;
    static constexpr core::u32 MostDelaySeconds = 32;
    static constexpr core::usize ShortestPassword = 8;
    static constexpr core::usize LongestPassword = 128;
    static constexpr std::int64_t TicketSeconds = 90;

    AccountDoor(IDataStore& store, Options options);
    ~AccountDoor();
    AccountDoor(const AccountDoor&) = delete;
    AccountDoor& operator=(const AccountDoor&) = delete;

    // **A sign-in, or a register**, from `peer` at `address`: answered in a
    // later `take`, after the same work whichever way it goes. `password` is
    // wiped once it has been hashed or checked.
    void ask(core::u32 peer, std::string_view address, std::string_view name, std::string password, bool registering);
    // The same, or a session of the game's Nakama (`password` its token):
    // answered with the engine's own ticket for the account `nakama:<user id>`.
    void ask(core::u32 peer, std::string_view address, std::string_view name, std::string password, Kind kind);
    // Once a pass: the asks whose delay is over handed to the pool, and what
    // the pool finished turned into answers.
    void frame();
    [[nodiscard]] std::vector<Result> take();
    // The matches answered since, each once its player's `Result` is.
    [[nodiscard]] std::vector<MatchFound> takeMatches();
    // Nothing waiting, in the pool or delayed: for a test.
    [[nodiscard]] bool idle() const;

    // **The work behind an answer** (amendment A): the Argon2 evaluations the
    // pool made for it and their cost (`core::passwordCost`), so that every
    // way an ask goes is shown to do the same work without a clock. An
    // answer given without the pool -- refused, invalid, busy -- made none.
    struct Work
    {
        core::u32 peer = 0;
        Failure failure = Failure::None;
        core::u32 hashes = 0;
        std::string cost;
    };
    // With `Options::keepWork`, one for each answer `take` gave, in order.
    [[nodiscard]] std::vector<Work> takeWork();
    // **The most Argon2 evaluations the pool has had in flight at once**: a
    // pool of four asked many at once reaches four -- its parallelism shown
    // without a clock, which a shared machine cannot be trusted with.
    [[nodiscard]] core::u32 mostAtOnce() const;

    // **The console's `account`** (§3): `create <name> <password>`, `rename
    // <name> <new name>`, `password <name> <password>`, `delete <name>`. A
    // hash made here is made at once: the operator waits on it, not a player.
    [[nodiscard]] std::string run(std::string_view line);

    // **A login name's skeleton** (amendment F): three to twenty of
    // `A-Z a-z 0-9 _ . -`, the case folded and `l I 1`, `o 0` made one; none
    // for a name that is not a login name.
    [[nodiscard]] static std::optional<std::string> skeletonOf(std::string_view name);

private:
    struct Job
    {
        core::u64 id = 0;
        bool hashing = false;
        // A session's token in `password`, asked of Nakama; and a match's id,
        // where the session is to be found in it.
        bool session = false;
        std::string match;
        std::string password;
        std::string hash;
    };
    struct Done
    {
        core::u64 id = 0;
        bool ok = false;
        std::string hash;
        // The Argon2 evaluations made, and at what cost.
        core::u32 hashes = 0;
        std::string cost;
        // A session's user, as Nakama said; and the match's players and queue.
        std::string user;
        std::vector<std::string> users;
        std::string queue;
    };
    // What waits for the pool, and what it is for.
    struct Asked
    {
        core::u32 peer = 0;
        std::string address;
        std::string name;
        std::string skeleton;
        bool registering = false;
        bool session = false;
        std::string match;
        // The account found by name for a sign-in; none, and the hash made at
        // start checked instead.
        std::optional<DataAccount> account;
        core::u64 notBeforeNs = 0;
        Job job;
    };
    struct Failures
    {
        core::u32 count = 0;
        core::u64 lastNs = 0;
    };

    [[nodiscard]] bool nameAllowed(std::string_view name) const;
    [[nodiscard]] std::string ticketFor(const DataAccount& account) const;
    void failed(const Asked& asked);
    void finish(Asked& asked, const Done& done);
    void answer(Result result, core::u32 hashes, std::string cost);
    void work();
    [[nodiscard]] core::u64 nowNs() const;

    IDataStore& m_store;
    Options m_options;
    std::optional<std::regex> m_pattern;
    // **Checked for a name with no account**, so that the time to answer is
    // a right one's (amendment A).
    std::string m_decoy;
    core::u64 m_nextJob = 1;
    std::deque<Asked> m_delayed;
    std::map<core::u64, Asked> m_inPool;
    std::vector<Result> m_results;
    std::vector<Work> m_work;
    std::vector<MatchFound> m_matched;
    std::map<std::string, std::deque<core::u64>, std::less<>> m_addressFailures;
    std::map<std::string, Failures, std::less<>> m_nameFailures;

    // The pool: its own lock, and nothing of the database.
    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    std::vector<Done> m_done;
    bool m_stopping = false;
    // Evaluations in flight now, and the most there have been.
    core::u32 m_hashing = 0;
    core::u32 m_mostHashing = 0;
    std::vector<std::thread> m_threads;
};

} // namespace engine::app
