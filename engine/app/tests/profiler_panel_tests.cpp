// The Profiler panel with no window (ADR 0210): the same drawing the editor's
// dock does, run against an ImGui that draws to nowhere, with a mouse and a
// keyboard this file moves. What it can say: the panel draws a run and a
// frame of it without tripping ImGui's own checks, pauses, goes to the worst
// frame and the next, chooses the frame pressed on the strip, steps with the
// arrows, shows each of its five tabs, writes its two switches, and opens a
// capture from a file. What it cannot: what any of it looks like -- no pixel
// is made here; `tests/editorshots/profiler.drive` is where one is.

#include <doctest/doctest.h>
#include <filesystem>
#include <imgui.h>
#include <optional>
#include <set>
#include <string>

#include "engine/app/debug_overlay.h"
#include "engine/app/profile_history.h"

using namespace engine;

namespace {

// A frame of the run: `ms` long in all, with scripts, passes, a heap and a
// match in it.
[[nodiscard]] app::ProfileSample frameOf(core::f32 ms, core::u64 corrections)
{
    app::ProfileSample sample;
    sample.scopes.push_back({"frame", -1, ms, 1});
    sample.scopes.push_back({"simulation", 0, ms * 0.3f, 2});
    sample.scopes.push_back({"scripts.render", 0, ms * 0.2f, 1});
    sample.scopes.push_back({"render.world", 0, ms * 0.4f, 1});
    sample.scopes.push_back({"render.shadow", 3, ms * 0.1f, 4});
    sample.entries = 9;
    sample.scripts.push_back({"Workspace.Spinner", ms * 0.15f, 3});
    sample.scripts.push_back({"Workspace.Idle", ms * 0.05f, 1});
    sample.lines.push_back({"Workspace.Spinner", 12, 3});
    sample.lines.push_back({"Workspace.Spinner", 14, 1});
    sample.passes.push_back({"shadow", 0.4f});
    sample.passes.push_back({"forward", 1.2f});
    sample.gpuTimingMs = 0.3f;
    sample.memory = {
        .scriptBytes = 3u << 20, .textureBytes = 80u << 20, .textures = 40, .bufferBytes = 9u << 20, .buffers = 12};
    sample.scriptMemory.push_back({"Workspace.Spinner", 2u << 20});
    sample.scriptMemory.push_back({"", 1u << 20});
    sample.network.connected = true;
    sample.network.peers = 1;
    sample.network.pingMs = 48.0f;
    sample.network.snapshotsPerSecond = 30.0f;
    sample.network.corrections = corrections;
    sample.network.playerPredicted = {true, true, false, false};
    sample.network.playerCorrections = {corrections, 0, 0, 0};
    return sample;
}

constexpr core::usize Frames = 100;
// The longest frame of the run, and the next longest.
constexpr core::usize Worst = 60;
constexpr core::usize NextWorst = 30;

struct Desk
{
    app::Profiler profiler{Frames};
    float mouseX = -100.0f;
    float mouseY = -100.0f;
    bool down = false;

    Desk()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400.0f, 600.0f);
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~Desk() { ImGui::DestroyContext(); }

    void fill()
    {
        core::u64 corrections = 0;
        for (core::usize index = 0; index < Frames; ++index) {
            if (index % 25 == 24)
                ++corrections;
            profiler.history.push(frameOf(index == Worst ? 40.0f : (index == NextWorst ? 25.0f : 12.0f), corrections));
        }
    }

    // One frame of the panel, in a window that fills the display.
    void frame()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouseX, mouseY);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        if (ImGui::Begin("Profiler", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
            app::drawProfilerPanel(profiler);
        ImGui::End();
        ImGui::EndFrame();
    }

    void click(float x, float y)
    {
        mouseX = x;
        mouseY = y;
        down = false;
        frame();
        frame();
        down = true;
        frame();
        frame();
        down = false;
        frame();
        frame();
    }

    void key(ImGuiKey which)
    {
        ImGui::GetIO().AddKeyEvent(which, true);
        frame();
        ImGui::GetIO().AddKeyEvent(which, false);
        frame();
    }
};

} // namespace

