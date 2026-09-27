#include "engine/app/ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"

#if ENG_DEBUG_UI
#include <imgui.h>
#endif

namespace engine::app {
namespace {

using core::Color3;
using core::f32;

// `#rrggbb`. A palette written as hex is a palette anybody can paste into a
// colour picker and check; written as three floats it is a palette nobody
// reads. Parsed at startup rather than stored as floats for the same reason
// `editor.json` writes hex -- there is one spelling of a colour in this
// repository and it is the one a designer uses.
[[nodiscard]] constexpr f32 channel(core::u32 packed, unsigned shift) noexcept
{
    return static_cast<f32>((packed >> shift) & 0xFFu) / 255.0f;
}

[[nodiscard]] constexpr Color3 rgb(core::u32 packed) noexcept
{
    return Color3{channel(packed, 16), channel(packed, 8), channel(packed, 0)};
}

// --- The themes -------------------------------------------------------------
//
// **Every number below was measured before it was committed**, and the numbers
// it replaced are the reason: `StyleColorsDark`'s disabled text is 0.50 grey on
// a 0.06 ground, which clears the bar, and its `TextDisabled` over a HOVERED row
// does not -- ImGui's default palette was drawn for a debug window on top of a
// game, not for an application somebody reads for six hours.
//
// The ratios are in `ui_theme_tests.cpp` as assertions, not as comments here,
// because a comment claiming 5.2:1 is a comment that stops being true the first
// time somebody nudges a hex digit.

// **The default pair follows VS Code's modern themes** (the owner, 2026-09-27):
// neutral greys with no hue of their own, the side bars a step darker than the
// editor, one blue for what is active, and hairline borders. A person who
// spends the day in that editor should not have to recalibrate their eyes
// between the two.
constexpr Theme kDark{
    .id = "dark",
    .name = "Dark",
    .dark = true,
    .palette =
        {
            // The side bars, panels and menus.
            .background = rgb(0x181818),
            // The editor's own ground: code, fields, lists.
            .surface = rgb(0x1F1F1F),
            .surfaceRaised = rgb(0x313131),
            .border = rgb(0x2B2B2B),
            .text = rgb(0xCCCCCC),
            .textMuted = rgb(0x9D9D9D),
            .accent = rgb(0x3794FF),
            .accentFill = rgb(0x0078D4),
            .onAccent = rgb(0xFFFFFF),
            .warning = rgb(0xCCA700),
            .danger = rgb(0xF14C4C),
            .success = rgb(0x89D185),
        },
    .syntax =
        {
            .keyword = rgb(0x569CD6),
            .identifier = rgb(0x9CDCFE),
            .number = rgb(0xB5CEA8),
            .string = rgb(0xCE9178),
            .comment = rgb(0x6A9955),
            .operatorToken = rgb(0xD4D4D4),
            .attribute = rgb(0xDCDCAA),
            .typeName = rgb(0x4EC9B0),
            .errorToken = rgb(0xF44747),
        },
};

constexpr Theme kLight{
    .id = "light",
    .name = "Light",
    .dark = false,
    .palette =
        {
            .background = rgb(0xF8F8F8),
            .surface = rgb(0xFFFFFF),
            .surfaceRaised = rgb(0xE8E8E8),
            .border = rgb(0xE5E5E5),
            .text = rgb(0x3B3B3B),
            .textMuted = rgb(0x616161),
            .accent = rgb(0x005FB8),
            .accentFill = rgb(0x005FB8),
            .onAccent = rgb(0xFFFFFF),
            .warning = rgb(0x895503),
            .danger = rgb(0xCD3131),
            .success = rgb(0x2E7D32),
        },
    .syntax =
        {
            .keyword = rgb(0x0000FF),
            .identifier = rgb(0x001080),
            .number = rgb(0x0A7B55),
            .string = rgb(0xA31515),
            .comment = rgb(0x008000),
            .operatorToken = rgb(0x3B3B3B),
            .attribute = rgb(0x795E26),
            .typeName = rgb(0x267F99),
            .errorToken = rgb(0xCD3131),
        },
};

// The Orbit pair (2026-09-23), kept for whoever chose it.
constexpr Theme kOrbitDark{
    .id = "orbit-dark",
    .name = "Orbit Dark",
    .dark = true,
    .palette =
        {
            // Graphite surfaces pair with the Orbit mint accent.
            .background = rgb(0x17242C),
            .surface = rgb(0x101C24),
            .surfaceRaised = rgb(0x263740),
            .border = rgb(0x344851),
            .text = rgb(0xE8F0F2),
            .textMuted = rgb(0xA6B8C0),
            .accent = rgb(0x55E0C5),
            .accentFill = rgb(0x55E0C5),
            .onAccent = rgb(0x102B27),
            .warning = rgb(0xE0A34A),
            .danger = rgb(0xF87F73),
            .success = rgb(0x46C46E),
        },
    .syntax =
        {
            .keyword = rgb(0xC9A0FF),
            .identifier = rgb(0xE8F0F2),
            .number = rgb(0xF5B682),
            .string = rgb(0x9CD67F),
            .comment = rgb(0x94A7B2),
            .operatorToken = rgb(0xA9B2BD),
            .attribute = rgb(0xFFD479),
            .typeName = rgb(0x5CD6E6),
            .errorToken = rgb(0xF87F73),
        },
};

constexpr Theme kOrbitLight{
    .id = "orbit-light",
    .name = "Orbit Light",
    .dark = false,
    .palette =
        {
            .background = rgb(0xF4F8F9),
            .surface = rgb(0xFFFFFF),
            .surfaceRaised = rgb(0xE4ECEE),
            .border = rgb(0xCBD8DD),
            .text = rgb(0x14252D),
            .textMuted = rgb(0x52666E),
            // Deep teal retains contrast for links on the light surfaces.
            .accent = rgb(0x076C63),
            .accentFill = rgb(0x076C63),
            .onAccent = rgb(0xFFFFFF),
            .warning = rgb(0x8A5A00),
            .danger = rgb(0xB3261E),
            .success = rgb(0x16733C),
        },
    .syntax =
        {
            .keyword = rgb(0x7B24C4),
            .identifier = rgb(0x14252D),
            .number = rgb(0xA6431F),
            .string = rgb(0x17693C),
            .comment = rgb(0x5E6773),
            .operatorToken = rgb(0x3D444D),
            .attribute = rgb(0x8A5A00),
            .typeName = rgb(0x0B6477),
            .errorToken = rgb(0xB3261E),
        },
};

constexpr Theme kThemes[]{kDark, kLight, kOrbitDark, kOrbitLight};

// sRGB -> linear, the specification's own piecewise transfer function.
[[nodiscard]] f32 linearize(f32 channelValue) noexcept
{
    return channelValue <= 0.04045f ? channelValue / 12.92f : std::pow((channelValue + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] f32 relativeLuminance(Color3 color) noexcept
{
    return 0.2126f * linearize(color.r) + 0.7152f * linearize(color.g) + 0.0722f * linearize(color.b);
}

} // namespace

std::span<const Theme> themes() noexcept
{
    return std::span<const Theme>(kThemes, std::size(kThemes));
}

const Theme& themeById(std::string_view id) noexcept
{
    for (const Theme& theme : kThemes) {
        if (theme.id == id)
            return theme;
    }
    return kThemes[0];
}

ThemeMetrics themeMetrics() noexcept
{
    return ThemeMetrics{};
}

namespace {
// Points into `kThemes`, which is static storage, so this can never dangle.
const Theme* g_current = nullptr;
} // namespace

const Theme& currentTheme() noexcept
{
    return g_current != nullptr ? *g_current : kThemes[0];
}

f32 contrastRatio(Color3 a, Color3 b) noexcept
{
    const f32 first = relativeLuminance(a);
    const f32 second = relativeLuminance(b);
    const f32 lighter = std::max(first, second);
    const f32 darker = std::min(first, second);
    return (lighter + 0.05f) / (darker + 0.05f);
}

std::filesystem::path appearanceFile()
{
    const std::filesystem::path& userDir = platform::paths().userDir;
    return userDir.empty() ? std::filesystem::path{} : userDir / "appearance.json";
}

Appearance loadAppearance(const std::filesystem::path& file)
{
    Appearance appearance{.themeId = std::string(kThemes[0].id), .scale = 0.0f};
    if (file.empty())
        return appearance;

    std::string text;
    if (!platform::readTextFile(file, text))
        return appearance;

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(text); !parsed.ok)
        return appearance;

    const core::JsonValue root = document.root();
    // Read through `themeById` rather than stored raw, so a file naming a theme
    // this build does not carry opens on the default instead of on nothing.
    appearance.themeId = std::string(themeById(root["theme"].asString()).id);
    if (const core::JsonValue scale = root["scale"]; scale.type() == core::JsonType::Number)
        appearance.scale = static_cast<f32>(scale.asNumber());
    return appearance;
}

bool saveAppearance(const std::filesystem::path& file, const Appearance& appearance)
{
    if (file.empty())
        return false;

    core::JsonWriter writer;
    writer.beginObject();
    writer.field("theme", appearance.themeId);
    writer.field("scale", static_cast<core::f64>(appearance.scale));
    writer.endObject();

    // SDL creates the preference directory on the way out of `SDL_GetPrefPath`,
    // but a file written into a directory somebody deleted mid-session is worth
    // one syscall to avoid.
    (void)platform::createDirectories(file.parent_path());
    return platform::writeTextFile(file, writer.text());
}

f32 resolveUiScale(f32 requested, f32 displayScale) noexcept
{
    // A display that answers zero or a nonsense number is a display with nothing
    // to say, and multiplying every padding in the shell by it would be worse
    // than ignoring it (`platform::windowDisplayScale` makes the same call).
    const f32 fromDisplay = displayScale > 0.0f ? displayScale : 1.0f;
    const f32 chosen = requested > 0.0f ? requested : fromDisplay;
    return std::clamp(chosen, kMinimumUiScale, kMaximumUiScale);
}

std::filesystem::path uiFontFile()
{
    return platform::paths().contentDir / "fonts" / "Inter.ttf";
}

std::filesystem::path codeFontFile()
{
    return platform::paths().contentDir / "fonts" / "Mono.ttf";
}

#if ENG_DEBUG_UI

namespace {

[[nodiscard]] ImVec4 opaque(Color3 color) noexcept
{
    return ImVec4(color.r, color.g, color.b, 1.0f);
}

[[nodiscard]] ImVec4 fade(Color3 color, float alpha) noexcept
{
    return ImVec4(color.r, color.g, color.b, alpha);
}

// Between two colours, in sRGB. Used for the handful of slots that want a step
// between two tokens rather than a token of their own -- a scrollbar grab is
// not a design decision, it is "the border, a bit closer to the text".
[[nodiscard]] Color3 mix(Color3 from, Color3 to, float amount) noexcept
{
    return Color3{from.r + (to.r - from.r) * amount, from.g + (to.g - from.g) * amount,
                  from.b + (to.b - from.b) * amount};
}

} // namespace

void applyTheme(const Theme& theme, f32 scale)
{
    if (ImGui::GetCurrentContext() == nullptr)
        return;

    // Remembered before anything is written, so `currentTheme()` is right even
    // for a caller that draws in the same frame the theme changed.
    g_current = &theme;

    const ThemePalette& p = theme.palette;
    const ThemeMetrics metrics = themeMetrics();

    // **From a default-constructed style, never from the live one.**
    // `ScaleAllSizes` multiplies in place, so applying a theme over an already
    // scaled style scales it twice -- which is invisible the first time and
    // grotesque the third.
    ImGuiStyle style;

    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.45f;

    style.WindowPadding = ImVec2(metrics.windowPaddingX, metrics.windowPaddingY);
    style.FramePadding = ImVec2(metrics.framePaddingX, metrics.framePaddingY);
    style.ItemSpacing = ImVec2(metrics.itemSpacingX, metrics.itemSpacingY);
    style.ItemInnerSpacing = ImVec2(metrics.itemInnerSpacingX, metrics.itemInnerSpacingY);
    style.CellPadding = ImVec2(metrics.cellPaddingX, metrics.cellPaddingY);
    style.IndentSpacing = metrics.indentSpacing;
    style.ScrollbarSize = metrics.scrollbarSize;
    style.GrabMinSize = metrics.grabMinSize;

    // Panels and tabs are square, as the side bars and editor tabs they follow
    // are; floating surfaces and controls take the small radius.
    style.WindowRounding = metrics.containerRounding;
    style.ChildRounding = metrics.rounding;
    style.PopupRounding = metrics.containerRounding;
    style.FrameRounding = metrics.rounding;
    style.ScrollbarRounding = metrics.rounding;
    style.GrabRounding = metrics.rounding;
    style.TabRounding = 0.0f;
    style.ImageRounding = metrics.rounding;
    style.MenuItemRounding = metrics.rounding;
    style.SelectableRounding = metrics.rounding;
    style.TreeLinesRounding = metrics.rounding;

    // Containers keep a fine boundary; filled controls do not need a second outline.
    style.WindowBorderSize = metrics.borderSize;
    style.ChildBorderSize = metrics.borderSize;
    style.PopupBorderSize = metrics.borderSize;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    // One close affordance per tab; the extra node button closes an entire group.
    style.DockingNodeHasCloseButton = false;
    style.TabCloseButtonMinWidthSelected = -1.0f;
    // The editor this follows shows a tab's close button on the active tab, and
    // on another only under the pointer: a row of crosses is a row of ways to
    // lose a panel by missing its title.
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.TabBarBorderSize = metrics.tabBarBorderSize;
    style.TabBarOverlineSize = metrics.tabBarOverlineSize;
    style.DockingSeparatorSize = metrics.dockingSeparatorSize;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = ImVec2(16.0f, metrics.framePaddingY);

    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    // Center action labels; navigation rows retain a shared left edge.
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = opaque(p.text);
    colors[ImGuiCol_TextDisabled] = opaque(p.textMuted);
    colors[ImGuiCol_WindowBg] = opaque(p.background);
    colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // Opaque rather than the default's 94%: a menu you can read the panel
    // through is a menu that is harder to read than the panel.
    colors[ImGuiCol_PopupBg] = opaque(p.background);
    colors[ImGuiCol_Border] = opaque(p.border);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // A field is a step off whatever it sits on, toward the text: the recessed
    // look of an input box, in either theme.
    // #313131 on the dark ground, the field and checkbox colour of the editor
    // this follows: a box somebody can see before hovering it.
    colors[ImGuiCol_FrameBg] = opaque(mix(p.surface, p.text, theme.dark ? 0.10f : 0.03f));
    colors[ImGuiCol_FrameBgHovered] = opaque(mix(p.surface, p.text, theme.dark ? 0.15f : 0.06f));
    colors[ImGuiCol_FrameBgActive] = opaque(mix(p.surface, p.accentFill, 0.20f));

    colors[ImGuiCol_TitleBg] = opaque(p.surface);
    colors[ImGuiCol_TitleBgActive] = opaque(p.surfaceRaised);
    colors[ImGuiCol_TitleBgCollapsed] = opaque(p.surface);
    colors[ImGuiCol_MenuBarBg] = opaque(p.background);

    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = opaque(mix(p.border, p.textMuted, 0.25f));
    colors[ImGuiCol_ScrollbarGrabHovered] = opaque(p.textMuted);
    colors[ImGuiCol_ScrollbarGrabActive] = opaque(p.textMuted);

    colors[ImGuiCol_CheckMark] = opaque(p.accent);
    colors[ImGuiCol_CheckboxSelectedBg] = fade(p.accentFill, 0.25f);
    colors[ImGuiCol_SliderGrab] = opaque(p.accentFill);
    colors[ImGuiCol_SliderGrabActive] = opaque(p.accent);

    // A button is the raised surface, not the accent. The accent is reserved for
    // what is selected and for the ONE primary action on a screen -- spend it on
    // every button and it stops meaning anything, which is the failure mode of
    // every palette that starts from a brand colour.
    colors[ImGuiCol_Button] = opaque(p.surfaceRaised);
    colors[ImGuiCol_ButtonHovered] = opaque(mix(p.surfaceRaised, p.text, 0.08f));
    colors[ImGuiCol_ButtonActive] = opaque(mix(p.surfaceRaised, p.text, 0.14f));

    // A list row: the accent's fill, faintly, for what is selected, and the
    // text's own colour at a whisper for what is under the pointer -- a hover
    // that looked like a selection would be a second selection.
    colors[ImGuiCol_Header] = fade(p.accentFill, theme.dark ? 0.40f : 0.18f);
    colors[ImGuiCol_HeaderHovered] = fade(p.text, 0.07f);
    colors[ImGuiCol_HeaderActive] = fade(p.accentFill, theme.dark ? 0.50f : 0.26f);

    colors[ImGuiCol_Separator] = opaque(p.border);
    colors[ImGuiCol_SeparatorHovered] = opaque(p.accentFill);
    colors[ImGuiCol_SeparatorActive] = opaque(p.accentFill);

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ResizeGripHovered] = fade(p.accentFill, 0.55f);
    colors[ImGuiCol_ResizeGripActive] = opaque(p.accentFill);

