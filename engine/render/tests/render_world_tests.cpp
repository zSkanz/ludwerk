#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <doctest/doctest.h>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "engine/asset/material.h"
#include "engine/core/types.h"
#include "engine/render/render_world.h"
#include "engine/render/terrain_loader.h"
#include "engine/render/transform_history.h"
#include "engine/render/water_loader.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/water.h"
#include "engine/scene/world.h"
#include "engine_test_nearly.h"

using namespace engine;

namespace {

// These cases are all about the debug-part path, which needs no meshes and no
// camera. One shared empty library says that once rather than at every call.
const render::MeshLibrary kNoMeshes;

using engine::testing::nearly;

// **A part's colour and see-through are the engine default material's two
// parameters** (ADR 0090): these write them as overrides, which is what a part
// with no material has instead of `BasePart.Color` and `BasePart.Transparency`.
void setColorOverride(scene::World& world, core::InstanceId id, core::Color3 colour)
{
    asset::MaterialProperties values;
    values.color = colour;
    REQUIRE(world.parts().find(id) != nullptr);
    (void)asset::setOverride(world.parts().find(id)->materialParameters, asset::MaterialField::Color, values);
}

void setTransparencyOverride(scene::World& world, core::InstanceId id, core::f32 transparency)
{
    asset::MaterialProperties values;
    values.transparency = transparency;
    REQUIRE(world.parts().find(id) != nullptr);
    (void)asset::setOverride(world.parts().find(id)->materialParameters, asset::MaterialField::Transparency, values);
}

// A hierarchy with just enough in it to have a root and a part. Hand-built
// rather than generated, because `render` must not depend on the API definition
// files to be testable -- that would make a rendering test fail for a reason
// that has nothing to do with rendering.
struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;

    scene::ClassId instanceClass = scene::InvalidClass;
    scene::ClassId folderClass = scene::InvalidClass;
    scene::ClassId partClass = scene::InvalidClass;
    // The materials the world's parts wear (ADR 0090), borrowed by the world.
    asset::MaterialLibrary materials;

    Fixture()
    {
        scene::ClassDescriptor instance;
        instance.name = atoms.intern("Instance");
        instance.defaultName = instance.name;
        instanceClass = classes.registerClass(instance);

        scene::ClassDescriptor folder;
        folder.name = atoms.intern("Folder");
        folder.super = instanceClass;
        folder.defaultName = folder.name;
        folderClass = classes.registerClass(folder);

        scene::ClassDescriptor part;
        part.name = atoms.intern("Part");
        part.super = instanceClass;
        part.defaultName = part.name;
        part.attachComponents = [](scene::World& w, core::InstanceId id) { w.parts().add(id, scene::PartComponent{}); };
        part.detachComponents = [](scene::World& w, core::InstanceId id) { w.parts().remove(id); };
        partClass = classes.registerClass(part);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    scene::World world{classes, enums, atoms, 1u};

    [[nodiscard]] core::InstanceId part(core::InstanceId parent)
    {
        const core::InstanceId id = world.create(partClass);
        (void)world.setParent(id, parent);
        return id;
    }

    // The M4 half of the fixture. Registered lazily so the cases above stay
    // exactly the world they were written against: a Workspace that carries a
    // camera reference changes what `extract` does on its first line.
    scene::ClassId workspaceClass = scene::InvalidClass;
    scene::ClassId cameraClass = scene::InvalidClass;
    scene::ClassId meshPartClass = scene::InvalidClass;
    scene::ClassId pointLightClass = scene::InvalidClass;

    void registerRenderClasses()
    {
        scene::ClassDescriptor workspace;
        workspace.name = atoms.intern("Workspace");
        workspace.super = instanceClass;
        workspace.defaultName = workspace.name;
        workspace.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.workspaces().add(id, scene::WorkspaceComponent{});
        };
        workspace.detachComponents = [](scene::World& w, core::InstanceId id) { w.workspaces().remove(id); };
        workspaceClass = classes.registerClass(workspace);

        scene::ClassDescriptor camera;
        camera.name = atoms.intern("Camera");
        camera.super = instanceClass;
        camera.defaultName = camera.name;
        camera.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.cameras().add(id, scene::CameraComponent{});
        };
        camera.detachComponents = [](scene::World& w, core::InstanceId id) { w.cameras().remove(id); };
        cameraClass = classes.registerClass(camera);

        scene::ClassDescriptor meshPart;
        meshPart.name = atoms.intern("MeshPart");
        meshPart.super = partClass;
        meshPart.defaultName = meshPart.name;
        meshPart.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.meshParts().add(id, scene::MeshPartComponent{});
        };
        meshPart.detachComponents = [](scene::World& w, core::InstanceId id) { w.meshParts().remove(id); };
        meshPartClass = classes.registerClass(meshPart);

        scene::ClassDescriptor pointLight;
        pointLight.name = atoms.intern("PointLight");
        pointLight.super = instanceClass;
        pointLight.defaultName = pointLight.name;
        pointLight.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.pointLights().add(id, scene::PointLightComponent{});
        };
        pointLight.detachComponents = [](scene::World& w, core::InstanceId id) { w.pointLights().remove(id); };
        pointLightClass = classes.registerClass(pointLight);
    }

    // A camera at the origin looking down -Z, which is the direction api-design
    // gives LookVector, with a 90-degree vertical field of view so the frustum's
    // side planes sit at 45 degrees and every expectation below is arithmetic.
    [[nodiscard]] core::InstanceId cameraLookingDownNegativeZ(core::InstanceId workspaceId, core::DVec3 at = {})
    {
        const core::InstanceId id = world.create(cameraClass);
        (void)world.setParent(id, workspaceId);
        scene::CameraComponent* component = world.cameras().find(id);
        REQUIRE(component != nullptr);
        component->cframe.position = at;
        component->fieldOfView = 90.0f;
        component->nearPlane = 1.0f;
        component->farPlane = 100.0f;
        world.workspaces().find(workspaceId)->currentCamera = id;
        return id;
    }

    [[nodiscard]] core::InstanceId meshPartAt(core::InstanceId parent, core::DVec3 at, core::NameAtom content)
    {
        const core::InstanceId id = world.create(meshPartClass);
        (void)world.setParent(id, parent);
        world.parts().find(id)->cframe.position = at;
        world.meshParts().find(id)->meshContent = content;
        return id;
    }
};

} // namespace

TEST_CASE("extraction copies what rendering needs and nothing that can go stale")
{
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);

    const core::InstanceId part = fixture.part(root);
    scene::PartComponent* component = fixture.world.parts().find(part);
    REQUIRE(component != nullptr);
    component->cframe.position = core::DVec3{1.0, 2.0, 3.0};
    component->size = core::Vec3{4.0f, 5.0f, 6.0f};
    setColorOverride(fixture.world, part, core::Color3{0.25f, 0.5f, 0.75f});
    setTransparencyOverride(fixture.world, part, 0.5f);
    component->shape = 2;

    render::RenderWorld snapshot;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    REQUIRE(snapshot.parts.size() == 1);
    CHECK(snapshot.parts[0].cframe.position.x == 1.0);
    CHECK(snapshot.parts[0].size == core::Vec3{4.0f, 5.0f, 6.0f});
    CHECK(snapshot.parts[0].color == core::Color3{0.25f, 0.5f, 0.75f});
    CHECK(snapshot.parts[0].transparency == 0.5f);
    CHECK(snapshot.parts[0].shape == 2);
}

TEST_CASE("only what is under the root is in the world")
{
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    const core::InstanceId elsewhere = fixture.world.create(fixture.folderClass);

    const core::InstanceId inside = fixture.part(root);
    const core::InstanceId nested = fixture.part(inside);
    (void)fixture.part(elsewhere);
    (void)fixture.world.create(fixture.partClass); // unparented
    (void)nested;

    render::RenderWorld snapshot;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    // Whatever is parented under the root is in the world and whatever is not,
    // is not -- at any depth.
    CHECK(snapshot.parts.size() == 2);
}

TEST_CASE("an invalid root extracts nothing rather than everything")
{
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    (void)fixture.part(root);

    render::RenderWorld snapshot;
    render::extract(fixture.world, {}, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    // The failure mode this guards is a boot that has not created `Workspace`
    // yet drawing the whole world, unparented instances included.
    CHECK(snapshot.parts.empty());
}

TEST_CASE("extraction clears first, so one buffer serves every frame")
{
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    (void)fixture.part(root);

    render::RenderWorld snapshot;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    CHECK(snapshot.parts.size() == 1);

    fixture.world.destroy(root);
    fixture.world.retireDestroyed();
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    // A retired root is not a root: nothing resolves through it, so nothing is
    // in the world.
    CHECK(snapshot.parts.empty());
}

TEST_CASE("drawSortKey: pass, then pipeline, then material, then mesh, then depth")
{
    // Asserted on the key rather than on a sorted list, because a sorted list
    // only proves that *something* was consistent. These are the ordering
    // contract every backend inherits.
    CHECK(render::drawSortKey(0, 9, 9, 9, 600.0f) < render::drawSortKey(1, 0, 0, 0, 0.0f));
    CHECK(render::drawSortKey(0, 0, 9, 9, 600.0f) < render::drawSortKey(0, 1, 0, 0, 0.0f));
    CHECK(render::drawSortKey(0, 0, 0, 9, 600.0f) < render::drawSortKey(0, 0, 1, 0, 0.0f));

    // The geometry field, added at M7.5, and the property that makes an
    // instanced run possible at all: two pieces of geometry sharing one material
    // must NOT interleave by depth, or a run of one is chopped into pieces by
    // draws of the other (ADR 0043).
    CHECK(render::drawSortKey(0, 0, 0, 0, 600.0f) < render::drawSortKey(0, 0, 0, 1, 0.0f));

    // And SECTION is part of it, which is the half that was missing and was
    // caught by measuring rather than by reading: a mesh with two sections and
    // one material interleaved its halves by depth, so every run was one draw
    // long and the instanced path drew nothing.
    CHECK(render::drawGeometryKey(7, 0) != render::drawGeometryKey(7, 1));
    CHECK(render::drawGeometryKey(7, 1) < render::drawGeometryKey(8, 0));
    CHECK(render::drawGeometryKey(4095, 15) <= 0xFFFFu);
    CHECK(render::drawSortKey(0, 0, 0, 0, 1.0f) < render::drawSortKey(0, 0, 0, 0, 2.0f));

    // Quantized, so a sub-millimetre camera wobble cannot reorder two draws and
    // change a golden command stream. One unit is a centimetre.
    CHECK(render::drawSortKey(0, 0, 0, 0, 1.0f) == render::drawSortKey(0, 0, 0, 0, 1.000001f));
    CHECK(render::drawSortKey(0, 0, 0, 0, 1.0f) != render::drawSortKey(0, 0, 0, 0, 1.02f));

    // Saturating rather than wrapping: a draw beyond the quantization range must
    // sort last, not first.
    CHECK(render::drawSortKey(0, 0, 0, 0, 5000.0f) >= render::drawSortKey(0, 0, 0, 0, 654.0f));
    CHECK(render::drawSortKey(0, 0, 0, 0, -5.0f) == render::drawSortKey(0, 0, 0, 0, 0.0f));

    // Each field is bounded, and a value past its width must not climb into the
    // one above it -- a material index of 65,536 that reordered the passes would
    // be a scene drawn in the wrong order for a reason nobody could see.
    CHECK(render::drawSortKey(0, 0, 65536, 0, 0.0f) < render::drawSortKey(0, 1, 0, 0, 0.0f));
    CHECK(render::drawSortKey(0, 0, 0, 65536, 0.0f) < render::drawSortKey(0, 0, 1, 0, 0.0f));
}

TEST_CASE("extraction resolves the camera, and answers nothing without one")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);

    render::RenderWorld snapshot;

    SUBCASE("no camera means no view, and therefore no draws")
    {
        render::extract(fixture.world, workspace, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        CHECK_FALSE(snapshot.camera.valid);
        CHECK(snapshot.draws.empty());
    }

    SUBCASE("a destroyed camera is the same as no camera")
    {
        const core::InstanceId camera = fixture.cameraLookingDownNegativeZ(workspace);
        fixture.world.destroy(camera);
        render::extract(fixture.world, workspace, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        // The failure this guards is a renderer drawing through a camera whose
        // instance was retired, which is a stale InstanceId reaching the GPU.
        CHECK_FALSE(snapshot.camera.valid);
    }

    SUBCASE("the origin is the camera position, so the GPU never sees a world coordinate")
    {
        (void)fixture.cameraLookingDownNegativeZ(workspace, core::DVec3{1000000.0, 0.0, 0.0});
        render::extract(fixture.world, workspace, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.camera.valid);
        // ADR 0014's whole point: a million metres out, the matrices are still
        // small numbers, because the camera sits at the origin of its own space.
        CHECK(snapshot.camera.origin.x == 1000000.0);
        CHECK(snapshot.camera.view.m[3][0] == 0.0f);
        CHECK(snapshot.camera.view.m[3][1] == 0.0f);
        CHECK(snapshot.camera.view.m[3][2] == 0.0f);
    }
}

TEST_CASE("extraction culls what the camera cannot see, and keeps what it can")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    meshes.set(content, entry);

    render::RenderWorld snapshot;

    SUBCASE("in front of the camera is drawn")
    {
        (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        CHECK(snapshot.draws.size() == 1);
        CHECK(snapshot.candidateDraws == 1);
        CHECK(snapshot.culledDraws == 0);
    }

    SUBCASE("behind the camera is culled, and counted as culled")
    {
        (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, 10.0}, content);
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        CHECK(snapshot.draws.empty());
        // The counter is what makes "the culler did something" assertable. A
        // frame where candidates and culls are both zero is a frame the culler
        // never ran on, which passes an emptiness check and proves nothing.
        CHECK(snapshot.candidateDraws == 1);
        CHECK(snapshot.culledDraws == 1);
    }

    SUBCASE("a mesh nothing has loaded is skipped rather than drawn as a placeholder")
    {
        const core::NameAtom absent = fixture.atoms.intern("asset://absent.glb");
        (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, absent);
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        CHECK(snapshot.draws.empty());
        // Not counted as a candidate either: nothing was ever a draw.
        CHECK(snapshot.candidateDraws == 0);
        CHECK(snapshot.culledDraws == 0);
    }
}

