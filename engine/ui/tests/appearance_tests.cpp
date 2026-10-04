#include <algorithm>
#include <doctest/doctest.h>
#include <optional>
#include <string>

#include "class_descriptors.gen.h"
#include "engine/scene/world.h"
#include "engine/ui/scene_types.h"
#include "engine/ui/ui.h"

namespace {

namespace core = engine::core;
namespace scene = engine::scene;
namespace ui = engine::ui;

using core::InstanceId;
using core::Vec2;

struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    InstanceId service;

    Fixture()
    {
        scene::generated::registerClasses(classes, atoms);
        ui::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
        service = make("UIService");
        ui::resetLayoutStats();
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

    [[nodiscard]] scene::UIObjectComponent& object(InstanceId id)
    {
        scene::UIObjectComponent* component = world->uiObjects().find(id);
        REQUIRE(component != nullptr);
        return *component;
    }

    void dirty(InstanceId screen) { world->screenGuis().find(screen)->layoutDirty = true; }

    void run(Vec2 windowSize = Vec2{1280.0f, 720.0f}) { ui::layout(*world, service, windowSize); }
};

} // namespace

// ADR 0110: what `UIGradient` and `UIStroke` put in the draw list. The pixels
// are `ui_appearance_gate`'s; these are the numbers the pixels come from.

TEST_CASE("a gradient rides on every quad its element draws, over the element's box")
{
    Fixture fixture;
    const InstanceId screen = fixture.child("ScreenGui", fixture.service);
    const InstanceId panel = fixture.child("Frame", screen);
    fixture.object(panel).position = core::UDim2{core::UDim{0.0f, 100.0f}, core::UDim{0.0f, 50.0f}};
    fixture.object(panel).size = core::UDim2{core::UDim{0.0f, 200.0f}, core::UDim{0.0f, 100.0f}};
    const InstanceId gradient = fixture.child("UIGradient", panel);
    scene::UIGradientComponent& settings = *fixture.world->uiGradients().find(gradient);
    settings.color =
        core::ColorSequence{{{0.0f, core::Color3{1.0f, 0.0f, 0.0f}}, {1.0f, core::Color3{0.0f, 0.0f, 1.0f}}}};
    settings.type = 2;
    settings.tileMode = 1;
    settings.rotation = 90.0f;
    settings.scale = 0.5f;
    settings.offset = Vec2{0.25f, -0.5f};
    fixture.run();

    ui::DrawList list;
    ui::buildDrawList(*fixture.world, fixture.service, list);
    REQUIRE(list.quads.size() == 1);
    REQUIRE(list.gradients.size() == 1);
    const ui::DrawQuad& quad = list.quads[0];
    CHECK(quad.gradient == 1u);
    CHECK(list.gradients[0].color == settings.color);
    CHECK(quad.gradientType == 2u);
    CHECK(quad.gradientTile == 1u);
    CHECK(static_cast<double>(quad.gradientAngle) == doctest::Approx(1.5707963));
    CHECK(static_cast<double>(quad.gradientScale) == doctest::Approx(0.5));
    // The offset in pixels: a quarter of the width, minus half the height.
    CHECK(static_cast<double>(quad.gradientOffset.x) == doctest::Approx(50.0));
    CHECK(static_cast<double>(quad.gradientOffset.y) == doctest::Approx(-50.0));
    CHECK(static_cast<double>(quad.gradientBox.min.x) == doctest::Approx(100.0));
    CHECK(static_cast<double>(quad.gradientBox.max.y) == doctest::Approx(150.0));

    // Disabled, it is as if there were none.
    settings.enabled = false;
    list.clear();
    ui::buildDrawList(*fixture.world, fixture.service, list);
    CHECK(list.quads[0].gradient == 0u);
    CHECK(list.gradients.empty());
}

TEST_CASE("two elements with the same colours share one row of the gradient table")
{
    Fixture fixture;
    const InstanceId screen = fixture.child("ScreenGui", fixture.service);
    for (int index = 0; index < 3; ++index) {
        const InstanceId panel = fixture.child("Frame", screen);
        fixture.object(panel).size = core::UDim2{core::UDim{0.0f, 20.0f}, core::UDim{0.0f, 20.0f}};
        const InstanceId gradient = fixture.child("UIGradient", panel);
        // The first two alike, the third different -- and the shape is not
        // part of the row, so the second's different shape still shares.
        if (index == 1)
            fixture.world->uiGradients().find(gradient)->type = 1;
        if (index == 2)
            fixture.world->uiGradients().find(gradient)->transparency =
                core::NumberSequence{{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}};
    }
    fixture.run();

    ui::DrawList list;
    ui::buildDrawList(*fixture.world, fixture.service, list);
    REQUIRE(list.quads.size() == 3);
    CHECK(list.gradients.size() == 2);
    CHECK(list.quads[0].gradient == list.quads[1].gradient);
    CHECK(list.quads[2].gradient != list.quads[0].gradient);
}

