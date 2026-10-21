// `--check-scripts`: the script pane's diagnostics with no pane. See
// script_check.h.
#include "engine/app/script_check.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

#include "engine/app/script_complete.h"
#include "engine/app/script_sides.h"
#include "engine/core/i18n.h"
#include "engine/scene/world.h"
#include "engine/script/bytecode.h"
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

namespace {

// **A compiler's refusal, as a diagnostic**: its own form is `:LINE:
// message`, and `where` says which compile refused -- the runtime's, an
// exported game's, or both.
[[nodiscard]] Diagnostic compileRefusal(std::string_view refusal, core::TextKey where)
{
    core::u32 line = 1;
    std::string_view words = refusal;
    if (words.starts_with(':')) {
        const std::size_t end = words.find(": ", 1);
        if (end != std::string_view::npos) {
            core::u32 read = 0;
            bool number = end > 1;
            for (std::size_t at = 1; at < end; ++at) {
                if (words[at] < '0' || words[at] > '9') {
                    number = false;
                    break;
                }
                read = read * 10 + static_cast<core::u32>(words[at] - '0');
            }
            if (number && read > 0) {
                line = read;
                words.remove_prefix(end + 2);
            }
        }
    }
    const std::string whereText = core::engineCatalog().format(where);
    const core::I18nArg args[] = {{"message", words}, {"where", std::string_view{whereText}}};
    return Diagnostic{
        .at = Position{line - 1, 0},
        .length = 0,
        .message = core::engineCatalog().format(ENG_TR("engine.check.err.compile"), args),
        .severity = Severity::Error,
    };
}

} // namespace

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
        // **What only the compiler refuses**: more than two hundred locals in
        // a function, upvalues, registers, constants. The parser and the type
        // checker take such a script; the compiler does not, and a lobby
        // module failed only once it was run. **Compiled twice**: as the
        // runtime compiles what it loads in development (`bytecodeOf`) and as
        // a package compiles what it ships (`compileForPackage`), whose
        // options fold a local that holds a constant away -- so a script one
        // takes the other can refuse, and the check names which. Said at the
        // line the compiler names, unless the parser has already said why
        // the script is not Luau.
        const bool unparsed = std::any_of(document.diagnostics().begin(), document.diagnostics().end(),
                                          [](const Diagnostic& diagnostic) { return diagnostic.syntax; });
        if (!unparsed) {
            std::string inDevelopment;
            if (std::string bytecode; !script::bytecodeOf(node.source, node.path, bytecode).has_value() &&
                                      !bytecode.empty() && bytecode.front() == '\0')
                inDevelopment = bytecode.substr(1);
            std::string inExport;
            if (std::string bytecode; !script::compileForPackage(node.source, bytecode, inExport)) {
                // A build with no compiler says so with its key; that is not
                // the script's fault.
                if (inExport.starts_with('['))
                    inExport.clear();
            }
            std::vector<Diagnostic> compiled;
            if (!inDevelopment.empty() && inDevelopment == inExport)
                compiled.push_back(compileRefusal(inDevelopment, ENG_TR("engine.check.compile.everywhere")));
            else {
                if (!inDevelopment.empty())
                    compiled.push_back(compileRefusal(inDevelopment, ENG_TR("engine.check.compile.development")));
                if (!inExport.empty())
                    compiled.push_back(compileRefusal(inExport, ENG_TR("engine.check.compile.export")));
            }
            document.appendDiagnostics(compiled);
        }

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
