// The look of a world (ADR 0096): where an instance counts, and how several of
// one kind combine. `resolveLook` is the one place the rules live, and
// `lookStanding` answers the editor by the same walk.
#include <cmath>
#include <doctest/doctest.h>

#include "engine/core/types.h"
#include "engine/render/look.h"
#include "engine/render/render_world.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"
#include "engine_test_nearly.h"

using namespace engine;
using engine::testing::nearly;

namespace {

// Hand-built, as `render_world_tests.cpp`'s is: a rendering test must not fail
// for a reason that lives in the API definition files.
struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;

    scene::ClassId instanceClass = scene::InvalidClass;
    scene::ClassId folderClass = scene::InvalidClass;
    scene::ClassId lightingClass = scene::InvalidClass;
    scene::ClassId cameraClass = scene::InvalidClass;
    scene::ClassId postEffectClass = scene::InvalidClass;
    scene::ClassId bloomClass = scene::InvalidClass;
    scene::ClassId correctionClass = scene::InvalidClass;
    scene::ClassId blurClass = scene::InvalidClass;
    scene::ClassId focusClass = scene::InvalidClass;
    scene::ClassId raysClass = scene::InvalidClass;
    scene::ClassId atmosphereClass = scene::InvalidClass;
    scene::ClassId skyClass = scene::InvalidClass;

    template <typename Attach>
    scene::ClassId add(const char* name, scene::ClassId super, Attach attach)
    {
        scene::ClassDescriptor descriptor;
        descriptor.name = atoms.intern(name);
        descriptor.super = super;
        descriptor.defaultName = descriptor.name;
        descriptor.attachComponents = attach;
        return classes.registerClass(descriptor);
    }

    Fixture()
    {
        scene::ClassDescriptor instance;
        instance.name = atoms.intern("Instance");
        instance.defaultName = instance.name;
        instanceClass = classes.registerClass(instance);

        folderClass = add("Folder", instanceClass, nullptr);
        lightingClass = add("Lighting", instanceClass, [](scene::World& w, core::InstanceId id) {
            w.lighting().add(id, scene::LightingComponent{});
        });
        cameraClass = add("Camera", instanceClass,
                          [](scene::World& w, core::InstanceId id) { w.cameras().add(id, scene::CameraComponent{}); });
        postEffectClass = add("PostEffect", instanceClass, [](scene::World& w, core::InstanceId id) {
            w.postEffects().add(id, scene::PostEffectComponent{});
        });
        bloomClass = add("BloomEffect", postEffectClass, [](scene::World& w, core::InstanceId id) {
            w.bloomEffects().add(id, scene::BloomEffectComponent{});
        });
        correctionClass = add("ColorCorrectionEffect", postEffectClass, [](scene::World& w, core::InstanceId id) {
            w.colorCorrectionEffects().add(id, scene::ColorCorrectionEffectComponent{});
        });
        blurClass = add("BlurEffect", postEffectClass, [](scene::World& w, core::InstanceId id) {
            w.blurEffects().add(id, scene::BlurEffectComponent{});
        });
        focusClass = add("DepthOfFieldEffect", postEffectClass, [](scene::World& w, core::InstanceId id) {
            w.depthOfFieldEffects().add(id, scene::DepthOfFieldEffectComponent{});
        });
        raysClass = add("SunRaysEffect", postEffectClass, [](scene::World& w, core::InstanceId id) {
            w.sunRaysEffects().add(id, scene::SunRaysEffectComponent{});
        });
        atmosphereClass = add("Atmosphere", instanceClass, [](scene::World& w, core::InstanceId id) {
            w.atmospheres().add(id, scene::AtmosphereComponent{});
        });
        skyClass = add("Sky", instanceClass,
                       [](scene::World& w, core::InstanceId id) { w.skies().add(id, scene::SkyComponent{}); });
        lighting = world.create(lightingClass);
        camera = world.create(cameraClass);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    scene::World world{classes, enums, atoms, 1u};
    // Made in the constructor's body, after the classes: a default member
    // initialiser here would run before them and create two instances of no
    // class at all.
    core::InstanceId lighting;
    core::InstanceId camera;

    core::InstanceId make(scene::ClassId classId, core::InstanceId parent)
    {
        const core::InstanceId id = world.create(classId);
        REQUIRE_FALSE(world.setParent(id, parent).has_value());
        return id;
    }

    void disable(core::InstanceId id) { world.postEffects().find(id)->enabled = false; }

    [[nodiscard]] render::RenderLook resolve()
    {
        render::RenderLook look;
        render::resolveLook(world, lighting, camera, look);
        return look;
    }

    [[nodiscard]] render::LookStanding standing(core::InstanceId id)
    {
        return render::lookStanding(world, id, lighting, camera);
    }
};

} // namespace

