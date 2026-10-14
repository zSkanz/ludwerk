#pragma once

// **A canvas of nodes** (ADR 0211): what the editor's Graph panel is a picture
// of, whichever graph is on it -- a material's, where a node is a step of a
// surface shader and a link carries a value, or an animation's, where a node
// is a state and a link is the transition from one to another.
//
// It knows nodes, pins, links, a selection, where the view is, and its own
// undo. It knows which links may be made, and says why one may not. It is
// driven by a pointer -- a place, a button, a wheel -- so everything a person
// does on it a test does too.
//
// Nothing here draws. How big a node is on the screen is the drawing's to
// say, and it says it through `CanvasMetrics`: a node's rectangle and its
// pins' places follow from those numbers alone, which is what lets a press
// be found with no window.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/types.h"

namespace engine::app {

// What a pin carries. A number widens to any width; the widths do not turn
// into each other. `Any` is a pin whose width is whatever is put into it --
// an arithmetic node's, until the graph it is in has said how wide it came
// out. `Flow` is an animation graph's: a state is entered and left, and
// nothing is carried.
enum class PinType : core::u8
{
    Number,
    Vector2,
    Vector3,
    Vector4,
    Any,
    Flow,
};

// True where a value of `from` may go into a pin of `to`.
[[nodiscard]] bool pinAccepts(PinType to, PinType from) noexcept;

struct CanvasPin
{
    std::string name;
    PinType type = PinType::Number;
};

// A field a node carries that is not a pin: a constant's value, a
// parameter's name, a state's clip. Text, as the graph's file has it; what
// it means is the graph's to say.
struct CanvasField
{
    std::string name;
    std::string value;
};

struct CanvasNode
{
    // Its own for as long as the canvas lives, and never given to another.
    core::u32 id = 0;
    // What it is -- "Multiply", "State" -- which is the graph's word; and
    // what it is called on the canvas, which is the person's.
    std::string kind;
    std::string title;
    // The top-left corner, in the canvas's own units: a pixel at a zoom of one.
    core::f32 x = 0.0f;
    core::f32 y = 0.0f;
    std::vector<CanvasPin> inputs;
    std::vector<CanvasPin> outputs;
    std::vector<CanvasField> fields;

    [[nodiscard]] const std::string* field(std::string_view name) const noexcept;
};

// From an output of one node to an input of another.
struct CanvasLink
{
    core::u32 from = 0;
    core::u32 output = 0;
    core::u32 to = 0;
    core::u32 input = 0;

    [[nodiscard]] bool operator==(const CanvasLink&) const noexcept = default;
};

// Why a link was not made.
enum class LinkRefusal : core::u8
{
    None,
    // One of the two is not there, or has no such pin.
    NoSuchPin,
    ToItself,
    // What the output carries is not what the input takes.
    TypesDoNotFit,
    // The same link, already made.
    AlreadyMade,
    // It would have a node feed itself, a step or many steps round.
    Cycle,
};

// What a graph allows, said once when a canvas is made for it.
struct CanvasRules
{
    // A value is worked out once and cannot depend on itself; a state may be
    // gone back to.
    bool cycles = false;
    // A value's input has one source, and a new link replaces the old; a
    // state is entered from as many others as there are.
    bool manyIntoAnInput = false;
    // A state may have an arrow to itself.
    bool toItself = false;
    // **Arrows, not wires**: a node is a box with no pins drawn, a link
    // leaves it from its EDGE -- a press on the rim draws one out, a press
    // inside moves the box -- and lands on whatever box it is let go over.
    // Such a node still has its one output and its one input, which is what
    // the link is between.
    bool arrows = false;
};

// The numbers a node's shape on the screen follows from. The drawing fills
// them from its font; a test fills them with round ones.
struct CanvasMetrics
{
    core::f32 nodeWidth = 180.0f;
    core::f32 titleHeight = 24.0f;
    core::f32 rowHeight = 22.0f;
    // How near a pin's centre a press has to be to be on it.
    core::f32 pinReach = 9.0f;
};

struct CanvasRect
{
    core::f32 x = 0.0f;
    core::f32 y = 0.0f;
    core::f32 width = 0.0f;
    core::f32 height = 0.0f;

