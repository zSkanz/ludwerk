// **Luau's flags as a project setting** (ADR 0217).
//
// Luau ships each change to its language, its compiler and its VM behind a
// flag, off until the change has been through Luau's own release train. A
// project turns one on in `project.toml`:
//
//     [luau.flags]
//     LuauExperimentalIfLocalSyntax = true
//
// and the same values reach every place a script is read: the runtime's
// compiler and VM, the editor's checker, hot reload, `--check-scripts`,
// `ludwerk check` (which passes them to luau-lsp), and VS Code (through the
// project's `.vscode/settings.json`). A flag is process-wide, so they are
// applied once, before the first script is compiled, and again when the
// editor's Project Settings change them.
//
// **What the engine runs with when the project says nothing** is Luau's own
// default for each flag, except the type checker's, which are on as Luau's
// own analyser runs them (ADR 0093, amended 2026-10-06) -- less the few Luau
// calls experimental.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/text_key.h"
#include "engine/core/types.h"

namespace engine::app {

// The library that reads a flag, which decides what the engine may do with it.
enum class LuauFlagArea : core::u8
{
    // What a script parses as: a project's syntax.
    Parser,
    // What a script compiles to.
    Compiler,
    // How compiled code runs.
    Vm,
    // Native code generation; read only where the VM generates any.
    CodeGen,
    // The type checker: the editor's diagnostics and nothing a game runs.
    Analysis,
    // Configuration and `require` resolution, and what Luau has nowhere else.
    Other,
};

// One flag, as the editor shows it.
struct LuauFlagInfo
{
    std::string name;
    LuauFlagArea area = LuauFlagArea::Other;
    // A number rather than a switch.
    bool number = false;
    // Luau's own instrumentation (`Debug*`), or the solver's switch, which R2
    // decides: never set by a project.
    bool debug = false;
    // On Luau's own list of checker flags that are not final.
    bool experimental = false;
    // What the engine runs with when the project says nothing, and now.
    core::i64 engineDefault = 0;
    core::i64 value = 0;
};

// One `[luau.flags]` line of `project.toml`.
struct LuauFlagSetting
{
    std::string name;
    core::i64 value = 0;
    bool boolean = true;

    [[nodiscard]] bool operator==(const LuauFlagSetting&) const = default;
};

// **A flag that changes what a script compiles to or how it runs** -- the
// parser's, the compiler's, the VM's, code generation's and `require`'s.
// Every machine in a match must agree on these, and a replay recorded with
// other values is not the same program.
[[nodiscard]] bool changesCompiledCode(LuauFlagArea area) noexcept;

// Every flag this build of the engine carries, by name.
[[nodiscard]] std::vector<LuauFlagInfo> listLuauFlags();

// **Every flag to the engine's default, then the project's on top.** Returns
// a line for each setting that was not taken, already in the reader's
// language: a flag this Luau does not have (one that graduated, or a typo), a
// debugging flag, or a value of the wrong kind.
std::vector<std::string> applyLuauFlags(std::span<const LuauFlagSetting> settings);

// The engine's defaults, once, if nothing has applied them yet: what the
// editor's checker and a test that makes one call before reading any flag.
void ensureLuauFlags();

// **What the project changed that compiled code can tell**, as one number:
// nought when it changed nothing. Two machines in a match, and a replay and
// the run checking it, must agree on it.
[[nodiscard]] core::u64 luauFlagsFingerprint();

// **The values a tool that checks scripts must be told**, `Name` and `true` or
// a number: every parser flag at what the engine runs it with -- luau-lsp
// turns every flag on unless told, and a syntax the runtime refuses would
// pass it -- and every other flag the project changed.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> luauToolFlags();

// **A language feature people ask for by its name**, behind the flags that
// make it: what Project Settings > Luau offers before every flag.
struct LuauFeature
{
    core::TextKey name;
    core::TextKey description;
    std::span<const std::string_view> flags;
};
[[nodiscard]] std::span<const LuauFeature> luauFeatures() noexcept;

// **`.vscode/settings.json` told the same**: `luau-lsp.fflags.override` set
// to `luauToolFlags` and the new solver, the rest of the file as it was --
// comments, order and spacing. Nothing is written where the file is absent;
// false, with nothing written, where it cannot be read as a settings file.
bool writeVsCodeLuauFlags(const std::filesystem::path& projectRoot);

// The text of a settings file with `key` set to `rendered` at its top level --
// replaced where it is, added before the closing brace where it is not.
// Nothing where the text has no top-level object to put it in.
[[nodiscard]] std::optional<std::string> withSettingsKey(std::string_view settings, std::string_view key,
                                                         std::string_view rendered);

} // namespace engine::app