TEST_CASE("a world with none of these resolves to the look it always had")
{
    Fixture fixture;
    // Not one of these classes, under both parents: nothing.
    (void)fixture.make(fixture.folderClass, fixture.lighting);
    (void)fixture.make(fixture.folderClass, fixture.camera);
    CHECK(fixture.resolve() == render::RenderLook{});

    // And the defaults ARE the engine's own picture: its bloom, no grade, no
    // blur, no depth of field, no rays, the linear fog and the analytic sky.
    const render::RenderLook none{};
    CHECK_FALSE(none.bloomGoverned);
    CHECK(none.bloomEnabled);
    CHECK_FALSE(none.graded);
    CHECK(none.blurSize == 0.0f);
    CHECK_FALSE(none.depthOfField);
    CHECK_FALSE(none.sunRays);
    CHECK_FALSE(none.atmosphere.present);
    CHECK_FALSE(none.sky.present);

    // No host at all -- an engine without the render module -- is the same.
    render::RenderLook look;
    render::resolveLook(fixture.world, core::InstanceId{}, core::InstanceId{}, look);
    CHECK(look == render::RenderLook{});
}

TEST_CASE("extraction carries the resolved look, and none without a Lighting host")
{
    Fixture fixture;
    const core::InstanceId blur = fixture.make(fixture.blurClass, fixture.lighting);
    fixture.world.blurEffects().find(blur)->size = 6.0f;
    const render::MeshLibrary meshes;
    render::RenderWorld snapshot;
    render::extract(fixture.world, fixture.lighting, fixture.lighting, meshes, 1.0f, 0.0f, nullptr, 0.0f, nullptr,
                    snapshot);
    CHECK(nearly(snapshot.look.blurSize, 6.0f));
    snapshot.clear();
    CHECK(snapshot.look == render::RenderLook{});
}

TEST_CASE("bloom: none keeps the engine's own; a disabled one turns it off; the first enabled one governs")
{
    Fixture fixture;
    CHECK_FALSE(fixture.resolve().bloomGoverned);

    const core::InstanceId off = fixture.make(fixture.bloomClass, fixture.lighting);
    fixture.disable(off);
    {
        const render::RenderLook look = fixture.resolve();
        CHECK(look.bloomGoverned);
        CHECK_FALSE(look.bloomEnabled);
        CHECK(fixture.standing(off) == render::LookStanding::Disabled);
    }

    const core::InstanceId first = fixture.make(fixture.bloomClass, fixture.lighting);
    fixture.world.bloomEffects().find(first)->intensity = 2.0f;
    const core::InstanceId second = fixture.make(fixture.bloomClass, fixture.camera);
    fixture.world.bloomEffects().find(second)->intensity = 3.0f;
    const render::RenderLook look = fixture.resolve();
    CHECK(look.bloomGoverned);
    CHECK(look.bloomEnabled);
    CHECK(nearly(look.bloomIntensity, 2.0f));
    CHECK(fixture.standing(first) == render::LookStanding::Counts);
    CHECK(fixture.standing(second) == render::LookStanding::Outranked);
}

TEST_CASE("an inserted bloom with its defaults is the engine's own bloom exactly")
{
    Fixture fixture;
    (void)fixture.make(fixture.bloomClass, fixture.lighting);
    const render::RenderLook look = fixture.resolve();
    const render::RenderLook none{};
    CHECK(look.bloomEnabled);
    CHECK(look.bloomIntensity == none.bloomIntensity);
    CHECK(look.bloomSize == none.bloomSize);
    CHECK(look.bloomThreshold == none.bloomThreshold);
}

TEST_CASE("Lighting's effects come before the camera's, whatever order they were made in")
{
    Fixture fixture;
    const core::InstanceId viewers = fixture.make(fixture.raysClass, fixture.camera);
    fixture.world.sunRaysEffects().find(viewers)->intensity = 0.9f;
    const core::InstanceId worlds = fixture.make(fixture.raysClass, fixture.lighting);
    fixture.world.sunRaysEffects().find(worlds)->intensity = 0.1f;
    const render::RenderLook look = fixture.resolve();
    CHECK(look.sunRays);
    CHECK(nearly(look.sunRaysIntensity, 0.1f));
    CHECK(fixture.standing(worlds) == render::LookStanding::Counts);
    CHECK(fixture.standing(viewers) == render::LookStanding::Outranked);
}

