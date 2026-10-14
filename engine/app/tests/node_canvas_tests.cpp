// The canvas of nodes (ADR 0211), with no window: what may be linked and why
// not, what a pointer does on it, and its own undo.
#include <cmath>
#include <doctest/doctest.h>
#include <optional>

#include "engine/app/node_canvas.h"

namespace {

using engine::app::CanvasEvent;
using engine::app::CanvasLink;
using engine::app::CanvasMetrics;
using engine::app::CanvasNode;
using engine::app::CanvasPointer;
using engine::app::LinkRefusal;
using engine::app::NodeCanvas;
using engine::app::PinType;
using engine::core::f32;
using engine::core::u32;

[[nodiscard]] bool nearly(f32 a, f32 b)
{
    return std::fabs(a - b) <= 1.0e-3f;
}

// A node of a value graph: `inputs` numbers in, one thing out.
[[nodiscard]] CanvasNode value(const char* title, f32 x, f32 y, u32 inputs, PinType out = PinType::Number)
{
    CanvasNode node;
    node.kind = "Value";
    node.title = title;
    node.x = x;
    node.y = y;
    for (u32 pin = 0; pin < inputs; ++pin)
        node.inputs.push_back({"In", PinType::Number});
    node.outputs.push_back({"Out", out});
    return node;
}

// A press at a place, the frames between, and a release at another: a drag
// as a person makes one, in the panel's pixels.
CanvasEvent drag(NodeCanvas& canvas, const CanvasMetrics& metrics, f32 fromX, f32 fromY, f32 toX, f32 toY,
                 bool adding = false)
{
    CanvasPointer at;
    at.x = fromX;
    at.y = fromY;
    at.adding = adding;
    at.down = true;
    at.pressed = true;
    CanvasEvent first = canvas.pointer(at, metrics);
    at.pressed = false;
    for (int step = 1; step <= 4; ++step) {
        at.x = fromX + (toX - fromX) * static_cast<f32>(step) / 4.0f;
        at.y = fromY + (toY - fromY) * static_cast<f32>(step) / 4.0f;
        (void)canvas.pointer(at, metrics);
    }
    at.down = false;
    at.released = true;
    const CanvasEvent last = canvas.pointer(at, metrics);
    return last.kind != CanvasEvent::Kind::None ? last : first;
}

} // namespace

TEST_CASE("ADR 0211: a number goes into any width, and a width only into its own")
{
    using engine::app::pinAccepts;
    CHECK(pinAccepts(PinType::Vector3, PinType::Number));
    CHECK(pinAccepts(PinType::Vector3, PinType::Vector3));
    CHECK_FALSE(pinAccepts(PinType::Vector3, PinType::Vector2));
    CHECK_FALSE(pinAccepts(PinType::Number, PinType::Vector3));
    // A state's arrow carries nothing and goes only where arrows go.
    CHECK(pinAccepts(PinType::Flow, PinType::Flow));
    CHECK_FALSE(pinAccepts(PinType::Flow, PinType::Number));
    CHECK_FALSE(pinAccepts(PinType::Number, PinType::Flow));
}

TEST_CASE("ADR 0211: a value's link is refused where it cannot be, and says why")
{
    NodeCanvas canvas;
    const u32 a = canvas.add(value("A", 0, 0, 1));
    const u32 b = canvas.add(value("B", 300, 0, 2));
    const u32 c = canvas.add(value("C", 600, 0, 1));
    const u32 wide = canvas.add(value("Wide", 0, 200, 0, PinType::Vector3));

    CHECK(canvas.link({a, 0, b, 0}) == LinkRefusal::None);
    CHECK(canvas.link({a, 0, b, 0}) == LinkRefusal::AlreadyMade);
    CHECK(canvas.link({a, 0, b, 7}) == LinkRefusal::NoSuchPin);
    CHECK(canvas.link({99, 0, b, 0}) == LinkRefusal::NoSuchPin);
    CHECK(canvas.link({a, 0, a, 0}) == LinkRefusal::ToItself);
    // Three wide does not go into a number.
    CHECK(canvas.link({wide, 0, b, 1}) == LinkRefusal::TypesDoNotFit);
    // B feeds C; C may not feed A, which feeds B.
    CHECK(canvas.link({b, 0, c, 0}) == LinkRefusal::None);
    CHECK(canvas.link({c, 0, a, 0}) == LinkRefusal::Cycle);
    CHECK(canvas.links().size() == 2);

    // **An input has one source**: a second link into it takes the first's place.
    CHECK(canvas.link({c, 0, b, 1}) == LinkRefusal::Cycle);
    const u32 d = canvas.add(value("D", 0, 400, 0));
    CHECK(canvas.link({d, 0, b, 0}) == LinkRefusal::None);
    REQUIRE(canvas.linkInto(b, 0).has_value());
    CHECK(canvas.linkInto(b, 0)->from == d);
    CHECK(canvas.links().size() == 2);

    // A node removed takes its links with it.
    CHECK(canvas.remove(b));
    CHECK(canvas.links().empty());
    CHECK_FALSE(canvas.remove(b));
}

