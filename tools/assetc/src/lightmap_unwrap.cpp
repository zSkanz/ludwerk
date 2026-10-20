#include "engine/assetc/lightmap_unwrap.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <xatlas.h>

namespace engine::assetc {

namespace {

using core::f32;
using core::u32;
using core::usize;

// The grid a file's own coordinates are checked on: a texel centre inside
// two triangles is two surfaces given the same light.
constexpr int CheckSide = 512;

} // namespace

bool lightmapOverlaps(const asset::Mesh& mesh)
{
    if (mesh.lightmapUvs.size() != mesh.vertices.size())
        return true;
    for (const asset::LightmapUv& uv : mesh.lightmapUvs) {
        if (!(uv.u >= 0.0f && uv.u <= 1.0f && uv.v >= 0.0f && uv.v <= 1.0f))
            return true;
    }
    std::vector<unsigned char> covered(static_cast<usize>(CheckSide) * CheckSide, 0);
    for (usize index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const asset::LightmapUv& a = mesh.lightmapUvs[mesh.indices[index]];
        const asset::LightmapUv& b = mesh.lightmapUvs[mesh.indices[index + 1]];
        const asset::LightmapUv& c = mesh.lightmapUvs[mesh.indices[index + 2]];
        const f32 area = (b.u - a.u) * (c.v - a.v) - (c.u - a.u) * (b.v - a.v);
        if (std::fabs(area) < 1e-12f)
            continue;
        const auto first = [](f32 low) { return std::max(0, static_cast<int>(std::floor(low * CheckSide - 0.5f))); };
        const auto last = [](f32 high) {
            return std::min(CheckSide - 1, static_cast<int>(std::ceil(high * CheckSide - 0.5f)));
        };
        const int x0 = first(std::min({a.u, b.u, c.u}));
        const int x1 = last(std::max({a.u, b.u, c.u}));
        const int y0 = first(std::min({a.v, b.v, c.v}));
        const int y1 = last(std::max({a.v, b.v, c.v}));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const f32 px = (static_cast<f32>(x) + 0.5f) / CheckSide;
                const f32 py = (static_cast<f32>(y) + 0.5f) / CheckSide;
                const f32 w0 = ((b.u - px) * (c.v - py) - (c.u - px) * (b.v - py)) / area;
                const f32 w1 = ((c.u - px) * (a.v - py) - (a.u - px) * (c.v - py)) / area;
                const f32 w2 = 1.0f - w0 - w1;
                // Strictly inside: two triangles that share an edge share no
                // centre on it.
                if (w0 <= 1e-4f || w1 <= 1e-4f || w2 <= 1e-4f)
                    continue;
                unsigned char& texel = covered[static_cast<usize>(y) * CheckSide + static_cast<usize>(x)];
                if (texel != 0)
                    return true;
                texel = 1;
            }
        }
    }
    return false;
}

bool unwrapLightmap(asset::Model& model)
{
    asset::Mesh& mesh = model.mesh;
    if (!model.skin.empty() || mesh.vertices.empty() || mesh.indices.size() < 3 || mesh.indices.size() % 3 != 0)
        return false;
    if (!mesh.lightmapUvs.empty()) {
        if (!lightmapOverlaps(mesh))
            return false;
        // The file's own overlap: it is charted afresh, as if it had none.
        mesh.lightmapUvs.clear();
    }

    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexPositionData = &mesh.vertices[0].position;
    decl.vertexPositionStride = sizeof(asset::Vertex);
    decl.vertexNormalData = &mesh.vertices[0].normal;
    decl.vertexNormalStride = sizeof(asset::Vertex);
    decl.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    decl.indexData = mesh.indices.data();
    decl.indexCount = static_cast<uint32_t>(mesh.indices.size());
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    // **Charts never cross a material**: a chart across two would be lit as
    // one surface where the file says there are two.
    std::vector<uint32_t> materials(mesh.indices.size() / 3, 0);
    for (const asset::Submesh& submesh : mesh.submeshes) {
        for (u32 index = submesh.firstIndex; index + 2 < submesh.firstIndex + submesh.indexCount; index += 3) {
            if (index / 3 < materials.size())
                materials[index / 3] = submesh.material;
        }
    }
    decl.faceMaterialData = materials.data();
    if (xatlas::AddMesh(atlas, decl, 1) != xatlas::AddMeshError::Success) {
        xatlas::Destroy(atlas);
        return false;
    }
    xatlas::PackOptions pack;
    // Two texels between charts at the resolution the packer picks, so a
    // bilinear tap at a chart's edge reads only its own.
    pack.padding = 2;
    xatlas::Generate(atlas, xatlas::ChartOptions{}, pack);
    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0 ||
        atlas->meshes[0].indexCount != mesh.indices.size()) {
        xatlas::Destroy(atlas);
        return false;
    }
    const xatlas::Mesh& out = atlas->meshes[0];

    // **By the longer side, both ways**: a part's rectangle of a page is
    // square, and a chart stretched to fill one of the packer's that is not
    // would be given more texels one way than the other.
    const auto side = static_cast<f32>(std::max(atlas->width, atlas->height));
    std::vector<asset::Vertex> vertices;
    std::vector<asset::LightmapUv> uvs;
    std::vector<std::vector<u32>> copies(mesh.vertices.size());
    vertices.reserve(out.vertexCount);
    uvs.reserve(out.vertexCount);
    for (uint32_t at = 0; at < out.vertexCount; ++at) {
        const xatlas::Vertex& vertex = out.vertexArray[at];
        vertices.push_back(mesh.vertices[vertex.xref]);
        uvs.push_back(asset::LightmapUv{std::clamp(vertex.uv[0] / side, 0.0f, 1.0f),
                                        std::clamp(vertex.uv[1] / side, 0.0f, 1.0f)});
        copies[vertex.xref].push_back(at);
    }
    // Each target's deltas follow the vertex they moved to every copy of it,
    // ascending by vertex as a target keeps them.
    for (asset::MorphTarget& target : model.morphs) {
        std::vector<asset::MorphDelta> spread;
        spread.reserve(target.deltas.size());
        for (const asset::MorphDelta& delta : target.deltas) {
            if (delta.vertex >= copies.size())
                continue;
            for (const u32 copy : copies[delta.vertex]) {
                asset::MorphDelta moved = delta;
                moved.vertex = copy;
                spread.push_back(moved);
            }
        }
        std::sort(spread.begin(), spread.end(),
                  [](const asset::MorphDelta& a, const asset::MorphDelta& b) { return a.vertex < b.vertex; });
        target.deltas = std::move(spread);
    }
    mesh.indices.assign(out.indexArray, out.indexArray + out.indexCount);
    mesh.vertices = std::move(vertices);
    mesh.lightmapUvs = std::move(uvs);
    xatlas::Destroy(atlas);
    return true;
}

} // namespace engine::assetc
