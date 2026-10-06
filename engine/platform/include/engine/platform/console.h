#pragma once

#include <string_view>

#include "engine/core/types.h"

namespace engine::platform {

enum class ConsoleStream : core::u8
{
    Out,
    Err,
};

// Writes UTF-8 text to a standard stream.
//
// Exists because writing catalog bytes with fwrite is wrong on Windows: a
// console decodes what it is handed using its own codepage, so the em dash in
// an English string arrives as "ÔÇö" under CP-850 and every accented locale is
// worse. When the stream is an attached console this converts to UTF-16 and
// uses the console's own wide entry point; when it is redirected -- a pipe, a
// file, CI -- the bytes go through untouched, which is what those consumers
// expect.
//
// This replaces M0's process-wide SetConsoleOutputCP, which worked but changed
// state belonging to the user's console rather than to us.
void writeConsole(ConsoleStream stream, std::string_view utf8);

// **Whether the console this process writes to goes when it does**: true on
// Windows for a program started by a double click, whose console window is
// its own and closes as it exits -- so a last error printed there is one
// nobody reads. False from a terminal, and everywhere else.
[[nodiscard]] bool consoleClosesWithProcess() noexcept;

// **Whether what this process prints is read by nobody** (D568): a phone's
// app, which has no console; a game started by a double click, with no
// console or one that closes with it; a desktop entry, whose output goes to
// nowhere. False from a terminal and wherever the output is a pipe or a file
// -- a test, a script, CI -- where somebody is reading and a dialog would
// only be in the way.
[[nodiscard]] bool outputGoesUnread() noexcept;

} // namespace engine::platform
