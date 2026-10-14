#include "engine/scene/pivot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace engine::scene {

using core::f64;

core::CFrameD pivotOffsetOf(const World& world, core::InstanceId id) noexcept
{
    const PVComponent* pv = world.pvInstances().find(id);
    return pv == nullptr ? core::CFrameD{} : pv->pivotOffset;
}

bool worldExtents(const World& world, core::InstanceId id, core::DVec3& minimum, core::DVec3& maximum)
{
    std::vector<core::InstanceId> descendants;
    world.collectDescendants(id, descendants);

    bool any = false;
    for (const core::InstanceId descendant : descendants) {
        const PartComponent* part = world.parts().find(descendant);
        if (part == nullptr)
            continue;

        // The standard OBB-to-AABB bound. `Mat3` is column-major -- `m[c][r]` --
        // so the inner index is the column and the outer is the world axis.
        const core::Mat3& r = part->cframe.rotation;
        const f64 half[3] = {
            static_cast<f64>(part->size.x) * 0.5,
            static_cast<f64>(part->size.y) * 0.5,
            static_cast<f64>(part->size.z) * 0.5,
        };
        f64 extents[3] = {0.0, 0.0, 0.0};
        for (int axis = 0; axis < 3; ++axis) {
            for (int local = 0; local < 3; ++local)
                extents[axis] += std::fabs(static_cast<f64>(r.m[local][axis])) * half[local];
        }

        const core::DVec3 centre = part->cframe.position;
        const core::DVec3 low{centre.x - extents[0], centre.y - extents[1], centre.z - extents[2]};
        const core::DVec3 high{centre.x + extents[0], centre.y + extents[1], centre.z + extents[2]};
        if (!any) {
            minimum = low;
            maximum = high;
            any = true;
            continue;
        }
        minimum = core::DVec3{std::min(minimum.x, low.x), std::min(minimum.y, low.y), std::min(minimum.z, low.z)};
        maximum = core::DVec3{std::max(maximum.x, high.x), std::max(maximum.y, high.y), std::max(maximum.z, high.z)};
    }
    return any;
}

core::CFrameD pivotBase(const World& world, core::InstanceId id)
{
    if (const ModelComponent* model = world.models().find(id); model != nullptr) {
        if (world.alive(model->primaryPart)) {
            if (const PartComponent* part = world.parts().find(model->primaryPart)) {
                // The primary part's OWN pivot, offset included: a model whose
                // primary part hinges about its edge hinges about that edge too,
                // which is the property that makes assigning a primary part mean
                // something beyond "pick a position".
                return part->cframe * pivotOffsetOf(world, model->primaryPart);
            }
        }

        core::DVec3 minimum;
        core::DVec3 maximum;
        core::CFrameD pivot;
        // An identity fallback would move a model built far from the origin by
        // its whole distance the first time anything pivoted it.
        if (worldExtents(world, id, minimum, maximum)) {
            pivot.position = core::DVec3{(minimum.x + maximum.x) * 0.5, (minimum.y + maximum.y) * 0.5,
                                         (minimum.z + maximum.z) * 0.5};
        }
        return pivot;
    }

    if (const PartComponent* part = world.parts().find(id); part != nullptr)
        return part->cframe;
    if (const CameraComponent* camera = world.cameras().find(id); camera != nullptr)
        return camera->cframe;
    return {};
}

core::CFrameD pivotOf(const World& world, core::InstanceId id)
{
    return pivotBase(world, id) * pivotOffsetOf(world, id);
}

namespace {

// A rotation in f64, row by column: `r[row][column]`.
struct Basis
{
    f64 r[3][3];
};

[[nodiscard]] Basis widened(const core::Mat3& m) noexcept
{
    Basis out{};
    // `Mat3` is column-major, `m[column][row]`.
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            out.r[row][column] = static_cast<f64>(m.m[column][row]);
    return out;
}

// Orthonormal again, the look axis (column 2, back) authoritative and up a
// hint -- `core::orthonormalize`'s rule, in f64.
[[nodiscard]] Basis orthonormal(const Basis& b) noexcept
{
    const auto column = [&b](int c) { return std::array<f64, 3>{b.r[0][c], b.r[1][c], b.r[2][c]}; };
    const auto unit = [](std::array<f64, 3> v) {
        const f64 length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length > 0.0)
            for (f64& x : v)
                x /= length;
        return v;
    };
    const auto crossed = [](const std::array<f64, 3>& a, const std::array<f64, 3>& c) {
        return std::array<f64, 3>{a[1] * c[2] - a[2] * c[1], a[2] * c[0] - a[0] * c[2], a[0] * c[1] - a[1] * c[0]};
    };
    const std::array<f64, 3> back = unit(column(2));
    const std::array<f64, 3> right = unit(crossed(column(1), back));
    const std::array<f64, 3> up = crossed(back, right);
    Basis out{};
    for (int row = 0; row < 3; ++row) {
        out.r[row][0] = right[static_cast<usize>(row)];
        out.r[row][1] = up[static_cast<usize>(row)];
        out.r[row][2] = back[static_cast<usize>(row)];
    }
    return out;
}

