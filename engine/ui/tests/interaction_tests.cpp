#include <algorithm>
#include <doctest/doctest.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/scene/world.h"
#include "engine/ui/scene_types.h"
#include "engine/ui/text_edit.h"
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
    InstanceId screen;

    Fixture()
    {
        scene::generated::registerClasses(classes, atoms);
        ui::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
        // Interaction's memory is the process's: a focus left by the case
        // before names an id this new world may reuse.
        ui::resetInteraction();
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

    // A box at an exact place, so a hit test is arithmetic a reader can check.
    InstanceId box(InstanceId parent, float x, float y, float w, float h)
    {
        const InstanceId id = child("TextButton", parent);
        scene::UIObjectComponent* object = world->uiObjects().find(id);
        object->position = core::UDim2{core::UDim{0.0f, x}, core::UDim{0.0f, y}};
        object->size = core::UDim2{core::UDim{0.0f, w}, core::UDim{0.0f, h}};
        return id;
    }

    void run() { ui::layout(*world, service, Vec2{800.0f, 600.0f}); }

    ui::InteractionResult interact(Vec2 pointer, bool pressed = false, bool released = false,
                                   std::string_view text = {}, bool backspace = false, bool submit = false)
    {
        ui::InteractionInput input;
        input.pointer = pointer;
        input.pressed = pressed;
        input.released = released;
        input.text = text;
        input.backspace = backspace;
        input.submit = submit;
        return ui::updateInteraction(*world, service, input);
    }

    // The caret's own keys (S6.7), as a whole `InteractionInput` rather than six
    // more defaulted parameters on the one above -- which would be a signature
    // nobody can read a call to.
    ui::InteractionResult send(const ui::InteractionInput& input)
    {
        return ui::updateInteraction(*world, service, input);
    }

    [[nodiscard]] std::vector<std::string> events()
    {
        std::vector<std::string> names;
        for (const scene::Change& change : world->changes().take()) {
            if (change.kind == scene::ChangeKind::InstanceEventNoArgs)
                names.emplace_back(atoms.text(change.name));
            // `FocusLost` carries how the field was left (D215), and why
            // (ADR 0139).
            if (change.kind == scene::ChangeKind::InstanceEventBool || change.kind == scene::ChangeKind::FocusLost)
                names.emplace_back(std::string(atoms.text(change.name)) +
                                   (change.other.index != 0 ? "(true)" : "(false)"));
            if (change.kind == scene::ChangeKind::FocusLost) {
                constexpr const char* Reasons[] = {"Submitted", "Moved", "Cancelled", "Script"};
                names.emplace_back(std::string("FocusLost:") + Reasons[std::min(change.other.generation, 3u)]);
            }
            if (change.kind == scene::ChangeKind::InstanceEventText)
                names.emplace_back(std::string(atoms.text(change.name)) + ":" +
                                   std::string(world->changes().drainedText(change)));
            if (change.kind == scene::ChangeKind::InstanceEvent)
                names.emplace_back(atoms.text(change.name));
        }
        return names;
    }
};

} // namespace

TEST_CASE("the pointer finds the element drawn on top")
{
    Fixture fixture;
    const InstanceId back = fixture.box(fixture.screen, 0.0f, 0.0f, 200.0f, 200.0f);
    const InstanceId front = fixture.box(fixture.screen, 50.0f, 50.0f, 50.0f, 50.0f);
    fixture.run();

    CHECK(ui::hitTest(*fixture.world, fixture.service, Vec2{10.0f, 10.0f}) == back);
    // The overlapping region belongs to whichever draws last, which is document
    // order at equal ZIndex -- the same rule the draw list uses, from the same
    // numbers.
    CHECK(ui::hitTest(*fixture.world, fixture.service, Vec2{60.0f, 60.0f}) == front);
    CHECK_FALSE(ui::hitTest(*fixture.world, fixture.service, Vec2{500.0f, 500.0f}).valid());
}

TEST_CASE("an invisible element takes no clicks")
{
    Fixture fixture;
    const InstanceId hidden = fixture.box(fixture.screen, 0.0f, 0.0f, 100.0f, 100.0f);
    fixture.world->uiObjects().find(hidden)->visible = false;
    fixture.run();

    // All three together, which `Visible`'s doc promises: an invisible element
    // that still swallowed clicks is the defect that sentence rules out.
    CHECK_FALSE(ui::hitTest(*fixture.world, fixture.service, Vec2{10.0f, 10.0f}).valid());
}

