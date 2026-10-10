#include "engine/scene/localization.h"

#include <algorithm>

#include "engine/core/json.h"

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

// --- The voice language (ADR 0200) ---------------------------------------------

std::string narrowVoice(std::string_view wanted, std::span<const std::string> playable)
{
    const std::string name = canonicalLocale(wanted);
    if (name.empty())
        return {};
    for (const std::string& candidate : playable) {
        if (candidate == name)
            return candidate;
    }
    // The language alone, then any of that language, in the list's order --
    // which the host made the same way on every machine.
    const std::string_view language = languageOf(name);
    for (const std::string& candidate : playable) {
        if (candidate == language)
            return candidate;
    }
    for (const std::string& candidate : playable) {
        if (languageOf(candidate) == language)
            return candidate;
    }
    return {};
}

std::string voiceLocaleFor(std::string_view chosen, std::string_view textLocale, std::span<const std::string> playable,
                           std::span<const std::string> system)
{
    if (std::string own = narrowVoice(chosen, playable); !own.empty())
        return own;
    if (std::string follows = narrowVoice(textLocale, playable); !follows.empty())
        return follows;
    // A game read in a language it has no voice for: the first language of
    // the machine it does have a voice for, which need not be the one it has
    // text for.
    for (const std::string& preferred : system) {
        if (std::string heard = narrowVoice(preferred, playable); !heard.empty())
            return heard;
    }
    return playable.empty() ? std::string{} : playable.front();
}

std::vector<std::string> voiceLocaleList(std::string_view defaultLocale, std::span<const std::string> others)
{
    std::vector<std::string> list;
    for (const std::string& other : others) {
        std::string name = canonicalLocale(other);
        if (!name.empty())
            list.push_back(std::move(name));
    }
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    std::string first = canonicalLocale(defaultLocale);
    if (first.empty())
        first = "en";
    list.erase(std::remove(list.begin(), list.end(), first), list.end());
    list.insert(list.begin(), std::move(first));
    return list;
}

// --- Lines of dialogue (ADR 0200 section 5) --------------------------------------

namespace {

// `#E8B04A` as a colour; false for anything else.
[[nodiscard]] bool hexColor(std::string_view text, core::Color3& out) noexcept
{
    if (text.size() != 7 || text[0] != '#')
        return false;
    core::u32 channels[3] = {0, 0, 0};
    for (core::usize index = 0; index < 6; ++index) {
        const char c = lower(text[index + 1]);
        core::u32 digit = 0;
        if (c >= '0' && c <= '9')
            digit = static_cast<core::u32>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<core::u32>(c - 'a') + 10u;
        else
            return false;
        channels[index / 2] = channels[index / 2] * 16u + digit;
    }
    out = core::Color3{static_cast<core::f32>(channels[0]) / 255.0f, static_cast<core::f32>(channels[1]) / 255.0f,
                       static_cast<core::f32>(channels[2]) / 255.0f};
    return true;
}

} // namespace

bool DialogueLines::load(std::string_view name, std::string_view json, std::string* diagnostic)
{
    const auto refuse = [diagnostic](std::string why) {
        if (diagnostic != nullptr)
            *diagnostic = std::move(why);
        return false;
    };
    if (name.empty() || name.find('.') != std::string_view::npos)
        return refuse("a lines file is named <name>.lines.json, and <name> has no dot in it");
    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json, name); !parsed)
        return refuse(parsed.diagnostic);
    const core::JsonValue root = document.root();
    const core::JsonValue lines = root["lines"];
    if (root.type() != core::JsonType::Object || lines.type() != core::JsonType::Object)
        return refuse("a lines file is an object with a \"lines\" object in it");
    const core::JsonValue speakers = root["speakers"];
    if (root.has("speakers") && speakers.type() != core::JsonType::Object)
        return refuse("\"speakers\" is an object: a speaker's id to its name and colour");

    // Into a table of its own first: a file that is refused leaves the lines
    // its name had.
    std::map<std::string, DialogueLine, std::less<>> read;
    for (core::usize index = 0; index < lines.size(); ++index) {
        const std::string_view id = lines.keyAt(index);
        const core::JsonValue entry = lines[id];
        if (id.empty() || entry.type() != core::JsonType::Object)
            return refuse("line \"" + std::string(id) + "\" is not an object");
        std::string full(name);
        full += '.';
        full += id;

        DialogueLine line;
        line.text = entry["text"].type() == core::JsonType::String ? std::string(entry["text"].asString()) : full;
        if (entry["sound"].type() == core::JsonType::String) {
            line.sound = std::string(entry["sound"].asString());
        }
        else {
            line.sound = "asset://voice/";
            line.sound += name;
            line.sound += '/';
            line.sound += id;
            line.sound += ".ogg";
        }
        if (entry.has("seconds")) {
            const core::f64 seconds = entry["seconds"].asNumber(-1.0);
            if (entry["seconds"].type() != core::JsonType::Number || !(seconds >= 0.0) || seconds > 3600.0)
                return refuse("line \"" + std::string(id) + "\": \"seconds\" is a number from 0 to 3600");
            line.seconds = static_cast<core::f32>(seconds);
        }
        if (entry["speaker"].type() == core::JsonType::String && !entry["speaker"].asString().empty()) {
            const std::string_view who = entry["speaker"].asString();
            const core::JsonValue speaker = speakers[who];
            if (speaker.type() != core::JsonType::Object)
                return refuse("line \"" + std::string(id) + "\" is said by \"" + std::string(who) +
                              "\", which \"speakers\" does not have");
            // A speaker with no name of its own is named by its id's key.
            line.speaker = speaker["name"].type() == core::JsonType::String ? std::string(speaker["name"].asString())
                                                                            : "speaker." + std::string(who);
            if (speaker.has("color") && !hexColor(speaker["color"].asString(), line.color))
                return refuse("speaker \"" + std::string(who) + "\": \"color\" is written #RRGGBB");
        }
        read.emplace(std::move(full), std::move(line));
    }

    forget(name);
    lines_.merge(read);
    return true;
}

void DialogueLines::forget(std::string_view name)
{
    std::string prefix(name);
    prefix += '.';
    for (auto at = lines_.lower_bound(prefix); at != lines_.end() && at->first.starts_with(prefix);)
        at = lines_.erase(at);
}

void DialogueLines::clear()
{
    lines_.clear();
}

const DialogueLine* DialogueLines::find(std::string_view line) const
{
    const auto found = lines_.find(line);
    return found != lines_.end() ? &found->second : nullptr;
}

std::vector<std::string> DialogueLines::ids() const
{
    std::vector<std::string> out;
    out.reserve(lines_.size());
    for (const auto& [id, line] : lines_)
        out.push_back(id);
    return out;
}

core::f64 captionSeconds(core::f32 seconds, std::string_view text) noexcept
{
    if (seconds > 0.0f)
        return static_cast<core::f64>(seconds);
    // Code points, not bytes: a continuation byte is `10xxxxxx` and is not
    // counted, so a line reads as long in Japanese as its characters are many.
    core::u64 characters = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u)
            ++characters;
    }
    // In whole hundredths, so the sum is exact and the same on every machine.
    const core::u64 hundredths = std::min<core::u64>(150u + 6u * characters, 1200u);
    return static_cast<core::f64>(hundredths) / 100.0;
}

} // namespace engine::scene
