#include "engine/render/voxel_loader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include "engine/asset/terrain.h"
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

void VoxelLoader::appendOccluders(const scene::World& world, RenderWorld& out)
{
    constexpr i32 Edge = static_cast<i32>(asset::VoxelChunkEdge);
    constexpr i32 Reach = OccluderReachBlocks;
    const scene::VoxelComponent* voxels = nullptr;
    world.voxels().forEach([&voxels](core::InstanceId, const scene::VoxelComponent& found) {
        if (voxels == nullptr)
            voxels = &found;
    });
    if (voxels == nullptr || voxels->grid.chunkCount() == 0)
        return;
    const asset::VoxelGrid& grid = voxels->grid;
    const double size = static_cast<double>(voxels->blockSize);
    const core::DVec3 camera = out.camera.origin;
    // The window, in whole chunks round the block under the camera.
    const i32 lowX = asset::floorDiv(static_cast<i32>(std::floor(camera.x / size)) - Reach, Edge);
    const i32 lowZ = asset::floorDiv(static_cast<i32>(std::floor(camera.z / size)) - Reach, Edge);
    constexpr i32 Chunks = 2 * Reach / Edge + 1;

    // Made from: where it is, the blocks, and which chunks are not drawn as
    // they are.
    u64 made = 0xA0761D6478BD642Full;
    const auto mix = [&](u64 value) { made = (made ^ value) * 0x100000001B3ull; };
    mix(static_cast<u64>(static_cast<u32>(lowX)));
    mix(static_cast<u64>(static_cast<u32>(lowZ)));
    mix(voxels->revision);
    mix(m_looksDigest);
    mix(std::bit_cast<u32>(voxels->blockSize));
    mix(m_hasFocus ? std::bit_cast<u64>(m_viewDistance) : 0u);
    const auto mixKey = [&](asset::VoxelChunkKey key) {
        mix((static_cast<u64>(static_cast<u32>(key.x)) << 32) ^ (static_cast<u64>(static_cast<u32>(key.y)) << 16) ^
            static_cast<u64>(static_cast<u32>(key.z)));
    };
    for (const asset::VoxelChunkKey key : m_dirty)
        mixKey(key);
    for (const Batch& batch : m_batches) {
        for (const std::unique_ptr<Build>& build : batch.builds)
            mixKey(build->key);
    }

    if (made != m_occluderMade) {
        m_occluderMade = made;
        m_occluderBoxes.clear();
        const double chunkMetres = static_cast<double>(Edge) * size;
        const auto drawnAsItIs = [&](asset::VoxelChunkKey key) {
            if (std::binary_search(m_dirty.begin(), m_dirty.end(), key) || inFlight(key))
                return false;
            if (!m_hasFocus)
                return true;
            // In range, by the measure `sync` keeps chunks by.
            const auto axis = [&](i32 index, double focus) {
                const double low = static_cast<double>(index) * chunkMetres;
                return std::max({low - focus, 0.0, focus - (low + chunkMetres)});
            };
            const auto from = [&](const core::DVec3& viewer) {
                const double dx = axis(key.x, viewer.x);
                const double dy = axis(key.y, viewer.y);
                const double dz = axis(key.z, viewer.z);
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            };
            double distance = from(m_focus);
            for (const core::DVec3& other : m_otherFoci)
                distance = std::min(distance, from(other));
            return distance <= m_viewDistance;
        };
        // Which blocks hide what is behind them: opaque, and not a fluid.
        const auto opaque = [&](asset::BlockId block) {
            if (block == asset::AirBlock)
                return false;
            const u32 type = asset::blockTypeOf(block);
            if (type == 0 || type > voxels->types.size())
                return true;
            const scene::VoxelBlockType& look = voxels->types[type - 1];
            return look.opacity == 0 && look.fluidReach == 0;
        };

        // The window's chunks that are drawn as they are, and how high they
        // reach.
        std::vector<const asset::VoxelGrid::Entry*> inside;
        i32 lowY = std::numeric_limits<i32>::max();
        i32 highY = std::numeric_limits<i32>::min();
        for (const asset::VoxelGrid::Entry& entry : grid.chunks()) {
            const asset::VoxelChunkKey key = entry.first;
            if (key.x < lowX || key.z < lowZ || key.x >= lowX + Chunks || key.z >= lowZ + Chunks ||
                entry.second == nullptr || entry.second->solid == 0 || !drawnAsItIs(key))
                continue;
            inside.push_back(&entry);
            lowY = std::min(lowY, key.y);
            highY = std::max(highY, key.y);
        }
        if (!inside.empty() && highY - lowY < MostOccluderChunksHigh) {
            // Every opaque block of them, a byte each.
            const i32 width = Chunks * Edge;
            const i32 height = (highY - lowY + 1) * Edge;
            std::vector<core::u8> solid(
                static_cast<usize>(width) * static_cast<usize>(width) * static_cast<usize>(height), 0);
            const auto cell = [&](i32 x, i32 y, i32 z) {
                return (static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(z)) *
                           static_cast<usize>(width) +
                       static_cast<usize>(x);
            };
            for (const asset::VoxelGrid::Entry* entry : inside) {
                const i32 baseX = (entry->first.x - lowX) * Edge;
                const i32 baseY = (entry->first.y - lowY) * Edge;
                const i32 baseZ = (entry->first.z - lowZ) * Edge;
                for (u32 y = 0; y < asset::VoxelChunkEdge; ++y) {
                    for (u32 z = 0; z < asset::VoxelChunkEdge; ++z) {
                        for (u32 x = 0; x < asset::VoxelChunkEdge; ++x) {
                            if (opaque(entry->second->blocks[asset::voxelIndex(x, y, z)]))
                                solid[cell(baseX + static_cast<i32>(x), baseY + static_cast<i32>(y),
                                           baseZ + static_cast<i32>(z))] = 1;
                        }
                    }
                }
            }
            // **Greedy boxes**: from each solid block not yet in a box, as far
            // as it goes along x, then along z while every row is solid, then
            // up while every layer is. A wall is one box, and a hill a few.
            const auto free = [&](i32 x, i32 y, i32 z) { return solid[cell(x, y, z)] == 1; };
            for (i32 y = 0; y < height; ++y) {
                for (i32 z = 0; z < width; ++z) {
                    for (i32 x = 0; x < width; ++x) {
                        if (!free(x, y, z))
                            continue;
                        i32 x1 = x + 1;
                        while (x1 < width && free(x1, y, z))
                            ++x1;
                        i32 z1 = z + 1;
                        const auto rowFree = [&](i32 atY, i32 atZ) {
                            for (i32 at = x; at < x1; ++at) {
                                if (!free(at, atY, atZ))
                                    return false;
                            }
                            return true;
                        };
                        while (z1 < width && rowFree(y, z1))
                            ++z1;
                        i32 y1 = y + 1;
                        const auto layerFree = [&](i32 atY) {
                            for (i32 at = z; at < z1; ++at) {
                                if (!rowFree(atY, at))
                                    return false;
                            }
                            return true;
                        };
                        while (y1 < height && layerFree(y1))
                            ++y1;
                        for (i32 by = y; by < y1; ++by) {
                            for (i32 bz = z; bz < z1; ++bz) {
                                for (i32 bx = x; bx < x1; ++bx)
                                    solid[cell(bx, by, bz)] = 2;
                            }
                        }
                        // A box too small to hide much is left to the draws.
                        if ((x1 - x) * (y1 - y) * (z1 - z) < SmallestOccluderBlocks)
                            continue;
                        const auto metres = [&](i32 block, i32 chunk) {
                            return (static_cast<double>(chunk) * Edge + block) * size;
                        };
                        m_occluderBoxes.push_back(
                            BlockBox{core::DVec3{metres(x, lowX), metres(y, lowY), metres(z, lowZ)},
                                     core::DVec3{metres(x1, lowX), metres(y1, lowY), metres(z1, lowZ)}});
                    }
                }
            }
        }
    }
    // Each frame, only placed: the camera-relative unit cube of each box.
    for (const BlockBox& box : m_occluderBoxes) {
        const core::DVec3 middle{(box.low.x + box.high.x) * 0.5 - camera.x, (box.low.y + box.high.y) * 0.5 - camera.y,
                                 (box.low.z + box.high.z) * 0.5 - camera.z};
        const core::Vec3 extent{static_cast<core::f32>(box.high.x - box.low.x),
                                static_cast<core::f32>(box.high.y - box.low.y),
                                static_cast<core::f32>(box.high.z - box.low.z)};
        const core::Vec3 centre{static_cast<core::f32>(middle.x), static_cast<core::f32>(middle.y),
                                static_cast<core::f32>(middle.z)};
        out.occluderBoxes.push_back(OccluderBox{.box = core::translation(centre) * core::scaling(extent),
                                                .centre = centre,
                                                .radius = 0.5f * core::length(extent)});
    }
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
                const auto from = [&](const core::DVec3& viewer) {
                    const double dx = axis(key.x, viewer.x);
                    const double dy = axis(key.y, viewer.y);
                    const double dz = axis(key.z, viewer.z);
                    return std::sqrt(dx * dx + dy * dy + dz * dz);
                };
                // The nearest of the viewers (ADR 0205).
                distance = from(m_focus);
                for (const core::DVec3& other : m_otherFoci)
                    distance = std::min(distance, from(other));
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