    colors[ImGuiCol_InputTextCursor] = opaque(p.text);

    // Tabs: the unselected ones on the bar's own ground, the selected one on
    // the ground of what it shows, with the accent's line along its top.
    colors[ImGuiCol_Tab] = opaque(p.background);
    colors[ImGuiCol_TabHovered] = opaque(mix(p.background, p.text, 0.06f));
    colors[ImGuiCol_TabSelected] = opaque(p.surface);
    colors[ImGuiCol_TabSelectedOverline] = opaque(p.accentFill);
    colors[ImGuiCol_TabDimmed] = opaque(p.background);
    colors[ImGuiCol_TabDimmedSelected] = opaque(p.surface);
    // Muted rather than absent: an unfocused dock node still has a selected tab,
    // and hiding the mark makes the panel look like it has none.
    colors[ImGuiCol_TabDimmedSelectedOverline] = opaque(p.border);

    colors[ImGuiCol_DockingPreview] = fade(p.accentFill, 0.45f);
    colors[ImGuiCol_DockingEmptyBg] = opaque(p.surface);

    colors[ImGuiCol_PlotLines] = opaque(p.accent);
    colors[ImGuiCol_PlotLinesHovered] = opaque(mix(p.accent, p.text, 0.30f));
    colors[ImGuiCol_PlotHistogram] = opaque(p.accent);
    colors[ImGuiCol_PlotHistogramHovered] = opaque(mix(p.accent, p.text, 0.30f));