TEST_CASE("ADR 0211: states may be gone back to, entered from many, and left for themselves")
{
    NodeCanvas canvas(engine::app::CanvasRules{.cycles = true, .manyIntoAnInput = true, .toItself = true});
    const auto state = [](const char* title) {
        CanvasNode node;
        node.kind = "State";
        node.title = title;
        node.inputs.push_back({"", PinType::Flow});
        node.outputs.push_back({"", PinType::Flow});
        return node;
    };
    const u32 idle = canvas.add(state("Idle"));
    const u32 walk = canvas.add(state("Walk"));
    const u32 jump = canvas.add(state("Jump"));
    CHECK(canvas.link({idle, 0, walk, 0}) == LinkRefusal::None);
    CHECK(canvas.link({walk, 0, idle, 0}) == LinkRefusal::None);
    CHECK(canvas.link({jump, 0, idle, 0}) == LinkRefusal::None);
    CHECK(canvas.link({idle, 0, idle, 0}) == LinkRefusal::None);
    CHECK(canvas.links().size() == 4);
}

TEST_CASE("ADR 0211: the canvas's own undo takes an edit back and puts it forward, and knows what is unsaved")
{
    NodeCanvas canvas;
    const u32 a = canvas.add(value("A", 0, 0, 0));
    const u32 b = canvas.add(value("B", 300, 0, 1));
    canvas.commit();
    canvas.markSaved();
    CHECK_FALSE(canvas.dirty());
    CHECK_FALSE(canvas.canUndo());

    CHECK(canvas.link({a, 0, b, 0}) == LinkRefusal::None);
    canvas.commit();
    CHECK(canvas.dirty());
    CHECK(canvas.remove(a));
    canvas.commit();
    CHECK(canvas.nodes().size() == 1);

    REQUIRE(canvas.undo());
    CHECK(canvas.nodes().size() == 2);
    CHECK(canvas.links().size() == 1);
    REQUIRE(canvas.undo());
    CHECK(canvas.links().empty());
    // Back where it was saved.
    CHECK_FALSE(canvas.dirty());
    CHECK_FALSE(canvas.undo());
    REQUIRE(canvas.redo());
    CHECK(canvas.links().size() == 1);
    CHECK(canvas.dirty());
    // A node made after an undo has an id no node has had.
    const u32 c = canvas.add(value("C", 0, 300, 0));
    CHECK(c != a);
    CHECK(c != b);
    // A new edit, and what was undone cannot be put forward any more.
    canvas.commit();
    CHECK_FALSE(canvas.canRedo());
}

