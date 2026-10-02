// The editor's own performance preferences (ADR 0147, section 6): what the
// file keeps, what a broken one is, and what a preference does to the pacing.
#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/app/editor_performance.h"
#include "engine/platform/file.h"

using namespace engine;

namespace {

struct Scratch
{
    std::filesystem::path root;

    explicit Scratch(const char* name)
    {
        root = std::filesystem::temp_directory_path() / "engine-editor-performance-tests" / name;
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root, error);
    }
    ~Scratch()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

} // namespace

TEST_CASE("the editor's performance preferences survive a process")
{
    const Scratch scratch("roundtrip");
    const std::filesystem::path file = scratch.root / "editor-performance.json";

    const app::EditorPerformance chosen{
        .frameRate = 60, .backgroundThrottle = false, .viewportQuality = 1, .playWithSavedSettings = true};
    CHECK(app::saveEditorPerformance(file, chosen));
    CHECK(app::loadEditorPerformance(file) == chosen);

    const app::EditorPerformance unlimited{.frameRate = app::EditorPerformance::kUnlimited};
    CHECK(app::saveEditorPerformance(file, unlimited));
    CHECK(app::loadEditorPerformance(file) == unlimited);
}

TEST_CASE("a missing, broken or out-of-range performance file is the defaults, value by value")
{
    const Scratch scratch("broken");
    const std::filesystem::path file = scratch.root / "editor-performance.json";
    const app::EditorPerformance defaults;

    CHECK(app::loadEditorPerformance(file) == defaults);
    CHECK(app::loadEditorPerformance(std::filesystem::path{}) == defaults);
    CHECK_FALSE(app::saveEditorPerformance(std::filesystem::path{}, defaults));

    REQUIRE(platform::writeTextFile(file, "{ not json"));
    CHECK(app::loadEditorPerformance(file) == defaults);

    // A rate the dialog does not offer and a level there is none of are the
    // defaults; the values beside them are kept.
    REQUIRE(platform::writeTextFile(
        file,
        R"({"frame_rate": 77, "background_throttle": false, "viewport_quality": 9, "play_with_saved_settings": 1})"));
    const app::EditorPerformance read = app::loadEditorPerformance(file);
    CHECK(read.frameRate == app::EditorPerformance::kMatchMonitor);
    CHECK_FALSE(read.backgroundThrottle);
    CHECK(read.viewportQuality == app::EditorPerformance::kProjectQuality);
    CHECK_FALSE(read.playWithSavedSettings);
}

TEST_CASE("the editor's frame rate is the display's by default, a cap under it, or unlimited")
{
    const app::FramePacing base{.vsync = true, .maxFrameRate = 0, .backgroundFrameRate = 10};

    const app::FramePacing matched = app::editorPacing(app::EditorPerformance{}, base);
    CHECK(matched.vsync);
    CHECK(matched.maxFrameRate == 0);
    CHECK(matched.backgroundFrameRate == 10);

    const app::FramePacing capped = app::editorPacing(app::EditorPerformance{.frameRate = 60}, base);
    CHECK(capped.vsync);
    CHECK(capped.maxFrameRate == 60);

    const app::FramePacing unlimited =
        app::editorPacing(app::EditorPerformance{.frameRate = app::EditorPerformance::kUnlimited}, base);
    CHECK_FALSE(unlimited.vsync);
    CHECK(unlimited.maxFrameRate == 0);
    // Still ten a second behind another window: unlimited is about the front.
    CHECK(unlimited.backgroundFrameRate == 10);

    const app::FramePacing awake = app::editorPacing(app::EditorPerformance{.backgroundThrottle = false}, base);
    CHECK(awake.backgroundFrameRate == 0);
}

TEST_CASE("a flag the editor was started with stands over a preference")
{
    // `--no-vsync --max-frame-rate=90 --background-frame-rate=5`.
    const app::FramePacing base{.vsync = false, .maxFrameRate = 90, .backgroundFrameRate = 5};
    const app::EditorPerformance chosen{.frameRate = 30, .backgroundThrottle = false};

    const app::FramePacing all = app::editorPacing(
        chosen, base, app::EditorPacingFlags{.vsync = true, .maxFrameRate = true, .backgroundFrameRate = true});
    CHECK_FALSE(all.vsync);
    CHECK(all.maxFrameRate == 90);
    CHECK(all.backgroundFrameRate == 5);

    // Only the cap was a flag: the sync and the throttle are the preference's.
    const app::FramePacing some = app::editorPacing(chosen, base, app::EditorPacingFlags{.maxFrameRate = true});
    CHECK(some.vsync);
    CHECK(some.maxFrameRate == 90);
    CHECK(some.backgroundFrameRate == 0);
}

TEST_CASE("a changed preference moves the revision, and the same one does not")
{
    const app::EditorPerformance before = app::editorPerformance();
    const core::u64 revision = app::editorPerformanceRevision();

    app::setEditorPerformance(before);
    CHECK(app::editorPerformanceRevision() == revision);

    app::EditorPerformance changed = before;
    changed.viewportQuality = before.viewportQuality == 0 ? 1 : 0;
    app::setEditorPerformance(changed);
    CHECK(app::editorPerformanceRevision() == revision + 1);
    CHECK(app::editorPerformance() == changed);

    app::setEditorPerformance(before);
    CHECK(app::editorPerformance() == before);
}
