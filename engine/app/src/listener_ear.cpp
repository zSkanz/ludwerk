#include "engine/app/listener_ear.h"

#include <algorithm>

#include "engine/render/draw_poses.h"
#include "engine/scene/world.h"

namespace engine::app {

core::CFrameD listenerEar(const scene::World& world, const render::DrawPoses& poses, const core::CFrameD& eye,
                          core::InstanceId player)
{
    const scene::EngineState& state = world.engineState();
    // `Enum.ListenerMode`: Camera 0, Character 1, Custom 2.
    if (state.listenerMode == 2)
        return state.listenerCFrame;
    if (state.listenerMode != 1)
        return eye;
    const scene::PlayerComponent* who = player.valid() ? world.players().find(player) : nullptr;
    if (who == nullptr || !who->character.valid() || world.parts().find(who->character) == nullptr)
        return eye;
    // Where it is drawn this frame, not where the last tick left it: the ear
    // glides with the character as the picture does.
    const core::DVec3 body = poses.part(who->character).position;
    const core::f64 blend = std::clamp(static_cast<core::f64>(state.listenerBlend), 0.0, 1.0);
    core::CFrameD ear = eye;
    ear.position = core::DVec3{eye.position.x + (body.x - eye.position.x) * blend,
                               eye.position.y + (body.y - eye.position.y) * blend,
                               eye.position.z + (body.z - eye.position.z) * blend};
    return ear;
}

} // namespace engine::app
