// `--check-scripts`: the script pane's diagnostics with no pane. See
// script_check.h.
#include "engine/app/script_check.h"

#include <algorithm>

#include "engine/app/script_complete.h"
#include "engine/app/script_sides.h"
#include "engine/core/i18n.h"
#include "engine/scene/world.h"
#include "engine/script/modules.h"

namespace engine::app {

core::usize ScriptCheckReport::errors() const noexcept
{
    return static_cast<core::usize>(std::count_if(problems.begin(), problems.end(), [](const ScriptProblem& problem) {
        return problem.severity == Severity::Error;
    }));
}

void appendTreeDiagnostics(ScriptDocument& document, const scene::World& world, core::InstanceId root,
                           core::InstanceId script, const std::filesystem::path& projectRoot, bool sides)
{
    // `refreshDiagnostics` parses TEXT and knows nothing about a world; dot
    // access to a live child can only be told from a plain table field by
    // resolving the path, so it is a second pass that lands in the same list.
    std::vector<Diagnostic> reached;
    lintInstanceAccess(document, world.classes(), world.atoms(), CompletionWorld{&world, root, script}, reached);
    // **Where it runs** (ADR 0138 section 8): a client script reaching for the
    // server's storage, a server script for a player's camera, a script for
    // both that never asks which one it is on.
    if (sides && world.alive(script) &&
        world.classOf(script) == world.classes().findId(world.atoms().lookup("Script"))) {
        const bool decided = script::serviceSideOf(world, script).has_value();
        const std::string text = document.text();
        for (const SideFinding& finding :
             lintScriptSide(text, script::scriptSideOf(world, script), decided, projectIsMultiplayer(projectRoot))) {
            const core::I18nArg args[] = {{"word", std::string_view{finding.word}}};
            reached.push_back(Diagnostic{
                .at = Position{finding.line, finding.column},
                .length = finding.length,
                .message = core::engineCatalog().format(finding.key, args),
                .severity = Severity::Warning,
            });
        }
    }
    document.appendDiagnostics(reached);
}

ScriptCheckReport checkScripts(const scene::World& world, core::InstanceId dataModel, const ScriptCheckOptions& options)
{
    ScriptCheckReport report;
    const LanguageTree tree = captureLanguageTree(world, dataModel, &options.files);
    LanguageCore core(options.definitions);
    report.loadError = core.loadError();
    core.update(tree);

    for (const LanguageTree::Node& node : tree.nodes) {
        if (!node.script)
            continue;
        ++report.scripts;
        // What a tab does when its text comes to rest: the document's own
        // parse, the lints that need the tree, then the checker's answer.
        ScriptDocument document(node.source);
        document.refreshDiagnostics();
        appendTreeDiagnostics(document, world, dataModel, node.id, options.files.projectRoot, options.sides);
        const LanguageCheck checked = core.check(node.path);
        document.appendDiagnostics(checked.diagnostics);

        std::vector<Diagnostic> found(document.diagnostics().begin(), document.diagnostics().end());
        std::stable_sort(found.begin(), found.end(), [](const Diagnostic& a, const Diagnostic& b) {
            return a.at.line != b.at.line ? a.at.line < b.at.line : a.at.column < b.at.column;
        });
        for (Diagnostic& diagnostic : found) {
            report.problems.push_back(ScriptProblem{
                .script = node.file.empty() ? node.path : node.file,
                .at = diagnostic.at,
                .message = std::move(diagnostic.message),
                .severity = diagnostic.severity,
            });
        }
    }
    return report;
}

std::string formatProblem(const ScriptProblem& problem)
{
    std::string line = problem.script;
    line += '(';
    line += std::to_string(problem.at.line + 1);
    line += ',';
    line += std::to_string(problem.at.column + 1);
    line += "): ";
    // The two words an editor's terminal reads a severity by, which is why
    // they are not the catalog's: they are a format, as the parentheses are.
    line += problem.severity == Severity::Error ? "error" : "warning";
    line += ": ";
    // One line each: Luau's own messages run over several.
    for (const char c : problem.message)
        line += (c == '\n' || c == '\r') ? ' ' : c;
    return line;
}

} // namespace engine::app
