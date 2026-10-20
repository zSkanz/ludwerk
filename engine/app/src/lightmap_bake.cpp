#include "engine/app/lightmap_bake.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/jobs/jobs.h"
#include "engine/platform/platform.h"

namespace engine::app {

using core::Color3;
using core::DVec3;
using core::f32;
using core::f64;
using core::u32;
using core::u64;
using core::u8;
using core::usize;
using core::Vec2;
using core::Vec3;

namespace {

constexpr f32 kPi = 3.14159265358979f;
// How far off its surface a texel's rays begin, as a volume's do.
constexpr f32 kOffSurface = 0.02f;
// Rays a texel casts at the sun for its mask, and the sun's own radius as
// it is seen from here, in radians.
constexpr u32 kMaskRays = 16;
constexpr f32 kSunRadius = 0.0047f;
// The cells a pass's light is kept by for the next to read, in metres.
constexpr f32 kCellSize = 0.25f;
// Texels a piece of work, between two looks at the clock.
constexpr usize kChunk = 2048;
constexpr usize kGrain = 32;

[[nodiscard]] Color3 operator*(Color3 a, f32 b) noexcept
{
    return Color3{a.r * b, a.g * b, a.b * b};
}

[[nodiscard]] Color3 operator*(Color3 a, Color3 b) noexcept
{
    return Color3{a.r * b.r, a.g * b.g, a.b * b.b};
}

[[nodiscard]] Color3 operator+(Color3 a, Color3 b) noexcept
{
    return Color3{a.r + b.r, a.g + b.g, a.b + b.b};
}

[[nodiscard]] Vec3 scaled(Vec3 v, f32 by) noexcept
{
    return Vec3{v.x * by, v.y * by, v.z * by};
}

[[nodiscard]] Vec3 added(Vec3 a, Vec3 b) noexcept
{
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Vec3 taken(Vec3 a, Vec3 b) noexcept
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

// Two directions square to `normal` and to each other (Duff and others'
// branchless basis), so a set of directions about +z can be turned to face
// any way.
void basisOf(Vec3 normal, Vec3& tangent, Vec3& bitangent) noexcept
{
    const f32 sign = std::copysign(1.0f, normal.z);
    const f32 a = -1.0f / (sign + normal.z);
    const f32 b = normal.x * normal.y * a;
    tangent = Vec3{1.0f + sign * normal.x * normal.x * a, sign * b, -sign * normal.x};
    bitangent = Vec3{b, sign + normal.y * normal.y * a, -normal.y};
}

// A number from a texel's place in the list, spread over [0, 1): what turns
// its set of rays, so neighbouring texels do not all look the same ways.
[[nodiscard]] f32 spread(u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    value ^= value >> 31;
    return static_cast<f32>(value >> 40) / static_cast<f32>(1ull << 24);
}

// The `index`th of `count` directions over the hemisphere about +z, as many
// where it is lit squarely as a cosine asks and fewer towards the horizon:
// points spread evenly over a disc, lifted onto the hemisphere above it.
// Their mean light is then what reaches a surface, over pi.
[[nodiscard]] Vec3 hemisphereDirection(u32 index, u32 count, f32 turn) noexcept
{
    const f32 radius = std::sqrt((static_cast<f32>(index) + 0.5f) / static_cast<f32>(count));
    const f32 angle = static_cast<f32>(index) * 2.39996323f + turn * 2.0f * kPi;
    return Vec3{radius * std::cos(angle), radius * std::sin(angle), std::sqrt(std::max(1.0f - radius * radius, 0.0f))};
}

[[nodiscard]] Vec3 turned(const core::CFrameD& place, Vec3 v) noexcept
{
    return core::transformDirection(place, v);
}

} // namespace

f32 surfaceArea(std::span<const Vec3> positions, std::span<const u32> indices, Vec3 stretch) noexcept
{
    f64 area = 0.0;
    for (usize index = 0; index + 2 < indices.size(); index += 3) {
        if (indices[index] >= positions.size() || indices[index + 1] >= positions.size() ||
            indices[index + 2] >= positions.size())
            continue;
        const auto at = [&](u32 vertex) {
            const Vec3 p = positions[vertex];
            return Vec3{p.x * stretch.x, p.y * stretch.y, p.z * stretch.z};
        };
        const Vec3 a = at(indices[index]);
        const Vec3 crossed = core::cross(taken(at(indices[index + 1]), a), taken(at(indices[index + 2]), a));
        area += 0.5 * static_cast<f64>(core::length(crossed));
    }
    return static_cast<f32>(area);
}

std::vector<Vec2> chartSizes(std::span<const Vec3> positions, std::span<const Vec2> uvs, std::span<const u32> indices,
                             Vec3 stretch, u32 columns, u32 rows)
{
    const u32 cells = columns == 0 ? 1 : columns * rows;
    // Each chart's world length per unit of its own coordinates, across and
    // down, weighed by how much of the chart each triangle is.
    std::vector<f64> across(cells, 0.0);
    std::vector<f64> down(cells, 0.0);
    std::vector<f64> weights(cells, 0.0);
    std::vector<f64> areas(cells, 0.0);
    const f32 cellWidth = columns == 0 ? 1.0f : 1.0f / static_cast<f32>(columns);
    const f32 cellHeight = columns == 0 ? 1.0f : 1.0f / static_cast<f32>(rows);
    for (usize at = 0; at + 2 < indices.size(); at += 3) {
        const u32 i0 = indices[at];
        const u32 i1 = indices[at + 1];
        const u32 i2 = indices[at + 2];
        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size() || i0 >= uvs.size() ||
            i1 >= uvs.size() || i2 >= uvs.size())
            continue;
        const auto world = [&](u32 vertex) {
            const Vec3 p = positions[vertex];
            return Vec3{p.x * stretch.x, p.y * stretch.y, p.z * stretch.z};
        };
        const Vec3 pa = world(i0);
        const Vec3 e1 = taken(world(i1), pa);
        const Vec3 e2 = taken(world(i2), pa);
        // In the chart's own coordinates: its cell as the unit square.
        const Vec2 a = uvs[i0];
        const f32 u1 = (uvs[i1].x - a.x) / cellWidth;
        const f32 v1 = (uvs[i1].y - a.y) / cellHeight;
        const f32 u2 = (uvs[i2].x - a.x) / cellWidth;
        const f32 v2 = (uvs[i2].y - a.y) / cellHeight;
        const f32 det = u1 * v2 - u2 * v1;
        const f32 area = 0.5f * core::length(core::cross(e1, e2));
        if (std::fabs(det) < 1.0e-12f || area <= 0.0f)
            continue;
        u32 cell = 0;
        if (columns != 0) {
            const f32 cu = (uvs[i0].x + uvs[i1].x + uvs[i2].x) / 3.0f;
            const f32 cv = (uvs[i0].y + uvs[i1].y + uvs[i2].y) / 3.0f;
            const u32 column = std::min(columns - 1, static_cast<u32>(std::max(0.0f, cu / cellWidth)));
            const u32 row = std::min(rows - 1, static_cast<u32>(std::max(0.0f, cv / cellHeight)));
            cell = row * columns + column;
        }
        const Vec3 alongU = scaled(taken(scaled(e1, v2), scaled(e2, v1)), 1.0f / det);
        const Vec3 alongV = scaled(taken(scaled(e2, u1), scaled(e1, u2)), 1.0f / det);
        across[cell] += static_cast<f64>(core::length(alongU) * area);
        down[cell] += static_cast<f64>(core::length(alongV) * area);
        weights[cell] += static_cast<f64>(area);
        areas[cell] += static_cast<f64>(area);
    }
    std::vector<Vec2> sizes(cells, Vec2{0.0f, 0.0f});
    for (u32 cell = 0; cell < cells; ++cell) {
        if (weights[cell] <= 0.0)
            continue;
        if (columns == 0) {
            const auto side = static_cast<f32>(std::sqrt(areas[cell]));
            sizes[cell] = Vec2{side, side};
        }
        else {
            sizes[cell] =
                Vec2{static_cast<f32>(across[cell] / weights[cell]), static_cast<f32>(down[cell] / weights[cell])};
        }
    }
    return sizes;
}

LightmapLayout layOutLightmaps(std::span<const Vec2> sizes, std::span<const f32> scales, f32 density, u32 pageSize,
                               u32 maxPages)
{
    LightmapLayout layout;
    layout.density = density;
    const usize count = sizes.size();
    if (count == 0 || pageSize <= 2 * kLightmapPadding)
        return layout;
    const u32 largest = pageSize - 2 * kLightmapPadding;
    const auto texels = [&](f32 metres, f32 scale) {
        const f32 wanted = std::ceil(std::max(metres, 0.0f) * layout.density * scale);
        return std::clamp(static_cast<u32>(std::min(wanted, 1.0e6f)), 4u, largest);
    };
    for (;;) {
        std::vector<u32> widths(count);
        std::vector<u32> heights(count);
        for (usize index = 0; index < count; ++index) {
            const f32 scale = index < scales.size() && scales[index] > 0.0f ? scales[index] : 1.0f;
            widths[index] = texels(sizes[index].x, scale);
            heights[index] = texels(sizes[index].y, scale);
        }
        std::vector<usize> order(count);
        std::iota(order.begin(), order.end(), usize{0});
        std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return heights[a] > heights[b]; });

