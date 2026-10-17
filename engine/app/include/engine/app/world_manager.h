// **The manager of an online game's worlds** (ADR 0220): `engine-host
// --manager servers.toml`.
//
// It starts each world as a process of the server's own binary, watches it by
// its heartbeat, restarts it when it falls -- within a limit, past which it is
// `Failed` -- keeps its log, and answers players at a front door that sends
// each to a world with a pass (ADR 0222). It speaks to its worlds over their
// pipes only (`manager_link.h`).
//
// Everything it acts on comes in through `Options`: the processes it starts,
// the transport its front door listens on and the clocks, so a test runs it
// with worlds of its own making.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/manager_link.h"
#include "engine/app/servers_config.h"
#include "engine/core/crypto.h"
#include "engine/core/error.h"
#include "engine/net/transport.h"

namespace engine::app {

// A world's process, as the manager sees it.
class WorldProcess
{
public:
    virtual ~WorldProcess() = default;
    virtual bool write(std::string_view text) = 0;
    // What it wrote since the last call.
    [[nodiscard]] virtual std::string read() = 0;
    [[nodiscard]] virtual bool running() = 0;
    virtual void kill() = 0;
    virtual void closeInput() = 0;
};

// What a world is started with.
struct WorldLaunch
{
    std::string name;
    std::string scene;
    core::u16 port = 0;
    core::u32 maxPlayers = 0;
    // The manager's public key, base64: the passes the world takes.
    std::string managerKey;
    std::filesystem::path saves;
    // Answers `/health` on its port number over TCP.
    bool health = false;
    // Slows its clock when overloaded (ADR 0233).
    bool timeDilation = false;
    // **Its own log file**, beside what the manager keeps of its output:
    // every world runs in the game's folder, and one `engine.log` there for
    // all of them was several processes writing one file.
    std::filesystem::path logFile;
};

using WorldLauncher = std::function<std::unique_ptr<WorldProcess>(const WorldLaunch&)>;

// **The engine's own world process**: `engine` run on the project, serving.
[[nodiscard]] WorldLauncher engineWorldLauncher(std::filesystem::path engine, std::filesystem::path project);

class WorldManager
{
public:
    enum class State : core::u8
    {
        Starting,
        Ready,
        Draining,
        Stopped,
        Failed,
    };

    struct Options
    {
        ServersConfig config;
        // Where `logs`, the keys and the worlds' saves are.
        std::filesystem::path root;
        std::string projectId;
        // What the front door's handshakes bind; the game's own
        // (`matchPrologue(projectId)`) when absent.
        std::optional<std::vector<core::u8>> prologue;
        WorldLauncher launcher;
        // **The worlds of another build** (`update <folder>`, ADR 0220 §7):
        // the launcher for the build in that folder, or none when the folder
        // holds no game -- which refuses the update before anything moves.
        // Absent, there is no update.
        std::function<WorldLauncher(const std::filesystem::path& folder)> launcherFor;
        // The front door's transport; null opens none.
        std::function<std::unique_ptr<net::ITransport>()> transports;
        std::function<core::u64()> clock;
        std::function<std::int64_t()> unixClock;
        // What the manager says, a line at a time: its standard output.
        std::function<void(const std::string&)> print;
        // **Each line a world writes**, as it is kept in its log: for the
        // editor, which shows them as they come (ADR 0225).
        std::function<void(const std::string& world, std::string_view line)> worldLine;
        // At most this many worlds and instances at once, 0 for no limit
        // beyond each template's: the editor's eight (ADR 0225 §3).
        core::u32 mostWorlds = 0;
    };

    // One row of `worlds`.
    struct WorldView
    {
        std::string name;
        State state = State::Starting;
        core::u16 port = 0;
        core::u32 players = 0;
        core::f64 tickMs = 0.0;
        core::u32 restarts = 0;
        // An instance, and the template it was made from.
        bool instance = false;
        std::string templateName;
    };

    explicit WorldManager(Options options);
    ~WorldManager();

    WorldManager(const WorldManager&) = delete;
    WorldManager& operator=(const WorldManager&) = delete;

    // Keys, the front door, and every world started.
    [[nodiscard]] std::optional<core::EngineError> start();
    // One pass: the door, each world's output, its health, the travels.
    void frame();
    // **A line of the manager's own console** (ADR 0220 §8), and its answer.
    [[nodiscard]] std::string run(std::string_view line);
    // Every world stopped and gone: the process ends.
    [[nodiscard]] bool finished() const noexcept { return m_finished; }
    // Asked to stop from outside (Ctrl+C): as `stop` with no minutes.
    void stop();

