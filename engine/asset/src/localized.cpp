#include "engine/asset/localized.h"

#include <algorithm>
#include <system_error>

#include "engine/asset/content.h"
#include "engine/core/json.h"

namespace engine::asset {

namespace {

constexpr std::string_view Scheme = "asset://";

[[nodiscard]] char lowered(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] bool endsWith(std::string_view text, std::string_view tail) noexcept
{
    if (text.size() < tail.size())
        return false;
    const std::string_view end = text.substr(text.size() - tail.size());
    for (core::usize index = 0; index < tail.size(); ++index) {
        if (lowered(end[index]) != tail[index])
            return false;
    }
    return true;
}

// A name under `l10n/`, as its path after the scheme.
[[nodiscard]] bool underLocalized(std::string_view path) noexcept
{
    return path.size() > LocalizedFolder.size() && path.substr(0, LocalizedFolder.size()) == LocalizedFolder &&
           path[LocalizedFolder.size()] == '/';
}

void readNames(const core::JsonValue& list, std::vector<std::string>& out)
{
    for (core::usize index = 0; index < list.size(); ++index) {
        const std::string_view name = list.at(index).asString();
        if (!name.empty())
            out.emplace_back(name);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace

bool isSoundPath(std::string_view path) noexcept
{
    return endsWith(path, ".ogg") || endsWith(path, ".wav") || endsWith(path, ".mp3") || endsWith(path, ".flac");
}

void localizedCandidates(std::string_view urn, std::string_view locale, std::vector<std::string>& out)
{
    out.clear();
    if (urn.size() > Scheme.size() && urn.substr(0, Scheme.size()) == Scheme && !locale.empty()) {
        const std::string_view path = urn.substr(Scheme.size());
        if (!underLocalized(path)) {
            const auto under = [&](std::string_view folder) {
                std::string name;
                name.reserve(Scheme.size() + LocalizedFolder.size() + folder.size() + path.size() + 2);
                name.append(Scheme).append(LocalizedFolder).append("/").append(folder).append("/").append(path);
                out.push_back(std::move(name));
            };
            under(locale);
            // `pt` after `pt-BR`: a language recorded once serves every
            // region of it, as a catalog does.
            if (const core::usize dash = locale.find('-'); dash != std::string_view::npos && dash > 0)
                under(locale.substr(0, dash));
        }
    }
    out.emplace_back(urn);
}

std::string resolveLocalized(const ContentMounts& mounts, std::string_view urn, std::string_view locale)
{
    std::vector<std::string> names;
    localizedCandidates(urn, locale, names);
    for (const std::string& name : names) {
        if (mounts.contains(name))
            return name;
    }
    return std::string(urn);
}

std::string_view localeOfResolved(std::string_view urn) noexcept
{
    if (urn.size() <= Scheme.size() || urn.substr(0, Scheme.size()) != Scheme)
        return {};
    const std::string_view path = urn.substr(Scheme.size());
    if (!underLocalized(path))
        return {};
    const std::string_view rest = path.substr(LocalizedFolder.size() + 1);
    const core::usize slash = rest.find('/');
    return slash == std::string_view::npos ? std::string_view{} : rest.substr(0, slash);
}

std::optional<core::u64> LocalizationIndex::longest(std::string_view path) const
{
    const auto at = std::lower_bound(lengths.begin(), lengths.end(), path,
                                     [](const auto& entry, std::string_view key) { return entry.first < key; });
    if (at == lengths.end() || at->first != path)
        return std::nullopt;
    return at->second;
}

bool readLocalizationIndex(std::string_view json, LocalizationIndex& out, std::string* what)
{
    out = LocalizationIndex{};
    const auto refuse = [&](std::string_view why) {
        if (what != nullptr)
            *what = std::string(why);
        out = LocalizationIndex{};
        return false;
    };
    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json, "l10n/index.json"); !parsed)
        return refuse(parsed.diagnostic);
    const core::JsonValue root = document.root();
    if (root.type() != core::JsonType::Object)
        return refuse("not an object");
    // One version, and a file from a later one is not guessed at: its lengths
    // decide the tick a sound ends on.
    if (root["version"].asInteger(0) != 1)
        return refuse("version");

    readNames(root["voice"], out.voice);
    readNames(root["shipped"], out.shipped);
    readNames(root["lines"], out.lines);
    // A language cannot be shipped and not exist.
    std::erase_if(out.shipped, [&](const std::string& name) {
        return !std::binary_search(out.voice.begin(), out.voice.end(), name);
    });

    const core::JsonValue lengths = root["lengths"];
    if (lengths && lengths.type() != core::JsonType::Object)
        return refuse("lengths");
    out.lengths.reserve(lengths.size());
    for (core::usize index = 0; index < lengths.size(); ++index) {
        const std::string_view path = lengths.keyAt(index);
        const core::i64 frames = lengths.at(index).asInteger(-1);
        if (path.empty() || frames < 0)
            return refuse("lengths");
        out.lengths.emplace_back(std::string(path), static_cast<core::u64>(frames));
    }
    std::sort(out.lengths.begin(), out.lengths.end());
    out.measured = true;
    return true;
}

void scanLocalizedContent(const std::filesystem::path& content, LocalizationIndex& out)
{
    out = LocalizationIndex{};
    std::error_code error;

    const std::filesystem::path localized = content / std::filesystem::path(LocalizedFolder);
    for (std::filesystem::directory_iterator folder(localized, error), end; !error && folder != end;
         folder.increment(error)) {
        if (!folder->is_directory(error))
            continue;
        // A language has voice when one sound is anywhere under it; a folder
        // of translated pictures alone does not make one.
        bool voiced = false;
        std::error_code inside;
        for (std::filesystem::recursive_directory_iterator file(folder->path(), inside), last;
             !inside && file != last && !voiced; file.increment(inside)) {
            if (file->is_regular_file(inside) && isSoundPath(file->path().filename().generic_string()))
                voiced = true;
        }
        if (voiced)
            out.voice.push_back(folder->path().filename().generic_string());
    }
    std::sort(out.voice.begin(), out.voice.end());
    // Loose files are all here: what there is, this machine can play.
    out.shipped = out.voice;

    error.clear();
    const std::filesystem::path dialogue = content / "dialogue";
    for (std::filesystem::directory_iterator file(dialogue, error), end; !error && file != end; file.increment(error)) {
        if (!file->is_regular_file(error))
            continue;
        const std::string name = file->path().filename().generic_string();
        if (endsWith(name, ".lines.json"))
            out.lines.push_back("dialogue/" + name);
    }
    std::sort(out.lines.begin(), out.lines.end());
}

} // namespace engine::asset