TEST_CASE("a clipped-away element takes no clicks either")
{
    Fixture fixture;
    const InstanceId strip = fixture.box(fixture.screen, 0.0f, 0.0f, 100.0f, 40.0f);
    fixture.world->uiObjects().find(strip)->clipsDescendants = true;
    const InstanceId overhang = fixture.box(strip, 50.0f, 0.0f, 200.0f, 40.0f);
    fixture.run();

    // Inside both: the child answers.
    CHECK(ui::hitTest(*fixture.world, fixture.service, Vec2{60.0f, 10.0f}) == overhang);
    // Past the parent's edge: nothing does. An element scrolled off the end of a
    // list must not answer a click that lands where it would have been.
    CHECK_FALSE(ui::hitTest(*fixture.world, fixture.service, Vec2{150.0f, 10.0f}).valid());
}

TEST_CASE("hover is a pair of edges rather than a state")
{
    Fixture fixture;
    const InstanceId button = fixture.box(fixture.screen, 0.0f, 0.0f, 100.0f, 100.0f);
    fixture.run();

    fixture.interact(Vec2{10.0f, 10.0f});
    CHECK(fixture.events() == std::vector<std::string>{"PointerEntered"});

    // Still inside: told once, not once a frame.
    fixture.interact(Vec2{20.0f, 20.0f});
    CHECK(fixture.events().empty());

    fixture.interact(Vec2{500.0f, 500.0f});
    CHECK(fixture.events() == std::vector<std::string>{"PointerExited"});
    (void)button;
}

TEST_CASE("Activated needs both ends of the press on one element")
{
    Fixture fixture;
    const InstanceId button = fixture.box(fixture.screen, 0.0f, 0.0f, 100.0f, 100.0f);
    fixture.run();

    fixture.interact(Vec2{10.0f, 10.0f}, true, false);
    (void)fixture.events();
    fixture.interact(Vec2{20.0f, 20.0f}, false, true);
    CHECK(fixture.events() == std::vector<std::string>{"Activated"});

    // Pressed on the button and released off it: cancelled, which is what
    // every UI does and what people rely on to change their minds.
    fixture.interact(Vec2{10.0f, 10.0f}, true, false);
    (void)fixture.events();
    fixture.interact(Vec2{500.0f, 500.0f}, false, true);
    const std::vector<std::string> after = fixture.events();
    CHECK(std::ranges::find(after, "Activated") == after.end());
    (void)button;
}

TEST_CASE("a click inside one frame, and a double click whose middle is one frame, activate every time (D362)")
{
    Fixture fixture;
    (void)fixture.box(fixture.screen, 0.0f, 0.0f, 100.0f, 100.0f);
    fixture.run();
    const auto activations = [&fixture] {
        const std::vector<std::string> fired = fixture.events();
        return std::ranges::count(fired, std::string("Activated"));
    };

    // A tap: down and up between two frames.
    fixture.interact(Vec2{10.0f, 10.0f}, true, true);
    CHECK(activations() == 1);

    // A quick double click: the first press, then the first release and the
    // second press in one frame, then the second release. Two activations --
    // the release is the first of that frame's two, not the second.
    fixture.interact(Vec2{10.0f, 10.0f}, true, false);
    ui::InteractionInput middle;
    middle.pointer = Vec2{10.0f, 10.0f};
    middle.pressed = true;
    middle.released = true;
    middle.releasedFirst = true;
    middle.clicks = 2;
    (void)fixture.send(middle);
    fixture.interact(Vec2{10.0f, 10.0f}, false, true);
    CHECK(activations() == 2);
}

