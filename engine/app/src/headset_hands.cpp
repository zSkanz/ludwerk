#include "engine/app/headset_hands.h"

#include <cmath>

namespace engine::app {

void readHands(rhi::HeadsetInput& read, HandPinches& pinches, bool enabled) noexcept
{
    for (core::usize hand = 0; hand < 2; ++hand) {
        std::array<rhi::HeadsetPose, rhi::HandJointCount>& joints = read.joints[hand];
        if (!enabled)
            joints = {};
        const rhi::HeadsetPose& palm = joints[rhi::HandJointPalm];
        // A controller held is the hand: its grip stays, and the runtime
        // tracks no joints while it is held.
        if (palm.valid && !read.grip[hand].valid) {
            read.grip[hand] = palm;
            read.aim[hand] = palm;
        }
        const rhi::HeadsetPose& thumb = joints[rhi::HandJointThumbTip];
        const rhi::HeadsetPose& index = joints[rhi::HandJointIndexTip];
        bool& pinching = pinches.pinching[hand];
        if (!thumb.valid || !index.valid) {
            pinching = false;
        }
        else {
            const core::f32 dx = thumb.position[0] - index.position[0];
            const core::f32 dy = thumb.position[1] - index.position[1];
            const core::f32 dz = thumb.position[2] - index.position[2];
            const core::f32 apart = std::sqrt(dx * dx + dy * dy + dz * dz);
            pinching = pinching ? apart < PinchOpenMetres : apart < PinchClosedMetres;
        }
        read.keys[rhi::HeadsetPinchLeft + hand] = pinching ? 1.0f : 0.0f;
    }
}

} // namespace engine::app
