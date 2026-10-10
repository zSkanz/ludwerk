#include "engine/render/mesh_cache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

#include "engine/core/i18n.h"
#include "engine/core/text_key.h"

namespace engine::render {
namespace {

using core::i32;

constexpr usize kVertexSize = sizeof(asset::Vertex);
constexpr usize kIndexSize = sizeof(u32);

// A pooled page, in elements: about three megabytes of vertices and one of
// indices, a few hundred block chunks' worth. A mesh larger than a page gets
// a page of its own size.
constexpr u32 kPoolVertexPage = 65536;
constexpr u32 kPoolIndexPage = 262144;

[[nodiscard]] std::span<const std::byte> asBytes(const asset::Mesh& mesh) noexcept
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(mesh.vertices.data()),
                                      mesh.vertices.size() * kVertexSize);
}

[[nodiscard]] std::span<const std::byte> indexBytes(const asset::Mesh& mesh) noexcept
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(mesh.indices.data()),
                                      mesh.indices.size() * kIndexSize);
}

// Doubling from the high-water mark rather than fitting exactly: a ring that
// grows to precisely what one frame asked for grows again on the next frame that
// asks for one more vertex, and each growth is a device allocation.
[[nodiscard]] u32 grownTo(u32 current, u32 needed) noexcept
{
    u32 next = current == 0 ? 1u : current;
    while (next < needed) {
        // Saturate rather than wrap. A request this large is a bug upstream, and
        // the allocation below will fail with a keyed error instead of silently
        // producing a tiny buffer.
        if (next > 0x7FFFFFFFu)
            return needed;
        next *= 2u;
    }
    return next;
}

} // namespace

// The span in `Entry::resolved` points into `Entry::sections`'s heap buffer, so
// it survives `entries_` reallocating only because growth MOVES an Entry and a
// moved vector hands over its buffer. Copying one would leave the span pointing
// at the original's storage.
static_assert(std::is_nothrow_move_constructible_v<std::vector<MeshSection>>,
              "MeshCache::Entry must be moved, never copied, or Resolved::sections dangles");

MeshCache::~MeshCache()
{
    // Nothing to release here: GPU handles need the device, and holding a
    // reference to it for the lifetime of the cache would make destruction order
    // a trap. `destroy(device)` is the contract, and a cache destroyed without
    // it leaks -- loudly, in the backend's own leak report, rather than by
    // crashing during shutdown.
}

std::optional<core::EngineError> MeshCache::create(rhi::IDevice& device, u32 ringVertexCapacity, u32 ringIndexCapacity)
{
    if (auto error = growRing(device, ringVertexCapacity, ringIndexCapacity); error.has_value())
        return error;
    return std::nullopt;
}

std::optional<core::EngineError> MeshCache::growRing(rhi::IDevice& device, u32 vertices, u32 indices)
{
    const u32 nextVertices = grownTo(ringVertexCapacity_, vertices);
    const u32 nextIndices = grownTo(ringIndexCapacity_, indices);

    const rhi::BufferHandle newVertices = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = static_cast<u32>(nextVertices * kVertexSize),
        .debugName = "mesh-ring-vertices",
    });
    if (!newVertices.valid())
        return core::makeError(ENG_TR("render.err.mesh_buffer_failed"), {}, "dynamic vertex ring");

    const rhi::BufferHandle newIndices = device.createBuffer({
        .usage = rhi::BufferUsage::Index,
        .sizeBytes = static_cast<u32>(nextIndices * kIndexSize),
        .debugName = "mesh-ring-indices",
    });
    if (!newIndices.valid()) {
        device.destroy(newVertices);
        return core::makeError(ENG_TR("render.err.mesh_buffer_failed"), {}, "dynamic index ring");
    }

    // The old buffers are retired rather than destroyed, because dynamic handles
    // already issued THIS frame still name them and are still legal to draw.
    // They die at the next `beginFrame`, which is the moment those handles stop
    // resolving anyway.
    if (ringVertices_.valid())
        retiring_.push_back({ringVertices_, ringIndices_});

    ringVertices_ = newVertices;
    ringIndices_ = newIndices;
    ringVertexCapacity_ = nextVertices;
    ringIndexCapacity_ = nextIndices;
    ringVertexUsed_ = 0;
    ringIndexUsed_ = 0;
    return std::nullopt;
}