        layout.rects.assign(count, LightmapRect{});
        u32 page = 0;
        u32 x = kLightmapPadding;
        u32 y = kLightmapPadding;
        u32 row = 0;
        for (const usize index : order) {
            const u32 width = widths[index];
            const u32 height = heights[index];
            if (x + width + kLightmapPadding > pageSize) {
                x = kLightmapPadding;
                y += row + kLightmapPadding;
                row = 0;
            }
            if (y + height + kLightmapPadding > pageSize) {
                ++page;
                x = kLightmapPadding;
                y = kLightmapPadding;
                row = 0;
            }
            layout.rects[index] = LightmapRect{page, x, y, width, height};
            x += width + kLightmapPadding;
            row = std::max(row, height);
        }
        layout.pages = page + 1;
        // At the smallest rectangles there is nothing left to lower.
        bool smallest = true;
        for (usize index = 0; index < count; ++index)
            smallest = smallest && widths[index] <= 4u && heights[index] <= 4u;
        if (layout.pages <= maxPages || maxPages == 0 || smallest)
            return layout;
        layout.density *= 0.85f;
        layout.lowered = true;
    }
}

void LightmapBake::begin(std::span<const LightmapSurface> surfaces, u32 pageSize, u32 pages, u32 bounces)
{
    *this = LightmapBake{};
    m_pageSize = pageSize;
    m_bounces = bounces;
    m_pages.resize(pages);
    m_owner.resize(pages);
    for (u32 page = 0; page < pages; ++page) {
        m_pages[page].size = pageSize;
        m_owner[page].assign(static_cast<usize>(pageSize) * pageSize, 0);
    }
    if (!surfaces.empty())
        m_origin = surfaces.front().place.position;
    for (u32 index = 0; index < surfaces.size(); ++index)
        rasterize(surfaces[index], index);
    m_texelCount = m_texels.size();
    m_indirect.assign(m_texels.size(), Color3{0.0f, 0.0f, 0.0f});
    m_inside.assign(m_texels.size(), 0);
    m_mask.assign(m_texels.size(), 0.0f);
    m_stage = Stage::Mask;
}