TEST_CASE("border strokes come after the element, in ZIndex order, with their band")
{
    Fixture fixture;
    const InstanceId screen = fixture.child("ScreenGui", fixture.service);
    const InstanceId panel = fixture.child("Frame", screen);
    fixture.object(panel).size = core::UDim2{core::UDim{0.0f, 100.0f}, core::UDim{0.0f, 40.0f}};

    const InstanceId front = fixture.child("UIStroke", panel);
    const InstanceId back = fixture.child("UIStroke", panel);
    scene::UIStrokeComponent& first = *fixture.world->uiStrokes().find(front);
    first.thickness = 4.0f;
    first.zIndex = 2.0f;
    first.borderStrokePosition = 2; // Inner
    scene::UIStrokeComponent& second = *fixture.world->uiStrokes().find(back);
    second.thickness = 0.1f;
    second.strokeSizingMode = 1; // a tenth of the shorter side: 4 px
    second.zIndex = 1.0f;
    second.borderOffset = core::UDim{0.0f, 3.0f};
    second.lineJoinMode = 2;
    fixture.run();

    ui::DrawList list;
    ui::buildDrawList(*fixture.world, fixture.service, list);
    REQUIRE(list.quads.size() == 3);
    CHECK_FALSE(list.quads[0].borderStroke);

    // ZIndex 1 first: outer, offset three pixels, a tenth of forty thick.
    const ui::DrawQuad& lower = list.quads[1];
    REQUIRE(lower.borderStroke);
    CHECK(static_cast<double>(lower.strokeInner) == doctest::Approx(3.0));
    CHECK(static_cast<double>(lower.strokeOuter) == doctest::Approx(7.0));
    CHECK(lower.strokeJoin == 2u);
    // Its quad covers the band's outside and a pixel of soft edge.
    CHECK(static_cast<double>(lower.min.x) == doctest::Approx(-8.0));
    CHECK(static_cast<double>(lower.max.x) == doctest::Approx(108.0));

    // ZIndex 2 last: inner, four pixels inside the edge.
    const ui::DrawQuad& upper = list.quads[2];
    REQUIRE(upper.borderStroke);
    CHECK(static_cast<double>(upper.strokeInner) == doctest::Approx(-4.0));
    CHECK(static_cast<double>(upper.strokeOuter) == doctest::Approx(0.0));
}

TEST_CASE("a stroke on text outlines the glyphs, outlines first, unless it is asked for the border")
{
    Fixture fixture;
    const InstanceId screen = fixture.child("ScreenGui", fixture.service);
    const InstanceId label = fixture.child("TextLabel", screen);
    fixture.object(label).size = core::UDim2{core::UDim{0.0f, 200.0f}, core::UDim{0.0f, 40.0f}};
    fixture.object(label).backgroundTransparency = 1.0f;
    fixture.world->textLabels().find(label)->text = "ab";
    const InstanceId stroke = fixture.child("UIStroke", label);
    fixture.world->uiStrokes().find(stroke)->color = core::Color3{1.0f, 0.0f, 0.0f};
    fixture.run();

    ui::DrawList list;
    ui::buildDrawList(*fixture.world, fixture.service, list);
    REQUIRE_FALSE(list.quads.empty());
    // Every outline before every glyph, and no border.
    std::size_t outlines = 0;
    bool glyphSeen = false;
    for (const ui::DrawQuad& quad : list.quads) {
        CHECK_FALSE(quad.borderStroke);
        if (quad.outline) {
            CHECK_FALSE(glyphSeen);
            CHECK(static_cast<double>(quad.color.r) == doctest::Approx(1.0));
            ++outlines;
        }
        else {
            glyphSeen = true;
        }
    }
    CHECK(outlines > 0);
    CHECK(glyphSeen);

    // `Border` outlines the label's box instead.
    fixture.world->uiStrokes().find(stroke)->applyStrokeMode = 1;
    list.clear();
    ui::buildDrawList(*fixture.world, fixture.service, list);
    bool bordered = false;
    for (const ui::DrawQuad& quad : list.quads) {
        CHECK_FALSE(quad.outline);
        bordered = bordered || quad.borderStroke;
    }
    CHECK(bordered);
}

