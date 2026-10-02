#include "engine/app/script_sides.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <span>

#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/toml.h"
#include "engine/platform/file.h"

namespace engine::app {

namespace {

struct Word
{
    std::string_view text;
    core::u32 line = 0;
    core::u32 column = 0;
};

[[nodiscard]] bool wordStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

[[nodiscard]] bool wordPart(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The level of a long bracket opening at `at` (`[[` is 0, `[==[` is 2), or
// nullopt when there is none there.
[[nodiscard]] std::optional<core::usize> longBracket(std::string_view text, core::usize at)
{
    if (at >= text.size() || text[at] != '[')
        return std::nullopt;
    core::usize level = 0;
    core::usize cursor = at + 1;
    while (cursor < text.size() && text[cursor] == '=') {
        ++level;
        ++cursor;
    }
    if (cursor < text.size() && text[cursor] == '[')
        return level;
    return std::nullopt;
}

// **Every word of `source` outside its comments**, with where it is. Words
// inside strings count: `GetService("UIService")` names the service in one.
std::vector<Word> wordsOf(std::string_view source)
{
    std::vector<Word> words;
    core::u32 line = 0;
    core::usize lineStart = 0;
    const auto advanceLines = [&](core::usize from, core::usize to) {
        for (core::usize at = from; at < to && at < source.size(); ++at) {
            if (source[at] == '\n') {
                ++line;
                lineStart = at + 1;
            }
        }
    };
    core::usize at = 0;
    while (at < source.size()) {
        const char c = source[at];
        if (c == '-' && at + 1 < source.size() && source[at + 1] == '-') {
            // A comment: long to its closing bracket, or to the end of the line.
            if (const std::optional<core::usize> level = longBracket(source, at + 2); level.has_value()) {
                const std::string close = "]" + std::string(*level, '=') + "]";
                const core::usize end = source.find(close, at + 2);
                const core::usize stop = end == std::string_view::npos ? source.size() : end + close.size();
                advanceLines(at, stop);
                at = stop;
            }
            else {
                const core::usize end = source.find('\n', at);
                at = end == std::string_view::npos ? source.size() : end;
            }
            continue;
        }
        if (c == '\n') {
            ++line;
            lineStart = at + 1;
            ++at;
            continue;
        }
        if (wordStart(c)) {
            const core::usize begin = at;
            while (at < source.size() && wordPart(source[at]))
                ++at;
            words.push_back(Word{source.substr(begin, at - begin), line, static_cast<core::u32>(begin - lineStart)});
            continue;
        }
        ++at;
    }
    return words;
}

[[nodiscard]] bool oneOf(std::string_view word, std::span<const std::string_view> list)
{
    return std::find(list.begin(), list.end(), word) != list.end();
}

} // namespace

std::vector<SideFinding> lintScriptSide(std::string_view source, script::ScriptSide side, bool decidedByService,
                                        bool multiplayer)
{
    std::vector<SideFinding> findings;
    // **A game with no multiplayer has one side**: solo is the server and the
    // player at once, and every one of these works there.
    if (!multiplayer)
        return findings;
    const std::vector<Word> words = wordsOf(source);
    constexpr std::array<std::string_view, 2> ServerOnly{"ServerStorage", "ServerScriptService"};
    constexpr std::array<std::string_view, 5> PlayerOnly{"CurrentCamera", "UIService", "InputService",
                                                         "GraphicsService", "LocalizationService"};
    for (const Word& word : words) {
        if (side == script::ScriptSide::Client && oneOf(word.text, ServerOnly)) {
            findings.push_back(SideFinding{word.line, word.column, static_cast<core::u32>(word.text.size()),
                                           ENG_TR("script.warn.side_client_reads_server"), std::string(word.text)});
        }
        else if (side == script::ScriptSide::Server && oneOf(word.text, PlayerOnly)) {
            findings.push_back(SideFinding{word.line, word.column, static_cast<core::u32>(word.text.size()),
                                           ENG_TR("script.warn.side_server_touches_player"), std::string(word.text)});
        }
    }
    if (side == script::ScriptSide::Anywhere && !decidedByService) {
        const bool asks = std::any_of(words.begin(), words.end(), [](const Word& w) { return w.text == "Authority"; });
        if (!asks)
            findings.push_back(SideFinding{0, 0, 0, ENG_TR("script.warn.side_shared_never_asks"), {}});
    }
    return findings;
}

bool projectIsMultiplayer(const std::filesystem::path& root)
{
    std::string text;
    core::TomlDocument document;
    if (root.empty() || !platform::readTextFile(root / "project.toml", text) || !document.parse(text).ok)
        return false;
    const std::string_view mode = document.string("export.multiplayer").value_or("none");
    return mode != "none";
}

namespace {

struct Checker
{
    std::filesystem::path root;
    bool multiplayer = false;
    core::usize count = 0;

