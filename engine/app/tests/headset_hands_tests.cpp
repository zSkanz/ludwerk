#include <doctest/doctest.h>

#include "engine/app/headset_hands.h"

using namespace engine;

namespace {

// A right hand tracked with no controller: the palm 1.2 m up, the thumb's tip
// `apart` metres from the index finger's.
rhi::HeadsetInput handOpen(core::f32 apart)
{
    rhi::HeadsetInput read;
    for (rhi::HeadsetPose& joint : read.joints[1]) {
        joint.valid = true;
        joint.position = {0.2f, 1.2f, -0.3f};
    }
    read.joints[1][rhi::HandJointThumbTip].position = {0.2f, 1.2f, -0.3f};
    read.joints[1][rhi::HandJointIndexTip].position = {0.2f + apart, 1.2f, -0.3f};
    return read;
}

} // namespace

TEST_CASE("a hand tracked with no controller is held at its palm, and pinches as a key")
{
    app::HandPinches pinches;
    rhi::HeadsetInput read = handOpen(0.06f);
    app::readHands(read, pinches, true);
    // The palm stands for the controller the hand does not hold.
    CHECK(read.grip[1].valid);
    CHECK(static_cast<double>(read.grip[1].position[1]) == doctest::Approx(1.2));
    CHECK(read.aim[1].valid);
    CHECK_FALSE(read.grip[0].valid);
    CHECK(read.keys[rhi::HeadsetPinchLeft + 1] == 0.0f);

    // Closed: a pinch.
    read = handOpen(0.015f);
    app::readHands(read, pinches, true);
    CHECK(read.keys[rhi::HeadsetPinchLeft + 1] == 1.0f);
    CHECK(read.keys[rhi::HeadsetPinchLeft] == 0.0f);

    // Opened a little, past where it closed but short of where it lets go:
    // still a pinch.
    read = handOpen(0.03f);
    app::readHands(read, pinches, true);
    CHECK(read.keys[rhi::HeadsetPinchLeft + 1] == 1.0f);

    // Opened: let go.
    read = handOpen(0.05f);
    app::readHands(read, pinches, true);
    CHECK(read.keys[rhi::HeadsetPinchLeft + 1] == 0.0f);

    // And at the edge from open, not yet a pinch.
    read = handOpen(0.03f);
    app::readHands(read, pinches, true);
    CHECK(read.keys[rhi::HeadsetPinchLeft + 1] == 0.0f);
}

TEST_CASE("a hand holding its controller keeps the controller's grip")
{
    app::HandPinches pinches;
    rhi::HeadsetInput read = handOpen(0.06f);
    read.grip[1].valid = true;
    read.grip[1].position = {0.5f, 1.0f, 0.0f};
    app::readHands(read, pinches, true);
    CHECK(static_cast<double>(read.grip[1].position[0]) == doctest::Approx(0.5));
}

TEST_CASE("with [xr] hands off, the joints are dropped and nothing pinches")
{
    app::HandPinches pinches;
    rhi::HeadsetInput read = handOpen(0.01f);
    app::readHands(read, pinches, false);
    CHECK_FALSE(read.joints[1][rhi::HandJointPalm].valid);
    CHECK_FALSE(read.grip[1].valid);
    CHECK(read.keys[rhi::HeadsetPinchLeft + 1] == 0.0f);
}
