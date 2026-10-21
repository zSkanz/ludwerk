// Where a view hears from (ADR 0256 §5): the camera, the player's character a
// share of the way from it, or a place the game sets.
#include <doctest/doctest.h>
#include <optional>

#include "class_descriptors.gen.h"
#include "engine/app/listener_ear.h"
#include "engine/render/draw_poses.h"
#include "engine/render/scene_types.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"

using namespace engine;

namespace {

struct Stage
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> made;
    core::InstanceId player;
    core::InstanceId body;

    Stage()
    {
        scene::generated::registerClasses(classes, atoms);
        render::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        made.emplace(classes, enums, atoms, 1u);
        scene::World& world = *made;
        (void)world.create(world.classes().findId(world.atoms().intern("Workspace")));
        const core::InstanceId network = world.create(world.classes().findId(world.atoms().intern("NetworkService")));
        player = scene::createPlayer(world, network, 1, true);
        body = world.create(world.classes().findId(world.atoms().intern("Part")));
        world.parts().find(body)->cframe.position = core::DVec3{10.0, 0.0, -20.0};
        world.players().find(player)->character = body;
    }
};

// A camera thirty metres up, looking down at the origin.
[[nodiscard]] core::CFrameD aboveIt()
{
    return core::lookAtCFrame(core::DVec3{0.0, 30.0, 0.0}, core::DVec3{0.0, 0.0, -1.0}, core::Vec3{0.0f, 1.0f, 0.0f});
}

} // namespace

TEST_CASE("ADR 0256: a view hears from its camera until the game says otherwise")
{
    Stage stage;
    render::DrawPoses poses;
    poses.begin(*stage.made, nullptr, 1.0f);
    const core::CFrameD eye = aboveIt();
    CHECK(app::listenerEar(*stage.made, poses, eye, stage.player).position == eye.position);
}

TEST_CASE("ADR 0256: in Character mode the ear is the character's, a blend of the way from the camera")
{
    Stage stage;
    scene::EngineState& state = stage.made->engineState();
    state.listenerMode = 1;
    render::DrawPoses poses;
    poses.begin(*stage.made, nullptr, 1.0f);
    const core::CFrameD eye = aboveIt();

    const core::CFrameD at = app::listenerEar(*stage.made, poses, eye, stage.player);
    CHECK(at.position == core::DVec3{10.0, 0.0, -20.0});
    // Facing as the camera does: left on the screen is still left.
    CHECK(at.rotation == eye.rotation);

    state.listenerBlend = 0.8f;
    const core::CFrameD above = app::listenerEar(*stage.made, poses, eye, stage.player);
    CHECK(above.position.x == doctest::Approx(8.0));
    CHECK(above.position.y == doctest::Approx(6.0));
    CHECK(above.position.z == doctest::Approx(-16.0));

    // No character, no player: the camera.
    stage.made->players().find(stage.player)->character = core::InstanceId{};
    CHECK(app::listenerEar(*stage.made, poses, eye, stage.player).position == eye.position);
    CHECK(app::listenerEar(*stage.made, poses, eye, core::InstanceId{}).position == eye.position);
}

TEST_CASE("ADR 0256: in Custom mode the ear is where the game put it")
{
    Stage stage;
    scene::EngineState& state = stage.made->engineState();
    state.listenerMode = 2;
    state.listenerCFrame.position = core::DVec3{1.0, 2.0, 3.0};
    render::DrawPoses poses;
    poses.begin(*stage.made, nullptr, 1.0f);
    CHECK(app::listenerEar(*stage.made, poses, aboveIt(), stage.player).position == core::DVec3{1.0, 2.0, 3.0});
}
