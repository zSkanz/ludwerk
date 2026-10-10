// Turning a `MeshPart`'s content URN into geometry on the GPU (roadmap M4).
//
// This is the runtime import path ADR 0010 keeps forever as the dev-mode one.
// It lives in `render` rather than in `app` because the result is GPU geometry
// and the renderer owns that; `asset` produces CPU-side data and knows nothing
// about a device, which is architecture.md §2's split.
//
// **Loading is synchronous, and that is a deliberate narrowing of this
// milestone's own brief.** Decision 1 said loads would go on the `jobs` pool
// with the result applied at the FrameStart safe point. What is here reads the
// file on the calling thread -- at that safe point, so nothing mutates
// mid-frame, but without the pool. The reason is that the pool buys latency
// hiding for a streaming system, M4 has no streaming, and a background loader
// with one caller and no eviction policy is the speculative half of the design.
// M7 is the milestone that has something to stream and is where this grows a
// queue.
#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/asset/content.h"
#include "engine/asset/mesh_format.h"
#include "engine/asset/texture.h"
#include "engine/core/id.h"
#include "engine/jobs/jobs.h"
#include "engine/platform/async_io.h"
#include "engine/render/animation.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/rhi/device.h"

namespace engine::scene {
class World;
}

namespace engine::render {

class MeshLoader
{
public:
    // `contentRoot` is what an `asset://` URN resolves against -- the project's
    // own content directory, so a URN means the same thing to the engine as it
    // does in the file the developer wrote.
    void setContentRoot(std::filesystem::path root);

    // Where a URN resolves when a pack is mounted. Optional and not owned: a
    // host with no packs -- every example before M7, the capture harness --
    // leaves it null and gets exactly the loose-file path it had. Two feeds,
    // one library.
    void setContentMounts(const asset::ContentMounts* mounts) noexcept { mounts_ = mounts; }

    // What the device can sample. The default allows BC7, which is the whole
    // point of shipping transcodable textures; a headless or capture run sets
    // `forceUncompressed` so a golden does not depend on a GPU's format
    // support.
    void setTranscodeOptions(const asset::TranscodeOptions& options) noexcept { transcode_ = options; }

    // **How many of its largest levels a texture of the world is loaded
    // without** (`GraphicsService.TextureQuality`, D609): nought, one or two.
    // For what is seen at a distance and filtered -- a material's maps, a
    // model's own images, a decal, a particle -- and for those as the
    // compiler made them, with their levels; a loose picture is taken whole.
    //
    // **Not for what is drawn at its own size**: a sprite, a tileset, a block
    // world's faces, the sun and the moon. A pixel-art tileset at half its
    // size is a different, blurred tileset. One of those that names a texture
    // already loaded smaller for a material has it loaded again, whole.
    //
    // It is what is loaded FROM NOW ON: a texture already on the card keeps
    // the size it came in at until it is loaded again.
    void setTextureSkip(core::u32 levels) noexcept { textureSkip_ = levels; }
    [[nodiscard]] core::u32 textureSkip() const noexcept { return textureSkip_; }
    // How many textures are on the card smaller than their file.
    [[nodiscard]] core::usize texturesReduced() const noexcept { return reduced_.size(); }

    // Loads every `MeshPart` content the world names and the library does not
    // yet hold. Call at the FrameStart safe point with a command list open and
    // no render pass: uploads are copies, and a copy cannot run inside a pass.
    //
    // Returns how many meshes it loaded this call, which is zero on the frames
    // that matter -- a non-zero count every frame means something is asking for
    // a file that keeps failing, and the log will say which.
    // `skeletons` is where a skinned file's joints and clips go. A pointer
    // because a caller that draws but does not simulate -- a screenshot tool, a
    // capture harness -- has nowhere to put them, and because the library
    // belongs to the HOST rather than to the renderer: animation advances on the
    // SimClock and has to run in a headless replay.
    core::u32 sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world, core::InstanceId root,
                   MeshCache& cache, MeshLibrary& library, SkeletonLibrary* skeletons = nullptr,
                   std::vector<core::NameAtom>* completed = nullptr);

