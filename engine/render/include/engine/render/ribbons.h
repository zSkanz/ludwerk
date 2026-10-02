// Beams and trails (ADR 0129): ribbons, built on the frame.
//
// **A picture, not the world**, as particles are (ADR 0072): a `Beam` is a
// band between two attachments and a `Trail` is the ribbon two attachments
// leave behind, and neither decides anything a script can observe. So nothing
// here is in the world, the hash or a snapshot: the world holds what was
// asked for (`BeamComponent`, `TrailComponent`), and this holds what it takes
// to draw it -- for a beam nothing at all, for a trail the places its two
// ends have been.
//
// A trail is sampled where its attachments are DRAWN (ADR 0134), once a
// frame, on the render clock. A headless run's frame is one tick long, so a
// golden with a trail in it is one picture.
#pragma once

#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/render/draw_poses.h"
#include "engine/scene/components.h"

namespace engine::scene {
class World;
}

namespace engine::render {

struct RenderWorld;
class TextureLibrary;

class RibbonSystem
{
public:
    // The most pieces one trail keeps: at the smallest `MinLength` a trail
    // that long is still a few kilobytes, and one past it loses its oldest.
    static constexpr core::usize MaxTrailPieces = 512;
    // The most vertices drawn in a frame, six to a piece. Past it the
    // furthest ribbons are the ones left out.
    static constexpr core::usize MaxVertices = 98304;
    // The longest one frame is allowed to be, as it is for particles: a hitch
    // must not age every trail away at once.
    static constexpr core::f64 MaxStep = 0.1;

    // Ages every trail by `dt`, drops what has outlived its `Lifetime`, and
    // adds a piece where its attachments have moved far enough. Beams have
    // nothing to advance but the clock their textures run on.
    void update(const scene::World& world, core::InstanceId root, core::f64 dt, const DrawPoses* poses = nullptr);

    // This frame's ribbons into `out`, camera-relative and back to front. The
    // beams are built here, from the world as it is drawn: they keep nothing.
    void append(const scene::World& world, core::InstanceId root, RenderWorld& out, const TextureLibrary* textures,
                const DrawPoses* poses = nullptr) const;

    // How long a trail is now, in metres along its middle: what `Lifetime`
    // and `MaxLength` bound. Zero for one this has never seen.
    [[nodiscard]] core::f32 trailLength(core::InstanceId trail) const noexcept;
    [[nodiscard]] core::usize trailPieces(core::InstanceId trail) const noexcept;

    struct Stats
    {
        core::u32 beams = 0;
        core::u32 trails = 0;
        // Quads drawn, across both.
        core::u32 pieces = 0;
    };
    // What the last `append` drew.
    [[nodiscard]] Stats stats() const noexcept { return m_stats; }

    void clear() noexcept
    {
        m_trails.clear();
        m_time = 0.0;
    }

private:
    // Where a trail's two ends were, and when.
    struct Piece
    {
        core::DVec3 a;
        core::DVec3 b;
        core::f64 born = 0.0;
        // Metres along the trail's middle since it began: what a `Static`
        // texture is laid by, and what its length is measured in.
        core::f64 travelled = 0.0;
    };

    struct Trail
    {
        core::InstanceId id;
        std::vector<Piece> pieces;
        // Where the ends are this frame: the trail's live edge, which is
        // always drawn and becomes a piece once it has moved far enough.
        Piece head;
        bool hasHead = false;
        core::u32 cleared = 0;
        bool seen = false;
    };

    [[nodiscard]] const Trail* find(core::InstanceId id) const noexcept;

    std::vector<Trail> m_trails;
    core::f64 m_time = 0.0;
    mutable Stats m_stats;
};

} // namespace engine::render