TEST_CASE("ADR 0211: a pointer selects, drags what is selected, and boxes")
{
    NodeCanvas canvas;
    const CanvasMetrics metrics;
    const u32 a = canvas.add(value("A", 100, 100, 1));
    const u32 b = canvas.add(value("B", 400, 100, 1));
    canvas.commit();

    // A press on a node's title selects it; dragged, it goes with the pointer.
    CanvasEvent event = drag(canvas, metrics, 150, 110, 250, 310);
    CHECK(event.kind == CanvasEvent::Kind::NodesMoved);
    CHECK(canvas.selected(a));
    CHECK(nearly(canvas.node(a)->x, 200.0f));
    CHECK(nearly(canvas.node(a)->y, 300.0f));
    CHECK(nearly(canvas.node(b)->x, 400.0f));
    // One edit to take back.
    REQUIRE(canvas.undo());
    CHECK(nearly(canvas.node(a)->y, 100.0f));

    // A click that slips a pixel moves nothing.
    event = drag(canvas, metrics, 450, 110, 451, 110);
    CHECK(canvas.selected(b));
    CHECK_FALSE(canvas.selected(a));
    CHECK(nearly(canvas.node(b)->x, 400.0f));

    // With the adding key the other joins, and the two are dragged as one.
    (void)drag(canvas, metrics, 150, 110, 150, 110, true);
    CHECK(canvas.selection().size() == 2);
    (void)drag(canvas, metrics, 150, 110, 170, 130);
    CHECK(nearly(canvas.node(a)->x, 120.0f));
    CHECK(nearly(canvas.node(b)->x, 420.0f));

    // A press on nothing lets go of them, and a box takes what it touches.
    (void)drag(canvas, metrics, 20, 20, 21, 20);
    CHECK(canvas.selection().empty());
    event = drag(canvas, metrics, 50, 50, 330, 400);
    CHECK(event.kind == CanvasEvent::Kind::SelectionChanged);
    CHECK(canvas.selected(a));
    CHECK_FALSE(canvas.selected(b));
}

TEST_CASE("ADR 0211: a link is drawn from a pin to a pin, either way round, and a refusal is said")
{
    NodeCanvas canvas;
    const CanvasMetrics metrics;
    const u32 a = canvas.add(value("A", 100, 100, 1));
    const u32 b = canvas.add(value("B", 400, 100, 1));
    const u32 wide = canvas.add(value("Wide", 100, 300, 0, PinType::Vector3));
    canvas.commit();
    const engine::app::CanvasPoint out = engine::app::outputPlace(*canvas.node(a), 0, metrics);
    const engine::app::CanvasPoint in = engine::app::inputPlace(*canvas.node(b), 0, metrics);

    // From the output to the input.
    CanvasEvent event = drag(canvas, metrics, out.x, out.y, in.x, in.y);
    CHECK(event.kind == CanvasEvent::Kind::LinkMade);
    CHECK(event.link == CanvasLink{a, 0, b, 0});
    REQUIRE(canvas.links().size() == 1);
    // The node did not move with it.
    CHECK(nearly(canvas.node(a)->x, 100.0f));
    REQUIRE(canvas.undo());
    CHECK(canvas.links().empty());

    // From the input back to the output: the same link.
    event = drag(canvas, metrics, in.x, in.y, out.x, out.y);
    CHECK(event.kind == CanvasEvent::Kind::LinkMade);
    CHECK(event.link == CanvasLink{a, 0, b, 0});

    // Three wide into a number: refused, and the event says which and why.
    const engine::app::CanvasPoint wideOut = engine::app::outputPlace(*canvas.node(wide), 0, metrics);
    event = drag(canvas, metrics, wideOut.x, wideOut.y, in.x, in.y);
    CHECK(event.kind == CanvasEvent::Kind::LinkRefused);
    CHECK(event.refusal == LinkRefusal::TypesDoNotFit);
    CHECK(canvas.links().size() == 1);

    // **Let go over nothing**: the end in hand is handed over, with where,
    // for a list of what could be put there.
    event = drag(canvas, metrics, wideOut.x, wideOut.y, 700, 500);
    CHECK(event.kind == CanvasEvent::Kind::LinkDropped);
    CHECK(event.fromOutput);
    CHECK(event.link.from == wide);
    CHECK(nearly(event.at.x, 700.0f));
    CHECK(nearly(event.at.y, 500.0f));

    // A press on the link's line chooses the link, and it is what is removed.
    (void)drag(canvas, metrics, (out.x + in.x) * 0.5f, (out.y + in.y) * 0.5f, (out.x + in.x) * 0.5f,
               (out.y + in.y) * 0.5f);
    REQUIRE(canvas.selectedLink().has_value());
    CHECK(canvas.selection().empty());
    CHECK(canvas.removeSelected() == 1);
    CHECK(canvas.links().empty());
    CHECK(canvas.nodes().size() == 3);
}

