#include "engine/app/content_import.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <future>
#include <map>
#include <optional>
#include <string>

#include "engine/app/content_tree.h"
#include "engine/asset/gltf.h"
#include "engine/asset/material.h"
#include "engine/asset/model.h"
#include "engine/core/content_hash.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/platform/file.h"

#if ENG_DEBUG_UI
#include "engine/assetc/compiler.h"
#endif

namespace engine::app {

std::filesystem::path importObjectsDir(const std::filesystem::path& projectRoot)
{
    return projectRoot / ".engine" / "import" / "objects";
}

std::filesystem::path importIndexPath(const std::filesystem::path& projectRoot)
{
    return projectRoot / ".engine" / "import" / "index.json";
}

std::filesystem::path importSourcesPath(const std::filesystem::path& projectRoot)
{
    return projectRoot / ".engine" / "import" / "sources.json";
}

#if ENG_DEBUG_UI

namespace {

// **What a source was when it last compiled** (D507): its size and its time,
// as text -- a time is a count of ticks past a double's exact integers.
struct SourceStamp
{
    std::string size;
    std::string written;
    bool operator==(const SourceStamp&) const = default;
};

[[nodiscard]] std::optional<SourceStamp> stampOf(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec)
        return std::nullopt;
    const auto written = std::filesystem::last_write_time(file, ec);
    if (ec)
        return std::nullopt;
    // As a 64-bit count: the clock's own count is 128 bits on macOS, which
    // `std::to_string` has no overload for.
    return SourceStamp{std::to_string(static_cast<unsigned long long>(size)),
                       std::to_string(static_cast<long long>(written.time_since_epoch().count()))};
}

// The stamps remembered under `fingerprint`, or none: a different importer, or
// a store that is not there to have been compiled into, remembers nothing.
[[nodiscard]] std::map<std::string, SourceStamp> readStamps(const std::filesystem::path& projectRoot,
                                                            std::string_view fingerprint)
{
    std::map<std::string, SourceStamp> stamps;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(importIndexPath(projectRoot), ec))
        return stamps;
    std::string text;
    if (!platform::readTextFile(importSourcesPath(projectRoot), text))
        return stamps;
    core::JsonDocument document;
    if (!document.parse(text, importSourcesPath(projectRoot).string()).ok)
        return stamps;
    const core::JsonValue root = document.root();
    if (root["fingerprint"].asString() != fingerprint)
        return stamps;
    const core::JsonValue sources = root["sources"];
    for (core::usize at = 0; at < sources.size(); ++at) {
        const core::JsonValue entry = sources.at(at);
        stamps.emplace(std::string(entry["path"].asString()),
                       SourceStamp{std::string(entry["size"].asString()), std::string(entry["written"].asString())});
    }
    return stamps;
}

void writeStamps(const std::filesystem::path& projectRoot, std::string_view fingerprint,
                 const std::map<std::string, SourceStamp>& stamps)
{
    core::JsonWriter out(core::JsonLayout::Indented);
    out.beginObject();
    out.field("fingerprint", fingerprint);
    out.key("sources");
    out.beginArray();
    for (const auto& [path, stamp] : stamps) {
        out.beginObject();
        out.field("path", path);
        out.field("size", stamp.size);
        out.field("written", stamp.written);
        out.endObject();
    }
    out.endArray();
    out.endObject();
    // A cache: a write that fails costs the next open its speed, never its
    // content.
    (void)platform::writeTextFile(importSourcesPath(projectRoot), out.text());
}

// **What decides how a source compiles, apart from its own bytes**: the
// importer's pinned options and rules, and the project's materials -- a
// material decides whether a loose image is colour, and so its bytes.
[[nodiscard]] std::string importFingerprint(const assetc::CompileOptions& options,
                                            const std::filesystem::path& contentRoot)
{
    core::ContentHasher hasher;
    const core::ContentHash importer = assetc::importerFingerprint(options);
    const std::string importerText = importer.toHex();
    hasher.update(std::as_bytes(std::span<const char>(importerText.data(), importerText.size())));
    ContentTree tree;
    (void)tree.open(contentRoot);
    for (const std::string& material : tree.filesOfKind(ContentKind::Material)) {
        const std::optional<SourceStamp> stamp = stampOf(contentRoot / std::filesystem::path(material));
        const std::string line = material + "|" + (stamp ? stamp->size + "|" + stamp->written : std::string());
        hasher.update(std::as_bytes(std::span<const char>(line.data(), line.size())));
    }
    return hasher.finish().toHex();
}

} // namespace