TEST_CASE("extraction orders draws near to far, stably")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    meshes.set(content, entry);

    // Created far-to-near, so a snapshot that merely preserved creation order
    // would come out backwards.
    (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -50.0}, content);
    (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);
    (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -30.0}, content);

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    REQUIRE(snapshot.draws.size() == 3);
    CHECK(snapshot.draws[0].sortKey < snapshot.draws[1].sortKey);
    CHECK(snapshot.draws[1].sortKey < snapshot.draws[2].sortKey);
    CHECK(nearly(snapshot.draws[0].transform.m[3][2], -10.0f));
    CHECK(nearly(snapshot.draws[2].transform.m[3][2], -50.0f));

    // R10: the same world extracted twice gives the same order, every time.
    render::RenderWorld again;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, again);
    REQUIRE(again.draws.size() == snapshot.draws.size());
    for (std::size_t index = 0; index < again.draws.size(); ++index)
        CHECK(again.draws[index].sortKey == snapshot.draws[index].sortKey);
}

TEST_CASE("extraction uploads a shared pose once and keeps independent rigs and overrides separate")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const auto root = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(root);
    const auto content = fixture.atoms.intern("asset://models/shared-rig.glb");
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry mesh;
    mesh.mesh = render::MeshHandle{0, 1};
    mesh.bounds = core::AABB::fromCenterSize({}, {1.0f, 1.0f, 1.0f});
    mesh.sectionCount = 2;
    meshes.set(content, mesh);

    render::SkeletonLibrary skeletons;
    render::SkeletonLibrary::Entry skeleton;
    skeleton.joints.resize(2);
    skeleton.joints[0].name = "root";
    skeleton.joints[1].name = "child";
    skeleton.joints[1].parent = 0;
    asset::AnimationChannel channel;
    channel.joint = 1;
    channel.target = asset::AnimationChannel::Target::Translation;
    channel.stride = 3;
    channel.times = {0.0f, 1.0f};
    channel.values = {0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f};
    asset::AnimationClip clip;
    clip.name = "Slide";
    clip.duration = 1.0f;
    clip.channels.push_back(channel);
    skeleton.clips.push_back(clip);
    skeletons.set(content, std::move(skeleton));
    render::AnimationSystem animation{fixture.world, skeletons};
    std::array<core::InstanceId, 4> rigs{};
    for (core::usize index = 0; index < rigs.size(); ++index) {
        rigs[index] = fixture.meshPartAt(root, {static_cast<core::f64>(index * 2), 0.0, -10.0}, content);
        if (index == 3)
            continue; // This rig stays in bind pose.
        const auto player = fixture.world.create(fixture.instanceClass);
        REQUIRE_FALSE(fixture.world.setParent(player, rigs[index]).has_value());
        const auto track = animation.createTrack(player, {}, "Slide");
        animation.play(track, 0.0f, 1.0f, index == 2 ? 2.0f : 1.0f);
    }
    animation.sample(0.25);
    REQUIRE(animation.pose(rigs[0]) == animation.pose(rigs[1]));
    REQUIRE(animation.pose(rigs[0]) != animation.pose(rigs[2]));

    render::RenderWorld snapshot;
    const auto checkSnapshot = [&] {
        render::extract(fixture.world, root, {}, meshes, 1.0f, 0.0f, &animation, 0.0f, nullptr, snapshot);
        REQUIRE(snapshot.draws.size() == 8);
        for (const auto& draw : snapshot.draws) {
            const auto index = static_cast<core::usize>(draw.transform.m[3][0] / 2.0f);
            REQUIRE(index < rigs.size());
            const auto* pose = animation.pose(rigs[index]);
            if (pose == nullptr) {
                CHECK(draw.boneCount == 0);
                continue;
            }
            REQUIRE(draw.boneCount == 2);
            REQUIRE(draw.firstBone + draw.boneCount <= snapshot.bones.size());
            for (core::usize joint = 0; joint < draw.boneCount; ++joint)
                for (core::usize column = 0; column < 4; ++column)
                    for (core::usize row = 0; row < 4; ++row)
                        CHECK(snapshot.bones[draw.firstBone + joint].m[column][row] ==
                              pose->palette[joint].m[column][row]);
        }
    };
    checkSnapshot();
    CHECK(snapshot.bones.size() == 4); // Two poses, not three rigs or six sections.
    animation.sample(0.125);
    checkSnapshot();
    CHECK(snapshot.bones.size() == 4); // No offsets from the previous extract survive.
    core::CFrameD overridden;
    overridden.position.y = 9.0;
    animation.setJointOverride(rigs[0], 0, overridden);
    animation.commitOverrides();
    checkSnapshot();
    CHECK(snapshot.bones.size() == 6); // The overridden rig owns its palette.
}

TEST_CASE("extraction reads the environment from Lighting, and defaults without it")
{
    Fixture fixture;
    fixture.registerRenderClasses();

    scene::ClassDescriptor lighting;
    lighting.name = fixture.atoms.intern("Lighting");
    lighting.super = fixture.instanceClass;
    lighting.defaultName = lighting.name;
    lighting.attachComponents = [](scene::World& w, core::InstanceId id) {
        w.lighting().add(id, scene::LightingComponent{});
    };
    lighting.detachComponents = [](scene::World& w, core::InstanceId id) { w.lighting().remove(id); };
    const scene::ClassId lightingClass = fixture.classes.registerClass(lighting);

    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    const core::InstanceId host = fixture.world.create(lightingClass);
    scene::LightingComponent* component = fixture.world.lighting().find(host);
    REQUIRE(component != nullptr);
    component->clockTime = 6.0f;
    component->geographicLatitude = 0.0f;

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, host, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    // Six in the morning at the equator: the sun is due east, which is +X.
    CHECK(nearly(snapshot.environment.sunDirection.x, 1.0f));
    CHECK(nearly(snapshot.environment.sunDirection.y, 0.0f));

    // An engine with no render module registers no Lighting at all, and an
    // invalid host must read as "use the defaults" rather than as an error.
    render::RenderWorld without;
    render::extract(fixture.world, workspace, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                    without);
    CHECK(nearly(without.environment.sunDirection.y, 1.0f));
}

TEST_CASE("Transparency reaches the draw, picks the pass, and reverses the sort")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    meshes.set(content, entry);

    render::RenderWorld snapshot;

    SUBCASE("an opaque part is in the opaque pass with alpha one")
    {
        (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.draws.size() == 1);
        CHECK_FALSE(snapshot.draws[0].transparent);
        CHECK(nearly(snapshot.draws[0].alpha, 1.0f));
        CHECK((snapshot.draws[0].sortKey >> 56) == render::kOpaquePass);
    }

    SUBCASE("Transparency reaches the draw as one minus itself")
    {
        const core::InstanceId id = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);
        setTransparencyOverride(fixture.world, id, 0.25f);
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.draws.size() == 1);
        CHECK(snapshot.draws[0].transparent);
        CHECK(nearly(snapshot.draws[0].alpha, 0.75f));
        CHECK((snapshot.draws[0].sortKey >> 56) == render::kTransparentPass);
    }

    SUBCASE("the material's own alpha multiplies with the part's")
    {
        render::MeshLibrary::Entry translucent = entry;
        translucent.sectionMaterial = {0};
        render::RenderMaterial material;
        material.uniforms.baseColor[3] = 0.5f;
        translucent.materials = {material};
        render::MeshLibrary library;
        library.set(content, translucent);

        const core::InstanceId id = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);
        setTransparencyOverride(fixture.world, id, 0.5f);
        render::extract(fixture.world, workspace, core::InstanceId{}, library, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.draws.size() == 1);
        // A glTF material can be see-through on its own and a script can make an
        // opaque mesh see-through; honouring one and not the other leaves a case
        // that renders wrong, so the two multiply.
        CHECK(nearly(snapshot.draws[0].alpha, 0.25f));
        CHECK(snapshot.draws[0].transparent);
    }

    SUBCASE("a fully transparent part is not drawn at all")
    {
        const core::InstanceId id = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);
        setTransparencyOverride(fixture.world, id, 1.0f);
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        // Neither pass, and the debug path's existing rule: `submitWorld` skips
        // a part at `transparency >= 1`. A shadow cast by something nobody can
        // see is a defect whoever sees it reports.
        CHECK(snapshot.draws.empty());
    }

    SUBCASE("the transparent pass runs after the opaque one and sorts far to near")
    {
        (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -40.0}, content);
        const core::InstanceId near = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);
        const core::InstanceId far = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -30.0}, content);
        setTransparencyOverride(fixture.world, near, 0.4f);
        setTransparencyOverride(fixture.world, far, 0.6f);

        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.draws.size() == 3);

        // Opaque first, because the blended pass tests against the depth the
        // opaque one wrote.
        CHECK_FALSE(snapshot.draws[0].transparent);
        CHECK(snapshot.draws[1].transparent);
        CHECK(snapshot.draws[2].transparent);

        // And back to front within it, which is the opposite of the opaque
        // order. Read off the transform rather than off the key, so this fails
        // if the inversion is dropped even though the keys stay ordered.
        CHECK(nearly(snapshot.draws[1].transform.m[3][2], -30.0f));
        CHECK(nearly(snapshot.draws[2].transform.m[3][2], -10.0f));
    }
}

