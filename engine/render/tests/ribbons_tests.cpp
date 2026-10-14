// Beams and trails (ADR 0129): what the frame builds from two attachments.
//
// A trail is the one thing here with a memory, so most of this is about it:
// that its length is its speed times its `Lifetime`, that `MaxLength` cuts it
// shorter, that `Clear` and `Enabled` do what they say, and that standing
// still grows nothing. A beam keeps nothing at all, so what is asserted is
// what one frame's ribbon is: how many quads, how wide, which way it faces.
#include <cmath>
#include <doctest/doctest.h>

#include "engine/render/render_world.h"
#include "engine/render/ribbons.h"
#include "engine/scene/world.h"

using namespace engine;
using namespace engine::render;
using core::f32;
using core::f64;

namespace {

struct RibbonFixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId folderClass = classes.registerClass({.name = atoms.intern("Folder")});
    scene::ClassId partClass = classes.registerClass({.name = atoms.intern("Part")});
    scene::ClassId attachmentClass = classes.registerClass({.name = atoms.intern("Attachment")});
    scene::ClassId trailClass = classes.registerClass({.name = atoms.intern("Trail")});
    scene::ClassId beamClass = classes.registerClass({.name = atoms.intern("Beam")});
    scene::World world{classes, enums, atoms, 1234u};
    core::InstanceId root;
    // A blade: one part, an attachment at its hilt and one a metre up it.
    core::InstanceId blade;
    core::InstanceId hilt;
    core::InstanceId tip;
    // And a post ten metres along X, for a beam's other end.
    core::InstanceId post;
    core::InstanceId far;
    RibbonSystem system;

    RibbonFixture()
    {
        root = world.create(folderClass);
        blade = part(core::DVec3{0.0, 10.0, 0.0});
        hilt = attachment(blade, core::Vec3{0.0f, 0.0f, 0.0f});
        tip = attachment(blade, core::Vec3{0.0f, 1.0f, 0.0f});
        post = part(core::DVec3{10.0, 10.0, 0.0});
        far = attachment(post, core::Vec3{0.0f, 0.0f, 0.0f});
    }

    core::InstanceId part(core::DVec3 at)
    {
        const core::InstanceId id = world.create(partClass);
        world.parts().add(id, scene::PartComponent{});
        world.parts().find(id)->cframe.position = at;
        REQUIRE_FALSE(world.setParent(id, root).has_value());
        return id;
    }

    core::InstanceId attachment(core::InstanceId on, core::Vec3 offset)
    {
        const core::InstanceId id = world.create(attachmentClass);
        scene::AttachmentComponent component;
        component.cframe.position = core::toDVec3(offset);
        world.attachments().add(id, component);
        REQUIRE_FALSE(world.setParent(id, on).has_value());
        return id;
    }

    core::InstanceId trail()
    {
        const core::InstanceId id = world.create(trailClass);
        scene::TrailComponent component;
        component.attachment0 = hilt;
        component.attachment1 = tip;
        world.trails().add(id, component);
        REQUIRE_FALSE(world.setParent(id, blade).has_value());
        return id;
    }

    core::InstanceId beam()
    {
        const core::InstanceId id = world.create(beamClass);
        scene::BeamComponent component;
        component.attachment0 = hilt;
        component.attachment1 = far;
        world.beams().add(id, component);
        REQUIRE_FALSE(world.setParent(id, root).has_value());
        return id;
    }

    // The blade carried along +X at `speed` for `seconds`, a frame at a time.
    void sweep(f64 speed, f64 seconds, f64 dt = 1.0 / 60.0)
    {
        const int frames = static_cast<int>(std::lround(seconds / dt));
        for (int at = 0; at < frames; ++at) {
            world.parts().find(blade)->cframe.position.x += speed * dt;
            system.update(world, root, dt);
        }
    }

    // What one frame draws, from a camera at `eye` (camera-relative space has
    // the camera at its origin).
    RenderWorld drawn(core::DVec3 eye = core::DVec3{0.0, 10.0, 20.0})
    {
        RenderWorld out;
        out.camera.origin = eye;
        system.append(world, root, out, nullptr);
        return out;
    }
};

[[nodiscard]] f32 distance(core::Vec3 a, core::Vec3 b)
{
    const core::Vec3 d = a - b;
    return std::sqrt(core::dot(d, d));
}

} // namespace

