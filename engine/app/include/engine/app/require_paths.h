#pragma once

// What a string handed to `require` names (api-design.md section 1.3).
//
// **One rule, read in two places.** The world host resolves a require when a
// script runs; the script editor's checker resolves the same require to say
// what it returns. They were two rules -- the checker followed a require only
// through the tree (`script.Parent.Module`) and took a path for something it
// could not know -- and a project that requires by path, which is what a
// project laid out as files does, opened in the editor as a page of unknown
// types. Both now ask this.

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace engine::app {

// A `.luaurc`'s `aliases`: `std` to `.engine/types/std`.
using RequireAliases = std::unordered_map<std::string, std::string>;

// `src/client/hud.luau` is in `src/client`; a file at the root is in ``.
[[nodiscard]] std::string_view requireDirectoryOf(std::string_view path);

// The project-relative file `specifier` names when written in the file
// `fromPath`, or false when it names none.
//
// - `./x` and `../x` are beside the requiring file; `@self/x` is the same.
// - `@name/x` is whatever `aliases` says `name` is.
// - Anything else is from the project's root: one place to look is one answer.
//
// The extension is added rather than required, and three spellings are tried
// in a fixed order so the answer never depends on which file was made first:
// `x.luau`, `x.module.luau` (a module outside `src/shared` says so in its
// name), `x/init.luau`. `present` says whether the project has a file.
//
// `.` and `..` are resolved without touching the disk, and a specifier cannot
// leave the project by spelling enough `..`s.
[[nodiscard]] bool resolveRequire(std::string_view fromPath, std::string_view specifier, const RequireAliases& aliases,
                                  const std::function<bool(const std::string&)>& present, std::string& outPath);

} // namespace engine::app
