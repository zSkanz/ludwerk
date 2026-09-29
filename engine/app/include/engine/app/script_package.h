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

// **The scripts a scene, a stamp or `global.json` carries, compiled too**
// (S0.3, ADR 0138 §5): every `"Source"` string in every `*.scene.json`,
// `*.stamp.json` and `global.json` under `content` becomes
// `scene::CompiledSourcePrefix` and its bytecode in base64, which the scene
// reader turns back into the bytecode a script loads. `ludwerk build` runs it on
// the STAGED content before the pack is made, so neither the pack nor the folder
// carries a scene script's text. A file it changes is removed before it is
// written: the staged copy is hard links, and the project's own file must never
// be written through one. False, with `report.failed` named, as above.
[[nodiscard]] bool compileContentScripts(const std::filesystem::path& content, ScriptPackageReport& report);

} // namespace engine::app