TEST_CASE("ADR 0211: the view is dragged and zoomed about the pointer, and a press lands where the node is drawn")
{
    NodeCanvas canvas;
    const CanvasMetrics metrics;
    const u32 a = canvas.add(value("A", 100, 100, 0));
    canvas.commit();

    // The other button drags the view.
    CanvasPointer at;
    at.x = 500;
    at.y = 300;
    at.panning = true;
    (void)canvas.pointer(at, metrics);
    at.x = 560;
    at.y = 340;
    (void)canvas.pointer(at, metrics);
    at.panning = false;
    (void)canvas.pointer(at, metrics);
    CHECK(nearly(canvas.pan().x, 60.0f));
    CHECK(nearly(canvas.pan().y, 40.0f));

    // The wheel zooms, and what was under the pointer still is.
    const engine::app::CanvasPoint under = canvas.toCanvas(300, 200);
    at.x = 300;
    at.y = 200;
    at.wheel = 2.0f;
    (void)canvas.pointer(at, metrics);
    at.wheel = 0.0f;
    CHECK(canvas.zoom() > 1.0f);
    const engine::app::CanvasPoint still = canvas.toCanvas(300, 200);
    CHECK(nearly(still.x, under.x));
    CHECK(nearly(still.y, under.y));

    // The node is pressed where it is on the panel now.
    const engine::app::CanvasPoint shown = canvas.toPanel(110, 110);
    (void)drag(canvas, metrics, shown.x, shown.y, shown.x, shown.y);
    CHECK(canvas.selected(a));

    // Framed, every node is inside the panel.
    canvas.setView({-5000.0f, 9000.0f}, 2.0f);
    canvas.frameAll(800.0f, 600.0f, metrics);
    const engine::app::CanvasRect rect = engine::app::nodeRect(*canvas.node(a), metrics);
    const engine::app::CanvasPoint corner = canvas.toPanel(rect.x, rect.y);
    const engine::app::CanvasPoint far = canvas.toPanel(rect.x + rect.width, rect.y + rect.height);
    CHECK(corner.x >= 0.0f);
    CHECK(corner.y >= 0.0f);
    CHECK(far.x <= 800.0f);
    CHECK(far.y <= 600.0f);
    CHECK(canvas.zoom() <= 1.0f);
}

TEST_CASE("ADR 0211: a copy of a selection brings the links among it and leaves the others")
{
    NodeCanvas canvas;
    const u32 a = canvas.add(value("A", 0, 0, 0));
    const u32 b = canvas.add(value("B", 300, 0, 1));
    const u32 c = canvas.add(value("C", 600, 0, 1));
    CHECK(canvas.link({a, 0, b, 0}) == LinkRefusal::None);
    CHECK(canvas.link({b, 0, c, 0}) == LinkRefusal::None);
    canvas.select(a);
    canvas.select(b, true);
    CHECK(canvas.duplicateSelected() == 2);
    CHECK(canvas.nodes().size() == 5);
    // A to B came with them; B to C, which was not copied, did not.
    CHECK(canvas.links().size() == 3);
    // The copies are what is selected, a step from where they came.
    REQUIRE(canvas.selection().size() == 2);
    CHECK_FALSE(canvas.selected(a));
    const CanvasNode* const copy = canvas.node(canvas.selection()[0]);
    REQUIRE(copy != nullptr);
    CHECK(copy->title == "A");
    CHECK(copy->x > 0.0f);
    CHECK(canvas.removeSelected() == 2);
    CHECK(canvas.nodes().size() == 3);
    CHECK(canvas.links().size() == 2);
}

