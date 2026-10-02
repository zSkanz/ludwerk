#include "engine/app/editor_performance.h"

#include <algorithm>
#include <string>

#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"

namespace engine::app {
namespace {

// `Enum.GraphicsQuality`: Low is 0 and Ultra is 3.
constexpr core::i32 kLowestQuality = 0;
constexpr core::i32 kHighestQuality = 3;

struct ProcessState
{
    EditorPerformance performance;
    core::u64 revision = 1;
};

ProcessState& processState() noexcept
{
    static ProcessState state;
    return state;
}

[[nodiscard]] bool offeredRate(core::i64 rate) noexcept
{
    return rate == EditorPerformance::kMatchMonitor || rate == EditorPerformance::kUnlimited ||
           std::find(std::begin(kEditorFrameRates), std::end(kEditorFrameRates), static_cast<core::i32>(rate)) !=
               std::end(kEditorFrameRates);
}

} // namespace

std::filesystem::path editorPerformanceFile()
{
    const std::filesystem::path& userDir = platform::paths().userDir;
    return userDir.empty() ? std::filesystem::path{} : userDir / "editor-performance.json";
}

EditorPerformance loadEditorPerformance(const std::filesystem::path& file)
{
    EditorPerformance performance;
    if (file.empty())
        return performance;

    std::string text;
    if (!platform::readTextFile(file, text))
        return performance;

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(text); !parsed.ok)
        return performance;

    // Each value by itself: one somebody typed wrong does not take the others
    // with it, and a rate this build does not offer is the default rather than
    // a number the dialog cannot show.
    const core::JsonValue root = document.root();
    if (const core::JsonValue rate = root["frame_rate"]; rate.type() == core::JsonType::Number) {
        if (offeredRate(rate.asInteger()))
            performance.frameRate = static_cast<core::i32>(rate.asInteger());
    }
    if (const core::JsonValue throttle = root["background_throttle"]; throttle.type() == core::JsonType::Boolean)
        performance.backgroundThrottle = throttle.asBool();
    if (const core::JsonValue quality = root["viewport_quality"]; quality.type() == core::JsonType::Number) {
        const core::i64 level = quality.asInteger();
        if (level >= kLowestQuality && level <= kHighestQuality)
            performance.viewportQuality = static_cast<core::i32>(level);
    }
    if (const core::JsonValue saved = root["play_with_saved_settings"]; saved.type() == core::JsonType::Boolean)
        performance.playWithSavedSettings = saved.asBool();
    return performance;
}

bool saveEditorPerformance(const std::filesystem::path& file, const EditorPerformance& performance)
{
    if (file.empty())
        return false;

    core::JsonWriter writer;
    writer.beginObject();
    writer.field("frame_rate", static_cast<core::i64>(performance.frameRate));
    writer.field("background_throttle", performance.backgroundThrottle);
    writer.field("viewport_quality", static_cast<core::i64>(performance.viewportQuality));
    writer.field("play_with_saved_settings", performance.playWithSavedSettings);
    writer.endObject();

    (void)platform::createDirectories(file.parent_path());
    return platform::writeTextFile(file, writer.text());
}

FramePacing editorPacing(const EditorPerformance& performance, const FramePacing& base,
                         EditorPacingFlags flags) noexcept
{
    FramePacing pacing = base;
    // **A number is a cap under the display's sync, not instead of it**: 60 on
    // a 144 Hz display is sixty whole frames, and 240 on a 60 Hz one is sixty
    // -- "unlimited" is how to ask for more than the display shows.
    if (!flags.vsync)
        pacing.vsync = performance.frameRate != EditorPerformance::kUnlimited;
    if (!flags.maxFrameRate)
        pacing.maxFrameRate = performance.frameRate > 0 ? static_cast<core::u32>(performance.frameRate) : 0u;
    if (!flags.backgroundFrameRate && !performance.backgroundThrottle)
        pacing.backgroundFrameRate = 0;
    return pacing;
}

const EditorPerformance& editorPerformance() noexcept
{
    return processState().performance;
}

core::u64 editorPerformanceRevision() noexcept
{
    return processState().revision;
}

void setEditorPerformance(const EditorPerformance& performance)
{
    ProcessState& state = processState();
    if (state.performance == performance)
        return;
    state.performance = performance;
    state.revision += 1;
}

void loadEditorPerformanceForProcess()
{
    ProcessState& state = processState();
    state.performance = loadEditorPerformance(editorPerformanceFile());
    state.revision += 1;
}

} // namespace engine::app