TEST_CASE("a button is its whole box: an image button with or without an image, a text button with no text")
{
    // **The owner**: a friend's buttons did not answer. The box is what takes
    // the click -- not the picture, not the text, not whether the background
    // is drawn -- so each of these is pressed anywhere inside it.
    Fixture fixture;
    const auto place = [&](InstanceId id, float x, float y) {
        scene::UIObjectComponent* object = fixture.world->uiObjects().find(id);
        object->position = core::UDim2{core::UDim{0.0f, x}, core::UDim{0.0f, y}};
        object->size = core::UDim2{core::UDim{0.0f, 100.0f}, core::UDim{0.0f, 60.0f}};
    };
    const auto press = [&](Vec2 at) {
        fixture.interact(at, true, false);
        (void)fixture.events();
        fixture.interact(at, false, true);
        const std::vector<std::string> after = fixture.events();
        return std::ranges::find(after, "Activated") != after.end();
    };

    // No image: the background's box.
    const InstanceId plain = fixture.child("ImageButton", fixture.screen);
    place(plain, 0.0f, 0.0f);
    // An image and no background drawn at all: still the box.
    const InstanceId pictured = fixture.child("ImageButton", fixture.screen);
    place(pictured, 200.0f, 0.0f);
    (void)fixture.world->setProperty(pictured, fixture.atoms.intern("Image"),
                                     scene::Value{std::string("asset://icon.png")});
    (void)fixture.world->setProperty(pictured, fixture.atoms.intern("BackgroundTransparency"), scene::Value{1.0});
    // The friend's: an empty text button inside a rounded frame, beside a label.
    const InstanceId holder = fixture.child("Frame", fixture.screen);
    place(holder, 0.0f, 200.0f);
    fixture.world->uiObjects().find(holder)->size = core::UDim2{core::UDim{0.0f, 400.0f}, core::UDim{0.0f, 300.0f}};
    (void)fixture.child("UICorner", holder);
    const InstanceId label = fixture.child("TextLabel", holder);
    place(label, 0.0f, 0.0f);
    const InstanceId restart = fixture.box(holder, 50.0f, 150.0f, 200.0f, 80.0f);
    (void)fixture.world->setProperty(restart, fixture.atoms.intern("Text"), scene::Value{std::string()});
    fixture.run();

    CHECK(press(Vec2{50.0f, 30.0f}));
    CHECK(press(Vec2{250.0f, 30.0f}));
    CHECK(press(Vec2{100.0f, 390.0f}));
    // And a click on the label beside it is the label's, not the button's.
    CHECK(ui::hitTest(*fixture.world, fixture.service, Vec2{50.0f, 230.0f}) == label);
    CHECK(ui::hitTest(*fixture.world, fixture.service, Vec2{100.0f, 390.0f}) == restart);
}

TEST_CASE("the UI reports whether it took the pointer")
{
    Fixture fixture;
    fixture.box(fixture.screen, 0.0f, 0.0f, 100.0f, 100.0f);
    fixture.run();

    // The flag `input` consumes the mouse codes on, so a button over the world
    // does not also shoot the gun.
    CHECK(fixture.interact(Vec2{10.0f, 10.0f}).pointerOverUi);
    CHECK_FALSE(fixture.interact(Vec2{500.0f, 500.0f}).pointerOverUi);
}

TEST_CASE("focus follows the press, and typing reaches the focused field")
{
    Fixture fixture;
    const InstanceId field = fixture.child("TextInput", fixture.screen);
    scene::UIObjectComponent* object = fixture.world->uiObjects().find(field);
    object->size = core::UDim2{core::UDim{0.0f, 200.0f}, core::UDim{0.0f, 30.0f}};
    fixture.run();

    fixture.interact(Vec2{10.0f, 10.0f}, true, false);
    CHECK(fixture.world->textInputs().find(field)->focused);

    fixture.interact(Vec2{10.0f, 10.0f}, false, false, "ab");
    CHECK(fixture.world->textLabels().find(field)->text == "ab");

    // Backspace removes a whole UTF-8 sequence, not a byte: dropping one byte of
    // a two-byte character leaves a string no renderer can read.
    //
    // **Through `setProperty` rather than into the component**, which is what
    // a script's own assignment does -- and it is what puts the caret at the
    // end of the new string (S6.7). Writing the component directly leaves the
    // caret pointing into text that no longer exists, which is a state nothing
    // outside a test can produce.
    REQUIRE(fixture.world->setProperty(field, fixture.world->atoms().intern("Text"),
                                       scene::Value{std::string("a\xC3\xA9")}) == scene::World::SetResult::Changed);
    fixture.interact(Vec2{10.0f, 10.0f}, false, false, {}, true);
    CHECK(fixture.world->textLabels().find(field)->text == "a");

    // A press elsewhere takes focus away, including a press on nothing. A field
    // that kept focus after the player clicked the world would go on eating
    // their keystrokes.
    fixture.interact(Vec2{600.0f, 400.0f}, true, false);
    CHECK_FALSE(fixture.world->textInputs().find(field)->focused);
    fixture.interact(Vec2{600.0f, 400.0f}, false, false, "zz");
    CHECK(fixture.world->textLabels().find(field)->text == "a");
}