TEST_CASE("Size over MeshSize is what a mesh is drawn at")
{
    // What makes `Size` mean the same thing on a `MeshPart` as it does on a
    // `Part`. `MeshSize` is what the mesh measures as authored, so the ratio is
    // the stretch, and the CULL BOX has to be built from the scaled matrix or a
    // stretched mesh gets culled by the bounds of the unstretched one.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    const core::InstanceId id = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);

    render::MeshLibrary meshes;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    meshes.set(content, entry);

    SUBCASE("both at one, which is every scene written before this existed")
    {
        render::RenderWorld snapshot;
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.draws.size() == 1);
        // The identity, exactly: the multiply is skipped rather than done with
        // ones, so the matrix is bit-for-bit what it was.
        CHECK(snapshot.draws[0].transform.m[0][0] == 1.0f);
        CHECK(snapshot.draws[0].transform.m[1][1] == 1.0f);
        CHECK(snapshot.draws[0].transform.m[2][2] == 1.0f);
    }

    SUBCASE("a part twice its mesh's size draws twice as big")
    {
        fixture.world.parts().find(id)->size = core::Vec3{4.0f, 6.0f, 2.0f};
        fixture.world.meshParts().find(id)->meshSize = core::Vec3{2.0f, 2.0f, 4.0f};

        render::RenderWorld snapshot;
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.draws.size() == 1);
        CHECK(snapshot.draws[0].transform.m[0][0] == 2.0f);
        CHECK(snapshot.draws[0].transform.m[1][1] == 3.0f);
        CHECK(snapshot.draws[0].transform.m[2][2] == 0.5f);
        // And the translation is untouched: a stretch about the part's own
        // origin, not a move.
        CHECK(snapshot.draws[0].transform.m[3][2] == -10.0f);
    }
}

TEST_CASE("a MeshPart's wire box appears only while its mesh has not loaded")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    (void)fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);

    render::RenderWorld snapshot;

    SUBCASE("nothing loaded: the box is the only sign the part exists")
    {
        render::extract(fixture.world, workspace, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        CHECK(snapshot.parts.size() == 1);
        CHECK(snapshot.draws.empty());
    }

    SUBCASE("loaded: the real geometry replaces it")
    {
        render::MeshLibrary meshes;
        render::MeshLibrary::Entry entry;
        entry.mesh = render::MeshHandle{0, 1};
        entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
        entry.sectionCount = 1;
        meshes.set(content, entry);

        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        // No wire box: the real geometry is on screen, and a box drawn from
        // `Size` over it would be a second, differently-shaped outline of the
        // same thing. (`Size` DOES scale the mesh now -- `Size / MeshSize` --
        // but the box would still be the part's box and not the mesh's.)
        CHECK(snapshot.parts.empty());
        CHECK(snapshot.draws.size() == 1);
    }

    SUBCASE("an ordinary Part still gets its box either way")
    {
        const core::InstanceId plain = fixture.world.create(fixture.partClass);
        (void)fixture.world.setParent(plain, workspace);
        render::extract(fixture.world, workspace, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        CHECK(snapshot.parts.size() == 2);
    }
}

// --- Render interpolation (D047, architecture.md §3) -------------------------
//
// The simulation is a fixed 60 Hz and a display is not, so a frame between two
// ticks has to be drawn between two states or the world steps while the camera
// does not. `Frame::alpha` has existed since M1 and had no consumer until M8.

TEST_CASE("a frame between two ticks is drawn between two states")
{
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    const core::InstanceId part = fixture.part(root);

    scene::PartComponent* component = fixture.world.parts().find(part);
    REQUIRE(component != nullptr);
    component->cframe.position = core::DVec3{0.0, 0.0, 0.0};
    // Twenty metres long, so ten metres in a tick is a glide and not a
    // teleport (see the case after this one).
    component->size = core::Vec3{20.0f, 1.0f, 1.0f};

    // The tick boundary: capture where it is, then move it, exactly as the
    // frame loop does.
    render::TransformHistory history;
    history.capture(fixture.world);
    component->cframe.position = core::DVec3{10.0, 0.0, 0.0};

    render::RenderWorld halfway;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.5f, &history, halfway);
    REQUIRE(halfway.parts.size() == 1);
    CHECK(nearly(static_cast<core::f32>(halfway.parts[0].cframe.position.x), 5.0f));

    // And the two ends are the two ticks themselves: zero the one before,
    // which the frame just short of one reached from the other side (D253).
    render::RenderWorld atTick;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, &history, atTick);
    CHECK(atTick.parts[0].cframe.position.x == 0.0);

    render::RenderWorld nextTick;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 1.0f, &history, nextTick);
    CHECK(nearly(static_cast<core::f32>(nextTick.parts[0].cframe.position.x), 10.0f));
}

TEST_CASE("a part that jumped further than its own size in a tick is drawn where it landed")
{
    // **The owner's snake**: its tail is moved to the front of its head with
    // one `CFrame` write, and the frames between two ticks drew it sliding
    // through the body to get there. A move longer than the part's largest
    // side is a teleport; one shorter still glides.
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    const core::InstanceId part = fixture.part(root);
    scene::PartComponent* component = fixture.world.parts().find(part);
    REQUIRE(component != nullptr);
    component->size = core::Vec3{1.0f, 1.0f, 1.0f};
    component->cframe.position = core::DVec3{0.0, 0.0, 0.0};

    render::TransformHistory history;
    history.capture(fixture.world);
    component->cframe.position = core::DVec3{3.0, 0.0, 0.0};

    render::RenderWorld jumped;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.5f, &history, jumped);
    REQUIRE(jumped.parts.size() == 1);
    CHECK(jumped.parts[0].cframe.position.x == 3.0);

    // Under its own size, it glides.
    history.capture(fixture.world);
    component->cframe.position = core::DVec3{3.5, 0.0, 0.0};
    render::RenderWorld glided;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.5f, &history, glided);
    CHECK(nearly(static_cast<core::f32>(glided.parts[0].cframe.position.x), 3.25f));
}

TEST_CASE("no history is the world exactly as the last tick left it")
{
    // **The property every golden in this repository depends on.** A headless
    // run drives one fixed step per frame and passes zero, so nothing it records
    // can move by a fraction of a tick.
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    const core::InstanceId part = fixture.part(root);

    scene::PartComponent* component = fixture.world.parts().find(part);
    REQUIRE(component != nullptr);
    component->cframe.position = core::DVec3{0.0, 0.0, 0.0};

    render::TransformHistory history;
    history.capture(fixture.world);
    component->cframe.position = core::DVec3{10.0, 0.0, 0.0};

    render::RenderWorld withoutHistory;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.5f, nullptr,
                    withoutHistory);
    CHECK(withoutHistory.parts[0].cframe.position.x == 10.0);
}

TEST_CASE("something that has just arrived is drawn where it is, not smeared in from nowhere")
{
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);

    // The capture happens BEFORE the part exists, which is what a chunk
    // streaming in looks like: there is no previous position to come from, and
    // interpolating from a stale slot would drag it across the world.
    render::TransformHistory history;
    history.capture(fixture.world);

    const core::InstanceId part = fixture.part(root);
    scene::PartComponent* component = fixture.world.parts().find(part);
    REQUIRE(component != nullptr);
    component->cframe.position = core::DVec3{600.0, 0.0, 0.0};

    render::RenderWorld snapshot;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.5f, &history, snapshot);
    REQUIRE(snapshot.parts.size() == 1);
    CHECK(snapshot.parts[0].cframe.position.x == 600.0);
}

TEST_CASE("a slot reused by a different instance has no history")
{
    // The generation check, and it is not theoretical: the ECS reclaims slots,
    // so a part destroyed and another created can land on the same index within
    // one tick -- and the new one would inherit the old one's position.
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    const core::InstanceId first = fixture.part(root);
    scene::PartComponent* component = fixture.world.parts().find(first);
    REQUIRE(component != nullptr);
    component->cframe.position = core::DVec3{5.0, 0.0, 0.0};

    render::TransformHistory history;
    history.capture(fixture.world);

    const core::InstanceId stale{first.index, first.generation + 1};
    CHECK(history.previous(stale) == nullptr);
    CHECK(history.previous(first) != nullptr);
}

TEST_CASE("a corrected character's last place moves with it, and so does everything on it (ADR 0134)")
{
    // A correction moves a replica's own character, and its slide draws it
    // where it was; its last captured place has to move by as much, or the
    // step between ticks counts the correction a second time. Only its
    // subtree: another part keeps its own last place.
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    const core::InstanceId body = fixture.part(root);
    const core::InstanceId hat = fixture.part(body);
    const core::InstanceId crate = fixture.part(root);
    for (const core::InstanceId id : {body, hat, crate})
        fixture.world.parts().find(id)->cframe.position = core::DVec3{1.0, 2.0, 3.0};

    render::TransformHistory history;
    history.capture(fixture.world);
    history.shift(fixture.world, body, core::DVec3{0.5, 0.0, -0.25});

    CHECK(history.previous(body)->position.x == 1.5);
    CHECK(history.previous(body)->position.z == 2.75);
    CHECK(history.previous(hat)->position.x == 1.5);
    CHECK(history.previous(crate)->position.x == 1.0);
}

// --- D070: a history whose world has been replaced under it -----------------
//
// A snapshot restore preserves generations, precisely so that an `InstanceId`
// means the same thing after one as before it -- which is also what let a
// `TransformHistory` entry go on answering for a part the restore had moved
// metres. `world.h` states the obligation the frame loop owes here in so many
// words: these caches are "rebuilt from the tree rather than restored ... safe
// order: restore, then rebuild".
//
// The symptom was the flagship's character capsule flickering after a stop,
// interpolated every frame between where it had walked to and where it was put
// back -- with an alpha that goes on sweeping [0, 1) because the frame
// scheduler drains its accumulator whether or not the editor let a tick
// through.
TEST_CASE("clearing the history is what makes a restored world stop interpolating")
{
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    const core::InstanceId part = fixture.part(root);

    scene::PartComponent* component = fixture.world.parts().find(part);
    REQUIRE(component != nullptr);
    component->cframe.position = core::DVec3{40.0, 0.0, 0.0};
    // A hundred metres long, so forty is within what the renderer treats as a
    // glide: a smaller part would be taken for a teleport and drawn where it
    // is, which hides the stale history this case is about rather than
    // clearing it.
    component->size = core::Vec3{100.0f, 1.0f, 1.0f};

    render::TransformHistory history;
    history.capture(fixture.world);

    // The restore. Same id, same generation -- that is the whole difficulty --
    // and a position nowhere near what the history holds.
    component->cframe.position = core::DVec3{0.0, 0.0, 0.0};

    render::RenderWorld stale;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.5f, &history, stale);
    REQUIRE(stale.parts.size() == 1);
    // Twenty metres from anywhere the world says it is, and a different number
    // every frame as alpha sweeps.
    CHECK(nearly(static_cast<core::f32>(stale.parts[0].cframe.position.x), 20.0f));

    history.clear();

    render::RenderWorld settled;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.5f, &history, settled);
    REQUIRE(settled.parts.size() == 1);
    CHECK(settled.parts[0].cframe.position.x == 0.0);
}

