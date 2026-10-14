// Luau's flags as a project setting (ADR 0217): one set of values, read the
// same way by the runtime's compiler, the editor's checker and the tools that
// check a project's scripts.
#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/language_service.h"
#include "engine/app/luau_flags.h"
#include "engine/app/project_config.h"
#include "engine/app/replay.h"
#include "engine/app/script_document.h"
#include "engine/platform/file.h"
#include "engine/script/bytecode.h"
#include "inspector_fixture.h"

using namespace engine;
using app::LuauFlagSetting;

namespace {

// Every flag back to the engine's own when the test ends, passed or not: a
// flag is process-wide, and the next test must not run with this one's.
struct EngineFlagsAfter
{
    EngineFlagsAfter() = default;
    EngineFlagsAfter(const EngineFlagsAfter&) = delete;
    EngineFlagsAfter& operator=(const EngineFlagsAfter&) = delete;
    ~EngineFlagsAfter() { (void)app::applyLuauFlags({}); }
};

[[nodiscard]] std::string definitions()
{
    const std::filesystem::path path =
        std::filesystem::path(ENG_TEST_CATALOG).parent_path().parent_path() / "runtime" / "types" / "engine.d.luau";
    std::string text;
    REQUIRE(platform::readTextFile(path, text));
    return text;
}

[[nodiscard]] std::string toolValue(std::string_view name)
{
    for (const auto& [flag, value] : app::luauToolFlags()) {
        if (flag == name)
            return value;
    }
    return {};
}

// What each reader says of one script: whether the runtime compiles it,
// whether the editor's tab has a problem with it, and whether the checker
// does.
struct Verdicts
{
    bool compiles = false;
    bool tabClean = false;
    bool checkerClean = false;
};

[[nodiscard]] Verdicts readersOf(const std::string& source)
{
    Verdicts out;
    std::string bytecode;
    // A compile error comes back as bytecode of version nought, carrying the
    // message `luau_load` then reports.
    out.compiles =
        !script::bytecodeOf(source, "=IfLocal", bytecode).has_value() && !bytecode.empty() && bytecode.front() != 0;

    app::ScriptDocument document(source);
    document.refreshDiagnostics();
    out.tabClean = document.diagnostics().empty();

    app::LanguageCore core(definitions());
    app::LanguageTree tree;
    app::LanguageTree::Node game;
    game.name = "game";
    game.className = "DataModel";
    game.path = "game";
    tree.nodes.push_back(game);
    app::LanguageTree::Node script;
    script.name = "IfLocal";
    script.className = "Script";
    script.parent = 0;
    script.path = "game.IfLocal";
    script.script = true;
    script.source = source;
    tree.nodes[0].children.push_back(1);
    tree.nodes.push_back(script);
    core.update(tree);
    out.checkerClean = core.check("game.IfLocal").diagnostics.empty();
    return out;
}

} // namespace

TEST_CASE("a language feature a project turns on is the same to the runtime, the editor and the checker")
{
    // **Off, everything refuses it; on, everything takes it** -- and what
    // `ludwerk check` hands luau-lsp says the same, so a script VS Code
    // calls clean is one the game runs.
    const EngineFlagsAfter reset;
    const std::string source = "--!strict\n"
                               "local function find(): number?\n"
                               "    return 1\n"
                               "end\n"
                               "if local found = find() then\n"
                               "    print(found + 1)\n"
                               "end\n";

    REQUIRE(app::applyLuauFlags({}).empty());
    const Verdicts off = readersOf(source);
    CHECK_FALSE(off.compiles);
    CHECK_FALSE(off.tabClean);
    CHECK_FALSE(off.checkerClean);
    CHECK(toolValue("LuauExperimentalIfLocalSyntax") == "false");
    // The solver is the engine's (R2): never handed to a tool as a parser's.
    CHECK(toolValue("LuauSolverV2").empty());
    CHECK(app::luauFlagsFingerprint() == 0);

    const std::vector<LuauFlagSetting> ifLocal{{"LuauExperimentalIfLocalSyntax", 1, true},
                                               {"LuauExperimentalIfLocalAnalysis", 1, true}};
    REQUIRE(app::applyLuauFlags(ifLocal).empty());
    const Verdicts on = readersOf(source);
    CHECK(on.compiles);
    CHECK(on.tabClean);
    CHECK(on.checkerClean);
    CHECK(toolValue("LuauExperimentalIfLocalSyntax") == "true");
    CHECK(app::luauFlagsFingerprint() != 0);

    // And taken back, everything refuses it again.
    REQUIRE(app::applyLuauFlags({}).empty());
    CHECK_FALSE(readersOf(source).compiles);
}

