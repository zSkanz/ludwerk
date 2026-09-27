#include "engine/app/command_palette.h"

#include <algorithm>
#include <cctype>

#if ENG_DEBUG_UI
#include <imgui.h>

#include "engine/app/ui_theme.h"
#endif

namespace engine::app {
namespace {

[[nodiscard]] char lower(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// A word starts after a space, a colon, a slash, a dot or an underscore, or
// where a lower-case letter meets an upper-case one ("LineJoin").
[[nodiscard]] bool wordStart(std::string_view text, std::size_t at) noexcept
{
    if (at == 0)
        return true;
    const char before = text[at - 1];
    const char here = text[at];
    if (before == ' ' || before == ':' || before == '/' || before == '.' || before == '_' || before == '-')
        return true;
    return std::islower(static_cast<unsigned char>(before)) != 0 && std::isupper(static_cast<unsigned char>(here)) != 0;
}

} // namespace

int fuzzyScore(std::string_view query, std::string_view text, std::vector<int>* positions)
{
    if (positions != nullptr)
        positions->clear();
    if (query.find_first_not_of(' ') == std::string_view::npos)
        return 0;
    int score = 0;
    std::size_t at = 0;
    int run = 0;
    for (const char wanted : query) {
        // Spaces in the query separate words and match anywhere, so "ins part"
        // is two searches in order rather than one with a space in it.
        if (wanted == ' ') {
            run = 0;
            continue;
        }
        const char needle = lower(wanted);
        bool found = false;
        while (at < text.size()) {
            if (lower(text[at]) == needle) {
                found = true;
                break;
            }
            ++at;
            run = 0;
        }
        if (!found)
            return -1;
        score += 1;
        if (run > 0)
            score += 4 * run;
        if (wordStart(text, at))
            score += 8;
        if (positions != nullptr)
            positions->push_back(static_cast<int>(at));
        ++run;
        ++at;
    }
    // Shorter is better among equals: "Save" before "Save Scene As".
    return score * 16 - static_cast<int>(std::min<std::size_t>(text.size(), 15));
}

void CommandPalette::open(Mode mode)
{
    open_ = true;
    focusInput_ = true;
    mode_ = mode;
    query_.clear();
    selected_ = 0;
    revealSelected_ = true;
    shownFrames_ = 0;
}

#if ENG_DEBUG_UI

namespace {

struct Match
{
    const PaletteItem* item = nullptr;
    int score = 0;
    int order = 0;
    std::vector<int> positions;
};

// A title with its matched characters in the accent, drawn at the cursor.
void drawHighlighted(const std::string& title, const std::vector<int>& positions, bool enabled)
{
    const ThemePalette& p = currentTheme().palette;
    const ImVec4 normal = ImGui::GetStyleColorVec4(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec4 hit(p.accent.r, p.accent.g, p.accent.b, normal.w);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    std::size_t next = 0;
    for (std::size_t index = 0; index < title.size(); ++index) {
        const bool matched = next < positions.size() && positions[next] == static_cast<int>(index);
        if (matched)
            ++next;
        const char* begin = title.c_str() + index;
        draw->AddText(at, ImGui::GetColorU32(matched ? hit : normal), begin, begin + 1);
        at.x += ImGui::CalcTextSize(begin, begin + 1).x;
    }
    ImGui::Dummy(ImVec2(ImGui::CalcTextSize(title.c_str()).x, ImGui::GetTextLineHeight()));
}

// A chord drawn as keys: each part in a small box, as keybindings are shown.
void drawKeys(const std::string& shortcut, float right)
{
    if (shortcut.empty())
        return;
    const ThemePalette& p = currentTheme().palette;
    const float padding = 4.0f * ImGui::GetStyle().FontScaleMain;
    const float gap = 3.0f * ImGui::GetStyle().FontScaleMain;
    // Only the first chord of "Ctrl+Y / Ctrl+Shift+Z".
    std::string first = shortcut.substr(0, shortcut.find(" / "));
    std::vector<std::string> keys;
    std::size_t start = 0;
    while (start <= first.size()) {
        const std::size_t plus = first.find('+', start + 1);
        keys.push_back(first.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }
    float width = 0.0f;
    for (const std::string& key : keys)
        width += ImGui::CalcTextSize(key.c_str()).x + padding * 2.0f + gap;
    ImGui::SameLine(std::max(right - width, ImGui::GetCursorPosX() + 8.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeight() + 2.0f;
    for (const std::string& key : keys) {
        const float w = ImGui::CalcTextSize(key.c_str()).x + padding * 2.0f;
        draw->AddRectFilled(at, ImVec2(at.x + w, at.y + height),
                            ImGui::GetColorU32(ImVec4(p.surfaceRaised.r, p.surfaceRaised.g, p.surfaceRaised.b, 0.9f)),
                            3.0f);
        draw->AddRect(at, ImVec2(at.x + w, at.y + height),
                      ImGui::GetColorU32(ImVec4(p.border.r, p.border.g, p.border.b, 1.0f)), 3.0f);
        draw->AddText(ImVec2(at.x + padding, at.y + 1.0f),
                      ImGui::GetColorU32(ImVec4(p.textMuted.r, p.textMuted.g, p.textMuted.b, 1.0f)), key.c_str());
        at.x += w + gap;
    }
    ImGui::Dummy(ImVec2(width, height));
}

} // namespace

void CommandPalette::draw(std::span<const PaletteItem> commands, std::span<const PaletteItem> files,
                          const std::function<void(std::string_view icon, float size)>& drawIcon)
{
    if (!open_)
        return;

    // `>` at the start of quick open is the palette, and deleting it goes back.
    if (mode_ == Mode::Files && !query_.empty() && query_.front() == '>') {
        mode_ = Mode::Commands;
        query_.erase(0, 1);
    }
    const bool commandsMode = mode_ == Mode::Commands;
    const std::span<const PaletteItem> source = commandsMode ? commands : files;

    // What matches, best first; with nothing typed, the recent commands first
    // and then everything in the order it was listed.
    std::vector<Match> matches;
    matches.reserve(source.size());
    int order = 0;
    for (const PaletteItem& item : source) {
        Match match;
        match.item = &item;
        match.order = order++;
        if (!query_.empty()) {
            match.score = fuzzyScore(query_, item.title, &match.positions);
            if (match.score < 0) {
                // A file is found by its folder too.
                if (item.detail.empty() || fuzzyScore(query_, item.detail) < 0)
                    continue;
                match.score = 0;
            }
        }
        else if (commandsMode) {
            const auto recent = std::find(recent_.begin(), recent_.end(), item.title);
            if (recent != recent_.end())
                match.score = 100000 - static_cast<int>(recent - recent_.begin());
        }
        matches.push_back(std::move(match));
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        return a.score != b.score ? a.score > b.score : a.order < b.order;
    });
    const int count = static_cast<int>(matches.size());
    selected_ = count == 0 ? 0 : std::clamp(selected_, 0, count - 1);

    // Across the top of the window, a third of its width, as the editor this
    // follows places its own.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::clamp(viewport->WorkSize.x * 0.4f, 420.0f * ImGui::GetStyle().FontScaleMain,
                                   760.0f * ImGui::GetStyle().FontScaleMain);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f,
                                   viewport->Pos.y + ImGui::GetFrameHeight() + 6.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav;
    bool close = false;
    const PaletteItem* chosen = nullptr;
    if (ImGui::Begin("##command-palette", nullptr, flags)) {
        ImGui::SetNextItemWidth(-1.0f);
        if (focusInput_) {
            ImGui::SetKeyboardFocusHere();
            focusInput_ = false;
        }
        char buffer[256]{};
        const std::size_t length = std::min(query_.size(), sizeof(buffer) - 1);
        std::copy_n(query_.data(), length, buffer);
        const char* hint = commandsMode ? "> Type the name of a command" : "Search files by name (type > for commands)";
        if (ImGui::InputTextWithHint("##query", hint, buffer, sizeof(buffer))) {
            query_ = buffer;
            selected_ = 0;
            revealSelected_ = true;
        }
        const int before = selected_;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            selected_ = count == 0 ? 0 : (selected_ + 1) % count;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            selected_ = count == 0 ? 0 : (selected_ + count - 1) % count;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
            selected_ = std::min(selected_ + 10, std::max(count - 1, 0));
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            selected_ = std::max(selected_ - 10, 0);
        if (selected_ != before)
            revealSelected_ = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            close = true;
        if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && count > 0 &&
            matches[static_cast<std::size_t>(selected_)].item->enabled)
            chosen = matches[static_cast<std::size_t>(selected_)].item;

        // At most a dozen rows showing; the list scrolls with the selection.
        const float row = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
        const int visible = std::min(count, 12);
        if (count == 0) {
            ImGui::TextDisabled(commandsMode ? "No matching commands" : "No matching files");
        }
        else if (ImGui::BeginChild("##results", ImVec2(0.0f, row * static_cast<float>(visible) + 2.0f),
                                   ImGuiChildFlags_None)) {
            const float right = ImGui::GetContentRegionAvail().x;
            for (int index = 0; index < count; ++index) {
                const Match& match = matches[static_cast<std::size_t>(index)];
                const PaletteItem& item = *match.item;
                ImGui::PushID(index);
                const bool isSelected = index == selected_;
                const ImVec2 rowStart = ImGui::GetCursorPos();
                if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, row))) {
                    if (item.enabled)
                        chosen = &item;
                }
                if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x != 0.0f)
                    selected_ = index;
                // Into view by the least scroll that shows it, and only when
                // the keyboard moved it there -- the wheel is the mouse's.
                if (isSelected && revealSelected_) {
                    revealSelected_ = false;
                    const float top = ImGui::GetWindowPos().y;
                    const float bottom = top + ImGui::GetWindowHeight();
                    if (ImGui::GetItemRectMin().y < top)
                        ImGui::SetScrollHereY(0.0f);
                    else if (ImGui::GetItemRectMax().y > bottom)
                        ImGui::SetScrollHereY(1.0f);
                }
                ImGui::SetCursorPos(ImVec2(rowStart.x + 6.0f, rowStart.y + ImGui::GetStyle().FramePadding.y));
                if (!item.icon.empty()) {
                    drawIcon(item.icon, ImGui::GetTextLineHeight());
                    ImGui::SameLine();
                }
                drawHighlighted(item.title, match.positions, item.enabled);
                if (!item.detail.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", item.detail.c_str());
                }
                // The recently used commands say so, as the editor this follows
                // labels them.
                if (commandsMode && query_.empty() && match.score >= 100000 - 64 && item.shortcut.empty()) {
                    const char* label = "recently used";
                    ImGui::SameLine(right - ImGui::CalcTextSize(label).x - 6.0f);
                    ImGui::TextDisabled("%s", label);
                }
                else {
                    drawKeys(item.shortcut, right - 6.0f);
                }
                ImGui::SetCursorPos(ImVec2(rowStart.x, rowStart.y + row));
                ImGui::PopID();
            }
        }
        if (count > 0)
            ImGui::EndChild();

        // Clicking anywhere else closes it -- after its first frames, so the
        // click that opened it from a menu does not also close it.
        if (shownFrames_ > 2 && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            close = true;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ++shownFrames_;

    if (chosen != nullptr) {
        if (commandsMode) {
            recent_.erase(std::remove(recent_.begin(), recent_.end(), chosen->title), recent_.end());
            recent_.insert(recent_.begin(), chosen->title);
            if (recent_.size() > 8)
                recent_.resize(8);
        }
        const std::function<void()> run = chosen->run;
        open_ = false;
        if (run)
            run();
        return;
    }
    if (close)
        open_ = false;
}

#else

void CommandPalette::draw(std::span<const PaletteItem>, std::span<const PaletteItem>,
                          const std::function<void(std::string_view, float)>&)
{}

#endif

} // namespace engine::app