ContentImportReport compileImported(const std::filesystem::path& projectRoot, const std::filesystem::path& contentRoot,
                                    std::span<const std::string> names, const ImportProgress& progress,
                                    bool skipUnchanged)
{
    ContentImportReport report;
    if (projectRoot.empty() || contentRoot.empty() || names.empty())
        return report;

    assetc::CompileOptions options;
    options.inputRoot = contentRoot;
    // **The project's own cache**, beside the store rather than inside
    // `content/`: a build that compiled its own cache would produce a store that
    // grew every import, which is the mistake `collectSources`'s exclusion
    // already exists to prevent on the command-line side.
    options.cacheRoot = projectRoot / ".engine" / "import" / "cache";

    // **Only what changed since it last compiled** (D507). Opening a project
    // asked the compiler about every source, every time: it read each one,
    // hashed it, found it in the cache and wrote the store's index again --
    // 3.8 s before the first frame of a project of two hundred sources, with
    // nothing to do. A source the same size and time as when it compiled, under
    // the same importer and materials, is skipped without being opened.
    const std::string fingerprint = skipUnchanged ? importFingerprint(options, contentRoot) : std::string();
    const std::map<std::string, SourceStamp> known =
        skipUnchanged ? readStamps(projectRoot, fingerprint) : std::map<std::string, SourceStamp>{};
    std::map<std::string, SourceStamp> stamps;

    std::vector<std::string_view> todo;
    for (const std::string& name : names) {
        // Only what the compiler has something to do with. A script or a scene
        // is content the project reads directly, and skipping it is not a
        // failure -- there is nothing to compile.
        const ContentKind kind = contentKindOf(name);
        if (kind != ContentKind::Mesh && kind != ContentKind::Texture)
            continue;
        if (skipUnchanged) {
            const std::optional<SourceStamp> stamp = stampOf(contentRoot / std::filesystem::path(name));
            const auto found = known.find(name);
            if (stamp.has_value() && found != known.end() && found->second == *stamp) {
                stamps.emplace(name, *stamp);
                continue;
            }
        }
        todo.push_back(name);
    }

    for (core::usize at = 0; at < todo.size(); ++at) {
        const std::string name(todo[at]);
        if (progress)
            progress(at, todo.size(), name);

        const std::filesystem::path source = contentRoot / std::filesystem::path(name);
        const std::optional<SourceStamp> before = skipUnchanged ? stampOf(source) : std::nullopt;
        // **On a thread of its own, one source at a time, while the caller's
        // window keeps answering** (D507): one model took nine seconds, and a
        // window silent for five is one Windows covers with a white copy of it.
        // One at a time because the encoder's format setters are process-wide
        // (`compiler.h`, rule 4).
        assetc::CompileResult result;
        if (progress) {
            std::future<assetc::CompileResult> pending =
                std::async(std::launch::async, [&options, &source] { return assetc::importOne(options, source); });
            while (pending.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
                progress(at, todo.size(), name);
            result = pending.get();
        }
        else {
            result = assetc::importOne(options, source);
        }
        if (!result.ok) {
            report.failed.push_back(name);
            if (report.diagnostic.empty())
                report.diagnostic = result.diagnostic;
            continue;
        }

        // **Written per source rather than once at the end**, so an import of
        // forty files that fails on the thirty-first leaves thirty compiled
        // rather than nothing. The store merges, which is what makes that safe.
        if (const auto error =
                assetc::writeObjectStore(result, importObjectsDir(projectRoot), importIndexPath(projectRoot))) {
            report.failed.push_back(name);
            if (report.diagnostic.empty())
                report.diagnostic = error->message;
            continue;
        }

        report.compiled.push_back(name);
        if (before.has_value())
            stamps.emplace(name, *before);
        report.meshes += result.meshCount;
        report.textures += result.textureCount;
        report.cacheHits += result.stats.cacheHits;
        report.cacheMisses += result.stats.cacheMisses;

        // The fragments this source produced, read off the rows it produced --
        // so the editor names an instance with the same word the store is keyed
        // by, rather than deriving it a second time and hoping.
        std::vector<std::string> fragments;
        const std::string urn = "asset://" + std::filesystem::path(name).generic_string();
        for (const assetc::ManifestEntry& entry : result.entries) {
            const std::size_t hash = entry.urn.find('#');
            if (hash == std::string::npos || entry.urn.compare(0, hash, urn) != 0)
                continue;
            fragments.push_back(entry.urn.substr(hash + 1));
        }
        if (!fragments.empty())
            report.pieces.emplace_back(name, std::move(fragments));
    }
    if (progress && !todo.empty())
        progress(todo.size(), todo.size(), {});

    // Only what compiled, or was already compiled and has not moved: a source
    // that failed is asked about again next time.
    if (skipUnchanged && (!todo.empty() || stamps.size() != known.size()))
        writeStamps(projectRoot, fingerprint, stamps);

    return report;
}

#else

ContentImportReport compileImported(const std::filesystem::path&, const std::filesystem::path&,
                                    std::span<const std::string>, const ImportProgress&, bool)
{
    // A build with no editor imports nothing, because it has no browser to
    // import from. The symbol exists so the call site needs no `#ifdef` of its
    // own -- the same shape `DebugOverlay` uses to keep the frame loop free of
    // them.
    return {};
}

#endif

ContentImportReport openProjectContent(const std::filesystem::path& projectRoot,
                                       const std::filesystem::path& contentRoot, asset::ContentMounts& mounts,
                                       const ImportProgress& progress)
{
    ContentImportReport report;
    std::error_code ec;

    // **A source tree is optional and the store is not.** They are separate
    // conditions rather than one, because the compiled form is the authoritative
    // one now: a project whose `content/` has been stripped -- shipped compiled,
    // or simply deleted -- still has everything it needs to run, and refusing to
    // mount its store because the sources are gone would be the tail wagging the
    // dog.
    if (!contentRoot.empty() && std::filesystem::is_directory(contentRoot, ec)) {
        mounts.mountDirectory(contentRoot);

        // **The tree is walked here rather than taken from the editor's**, so a
        // headless run needs no browser. A `ContentTree` is a directory scan and
        // nothing else -- no watcher, no window -- and the editor's own copy
        // stays its own, because a tool's view of a folder is a tool's business.
        ContentTree tree;
        (void)tree.open(contentRoot);
        std::vector<std::string> pending = tree.filesOfKind(ContentKind::Mesh);
        for (std::string& texture : tree.filesOfKind(ContentKind::Texture))
            pending.push_back(std::move(texture));
        if (!pending.empty())
            report = compileImported(projectRoot, contentRoot, pending, progress, true);
    }

    // **Above the source directory**, which is the point: `resolve` walks mounts
    // in reverse, so the compiled form of a texture -- BC7 with mips -- outranks
    // the raw PNG beside it, and the compiled form of a mesh outranks the JSON
    // that no longer has a reader.
    //
    // A store that is not there is not an error. A project whose content the
    // compiler had nothing to do with has none, and so does one in a build with
    // no compiler.
    const std::filesystem::path objects = importObjectsDir(projectRoot);
    const std::filesystem::path index = importIndexPath(projectRoot);
    if (std::filesystem::is_regular_file(index, ec))
        (void)mounts.mountObjects(objects, index);

    return report;
}

// --- The materials of an imported model (ADR 0090) ----------------------------

namespace {

// The JSON a glTF carries: the whole file for a `.gltf`, the first chunk of a
// `.glb`.
[[nodiscard]] std::string gltfJson(std::span<const std::byte> bytes)
{
    const auto word = [&](core::usize at) -> core::u32 {
        core::u32 value = 0;
        for (core::usize index = 0; index < 4; ++index)
            value |= static_cast<core::u32>(bytes[at + index]) << (8 * index);
        return value;
    };
    // "glTF", then a version and a length; the JSON chunk follows.
    if (bytes.size() >= 20 && static_cast<char>(bytes[0]) == 'g' && static_cast<char>(bytes[1]) == 'l' &&
        static_cast<char>(bytes[2]) == 'T' && static_cast<char>(bytes[3]) == 'F') {
        const core::u32 length = word(12);
        if (20 + static_cast<core::usize>(length) > bytes.size())
            return {};
        return std::string(reinterpret_cast<const char*>(bytes.data() + 20), length);
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A URI relative to the model, as a content path: `%20` decoded, `./` and
// `a/../` folded away. Empty for anything that is not a plain relative file.
[[nodiscard]] std::string contentPathOf(const std::string& folder, std::string_view uri)
{
    if (uri.empty() || uri.starts_with("data:") || uri.find("://") != std::string_view::npos)
        return {};
    std::string decoded;
    for (core::usize index = 0; index < uri.size(); ++index) {
        if (uri[index] == '%' && index + 2 < uri.size()) {
            const std::string hex(uri.substr(index + 1, 2));
            decoded.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            index += 2;
            continue;
        }
        decoded.push_back(uri[index] == '\\' ? '/' : uri[index]);
    }
    const std::filesystem::path joined =
        (folder.empty() ? std::filesystem::path(decoded) : std::filesystem::path(folder) / decoded).lexically_normal();
    const std::string text = joined.generic_string();
    return text.starts_with("..") ? std::string{} : text;
}

// A file name from a material's name.
[[nodiscard]] std::string materialStem(std::string_view name, core::usize index)
{
    std::string out;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(std::isalnum(byte) != 0 || c == '-' || c == '_' ? static_cast<char>(std::tolower(byte)) : '-');
    }
    return out.empty() ? "material-" + std::to_string(index) : out;
}

} // namespace

std::string ModelMaterials::whole() const
{
    if (bySubmesh.empty())
        return {};
    for (const std::string& path : bySubmesh) {
        if (path != bySubmesh.front())
            return {};
    }
    return bySubmesh.front();
}

std::string ModelMaterials::ofPiece(std::string_view piece) const
{
    for (core::usize index = 0; index < submeshNames.size() && index < bySubmesh.size(); ++index) {
        if (submeshNames[index] == piece)
            return bySubmesh[index];
    }
    return {};
}

ModelMaterials writeModelMaterials(const std::filesystem::path& contentRoot, const std::string& modelRelative)
{
    ModelMaterials out;
    const std::filesystem::path source = contentRoot / std::filesystem::path(modelRelative);
    std::vector<std::byte> bytes;
    if (!platform::readFile(source, bytes))
        return out;

    // Which source material each submesh draws with -- the importer's own
    // answer, so a piece and its material are one reading of the file.
    asset::Model model;
    if (asset::importGltf(bytes, source.parent_path(), asset::GltfImportOptions{}, model).has_value() ||
        model.materialSources.size() != model.materials.size())
        return out;

    core::JsonDocument document;
    if (!document.parse(gltfJson(bytes), modelRelative))
        return out;
    const core::JsonValue gltf = document.root();
    const core::JsonValue materials = gltf["materials"];
    const core::JsonValue textures = gltf["textures"];
    const core::JsonValue images = gltf["images"];

    const std::string folder = std::filesystem::path(modelRelative).parent_path().generic_string();
    const std::string stem = std::filesystem::path(modelRelative).stem().string();

    // The content path of the image a texture reference names, or empty.
    const auto mapOf = [&](const core::JsonValue& info, bool& unresolved) -> std::string {
        if (info.type() != core::JsonType::Object)
            return {};
        const core::i64 texture = info["index"].asInteger(-1);
        const core::i64 imageIndex = texture >= 0 && static_cast<core::usize>(texture) < textures.size()
                                         ? textures.at(static_cast<core::usize>(texture))["source"].asInteger(-1)
                                         : -1;
        const std::string path =
            imageIndex >= 0 && static_cast<core::usize>(imageIndex) < images.size()
                ? contentPathOf(folder, images.at(static_cast<core::usize>(imageIndex))["uri"].asString())
                : std::string{};
        if (path.empty())
            unresolved = true;
        return path.empty() ? std::string{} : std::string(asset::AssetScheme) + path;
    };

    std::vector<std::string> bySource(materials.size());
    std::vector<std::string> usedStems;
    for (core::usize index = 0; index < materials.size(); ++index) {
        const core::JsonValue material = materials.at(index);
        const core::JsonValue pbr = material["pbrMetallicRoughness"];
        asset::MaterialAsset asset;
        asset.written = asset::AllMaterialFields;
        asset::MaterialProperties& p = asset.properties;
        // glTF's own defaults where the file says nothing: metal and rough.
        p.metalness = static_cast<core::f32>(pbr["metallicFactor"].asNumber(1.0));
        p.roughness = static_cast<core::f32>(pbr["roughnessFactor"].asNumber(1.0));
        const std::string_view alphaMode = material["alphaMode"].asString("OPAQUE");
        p.alphaMode = alphaMode == "MASK" ? 1 : alphaMode == "BLEND" ? 2 : 0;
        p.alphaCutoff = static_cast<core::f32>(material["alphaCutoff"].asNumber(0.5));
        p.doubleSided = material["doubleSided"].asBool(false);
        if (const core::JsonValue factor = pbr["baseColorFactor"]; factor.size() >= 3) {
            p.color = core::Color3{static_cast<core::f32>(factor.at(0).asNumber(1.0)),
                                   static_cast<core::f32>(factor.at(1).asNumber(1.0)),
                                   static_cast<core::f32>(factor.at(2).asNumber(1.0))};
            // Alpha is see-through only where the file blends it.
            if (factor.size() >= 4 && p.alphaMode == 2)
                p.transparency = 1.0f - static_cast<core::f32>(factor.at(3).asNumber(1.0));
        }
        if (const core::JsonValue factor = material["emissiveFactor"]; factor.size() >= 3) {
            p.emissive = core::Color3{static_cast<core::f32>(factor.at(0).asNumber(0.0)),
                                      static_cast<core::f32>(factor.at(1).asNumber(0.0)),
                                      static_cast<core::f32>(factor.at(2).asNumber(0.0))};
        }
        bool unresolved = false;
        p.colorMap = mapOf(pbr["baseColorTexture"], unresolved);
        p.metallicRoughnessMap = mapOf(pbr["metallicRoughnessTexture"], unresolved);
        p.normalMap = mapOf(material["normalTexture"], unresolved);
        p.normalScale = static_cast<core::f32>(material["normalTexture"]["scale"].asNumber(1.0));
        p.emissiveMap = mapOf(material["emissiveTexture"], unresolved);
        if (unresolved)
            continue;

        std::string name = materialStem(material["name"].asString(), index);
        for (int suffix = 2; std::find(usedStems.begin(), usedStems.end(), name) != usedStems.end(); ++suffix)
            name = materialStem(material["name"].asString(), index) + "-" + std::to_string(suffix);
        usedStems.push_back(name);

        const std::string relative = "materials/" + stem + "/" + name + std::string(asset::MaterialSuffix);
        const std::filesystem::path absolute = contentRoot / std::filesystem::path(relative);
        std::error_code ec;
        if (!std::filesystem::exists(absolute, ec)) {
            if (!platform::createDirectories(absolute.parent_path()) ||
                !platform::writeTextFile(absolute, asset::writeMaterialAsset(asset)))
                continue;
            out.written.push_back(relative);
        }
        bySource[index] = relative;
    }

    out.submeshNames = model.submeshNames;
    for (const asset::Submesh& submesh : model.mesh.submeshes) {
        const core::u32 origin = submesh.material < model.materialSources.size()
                                     ? model.materialSources[submesh.material]
                                     : asset::Model::NoSourceMaterial;
        out.bySubmesh.push_back(origin < bySource.size() ? bySource[origin] : std::string{});
    }
    return out;
}

} // namespace engine::app