    [[nodiscard]] bool holds(core::f32 px, core::f32 py) const noexcept
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

struct CanvasPoint
{
    core::f32 x = 0.0f;
    core::f32 y = 0.0f;
};

// The fields a node shows on itself: those with something in them. One left
// empty -- a parameter with no range -- takes no row.
[[nodiscard]] core::usize shownFields(const CanvasNode& node) noexcept;

// A node's rectangle and where its pins are, in the canvas's units: a row a
// pin under the title, inputs down the left edge and outputs down the right,
// and a row under those for each field it shows.
[[nodiscard]] CanvasRect nodeRect(const CanvasNode& node, const CanvasMetrics& metrics) noexcept;
[[nodiscard]] CanvasPoint inputPlace(const CanvasNode& node, core::u32 pin, const CanvasMetrics& metrics) noexcept;
[[nodiscard]] CanvasPoint outputPlace(const CanvasNode& node, core::u32 pin, const CanvasMetrics& metrics) noexcept;

// The two ends of an arrow between two boxes: on their rims, along the line
// between their centres, and a little to the right of the way it points so
// the arrow back does not lie on it.
struct CanvasLine
{
    CanvasPoint from;
    CanvasPoint to;
};
[[nodiscard]] CanvasLine arrowLine(const CanvasNode& from, const CanvasNode& to, const CanvasMetrics& metrics) noexcept;

// One frame of a pointer, in the PANEL's pixels from its top-left corner.
struct CanvasPointer
{
    core::f32 x = 0.0f;
    core::f32 y = 0.0f;
    // The main button: held, and the frames it went down and came up in.
    bool down = false;
    bool pressed = false;
    bool released = false;
    // The button the view is dragged by.
    bool panning = false;
    // Adds to the selection instead of replacing it.
    bool adding = false;
    core::f32 wheel = 0.0f;
};

// What a frame of the pointer did, for whoever owns the graph to act on.
struct CanvasEvent
{
    enum class Kind : core::u8
    {
        None,
        NodesMoved,
        LinkMade,
        LinkRefused,
        // A link let go of over nothing: the end that was in hand is in
        // `link` (its other end nought), `fromOutput` says which end that
        // is, and `at` is where on the canvas. For whoever owns the graph
        // to offer what could be put there.
        LinkDropped,
        SelectionChanged,
    };
    Kind kind = Kind::None;
    LinkRefusal refusal = LinkRefusal::None;
    CanvasLink link;
    bool fromOutput = true;
    CanvasPoint at;
};

class NodeCanvas
{
public:
    NodeCanvas() = default;
    explicit NodeCanvas(CanvasRules rules) : m_rules(rules) {}

    [[nodiscard]] const CanvasRules& rules() const noexcept { return m_rules; }
    [[nodiscard]] const std::vector<CanvasNode>& nodes() const noexcept { return m_nodes; }
    [[nodiscard]] const std::vector<CanvasLink>& links() const noexcept { return m_links; }
    [[nodiscard]] const CanvasNode* node(core::u32 id) const noexcept;

    // --- What a graph is made of -------------------------------------------------
    // Adds a node and says its id. `node.id` is not read; `wanted` is the id
    // it is given where that is free -- a node read from a file keeps the id
    // the file gave it, so the file is the same file when it is saved.
    core::u32 add(CanvasNode node, core::u32 wanted = 0);
    // Removes a node and every link it had. False for one that is not there.
    bool remove(core::u32 id);
    bool move(core::u32 id, core::f32 x, core::f32 y);
    bool rename(core::u32 id, std::string_view title);
    // Sets a field, adding it where the node has none by that name.
    bool setField(core::u32 id, std::string_view name, std::string_view value);
    // What a pin carries, once the graph has said: not an edit, and no link
    // is looked at again.
    bool setPinType(core::u32 id, bool output, core::u32 pin, PinType type);
    // Whether a link may be made, and why not.
    [[nodiscard]] LinkRefusal canLink(const CanvasLink& link) const noexcept;
    // Makes it. Where an input takes one source, the link it had is removed.
    LinkRefusal link(const CanvasLink& link);
    bool unlink(const CanvasLink& link);
    // The link into an input, where it takes one.
    [[nodiscard]] std::optional<CanvasLink> linkInto(core::u32 node, core::u32 input) const noexcept;
    void clear();

    // --- The selection --------------------------------------------------------------
    [[nodiscard]] const std::vector<core::u32>& selection() const noexcept { return m_selection; }
    [[nodiscard]] bool selected(core::u32 id) const noexcept;
    void select(core::u32 id, bool adding = false);
    void selectNone() noexcept { m_selection.clear(); }
    // A link chosen by itself, to be read or cut: an animation's arrow is a
    // thing with fields of its own.
    [[nodiscard]] std::optional<CanvasLink> selectedLink() const noexcept { return m_selectedLink; }
    void selectLink(const CanvasLink& link);
    // Removes what is selected -- the nodes, or the link. How many things.
    core::u32 removeSelected();
    // A copy of each selected node, a step down and to the right, selected in
    // their place; links among them are copied and links to others are not.
    core::u32 duplicateSelected();

