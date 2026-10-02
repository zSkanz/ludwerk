#pragma once

// The block world on the GPU (V1, `VoxelService`).
//
// Each chunk that holds blocks is meshed by `asset::meshVoxelChunk` -- greedy
// faces, hidden faces culled, ambient occlusion per corner -- uploaded through
// `MeshCache`, and filed in `MeshLibrary` under `voxelChunkUrn`, where `extract`
// finds it and emits a draw the renderer shades with the block shader.
//
// **A chunk is re-meshed when anything it READ changed**: its own blocks, and
// the one-block shell of the 26 chunks around it, because a face at the chunk's
// edge is hidden by a neighbour's block and a corner's occlusion is darkened by
// one. So an edit re-meshes the chunk it is in and, at an edge, the neighbours
// whose faces it touches -- and nothing else.
//
// **A chunk that changes does not cost the frame that draws it** (D449, the
// voxel-world ledger's B1). The frame finds WHICH chunks may have changed --
// one walk of the chunks' own digests against the ones it last saw -- and
// hands each to a worker with the chunks it reads, shared and never copied.
// The worker decides whether the mesh would differ and, if so, makes it. The
// frame goes on drawing the old mesh until every chunk asked for together is
// ready, and then swaps them in one frame, into slices of buffers the mesh
// cache already has. What a frame pays is the walk and the copies.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/asset/voxel.h"
#include "engine/asset/voxel_mesher.h"
#include "engine/jobs/jobs.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/rhi/device.h"
#include "engine/scene/world.h"

namespace engine::render {

// `voxel://<x>,<y>,<z>`, in chunk keys.
[[nodiscard]] std::string voxelChunkUrn(asset::VoxelChunkKey key);
// The same chunk's translucent faces -- glass, water -- which are a second mesh
// because they are a second draw.
[[nodiscard]] std::string voxelTranslucentUrn(asset::VoxelChunkKey key);
// And its cutout faces -- leaves -- which the depth prepass must not draw.
[[nodiscard]] std::string voxelCutoutUrn(asset::VoxelChunkKey key);

class VoxelLoader
{
public:
    VoxelLoader() = default;
    // Waits for what its workers still hold: they read state this owns.
    ~VoxelLoader();
    VoxelLoader(const VoxelLoader&) = delete;
    VoxelLoader& operator=(const VoxelLoader&) = delete;

    // Finds what changed near the viewer and asks for it, swaps in what the
    // workers finished, and releases what is out of range. Budgeted by a
    // COUNT of chunks, never a clock. Answers how many chunks it swapped.
    core::u32 sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world, core::AtomTable& atoms,
                   MeshCache& cache, MeshLibrary& library);

    // Where the viewer is. Chunks are meshed nearest first, and only within
    // `viewDistance` of it; with no focus, everything is.
    void setFocus(core::DVec3 focus) noexcept
    {
        m_focus = focus;
        m_hasFocus = true;
    }
    void setViewDistance(double metres) noexcept { m_viewDistance = metres; }

    // **The next `sync` finishes everything**: every chunk that changed is
    // meshed and swapped in before it returns. What the frame a picture is
    // taken from asks for -- a screenshot is of the world, not of how far the
    // workers had got -- and what no interactive frame ever does.
    void settleNext() noexcept { m_settle = true; }

    void destroy(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library);

    [[nodiscard]] core::usize residentCount() const noexcept { return m_resident.size(); }
    [[nodiscard]] core::u32 lastRebuilds() const noexcept { return m_lastRebuilds; }
    // Chunks asked for and not yet swapped in, and chunks known to need a look
    // that have not been asked for yet.
    [[nodiscard]] core::usize pendingCount() const noexcept;

private:
    struct Resident
    {
        asset::VoxelChunkKey key;
        core::NameAtom urn;
        core::NameAtom translucentUrn;
        core::NameAtom cutoutUrn;
        MeshHandle mesh;
        MeshHandle translucent;
        MeshHandle cutout;
        // What the mesh read: the chunk's own blocks, the one-block shell
        // around it, how each block type looks, and the block size.
        core::u64 exact = 0;
        bool seen = false;
    };

    // One chunk at a worker. Owned here and at a fixed address until its job
    // has finished: the job holds a pointer to it and nothing else.
    struct Build
    {
        asset::VoxelChunkKey key;
        // The chunk and the 26 around it, shared with the world.
        asset::VoxelGrid grid;
        std::shared_ptr<const std::vector<asset::BlockLook>> looks;
        core::u64 looksDigest = 0;
        float blockSize = 1.0f;
        // What the resident mesh read, or zero for a chunk with none: a build
        // that would read the same makes nothing.
        core::u64 residentExact = 0;
        // The worker's answer.
        core::u64 exact = 0;
        bool unchanged = false;
        asset::VoxelMesh meshed;
        jobs::JobHandle job;
    };
    // Chunks asked for in one `sync`, swapped in together when the last of
    // them is ready -- so the two chunks either side of a broken block never
    // show one new face and one old for a frame.
    struct Batch
    {
        std::vector<std::unique_ptr<Build>> builds;
    };

    [[nodiscard]] bool batchFinished(const Batch& batch) const noexcept;
    void waitAll() noexcept;
    // Swaps one finished batch in. Answers how many chunks changed.
    core::u32 apply(rhi::IDevice& device, rhi::ICmdList& cmd, core::AtomTable& atoms, MeshCache& cache,
                    MeshLibrary& library, Batch& batch, const asset::VoxelGrid* grid);
    void release(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library, Resident& resident);
    [[nodiscard]] bool inFlight(asset::VoxelChunkKey key) const noexcept;

    core::DVec3 m_focus;
    bool m_hasFocus = false;
    double m_viewDistance = 384.0;
    core::u32 m_lastRebuilds = 0;
    bool m_settle = false;
    // Sorted by key (R10).
    std::vector<Resident> m_resident;
    // Every chunk's own digest as the last `sync` saw it, in key order: what
    // this one's are compared with to find the chunks that changed.
    std::vector<std::pair<asset::VoxelChunkKey, core::u64>> m_known;
    // Chunks whose mesh may be out of date, not yet handed to a worker.
    // Sorted, each once.
    std::vector<asset::VoxelChunkKey> m_dirty;
    // Oldest first.
    std::vector<Batch> m_batches;
    // What every mesh was last made with: a change to either re-meshes all.
    core::u64 m_looksDigest = 0;
    float m_blockSize = 0.0f;
};

} // namespace engine::render
