#include <doctest/doctest.h>

#include "engine/app/space_motion.h"

using engine::app::SpaceMotion;
using engine::app::spaceMotionCut;
using engine::core::DVec3;

TEST_CASE("the player's space moving fast and steadily is never a cut, and a teleport said is one")
{
    SpaceMotion motion;
    // The first frame has none before it.
    CHECK(spaceMotionCut(motion, DVec3{}, false));

    // A vehicle at 70 m/s, drawn at 36 frames a second: almost two metres a
    // frame, every frame, after a start that takes a second to reach it.
    for (int frame = 1; frame <= 36; ++frame)
        CHECK_FALSE(spaceMotionCut(motion, DVec3{70.0 / 36.0 * frame / 36.0, 0.0, 0.0}, false));
    for (int frame = 0; frame < 360; ++frame)
        CHECK_FALSE(spaceMotionCut(motion, DVec3{70.0 / 36.0, 0.0, 0.0}, false));

    // A teleport, said: a cut, and the step after it is measured from rest.
    CHECK(spaceMotionCut(motion, DVec3{0.0, 0.0, 12.0}, true));
    CHECK_FALSE(spaceMotionCut(motion, DVec3{}, false));
}

TEST_CASE("a space moved abruptly and not said is a cut by its jump against the step before")
{
    SpaceMotion motion;
    (void)spaceMotionCut(motion, DVec3{}, false);
    CHECK_FALSE(spaceMotionCut(motion, DVec3{0.02, 0.0, 0.0}, false));
    // Twelve metres from standing still in one frame.
    CHECK(spaceMotionCut(motion, DVec3{12.0, 0.0, 0.0}, false));
    // A car stopped dead from 32 m/s -- 0.9 m a frame to none -- is a sharp
    // change, and still motion the frames between can be made from.
    for (int frame = 0; frame < 10; ++frame)
        (void)spaceMotionCut(motion, DVec3{0.9, 0.0, 0.0}, false);
    CHECK_FALSE(spaceMotionCut(motion, DVec3{0.0, 0.0, 0.0}, false));
}