    [[nodiscard]] std::vector<WorldView> worlds() const;
    [[nodiscard]] const std::array<core::u8, 32>& publicKey() const noexcept { return m_signing.pub; }

private:
    struct World
    {
        ServersConfig::World config;
        State state = State::Starting;
        core::u16 port = 0;
        std::unique_ptr<WorldProcess> process;
        std::string partial;
        core::u64 startedNs = 0;
        core::u64 lastHeartbeatNs = 0;
        core::u32 players = 0;
        core::f64 tickMs = 0.0;
        std::deque<core::u64> restartsNs;
        core::u32 restarts = 0;
        // Who is in it, by key, with their account.
        std::map<std::string, std::string> present;
        bool stopAsked = false;
        // Its own public key, hexadecimal, as it said when ready.
        std::string key;
        // The protocol it speaks, as it said when ready.
        core::u32 protocol = 0;
        // **An update** (§7): the world of the new build taking this one's
        // place, while this one drains into it; and when it began to.
        World* successor = nullptr;
        bool replacing = false;
        core::u64 drainedSinceNs = 0;
        // Gone for good once stopped: an old build's world after an update.
        bool retired = false;
        // The topics it hears (ADR 0223).
        std::set<std::string> topics;
        // **Who is on the way to it**, by key, with when their pass runs out:
        // given a pass, not yet in -- so not in what its heartbeat counts.
        // Each leaves the list when they arrive, or when the pass could no
        // longer be used. Cleared by a heartbeat, a door that sent forty in a
        // second sent the next ones to the room the forty were taking.
        std::map<std::string, core::u64> expected;
        [[nodiscard]] core::u32 incoming() const noexcept { return static_cast<core::u32>(expected.size()); }
        // **How many it holds and will**: who is in, as their arrivals and
        // departures say at once -- a heartbeat's count is a second old --
        // and who is on the way.
        [[nodiscard]] core::u32 held() const noexcept
        {
            return std::max(players, static_cast<core::u32>(present.size())) + incoming();
        }

        // --- A channel (ADR 0230) ---------------------------------------
        // The `[[world]]` it is a channel of, and which; empty and 0 for a
        // world without channels.
        std::string base;
        core::u32 channel = 0;
        // Since when it has been empty, or under a quarter full; and when a
        // merge it was warned of moves its players.
        core::u64 emptySinceChannelNs = 0;
        core::u64 lowSinceNs = 0;
        core::u64 mergeAtNs = 0;

        // --- An instance (ADR 0221) -------------------------------------
        bool instance = false;
        std::string templateName;
        // Started ahead and given to nobody yet.
        bool warm = false;
        // Who may enter, by key.
        std::set<std::string> allowed;
        // When it was last seen empty, and how long it is kept open anyway.
        core::u64 emptySinceNs = 0;
        core::u64 keepOpenUntilNs = 0;
        // Told it closes, and when it is stopped.
        core::u64 closingStopNs = 0;
        // Where each player came from, for a crash to send them back.
        std::map<std::string, std::string> origins;
        // The world that asked for it, waiting for it to be ready; and the one
        // that made it, told when it is gone.
        World* asker = nullptr;
        World* creator = nullptr;
        core::u32 askedId = 0;
        std::vector<std::string> askedKeys;
        std::string askedData;
    };

    struct Door;

    void launch(World& world);
    void readWorld(World& world);
    void handle(World& world, const ManagerMessage& message);
    void health(World& world);
    void log(const World& world, std::string_view line);
    [[nodiscard]] core::u16 freePort() const;
    [[nodiscard]] World* worldNamed(std::string_view name);
    [[nodiscard]] std::string pass(std::string_view player, std::string_view origin, std::string_view target,
                                   std::string_view data, std::string_view ticket, std::string_view reason = {});
    void send(World& world, const core::JsonWriter& message);
    void update(const std::filesystem::path& folder);
    // An update whose new build would not start or speaks another protocol:
    // what starts next is the old build's again.
    void updateFailed();
    void moveAll(World& from, World& to);
    void createInstance(World& asker, const ManagerMessage& message);
    void allocate(World& instance);
    World& startInstance(const ServersConfig::Instance& config, bool warm);
    void topUpWarm();
    void instances();
    void reply(World& asker, core::u32 id, std::string_view instance, std::string_view why);
    // ADR 0230: the channels opened, closed and merged; the one a player
    // going to `base` is sent to; where a world's files are named.
    void channels();
    World& startChannel(const ServersConfig::World& config, core::u32 channel);
    [[nodiscard]] World* channelFor(std::string_view base, core::u32 travellers, const World* except = nullptr);
    [[nodiscard]] World* routed(std::string_view target, core::u32 travellers);
    void closed(World& instance, std::string_view reason);
    void say(std::string_view text) const;

    Options m_options;
    core::Ed25519Pair m_signing;
    std::vector<std::unique_ptr<World>> m_worlds;
    // The launcher before an update, for one that fails.
    WorldLauncher m_launcherBefore;
    std::unique_ptr<Door> m_door;
    core::u32 m_nextInstance = 1;
    // **Players a fallen instance sends back** (ADR 0221 §5), by key: the world
    // they came from, until the front door has sent them there.
    struct Return
    {
        std::string origin;
        core::u64 untilNs = 0;
    };
    std::map<std::string, Return> m_returns;
    // Players of a live instance, by key: one who drops comes back to it.
    std::map<std::string, std::string> m_members;
    // **Who left a world, by key** (ADR 0232 §3): one who comes back within
    // the grace goes back to it without waiting in line.
    std::map<std::string, Return> m_recent;
    // ADR 0232: the room the game has for one more, at `to`; and the line.
    [[nodiscard]] bool roomAt(const World* to) const;
    void expect(World& to, std::string_view key);
    void line();
    bool m_stopping = false;
    bool m_finished = false;
    core::u64 m_stopDeadlineNs = 0;
};

// A state as the console says it.
[[nodiscard]] core::TextKey stateKey(WorldManager::State state);

} // namespace engine::app
