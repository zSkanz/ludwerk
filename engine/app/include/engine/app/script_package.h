#pragma once

// **An exported game carries bytecode, not source** (ADR 0112).
//
// `ludwerk build` runs the host with `--compile-scripts <game>` after laying
// the game out: every script under `game/src/` becomes its `.luauc` beside it,
// compiled by this engine's own compiler with the options it runs scripts
// with, and the source is removed. In the host rather than the command-line
// tool because the compiler, its options and the deterministic-math patch are
// the engine's, and a second compiler would be a second answer.

#include <filesystem>
#include <string>

#include "engine/core/types.h"

namespace engine::app {

struct ScriptPackageReport
{
    core::u32 compiled = 0;
    // The first script that did not compile, project-relative, and why.
    std::string failed;
    std::string message;
};

// False, with `report.failed` named, when a script does not compile or a file
// cannot be written; the scripts before it are already compiled.
[[nodiscard]] bool compileGameScripts(const std::filesystem::path& game, ScriptPackageReport& report);

} // namespace engine::app
