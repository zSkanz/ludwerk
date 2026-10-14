#include "engine/app/doc_summary.h"

#include <cstddef>

namespace engine::app {

std::string docSummary(std::string_view doc)
{
    if (const std::size_t end = doc.find("\n\n"); end != std::string_view::npos)
        doc = doc.substr(0, end);

    std::string plain;
    plain.reserve(doc.size());
    for (std::size_t i = 0; i < doc.size(); ++i) {
        const char c = doc[i];
        if (c == '`')
            continue;
        if (c == '*' && i + 1 < doc.size() && doc[i + 1] == '*') {
            ++i;
            continue;
        }
        plain.push_back(c == '\n' ? ' ' : c);
    }
    return plain;
}

namespace {

// One line of `text`, which has no line break in it, added to `wrapped`.
void wrapLine(std::string& wrapped, std::string_view text, float width,
              const std::function<float(std::string_view)>& measure)
{
    std::size_t lineStart = wrapped.size();
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(' ', at);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view word = text.substr(at, end - at);
        const bool first = wrapped.size() == lineStart;
        if (!first) {
            const std::string candidate = wrapped.substr(lineStart) + " " + std::string(word);
            if (measure(candidate) > width) {
                wrapped.push_back('\n');
                lineStart = wrapped.size();
            }
            else {
                wrapped.push_back(' ');
            }
        }
        wrapped.append(word);
        at = end + 1;
    }
}

} // namespace

std::string wrapAtSpaces(std::string_view text, float width, const std::function<float(std::string_view)>& measure)
{
    std::string wrapped;
    wrapped.reserve(text.size());
    std::size_t at = 0;
    while (true) {
        const std::size_t end = text.find('\n', at);
        wrapLine(wrapped, text.substr(at, end == std::string_view::npos ? end : end - at), width, measure);
        if (end == std::string_view::npos)
            break;
        wrapped.push_back('\n');
        at = end + 1;
    }
    return wrapped;
}

} // namespace engine::app