TEST_CASE("depth of field: a disabled one is skipped and the next enabled one wins")
{
    Fixture fixture;
    const core::InstanceId skipped = fixture.make(fixture.focusClass, fixture.lighting);
    fixture.world.depthOfFieldEffects().find(skipped)->focusDistance = 5.0f;
    fixture.disable(skipped);
    const core::InstanceId used = fixture.make(fixture.focusClass, fixture.lighting);
    fixture.world.depthOfFieldEffects().find(used)->focusDistance = 40.0f;
    const render::RenderLook look = fixture.resolve();
    CHECK(look.depthOfField);
    CHECK(nearly(look.focusDistance, 40.0f));
}

TEST_CASE("blurs combine by their squares: two of 8 are one of about 11.3")
{
    Fixture fixture;
    const core::InstanceId a = fixture.make(fixture.blurClass, fixture.lighting);
    const core::InstanceId b = fixture.make(fixture.blurClass, fixture.camera);
    fixture.world.blurEffects().find(a)->size = 8.0f;
    fixture.world.blurEffects().find(b)->size = 8.0f;
    CHECK(nearly(fixture.resolve().blurSize, std::sqrt(128.0f)));

    fixture.disable(b);
    CHECK(nearly(fixture.resolve().blurSize, 8.0f));
    CHECK(fixture.standing(b) == render::LookStanding::Disabled);
}

TEST_CASE("colour corrections apply in order, a stage each")
{
    Fixture fixture;
    // Maps a colour through the resolved grade, as the tonemap will.
    const auto apply = [](const render::RenderLook& look, core::f32 r, core::f32 g, core::f32 b) {
        const core::Color3 out = render::applyGrade(look, core::Color3{r, g, b});
        return core::Vec3{out.r, out.g, out.b};
    };

    // One with its defaults changes nothing, and is no stage at all: the frame
    // draws through the plain tonemap, the same to the bit.
    const core::InstanceId neutral = fixture.make(fixture.correctionClass, fixture.lighting);
    {
        const render::RenderLook look = fixture.resolve();
        CHECK_FALSE(look.graded);
        CHECK(look.gradeCount == 0);
        const core::Vec3 same = apply(look, 0.3f, 0.6f, 0.9f);
        CHECK(same.x == 0.3f);
        CHECK(same.y == 0.6f);
        CHECK(same.z == 0.9f);
    }

    // A tint, then a brightness: the brightness is added AFTER the tint
    // multiplied, because the second effect applies to what the first made.
    fixture.world.colorCorrectionEffects().find(neutral)->tintColor = core::Color3{0.5f, 1.0f, 1.0f};
    const core::InstanceId lift = fixture.make(fixture.correctionClass, fixture.camera);
    fixture.world.colorCorrectionEffects().find(lift)->brightness = 0.1f;
    {
        const render::RenderLook look = fixture.resolve();
        CHECK(look.graded);
        CHECK(look.gradeCount == 2);
        const core::Vec3 graded = apply(look, 0.4f, 0.4f, 0.4f);
        CHECK(nearly(graded.x, 0.4f * 0.5f + 0.1f));
        CHECK(nearly(graded.y, 0.5f));
    }

    // Saturation -1 is grey: every channel becomes the luminance.
    fixture.disable(lift);
    fixture.world.colorCorrectionEffects().find(neutral)->tintColor = core::Color3{1.0f, 1.0f, 1.0f};
    fixture.world.colorCorrectionEffects().find(neutral)->saturation = -1.0f;
    {
        const core::Vec3 grey = apply(fixture.resolve(), 1.0f, 0.0f, 0.0f);
        CHECK(nearly(grey.x, 0.2126f));
        CHECK(nearly(grey.y, 0.2126f));
        CHECK(nearly(grey.z, 0.2126f));
    }

    // Contrast turns about the exposed average, 0.45: it does not move.
    fixture.world.colorCorrectionEffects().find(neutral)->saturation = 0.0f;
    fixture.world.colorCorrectionEffects().find(neutral)->contrast = 0.5f;
    {
        const render::RenderLook look = fixture.resolve();
        const core::Vec3 pivot = apply(look, 0.45f, 0.45f, 0.45f);
        CHECK(nearly(pivot.x, 0.45f));
        // Brighter goes further from it, darker goes darker.
        CHECK(apply(look, 0.65f, 0.65f, 0.65f).x > 0.65f);
        CHECK(apply(look, 0.2f, 0.2f, 0.2f).x < 0.2f);
    }
}

