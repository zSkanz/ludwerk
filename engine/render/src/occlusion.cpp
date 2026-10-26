#include "engine/render/occlusion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "engine/jobs/jobs.h"
#include "engine/render/render_world.h"

namespace engine::render {
namespace {

using core::f32;
using core::f64;
using core::i64;
using core::Mat4;
using core::u32;
using core::u64;
using core::usize;
using core::Vec3;

constexpr f32 Far = std::numeric_limits<f32>::infinity();

// Whether a camera is a perspective one: its clip w is the depth.
[[nodiscard]] bool perspective(const Mat4& projection) noexcept
{
    return projection.m[3][3] == 0.0f;
}

} // namespace

// `Mat4` is `m[column][row]`.
OcclusionCuller::Clip OcclusionCuller::toClip(const Mat4& m, Vec3 p) noexcept
{
    return {m.m[0][0] * p.x + m.m[1][0] * p.y + m.m[2][0] * p.z + m.m[3][0],
            m.m[0][1] * p.x + m.m[1][1] * p.y + m.m[2][1] * p.z + m.m[3][1],
            m.m[0][2] * p.x + m.m[1][2] * p.y + m.m[2][2] * p.z + m.m[3][2],
            m.m[0][3] * p.x + m.m[1][3] * p.y + m.m[2][3] * p.z + m.m[3][3]};
}

OcclusionCuller::ScreenPoint OcclusionCuller::toScreen(const Clip& clip) noexcept
{
    const f32 invW = 1.0f / clip.w;
    return {(clip.x * invW * 0.5f + 0.5f) * static_cast<f32>(Width),
            (0.5f - clip.y * invW * 0.5f) * static_cast<f32>(Height), invW};
}

void OcclusionCuller::clear()
{
    depth_.assign(static_cast<usize>(Width) * Height, Far);
    frameFaces_.clear();
    frameCorners_.clear();
}

bool OcclusionCuller::setUpFace(Face& face) noexcept
{
    const usize count = face.count;
    const auto& v = face.v;
    f32 area = 0.0f;
    for (usize i = 0; i < count; ++i)
        area += v[i].x * v[(i + 1) % count].y - v[(i + 1) % count].x * v[i].y;
    if (std::fabs(area) < 1e-6f)
        return false;
    const f32 sign = area > 0.0f ? 1.0f : -1.0f;
    // Each edge as e(x, y) = A x + B y + C, positive inside, and how far e
    // moves over half a pixel: a pixel touches the face's side of an edge
    // where e at its middle is above -half, and is wholly on it above +half.
    for (usize e = 0; e < count; ++e) {
        const ScreenPoint& from = v[e];
        const ScreenPoint& to = v[(e + 1) % count];
        face.a[e] = -(to.y - from.y) * sign;
        face.b[e] = (to.x - from.x) * sign;
        face.c[e] = -(face.a[e] * from.x + face.b[e] * from.y);
        face.half[e] = 0.5f * (std::fabs(face.a[e]) + std::fabs(face.b[e]));
    }
    // 1/w is linear on the screen over a flat face: its plane, from three
    // of its corners, for the farthest depth the face has over a pixel.
    const ScreenPoint& p0 = v[0];
    const ScreenPoint& p1 = v[1];
    const ScreenPoint& p2 = v[2];
    const f32 det = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    if (std::fabs(det) < 1e-9f)
        return false;
    face.slopeX = ((p1.invW - p0.invW) * (p2.y - p0.y) - (p2.invW - p0.invW) * (p1.y - p0.y)) / det;
    face.slopeY = ((p2.invW - p0.invW) * (p1.x - p0.x) - (p1.invW - p0.invW) * (p2.x - p0.x)) / det;
    face.reach = 0.5f * (std::fabs(face.slopeX) + std::fabs(face.slopeY));
    face.lowX = face.highX = v[0].x;
    face.lowY = face.highY = v[0].y;
    for (usize i = 1; i < count; ++i) {
        face.lowX = std::min(face.lowX, v[i].x);
        face.highX = std::max(face.highX, v[i].x);
        face.lowY = std::min(face.lowY, v[i].y);
        face.highY = std::max(face.highY, v[i].y);
    }
    return true;
}

f32 OcclusionCuller::farthest(const Face& face, f32 x, f32 y) noexcept
{
    const f32 invW = face.v[0].invW + face.slopeX * (x - face.v[0].x) + face.slopeY * (y - face.v[0].y) - face.reach;
    return invW > 0.0f ? 1.0f / invW : Far;
}

void OcclusionCuller::drawFaces(std::span<const Face> faces, f32 bandLow, f32 bandHigh)
{
    for (const Face& face : faces) {
        if (!face.valid)
            continue;
        if (face.highX < 0.0f || face.highY < 0.0f || face.lowX > static_cast<f32>(Width) ||
            face.lowY > static_cast<f32>(Height))
            continue;
        const f32 minX = std::max(0.0f, std::floor(face.lowX));
        const f32 maxX = std::min(static_cast<f32>(Width) - 1.0f, std::ceil(face.highX));
        const f32 minY = std::max(bandLow, std::floor(face.lowY));
        const f32 maxY = std::min(bandHigh, std::ceil(face.highY));
        for (f32 y = minY; y <= maxY; y += 1.0f) {
            const f32 cy = y + 0.5f;
            // **The row's span**: the pixels whose middles every edge lets
            // touch the face -- a solved inequality an edge, where the box
            // would have tested each pixel against all of them.
            f32 rowLow = minX;
            f32 rowHigh = maxX;
            for (u32 e = 0; e < face.count; ++e) {
                const f32 rest = -face.half[e] - face.b[e] * cy - face.c[e];
                if (face.a[e] > 0.0f)
                    rowLow = std::max(rowLow, std::floor(rest / face.a[e] - 0.5f));
                else if (face.a[e] < 0.0f)
                    rowHigh = std::min(rowHigh, std::ceil(rest / face.a[e] - 0.5f));
                else if (rest > 0.0f)
                    rowHigh = rowLow - 1.0f;
            }
            for (f32 x = rowLow; x <= rowHigh; x += 1.0f) {
                const f32 cx = x + 0.5f;
                // Which edges the pixel straddles: none, and the face covers
                // it; one, and it covers it with the face across that edge.
                u32 straddled = 0;
                u32 edge = 0;
                bool touches = true;
                for (u32 e = 0; e < face.count && touches; ++e) {
                    const f32 value = face.a[e] * cx + face.b[e] * cy + face.c[e];
                    touches = value >= -face.half[e];
                    if (value < face.half[e]) {
                        ++straddled;
                        edge = e;
                    }
                }
                if (!touches || straddled > 1)
                    continue;
                f32 depth = farthest(face, cx, cy);
                if (straddled == 1) {
                    // **A pixel on an edge two faces share** is covered by the
                    // two only when it is wholly inside the other's other
                    // edges too, and then as far as the farther of them is
                    // over it. On the outline, or by a corner, it is nobody's:
                    // part of it is what is behind.
                    if (face.across[edge] < 0)
                        continue;
                    const Face& other = faces[static_cast<usize>(face.across[edge])];
                    bool inside = true;
                    for (u32 e = 0; e < other.count && inside; ++e) {
                        if (e != face.acrossEdge[edge])
                            inside = other.a[e] * cx + other.b[e] * cy + other.c[e] >= other.half[e];
                    }
                    if (!inside)
                        continue;
                    depth = std::max(depth, farthest(other, cx, cy));
                }
                if (depth == Far)
                    continue;
                f32& kept = depth_[static_cast<usize>(y) * Width + static_cast<usize>(x)];
                kept = std::min(kept, depth);
            }
        }
    }
}

void OcclusionCuller::drawCorners(std::span<const Face> faces, std::span<const Corner> corners, f32 bandLow,
                                  f32 bandHigh)
{
    for (const Corner& corner : corners) {
        if (corner.count == 0)
            continue;
        bool whole = true;
        for (u32 k = 0; k < corner.count && whole; ++k)
            whole = corner.faces[k] >= 0 && faces[static_cast<usize>(corner.faces[k])].valid;
        if (!whole)
            continue;
        const Face& first = faces[static_cast<usize>(corner.faces[0])];
        const ScreenPoint at = first.v[corner.slot[0]];
        // How near the fan's outer edges come: the lines of every edge of its
        // faces that does not touch the corner.
        f32 nearest = Far;
        for (u32 k = 0; k < corner.count; ++k) {
            const Face& face = faces[static_cast<usize>(corner.faces[k])];
            const u32 slot = corner.slot[k];
            for (u32 e = 0; e < face.count; ++e) {
                if (e == slot || e == (slot + face.count - 1) % face.count)
                    continue;
                const f32 length = std::sqrt(face.a[e] * face.a[e] + face.b[e] * face.b[e]);
                if (length <= 0.0f)
                    continue;
                nearest = std::min(nearest, (face.a[e] * at.x + face.b[e] * at.y + face.c[e]) / length);
            }
        }
        // A pixel's farthest point is half its diagonal from its middle.
        const f32 reach = nearest - 0.7072f;
        if (!(reach > 0.0f) || reach == Far)
            continue;
        const f32 minX = std::max(0.0f, std::floor(at.x - reach));
        const f32 maxX = std::min(static_cast<f32>(Width) - 1.0f, std::floor(at.x + reach));
        const f32 minY = std::max(bandLow, std::floor(at.y - reach));
        const f32 maxY = std::min(bandHigh, std::floor(at.y + reach));
        for (f32 y = minY; y <= maxY; y += 1.0f) {
            const f32 cy = y + 0.5f;
            for (f32 x = minX; x <= maxX; x += 1.0f) {
                const f32 cx = x + 0.5f;
                if ((cx - at.x) * (cx - at.x) + (cy - at.y) * (cy - at.y) > reach * reach)
                    continue;
                f32 depth = 0.0f;
                for (u32 k = 0; k < corner.count; ++k)
                    depth = std::max(depth, farthest(faces[static_cast<usize>(corner.faces[k])], cx, cy));
                if (depth == Far)
                    continue;
                f32& kept = depth_[static_cast<usize>(y) * Width + static_cast<usize>(x)];
                kept = std::min(kept, depth);
            }
        }
    }
}

void OcclusionCuller::drawBox(const Mat4& viewProjection, const Mat4& box, f32 nearPlane)
{
    // The unit cube's corners, and its six faces, each wound outward.
    static constexpr std::array<Vec3, 8> Corners{{{-0.5f, -0.5f, -0.5f},
                                                  {0.5f, -0.5f, -0.5f},
                                                  {0.5f, 0.5f, -0.5f},
                                                  {-0.5f, 0.5f, -0.5f},
                                                  {-0.5f, -0.5f, 0.5f},
                                                  {0.5f, -0.5f, 0.5f},
                                                  {0.5f, 0.5f, 0.5f},
                                                  {-0.5f, 0.5f, 0.5f}}};
    static constexpr std::array<std::array<u32, 4>, 6> Faces{
        {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}}};
    // The face across each edge of each face, and which of its edges that
    // is, found once.
    struct Adjacency
    {
        std::array<std::array<u32, 4>, 6> face{};
        std::array<std::array<u32, 4>, 6> edge{};
    };
    static const Adjacency Across = [] {
        Adjacency across;
        for (usize face = 0; face < Faces.size(); ++face) {
            for (usize e = 0; e < 4; ++e) {
                const u32 a = Faces[face][e];
                const u32 b = Faces[face][(e + 1) % 4];
                for (usize other = 0; other < Faces.size(); ++other) {
                    for (usize oe = 0; oe < 4 && other != face; ++oe) {
                        if (Faces[other][oe] == b && Faces[other][(oe + 1) % 4] == a) {
                            across.face[face][e] = static_cast<u32>(other);
                            across.edge[face][e] = static_cast<u32>(oe);
                        }
                    }
                }
            }
        }
        return across;
    }();

