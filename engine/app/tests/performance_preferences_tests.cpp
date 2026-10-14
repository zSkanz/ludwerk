// Preferences' Performance page with no window (ADR 0147, section 6): the
// editor's frame rate, its throttle in the background, the viewport's quality
// and Play with the player's saved settings, drawn against an ImGui that draws
// to nowhere. What it can say: the page lays its four settings out without
// tripping ImGui's own checks, and a search finds each by its name. What the
// choices do is held by `editor_performance_tests.cpp`.

#include <doctest/doctest.h>
#include <imgui.h>

#include "engine/app/debug_overlay.h"
#include "engine/app/editor_performance.h"

using namespace engine;

namespace {

struct Desk
{
    Desk()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(900.0f, 600.0f);
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~Desk() { ImGui::DestroyContext(); }

    // One frame of the page, in a window that fills the display: how many
    // settings `search` found.
    std::size_t frame(std::string_view search)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        std::size_t found = 0;
        if (ImGui::Begin("Preferences", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
            found = app::drawPerformancePreferencesPage(search);
        ImGui::End();
        ImGui::EndFrame();
        return found;
    }
};

} // namespace

TEST_CASE("ADR 0147: Preferences' Performance page lays out its five settings, and a search finds each")
{
    Desk desk;
    // Drawn at every choice the page offers, so each one's name is asked for.
    const app::EditorPerformance before = app::editorPerformance();
    for (const app::EditorPerformance shown :
         {app::EditorPerformance{},
          app::EditorPerformance{.frameRate = 144, .backgroundThrottle = false, .viewportQuality = 0},
          app::EditorPerformance{
              .frameRate = app::EditorPerformance::kUnlimited, .viewportQuality = 3, .playWithSavedSettings = true}}) {
        app::setEditorPerformance(shown);
        for (int frame = 0; frame < 3; ++frame)
            CHECK(desk.frame({}) == 5);
    }
    app::setEditorPerformance(before);

    CHECK(desk.frame("frame rate") == 1);
    CHECK(desk.frame("VIEWPORT") == 1);
    CHECK(desk.frame("saved player") == 1);
    CHECK(desk.frame("background") == 1);
    // The file-watching batch's: what a script changed during Play does.
    CHECK(desk.frame("during play") == 1);
    CHECK(desk.frame("nothing of the sort") == 0);
}