TEST_CASE("ADR 0210: a profiler measures only while somebody reads it")
{
    app::Profiler profiler;
    // Shut: nothing, whatever its switches say.
    profiler.gpuPasses = true;
    profiler.lines = true;
    CHECK_FALSE(profiler.measures(false, true));
    // On screen and reading the run.
    profiler.watching = true;
    CHECK(profiler.measures(false, true));
    // The interface hidden over it: nobody is reading.
    CHECK_FALSE(profiler.measures(false, false));
    // Reading a file: the run is not what is shown.
    profiler.file = "somewhere.profile.json";
    CHECK_FALSE(profiler.measures(false, true));
    // A capture asked for is a reader, with no panel at all.
    profiler.watching = false;
    profiler.file.clear();
    CHECK(profiler.measures(true, false));
}

TEST_CASE("ADR 0210: the Profiler panel, driven with no window: it pauses, finds the worst frame, chooses, steps, "
          "shows its tabs and writes its switches")
{
    Desk desk;
    // With no frame yet: a line of words, and nothing tripped.
    desk.frame();
    desk.frame();
    desk.fill();
    desk.frame();
    desk.frame();
    app::ProfileHistory& history = desk.profiler.history;
    REQUIRE(history.size() == Frames);
    CHECK_FALSE(history.held());

    // **The bar of buttons is found by pressing down the panel** at its left
    // edge until the run is held: where it is is the layout's to say.
    float bar = -1.0f;
    for (float y = 22.0f; y < 80.0f && bar < 0.0f; y += 3.0f) {
        desk.click(20.0f, y);
        if (history.held())
            bar = y;
    }
    REQUIRE(bar > 0.0f);
    // Held, nothing comes in.
    history.push(frameOf(12.0f, 9));
    CHECK(history.size() == Frames);
    CHECK_FALSE(history.chosen().has_value());

    // **Along that bar**: the worst frame, then the next worst, then the two
    // switches. Each is found by what it does.
    bool wentToWorst = false;
    bool wentToNext = false;
    bool passes = false;
    bool lines = false;
    for (float x = 70.0f; x < 900.0f; x += 6.0f) {
        const std::optional<core::usize> before = history.chosen();
        desk.click(x, bar);
        const std::optional<core::usize> after = history.chosen();
        if (!wentToWorst && after.has_value()) {
            // The first thing that chooses a frame chooses the longest.
            CHECK(*after == Worst);
            wentToWorst = true;
        }
        else if (wentToWorst && !wentToNext && before == std::optional<core::usize>{Worst} && after != before) {
            REQUIRE(after.has_value());
            CHECK(*after == NextWorst);
            wentToNext = true;
        }
        passes = passes || desk.profiler.gpuPasses;
        lines = lines || desk.profiler.lines;
    }
    CHECK(wentToWorst);
    CHECK(wentToNext);
    CHECK(passes);
    CHECK(lines);
    // A frame chosen is a run held.
    CHECK(history.held());

    // **The strip**: a press on it chooses the frame under the pointer. The
    // history is full, so its bars fill the panel's width: the window's
    // padding either side of a hundred of them.
    const float left = ImGui::GetStyle().WindowPadding.x;
    const float each = (1400.0f - 2.0f * left) / static_cast<float>(Frames);
    const float at = left + each * 20.5f;
    history.choose(Worst);
    float strip = -1.0f;
    for (float y = bar + 14.0f; y < 260.0f && strip < 0.0f; y += 4.0f) {
        desk.click(at, y);
        if (history.chosen() == std::optional<core::usize>{20})
            strip = y;
    }
    REQUIRE(strip > 0.0f);
    desk.click(left + each * 71.5f, strip);
    CHECK(history.chosen() == std::optional<core::usize>{71});

    // **The arrows step**, a frame either way, while the panel has the keys.
    desk.key(ImGuiKey_RightArrow);
    CHECK(history.chosen() == std::optional<core::usize>{72});
    desk.key(ImGuiKey_LeftArrow);
    desk.key(ImGuiKey_LeftArrow);
    CHECK(history.chosen() == std::optional<core::usize>{70});

    // **The tree is heaviest first until the Scope column's header is
    // pressed**, and Time's header has it back: the header is the first row
    // under the tabs that changes the order when pressed at its left.
    CHECK(desk.profiler.order == app::ProfileOrder::Heaviest);
    float header = -1.0f;
    for (float y = strip + 40.0f; y < strip + 160.0f && header < 0.0f; y += 4.0f) {
        desk.click(40.0f, y);
        if (desk.profiler.order == app::ProfileOrder::Entered)
            header = y;
    }
    REQUIRE(header > 0.0f);
    // Time is the second column, four ninths of the way across.
    desk.click(1400.0f * 0.45f, header);
    CHECK(desk.profiler.order == app::ProfileOrder::Heaviest);
    // A branch pressed folds, and the tree is drawn folded without tripping
    // anything: the first row under the header is the frame's.
    desk.click(24.0f, header + 22.0f);
    desk.frame();
    desk.click(24.0f, header + 22.0f);
    desk.frame();

    // **Every tab is drawn**, of the frame chosen: pressed along the rows
    // under the strip until all five have been the one shown.
    std::set<app::ProfilerTab> shown{desk.profiler.tab};
    for (float y = strip + 40.0f; y < strip + 200.0f && shown.size() < 5; y += 5.0f) {
        for (float x = 16.0f; x < 420.0f && shown.size() < 5; x += 12.0f) {
            desk.click(x, y);
            shown.insert(desk.profiler.tab);
            // Each drawn for a few frames, with a frame chosen.
            desk.frame();
        }
    }
    CHECK(shown.size() == 5);

    // And of the run as a whole, with none chosen: back at the bar's first
    // button, which lets go.
    desk.click(20.0f, bar);
    CHECK_FALSE(history.held());
    CHECK_FALSE(history.chosen().has_value());
    desk.frame();
}