TEST_CASE("a trail's length is how far its ends went in its lifetime")
{
    RibbonFixture fixture;
    const core::InstanceId trail = fixture.trail();
    fixture.world.trails().find(trail)->lifetime = 1.0f;
    fixture.world.trails().find(trail)->minLength = 0.1f;

    // Six metres a second for three seconds: three lifetimes, so the trail is
    // as long as it will ever be -- one second of travel.
    fixture.sweep(6.0, 3.0);
    CHECK(static_cast<f64>(fixture.system.trailLength(trail)) == doctest::Approx(6.0).epsilon(0.05));

    // Twice the lifetime is twice the trail.
    fixture.world.trails().find(trail)->lifetime = 2.0f;
    fixture.sweep(6.0, 4.0);
    CHECK(static_cast<f64>(fixture.system.trailLength(trail)) == doctest::Approx(12.0).epsilon(0.05));

    // And twice the speed, twice again.
    fixture.sweep(12.0, 4.0);
    CHECK(static_cast<f64>(fixture.system.trailLength(trail)) == doctest::Approx(24.0).epsilon(0.05));
}

TEST_CASE("a trail is no longer than its MaxLength")
{
    RibbonFixture fixture;
    const core::InstanceId trail = fixture.trail();
    fixture.world.trails().find(trail)->lifetime = 10.0f;
    fixture.world.trails().find(trail)->maxLength = 2.0f;
    fixture.sweep(6.0, 3.0);
    // The cap, and at most the one piece that straddles it.
    CHECK(fixture.system.trailLength(trail) <= 2.0f + 0.1f + 0.11f);
    CHECK(fixture.system.trailLength(trail) >= 1.8f);
}

TEST_CASE("a trail that stands still grows nothing, and one that is cleared begins again")
{
    RibbonFixture fixture;
    const core::InstanceId trail = fixture.trail();
    fixture.sweep(0.0, 1.0);
    CHECK(fixture.system.trailPieces(trail) == 1);
    CHECK(fixture.system.trailLength(trail) == 0.0f);
    // One piece and a live edge on top of it: nothing to draw.
    CHECK(fixture.drawn().ribbonVertices.empty());

    fixture.sweep(6.0, 1.0);
    CHECK(fixture.system.trailPieces(trail) > 10);
    CHECK_FALSE(fixture.drawn().ribbonVertices.empty());

    // `Clear`: the total goes up, and the next frame the ribbon is gone.
    ++fixture.world.trails().find(trail)->cleared;
    fixture.sweep(0.0, 1.0 / 60.0);
    CHECK(fixture.system.trailPieces(trail) == 1);
    CHECK(fixture.system.trailLength(trail) == 0.0f);
}

TEST_CASE("a disabled trail stops growing and what is there fades out over its lifetime")
{
    RibbonFixture fixture;
    const core::InstanceId trail = fixture.trail();
    fixture.world.trails().find(trail)->lifetime = 1.0f;
    fixture.sweep(6.0, 1.0);
    const core::usize before = fixture.system.trailPieces(trail);
    REQUIRE(before > 10);

    fixture.world.trails().find(trail)->enabled = false;
    fixture.sweep(6.0, 0.5);
    // Half of it has outlived its second; nothing new was laid.
    CHECK(fixture.system.trailPieces(trail) < before);
    CHECK(fixture.system.trailPieces(trail) > 0);
    fixture.sweep(6.0, 0.6);
    CHECK(fixture.system.trailPieces(trail) == 0);
    CHECK(fixture.drawn().ribbonVertices.empty());
}

TEST_CASE("a trail is the surface its two ends swept, as wide as they are apart times its WidthScale")
{
    RibbonFixture fixture;
    const core::InstanceId trail = fixture.trail();
    fixture.sweep(6.0, 0.5);
    const RenderWorld full = fixture.drawn();
    REQUIRE(full.ribbonVertices.size() >= 6);
    REQUIRE(full.ribbonRuns.size() == 1);
    CHECK(full.ribbonRuns[0].vertexCount == full.ribbonVertices.size());
    // A quad's first two corners are one rung: the hilt and the tip, a metre
    // apart.
    CHECK(static_cast<f64>(distance(full.ribbonVertices[0].position, full.ribbonVertices[1].position)) ==
          doctest::Approx(1.0).epsilon(0.01));

    // Half as wide for its whole life.
    fixture.world.trails().find(trail)->widthScale.keypoints = {core::NumberKeypoint{0.0f, 0.5f, 0.0f},
                                                                core::NumberKeypoint{1.0f, 0.5f, 0.0f}};
    const RenderWorld half = fixture.drawn();
    CHECK(static_cast<f64>(distance(half.ribbonVertices[0].position, half.ribbonVertices[1].position)) ==
          doctest::Approx(0.5).epsilon(0.01));

    // A piece that is gone is not drawn: fully transparent at the old end
    // means fewer quads than pieces.
    fixture.world.trails().find(trail)->transparency.keypoints = {core::NumberKeypoint{0.0f, 1.0f, 0.0f},
                                                                  core::NumberKeypoint{1.0f, 1.0f, 0.0f}};
    CHECK(fixture.drawn().ribbonVertices.empty());
}