MeshCache::PoolSlice MeshCache::takeSlice(rhi::IDevice& device, std::vector<PoolPage>& pages, u32 count, bool vertices)
{
    for (u32 at = 0; at < pages.size(); ++at) {
        PoolPage& page = pages[at];
        if (!page.buffer.valid())
            continue;
        for (usize range = 0; range < page.free.size(); ++range) {
            if (page.free[range].count < count)
                continue;
            const u32 offset = page.free[range].offset;
            page.free[range].offset += count;
            page.free[range].count -= count;
            if (page.free[range].count == 0)
                page.free.erase(page.free.begin() + static_cast<std::ptrdiff_t>(range));
            return PoolSlice{at, offset, true};
        }
    }

    // No page has the room: one more, in the first slot a destroyed page left.
    const u32 capacity = std::max(count, vertices ? kPoolVertexPage : kPoolIndexPage);
    const rhi::BufferHandle buffer = device.createBuffer({
        .usage = vertices ? rhi::BufferUsage::Vertex : rhi::BufferUsage::Index,
        .sizeBytes = static_cast<u32>(capacity * (vertices ? kVertexSize : kIndexSize)),
        .debugName = vertices ? "mesh-pool-vertices" : "mesh-pool-indices",
    });
    if (!buffer.valid())
        return {};
    u32 slot = static_cast<u32>(pages.size());
    for (u32 at = 0; at < pages.size(); ++at) {
        if (!pages[at].buffer.valid()) {
            slot = at;
            break;
        }
    }
    if (slot == pages.size())
        pages.emplace_back();
    PoolPage& page = pages[slot];
    page.buffer = buffer;
    page.capacity = capacity;
    page.free.clear();
    if (capacity > count)
        page.free.push_back({count, capacity - count});
    return PoolSlice{slot, 0, true};
}

void MeshCache::giveSlice(rhi::IDevice& device, std::vector<PoolPage>& pages, u32 pageIndex, u32 offset, u32 count)
{
    if (pageIndex >= pages.size() || count == 0)
        return;
    PoolPage& page = pages[pageIndex];
    const auto after = std::lower_bound(page.free.begin(), page.free.end(), offset,
                                        [](const PoolPage::Range& range, u32 at) { return range.offset < at; });
    const auto placed = page.free.insert(after, PoolPage::Range{offset, count});
    // Joined with the free range after it, then with the one before.
    if (const auto next = placed + 1; next != page.free.end() && placed->offset + placed->count == next->offset) {
        placed->count += next->count;
        page.free.erase(next);
    }
    if (placed != page.free.begin()) {
        const auto before = placed - 1;
        if (before->offset + before->count == placed->offset) {
            before->count += placed->count;
            page.free.erase(placed);
        }
    }

    // **A page nothing is in goes back, unless it is the last one empty**: a
    // world left behind gives its memory up, and a player walking does not
    // make and destroy a page at every chunk border.
    if (page.free.size() != 1 || page.free.front().count != page.capacity)
        return;
    const bool another = std::any_of(pages.begin(), pages.end(), [&](const PoolPage& other) {
        return &other != &page && other.buffer.valid() && other.free.size() == 1 &&
               other.free.front().count == other.capacity;
    });
    if (!another)
        return;
    device.destroy(page.buffer);
    page = PoolPage{};
}

