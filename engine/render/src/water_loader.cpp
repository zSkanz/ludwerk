#include "engine/render/water_loader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include "engine/core/log.h"
#include "engine/scene/components.h"
#include "engine/scene/water.h"

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

asset::Mesh riverRibbon(const scene::WaterCourse& course, float step)
{
    asset::Mesh mesh;
    const std::vector<scene::WaterCourseSample>& samples = course.samples;
    if (samples.size() < 2)
        return mesh;
    double widest = 0.0;
    for (const scene::WaterCourseSample& sample : samples)
        widest = std::max(widest, sample.width);
    if (!(widest > 0.0))
        return mesh;
    const u32 across =
        std::clamp(static_cast<u32>(std::ceil(static_cast<f32>(widest) / std::max(step, 0.1f))), 1u, 64u);
    Vec3 minimum = core::toVec3(samples.front().position);
    Vec3 maximum = minimum;
    for (usize at = 0; at < samples.size(); ++at) {
        // Square to the way it runs here: from the sample before to the one
        // after, across the ground.
        const core::DVec3& before = samples[at == 0 ? 0 : at - 1].position;
        const core::DVec3& after = samples[std::min(at + 1, samples.size() - 1)].position;
        double dx = after.x - before.x;
        double dz = after.z - before.z;
        const double length = std::sqrt(dx * dx + dz * dz);
        if (length > 1e-9) {
            dx /= length;
            dz /= length;
        }
        else {
            dx = 1.0;
            dz = 0.0;
        }
        const scene::WaterCourseSample& sample = samples[at];
        for (u32 column = 0; column <= across; ++column) {
            const double t = static_cast<double>(column) / static_cast<double>(across) - 0.5;
            asset::Vertex vertex;
            vertex.position =
                Vec3{static_cast<f32>(sample.position.x - dz * sample.width * t), static_cast<f32>(sample.position.y),
                     static_cast<f32>(sample.position.z + dx * sample.width * t)};
            vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
            vertex.tangent[0] = 1.0f;
            vertex.uv[0] = static_cast<f32>(t + 0.5);
            vertex.uv[1] = static_cast<f32>(at);
            minimum = Vec3{std::min(minimum.x, vertex.position.x), std::min(minimum.y, vertex.position.y),
                           std::min(minimum.z, vertex.position.z)};
            maximum = Vec3{std::max(maximum.x, vertex.position.x), std::max(maximum.y, vertex.position.y),
                           std::max(maximum.z, vertex.position.z)};
            mesh.vertices.push_back(vertex);
        }
    }
    const u32 stride = across + 1;
    const u32 rows = static_cast<u32>(samples.size());
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
    // The faces are made to face up whichever way the river turns.
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

asset::Mesh lakeSurface(const scene::WaterCourse& course, double level)
{
    asset::Mesh mesh;
    const std::vector<scene::WaterCourseSample>& outline = course.samples;
    if (!course.closed || outline.size() < 3)
        return mesh;
    const double spanX = course.maxX - course.minX;
    const double spanZ = course.maxZ - course.minZ;
    if (!(spanX > 0.0) || !(spanZ > 0.0))
        return mesh;
    // Cells a metre across, and never more than 128 a side: a lake a
    // kilometre wide is drawn with cells of eight.
    const double cell = std::max(1.0, std::max(spanX, spanZ) / 128.0);
    const u32 columns = static_cast<u32>(std::ceil(spanX / cell));
    const u32 rows = static_cast<u32>(std::ceil(spanZ / cell));
    const auto inside = [&](double x, double z) {
        bool in = false;
        for (usize at = 0, last = outline.size() - 1; at < outline.size(); last = at++) {
            const core::DVec3& a = outline[at].position;
            const core::DVec3& b = outline[last].position;
            if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x)
                in = !in;
        }
        return in;
    };
    // The nearest point of the outline to one outside it.
    const auto ontoOutline = [&](double& x, double& z) {
        double best = std::numeric_limits<double>::max();
        double bx = x;
        double bz = z;
        for (usize at = 0, last = outline.size() - 1; at < outline.size(); last = at++) {
            const core::DVec3& a = outline[last].position;
            const core::DVec3& b = outline[at].position;
            const double dx = b.x - a.x;
            const double dz = b.z - a.z;
            const double length = dx * dx + dz * dz;
            const double t = length > 1e-12 ? std::clamp(((x - a.x) * dx + (z - a.z) * dz) / length, 0.0, 1.0) : 0.0;
            const double px = a.x + dx * t;
            const double pz = a.z + dz * t;
            const double away = (px - x) * (px - x) + (pz - z) * (pz - z);
            if (away < best) {
                best = away;
                bx = px;
                bz = pz;
            }
        }
        x = bx;
        z = bz;
    };
    const u32 stride = columns + 1;
    std::vector<core::u8> within(static_cast<usize>(stride) * (rows + 1));
    for (u32 r = 0; r <= rows; ++r) {
        for (u32 c = 0; c <= columns; ++c)
            within[static_cast<usize>(r) * stride + c] =
                inside(course.minX + c * cell, course.minZ + r * cell) ? core::u8{1} : core::u8{0};
    }
    // A vertex for every corner of a cell the lake reaches: the ones inside
    // where they are, the ones outside on the outline.
    constexpr u32 None = ~u32{0};
    std::vector<u32> slot(within.size(), None);
    Vec3 minimum{static_cast<f32>(course.minX), static_cast<f32>(level), static_cast<f32>(course.minZ)};
    Vec3 maximum{static_cast<f32>(course.maxX), static_cast<f32>(level), static_cast<f32>(course.maxZ)};
    const auto vertexAt = [&](u32 c, u32 r) {
        const usize index = static_cast<usize>(r) * stride + c;
        if (slot[index] != None)
            return slot[index];
        double x = course.minX + c * cell;
        double z = course.minZ + r * cell;
        if (within[index] == 0)
            ontoOutline(x, z);
        asset::Vertex vertex;
        vertex.position = Vec3{static_cast<f32>(x), static_cast<f32>(level), static_cast<f32>(z)};
        vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
        vertex.tangent[0] = 1.0f;
        vertex.uv[0] = static_cast<f32>((x - course.minX) / spanX);
        vertex.uv[1] = static_cast<f32>((z - course.minZ) / spanZ);
        slot[index] = static_cast<u32>(mesh.vertices.size());
        mesh.vertices.push_back(vertex);
        return slot[index];
    };
    for (u32 r = 0; r < rows; ++r) {
        for (u32 c = 0; c < columns; ++c) {
            const usize at = static_cast<usize>(r) * stride + c;
            if (within[at] + within[at + 1] + within[at + stride] + within[at + stride + 1] == 0)
                continue;
            const u32 a = vertexAt(c, r);
            const u32 b = vertexAt(c + 1, r);
            const u32 d = vertexAt(c, r + 1);
            const u32 e = vertexAt(c + 1, r + 1);
            // Counter-clockwise seen from above, as the grid is.
            for (const u32 index : {a, d, b, b, d, e})
                mesh.indices.push_back(index);
        }
    }
    if (mesh.indices.empty())
        return mesh;
    mesh.bounds = core::AABB{minimum, maximum};
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
        const bool river = scene::waterIsRiver(water.shape);
        if (!(river || water.shape == scene::water_shape::Lake) || world.destroyed(id))
            return;
        // A digest of everything its course is made from: rebuilt only when
        // one of them moves.
        u64 shape = 0xCBF29CE484222325ull;
        const auto mix = [&shape](f32 value) {
            shape ^= std::bit_cast<u32>(value);
            shape *= 0x100000001B3ull;
        };
        mix(static_cast<f32>(water.shape));
        mix(water.size.x);
        mix(static_cast<f32>(water.surfaceLevel));
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
            if (const scene::WaterPointComponent* point = world.waterPoints().find(child)) {
                mix(point->position.x);
                mix(point->position.y);
                mix(point->position.z);
                mix(static_cast<f32>(point->width));
                mix(point->sharp ? 1.0f : 0.0f);
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
        const scene::WaterCourse course = scene::courseOf(world, id);
        upload(device, cmd, cache, library, river ? riverRibbon(course, 1.0f) : lakeSurface(course, water.surfaceLevel),
               found->urn, found->mesh);
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