void LightmapBake::rasterize(const LightmapSurface& surface, u32 index)
{
    const u32 cells = surface.columns == 0 ? 1 : surface.columns * surface.rows;
    if (surface.rects.size() != cells)
        return;
    // Which texels of each rectangle a triangle has given a place already: a
    // texel centre on the edge two triangles share is the first one's.
    std::vector<std::vector<u8>> claimedByChart(cells);
    for (u32 cell = 0; cell < cells; ++cell) {
        const LightmapRect& rect = surface.rects[cell];
        if (rect.page >= m_pages.size() || rect.x + rect.width > m_pageSize || rect.y + rect.height > m_pageSize)
            continue;
        claimedByChart[cell].assign(static_cast<usize>(rect.width) * rect.height, 0);
        std::vector<u32>& owner = m_owner[rect.page];
        for (u32 y = rect.y; y < rect.y + rect.height; ++y) {
            for (u32 x = rect.x; x < rect.x + rect.width; ++x)
                owner[static_cast<usize>(y) * m_pageSize + x] = index + 1;
        }
    }
    const f32 cellWidth = surface.columns == 0 ? 1.0f : 1.0f / static_cast<f32>(surface.columns);
    const f32 cellHeight = surface.columns == 0 ? 1.0f : 1.0f / static_cast<f32>(surface.rows);
    const f32 inner = 1.0f - 2.0f * surface.margin;
    const auto world = [&](Vec3 local) {
        return Vec3{local.x * surface.stretch.x, local.y * surface.stretch.y, local.z * surface.stretch.z};
    };
    const bool smooth = surface.normals.size() == surface.positions.size();
    for (usize at = 0; at + 2 < surface.indices.size(); at += 3) {
        const u32 i0 = surface.indices[at];
        const u32 i1 = surface.indices[at + 1];
        const u32 i2 = surface.indices[at + 2];
        if (i0 >= surface.positions.size() || i1 >= surface.positions.size() || i2 >= surface.positions.size() ||
            i0 >= surface.uvs.size() || i1 >= surface.uvs.size() || i2 >= surface.uvs.size())
            continue;
        // Its chart, by where its middle is.
        u32 cell = 0;
        if (surface.columns != 0) {
            const f32 cu = (surface.uvs[i0].x + surface.uvs[i1].x + surface.uvs[i2].x) / 3.0f;
            const f32 cv = (surface.uvs[i0].y + surface.uvs[i1].y + surface.uvs[i2].y) / 3.0f;
            const u32 column = std::min(surface.columns - 1, static_cast<u32>(std::max(0.0f, cu / cellWidth)));
            const u32 row = std::min(surface.rows - 1, static_cast<u32>(std::max(0.0f, cv / cellHeight)));
            cell = row * surface.columns + column;
        }
        if (claimedByChart[cell].empty())
            continue;
        const LightmapRect& rect = surface.rects[cell];
        std::vector<u8>& claimedTexels = claimedByChart[cell];
        const f32 column = surface.columns == 0 ? 0.0f : static_cast<f32>(cell % surface.columns);
        const f32 row = surface.columns == 0 ? 0.0f : static_cast<f32>(cell / surface.columns);
        // The triangle in the rectangle's texels -- its chart's margin taken
        // off, so the chart fills it -- and in the world.
        const auto inRect = [&](Vec2 uv) {
            const f32 u = (uv.x / cellWidth - column - surface.margin) / inner;
            const f32 v = (uv.y / cellHeight - row - surface.margin) / inner;
            return Vec2{u * static_cast<f32>(rect.width), v * static_cast<f32>(rect.height)};
        };
        const Vec2 a = inRect(surface.uvs[i0]);
        const Vec2 b = inRect(surface.uvs[i1]);
        const Vec2 c = inRect(surface.uvs[i2]);
        const f32 area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (std::fabs(area) < 1.0e-12f)
            continue;
        const Vec3 wa = turned(surface.place, world(surface.positions[i0]));
        const Vec3 wb = turned(surface.place, world(surface.positions[i1]));
        const Vec3 wc = turned(surface.place, world(surface.positions[i2]));
        const Vec3 face = core::normalize(core::cross(taken(wb, wa), taken(wc, wa)));
        // The world's step for one texel along each of the page's axes.
        const Vec3 along =
            scaled(taken(scaled(taken(wb, wa), c.y - a.y), scaled(taken(wc, wa), b.y - a.y)), 1.0f / area);
        const Vec3 downward =
            scaled(taken(scaled(taken(wc, wa), b.x - a.x), scaled(taken(wb, wa), c.x - a.x)), 1.0f / area);
        const auto normalAt = [&](u32 vertex) {
            const Vec3 n = surface.normals[vertex];
            const Vec3 across{n.x / surface.stretch.x, n.y / surface.stretch.y, n.z / surface.stretch.z};
            return turned(surface.place, across);
        };
        const u32 x0 = static_cast<u32>(std::max(0.0f, std::floor(std::min({a.x, b.x, c.x}))));
        const u32 y0 = static_cast<u32>(std::max(0.0f, std::floor(std::min({a.y, b.y, c.y}))));
        const u32 x1 = std::min(rect.width - 1, static_cast<u32>(std::max(0.0f, std::ceil(std::max({a.x, b.x, c.x})))));
        const u32 y1 =
            std::min(rect.height - 1, static_cast<u32>(std::max(0.0f, std::ceil(std::max({a.y, b.y, c.y})))));
        for (u32 y = y0; y <= y1; ++y) {
            for (u32 x = x0; x <= x1; ++x) {
                u8& claimed = claimedTexels[static_cast<usize>(y) * rect.width + x];
                if (claimed != 0)
                    continue;
                const f32 px = static_cast<f32>(x) + 0.5f;
                const f32 py = static_cast<f32>(y) + 0.5f;
                const f32 w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
                const f32 w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
                const f32 w2 = 1.0f - w0 - w1;
                if (w0 < -1.0e-4f || w1 < -1.0e-4f || w2 < -1.0e-4f)
                    continue;
                claimed = 1;
                Texel texel;
                const Vec3 local = added(added(scaled(wa, w0), scaled(wb, w1)), scaled(wc, w2));
                texel.place = surface.place.position + core::toDVec3(local);
                Vec3 normal = face;
                if (smooth) {
                    normal = core::normalize(
                        added(added(scaled(normalAt(i0), w0), scaled(normalAt(i1), w1)), scaled(normalAt(i2), w2)));
                    // A normal that leans past its own face is the face's.
                    if (core::dot(normal, face) <= 0.0f)
                        normal = face;
                }
                texel.normal = normal;
                texel.across = along;
                texel.down = downward;
                texel.page = rect.page;
                texel.x = rect.x + x;
                texel.y = rect.y + y;
                texel.surface = index;
                m_texels.push_back(texel);
            }
        }
    }
}

