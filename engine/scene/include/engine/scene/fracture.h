#pragma once

// **A box cut into pieces** (ADR 0215): what `BasePart:Fracture` makes of a
// part. The same seed and the same box are the same pieces on every machine
// and every run (R10), so a cut can be named by the two.

#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::scene {

// One piece, in the box's own space: its middle, and how large it is.
struct FracturePiece
{
    core::Vec3 center{};
    core::Vec3 size{};
};

// Two pieces that share a face, and the middle of what they share: where a
// joint holds them together.
struct FractureJoint
{
    core::u32 first = 0;
    core::u32 second = 0;
    core::Vec3 at{};
};

struct FractureCut
{
    std::vector<FracturePiece> pieces;
    std::vector<FractureJoint> joints;
};

// The most pieces one cut makes: each is a part on every machine in a match.
inline constexpr core::u32 MaxFracturePieces = 64;

// **`size` cut into `pieces` boxes**: the largest piece cut across its
// longest side, between three and seven tenths of the way, again and again,
// by the seed -- so the pieces are of different sizes, as broken things are,
// and none is a sliver. Then a joint for every two that share a face.
// `pieces` is held to 1 .. `MaxFracturePieces`.
[[nodiscard]] FractureCut cutBox(core::Vec3 size, core::u32 pieces, core::u64 seed);

} // namespace engine::scene