    std::array<Vec3, 8> placed{};
    std::array<Clip, 8> clip{};
    Vec3 middle{};
    for (usize corner = 0; corner < Corners.size(); ++corner) {
        placed[corner] = core::transformPoint(box, Corners[corner]);
        clip[corner] = toClip(viewProjection, placed[corner]);
        middle = middle + placed[corner] * 0.125f;
    }
    // Which faces are drawn: those that turn toward the camera -- at the
    // origin of this space -- whatever way the box is mirrored (its face's
    // middle is further out from the box's middle toward the camera), and
    // that do not reach the near plane. An occluder drawn with less of itself
    // hides less, never more.
    std::array<Face, 6> faces{};
    for (usize face = 0; face < Faces.size(); ++face) {
        Vec3 centre{};
        bool near = false;
        for (const u32 corner : Faces[face]) {
            centre = centre + placed[corner] * 0.25f;
            near = near || clip[corner].w <= nearPlane;
        }
        Face& f = faces[face];
        f.count = 4;
        if (near || core::dot(centre - middle, Vec3{} - centre) <= 0.0f)
            continue;
        for (usize i = 0; i < 4; ++i)
            f.v[i] = toScreen(clip[Faces[face][i]]);
        f.valid = setUpFace(f);
    }
    // **The outline is where no drawn face carries on.** Into the frame's
    // faces, numbered from where its faces start there.
    const auto base = static_cast<core::i32>(frameFaces_.size());
    for (usize face = 0; face < Faces.size(); ++face) {
        for (usize e = 0; e < 4; ++e) {
            const u32 other = Across.face[face][e];
            faces[face].across[e] = faces[other].valid ? base + static_cast<core::i32>(other) : -1;
            faces[face].acrossEdge[e] = Across.edge[face][e];
        }
    }
    frameFaces_.insert(frameFaces_.end(), faces.begin(), faces.end());
    static const std::array<Corner, 8> BoxCorners = [] {
        std::array<Corner, 8> corners{};
        for (usize face = 0; face < Faces.size(); ++face) {
            for (u32 slot = 0; slot < 4; ++slot) {
                Corner& corner = corners[Faces[face][slot]];
                corner.faces[corner.count] = static_cast<core::i32>(face);
                corner.slot[corner.count] = slot;
                ++corner.count;
            }
        }
        return corners;
    }();
    for (Corner corner : BoxCorners) {
        for (u32 k = 0; k < corner.count; ++k)
            corner.faces[k] = faces[static_cast<usize>(corner.faces[k])].valid ? base + corner.faces[k] : -1;
        frameCorners_.push_back(corner);
    }
}