void LightmapBake::maskTexel(usize index, const BakeTrace& trace)
{
    const Texel& texel = m_texels[index];
    const Vec3 sun = m_lit.sunDirection;
    if (core::dot(texel.normal, sun) <= 0.0f) {
        m_mask[index] = 0.0f;
        return;
    }
    Vec3 tangent;
    Vec3 bitangent;
    basisOf(sun, tangent, bitangent);
    u32 seen = 0;
    for (u32 ray = 0; ray < kMaskRays; ++ray) {
        // Across the texel on a four-by-four grid, kept off its very edges,
        // and towards a point of the sun's disc.
        const f32 u = (static_cast<f32>(ray % 4) + 0.5f) / 4.0f - 0.5f;
        const f32 v = (static_cast<f32>(ray / 4) + 0.5f) / 4.0f - 0.5f;
        const Vec3 offset = added(scaled(texel.across, u * 0.7f), scaled(texel.down, v * 0.7f));
        const Vec3 disc = hemisphereDirection(ray, kMaskRays, 0.0f);
        const Vec3 towards = core::normalize(
            added(sun, added(scaled(tangent, disc.x * kSunRadius), scaled(bitangent, disc.y * kSunRadius))));
        const DVec3 from = texel.place + core::toDVec3(added(offset, scaled(texel.normal, kOffSurface)));
        if (!trace(from, towards, kBakeReach).hit)
            ++seen;
    }
    m_mask[index] = static_cast<f32>(seen) / static_cast<f32>(kMaskRays);
}