    // `completed`, when given, is APPENDED with the content name of every mesh
    // this call put into the library.
    //
    // **A count cannot answer "which".** The frame loop hands a mesh's vertex
    // positions to the physics mirror, and it did that by walking the WHOLE
    // library whenever the count was non-zero -- a full copy of every loaded
    // mesh's positions, and a free of the previous one, every frame anything
    // landed. Synchronous loading hid it inside one or two frames; spreading N
    // completions over N frames turns it into N(N+1)/2 copies of everything.
    //
    // Names rather than entries, because the library owns the entry and the
    // caller is about to look it up anyway.

    // Loads every texture the world's `Material` instances name and the library
    // does not yet hold. Same safe point, same rules, same return as `sync`.
    //
    // Over the materials the parts WEAR (ADR 0090), each resolved once however
    // many parts wear it: a material is an asset now, and one nothing wears is
    // one nothing draws. The material editor's preview wears the material it
    // shows, which is how an asset being authored gets its maps.
    //
    // **The URNs name SOURCE images** -- the `.png` the artist shipped -- and
    // this decodes and uploads them. That is the dev-mode path ADR 0010 keeps
    // forever; a shipped game's textures arrive compiled and named by hash.
    core::u32 syncTextures(rhi::IDevice& device, rhi::ICmdList& cmd, scene::World& world, TextureLibrary& library);

    // **Forgets what a URN loaded, so the next sync reads it again** (S6.4).
    //
    // This is the whole of asset hot-reload, and it is that small for one
    // reason: the loaders already load everything MISSING. `syncTextures` reads
    // every map it cannot find and `syncPrimitives` does the same for meshes,
    // so making an entry missing is the reload -- there is no second path to
    // write and no state machine to get wrong.
    //
    // **It works because loose content is not cached.** `ContentMounts::resolve`
    // answers a loose URN with a PATH and reads nothing (D039 took the read out
    // of it), so the next load opens the file as it stands on disk. A URN served
    // from a pack or from the editor's object store resolves to bytes the mount
    // keeps, and forgetting it reloads the same bytes -- which is correct: those
    // are compiled artifacts, and changing one means recompiling it.
    //
    // Returns how many entries it actually dropped, so a caller can say "3
    // reloaded" rather than "a message arrived".
    core::u32 forget(rhi::IDevice& device, std::span<const core::NameAtom> urns, TextureLibrary& textures,
                     MeshLibrary& meshes, MeshCache& cache);

    // **An already-parsed model onto the GPU, under a name of the caller's
    // choosing** (S5.16).
    //
    // The ordinary feed reads and parses inside `sync`; this is for a caller
    // that has done both already and off the frame. The content browser's
    // previews are the one: `ThumbnailCache` parses a glTF on the job pool
    // precisely so the frame does not (D118), and the render half is then handed
    // a `Model` with nowhere to put it.
    //
    // Uploads the images and the geometry, fills the library entry, and returns
    // whether anything is there to draw. `urn` is what the entry is keyed by, so
    // a `MeshPart` naming it will find this.
    [[nodiscard]] bool uploadModel(rhi::IDevice& device, rhi::ICmdList& cmd, const asset::Model& model,
                                   core::NameAtom urn, MeshCache& cache, MeshLibrary& library);

    // **Whether a texture may be read and decoded off the frame.**
    //
    // Measured on ordinary 1024-square PNGs out of a texture pack: 14 to 36 ms
    // to decode, each. `syncTextures` loads every missing map it finds in one
    // frame and has no budget at all, so pointing a part at a material with
    // four maps costs a hundred milliseconds on the frame after the write. That
    // is a freeze on exactly the frame somebody changed something, which is how
    // an editor that measures fine comes to feel like it reloads the world
    // every time you touch it. A 4K source is sixteen times worse.
    //
    // Deferred, the read goes to `platform::readFileAsync`, the decode goes to
    // the job pool, and only the upload happens on the frame -- the same three
    // stages the streaming host and the browser's thumbnails already use, and
    // for the same measurement.
    //
    // **Off by default, and that default is what keeps every golden
    // byte-identical.** A capture records the frame it was told to record, and a
    // texture that arrives two frames later is a different picture; the same is
    // true of a screenshot gate and of any headless run whose output is
    // compared. Those all want the loader finished before the frame is. An
    // interactive shell wants the opposite, and says so.
    void setDeferredTextures(bool deferred) noexcept { deferredTextures_ = deferred; }