TEST_CASE("a project's flags are read from [luau.flags], and what cannot be taken is said and left")
{
    const EngineFlagsAfter reset;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "engine-luau-flags-config";
    std::filesystem::create_directories(root);
    {
        std::ofstream file(root / "project.toml", std::ios::binary);
        file << "[project]\nname = \"Flags\"\n\n[luau.flags]\nLuauExperimentalIfLocalSyntax = true\n"
                "LuauTypeInferIterationLimit = 4000\nLuauNoSuchFlagAnywhere = true\nDebugLuauTimeTracing = true\n";
    }
    const app::ProjectConfig config = app::loadProjectConfig(root, app::GraphicsOverrides{});
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    REQUIRE(config.luauFlags.size() == 4);
    CHECK(config.luauFlags[0] == LuauFlagSetting{"LuauExperimentalIfLocalSyntax", 1, true});
    CHECK(config.luauFlags[1] == LuauFlagSetting{"LuauTypeInferIterationLimit", 4000, false});

    // A flag this Luau does not have and a debugging one are said, and the
    // rest is taken.
    const std::vector<std::string> problems = app::applyLuauFlags(config.luauFlags);
    CHECK(problems.size() == 2);
    CHECK(toolValue("LuauExperimentalIfLocalSyntax") == "true");
    CHECK(toolValue("LuauTypeInferIterationLimit") == "4000");

    // The wrong kind is said too, and changes nothing.
    const std::vector<LuauFlagSetting> wrong{{"LuauTypeInferIterationLimit", 1, true}};
    CHECK(app::applyLuauFlags(wrong).size() == 1);
    CHECK(toolValue("LuauTypeInferIterationLimit").empty());
}