void LightmapBake::gatherTexel(usize index, const BakeTrace& trace)
{
    if (m_inside[index] != 0)
        return;
    const Texel& texel = m_texels[index];
    Vec3 tangent;
    Vec3 bitangent;
    basisOf(texel.normal, tangent, bitangent);
    const DVec3 from = texel.place + core::toDVec3(scaled(texel.normal, kOffSurface));
    const f32 turn = spread(index);
    Color3 sum{0.0f, 0.0f, 0.0f};
    u32 inside = 0;
    for (u32 ray = 0; ray < kBakeRays; ++ray) {
        const Vec3 local = hemisphereDirection(ray, kBakeRays, turn);
        const Vec3 direction =
            added(added(scaled(tangent, local.x), scaled(bitangent, local.y)), scaled(texel.normal, local.z));
        const BakeHit met = trace(from, direction, kBakeReach);
        if (!met.hit) {
            if (m_lit.sky)
                sum = sum + m_lit.sky(direction);
            continue;
        }
        if (met.inside) {
            ++inside;
            continue;
        }
        const DVec3 place = met.position + core::toDVec3(scaled(met.normal, kOffSurface));
        Color3 reaching = bakeDirectLight(place, met.normal, m_lit, trace);
        if (m_pass > 0)
            reaching = reaching + around(met.position, met.normal);
        sum = sum + met.emissive + met.albedo * reaching;
    }
    // Most of what it sees is the inside of something: it is in a wall, and
    // takes its neighbours' light.
    if (inside * 2 >= kBakeRays) {
        m_inside[index] = 1;
        return;
    }
    m_indirect[index] = sum * (1.0f / static_cast<f32>(kBakeRays));
}