TEST_CASE("rich text's stroke tag outlines what it encloses and nothing else")
{
    Fixture fixture;
    const InstanceId screen = fixture.child("ScreenGui", fixture.service);
    const InstanceId label = fixture.child("TextLabel", screen);
    fixture.object(label).size = core::UDim2{core::UDim{0.0f, 300.0f}, core::UDim{0.0f, 40.0f}};
    fixture.object(label).backgroundTransparency = 1.0f;
    scene::TextLabelComponent& text = *fixture.world->textLabels().find(label);
    text.richText = true;
    text.text = "ab <stroke color=\"#00ff00\" th=\"3\" joins=\"miter\">cd</stroke>";
    fixture.run();

    ui::DrawList list;
    ui::buildDrawList(*fixture.world, fixture.service, list);
    std::size_t outlines = 0;
    std::size_t glyphs = 0;
    for (const ui::DrawQuad& quad : list.quads) {
        if (quad.outline) {
            CHECK(static_cast<double>(quad.color.g) == doctest::Approx(1.0));
            ++outlines;
        }
        else {
            ++glyphs;
        }
    }
    // Two letters outlined, four letters drawn -- whatever quads a letter is.
    CHECK(outlines > 0);
    CHECK(glyphs > outlines);

    // A tag with a value it cannot read is text, as every other tag is.
    const std::string unreadable = "<stroke joins=\"wavy\">x</stroke>";
    CHECK(ui::plainTextOf(unreadable) == unreadable);
}

TEST_CASE("text that does not fit its box is shown, cut at the box, or ended with an ellipsis (ADR 0168)")
{
    // A player's name in a table's row: a game cut it by counting characters,
    // which no proportional font agrees with.
    Fixture fixture;
    const InstanceId screen = fixture.child("ScreenGui", fixture.service);
    const InstanceId label = fixture.child("TextLabel", screen);
    fixture.object(label).size = core::UDim2{core::UDim{0.0f, 120.0f}, core::UDim{0.0f, 24.0f}};
    fixture.object(label).backgroundTransparency = 1.0f;
    scene::TextLabelComponent& text = *fixture.world->textLabels().find(label);
    text.text = "Wolfgang Amadeus Mozart the Second";
    text.textSize = 16.0f;
    text.horizontalAlignment = 0;
    fixture.run();
    const core::Rect box{fixture.object(label).absolutePosition,
                         fixture.object(label).absolutePosition + fixture.object(label).absoluteSize};
    const float whole = ui::measureText(text.text, text.font, 16.0f, 0.0f).size.x;
    REQUIRE(whole > 120.0f);

    struct Drawn
    {
        std::size_t quads = 0;
        float right = 0.0f;
        float bottom = 0.0f;
        core::Rect scissor;
    };
    const auto draw = [&](int overflow) {
        text.textOverflow = overflow;
        ui::DrawList list;
        ui::buildDrawList(*fixture.world, fixture.service, list);
        Drawn drawn;
        drawn.quads = list.quads.size();
        for (const ui::DrawQuad& quad : list.quads) {
            drawn.right = std::max(drawn.right, quad.max.x);
            drawn.bottom = std::max(drawn.bottom, quad.max.y);
            drawn.scissor = list.scissors[quad.scissor];
        }
        return drawn;
    };

    // Shown: over the edge, as it always was, and cut by nothing of its own.
    const Drawn shown = draw(0);
    CHECK(shown.right > box.max.x + 20.0f);
    CHECK(shown.scissor.max.x > box.max.x);

    // Cut at the box: every glyph still there, and none of them past it.
    const Drawn clipped = draw(1);
    CHECK(clipped.quads == shown.quads);
    CHECK(clipped.scissor.min.x == box.min.x);
    CHECK(clipped.scissor.max.x == box.max.x);
    CHECK(clipped.scissor.max.y == box.max.y);

    // Ended with an ellipsis: fewer glyphs, the last of them inside the box.
    const Drawn ended = draw(2);
    CHECK(ended.quads < shown.quads);
    CHECK(ended.quads > 3);
    CHECK(ended.right <= box.max.x + 0.5f);
    const std::string cut = ui::ellipsizedText(text.text, text.font, 16.0f, 0.0f, Vec2{120.0f, 24.0f});
    CHECK(cut.size() < text.text.size());
    CHECK(cut.starts_with("Wolfgang"));
    CHECK((cut.ends_with("...") || cut.ends_with("\xE2\x80\xA6")));
    CHECK(ui::measureText(cut, text.font, 16.0f, 0.0f).size.x <= 120.0f);
    // By what the font measures, not by a count: narrow letters keep more.
    const std::string narrow =
        ui::ellipsizedText("illlllllllllllllllllllllllllllllllllllllll", text.font, 16.0f, 0.0f, Vec2{120.0f, 24.0f});
    const std::string wide =
        ui::ellipsizedText("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", text.font, 16.0f, 0.0f, Vec2{120.0f, 24.0f});
    CHECK(narrow.size() >= wide.size());

    // Text that fits is the text.
    CHECK(ui::ellipsizedText("Ada", text.font, 16.0f, 0.0f, Vec2{120.0f, 24.0f}) == "Ada");

    // **Wrapped**: the lines the box is tall enough for, the last of them
    // ended -- and a box three lines tall keeps three.
    text.textWrapped = true;
    text.text = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen";
    const float line = ui::textLineHeight(text.font, 16.0f);
    fixture.object(label).size = core::UDim2{core::UDim{0.0f, 120.0f}, core::UDim{0.0f, line * 2.0f + 2.0f}};
    fixture.dirty(screen);
    fixture.run();
    const Drawn wrappedShown = draw(0);
    const Drawn wrappedEnded = draw(2);
    const float top = fixture.object(label).absolutePosition.y;
    CHECK(wrappedShown.bottom > top + line * 3.0f);
    CHECK(wrappedEnded.quads < wrappedShown.quads);
    const std::string two = ui::ellipsizedText(text.text, text.font, 16.0f, 120.0f, Vec2{120.0f, line * 2.0f + 2.0f});
    CHECK(ui::measureText(two, text.font, 16.0f, 120.0f).lineCount == 2u);
    CHECK(two.starts_with("one two"));
    const std::string three = ui::ellipsizedText(text.text, text.font, 16.0f, 120.0f, Vec2{120.0f, line * 3.0f + 2.0f});
    CHECK(ui::measureText(three, text.font, 16.0f, 120.0f).lineCount == 3u);
    CHECK(three.size() > two.size());

    // Scaled text fits by what it is: nothing to cut.
    text.textWrapped = false;
    text.textScaled = true;
    text.text = "Wolfgang Amadeus Mozart the Second";
    fixture.dirty(screen);
    fixture.run();
    CHECK(draw(2).quads == draw(0).quads);
}