TEST_CASE("ADR 0211: a pin of no width yet takes any, a node keeps the id its file gave it")
{
    using engine::app::pinAccepts;
    CHECK(pinAccepts(PinType::Any, PinType::Vector3));
    CHECK(pinAccepts(PinType::Vector2, PinType::Any));
    CHECK(pinAccepts(PinType::Any, PinType::Any));
    // Nothing is carried along an arrow, whatever the pin.
    CHECK_FALSE(pinAccepts(PinType::Any, PinType::Flow));
    CHECK_FALSE(pinAccepts(PinType::Flow, PinType::Any));

    NodeCanvas canvas;
    CHECK(canvas.add(value("Seven", 0.0f, 0.0f, 0), 7) == 7);
    // Taken already: it is given the next that nobody has.
    CHECK(canvas.add(value("Again", 0.0f, 0.0f, 0), 7) == 8);
    CHECK(canvas.add(value("Next", 0.0f, 0.0f, 1)) == 9);
    CHECK(canvas.add(value("Low", 0.0f, 0.0f, 0), 3) == 3);
    CHECK(canvas.add(value("After", 0.0f, 0.0f, 0)) == 10);

    // Once the graph has said how wide a pin came out, it is that wide.
    const u32 wide = canvas.add(value("Wide", 0.0f, 0.0f, 0, PinType::Any));
    CHECK(canvas.canLink({wide, 0, 9, 0}) == LinkRefusal::None);
    CHECK(canvas.setPinType(wide, true, 0, PinType::Vector3));
    CHECK(canvas.canLink({wide, 0, 9, 0}) == LinkRefusal::TypesDoNotFit);
    CHECK_FALSE(canvas.setPinType(wide, true, 4, PinType::Number));
    CHECK_FALSE(canvas.dirty());
}