// --- E2: the selection reaches the renderer as a flag on the draw -----------
//
// The silhouette pass walks the same draw list every other pass walks and draws
// the ones marked here. That the mark ARRIVES is the half of it a headless test
// can hold; what the mask and the dilate then make of it needs a device and, in
// the end, a person looking at it.
//
// The differential is the point (MASTER_PROMPT.md §8): extracting the same world
// twice, once with a selection and once without, must not produce the same draw
// list.
TEST_CASE("a selected instance comes out marked, and nothing else does")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    meshes.set(content, entry);

    const core::InstanceId first = fixture.meshPartAt(workspace, core::DVec3{-3.0, 0.0, -10.0}, content);
    const core::InstanceId second = fixture.meshPartAt(workspace, core::DVec3{3.0, 0.0, -10.0}, content);
    (void)first;

    render::RenderWorld plain;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, plain);
    REQUIRE(plain.draws.size() == 2);
    for (const render::DrawItem& draw : plain.draws)
        CHECK_FALSE(draw.outlined);

    const std::array<core::InstanceId, 1> selection{second};
    render::RenderWorld selected;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, selected,
                    nullptr, selection);
    REQUIRE(selected.draws.size() == 2);

    core::usize marked = 0;
    for (const render::DrawItem& draw : selected.draws) {
        if (draw.outlined)
            ++marked;
    }
    CHECK(marked == 1);

    // And the two extractions differ, which is the whole assertion: a flag that
    // never reached the snapshot would leave these identical and every visual
    // check downstream would be looking at an image that could not have changed.
    bool differs = false;
    for (core::usize index = 0; index < plain.draws.size(); ++index)
        differs = differs || plain.draws[index].outlined != selected.draws[index].outlined;
    CHECK(differs);
}

TEST_CASE("a long selection is searched, and marks exactly what it holds in any order")
{
    // **Past sixteen the renderer sorts the list and searches it**, because a
    // selected model outlines every part inside it. The order the list arrives
    // in is the editor's, so the answer must not depend on it.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    meshes.set(content, entry);

    std::vector<core::InstanceId> parts;
    for (int index = 0; index < 24; ++index)
        parts.push_back(fixture.meshPartAt(workspace, core::DVec3{index * 1.5 - 18.0, 0.0, -30.0}, content));

    // Twenty of the twenty-four, reversed, with a stale id mixed in.
    std::vector<core::InstanceId> selection(parts.rbegin(), parts.rbegin() + 20);
    selection.insert(selection.begin() + 5, core::InstanceId{9999, 1});
    render::RenderWorld selected;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, selected,
                    nullptr, selection);
    REQUIRE(selected.draws.size() == 24);

    core::usize marked = 0;
    for (const render::DrawItem& draw : selected.draws) {
        if (draw.outlined)
            ++marked;
    }
    CHECK(marked == 20);
}

TEST_CASE("a disabled light contributes nothing, and does not spend a budget slot")
{
    // **`Enabled` is not a brightness of zero, and this is the difference.** A
    // light at zero brightness is still a light: it is extracted, it is counted,
    // and it occupies one of the slots the renderer has to spend. A disabled one
    // is skipped before any of that, so turning a room's lights off gives the
    // rest of the scene the slots back.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::InstanceId host = fixture.part(workspace);
    fixture.world.parts().find(host)->cframe.position = core::DVec3{0.0, 0.0, -5.0};

    const core::InstanceId lamp = fixture.world.create(fixture.pointLightClass);
    (void)fixture.world.setParent(lamp, host);

    render::MeshLibrary meshes;
    render::RenderWorld lit;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, lit);
    REQUIRE(lit.lights.size() == 1);

    // Zero brightness is still a light, which is what makes the two properties
    // different questions rather than two spellings of one.
    fixture.world.pointLights().find(lamp)->brightness = 0.0f;
    render::RenderWorld dark;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, dark);
    CHECK(dark.lights.size() == 1);

    fixture.world.pointLights().find(lamp)->enabled = false;
    render::RenderWorld off;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, off);
    CHECK(off.lights.empty());
}

// --- BasePart.Material (ADR 0090) -------------------------------------------

namespace {

constexpr std::string_view HalfGrey = "asset://materials/half-grey.material.json";

// A material asset with a half-grey base, a green glow and bare metal, so a
// value from it is a number nothing else in this file would produce. It lets a
// part override its `Color` and nothing else.
void putHalfGrey(Fixture& fixture, asset::MaterialFieldMask declares = asset::fieldBit(asset::MaterialField::Color),
                 std::string colorMap = {})
{
    asset::MaterialAsset material;
    material.properties.color = core::Color3{0.5f, 0.5f, 0.5f};
    material.properties.emissive = core::Color3{0.0f, 0.8f, 0.0f};
    material.properties.metalness = 1.0f;
    material.properties.roughness = 0.2f;
    material.properties.colorMap = std::move(colorMap);
    material.instanceParameters = declares;
    material.written = asset::AllMaterialFields;
    fixture.materials.put(HalfGrey, material);
    fixture.world.setMaterialLibrary(&fixture.materials);
}

void wear(Fixture& fixture, core::InstanceId id, std::string_view urn)
{
    REQUIRE(fixture.world.parts().find(id) != nullptr);
    fixture.world.parts().find(id)->material = fixture.atoms.intern(urn);
}

[[nodiscard]] bool nearF(core::f32 value, core::f32 expected) noexcept
{
    // Compared at f32: `doctest::Approx` takes a double and letting a uniform
    // promote into it is a `-Wdouble-promotion` error under Clang.
    return std::fabs(value - expected) < 1e-4f;
}

// A `Part` draws through the primitive registered under its shape's reserved
// URN, exactly as `MeshLoader::syncPrimitives` would have put it there.
void registerBlock(Fixture& fixture, render::MeshLibrary& meshes)
{
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    meshes.set(fixture.atoms.intern(render::primitiveContent(0)), entry);
}

[[nodiscard]] core::InstanceId blockAt(Fixture& fixture, core::InstanceId workspace, core::f64 x = 0.0)
{
    const core::InstanceId id = fixture.world.create(fixture.partClass);
    (void)fixture.world.setParent(id, workspace);
    fixture.world.parts().find(id)->cframe.position = core::DVec3{x, 0.0, -10.0};
    return id;
}

} // namespace

TEST_CASE("a Part with no material draws its Color override, exactly as BasePart.Color always drew")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::InstanceId id = blockAt(fixture, workspace);
    setColorOverride(fixture.world, id, core::Color3{0.25f, 0.5f, 0.75f});

    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    REQUIRE(snapshot.materials.size() == 1);
    // The default's white times the colour IS the colour, which is what makes
    // every scene written before ADR 0090 draw the same pixels.
    CHECK(nearF(snapshot.materials[0].uniforms.baseColor[0], 0.25f));
    CHECK(nearF(snapshot.materials[0].uniforms.baseColor[1], 0.5f));
    CHECK(nearF(snapshot.materials[0].uniforms.baseColor[2], 0.75f));
    CHECK(nearF(snapshot.materials[0].uniforms.metallicRoughnessNormalCutoff[1], 0.7f));
}

TEST_CASE("BasePart.ReceivesDecals: a part asked to take none is drawn as it was, and says so to the renderer")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);
    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);

    const core::InstanceId floor = blockAt(fixture, workspace);
    const core::InstanceId hero = blockAt(fixture, workspace);
    // On until a game says otherwise.
    CHECK(fixture.world.parts().find(floor)->receivesDecals);
    fixture.world.parts().find(hero)->receivesDecals = false;

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    REQUIRE(snapshot.draws.size() == 2);
    int receiving = 0;
    for (const render::DrawItem& draw : snapshot.draws) {
        // Still drawn, still casting: only what is painted on it changes.
        CHECK(draw.inCameraFrustum);
        CHECK(render::castsShadow(draw));
        receiving += draw.receivesDecals ? 1 : 0;
    }
    CHECK(receiving == 1);
    // What is not a part receives: the default of a draw.
    CHECK(render::DrawItem{}.receivesDecals);
}

TEST_CASE("BasePart.Fade: what one machine leaves out of its own picture is blended there, and still casts")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);
    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);

    const core::InstanceId solid = blockAt(fixture, workspace);
    const core::InstanceId thinned = blockAt(fixture, workspace);
    const core::InstanceId gone = blockAt(fixture, workspace);
    // Nothing until a game says otherwise.
    CHECK(fixture.world.parts().find(solid)->fade == 0.0f);
    fixture.world.parts().find(thinned)->fade = 0.75f;
    fixture.world.parts().find(gone)->fade = 1.0f;

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    // All three are in the list: a tree hidden from a camera has not left the
    // world, and the one thinned to nothing is there for its shadow.
    REQUIRE(snapshot.draws.size() == 3);
    int opaque = 0;
    int blended = 0;
    int unseen = 0;
    for (const render::DrawItem& draw : snapshot.draws) {
        // Every one of them casts the shadow it cast before it was thinned.
        CHECK(render::castsShadow(draw));
        if (!draw.transparent) {
            ++opaque;
            CHECK(nearF(draw.alpha, 1.0f));
            CHECK_FALSE(draw.fadedOnly);
        }
        else if (draw.alpha > 0.0f) {
            ++blended;
            // A quarter of it is left, drawn in the blended pass.
            CHECK(nearF(draw.alpha, 0.25f));
            CHECK(draw.fadedOnly);
        }
        else {
            ++unseen;
            CHECK(draw.fadedOnly);
        }
    }
    CHECK(opaque == 1);
    CHECK(blended == 1);
    CHECK(unseen == 1);

    // A part that is see-through of its own is see-through, and casts none:
    // thinning it further does not give it a shadow back.
    const core::InstanceId glass = blockAt(fixture, workspace);
    setTransparencyOverride(fixture.world, glass, 0.5f);
    fixture.world.parts().find(glass)->fade = 0.5f;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    REQUIRE(snapshot.draws.size() == 4);
    int glassy = 0;
    for (const render::DrawItem& draw : snapshot.draws) {
        if (draw.transparent && !draw.fadedOnly) {
            ++glassy;
            CHECK(nearF(draw.alpha, 0.25f));
            CHECK_FALSE(render::castsShadow(draw));
        }
    }
    CHECK(glassy == 1);
}

