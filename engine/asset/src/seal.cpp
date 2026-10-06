#include "engine/asset/seal.h"

#include <algorithm>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include "engine/asset/content.h"
#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/platform/file.h"

namespace engine::asset {
namespace {

using core::I18nArg;
using core::u64;
using core::usize;

// What of a game's folder is the game's own and not its content: its
// settings, its aliases, its code and its text. Everything else there is
// either the pack or beside it for a reason -- the terrain's cells, which are
// streamed from their own files.
constexpr std::string_view OwnFiles[] = {"project.toml", ".luaurc"};
constexpr std::string_view OwnFolders[] = {"src", "i18n"};

[[nodiscard]] std::string nameOf(std::string_view relative)
{
    std::string name(GameScheme);
    name.append(relative);
    return name;
}

[[nodiscard]] std::optional<core::EngineError> failed(core::TextKey key, const std::filesystem::path& path)
{
    const I18nArg args[] = {{"path", path.generic_string()}};
    return core::makeError(key, args);
}

// The game's own files under `gameDir`, as paths with `/`, sorted.
[[nodiscard]] std::vector<std::string> ownFiles(const std::filesystem::path& gameDir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const std::string_view file : OwnFiles) {
        if (std::filesystem::is_regular_file(gameDir / std::filesystem::path(file), ec))
            out.emplace_back(file);
    }
    for (const std::string_view folder : OwnFolders) {
        const std::filesystem::path root = gameDir / std::filesystem::path(folder);
        if (!std::filesystem::is_directory(root, ec))
            continue;
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_regular_file(ec))
                out.push_back(std::filesystem::relative(it->path(), gameDir, ec).generic_string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] std::span<const std::byte> bytesOf(std::string_view text) noexcept
{
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// The lines of a listing: one path a line, none empty.
[[nodiscard]] std::vector<std::string> linesOf(std::span<const std::byte> listing)
{
    std::vector<std::string> out;
    const std::string_view text(reinterpret_cast<const char*>(listing.data()), listing.size());
    usize at = 0;
    while (at < text.size()) {
        const usize end = std::min(text.find('\n', at), text.size());
        if (end > at)
            out.emplace_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

} // namespace

std::filesystem::path gamePackPath(const std::filesystem::path& gameDir)
{
    return gameDir / ".engine" / "content.lpack";
}

std::filesystem::path gameManifestPath(const std::filesystem::path& gameDir)
{
    return gameDir / ".engine" / "content.manifest.json";
}

std::optional<core::EngineError> sealGame(const std::filesystem::path& gameDir, SealReport* report)
{
    const std::filesystem::path packPath = gamePackPath(gameDir);
    const std::filesystem::path manifestPath = gameManifestPath(gameDir);
    SealReport told;
    PackWriter writer;
    std::vector<PackName> names;
    std::error_code ec;

    if (std::filesystem::is_regular_file(packPath, ec)) {
        // Every blob against its name on the way in: what is sealed is what
        // was built, or nothing is.
        Pack source;
        if (auto error = openPackFile(packPath, source, true))
            return error;
        if (source.names() != nullptr)
            return failed(ENG_TR("asset.seal.err.already_sealed"), packPath);
        std::vector<ManifestRow> rows;
        if (auto error = readContentManifest(manifestPath, rows))
            return error;
        for (const PackEntry& entry : source.entries())
            writer.add(entry.hash, entry.kind, source.blob(entry.hash));
        for (const ManifestRow& row : rows) {
            if (!source.contains(row.hash)) {
                const I18nArg args[] = {{"content", row.urn}};
                return core::makeError(ENG_TR("asset.manifest.err.missing_blob"), args);
            }
            names.push_back(PackName{core::hashText(row.urn), row.hash, row.kind});
        }
        told.assets = rows.size();
        told.bytesBefore += std::filesystem::file_size(packPath, ec) + std::filesystem::file_size(manifestPath, ec);
        // The pack goes out of scope here, and its file is let go: it is about
        // to be written over.
    }

    const std::vector<std::string> files = ownFiles(gameDir);
    std::string listing;
    for (const std::string& file : files) {
        std::vector<std::byte> bytes;
        const std::filesystem::path path = gameDir / std::filesystem::path(file);
        if (!platform::readFile(path, bytes))
            return failed(ENG_TR("asset.seal.err.read_failed"), path);
        told.bytesBefore += bytes.size();
        names.push_back(
            PackName{core::hashText(nameOf(file)), writer.addContent(AssetKind::Raw, bytes), AssetKind::Raw});
        listing.append(file);
        listing.push_back('\n');
    }
    told.files = files.size();
    names.push_back(
        PackName{core::hashText(GameScheme), writer.addContent(AssetKind::Raw, bytesOf(listing)), AssetKind::Raw});
    (void)writer.addContent(AssetKind::Names, encodePackNames(std::move(names)));

    const std::vector<std::byte> sealed = writer.buildSealed();
    // Beside it first, then over it: a build stopped half way leaves the pack
    // it had.
    std::filesystem::path fresh = packPath;
    fresh += ".sealing";
    if (!platform::createDirectories(packPath.parent_path()) || !platform::writeFile(fresh, sealed))
        return failed(ENG_TR("asset.seal.err.write_failed"), fresh);
    (void)platform::removeFile(packPath);
    if (!platform::renameFile(fresh, packPath))
        return failed(ENG_TR("asset.seal.err.write_failed"), packPath);
    told.bytesAfter = sealed.size();

    // And what it now holds is not left beside it.
    (void)platform::removeFile(manifestPath);
    for (const std::string& file : files)
        (void)platform::removeFile(gameDir / std::filesystem::path(file));
    for (const std::string_view folder : OwnFolders)
        std::filesystem::remove_all(gameDir / std::filesystem::path(folder), ec);

    if (report != nullptr)
        *report = told;
    return std::nullopt;
}

std::optional<core::EngineError> unsealGame(const std::filesystem::path& gameDir, const std::filesystem::path& outDir)
{
    const std::filesystem::path packPath = gamePackPath(gameDir);
    Pack pack;
    if (auto error = openPackFile(packPath, pack, true))
        return error;
    const PackEntry* const table = pack.names();
    std::vector<PackName> names;
    if (table == nullptr || !decodePackNames(pack.blob(table->hash), names))
        return failed(ENG_TR("asset.seal.err.not_sealed"), packPath);

    std::vector<core::ContentHash> own;
    const PackName* const listing = findPackName(names, GameScheme);
    const std::vector<std::string> files =
        listing != nullptr ? linesOf(pack.blob(listing->content)) : std::vector<std::string>{};
    if (listing != nullptr)
        own.push_back(listing->content);
    for (const std::string& file : files) {
        const PackName* const row = findPackName(names, nameOf(file));
        const std::filesystem::path target = outDir / std::filesystem::path(file);
        if (row == nullptr || !platform::createDirectories(target.parent_path()) ||
            !platform::writeFile(target, pack.blob(row->content)))
            return failed(ENG_TR("asset.seal.err.write_failed"), target);
        own.push_back(row->content);
    }

    // What else was text: a scene, a stamp, a material. By its hash, because
    // its name is not in the pack to give it.
    const std::filesystem::path blobs = outDir / ".blobs";
    for (const PackEntry& entry : pack.entries()) {
        const bool text =
            entry.kind == AssetKind::Raw || entry.kind == AssetKind::Material || entry.kind == AssetKind::Surface;
        if (!text || std::find(own.begin(), own.end(), entry.hash) != own.end())
            continue;
        const std::filesystem::path target = blobs / entry.hash.toHex();
        if (!platform::createDirectories(blobs) || !platform::writeFile(target, pack.blob(entry.hash)))
            return failed(ENG_TR("asset.seal.err.write_failed"), target);
    }
    return std::nullopt;
}

std::shared_ptr<const SealedGame> SealedGame::open(const std::filesystem::path& gameDir)
{
    // One for a folder: the host asks at start, each world asks as it mounts,
    // and a pack is mapped once.
    static std::mutex guard;
    static std::unordered_map<std::string, std::weak_ptr<const SealedGame>> opened;
    std::error_code ec;
    const std::filesystem::path packPath = gamePackPath(gameDir);
    if (!std::filesystem::is_regular_file(packPath, ec))
        return nullptr;
    const std::string key = std::filesystem::weakly_canonical(packPath, ec).generic_string();

    const std::lock_guard lock(guard);
    if (const auto held = opened.find(key); held != opened.end()) {
        if (std::shared_ptr<const SealedGame> alive = held->second.lock())
            return alive;
    }
    auto game = std::make_shared<SealedGame>();
    if (openPackFile(packPath, game->m_pack).has_value())
        return nullptr;
    const PackEntry* const table = game->m_pack.names();
    if (table == nullptr || !decodePackNames(game->m_pack.blob(table->hash), game->m_names))
        return nullptr;
    const PackName* const listing = findPackName(game->m_names, GameScheme);
    if (listing == nullptr)
        return nullptr;
    game->m_files = linesOf(game->m_pack.blob(listing->content));
    std::sort(game->m_files.begin(), game->m_files.end());
    opened[key] = game;
    return game;
}

bool SealedGame::has(std::string_view relative) const noexcept
{
    return std::binary_search(m_files.begin(), m_files.end(), relative,
                              [](std::string_view a, std::string_view b) { return a < b; });
}

bool SealedGame::hasUnder(std::string_view directory) const noexcept
{
    std::string prefix(directory);
    prefix.push_back('/');
    const auto at = std::lower_bound(m_files.begin(), m_files.end(), prefix);
    return at != m_files.end() && at->starts_with(prefix);
}

std::vector<std::string> SealedGame::filesUnder(std::string_view directory) const
{
    std::string prefix(directory);
    prefix.push_back('/');
    std::vector<std::string> out;
    for (auto at = std::lower_bound(m_files.begin(), m_files.end(), prefix);
         at != m_files.end() && at->starts_with(prefix); ++at)
        out.push_back(*at);
    return out;
}

std::span<const std::byte> SealedGame::read(std::string_view relative) const
{
    const PackName* const row = findPackName(m_names, nameOf(relative));
    return row != nullptr ? m_pack.blob(row->content) : std::span<const std::byte>{};
}

bool SealedGame::readText(std::string_view relative, std::string& out) const
{
    if (!has(relative))
        return false;
    const std::span<const std::byte> bytes = read(relative);
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

} // namespace engine::asset
