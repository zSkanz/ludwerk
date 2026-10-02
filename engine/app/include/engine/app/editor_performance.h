// How the EDITOR runs on this person's machine (ADR 0147, section 6): its own
// frame rate, whether it slows down in the background, the level the Viewport
// is drawn at, and whether Play starts with the settings a player saved.
//
// **Not the game's settings, and not the project's.** `[display]` in
// `project.toml` is how the game's window behaves, and a game's options menu
// tried in Play never reaches the editor's window. This is the other half: a
// heavy game edited on a light laptop, by somebody who does not want to change
// the project to do it.
#pragma once

#include <filesystem>

#include "engine/app/frame_pacing.h"
#include "engine/core/types.h"

namespace engine::app {

struct EditorPerformance
{
    // Frames a second at most. `kMatchMonitor` waits for the display, which is
    // the default; `kUnlimited` waits for nothing.
    static constexpr core::i32 kMatchMonitor = 0;
    static constexpr core::i32 kUnlimited = -1;
    core::i32 frameRate = kMatchMonitor;
    // Whether the editor drops to the background rate when it is not in front
    // or is minimised.
    bool backgroundThrottle = true;
    // The quality level the Viewport is drawn at while editing, as
    // `Enum.GraphicsQuality`'s `Low` to `Ultra`; `kProjectQuality` is whatever
    // the project says.
    static constexpr core::i32 kProjectQuality = -1;
    core::i32 viewportQuality = kProjectQuality;
    // Whether Play starts with the settings a player saved, rather than with
    // the project's defaults.
    bool playWithSavedSettings = false;

    [[nodiscard]] bool operator==(const EditorPerformance&) const = default;
};

// The rates the preference offers, besides the two that are not numbers.
inline constexpr core::i32 kEditorFrameRates[] = {30, 60, 120, 144, 240};

// `<userDir>/editor-performance.json`. Empty when the platform has no user
// directory. Per user for the reason `appearance.json` is: it is a thing about
// this machine, and a second project should not ask again.
[[nodiscard]] std::filesystem::path editorPerformanceFile();

// Reads it. A file that is not there or does not parse is the defaults, and a
// value out of range is the default for that value.
[[nodiscard]] EditorPerformance loadEditorPerformance(const std::filesystem::path& file);

// Writes it. False when there is nowhere to write.
[[nodiscard]] bool saveEditorPerformance(const std::filesystem::path& file, const EditorPerformance& performance);

// The pacing the editor's window runs at: `base` -- what the command line and
// the engine's defaults came to -- with the preference in place of whatever
// `flags` did not name. A flag the editor was started with stands over a
// preference, as it stands over everything.
struct EditorPacingFlags
{
    bool vsync = false;
    bool maxFrameRate = false;
    bool backgroundFrameRate = false;
};
[[nodiscard]] FramePacing editorPacing(const EditorPerformance& performance, const FramePacing& base,
                                       EditorPacingFlags flags = {}) noexcept;

// What this process is running by, and a count that moves whenever it is
// changed -- so the frame loop applies a change made in Preferences without
// either knowing the other. Setting it writes no file: the dialog that changed
// it saves it, as it saves the appearance, and a test that sets one does not
// reach into the user's own folder.
[[nodiscard]] const EditorPerformance& editorPerformance() noexcept;
[[nodiscard]] core::u64 editorPerformanceRevision() noexcept;
void setEditorPerformance(const EditorPerformance& performance);
// Reads the user's file into the process's copy. Once, when an editor starts.
void loadEditorPerformanceForProcess();

} // namespace engine::app
