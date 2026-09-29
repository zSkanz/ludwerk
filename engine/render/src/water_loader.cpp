#include "engine/render/water_loader.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/core/log.h"
#include "engine/scene/components.h"

namespace engine::render {

using core::f32;
using core::u32;
using core::u64;
using core::usize;
using core::Vec3;

std::string waterGridUrn()
{
    return "water://grid";
}

std::string waterRiverUrn(core::InstanceId water)
{
    return "water://river/" + std::to_string(water.index) + "." + std::to_string(water.generation);
}

asset::Mesh waterGrid(u32 quads)
{
    asset::Mesh mesh;
    const u32 side = quads + 1;
    mesh.vertices.reserve(static_cast<usize>(side) * side);
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) {
            asset::Vertex vertex;
            const f32 u = static_cast<f32>(x) / static_cast<f32>(quads);
            const f32 v = static_cast<f32>(z) / static_cast<f32>(quads);
            vertex.position = Vec3{u - 0.5f, 0.0f, v - 0.5f};
            vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
            vertex.tangent[0] = 1.0f;
            vertex.uv[0] = u;
            vertex.uv[1] = v;
            mesh.vertices.push_back(vertex);
        }
    }
    mesh.indices.reserve(static_cast<usize>(quads) * quads * 6);
    for (u32 z = 0; z < quads; ++z) {
        for (u32 x = 0; x < quads; ++x) {
            const u32 a = z * side + x;
            const u32 b = a + 1;
            const u32 c = a + side;
            const u32 d = c + 1;
            // Counter-clockwise seen from above.
            for (const u32 index : {a, c, b, b, c, d})
                mesh.indices.push_back(index);
        }
    }
    mesh.bounds = core::AABB{Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 0.0f, 0.5f}};
    mesh.submeshes.push_back(asset::Submesh{0, static_cast<u32>(mesh.indices.size()), 0, mesh.bounds});
    return mesh;
}

asset::Mesh riverRibbon(const std::vector<Vec3>& points, float width, float step)
{
    asset::Mesh mesh;
    if (points.size() < 2 || !(width > 0.0f))
        return mesh;
    // Every stretch cut into rows about `step` apart; each row a line across,
    // square to the stretch, `width` long, of `across` quads.
    const u32 across = std::clamp(static_cast<u32>(std::ceil(width / std::max(step, 0.1f))), 1u, 64u);
    Vec3 minimum{points.front().x, 0.0f, points.front().z};
    Vec3 maximum = minimum;
    u32 rows = 0;
    const auto row = [&](Vec3 at, Vec3 along) {
        const Vec3 sideways = Vec3{-along.z, 0.0f, along.x};
        for (u32 column = 0; column <= across; ++column) {
            const f32 t = static_cast<f32>(column) / static_cast<f32>(across) - 0.5f;
            asset::Vertex vertex;
            vertex.position = Vec3{at.x + sideways.x * width * t, 0.0f, at.z + sideways.z * width * t};
            vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
            vertex.tangent[0] = 1.0f;
            vertex.uv[0] = t + 0.5f;
            vertex.uv[1] = static_cast<f32>(rows);
            minimum = Vec3{std::min(minimum.x, vertex.position.x), 0.0f, std::min(minimum.z, vertex.position.z)};
            maximum = Vec3{std::max(maximum.x, vertex.position.x), 0.0f, std::max(maximum.z, vertex.position.z)};
            mesh.vertices.push_back(vertex);
        }
        ++rows;
    };
    for (usize at = 0; at + 1 < points.size(); ++at) {
        const Vec3 from{points[at].x, 0.0f, points[at].z};
        const Vec3 to{points[at + 1].x, 0.0f, points[at + 1].z};
        const Vec3 span = to - from;
        const f32 length = core::length(span);
        if (length < 1e-3f)
            continue;
        const Vec3 along = span * (1.0f / length);
        const u32 cuts = std::clamp(static_cast<u32>(std::ceil(length / std::max(step, 0.1f))), 1u, 4096u);
        for (u32 cut = at == 0 ? 0u : 1u; cut <= cuts; ++cut)
            row(from + span * (static_cast<f32>(cut) / static_cast<f32>(cuts)), along);
    }
    const u32 stride = across + 1;
    for (u32 r = 0; r + 1 < rows; ++r) {
        for (u32 column = 0; column < across; ++column) {
            const u32 a = r * stride + column;
            const u32 b = a + 1;
            const u32 c = a + stride;
            const u32 d = c + 1;
            for (const u32 index : {a, c, b, b, c, d})
                mesh.indices.push_back(index);
        }
    }
    mesh.bounds = core::AABB{minimum, maximum};
    // Both windings are drawn the same by a flat ribbon; the faces are made to
    // face up whichever way the river turns.
    for (usize at = 0; at + 2 < mesh.indices.size(); at += 3) {
        const Vec3& pa = mesh.vertices[mesh.indices[at]].position;
        const Vec3& pb = mesh.vertices[mesh.indices[at + 1]].position;
        const Vec3& pc = mesh.vertices[mesh.indices[at + 2]].position;
        const f32 up = (pb.z - pa.z) * (pc.x - pa.x) - (pb.x - pa.x) * (pc.z - pa.z);
        if (up < 0.0f)
            std::swap(mesh.indices[at + 1], mesh.indices[at + 2]);
    }
    mesh.submeshes.push_back(asset::Submesh{0, static_cast<u32>(mesh.indices.size()), 0, mesh.bounds});
    return mesh;
}