void OcclusionCuller::drawGrid(const Mat4& viewProjection, const OccluderGrid& grid, f32 nearPlane)
{
    const u32 columns = grid.columns;
    const u32 rows = grid.rows;
    if (columns == 0 || rows == 0 || grid.heights.size() != static_cast<usize>(columns + 1) * (rows + 1) ||
        grid.solid.size() != static_cast<usize>(columns) * rows)
        return;
    const usize stride = columns + 1;
    std::vector<Vec3>& placed = gridPlaced_;
    std::vector<Clip>& clip = gridClip_;
    placed.resize(grid.heights.size());
    clip.resize(grid.heights.size());
    for (u32 j = 0; j <= rows; ++j) {
        for (u32 i = 0; i <= columns; ++i) {
            const usize at = j * stride + i;
            placed[at] =
                grid.origin + Vec3{static_cast<f32>(i) * grid.tile, grid.heights[at], static_cast<f32>(j) * grid.tile};
            clip[at] = toClip(viewProjection, placed[at]);
        }
    }
    // Each tile is two triangles, (a, b, c) and (a, c, d), with a at (i, j),
    // b at (i + 1, j), c at (i + 1, j + 1) and d at (i, j + 1). One is drawn
    // when its tile is solid ground, it faces the camera -- from above -- and
    // it does not reach the near plane.
    // Only the triangles drawn are made, and found through an index a
    // triangle: a grid's worth of faces cleared every frame was most of what
    // it cost.
    std::vector<Face>& faces = frameFaces_;
    std::vector<core::i32>& index = gridIndex_;
    index.assign(static_cast<usize>(columns) * rows * 2, -1);
    const auto cornerOf = [&](u32 i, u32 j) { return j * stride + i; };
    // Wholly off one side of the picture: never drawn.
    const auto offScreen = [&](const std::array<usize, 3>& corner) {
        const auto all = [&](auto&& outside) {
            return outside(clip[corner[0]]) && outside(clip[corner[1]]) && outside(clip[corner[2]]);
        };
        return all([](const Clip& c) { return c.x > c.w; }) || all([](const Clip& c) { return c.x < -c.w; }) ||
               all([](const Clip& c) { return c.y > c.w; }) || all([](const Clip& c) { return c.y < -c.w; });
    };
    for (u32 j = 0; j < rows; ++j) {
        for (u32 i = 0; i < columns; ++i) {
            if (grid.solid[static_cast<usize>(j) * columns + i] == 0)
                continue;
            const usize a = cornerOf(i, j);
            const usize b = cornerOf(i + 1, j);
            const usize c = cornerOf(i + 1, j + 1);
            const usize d = cornerOf(i, j + 1);
            for (u32 half = 0; half < 2; ++half) {
                const std::array<usize, 3> corner =
                    half == 0 ? std::array<usize, 3>{a, b, c} : std::array<usize, 3>{a, c, d};
                if (clip[corner[0]].w <= nearPlane || clip[corner[1]].w <= nearPlane || clip[corner[2]].w <= nearPlane)
                    continue;
                Vec3 normal = core::cross(placed[corner[1]] - placed[corner[0]], placed[corner[2]] - placed[corner[0]]);
                if (normal.y < 0.0f)
                    normal = normal * -1.0f;
                if (core::dot(normal, Vec3{} - placed[corner[0]]) <= 0.0f || offScreen(corner))
                    continue;
                Face f;
                f.count = 3;
                for (usize k = 0; k < 3; ++k)
                    f.v[k] = toScreen(clip[corner[k]]);
                f.valid = setUpFace(f);
                if (!f.valid)
                    continue;
                index[(static_cast<usize>(j) * columns + i) * 2 + half] = static_cast<core::i32>(faces.size());
                faces.push_back(f);
            }
        }
    }
    // The triangle across each edge, in the order of the corners, and which
    // of its edges that is: (a, b) is the tile before's (c, d), (b, c) the
    // tile right's (d, a), (c, a) this tile's other (a, c), and the same
    // round the other way.
    const auto indexOf = [&](i64 i, i64 j, u32 half) -> core::i32 {
        if (i < 0 || j < 0 || i >= static_cast<i64>(columns) || j >= static_cast<i64>(rows))
            return -1;
        return index[(static_cast<usize>(j) * columns + static_cast<usize>(i)) * 2 + half];
    };
    for (u32 j = 0; j < rows; ++j) {
        for (u32 i = 0; i < columns; ++i) {
            const i64 x = i;
            const i64 z = j;
            if (const core::i32 at = indexOf(x, z, 0); at >= 0) {
                Face& first = faces[static_cast<usize>(at)];
                first.across = {indexOf(x, z - 1, 1), indexOf(x + 1, z, 1), indexOf(x, z, 1), -1};
                first.acrossEdge = {1, 2, 0, 0};
            }
            if (const core::i32 at = indexOf(x, z, 1); at >= 0) {
                Face& second = faces[static_cast<usize>(at)];
                second.across = {indexOf(x, z, 0), indexOf(x, z + 1, 0), indexOf(x - 1, z, 0), -1};
                second.acrossEdge = {2, 0, 1, 0};
            }
        }
    }
    // Corner (i, j) is a of both of tile (i, j), b of the first of tile
    // (i - 1, j), c of both of tile (i - 1, j - 1) and d of the second of
    // tile (i, j - 1).
    std::vector<Corner>& corners = frameCorners_;
    for (u32 j = 1; j < rows; ++j) {
        for (u32 i = 1; i < columns; ++i) {
            Corner corner;
            corner.faces = {indexOf(i, j, 0),         indexOf(i, j, 1),         indexOf(i - 1, j, 0),
                            indexOf(i - 1, j - 1, 0), indexOf(i - 1, j - 1, 1), indexOf(i, j - 1, 1)};
            corner.slot = {0, 0, 1, 2, 1, 2};
            corner.count = 6;
            if (std::all_of(corner.faces.begin(), corner.faces.end(), [](core::i32 face) { return face >= 0; }))
                corners.push_back(corner);
        }
    }
}

