// Where a thing was one tick ago, so a frame can be drawn between two ticks
// (architecture.md §3, D047).
//
// **The simulation is a fixed 60 Hz and a display is not.** This machine renders
// about a hundred frames for every sixty ticks, so without interpolation two
// frames out of three show the world exactly where the previous one did while
// the third jumps a whole tick forward. A human walking `examples/10-open-world`
// described that as the world vibrating, and they were describing it precisely.
//
// `FrameScheduler` has computed the factor for this since M1 -- `Frame::alpha`,
// "where rendering sits between the last tick and the next" -- and until M8
// nothing read it. This is the consumer.
//
// **It lives in `render` and not in `scene`, deliberately.** A previous
// transform is not world state: it is not simulated, it must not enter the world
// hash, and a replay must not carry it. Keeping it here means `scene`'s
// components are untouched, `World::snapshot` still memcpys the same bytes, and
// every recorded determinism trace stays valid.
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

class TransformHistory
{
public:
    // Records where every part and camera is RIGHT NOW. Called at the start of
    // each tick, so that once the tick has run this holds the state the frame is
    // interpolating FROM and the world holds the state it is interpolating TO.
    //
    // A frame that runs no ticks leaves both alone, which is exactly right: the
    // interval has not changed, only where in it the frame sits.
    void capture(const scene::World& world);
    // **One subtree's last place, moved by `by`**: a replica's own character,
    // which a correction just moved by that much. Its slide (`setVisualOffset`)
    // already draws it where it was; moved with it, its last place keeps the
    // step between ticks what it was, rather than the correction counted a
    // second time (ADR 0134). Everything else keeps the capture from before
    // the snapshot, so what the snapshot moved -- another player, a part the
    // authority moves -- is drawn getting there.
    void shift(const scene::World& world, core::InstanceId root, core::DVec3 by);

    // Where this instance was at the last capture, or null when it was not
    // there -- something that streamed in, or was created, has no previous and
    // must be drawn where it is rather than smeared in from nowhere.
    [[nodiscard]] const core::CFrameD* previous(core::InstanceId id) const noexcept;

    void clear() noexcept;

    // **One instance drawn off where it is** (the multiplayer smoothness
    // brief): a replica's own character, after a correction, sliding to where
    // the simulation already is -- and with it everything under it, which
    // `DrawPoses` applies (ADR 0134). Drawn, never simulated -- it is not the
    // world's, for the reason this class is not.
    void setVisualOffset(core::InstanceId id, core::DVec3 offset) noexcept
    {
        offsetId_ = id;
        offset_ = offset;
    }
    [[nodiscard]] core::InstanceId visualOffsetId() const noexcept { return offsetId_; }
    [[nodiscard]] core::DVec3 visualOffset() const noexcept { return offset_; }

private:
    struct Entry
    {
        // Both are needed and for different reasons: the generation catches a
        // slot reused by a different instance, and the stamp catches an
        // instance that existed at some earlier capture but not the last one.
        core::u32 generation = 0;
        core::u64 stamp = 0;
        core::CFrameD cframe;
    };

    // Indexed by `InstanceId::index`, which is what makes the lookup a bounds
    // check rather than a hash -- and an unordered container here would be a
    // determinism smell even though nothing iterates it (R10).
    std::vector<Entry> entries_;
    core::u64 stamp_ = 0;
    core::InstanceId offsetId_;
    core::DVec3 offset_{};
};

// A part's own largest side, and never less than half a metre, so a small fast
// thing -- a ball, a bullet -- keeps its interpolation. Past it, a move in one
// tick is a teleport.
[[nodiscard]] core::f64 teleportReach(core::Vec3 size) noexcept;

// **Where `id` is between two ticks**: `alpha` of the way from where the last
// capture saw it to `current`. Anything with no previous transform is where
// it is, and a move longer than `teleport` is where it landed. Not where it is
// DRAWN -- that adds a correction's slide, and is `DrawPoses`, which
// everything visual asks (ADR 0134).
[[nodiscard]] core::CFrameD interpolatedCFrame(const TransformHistory* history, core::InstanceId id,
                                               const core::CFrameD& current, core::f32 alpha,
                                               core::f64 teleport = std::numeric_limits<core::f64>::infinity());

} // namespace engine::render