void MeshCache::destroy(rhi::IDevice& device)
{
    for (std::vector<PoolPage>* pages : {&vertexPages_, &indexPages_}) {
        for (PoolPage& page : *pages) {
            if (page.buffer.valid())
                device.destroy(page.buffer);
        }
        pages->clear();
    }
    for (Entry& entry : entries_) {
        // A pooled mesh's buffers are the pages', gone above.
        if (entry.live && !entry.dynamic && !entry.pooled) {
            device.destroy(entry.resolved.vertices);
            device.destroy(entry.resolved.indices);
            if (entry.resolved.skin.valid())
                device.destroy(entry.resolved.skin);
            if (entry.resolved.morph.valid())
                device.destroy(entry.resolved.morph);
        }
        entry.live = false;
    }
    entries_.clear();
    freeSlots_.clear();

    for (const RetiredRing& ring : retiring_) {
        device.destroy(ring.vertices);
        device.destroy(ring.indices);
    }
    retiring_.clear();
    for (const RetiredRing& ring : retired_) {
        device.destroy(ring.vertices);
        device.destroy(ring.indices);
    }
    retired_.clear();

    if (ringVertices_.valid())
        device.destroy(ringVertices_);
    if (ringIndices_.valid())
        device.destroy(ringIndices_);
    ringVertices_ = {};
    ringIndices_ = {};
    ringVertexCapacity_ = 0;
    ringIndexCapacity_ = 0;
    ringVertexUsed_ = 0;
    ringIndexUsed_ = 0;
}

void MeshCache::beginFrame(rhi::IDevice& device)
{
    // A dynamic entry is not erased -- its slot is recycled, and the frame stamp
    // is what stops its handle resolving. Erasing would let a slot be reused by
    // a static mesh with the same generation, which is the one way a stale
    // dynamic handle could come back as somebody else's geometry.
    for (usize index = 0; index < entries_.size(); ++index) {
        Entry& entry = entries_[index];
        if (entry.live && entry.dynamic) {
            entry.live = false;
            entry.sections.clear();
            entry.lods.clear();
            entry.resolved = Resolved{};
            freeSlots_.push_back(static_cast<u32>(index));
        }
    }

    ringVertexUsed_ = 0;
    ringIndexUsed_ = 0;

    // Two frames of slack, not one. A handle issued before a mid-frame grow is
    // legal to draw for the rest of that frame, and the GPU may still be
    // executing those commands when the next frame begins.
    for (const RetiredRing& ring : retired_) {
        device.destroy(ring.vertices);
        device.destroy(ring.indices);
    }
    retired_.swap(retiring_);
    retiring_.clear();
}

MeshHandle MeshCache::createSkinned(rhi::IDevice& device, rhi::ICmdList& cmd, const asset::Mesh& mesh,
                                    std::span<const asset::SkinVertex> skin, core::EngineError* outError,
                                    std::span<const MeshLodRange> lods)
{
    if (skin.size() != mesh.vertices.size()) {
        if (outError != nullptr)
            *outError = core::makeError(ENG_TR("render.err.mesh_buffer_failed"), {}, "skin stream length");
        return {};
    }

    const MeshHandle handle = create(device, cmd, mesh, MeshUsage::Static, outError, lods);
    if (!handle.valid() || skin.empty())
        return handle;

    Entry& entry = entries_[handle.index];
    const auto sizeBytes = static_cast<u32>(skin.size() * sizeof(asset::SkinVertex));
    entry.resolved.skin = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = sizeBytes,
        .debugName = "mesh-skin",
    });
    if (!entry.resolved.skin.valid()) {
        // The geometry is already up and drawable; what is lost is the skinning.
        // Releasing the whole mesh over it would turn a character that stands
        // still into a character that is not there, which is the worse failure.
        if (outError != nullptr)
            *outError = core::makeError(ENG_TR("render.err.mesh_buffer_failed"), {}, "skin stream");
        return handle;
    }

    cmd.upload(entry.resolved.skin, std::as_bytes(skin), 0);
    return handle;
}