void makeGroundGrid(std::span<const GroundTile> tiles, u32 columns, u32 rows, OccluderGrid& out)
{
    out.columns = columns;
    out.rows = rows;
    out.heights.assign(static_cast<usize>(columns + 1) * (rows + 1), 0.0f);
    out.solid.assign(static_cast<usize>(columns) * rows, 0);
    const usize stride = columns + 1;
    if (tiles.size() != stride * (rows + 1))
        return;
    const auto tile = [&](u32 i, u32 j) -> const GroundTile& { return tiles[j * stride + i]; };
    for (u32 j = 0; j <= rows; ++j) {
        for (u32 i = 0; i <= columns; ++i) {
            f32 lowest = Far;
            for (u32 dj = 0; dj < 2; ++dj) {
                for (u32 di = 0; di < 2; ++di) {
                    if ((di == 0 && i == 0) || (dj == 0 && j == 0))
                        continue;
                    const GroundTile& round = tile(i - 1 + di, j - 1 + dj);
                    if (round.solid)
                        lowest = std::min(lowest, round.top);
                }
            }
            out.heights[j * stride + i] = lowest == Far ? 0.0f : lowest;
        }
    }
    for (u32 j = 0; j < rows; ++j) {
        for (u32 i = 0; i < columns; ++i) {
            f32 bottom = -Far;
            bool solid = true;
            for (u32 dj = 0; dj < 2 && solid; ++dj) {
                for (u32 di = 0; di < 2 && solid; ++di) {
                    const GroundTile& round = tile(i + di, j + dj);
                    solid = round.solid;
                    bottom = std::max(bottom, round.bottom);
                }
            }
            for (u32 dj = 0; dj < 2 && solid; ++dj) {
                for (u32 di = 0; di < 2 && solid; ++di)
                    solid = out.heights[(j + dj) * stride + i + di] >= bottom;
            }
            out.solid[static_cast<usize>(j) * columns + i] = solid ? 1 : 0;
        }
    }
}

