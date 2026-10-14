// What ADR 0128 adds to the draw list: a `CanvasGroup` drawn as one picture,
// the look of a selected object, and scaled text kept between two sizes.
#include <algorithm>
#include <doctest/doctest.h>
#include <optional>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/scene/world.h"
#include "engine/ui/scene_types.h"
#include "engine/ui/ui.h"

namespace {

namespace core = engine::core;
namespace scene = engine::scene;
namespace ui = engine::ui;

using core::InstanceId;
using core::UDim;
using core::UDim2;
using core::Vec2;

struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    InstanceId service;
    InstanceId screen;
    ui::DrawList list;

    Fixture()
    {
        scene::generated::registerClasses(classes, atoms);
        ui::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
        service = make("UIService");
        screen = child("ScreenGui", service);
    }

    InstanceId make(const char* className)
    {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != scene::InvalidClass);
        return world->create(id);
    }

    InstanceId child(const char* className, InstanceId parent)
    {
        const InstanceId id = make(className);
        REQUIRE_FALSE(world->setParent(id, parent).has_value());
        return id;
    }

    InstanceId box(const char* className, InstanceId parent, float x, float y, float w, float h)
    {
        const InstanceId id = child(className, parent);
        object(id).position = UDim2{UDim{0.0f, x}, UDim{0.0f, y}};
        object(id).size = UDim2{UDim{0.0f, w}, UDim{0.0f, h}};
        return id;
    }

    [[nodiscard]] scene::UIObjectComponent& object(InstanceId id)
    {
        scene::UIObjectComponent* component = world->uiObjects().find(id);
        REQUIRE(component != nullptr);
        return *component;
    }

    // Laid out and drawn, as a frame does.
    const ui::DrawList& draw()
    {
        world->screenGuis().find(screen)->layoutDirty = true;
        ui::layout(*world, service, Vec2{800.0f, 600.0f});
        ui::buildDrawList(*world, service, list);
        return list;
    }
};

} // namespace