    // **Whether a mesh may take more than one frame to arrive** (D125).
    //
    // Measured on the model E9 opened for: 21 ms to read and **191 ms to
    // parse** a 2937 KiB glTF of 60,688 vertices and 677 joints. `sync` loaded
    // every missing mesh it found in one frame with no budget at all, so
    // dropping a folder of five models into a project meant one frame of about
    // a second -- and a `MeshPart` created by a script did the same thing mid-
    // play.
    //
    // Deferred, one mesh lands per call. **A budget of one and not a
    // millisecond count**, because a parse cannot be split: any budget needs a
    // floor of one whole mesh and one whole mesh already exceeds a frame. What
    // it buys is that N meshes cost N frames rather than one frame N times as
    // long.
    //
    // **What a part collides as while it waits.** A `MeshPart` whose points
    // have not arrived collides as a Box of its `Size` -- deliberate, tested
    // (`physics_sync_tests.cpp`), and the reason it never falls through
    // anything. The honest caveat is that the box is a superset of the hull only
    // when `MeshSize` was authored correctly and the mesh is centred on its
    // origin, neither of which is enforced; a rare pop is possible where a hull
    // would have let something through.
    //
    // **Off by default**, and that default is what keeps every golden
    // byte-identical: a capture records the frame it was told to record, and
    // geometry that arrives three frames later is a different picture.
    void setDeferredMeshes(bool deferred) noexcept { deferredMeshes_ = deferred; }

    // How many textures are being read or decoded right now. Zero in the
    // synchronous mode, always.
    [[nodiscard]] core::usize texturesInFlight() const noexcept { return pendingTextures_.size(); }

    // How many meshes the last `sync` found missing and left for a later call,
    // the deferred budget being spent. Zero in the synchronous mode, always.
    // With `texturesInFlight` it says when a world has arrived (D507).
    [[nodiscard]] core::usize meshesWaiting() const noexcept { return meshesWaiting_; }

    // How many maps may be on their way in at once. Bounded because a world
    // whose materials name four hundred textures must not open four hundred
    // files and hold four hundred decoded images at the same time -- and because
    // the pictures somebody is looking at now are worth more than the ones two
    // rooms away, which a queue of four hundred cannot express.
    static constexpr core::usize MaxTexturesInFlight = 4;
    // And compiled meshes whose images are being made ready (D571): each is a
    // mesh decoded and held, with its images' bytes beside it.
    static constexpr core::usize MaxMeshesInFlight = 4;

    // Builds and uploads the five `Enum.PartShape` solids and registers them in
    // `library` under their reserved URNs (`primitiveContent`). Idempotent: the
    // second call does nothing, which is what makes it safe to put at the top of
    // a per-frame `sync`.
    //
    // It is here rather than in the renderer because this is the module that
    // already turns geometry into GPU buffers -- and because the whole point of
    // M4's constraint is that generated geometry takes the SAME route an
    // imported mesh does. Interning the names needs a mutable atom table, which
    // is why this takes the world rather than a const reference to one.
    void syncPrimitives(rhi::IDevice& device, rhi::ICmdList& cmd, scene::World& world, MeshCache& cache,
                        MeshLibrary& library);

    // Releases every GPU resource this loader created. The cache's meshes are
    // the cache's to free; the textures are this one's.
    void destroy(rhi::IDevice& device);

    // **Try the missing ones again** (the owner: textures imported after a
    // script asked for them showed only after the engine was reopened). A
    // texture not found is remembered so it is not read and warned about every
    // frame -- and was remembered for good. The content changing, or play
    // starting, is when one of them may be there now.
    void retryMissing() noexcept { failed_.clear(); }

    // **Meshes and pictures wanted before anything shows them**: what a scene
    // being prepared names (ADR 0125) and what `PreloadAsync` asks for (ADR
    // 0131). Loaded by `sync` and `syncTextures` from the same feeds as the
    // world's, after it; each name leaves the list once it has arrived.
    void warmMeshes(std::span<const core::NameAtom> meshes);
    void warmTextures(std::span<const core::NameAtom> images);

