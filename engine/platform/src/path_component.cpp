#include "engine/platform/file.h"
namespace engine::platform {
std::string pathComponent(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool reserved = byte < 0x20 || byte == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' ||
                              c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
        out.push_back(reserved ? '-' : c);
    }
    // Windows drops a trailing dot or space from a name, and a name of dots
    // alone is this directory or its parent.
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    while (!out.empty() && (out.front() == '.' || out.front() == ' '))
        out.erase(out.begin());
    return out.empty() ? std::string("game") : out;
}

} // namespace engine::platform