TEST_CASE("Return submits and releases focus")
{
    Fixture fixture;
    const InstanceId field = fixture.child("TextInput", fixture.screen);
    fixture.world->uiObjects().find(field)->size = core::UDim2{core::UDim{0.0f, 200.0f}, core::UDim{0.0f, 30.0f}};
    fixture.run();

    fixture.interact(Vec2{10.0f, 10.0f}, true, false);
    (void)fixture.events();

    fixture.interact(Vec2{10.0f, 10.0f}, false, false, {}, false, true);
    CHECK_FALSE(fixture.world->textInputs().find(field)->focused);
    const std::vector<std::string> after = fixture.events();
    CHECK(std::ranges::find(after, "FocusLost(true)") != after.end());

    // And a press elsewhere is not a submit.
    fixture.interact(Vec2{10.0f, 10.0f}, true, false);
    (void)fixture.events();
    fixture.interact(Vec2{600.0f, 400.0f}, true, false);
    const std::vector<std::string> away = fixture.events();
    CHECK(std::ranges::find(away, "FocusLost(false)") != away.end());
}

// --- The caret (S6.7) ---------------------------------------------------------
//
// **`TextInput`'s own doc has promised "typed text, backspace and a caret"
// since the class existed**, and the caret was the one it did not have: the
// editor appended and backspaced at the END, so a typo four characters back
// meant deleting everything after it.
//
// The field was here once with nothing moving it, and it was removed for that
// reason -- `inertcheck` found it, and the comment that replaced it said a real
// caret would arrive with the code that moves it. These are that code's cases.

namespace {

// A focused field with `seed` in it and the caret wherever focus left it.
[[nodiscard]] InstanceId focusedField(Fixture& fixture, std::string_view seed)
{
    const InstanceId field = fixture.child("TextInput", fixture.screen);
    fixture.world->uiObjects().find(field)->size = core::UDim2{core::UDim{0.0f, 200.0f}, core::UDim{0.0f, 30.0f}};
    fixture.run();
    fixture.interact(Vec2{10.0f, 10.0f}, true, false);
    if (!seed.empty())
        fixture.interact(Vec2{10.0f, 10.0f}, false, false, seed);
    return field;
}

} // namespace

TEST_CASE("a press after the text puts the caret at its end")
{
    // Which is what clicking into a field means everywhere: a press past the
    // text puts the caret after it and typing continues it -- and, since ADR
    // 0139, a press ON the text puts it under the pointer.
    Fixture fixture;
    const InstanceId field = fixture.child("TextInput", fixture.screen);
    fixture.world->uiObjects().find(field)->size = core::UDim2{core::UDim{0.0f, 200.0f}, core::UDim{0.0f, 30.0f}};
    REQUIRE(fixture.world->setProperty(field, fixture.world->atoms().intern("Text"),
                                       scene::Value{std::string("hello")}) == scene::World::SetResult::Changed);
    fixture.run();

    fixture.interact(Vec2{199.0f, 10.0f}, true, false);
    CHECK(fixture.world->textInputs().find(field)->caret == 5);
}

TEST_CASE("typing inserts where the caret is, not at the end")
{
    // The whole feature. Without it a typo four characters back means deleting
    // everything after it.
    Fixture fixture;
    const InstanceId field = focusedField(fixture, "hello");

    ui::InteractionInput left;
    left.pointer = Vec2{10.0f, 10.0f};
    left.caretLeft = true;
    fixture.send(left);
    fixture.send(left);
    CHECK(fixture.world->textInputs().find(field)->caret == 3);

    fixture.interact(Vec2{10.0f, 10.0f}, false, false, "XY");
    CHECK(fixture.world->textLabels().find(field)->text == "helXYlo");
    CHECK(fixture.world->textInputs().find(field)->caret == 5);
}

