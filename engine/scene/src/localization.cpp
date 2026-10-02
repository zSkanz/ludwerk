#include "engine/scene/localization.h"

#include <algorithm>

namespace engine::scene {
namespace {

[[nodiscard]] char lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] char upper(char c) noexcept
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// The language of a locale: what is before its first separator.
[[nodiscard]] std::string_view languageOf(std::string_view locale) noexcept
{
    const core::usize cut = locale.find('-');
    return cut == std::string_view::npos ? locale : locale.substr(0, cut);
}

bool loadInto(std::map<std::string, core::Catalog, std::less<>>& catalogs, std::string_view locale,
              std::string_view json, std::string* diagnostic)
{
    const std::string name = canonicalLocale(locale);
    if (name.empty()) {
        if (diagnostic != nullptr)
            *diagnostic = "a catalog's file is named for its locale";
        return false;
    }
    // Into a catalog of its own first: a file that does not parse leaves the
    // words the locale had.
    core::Catalog catalog;
    const core::Catalog::LoadResult result = catalog.loadFromJson(json, name);
    if (!result.ok) {
        if (diagnostic != nullptr)
            *diagnostic = result.diagnostic;
        return false;
    }
    catalog.setLocale(name);
    catalogs[name] = std::move(catalog);
    return true;
}

[[nodiscard]] const core::Catalog* find(const std::map<std::string, core::Catalog, std::less<>>& catalogs,
                                        std::string_view locale)
{
    const auto found = catalogs.find(locale);
    return found != catalogs.end() ? &found->second : nullptr;
}

} // namespace

std::string canonicalLocale(std::string_view locale)
{
    std::string out;
    out.reserve(locale.size());
    core::usize part = 0;
    core::usize partStart = 0;
    for (core::usize index = 0; index <= locale.size(); ++index) {
        const bool end = index == locale.size();
        const char c = end ? '-' : locale[index];
        if (c != '-' && c != '_') {
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            const bool digit = c >= '0' && c <= '9';
            if (!letter && !digit)
                return {};
            continue;
        }
        const std::string_view piece = locale.substr(partStart, index - partStart);
        if (piece.empty())
            return part == 0 || !end ? std::string{} : out;
        if (part > 0)
            out.push_back('-');
        // The language in lower case; a two-letter region in upper; a script
        // -- four letters -- with its first capital; the rest as lower.
        for (core::usize at = 0; at < piece.size(); ++at) {
            if (part == 0)
                out.push_back(lower(piece[at]));
            else if (piece.size() == 2)
                out.push_back(upper(piece[at]));
            else if (piece.size() == 4)
                out.push_back(at == 0 ? upper(piece[at]) : lower(piece[at]));
            else
                out.push_back(lower(piece[at]));
        }
        ++part;
        partStart = index + 1;
    }
    return out;
}

bool Localization::load(std::string_view locale, std::string_view json, std::string* diagnostic)
{
    if (!loadInto(project_, locale, json, diagnostic))
        return false;
    ++revision_;
    return true;
}

bool Localization::loadEngine(std::string_view locale, std::string_view json, std::string* diagnostic)
{
    if (!loadInto(engine_, locale, json, diagnostic))
        return false;
    ++revision_;
    return true;
}

void Localization::clear()
{
    project_.clear();
    engine_.clear();
    ++revision_;
}

void Localization::setDefaultLocale(std::string_view locale)
{
    const std::string name = canonicalLocale(locale);
    defaultLocale_ = name.empty() ? "en" : name;
}

std::vector<std::string> Localization::locales() const
{
    std::vector<std::string> names;
    names.reserve(project_.size());
    for (const auto& [name, catalog] : project_)
        names.push_back(name);
    if (names.empty())
        names.push_back(defaultLocale_);
    return names;
}

std::string Localization::narrow(std::string_view wanted) const
{
    const std::string name = canonicalLocale(wanted);
    if (name.empty())
        return {};
    // A project with no catalog has one locale: its default.
    if (project_.empty())
        return languageOf(name) == languageOf(defaultLocale_) ? defaultLocale_ : std::string{};
    if (project_.contains(name))
        return name;
    // The language alone, then any catalog of that language, in order -- so
    // the answer is the same on every machine (R10).
    const std::string_view language = languageOf(name);
    if (const auto plain = project_.find(language); plain != project_.end())
        return plain->first;
    for (const auto& [candidate, catalog] : project_) {
        if (languageOf(candidate) == language)
            return candidate;
    }
    return {};
}

std::optional<std::string> Localization::translate(std::string_view locale, std::string_view key,
                                                   std::span<const core::I18nArg> arguments) const
{
    const core::TextKey hashed{core::hashTextKey(key)};
    const std::string name = canonicalLocale(locale);
    // The project's word in the player's language, then in the project's own.
    for (const std::string_view from : {std::string_view{name}, std::string_view{defaultLocale_}}) {
        if (const core::Catalog* catalog = find(project_, from); catalog != nullptr && catalog->contains(hashed))
            return catalog->format(hashed, arguments);
    }
    // The engine's, in the player's language where it has it, then its own.
    for (const std::string_view from : {std::string_view{name}, languageOf(name)}) {
        if (const core::Catalog* catalog = find(engine_, from); catalog != nullptr && catalog->contains(hashed))
            return catalog->format(hashed, arguments);
    }
    if (const core::Catalog& own = core::engineCatalog(); own.contains(hashed))
        return own.format(hashed, arguments);
    return std::nullopt;
}

} // namespace engine::scene