    void report(std::string_view where, const SideFinding& finding)
    {
        const core::I18nArg wordArg[] = {{"word", std::string_view{finding.word}}};
        const std::string message = core::engineCatalog().format(finding.key, wordArg);
        const core::I18nArg args[] = {{"where", where},
                                      {"line", static_cast<core::i64>(finding.line + 1)},
                                      {"column", static_cast<core::i64>(finding.column + 1)},
                                      {"message", std::string_view{message}}};
        core::log(core::LogLevel::Warn, ENG_TR("engine.cli.side_warning"), args);
        ++count;
    }

    void lint(std::string_view where, std::string_view source, script::ScriptSide side, bool decided)
    {
        // A packaged scene's code is bytecode, and there is nothing to read.
        if (source.starts_with("luauc:"))
            return;
        for (const SideFinding& finding : lintScriptSide(source, side, decided, multiplayer))
            report(where, finding);
    }

    // `decided` is the side the tree above decided, if any.
    void walk(const core::JsonValue node, const std::string& where, std::optional<script::ScriptSide> decided)
    {
        if (node.type() != core::JsonType::Object)
            return;
        const std::string name(node["name"].asString());
        const std::string here = where.empty() || where.ends_with("> ") ? where + name : where + "." + name;
        if (node["class"].asString() == "Script") {
            const core::JsonValue properties = node["properties"];
            const std::string_view context = properties["RunContext"].asString("Shared");
            const script::ScriptSide own = context == "Server"   ? script::ScriptSide::Server
                                           : context == "Client" ? script::ScriptSide::Client
                                                                 : script::ScriptSide::Anywhere;
            lint(here, properties["Source"].asString(), decided.value_or(own), decided.has_value());
        }
        const core::JsonValue children = node["children"];
        for (core::usize index = 0; index < children.size(); ++index)
            walk(children.at(index), here, decided);
    }

    void file(const std::filesystem::path& path, bool global)
    {
        std::string text;
        core::JsonDocument document;
        if (!platform::readTextFile(path, text) || !document.parse(text, path.generic_string()).ok)
            return;
        const std::string where = std::filesystem::relative(path, root).generic_string() + " > ";
        const core::JsonValue top = document.root();
        if (global) {
            // `global.json`: the `Server` and `Client` folders decide.
            const core::JsonValue children = top["root"]["children"];
            for (core::usize index = 0; index < children.size(); ++index) {
                const core::JsonValue folder = children.at(index);
                const std::string_view name = folder["name"].asString();
                const std::optional<script::ScriptSide> side =
                    name == "Server"   ? std::optional(script::ScriptSide::Server)
                    : name == "Client" ? std::optional(script::ScriptSide::Client)
                                       : std::nullopt;
                walk(folder, where + "GlobalScriptService", side);
            }
            return;
        }
        walk(top["root"], where, std::nullopt);
        const core::JsonValue storage = top["storage"];
        for (core::usize index = 0; index < storage.size(); ++index) {
            const std::string_view service = storage.keyAt(index);
            // Storage never runs a script (ADR 0137 §3); the load warns.
            if (service == "ServerStorage" || service == "ReplicatedStorage")
                continue;
            const std::optional<script::ScriptSide> side =
                service == "ServerScriptService" ? std::optional(script::ScriptSide::Server)
                : service == "ClientScriptService" || service == "ScriptService"
                    ? std::optional(script::ScriptSide::Client)
                    : std::nullopt;
            walk(storage[service], where, side);
        }
    }

    void sources(const std::filesystem::path& folder, script::ScriptSide side)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec))
            return;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".luau")
                continue;
            std::string text;
            if (platform::readTextFile(it->path(), text))
                lint(std::filesystem::relative(it->path(), root).generic_string(), text, side, true);
        }
    }
};

} // namespace

core::usize checkProjectSides(const std::filesystem::path& root)
{
    Checker checker{root, projectIsMultiplayer(root), 0};
    const std::filesystem::path content = root / "content";
    std::error_code ec;
    if (std::filesystem::is_directory(content, ec)) {
        std::vector<std::filesystem::path> files;
        for (auto it = std::filesystem::recursive_directory_iterator(content, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (it->is_regular_file(ec) && (name.ends_with(".scene.json") || name.ends_with(".stamp.json")))
                files.push_back(it->path());
        }
        // In a fixed order, so two runs print the same thing.
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& file : files)
            checker.file(file, false);
        checker.file(content / "global.json", true);
    }
    const std::filesystem::path src = root / "src";
    checker.sources(src / "server", script::ScriptSide::Server);
    checker.sources(src / "client", script::ScriptSide::Client);
    checker.sources(src / "scripts", script::ScriptSide::Client);
    if (std::filesystem::is_directory(src / "scenes", ec)) {
        for (const auto& scene : std::filesystem::directory_iterator(src / "scenes", ec)) {
            checker.sources(scene.path() / "server", script::ScriptSide::Server);
            checker.sources(scene.path() / "client", script::ScriptSide::Client);
        }
    }
    return checker.count;
}

} // namespace engine::app