    colors[ImGuiCol_TableHeaderBg] = opaque(p.background);
    colors[ImGuiCol_TableBorderStrong] = opaque(p.border);
    colors[ImGuiCol_TableBorderLight] = opaque(mix(p.background, p.border, 0.60f));
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // Barely there, and on purpose: banding a long property grid helps the eye
    // track a row across, and any more than this reads as two kinds of row.
    colors[ImGuiCol_TableRowBgAlt] = fade(theme.dark ? p.text : p.background, 0.03f);

    colors[ImGuiCol_TextLink] = opaque(p.accent);
    colors[ImGuiCol_TextSelectedBg] = fade(p.accentFill, 0.45f);
    // The indent guides: there, but no louder than a border.
    colors[ImGuiCol_TreeLines] = opaque(mix(p.background, p.textMuted, 0.35f));
    colors[ImGuiCol_DragDropTarget] = opaque(p.accentFill);
    colors[ImGuiCol_DragDropTargetBg] = fade(p.accentFill, 0.15f);
    // A dot on an unsaved tab, the text's own colour, as the tabs this follows
    // have it.
    colors[ImGuiCol_UnsavedMarker] = opaque(p.text);
    colors[ImGuiCol_NavCursor] = opaque(p.accentFill);
    colors[ImGuiCol_NavWindowingHighlight] = fade(p.accentFill, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);

