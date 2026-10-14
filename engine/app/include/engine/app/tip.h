// A tip under the pointer, on lines a reader follows to their end.
#pragma once

#include <imgui.h>

namespace engine::app {

// The widest a line of a tip is drawn, in pixels, at the font in use.
[[nodiscard]] float tipWidth();

// **The tip of the item drawn last**, shown once the pointer has rested on it:
// what `ImGui::SetItemTooltip` shows, on lines that break at a space. The
// toolkit draws a tip as one line however long it is, and a sentence wider
// than the window ran off its edge with its end unread. A line break in the
// words is kept.
void itemTip(const char* format, ...) IM_FMTARGS(1);

// **A tip at the pointer, now**, in place of any this frame already has: what
// `ImGui::SetTooltip` shows, on the same lines.
void pointerTip(const char* format, ...) IM_FMTARGS(1);

} // namespace engine::app
