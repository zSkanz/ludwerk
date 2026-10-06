#pragma once

// `engine-host <project> --check-scripts`: every script of a project, checked
// as the editor's script pane checks the one a tab shows (ADR 0093).
//
// **Why it exists.** `ludwerk check` and the script pane were two checkers with
// two configurations, and a project the first called clean opened in the
// second as a page of errors -- found by the owner on a game of a hundred
// files, hours before it shipped. This is the pane's checker with no pane: the
// same snapshot of the tree, the same definitions, the same document parse and
// lint, over every script instead of the one being typed in. `ludwerk check`
// runs it, so what the command says is what the editor shows, and a gate test
// runs it over a project whose requires are by path.
//
// Built only where the editor is (`ENG_DEBUG_UI`), like the checker itself.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/language_service.h"
#include "engine/app/script_document.h"
#include "engine/core/id.h"
#include "engine/core/types.h"

namespace engine::scene {
class World;
}

namespace engine::app {

struct ScriptProblem
{
    // The script's file, relative to the project, or its place in the tree
    // (`game.Workspace.Door.Open`) for one a scene carries.
    std::string script;
    Position at;
    std::string message;
    Severity severity = Severity::Error;
};

struct ScriptCheckReport
{
    core::usize scripts = 0;
    // In the tree's order, then down each script.
    std::vector<ScriptProblem> problems;
    // Why the engine's definitions did not load, when they did not.
    std::string loadError;

    [[nodiscard]] core::usize errors() const noexcept;
};

// **The half of a tab's diagnostics that needs the tree and not the checker**
// (decision 10 of the script pane, and ADR 0138 section 8): dot access to a
// child that is not there, and a script reaching for the other side's
// services. Appended to `document`, whose own parse is already in it. One
// function, called by the pane and by `checkScripts`, so the two cannot part.
// `sides` off leaves the second out, for a caller that has it said elsewhere.
void appendTreeDiagnostics(ScriptDocument& document, const scene::World& world, core::InstanceId root,
                           core::InstanceId script, const std::filesystem::path& projectRoot, bool sides = true);

struct ScriptCheckOptions
{
    // The text of `engine.d.luau`.
    std::string_view definitions;
    // The project's folder, the file each script was mounted from, its
    // aliases and the engine's own modules: see `LanguageTree`.
    LanguageFiles files;
    // The warnings about where a script runs (ADR 0138 section 8). Off for
    // `--check-scripts`: `ludwerk check` has `--check-sides` say them, which
    // reads a stamp's scripts too -- and said twice is said worse.
    bool sides = true;
};

// Checks every script under `dataModel`: each one's parse, its tree lints and
// the type checker's answer -- what a tab holding it would show.
[[nodiscard]] ScriptCheckReport checkScripts(const scene::World& world, core::InstanceId dataModel,
                                             const ScriptCheckOptions& options);

// One line a problem is printed as: `src/client/hud.luau(12,5): error: ...`,
// which an editor's terminal turns into a link.
[[nodiscard]] std::string formatProblem(const ScriptProblem& problem);

} // namespace engine::app
