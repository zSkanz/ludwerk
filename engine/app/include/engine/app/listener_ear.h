#pragma once

// **Where a view hears from** (ADR 0256 §5, `AudioService.ListenerMode`):
// its camera, its player's character, or the place the game set.

#include "engine/core/id.h"
#include "engine/core/math.h"

namespace engine::scene {
class World;
}
namespace engine::render {
class DrawPoses;
}

namespace engine::app {

// The ear of a view whose camera is at `eye` and whose player is `player`:
// `eye` itself in `Camera` mode; in `Character` mode the character's drawn
// place a `ListenerBlend` of the way from `eye`, facing as `eye` does -- or
// `eye`, when the player has no character that is a part; in `Custom` mode
// `AudioService.ListenerCFrame`.
[[nodiscard]] core::CFrameD listenerEar(const scene::World& world, const render::DrawPoses& poses,
                                        const core::CFrameD& eye, core::InstanceId player);

} // namespace engine::app