    // Sizes first, then the font: `ScaleAllSizes` deliberately does not touch
    // fonts, and the two multipliers have to agree or the padding around a
    // widget stops matching the text inside it.
    style.ScaleAllSizes(scale);
    style.FontSizeBase = metrics.fontSize;
    style.FontScaleMain = scale;

    ImGui::GetStyle() = style;
}

namespace {
// Held here rather than handed back through `loadCodeFont`, so that the header
// can promise an `ImFont*` without ever naming ImGui's own header.
ImFont* g_codeFont = nullptr;
} // namespace

ImFont* codeFont() noexcept
{
    return g_codeFont;
}

std::string tabIconPad()
{
    // The icon is drawn at the tab's own text height, with one inner spacing
    // after it -- the same gap every other icon-and-label pair in the shell
    // uses, so a tab reads like a tree row.
    const float want = ImGui::GetFontSize() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float space = ImGui::CalcTextSize(" ").x;
    if (space <= 0.0f)
        return std::string(3, ' ');
    // Rounded UP: a tab one pixel wider than it needs is invisible, and a tab
    // one pixel narrower puts the icon on top of the first letter.
    const int count = static_cast<int>(std::ceil(want / space));
    return std::string(static_cast<std::size_t>(std::clamp(count, 1, 16)), ' ');
}

bool loadCodeFont()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return false;