bool MeshCache::attachMorphs(rhi::IDevice& device, rhi::ICmdList& cmd, MeshHandle handle, const MorphTable& table,
                             core::EngineError* outError)
{
    if (table.empty() || !handle.valid() || handle.index >= entries_.size())
        return false;
    Entry& entry = entries_[handle.index];
    // Static only: the shader finds a vertex's row by the vertex's number,
    // and a slice of a shared buffer numbers its vertices from the slice on
    // one backend and from the buffer on another.
    if (!entry.live || entry.generation != handle.generation || entry.dynamic || entry.pooled ||
        entry.resolved.vertexOffset != 0 || entry.resolved.morph.valid())
        return false;

    entry.resolved.morph = device.createBuffer({
        .usage = rhi::BufferUsage::GraphicsStorageRead,
        .sizeBytes = static_cast<u32>(table.rows.size() * sizeof(GpuMorphDelta)),
        .debugName = "mesh-morphs",
    });
    if (!entry.resolved.morph.valid()) {
        // The mesh still draws, at rest: what is lost is the targets.
        if (outError != nullptr)
            *outError = core::makeError(ENG_TR("render.err.mesh_buffer_failed"), {}, "morph table");
        return false;
    }
    cmd.upload(entry.resolved.morph, std::as_bytes(std::span<const GpuMorphDelta>{table.rows}), 0);
    entry.resolved.morphFirstVertex = table.firstVertex;
    entry.resolved.morphVertexCount = table.vertexCount;
    entry.resolved.morphTargetCount = table.targetCount;
    return true;
}

