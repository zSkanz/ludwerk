#include "engine/app/luau_flags.h"

#include <Luau/Common.h>
#include <Luau/ExperimentalFlags.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>

#include "engine/core/i18n.h"
#include "engine/platform/file.h"
#include "luau_flags.gen.h"

namespace engine::app {
namespace {

[[nodiscard]] bool listed(std::span<const std::string_view> names, std::string_view name) noexcept
{
    return !name.empty() && std::binary_search(names.begin(), names.end(), name);
}

// The generated lists end in an empty placeholder, which sorts first; the
// rest are in order.
[[nodiscard]] std::span<const std::string_view> namesOf(std::span<const std::string_view> generated) noexcept
{
    return generated.first(generated.size() - 1);
}

[[nodiscard]] LuauFlagArea areaOf(std::string_view name) noexcept
{
    if (listed(namesOf(LuauParserFlags), name))
        return LuauFlagArea::Parser;
    if (listed(namesOf(LuauCompilerFlags), name))
        return LuauFlagArea::Compiler;
    if (listed(namesOf(LuauVmFlags), name))
        return LuauFlagArea::Vm;
    if (listed(namesOf(LuauCodeGenFlags), name))
        return LuauFlagArea::CodeGen;
    if (listed(namesOf(LuauAnalysisFlags), name))
        return LuauFlagArea::Analysis;
    return LuauFlagArea::Other;
}

[[nodiscard]] bool isDebug(std::string_view name) noexcept
{
    return name.starts_with("Debug");
}

// **Which solver checks a script is the engine's, not a project's** (R2):
// every script is checked by the new one. Its flag is defined beside the
// parser's, so it is named here, never printed for a tool -- which is told
// `LuauSolverV2=true` on its own -- and refused from a project.
constexpr std::string_view SolverFlag = "LuauSolverV2";

// **Luau's own default for each flag**, read before anything changed one: the
// value each `FValue` was constructed with. Taken on the first call, which
// every path that sets a flag makes first.
struct Shipped
{
    std::map<std::string, bool, std::less<>> switches;
    std::map<std::string, int, std::less<>> numbers;
};

[[nodiscard]] const Shipped& shipped()
{
    static const Shipped values = [] {
        Shipped out;
        for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr; flag = flag->next)
            out.switches.emplace(flag->name, flag->value);
        for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag != nullptr; flag = flag->next)
            out.numbers.emplace(flag->name, flag->value);
        return out;
    }();
    return values;
}

// **The engine's default**: Luau's, except the checker's own switches, on as
// Luau's analyser runs them, less the ones Luau calls experimental.
[[nodiscard]] bool engineDefaultSwitch(std::string_view name, bool luauDefault)
{
    if (areaOf(name) == LuauFlagArea::Analysis && name.starts_with("Luau") &&
        !Luau::isAnalysisFlagExperimental(std::string(name).c_str()))
        return true;
    return luauDefault;
}

std::mutex& stateMutex()
{
    static std::mutex mutex;
    return mutex;
}

bool& applied()
{
    static bool once = false;
    return once;
}

void resetToEngineDefaults()
{
    const Shipped& luau = shipped();
    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr; flag = flag->next) {
        const auto found = luau.switches.find(std::string_view(flag->name));
        flag->value = engineDefaultSwitch(flag->name, found != luau.switches.end() && found->second);
    }
    for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag != nullptr; flag = flag->next) {
        const auto found = luau.numbers.find(std::string_view(flag->name));
        if (found != luau.numbers.end())
            flag->value = found->second;
    }
}

// The features Project Settings > Luau names before every flag. A feature is
// on when all its flags are. Exact tables are not a flag at 0.742; each Luau
// upgrade looks for the features it carries (the upgrade checklist, ADR 0217).
constexpr std::array<std::string_view, 2> IfLocalFlags{"LuauExperimentalIfLocalSyntax",
                                                       "LuauExperimentalIfLocalAnalysis"};
constexpr std::array<std::string_view, 1> ExportValueFlags{"LuauExportValueSyntax"};

const std::array<LuauFeature, 2> Features{{
    {ENG_TR("engine.luau_flags.feature.if_local"), ENG_TR("engine.luau_flags.feature.if_local_tip"), IfLocalFlags},
    {ENG_TR("engine.luau_flags.feature.export_values"), ENG_TR("engine.luau_flags.feature.export_values_tip"),
     ExportValueFlags},
}};

[[nodiscard]] std::string rendered(const LuauFlagInfo& flag)
{
    if (!flag.number)
        return flag.value != 0 ? "true" : "false";
    return std::to_string(flag.value);
}

} // namespace

bool changesCompiledCode(LuauFlagArea area) noexcept
{
    return area != LuauFlagArea::Analysis;
}