TEST_CASE("BasePart.CastShadow: a part asked to cast none is drawn as it was, and casts none")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);
    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);

    const core::InstanceId casts = blockAt(fixture, workspace);
    const core::InstanceId flash = blockAt(fixture, workspace);
    // On until a game says otherwise.
    CHECK(fixture.world.parts().find(casts)->castShadow);
    fixture.world.parts().find(flash)->castShadow = false;

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    // Both are drawn, solid: the property takes away what a part casts and
    // nothing else.
    REQUIRE(snapshot.draws.size() == 2);
    int casting = 0;
    for (const render::DrawItem& draw : snapshot.draws) {
        CHECK_FALSE(draw.transparent);
        CHECK(draw.inCameraFrustum);
        casting += render::castsShadow(draw) ? 1 : 0;
    }
    CHECK(casting == 1);
    // What is not a part casts: the default of a draw.
    CHECK(render::castsShadow(render::DrawItem{}));
}

TEST_CASE("D423: a plain part faded past a half is still drawn -- its block has no alpha cutoff")
{
    // Six panes of glass at 0.25 to 0.9: two blended, one was stripes and
    // three were not drawn. The plain part's block began at a cutoff of a
    // half, and the shader clips at the cutoff whatever the material's mode.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);
    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);

    for (const core::f32 transparency : {0.25f, 0.5f, 0.55f, 0.9f}) {
        const core::InstanceId id = blockAt(fixture, workspace);
        setTransparencyOverride(fixture.world, id, transparency);
    }

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    REQUIRE(snapshot.draws.size() == 4);
    for (const render::DrawItem& draw : snapshot.draws) {
        CHECK(draw.transparent);
        REQUIRE(draw.material < snapshot.materials.size());
        // Nothing of it is clipped: the cutoff is a mask's, and this is none.
        CHECK(snapshot.materials[draw.material].uniforms.metallicRoughnessNormalCutoff[3] == 0.0f);
        // And what is see-through casts no shadow.
        CHECK_FALSE(render::castsShadow(draw));
    }
    // A block nobody set anything on has none either.
    CHECK(render::RenderMaterial{}.uniforms.metallicRoughnessNormalCutoff[3] == 0.0f);
}

TEST_CASE("a plain part glows: the engine default declares Emissive")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);
    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);

    const core::InstanceId ember = blockAt(fixture, workspace);
    asset::MaterialProperties values;
    values.emissive = core::Color3{4.0f, 1.5f, 0.25f};
    REQUIRE((asset::DefaultMaterialParameters & asset::fieldBit(asset::MaterialField::Emissive)) != 0);
    REQUIRE(asset::setOverride(fixture.world.parts().find(ember)->materialParameters, asset::MaterialField::Emissive,
                               values));
    // One beside it that does not glow is a block of its own.
    (void)blockAt(fixture, workspace);

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    REQUIRE(snapshot.draws.size() == 2);
    REQUIRE(snapshot.materials.size() == 2);
    bool glowing = false;
    bool dark = false;
    for (const render::RenderMaterial& material : snapshot.materials) {
        if (nearF(material.uniforms.emissive[0], 4.0f) && nearF(material.uniforms.emissive[1], 1.5f) &&
            nearF(material.uniforms.emissive[2], 0.25f))
            glowing = true;
        if (material.uniforms.emissive[0] == 0.0f && material.uniforms.emissive[1] == 0.0f)
            dark = true;
    }
    CHECK(glowing);
    CHECK(dark);
    // A solid part casts.
    CHECK(render::castsShadow(snapshot.draws[0]));
}

TEST_CASE("a part wearing a material draws it as authored, and a declared override replaces one value")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    putHalfGrey(fixture);
    const core::InstanceId id = blockAt(fixture, workspace);
    wear(fixture, id, HalfGrey);

    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);
    render::RenderWorld snapshot;

    SUBCASE("worn as authored")
    {
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.materials.size() == 1);
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[0], 0.5f));
        CHECK(nearF(snapshot.materials[0].uniforms.emissive[1], 0.8f));
        // Its own metalness and roughness, rather than the dielectric defaults
        // an untextured part gets.
        CHECK(nearF(snapshot.materials[0].uniforms.metallicRoughnessNormalCutoff[0], 1.0f));
        CHECK(nearF(snapshot.materials[0].uniforms.metallicRoughnessNormalCutoff[1], 0.2f));
    }

    SUBCASE("a declared Color override replaces the colour, and only the colour")
    {
        setColorOverride(fixture.world, id, core::Color3{1.0f, 0.5f, 0.0f});
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.materials.size() == 1);
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[0], 1.0f));
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[1], 0.5f));
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[2], 0.0f));
        // An override is the value, not a multiplier: the glow is the
        // material's, because the material did not let the part change it.
        CHECK(nearF(snapshot.materials[0].uniforms.emissive[1], 0.8f));
    }

    SUBCASE("an override the material does not declare is kept and ignored")
    {
        putHalfGrey(fixture, 0);
        setColorOverride(fixture.world, id, core::Color3{1.0f, 0.0f, 0.0f});
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.materials.size() == 1);
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[1], 0.5f));
        CHECK(fixture.world.parts().find(id)->materialParameters.has(asset::MaterialField::Color));
    }

    SUBCASE("a material whose maps have not loaded still draws its numbers")
    {
        putHalfGrey(fixture, asset::fieldBit(asset::MaterialField::Color), "asset://t.png");
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot, nullptr, {}, nullptr);
        REQUIRE(snapshot.materials.size() == 1);
        // A surface that vanished while its texture loaded would be worse than
        // one that arrives plain and then gets its map.
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[0], 0.5f));
        CHECK(snapshot.draws.size() == 1);
    }
}

TEST_CASE("authored blocks stay independent across frame cache growth and per-part shader overrides")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);
    fixture.world.setMaterialLibrary(&fixture.materials);
    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);
    constexpr core::u32 Count = 300;
    const auto urn = [](core::u32 index) { return "asset://materials/cache-growth-" + std::to_string(index); };
    asset::MaterialAsset authored;
    authored.properties.shader = "asset://shaders/shared-surface-with-a-long-name.surface.hlsl";
    authored.properties.alphaMode = static_cast<core::i32>(asset::MaterialAlphaMode::Mask);
    authored.properties.alphaCutoff = 0.37f;
    authored.properties.shaderParameters = {{.name = "Glow", .value = {1.0f, 2.0f, 3.0f, 0.0f}, .texture = {}}};
    authored.instanceShaderParameters = {"Added", "Glow"};
    authored.written = asset::AllMaterialFields;
    for (core::u32 index = 0; index < Count; ++index) {
        authored.properties.color.r = static_cast<core::f32>(index) / static_cast<core::f32>(Count);
        fixture.materials.put(urn(index), authored);
        wear(fixture, blockAt(fixture, workspace), urn(index));
    }
    const auto repeated = blockAt(fixture, workspace);
    wear(fixture, repeated, urn(0));
    fixture.world.setPartShaderParameter(repeated, {.name = "Ignored", .value = {9.0f}, .texture = {}});
    const auto overridden = blockAt(fixture, workspace);
    wear(fixture, overridden, urn(0));
    fixture.world.setPartShaderParameter(overridden,
                                         {.name = "Glow", .value = {8.0f, 7.0f, 6.0f, 5.0f}, .texture = {}});
    fixture.world.setPartShaderParameter(overridden, {.name = "Added", .value = {2.0f}, .texture = {}});
    const auto equivalent = blockAt(fixture, workspace);
    wear(fixture, equivalent, urn(0));
    fixture.world.setPartShaderParameter(equivalent,
                                         {.name = "Glow", .value = {1.0f, 2.0f, 3.0f, -0.0f}, .texture = {}});
    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, {}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    REQUIRE(snapshot.materials.size() == Count + 1);
    for (core::u32 index = 0; index < Count; ++index) {
        const auto& block = snapshot.materials[index];
        CHECK(block.surface == authored.properties.shader);
        REQUIRE(block.surfaceValues.size() == 1);
        CHECK(block.surfaceValues[0].name == "Glow");
        CHECK(nearF(block.surfaceValues[0].value[0], 1.0f));
        CHECK(nearF(block.uniforms.baseColor[0], static_cast<core::f32>(index) / static_cast<core::f32>(Count)));
        CHECK(nearF(block.uniforms.metallicRoughnessNormalCutoff[3], 0.37f));
    }
    const auto& own = snapshot.materials.back();
    REQUIRE(own.surfaceValues.size() == 2);
    CHECK(own.surfaceValues[0].name == "Added");
    CHECK(nearF(own.surfaceValues[0].value[0], 2.0f));
    CHECK(own.surfaceValues[1].name == "Glow");
    CHECK(nearF(own.surfaceValues[1].value[0], 8.0f));
    // A new extraction observes material edits, without changing an earlier
    // snapshot or allowing one part's shader parameters to mutate its peers.
    authored.properties.shaderParameters[0].value[0] = 12.0f;
    fixture.materials.put(urn(0), authored);
    fixture.world.clearPartShaderParameter(equivalent, "Glow");
    render::RenderWorld next;
    render::extract(fixture.world, workspace, {}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, next);
    CHECK(nearF(next.materials[0].surfaceValues[0].value[0], 12.0f));
    CHECK(nearF(snapshot.materials[0].surfaceValues[0].value[0], 1.0f));
    CHECK(nearF(next.materials.back().surfaceValues[1].value[0], 8.0f));
}

TEST_CASE("a material's Transparency fades the draw and puts it in the blended pass")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    asset::MaterialAsset glass;
    glass.properties.transparency = 0.5f;
    glass.written = asset::AllMaterialFields;
    fixture.materials.put("asset://materials/glass.material.json", glass);
    fixture.world.setMaterialLibrary(&fixture.materials);
    const core::InstanceId id = blockAt(fixture, workspace);
    wear(fixture, id, "asset://materials/glass.material.json");

    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);
    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    REQUIRE(snapshot.draws.size() == 1);
    CHECK(snapshot.draws[0].transparent);
    CHECK(nearly(snapshot.draws[0].alpha, 0.5f));
    // In the draw, not twice: the block's own alpha stays one.
    CHECK(nearF(snapshot.materials[0].uniforms.baseColor[3], 1.0f));
}

TEST_CASE("two parts differing only in an override are two bind sets")
{
    // The dedupe key has to carry what reaches the block, or the second part
    // draws in the first one's colour.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    putHalfGrey(fixture);
    for (int index = 0; index < 2; ++index) {
        const core::InstanceId id = blockAt(fixture, workspace, static_cast<core::f64>(index));
        wear(fixture, id, HalfGrey);
        if (index == 1)
            setColorOverride(fixture.world, id, core::Color3{1.0f, 0.0f, 0.0f});
    }

    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    REQUIRE(snapshot.materials.size() == 2);
    CHECK(nearF(snapshot.materials[0].uniforms.baseColor[1], 0.5f));
    CHECK(nearF(snapshot.materials[1].uniforms.baseColor[1], 0.0f));

    // And two parts that agree ARE one bind set, which is the half of the rule
    // that makes a wall of a hundred bricks one material. An override equal to
    // the material's own value is the same look as none.
    fixture.world.parts().forEach([&](core::InstanceId id, scene::PartComponent&) {
        setColorOverride(fixture.world, id, core::Color3{0.5f, 0.5f, 0.5f});
    });
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    CHECK(snapshot.materials.size() == 1);
}

