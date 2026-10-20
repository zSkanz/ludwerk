#pragma once

// **When the player's space moving is a cut** (ADR 0252 §6): a frame a
// headset's runtime makes nothing from, because what it would make the
// frames between from -- the space's step since the frame before -- is not
// how it moved.
//
// A cut is said, not guessed from how far the space went: a kart at 250 km/h
// steps seven metres a drawn frame at 36, every frame, and a rule on the
// distance made each of them a teleport -- half the rate exactly when the
// frames between matter. `XRService:MarkCut()` says one (`@engine/xrcomfort`
// does for its teleport and its snap turn), and so does a scene changed.
// What is left to notice is a game that moves the space abruptly and says
// nothing: a step that jumps against the step before it -- a change of
// velocity no vehicle makes in a frame -- and not a step that is merely long.

#include "engine/core/math.h"

namespace engine::app {

// A change in the space's step from one drawn frame to the next, in metres,
// past which the step is a jump: a metre a frame's change of a frame's step,
// at 36 frames a second, is over a thousand metres a second squared.
inline constexpr core::f64 SpaceJumpMetres = 1.0;

struct SpaceMotion
{
    core::DVec3 lastStep{};
    bool known = false;
};

// Whether this frame's step is a cut, given whether one was said; and the
// step remembered for the next. The first step known is a cut: there is no
// frame before it to make anything from.
[[nodiscard]] inline bool spaceMotionCut(SpaceMotion& motion, const core::DVec3& step, bool said) noexcept
{
    const core::DVec3 change = step - motion.lastStep;
    const bool jumped =
        change.x * change.x + change.y * change.y + change.z * change.z > SpaceJumpMetres * SpaceJumpMetres;
    const bool cut = said || !motion.known || jumped;
    // After a cut the next step is measured against standing still, as a
    // teleport leaves the player: the step that carried the cut is not motion.
    motion.lastStep = cut ? core::DVec3{} : step;
    motion.known = true;
    return cut;
}

} // namespace engine::app