TEST_CASE("backspace takes what is before the caret and delete takes what is after")
{
    // Two keys and two edits. With only one of them a caret can be used to
    // insert and nothing else.
    Fixture fixture;
    const InstanceId field = focusedField(fixture, "abcd");

    ui::InteractionInput left;
    left.pointer = Vec2{10.0f, 10.0f};
    left.caretLeft = true;
    fixture.send(left);
    fixture.send(left);
    REQUIRE(fixture.world->textInputs().find(field)->caret == 2);

    ui::InteractionInput back;
    back.pointer = Vec2{10.0f, 10.0f};
    back.backspace = true;
    fixture.send(back);
    CHECK(fixture.world->textLabels().find(field)->text == "acd");
    CHECK(fixture.world->textInputs().find(field)->caret == 1);

    ui::InteractionInput forward;
    forward.pointer = Vec2{10.0f, 10.0f};
    forward.forwardDelete = true;
    fixture.send(forward);
    CHECK(fixture.world->textLabels().find(field)->text == "ad");
    // Forward delete does NOT move the caret, which is what makes holding it
    // eat the rest of the line rather than walking backwards through it.
    CHECK(fixture.world->textInputs().find(field)->caret == 1);
}

TEST_CASE("Home and End go to the two ends")
{
    Fixture fixture;
    const InstanceId field = focusedField(fixture, "abcd");

    ui::InteractionInput home;
    home.pointer = Vec2{10.0f, 10.0f};
    home.caretHome = true;
    fixture.send(home);
    CHECK(fixture.world->textInputs().find(field)->caret == 0);

    ui::InteractionInput end;
    end.pointer = Vec2{10.0f, 10.0f};
    end.caretEnd = true;
    fixture.send(end);
    CHECK(fixture.world->textInputs().find(field)->caret == 4);
}

TEST_CASE("the caret steps whole characters, so it never lands inside one")
{
    // Bytes everywhere except where a step has to be a character. A caret inside
    // a UTF-8 sequence is an insertion that splits it, and a string no renderer
    // can read.
    Fixture fixture;
    const InstanceId field = focusedField(fixture, "a\xC3\xA9z");
    REQUIRE(fixture.world->textInputs().find(field)->caret == 4);

    ui::InteractionInput left;
    left.pointer = Vec2{10.0f, 10.0f};
    left.caretLeft = true;
    fixture.send(left); // past 'z'
    CHECK(fixture.world->textInputs().find(field)->caret == 3);
    fixture.send(left); // past the two-byte sequence, in one step
    CHECK(fixture.world->textInputs().find(field)->caret == 1);
}

TEST_CASE("the caret stops at both ends rather than running off them")
{
    Fixture fixture;
    const InstanceId field = focusedField(fixture, "ab");

    ui::InteractionInput left;
    left.pointer = Vec2{10.0f, 10.0f};
    left.caretLeft = true;
    for (int press = 0; press < 6; ++press)
        fixture.send(left);
    CHECK(fixture.world->textInputs().find(field)->caret == 0);

    // And a backspace at the start does nothing rather than eating a byte that
    // is not there.
    ui::InteractionInput back;
    back.pointer = Vec2{10.0f, 10.0f};
    back.backspace = true;
    fixture.send(back);
    CHECK(fixture.world->textLabels().find(field)->text == "ab");

    ui::InteractionInput right;
    right.pointer = Vec2{10.0f, 10.0f};
    right.caretRight = true;
    for (int press = 0; press < 6; ++press)
        fixture.send(right);
    CHECK(fixture.world->textInputs().find(field)->caret == 2);

    ui::InteractionInput forward;
    forward.pointer = Vec2{10.0f, 10.0f};
    forward.forwardDelete = true;
    fixture.send(forward);
    CHECK(fixture.world->textLabels().find(field)->text == "ab");
}

TEST_CASE("a script assigning Text puts the caret at the end")
{
    // The only answer that is always in range. A caret left where it was points
    // into a string that no longer exists: at best somewhere arbitrary, at worst
    // inside a UTF-8 sequence.
    Fixture fixture;
    const InstanceId field = focusedField(fixture, "abcd");

    ui::InteractionInput home;
    home.pointer = Vec2{10.0f, 10.0f};
    home.caretHome = true;
    fixture.send(home);
    REQUIRE(fixture.world->textInputs().find(field)->caret == 0);

    REQUIRE(fixture.world->setProperty(field, fixture.world->atoms().intern("Text"),
                                       scene::Value{std::string("a much longer value")}) ==
            scene::World::SetResult::Changed);
    CHECK(fixture.world->textInputs().find(field)->caret == 19);
}