TEST_CASE("D427: contrast darkens the dark half without taking it to black")
{
    // It was a line through the pivot: at +0.12 every channel under about
    // sRGB 62 came out below zero and was clamped -- a night sky of
    // (9, 30, 64) became (1, 1, 41) and at +0.5 (1, 1, 1); grass lost its
    // blue and went neon. A power about the pivot leaves no channel at zero
    // that was above it, and keeps the three in the order they were.
    Fixture fixture;
    const core::InstanceId correction = fixture.make(fixture.correctionClass, fixture.lighting);
    // A night sky and night grass, as exposed linear light.
    const core::Color3 sky{0.003f, 0.013f, 0.051f};
    const core::Color3 grass{0.127f, 0.216f, 0.037f};

    for (const core::f32 contrast : {0.12f, 0.5f, 1.0f}) {
        CAPTURE(contrast);
        fixture.world.colorCorrectionEffects().find(correction)->contrast = contrast;
        const render::RenderLook look = fixture.resolve();
        for (const core::Color3 colour : {sky, grass}) {
            const core::Color3 graded = render::applyGrade(look, colour);
            CHECK(graded.r > 0.0f);
            CHECK(graded.g > 0.0f);
            CHECK(graded.b > 0.0f);
            // Darker than it was, being under the pivot...
            CHECK(graded.b < colour.b);
            // ...and its channels in the order they were: the sky is still
            // blue, the grass still green.
            CHECK((graded.r < graded.g) == (colour.r < colour.g));
            CHECK((graded.g < graded.b) == (colour.g < colour.b));
        }
    }

    // Less contrast lifts the dark towards the pivot, and not past it.
    fixture.world.colorCorrectionEffects().find(correction)->contrast = -0.5f;
    const core::Color3 lifted = render::applyGrade(fixture.resolve(), sky);
    CHECK(lifted.b > sky.b);
    CHECK(lifted.b < render::GradePivot);

    // A brightness below zero stops at black and goes no further.
    fixture.world.colorCorrectionEffects().find(correction)->contrast = 0.0f;
    fixture.world.colorCorrectionEffects().find(correction)->brightness = -0.5f;
    const core::Color3 dark = render::applyGrade(fixture.resolve(), sky);
    CHECK(dark.r == 0.0f);
    CHECK(dark.b == 0.0f);
}

TEST_CASE("the sun's rays reach further round a hidden sun as Spread grows")
{
    // The lobe of sky that can shine was fixed, so with the disc behind one
    // tree's crown nothing in it could. Up to the default spread it is what
    // it was; past it, wider.
    CHECK(render::raysLobe(0.0f) == 24.0f);
    CHECK(render::raysLobe(0.5f) == 24.0f);
    CHECK(render::raysLobe(0.75f) < 24.0f);
    CHECK(nearly(render::raysLobe(1.0f), 1.5f));
    CHECK(render::raysLobe(1.0f) < render::raysLobe(0.75f));
}

TEST_CASE("where an effect counts: directly under Lighting or the camera, and nowhere else")
{
    Fixture fixture;
    const core::InstanceId folder = fixture.make(fixture.folderClass, fixture.lighting);
    const core::InstanceId buried = fixture.make(fixture.blurClass, folder);
    fixture.world.blurEffects().find(buried)->size = 10.0f;
    CHECK(fixture.resolve().blurSize == 0.0f);
    CHECK(fixture.standing(buried) == render::LookStanding::WrongParent);

    const core::InstanceId loose = fixture.world.create(fixture.blurClass);
    CHECK(fixture.standing(loose) == render::LookStanding::WrongParent);
    CHECK(fixture.standing(folder) == render::LookStanding::NotALook);
}

TEST_CASE("an Atmosphere and a Sky count only under Lighting, and only the first of each")
{
    Fixture fixture;
    const core::InstanceId onCamera = fixture.make(fixture.skyClass, fixture.camera);
    CHECK_FALSE(fixture.resolve().sky.present);
    CHECK(fixture.standing(onCamera) == render::LookStanding::WrongParent);

    const core::InstanceId sky = fixture.make(fixture.skyClass, fixture.lighting);
    fixture.world.skies().find(sky)->cloudCover = 0.4f;
    const core::InstanceId spare = fixture.make(fixture.skyClass, fixture.lighting);
    fixture.world.skies().find(spare)->cloudCover = 0.9f;
    const core::InstanceId air = fixture.make(fixture.atmosphereClass, fixture.lighting);
    fixture.world.atmospheres().find(air)->density = 0.7f;
    const core::InstanceId secondAir = fixture.make(fixture.atmosphereClass, fixture.lighting);

    const render::RenderLook look = fixture.resolve();
    CHECK(look.sky.present);
    CHECK(nearly(look.sky.cloudCover, 0.4f));
    CHECK_FALSE(look.sky.hasImages());
    CHECK(look.atmosphere.present);
    CHECK(nearly(look.atmosphere.density, 0.7f));
    CHECK(fixture.standing(sky) == render::LookStanding::Counts);
    CHECK(fixture.standing(spare) == render::LookStanding::NotFirst);
    CHECK(fixture.standing(secondAir) == render::LookStanding::NotFirst);
}