TEST_CASE("a material actually uses the maps it was given")
{
    // **The case whose absence let a whole feature be dead.** A number arrives
    // through the uniforms whether or not the texture flags say anything, so a
    // material could bind four handles, leave all four flags at zero, and pass
    // every test that checked a number -- while the shader multiplied each
    // sample by zero. From the outside that is a material that ignores its
    // diffuse map, which is exactly how it was reported.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    asset::MaterialAsset wood;
    wood.properties.colorMap = "asset://textures/wood.png";
    wood.properties.metallicRoughnessMap = "asset://textures/wood_arm.png";
    wood.written = asset::AllMaterialFields;
    fixture.materials.put("asset://materials/wood.material.json", wood);
    fixture.world.setMaterialLibrary(&fixture.materials);

    const core::InstanceId id = blockAt(fixture, workspace);
    wear(fixture, id, "asset://materials/wood.material.json");

    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);

    // Keyed by the atom `MeshLoader::syncTextures` interns each map as.
    render::TextureLibrary textures;
    textures.set(fixture.atoms.intern("asset://textures/wood.png"), rhi::TextureHandle{7});
    textures.set(fixture.atoms.intern("asset://textures/wood_arm.png"), rhi::TextureHandle{8});

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot,
                    nullptr, {}, &textures);

    REQUIRE(snapshot.materials.size() == 1);
    const render::RenderMaterial& drawn = snapshot.materials[0];
    // Bound, and switched on -- the half that was missing.
    CHECK(drawn.baseColor.valid());
    CHECK(drawn.metallicRoughness.valid());
    CHECK(nearF(drawn.uniforms.textureFlags[0], 1.0f));
    CHECK(nearF(drawn.uniforms.textureFlags[2], 1.0f));
    // And the two the material did not name stay off, because a stand-in
    // sampled at full strength would tint every surface by a 1x1 white.
    CHECK_FALSE(drawn.normal.valid());
    CHECK(nearF(drawn.uniforms.textureFlags[1], 0.0f));
    CHECK(nearF(drawn.uniforms.textureFlags[3], 0.0f));
}

TEST_CASE("a map the library has not loaded yet is off, not bound")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    putHalfGrey(fixture, 0, "asset://textures/not-loaded-yet.png");
    const core::InstanceId id = blockAt(fixture, workspace);
    wear(fixture, id, HalfGrey);

    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);
    render::TextureLibrary textures;

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot,
                    nullptr, {}, &textures);

    REQUIRE(snapshot.materials.size() == 1);
    CHECK_FALSE(snapshot.materials[0].baseColor.valid());
    CHECK(nearF(snapshot.materials[0].uniforms.textureFlags[0], 0.0f));
}

TEST_CASE("a MeshPart's material replaces the block its file described")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    const core::InstanceId id = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);

    render::MeshLibrary meshes;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    entry.sectionMaterial.push_back(0);
    render::RenderMaterial fromFile;
    fromFile.uniforms.baseColor[0] = 1.0f;
    fromFile.uniforms.baseColor[1] = 0.0f;
    fromFile.uniforms.baseColor[2] = 0.0f;
    entry.materials.push_back(fromFile);
    meshes.set(content, entry);

    render::RenderWorld snapshot;

    SUBCASE("wearing nothing it keeps the file's block")
    {
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.materials.size() == 1);
        // Red, from the file. This is what stops every existing mesh scene going
        // white the moment materials are assets.
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[0], 1.0f));
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[1], 0.0f));
    }

    SUBCASE("wearing nothing, a Color override tints the file's block, as BasePart.Color did")
    {
        setColorOverride(fixture.world, id, core::Color3{0.5f, 1.0f, 1.0f});
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.materials.size() == 1);
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[0], 0.5f));
    }

    SUBCASE("wearing one it replaces it whole")
    {
        putHalfGrey(fixture);
        wear(fixture, id, HalfGrey);
        render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                        snapshot);
        REQUIRE(snapshot.materials.size() == 1);
        // Not a merge: every channel comes from the material, including the ones
        // the file also had an opinion about.
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[0], 0.5f));
        CHECK(nearF(snapshot.materials[0].uniforms.baseColor[1], 0.5f));
    }
}

// --- Forgetting one URN, which is the whole of asset hot-reload (S6.4) -------
//
// `asset-changed` was reserved by the dev protocol at M3 and deferred because
// "no asset pipeline exists before M4/M7". Both shipped, so the reason expired.
//
// The implementation is a removal, because the loaders already load everything
// MISSING: `syncTextures` reads every map it cannot find, so making an entry
// missing IS the reload. What these hold down is that the removal is exact --
// one URN, not its neighbours -- and that the handle comes back so the caller
// can destroy it rather than leaking one texture per save.

TEST_CASE("taking a texture removes exactly that one and hands its handle back")
{
    render::TextureLibrary textures;
    core::AtomTable atoms;
    const core::NameAtom first = atoms.intern("asset://textures/brick.png");
    const core::NameAtom second = atoms.intern("asset://textures/wood.png");

    textures.set(first, rhi::TextureHandle{11});
    textures.set(second, rhi::TextureHandle{12});
    REQUIRE(textures.size() == 2);

    // The handle comes BACK rather than being destroyed in here: this class has
    // no device, and a map that owned GPU lifetime would be a second place to
    // look when a texture outlives its frame.
    const rhi::TextureHandle taken = textures.take(first);
    CHECK(taken.id == 11u);
    CHECK(textures.size() == 1);
    CHECK_FALSE(textures.find(first).valid());

    // The neighbour is untouched, which is what makes this a reload of one file
    // rather than of the project.
    CHECK(textures.find(second).id == 12u);
}

TEST_CASE("taking a URN nothing loaded is not an error")
{
    render::TextureLibrary textures;
    core::AtomTable atoms;

    // The ordinary case on a change to a file nothing has drawn yet. A watcher
    // reports every save, and most of them are for content no frame has asked
    // for -- so this has to be a no-op rather than a diagnostic.
    CHECK_FALSE(textures.take(atoms.intern("asset://textures/never.png")).valid());
    CHECK(textures.size() == 0);

    textures.set(atoms.intern("asset://a.png"), rhi::TextureHandle{3});
    CHECK_FALSE(textures.take(atoms.intern("asset://b.png")).valid());
    CHECK(textures.size() == 1);
}

TEST_CASE("a taken texture is loaded again, because the library is what says it is missing")
{
    render::TextureLibrary textures;
    core::AtomTable atoms;
    const core::NameAtom urn = atoms.intern("asset://textures/brick.png");

    textures.set(urn, rhi::TextureHandle{5});
    CHECK(textures.find(urn).valid());

    (void)textures.take(urn);
    // **This is the reload.** `syncTextures` loads every map it cannot find, so
    // an absent entry is a request to read the file again -- there is no second
    // path and no state machine.
    CHECK_FALSE(textures.find(urn).valid());

    textures.set(urn, rhi::TextureHandle{6});
    CHECK(textures.find(urn).id == 6u);
}

// --- Terrain (ADR 0082) ------------------------------------------

TEST_CASE("a world with no terrain extracts exactly what it did before")
{
    // The claim every step of F1 has to keep, asserted at the render seam: the
    // terrain loop is a third walk over a pool that is empty in every project
    // that never touches it.
    Fixture fixture;
    const core::InstanceId root = fixture.world.create(fixture.folderClass);
    (void)fixture.part(root);

    render::RenderWorld snapshot;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);

    CHECK(snapshot.parts.size() == 1);
    CHECK(snapshot.draws.empty());
}

TEST_CASE("a terrain node draws once the loader has chosen it and its mesh is in the library")
{
    // A workspace with a camera, because that is what a draw needs: `extract`
    // builds its frustum from `Workspace.CurrentCamera` and a world with no
    // camera renders nothing rather than inventing one.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId root = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(root);

    const core::InstanceId terrain = fixture.world.create(fixture.folderClass);
    scene::TerrainComponent component;
    component.field = asset::TerrainField(asset::FieldSettings{.voxelSize = 1.0f});
    (void)asset::fillFlat(component.field, core::DVec3{0.0, 0.0, 0.0}, 32.0f, 2.0f, 1);
    (void)asset::fillBall(component.field, core::DVec3{4.0, -1.0, 4.0}, 1.5, 0);
    component.fieldRevision = 1;
    fixture.world.terrains().add(terrain, component);
    REQUIRE(fixture.world.setParent(terrain, root) == std::nullopt);

    const core::NameAtom urn = fixture.atoms.intern(render::terrainNodeUrn(terrain, render::TerrainNodeKey{0, 0, 0}));
    const std::array<render::TerrainNodeDraw, 1> chosen{render::TerrainNodeDraw{terrain, urn}};

    // **Nothing yet**, because the geometry has not been built. Skipped rather
    // than substituted: ground whose mesh is a frame behind is ground that is not
    // there for a frame, and a placeholder for it is a hole nobody notices.
    render::RenderWorld before;
    render::extract(fixture.world, root, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, before,
                    nullptr, {}, nullptr, chosen);
    CHECK(before.draws.empty());

    render::MeshLibrary library;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{1, 1};
    entry.bounds = core::AABB{core::Vec3{-8.0f, -2.0f, -18.0f}, core::Vec3{8.0f, 2.0f, -2.0f}};
    entry.sectionCount = 1;
    entry.sectionMaterial.assign(1, 0u);
    entry.materials.push_back(render::RenderMaterial{});
    library.set(urn, std::move(entry));

    // Not chosen: not drawn, whatever is in the library.
    render::RenderWorld unchosen;
    render::extract(fixture.world, root, core::InstanceId{}, library, 1.0f, 0.0f, nullptr, 0.0f, nullptr, unchosen);
    CHECK(unchosen.draws.empty());

    render::RenderWorld after;
    render::extract(fixture.world, root, core::InstanceId{}, library, 1.0f, 0.0f, nullptr, 0.0f, nullptr, after,
                    nullptr, {}, nullptr, chosen);
    REQUIRE(after.draws.size() == 1);
    CHECK(after.draws[0].mesh.index == 1);
    CHECK(after.draws[0].terrain);
    // **Opaque and unskinned**, which terrain always is: there is no
    // `Transparency` on ground and no rig under it.
    CHECK_FALSE(after.draws[0].transparent);
    CHECK(after.draws[0].boneCount == 0);
    CHECK(after.draws[0].alpha == 1.0f);
    // And it produced no `parts` entry, which is the whole reason terrain is not
    // made of `MeshPart`s: no phantom body, no Explorer row, no snapshot cost.
    CHECK(after.parts.empty());

    // **A selected terrain outlines nothing** (D161). The outline pass draws
    // through everything in front of it, and a node's skirts, when it had them,
    // were buried in the ground: outlined, they showed through a dug crater as
    // a tinted lid and a row of boxes -- which is what selecting a terrain looked like, and
    // `Create Terrain` selects the terrain it makes.
    const std::array<core::InstanceId, 1> selection{terrain};
    render::RenderWorld selected;
    render::extract(fixture.world, root, core::InstanceId{}, library, 1.0f, 0.0f, nullptr, 0.0f, nullptr, selected,
                    nullptr, selection, nullptr, chosen);
    REQUIRE(selected.draws.size() == 1);
    CHECK_FALSE(selected.draws[0].outlined);
}