void OcclusionCuller::buildPyramid()
{
    pyramid_.clear();
    u32 width = Width;
    u32 height = Height;
    const std::vector<f32>* below = &depth_;
    while (width > 1 || height > 1) {
        const u32 nextWidth = std::max(width / 2, 1u);
        const u32 nextHeight = std::max(height / 2, 1u);
        std::vector<f32> level(static_cast<usize>(nextWidth) * nextHeight, 0.0f);
        for (u32 y = 0; y < nextHeight; ++y) {
            for (u32 x = 0; x < nextWidth; ++x) {
                f32 farthest = 0.0f;
                for (u32 dy = 0; dy < 2; ++dy) {
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 sx = std::min(x * 2 + dx, width - 1);
                        const u32 sy = std::min(y * 2 + dy, height - 1);
                        farthest = std::max(farthest, (*below)[static_cast<usize>(sy) * width + sx]);
                    }
                }
                level[static_cast<usize>(y) * nextWidth + x] = farthest;
            }
        }
        pyramid_.push_back(std::move(level));
        below = &pyramid_.back();
        width = nextWidth;
        height = nextHeight;
    }
}

void OcclusionCuller::rasterize()
{
    // **A band of rows a job**: every face is offered to every band, and each
    // writes only its own rows -- the nearest depth whatever order they ran.
    jobs::parallelFor("render.occlusion.raster", jobs::Domain::Render, 0, Height, RowsPerBand,
                      [this](usize first, usize last, u32) noexcept {
                          const f32 low = static_cast<f32>(first);
                          const f32 high = static_cast<f32>(last) - 1.0f;
                          drawFaces(frameFaces_, low, high);
                          drawCorners(frameFaces_, frameCorners_, low, high);
                      });
}

