// **The editor, driven from a file** (`--editor-drive=FILE`): a development
// instrument that puts input into ImGui itself, frame by frame, so a picture
// of any state of the editor can be taken with nobody at the machine.
//
// E1 recorded that the ImGui shell cannot render headlessly, and it still
// cannot; what it can do is run in a window that nobody touches. Real input
// injected through the OS works (E8) but it moves the person's own cursor and
// needs their window focused. This one reaches ImGui and nothing else: the
// mouse the editor sees is the script's, wherever the real one is.
//
// One step a line; `#` starts a comment. Coordinates are the window's own
// pixels, the ones ImGui lays out in.
//
//     wait 30             frames to do nothing
//     move 120 40         the pointer, with no button
//     click 120 40        left press and release there
//     rclick 120 40       the same, right button
//     dbl 120 40          a double click
//     drag 10 10 200 10   press at one point, release at the other
//     wheel -3            scroll, lines
//     key ctrl+shift+p    a chord: ctrl, shift, alt and super, then a key
//     text hello world    characters typed, as a keyboard would
//     shot name           logs "[drive] shot name", then waits a moment,
//                         so whoever reads the log can photograph the window
//     quit                asks the editor to close
//
// Not in a shipping build: the editor it drives is not there either.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine::app {

class EditorDrive
{
public:
    // False when the file cannot be read; the steps it did read are kept.
    bool load(const std::filesystem::path& file);

    [[nodiscard]] bool active() const noexcept { return next_ < steps_.size(); }

    // Feeds this frame's events into the current ImGui context. Called between
    // the platform backend's new frame and `ImGui::NewFrame`, so what it adds
    // lands after the real input and wins. True when the script asked to quit.
    bool feed();

private:
    struct Step
    {
        std::string verb;
        std::vector<std::string> words;
    };

    std::vector<Step> steps_;
    std::size_t next_ = 0;
    // Frames still to wait, and the phase within a multi-frame step (a click
    // is a move, a press and a release on three frames).
    int waiting_ = 0;
    int phase_ = 0;
    // Where the script's pointer is, sent again every frame: the platform
    // backend queues the real pointer each frame the window has focus, and
    // a scripted press would otherwise land wherever the person's mouse is.
    float pointerX_ = -1.0f;
    float pointerY_ = -1.0f;
};

} // namespace engine::app
