#include "engine/app/manager_main.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <thread>

#include "engine/app/admin_channel.h"
#include "engine/app/admin_server.h"
#include "engine/app/manager_link.h"
#include "engine/app/match_keys.h"
#include "engine/app/project_config.h"
#include "engine/app/server_console.h"
#include "engine/app/servers_config.h"
#include "engine/app/world_manager.h"
#include "engine/core/i18n.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/net/transport.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/platform/stop_signal.h"

namespace engine::app {

namespace {

// **The game a folder holds**: a project beside `servers.toml`, or a package's
// `game/` beside its binary.
[[nodiscard]] std::filesystem::path projectIn(const std::filesystem::path& folder)
{
    std::error_code ec;
    return std::filesystem::exists(folder / "project.toml", ec) ? folder : folder / "game";
}

} // namespace

int runManager(const std::filesystem::path& serversFile, const std::filesystem::path& engine, const ManagerPlay& play)
{
    const bool playing = play.port != 0;
    const std::filesystem::path root = playing                         ? play.root
                                       : serversFile.has_parent_path() ? serversFile.parent_path()
                                                                       : std::filesystem::current_path();
    const std::filesystem::path project = playing ? play.project : projectIn(root);
    std::string text;
    if (!platform::readTextFile(serversFile, text)) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", serversFile.string()}};
        core::log(core::LogLevel::Error, ENG_TR("manager.err.servers_missing"), args);
        return 2;
    }
    std::optional<core::EngineError> error;
    std::optional<ServersConfig> config = readServersConfig(text, serversFile.string(), error);
    if (!config.has_value()) {
        core::logText(core::LogLevel::Error, error.has_value() ? error->message : std::string{});
        return 2;
    }
    // **Tried in the editor** (ADR 0225): this machine's address, ports from
    // the editor's, nothing listening for administrators or probes.
    if (playing) {
        config->publicAddress = "127.0.0.1";
        config->port = play.port;
        config->firstWorldPort = static_cast<core::u16>(play.port + 1);
        config->lastWorldPort = static_cast<core::u16>(play.port + 16);
        config->admin = false;
        config->health = false;
    }
    // The game it runs: the project beside `servers.toml`, by its id.
    const std::string projectId = loadProjectConfig(project, GraphicsOverrides{}).id;
    (void)core::openLogFile(root / config->logs / "manager.log");

    WorldManager::Options options;
    options.config = *config;
    options.root = root;
    options.projectId = projectId;
    options.launcher = engineWorldLauncher(engine, project);
    // Another build: its own binary, beside its own game -- or, for a folder
    // with no binary of its own, this one's on the game the folder holds: the
    // editor's Restart with this build, which changed scripts and not the engine.
    options.launcherFor = [engine](const std::filesystem::path& folder) -> WorldLauncher {
        std::error_code ec;
        if (!std::filesystem::exists(projectIn(folder) / "project.toml", ec))
            return {};
        const std::filesystem::path own = folder / engine.filename();
        return engineWorldLauncher(std::filesystem::exists(own, ec) ? own : engine, projectIn(folder));
    };
    if (playing) {
        options.mostWorlds = 8;
        options.worldLine = [](const std::string& world, std::string_view line) {
            core::JsonWriter message(core::JsonLayout::Compact);
            message.beginObject();
            message.field("type", "line");
            message.field("world", std::string_view{world});
            message.field("text", line);
            message.endObject();
            const std::string said = managerLine(message);
            std::fwrite(said.data(), 1, said.size(), stdout);
            std::fflush(stdout);
        };
    }
    options.trialAccounts = playing;
    options.exitAtTransaction = play.exitAtTransaction;
    options.exitAfterCommit = play.exitAfterCommit;
    options.transports = []() { return net::createEnetTransport(); };
    options.clock = []() { return platform::nowNs(); };
    options.unixClock = []() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    // **Who asked a world, in order** (ADR 0219 with ADR 0220 §8): a line
    // sent to a world is answered later, when its pipe says so, and that
    // answer goes back to whoever sent it -- an administrator's session, or 0
    // for the operator at this terminal.
    AdminServer adminServer;
    std::map<std::string, std::deque<core::u32>, std::less<>> asked;
    options.print = [&adminServer, &asked](const std::string& line) {
        std::fprintf(stdout, "%s\n", line.c_str());
        std::fflush(stdout);
        const std::size_t colon = line.find(": ");
        if (colon == std::string::npos)
            return;
        const auto waiting = asked.find(std::string_view{line}.substr(0, colon));
        if (waiting == asked.end() || waiting->second.empty())
            return;
        const core::u32 session = waiting->second.front();
        waiting->second.pop_front();
        if (session != 0)
            adminServer.answer(session, line, false);
    };
    WorldManager manager(std::move(options));
    if (std::optional<core::EngineError> started = manager.start(); started.has_value()) {
        core::logText(core::LogLevel::Error, started->message);
        return 1;
    }
    const std::array<core::I18nArg, 2> args{core::I18nArg{"worlds", static_cast<core::i64>(config->worlds.size())},
                                            core::I18nArg{"path", serversFile.string()}};
    core::log(core::LogLevel::Info, ENG_TR("manager.info.running"), args);

