// A game sealed into its pack (ADR 0183).
//
// `ludwerk build` used to leave a game as a folder of its parts: every script
// as a file under its own name and folder, the catalogues, `project.toml`, and
// beside the content pack a manifest listing every asset's path. Sealed, the
// game's folder holds one pack: its content, its scripts, its text and its
// settings, each answered for by the hash of its name, what is text
// compressed, and every entry checked against the hash it is filed under.
//
// **This is not a lock.** The format is this file and `pack.h`, and
// `unsealGame` below is the way back: what it keeps a game from is being
// handed over as a folder to read, and from being changed without that
// showing. Somebody who sets out to take a game apart will.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/pack.h"
#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::asset {

// What a game's own file is named by inside its pack: `game://` and its path
// under the game's folder, with `/` between the parts whatever the machine.
// `game://` alone names the list of them, one a line.
inline constexpr std::string_view GameScheme = "game://";

// The pack of a game's folder, and the manifest that names its content until
// the game is sealed.
[[nodiscard]] std::filesystem::path gamePackPath(const std::filesystem::path& gameDir);
[[nodiscard]] std::filesystem::path gameManifestPath(const std::filesystem::path& gameDir);

struct SealReport
{
    // Content the pack held before, and the game's own files taken into it.
    core::usize assets = 0;
    core::usize files = 0;
    // The pack and the loose files before, and the pack after.
    core::u64 bytesBefore = 0;
    core::u64 bytesAfter = 0;
};

// **Seals the game in `gameDir`**: its pack is written again with the names
// its manifest gave it, hashed; `project.toml`, `.luaurc` and everything under
// `src/` and `i18n/` are taken into it; and those files and the manifest are
// removed. A folder with no pack is given one. A game already sealed is an
// error, as is anything that cannot be read: a game half sealed is not left.
[[nodiscard]] std::optional<core::EngineError> sealGame(const std::filesystem::path& gameDir,
                                                        SealReport* report = nullptr);

// **The way back, for a test and for anybody who wants to look**: the game's
// own files written under `outDir` at the paths they had, and every other
// entry that was stored as text under `outDir/.blobs/<its hash>`.
[[nodiscard]] std::optional<core::EngineError> unsealGame(const std::filesystem::path& gameDir,
                                                          const std::filesystem::path& outDir);

// **A sealed game's own files, read where they are** -- what the host asks in
// place of the disk when a game's folder has no `src/`: is this file there,
// what is under this folder, what does it hold.
class SealedGame
{
public:
    // The sealed game in `gameDir`, or null: no pack, a pack that is not
    // sealed, or one that holds no game. One for a folder however many ask.
    [[nodiscard]] static std::shared_ptr<const SealedGame> open(const std::filesystem::path& gameDir);

    // Every file, as its path under the game's folder, sorted.
    [[nodiscard]] std::span<const std::string> files() const noexcept { return m_files; }
    [[nodiscard]] bool has(std::string_view relative) const noexcept;
    // Whether any file is under `directory` (`src/scripts`).
    [[nodiscard]] bool hasUnder(std::string_view directory) const noexcept;
    // The files under `directory`, at any depth, sorted.
    [[nodiscard]] std::vector<std::string> filesUnder(std::string_view directory) const;
    // Empty for a file it does not hold -- and for one that holds nothing.
    [[nodiscard]] std::span<const std::byte> read(std::string_view relative) const;
    [[nodiscard]] bool readText(std::string_view relative, std::string& out) const;

    [[nodiscard]] const Pack& pack() const noexcept { return m_pack; }

private:
    Pack m_pack;
    std::vector<PackName> m_names;
    std::vector<std::string> m_files;
};

} // namespace engine::asset