std::vector<LuauFlagInfo> listLuauFlags()
{
    const std::lock_guard lock(stateMutex());
    const Shipped& luau = shipped();
    std::vector<LuauFlagInfo> flags;
    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr; flag = flag->next) {
        LuauFlagInfo info;
        info.name = flag->name;
        info.area = areaOf(info.name);
        info.debug = isDebug(info.name) || info.name == SolverFlag;
        info.experimental = Luau::isAnalysisFlagExperimental(flag->name);
        const auto found = luau.switches.find(std::string_view(flag->name));
        info.engineDefault = engineDefaultSwitch(info.name, found != luau.switches.end() && found->second) ? 1 : 0;
        info.value = flag->value ? 1 : 0;
        flags.push_back(std::move(info));
    }
    for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag != nullptr; flag = flag->next) {
        LuauFlagInfo info;
        info.name = flag->name;
        info.area = areaOf(info.name);
        info.number = true;
        info.debug = isDebug(info.name);
        const auto found = luau.numbers.find(std::string_view(flag->name));
        info.engineDefault = found != luau.numbers.end() ? found->second : flag->value;
        info.value = flag->value;
        flags.push_back(std::move(info));
    }
    std::sort(flags.begin(), flags.end(), [](const LuauFlagInfo& a, const LuauFlagInfo& b) { return a.name < b.name; });
    return flags;
}

std::vector<std::string> applyLuauFlags(std::span<const LuauFlagSetting> settings)
{
    const std::lock_guard lock(stateMutex());
    (void)shipped();
    resetToEngineDefaults();
    applied() = true;

    std::vector<std::string> problems;
    for (const LuauFlagSetting& setting : settings) {
        const std::array<core::I18nArg, 1> named{core::I18nArg{"name", setting.name}};
        if (isDebug(setting.name)) {
            problems.push_back(core::engineCatalog().format(ENG_TR("engine.luau_flags.err.debug"), named));
            continue;
        }
        if (setting.name == SolverFlag) {
            problems.push_back(core::engineCatalog().format(ENG_TR("engine.luau_flags.err.solver"), named));
            continue;
        }
        bool found = false;
        for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr && !found; flag = flag->next) {
            if (setting.name != flag->name)
                continue;
            found = true;
            if (!setting.boolean)
                problems.push_back(core::engineCatalog().format(ENG_TR("engine.luau_flags.err.not_a_switch"), named));
            else
                flag->value = setting.value != 0;
        }
        for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag != nullptr && !found; flag = flag->next) {
            if (setting.name != flag->name)
                continue;
            found = true;
            if (setting.boolean)
                problems.push_back(core::engineCatalog().format(ENG_TR("engine.luau_flags.err.not_a_number"), named));
            else
                flag->value = static_cast<int>(std::clamp<core::i64>(setting.value, std::numeric_limits<int>::min(),
                                                                     std::numeric_limits<int>::max()));
        }
        if (!found)
            problems.push_back(core::engineCatalog().format(ENG_TR("engine.luau_flags.err.unknown"), named));
    }
    return problems;
}

void ensureLuauFlags()
{
    {
        const std::lock_guard lock(stateMutex());
        if (applied())
            return;
    }
    (void)applyLuauFlags({});
}

core::u64 luauFlagsFingerprint()
{
    // FNV-1a over `name=value;` of each, in name order: the same set of
    // changes is the same number on every machine and every platform.
    core::u64 hash = 0xCBF29CE484222325ull;
    bool any = false;
    for (const LuauFlagInfo& flag : listLuauFlags()) {
        if (flag.debug || !changesCompiledCode(flag.area) || flag.value == flag.engineDefault)
            continue;
        any = true;
        const std::string line = flag.name + "=" + rendered(flag) + ";";
        for (const char c : line) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 0x100000001B3ull;
        }
    }
    return any ? hash : 0;
}

std::vector<std::pair<std::string, std::string>> luauToolFlags()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const LuauFlagInfo& flag : listLuauFlags()) {
        if (flag.debug || flag.name == SolverFlag)
            continue;
        if ((flag.area == LuauFlagArea::Parser && !flag.number) || flag.value != flag.engineDefault)
            out.emplace_back(flag.name, rendered(flag));
    }
    return out;
}

std::span<const LuauFeature> luauFeatures() noexcept
{
    return Features;
}

