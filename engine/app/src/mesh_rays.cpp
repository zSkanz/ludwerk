#include "engine/app/mesh_rays.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace engine::app {

namespace {

using core::f32;
using core::u32;
using core::usize;
using core::Vec3;

// A leaf holds this many triangles or fewer; a tree is no deeper than this.
constexpr u32 kLeaf = 4;
constexpr u32 kDeepest = 48;

[[nodiscard]] f32 axisOf(Vec3 v, u32 axis) noexcept
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

[[nodiscard]] Vec3 least(Vec3 a, Vec3 b) noexcept
{
    return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

[[nodiscard]] Vec3 most(Vec3 a, Vec3 b) noexcept
{
    return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

} // namespace

void TriangleTree::build(std::span<const Vec3> positions, std::span<const u32> indices)
{
    m_triangles.clear();
    m_nodes.clear();
    std::vector<Triangle> triangles;
    triangles.reserve(indices.size() / 3);
    std::vector<Vec3> centres;
    centres.reserve(indices.size() / 3);
    for (usize index = 0; index + 2 < indices.size(); index += 3) {
        const u32 ia = indices[index];
        const u32 ib = indices[index + 1];
        const u32 ic = indices[index + 2];
        if (ia >= positions.size() || ib >= positions.size() || ic >= positions.size())
            continue;
        const Vec3 a = positions[ia];
        const Vec3 ab = positions[ib] - a;
        const Vec3 ac = positions[ic] - a;
        const Vec3 normal = core::cross(ab, ac);
        if (!(core::dot(normal, normal) > 0.0f))
            continue;
        triangles.push_back(Triangle{a, ab, ac, static_cast<u32>(index / 3)});
        centres.push_back(a + (ab + ac) * (1.0f / 3.0f));
    }
    if (triangles.empty())
        return;
    // The triangles stay where they are while the tree is made: what is
    // divided is a list of their numbers, in place, and they are put in the
    // tree's order once at the end.
    std::vector<u32> order(triangles.size());
    for (u32 index = 0; index < order.size(); ++index)
        order[index] = index;
    m_nodes.reserve(triangles.size() / 2 + 8);
    m_nodes.push_back(Node{.low = {}, .high = {}, .first = 0, .count = static_cast<u32>(triangles.size())});
    split(0, order, triangles, centres, 0);
    m_triangles.resize(triangles.size());
    for (usize index = 0; index < order.size(); ++index)
        m_triangles[index] = triangles[order[index]];
}

void TriangleTree::split(u32 node, std::vector<u32>& order, const std::vector<Triangle>& triangles,
                         const std::vector<Vec3>& centres, u32 depth)
{
    const u32 first = m_nodes[node].first;
    const u32 count = m_nodes[node].count;
    Vec3 low{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
    Vec3 high{-low.x, -low.y, -low.z};
    Vec3 centreLow = low;
    Vec3 centreHigh = high;
    for (u32 index = first; index < first + count; ++index) {
        const Triangle& triangle = triangles[order[index]];
        for (const Vec3 corner : {triangle.a, triangle.a + triangle.ab, triangle.a + triangle.ac}) {
            low = least(low, corner);
            high = most(high, corner);
        }
        centreLow = least(centreLow, centres[order[index]]);
        centreHigh = most(centreHigh, centres[order[index]]);
    }
    m_nodes[node].low = low;
    m_nodes[node].high = high;
    if (count <= kLeaf || depth >= kDeepest)
        return;

    // Across the side their middles are spread widest along, at the middle
    // triangle: two halves of equal number, whatever the mesh's shape.
    const Vec3 spread = centreHigh - centreLow;
    const u32 axis = spread.x >= spread.y && spread.x >= spread.z ? 0u : (spread.y >= spread.z ? 1u : 2u);
    if (!(axisOf(spread, axis) > 0.0f))
        return;
    const u32 middle = count / 2;
    std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(first),
                     order.begin() + static_cast<std::ptrdiff_t>(first + middle),
                     order.begin() + static_cast<std::ptrdiff_t>(first + count), [&](u32 a, u32 b) {
                         const f32 left = axisOf(centres[a], axis);
                         const f32 right = axisOf(centres[b], axis);
                         // By their number where they are level: the tree is
                         // the same tree on every machine.
                         return left < right || (left == right && a < b);
                     });

    const auto left = static_cast<u32>(m_nodes.size());
    m_nodes.push_back(Node{.low = {}, .high = {}, .first = first, .count = middle});
    m_nodes.push_back(Node{.low = {}, .high = {}, .first = first + middle, .count = count - middle});
    m_nodes[node].first = left;
    m_nodes[node].count = 0;
    split(left, order, triangles, centres, depth + 1);
    split(left + 1, order, triangles, centres, depth + 1);
}

bool TriangleTree::cast(Vec3 origin, Vec3 direction, f32 reach, Hit& out) const noexcept
{
    if (m_nodes.empty())
        return false;
    // One over each part of the direction, large where the ray runs along a
    // side: a box is then entered at once or never, as it should be.
    const auto inverse = [](f32 value) {
        return std::abs(value) > 1.0e-12f ? 1.0f / value : (value < 0.0f ? -1.0e12f : 1.0e12f);
    };
    const Vec3 over{inverse(direction.x), inverse(direction.y), inverse(direction.z)};
    // Where the ray enters a box, or a negative number where it does not
    // before `nearest`.
    const auto enters = [&](const Node& node, f32 nearest) {
        f32 from = 0.0f;
        f32 until = nearest;
        for (u32 axis = 0; axis < 3; ++axis) {
            const f32 start = axisOf(origin, axis);
            const f32 rate = axisOf(over, axis);
            f32 a = (axisOf(node.low, axis) - start) * rate;
            f32 b = (axisOf(node.high, axis) - start) * rate;
            if (a > b)
                std::swap(a, b);
            from = std::max(from, a);
            until = std::min(until, b);
        }
        return from <= until ? from : -1.0f;
    };

    bool met = false;
    f32 nearest = reach;
    std::array<u32, kDeepest * 2 + 16> stack{};
    u32 held = 0;
    if (enters(m_nodes[0], nearest) >= 0.0f)
        stack[held++] = 0;
    while (held > 0) {
        const Node& node = m_nodes[stack[--held]];
        if (node.count == 0) {
            // The nearer child first: what it meets shortens the ray for the
            // other, which is then most often not entered at all.
            const f32 one = enters(m_nodes[node.first], nearest);
            const f32 two = enters(m_nodes[node.first + 1], nearest);
            if (held + 2 > stack.size())
                continue;
            if (one >= 0.0f && two >= 0.0f) {
                stack[held++] = one <= two ? node.first + 1 : node.first;
                stack[held++] = one <= two ? node.first : node.first + 1;
            }
            else if (one >= 0.0f) {
                stack[held++] = node.first;
            }
            else if (two >= 0.0f) {
                stack[held++] = node.first + 1;
            }
            continue;
        }
        // Entered when it was put on the stack; the ray may have been
        // shortened since.
        if (enters(node, nearest) < 0.0f)
            continue;
        for (u32 index = node.first; index < node.first + node.count; ++index) {
            const Triangle& triangle = m_triangles[index];
            // Moller and Trumbore's: the ray against the plane and the two
            // sides at once, both faces.
            const Vec3 p = core::cross(direction, triangle.ac);
            const f32 det = core::dot(triangle.ab, p);
            if (std::abs(det) < 1.0e-12f)
                continue;
            const f32 inv = 1.0f / det;
            const Vec3 s = origin - triangle.a;
            const f32 u = core::dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f)
                continue;
            const Vec3 q = core::cross(s, triangle.ab);
            const f32 v = core::dot(direction, q) * inv;
            if (v < 0.0f || u + v > 1.0f)
                continue;
            const f32 along = core::dot(triangle.ac, q) * inv;
            if (along < 0.0f || along >= nearest)
                continue;
            nearest = along;
            met = true;
            out.distance = along;
            out.triangle = triangle.index;
            out.normal = core::normalize(core::cross(triangle.ab, triangle.ac));
        }
    }
    return met;
}

void BoxTree::build(std::span<const core::DVec3> lows, std::span<const core::DVec3> highs)
{
    m_nodes.clear();
    m_order.clear();
    m_lows.clear();
    m_highs.clear();
    const usize count = std::min(lows.size(), highs.size());
    if (count == 0)
        return;
    core::DVec3 sum{0.0, 0.0, 0.0};
    for (usize index = 0; index < count; ++index)
        sum = sum + lows[index] + highs[index];
    const double share = 0.5 / static_cast<double>(count);
    m_origin = core::DVec3{sum.x * share, sum.y * share, sum.z * share};
    m_lows.resize(count);
    m_highs.resize(count);
    m_order.resize(count);
    for (usize index = 0; index < count; ++index) {
        m_lows[index] = core::toVec3(lows[index] - m_origin);
        m_highs[index] = core::toVec3(highs[index] - m_origin);
        m_order[index] = static_cast<u32>(index);
    }
    m_nodes.reserve(count * 2);
    m_nodes.push_back(Node{.low = {}, .high = {}, .first = 0, .count = static_cast<u32>(count)});
    split(0, 0);
}

void BoxTree::split(u32 node, u32 depth)
{
    const std::vector<Vec3>& lows = m_lows;
    const std::vector<Vec3>& highs = m_highs;
    const u32 first = m_nodes[node].first;
    const u32 count = m_nodes[node].count;
    Vec3 low{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
    Vec3 high{-low.x, -low.y, -low.z};
    Vec3 centreLow = low;
    Vec3 centreHigh = high;
    for (u32 index = first; index < first + count; ++index) {
        const u32 box = m_order[index];
        low = least(low, lows[box]);
        high = most(high, highs[box]);
        const Vec3 centre = (lows[box] + highs[box]) * 0.5f;
        centreLow = least(centreLow, centre);
        centreHigh = most(centreHigh, centre);
    }
    m_nodes[node].low = low;
    m_nodes[node].high = high;
    if (count <= 2 || depth >= 40)
        return;
    const Vec3 spread = centreHigh - centreLow;
    const u32 axis = spread.x >= spread.y && spread.x >= spread.z ? 0u : (spread.y >= spread.z ? 1u : 2u);
    if (!(axisOf(spread, axis) > 0.0f))
        return;
    const u32 middle = count / 2;
    std::nth_element(m_order.begin() + static_cast<std::ptrdiff_t>(first),
                     m_order.begin() + static_cast<std::ptrdiff_t>(first + middle),
                     m_order.begin() + static_cast<std::ptrdiff_t>(first + count), [&](u32 a, u32 b) {
                         const f32 left = axisOf(lows[a], axis) + axisOf(highs[a], axis);
                         const f32 right = axisOf(lows[b], axis) + axisOf(highs[b], axis);
                         return left < right || (left == right && a < b);
                     });
    const auto child = static_cast<u32>(m_nodes.size());
    m_nodes.push_back(Node{.low = {}, .high = {}, .first = first, .count = middle});
    m_nodes.push_back(Node{.low = {}, .high = {}, .first = first + middle, .count = count - middle});
    m_nodes[node].first = child;
    m_nodes[node].count = 0;
    split(child, depth + 1);
    split(child + 1, depth + 1);
}

core::usize TriangleTree::bytes() const noexcept
{
    return m_triangles.capacity() * sizeof(Triangle) + m_nodes.capacity() * sizeof(Node);
}

} // namespace engine::app