MeshHandle MeshCache::create(rhi::IDevice& device, rhi::ICmdList& cmd, const asset::Mesh& mesh, MeshUsage usage,
                             core::EngineError* outError, std::span<const MeshLodRange> lods)
{
    const auto vertexCount = static_cast<u32>(mesh.vertices.size());
    const auto indexCount = static_cast<u32>(mesh.indices.size());

    Resolved resolved;
    resolved.bounds = mesh.bounds;
    PoolSlice vertexSlice;
    PoolSlice indexSlice;

    if (usage == MeshUsage::Pooled) {
        // A slice of a shared page each, and a copy into it: no buffer is made
        // unless every page is full. An empty mesh takes nothing.
        if (vertexCount > 0) {
            vertexSlice = takeSlice(device, vertexPages_, vertexCount, true);
            indexSlice =
                vertexSlice.valid ? takeSlice(device, indexPages_, std::max(indexCount, 1u), false) : PoolSlice{};
            if (!vertexSlice.valid || !indexSlice.valid) {
                if (vertexSlice.valid)
                    giveSlice(device, vertexPages_, vertexSlice.page, vertexSlice.offset, vertexCount);
                if (outError != nullptr)
                    *outError = core::makeError(ENG_TR("render.err.mesh_buffer_failed"), {}, "pooled mesh");
                return {};
            }
            resolved.vertices = vertexPages_[vertexSlice.page].buffer;
            resolved.indices = indexPages_[indexSlice.page].buffer;
            resolved.vertexOffset = static_cast<i32>(vertexSlice.offset);
            resolved.firstIndex = indexSlice.offset;
            cmd.upload(resolved.vertices, asBytes(mesh), static_cast<u32>(vertexSlice.offset * kVertexSize));
            if (indexCount > 0)
                cmd.upload(resolved.indices, indexBytes(mesh), static_cast<u32>(indexSlice.offset * kIndexSize));
        }
    }
    else if (usage == MeshUsage::Static) {
        // An empty mesh still gets a handle: a generator that produced nothing
        // this frame is not an error, and the alternative is every caller
        // branching on emptiness before it can draw.
        if (vertexCount > 0) {
            resolved.vertices = device.createBuffer({
                .usage = rhi::BufferUsage::Vertex,
                .sizeBytes = static_cast<u32>(vertexCount * kVertexSize),
                .debugName = "mesh-vertices",
            });
            resolved.indices = device.createBuffer({
                .usage = rhi::BufferUsage::Index,
                .sizeBytes = static_cast<u32>(indexCount * kIndexSize),
                .debugName = "mesh-indices",
            });
            if (!resolved.vertices.valid() || !resolved.indices.valid()) {
                if (resolved.vertices.valid())
                    device.destroy(resolved.vertices);
                if (resolved.indices.valid())
                    device.destroy(resolved.indices);
                if (outError != nullptr)
                    *outError = core::makeError(ENG_TR("render.err.mesh_buffer_failed"), {}, "static mesh");
                return {};
            }

            cmd.upload(resolved.vertices, asBytes(mesh), 0);
            if (indexCount > 0)
                cmd.upload(resolved.indices, indexBytes(mesh), 0);
        }
    }
    else {
        if (ringVertexUsed_ + vertexCount > ringVertexCapacity_ || ringIndexUsed_ + indexCount > ringIndexCapacity_) {
            if (auto error = growRing(device, ringVertexUsed_ + vertexCount, ringIndexUsed_ + indexCount);
                error.has_value()) {
                if (outError != nullptr)
                    *outError = *error;
                return {};
            }
        }

        resolved.vertices = ringVertices_;
        resolved.indices = ringIndices_;
        resolved.firstIndex = ringIndexUsed_;
        resolved.vertexOffset = static_cast<i32>(ringVertexUsed_);

        if (vertexCount > 0)
            cmd.upload(resolved.vertices, asBytes(mesh), static_cast<u32>(ringVertexUsed_ * kVertexSize));
        if (indexCount > 0)
            cmd.upload(resolved.indices, indexBytes(mesh), static_cast<u32>(ringIndexUsed_ * kIndexSize));

        ringVertexUsed_ += vertexCount;
        ringIndexUsed_ += indexCount;
        ringVertexHighWater_ = ringVertexUsed_ > ringVertexHighWater_ ? ringVertexUsed_ : ringVertexHighWater_;
        ringIndexHighWater_ = ringIndexUsed_ > ringIndexHighWater_ ? ringIndexUsed_ : ringIndexHighWater_;
    }

    u32 slot = 0;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
    }
    else {
        slot = static_cast<u32>(entries_.size());
        entries_.emplace_back();
    }

    Entry& entry = entries_[slot];
    // Generation starts at 1 and only ever rises, so a default-constructed
    // `MeshHandle` (generation 0) can never name a live entry.
    ++entry.generation;
    entry.dynamic = usage == MeshUsage::Dynamic;
    entry.live = true;
    entry.pooled = usage == MeshUsage::Pooled;
    entry.vertexPage = vertexSlice.page;
    entry.vertexCount = vertexSlice.valid ? vertexCount : 0;
    entry.indexPage = indexSlice.page;
    entry.indexCount = indexSlice.valid ? std::max(indexCount, 1u) : 0;

    entry.sections.clear();
    entry.sections.reserve(mesh.submeshes.size());
    for (const asset::Submesh& submesh : mesh.submeshes) {
        entry.sections.push_back(MeshSection{
            .firstIndex = submesh.firstIndex,
            .indexCount = submesh.indexCount,
            .material = submesh.material,
            .bounds = submesh.bounds,
        });
    }

    entry.lods.assign(lods.begin(), lods.end());
    if (entry.lods.empty()) {
        // One level covering everything. Never empty, so no caller downstream
        // has to handle "a mesh with no levels" -- a case that would exist only
        // to be forgotten.
        entry.lods.push_back(MeshLodRange{
            .firstSection = 0,
            .sectionCount = static_cast<u32>(entry.sections.size()),
            .error = 0.0f,
        });
    }

    entry.resolved = resolved;
    entry.resolved.sections = entry.sections;
    entry.resolved.lods = entry.lods;
    return MeshHandle{slot, entry.generation};
}

