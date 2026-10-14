#include "engine/scene/fracture.h"

#include <algorithm>
#include <cmath>

namespace engine::scene {
namespace {

// A number from a seed, the same on every machine: splitmix64.
[[nodiscard]] core::u64 nextOf(core::u64& state) noexcept
{
    state += 0x9e3779b97f4a7c15ull;
    core::u64 z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Between nought and one, from the top 24 bits: exact in an f32.
[[nodiscard]] core::f32 fractionOf(core::u64& state) noexcept
{
    return static_cast<core::f32>(nextOf(state) >> 40) / static_cast<core::f32>(1u << 24);
}

[[nodiscard]] core::f32 axisOf(const core::Vec3& v, int axis) noexcept
{
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

void setAxis(core::Vec3& v, int axis, core::f32 value) noexcept
{
    (axis == 0 ? v.x : axis == 1 ? v.y : v.z) = value;
}

} // namespace

FractureCut cutBox(core::Vec3 size, core::u32 pieces, core::u64 seed)
{
    FractureCut cut;
    const core::u32 wanted = std::clamp(pieces, 1u, MaxFracturePieces);
    cut.pieces.push_back(FracturePiece{core::Vec3{}, size});
    core::u64 state = seed;
    while (cut.pieces.size() < wanted) {
        // The largest piece, the first of equals: a pure function of the cut so far.
        core::usize largest = 0;
        core::f32 most = -1.0f;
        for (core::usize index = 0; index < cut.pieces.size(); ++index) {
            const core::Vec3& s = cut.pieces[index].size;
            const core::f32 volume = s.x * s.y * s.z;
            if (volume > most) {
                most = volume;
                largest = index;
            }
        }
        FracturePiece whole = cut.pieces[largest];
        int axis = 0;
        for (int each = 1; each < 3; ++each) {
            if (axisOf(whole.size, each) > axisOf(whole.size, axis))
                axis = each;
        }
        const core::f32 length = axisOf(whole.size, axis);
        const core::f32 share = 0.3f + 0.4f * fractionOf(state);
        const core::f32 first = length * share;
        const core::f32 low = axisOf(whole.center, axis) - length * 0.5f;
        FracturePiece a = whole;
        FracturePiece b = whole;
        setAxis(a.size, axis, first);
        setAxis(a.center, axis, low + first * 0.5f);
        setAxis(b.size, axis, length - first);
        setAxis(b.center, axis, low + first + (length - first) * 0.5f);
        cut.pieces[largest] = a;
        cut.pieces.push_back(b);
    }

    // **Two pieces share a face** where they meet along one axis and overlap
    // on both others: the joint is at the middle of the overlap.
    const core::f32 touching = 1.0e-4f * std::max({size.x, size.y, size.z, 1.0e-3f});
    for (core::u32 i = 0; i < cut.pieces.size(); ++i) {
        for (core::u32 j = i + 1; j < cut.pieces.size(); ++j) {
            const FracturePiece& p = cut.pieces[i];
            const FracturePiece& q = cut.pieces[j];
            for (int axis = 0; axis < 3; ++axis) {
                const core::f32 apart = std::abs(axisOf(p.center, axis) - axisOf(q.center, axis));
                if (std::abs(apart - (axisOf(p.size, axis) + axisOf(q.size, axis)) * 0.5f) > touching)
                    continue;
                core::Vec3 at{};
                bool overlap = true;
                for (int other = 0; other < 3 && overlap; ++other) {
                    if (other == axis)
                        continue;
                    const core::f32 lo = std::max(axisOf(p.center, other) - axisOf(p.size, other) * 0.5f,
                                                  axisOf(q.center, other) - axisOf(q.size, other) * 0.5f);
                    const core::f32 hi = std::min(axisOf(p.center, other) + axisOf(p.size, other) * 0.5f,
                                                  axisOf(q.center, other) + axisOf(q.size, other) * 0.5f);
                    overlap = hi - lo > touching;
                    setAxis(at, other, (lo + hi) * 0.5f);
                }
                if (!overlap)
                    continue;
                const core::f32 pFace =
                    axisOf(p.center, axis) +
                    (axisOf(q.center, axis) > axisOf(p.center, axis) ? 0.5f : -0.5f) * axisOf(p.size, axis);
                setAxis(at, axis, pFace);
                cut.joints.push_back(FractureJoint{i, j, at});
                break;
            }
        }
    }
    return cut;
}

} // namespace engine::scene
