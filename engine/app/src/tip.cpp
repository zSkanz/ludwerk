// A tip under the pointer (`tip.h`): the toolkit's own tooltip, with the words
// cut into lines before it draws them.
#if ENG_DEBUG_UI

#include "engine/app/tip.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <imgui_internal.h>
#include <string>
#include <string_view>

#include "engine/app/doc_summary.h"

namespace engine::app {

namespace {

[[nodiscard]] std::string formatted(const char* format, va_list arguments) IM_FMTLIST(1);

std::string formatted(const char* format, va_list arguments)
{
    va_list measured;
    va_copy(measured, arguments);
    const int length = std::vsnprintf(nullptr, 0, format, measured);
    va_end(measured);
    if (length <= 0)
        return {};
    std::string text(static_cast<std::size_t>(length), '\0');
    std::vsnprintf(text.data(), text.size() + 1, format, arguments);
    return text;
}

// As `ImGui::SetTooltipV`, which this stands in for: a tip submitted earlier
// in the frame is replaced, not added to.
void show(const std::string& text)
{
    if (!ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None))
        return;
    const std::string lines = wrapAtSpaces(text, tipWidth(), [](std::string_view run) {
        return ImGui::CalcTextSize(run.data(), run.data() + run.size()).x;
    });
    ImGui::TextUnformatted(lines.c_str(), lines.c_str() + lines.size());
    ImGui::EndTooltip();
}

} // namespace

float tipWidth()
{
    return ImGui::GetFontSize() * 32.0f;
}

void itemTip(const char* format, ...)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        return;
    va_list arguments;
    va_start(arguments, format);
    const std::string text = formatted(format, arguments);
    va_end(arguments);
    show(text);
}

void pointerTip(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const std::string text = formatted(format, arguments);
    va_end(arguments);
    show(text);
}

} // namespace engine::app

#endif
