// The editor's Play with players (ADR 0106 §5): a match of separate processes.
//
// **Separate processes, not worlds in the editor.** A host or a dedicated
// server and that many clients of the project, each the real engine over the
// real network, tiled on the screen so every window can be watched at once.
// What each prints arrives here and goes to the Output panel under its own
// name; Stop ends them all.
//
// The plan is data and the commands are a pure function of it, so a test holds
// the command lines still; the processes are `platform::ChildProcess`, which a
// test also starts, headless.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/core/types.h"
#include "engine/platform/process.h"
#include "engine/platform/window.h"

namespace engine::app {

struct MatchPlan
{
    // The engine binary to run, and the project it runs.
    std::filesystem::path host;
    std::filesystem::path project;
    // How many players: 1 to 4. Without a dedicated server the first of them
    // is the host.
    int players = 2;
    bool dedicated = false;
    core::u16 port = 7777;
    // Where each process writes its log, one file each.
    std::filesystem::path logDirectory;
    // The screen area the windows are tiled across; empty leaves each window
    // where the engine would put it.
    platform::WindowPlacement area;
    // Appended to every command: a test runs the match `--headless`.
    std::vector<std::string> extraArguments;
};

struct MatchCommand
{
    // `Server`, `Host`, `Client 1`...: what the Output panel calls it.
    std::string name;
    std::vector<std::string> arguments;
};

class MatchLauncher
{
public:
    // The processes a plan starts, server first. Pure.
    [[nodiscard]] static std::vector<MatchCommand> commands(const MatchPlan& plan);
    // `count` windows over `area`: one fills it, two side by side, three or
    // four in a grid of two by two. Pure.
    [[nodiscard]] static std::vector<platform::WindowPlacement> tiles(const platform::WindowPlacement& area, int count);

    ~MatchLauncher() { stop(); }

    // Starts the plan's processes. False, with nothing left running, when the
    // first one would not start.
    [[nodiscard]] bool start(const MatchPlan& plan);

    struct Line
    {
        std::string process;
        std::string text;
    };
    // Every whole line the processes wrote since the last call, in the order
    // the processes are listed and then the order each wrote them.
    [[nodiscard]] std::vector<Line> poll();

    // Whether any of them is still running.
    [[nodiscard]] bool running();
    [[nodiscard]] core::usize size() const noexcept { return m_members.size(); }
    // Ends every one of them.
    void stop();

private:
    struct Member
    {
        std::string name;
        std::unique_ptr<platform::ChildProcess> process;
        std::string partial;
    };
    std::vector<Member> m_members;
};

} // namespace engine::app