TEST_CASE("Project Settings writes what differs from the default, and takes out what no longer does")
{
    const EngineFlagsAfter reset;
    REQUIRE(app::applyLuauFlags({}).empty());
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "engine-luau-flags-write";
    std::filesystem::create_directories(root);
    {
        std::ofstream file(root / "project.toml", std::ios::binary);
        file << "[project]\nname = \"Flags\"\n\n[luau.flags]\nLuauExportValueSyntax = true\n"
                "LuauGoneAfterAnUpgrade = true\n";
    }
    std::vector<app::LuauFlagInfo> flags = app::listLuauFlags();
    std::map<std::string, core::i64, std::less<>> written{{"LuauExportValueSyntax", 1}, {"LuauGoneAfterAnUpgrade", 1}};
    for (app::LuauFlagInfo& flag : flags) {
        // `if local` turned on, the exported values put back to the default,
        // and one number changed.
        if (flag.name == "LuauExperimentalIfLocalSyntax")
            flag.value = 1;
        if (flag.name == "LuauTypeInferIterationLimit")
            flag.value = flag.engineDefault + 1;
    }
    const app::LuauFlagsWritten done = app::writeLuauFlagSettings(root, flags, written);
    CHECK(done.ok);
    CHECK(done.changed);
    CHECK(done.settings.size() == 2);

    const app::ProjectConfig config = app::loadProjectConfig(root, app::GraphicsOverrides{});
    const auto said = [&config](std::string_view name) {
        return std::any_of(config.luauFlags.begin(), config.luauFlags.end(),
                           [name](const LuauFlagSetting& setting) { return setting.name == name; });
    };
    CHECK(said("LuauExperimentalIfLocalSyntax"));
    CHECK(said("LuauTypeInferIterationLimit"));
    CHECK_FALSE(said("LuauExportValueSyntax"));
    // The file's own, which this Luau does not have, is left as it was.
    CHECK(said("LuauGoneAfterAnUpgrade"));

    // Written again with nothing changed: nothing changes.
    written.clear();
    for (const LuauFlagSetting& setting : done.settings)
        written[setting.name] = setting.value;
    CHECK_FALSE(app::writeLuauFlagSettings(root, flags, written).changed);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("only what compiled code can tell moves the fingerprint, and the editor hides nothing it may set")
{
    const EngineFlagsAfter reset;
    REQUIRE(app::applyLuauFlags({}).empty());
    CHECK(app::luauFlagsFingerprint() == 0);

    // The checker's own flags change what the editor says, not what a game
    // runs: two machines that differ in one still play the same match.
    const std::vector<app::LuauFlagInfo> flags = app::listLuauFlags();
    const auto checker = std::find_if(flags.begin(), flags.end(), [](const app::LuauFlagInfo& flag) {
        return flag.area == app::LuauFlagArea::Analysis && !flag.number && !flag.debug;
    });
    REQUIRE(checker != flags.end());
    const std::vector<LuauFlagSetting> analysisOnly{{checker->name, checker->engineDefault == 0 ? 1 : 0, true}};
    REQUIRE(app::applyLuauFlags(analysisOnly).empty());
    CHECK(app::luauFlagsFingerprint() == 0);

    // A parser's does, and is the same number for the same change.
    const std::vector<LuauFlagSetting> parser{{"LuauExperimentalIfLocalSyntax", 1, true}};
    REQUIRE(app::applyLuauFlags(parser).empty());
    const core::u64 once = app::luauFlagsFingerprint();
    REQUIRE(app::applyLuauFlags(parser).empty());
    CHECK(once != 0);
    CHECK(app::luauFlagsFingerprint() == once);

    // Every library has flags here, and each is filed under its own.
    const auto has = [&flags](app::LuauFlagArea area) {
        return std::any_of(flags.begin(), flags.end(),
                           [area](const app::LuauFlagInfo& flag) { return flag.area == area; });
    };
    CHECK(has(app::LuauFlagArea::Parser));
    CHECK(has(app::LuauFlagArea::Compiler));
    CHECK(has(app::LuauFlagArea::Vm));
    CHECK(has(app::LuauFlagArea::Analysis));
}

TEST_CASE("VS Code's settings are told the flags in place, and the rest of the file is as it was")
{
    // Replaced where it is: the comment and the other keys untouched.
    const std::string_view written = "{\n"
                                     "    // the platform\n"
                                     "    \"luau-lsp.platform.type\": \"standard\",\n"
                                     "    \"luau-lsp.fflags.override\": {\n"
                                     "        \"LuauSolverV2\": \"true\"\n"
                                     "    },\n"
                                     "    \"files.encoding\": \"utf8\"\n"
                                     "}\n";
    const std::optional<std::string> replaced =
        app::withSettingsKey(written, "luau-lsp.fflags.override", "{ \"LuauX\": \"true\" }");
    REQUIRE(replaced.has_value());
    CHECK(*replaced == "{\n"
                       "    // the platform\n"
                       "    \"luau-lsp.platform.type\": \"standard\",\n"
                       "    \"luau-lsp.fflags.override\": { \"LuauX\": \"true\" },\n"
                       "    \"files.encoding\": \"utf8\"\n"
                       "}\n");

    // A scalar replaced, the last member included.
    const std::optional<std::string> scalar = app::withSettingsKey(written, "files.encoding", "\"utf16\"");
    REQUIRE(scalar.has_value());
    CHECK(scalar->find("\"files.encoding\": \"utf16\"\n}") != std::string::npos);

    // Added where it is not, after a comma.
    const std::optional<std::string> added =
        app::withSettingsKey("{\n    \"a\": 1\n}\n", "luau-lsp.fflags.enableByDefault", "true");
    REQUIRE(added.has_value());
    CHECK(*added == "{\n    \"a\": 1,\n    \"luau-lsp.fflags.enableByDefault\": true\n}\n");
    const std::optional<std::string> first = app::withSettingsKey("{}", "k", "1");
    REQUIRE(first.has_value());
    CHECK(*first == "{\n    \"k\": 1\n}");

    // Nothing to put it in is said, not guessed at.
    CHECK_FALSE(app::withSettingsKey("[1, 2]", "k", "1").has_value());
    CHECK_FALSE(app::withSettingsKey("{ \"a\": ", "k", "1").has_value());
}

TEST_CASE("a trace recorded with the engine's flags is the bytes it was, and one with other flags says so")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "engine-luau-flags-trace.txt";
    app::ReplayTrace plain;
    plain.checkpoints.push_back({1, 0xABCDu});
    REQUIRE_FALSE(app::writeTrace(path, plain).has_value());
    std::string text;
    REQUIRE(platform::readTextFile(path, text));
    CHECK(text == "1 000000000000abcd\n");

    app::ReplayTrace flagged = plain;
    flagged.luauFlags = 0x1234u;
    REQUIRE_FALSE(app::writeTrace(path, flagged).has_value());
    app::ReplayTrace read;
    REQUIRE_FALSE(app::readTrace(path, read).has_value());
    CHECK(read.luauFlags == 0x1234u);
    REQUIRE(read.checkpoints.size() == 1);
    CHECK(read.checkpoints[0].hash == 0xABCDu);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