    // **A scene was left: what it alone held is let go** (D610). Every mesh
    // and texture a game had ever shown stayed on the graphics card until the
    // engine closed -- a game of six maps held all six by the time its player
    // reached the last, and a phone with two gigabytes ended it for the
    // memory.
    //
    // `leaveScene` says a scene was left. `sweep`, called each frame after
    // the walks of `syncTextures` and `sync`, waits `SweepFrames` for the new
    // scene to make what it makes, takes that frame's walk as the list of
    // what the world names, and forgets every mesh and texture of the
    // project's content (`asset://`) that is not on it. Kept besides:
    //
    //   - what `ContentProvider:PreloadAsync` or a scene being prepared asked
    //     for in the scene that was just left or since -- a loading screen
    //     preloads the next scene's content before it changes to it, and that
    //     is the next scene's, not the last one's;
    //   - what `ContentProvider:Keep` named, until `Release` names it: the
    //     game's own word that something is held whatever scenes come and go
    //     (`keep`, `release`);
    //   - anything on its way in.
    //
    // What a game shows again later is loaded again, as it was the first
    // time: a preload is how a game says it wants something held.
    // How many it let go, once, on the frame it does; nought otherwise.
    void leaveScene() noexcept;
    void keep(std::span<const core::NameAtom> urns);
    void release(std::span<const core::NameAtom> urns);
    [[nodiscard]] bool kept(core::NameAtom urn) const noexcept;

    // **What began to load while the game was being played** (D610's aid to
    // an author): with `watching` on -- the loading screen is down and the
    // scene has settled -- a mesh or a picture the WORLD named that was not
    // loaded is noted, once a name. Not what a preload or `Keep` asked for,
    // which is a game loading ahead on purpose, and not what was already on
    // its way when the watching began. `takeLateLoads` hands the notes over
    // and forgets them; the caller words the warning. Costs a pass over the
    // loads in flight, which are a handful.
    void watchLateLoads(bool watching) noexcept { lateWatching_ = watching; }
    [[nodiscard]] std::vector<core::NameAtom> takeLateLoads();
    core::u32 sweep(rhi::IDevice& device, const scene::World& world, TextureLibrary& textures, MeshLibrary& meshes,
                    MeshCache& cache);
    static constexpr core::u32 SweepFrames = 30;
    // Where one name stands: nothing while it is on its way, true once it is
    // in its library, false once it was given up on.
    [[nodiscard]] std::optional<bool> warmed(core::NameAtom content, const MeshLibrary& meshes,
                                             const TextureLibrary& textures) const;

    // Waits for any decode still running, because one is writing into memory
    // this object owns. See the definition: abandoning it is a use-after-free at
    // shutdown, which is the hardest kind to attribute.
    ~MeshLoader();

    MeshLoader() = default;
    MeshLoader(const MeshLoader&) = delete;
    MeshLoader& operator=(const MeshLoader&) = delete;

private:
    // The three stages of a deferred load, run once a frame from `syncTextures`.
    core::u32 pumpTextures(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world,
                           TextureLibrary& library);
    [[nodiscard]] bool textureInFlight(core::NameAtom urn) const noexcept;
    void releasePendingTextures() noexcept;

    std::filesystem::path contentRoot_;
    const asset::ContentMounts* mounts_ = nullptr;
    asset::TranscodeOptions transcode_;
    // The texture quality's levels to leave out, and the textures loaded
    // without them, in id order (D609).
    core::u32 textureSkip_ = 0;
    std::vector<core::NameAtom> reduced_;
    [[nodiscard]] asset::TranscodeOptions transcodeFor(bool whole) const noexcept
    {
        asset::TranscodeOptions options = transcode_;
        options.skipLevels = whole ? 0u : textureSkip_;
        return options;
    }
    void noteReduced(core::NameAtom urn, bool reduced);
    [[nodiscard]] bool isReduced(core::NameAtom urn) const noexcept;
    bool primitivesUploaded_ = false;
    bool deferredTextures_ = false;
    bool deferredMeshes_ = false;

