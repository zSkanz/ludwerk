// `engine-host --manager=servers.toml` (ADR 0220): the manager's whole run,
// with no world of its own and no window.
#pragma once

#include <filesystem>

#include "engine/core/types.h"

namespace engine::app {

// **A game's worlds tried in the editor** (ADR 0225): the manager on this
// machine's own address, its front door at `port` and its worlds on the ports
// after it, at most eight at once, its logs and saves in `root`, the game in
// `project` -- and what it knows said on its standard output as messages the
// editor reads: the worlds each second, and each line a world writes.
struct ManagerPlay
{
    core::u16 port = 0;
    std::filesystem::path project;
    std::filesystem::path root;
    // **A test's kill** (`--exit-at-transaction=N`, `--exit-after-commit`),
    // in a build with the test hooks: see `WorldManager::Options`.
    core::u32 exitAtTransaction = 0;
    bool exitAfterCommit = false;
};

// The manager of `serversFile`, starting its worlds with `engine`; the
// process's exit code.
[[nodiscard]] int runManager(const std::filesystem::path& serversFile, const std::filesystem::path& engine,
                             const ManagerPlay& play = {});

} // namespace engine::app