void MeshCache::release(rhi::IDevice& device, MeshHandle handle)
{
    if (!handle.valid() || handle.index >= entries_.size())
        return;

    Entry& entry = entries_[handle.index];
    if (!entry.live || entry.generation != handle.generation || entry.dynamic)
        return;

    if (entry.pooled) {
        // Its slices go back to their pages, for the next mesh.
        if (entry.vertexCount > 0)
            giveSlice(device, vertexPages_, entry.vertexPage, static_cast<u32>(entry.resolved.vertexOffset),
                      entry.vertexCount);
        if (entry.indexCount > 0)
            giveSlice(device, indexPages_, entry.indexPage, entry.resolved.firstIndex, entry.indexCount);
        entry.pooled = false;
        entry.vertexCount = 0;
        entry.indexCount = 0;
    }
    else {
        if (entry.resolved.vertices.valid())
            device.destroy(entry.resolved.vertices);
        if (entry.resolved.indices.valid())
            device.destroy(entry.resolved.indices);
        if (entry.resolved.skin.valid())
            device.destroy(entry.resolved.skin);
        if (entry.resolved.morph.valid())
            device.destroy(entry.resolved.morph);
    }

    entry.live = false;
    entry.sections.clear();
    entry.lods.clear();
    entry.resolved = Resolved{};
    freeSlots_.push_back(handle.index);
}

const MeshCache::Resolved* MeshCache::resolve(MeshHandle handle) const noexcept
{
    if (!handle.valid() || handle.index >= entries_.size())
        return nullptr;

    const Entry& entry = entries_[handle.index];
    if (!entry.live || entry.generation != handle.generation)
        return nullptr;
    return &entry.resolved;
}

usize MeshCache::staticMeshCount() const noexcept
{
    usize count = 0;
    for (const Entry& entry : entries_) {
        if (entry.live && !entry.dynamic)
            ++count;
    }
    return count;
}

u32 selectMeshLod(const MeshCache::Resolved& resolved, const core::Mat4& transform, core::f32 pixelsPerUnit,
                  core::f32 pixelError) noexcept
{
    if (resolved.lods.size() <= 1 || pixelsPerUnit <= 0.0f || pixelError <= 0.0f) {
        return 0;
    }

    // Camera-relative space, so the camera is the origin and the translation is
    // the offset to it (`render_world.h`). **Column three**: `Mat4` is `m[c][r]`
    // (`core/math.h`), and reading row three instead read zeros -- a distance
    // of nothing, so every imported mesh drew its finest level everywhere
    // (audit R1).
    const core::f32 x = transform.m[3][0];
    const core::f32 y = transform.m[3][1];
    const core::f32 z = transform.m[3][2];
    const core::f32 distance = std::sqrt(x * x + y * y + z * z);

    // At or inside the near plane the projected error is unbounded, and the
    // answer there is obvious: draw the best level.
    if (distance <= 0.001f) {
        return 0;
    }

    // The instance's LARGEST axis scale, because an error is a length and a
    // non-uniform scale stretches it differently along each axis. The largest is
    // the conservative choice: it never picks a level that looks worse than the
    // threshold, only sometimes one that looks better than it had to.
    const auto axisLength = [&transform](int column) {
        const core::f32 ax = transform.m[column][0];
        const core::f32 ay = transform.m[column][1];
        const core::f32 az = transform.m[column][2];
        return std::sqrt(ax * ax + ay * ay + az * az);
    };
    const core::f32 scale = std::max({axisLength(0), axisLength(1), axisLength(2)});

    // The COARSEST level whose error still fits, walked from the far end so the
    // answer is the cheapest acceptable one rather than the first acceptable
    // one. Level 0 always qualifies -- its error is zero by construction -- so
    // this terminates.
    for (u32 level = static_cast<u32>(resolved.lods.size()); level > 0; --level) {
        const core::f32 projected = resolved.lods[level - 1].error * scale * pixelsPerUnit / distance;
        if (projected <= pixelError) {
            return level - 1;
        }
    }
    return 0;
}

} // namespace engine::render