TEST_CASE("ADR 0211: an arrow is drawn out of a box's edge and lands on the box it is let go over")
{
    engine::app::CanvasRules rules;
    rules.cycles = true;
    rules.manyIntoAnInput = true;
    rules.arrows = true;
    NodeCanvas canvas(rules);
    const CanvasMetrics metrics;
    const auto state = [](const char* title, f32 x, f32 y) {
        CanvasNode node;
        node.kind = "State";
        node.title = title;
        node.x = x;
        node.y = y;
        node.inputs.push_back({"In", PinType::Flow});
        node.outputs.push_back({"Out", PinType::Flow});
        return node;
    };
    const u32 idle = canvas.add(state("Idle", 100.0f, 100.0f));
    const u32 run = canvas.add(state("Run", 500.0f, 100.0f));
    // What every arrow may leave and nothing may enter.
    CanvasNode any;
    any.kind = "Any";
    any.x = 100.0f;
    any.y = 400.0f;
    any.outputs.push_back({"Out", PinType::Flow});
    const u32 anyId = canvas.add(any);
    canvas.commit();
    const engine::app::CanvasRect box = nodeRect(*canvas.node(idle), metrics);

    SUBCASE("a press inside a box moves it, and makes no arrow")
    {
        const CanvasEvent moved = drag(canvas, metrics, 190.0f, 120.0f, 190.0f, 220.0f);
        CHECK(moved.kind == CanvasEvent::Kind::NodesMoved);
        CHECK(canvas.links().empty());
        CHECK(nearly(canvas.node(idle)->y, 200.0f));
    }
    SUBCASE("a press on its rim draws an arrow, to anywhere on the other box")
    {
        const CanvasEvent made = drag(canvas, metrics, box.x + box.width, 120.0f, 590.0f, 125.0f);
        CHECK(made.kind == CanvasEvent::Kind::LinkMade);
        REQUIRE(canvas.links().size() == 1);
        CHECK(canvas.links()[0] == CanvasLink{idle, 0, run, 0});
        // Idle did not move.
        CHECK(nearly(canvas.node(idle)->x, 100.0f));

        // And back, which is another arrow beside the first.
        const engine::app::CanvasRect other = nodeRect(*canvas.node(run), metrics);
        CHECK(drag(canvas, metrics, other.x, 120.0f, 190.0f, 120.0f).kind == CanvasEvent::Kind::LinkMade);
        REQUIRE(canvas.links().size() == 2);
        const engine::app::CanvasLine there = arrowLine(*canvas.node(idle), *canvas.node(run), metrics);
        const engine::app::CanvasLine back = arrowLine(*canvas.node(run), *canvas.node(idle), metrics);
        CHECK(std::fabs(there.from.y - back.to.y) > metrics.pinReach);
        // Each is found where it is drawn, and not where the other is.
        const std::optional<CanvasLink> upper =
            canvas.linkAt((there.from.x + there.to.x) * 0.5f, (there.from.y + there.to.y) * 0.5f, metrics);
        REQUIRE(upper.has_value());
        CHECK(upper->from == idle);
        const std::optional<CanvasLink> lower =
            canvas.linkAt((back.from.x + back.to.x) * 0.5f, (back.from.y + back.to.y) * 0.5f, metrics);
        REQUIRE(lower.has_value());
        CHECK(lower->from == run);
        // An arrow's ends are on the rims: between the two boxes.
        CHECK(there.from.x >= box.x + box.width - 0.5f);
        CHECK(there.to.x <= other.x + 0.5f);

        // The same arrow again is refused as made already.
        const CanvasEvent again = drag(canvas, metrics, box.x + box.width, 120.0f, 590.0f, 125.0f);
        CHECK(again.kind == CanvasEvent::Kind::LinkRefused);
        CHECK(again.refusal == LinkRefusal::AlreadyMade);
    }
    SUBCASE("let go over nothing, it asks what should be put there; over the box it left, nothing")
    {
        const CanvasEvent dropped = drag(canvas, metrics, box.x + box.width, 120.0f, 380.0f, 300.0f);
        CHECK(dropped.kind == CanvasEvent::Kind::LinkDropped);
        CHECK(dropped.link.from == idle);
        CHECK(dropped.fromOutput);
        CHECK(nearly(dropped.at.x, 380.0f));
        CHECK(drag(canvas, metrics, box.x + box.width, 120.0f, 190.0f, 125.0f).kind == CanvasEvent::Kind::None);
        CHECK(canvas.links().empty());
    }
    SUBCASE("from any: arrows leave it, and none may enter")
    {
        const engine::app::CanvasRect from = nodeRect(*canvas.node(anyId), metrics);
        CHECK(drag(canvas, metrics, from.x + from.width, from.y + 10.0f, 590.0f, 125.0f).kind ==
              CanvasEvent::Kind::LinkMade);
        const CanvasEvent refused = drag(canvas, metrics, box.x + box.width, 120.0f, from.x + 40.0f, from.y + 10.0f);
        CHECK(refused.kind == CanvasEvent::Kind::LinkRefused);
        CHECK(refused.refusal == LinkRefusal::NoSuchPin);
    }
}

TEST_CASE("ADR 0211: a locked canvas is looked at: it selects and moves its view, and nothing on it changes")
{
    NodeCanvas canvas;
    const CanvasMetrics metrics;
    const u32 a = canvas.add(value("A", 0.0f, 0.0f, 0));
    const u32 b = canvas.add(value("B", 400.0f, 0.0f, 1));
    canvas.commit();
    canvas.setLocked(true);
    CHECK(canvas.locked());

    // A drag on a node selects it and leaves it where it is.
    const CanvasEvent pressed = drag(canvas, metrics, 90.0f, 10.0f, 90.0f, 200.0f);
    CHECK(pressed.kind == CanvasEvent::Kind::SelectionChanged);
    CHECK(canvas.selected(a));
    CHECK(nearly(canvas.node(a)->y, 0.0f));
    // A drag from a pin to a pin draws no link.
    const engine::app::CanvasPoint out = outputPlace(*canvas.node(a), 0, metrics);
    const engine::app::CanvasPoint in = inputPlace(*canvas.node(b), 0, metrics);
    (void)drag(canvas, metrics, out.x, out.y, in.x, in.y);
    CHECK(canvas.links().empty());
    CHECK_FALSE(canvas.dirty());
    CHECK_FALSE(canvas.canUndo());

    // Unlocked, the same drag makes it.
    canvas.setLocked(false);
    CHECK(drag(canvas, metrics, out.x, out.y, in.x, in.y).kind == CanvasEvent::Kind::LinkMade);
}