    // Everything a decode job touches, in ONE heap allocation.
    //
    // **Not a field of `PendingTexture`, and that is the whole point.** The
    // vector below grows whenever another map is asked for, which can happen on
    // the same frame a job is running -- so a job holding the address of
    // anything inside an element would be writing into freed memory after the
    // reallocation. Three fields on the heap together is one indirection and no
    // way to get it wrong; three pointers into a vector is three chances to.
    struct TextureWork
    {
        std::vector<std::byte> bytes;
        // For a texture that is drawn and not read (D544): which one.
        std::string name;
        asset::Image image;
        // For a compiled map (D564): `bytes` is its blob, and this is what
        // the job made of it for this device.
        bool compiled = false;
        asset::TextureAsset texture;
        bool ok = false;
    };

    // One texture on its way in.
    struct PendingTexture
    {
        core::NameAtom urn;
        // Whether the image is a COLOUR -- a base colour, an emission, a block
        // face -- and so stored sRGB, as the compiler stores those.
        bool srgb = false;
        // Asked for by something drawn at its own size: taken whole, whatever
        // the texture quality (D609).
        bool whole = false;
        platform::IoRequest read;
        jobs::JobHandle decode;
        std::unique_ptr<TextureWork> work;
    };
    std::vector<PendingTexture> pendingTextures_;

    // **A compiled mesh on its way in** (D571). The images a mesh carries were
    // transcoded in the frame the mesh arrived in, one after another -- 23 ms
    // of a phone's frame for one character. With meshes deferred a mesh is
    // decoded when it is first asked for and parked here while a job makes its
    // images ready; the frame that finds the job done uploads it, one mesh a
    // frame as before. On the heap for the reason `TextureWork` is.
    struct MeshWork
    {
        asset::CompiledMesh compiled;
        // Each image's blob, copied: a mount's bytes are a mount's to take
        // away, and an object store's are read by the frame's thread alone.
        std::vector<std::vector<std::byte>> blobs;
        std::vector<asset::TextureAsset> textures;
        // One an image: whether it transcoded. A byte, not a bit: the job
        // writes them from another thread than reads them.
        std::vector<core::u8> ok;
        asset::TranscodeOptions options;
    };
    struct PendingMesh
    {
        core::NameAtom content;
        // Not valid for a mesh with no images: there is nothing to wait for.
        jobs::JobHandle images;
        std::unique_ptr<MeshWork> work;
    };
    std::vector<PendingMesh> pendingMeshes_;
    void releasePendingMeshes() noexcept;
    // Content URNs that failed to load, so a broken file costs one attempt and
    // one message rather than one of each per frame forever. Sorted, for the
    // same reason `MeshLibrary` is: R10 forbids an unordered container's order
    // reaching observable output, and a log is observable.
    std::vector<core::NameAtom> failed_;
    core::usize meshesWaiting_ = 0;
    std::vector<core::NameAtom> warmMeshes_;
    std::vector<core::NameAtom> warmTextures_;

    // The sweep (D610). `scene_` counts the scenes left; a preload is kept
    // through the scene after the one it was asked in.
    struct Preloaded
    {
        core::NameAtom urn;
        core::u32 scene = 0;
    };
    std::vector<Preloaded> preloaded_;
    // What a game asked to be held (`keep`), sorted by atom.
    std::vector<core::NameAtom> kept_;
    // The late loads: what is exempt because it was in flight before the
    // watching began, what has been said already, and what is to be said.
    bool lateWatching_ = false;
    std::vector<core::NameAtom> lateExempt_;
    std::vector<core::NameAtom> lateSaid_;
    std::vector<core::NameAtom> late_;
    void noteLateLoads();
    void notePreloaded(std::span<const core::NameAtom> urns);
    core::u32 scene_ = 0;
    // Frames left before the walk that is taken as the list, and that list:
    // filled by the walks of the one frame the countdown reaches nought on.
    core::u32 sweepIn_ = 0;
    bool sweepRecording_ = false;
    bool sweepWalkedTextures_ = false;
    bool sweepWalkedMeshes_ = false;
    std::vector<core::NameAtom> named_;
    std::vector<rhi::TextureHandle> textures_;
    // **What a `view://` name shows until something draws into it** (ADR
    // 0107): one black pixel, shared by every such name and never destroyed
    // by `forget` -- a screen that is off. The view host replaces the entry
    // with the real texture while one exists.
    rhi::TextureHandle viewBlack_{};
};

} // namespace engine::render
