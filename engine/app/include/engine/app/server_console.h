// **A server's console** (ADR 0218): what a dedicated server's operator types,
// and what a game adds to it.
//
// A process that serves with no window reads its standard input on a thread
// of its own, and a line runs between ticks -- where the dev server's commands
// run -- so a command that changes the world is an input like a player's.
// The engine's commands are here; a game's are the scripts' (`ConsoleService`),
// parsed here by the description they registered and run by the script
// runtime at the start of the next tick.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/core/types.h"
#include "engine/replication/types.h"
#include "engine/scene/world.h"

namespace engine::replication {
class IReplication;
} // namespace engine::replication

namespace engine::app {

// **Lines from standard input**, read on a thread of its own. A process with
// no input -- a service, a pipe closed at its far end -- has a console with
// nobody at it, and is otherwise the same. The thread is let go at exit: a read
// blocked on a terminal cannot be woken in any portable way, and the process
// ending ends it.
class ConsoleInput
{
public:
    ConsoleInput() = default;
    ~ConsoleInput();
    ConsoleInput(const ConsoleInput&) = delete;
    ConsoleInput& operator=(const ConsoleInput&) = delete;

    void start();
    // Every line read since the last call, oldest first.
    void take(std::vector<std::string>& out);
    // The input ended: nobody, or nothing, will write another line.
    [[nodiscard]] bool ended() const;

private:
    struct Shared
    {
        std::mutex mutex;
        std::deque<std::string> lines;
        std::atomic<bool> ended{false};
        // Asked to stop, and stopped.
        std::atomic<bool> stopping{false};
        std::atomic<bool> stopped{false};
    };
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    bool m_started = false;
    std::thread m_thread;
};

// **Who a server refuses** (`<saves>/bans.json`, ADR 0218): by key -- `key:`
// and the player's `PublicKey` -- or by account, `account:` and the id a game
// gave in `OnJoinRequest` (ADR 0226). Never by name, which a player picks, and
// never by address, which a household or a carrier shares.
class BanList
{
public:
    struct Ban
    {
        std::string who;
        std::string reason;
        // Unix seconds it ends at; 0 for good.
        std::int64_t until = 0;
        // Who banned, and when.
        std::string by;
        std::int64_t at = 0;
    };

    // **One file may be several worlds' list** (ADR 0226 §5): the worlds a
    // manager runs share theirs, so a ban in one holds in every one. What
    // another world wrote is read again when the file changes, and a change
    // is made under a lock, on the file as it is then.
    explicit BanList(std::filesystem::path file);

    // The ban that holds `who` at `now`, or null.
    [[nodiscard]] const Ban* find(std::string_view who, std::int64_t now) const;
    void add(Ban ban);
    bool remove(std::string_view who);
    [[nodiscard]] const std::vector<Ban>& all() const;

private:
    // Read again when the file is not what was last read.
    void refresh() const;
    void load() const;
    void save() const;

    std::filesystem::path m_file;
    mutable std::vector<Ban> m_bans;
    mutable std::filesystem::file_time_type m_readAt{};
    mutable std::uintmax_t m_readSize = 0;
};

class ServerConsole
{
public:
    using Caller = scene::EngineState::ConsoleCaller;
    // What a command answered, to whoever ran it.
    using Reply = std::function<void(const std::string& text, bool failed)>;

    // What the commands act on, this frame.
    struct Host
    {
        scene::World* world = nullptr;
        core::InstanceId dataModel;
        // The match, while there is one.
        replication::IReplication* replication = nullptr;
        std::function<void()> flushSaves;
        core::u64 nowNs = 0;
        std::int64_t nowUnix = 0;
    };

    explicit ServerConsole(std::filesystem::path bansFile);

    // **One line**, from the console or a script. The engine's commands
    // answer at once; a game's answers when its function has finished, which
    // may be ticks later.
    void run(std::string_view line, const Caller& caller, Reply reply, const Host& host);
    // Once a frame, after the ticks: the game's answers, a script's lines,
    // and a `stop` counting down.
    void frame(const Host& host);
    // How long a tick took, for `status`.
    void noteTick(core::f64 ms);

    // **What `status` says, as numbers**: for the health endpoint.
    struct Health
    {
        core::u64 uptimeSeconds = 0;
        // Over the last minute of ticks: what a person reads.
        core::f64 tickMeanMs = 0.0;
        core::f64 tickWorstMs = 0.0;
        // Since the world began, and only growing: what a tool that samples
        // reads, as it reads the byte counts -- the mean between two of its
        // samples is the difference of the one over the difference of the
        // other.
        core::u64 ticksTotal = 0;
        core::f64 tickMsTotal = 0.0;
        core::usize players = 0;
        core::u32 maxPlayers = 0;
        core::u64 memoryBytes = 0;
        std::string scene;
        // Counting down to a stop, or closing: no longer taking anybody.
        bool stopping = false;
    };
    [[nodiscard]] Health health(const Host& host) const;

    // **Whether somebody may join** (the session's gate): refused while the
    // server stops, or when banned -- else nothing to say, and the game's own
    // gate decides.
    [[nodiscard]] std::optional<replication::JoinAnswer> refusal(const replication::JoinAsk& ask,
                                                                 std::int64_t nowUnix) const;
    // And for an account a game named (ADR 0226 §5): a ban by account.
    [[nodiscard]] std::optional<replication::JoinAnswer> accountRefusal(const std::string& account,
                                                                        std::int64_t nowUnix) const;
    // `stop` has done everything it does: the process ends, cleanly.
    [[nodiscard]] bool finished() const noexcept { return m_phase == Phase::Finished; }

private:
    enum class Phase : core::u8
    {
        Running,
        Counting,
        Closing,
        Finished,
    };

    struct Pending
    {
        Reply reply;
        std::string line;
        Caller caller;
    };

    void answer(const Reply& reply, std::string_view line, const Caller& caller, const std::string& text,
                bool failed) const;
    void runEngineCommand(std::string_view name, std::string_view rest, const Caller& caller, const Reply& reply,
                          std::string_view line, const Host& host);
    void announce(const Host& host, const std::string& text) const;
    void beginClosing(const Host& host);

    BanList m_bans;
    std::map<core::u32, Pending> m_pending;
    core::u32 m_nextCall = 1;
    std::deque<core::f64> m_ticks;
    core::u64 m_ticksTotal = 0;
    core::f64 m_tickMsTotal = 0.0;
    core::u64 m_startedNs = 0;
    Phase m_phase = Phase::Running;
    core::u64 m_stopAtNs = 0;
    core::u64 m_nextWarningNs = 0;
    core::u32 m_closingFrames = 0;
};

} // namespace engine::app