void OcclusionCuller::drawOccluders(const Mat4& viewProjection, std::span<const Mat4> boxes,
                                    std::span<const OccluderGrid> grids, f32 nearPlane)
{
    clear();
    for (const OccluderGrid& grid : grids)
        drawGrid(viewProjection, grid, nearPlane);
    for (const Mat4& box : boxes)
        drawBox(viewProjection, box, nearPlane);
    rasterize();
    buildPyramid();
}

bool OcclusionCuller::hides(const Mat4& viewProjection, Vec3 centre, f32 radius, f32 nearPlane) const
{
    if (depth_.empty())
        return false;
    const Clip middle = toClip(viewProjection, centre);
    const f32 nearest = middle.w - radius;
    if (nearest <= nearPlane)
        return false;
    // The sphere's box on screen, from its eight corners.
    f32 minX = Far;
    f32 minY = Far;
    f32 maxX = -Far;
    f32 maxY = -Far;
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec3 at{centre.x + ((corner & 1) != 0 ? radius : -radius),
                      centre.y + ((corner & 2) != 0 ? radius : -radius),
                      centre.z + ((corner & 4) != 0 ? radius : -radius)};
        const Clip clip = toClip(viewProjection, at);
        if (clip.w <= nearPlane)
            return false;
        const ScreenPoint screen = toScreen(clip);
        minX = std::min(minX, screen.x);
        minY = std::min(minY, screen.y);
        maxX = std::max(maxX, screen.x);
        maxY = std::max(maxY, screen.y);
    }
    // Off the buffer's edge, it may be seen there: drawn.
    if (minX < 0.0f || minY < 0.0f || maxX > static_cast<f32>(Width) || maxY > static_cast<f32>(Height))
        return false;
    // The level where the box is a few texels across, and every texel it
    // touches there.
    u32 level = 0;
    f32 span = std::max(maxX - minX, maxY - minY);
    while (span > 4.0f && level < pyramid_.size()) {
        span *= 0.5f;
        ++level;
    }
    const std::vector<f32>& texels = level == 0 ? depth_ : pyramid_[level - 1];
    const u32 width = std::max(Width >> level, 1u);
    const u32 height = std::max(Height >> level, 1u);
    const f32 scale = 1.0f / static_cast<f32>(1u << level);
    const u32 x0 = static_cast<u32>(std::floor(minX * scale));
    const u32 y0 = static_cast<u32>(std::floor(minY * scale));
    const u32 x1 = std::min(static_cast<u32>(std::floor(maxX * scale)), width - 1);
    const u32 y1 = std::min(static_cast<u32>(std::floor(maxY * scale)), height - 1);
    for (u32 y = y0; y <= y1; ++y) {
        for (u32 x = x0; x <= x1; ++x) {
            if (texels[static_cast<usize>(y) * width + x] >= nearest)
                return false;
        }
    }
    return true;
}