TEST_CASE("a destroyed effect is not part of the look")
{
    Fixture fixture;
    const core::InstanceId blur = fixture.make(fixture.blurClass, fixture.lighting);
    fixture.world.blurEffects().find(blur)->size = 12.0f;
    CHECK(nearly(fixture.resolve().blurSize, 12.0f));
    (void)fixture.world.destroy(blur);
    CHECK(fixture.resolve().blurSize == 0.0f);
}

TEST_CASE("the air hides what its documentation says, at the distances it names")
{
    // `Atmosphere.Density`'s own sentence: at the default, half of what stands
    // 280 metres away still shows at the height of `Offset`; at 1, half is gone
    // within 35 metres. Level rays, at the offset's own height.
    render::RenderAtmosphere air;
    air.present = true;
    air.density = 0.35f;
    const render::AirMedium medium = render::airMediumOf(air, 0.0);
    CHECK(nearly(std::exp(-render::airOpticalDepth(medium, 0.0f, 280.0f)), 0.5f, 0.02f));

    air.density = 1.0f;
    const render::AirMedium thick = render::airMediumOf(air, 0.0);
    CHECK(nearly(std::exp(-render::airOpticalDepth(thick, 0.0f, 35.0f)), 0.5f, 0.02f));

    // Thinner with height: from 100 metres up, with `Decay` at its default,
    // the same level ray crosses half the air.
    const render::AirMedium high = render::airMediumOf(render::RenderAtmosphere{.present = true}, 100.0);
    CHECK(nearly(render::airOpticalDepth(high, 0.0f, 280.0f) / render::airOpticalDepth(medium, 0.0f, 280.0f), 0.5f,
                 1e-3f));

    // Looking up, the sky's reach is finite even with no end to the air: the
    // integral of a falling exponential converges.
    const core::f32 upward = render::airOpticalDepth(medium, 1.0f, 40000.0f);
    CHECK(std::isfinite(upward));
    CHECK(upward < render::airOpticalDepth(medium, 0.0f, 40000.0f));
}

TEST_CASE("D460: the automatic exposure is held to the world's own limits, and the defaults are the old ones")
{
    // An evenly lit world was as bright at eleven at night as at noon: the
    // meter's limits were the engine's and no world could move them.
    const render::ExposureRange defaults =
        render::exposureRange(render::DefaultExposureMin, render::DefaultExposureMax);
    // To the bit: what every picture before this was made with.
    CHECK(defaults.lowest == 0.15f);
    CHECK(defaults.highest == 3.0f);
    // And what the numbers mean: a stop and a half up, two and three quarters down.
    CHECK(static_cast<double>(render::GradePivot / defaults.lowest) == doctest::Approx(3.0));
    CHECK(static_cast<double>(std::log2(render::GradePivot / defaults.highest)) ==
          doctest::Approx(-2.737).epsilon(0.001));

    // **Night stays dark**: with no brightening allowed, the lowest average
    // the meter accepts is the key itself, so the gain never passes one.
    const render::ExposureRange night = render::exposureRange(render::DefaultExposureMin, 0.0f);
    CHECK(night.lowest == render::GradePivot);
    CHECK(night.highest == 3.0f);
    // Both at one number: the exposure is fixed there.
    const render::ExposureRange fixed = render::exposureRange(1.0f, 1.0f);
    CHECK(static_cast<double>(fixed.lowest) == doctest::Approx(0.225));
    CHECK(fixed.lowest == fixed.highest);
    // Written the wrong way round, the smaller is still the floor.
    const render::ExposureRange swapped = render::exposureRange(2.0f, -1.0f);
    CHECK(swapped.lowest < swapped.highest);
    CHECK(static_cast<double>(swapped.lowest) == doctest::Approx(0.1125));
    CHECK(static_cast<double>(swapped.highest) == doctest::Approx(0.9));
}
