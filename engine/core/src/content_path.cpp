#include "engine/core/content_path.h"

#include <array>
#include <cctype>
#include <vector>

namespace engine::core {
namespace {

// The names Windows answers in every folder, with or without an extension.
[[nodiscard]] bool deviceName(std::string_view segment)
{
    const std::string_view stem = segment.substr(0, segment.find('.'));
    if (stem.size() < 3 || stem.size() > 4)
        return false;
    std::string upper;
    for (const char c : stem)
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    constexpr std::array<std::string_view, 4> Plain{"CON", "PRN", "AUX", "NUL"};
    for (const std::string_view name : Plain) {
        if (upper == name)
            return true;
    }
    return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' &&
           upper[3] <= '9';
}

} // namespace

std::optional<std::string> safeRelativePath(std::string_view path)
{
    if (path.empty() || path.front() == '/')
        return std::nullopt;
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\' || c == ':' || byte < 0x20 || byte == 0x7F)
            return std::nullopt;
    }

    std::vector<std::string_view> stack;
    std::string_view::size_type start = 0;
    for (std::string_view::size_type index = 0; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/')
            continue;
        const std::string_view segment = path.substr(start, index - start);
        start = index + 1;
        if (segment.empty() || segment == ".")
            continue;
        if (segment == "..") {
            if (stack.empty())
                return std::nullopt;
            stack.pop_back();
            continue;
        }
        // A segment of dots other than those two, or one ending in a dot or a
        // space, names the same file as another on Windows.
        if (segment.back() == '.' || segment.back() == ' ' || deviceName(segment))
            return std::nullopt;
        stack.push_back(segment);
    }

    std::string out;
    for (const std::string_view segment : stack) {
        if (!out.empty())
            out.push_back('/');
        out.append(segment);
    }
    if (out.empty())
        return std::nullopt;
    return out;
}

std::optional<std::filesystem::path> resolveUnder(const std::filesystem::path& root, std::string_view path)
{
    const std::optional<std::string> safe = safeRelativePath(path);
    if (!safe.has_value())
        return std::nullopt;
    // `u8path`'s successor: the text is UTF-8 whatever the platform's own
    // narrow encoding is.
    const std::u8string text(safe->begin(), safe->end());
    return root / std::filesystem::path(text);
}

} // namespace engine::core