TEST_CASE("terrain outside the root is not in the world")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId root = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(root);
    const core::InstanceId elsewhere = fixture.world.create(fixture.folderClass);

    const core::InstanceId terrain = fixture.world.create(fixture.folderClass);
    scene::TerrainComponent component;
    component.field = asset::TerrainField(asset::FieldSettings{.voxelSize = 1.0f});
    (void)asset::fillFlat(component.field, core::DVec3{0.0, 0.0, 0.0}, 32.0f, 2.0f, 1);
    fixture.world.terrains().add(terrain, component);
    REQUIRE(fixture.world.setParent(terrain, elsewhere) == std::nullopt);

    const core::NameAtom urn = fixture.atoms.intern(render::terrainNodeUrn(terrain, render::TerrainNodeKey{0, 0, 0}));
    render::MeshLibrary library;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{1, 1};
    entry.sectionCount = 1;
    entry.sectionMaterial.assign(1, 0u);
    entry.materials.push_back(render::RenderMaterial{});
    library.set(urn, std::move(entry));
    const std::array<render::TerrainNodeDraw, 1> chosen{render::TerrainNodeDraw{terrain, urn}};

    render::RenderWorld snapshot;
    render::extract(fixture.world, root, core::InstanceId{}, library, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot,
                    nullptr, {}, nullptr, chosen);

    // Whatever is parented under the root is in the world and whatever is not,
    // is not -- and terrain is not an exception to that.
    CHECK(snapshot.draws.empty());
}

TEST_CASE("a character is drawn as the capsule it moves as, not as a block")
{
    // **The owner's report**: a `CharacterBody` has no `Shape` of its own, so
    // it drew as the default block while the physics swept a capsule.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);
    render::MeshLibrary::Entry capsule;
    capsule.mesh = render::MeshHandle{7, 1};
    capsule.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 2.0f, 1.0f});
    capsule.sectionCount = 1;
    meshes.set(fixture.atoms.intern(render::primitiveContent(3)), capsule);

    const core::InstanceId block = blockAt(fixture, workspace, -2.0);
    const core::InstanceId character = blockAt(fixture, workspace, 2.0);
    (void)fixture.world.characterBodies().add(character, scene::CharacterBodyComponent{});

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    REQUIRE(snapshot.draws.size() == 2);
    int capsules = 0;
    for (const render::DrawItem& draw : snapshot.draws)
        capsules += draw.mesh == capsule.mesh ? 1 : 0;
    CHECK(capsules == 1);
    (void)block;
}

TEST_CASE("parts that differ only by colour are one material family, sorted together (D184)")
{
    // A snake of differently tinted segments was one material, and therefore
    // one draw, per segment. They now share a family: the sort key groups by
    // it so the instancer can make them one call, each colour in its instance.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    const core::NameAtom content = fixture.atoms.intern("asset://models/box.glb");
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry entry;
    entry.mesh = render::MeshHandle{0, 1};
    entry.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    entry.sectionCount = 1;
    meshes.set(content, entry);

    const core::InstanceId red = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -10.0}, content);
    const core::InstanceId green = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -20.0}, content);
    const core::InstanceId blue = fixture.meshPartAt(workspace, core::DVec3{0.0, 0.0, -30.0}, content);
    setColorOverride(fixture.world, red, core::Color3{1.0f, 0.0f, 0.0f});
    setColorOverride(fixture.world, green, core::Color3{0.0f, 1.0f, 0.0f});
    setColorOverride(fixture.world, blue, core::Color3{0.0f, 0.0f, 1.0f});

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    REQUIRE(snapshot.draws.size() == 3);
    // Three materials -- the colours are real and a draw drawn alone still
    // binds its own -- in one family.
    REQUIRE(snapshot.materials.size() == 3);
    const core::u32 family = snapshot.familyOf(snapshot.draws[0].material);
    for (const render::DrawItem& draw : snapshot.draws)
        CHECK(snapshot.familyOf(draw.material) == family);
    // Sorted near to far across the colours, as one material would be.
    CHECK(snapshot.draws[0].sortKey < snapshot.draws[1].sortKey);
    CHECK(snapshot.draws[1].sortKey < snapshot.draws[2].sortKey);
    CHECK(nearly(snapshot.draws[0].transform.m[3][2], -10.0f));
    CHECK(nearly(snapshot.draws[2].transform.m[3][2], -30.0f));
}

TEST_CASE("a light with no part shines from its own CFrame; in a part, relative to it (ADR 0095)")
{
    // **The owner's call**: a light dropped into the Workspace shines, from
    // where its `CFrame` puts it. Held by a part, the identity is the part.
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);
    render::MeshLibrary meshes;

    const core::InstanceId free = fixture.world.create(fixture.pointLightClass);
    (void)fixture.world.setParent(free, workspace);
    fixture.world.pointLights().find(free)->cframe.position = core::DVec3{3.0, 2.0, -8.0};

    render::RenderWorld world;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, world);
    REQUIRE(world.lights.size() == 1);
    CHECK(nearly(world.lights[0].position.x, 3.0f));
    CHECK(nearly(world.lights[0].position.y, 2.0f));
    CHECK(nearly(world.lights[0].position.z, -8.0f));

    // In a part, the light's CFrame is an offset from it.
    const core::InstanceId host = fixture.part(workspace);
    fixture.world.parts().find(host)->cframe.position = core::DVec3{0.0, 0.0, -5.0};
    (void)fixture.world.setParent(free, host);
    fixture.world.pointLights().find(free)->cframe.position = core::DVec3{0.0, 1.0, 0.0};
    render::RenderWorld held;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, held);
    REQUIRE(held.lights.size() == 1);
    CHECK(nearly(held.lights[0].position.y, 1.0f));
    CHECK(nearly(held.lights[0].position.z, -5.0f));
}

TEST_CASE("a clip plane cuts what is behind it out of the camera's depth range (ADR 0107)")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    render::RenderWorld snapshot;
    // A camera at z = -11.8 looking along +Z, and a plane at z = -5.9 facing
    // +Z: the mirror's camera and the mirror of `tests/screenshots/mirrors`.
    render::ViewOverride lens;
    lens.cframe =
        core::lookAtCFrame(core::DVec3{0.0, 2.0, -11.8}, core::DVec3{0.0, 2.0, 0.0}, core::Vec3{0.0f, 1.0f, 0.0f});
    lens.fieldOfView = 86.0f;
    lens.clipPlane =
        core::lookAtCFrame(core::DVec3{-3.0, 2.0, -5.9}, core::DVec3{-3.0, 2.0, 0.0}, core::Vec3{0.0f, 1.0f, 0.0f});
    lens.clipPlaneOn = true;
    render::extract(fixture.world, workspace, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                    snapshot, &lens);
    REQUIRE(snapshot.camera.valid);

    // Depth of a camera-relative point, as the GPU would divide it.
    const auto depth = [&](core::Vec3 point) {
        const core::Mat4& m = snapshot.camera.viewProjection;
        const float z = m.m[0][2] * point.x + m.m[1][2] * point.y + m.m[2][2] * point.z + m.m[3][2];
        const float w = m.m[0][3] * point.x + m.m[1][3] * point.y + m.m[2][3] * point.z + m.m[3][3];
        return z / w;
    };
    // Between the camera and the plane: behind it, clipped.
    CHECK(depth(core::Vec3{0.0f, 0.0f, 3.8f}) < 0.0f);
    // Beyond the plane: drawn.
    const float beyond = depth(core::Vec3{0.0f, 0.0f, 15.0f});
    CHECK(beyond > 0.0f);
    CHECK(beyond < 1.0f);
}

TEST_CASE("an orthographic camera sees its whole column, above where it stands too")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    render::RenderWorld snapshot;
    // Top-down, standing one metre up: a ball resting two metres up is wholly
    // above the camera, and a top-down view must still show it.
    render::ViewOverride lens;
    lens.cframe =
        core::lookAtCFrame(core::DVec3{0.0, 1.0, 0.0}, core::DVec3{0.0, 0.0, 0.0}, core::Vec3{0.0f, 0.0f, -1.0f});
    lens.projection = 1;
    lens.orthographicSize = 10.0f;
    lens.farPlane = 500.0f;
    render::extract(fixture.world, workspace, core::InstanceId{}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                    snapshot, &lens);
    REQUIRE(snapshot.camera.valid);

    const auto depth = [&](core::Vec3 point) {
        const core::Mat4& m = snapshot.camera.viewProjection;
        return m.m[0][2] * point.x + m.m[1][2] * point.y + m.m[2][2] * point.z + m.m[3][2];
    };
    // Camera-relative: a metre above the camera, and a metre below it.
    const float above = depth(core::Vec3{0.0f, 1.0f, 0.0f});
    const float below = depth(core::Vec3{0.0f, -1.0f, 0.0f});
    CHECK(above > 0.0f);
    CHECK(above < 1.0f);
    CHECK(below > above);
    CHECK(below < 1.0f);
    // And what is above is in the frustum a draw is culled against.
    CHECK(core::intersects(snapshot.camera.frustum,
                           core::AABB::fromMinMax(core::Vec3{-0.75f, 0.25f, -0.75f}, core::Vec3{0.75f, 1.75f, 0.75f})));
}

