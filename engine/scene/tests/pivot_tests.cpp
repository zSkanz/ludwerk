// `PivotTo` below the VM (D512): exact enough to move a model every frame.
#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/core/math.h"
#include "engine/scene/components.h"
#include "engine/scene/pivot.h"
#include "engine/scene/world.h"
#include "scene_fixture.h"

using namespace engine;
using engine::scene::testing::Fixture;

namespace {

[[nodiscard]] core::CFrameD at(double x, double y, double z, core::Mat3 rotation = {})
{
    core::CFrameD out;
    out.position = core::DVec3{x, y, z};
    out.rotation = rotation;
    return out;
}

[[nodiscard]] core::Mat3 turned(float x, float y, float z)
{
    return core::rotationY(y) * core::rotationX(x) * core::rotationZ(z);
}

// How far a basis is from orthonormal: the worst entry of R^T R - I.
[[nodiscard]] double skew(const core::Mat3& r)
{
    double worst = 0.0;
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k)
                dot += static_cast<double>(r.m[a][k]) * static_cast<double>(r.m[b][k]);
            worst = std::max(worst, std::abs(dot - (a == b ? 1.0 : 0.0)));
        }
    }
    return worst;
}

[[nodiscard]] double distance(const core::DVec3& a, const core::DVec3& b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

struct Figure
{
    core::InstanceId model;
    core::InstanceId primary;
    std::vector<core::InstanceId> parts;
    // Each part in the primary's frame, as built.
    std::vector<core::CFrameD> offsets;
};

[[nodiscard]] Figure figure(Fixture& fixture, core::InstanceId parent)
{
    Figure out;
    out.model = fixture.model("Head");
    REQUIRE_FALSE(fixture.world.setParent(out.model, parent).has_value());
    const core::CFrameD frames[] = {
        at(0.0, 1.1, 0.0),
        at(0.0, 1.7, 0.0, turned(0.6f, 0.6f, 0.3f)),
        at(0.4, 1.3, -0.2, turned(-0.9f, 0.2f, 1.1f)),
        at(-0.5, 0.9, 0.3, turned(0.3f, -1.4f, 0.7f)),
        at(0.1, 2.2, 0.6, turned(1.3f, 0.9f, -0.4f)),
        at(-0.2, 1.5, -0.7, turned(-0.2f, 2.5f, 0.9f)),
    };
    for (const core::CFrameD& frame : frames) {
        const core::InstanceId part = fixture.part("Piece");
        REQUIRE_FALSE(fixture.world.setParent(part, out.model).has_value());
        fixture.world.parts().find(part)->cframe = frame;
        out.parts.push_back(part);
    }
    out.primary = out.parts.front();
    fixture.world.models().find(out.model)->primaryPart = out.primary;
    for (const core::InstanceId part : out.parts)
        out.offsets.push_back(core::inverse(frames[0]) * fixture.world.parts().find(part)->cframe);
    return out;
}

} // namespace

TEST_CASE("D512: a model moved by PivotTo ten thousand times stays where it was put, and rigid")
{
    // A model turned by `PivotTo` every frame left for kilometres within a
    // second: each call inverted its pivot's f32 rotation by transpose, and the
    // drift off orthonormal came back as a scale on the next one.
    Fixture fixture;
    const core::InstanceId workspace = fixture.folder("Workspace");
    const Figure head = figure(fixture, workspace);

    float yaw = 0.0f;
    for (int frame = 0; frame < 10000; ++frame) {
        yaw += 0.8f / 60.0f;
        const core::CFrameD target = at(4.0, 1.1, 0.0, core::rotationY(yaw));
        scene::pivotTo(fixture.world, head.model, target);
    }

    const core::CFrameD target = at(4.0, 1.1, 0.0, core::rotationY(yaw));
    CHECK(distance(scene::pivotOf(fixture.world, head.model).position, target.position) < 1e-6);
    for (std::size_t index = 0; index < head.parts.size(); ++index) {
        const core::CFrameD& placed = fixture.world.parts().find(head.parts[index])->cframe;
        const core::CFrameD expected = target * head.offsets[index];
        CHECK(distance(placed.position, expected.position) < 1e-4);
        CHECK(skew(placed.rotation) < 1e-5);
    }
}

TEST_CASE("D512: PivotTo onto the pivot a model already has changes no bit")
{
    Fixture fixture;
    const core::InstanceId workspace = fixture.folder("Workspace");
    const Figure head = figure(fixture, workspace);
    scene::pivotTo(fixture.world, head.model, at(4.0, 1.1, 0.0, core::rotationY(0.7f)));

    std::vector<core::CFrameD> before;
    for (const core::InstanceId part : head.parts)
        before.push_back(fixture.world.parts().find(part)->cframe);
    scene::pivotTo(fixture.world, head.model, scene::pivotOf(fixture.world, head.model));
    for (std::size_t index = 0; index < head.parts.size(); ++index) {
        const core::CFrameD& now = fixture.world.parts().find(head.parts[index])->cframe;
        CHECK(now.position.x == before[index].position.x);
        CHECK(now.position.y == before[index].position.y);
        CHECK(now.position.z == before[index].position.z);
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row)
                CHECK(now.rotation.m[column][row] == before[index].rotation.m[column][row]);
    }
}

TEST_CASE("D512: a model inside a model turned every frame keeps its place in it")
{
    Fixture fixture;
    const core::InstanceId workspace = fixture.folder("Workspace");
    const core::InstanceId tower = fixture.model("Tower");
    REQUIRE_FALSE(fixture.world.setParent(tower, workspace).has_value());
    const core::InstanceId base = fixture.part("Base");
    REQUIRE_FALSE(fixture.world.setParent(base, tower).has_value());
    fixture.world.parts().find(base)->cframe = at(0.0, 0.0, 0.0);
    fixture.world.models().find(tower)->primaryPart = base;
    const Figure head = figure(fixture, tower);

    float yaw = 0.0f;
    for (int frame = 0; frame < 5000; ++frame) {
        yaw += 0.8f / 60.0f;
        // The head turns in the tower's frame, as a turret's does.
        scene::pivotTo(fixture.world, head.model,
                       scene::pivotOf(fixture.world, tower) * at(0.0, 1.1, 0.0, core::rotationY(yaw)));
        scene::pivotTo(fixture.world, tower, at(10.0, 0.0, 5.0, core::rotationY(-yaw * 0.5f)));
    }

    // The head, in the tower's frame, where its last move put it.
    const core::CFrameD towerPivot = scene::pivotOf(fixture.world, tower);
    const core::CFrameD headPivot = scene::pivotOf(fixture.world, head.model);
    const core::CFrameD relative = core::inverse(towerPivot) * headPivot;
    CHECK(distance(relative.position, core::DVec3{0.0, 1.1, 0.0}) < 1e-4);
    for (std::size_t index = 0; index < head.parts.size(); ++index) {
        const core::CFrameD& placed = fixture.world.parts().find(head.parts[index])->cframe;
        CHECK(distance(placed.position, (headPivot * head.offsets[index]).position) < 1e-4);
        CHECK(skew(placed.rotation) < 1e-5);
    }
}
