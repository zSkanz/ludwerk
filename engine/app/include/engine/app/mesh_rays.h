// **A mesh's own triangles, arranged to be asked what a ray meets** (ADR 0203).
//
// The light an `IrradianceVolume` works out is cast against the physics
// world, and to the physics world a mesh is its collision shape: one hull
// round a whole building, its doorway and its windows closed. So an anchored
// mesh is asked here instead, by its triangles -- which the mesh library keeps
// for this and for nothing else.
//
// A tree of boxes over the triangles, built once a mesh and kept while
// anything bakes round it. Nothing here knows of the GPU, of physics or of a
// frame: it is points, indices and a ray.
#pragma once

#include <span>
#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::app {

class TriangleTree
{
public:
    // `indices` three a triangle, into `positions`. A triangle that names a
    // point past the end, or has no area, is left out.
    void build(std::span<const core::Vec3> positions, std::span<const core::u32> indices);

    struct Hit
    {
        // Along the ray, in units of the direction's own length.
        core::f32 distance = 0.0f;
        // Which triangle: its first index is three times this.
        core::u32 triangle = 0;
        // The triangle's own normal, unit, by its winding -- so it points
        // along the ray, not against it, where the ray met its back.
        core::Vec3 normal{0.0f, 1.0f, 0.0f};
    };
    // The nearest triangle the ray meets within `reach`, front or back.
    // `direction` need not be unit: `reach` and the answer are in its lengths,
    // which is what lets a ray be asked in a stretched mesh's own space.
    [[nodiscard]] bool cast(core::Vec3 origin, core::Vec3 direction, core::f32 reach, Hit& out) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_triangles.empty(); }
    [[nodiscard]] core::usize triangleCount() const noexcept { return m_triangles.size(); }
    [[nodiscard]] core::usize bytes() const noexcept;

private:
    struct Triangle
    {
        core::Vec3 a;
        core::Vec3 ab;
        core::Vec3 ac;
        // Its place in the mesh's own list.
        core::u32 index = 0;
    };
    struct Node
    {
        core::Vec3 low;
        core::Vec3 high;
        // A leaf holds `count` triangles from `first`; a branch's children
        // are `first` and `first + 1`, and its count is nought.
        core::u32 first = 0;
        core::u32 count = 0;
    };
    void split(core::u32 node, std::vector<core::u32>& order, const std::vector<Triangle>& triangles,
               const std::vector<core::Vec3>& centres, core::u32 depth);

    std::vector<Triangle> m_triangles;
    std::vector<Node> m_nodes;
};

// **Boxes in the world, arranged to be asked which of them a ray enters**:
// the meshes and parts a volume's rays are asked of by their triangles, of
// which a scene may have thousands and a ray passes nearly all by.
class BoxTree
{
public:
    // A box each, `lows[i]` to `highs[i]`.
    void build(std::span<const core::DVec3> lows, std::span<const core::DVec3> highs);

    // Calls `visit(index, reach)` for each box the ray enters within `reach`,
    // roughly nearest first; what it answers is the ray's reach from then on,
    // so a box behind what was met is not visited.
    template <class Visit>
    void cast(const core::DVec3& origin, core::Vec3 direction, core::f32 reach, Visit&& visit) const
    {
        if (m_nodes.empty())
            return;
        const core::Vec3 start = core::toVec3(origin - m_origin);
        const auto inverse = [](core::f32 value) {
            return (value > 1.0e-12f || value < -1.0e-12f) ? 1.0f / value : (value < 0.0f ? -1.0e12f : 1.0e12f);
        };
        const core::Vec3 over{inverse(direction.x), inverse(direction.y), inverse(direction.z)};
        const auto entersBox = [&](core::Vec3 low, core::Vec3 high, core::f32 nearest) {
            core::f32 from = 0.0f;
            core::f32 until = nearest;
            const core::f32 starts[3] = {start.x, start.y, start.z};
            const core::f32 rates[3] = {over.x, over.y, over.z};
            const core::f32 lows[3] = {low.x, low.y, low.z};
            const core::f32 highs[3] = {high.x, high.y, high.z};
            for (core::u32 axis = 0; axis < 3; ++axis) {
                core::f32 a = (lows[axis] - starts[axis]) * rates[axis];
                core::f32 b = (highs[axis] - starts[axis]) * rates[axis];
                if (a > b) {
                    const core::f32 swapped = a;
                    a = b;
                    b = swapped;
                }
                from = from > a ? from : a;
                until = until < b ? until : b;
            }
            return from <= until ? from : -1.0f;
        };
        const auto enters = [&](const Node& node, core::f32 nearest) {
            return entersBox(node.low, node.high, nearest);
        };
        core::u32 stack[96];
        core::u32 held = 0;
        if (enters(m_nodes[0], reach) >= 0.0f)
            stack[held++] = 0;
        while (held > 0) {
            const Node& node = m_nodes[stack[--held]];
            if (enters(node, reach) < 0.0f)
                continue;
            if (node.count > 0) {
                for (core::u32 index = node.first; index < node.first + node.count; ++index) {
                    const core::u32 box = m_order[index];
                    if (entersBox(m_lows[box], m_highs[box], reach) >= 0.0f)
                        reach = visit(box, reach);
                }
                continue;
            }
            if (held + 2 > 96)
                continue;
            const core::f32 one = enters(m_nodes[node.first], reach);
            const core::f32 two = enters(m_nodes[node.first + 1], reach);
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
        }
    }

    [[nodiscard]] bool empty() const noexcept { return m_nodes.empty(); }

private:
    struct Node
    {
        core::Vec3 low;
        core::Vec3 high;
        core::u32 first = 0;
        core::u32 count = 0;
    };
    void split(core::u32 node, core::u32 depth);

    // Boxes are kept about their own middle: a world's places are too large
    // for the numbers a ray is walked in.
    core::DVec3 m_origin;
    std::vector<Node> m_nodes;
    std::vector<core::u32> m_order;
    std::vector<core::Vec3> m_lows;
    std::vector<core::Vec3> m_highs;
};

} // namespace engine::app