TEST_CASE("a canvas group's contents are drawn into its picture, and one quad shows it")
{
    Fixture fixture;
    const InstanceId window = fixture.box("CanvasGroup", fixture.screen, 100.0f, 50.0f, 200.0f, 100.0f);
    fixture.world->canvasGroups().find(window)->groupTransparency = 0.5f;
    const InstanceId first = fixture.box("Frame", window, 10.0f, 10.0f, 50.0f, 50.0f);
    const InstanceId second = fixture.box("Frame", window, 30.0f, 30.0f, 50.0f, 50.0f);
    fixture.object(first).backgroundColor = core::Color3{1.0f, 0.0f, 0.0f};
    fixture.object(second).backgroundColor = core::Color3{0.0f, 0.0f, 1.0f};
    (void)fixture.box("Frame", fixture.screen, 500.0f, 400.0f, 20.0f, 20.0f);

    const ui::DrawList& list = fixture.draw();
    REQUIRE(list.groups.size() == 1);
    CHECK(list.groups[0].owner == window);
    CHECK(list.groups[0].parent == 0);
    CHECK(list.groups[0].box.min.x == doctest::Approx(100.0));
    CHECK(list.groups[0].box.max.y == doctest::Approx(150.0));

    // The window's own background and its two children, at full strength, in
    // the picture; then the picture, half there, on the screen; then the frame
    // that is not in the group.
    REQUIRE(list.quads.size() == 5);
    for (size_t index = 0; index < 3; ++index) {
        CHECK(list.quads[index].group == 1);
        CHECK(list.quads[index].groupPicture == 0);
        CHECK(list.quads[index].alpha == doctest::Approx(1.0));
    }
    const ui::DrawQuad& picture = list.quads[3];
    CHECK(picture.group == 0);
    CHECK(picture.groupPicture == 1);
    CHECK(picture.alpha == doctest::Approx(0.5));
    CHECK(picture.min.x == doctest::Approx(100.0));
    CHECK(picture.min.y == doctest::Approx(50.0));
    CHECK(picture.max.x == doctest::Approx(300.0));
    CHECK(picture.max.y == doctest::Approx(150.0));
    CHECK(list.quads[4].group == 0);
    CHECK(list.quads[4].groupPicture == 0);

    // Its contents are clipped to its box, whatever the screen's clip is.
    const core::Rect& clip = list.scissors[list.quads[1].scissor];
    CHECK(clip.min.x == doctest::Approx(100.0));
    CHECK(clip.max.x == doctest::Approx(300.0));
    CHECK(list.quads[3].scissor == 0);

    SUBCASE("the picture is the same until something in it changes")
    {
        const core::u64 before = list.groups[0].signature;
        CHECK(fixture.draw().groups[0].signature == before);
        // How solid the group is, is not in the picture.
        fixture.world->canvasGroups().find(window)->groupTransparency = 0.25f;
        CHECK(fixture.draw().groups[0].signature == before);
        fixture.object(second).backgroundColor = core::Color3{0.0f, 1.0f, 0.0f};
        CHECK(fixture.draw().groups[0].signature != before);
        const core::u64 recoloured = fixture.draw().groups[0].signature;
        fixture.object(first).position = UDim2{UDim{0.0f, 11.0f}, UDim{0.0f, 10.0f}};
        CHECK(fixture.draw().groups[0].signature != recoloured);
        CHECK_FALSE(fixture.draw().groups[0].live);
    }

    SUBCASE("a group that is not there at all draws no picture")
    {
        fixture.world->canvasGroups().find(window)->groupTransparency = 1.0f;
        const ui::DrawList& gone = fixture.draw();
        CHECK(std::none_of(gone.quads.begin(), gone.quads.end(),
                           [](const ui::DrawQuad& quad) { return quad.groupPicture != 0; }));
    }

    SUBCASE("the group's colour tints the picture, not the things in it")
    {
        fixture.world->canvasGroups().find(window)->groupColor = core::Color3{0.5f, 0.25f, 1.0f};
        const ui::DrawList& tinted = fixture.draw();
        CHECK(tinted.quads[3].color.r == doctest::Approx(0.5));
        CHECK(tinted.quads[3].color.g == doctest::Approx(0.25));
        CHECK(tinted.quads[1].color.r == doctest::Approx(1.0));
    }

    SUBCASE("a group inside a group is a picture in a picture, listed after it")
    {
        const InstanceId inner = fixture.box("CanvasGroup", window, 100.0f, 0.0f, 80.0f, 80.0f);
        (void)fixture.box("Frame", inner, 0.0f, 0.0f, 10.0f, 10.0f);
        const ui::DrawList& nested = fixture.draw();
        REQUIRE(nested.groups.size() == 2);
        CHECK(nested.groups[0].owner == window);
        CHECK(nested.groups[1].owner == inner);
        CHECK(nested.groups[1].parent == 1);
        // The inner picture is a quad of the outer one.
        const auto shown = std::find_if(nested.quads.begin(), nested.quads.end(),
                                        [](const ui::DrawQuad& quad) { return quad.groupPicture == 2; });
        REQUIRE(shown != nested.quads.end());
        CHECK(shown->group == 1);
    }

    SUBCASE("on a world canvas a group is a frame")
    {
        const InstanceId sign = fixture.make("SurfaceGui");
        const InstanceId onSign = fixture.box("CanvasGroup", sign, 0.0f, 0.0f, 50.0f, 50.0f);
        (void)fixture.box("Frame", onSign, 0.0f, 0.0f, 10.0f, 10.0f);
        ui::layoutCanvas(*fixture.world, sign, Vec2{100.0f, 100.0f});
        ui::DrawList canvas;
        ui::buildCanvasDrawList(*fixture.world, sign, canvas);
        CHECK(canvas.groups.empty());
        CHECK(canvas.quads.size() == 2);
        CHECK(std::none_of(canvas.quads.begin(), canvas.quads.end(),
                           [](const ui::DrawQuad& quad) { return quad.group != 0 || quad.groupPicture != 0; }));
    }
}

TEST_CASE("ZIndex orders a group's contents among themselves, inside its picture")
{
    Fixture fixture;
    const InstanceId window = fixture.box("CanvasGroup", fixture.screen, 0.0f, 0.0f, 100.0f, 100.0f);
    fixture.object(window).backgroundTransparency = 1.0f;
    const InstanceId under = fixture.box("Frame", window, 0.0f, 0.0f, 10.0f, 10.0f);
    const InstanceId over = fixture.box("Frame", window, 0.0f, 0.0f, 20.0f, 20.0f);
    fixture.object(under).zIndex = 5.0f;
    // Something outside the group with a ZIndex between the two: it is not
    // between them, because the group is one thing on the screen.
    const InstanceId outside = fixture.box("Frame", fixture.screen, 0.0f, 0.0f, 30.0f, 30.0f);
    fixture.object(outside).zIndex = 3.0f;
    (void)over;

    const ui::DrawList& list = fixture.draw();
    REQUIRE(list.quads.size() == 4);
    // In the picture: the 20 (ZIndex 0) and then the 10 (ZIndex 5).
    CHECK(list.quads[0].max.x == doctest::Approx(20.0));
    CHECK(list.quads[1].max.x == doctest::Approx(10.0));
    CHECK(list.quads[2].groupPicture == 1);
    CHECK(list.quads[3].max.x == doctest::Approx(30.0));
}

