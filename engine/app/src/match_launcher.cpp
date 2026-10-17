#include "engine/app/match_launcher.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "engine/app/manager_link.h"
#include "engine/core/i18n.h"

namespace engine::app {

std::vector<platform::WindowPlacement> MatchLauncher::tiles(const platform::WindowPlacement& area, int count)
{
    std::vector<platform::WindowPlacement> out;
    if (count <= 0 || area.width <= 0 || area.height <= 0)
        return out;
    const int columns = count == 1 ? 1 : 2;
    const int rows = count <= 2 ? 1 : 2;
    const int width = area.width / columns;
    const int height = area.height / rows;
    for (int index = 0; index < count; ++index) {
        platform::WindowPlacement tile;
        tile.x = area.x + (index % columns) * width;
        tile.y = area.y + (index / columns) * height;
        tile.width = width;
        tile.height = height;
        out.push_back(tile);
    }
    return out;
}

std::vector<MatchCommand> MatchLauncher::commands(const MatchPlan& plan)
{
    std::vector<MatchCommand> out;
    const int players = std::clamp(plan.players, 1, 4);
    const std::string port = std::to_string(plan.port);
    const std::string host = plan.host.string();
    const std::string project = plan.project.string();

    // The windows: every player's. A dedicated server has none.
    const std::vector<platform::WindowPlacement> placed = tiles(plan.area, players);
    const auto windowOf = [&](int player, std::vector<std::string>& arguments) {
        if (player < static_cast<int>(placed.size())) {
            const platform::WindowPlacement& tile = placed[static_cast<std::size_t>(player)];
            arguments.push_back("--window=" + std::to_string(tile.x) + "," + std::to_string(tile.y) + "," +
                                std::to_string(tile.width) + "," + std::to_string(tile.height));
        }
    };
    const auto logOf = [&](const std::string& file, std::vector<std::string>& arguments) {
        if (!plan.logDirectory.empty())
            arguments.push_back("--log-file=" + (plan.logDirectory / file).string());
    };
    const auto finish = [&](std::vector<std::string>& arguments) {
        arguments.insert(arguments.end(), plan.extraArguments.begin(), plan.extraArguments.end());
    };

    // **A game of several worlds** (ADR 0225): the manager, windowless, and
    // every player a client of its front door.
    if (!plan.servers.empty()) {
        MatchCommand manager;
        manager.name = core::tr(ENG_TR("engine.editor.match.manager"));
        manager.manager = true;
        manager.arguments = {host, "--manager=" + plan.servers.string(), "--manager-play=" + port,
                             "--manager-project=" + project, "--manager-root=" + plan.playRoot.string()};
        out.push_back(std::move(manager));
        for (int index = 1; index <= players; ++index) {
            MatchCommand client;
            client.name = core::tr(ENG_TR("engine.editor.match.client"), {{"index", static_cast<core::i64>(index)}});
            client.arguments = {host, project, "--join=127.0.0.1:" + port, "--label=" + client.name};
            logOf("client" + std::to_string(index) + ".log", client.arguments);
            windowOf(index - 1, client.arguments);
            finish(client.arguments);
            out.push_back(std::move(client));
        }
        return out;
    }

    int player = 0;
    MatchCommand authority;
    authority.name =
        core::tr(plan.dedicated ? ENG_TR("engine.editor.match.server") : ENG_TR("engine.editor.match.host"));
    authority.arguments = {host, project, (plan.dedicated ? "--serve=" : "--host=") + port,
                           "--label=" + authority.name};
    logOf(plan.dedicated ? "server.log" : "host.log", authority.arguments);
    if (!plan.dedicated)
        windowOf(player++, authority.arguments);
    finish(authority.arguments);
    out.push_back(std::move(authority));

    const int clients = plan.dedicated ? players : players - 1;
    for (int index = 1; index <= clients; ++index) {
        MatchCommand client;
        client.name = core::tr(ENG_TR("engine.editor.match.client"), {{"index", static_cast<core::i64>(index)}});
        client.arguments = {host, project, "--join=127.0.0.1:" + port, "--label=" + client.name};
        logOf("client" + std::to_string(index) + ".log", client.arguments);
        windowOf(player++, client.arguments);
        finish(client.arguments);
        out.push_back(std::move(client));
    }
    return out;
}

bool MatchLauncher::start(const MatchPlan& plan)
{
    stop();
    if (!plan.logDirectory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(plan.logDirectory, ec);
    }
    for (MatchCommand& command : commands(plan)) {
        platform::ProcessOptions options;
        options.pipeInput = command.manager;
        std::unique_ptr<platform::ChildProcess> process = platform::ChildProcess::start(command.arguments, options);
        if (process == nullptr) {
            // The authority is the match: without it there is nothing to join.
            if (m_members.empty())
                return false;
            continue;
        }
        m_members.push_back(Member{std::move(command.name), std::move(process), {}, command.manager});
    }
    return !m_members.empty();
}

std::vector<MatchLauncher::Line> MatchLauncher::poll()
{
    std::vector<Line> lines;
    for (Member& member : m_members) {
        member.partial += member.process->readAvailable();
        std::size_t start = 0;
        for (std::size_t end = member.partial.find('\n'); end != std::string::npos;
             end = member.partial.find('\n', start)) {
            std::string text = member.partial.substr(start, end - start);
            if (!text.empty() && text.back() == '\r')
                text.pop_back();
            start = end + 1;
            // The manager's messages: a world's line under the world's name,
            // and the worlds themselves for the panel.
            if (member.manager) {
                if (const std::optional<ManagerMessage> message = ManagerMessage::read(text); message.has_value()) {
                    const core::JsonValue root = message->root();
                    if (message->type() == "line") {
                        lines.push_back(
                            Line{std::string(root["world"].asString()), std::string(root["text"].asString())});
                    }
                    else if (message->type() == "worlds") {
                        m_worlds.clear();
                        const core::JsonValue list = root["worlds"];
                        for (core::usize at = 0; at < list.size(); ++at) {
                            const core::JsonValue one = list.at(at);
                            m_worlds.push_back(
                                World{std::string(one["name"].asString()), static_cast<int>(one["state"].asInteger(0)),
                                      static_cast<core::u16>(one["port"].asInteger(0)),
                                      static_cast<core::u32>(one["players"].asInteger(0)), one["tick"].asNumber(0.0),
                                      static_cast<core::u32>(one["restarts"].asInteger(0)),
                                      one["instance"].asBool(false), std::string(one["template"].asString())});
                        }
                    }
                    continue;
                }
            }
            lines.push_back(Line{member.name, std::move(text)});
        }
        member.partial.erase(0, start);
    }
    return lines;
}

bool MatchLauncher::managed() const noexcept
{
    return std::any_of(m_members.begin(), m_members.end(), [](const Member& member) { return member.manager; });
}

bool MatchLauncher::tellManager(std::string_view line)
{
    for (Member& member : m_members)
        if (member.manager)
            return member.process->write(std::string(line) + "\n");
    return false;
}

bool MatchLauncher::running()
{
    return std::any_of(m_members.begin(), m_members.end(), [](Member& member) { return member.process->running(); });
}

void MatchLauncher::stop()
{
    // **The manager is asked, not killed**: it stops its worlds, which a
    // kill would leave running with nobody to end them.
    for (Member& member : m_members) {
        if (member.manager && member.process->running()) {
            (void)member.process->write("stop\n");
            for (int waited = 0; waited < 100 && member.process->running(); ++waited)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        member.process->kill();
    }
    m_members.clear();
    m_worlds.clear();
}

} // namespace engine::app
