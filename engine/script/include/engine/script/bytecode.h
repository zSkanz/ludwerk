#pragma once

// **A script is source or bytecode, and every loader takes either** (ADR 0112).
//
// An exported game carries its scripts compiled, so what a player reads is
// bytecode; the editor, `engine dev` and every test read source. One function
// turns either into what `luau_load` wants, so the require path and the script
// path cannot disagree about which they accept -- and a build with no compiler
// (the `shipping` profile, ADR 0002) runs a compiled game rather than refusing
// every script in it.

#include <optional>
#include <string>
#include <string_view>

#include "engine/core/error.h"

namespace engine::script {

// A compiled script's file: `init.luau` is packaged as `init.luauc`, and a
// loader asked for the first finds the second.
inline constexpr std::string_view CompiledExtension = ".luauc";

// Whether `chunk` is bytecode rather than source. Bytecode begins with its
// version byte and then its types version: a control character (the current
// version, nine, IS the tab) followed by a number below four. No text begins
// with that pair; a compile error's bytecode begins with a zero.
[[nodiscard]] bool isBytecode(std::string_view chunk) noexcept;

// What `luau_load` is given for `chunk`: the chunk itself when it is bytecode
// whose version this VM reads, compiled with the runtime's options when it is
// source. `chunkName` only names the script in an error.
//
// Errors: `script.err.bytecode_version` for bytecode another engine build
// wrote, `script.err.no_compiler` for source in a build without a compiler. A
// syntax error is NOT one: it compiles to bytecode carrying the message, and
// `luau_load` reports it as it always did.
[[nodiscard]] std::optional<core::EngineError> bytecodeOf(std::string_view chunk, std::string_view chunkName,
                                                          std::string& outBytecode);

// Compiles `source` the way a package carries it: the runtime's options, with
// debug information at level 1 -- lines survive, so an error in a player's log
// names the script and the line, and local names do not. False with the
// compiler's message for a script that does not compile, and in a build
// without a compiler.
[[nodiscard]] bool compileForPackage(std::string_view source, std::string& outBytecode, std::string& outError);

} // namespace engine::script