namespace {

[[nodiscard]] u64 cellKey(const DVec3& place, const DVec3& origin, Vec3 normal) noexcept
{
    const auto cell = [](f64 at) {
        const auto index = static_cast<core::i64>(std::floor(at / static_cast<f64>(kCellSize)));
        return static_cast<u64>(std::clamp<core::i64>(index + (1 << 19), 0, (1 << 20) - 1));
    };
    const DVec3 from = place - origin;
    // Which of the six ways the surface most faces: a wall's two sides are
    // two cells.
    const f32 ax = std::fabs(normal.x);
    const f32 ay = std::fabs(normal.y);
    const f32 az = std::fabs(normal.z);
    u64 facing = 0;
    if (ax >= ay && ax >= az)
        facing = normal.x >= 0.0f ? 0 : 1;
    else if (ay >= az)
        facing = normal.y >= 0.0f ? 2 : 3;
    else
        facing = normal.z >= 0.0f ? 4 : 5;
    return (cell(from.x) << 43) | (cell(from.y) << 23) | (cell(from.z) << 3) | facing;
}

} // namespace

Color3 LightmapBake::around(const DVec3& place, Vec3 normal) const noexcept
{
    const u64 key = cellKey(place, m_origin, normal);
    const auto found =
        std::lower_bound(m_cells.begin(), m_cells.end(), key, [](const Cell& cell, u64 k) { return cell.key < k; });
    if (found == m_cells.end() || found->key != key || found->count == 0)
        return Color3{0.0f, 0.0f, 0.0f};
    return found->light * (1.0f / static_cast<f32>(found->count));
}

void LightmapBake::passEnd()
{
    // What reached each texel this pass, kept by cell for the next pass's
    // rays to find where they meet a baked surface.
    std::vector<Cell> cells;
    cells.reserve(m_texels.size());
    for (usize index = 0; index < m_texels.size(); ++index) {
        if (m_inside[index] != 0)
            continue;
        const Texel& texel = m_texels[index];
        cells.push_back(Cell{cellKey(texel.place, m_origin, texel.normal), m_indirect[index], 1});
    }
    std::stable_sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) { return a.key < b.key; });
    m_cells.clear();
    for (const Cell& cell : cells) {
        if (!m_cells.empty() && m_cells.back().key == cell.key) {
            m_cells.back().light = m_cells.back().light + cell.light;
            m_cells.back().count += 1;
        }
        else {
            m_cells.push_back(cell);
        }
    }
}

