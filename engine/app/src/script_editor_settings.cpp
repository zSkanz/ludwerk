#include "engine/app/script_editor_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "engine/app/ui_theme.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"

namespace engine::app {
namespace {

using core::Color3;
using core::f32;

[[nodiscard]] constexpr Color3 rgb(core::u32 packed) noexcept
{
    return Color3{static_cast<f32>((packed >> 16) & 0xFFu) / 255.0f, static_cast<f32>((packed >> 8) & 0xFFu) / 255.0f,
                  static_cast<f32>(packed & 0xFFu) / 255.0f};
}

[[nodiscard]] Color3 mix(Color3 from, Color3 to, f32 amount) noexcept
{
    return Color3{from.r + (to.r - from.r) * amount, from.g + (to.g - from.g) * amount,
                  from.b + (to.b - from.b) * amount};
}

constexpr std::array<ScriptColorInfo, kScriptColorCount> kColors{{
    {"text", ENG_TR("engine.editor.script_colour.text")},
    {"keyword", ENG_TR("engine.editor.script_colour.keyword")},
    {"luauKeyword", ENG_TR("engine.editor.script_colour.luau_keyword")},
    {"function", ENG_TR("engine.editor.script_colour.function")},
    {"local", ENG_TR("engine.editor.script_colour.local")},
    {"nil", ENG_TR("engine.editor.script_colour.nil")},
    {"bool", ENG_TR("engine.editor.script_colour.bool")},
    {"self", ENG_TR("engine.editor.script_colour.self")},
    {"number", ENG_TR("engine.editor.script_colour.number")},
    {"string", ENG_TR("engine.editor.script_colour.string")},
    {"comment", ENG_TR("engine.editor.script_colour.comment")},
    {"todo", ENG_TR("engine.editor.script_colour.todo")},
    {"operator", ENG_TR("engine.editor.script_colour.operator")},
    {"bracket", ENG_TR("engine.editor.script_colour.bracket")},
    {"builtin", ENG_TR("engine.editor.script_colour.builtin")},
    {"functionName", ENG_TR("engine.editor.script_colour.function_name")},
    {"method", ENG_TR("engine.editor.script_colour.method")},
    {"property", ENG_TR("engine.editor.script_colour.property")},
    {"type", ENG_TR("engine.editor.script_colour.type")},
    {"attribute", ENG_TR("engine.editor.script_colour.attribute")},
    {"broken", ENG_TR("engine.editor.script_colour.broken")},
    {"background", ENG_TR("engine.editor.script_colour.background")},
    {"selection", ENG_TR("engine.editor.script_colour.selection")},
    {"currentLine", ENG_TR("engine.editor.script_colour.current_line")},
    {"matchingWord", ENG_TR("engine.editor.script_colour.matching_word")},
    {"lineNumber", ENG_TR("engine.editor.script_colour.line_number")},
    {"whitespace", ENG_TR("engine.editor.script_colour.whitespace")},
    {"caret", ENG_TR("engine.editor.script_colour.caret")},
    {"error", ENG_TR("engine.editor.script_colour.error")},
    {"warning", ENG_TR("engine.editor.script_colour.warning")},
    {"debuggerCurrentLine", ENG_TR("engine.editor.script_colour.debugger_current_line")},
    {"debuggerErrorLine", ENG_TR("engine.editor.script_colour.debugger_error_line")},
}};

constexpr std::array<ScriptActionInfo, kScriptActionCount> kActions{{
    {"save", ENG_TR("engine.editor.script_action.save"), "Ctrl+S"},
    {"find", ENG_TR("engine.editor.script_action.find"), "Ctrl+F"},
    {"replace", ENG_TR("engine.editor.script_action.replace"), "Ctrl+H"},
    {"goToLine", ENG_TR("engine.editor.script_action.go_to_line"), "Ctrl+G"},
    {"selectAll", ENG_TR("engine.editor.script_action.select_all"), "Ctrl+A"},
    {"undo", ENG_TR("engine.editor.script_action.undo"), "Ctrl+Z"},
    {"redo", ENG_TR("engine.editor.script_action.redo"), "Ctrl+Y"},
    {"cut", ENG_TR("engine.editor.script_action.cut"), "Ctrl+X"},
    {"copy", ENG_TR("engine.editor.script_action.copy"), "Ctrl+C"},
    {"paste", ENG_TR("engine.editor.script_action.paste"), "Ctrl+V"},
    {"toggleComment", ENG_TR("engine.editor.script_action.toggle_comment"), "Ctrl+Slash"},
    {"blockComment", ENG_TR("engine.editor.script_action.block_comment"), "Shift+Alt+A"},
    {"deleteLine", ENG_TR("engine.editor.script_action.delete_line"), "Ctrl+Shift+K"},
    {"selectLine", ENG_TR("engine.editor.script_action.select_line"), "Ctrl+L"},
    {"indent", ENG_TR("engine.editor.script_action.indent"), "Ctrl+RightBracket"},
    {"outdent", ENG_TR("engine.editor.script_action.outdent"), "Ctrl+LeftBracket"},
    {"jumpToBracket", ENG_TR("engine.editor.script_action.jump_to_bracket"), "Ctrl+Shift+Backslash"},
    {"moveLineUp", ENG_TR("engine.editor.script_action.move_line_up"), "Alt+UpArrow"},
    {"moveLineDown", ENG_TR("engine.editor.script_action.move_line_down"), "Alt+DownArrow"},
    {"copyLineUp", ENG_TR("engine.editor.script_action.copy_line_up"), "Shift+Alt+UpArrow"},
    {"copyLineDown", ENG_TR("engine.editor.script_action.copy_line_down"), "Shift+Alt+DownArrow"},
    {"insertLineBelow", ENG_TR("engine.editor.script_action.insert_line_below"), "Ctrl+Enter"},
    {"insertLineAbove", ENG_TR("engine.editor.script_action.insert_line_above"), "Ctrl+Shift+Enter"},
    {"addNextOccurrence", ENG_TR("engine.editor.script_action.add_next_occurrence"), "Ctrl+D"},
    {"selectAllOccurrences", ENG_TR("engine.editor.script_action.select_all_occurrences"), "Shift+Alt+L"},
    {"addCaretAbove", ENG_TR("engine.editor.script_action.add_caret_above"), "Ctrl+Alt+UpArrow"},
    {"addCaretBelow", ENG_TR("engine.editor.script_action.add_caret_below"), "Ctrl+Alt+DownArrow"},
    {"removeLastCaret", ENG_TR("engine.editor.script_action.remove_last_caret"), "Ctrl+U"},
    {"splitIntoLines", ENG_TR("engine.editor.script_action.split_into_lines"), "Shift+Alt+I"},
    {"triggerCompletion", ENG_TR("engine.editor.script_action.trigger_completion"), "Ctrl+Space"},
    {"resetZoom", ENG_TR("engine.editor.script_action.reset_zoom"), "Ctrl+0"},
    {"closeTab", ENG_TR("engine.editor.script_action.close_tab"), "Ctrl+W"},
}};

[[nodiscard]] bool sameWord(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

[[nodiscard]] std::string hexOf(Color3 c)
{
    const auto byte = [](f32 v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    char text[8]{};
    (void)std::snprintf(text, sizeof(text), "#%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b));
    return text;
}

[[nodiscard]] std::optional<Color3> colorOf(std::string_view text)
{
    if (text.size() != 7 || text[0] != '#')
        return std::nullopt;
    core::u32 packed = 0;
    for (std::size_t i = 1; i < 7; ++i) {
        const char c = text[i];
        packed <<= 4;
        if (c >= '0' && c <= '9')
            packed |= static_cast<core::u32>(c - '0');
        else if (c >= 'a' && c <= 'f')
            packed |= static_cast<core::u32>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            packed |= static_cast<core::u32>(c - 'A' + 10);
        else
            return std::nullopt;
    }
    return rgb(packed);
}

} // namespace

std::span<const ScriptColorInfo, kScriptColorCount> scriptColorInfo() noexcept
{
    return kColors;
}

std::span<const ScriptActionInfo, kScriptActionCount> scriptActionInfo() noexcept
{
    return kActions;
}

Color3 defaultScriptColor(ScriptColor color, const Theme& theme) noexcept
{
    const SyntaxPalette& s = theme.syntax;
    const ThemePalette& p = theme.palette;
    const bool dark = theme.dark;
    // The colours the theme's own syntax palette names follow it; the ones the
    // reference editor added on top are chosen here per ground, each clearing
    // the same 4.5:1 floor against the pane (`script_editor_settings_tests`).
    switch (color) {
    case ScriptColor::Text:
        return p.text;
    case ScriptColor::Keyword:
    case ScriptColor::LuauKeyword:
    case ScriptColor::FunctionKeyword:
    case ScriptColor::LocalKeyword:
        return s.keyword;
    case ScriptColor::Nil:
    case ScriptColor::Bool:
    case ScriptColor::Number:
        return s.number;
    case ScriptColor::Self:
        return dark ? rgb(0xF28FB0) : rgb(0xA3245E);
    case ScriptColor::String:
        return s.string;
    case ScriptColor::Comment:
        return s.comment;
    case ScriptColor::Todo:
        return s.attribute;
    case ScriptColor::Operator:
        return s.operatorToken;
    case ScriptColor::Bracket:
        return dark ? rgb(0xC4CCD6) : rgb(0x3D444D);
    case ScriptColor::BuiltinFunction:
        return dark ? rgb(0x6FC3FF) : rgb(0x005FA3);
    case ScriptColor::FunctionName:
    case ScriptColor::Method:
        return dark ? rgb(0xFFE08A) : rgb(0x7A5200);
    case ScriptColor::Property:
        return dark ? rgb(0x8FB8FF) : rgb(0x1F55A6);
    case ScriptColor::Type:
        return s.typeName;
    case ScriptColor::Attribute:
        return s.attribute;
    case ScriptColor::BrokenToken:
        return s.errorToken;
    case ScriptColor::Background:
        return p.surface;
    case ScriptColor::Selection:
        return mix(p.surface, p.accent, 0.30f);
    case ScriptColor::CurrentLine:
        return mix(p.surface, p.surfaceRaised, 0.45f);
    case ScriptColor::MatchingWord:
        return mix(p.surface, p.warning, 0.22f);
    case ScriptColor::LineNumber:
        return p.textMuted;
    case ScriptColor::Whitespace:
        return mix(p.surface, p.textMuted, 0.22f);
    case ScriptColor::Caret:
        return p.accent;
    case ScriptColor::ErrorUnderline:
        return p.danger;
    case ScriptColor::WarningUnderline:
        return p.warning;
    case ScriptColor::DebuggerCurrentLine:
        return mix(p.surface, p.warning, 0.22f);
    case ScriptColor::DebuggerErrorLine:
        return mix(p.surface, p.danger, 0.22f);
    case ScriptColor::Count:
        break;
    }
    return p.text;
}

std::optional<KeyChord> parseChord(std::string_view text)
{
    KeyChord chord;
    while (!text.empty()) {
        // The key itself may be `+` -- ImGui has no key by that name, so a
        // `+` is always a separator, and a trailing part is the key.
        const std::size_t plus = text.find('+');
        const std::string_view part = text.substr(0, plus);
        if (plus == std::string_view::npos) {
            if (part.empty())
                return std::nullopt;
            chord.key = std::string(part);
            return chord;
        }
        if (sameWord(part, "ctrl") || sameWord(part, "control"))
            chord.ctrl = true;
        else if (sameWord(part, "shift"))
            chord.shift = true;
        else if (sameWord(part, "alt"))
            chord.alt = true;
        else
            return std::nullopt;
        text.remove_prefix(plus + 1);
    }
    return std::nullopt;
}

std::string formatChord(const KeyChord& chord)
{
    if (chord.key.empty())
        return {};
    std::string text;
    if (chord.ctrl)
        text += "Ctrl+";
    if (chord.shift)
        text += "Shift+";
    if (chord.alt)
        text += "Alt+";
    return text + chord.key;
}

Color3 ScriptEditorSettings::color(ScriptColor which, const Theme& theme) const noexcept
{
    const std::size_t index = static_cast<std::size_t>(which);
    if (index < colors.size() && colors[index].has_value())
        return *colors[index];
    return defaultScriptColor(which, theme);
}

KeyChord ScriptEditorSettings::chord(ScriptAction which) const
{
    const std::size_t index = static_cast<std::size_t>(which);
    if (index < keys.size() && keys[index].has_value())
        return *keys[index];
    return parseChord(kActions[index].chord).value_or(KeyChord{});
}

std::optional<ScriptAction> ScriptEditorSettings::conflictOf(ScriptAction which, const KeyChord& wanted) const
{
    if (wanted.key.empty())
        return std::nullopt;
    for (std::size_t index = 0; index < kScriptActionCount; ++index) {
        const auto other = static_cast<ScriptAction>(index);
        if (other != which && chord(other) == wanted)
            return other;
    }
    return std::nullopt;
}

ScriptEditorSettings& scriptEditorSettings() noexcept
{
    static ScriptEditorSettings settings;
    return settings;
}

std::filesystem::path scriptEditorSettingsFile()
{
    const std::filesystem::path& userDir = platform::paths().userDir;
    return userDir.empty() ? std::filesystem::path{} : userDir / "script-editor.json";
}

ScriptEditorSettings loadScriptEditorSettings(const std::filesystem::path& file)
{
    ScriptEditorSettings settings;
    std::string text;
    if (file.empty() || !platform::readTextFile(file, text))
        return settings;
    core::JsonDocument document;
    if (!document.parse(text).ok)
        return settings;
    const core::JsonValue root = document.root();

    const core::JsonValue colors = root["colors"];
    for (std::size_t index = 0; index < kScriptColorCount; ++index) {
        if (const core::JsonValue value = colors[kColors[index].id]; value.type() == core::JsonType::String)
            settings.colors[index] = colorOf(value.asString());
    }
    const core::JsonValue keys = root["keys"];
    for (std::size_t index = 0; index < kScriptActionCount; ++index) {
        const core::JsonValue value = keys[kActions[index].id];
        if (value.type() != core::JsonType::String)
            continue;
        // An empty string is a command somebody unbound, which is a choice.
        settings.keys[index] =
            value.asString().empty() ? std::optional<KeyChord>(KeyChord{}) : parseChord(value.asString());
    }
    return settings;
}

bool saveScriptEditorSettings(const std::filesystem::path& file, const ScriptEditorSettings& settings)
{
    if (file.empty())
        return false;
    core::JsonWriter writer;
    writer.beginObject();
    writer.key("colors");
    writer.beginObject();
    for (std::size_t index = 0; index < kScriptColorCount; ++index) {
        if (settings.colors[index].has_value())
            writer.field(kColors[index].id, hexOf(*settings.colors[index]));
    }
    writer.endObject();
    writer.key("keys");
    writer.beginObject();
    for (std::size_t index = 0; index < kScriptActionCount; ++index) {
        if (settings.keys[index].has_value())
            writer.field(kActions[index].id, formatChord(*settings.keys[index]));
    }
    writer.endObject();
    writer.endObject();
    (void)platform::createDirectories(file.parent_path());
    return platform::writeTextFile(file, writer.text());
}

} // namespace engine::app