[[nodiscard]] Basis product(const Basis& a, const Basis& b) noexcept
{
    Basis out{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out.r[row][column] =
                a.r[row][0] * b.r[0][column] + a.r[row][1] * b.r[1][column] + a.r[row][2] * b.r[2][column];
    return out;
}

[[nodiscard]] Basis transposed(const Basis& a) noexcept
{
    Basis out{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out.r[row][column] = a.r[column][row];
    return out;
}

[[nodiscard]] core::DVec3 applied(const Basis& a, const core::DVec3& v) noexcept
{
    return core::DVec3{a.r[0][0] * v.x + a.r[0][1] * v.y + a.r[0][2] * v.z,
                       a.r[1][0] * v.x + a.r[1][1] * v.y + a.r[1][2] * v.z,
                       a.r[2][0] * v.x + a.r[2][1] * v.y + a.r[2][2] * v.z};
}

[[nodiscard]] core::Mat3 narrowed(const Basis& b) noexcept
{
    core::Mat3 out;
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            out.m[column][row] = static_cast<core::f32>(b.r[row][column]);
    return out;
}

[[nodiscard]] bool same(const core::CFrameD& a, const core::CFrameD& b) noexcept
{
    if (a.position.x != b.position.x || a.position.y != b.position.y || a.position.z != b.position.z)
        return false;
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            if (a.rotation.m[column][row] != b.rotation.m[column][row])
                return false;
    return true;
}

} // namespace

core::CFrameD movedWith(const core::CFrameD& to, const core::CFrameD& from, const core::CFrameD& of) noexcept
{
    const Basis target = orthonormal(widened(to.rotation));
    const Basis pivotInverse = transposed(orthonormal(widened(from.rotation)));
    const Basis own = orthonormal(widened(of.rotation));

    const core::DVec3 offset{of.position.x - from.position.x, of.position.y - from.position.y,
                             of.position.z - from.position.z};
    const core::DVec3 moved = applied(target, applied(pivotInverse, offset));

    core::CFrameD out;
    out.position = core::DVec3{to.position.x + moved.x, to.position.y + moved.y, to.position.z + moved.z};
    out.rotation = narrowed(orthonormal(product(target, product(pivotInverse, own))));
    return out;
}

void pivotTo(World& world, core::InstanceId id, const core::CFrameD& target)
{
    const core::CFrameD pivot = pivotOf(world, id);
    // Already there: nothing moves, not even by a rounding.
    if (same(pivot, target))
        return;
    const core::NameAtom cframeProperty = world.atoms().intern("CFrame");

    if (world.models().find(id) != nullptr) {
        std::vector<core::InstanceId> descendants;
        world.collectDescendants(id, descendants);
        for (const core::InstanceId descendant : descendants) {
            const PartComponent* part = world.parts().find(descendant);
            if (part == nullptr)
                continue;
            const core::CFrameD moved = movedWith(target, pivot, part->cframe);
            (void)world.setProperty(descendant, cframeProperty, Value{moved});
        }
        return;
    }

    // A part moves alone: what hangs off it is welds and constraints, not
    // geometry.
    if (const PartComponent* part = world.parts().find(id); part != nullptr) {
        const core::CFrameD moved = movedWith(target, pivot, part->cframe);
        (void)world.setProperty(id, cframeProperty, Value{moved});
        return;
    }

    if (const CameraComponent* camera = world.cameras().find(id); camera != nullptr) {
        const core::CFrameD moved = movedWith(target, pivot, camera->cframe);
        (void)world.setProperty(id, cframeProperty, Value{moved});
    }
}

} // namespace engine::scene