TEST_CASE("the selected object is outlined, over everything on its screen")
{
    Fixture fixture;
    const InstanceId button = fixture.box("TextButton", fixture.screen, 100.0f, 100.0f, 80.0f, 30.0f);
    (void)fixture.box("Frame", fixture.screen, 0.0f, 0.0f, 400.0f, 400.0f);

    // Nothing selected: nothing drawn for it.
    const size_t plain = fixture.draw().quads.size();
    fixture.world->engineState().uiSelected = button;
    const ui::DrawList& list = fixture.draw();
    REQUIRE(list.quads.size() == plain + 1);
    const ui::DrawQuad& outline = list.quads.back();
    CHECK(outline.borderStroke);
    CHECK(outline.strokeBox.min.x == doctest::Approx(100.0));
    CHECK(outline.strokeBox.max.x == doctest::Approx(180.0));
    CHECK(outline.strokeBox.max.y == doctest::Approx(130.0));
    // Outside the box, not over it.
    CHECK(outline.strokeInner > 0.0f);
    CHECK(outline.strokeOuter > outline.strokeInner);

    SUBCASE("it follows the object's corners")
    {
        const InstanceId corner = fixture.child("UICorner", button);
        fixture.world->uiCorners().find(corner)->cornerRadius = UDim{0.0f, 6.0f};
        CHECK(fixture.draw().quads.back().cornerRadius == doctest::Approx(6.0));
    }

    SUBCASE("a SelectionImageObject is drawn in its place, at the object's box")
    {
        const InstanceId image = fixture.make("Frame");
        fixture.object(image).backgroundColor = core::Color3{1.0f, 1.0f, 0.0f};
        fixture.object(image).backgroundTransparency = 0.6f;
        fixture.object(button).selectionImageObject = image;
        const ui::DrawList& custom = fixture.draw();
        REQUIRE(custom.quads.size() == plain + 1);
        const ui::DrawQuad& shown = custom.quads.back();
        CHECK_FALSE(shown.borderStroke);
        CHECK(shown.min.x == doctest::Approx(100.0));
        CHECK(shown.max.y == doctest::Approx(130.0));
        CHECK(shown.color.b == doctest::Approx(0.0));
        CHECK(shown.alpha == doctest::Approx(0.4));
    }

    SUBCASE("a hidden object shows no selection")
    {
        fixture.object(button).visible = false;
        const ui::DrawList& hidden = fixture.draw();
        CHECK(std::none_of(hidden.quads.begin(), hidden.quads.end(),
                           [](const ui::DrawQuad& quad) { return quad.borderStroke; }));
    }
}

TEST_CASE("a text size constraint keeps scaled text between two sizes")
{
    Fixture fixture;
    const InstanceId label = fixture.box("TextLabel", fixture.screen, 0.0f, 0.0f, 600.0f, 300.0f);
    scene::TextLabelComponent& text = *fixture.world->textLabels().find(label);
    text.text = "Hi";
    text.textScaled = true;
    fixture.object(label).backgroundTransparency = 1.0f;

    // How tall what is drawn is: the glyphs' own extent.
    const auto drawnHeight = [&fixture]() {
        const ui::DrawList& list = fixture.draw();
        float top = 1.0e9f;
        float bottom = -1.0e9f;
        for (const ui::DrawQuad& quad : list.quads) {
            top = std::min(top, quad.min.y);
            bottom = std::max(bottom, quad.max.y);
        }
        return bottom - top;
    };

    const float free = drawnHeight();
    const InstanceId limit = fixture.child("UITextSizeConstraint", label);
    scene::UITextSizeConstraintComponent& settings = *fixture.world->uiTextSizeConstraints().find(limit);
    settings.maxTextSize = 20.0f;
    const float capped = drawnHeight();
    // Two letters in 600 by 300 are as large as scaled text goes; held to 20
    // they are a small fraction of that.
    CHECK(free > 40.0f);
    CHECK(capped < free * 0.4f);
    CHECK(capped <= 20.0f * 1.5f);

    // And a floor: text that would be tiny in a tiny box is not.
    fixture.object(label).size = UDim2{UDim{0.0f, 12.0f}, UDim{0.0f, 6.0f}};
    settings.minTextSize = 0.0f;
    settings.maxTextSize = 100.0f;
    const float tiny = drawnHeight();
    settings.minTextSize = 16.0f;
    const float held = drawnHeight();
    CHECK(held > tiny * 1.5f);
}
