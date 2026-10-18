// **The hands themselves, in a headset** (ADR 0245 §4): what the host makes of
// a hand tracked joint by joint, with no controller in it -- the palm where a
// controller would be held, and a pinch of thumb and index finger as a key.
// Apart from the frame loop so a test can drive it with made-up joints.
#pragma once

#include <array>

#include "engine/rhi/xr.h"

namespace engine::app {

// **A pinch**: the tips of the thumb and the index finger closer than this,
// and apart again only past the second -- a hand held at the edge does not
// flicker between the two.
inline constexpr core::f32 PinchClosedMetres = 0.02f;
inline constexpr core::f32 PinchOpenMetres = 0.035f;

// Whether each hand was pinching at the last frame.
struct HandPinches
{
    std::array<bool, 2> pinching{};
};

// **A frame's hands, into what the rest of the frame reads**: with `enabled`
// (`[xr] hands`), a hand tracked with no controller has its palm as its
// grip and its aim, and its pinch as its `XRPinch` key; without it, every
// joint is dropped and nothing pinches.
void readHands(rhi::HeadsetInput& read, HandPinches& pinches, bool enabled) noexcept;

} // namespace engine::app
