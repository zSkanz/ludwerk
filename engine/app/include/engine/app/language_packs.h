// The voices a game ships apart from itself (ADR 0200 §7).
//
// An export leaves the sounds of the voice languages it was not told to ship
// out of the game's own pack and writes each language's into a pack of its
// own, `l10n-<locale>.lpack`, which holds nothing but the names
// `asset://l10n/<locale>/...`. A player installs one by putting it in the
// game's `.engine` folder, beside `content.lpack`, and the host mounts what it
// finds there once, at start, after the game's own pack.
//
// **Mounted after, and that is the whole mechanism.** `ContentMounts` resolves
// through its mounts newest first, so a name the game's pack does not hold --
// every sound of a language that shipped apart -- is answered by the language
// pack and by nothing else. Nothing here knows what a voice is: the engine's
// resolver asks for `asset://l10n/<locale>/<name>` and finds it or does not.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/content.h"

namespace engine::app {

// `<engineFolder>/l10n-<locale>.lpack`: where a language's pack is looked for.
[[nodiscard]] std::filesystem::path languagePackPath(const std::filesystem::path& engineFolder,
                                                     std::string_view locale);

// **Mounts every language pack that is installed**, and answers with the
// locales it mounted, in the order `voice` names them.
//
// `voice` and `shipped` are the localization index's two lists
// (`asset://l10n/index.json`): every voice language the project has, and those
// whose sounds are in the game's own pack. Only a language in the first and
// not in the second has a pack to look for, so a file named for a language the
// game does not have is never opened.
//
// A pack is opened as the game's own is -- sealed, or plain with its manifest
// beside it (`l10n-<locale>.manifest.json`) -- and checked before it is
// mounted: **every entry against the hash it is filed under**, here and not on
// a worker as the game's is, because a mount cannot be taken back and a pack
// that is damaged must never have answered for a name. One that carries a
// game's own files is a game's pack under another name; a plain one that names
// anything outside `asset://l10n/<locale>/` is not this language's. Each of
// those is refused with a warning, and the game goes on without it.
//
// **A sealed pack's names are hashes, so what it answers to cannot be listed**
// (ADR 0183): that a sealed language pack names only its own language's
// sounds is what the export made true and not something this can check.
//
// `mounts` owns every pack it mounts; nothing is kept here.
[[nodiscard]] std::vector<std::string> mountLanguagePacks(asset::ContentMounts& mounts,
                                                          const std::filesystem::path& engineFolder,
                                                          std::span<const std::string> voice,
                                                          std::span<const std::string> shipped);

} // namespace engine::app
