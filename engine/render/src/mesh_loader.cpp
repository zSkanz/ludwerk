#include "engine/render/mesh_loader.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <string_view>

#include "engine/asset/content.h"
#include "engine/asset/gltf.h"
#include "engine/asset/image.h"
#include "engine/asset/mesh_format.h"
#include "engine/asset/primitives.h"
#include "engine/asset/terrain_layers.h"
#include "engine/asset/texture.h"
#include "engine/core/content_path.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/text_key.h"
#include "engine/platform/file.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::render {
namespace {

using core::f32;
using core::u32;

constexpr std::string_view kAssetScheme = "asset://";

// `asset://models/tree.glb` resolves under the project's content directory. A
// URN with no scheme is taken as a path relative to the same root rather than
// rejected: the error a developer gets from a mistyped scheme should be "no such
// file", naming the path that was tried, and not a lecture about URNs.
//
// **Under the root or nowhere** (audit F4): a name that is not a path under it
// -- a drive, a share, a `..` above it -- resolves to no file, and reads as
// missing.
[[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& root, std::string_view urn)
{
    std::string_view relative = urn;
    if (relative.substr(0, kAssetScheme.size()) == kAssetScheme)
        relative.remove_prefix(kAssetScheme.size());
    return core::resolveUnder(root, relative).value_or(std::filesystem::path{});
}

// **sRGB for a colour, linear for data**, which is what the asset compiler
// decided for the compiled forms (E9): a base colour or an emission is authored
// in sRGB and must reach the shader linear, and a normal or a metal/roughness
// map is numbers that must not be touched. The loose path uploaded everything
// as linear for eight milestones, so a colour map drew visibly paler before its
// project was compiled than after.
[[nodiscard]] rhi::TextureHandle uploadImage(rhi::IDevice& device, rhi::ICmdList& cmd, const asset::Image& image,
                                             const char* debugName, bool srgb = false)
{
    if (!image.valid())
        return {};

    const rhi::TextureHandle handle = device.createTexture({
        .format = srgb ? rhi::TextureFormat::Rgba8UnormSrgb : rhi::TextureFormat::Rgba8Unorm,
        .usage = rhi::TextureUsage::Sampled,
        .width = image.width,
        .height = image.height,
        .debugName = debugName,
    });
    if (!handle.valid())
        return {};

    cmd.uploadTexture(handle, image.pixels, 0);
    return handle;
}

// **The transfer function the compiler wrote, honoured** (ADR 0073). It
// ignored `srgb` for as long as the compiled path existed, so every colour the
// compiler had correctly marked sRGB was sampled as if it were already linear
// -- the pale, washed look every imported model had.
[[nodiscard]] rhi::TextureFormat toRhi(asset::TextureFormat format, bool srgb) noexcept
{
    switch (format) {
    case asset::TextureFormat::Bc1Rgb:
        return srgb ? rhi::TextureFormat::Bc1RgbaUnormSrgb : rhi::TextureFormat::Bc1RgbaUnorm;
    case asset::TextureFormat::Bc3Rgba:
        return srgb ? rhi::TextureFormat::Bc3RgbaUnormSrgb : rhi::TextureFormat::Bc3RgbaUnorm;
    case asset::TextureFormat::Bc5Rg:
        // Two channels of data -- a normal map -- and never a colour.
        return rhi::TextureFormat::Bc5RgUnorm;
    case asset::TextureFormat::Bc7Rgba:
        return srgb ? rhi::TextureFormat::Bc7RgbaUnormSrgb : rhi::TextureFormat::Bc7RgbaUnorm;
    case asset::TextureFormat::Astc4x4Rgba:
        return srgb ? rhi::TextureFormat::Astc4x4RgbaUnormSrgb : rhi::TextureFormat::Astc4x4RgbaUnorm;
    case asset::TextureFormat::Rgba8:
    case asset::TextureFormat::Unknown:
        break;
    }
    return srgb ? rhi::TextureFormat::Rgba8UnormSrgb : rhi::TextureFormat::Rgba8Unorm;
}

// A transcoded texture, with every mip it carries. The block-compressed path is
// the point of the pipeline: a BC7 texture is a quarter of the GPU memory an
// RGBA8 one costs, and the memory ceiling is what M7's gate measures.
[[nodiscard]] rhi::TextureHandle uploadTranscoded(rhi::IDevice& device, rhi::ICmdList& cmd,
                                                  const asset::TextureAsset& texture, const char* debugName)
{
    if (!texture.valid())
        return {};

    const rhi::TextureHandle handle = device.createTexture({
        .format = toRhi(texture.format, texture.srgb),
        .usage = rhi::TextureUsage::Sampled,
        .width = texture.width,
        .height = texture.height,
        .layers = 1,
        .mipLevels = static_cast<u32>(texture.mips.size()),
        .debugName = debugName,
    });
    if (!handle.valid())
        return {};

    for (u32 level = 0; level < static_cast<u32>(texture.mips.size()); ++level) {
        const asset::TextureMip& mip = texture.mips[level];
        cmd.uploadTexture(handle, std::span<const std::byte>(texture.pixels.data() + mip.offset, mip.size), level);
    }
    return handle;
}

// **A mesh's morph targets, on the card and in its entry** (ADR 0196), for
// both feeds. `submeshes` and `indices` are level zero's: a coarser level is
// other triangles over the same vertices, so a section no target reaches at
// level zero is reached at none.
void giveMorphs(rhi::IDevice& device, rhi::ICmdList& cmd, MeshCache& cache, MeshLibrary::Entry& entry,
                std::span<const asset::MorphTarget> targets, core::usize vertexCount,
                std::span<const asset::Submesh> submeshes, std::span<const u32> indices, std::string_view urn)
{
    if (targets.empty())
        return;
    const MorphTable table = buildMorphTable(targets, static_cast<u32>(vertexCount));
    if (table.refusedBytes != 0) {
        const core::I18nArg args[] = {
            {"mesh", urn.empty() ? std::string_view{"a mesh"} : urn},
            {"megabytes", static_cast<core::i64>(table.refusedBytes / (1024u * 1024u))},
            {"most", static_cast<core::i64>(kMaxMorphTableBytes / (1024u * 1024u))},
        };
        core::log(core::LogLevel::Warn, ENG_TR("render.warn.morph_table_too_large"), args);
        return;
    }
    core::EngineError error;
    if (!cache.attachMorphs(device, cmd, entry.mesh, table, &error)) {
        // Targets that move nothing make no table and are no error.
        if (error.key.hash != 0)
            core::logText(core::LogLevel::Warn, error.message);
        return;
    }

    entry.morphNames.reserve(targets.size());
    entry.morphDefaults.reserve(targets.size());
    for (const asset::MorphTarget& target : targets) {
        entry.morphNames.push_back(target.name);
        entry.morphDefaults.push_back(target.defaultWeight);
    }
    const u32 first = table.firstVertex;
    const u32 end = table.firstVertex + table.vertexCount;
    entry.sectionMorphed.assign(submeshes.size(), 0);
    for (core::usize section = 0; section < submeshes.size(); ++section) {
        const asset::Submesh& submesh = submeshes[section];
        const core::usize from = std::min<core::usize>(submesh.firstIndex, indices.size());
        const core::usize to = std::min<core::usize>(from + submesh.indexCount, indices.size());
        for (core::usize index = from; index < to; ++index) {
            if (indices[index] >= first && indices[index] < end) {
                entry.sectionMorphed[section] = 1;
                break;
            }
        }
    }
}

// Everything after "the geometry is on the GPU and the images are uploaded",
// shared by the two feeds. A compiled mesh out of a pack and a glTF parsed on
// the way in differ in how they arrive and in nothing after that -- and one
// copy of this is what keeps them agreeing.
void fillEntry(MeshLibrary::Entry& entry, const core::AABB& bounds, std::span<const asset::Submesh> submeshes,
               std::span<const asset::MaterialDef> materials, std::span<const rhi::TextureHandle> images,
               std::string_view urn = {})
{
    entry.bounds = bounds;
    entry.sectionCount = static_cast<u32>(submeshes.size());
    entry.sectionMaterial.reserve(submeshes.size());
    for (const asset::Submesh& submesh : submeshes)
        entry.sectionMaterial.push_back(submesh.material);

    bool warnedSecondUvSet = false;
    const auto textureOf = [&](const asset::TextureRef& reference) -> rhi::TextureHandle {
        if (!reference.present() || reference.image >= images.size())
            return {};
        // The importer records the UV set the file declared, and this vertex
        // layout carries one. A material sampling TEXCOORD_1 would silently
        // read TEXCOORD_0, so it is dropped instead -- untextured is a visible
        // wrong, silently-wrong-texture is not.
        //
        // **And it says so** (S6.6). Dropping was already right; doing it in
        // silence was the half that was left. Somebody imports a model with a
        // lightmap, one surface comes up untextured, and nothing anywhere tells
        // them the file asked for a second UV set -- which is the same shape as
        // a refusal that answers "has no member named" about a thing you can
        // see. Once per mesh rather than per material: a file with a second UV
        // set usually has it on several, and eight identical lines is a log
        // people stop reading.
        if (reference.uvSet != 0) {
            if (!warnedSecondUvSet) {
                warnedSecondUvSet = true;
                const core::I18nArg args[] = {
                    {"mesh", urn.empty() ? std::string_view{"a mesh"} : urn},
                    {"set", static_cast<core::i64>(reference.uvSet)},
                };
                core::log(core::LogLevel::Warn, ENG_TR("render.warn.second_uv_set"), args);
            }
            return {};
        }
        return images[reference.image];
    };

    entry.materials.reserve(materials.size());
    for (const asset::MaterialDef& source : materials) {
        RenderMaterial material;
        material.uniforms.baseColor[0] = source.baseColorFactor.r;
        material.uniforms.baseColor[1] = source.baseColorFactor.g;
        material.uniforms.baseColor[2] = source.baseColorFactor.b;
        material.uniforms.baseColor[3] = source.baseColorAlpha;
        material.uniforms.emissive[0] = source.emissiveFactor.r;
        material.uniforms.emissive[1] = source.emissiveFactor.g;
        material.uniforms.emissive[2] = source.emissiveFactor.b;
        material.uniforms.metallicRoughnessNormalCutoff[0] = source.metallicFactor;
        material.uniforms.metallicRoughnessNormalCutoff[1] = source.roughnessFactor;
        material.uniforms.metallicRoughnessNormalCutoff[2] = source.normalScale;
        material.uniforms.metallicRoughnessNormalCutoff[3] =
            source.alphaMode == asset::AlphaMode::Mask ? source.alphaCutoff : 0.0f;

        material.setMaps(textureOf(source.baseColor), textureOf(source.normal), textureOf(source.metallicRoughness),
                         textureOf(source.emissive));

        entry.materials.push_back(material);
    }
}

} // namespace

void MeshLoader::setContentRoot(std::filesystem::path root)
{
    contentRoot_ = std::move(root);
}

void MeshLoader::destroy(rhi::IDevice& device)
{
    // **Waited for, not abandoned.** A decode job writes into buffers these
    // entries own, and returning while one is running frees the memory the pool
    // is still writing into -- a use-after-free that reproduces on a fast
    // machine and never on a slow one, at shutdown, where a crash reads as "the
    // editor crashed when I closed it" and points at nothing.
    releasePendingTextures();
    releasePendingMeshes();

    for (const rhi::TextureHandle texture : textures_) {
        if (texture.valid())
            device.destroy(texture);
    }
    if (viewBlack_.valid())
        device.destroy(viewBlack_);
    viewBlack_ = {};
    textures_.clear();
    failed_.clear();
}

MeshLoader::~MeshLoader()
{
    // No device here, so no texture can be freed -- `destroy` is what does that,
    // and a caller who forgot it has leaked them whatever this does. What cannot
    // be left is a job still writing into memory this object is about to
    // release, so that much happens unconditionally.
    releasePendingTextures();
    releasePendingMeshes();
}

void MeshLoader::releasePendingMeshes() noexcept
{
    // Waited for, as a texture's decode is: the job writes into what these own.
    for (PendingMesh& pending : pendingMeshes_) {
        if (pending.images.valid())
            jobs::wait(pending.images);
    }
    pendingMeshes_.clear();
}

void MeshLoader::releasePendingTextures() noexcept
{
    for (PendingTexture& pending : pendingTextures_) {
        if (pending.decode.valid())
            jobs::wait(pending.decode);
        if (pending.read.valid())
            platform::cancelIo(pending.read);
    }
    pendingTextures_.clear();
}

// **The deferred half of `syncTextures`** (D118). See the header for the
// measurement: a 1024-square PNG costs 14 to 36 ms to decode, and the
// synchronous path loads every missing map it finds in one frame with no budget
// at all -- so pointing a part at a four-map material froze the frame after the
// write for a tenth of a second.
//
// Three stages, and only the last one is on the frame, because only the frame
// has a command list. The same shape `StreamingHost::pump` and the content
// browser's thumbnails use, for the same reason.
void MeshLoader::noteReduced(core::NameAtom urn, bool reduced)
{
    const auto byId = [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; };
    const auto at = std::lower_bound(reduced_.begin(), reduced_.end(), urn, byId);
    const bool held = at != reduced_.end() && at->id == urn.id;
    if (reduced && !held)
        reduced_.insert(at, urn);
    else if (!reduced && held)
        reduced_.erase(at);
}

bool MeshLoader::isReduced(core::NameAtom urn) const noexcept
{
    return std::binary_search(reduced_.begin(), reduced_.end(), urn,
                              [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
}

core::u32 MeshLoader::pumpTextures(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world,
                                   TextureLibrary& library)
{
    u32 loaded = 0;

    // **Completions land during `pumpIo` and nowhere else** (`async_io.h`), so
    // this pumps rather than assuming somebody else did. `StreamingHost::pump`
    // and the content browser's thumbnails each pump their own for the same
    // reason: the service is a drain, draining it twice costs nothing, and a
    // subsystem that only worked when another one happened to be running is a
    // subsystem that works on the flagship and not on a fresh project.
    platform::pumpIo();

    const auto markFailed = [&](core::NameAtom urn) {
        const auto position = std::lower_bound(failed_.begin(), failed_.end(), urn,
                                               [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
        if (position == failed_.end() || position->id != urn.id)
            failed_.insert(position, urn);
    };

    for (usize index = 0; index < pendingTextures_.size();) {
        PendingTexture& pending = pendingTextures_[index];
        const auto drop = [&] {
            pendingTextures_.erase(pendingTextures_.begin() + static_cast<std::ptrdiff_t>(index));
        };

        if (pending.read.valid()) {
            const platform::IoStatus status = platform::ioStatus(pending.read);
            if (status == platform::IoStatus::Pending) {
                ++index;
                continue;
            }

            pending.work = std::make_unique<TextureWork>();
            const bool got =
                status == platform::IoStatus::Ready && platform::takeIoResult(pending.read, pending.work->bytes);
            // **A request that ended without being TAKEN still holds its
            // slot** -- `takeIoResult` releases one only for `Ready`, and the
            // pool is a fixed 512. A project with missing files would fill it
            // and every later read would be refused, which reads as "textures
            // stopped loading after a while" and has nothing in the log.
            // `cancelIo` is the documented way to let a terminal one go.
            if (!got)
                platform::cancelIo(pending.read);
            pending.read = {};
            if (!got || pending.work->bytes.empty()) {
                // A material without its texture still draws, in its own
                // numbers. A material refused for a missing map would take the
                // surface with it.
                const std::array<core::I18nArg, 1> args{
                    core::I18nArg{"path", std::string(world.atoms().text(pending.urn))}};
                core::log(core::LogLevel::Warn, ENG_TR("render.err.material_texture_missing"), args);
                markFailed(pending.urn);
                drop();
                continue;
            }

            // **One pointer, to memory that does not move.** Another map asked
            // for between now and the job finishing reallocates
            // `pendingTextures_`, so anything the job addresses has to live
            // somewhere the vector is not.
            TextureWork* work = pending.work.get();
            pending.decode = jobs::schedule("texture-decode", jobs::Domain::AssetIo, [work]() noexcept {
                work->ok = !asset::decodeImage(work->bytes, work->image).has_value();
                // The encoded bytes are the biggest allocation in the pipeline
                // and nothing needs them again.
                work->bytes.clear();
                work->bytes.shrink_to_fit();
            });
            if (!pending.decode.valid()) {
                markFailed(pending.urn);
                drop();
                continue;
            }
            ++index;
            continue;
        }

        if (!jobs::finished(pending.decode)) {
            ++index;
            continue;
        }

        if (pending.work == nullptr || !pending.work->ok) {
            // A compiled map that cannot be read is refused as it was when
            // the frame transcoded it: once, and without a word of a file.
            if (pending.work == nullptr || !pending.work->compiled) {
                const std::array<core::I18nArg, 1> args{
                    core::I18nArg{"path", std::string(world.atoms().text(pending.urn))}};
                core::log(core::LogLevel::Warn, ENG_TR("render.err.material_texture_missing"), args);
            }
            markFailed(pending.urn);
            drop();
            continue;
        }

        const TextureWork& made = *pending.work;
        const rhi::TextureHandle handle = made.compiled
                                              ? uploadTranscoded(device, cmd, made.texture, "material")
                                              : uploadImage(device, cmd, made.image, "material", pending.srgb);
        if (!handle.valid()) {
            markFailed(pending.urn);
            drop();
            continue;
        }
        textures_.push_back(handle);
        if (made.compiled) {
            library.set(pending.urn, handle, made.texture.width, made.texture.height);
            noteReduced(pending.urn, made.texture.skippedLevels > 0);
        }
        else
            library.set(pending.urn, handle, made.image.width, made.image.height);
        ++loaded;
        drop();
    }

    return loaded;
}

// Whether this URN is already on its way in. Linear over a list bounded by
// `MaxTexturesInFlight`, which is a handful.
bool MeshLoader::textureInFlight(core::NameAtom urn) const noexcept
{
    for (const PendingTexture& pending : pendingTextures_) {
        if (pending.urn == urn)
            return true;
    }
    return false;
}

core::u32 MeshLoader::syncTextures(rhi::IDevice& device, rhi::ICmdList& cmd, scene::World& world,
                                   TextureLibrary& library)
{
    // Completions first, so a read that finished during the frame is uploaded in
    // the same one and the slot it frees is available to whatever the walk below
    // asks for. `StreamingHost::pump` orders its own pipeline the same way.
    core::u32 loaded = deferredTextures_ ? pumpTextures(device, cmd, world, library) : 0u;

    const auto load = [&](core::NameAtom urn, bool srgb, bool whole = false) {
        if (urn.id == 0)
            return;
        // Named by the world on the frame a sweep takes its list (D610).
        if (sweepRecording_)
            named_.push_back(urn);
        if (library.find(urn).valid()) {
            // **Loaded smaller for a material, and now a sprite's** (D609):
            // what is drawn at its own size has it whole. Let go and loaded
            // again -- once, since what comes back is not reduced.
            if (!whole || !isReduced(urn))
                return;
            if (const rhi::TextureHandle held = library.take(urn); held.valid() && held != viewBlack_) {
                std::erase(textures_, held);
                device.destroy(held);
            }
            noteReduced(urn, false);
        }
        // **A `view://` name is drawn, not read** (ADR 0107): the view host
        // puts the texture here while something draws into it. Until then it
        // is black, never a file lookup and never the missing-map warning.
        if (world.atoms().text(urn).starts_with("view://")) {
            if (!viewBlack_.valid()) {
                viewBlack_ = device.createTexture({
                    .format = rhi::TextureFormat::Rgba8Unorm,
                    .usage = rhi::TextureUsage::Sampled,
                    .width = 1,
                    .height = 1,
                    .debugName = "view-black",
                });
                const std::array<std::byte, 4> black{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xFF}};
                if (viewBlack_.valid())
                    cmd.uploadTexture(viewBlack_, black, 0);
            }
            if (viewBlack_.valid())
                library.set(urn, viewBlack_, 1, 1);
            return;
        }
        // **The engine's own terrain textures are drawn, not read** (ADR
        // 0113): from noise, once, and uploaded like a loose image.
        //
        // **Off the frame, where a file would be** (D544): a sheet is some tens
        // of milliseconds of noise, and a terrain's eight layers in the frame
        // that first asked were 400 ms of it. Past `MaxTexturesInFlight`, which
        // bounds files open and images held: these are neither, and a layer's
        // four maps are one sheet.
        if (asset::isEngineTexture(world.atoms().text(urn))) {
            if (deferredTextures_) {
                if (textureInFlight(urn))
                    return;
                PendingTexture pending;
                pending.urn = urn;
                pending.srgb = srgb;
                pending.work = std::make_unique<TextureWork>();
                TextureWork* work = pending.work.get();
                work->name = std::string(world.atoms().text(urn));
                pending.decode = jobs::schedule("texture-draw", jobs::Domain::AssetIo, [work]() noexcept {
                    std::optional<asset::Image> drawn = asset::engineTexture(work->name);
                    work->ok = drawn.has_value();
                    if (work->ok)
                        work->image = std::move(*drawn);
                });
                if (pending.decode.valid()) {
                    pendingTextures_.push_back(std::move(pending));
                    return;
                }
                // No job to be had: drawn here, as it is with nothing deferred.
            }
            const std::optional<asset::Image> drawn = asset::engineTexture(world.atoms().text(urn));
            if (drawn.has_value()) {
                const rhi::TextureHandle handle = uploadImage(device, cmd, *drawn, "engine-terrain", srgb);
                if (handle.valid()) {
                    textures_.push_back(handle);
                    library.set(urn, handle, drawn->width, drawn->height);
                    ++loaded;
                }
            }
            return;
        }
        if (std::binary_search(failed_.begin(), failed_.end(), urn,
                               [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; })) {
            return;
        }

        const std::string text(world.atoms().text(urn));
        // Remembered as failed before anything else can go wrong, so a map that
        // is not there costs one attempt rather than one per frame.
        const auto markFailed = [&]() {
            const auto position = std::lower_bound(failed_.begin(), failed_.end(), urn,
                                                   [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
            failed_.insert(position, urn);
        };

        const asset::ResolvedContent resolved = mounts_ != nullptr ? mounts_->resolve(text) : asset::ResolvedContent{};

        // **The compiled form first, and it needs no IO at all** (E9 step 14).
        //
        // A texture the compiler has seen is BC7 with a mip chain, and it was
        // already being produced -- `importOne` writes one `.ktx2` per image
        // into the project's object store, and this function then read the raw
        // PNG beside it anyway. So every editor session was paying a 14-to-36 ms
        // decode to upload four times the GPU memory of a file it already had.
        //
        // **This is the branch that makes "BC7 and mips reach editor content"
        // true**, which the plan promised and nothing had delivered. It also
        // needs no deferral: `resolved.bytes` is already resident -- that is the
        // documented lifetime of a mount -- so there is no read to move off the
        // frame, and a transcode is a fraction of a decode.
        if (resolved.source == asset::ResolvedContent::Source::Pack && resolved.kind == asset::AssetKind::Texture) {
            // **Off the frame, where a file's decode is** (D564). "A transcode
            // is a fraction of a decode", the paragraph above says, and it is
            // -- and it is still ten milliseconds and more of the frame a map
            // is first drawn in, twenty-five on a phone. The blob is copied
            // for the job: a mount's bytes are a mount's to take away.
            if (deferredTextures_) {
                if (textureInFlight(urn) || pendingTextures_.size() >= MaxTexturesInFlight)
                    return;
                PendingTexture pending;
                pending.urn = urn;
                pending.srgb = srgb;
                pending.whole = whole;
                pending.work = std::make_unique<TextureWork>();
                TextureWork* work = pending.work.get();
                work->compiled = true;
                work->bytes.assign(resolved.bytes.begin(), resolved.bytes.end());
                pending.decode = jobs::schedule(
                    "texture-transcode", jobs::Domain::AssetIo, [work, options = transcodeFor(whole)]() noexcept {
                        work->ok = !asset::transcodeTexture(work->bytes, options, work->texture).has_value();
                        work->bytes.clear();
                        work->bytes.shrink_to_fit();
                    });
                if (pending.decode.valid()) {
                    pendingTextures_.push_back(std::move(pending));
                    return;
                }
                // No job to be had: transcoded here, as with nothing deferred.
            }
            asset::TextureAsset texture;
            if (asset::transcodeTexture(resolved.bytes, transcodeFor(whole), texture).has_value()) {
                markFailed();
                return;
            }
            const rhi::TextureHandle handle = uploadTranscoded(device, cmd, texture, "material");
            if (!handle.valid()) {
                markFailed();
                return;
            }
            textures_.push_back(handle);
            library.set(urn, handle, texture.width, texture.height);
            noteReduced(urn, texture.skippedLevels > 0);
            ++loaded;
            return;
        }

        // **The source file, for a map that has no compiled form.** Unlike a
        // mesh, this is kept rather than deleted, and the difference is the
        // pathology each one had: a loose `.gltf` was a 191 ms parse on the
        // frame thread that also reached out to read its own companion files,
        // and a loose PNG is one read the async pipeline above already handles.
        // A map the compiler has not seen -- one written by a script, one in a
        // format it declines -- still draws.
        const std::filesystem::path path =
            resolved.source == asset::ResolvedContent::Source::Loose ? resolved.path : resolve(contentRoot_, text);

        if (deferredTextures_) {
            // Queued rather than read, and only up to a bound: a world whose
            // materials name four hundred maps must not open four hundred files
            // and hold four hundred decoded images at once.
            if (textureInFlight(urn) || pendingTextures_.size() >= MaxTexturesInFlight)
                return;
            PendingTexture pending;
            pending.urn = urn;
            pending.srgb = srgb;
            // `Normal`, above a thumbnail and below a chunk the camera is about
            // to reach: this is a surface somebody is looking at right now.
            pending.read = platform::readFileAsync(path, platform::IoPriority::Normal);
            if (pending.read.valid()) {
                pendingTextures_.push_back(std::move(pending));
                return;
            }
            // **No IO service, or its pool is full: fall through and read it
            // here.** Deferring is a way of doing this work, not a permission to
            // skip it -- a build where `initIo` failed must still show its
            // textures, and a frame that costs a decode is better than a
            // material that is white forever. The synchronous path below is the
            // one this mode is an optimisation OF.
        }

        std::vector<std::byte> bytes;
        asset::Image image;
        if (!platform::readFile(path, bytes) || asset::decodeImage(bytes, image).has_value()) {
            // A material without its texture still draws, in its own numbers. A
            // material refused for a missing map would take the surface with it.
            const std::array<core::I18nArg, 1> args{core::I18nArg{"path", path.string()}};
            core::log(core::LogLevel::Warn, ENG_TR("render.err.material_texture_missing"), args);
            markFailed();
            return;
        }

        const rhi::TextureHandle handle = uploadImage(device, cmd, image, "material", srgb);
        if (!handle.valid()) {
            markFailed();
            return;
        }
        textures_.push_back(handle);
        library.set(urn, handle, image.width, image.height);
        ++loaded;
    };

    // **The maps of every material a part wears** (ADR 0090), each material
    // resolved once however many parts wear it. A map is interned here because
    // this is where it is keyed: the extraction finds it by the same atom.
    // Colour maps are sRGB and data maps are linear -- `ColorMap` and
    // `EmissiveMap` against `NormalMap` and `MetallicRoughnessMap`, the rule
    // `assetc`'s `kMaterialMaps` states for the compiler.
    std::vector<std::pair<core::NameAtom, u32>> visited;
    world.parts().forEach([&](core::InstanceId, const scene::PartComponent& part) {
        if (!part.material.valid())
            return;
        const std::pair<core::NameAtom, u32> key{part.material, part.materialClone};
        if (std::find(visited.begin(), visited.end(), key) != visited.end())
            return;
        visited.push_back(key);
        const asset::ResolvedMaterial material = world.resolveMaterial(part.material, part.materialClone);
        const auto map = [&](const std::string& urn, bool srgb) {
            if (!urn.empty())
                load(world.atoms().intern(urn), srgb);
        };
        map(material.properties.colorMap, true);
        map(material.properties.normalMap, false);
        map(material.properties.metallicRoughnessMap, false);
        map(material.properties.emissiveMap, true);
        // A surface shader's textures (ADR 0091): colours unless the material
        // says the texture is data.
        for (const asset::ShaderParameter& parameter : material.properties.shaderParameters) {
            if (parameter.isTexture())
                map(parameter.texture, !parameter.linear);
        }
    });
    // A terrain's layers (ADR 0113): the maps its arrays are built from.
    world.terrains().forEach([&](core::InstanceId, const scene::TerrainComponent& terrain) {
        for (const std::string& layer : terrain.layers) {
            if (layer.empty())
                continue;
            const asset::ResolvedMaterial material = world.resolveMaterial(world.atoms().intern(layer), 0);
            const auto map = [&](const std::string& urn, bool srgb) {
                if (!urn.empty())
                    load(world.atoms().intern(urn), srgb);
            };
            map(material.properties.colorMap, true);
            map(material.properties.normalMap, false);
            map(material.properties.metallicRoughnessMap, false);
            // The height, packed into the surface array's R (TA13).
            map(material.properties.heightMap, false);
        }
    });
    // A foliage mesh's material (ADR 0116), when it wears one of its own.
    world.foliageMeshes().forEach([&](core::InstanceId, const scene::FoliageMeshComponent& mesh) {
        if (!mesh.material.valid())
            return;
        const std::pair<core::NameAtom, u32> key{mesh.material, 0u};
        if (std::find(visited.begin(), visited.end(), key) != visited.end())
            return;
        visited.push_back(key);
        const asset::ResolvedMaterial material = world.resolveMaterial(mesh.material, 0);
        const auto map = [&](const std::string& urn, bool srgb) {
            if (!urn.empty())
                load(world.atoms().intern(urn), srgb);
        };
        map(material.properties.colorMap, true);
        map(material.properties.normalMap, false);
        map(material.properties.metallicRoughnessMap, false);
        map(material.properties.emissiveMap, true);
    });
    // A sky's sun and moon (ADR 0096): colours. Its six faces are not here --
    // they are resampled on the CPU by `SkyLoader`, which reads them itself.
    world.skies().forEach([&](core::InstanceId, const scene::SkyComponent& sky) {
        load(sky.sunTexture, true, true);
        load(sky.moonTexture, true, true);
    });
    // Decal images (F2): colours, like base colours.
    world.decals().forEach([&](core::InstanceId, const scene::DecalComponent& decal) { load(decal.texture, true); });
    // A particle's picture (ADR 0160), the same.
    world.particleEmitters().forEach(
        [&](core::InstanceId, const scene::ParticleEmitterComponent& emitter) { load(emitter.texture, true); });
    // The 2D layer's pictures: a sprite's image and a tilemap's tileset.
    // **Whole, whatever the texture quality** (D609): a sprite, a tileset and
    // a block's face are drawn at their own size, texel for pixel.
    world.parts2d().forEach(
        [&](core::InstanceId, const scene::Part2DComponent& part) { load(part.image, true, true); });
    world.tilemaps2d().forEach(
        [&](core::InstanceId, const scene::Tilemap2DComponent& tilemap) { load(tilemap.tileset, true, true); });
    // A block world's images (V1), through the same door: compiled when the
    // compiler has seen them, a loose file when it has not.
    world.voxels().forEach([&](core::InstanceId, const scene::VoxelComponent& voxels) {
        for (const scene::VoxelBlockType& type : voxels.types) {
            load(type.texture, true, true);
            load(type.sideTexture, true, true);
            load(type.bottomTexture, true, true);
        }
    });
    // Pictures wanted before anything shows them (ADR 0131): as colour, which
    // is what a picture a script names is -- a material's maps come with it.
    for (const core::NameAtom image : warmTextures_)
        load(image, true);
    sweepWalkedTextures_ = sweepRecording_;
    noteLateLoads();
    std::erase_if(warmTextures_, [&](core::NameAtom image) {
        return library.find(image).valid() ||
               std::binary_search(failed_.begin(), failed_.end(), image,
                                  [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
    });

    return loaded;
}

void MeshLoader::syncPrimitives(rhi::IDevice& device, rhi::ICmdList& cmd, scene::World& world, MeshCache& cache,
                                MeshLibrary& library)
{
    if (primitivesUploaded_)
        return;
    primitivesUploaded_ = true;

    for (core::i32 shape = 0; shape < static_cast<core::i32>(asset::PrimitiveShape::Count); ++shape) {
        const char* name = primitiveContent(shape);
        if (name == nullptr)
            continue;

        const asset::Mesh mesh = asset::makePrimitive(static_cast<asset::PrimitiveShape>(shape));
        core::EngineError uploadError;
        const MeshHandle handle = cache.create(device, cmd, mesh, MeshUsage::Static, &uploadError);
        if (!handle.valid()) {
            // A failure here leaves every `Part` on the debug wire path, which
            // is exactly what that path is for -- so it is a warning and not a
            // reason to refuse to draw a frame.
            core::logText(core::LogLevel::Warn, uploadError.message);
            continue;
        }

        MeshLibrary::Entry entry;
        entry.mesh = handle;
        entry.bounds = mesh.bounds;
        entry.sectionCount = 1;
        entry.sectionMaterial.push_back(0);
        // No materials: a `Part`'s look is its own properties, and `extract`
        // builds the material from them. An entry with an empty material list
        // is already handled -- the mesh loop falls back to `RenderMaterial{}`
        // for a section whose material the importer did not produce.
        library.set(world.atoms().intern(name), entry);
    }
}

u32 MeshLoader::sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world, core::InstanceId root,
                     MeshCache& cache, MeshLibrary& library, SkeletonLibrary* skeletons,
                     std::vector<core::NameAtom>* completed)
{
    if (!root.valid())
        return 0;

    u32 loaded = 0;

    core::u32 meshesThisCall = 0;
    meshesWaiting_ = 0;
    // One mesh content, loaded once whatever names it.
    const auto load = [&](const core::NameAtom content) {
        if (content.id != 0 && sweepRecording_)
            named_.push_back(content);
        if (content.id == 0 || library.find(content) != nullptr)
            return;
        if (std::binary_search(failed_.begin(), failed_.end(), content,
                               [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; }))
            return;

        const std::string urn(world.atoms().text(content));

        // Remembered as failed before anything else can go wrong, so every
        // early return below costs one attempt rather than one per frame.
        const auto markFailed = [&]() {
            const auto position = std::lower_bound(failed_.begin(), failed_.end(), content,
                                                   [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
            failed_.insert(position, content);
        };

        // **A grid the engine carries** (ADR 0091): built the first time a part
        // names one, never before -- uploading them with the primitives would
        // put three meshes into every world's command stream, and into every
        // capture golden of a scene that has no water.
        constexpr std::string_view GridPrefix = "engine://mesh/grid-";
        if (urn.starts_with(GridPrefix)) {
            core::u32 segments = 0;
            const std::string_view digits = std::string_view(urn).substr(GridPrefix.size());
            (void)std::from_chars(digits.data(), digits.data() + digits.size(), segments);
            if (std::find(asset::BuiltInGridSegments.begin(), asset::BuiltInGridSegments.end(), segments) ==
                asset::BuiltInGridSegments.end()) {
                markFailed();
                return;
            }
            const asset::Mesh mesh = asset::makeGrid(segments);
            core::EngineError uploadError;
            const MeshHandle handle = cache.create(device, cmd, mesh, MeshUsage::Static, &uploadError);
            if (!handle.valid()) {
                core::logText(core::LogLevel::Warn, uploadError.message);
                markFailed();
                return;
            }
            MeshLibrary::Entry entry;
            entry.mesh = handle;
            entry.bounds = mesh.bounds;
            entry.sectionCount = 1;
            entry.sectionMaterial.push_back(0);
            library.set(content, entry);
            ++loaded;
            return;
        }

        // **One feed** (E9 step 14). There were two: a mounted pack answering
        // with a compiled mesh, and a content directory answering with the
        // source file parsed on the way in. ADR 0010 kept the second as the
        // dev-mode path; the cut-over deleted it, and a project that has no
        // compiled form is compiled when it opens rather than parsed when it
        // draws. What reaches here is meshopt streams and transcodable
        // textures, and nothing else.
        const asset::ResolvedContent resolved = mounts_ != nullptr ? mounts_->resolve(urn) : asset::ResolvedContent{};

        // **One mesh per call while deferred** (D125). A parse is the largest
        // synchronous thing left in a frame -- measured at 191 ms for a
        // 60,000-vertex glTF with 677 joints, plus 21 ms to read it -- and
        // `sync` loaded EVERY missing mesh it found in one frame with no budget
        // at all, so dropping a folder of five models in meant one frame of
        // roughly a second.
        //
        // A budget of one rather than a millisecond count, because a parse
        // cannot be split: any budget needs a floor of one whole mesh, and one
        // whole mesh is already more than a frame. What this buys is that N
        // meshes cost N frames instead of one frame N times as long, which is
        // the difference between a tool that hitches and a tool that stops.
        //
        // **The budget outlived the parse it was sized for**, and that is
        // deliberate rather than forgotten. It was one-per-call because
        // `importGltf` cost 191 ms and could not be split; a compiled mesh is
        // `decodeMesh` over meshopt streams, which is a different order of
        // magnitude. It stays because the cost that remains is the UPLOAD --
        // vertex and index buffers, plus a transcode per texture slot -- and a
        // folder of five models dropped in at once would still put all of it in
        // one frame. Raising it is a measurement, not a deletion, and there is
        // no measurement here yet.
        // **A compiled mesh's images are made ready off the frame** (D571).
        // First asked for, the mesh is decoded -- which is small -- and
        // parked while a job transcodes what it carries; asked for again with
        // the job done, it is uploaded under the budget above, images and
        // all. Only with meshes deferred: a headless run and a capture load
        // in the frame that asks, as they did.
        std::unique_ptr<MeshWork> ready;
        if (deferredMeshes_ && resolved.source == asset::ResolvedContent::Source::Pack &&
            resolved.kind == asset::AssetKind::Mesh) {
            const auto parked = std::find_if(pendingMeshes_.begin(), pendingMeshes_.end(),
                                             [&](const PendingMesh& pending) { return pending.content == content; });
            if (parked == pendingMeshes_.end()) {
                ++meshesWaiting_;
                if (pendingMeshes_.size() >= MaxMeshesInFlight)
                    return;
                PendingMesh pending;
                pending.content = content;
                pending.work = std::make_unique<MeshWork>();
                MeshWork* work = pending.work.get();
                if (auto error = asset::decodeMesh(resolved.bytes, work->compiled); error.has_value()) {
                    core::logText(core::LogLevel::Warn, error->message);
                    markFailed();
                    return;
                }
                work->options = transcodeFor(false);
                work->blobs.resize(work->compiled.images.size());
                work->textures.resize(work->compiled.images.size());
                work->ok.assign(work->compiled.images.size(), core::u8{0});
                for (usize index = 0; index < work->compiled.images.size(); ++index) {
                    const std::span<const std::byte> blob = mounts_->blob(work->compiled.images[index].hash);
                    work->blobs[index].assign(blob.begin(), blob.end());
                }
                if (!work->blobs.empty()) {
                    pending.images = jobs::schedule("mesh-images", jobs::Domain::AssetIo, [work]() noexcept {
                        for (usize index = 0; index < work->blobs.size(); ++index) {
                            work->ok[index] = !work->blobs[index].empty() &&
                                                      !asset::transcodeTexture(work->blobs[index], work->options,
                                                                               work->textures[index])
                                                           .has_value()
                                                  ? core::u8{1}
                                                  : core::u8{0};
                            work->blobs[index].clear();
                            work->blobs[index].shrink_to_fit();
                        }
                    });
                    // No job to be had: made ready here, as with nothing deferred.
                    if (!pending.images.valid()) {
                        for (usize index = 0; index < work->blobs.size(); ++index) {
                            work->ok[index] = !work->blobs[index].empty() &&
                                                      !asset::transcodeTexture(work->blobs[index], work->options,
                                                                               work->textures[index])
                                                           .has_value()
                                                  ? core::u8{1}
                                                  : core::u8{0};
                        }
                    }
                }
                pendingMeshes_.push_back(std::move(pending));
                return;
            }
            if ((parked->images.valid() && !jobs::finished(parked->images)) || meshesThisCall > 0) {
                ++meshesWaiting_;
                return;
            }
            ready = std::move(parked->work);
            pendingMeshes_.erase(parked);
        }

        if (deferredMeshes_) {
            if (meshesThisCall > 0) {
                ++meshesWaiting_;
                return;
            }
            ++meshesThisCall;
        }

        MeshLibrary::Entry entry;
        core::EngineError uploadError;
        core::u32 triangles = 0;

        if (resolved.source == asset::ResolvedContent::Source::Pack && resolved.kind == asset::AssetKind::Mesh) {
            asset::CompiledMesh compiled;
            if (ready != nullptr) {
                compiled = std::move(ready->compiled);
            }
            else if (auto error = asset::decodeMesh(resolved.bytes, compiled); error.has_value()) {
                core::logText(core::LogLevel::Warn, error->message);
                markFailed();
                return;
            }

            // THE WHOLE CHAIN, flattened into one index buffer with one section
            // list, and a range per level. One upload and one bind: choosing a
            // level is choosing a range of indices, never a different resource,
            // which is what keeps the selector free to change its mind every
            // frame without touching the GPU.
            //
            // A mesh with no chain -- one level -- lands here as one range and
            // draws byte-identically to how it did before this existed.
            asset::Mesh geometry;
            geometry.vertices = std::move(compiled.vertices);
            geometry.bounds = compiled.bounds;

            std::vector<MeshLodRange> lods;
            lods.reserve(compiled.lods.size());
            for (const asset::MeshLod& lod : compiled.lods) {
                const auto indexBase = static_cast<core::u32>(geometry.indices.size());
                const auto sectionBase = static_cast<core::u32>(geometry.submeshes.size());
                geometry.indices.insert(geometry.indices.end(), lod.indices.begin(), lod.indices.end());
                for (asset::Submesh submesh : lod.submeshes) {
                    // Rebased into the combined buffer. The submesh order is
                    // identical at every level (`asset/mesh_format.h`), so a
                    // draw that named section N of one level means section N of
                    // any other -- which is what makes the swap invisible
                    // upstream.
                    submesh.firstIndex += indexBase;
                    geometry.submeshes.push_back(submesh);
                }
                lods.push_back(MeshLodRange{
                    .firstSection = sectionBase,
                    .sectionCount = static_cast<core::u32>(lod.submeshes.size()),
                    .error = lod.error,
                });
            }

            // LEVEL ZERO's triangles, not the whole chain's: this number is
            // what a stats panel calls "the mesh", and counting every level
            // would report a mesh roughly twice the size of the one on screen.
            triangles = static_cast<core::u32>(compiled.lods[0].indices.size() / 3);

            // Its bounds as far as its morph targets reach (ADR 0196), before
            // the cache copies them.
            growBoundsForMorphs(geometry, compiled.morphs);

            const bool skinned = !compiled.joints.empty() && !compiled.skin.empty();
            // **A skinned mesh takes its whole chain too** (H4). It took level
            // zero only, on the reasoning that a coarser level drops vertices
            // and would need its own skin stream -- but a level is a new INDEX
            // list over the same vertices, so the one skin stream, a vertex at
            // a time, serves every level. Five hundred characters across a
            // field drew every triangle of each.
            const MeshHandle handle =
                skinned ? cache.createSkinned(device, cmd, geometry, compiled.skin, &uploadError, lods)
                        : cache.create(device, cmd, geometry, MeshUsage::Static, &uploadError, lods);
            if (!handle.valid()) {
                core::logText(core::LogLevel::Warn, uploadError.message);
                markFailed();
                return;
            }
            entry.mesh = handle;

            std::vector<rhi::TextureHandle> images;
            images.reserve(compiled.images.size());
            for (usize slotIndex = 0; slotIndex < compiled.images.size(); ++slotIndex) {
                const asset::TextureSlot& slot = compiled.images[slotIndex];
                asset::TextureAsset texture;
                bool transcoded = false;
                if (ready != nullptr) {
                    // Made ready by the job (D571).
                    transcoded = slotIndex < ready->ok.size() && ready->ok[slotIndex] != 0;
                    if (transcoded)
                        texture = std::move(ready->textures[slotIndex]);
                }
                else {
                    const std::span<const std::byte> blob = mounts_->blob(slot.hash);
                    transcoded =
                        !blob.empty() && !asset::transcodeTexture(blob, transcodeFor(false), texture).has_value();
                }
                if (!transcoded) {
                    // A material without its texture still draws, tinted. A
                    // mesh refused for a missing texture would take the whole
                    // world with it.
                    images.push_back({});
                    continue;
                }
                const rhi::TextureHandle uploaded = uploadTranscoded(device, cmd, texture, "material");
                if (uploaded.valid())
                    textures_.push_back(uploaded);
                images.push_back(uploaded);
            }

            // LEVEL ZERO's submeshes, not the flattened list. `sectionCount`
            // is how many draws an instance emits, and every level has the same
            // submeshes in the same order -- so the flattened list would emit a
            // draw per section PER LEVEL and render the mesh several times over.
            fillEntry(entry, geometry.bounds, compiled.lods[0].submeshes, compiled.materials, images, urn);
            giveMorphs(device, cmd, cache, entry, compiled.morphs, geometry.vertices.size(), compiled.lods[0].submeshes,
                       compiled.lods[0].indices, urn);
            entry.positions.reserve(geometry.vertices.size());
            for (const asset::Vertex& vertex : geometry.vertices)
                entry.positions.push_back(vertex.position);
            library.set(content, entry);
            if (completed != nullptr)
                completed->push_back(content);

            // A rig, or morph targets, or both: what the animation plays on
            // this mesh (ADR 0196 for the second).
            if (skeletons != nullptr && (!compiled.joints.empty() || !compiled.morphs.empty())) {
                SkeletonLibrary::Entry rig;
                rig.joints = std::move(compiled.joints);
                rig.clips = std::move(compiled.clips);
                rig.morphNames.reserve(compiled.morphs.size());
                rig.morphDefaults.reserve(compiled.morphs.size());
                for (const asset::MorphTarget& target : compiled.morphs) {
                    rig.morphNames.push_back(target.name);
                    rig.morphDefaults.push_back(target.defaultWeight);
                }
                skeletons->set(content, std::move(rig));
            }
        }
        else {
            // **A loose `.gltf` no longer feeds the runtime** (E9 step 14, the
            // cut-over -- the human's constraint in their own words).
            //
            // What used to be here read the source file and parsed it on the
            // frame: 3 MB of JSON and 14 MB of buffer for one horse, on every
            // launch, with no LOD chain, no meshlets, and its textures uploaded
            // as raw RGBA8 with no mips. It was ADR 0010's dev-mode path and it
            // was the reason `sync` could only afford ONE mesh per call (D125),
            // because a parse cannot be split and a 191 ms parse is three
            // frames.
            //
            // Everything arrives compiled now. `engine.cpp` compiles a project
            // that has no compiled form when it opens it -- in EVERY host mode,
            // not only the editor, precisely because this fallback is gone --
            // so a project cloned from git still works with no command, which
            // was the whole of assumption 3.
            //
            // **Named rather than silent.** A part that quietly turns invisible
            // is harder to diagnose than one that says why, and the two reasons
            // this can fire are both actionable: the file is not something the
            // compiler accepted, or the URN names a piece that no longer
            // exists. `markFailed` means one line per mesh rather than one per
            // frame.
            const std::array<core::I18nArg, 1> args{core::I18nArg{"path", urn}};
            core::log(core::LogLevel::Warn, ENG_TR("render.err.mesh_not_compiled"), args);
            markFailed();
            return;
        }

        ++loaded;

        const std::array<core::I18nArg, 2> args{
            core::I18nArg{"path", urn},
            core::I18nArg{"triangles", static_cast<core::i64>(triangles)},
        };
        core::log(core::LogLevel::Info, ENG_TR("render.info.mesh_loaded"), args);
    };
    world.meshParts().forEach(
        [&](core::InstanceId, const scene::MeshPartComponent& meshPart) { load(meshPart.meshContent); });
    // A foliage layer's meshes (ADR 0116), from the same feed and the same
    // budget: a field of grass is one mesh, loaded once.
    world.foliageMeshes().forEach([&](core::InstanceId, const scene::FoliageMeshComponent& mesh) { load(mesh.mesh); });
    // And what is wanted before it is shown (ADR 0125, 0131), after what is
    // on screen; each name leaves the list once it has arrived.
    for (const core::NameAtom content : warmMeshes_)
        load(content);
    sweepWalkedMeshes_ = sweepRecording_;
    noteLateLoads();
    std::erase_if(warmMeshes_, [&](core::NameAtom content) {
        return library.find(content) != nullptr ||
               std::binary_search(failed_.begin(), failed_.end(), content,
                                  [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
    });

    return loaded;
}

void MeshLoader::warmMeshes(std::span<const core::NameAtom> meshes)
{
    for (const core::NameAtom content : meshes) {
        if (std::find(warmMeshes_.begin(), warmMeshes_.end(), content) == warmMeshes_.end())
            warmMeshes_.push_back(content);
    }
    notePreloaded(meshes);
}

void MeshLoader::warmTextures(std::span<const core::NameAtom> images)
{
    for (const core::NameAtom content : images) {
        if (std::find(warmTextures_.begin(), warmTextures_.end(), content) == warmTextures_.end())
            warmTextures_.push_back(content);
    }
    notePreloaded(images);
}

std::optional<bool> MeshLoader::warmed(core::NameAtom content, const MeshLibrary& meshes,
                                       const TextureLibrary& textures) const
{
    if (meshes.find(content) != nullptr || textures.find(content).valid())
        return true;
    if (std::binary_search(failed_.begin(), failed_.end(), content,
                           [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; }))
        return false;
    return std::nullopt;
}

bool MeshLoader::uploadModel(rhi::IDevice& device, rhi::ICmdList& cmd, const asset::Model& model, core::NameAtom urn,
                             MeshCache& cache, MeshLibrary& library)
{
    if (!urn.valid() || model.mesh.vertices.empty())
        return false;

    core::EngineError uploadError;
    // A file with a skin gets the second stream and one without gets exactly
    // what an ordinary load uploads -- the same branch `sync` takes, so a
    // preview and a viewport draw the same geometry.
    //
    // A model with morph targets is uploaded with its bounds grown to where
    // they reach (ADR 0196): a copy of the mesh, for the few that have any.
    asset::Mesh grown;
    if (!model.morphs.empty()) {
        grown = model.mesh;
        growBoundsForMorphs(grown, model.morphs);
    }
    const asset::Mesh& mesh = model.morphs.empty() ? model.mesh : grown;
    const MeshHandle handle = model.skinned() ? cache.createSkinned(device, cmd, mesh, model.skin, &uploadError)
                                              : cache.create(device, cmd, mesh, MeshUsage::Static, &uploadError);
    if (!handle.valid()) {
        core::logText(core::LogLevel::Warn, uploadError.message);
        return false;
    }

    std::vector<rhi::TextureHandle> images;
    images.reserve(model.images.size());
    for (const asset::Image& image : model.images) {
        const rhi::TextureHandle texture = uploadImage(device, cmd, image, "preview");
        if (texture.valid())
            textures_.push_back(texture);
        images.push_back(texture);
    }

    MeshLibrary::Entry entry;
    entry.mesh = handle;
    fillEntry(entry, mesh.bounds, model.mesh.submeshes, model.materials, images, std::string_view{});
    giveMorphs(device, cmd, cache, entry, model.morphs, model.mesh.vertices.size(), model.mesh.submeshes,
               model.mesh.indices, std::string_view{});
    entry.positions.reserve(model.mesh.vertices.size());
    for (const asset::Vertex& vertex : model.mesh.vertices)
        entry.positions.push_back(vertex.position);
    library.set(urn, entry);
    return true;
}

void MeshLoader::notePreloaded(std::span<const core::NameAtom> urns)
{
    for (const core::NameAtom urn : urns) {
        const auto held = std::find_if(preloaded_.begin(), preloaded_.end(),
                                       [urn](const Preloaded& other) { return other.urn.id == urn.id; });
        if (held != preloaded_.end())
            held->scene = scene_;
        else
            preloaded_.push_back(Preloaded{urn, scene_});
    }
}

void MeshLoader::keep(std::span<const core::NameAtom> urns)
{
    const auto byId = [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; };
    for (const core::NameAtom urn : urns) {
        const auto at = std::lower_bound(kept_.begin(), kept_.end(), urn, byId);
        if (at == kept_.end() || at->id != urn.id)
            kept_.insert(at, urn);
    }
}

void MeshLoader::release(std::span<const core::NameAtom> urns)
{
    for (const core::NameAtom urn : urns)
        std::erase_if(kept_, [urn](core::NameAtom held) { return held.id == urn.id; });
}

bool MeshLoader::kept(core::NameAtom urn) const noexcept
{
    return std::binary_search(kept_.begin(), kept_.end(), urn,
                              [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
}

void MeshLoader::noteLateLoads()
{
    const auto holds = [](const std::vector<core::NameAtom>& list, core::NameAtom urn) {
        return std::find_if(list.begin(), list.end(), [urn](core::NameAtom other) { return other.id == urn.id; }) !=
               list.end();
    };
    const auto each = [this](const auto& fn) {
        for (const PendingTexture& pending : pendingTextures_)
            fn(pending.urn);
        for (const PendingMesh& pending : pendingMeshes_)
            fn(pending.content);
    };
    if (!lateWatching_) {
        // Everything in flight now began behind the loading screen.
        lateExempt_.clear();
        each([this](core::NameAtom urn) { lateExempt_.push_back(urn); });
        return;
    }
    // What was exempt and has arrived is exempt no longer: let go later and
    // named again during play, it is a late load like any other.
    std::erase_if(lateExempt_, [&](core::NameAtom urn) {
        bool flying = false;
        each([&](core::NameAtom pending) { flying = flying || pending.id == urn.id; });
        return !flying;
    });
    each([&](core::NameAtom urn) {
        if (holds(lateExempt_, urn) || holds(lateSaid_, urn) || kept(urn))
            return;
        // Asked for in this scene: a game loading ahead on purpose.
        if (std::find_if(preloaded_.begin(), preloaded_.end(), [this, urn](const Preloaded& held) {
                return held.urn.id == urn.id && held.scene == scene_;
            }) != preloaded_.end())
            return;
        lateSaid_.push_back(urn);
        late_.push_back(urn);
    });
}

std::vector<core::NameAtom> MeshLoader::takeLateLoads()
{
    std::vector<core::NameAtom> taken;
    taken.swap(late_);
    return taken;
}

void MeshLoader::leaveScene() noexcept
{
    ++scene_;
    sweepIn_ = SweepFrames;
    sweepRecording_ = false;
    sweepWalkedTextures_ = false;
    sweepWalkedMeshes_ = false;
    named_.clear();
}

core::u32 MeshLoader::sweep(rhi::IDevice& device, const scene::World& world, TextureLibrary& textures,
                            MeshLibrary& meshes, MeshCache& cache)
{
    if (sweepIn_ == 0)
        return 0;
    if (sweepIn_ > 1) {
        // The frame before the last: the walks of the next one are the list.
        if (--sweepIn_ == 1) {
            sweepRecording_ = true;
            named_.clear();
        }
        return 0;
    }
    // A frame whose walks did not both run -- no workspace yet -- is not a
    // list of anything: the next one.
    if (!sweepRecording_ || !sweepWalkedTextures_ || !sweepWalkedMeshes_) {
        sweepRecording_ = true;
        sweepWalkedTextures_ = false;
        sweepWalkedMeshes_ = false;
        named_.clear();
        return 0;
    }
    sweepIn_ = 0;
    sweepRecording_ = false;

    const auto byId = [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; };
    // And what was preloaded in the scene just left, or since: the scene now
    // open's. Older than that is nobody's any more.
    std::erase_if(preloaded_, [this](const Preloaded& held) { return held.scene + 1 < scene_; });
    for (const Preloaded& held : preloaded_)
        named_.push_back(held.urn);
    named_.insert(named_.end(), kept_.begin(), kept_.end());
    for (const PendingTexture& pending : pendingTextures_)
        named_.push_back(pending.urn);
    for (const PendingMesh& pending : pendingMeshes_)
        named_.push_back(pending.content);
    std::sort(named_.begin(), named_.end(), byId);
    named_.erase(
        std::unique(named_.begin(), named_.end(), [](core::NameAtom a, core::NameAtom b) { return a.id == b.id; }),
        named_.end());

    // In atom order, both libraries: the same list on every run.
    std::vector<core::NameAtom> gone;
    const auto consider = [&](core::NameAtom urn) {
        if (!std::binary_search(named_.begin(), named_.end(), urn, byId))
            gone.push_back(urn);
    };
    textures.forEachName(consider);
    meshes.forEach([&](core::NameAtom urn, const MeshLibrary::Entry&) { consider(urn); });
    named_.clear();
    named_.shrink_to_fit();
    if (gone.empty())
        return 0;
    // **Only the project's content.** The five solids, a terrain's and a
    // water's meshes, a view's picture: each has its own name and its own
    // keeper, and none is a file a scene named.
    std::erase_if(gone, [&world](core::NameAtom urn) { return !world.atoms().text(urn).starts_with("asset://"); });
    return gone.empty() ? 0u : forget(device, gone, textures, meshes, cache);
}

core::u32 MeshLoader::forget(rhi::IDevice& device, std::span<const core::NameAtom> urns, TextureLibrary& textures,
                             MeshLibrary& meshes, MeshCache& cache)
{
    core::u32 dropped = 0;
    for (const core::NameAtom urn : urns) {
        if (!urn.valid())
            continue;

        // **The blacklist is cleared too, and leaving it was a defect that
        // defeated the whole point of this function.**
        //
        // `failed_` remembers a URN that would not load so it costs one attempt
        // rather than one per frame for ever -- and it was cleared nowhere but
        // `destroy`. So an asset that failed to import once was blacklisted for
        // the life of the loader: somebody fixed the file, the watcher called
        // this to drop the stale entry, `sync` found the URN still blacklisted
        // and skipped it, and the fix did not appear until the editor was
        // restarted. Which is precisely what ADR 0062's "a changed asset
        // reloads itself" promises does not happen.
        //
        // Forgetting a URN means forgetting everything about it, the refusal
        // included: the next `sync` is entitled to try again, because the reason
        // it failed may be the thing that just changed.
        if (const auto at = std::lower_bound(failed_.begin(), failed_.end(), urn,
                                             [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
            at != failed_.end() && at->id == urn.id) {
            failed_.erase(at);
        }

        // The texture, whose handle this owns the lifetime of once it is out of
        // the library. Destroyed here rather than left: a dev session that
        // reloads one 4K map fifty times would otherwise hold fifty of them.
        if (const rhi::TextureHandle held = textures.take(urn); held.valid() && held != viewBlack_) {
            std::erase(textures_, held);
            device.destroy(held);
            ++dropped;
        }
        noteReduced(urn, false);

        // One on its way in is another file's by the time it lands (D571).
        if (const auto parked = std::find_if(pendingMeshes_.begin(), pendingMeshes_.end(),
                                             [&](const PendingMesh& pending) { return pending.content == urn; });
            parked != pendingMeshes_.end()) {
            if (parked->images.valid())
                jobs::wait(parked->images);
            pendingMeshes_.erase(parked);
        }

        // The mesh, and its GPU buffers with it. `MeshLibrary::remove` drops the
        // entry; the cache is what holds the vertex and index buffers, so a
        // remove without this leaks the expensive half.
        if (const MeshLibrary::Entry* entry = meshes.find(urn); entry != nullptr) {
            const MeshHandle handle = entry->mesh;
            // **And the images the model carried** (D610): they are in no
            // library, only in the entry's materials, and went with nothing
            // -- a model forgotten left every one of its textures on the card.
            for (const RenderMaterial& material : entry->materials) {
                for (const rhi::TextureHandle image :
                     {material.baseColor, material.normal, material.metallicRoughness, material.emissive}) {
                    if (!image.valid())
                        continue;
                    // Only what this loader made: a material may wear a
                    // library's texture, which is the library's to let go.
                    if (std::erase(textures_, image) != 0)
                        device.destroy(image);
                }
            }
            meshes.remove(urn);
            if (handle.valid())
                cache.release(device, handle);
            ++dropped;
        }

        // **Anything in flight for this URN is left alone**, deliberately. A
        // deferred read that lands after the forget writes the OLD bytes into a
        // fresh entry, which the next forget would drop again -- so the worst
        // case is one stale frame, and the alternative is cancelling work from
        // the middle of a pipeline whose whole design is that nothing holds a
        // pointer into it (D131's neighbourhood).
    }
    return dropped;
}

} // namespace engine::render
