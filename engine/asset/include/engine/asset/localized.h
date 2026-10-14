#pragma once

// **An asset in the player's language** (ADR 0200).
//
// A localized asset is a file of the SAME name under `l10n/<locale>/`:
// `voice/intro_01.ogg` is the line in the project's default language and
// `l10n/pt-BR/voice/intro_01.ogg` is the line in Brazilian Portuguese. A game
// names the first, always, and what is found for a language is the file under
// that locale, else under its bare language, else the name itself -- so a
// missing translation is the default language's file and never nothing.
//
// Nothing here knows what a sound or a picture is beyond a file's extension:
// the audio system resolves a sound for the VOICE language, the interface a
// picture for the TEXT language, and both ask the same three names.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/types.h"

namespace engine::asset {

class ContentMounts;

// The folder localized files live under, as a name's first segment.
inline constexpr std::string_view LocalizedFolder = "l10n";
// The generated file an export carries: what a sealed pack cannot be asked.
inline constexpr std::string_view LocalizationIndexUrn = "asset://l10n/index.json";

// Whether a content path names a sound, by its extension: `.ogg`, `.wav`,
// `.mp3`, `.flac`, in any case. What "a language has voice" is counted in.
[[nodiscard]] bool isSoundPath(std::string_view path) noexcept;

// The names `urn` may be found under for `locale`, most particular first:
// the locale's own file, its language's when the locale names a region, and
// last the name as written. A name already under `l10n/`, a name that is not
// content, and an empty locale give the name alone.
void localizedCandidates(std::string_view urn, std::string_view locale, std::vector<std::string>& out);

// The first of those the mounts hold; `urn` itself when none is there, so a
// caller reports a missing file by the name the game wrote.
[[nodiscard]] std::string resolveLocalized(const ContentMounts& mounts, std::string_view urn, std::string_view locale);

// The locale a resolved name belongs to: `pt-BR` for
// `asset://l10n/pt-BR/voice/x.ogg`, empty for a name outside `l10n/` -- the
// default language's.
[[nodiscard]] std::string_view localeOfResolved(std::string_view urn) noexcept;

// **What a game holds in other languages.** In an export it is read from the
// index, which the asset compiler wrote; in a development run from the loose
// content folder itself, where the lengths are measured as they are asked.
struct LocalizationIndex
{
    // Every language that has at least one sound, sorted. The default
    // language is not among them: its files are the plain names.
    std::vector<std::string> voice;
    // Those whose sounds are inside the game's own pack. The rest come in a
    // language pack, or are not on this machine.
    std::vector<std::string> shipped;
    // For each sound that exists in some other language, by its path under
    // `content/`, sorted: the LONGEST of its recordings, in frames at the
    // mixer's rate. Empty in a development run.
    std::vector<std::pair<std::string, core::u64>> lengths;
    // The dialogue lines files, as paths under `content/`, sorted.
    std::vector<std::string> lines;
    // Whether `lengths` is the whole truth (an index was read) or the files
    // have to be measured.
    bool measured = false;

    [[nodiscard]] std::optional<core::u64> longest(std::string_view path) const;
    [[nodiscard]] bool empty() const noexcept { return voice.empty() && lengths.empty() && lines.empty(); }
};

// Reads an index as the asset compiler writes it. False, with `what` saying
// why in a word a caller can log, for anything that is not one.
[[nodiscard]] bool readLocalizationIndex(std::string_view json, LocalizationIndex& out, std::string* what = nullptr);

// The same facts from a loose content folder: the folders under `l10n/` that
// hold a sound, and `dialogue/*.lines.json`. No lengths. Every list sorted,
// so what a development run does never depends on a directory's order.
void scanLocalizedContent(const std::filesystem::path& content, LocalizationIndex& out);

} // namespace engine::asset
