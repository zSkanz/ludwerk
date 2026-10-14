// A tip under the pointer is read to its end: however long its sentence, its
// lines stay inside the window.
#include <doctest/doctest.h>

// The tip is the editor's, and a build without the editor has none.
#if ENG_DEBUG_UI

#include <imgui.h>
#include <imgui_internal.h>

#include "engine/app/tip.h"

namespace {

// A context with one window and one button in it, and nothing behind them.
struct Screen
{
    Screen()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0f, 900.0f);
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Screen() { ImGui::DestroyContext(); }
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
};

struct Shown
{
    bool any = false;
    ImVec2 size;
    float line = 0.0f;
    float widest = 0.0f;
    float padding = 0.0f;
};

// The tip a button shows once the pointer has rested on it, as `tip` draws it.
template <typename Tip>
[[nodiscard]] Shown tipOf(Tip&& tip)
{
    Screen screen;
    ImGuiIO& io = ImGui::GetIO();
    Shown shown;
    for (int frame = 0; frame < 40; ++frame) {
        io.DeltaTime = 0.1f;
        io.AddMousePosEvent(80.0f, 90.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f));
        ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
        ImGui::Begin("desk");
        ImGui::Button("Bake", ImVec2(120.0f, 60.0f));
        tip();
        shown.line = ImGui::GetTextLineHeight();
        shown.widest = engine::app::tipWidth();
        shown.padding = ImGui::GetStyle().WindowPadding.x;
        ImGui::End();
        ImGui::EndFrame();
        shown.any = false;
        for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
            if ((window->Flags & ImGuiWindowFlags_Tooltip) != 0 && window->Active && !window->Hidden) {
                shown.any = true;
                shown.size = window->Size;
            }
        }
    }
    return shown;
}

constexpr const char* kLong =
    "Computes this volume's light and saves it to the file that Baked names. A file name is chosen if Baked is "
    "empty. A game that ships the file casts no rays for this room, and a machine that computes no volumes lights "
    "the room with the file's average color. Bake again after changing the room or its lights.";

} // namespace

TEST_CASE("a long tip is drawn on lines no wider than a tip's width, not on one that leaves the window")
{
    const Shown shown = tipOf([] { engine::app::itemTip("%s", kLong); });
    REQUIRE(shown.any);
    // The words, and the tip's own padding at both sides of them.
    CHECK(shown.size.x <= shown.widest + 2.0f * shown.padding + 1.0f);
    CHECK(shown.size.x < 1600.0f);
    CHECK(shown.size.y > 3.0f * shown.line);
}

TEST_CASE("a short tip is one line, and a tip keeps the line breaks it was given")
{
    const Shown one = tipOf([] { engine::app::itemTip("%s", "Bake this volume"); });
    REQUIRE(one.any);
    CHECK(one.size.y < 2.0f * one.line + 2.0f * 8.0f);

    const Shown three = tipOf([] { engine::app::itemTip("%s\n%s\n%s", "Grass", "asset://grass", "layer 2"); });
    REQUIRE(three.any);
    CHECK(three.size.y > one.size.y + 1.5f * one.line);
}

TEST_CASE("a tip at the pointer is wrapped the same way, and replaces one the frame already had")
{
    const Shown shown = tipOf([] {
        if (ImGui::IsItemHovered()) {
            engine::app::pointerTip("%s", "first");
            engine::app::pointerTip("%s", kLong);
        }
    });
    REQUIRE(shown.any);
    CHECK(shown.size.x <= shown.widest + 2.0f * shown.padding + 1.0f);
    CHECK(shown.size.y > 3.0f * shown.line);
}

#endif
