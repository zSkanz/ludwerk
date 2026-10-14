#pragma once

// **Every mouth that moves with what is said, once a frame** (ADR 0200).
//
// A `LipSync` on a mesh says whose voice moves its face. Each frame, for each
// of them: is a `Voice` sound that this machine can hear playing under that
// instance? Then the face's shape keys follow it -- by the line's viseme track
// when the file this machine resolved the line to has one beside it, by the
// sound's three frequency bands when it has none and the face has keys for
// them, by its loudness alone at the least.
//
// **All of it is picture.** It reads what the audio system is playing HERE,
// in the language this machine hears, and writes the frame's own layer of
// weights on the mesh (`AnimationSystem::presentMorphWeights`). Nothing of it
// reaches a tick, a hash, a save or another machine.

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/asset/lip_sync.h"
#include "engine/core/id.h"
#include "engine/core/types.h"

namespace engine::asset {
class ContentMounts;
}
namespace engine::audio {
class AudioSystem;
}
namespace engine::render {
class AnimationSystem;
}
namespace engine::scene {
class World;
}

namespace engine::app {

class Mouths
{
public:
    // `seconds` since the frame before, which is what a weight is eased over.
    void step(const scene::World& world, audio::AudioSystem& audio, render::AnimationSystem& animation,
              const asset::ContentMounts* mounts, core::f32 seconds);

    // A track or a face map changed on disk: read again when next wanted.
    void forget(std::span<const std::string> names);
    void clear();

    // What the frame before did, for a readout and for a test: the mouths
    // that were moved, and by which of the three ways.
    struct Stats
    {
        core::u32 moved = 0;
        core::u32 byTrack = 0;
        core::u32 byBands = 0;
        core::u32 byLoudness = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    template <class File>
    struct Read
    {
        std::string name;
        bool found = false;
        File file;
    };
    // By name, each asked of the mounts once: a line with no track is asked
    // every frame it plays, and must cost a lookup here and not a file's.
    std::vector<Read<asset::VisemeTrack>> tracks_;
    std::vector<Read<asset::FaceMap>> maps_;
    [[nodiscard]] const asset::VisemeTrack* trackFor(const asset::ContentMounts* mounts, std::string_view sound);
    [[nodiscard]] const asset::FaceMap* mapFor(const asset::ContentMounts* mounts, std::string_view name);

    // Where each mouth's keys are, eased towards where they are going.
    struct Mouth
    {
        core::u64 key = 0;
        bool seen = false;
        std::vector<std::pair<std::string, core::f32>> weights;
    };
    std::vector<Mouth> mouths_;
    std::vector<std::pair<std::string, core::f32>> wanted_;
    Stats stats_;
};

} // namespace engine::app
