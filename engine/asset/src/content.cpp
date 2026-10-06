#include "engine/asset/content.h"

#include <algorithm>
#include <functional>

#include "engine/core/content_path.h"
#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/text_key.h"
#include "engine/platform/file.h"

namespace engine::asset {
namespace {

using core::I18nArg;

[[nodiscard]] AssetKind kindFromName(std::string_view name) noexcept
{
    if (name == "mesh") {
        return AssetKind::Mesh;
    }
    if (name == "texture") {
        return AssetKind::Texture;
    }
    if (name == "prefab") {
        return AssetKind::Prefab;
    }
    if (name == "chunk") {
        return AssetKind::Chunk;
    }
    if (name == "raw") {
        return AssetKind::Raw;
    }
    if (name == "material") {
        return AssetKind::Material;
    }
    if (name == "surface") {
        return AssetKind::Surface;
    }
    return AssetKind::Unknown;
}

// The manifest both compiled mount kinds carry: `content-manifest`, a
// list of `{urn, hash, kind}`. `onEntry` decides what to do with a row, which
// is the one place a pack and an object store differ -- a pack checks its TOC
// holds the blob, a store does not.
[[nodiscard]] std::optional<core::EngineError> readManifest(
    const std::filesystem::path& manifestPath,
    const std::function<std::optional<core::EngineError>(std::string_view, const core::ContentHash&, AssetKind)>&
        onEntry)
{
    std::string text;
    if (!platform::readTextFile(manifestPath, text)) {
        const I18nArg args[] = {{"content", manifestPath.string()}};
        return core::makeError(ENG_TR("asset.manifest.err.open_failed"), args);
    }

    core::JsonDocument document;
    const core::JsonDocument::ParseResult parsed = document.parse(text, manifestPath.string());
    if (!parsed.ok) {
        const I18nArg args[] = {{"detail", parsed.diagnostic}};
        return core::makeError(ENG_TR("asset.manifest.err.malformed"), args);
    }

    const core::JsonValue root = document.root();
    if (root["format"].asString() != "content-manifest") {
        const I18nArg args[] = {{"detail", "not a content manifest"}};
        return core::makeError(ENG_TR("asset.manifest.err.malformed"), args);
    }

    const core::JsonValue assets = root["assets"];
    for (usize i = 0; i < assets.size(); ++i) {
        const core::JsonValue entry = assets.at(i);
        const std::string_view urn = entry["urn"].asString();
        const std::string_view hex = entry["hash"].asString();

        core::ContentHash hash;
        if (!isValidUrn(urn) || !core::parseHex(hex, hash)) {
            const I18nArg args[] = {{"detail", std::string(urn)}};
            return core::makeError(ENG_TR("asset.manifest.err.malformed"), args);
        }

        if (auto error = onEntry(urn, hash, kindFromName(entry["kind"].asString())); error.has_value()) {
            return error;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<core::EngineError> readContentManifest(const std::filesystem::path& manifest,
                                                     std::vector<ManifestRow>& out)
{
    out.clear();
    return readManifest(manifest,
                        [&out](std::string_view urn, const core::ContentHash& hash,
                               AssetKind kind) -> std::optional<core::EngineError> {
                            out.push_back(ManifestRow{std::string(urn), hash, kind});
                            return std::nullopt;
                        });
}

bool isValidUrn(std::string_view urn)
{
    if (urn.size() <= AssetScheme.size() || urn.substr(0, AssetScheme.size()) != AssetScheme) {
        return false;
    }
    const std::string_view path = urn.substr(AssetScheme.size());
    if (path.empty() || path.front() == '/') {
        return false;
    }

    // A URN is a name inside the mount and never a way out of it. Refused here
    // rather than at the filesystem, because a shipped game resolving against a
    // pack has no filesystem to refuse it -- and because `..` inside a pack key
    // would silently never match, which reads as "missing asset" rather than as
    // "you wrote something that cannot work". **The one check** every outside
    // path goes through (audit F4), and already in its folded form: a drive, a
    // share, a `:` or a device name is not a name in any mount.
    const std::optional<std::string> safe = core::safeRelativePath(path);
    return safe.has_value() && *safe == path;
}

std::string_view urnPath(std::string_view urn)
{
    return isValidUrn(urn) ? urn.substr(AssetScheme.size()) : std::string_view{};
}

void ContentMounts::mountDirectory(std::filesystem::path root)
{
    Mount mount;
    mount.kind = MountKind::Directory;
    mount.directory = std::move(root);
    m_mounts.push_back(std::move(mount));
}

std::optional<core::EngineError> ContentMounts::mountPack(const std::filesystem::path& pack,
                                                          const std::filesystem::path& manifest)
{
    std::filesystem::path manifestPath = manifest;
    if (manifestPath.empty()) {
        manifestPath = pack;
        manifestPath.replace_extension();
        manifestPath += ".manifest.json";
    }

    Mount mount;
    mount.kind = MountKind::Pack;
    mount.pack = std::make_unique<Pack>();
    if (auto error = openPackFile(pack, *mount.pack)) {
        return error;
    }

    // **A sealed pack is its own manifest** (ADR 0183).
    if (const PackEntry* const table = mount.pack->names(); table != nullptr) {
        if (!decodePackNames(mount.pack->blob(table->hash), mount.names)) {
            const I18nArg args[] = {{"hash", table->hash.toHex()}};
            return core::makeError(ENG_TR("asset.pack.err.hash_mismatch"), args);
        }
        for (const PackName& name : mount.names) {
            if (!mount.pack->contains(name.content)) {
                const I18nArg args[] = {{"content", name.content.toHex()}};
                return core::makeError(ENG_TR("asset.manifest.err.missing_blob"), args);
            }
        }
        mount.sealed = true;
        m_mounts.push_back(std::move(mount));
        return std::nullopt;
    }

    if (auto error = readManifest(manifestPath,
                                  [&mount](std::string_view urn, const core::ContentHash& hash,
                                           AssetKind kind) -> std::optional<core::EngineError> {
                                      const PackEntry* const found = mount.pack->find(hash);
                                      if (found == nullptr) {
                                          // Refused at mount rather than at first use. A game that
                                          // starts and then cannot find its world is worse than one
                                          // that says why it will not start.
                                          const I18nArg args[] = {{"content", std::string(urn)}};
                                          return core::makeError(ENG_TR("asset.manifest.err.missing_blob"), args);
                                      }

                                      PackEntry record = *found;
                                      record.kind = kind;
                                      mount.byUrn.emplace(std::string(urn), record);
                                      return std::nullopt;
                                  });
        error.has_value()) {
        return error;
    }

    m_mounts.push_back(std::move(mount));
    return std::nullopt;
}

std::optional<core::EngineError> ContentMounts::mountObjects(std::filesystem::path objects,
                                                             const std::filesystem::path& index)
{
    Mount mount;
    mount.kind = MountKind::Objects;
    mount.objects = std::move(objects);

    // No blob check. See the header: this store is repaired by re-importing,
    // and one stale row must not take a whole project offline.
    if (auto error = readManifest(index,
                                  [&mount](std::string_view urn, const core::ContentHash& hash,
                                           AssetKind kind) -> std::optional<core::EngineError> {
                                      PackEntry record;
                                      record.hash = hash;
                                      record.kind = kind;
                                      // `offset` and the sizes stay zero: they describe a position
                                      // inside an archive, and there is no archive. The size a
                                      // caller sees is the span `blob` hands back, which is the
                                      // file's own length.
                                      mount.byUrn.emplace(std::string(urn), record);
                                      return std::nullopt;
                                  });
        error.has_value()) {
        return error;
    }

    m_mounts.push_back(std::move(mount));
    return std::nullopt;
}

std::filesystem::path ContentMounts::objectPath(const std::filesystem::path& objects, const core::ContentHash& hash)
{
    const std::string hex = hash.toHex();
    std::filesystem::path path = objects;
    path /= hex.substr(0, 2);
    path /= hex;
    return path;
}

std::span<const std::byte> ContentMounts::objectBytes(const Mount& mount, const core::ContentHash& hash) const
{
    if (const auto cached = mount.loaded.find(hash); cached != mount.loaded.end()) {
        return cached->second;
    }

    const std::filesystem::path path = objectPath(mount.objects, hash);
    std::vector<std::byte> bytes;
    if (!platform::readFile(path, bytes)) {
        const I18nArg args[] = {{"content", path.string()}};
        core::logText(core::LogLevel::Warn, core::makeError(ENG_TR("asset.objects.err.missing_object"), args).message);
        // Remembered as empty, so the next lookup neither re-opens the file nor
        // re-warns -- a mesh asked for once per frame would otherwise fill the
        // log with one line per frame for as long as the project is open.
        bytes.clear();
    }
    return mount.loaded.emplace(hash, std::move(bytes)).first->second;
}

void ContentMounts::clear()
{
    m_mounts.clear();
}

ResolvedContent ContentMounts::resolve(std::string_view urn) const
{
    ResolvedContent result;
    if (!isValidUrn(urn)) {
        return result;
    }
    const std::string_view relative = urn.substr(AssetScheme.size());

    // Reverse order: a later mount wins, so a project overrides engine content
    // by mounting after it.
    for (auto mount = m_mounts.rbegin(); mount != m_mounts.rend(); ++mount) {
        if (mount->sealed) {
            if (const PackName* const name = findPackName(mount->names, urn); name != nullptr) {
                result.source = ResolvedContent::Source::Pack;
                result.kind = name->kind;
                result.hash = name->content;
                result.bytes = mount->pack->blob(name->content);
                return result;
            }
            continue;
        }
        if (mount->kind != MountKind::Directory) {
            const auto entry = mount->byUrn.find(std::string(urn));
            if (entry != mount->byUrn.end()) {
                result.source = ResolvedContent::Source::Pack;
                result.kind = entry->second.kind;
                result.hash = entry->second.hash;
                result.bytes = mount->kind == MountKind::Pack ? mount->pack->blob(entry->second.hash)
                                                              : objectBytes(*mount, entry->second.hash);
                return result;
            }
            continue;
        }

        std::filesystem::path candidate = mount->directory;
        candidate /= std::filesystem::path(relative);
        // `platform::fileExists` -- an open and a close -- rather than
        // `std::filesystem::exists`, because inside an APK the content
        // directory is a set of zip entries and no path any C runtime can stat
        // (file.h). Existence is "can it be opened".
        //
        // It used to be `platform::readFile`, which READ THE WHOLE FILE and
        // threw the bytes away (D039). Every streamed chunk was therefore read
        // twice -- once synchronously here, on the frame thread, inside the
        // streaming pump, and once asynchronously by the caller that had just
        // asked where it was. On a slow filesystem that first read was a hitch
        // of tens of milliseconds.
        if (platform::fileExists(candidate)) {
            result.source = ResolvedContent::Source::Loose;
            result.path = std::move(candidate);
            return result;
        }
    }
    return result;
}

std::span<const std::byte> ContentMounts::blob(const core::ContentHash& hash) const
{
    for (auto mount = m_mounts.rbegin(); mount != m_mounts.rend(); ++mount) {
        if (mount->kind == MountKind::Objects) {
            const std::span<const std::byte> bytes = objectBytes(*mount, hash);
            if (!bytes.empty()) {
                return bytes;
            }
            // An empty answer here falls through to the next mount rather than
            // ending the search: this store holds whatever was last imported,
            // and a blob it is missing may still be in a pack underneath it.
            continue;
        }
        if (mount->kind != MountKind::Pack) {
            continue;
        }
        const std::span<const std::byte> bytes = mount->pack->blob(hash);
        if (!bytes.empty() || mount->pack->contains(hash)) {
            return bytes;
        }
    }
    return {};
}

std::span<const std::byte> ContentMounts::named(std::string_view name) const
{
    for (auto mount = m_mounts.rbegin(); mount != m_mounts.rend(); ++mount) {
        if (!mount->sealed)
            continue;
        if (const PackName* const row = findPackName(mount->names, name); row != nullptr)
            return mount->pack->blob(row->content);
    }
    return {};
}

std::vector<const Pack*> ContentMounts::packs() const
{
    std::vector<const Pack*> out;
    for (auto mount = m_mounts.rbegin(); mount != m_mounts.rend(); ++mount) {
        if (mount->kind == MountKind::Pack && mount->pack != nullptr)
            out.push_back(mount->pack.get());
    }
    return out;
}

std::vector<std::string> ContentMounts::packedUrns() const
{
    std::vector<std::string> urns;
    for (const Mount& mount : m_mounts) {
        for (const auto& entry : mount.byUrn) {
            urns.push_back(entry.first);
        }
    }
    // Sorted, and deduplicated: the same URN in two mounts is one name, and a
    // caller listing content should see what `resolve` would answer rather than
    // the mount history.
    std::sort(urns.begin(), urns.end());
    urns.erase(std::unique(urns.begin(), urns.end()), urns.end());
    return urns;
}

} // namespace engine::asset
