// **The editor, driven from a file** (`--editor-drive=FILE`): a development
// instrument that puts input into ImGui itself, frame by frame, so a picture
// of any state of the editor can be taken with nobody at the machine.
//
// The first editor recorded that the ImGui shell cannot render headlessly, and it still
// cannot; what it can do is run in a window that nobody touches. Real input
// injected through the OS works but it moves the person's own cursor and
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
//     key ctrl+shift+p    a chord: ctrl, shift, alt and super, then a key --
//                         a letter, a digit, f1-f12, or a name (enter,
//                         escape, comma, grave, pagedown...)
//     text hello world    characters typed, as a keyboard would
//     sleep 2000          milliseconds of the clock, for a game that follows
//                         it: a character that has to have landed. Frames
//                         are no measure of that -- a quick machine draws
//                         a thousand of them in the second a slow one draws
//                         thirty
//     idle                waits for the editor to finish what it began: a
//                         shader being compiled. Not a number of frames --
//                         how long a compile takes is the machine's, and a
//                         picture that waited a count would be a picture of
//                         how fast the machine is
//     shot name           logs "[drive] shot name", then waits a moment,
//                         so whoever reads the log can photograph the window
//                         -- and, in a run with no window
//                         (`--editor-shots=DIR`), is the picture itself:
//                         the frame is written to `DIR/name.png`
//     quit                asks the editor to close
//
// Not in a shipping build: the editor it drives is not there either.
#pragma once

#include <filesystem>
#include <functional>
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

    // **A run that writes its own pictures** waits two frames at a `shot` and
    // not ninety: nobody is reading the log with a camera.
    void setWritesPictures(bool writes) noexcept { writesPictures_ = writes; }
    // What `idle` waits on: true while the editor has work it began and has
    // not finished.
    void setBusy(std::function<bool()> busy) { busy_ = std::move(busy); }
    // The name of the `shot` this frame's `feed` reached, once: empty when
    // there was none, and after it has been asked for.
    [[nodiscard]] std::string takeShot()
    {
        std::string name = std::move(shot_);
        shot_.clear();
        return name;
    }

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
    // The toolkit's own double click interval, kept while a `dbl` is being
    // pressed and put back at the step after it; negative when none is.
    float doubleClickTime_ = -1.0f;
    // Where the script's pointer is, sent again every frame: the platform
    // backend queues the real pointer each frame the window has focus, and
    // a scripted press would otherwise land wherever the person's mouse is.
    float pointerX_ = -1.0f;
    float pointerY_ = -1.0f;
    bool writesPictures_ = false;
    std::string shot_;
    std::function<bool()> busy_;
    // When a `sleep` ends, as the steady clock's count of milliseconds;
    // nought while none is running.
    long long sleepUntil_ = 0;
};

} // namespace engine::app
