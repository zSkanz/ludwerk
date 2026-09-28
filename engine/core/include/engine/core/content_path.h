// **Every path a script or a content file hands the engine**, checked once, in
// one place (audit F2 to F5): `require`, a scene, a stamp, a texture, a glTF's
// companion file, an `asset://` name. A path that came from outside the engine
// is a request to read something under a root the engine chose -- the
// project's `content/`, its `src/`, a model's own folder -- never anywhere
// else on the machine.
//
// What is refused, whatever the platform: an absolute path or a root name (a
// leading `/`, a drive such as `C:`, a UNC `//host`), a backslash (Windows reads
// it as a separator, so `a\..\..\b` would pass a check made on `/` alone), a
// `:` anywhere (a drive, an alternate data stream), a control character, a
// device name Windows keeps in every folder (`CON`, `NUL`, `COM1`...), and a
// `..` that climbs above the root. `.` and a `..` that stays under it are
// folded away.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace engine::core {

// The path as `/`-separated segments with nothing to fold, or nothing when it
// may not be followed. Never empty when it answers.
[[nodiscard]] std::optional<std::string> safeRelativePath(std::string_view path);

// `root / path` for a path `safeRelativePath` accepts, or nothing. The answer
// is under `root` by construction; nothing on disk is consulted.
[[nodiscard]] std::optional<std::filesystem::path> resolveUnder(const std::filesystem::path& root,
                                                                std::string_view path);

} // namespace engine::core