void LightmapBake::finish()
{
    m_insideCount = static_cast<u32>(std::count(m_inside.begin(), m_inside.end(), core::u8{1}));
    const usize side = m_pageSize;
    // Each page's texels by place: one more than the texel's index.
    std::vector<std::vector<u32>> slots(m_pages.size(), std::vector<u32>(side * side, 0));
    for (usize index = 0; index < m_texels.size(); ++index) {
        const Texel& texel = m_texels[index];
        if (m_inside[index] == 0)
            slots[texel.page][static_cast<usize>(texel.y) * side + texel.x] = static_cast<u32>(index + 1);
    }

    for (usize page = 0; page < m_pages.size(); ++page) {
        LightmapPage& out = m_pages[page];
        out.light.assign(side * side * 4, 0.0f);
        out.mask.assign(side * side, 0.0f);
        const std::vector<u32>& slot = slots[page];

        // **A filter that keeps edges**: each texel takes its neighbours
        // within two texels that are the same surface facing the same way
        // and lying where it lies -- noise is evened out, and a corner's two
        // sides stay two.
        for (usize y = 0; y < side; ++y) {
            for (usize x = 0; x < side; ++x) {
                const u32 here = slot[y * side + x];
                if (here == 0)
                    continue;
                const Texel& texel = m_texels[here - 1];
                const f32 reach = 2.5f * std::max(core::length(texel.across), core::length(texel.down));
                Color3 sum{0.0f, 0.0f, 0.0f};
                f32 weights = 0.0f;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        const auto nx = static_cast<core::i64>(x) + dx;
                        const auto ny = static_cast<core::i64>(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<core::i64>(side) ||
                            ny >= static_cast<core::i64>(side))
                            continue;
                        const u32 there = slot[static_cast<usize>(ny) * side + static_cast<usize>(nx)];
                        if (there == 0)
                            continue;
                        const Texel& other = m_texels[there - 1];
                        if (other.surface != texel.surface || core::dot(other.normal, texel.normal) < 0.9f ||
                            core::length(core::toVec3(other.place - texel.place)) > reach)
                            continue;
                        const f32 weight = std::exp(-0.5f * static_cast<f32>(dx * dx + dy * dy) / 1.5f);
                        sum = sum + m_indirect[there - 1] * weight;
                        weights += weight;
                    }
                }
                const Color3 light = weights > 0.0f ? sum * (1.0f / weights) : m_indirect[here - 1];
                f32* at = &out.light[(y * side + x) * 4];
                at[0] = light.r;
                at[1] = light.g;
                at[2] = light.b;
                at[3] = 1.0f;
                out.mask[y * side + x] = m_mask[here - 1];
            }
        }

        // **The charts' borders filled outward**, each texel with none from
        // the texels round it that have: first inside each rectangle, where a
        // texel at a seam is half a surface, then one texel past it into the
        // padding, which is what a bilinear tap at the edge reads.
        const std::vector<u32>& owner = m_owner[page];
        const auto grow = [&](bool intoPadding) {
            std::vector<f32> light = out.light;
            std::vector<f32> mask = out.mask;
            bool grew = false;
            for (usize y = 0; y < side; ++y) {
                for (usize x = 0; x < side; ++x) {
                    const usize at = y * side + x;
                    if (out.light[at * 4 + 3] > 0.0f || (owner[at] == 0) != intoPadding)
                        continue;
                    f32 sum[4]{};
                    f32 count = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const auto nx = static_cast<core::i64>(x) + dx;
                            const auto ny = static_cast<core::i64>(y) + dy;
                            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= static_cast<core::i64>(side) ||
                                ny >= static_cast<core::i64>(side))
                                continue;
                            const usize beside = static_cast<usize>(ny) * side + static_cast<usize>(nx);
                            if (out.light[beside * 4 + 3] <= 0.0f)
                                continue;
                            // Within a rectangle, only from its own texels.
                            if (!intoPadding && owner[beside] != owner[at])
                                continue;
                            sum[0] += out.light[beside * 4];
                            sum[1] += out.light[beside * 4 + 1];
                            sum[2] += out.light[beside * 4 + 2];
                            sum[3] += out.mask[beside];
                            count += 1.0f;
                        }
                    }
                    if (count == 0.0f)
                        continue;
                    light[at * 4] = sum[0] / count;
                    light[at * 4 + 1] = sum[1] / count;
                    light[at * 4 + 2] = sum[2] / count;
                    light[at * 4 + 3] = 1.0f;
                    mask[at] = sum[3] / count;
                    grew = true;
                }
            }
            out.light = std::move(light);
            out.mask = std::move(mask);
            return grew;
        };
        for (u32 round = 0; round < 8 && grow(false); ++round) {
        }
        grow(true);
    }
    m_texels.clear();
    m_texels.shrink_to_fit();
    m_cells.clear();
    m_cells.shrink_to_fit();
}