TEST_CASE("a water is drawn on the surface the simulation floats things on (ADR 0118)")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId root = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(root);
    const core::InstanceId water = fixture.world.create(fixture.folderClass);
    REQUIRE_FALSE(fixture.world.setParent(water, root).has_value());
    scene::WaterComponent component;
    component.surfaceLevel = 3.0;
    (void)fixture.world.waters().add(water, component);
    for (const auto& [length, height, direction, steep] :
         {std::tuple{60.0, 1.2, 10.0, 0.4}, std::tuple{31.0, 0.6, 70.0, 0.2}, std::tuple{17.0, 0.3, 200.0, 0.0}}) {
        const core::InstanceId wave = fixture.world.create(fixture.folderClass);
        scene::WaterWaveComponent term;
        term.wavelength = length;
        term.amplitude = height;
        term.direction = direction;
        term.steepness = steep;
        (void)fixture.world.waterWaves().add(wave, term);
        REQUIRE_FALSE(fixture.world.setParent(wave, water).has_value());
    }
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry grid;
    grid.mesh = render::MeshHandle{1, 1};
    grid.bounds = core::AABB{core::Vec3{-0.5f, 0.0f, -0.5f}, core::Vec3{0.5f, 0.0f, 0.5f}};
    grid.sectionCount = 1;
    meshes.set(fixture.atoms.intern(render::waterGridUrn()), grid);

    render::RenderWorld snapshot;
    render::extract(fixture.world, root, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    // The sea: three rings of tiles, nine and eight and eight.
    CHECK(snapshot.draws.size() == 25);
    const auto found = std::find_if(snapshot.materials.begin(), snapshot.materials.end(),
                                    [](const render::RenderMaterial& material) { return material.surface == "water"; });
    REQUIRE(found != snapshot.materials.end());
    CHECK(found->readsSceneColor);

    // **The shader's sum, in its own floats, from the parameters it is
    // handed**, against the simulation's in doubles: one surface.
    const auto value = [&](const std::string& name) {
        for (const render::SurfaceValue& entry : found->surfaceValues) {
            if (entry.name == name)
                return entry.value;
        }
        FAIL("no parameter ", name);
        return std::array<float, 4>{};
    };
    const int count = static_cast<int>(value("WaveCount")[0] + 0.5f);
    CHECK(count == 3);
    const scene::WaterSurface surface = scene::surfaceOf(fixture.world, water);
    for (const auto& [x, z, t] : {std::tuple{0.0f, 0.0f, 0.0f}, std::tuple{12.5f, -40.0f, 3.25f},
                                  std::tuple{-77.0f, 18.0f, 61.0f}, std::tuple{300.0f, 250.0f, 600.0f}}) {
        float drawn = 0.0f;
        for (int index = 0; index < count; ++index) {
            const std::array<float, 4> a = value("WaveA" + std::to_string(index));
            const std::array<float, 4> b = value("WaveB" + std::to_string(index));
            const float p = a[0] * (a[2] * x + a[3] * z) - b[0] * t + b[1];
            drawn += a[1] * (std::sin(p) - 0.5f * b[2] * std::cos(2.0f * p));
        }
        CHECK(std::abs(static_cast<double>(drawn) + 3.0 -
                       surface.heightAt(static_cast<double>(x), static_cast<double>(z), static_cast<double>(t))) <
              2e-3);
    }

    // A lake is one tile over its box.
    fixture.world.waters().find(water)->shape = 1;
    render::extract(fixture.world, root, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    CHECK(snapshot.draws.size() == 1);
}

TEST_CASE("ADR 0127: a visible rope is one cylinder from end to end, and one that is not visible is nothing")
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const core::InstanceId workspace = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(workspace);

    render::MeshLibrary meshes;
    registerBlock(fixture, meshes);
    render::MeshLibrary::Entry cylinder;
    cylinder.mesh = render::MeshHandle{1, 1};
    cylinder.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    cylinder.sectionCount = 1;
    meshes.set(fixture.atoms.intern(render::primitiveContent(2)), cylinder);

    // Two blocks four metres apart, an attachment at the middle of each.
    const core::InstanceId left = blockAt(fixture, workspace, -2.0);
    const core::InstanceId right = blockAt(fixture, workspace, 2.0);
    const auto attachmentOn = [&](core::InstanceId part) {
        const core::InstanceId id = fixture.world.create(fixture.instanceClass);
        (void)fixture.world.setParent(id, part);
        fixture.world.attachments().add(id, scene::AttachmentComponent{});
        return id;
    };
    const core::InstanceId rope = fixture.world.create(fixture.instanceClass);
    (void)fixture.world.setParent(rope, workspace);
    scene::ConstraintComponent line;
    line.kind = scene::MoverKind::DistanceJoint;
    line.attachment0 = attachmentOn(left);
    line.attachment1 = attachmentOn(right);
    line.color = core::Color3{0.8f, 0.2f, 0.1f};
    line.thickness = 0.2f;
    fixture.world.constraints().add(rope, line);

    render::RenderWorld snapshot;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    CHECK(snapshot.draws.size() == 2);

    fixture.world.constraints().find(rope)->visible = true;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    REQUIRE(snapshot.draws.size() == 3);
    const render::DrawItem* drawn = nullptr;
    for (const render::DrawItem& draw : snapshot.draws) {
        if (draw.mesh == cylinder.mesh)
            drawn = &draw;
    }
    REQUIRE(drawn != nullptr);
    // Halfway between the two, and as long as they are apart: the cylinder's
    // own axis, Y, turned onto the line and stretched along it.
    CHECK(nearF(drawn->boundsCenter.x, 0.0f));
    CHECK(nearF(drawn->boundsCenter.z, -10.0f));
    const core::Vec3 end = core::transformPoint(drawn->transform, core::Vec3{0.0f, 0.5f, 0.0f});
    CHECK(nearF(std::fabs(end.x), 2.0f));
    CHECK(nearF(end.y, 0.0f));
    // Its own colour, over the default's white.
    CHECK(nearF(snapshot.materials[drawn->material].uniforms.baseColor[0], 0.8f));
    CHECK(nearF(snapshot.materials[drawn->material].uniforms.baseColor[1], 0.2f));

    // A hinge that is `Visible` is drawn by the editor's lines, not as a thing
    // in the world.
    fixture.world.constraints().find(rope)->kind = 2;
    render::extract(fixture.world, workspace, core::InstanceId{}, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
    CHECK(snapshot.draws.size() == 2);
}

TEST_CASE("spritesOnly: sprites and not one 3D surface, which is drawn at the window's resolution")
{
    // ADR 0158: a picture of sprites alone is not scaled, upscaled or jittered;
    // one with a mesh, a terrain or foliage among its sprites follows the 3D.
    using namespace render;
    RenderWorld world;
    CHECK_FALSE(spritesOnly(world));
    world.sprites.push_back(RenderSprite{});
    CHECK(spritesOnly(world));
    // Particles, decals and world UI are drawn over sprites, not 3D surfaces.
    world.particles.push_back(RenderParticle{});
    world.decals.push_back(RenderDecal{});
    CHECK(spritesOnly(world));

    RenderWorld withMesh = world;
    withMesh.draws.push_back(DrawItem{});
    CHECK_FALSE(spritesOnly(withMesh));
    RenderWorld withTerrain = world;
    withTerrain.terrains.push_back(RenderTerrain{});
    CHECK_FALSE(spritesOnly(withTerrain));
    RenderWorld withFoliage = world;
    withFoliage.foliageRuns.push_back(RenderFoliageRun{});
    CHECK_FALSE(spritesOnly(withFoliage));
    // And a 3D world with no sprites is not one either.
    withMesh.sprites.clear();
    CHECK_FALSE(spritesOnly(withMesh));
}

TEST_CASE("ADR 0185: the rectangle a frame's decals can reach, which is where their masks are drawn")
{
    using namespace render;
    // No decal: nowhere.
    CHECK(decalCoverage({}, core::Mat4{}, 100, 200).width == 0);

    // The unit box through a camera that changes nothing: the middle half of
    // the picture each way, and a pixel of slack round it.
    std::vector<RenderDecal> decals(1);
    const rhi::Rect middle = decalCoverage(decals, core::Mat4{}, 100, 200);
    CHECK(middle.x == 24);
    CHECK(middle.y == 49);
    CHECK(middle.width == 52);
    CHECK(middle.height == 102);

    // A second one, up and to the right: the rectangle is of both, and stops
    // at the picture's edge. Up is the top of the picture, where y is 0.
    RenderDecal corner;
    corner.boxToWorld.m[3][0] = 0.75f;
    corner.boxToWorld.m[3][1] = 0.75f;
    decals.push_back(corner);
    const rhi::Rect both = decalCoverage(decals, core::Mat4{}, 100, 200);
    CHECK(both.x == 24);
    CHECK(both.y == 0);
    CHECK(both.x + both.width == 100);
    CHECK(both.y + both.height == 151);

    // One wholly off the picture: nowhere, and nothing is drawn for it.
    std::vector<RenderDecal> aside(1);
    aside[0].boxToWorld.m[3][0] = 5.0f;
    const rhi::Rect none = decalCoverage(aside, core::Mat4{}, 100, 200);
    CHECK(none.width == 0);
    CHECK(none.height == 0);

    // A box the camera stands in, through a perspective: a corner behind the
    // camera projects to nowhere, so the answer is the whole picture.
    core::Mat4 perspective;
    perspective.m[2][3] = -1.0f;
    perspective.m[3][3] = 0.0f;
    const std::vector<RenderDecal> around(1);
    const rhi::Rect whole = decalCoverage(around, perspective, 100, 200);
    CHECK(whole.x == 0);
    CHECK(whole.y == 0);
    CHECK(whole.width == 100);
    CHECK(whole.height == 200);
    // And one in front of it is a rectangle again.
    std::vector<RenderDecal> ahead(1);
    ahead[0].boxToWorld.m[3][2] = -4.0f;
    const rhi::Rect small = decalCoverage(ahead, perspective, 100, 200);
    CHECK(small.width > 0);
    CHECK(small.width < 100);
    CHECK(small.height < 200);

    // How far the decals reach from the camera, which is how far a mask is
    // drawn: the unit box round it reaches to its corner, and one four metres
    // off to its far corner.
    CHECK(decalReach({}) == 0.0f);
    CHECK(nearF(decalReach(around), std::sqrt(0.75f)));
    CHECK(nearF(decalReach(ahead), std::sqrt(0.5f + 4.5f * 4.5f)));
}

TEST_CASE("render hierarchy extraction benchmark" * doctest::skip())
{
    Fixture fixture;
    fixture.registerRenderClasses();
    const auto root = fixture.world.create(fixture.workspaceClass);
    (void)fixture.cameraLookingDownNegativeZ(root);
    auto parent = root;
    for (int depth = 0; depth < 12; ++depth) {
        const auto next = fixture.world.create(fixture.folderClass);
        REQUIRE_FALSE(fixture.world.setParent(next, parent).has_value());
        parent = next;
    }
    const auto content = fixture.atoms.intern("test://hierarchy-mesh");
    render::MeshLibrary meshes;
    render::MeshLibrary::Entry mesh;
    mesh.mesh = {1, 1};
    mesh.bounds = core::AABB::fromCenterSize({}, {1.0f, 1.0f, 1.0f});
    mesh.sectionCount = 1;
    meshes.set(content, mesh);
    for (int i = 0; i < 1500; ++i) {
        const auto model = fixture.world.create(fixture.folderClass);
        REQUIRE_FALSE(fixture.world.setParent(model, parent).has_value());
        (void)fixture.meshPartAt(model, {static_cast<double>(i % 20) - 10, 0, -20}, content);
    }
    render::RenderWorld snapshot;
    for (int i = 0; i < 10; ++i)
        render::extract(fixture.world, root, {}, meshes, 1.0f, 120.0f, nullptr, 0.0f, nullptr, snapshot);
    for (int sample = 0; sample < 5; ++sample) {
        const auto began = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 500; ++frame)
            render::extract(fixture.world, root, {}, meshes, 1.0f, 120.0f, nullptr, 0.0f, nullptr, snapshot);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        REQUIRE(snapshot.draws.size() == 1500);
        std::printf("[hierarchy-benchmark] sample=%d meanMs=%.6f draws=%zu\n", sample, ms / 500, snapshot.draws.size());
    }
}

TEST_CASE("extraction membership follows moved subtrees and each requested root")
{
    Fixture fixture;
    const auto first = fixture.world.create(fixture.folderClass);
    const auto second = fixture.world.create(fixture.folderClass);
    auto branch = fixture.world.create(fixture.folderClass);
    REQUIRE_FALSE(fixture.world.setParent(branch, first).has_value());
    auto parent = branch;
    for (int depth = 0; depth < 32; ++depth) {
        const auto next = fixture.world.create(fixture.folderClass);
        REQUIRE_FALSE(fixture.world.setParent(next, parent).has_value());
        parent = next;
    }
    const auto a = fixture.part(parent);
    const auto b = fixture.part(parent);
    const auto outside = fixture.part(second);
    (void)outside;
    render::RenderWorld snapshot;
    const auto count = [&](core::InstanceId root) {
        render::extract(fixture.world, root, {}, kNoMeshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr, snapshot);
        return snapshot.parts.size();
    };
    CHECK(count(first) == 2);
    CHECK(count(second) == 1);
    CHECK(count(a) == 1);
    REQUIRE_FALSE(fixture.world.setParent(branch, second).has_value());
    CHECK(count(first) == 0);
    CHECK(count(second) == 3);
    CHECK(count(branch) == 2);
    REQUIRE_FALSE(fixture.world.setParent(b, first).has_value());
    CHECK(count(first) == 1);
    CHECK(count(second) == 2);
    fixture.world.destroy(a);
    CHECK(count(second) == 1);
    const auto replacement = fixture.part(parent);
    (void)replacement;
    CHECK(count(second) == 2);
    CHECK(count(core::InstanceId{first.index, first.generation + 1}) == 0);
}
