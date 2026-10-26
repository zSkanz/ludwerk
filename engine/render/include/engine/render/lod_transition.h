#pragma once

// **A mesh changes its level of detail without being seen to** (ADR 0261).
//
// The level a mesh is drawn at was chosen afresh each frame from how far it
// is, so a mesh standing near a level's edge flickered between two, and every
// change was a pop. This keeps each mesh's level from frame to frame:
//
//   - **Hysteresis**: a mesh goes coarser only when the coarser level would
//     still do seen a sixth nearer, and finer only when its level would not do
//     seen a sixth further; between the two it stays where it is.
//   - **A crossfade**: where the quality allows, a change is not a cut but a
//     quarter of a second in which both levels are drawn, each through the
//     other's holes of an ordered dither -- opaque throughout, so the depth
//     and the lighting are a solid mesh's, and the two never blend. On Low
//     and on a phone the change is a cut, made rarely by the hysteresis.
//   - **Distance by size** (`GraphicsService.ViewDistance`): a thing smaller
//     on the screen than `SmallestPixels`, divided by the view distance, is
//     not drawn.
//
// It decides the frame's levels before the frame is drawn, and writes them
// into its draws (`DrawItem::lodFixed`), which every pass then reads: the
// camera's, the shadows', the count of triangles.

#include <vector>

#include "engine/core/types.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/render/settings.h"

namespace engine::render {

struct LodTransitionStats
{
    // Draws whose level was kept from frame to frame, and those now between
    // two levels.
    core::u32 tracked = 0;
    core::u32 fading = 0;
    // Changes of level this frame, and draws left out for their size.
    core::u32 changed = 0;
    core::u32 tooSmall = 0;
};

class LodTransitions
{
public:
    // How much nearer or further a mesh is seen before its level changes.
    static constexpr core::f32 Hysteresis = 1.0f / 6.0f;
    // How long a crossfade lasts.
    static constexpr core::f64 FadeSeconds = 0.25;
    // How small, in pixels across, a thing is when it stops being drawn, at a
    // view distance of one.
    static constexpr core::f32 SmallestPixels = 2.0f;
    // A mesh unseen for this many frames is forgotten: it starts again at its
    // level, with no fade.
    static constexpr core::u32 ForgetFrames = 120;

    // One frame. `pixelsPerUnit` is what the renderer reads levels by
    // (`IRenderer::lodPixelsPerUnit`); `crossfade` whether a change fades
    // or cuts. Draws of the outgoing level are inserted beside the incoming
    // one's, drawn by the camera and casting no shadow.
    LodTransitionStats apply(RenderWorld& world, const MeshCache& meshes, core::f32 pixelsPerUnit,
                             const GraphicsSettings& settings, bool crossfade, core::f64 nowSeconds);

    void clear() noexcept { states_.clear(); }

private:
    struct State
    {
        core::u64 key = 0;
        core::u32 lod = 0;
        core::u32 from = 0;
        core::f64 since = -1.0;
        core::u64 seen = 0;
    };
    std::vector<State> states_;
    std::vector<DrawItem> scratch_;
    core::u64 frame_ = 0;
};

// **What a crossfading draw carries to its fragment** (ADR 0261), in the
// place of its opacity: the share of its pixels it keeps, `keep`, from 0 to
// 1, and whether it keeps the dither's lower values (the level coming in)
// or the upper (the level going out). An opaque draw's opacity is one, so
// anything from two up is a fade: `[2, 3)` keeps the lower share, `[4, 5)`
// the upper. Read by `engine_forward.hlsli`.
[[nodiscard]] constexpr core::f32 encodeLodFade(core::f32 keep, bool incoming) noexcept
{
    const core::f32 share = keep < 0.0f ? 0.0f : keep > 0.999f ? 0.999f : keep;
    return (incoming ? 2.0f : 4.0f) + share;
}

} // namespace engine::render