TEST_CASE("a button in the world is pressed like one on the screen, and the screen covers it")
{
    Fixture fixture;
    // A world canvas's element, as the host would have found it with the
    // pointer's ray -- the screen has nothing under the pointer here.
    const InstanceId sign = fixture.child("TextButton", fixture.make("Folder"));
    fixture.run();

    ui::InteractionInput press;
    press.pointer = Vec2{400.0f, 300.0f};
    press.pressed = true;
    press.worldOver = sign;
    CHECK(fixture.send(press).pointerOverUi);
    (void)fixture.events();
    ui::InteractionInput release = press;
    release.pressed = false;
    release.released = true;
    (void)fixture.send(release);
    const std::vector<std::string> fired = fixture.events();
    CHECK(std::ranges::find(fired, "Activated") != fired.end());

    // A screen button over the same pixel takes the press instead.
    const InstanceId hud = fixture.box(fixture.screen, 300.0f, 200.0f, 200.0f, 200.0f);
    fixture.world->screenGuis().find(fixture.screen)->layoutDirty = true;
    fixture.run();
    CHECK(ui::hitTest(*fixture.world, fixture.service, press.pointer) == hud);
    (void)fixture.send(press);
    (void)fixture.events();
    (void)fixture.send(release);
    CHECK(fixture.events() == std::vector<std::string>{"Activated"});
}

// --- Everything a text field does (ADR 0139) -----------------------------------

namespace {

using Kind = ui::TextCommand::Kind;

[[nodiscard]] ui::TextCommand command(Kind kind, ui::TextMove move = ui::TextMove::Left, bool extend = false)
{
    ui::TextCommand out;
    out.kind = kind;
    out.move = static_cast<core::u8>(move);
    out.extend = extend;
    return out;
}

[[nodiscard]] ui::TextCommand typing(std::string_view text)
{
    ui::TextCommand out;
    out.kind = Kind::Insert;
    out.text = text;
    return out;
}

// A field 400 pixels wide at the top left, focused.
struct Field
{
    Fixture fixture;
    InstanceId field;

    Field()
    {
        field = fixture.child("TextInput", fixture.screen);
        fixture.world->uiObjects().find(field)->size = core::UDim2{core::UDim{0.0f, 400.0f}, core::UDim{0.0f, 30.0f}};
        // From the left edge, so a press's x is a width a reader can check.
        fixture.world->textLabels().find(field)->horizontalAlignment = 0;
        fixture.run();
        fixture.interact(Vec2{390.0f, 10.0f}, true, false);
        fixture.interact(Vec2{390.0f, 10.0f}, false, true);
        (void)fixture.events();
    }

    std::vector<std::string> keys(std::vector<ui::TextCommand> commands, ui::InteractionInput input = {})
    {
        input.pointer = Vec2{390.0f, 10.0f};
        input.commands = commands;
        (void)fixture.send(input);
        return fixture.events();
    }

    [[nodiscard]] std::string text() { return fixture.world->textLabels().find(field)->text; }
    [[nodiscard]] scene::TextInputComponent& input() { return *fixture.world->textInputs().find(field); }
};

[[nodiscard]] bool has(const std::vector<std::string>& events, std::string_view name)
{
    return std::ranges::find(events, name) != events.end();
}

} // namespace

TEST_CASE("the keys work in the order they were pressed, and the clipboard is the host's")
{
    Field f;
    std::string clipboard;
    ui::InteractionInput input;
    input.writeClipboard = [&clipboard](std::string_view text) { clipboard.assign(text); };
    input.readClipboard = [&clipboard]() -> std::optional<std::string> { return clipboard; };

    (void)f.keys({typing("hello world")}, input);
    // Ctrl+A then a letter: the letter replaces everything.
    (void)f.keys({command(Kind::SelectAll), command(Kind::Copy), typing("x")}, input);
    CHECK(clipboard == "hello world");
    CHECK(f.text() == "x");
    // Ctrl+V, twice.
    (void)f.keys({command(Kind::Paste), command(Kind::Paste)}, input);
    CHECK(f.text() == "xhello worldhello world");
    // Shift+Ctrl+Left selects the last word; Ctrl+X takes it.
    (void)f.keys({command(Kind::Move, ui::TextMove::WordLeft, true), command(Kind::Cut)}, input);
    CHECK(clipboard == "world");
    CHECK(f.text() == "xhello worldhello ");
    // Ctrl+Z takes back the cut, then the pastes.
    (void)f.keys({command(Kind::Undo)}, input);
    CHECK(f.text() == "xhello worldhello world");
}

