#include "engine/app/editor_drive.h"

#if ENG_DEBUG_UI
#include <imgui.h>
#endif

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "engine/core/log.h"
#include "engine/core/text_key.h"

namespace engine::app {
#if ENG_DEBUG_UI
namespace {

[[nodiscard]] float number(const std::vector<std::string>& words, std::size_t index)
{
    return index < words.size() ? std::strtof(words[index].c_str(), nullptr) : 0.0f;
}

[[nodiscard]] ImGuiKey keyNamed(const std::string& name)
{
    if (name.size() == 1) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
        if (c >= 'a' && c <= 'z')
            return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'a'));
        if (c >= '0' && c <= '9')
            return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
    }
    if (name.size() >= 2 && (name[0] == 'f' || name[0] == 'F')) {
        const int index = std::atoi(name.c_str() + 1);
        if (index >= 1 && index <= 12)
            return static_cast<ImGuiKey>(ImGuiKey_F1 + index - 1);
    }
    static const std::pair<const char*, ImGuiKey> Named[] = {
        {"enter", ImGuiKey_Enter},         {"escape", ImGuiKey_Escape},     {"tab", ImGuiKey_Tab},
        {"backspace", ImGuiKey_Backspace}, {"delete", ImGuiKey_Delete},     {"space", ImGuiKey_Space},
        {"up", ImGuiKey_UpArrow},          {"down", ImGuiKey_DownArrow},    {"left", ImGuiKey_LeftArrow},
        {"right", ImGuiKey_RightArrow},    {"home", ImGuiKey_Home},         {"end", ImGuiKey_End},
        {"pageup", ImGuiKey_PageUp},       {"pagedown", ImGuiKey_PageDown},
    };
    for (const auto& [text, key] : Named) {
        if (name == text)
            return key;
    }
    return ImGuiKey_None;
}

// The chord's modifiers and its key, from `ctrl+shift+p`.
struct Chord
{
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool super = false;
    ImGuiKey key = ImGuiKey_None;
};

[[nodiscard]] Chord chordOf(const std::string& text)
{
    Chord chord;
    std::stringstream parts(text);
    std::string part;
    while (std::getline(parts, part, '+')) {
        if (part == "ctrl")
            chord.ctrl = true;
        else if (part == "shift")
            chord.shift = true;
        else if (part == "alt")
            chord.alt = true;
        else if (part == "super")
            chord.super = true;
        else
            chord.key = keyNamed(part);
    }
    return chord;
}

void setModifiers(ImGuiIO& io, const Chord& chord, bool down)
{
    if (chord.ctrl) {
        io.AddKeyEvent(ImGuiMod_Ctrl, down);
        io.AddKeyEvent(ImGuiKey_LeftCtrl, down);
    }
    if (chord.shift) {
        io.AddKeyEvent(ImGuiMod_Shift, down);
        io.AddKeyEvent(ImGuiKey_LeftShift, down);
    }
    if (chord.alt) {
        io.AddKeyEvent(ImGuiMod_Alt, down);
        io.AddKeyEvent(ImGuiKey_LeftAlt, down);
    }
    if (chord.super) {
        io.AddKeyEvent(ImGuiMod_Super, down);
        io.AddKeyEvent(ImGuiKey_LeftSuper, down);
    }
}

} // namespace
#endif

bool EditorDrive::load(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        if (const std::size_t hash = line.find('#'); hash != std::string::npos)
            line.erase(hash);
        std::stringstream words(line);
        Step step;
        if (!(words >> step.verb))
            continue;
        if (step.verb == "text") {
            // The rest of the line, as typed.
            std::string rest;
            std::getline(words, rest);
            if (!rest.empty() && rest.front() == ' ')
                rest.erase(0, 1);
            step.words.push_back(rest);
        }
        else {
            std::string word;
            while (words >> word)
                step.words.push_back(word);
        }
        steps_.push_back(std::move(step));
    }
    return true;
}

bool EditorDrive::feed()
{
#if !ENG_DEBUG_UI
    // No editor to drive in this build.
    return false;
#else
    ImGuiIO& io = ImGui::GetIO();
    if (pointerX_ >= 0.0f)
        io.AddMousePosEvent(pointerX_, pointerY_);
    if (waiting_ > 0) {
        --waiting_;
        return false;
    }
    if (!active())
        return false;

    const auto point = [&](float x, float y) {
        pointerX_ = x;
        pointerY_ = y;
        io.AddMousePosEvent(x, y);
    };
    const Step& step = steps_[next_];
    const auto advance = [&](int wait = 1) {
        ++next_;
        phase_ = 0;
        waiting_ = wait;
    };

    if (step.verb == "wait") {
        advance(static_cast<int>(number(step.words, 0)));
    }
    else if (step.verb == "move") {
        point(number(step.words, 0), number(step.words, 1));
        advance();
    }
    else if (step.verb == "click" || step.verb == "rclick" || step.verb == "dbl") {
        const int button = step.verb == "rclick" ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
        const int presses = step.verb == "dbl" ? 2 : 1;
        // Move, then press and release once or twice, one event a frame: a
        // press on the frame the pointer arrives is a press on whatever it
        // left.
        if (phase_ == 0)
            point(number(step.words, 0), number(step.words, 1));
        else
            io.AddMouseButtonEvent(button, phase_ % 2 == 1);
        if (++phase_ > presses * 2)
            advance(2);
    }
    else if (step.verb == "drag") {
        if (phase_ == 0)
            point(number(step.words, 0), number(step.words, 1));
        else if (phase_ == 1)
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        else if (phase_ < 8) {
            const float t = static_cast<float>(phase_ - 1) / 6.0f;
            point(number(step.words, 0) + (number(step.words, 2) - number(step.words, 0)) * t,
                  number(step.words, 1) + (number(step.words, 3) - number(step.words, 1)) * t);
        }
        else
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        if (++phase_ > 8)
            advance(2);
    }
    else if (step.verb == "wheel") {
        io.AddMouseWheelEvent(0.0f, number(step.words, 0));
        advance(2);
    }
    else if (step.verb == "key") {
        const Chord chord = chordOf(step.words.empty() ? std::string{} : step.words[0]);
        if (phase_ == 0) {
            setModifiers(io, chord, true);
            if (chord.key != ImGuiKey_None)
                io.AddKeyEvent(chord.key, true);
            phase_ = 1;
        }
        else {
            if (chord.key != ImGuiKey_None)
                io.AddKeyEvent(chord.key, false);
            setModifiers(io, chord, false);
            advance(2);
        }
    }
    else if (step.verb == "text") {
        io.AddInputCharactersUTF8(step.words.empty() ? "" : step.words[0].c_str());
        advance(2);
    }
    else if (step.verb == "shot") {
        // Developer-facing, like everything this instrument says.
        const core::I18nArg args[] = {{"name", step.words.empty() ? std::string{} : step.words[0]}};
        core::log(core::LogLevel::Info, ENG_TR("engine.drive.info.shot"), args);
        // Long enough for a reader polling the log to photograph a still frame.
        advance(90);
    }
    else if (step.verb == "quit") {
        advance();
        return true;
    }
    else {
        const core::I18nArg args[] = {{"verb", step.verb}};
        core::log(core::LogLevel::Warn, ENG_TR("engine.drive.warn.unknown_step"), args);
        advance();
    }
    return false;
#endif
}

} // namespace engine::app
