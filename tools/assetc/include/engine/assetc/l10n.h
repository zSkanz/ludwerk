// The localization index of an exported game (ADR 0200 §2 and §7).
//
// A localized sound is the file of the same name under `l10n/<locale>/`, and
// it lasts as long as its longest language: the tick a line ends on is the
// same on every machine, whichever language each of them hears. A dev run
// reads those lengths from the files as it meets them. An exported game
// cannot: its names are hashes and cannot be listed, and the files of a
// language that ships apart are not there at all -- on a dedicated server, on
// a machine with no language pack installed. So the export writes what the
// files said into the game, as `asset://l10n/index.json`.
//
// **Every length is `audio::detail::probeFrames` of the file's bytes**, the
// function the engine measures the same file with in a dev run: frames at the
// mixer's 48 kHz, whatever rate the file was recorded at. Called, not written
// again here, because the two numbers have to agree to the frame and two
// implementations of one header parse agree until the day one is changed.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/types.h"

namespace engine::assetc {

// What `probeFrames` counts in: frames a second, the mixer's rate. Written
// into the per-file listing so a reader turns frames into seconds without
// carrying the number itself.
inline constexpr core::u32 L10nFrameRate = 48000;

// `.ogg`, `.wav`, `.mp3`, `.flac`, in any case: what under `l10n/<locale>/`
// is a voice. Everything else there is a localized picture.
[[nodiscard]] bool isSoundFile(const std::filesystem::path& path);

// One name that has a recording in some language other than the default.
struct LocalizedSound
{
    // Relative to the content directory, with `/`: `voice/act1/intro_01.ogg`.
    std::string path;
    // Every file that carries it, relative to the content directory: the
    // default language's when there is one, then each `l10n/<locale>/<path>`,
    // by locale.
    std::vector<std::string> files;
};

struct L10nScan
{
    // Every folder under `l10n/` that holds a sound file at any depth, sorted.
    std::vector<std::string> voice;
    // Sorted by path.
    std::vector<LocalizedSound> sounds;
    // `dialogue/*.lines.json`, direct children only, sorted.
    std::vector<std::string> lines;

    // Nothing to say: no voice language and no lines file.
    [[nodiscard]] bool empty() const noexcept { return voice.empty() && sounds.empty() && lines.empty(); }
};

// What `root` holds that the index describes. Reads names only, no file's
// bytes. False, with `diagnostic` set, when the tree cannot be walked.
[[nodiscard]] bool scanL10n(const std::filesystem::path& root, L10nScan& out, std::string& diagnostic);

struct L10nResult
{
    bool ok = false;
    // Developer-facing, as this tool's own output is: names the file.
    std::string diagnostic;
    // The file's text, ending in a newline. Empty with `ok` when there is
    // nothing to index, and then no file is written.
    std::string json;
};

// **The index** (`l10n/index.json` in the content that is packed):
//
//   { "version": 1, "voice": [...], "shipped": [...],
//     "lengths": { "<sound path>": <frames> }, "lines": [...] }
//
// `shipped` is the languages whose sounds are in the game's own pack: those
// of `shipped` that are voice languages, or every voice language when it is
// not given. A length is the longest among the default language's file and
// every language's. A file `probeFrames` cannot measure fails the build and
// is named: a length missing from the index would be a line that ends on a
// different tick on a machine that has the file than on one that has not.
//
// The same tree gives the same bytes: sorted, `\n`, no time and no path of
// this machine.
[[nodiscard]] L10nResult buildL10nIndex(const std::filesystem::path& root,
                                        const std::optional<std::vector<std::string>>& shipped);

// **Each file's own length**, for `ludwerk voice`, which says how far apart a
// line's languages are:
//
//   { "version": 1, "rate": 48000,
//     "files": { "<content-relative file>": <frames> }, "unmeasured": [...] }
//
// Every file of every localized sound, the default language's included. A
// file the probe cannot measure is listed and fails nothing: this is a report.
[[nodiscard]] L10nResult measureL10n(const std::filesystem::path& root);

} // namespace engine::assetc