TEST_CASE("the player's edits fire TextChanged, and MaxLength says what it kept out")
{
    Field f;
    f.input().maxLength = 5;
    const std::vector<std::string> events = f.keys({typing("abcdefg")});
    CHECK(f.text() == "abcde");
    CHECK(has(events, "TextChanged:abcde"));
    CHECK(has(events, "InputRejected:fg"));

    // A script's write is not the player's.
    REQUIRE(f.fixture.world->setProperty(f.field, f.fixture.world->atoms().intern("Text"),
                                         scene::Value{std::string("set")}) == scene::World::SetResult::Changed);
    const std::vector<std::string> after = f.keys({});
    CHECK_FALSE(has(after, "TextChanged:set"));
}

TEST_CASE("Return submits, and a chat box keeps focus for the next line")
{
    Field f;
    f.input().releaseFocusOnSubmit = false;
    std::vector<std::string> events = f.keys({typing("hi"), command(Kind::Return)});
    CHECK(has(events, "Submitted:hi"));
    CHECK(f.input().focused);

    f.input().releaseFocusOnSubmit = true;
    events = f.keys({command(Kind::Return)});
    CHECK(has(events, "Submitted:hi"));
    CHECK(has(events, "FocusLost:Submitted"));
    CHECK(has(events, "TextInputFocusReleased"));
    CHECK_FALSE(f.input().focused);
}

TEST_CASE("Escape lets go, and puts back the text it had when asked to")
{
    Field f;
    f.input().revertOnEscape = true;
    (void)f.keys({typing("draft"), command(Kind::Return)});
    // Focused again on "draft", edited, then cancelled.
    f.fixture.interact(Vec2{390.0f, 10.0f}, true, false);
    f.fixture.interact(Vec2{390.0f, 10.0f}, false, true);
    (void)f.fixture.events();
    const std::vector<std::string> events = f.keys({typing(" more"), command(Kind::Escape)});
    CHECK(has(events, "FocusLost:Cancelled"));
    CHECK(f.text() == "draft");
}

TEST_CASE("Tab goes to the next field on the screen, and Shift+Tab back")
{
    Field f;
    const InstanceId second = f.fixture.child("TextInput", f.fixture.screen);
    f.fixture.world->uiObjects().find(second)->position = core::UDim2{core::UDim{0.0f, 0.0f}, core::UDim{0.0f, 100.0f}};
    f.fixture.run();
    std::vector<std::string> events = f.keys({command(Kind::Tab)});
    CHECK(has(events, "FocusLost:Moved"));
    CHECK(f.fixture.world->textInputs().find(second)->focused);
    ui::TextCommand back = command(Kind::Tab);
    back.extend = true;
    (void)f.fixture.send([&] {
        ui::InteractionInput input;
        input.commands = std::span<const ui::TextCommand>(&back, 1);
        return input;
    }());
    CHECK(f.input().focused);
}

TEST_CASE("a press puts the caret under the pointer, and a double press takes the word")
{
    Field f;
    (void)f.keys({typing("hello world")});
    const float before = ui::textWidth("hello wo", "", f.fixture.world->textLabels().find(f.field)->textSize);
    const float scale = ui::textWidth("hello world", "", f.fixture.world->textLabels().find(f.field)->textSize);
    REQUIRE(scale > 0.0f);

    ui::InteractionInput press;
    press.pointer = Vec2{before + 1.0f, 10.0f};
    press.pressed = true;
    (void)f.fixture.send(press);
    CHECK(f.input().caret == 8);

    press.clicks = 2;
    (void)f.fixture.send(press);
    CHECK(f.input().anchor == 6);
    CHECK(f.input().caret == 11);
}

TEST_CASE("a script's CaptureFocus and ReleaseFocus are taken at the next frame")
{
    Fixture fixture;
    const InstanceId field = fixture.child("TextInput", fixture.screen);
    fixture.run();
    fixture.world->textInputs().find(field)->focusRequest = 1;
    fixture.interact(Vec2{600.0f, 400.0f});
    CHECK(fixture.world->textInputs().find(field)->focused);
    std::vector<std::string> events = fixture.events();
    CHECK(has(events, "Focused"));
    CHECK(has(events, "TextInputFocused"));

    fixture.world->textInputs().find(field)->focusRequest = -1;
    fixture.world->textInputs().find(field)->releaseSubmitted = true;
    fixture.interact(Vec2{600.0f, 400.0f});
    CHECK_FALSE(fixture.world->textInputs().find(field)->focused);
    events = fixture.events();
    CHECK(has(events, "FocusLost(true)"));
    CHECK(has(events, "FocusLost:Script"));
}
