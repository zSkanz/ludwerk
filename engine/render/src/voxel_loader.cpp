#include "engine/render/voxel_loader.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/core/log.h"

namespace engine::render {
namespace {

using core::i32;
using core::u32;
using core::u64;
using core::usize;

// How many chunks one `sync` hands to the workers, and so how many are swapped
// in together. A swap is a copy into a slice of a buffer that already exists
// -- some tens of microseconds a chunk -- so a batch arriving costs its frame
// about a millisecond; a world's first sight fills in over a handful of
// frames, nearest first.
constexpr u32 ChunksPerBatch = 32;

// How many batches may be at the workers at once. Two, so the workers are not
// idle while one batch waits for the frame that swaps it in, and no more: a
// chunk asked for is drawn from the blocks it had when it was asked, and a
// long queue is a long way behind.
constexpr usize BatchesInFlight = 2;

// **A batch this small is waited for where it was asked** -- the calling
// thread helps, which is what a wait is here. It is a block broken or placed:
// a chunk and, at an edge, the neighbour whose face it touches, a third of a
// millisecond each -- and the player sees the hole in the frame they made it.
constexpr usize InlineBatch = 4;

// Order-sensitive, which is what a key built from an ordered walk wants.
[[nodiscard]] u64 combine(u64 seed, u64 value) noexcept
{
    u64 z = seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Adds `key` to a sorted list that holds each key once.
void insertSorted(std::vector<asset::VoxelChunkKey>& keys, asset::VoxelChunkKey key)
{
    const auto at = std::lower_bound(keys.begin(), keys.end(), key);
    if (at == keys.end() || !(*at == key))
        keys.insert(at, key);
}

} // namespace

std::string voxelChunkUrn(asset::VoxelChunkKey key)
{
    return "voxel://" + std::to_string(key.x) + "," + std::to_string(key.y) + "," + std::to_string(key.z);
}

std::string voxelTranslucentUrn(asset::VoxelChunkKey key)
{
    return voxelChunkUrn(key) + "#translucent";
}

std::string voxelCutoutUrn(asset::VoxelChunkKey key)
{
    return voxelChunkUrn(key) + "#cutout";
}

VoxelLoader::~VoxelLoader()
{
    waitAll();
}

bool VoxelLoader::batchFinished(const Batch& batch) const noexcept
{
    return std::all_of(batch.builds.begin(), batch.builds.end(),
                       [](const std::unique_ptr<Build>& build) { return jobs::finished(build->job); });
}

void VoxelLoader::waitAll() noexcept
{
    for (const Batch& batch : m_batches) {
        for (const std::unique_ptr<Build>& build : batch.builds)
            jobs::wait(build->job);
    }
}

bool VoxelLoader::inFlight(asset::VoxelChunkKey key) const noexcept
{
    for (const Batch& batch : m_batches) {
        for (const std::unique_ptr<Build>& build : batch.builds) {
            if (build->key == key)
                return true;
        }
    }
    return false;
}

core::usize VoxelLoader::pendingCount() const noexcept
{
    usize count = m_dirty.size();
    for (const Batch& batch : m_batches)
        count += batch.builds.size();
    return count;
}

void VoxelLoader::release(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library, Resident& resident)
{
    if (resident.mesh.valid())
        cache.release(device, resident.mesh);
    if (resident.translucent.valid())
        cache.release(device, resident.translucent);
    if (resident.cutout.valid())
        cache.release(device, resident.cutout);
    library.remove(resident.urn);
    library.remove(resident.translucentUrn);
    library.remove(resident.cutoutUrn);
}

u32 VoxelLoader::apply(rhi::IDevice& device, rhi::ICmdList& cmd, core::AtomTable& atoms, MeshCache& cache,
                       MeshLibrary& library, Batch& batch, const asset::VoxelGrid* grid)
{
    u32 swapped = 0;
    for (const std::unique_ptr<Build>& build : batch.builds) {
        // The worker found the mesh would read what the one drawn read.
        if (build->unchanged)
            continue;
        // Gone while it was being made: nothing to draw it for.
        if (grid == nullptr || grid->findChunk(build->key) == nullptr)
            continue;

        auto at = std::lower_bound(m_resident.begin(), m_resident.end(), build->key,
                                   [](const Resident& entry, asset::VoxelChunkKey probe) { return entry.key < probe; });
        const bool exists = at != m_resident.end() && at->key == build->key;
        const core::NameAtom urn = exists ? at->urn : atoms.intern(voxelChunkUrn(build->key));
        const core::NameAtom translucentUrn =
            exists ? at->translucentUrn : atoms.intern(voxelTranslucentUrn(build->key));
        const core::NameAtom cutoutUrn = exists ? at->cutoutUrn : atoms.intern(voxelCutoutUrn(build->key));

        // One mesh into the library under one name, or its name out of it.
        // **A slice of a buffer the cache already has** (`MeshUsage::Pooled`):
        // making two buffers a mesh was eighteen of the twenty-six
        // milliseconds a batch of thirty-two cost its frame.
        const auto upload = [&](const asset::Mesh& mesh, core::NameAtom name) {
            MeshHandle handle;
            if (!mesh.indices.empty()) {
                core::EngineError uploadError;
                handle = cache.create(device, cmd, mesh, MeshUsage::Pooled, &uploadError);
                if (!handle.valid())
                    core::logText(core::LogLevel::Warn, uploadError.message);
            }
            if (handle.valid()) {
                MeshLibrary::Entry entry;
                entry.mesh = handle;
                entry.bounds = mesh.bounds;
                entry.sectionCount = 1;
                entry.sectionMaterial.assign(1, 0u);
                entry.materials.push_back(RenderMaterial{});
                library.set(name, std::move(entry));
            }
            else {
                library.remove(name);
            }
            return handle;
        };
        // The old meshes go back first, so the new ones can take their place
        // in the page.
        if (exists) {
            if (at->mesh.valid())
                cache.release(device, at->mesh);
            if (at->translucent.valid())
                cache.release(device, at->translucent);
            if (at->cutout.valid())
                cache.release(device, at->cutout);
        }
        const MeshHandle handle = upload(build->meshed.mesh, urn);
        const MeshHandle translucent = upload(build->meshed.translucent, translucentUrn);
        const MeshHandle cutout = upload(build->meshed.cutout, cutoutUrn);

        if (exists) {
            at->mesh = handle;
            at->translucent = translucent;
            at->cutout = cutout;
            at->exact = build->exact;
        }
        else {
            // Seen: it is in the world, and the walk that would say so has
            // already run this frame. One out of range goes at the next.
            m_resident.insert(at, Resident{build->key, urn, translucentUrn, cutoutUrn, handle, translucent, cutout,
                                           build->exact, true});
        }
        swapped += 1;
    }
    return swapped;
}

u32 VoxelLoader::sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world, core::AtomTable& atoms,
                      MeshCache& cache, MeshLibrary& library)
{
    m_lastRebuilds = 0;
    for (Resident& resident : m_resident)
        resident.seen = false;

    const scene::VoxelComponent* voxels = nullptr;
    world.voxels().forEach([&voxels](core::InstanceId, const scene::VoxelComponent& found) {
        if (voxels == nullptr)
            voxels = &found;
    });
    const asset::VoxelGrid* grid = voxels != nullptr ? &voxels->grid : nullptr;

    // What shows through what, by id, and a digest of it that every chunk's
    // key carries: making a block type see-through changes the faces of
    // every chunk that holds one, and nothing about their blocks.
    std::shared_ptr<std::vector<asset::BlockLook>> looks;
    u64 looksDigest = 0x6C6F6F6Bull;
    if (voxels != nullptr) {
        looks = std::make_shared<std::vector<asset::BlockLook>>(voxels->types.size() + 1, asset::BlockLook{});
        for (usize at = 0; at < voxels->types.size(); ++at) {
            (*looks)[at + 1].opacity = static_cast<asset::BlockOpacity>(std::clamp(voxels->types[at].opacity, 0, 2));
            (*looks)[at + 1].fluid = voxels->types[at].fluidReach > 0;
            (*looks)[at + 1].reach = voxels->types[at].fluidReach;
            looksDigest = combine(looksDigest, static_cast<u64>(voxels->types[at].opacity) + 1u);
            looksDigest = combine(looksDigest, static_cast<u64>(voxels->types[at].fluidReach));
        }
    }

    // In range of the viewer, nearest first when asked: every chunk of the
    // world with its distance, in key order.
    struct Near
    {
        asset::VoxelChunkKey key;
        double distance = 0.0;
    };
    std::vector<Near> near;

    if (voxels == nullptr) {
        m_known.clear();
        m_dirty.clear();
    }
    else {
        const float size = voxels->blockSize;
        const double chunkMetres = static_cast<double>(asset::VoxelChunkEdge) * static_cast<double>(size);
        const bool everything = looksDigest != m_looksDigest || size != m_blockSize;
        m_looksDigest = looksDigest;
        m_blockSize = size;

        // **Which chunks changed, from their own digests against the ones the
        // last walk saw** -- one pass over two sorted lists. It asked every
        // chunk for the digests of all 27 around it, every frame: twenty
        // thousand lookups for a world nobody had touched.
        std::vector<asset::VoxelChunkKey> changed;
        std::vector<std::pair<asset::VoxelChunkKey, u64>> seen;
        seen.reserve(grid->chunkCount());
        near.reserve(grid->chunkCount());
        usize known = 0;
        for (const asset::VoxelGrid::Entry& entry : grid->chunks()) {
            const asset::VoxelChunkKey key = entry.first;
            const u64 digest = asset::digestOf(*entry.second);
            // What the last walk saw and this one does not: gone.
            while (known < m_known.size() && m_known[known].first < key)
                changed.push_back(m_known[known++].first);
            if (known < m_known.size() && m_known[known].first == key) {
                if (m_known[known].second != digest)
                    changed.push_back(key);
                ++known;
            }
            else {
                changed.push_back(key);
            }
            seen.emplace_back(key, digest);

            double distance = 0.0;
            if (m_hasFocus) {
                const auto axis = [&](i32 index, double focus) {
                    const double low = static_cast<double>(index) * chunkMetres;
                    return std::max({low - focus, 0.0, focus - (low + chunkMetres)});
                };
                const double dx = axis(key.x, m_focus.x);
                const double dy = axis(key.y, m_focus.y);
                const double dz = axis(key.z, m_focus.z);
                distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance > m_viewDistance)
                    continue; // not seen: released below
            }
            near.push_back(Near{key, distance});
        }
        while (known < m_known.size())
            changed.push_back(m_known[known++].first);
        m_known = std::move(seen);

        // A chunk that changed may have changed what the 26 around it read.
        // Whether it did is the worker's to say.
        for (const asset::VoxelChunkKey key : changed) {
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx)
                        insertSorted(m_dirty, asset::VoxelChunkKey{key.x + dx, key.y + dy, key.z + dz});
                }
            }
        }

        // Dirty is what is in the world and in range; a chunk in range with
        // no mesh yet -- it has just come into view -- is dirty too.
        const auto inRange = [&near](asset::VoxelChunkKey key) {
            const auto at =
                std::lower_bound(near.begin(), near.end(), key,
                                 [](const Near& entry, asset::VoxelChunkKey probe) { return entry.key < probe; });
            return at != near.end() && at->key == key;
        };
        std::erase_if(m_dirty, [&](asset::VoxelChunkKey key) { return !inRange(key); });
        for (const Near& entry : near) {
            const auto at = std::lower_bound(
                m_resident.begin(), m_resident.end(), entry.key,
                [](const Resident& resident, asset::VoxelChunkKey probe) { return resident.key < probe; });
            if (at != m_resident.end() && at->key == entry.key) {
                at->seen = true;
                if (!everything)
                    continue;
            }
            else if (inFlight(entry.key)) {
                continue;
            }
            insertSorted(m_dirty, entry.key);
        }
    }

    const bool settle = m_settle;
    m_settle = false;
    for (;;) {
        // --- asked for ---------------------------------------------------------
        if (voxels != nullptr && !m_dirty.empty() && (settle || m_batches.size() < BatchesInFlight)) {
            // Nearest first, then by key so equal distances cannot trade places.
            std::vector<Near> order;
            order.reserve(m_dirty.size());
            for (const asset::VoxelChunkKey key : m_dirty) {
                // One already at a worker waits for it: what comes back is
                // from the blocks as they were, and it is asked again then.
                if (inFlight(key))
                    continue;
                const auto at =
                    std::lower_bound(near.begin(), near.end(), key,
                                     [](const Near& entry, asset::VoxelChunkKey probe) { return entry.key < probe; });
                order.push_back(Near{key, at != near.end() && at->key == key ? at->distance : 0.0});
            }
            std::sort(order.begin(), order.end(), [](const Near& a, const Near& b) {
                if (a.distance != b.distance)
                    return a.distance < b.distance;
                return a.key < b.key;
            });
            if (order.size() > ChunksPerBatch)
                order.resize(ChunksPerBatch);

            if (!order.empty()) {
                Batch batch;
                batch.builds.reserve(order.size());
                for (const Near& wanted : order) {
                    auto build = std::make_unique<Build>();
                    build->key = wanted.key;
                    build->grid = grid->around(wanted.key);
                    build->looks = looks;
                    build->looksDigest = looksDigest;
                    build->blockSize = voxels->blockSize;
                    const auto at = std::lower_bound(
                        m_resident.begin(), m_resident.end(), wanted.key,
                        [](const Resident& resident, asset::VoxelChunkKey probe) { return resident.key < probe; });
                    if (at != m_resident.end() && at->key == wanted.key)
                        build->residentExact = at->exact;
                    Build* shared = build.get();
                    build->job = jobs::schedule("voxel.mesh", jobs::Domain::Render, [shared]() noexcept {
                        // What the mesh would read. The same as the one drawn
                        // read, and there is nothing to make -- which is what
                        // most of the 26 around a changed chunk answer.
                        shared->exact =
                            combine(combine(asset::shellDigestOf(shared->grid, shared->key), shared->looksDigest),
                                    static_cast<u64>(std::bit_cast<u32>(shared->blockSize)));
                        if (shared->residentExact != 0 && shared->exact == shared->residentExact)
                            shared->unchanged = true;
                        else
                            shared->meshed =
                                asset::meshVoxelChunk(shared->grid, shared->key, *shared->looks, shared->blockSize);
                        // The world's chunks are let go here, not when the
                        // frame gets round to it: a chunk still shared is a
                        // chunk the next write has to copy.
                        shared->grid.clear();
                    });
                    const auto dirty = std::lower_bound(m_dirty.begin(), m_dirty.end(), wanted.key);
                    if (dirty != m_dirty.end() && *dirty == wanted.key)
                        m_dirty.erase(dirty);
                    batch.builds.push_back(std::move(build));
                }
                if (settle || batch.builds.size() <= InlineBatch) {
                    for (const std::unique_ptr<Build>& build : batch.builds)
                        jobs::wait(build->job);
                }
                m_batches.push_back(std::move(batch));
            }
        }

        // --- swapped in ----------------------------------------------------------
        if (settle)
            waitAll();
        for (usize at = 0; at < m_batches.size();) {
            if (!batchFinished(m_batches[at])) {
                ++at;
                continue;
            }
            m_lastRebuilds += apply(device, cmd, atoms, cache, library, m_batches[at], grid);
            m_batches.erase(m_batches.begin() + static_cast<std::ptrdiff_t>(at));
        }

        // A picture's frame goes round until nothing is left to make.
        if (!settle || voxels == nullptr || (m_dirty.empty() && m_batches.empty()))
            break;
    }

    // Chunks no longer in the world, or out of range, give their meshes back.
    for (usize at = m_resident.size(); at > 0; --at) {
        Resident& resident = m_resident[at - 1];
        if (resident.seen)
            continue;
        release(device, cache, library, resident);
        m_resident.erase(m_resident.begin() + static_cast<std::ptrdiff_t>(at - 1));
    }
    return m_lastRebuilds;
}

void VoxelLoader::destroy(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library)
{
    waitAll();
    m_batches.clear();
    m_dirty.clear();
    m_known.clear();
    for (Resident& resident : m_resident)
        release(device, cache, library, resident);
    m_resident.clear();
}

} // namespace engine::render