std::optional<std::string> withSettingsKey(std::string_view settings, std::string_view key, std::string_view rendered)
{
    // A scan that knows strings and both kinds of comment -- a VS Code
    // settings file may have either -- and finds the top-level object's
    // members by depth. Everything it does not replace stays as written.
    struct Member
    {
        std::size_t keyBegin = 0;
        std::size_t valueBegin = 0;
        std::size_t valueEnd = 0;
    };
    std::optional<Member> found;
    std::size_t rootOpen = std::string_view::npos;
    std::size_t rootClose = std::string_view::npos;
    std::size_t lastValueEnd = std::string_view::npos;
    int depth = 0;
    std::size_t at = 0;
    const std::size_t size = settings.size();
    // At depth one, the member being read: where its key began, and whether
    // the colon has passed (so the next thing is its value).
    std::optional<std::size_t> memberKey;
    std::string memberName;
    bool inValue = false;
    std::size_t valueBegin = 0;
    const auto endValue = [&](std::size_t end) {
        if (memberKey.has_value() && inValue) {
            if (memberName == key)
                found = Member{*memberKey, valueBegin, end};
            lastValueEnd = end;
        }
        memberKey.reset();
        inValue = false;
    };
    while (at < size) {
        const char c = settings[at];
        if (c == '/' && at + 1 < size && settings[at + 1] == '/') {
            at = settings.find('\n', at);
            at = at == std::string_view::npos ? size : at;
            continue;
        }
        if (c == '/' && at + 1 < size && settings[at + 1] == '*') {
            const std::size_t close = settings.find("*/", at + 2);
            at = close == std::string_view::npos ? size : close + 2;
            continue;
        }
        if (c == '"') {
            const std::size_t begin = at;
            ++at;
            std::string text;
            while (at < size && settings[at] != '"') {
                if (settings[at] == '\\' && at + 1 < size) {
                    text.push_back(settings[at + 1]);
                    at += 2;
                    continue;
                }
                text.push_back(settings[at]);
                ++at;
            }
            ++at;
            if (depth == 1 && !memberKey.has_value()) {
                memberKey = begin;
                memberName = std::move(text);
            }
            else if (depth == 1 && inValue && valueBegin == begin) {
                endValue(at);
            }
            continue;
        }
        if (c == ':' && depth == 1 && memberKey.has_value() && !inValue) {
            inValue = true;
            ++at;
            while (at < size &&
                   (settings[at] == ' ' || settings[at] == '\t' || settings[at] == '\r' || settings[at] == '\n'))
                ++at;
            valueBegin = at;
            continue;
        }
        if (c == '{' || c == '[') {
            if (depth == 0 && c == '{' && rootOpen == std::string_view::npos)
                rootOpen = at;
            ++depth;
            ++at;
            continue;
        }
        if (c == '}' || c == ']') {
            --depth;
            if (depth == 1 && inValue)
                endValue(at + 1);
            if (depth == 0 && c == '}' && rootClose == std::string_view::npos) {
                if (inValue) {
                    // A scalar value runs to the brace, less trailing space.
                    std::size_t end = at;
                    while (end > valueBegin &&
                           std::string_view(" \t\r\n").find(settings[end - 1]) != std::string_view::npos)
                        --end;
                    endValue(end);
                }
                rootClose = at;
            }
            ++at;
            continue;
        }
        if (c == ',' && depth == 1 && inValue) {
            std::size_t end = at;
            while (end > valueBegin && std::string_view(" \t\r\n").find(settings[end - 1]) != std::string_view::npos)
                --end;
            endValue(end);
        }
        ++at;
    }
    if (rootOpen == std::string_view::npos || rootClose == std::string_view::npos || depth != 0)
        return std::nullopt;

    std::string out;
    if (found.has_value()) {
        out.append(settings.substr(0, found->valueBegin));
        out.append(rendered);
        out.append(settings.substr(found->valueEnd));
        return out;
    }
    // Added last, after a comma if there is a member before it.
    std::string_view before = settings.substr(0, rootClose);
    while (!before.empty() && std::string_view(" \t\r\n").find(before.back()) != std::string_view::npos)
        before.remove_suffix(1);
    out.append(before);
    if (lastValueEnd != std::string_view::npos)
        out.push_back(',');
    out.append("\n    \"");
    out.append(key);
    out.append("\": ");
    out.append(rendered);
    out.append("\n");
    out.append(settings.substr(rootClose));
    return out;
}

bool writeVsCodeLuauFlags(const std::filesystem::path& projectRoot)
{
    const std::filesystem::path file = projectRoot / ".vscode" / "settings.json";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
        return true;
    std::string text;
    if (!platform::readTextFile(file, text))
        return false;

    std::string object = "{\n        \"LuauSolverV2\": \"true\"";
    for (const auto& [name, value] : luauToolFlags()) {
        object += ",\n        \"" + name + "\": \"" + value + "\"";
    }
    object += "\n    }";
    std::optional<std::string> changed = withSettingsKey(text, "luau-lsp.fflags.override", object);
    // Every flag luau-lsp has, on, as Luau's own analyser runs them; the
    // override above then pins the parser's and the project's.
    if (changed.has_value())
        changed = withSettingsKey(*changed, "luau-lsp.fflags.enableByDefault", "true");
    if (!changed.has_value())
        return false;
    if (*changed == text)
        return true;
    return platform::writeTextFile(file, *changed);
}

} // namespace engine::app