OcclusionStats OcclusionCuller::cull(RenderWorld& world, f64 perDrawMs, f64 nowSeconds)
{
    OcclusionStats stats;
    const RenderCamera& camera = world.camera;
    if (!perspective(camera.projection) || (world.occluderBoxes.empty() && world.occluderGrids.empty()))
        return stats;

    // **Too little to save**: a handful of draws in view never pays for the
    // occluders drawn to hide them.
    u32 inView = 0;
    for (const DrawItem& draw : world.draws) {
        if (draw.inCameraFrustum && draw.boundsRadius > 0.0f && ++inView >= FewestDraws)
            break;
    }
    if (inView < FewestDraws && !alwaysTest_) {
        stats.resting = true;
        return stats;
    }

    // **Resting**: one frame a second is tried, and wakes it if it pays.
    if (alwaysTest_)
        resting_ = false;
    const bool probing = resting_;
    if (resting_ && nowSeconds - lastProbe_ < ProbeSeconds) {
        stats.resting = true;
        return stats;
    }

    const auto started = std::chrono::steady_clock::now();
    const Mat4& viewProjection = camera.unjitteredViewProjection;
    const f32 nearPlane = camera.nearPlane;

    // The biggest on screen first, up to the most a frame draws.
    const f32 focal = camera.projection.m[1][1] * 0.5f * static_cast<f32>(Height);
    struct Pick
    {
        f32 size;
        usize index;
    };
    std::vector<Pick> picks;
    picks.reserve(world.occluderBoxes.size());
    for (usize index = 0; index < world.occluderBoxes.size(); ++index) {
        const OccluderBox& box = world.occluderBoxes[index];
        const Clip middle = toClip(viewProjection, box.centre);
        if (middle.w <= nearPlane)
            continue;
        const f32 size = box.radius * focal / middle.w * 2.0f;
        if (size >= SmallestOccluderPixels)
            picks.push_back({size, index});
    }
    std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.size > b.size; });
    if (picks.size() > MostOccluders)
        picks.resize(MostOccluders);

    clear();
    // The ground first: it is under everything, and most of what a valley
    // hides.
    for (const OccluderGrid& grid : world.occluderGrids)
        drawGrid(viewProjection, grid, nearPlane);
    for (const Pick& pick : picks)
        drawBox(viewProjection, world.occluderBoxes[pick.index].box, nearPlane);
    rasterize();
    buildPyramid();
    stats.occluders = static_cast<u32>(picks.size() + world.occluderGrids.size());

    // **A run of draws a job**: each asks the buffer alone and changes only
    // its own draws, and the runs' counts are added in their order.
    const usize draws = world.draws.size();
    const u32 runs = jobs::rangeCount(0, draws, DrawsPerJob);
    runCounts_.assign(static_cast<usize>(runs) * 2, 0);
    jobs::parallelFor("render.occlusion.test", jobs::Domain::Render, 0, draws, DrawsPerJob,
                      [&](usize first, usize last, u32 run) noexcept {
                          u32 candidates = 0;
                          u32 hidden = 0;
                          for (usize at = first; at < last; ++at) {
                              DrawItem& draw = world.draws[at];
                              if (!draw.inCameraFrustum || draw.boundsRadius <= 0.0f)
                                  continue;
                              ++candidates;
                              if (hides(viewProjection, draw.boundsCenter, draw.boundsRadius, nearPlane)) {
                                  draw.inCameraFrustum = false;
                                  ++hidden;
                              }
                          }
                          runCounts_[static_cast<usize>(run) * 2] = candidates;
                          runCounts_[static_cast<usize>(run) * 2 + 1] = hidden;
                      });
    for (u32 run = 0; run < runs; ++run) {
        stats.candidates += runCounts_[static_cast<usize>(run) * 2];
        stats.hidden += runCounts_[static_cast<usize>(run) * 2 + 1];
    }

    stats.tested = true;
    stats.ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    // **Does it pay?** What it hid, at what a draw cost to record, against its
    // own time -- or a tenth of what was in view hidden: the time a hidden
    // draw saves the GPU, which the record time does not count. A street of
    // houses drawn in a few instanced calls saves little recording and five
    // sixths of its triangles. Losing `FramesBeforeRest` frames in a row, it
    // rests; a frame tried while resting that pays wakes it.
    const f64 drawMs = perDrawMs > 0.0 ? perDrawMs : 0.003;
    const bool pays = static_cast<f64>(stats.hidden) * drawMs >= stats.ms ||
                      static_cast<u64>(stats.hidden) * HiddenShareToPay >= stats.candidates;
    if (probing) {
        lastProbe_ = nowSeconds;
        if (pays) {
            resting_ = false;
            losingFrames_ = 0;
        }
        return stats;
    }
    losingFrames_ = pays ? 0 : losingFrames_ + 1;
    if (losingFrames_ >= FramesBeforeRest && !alwaysTest_) {
        resting_ = true;
        lastProbe_ = nowSeconds;
    }
    return stats;
}

} // namespace engine::render