    // **Administered from another machine** (ADR 0219): the manager's own
    // key beside its saves, and `admins.toml` beside `servers.toml`.
    platform::FileLock adminKeyHeld;
    if (config->admin) {
        AdminServer::Options admin;
        admin.port = config->adminPort;
        admin.allow = config->adminAllow;
        admin.serverKey = loadOrMakeMatchKey(root / "keys" / "manager-admin.key", adminKeyHeld);
        admin.prologue = adminPrologue();
        const std::filesystem::path list = root / "admins.toml";
        admin.adminsSource = list.string();
        if (std::string listed; platform::readTextFile(list, listed)) {
            if (platform::readableByOthers(list)) {
                const std::array<core::I18nArg, 1> where{core::I18nArg{"path", list.string()}};
                core::log(core::LogLevel::Error, ENG_TR("admin.err.admins_readable"), where);
                return 2;
            }
            std::optional<core::EngineError> listError;
            admin.admins = readAdmins(listed, list.string(), listError);
            if (listError.has_value()) {
                core::logText(core::LogLevel::Error, listError->message);
                return 2;
            }
        }
        if (std::optional<core::EngineError> adminError = adminServer.start(std::move(admin)); adminError.has_value()) {
            core::logText(core::LogLevel::Error, adminError->message);
            return 1;
        }
    }
    // A line for one world is answered when the world says so; any other,
    // at once.
    const auto forWorld = [&manager](std::string_view line) -> std::optional<std::string> {
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || line.find(' ') < colon)
            return std::nullopt;
        for (const WorldManager::WorldView& view : manager.worlds())
            if (view.name == line.substr(0, colon))
                return view.name;
        return std::nullopt;
    };

    // **Its own console** (ADR 0220 §8): the operator's lines, between passes.
    ConsoleInput input;
    input.start();
    platform::installStopSignals();
    // **`worlds --watch`**: the table again each second, until the next
    // line the operator types -- a live view with no terminal interface.
    bool watching = false;
    core::u64 nextWatchNs = 0;
    while (!manager.finished()) {
        std::vector<std::string> lines;
        input.take(lines);
        for (const std::string& line : lines) {
            if (watching) {
                watching = false;
                continue;
            }
            if (line.starts_with("worlds --watch"))
                watching = true;
            if (const std::optional<std::string> world = forWorld(line); world.has_value())
                asked[*world].push_back(0);
            if (const std::string answer = manager.run(line); !answer.empty()) {
                std::fprintf(stdout, "%s\n", answer.c_str());
                std::fflush(stdout);
            }
        }
        // An administrator's lines: the whole manager is level 3's alone,
        // since a world's line runs there as its operator's.
        std::vector<AdminServer::Line> administered;
        adminServer.take(administered);
        for (const AdminServer::Line& line : administered) {
            if (line.caller.permission < 3) {
                adminServer.answer(line.session, core::engineCatalog().format(ENG_TR("admin.err.manager_level")), true);
                continue;
            }
            const std::optional<std::string> world = forWorld(line.line);
            if (world.has_value())
                asked[*world].push_back(line.session);
            const std::string answer = manager.run(line.line);
            const std::string who = line.caller.name + " (" + line.caller.key + ")";
            const std::array<core::I18nArg, 3> ran{core::I18nArg{"who", std::string_view{who}},
                                                   core::I18nArg{"line", std::string_view{line.line}},
                                                   core::I18nArg{"answer", std::string_view{answer}}};
            core::log(core::LogLevel::Info, ENG_TR("console.info.ran"), ran);
            if (!world.has_value() || !answer.empty()) {
                if (world.has_value())
                    asked[*world].pop_back();
                adminServer.answer(line.session, answer, false);
            }
        }
        // **The editor's panel**, each second: every world and instance.
        if (playing && platform::nowNs() >= nextWatchNs) {
            nextWatchNs = platform::nowNs() + 1'000'000'000ull;
            core::JsonWriter message(core::JsonLayout::Compact);
            message.beginObject();
            message.field("type", "worlds");
            message.key("worlds");
            message.beginArray();
            for (const WorldManager::WorldView& view : manager.worlds()) {
                message.beginObject();
                message.field("name", std::string_view{view.name});
                message.field("state", static_cast<core::i64>(view.state));
                message.field("port", static_cast<core::i64>(view.port));
                message.field("players", static_cast<core::i64>(view.players));
                message.field("tick", view.tickMs);
                message.field("restarts", static_cast<core::i64>(view.restarts));
                message.field("instance", view.instance);
                message.field("template", std::string_view{view.templateName});
                message.endObject();
            }
            message.endArray();
            message.endObject();
            const std::string said = managerLine(message);
            std::fwrite(said.data(), 1, said.size(), stdout);
            std::fflush(stdout);
        }
        if (watching && platform::nowNs() >= nextWatchNs) {
            nextWatchNs = platform::nowNs() + 1'000'000'000ull;
            // The screen cleared first, where the terminal reads the sequence.
            std::fprintf(stdout, "\x1b[2J\x1b[H%s\n", manager.run("worlds").c_str());
            std::fflush(stdout);
        }
        if (platform::stopRequested())
            manager.stop();
        manager.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}

} // namespace engine::app
