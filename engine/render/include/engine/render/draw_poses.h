// **Where each thing is drawn this frame** (ADR 0134).
//
// The simulation moves at 60 Hz and a display does not, so what is drawn is
// placed between two ticks (`transform_history.h`, D047) -- and, on a replica,
// slid off where the simulation already is after a correction. Parts, meshes,
// decals and lights were drawn there; a name over a character, the particles
// it trailed, a camera looking at it, the 2D layer and the pointer's pick read
// the simulated position, which moves a tick at a time. In a match the name
// smeared across the character it belonged to.
//
// So there is one answer to "where is this drawn now", resolved once per
// instance per frame, and everything visual asks it: the extraction, the world
// UI (drawn and clicked), particles, views, sprites, prompts and picking. What
// asks for a simulated position instead is simulation -- scripts, physics,
// replication -- or sound, which follows the simulation by choice
// (`docs/architecture.md`). `tools/repo/drawcheck.luau` fails a visual file
// that reads a raw `cframe` or `worldCFrame`.
//
// Drawn, never simulated: like the history it reads, nothing here reaches the
// world, its hash or a replay.
#pragma once

#include <limits>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::scene {
class World;
}

namespace engine::render {

class TransformHistory;

// A 2D part's drawn place: its position and its turn, in degrees as the
// component keeps it.
struct Pose2D
{
    core::DVec3 position{};
    core::f32 rotation = 0.0f;
};

class DrawPoses
{
public:
    // Nothing between ticks: every pose is the simulated one. What a preview,
    // a test or a server draws.
    DrawPoses() = default;

    // A frame `alpha` of the way from the tick before the last to the last,
    // from what `history` saw at the start of the last. No history is the last
    // tick exactly, which is what a world drawn at its tick -- headless, or an
    // editor holding it still -- passes. Answers are remembered until the next
    // `begin`, so asking twice costs a lookup.
    void begin(const scene::World& world, const TransformHistory* history, core::f32 alpha) noexcept;

    // Where a part is drawn -- or, for anything that is not a part, where the
    // nearest part above it is. A correction's slide moves the corrected part
    // and everything under it together.
    [[nodiscard]] core::CFrameD part(core::InstanceId id) const;
    // A camera the world holds.
    [[nodiscard]] core::CFrameD camera(core::InstanceId id) const;
    // An attachment: where it sits on the part it is on, that part drawn.
    [[nodiscard]] core::CFrameD attachment(core::InstanceId id) const;
    // A 2D part.
    [[nodiscard]] Pose2D part2d(core::InstanceId id) const;

    // **A place given for this frame, over the one worked out**: what is held
    // to a bone is drawn where the frame has the bone (ADR 0198), which is
    // not where the simulation has it. Asked after this, the part or the
    // attachment answers with `cframe`, and so does whatever is worked out
    // from it; what was asked before is as it was.
    void carryPart(core::InstanceId id, const core::CFrameD& cframe);
    void carryAttachment(core::InstanceId id, const core::CFrameD& cframe);
    // What was worked out for `id` this frame is worked out again when next
    // asked: what it hangs from was carried since.
    void forget(core::InstanceId id) noexcept;

    [[nodiscard]] const scene::World* world() const noexcept { return world_; }
    [[nodiscard]] core::f32 alpha() const noexcept { return alpha_; }
    [[nodiscard]] const TransformHistory* history() const noexcept { return history_; }

private:
    // Which question an answer was to: an id is one instance, but a camera
    // asked as a part must not be handed the camera's answer.
    enum class Kind : core::u8
    {
        Part,
        Camera,
        Attachment,
        Part2D,
    };
    struct Entry
    {
        core::u32 generation = 0;
        core::u64 stamp = 0;
        Kind kind = Kind::Part;
        core::CFrameD cframe;
    };
    [[nodiscard]] const core::CFrameD* remembered(core::InstanceId id, Kind kind) const noexcept;
    void keep(core::InstanceId id, Kind kind, const core::CFrameD& cframe) const;
    [[nodiscard]] core::DVec3 slideOf(core::InstanceId id) const noexcept;

    const scene::World* world_ = nullptr;
    const TransformHistory* history_ = nullptr;
    core::f32 alpha_ = 0.0f;
    core::u64 stamp_ = 0;
    // Indexed by `InstanceId::index`, as the history is: a bounds check, never
    // a hash (R10).
    mutable std::vector<Entry> entries_;
};

} // namespace engine::render