TEST_CASE("ADR 0210: the Profiler panel opens a capture written to a file, and goes back to the run")
{
    Desk desk;
    desk.fill();
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "engine-profiler-panel.profile.json";
    std::filesystem::remove(file);
    {
        // A capture from somewhere else: seven frames, one of them long.
        app::ProfileHistory elsewhere(16);
        for (int index = 0; index < 7; ++index)
            elsewhere.push(frameOf(index == 4 ? 33.0f : 8.0f, 0));
        REQUIRE(elsewhere.save(file));
    }
    desk.frame();
    desk.frame();

    // The bar, found as before: by the run being held from its first button.
    float bar = -1.0f;
    for (float y = 22.0f; y < 80.0f && bar < 0.0f; y += 3.0f) {
        desk.click(20.0f, y);
        if (desk.profiler.history.held())
            bar = y;
    }
    REQUIRE(bar > 0.0f);
    // **The field is found by pressing along the bar** until ImGui wants
    // text; the path is typed into it.
    float field = -1.0f;
    for (float x = 500.0f; x < 1300.0f && field < 0.0f; x += 20.0f) {
        desk.click(x, bar);
        if (ImGui::GetIO().WantTextInput)
            field = x;
    }
    REQUIRE(field > 0.0f);
    ImGui::GetIO().AddInputCharactersUTF8(file.generic_string().c_str());
    desk.frame();
    desk.frame();
    // And the button after it, found by the file being read.
    for (float x = field; x < 1396.0f && desk.profiler.file.empty(); x += 6.0f)
        desk.click(x, bar);
    REQUIRE_FALSE(desk.profiler.file.empty());
    CHECK(desk.profiler.fileError == app::ProfileFileError::None);
    // What is shown is the file: its frames, held, and nothing measured.
    CHECK(desk.profiler.history.size() == 7);
    CHECK_FALSE(desk.profiler.measures(false, true));
    desk.frame();
    // Its worst frame is found as a run's is.
    REQUIRE(desk.profiler.history.chooseWorst());
    CHECK(desk.profiler.history.chosen() == std::optional<core::usize>{4});
    desk.frame();
    desk.frame();

    // **Back to the run**: the bar's first button while a file is read.
    desk.click(20.0f, bar);
    REQUIRE(desk.profiler.file.empty());
    CHECK(desk.profiler.history.size() == 0);
    CHECK_FALSE(desk.profiler.history.held());
    desk.frame();
    std::filesystem::remove(file);
}
