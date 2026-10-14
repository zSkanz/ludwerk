#include "engine/assetc/l10n.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "engine/audio/audio.h"
#include "engine/platform/file.h"

namespace engine::assetc {
namespace {

using core::u64;

constexpr std::string_view L10nFolder = "l10n";
constexpr std::string_view DialogueFolder = "dialogue";
constexpr std::string_view LinesSuffix = ".lines.json";

[[nodiscard]] std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A string as JSON writes one. A file's name may hold a quote or a backslash
// on some file systems, and an index that did not parse would be a game with
// no voices and no reason given.
void appendQuoted(std::string& out, std::string_view text)
{
    static constexpr char Hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(raw);
        }
        else if (c < 0x20) {
            out.append("\\u00");
            out.push_back(Hex[c >> 4]);
            out.push_back(Hex[c & 0xF]);
        }
        else {
            out.push_back(raw);
        }
    }
    out.push_back('"');
}

// `["a", "b"]` on one line: these lists are a handful of names.
void appendList(std::string& out, const std::vector<std::string>& items)
{
    out.push_back('[');
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0)
            out.append(", ");
        appendQuoted(out, items[index]);
    }
    out.push_back(']');
}

// `{ "name": frames }`, one a line, in the order given -- which is sorted.
void appendLengths(std::string& out, const std::vector<std::pair<std::string, u64>>& rows)
{
    if (rows.empty()) {
        out.append("{}");
        return;
    }
    out.append("{\n");
    for (std::size_t index = 0; index < rows.size(); ++index) {
        out.append("    ");
        appendQuoted(out, rows[index].first);
        out.append(": ").append(std::to_string(rows[index].second));
        out.append(index + 1 == rows.size() ? "\n" : ",\n");
    }
    out.append("  }");
}

// The file's length as the engine measures it, or nothing: unreadable, or a
// file that declares no length.
[[nodiscard]] std::optional<u64> framesOf(const std::filesystem::path& file)
{
    std::vector<std::byte> bytes;
    if (!platform::readFile(file, bytes) || bytes.empty())
        return std::nullopt;
    return audio::detail::probeFrames(bytes);
}

} // namespace

bool isSoundFile(const std::filesystem::path& path)
{
    const std::string extension = lowercase(path.extension().generic_string());
    return extension == ".ogg" || extension == ".wav" || extension == ".mp3" || extension == ".flac";
}

bool scanL10n(const std::filesystem::path& root, L10nScan& out, std::string& diagnostic)
{
    out = L10nScan{};
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        diagnostic = "not a directory: " + root.string();
        return false;
    }

    // Each sound's name to the languages that have it. A map, so the order is
    // the names' own and not the file system's.
    std::map<std::string, std::vector<std::string>> localized;
    const std::filesystem::path l10n = root / std::filesystem::path(L10nFolder);
    if (std::filesystem::is_directory(l10n, ec)) {
        std::vector<std::filesystem::path> locales;
        for (std::filesystem::directory_iterator it(l10n, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec))
                locales.push_back(it->path());
        }
        if (ec) {
            diagnostic = "could not walk " + l10n.string() + ": " + ec.message();
            return false;
        }
        for (const std::filesystem::path& folder : locales) {
            const std::string locale = folder.filename().generic_string();
            bool voiced = false;
            std::filesystem::recursive_directory_iterator it(folder, ec);
            for (const std::filesystem::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || !isSoundFile(it->path()))
                    continue;
                voiced = true;
                localized[std::filesystem::relative(it->path(), folder, ec).generic_string()].push_back(locale);
            }
            if (ec) {
                diagnostic = "could not walk " + folder.string() + ": " + ec.message();
                return false;
            }
            if (voiced)
                out.voice.push_back(locale);
        }
        std::sort(out.voice.begin(), out.voice.end());
    }

    for (auto& [path, locales] : localized) {
        LocalizedSound sound;
        sound.path = path;
        if (std::filesystem::is_regular_file(root / std::filesystem::path(path), ec))
            sound.files.push_back(path);
        std::sort(locales.begin(), locales.end());
        for (const std::string& locale : locales)
            sound.files.push_back(std::string(L10nFolder) + "/" + locale + "/" + path);
        out.sounds.push_back(std::move(sound));
    }

    const std::filesystem::path dialogue = root / std::filesystem::path(DialogueFolder);
    if (std::filesystem::is_directory(dialogue, ec)) {
        for (std::filesystem::directory_iterator it(dialogue, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().generic_string();
            if (it->is_regular_file(ec) && name.size() > LinesSuffix.size() && name.ends_with(LinesSuffix))
                out.lines.push_back(std::string(DialogueFolder) + "/" + name);
        }
        if (ec) {
            diagnostic = "could not walk " + dialogue.string() + ": " + ec.message();
            return false;
        }
        std::sort(out.lines.begin(), out.lines.end());
    }
    return true;
}

L10nResult buildL10nIndex(const std::filesystem::path& root, const std::optional<std::vector<std::string>>& shipped)
{
    L10nResult result;
    L10nScan scan;
    if (!scanL10n(root, scan, result.diagnostic))
        return result;
    if (scan.empty()) {
        result.ok = true;
        return result;
    }

    std::vector<std::string> inPack;
    for (const std::string& locale : scan.voice) {
        if (!shipped.has_value() || std::find(shipped->begin(), shipped->end(), locale) != shipped->end())
            inPack.push_back(locale);
    }

    std::vector<std::pair<std::string, u64>> lengths;
    lengths.reserve(scan.sounds.size());
    for (const LocalizedSound& sound : scan.sounds) {
        u64 longest = 0;
        for (const std::string& file : sound.files) {
            const std::optional<u64> frames = framesOf(root / std::filesystem::path(file));
            if (!frames.has_value()) {
                result.diagnostic = "cannot measure " + file +
                                    ": it cannot be read, or it is not a sound file that says how long it is";
                return result;
            }
            longest = std::max(longest, *frames);
        }
        lengths.emplace_back(sound.path, longest);
    }

    std::string& json = result.json;
    json.append("{\n  \"version\": 1,\n  \"voice\": ");
    appendList(json, scan.voice);
    json.append(",\n  \"shipped\": ");
    appendList(json, inPack);
    json.append(",\n  \"lengths\": ");
    appendLengths(json, lengths);
    json.append(",\n  \"lines\": ");
    appendList(json, scan.lines);
    json.append("\n}\n");
    result.ok = true;
    return result;
}

L10nResult measureL10n(const std::filesystem::path& root)
{
    L10nResult result;
    L10nScan scan;
    if (!scanL10n(root, scan, result.diagnostic))
        return result;

    std::vector<std::pair<std::string, u64>> files;
    std::vector<std::string> unmeasured;
    for (const LocalizedSound& sound : scan.sounds) {
        for (const std::string& file : sound.files) {
            if (const std::optional<u64> frames = framesOf(root / std::filesystem::path(file)))
                files.emplace_back(file, *frames);
            else
                unmeasured.push_back(file);
        }
    }
    std::sort(files.begin(), files.end());
    std::sort(unmeasured.begin(), unmeasured.end());

    std::string& json = result.json;
    json.append("{\n  \"version\": 1,\n  \"rate\": ").append(std::to_string(L10nFrameRate));
    json.append(",\n  \"files\": ");
    appendLengths(json, files);
    json.append(",\n  \"unmeasured\": ");
    appendList(json, unmeasured);
    json.append("\n}\n");
    result.ok = true;
    return result;
}

} // namespace engine::assetc
