// **A volume's light, kept** (ADR 0207): what an `IrradianceVolume` worked out,
// as a file a game ships. A machine that works volumes out reads the file and
// casts no ray; a machine that works none out reads one colour of it -- the
// mean of the whole -- and lights the room with that.
//
// The file holds what the volume was when it was worked out: where its box
// stood and how large, how far apart its points were, how many bounces, which
// mode. A volume that is no longer that volume does not take the file's light:
// it would be the light of another room.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::app {

// One piece of a kept volume: the whole of it, or one of its tiles.
struct BakedPiece
{
    core::u32 counts[3] = {1, 1, 1};
    // Twelve numbers a point, as `RenderIrradianceVolume::samples` has them.
    std::vector<core::f32> samples;
};

struct BakedLight
{
    core::CFrameD box;
    core::Vec3 size{1.0f, 1.0f, 1.0f};
    core::f32 spacing = 2.0f;
    core::i32 bounces = 1;
    core::i32 mode = 0;
    // **What stood in the room when it was worked out**, as one number
    // (`IndirectLight::roomOf`): where each thing that does not move stood and
    // how large it was. Nought says nothing was noted.
    core::u64 room = 0;
    std::vector<BakedPiece> pieces;
    // The light from every side, as one colour for the whole box: the mean of
    // every point's. What a machine that works no volume out lights with.
    core::Color3 mean{0.0f, 0.0f, 0.0f};
};

// The mean of every point's light from every side.
[[nodiscard]] core::Color3 bakedMean(std::span<const BakedPiece> pieces) noexcept;

// The file's bytes. `mean` is worked out here: what is written is the mean of
// what is written.
[[nodiscard]] std::vector<std::byte> writeBakedLight(const BakedLight& light);

// False for bytes that are not such a file, are of a version this build does
// not read, are cut short, or hold a number that is not one.
[[nodiscard]] bool readBakedLight(std::span<const std::byte> bytes, BakedLight& out);

} // namespace engine::app