    // --- Its own undo ------------------------------------------------------------------
    // What the graph was is kept each time `commit` is called: once an edit,
    // by whoever made it. A drag is one edit, committed as it ends.
    void commit();
    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const noexcept { return m_undo.size() > 1; }
    [[nodiscard]] bool canRedo() const noexcept { return !m_redo.empty(); }
    // Changes since `markSaved`.
    [[nodiscard]] bool dirty() const noexcept { return m_edits != m_savedAt; }
    void markSaved() noexcept { m_savedAt = m_edits; }

    // --- The view -----------------------------------------------------------------------
    [[nodiscard]] core::f32 zoom() const noexcept { return m_zoom; }
    [[nodiscard]] CanvasPoint pan() const noexcept { return m_pan; }
    void setView(CanvasPoint pan, core::f32 zoom) noexcept;
    // A place on the panel as a place on the canvas, and back.
    [[nodiscard]] CanvasPoint toCanvas(core::f32 panelX, core::f32 panelY) const noexcept;
    [[nodiscard]] CanvasPoint toPanel(core::f32 canvasX, core::f32 canvasY) const noexcept;
    // Moves and scales the view so every node is in a panel of this size.
    void frameAll(core::f32 panelWidth, core::f32 panelHeight, const CanvasMetrics& metrics) noexcept;

    // --- Driven by a pointer --------------------------------------------------------
    // One frame. A press on a pin begins a link, and a release on a pin that
    // takes it makes it; a press on a node selects it and a drag moves what
    // is selected; a press on nothing begins a box, and a release selects
    // what the box touches; the other button drags the view, and the wheel
    // zooms about where the pointer is.
    CanvasEvent pointer(const CanvasPointer& at, const CanvasMetrics& metrics);
    // **Looked at, not changed**: while it is locked a pointer still selects,
    // boxes and moves the view, and neither moves a node nor draws a link.
    // What a graph that is running is.
    void setLocked(bool locked) noexcept { m_locked = locked; }
    [[nodiscard]] bool locked() const noexcept { return m_locked; }

    // What is in hand, for the drawing: a link being drawn out from a pin, or
    // a box.
    struct Dragging
    {
        enum class Kind : core::u8
        {
            Nothing,
            Nodes,
            Link,
            Box,
            View,
        };
        Kind kind = Kind::Nothing;
        // A link: the pin it leaves, and whether that pin is an output.
        core::u32 node = 0;
        core::u32 pin = 0;
        bool fromOutput = true;
        // Where the pointer went down and where it is, on the canvas.
        CanvasPoint from;
        CanvasPoint to;
    };
    [[nodiscard]] const Dragging& dragging() const noexcept { return m_dragging; }

    // The node at a place on the canvas, topmost first; and the pin.
    [[nodiscard]] std::optional<core::u32> nodeAt(core::f32 x, core::f32 y,
                                                  const CanvasMetrics& metrics) const noexcept;
    struct PinAt
    {
        core::u32 node = 0;
        core::u32 pin = 0;
        bool output = false;
    };
    [[nodiscard]] std::optional<PinAt> pinAt(core::f32 x, core::f32 y, const CanvasMetrics& metrics) const noexcept;
    // The link whose line passes nearest a place, within a pin's reach.
    [[nodiscard]] std::optional<CanvasLink> linkAt(core::f32 x, core::f32 y,
                                                   const CanvasMetrics& metrics) const noexcept;
    // Where arrows are: the box whose rim a place is on, which is where an
    // arrow is drawn out from.
    [[nodiscard]] std::optional<core::u32> rimAt(core::f32 x, core::f32 y, const CanvasMetrics& metrics) const noexcept;

private:
    struct Snapshot
    {
        std::vector<CanvasNode> nodes;
        std::vector<CanvasLink> links;
    };

    [[nodiscard]] CanvasNode* find(core::u32 id) noexcept;
    [[nodiscard]] bool reaches(core::u32 from, core::u32 to) const noexcept;
    void restore(const Snapshot& snapshot);

    CanvasRules m_rules;
    std::vector<CanvasNode> m_nodes;
    std::vector<CanvasLink> m_links;
    std::vector<core::u32> m_selection;
    std::optional<CanvasLink> m_selectedLink;
    core::u32 m_nextId = 1;

    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    core::u64 m_edits = 0;
    core::u64 m_savedAt = 0;

    CanvasPoint m_pan;
    core::f32 m_zoom = 1.0f;
    Dragging m_dragging;
    // Where each dragged node was when the drag began, and whether it moved.
    std::vector<std::pair<core::u32, CanvasPoint>> m_dragStart;
    CanvasPoint m_lastPanel;
    bool m_moved = false;
    bool m_locked = false;
};

} // namespace engine::app