TEST_CASE("D453: a picture has a see-through of its own, apart from its box's")
{
    // An icon could not be faded: a label's words had `TextTransparency`, and
    // a picture had only the background's.
    Fixture fixture;
    const InstanceId screen = fixture.child("ScreenGui", fixture.service);
    const InstanceId icon = fixture.child("ImageLabel", screen);
    fixture.object(icon).size = core::UDim2{core::UDim{0.0f, 64.0f}, core::UDim{0.0f, 64.0f}};
    fixture.object(icon).backgroundTransparency = 1.0f;
    scene::ImageLabelComponent* image = fixture.world->imageLabels().find(icon);
    REQUIRE(image != nullptr);
    // No provider resolves it, so it draws as its flat tint: one quad.
    image->image = "asset://icons/star.png";
    fixture.run();

    const auto drawn = [&] {
        ui::DrawList list;
        ui::buildDrawList(*fixture.world, fixture.service, list);
        return list;
    };
    REQUIRE(drawn().quads.size() == 1);
    CHECK(drawn().quads[0].alpha == doctest::Approx(1.0));

    image->imageTransparency = 0.75f;
    REQUIRE(drawn().quads.size() == 1);
    CHECK(static_cast<double>(drawn().quads[0].alpha) == doctest::Approx(0.25));

    // Gone at one, and past it: not a quad nobody can see.
    image->imageTransparency = 1.0f;
    CHECK(drawn().quads.empty());
    image->imageTransparency = 4.0f;
    CHECK(drawn().quads.empty());

    // The property, as a script writes it.
    CHECK(fixture.world->setProperty(icon, fixture.atoms.intern("ImageTransparency"), scene::Value{0.5}) ==
          scene::World::SetResult::Changed);
    CHECK(static_cast<double>(drawn().quads[0].alpha) == doctest::Approx(0.5));
}