namespace {

void upload(rhi::IDevice& device, rhi::ICmdList& cmd, MeshCache& cache, MeshLibrary& library, const asset::Mesh& mesh,
            core::NameAtom urn, MeshHandle& handle)
{
    if (handle.valid()) {
        cache.release(device, handle);
        handle = MeshHandle{};
    }
    if (mesh.indices.empty()) {
        library.remove(urn);
        return;
    }
    core::EngineError error;
    handle = cache.create(device, cmd, mesh, MeshUsage::Static, &error);
    if (!handle.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        library.remove(urn);
        return;
    }
    MeshLibrary::Entry entry;
    entry.mesh = handle;
    entry.bounds = mesh.bounds;
    entry.sectionCount = 1;
    entry.sectionMaterial.assign(1, 0u);
    entry.materials.push_back(RenderMaterial{});
    library.set(urn, std::move(entry));
}

} // namespace

u32 WaterLoader::sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world, core::AtomTable& atoms,
                      MeshCache& cache, MeshLibrary& library)
{
    u32 built = 0;
    if (world.waters().size() == 0 && m_rivers.empty())
        return 0;
    if (!m_grid.valid() && world.waters().size() > 0) {
        m_gridUrn = atoms.intern(waterGridUrn());
        upload(device, cmd, cache, library, waterGrid(WaterGridQuads), m_gridUrn, m_grid);
        ++built;
    }
    for (River& river : m_rivers)
        river.seen = false;
    world.waters().forEach([&](core::InstanceId id, const scene::WaterComponent& water) {
        if (water.shape != 2 || world.destroyed(id))
            return;
        // Its course, and a digest of it with its width: rebuilt only when
        // either moves.
        std::vector<Vec3> points;
        u64 shape = 0xCBF29CE484222325ull;
        const auto mix = [&shape](f32 value) {
            shape ^= std::bit_cast<u32>(value);
            shape *= 0x100000001B3ull;
        };
        mix(water.size.x);
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
            if (const scene::WaterPointComponent* point = world.waterPoints().find(child)) {
                points.push_back(point->position);
                mix(point->position.x);
                mix(point->position.z);
            }
        }
        auto found =
            std::find_if(m_rivers.begin(), m_rivers.end(), [&](const River& river) { return river.water == id; });
        if (found == m_rivers.end()) {
            m_rivers.push_back(River{id, 0, {}, atoms.intern(waterRiverUrn(id)), false});
            found = m_rivers.end() - 1;
        }
        found->seen = true;
        if (found->shape == shape && found->mesh.valid())
            return;
        found->shape = shape;
        upload(device, cmd, cache, library, riverRibbon(points, water.size.x, 1.0f), found->urn, found->mesh);
        ++built;
    });
    // A river gone, or no longer one, lets its ribbon go.
    for (usize at = m_rivers.size(); at > 0; --at) {
        River& river = m_rivers[at - 1];
        if (river.seen)
            continue;
        if (river.mesh.valid())
            cache.release(device, river.mesh);
        library.remove(river.urn);
        m_rivers.erase(m_rivers.begin() + static_cast<std::ptrdiff_t>(at - 1));
    }
    return built;
}

void WaterLoader::destroy(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library)
{
    if (m_grid.valid()) {
        cache.release(device, m_grid);
        library.remove(m_gridUrn);
        m_grid = MeshHandle{};
    }
    for (River& river : m_rivers) {
        if (river.mesh.valid())
            cache.release(device, river.mesh);
        library.remove(river.urn);
    }
    m_rivers.clear();
}

} // namespace engine::render