bool LightmapBake::step(const BakeTrace& trace, const BakeLight& light, u64 budgetNs)
{
    if (m_stage == Stage::Idle)
        return false;
    if (!m_lightHeld) {
        m_lit = light;
        m_lightHeld = true;
    }
    const u64 started = platform::nowNs();
    struct Work
    {
        LightmapBake* self;
        const BakeTrace* trace;
    };
    Work work{this, &trace};
    while (m_stage != Stage::Done) {
        const usize texels = m_texels.size();
        if (m_stage == Stage::Mask || m_stage == Stage::Gather) {
            const usize end = budgetNs == 0 ? texels : std::min(texels, m_cursor + kChunk);
            const bool mask = m_stage == Stage::Mask;
            if (mask) {
                jobs::parallelFor(
                    "lightmap.mask", jobs::Domain::Tooling, m_cursor, end, kGrain,
                    [](void* user, usize begin, usize last, u32) noexcept {
                        const Work& asked = *static_cast<const Work*>(user);
                        for (usize texel = begin; texel < last; ++texel)
                            asked.self->maskTexel(texel, *asked.trace);
                    },
                    &work);
            }
            else {
                jobs::parallelFor(
                    "lightmap.gather", jobs::Domain::Tooling, m_cursor, end, kGrain,
                    [](void* user, usize begin, usize last, u32) noexcept {
                        const Work& asked = *static_cast<const Work*>(user);
                        for (usize texel = begin; texel < last; ++texel)
                            asked.self->gatherTexel(texel, *asked.trace);
                    },
                    &work);
            }
            m_cursor = end;
            if (m_cursor >= texels) {
                m_cursor = 0;
                if (mask) {
                    m_stage = Stage::Gather;
                    m_pass = 0;
                }
                else {
                    passEnd();
                    if (m_pass >= m_bounces)
                        m_stage = Stage::Finish;
                    else
                        ++m_pass;
                }
            }
        }
        else if (m_stage == Stage::Finish) {
            finish();
            m_stage = Stage::Done;
        }
        if (budgetNs != 0 && platform::nowNs() - started >= budgetNs)
            break;
    }
    return m_stage == Stage::Done;
}

f32 LightmapBake::progress() const noexcept
{
    const f32 done = m_texels.empty() ? 1.0f : static_cast<f32>(m_cursor) / static_cast<f32>(m_texels.size());
    switch (m_stage) {
    case Stage::Idle:
        return 0.0f;
    case Stage::Mask:
        return 0.15f * done;
    case Stage::Gather:
        return 0.15f + 0.8f * (static_cast<f32>(m_pass) + done) / static_cast<f32>(1 + m_bounces);
    case Stage::Finish:
        return 0.95f;
    case Stage::Done:
        break;
    }
    return 1.0f;
}

} // namespace engine::app
