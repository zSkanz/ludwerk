#pragma once

#include <string>
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
// This replaces the bootstrap's process-wide SetConsoleOutputCP, which worked but changed
// state belonging to the user's console rather than to us.
void writeConsole(ConsoleStream stream, std::string_view utf8);

// **Whether the console this process writes to goes when it does**: true on
// Windows for a program started by a double click, whose console window is
// its own and closes as it exits -- so a last error printed there is one
// nobody reads. False from a terminal, and everywhere else.
[[nodiscard]] bool consoleClosesWithProcess() noexcept;

// **Whether what this process prints is read by nobody**: a phone's
// app, which has no console; a game started by a double click, with no
// console or one that closes with it; a desktop entry, whose output goes to
// nowhere. False from a terminal and wherever the output is a pipe or a file
// -- a test, a script, CI -- where somebody is reading and a dialog would
// only be in the way.
[[nodiscard]] bool outputGoesUnread() noexcept;

// What one read of the standard input found.
enum class InputRead : core::u8
{
    // Bytes, appended to `out`.
    Data,
    // Nothing within the time given; the input is still open.
    Nothing,
    // Closed, or there is none: nothing more will come.
    Ended,
};

// **The standard input, read without the C library**: at most `timeoutMs`
// waiting for bytes, and what came appended to `out`.
//
// Not `std::cin` nor `fgets`, because both take the C library's lock on
// `stdin` for as long as they wait -- and `exit()` takes every stream's lock
// to flush it. A thread waiting for a line nobody will type then holds the
// whole process open at exit, forever, at no CPU: a server started with an
// open pipe it never reads from, told to run five frames and end, did exactly
// that. Here the wait is the system's own, bounded, and holds nothing.
[[nodiscard]] InputRead readStandardInput(std::string& out, core::u32 timeoutMs);

} // namespace engine::platform
