#include "engine/app/match_launcher.h"

#include <algorithm>

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
        std::unique_ptr<platform::ChildProcess> process = platform::ChildProcess::start(command.arguments);
        if (process == nullptr) {
            // The authority is the match: without it there is nothing to join.
            if (m_members.empty())
                return false;
            continue;
        }
        m_members.push_back(Member{std::move(command.name), std::move(process), {}});
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
            lines.push_back(Line{member.name, std::move(text)});
            start = end + 1;
        }
        member.partial.erase(0, start);
    }
    return lines;
}

bool MatchLauncher::running()
{
    return std::any_of(m_members.begin(), m_members.end(), [](Member& member) { return member.process->running(); });
}

void MatchLauncher::stop()
{
    for (Member& member : m_members)
        member.process->kill();
    m_members.clear();
}

} // namespace engine::app