TEST_CASE("a straight beam is one quad between its attachments, and a curved one is its Segments")
{
    RibbonFixture fixture;
    const core::InstanceId beam = fixture.beam();
    scene::BeamComponent& config = *fixture.world.beams().find(beam);
    config.width0 = 2.0f;
    config.width1 = 0.5f;

    const RenderWorld straight = fixture.drawn();
    REQUIRE(straight.ribbonVertices.size() == 6);
    // The rung at the hilt is `Width0` across, the one at the post `Width1`.
    CHECK(static_cast<f64>(distance(straight.ribbonVertices[0].position, straight.ribbonVertices[1].position)) ==
          doctest::Approx(2.0).epsilon(0.001));
    CHECK(static_cast<f64>(distance(straight.ribbonVertices[2].position, straight.ribbonVertices[5].position)) ==
          doctest::Approx(0.5).epsilon(0.001));
    // Facing a camera on +Z, a beam along X is laid out along Y: its corners
    // are all at the camera's depth of it.
    for (const RenderRibbonVertex& vertex : straight.ribbonVertices)
        CHECK(static_cast<f64>(vertex.position.z) == doctest::Approx(-20.0).epsilon(0.001));
    // Stretched once over its length: 0 at one end and 1 at the other.
    CHECK(static_cast<f64>(straight.ribbonVertices[0].u) == doctest::Approx(0.0));
    CHECK(static_cast<f64>(straight.ribbonVertices[2].u) == doctest::Approx(1.0));
    CHECK(fixture.system.stats().beams == 1);
    CHECK(fixture.system.stats().pieces == 1);

    config.curveSize0 = 3.0f;
    config.segments = 12;
    CHECK(fixture.drawn().ribbonVertices.size() == 12 * 6);

    // `Wrap`: a repeat every `TextureLength` metres -- ten metres is five of
    // two.
    config.curveSize0 = 0.0f;
    config.textureMode = 1;
    config.textureLength = 2.0f;
    CHECK(static_cast<f64>(fixture.drawn().ribbonVertices[2].u) == doctest::Approx(5.0).epsilon(0.001));
}

TEST_CASE("a beam with an end missing, or disabled, or outside the world, draws nothing")
{
    RibbonFixture fixture;
    const core::InstanceId beam = fixture.beam();
    CHECK(fixture.drawn().ribbonVertices.size() == 6);

    fixture.world.beams().find(beam)->enabled = false;
    CHECK(fixture.drawn().ribbonVertices.empty());
    fixture.world.beams().find(beam)->enabled = true;

    fixture.world.beams().find(beam)->attachment1 = core::InstanceId{};
    CHECK(fixture.drawn().ribbonVertices.empty());
    fixture.world.beams().find(beam)->attachment1 = fixture.far;

    // Parented to nothing: not in the world being drawn.
    REQUIRE_FALSE(fixture.world.setParent(beam, core::InstanceId{}).has_value());
    CHECK(fixture.drawn().ribbonVertices.empty());
}

TEST_CASE("ribbons are drawn back to front, and neighbours with one texture are one run")
{
    RibbonFixture fixture;
    (void)fixture.beam();
    // A second beam, twenty metres further from the camera.
    const core::InstanceId back = fixture.part(core::DVec3{0.0, 10.0, -20.0});
    const core::InstanceId backEnd = fixture.attachment(back, core::Vec3{0.0f, 0.0f, 0.0f});
    const core::InstanceId backPost = fixture.part(core::DVec3{10.0, 10.0, -20.0});
    const core::InstanceId backFar = fixture.attachment(backPost, core::Vec3{0.0f, 0.0f, 0.0f});
    const core::InstanceId second = fixture.world.create(fixture.beamClass);
    scene::BeamComponent component;
    component.attachment0 = backEnd;
    component.attachment1 = backFar;
    fixture.world.beams().add(second, component);
    REQUIRE_FALSE(fixture.world.setParent(second, fixture.root).has_value());

    const RenderWorld out = fixture.drawn();
    REQUIRE(out.ribbonVertices.size() == 12);
    // The far one first.
    CHECK(out.ribbonVertices[0].position.z < out.ribbonVertices[6].position.z);
    // Neither has a texture: one run.
    CHECK(out.ribbonRuns.size() == 1);
    CHECK(out.ribbonRuns[0].vertexCount == 12);
}
