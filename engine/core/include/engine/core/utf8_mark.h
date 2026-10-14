// **A UTF-8 byte-order mark at the start of a text file is an encoding mark,
// not text.** Notepad writes one, and so do Windows PowerShell's `Out-File`
// and `Set-Content -Encoding utf8` and Visual Studio. The readers of JSON,
// `project.toml` and a script's source skip it, and anything that compares
// or hashes a file against text already read does the same, so the two agree
// whether the mark is there or not.
//
// Only at the very start, and only the UTF-8 spelling. In the middle of a file
// it is a zero-width no-break space in somebody's text.
#pragma once

#include <string>
#include <string_view>

namespace engine::core {

inline constexpr std::string_view Utf8Mark = "\xEF\xBB\xBF";

[[nodiscard]] inline std::string_view withoutUtf8Mark(std::string_view text) noexcept
{
    if (text.starts_with(Utf8Mark))
        text.remove_prefix(Utf8Mark.size());
    return text;
}

inline void dropUtf8Mark(std::string& text)
{
    if (std::string_view(text).starts_with(Utf8Mark))
        text.erase(0, Utf8Mark.size());
}

} // namespace engine::core
