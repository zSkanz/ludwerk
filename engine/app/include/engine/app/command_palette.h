// **The command palette and quick open** (the owner's queue of 2026-09-27, Q2):
// every action the editor has, run by typing part of its name, and every file
// the project has, opened the same way -- `Ctrl+Shift+P` (or `F1`) and `Ctrl+P`,
// the two keys a person who lives in VS Code presses without thinking.
//
// **The palette owns no actions.** The shell hands it the list each frame --
// the same actions its menus, its ribbon and its panels already run -- so
// nothing here is a second way to do anything, and a command added to a menu
// is found by typing as soon as it is listed there.
#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::app {

// One thing the palette can run: a command, or -- in quick open -- a file or
// an instance to open.
struct PaletteItem
{
    // What is searched and shown: "File: Save Scene", "Insert: Part",
    // "main.scene.json".
    std::string title;
    // Shown quieter beside the title: the file's folder, an instance's path.
    std::string detail;
    // "Ctrl+S", drawn as keys at the row's right end. Empty for none.
    std::string shortcut;
    // The row's icon in the shell's atlas; empty for none.
    std::string icon;
    bool enabled = true;
    std::function<void()> run;
    // What a menu finds this command by, for a title that changes with the
    // state ("Undo Move"). Not shown, not searched.
    std::string id;
};

// How well `query` matches `text`, or below zero for not at all. Every
// character of the query in order, case ignored; a run of consecutive
// characters, a match at the start of a word and a shorter text all score
// higher -- so "sv" finds "Save" before "Stamp: Convert", and "ins part" finds
// "Insert: Part". `positions`, when given, receives the matched offsets for
// highlighting.
[[nodiscard]] int fuzzyScore(std::string_view query, std::string_view text, std::vector<int>* positions = nullptr);

class CommandPalette
{
public:
    enum class Mode
    {
        Commands,
        Files,
    };

    void open(Mode mode);
    [[nodiscard]] bool isOpen() const noexcept { return open_; }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }

    // Draws the palette when it is open, over everything, and runs what is
    // chosen. `commands` is the full list for `Mode::Commands` and `files` for
    // `Mode::Files`; typing `>` in quick open turns it into the palette, as it
    // does in the editor this follows.
    // `drawIcon` draws one of the shell's icons inline at a size; the palette
    // has no atlas of its own.
    void draw(std::span<const PaletteItem> commands, std::span<const PaletteItem> files,
              const std::function<void(std::string_view icon, float size)>& drawIcon);

private:
    bool open_ = false;
    bool focusInput_ = false;
    Mode mode_ = Mode::Commands;
    std::string query_;
    int selected_ = 0;
    // Set when the KEYBOARD (or a new query) moved the selection, which is the
    // only time the list follows it: following it every frame fought the
    // mouse wheel back to the selected row (the owner).
    bool revealSelected_ = false;
    int shownFrames_ = 0;
    // The titles of the commands run most recently, newest first: an empty
    // palette lists these above everything else.
    std::vector<std::string> recent_;
};

} // namespace engine::app