    const std::filesystem::path file = codeFontFile();
    if (!platform::fileExists(file))
        return false;

    // The same size as the UI face. A code pane at a different point size from
    // the panel around it is the sort of thing nobody can name and everybody
    // notices, and the interface scale multiplies both.
    g_codeFont = ImGui::GetIO().Fonts->AddFontFromFileTTF(file.string().c_str(), themeMetrics().fontSize);
    return g_codeFont != nullptr;
}

bool loadUiFont()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return false;

    const std::filesystem::path file = uiFontFile();
    if (!platform::fileExists(file))
        return false;

    // Size zero would take ImGui's own default. The atlas is dynamic since 1.92,
    // so this is the size the face is DESIGNED against rather than the only one
    // it can be drawn at -- `PushFont(font, size)` still gets a real rasterisation
    // at any other size, which is what the launcher's heading uses.
    ImGuiIO& io = ImGui::GetIO();
    const ImFont* font = io.Fonts->AddFontFromFileTTF(file.string().c_str(), themeMetrics().fontSize);
    return font != nullptr;
}

#else

// ADR 0011 again: a shipping build has no ImGui to style. The data half above
// still compiles, and that is deliberate -- nothing about a palette needs a
// renderer, and a test that could only run in a dev build is a test that stops
// running the day somebody checks the shipping profile.

void applyTheme(const Theme& theme, f32)
{
    g_current = &theme;
}

bool loadUiFont()
{
    return false;
}

bool loadCodeFont()
{
    return false;
}

ImFont* codeFont() noexcept
{
    return nullptr;
}

std::string tabIconPad()
{
    return std::string();
}

#endif

} // namespace engine::app
