// The script that drives the editor for a gate's pictures: what it says it
// presses is pressed, however slowly the machine draws.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <string>

#include "engine/app/editor_drive.h"

// The drive is the editor's, and a build without the editor has neither.
#if ENG_DEBUG_UI

namespace {

// A context with nothing drawn in it: the drive speaks to the toolkit's input
// queue, and a frame here is that queue read.
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

[[nodiscard]] std::filesystem::path scriptOf(const char* name, const char* text)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / name;
    std::ofstream(file, std::ios::binary) << text;
    return file;
}

// How many frames of the script saw a double click, each frame `seconds` long.
[[nodiscard]] int doubleClicksAt(const char* script, float seconds)
{
    Screen screen;
    engine::app::EditorDrive drive;
    REQUIRE(drive.load(scriptOf("engine-editor-drive-test.drive", script)));
    ImGuiIO& io = ImGui::GetIO();
    int seen = 0;
    for (int frame = 0; frame < 60; ++frame) {
        io.DeltaTime = seconds;
        const bool quit = drive.feed();
        ImGui::NewFrame();
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            ++seen;
        ImGui::EndFrame();
        if (quit)
            break;
    }
    return seen;
}

} // namespace

TEST_CASE("a scripted double click is one whatever a frame takes")
{
    // A frame a sixtieth of a second long, and one a quarter of a second long:
    // a hosted runner drawing an editor and a running game is nearer the
    // second, and the toolkit's own double click is two presses within 0.3 s.
    CHECK(doubleClicksAt("dbl 400 300\nwait 3\nquit\n", 1.0f / 60.0f) == 1);
    CHECK(doubleClicksAt("dbl 400 300\nwait 3\nquit\n", 0.25f) == 1);
    CHECK(doubleClicksAt("dbl 400 300\nwait 3\nquit\n", 2.0f) == 1);
}

TEST_CASE("two scripted clicks in one place stay two clicks")
{
    // What the double click's allowance must not turn into one: a button
    // pressed twice, as a gate steps a paused game tick by tick.
    CHECK(doubleClicksAt("click 400 300\nwait 4\nclick 400 300\nwait 3\nquit\n", 0.25f) == 0);
    CHECK(doubleClicksAt("dbl 400 300\nwait 4\nclick 400 300\nwait 3\nquit\n", 0.25f) == 1);
}

#endif
