// Hit-testing, hover, activation and text focus (api-design.md §2.2's
// `UIObject` signals and `TextInput`; architecture.md §2's `ui`).
//
// Everything here reads rectangles the layout already produced and computes no
// layout of its own. That separation is what makes "what is on screen" and
// "what the pointer is over" the same answer: they come from one set of
// numbers, written once per frame.
//
// The events are enqueued as POD facts on the world's change queue, exactly as
// a property write is, and `script` turns them into deferred fires. Nothing in
// this file knows what a Luau value is (R17).
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "engine/scene/ui_pages.h"
#include "engine/scene/world.h"
#include "engine/ui/text_edit.h"
#include "engine/ui/ui.h"
#include "text_field.h"

namespace engine::ui {
namespace {

using core::Rect;
using core::Vec2;

// Frame-to-frame interaction state.
//
// Held here rather than in a component for one reason: none of it is world
// state. A snapshot that restored "the pointer was hovering this button" would
// be restoring a fact about a mouse, and a replay has no mouse -- it has the
// input stream, which produces the hover again on its own.
struct InteractionState
{
    core::InstanceId hovered;
    // What the press started on. `Activated` needs both ends on the same
    // element: a press that slid off a button and released elsewhere is a
    // cancelled press, which is what every UI in the world does and what people
    // rely on to change their minds.
    core::InstanceId pressedOn;
    core::InstanceId focused;

    // **The focused field's editing session** (ADR 0139): its undo history
    // above all, which is the player's for as long as the field has focus and
    // nobody else's. One, because one field has focus.
    TextEditState session;
    // Its text when it took focus, for `RevertOnEscape`.
    std::string focusText;
    // A press in the field is being dragged: the selection follows it.
    bool dragging = false;
    // When the player last did something in it, for the caret's blink.
    core::f64 lastActivity = 0.0;

    // --- A drag (ADR 0128) ------------------------------------------------------
    //
    // What a press landed on that can be dragged, and where the pointer and the
    // element were when it did. `dragMoved` is whether the pointer has left the
    // few pixels a press is allowed to wander: until then it is still a press.
    core::InstanceId dragDetector;
    core::InstanceId dragTarget;
    Vec2 dragPointer;
    Vec2 dragOrigin;
    core::UDim2 dragPosition;
    f32 dragRotation = 0.0f;
    f32 dragAngle = 0.0f;
    bool dragMoved = false;

    // A press on a `ScrollFrame`'s content, which a finger drags to scroll
    // (D477): the frame, where the pointer was and where its canvas was.
    core::InstanceId scrolling;
    Vec2 scrollPointer;
    Vec2 scrollCanvas;
    bool scrollMoved = false;

    // A press on the pages of a `UIPageLayout`: the layout, and where.
    core::InstanceId swiping;
    Vec2 swipePointer;

    // What `UIService.SelectedObject` was when the events for it last fired.
    core::InstanceId selected;
};

InteractionState g_state;

[[nodiscard]] bool contains(Rect box, Vec2 point) noexcept
{
    return point.x >= box.min.x && point.x < box.max.x && point.y >= box.min.y && point.y < box.max.y;
}

void fire(scene::World& world, core::InstanceId subject, const char* event)
{
    if (!subject.valid() || !world.alive(subject))
        return;
    world.changes().push(
        scene::Change{scene::ChangeKind::InstanceEventNoArgs, subject, {}, world.atoms().intern(event)});
}

// Depth-first, in draw order, keeping the LAST hit: the element drawn on top is
// the one the pointer is over, and draw order is `ZIndex` then document order.
// Walking the tree twice -- once to draw and once to hit-test -- is what keeps
// the two from disagreeing about which is on top.
// `everything`: whatever is drawn there, whether or not it takes the pointer
// -- what somebody arranging the interface points at.
void probe(const scene::World& world, core::InstanceId id, Vec2 point, Rect clip, core::InstanceId& best, f32& bestZ,
           bool everything = false)
{
    const scene::UIObjectComponent* self = world.uiObjects().find(id);
    if (self == nullptr || !self->visible)
        return;

    const Rect box{self->absolutePosition, self->absolutePosition + self->absoluteSize};
    Rect childClip = clip;
    if (self->clipsDescendants || world.scrollFrames().find(id) != nullptr) {
        childClip = Rect{Vec2{std::fmax(box.min.x, clip.min.x), std::fmax(box.min.y, clip.min.y)},
                         Vec2{std::fmin(box.max.x, clip.max.x), std::fmin(box.max.y, clip.max.y)}};
    }

    // Clipped OUT means not hit, which is the whole reason the clip is threaded
    // through: an element scrolled off the end of a list must not answer a
    // click that lands where it would have been.
    //
    // **And only what takes the pointer is hit** (D452). Every visible
    // element was: a press on the number in a button activated it and a press
    // on the star beside the number did nothing, the star being on top and a
    // label; and a clear frame laid over a screen to hold its children took
    // every press meant for the buttons in the screens under it.
    if (contains(box, point) && contains(clip, point) && self->zIndex >= bestZ &&
        (everything || takesPointer(world, id))) {
        best = id;
        bestZ = self->zIndex;
    }

    for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child))
        probe(world, child, point, childClip, best, bestZ, everything);
}

// --- The focused field (ADR 0139) ---------------------------------------------

// `FocusLost(submitted, reason)` for `subject`.
void focusLost(scene::World& world, core::InstanceId subject, bool submitted, u32 reason)
{
    if (!subject.valid() || !world.alive(subject))
        return;
    world.changes().push(scene::Change{scene::ChangeKind::FocusLost, subject,
                                       core::InstanceId{submitted ? 1u : 0u, reason},
                                       world.atoms().intern("FocusLost")});
}

// `UIService`'s own pair of focus events, carrying the field.
void serviceEvent(scene::World& world, core::InstanceId uiService, const char* event, core::InstanceId input)
{
    if (!uiService.valid() || !world.alive(uiService) || !world.alive(input))
        return;
    world.changes().push(
        scene::Change{scene::ChangeKind::InstanceEvent, uiService, input, world.atoms().intern(event)});
}

void textEvent(scene::World& world, core::InstanceId subject, const char* event, std::string text)
{
    if (!subject.valid() || !world.alive(subject))
        return;
    world.changes().pushText(subject, world.atoms().intern(event), std::move(text));
}

// The layout of the screen `id` is on is stale: its text changed.
void markScreenDirty(scene::World& world, core::InstanceId id)
{
    for (core::InstanceId current = id; current.valid(); current = world.parentOf(current)) {
        if (scene::ScreenGuiComponent* tree = world.screenGuis().find(current); tree != nullptr) {
            tree->layoutDirty = true;
            return;
        }
    }
}

// `Enum.FocusLossReason`'s values.
constexpr u32 LostSubmitted = 0;
constexpr u32 LostMoved = 1;
constexpr u32 LostCancelled = 2;
constexpr u32 LostScript = 3;

[[nodiscard]] TextEditRules rulesOf(const scene::TextInputComponent& field) noexcept
{
    TextEditRules rules;
    rules.maxLength = field.maxLength;
    rules.multiLine = field.multiLine;
    rules.masked = field.masked;
    rules.editable = field.editable;
    return rules;
}

void releaseFocus(scene::World& world, core::InstanceId uiService, bool submitted, u32 reason)
{
    const core::InstanceId was = g_state.focused;
    if (!was.valid())
        return;
    if (scene::TextInputComponent* field = world.textInputs().find(was); field != nullptr) {
        field->focused = false;
        field->composition.clear();
        field->anchor = field->caret;
    }
    g_state.focused = {};
    g_state.dragging = false;
    focusLost(world, was, submitted, reason);
    serviceEvent(world, uiService, "TextInputFocusReleased", was);
}

// Gives `id` the keyboard; whatever had it loses it for `reason`.
void takeFocus(scene::World& world, core::InstanceId uiService, core::InstanceId id, u32 reason)
{
    if (const scene::TextInputComponent* already = world.textInputs().find(id);
        g_state.focused == id && already != nullptr && already->focused)
        return;
    releaseFocus(world, uiService, false, reason);
    scene::TextInputComponent* field = world.textInputs().find(id);
    scene::TextLabelComponent* label = world.textLabels().find(id);
    if (field == nullptr || label == nullptr)
        return;
    field->focused = true;
    field->textReplaced = false;
    g_state.focused = id;
    g_state.focusText = label->text;
    setText(g_state.session, label->text);
    if (field->clearTextOnFocus && !label->text.empty()) {
        label->text.clear();
        setText(g_state.session, {});
        world.changes().push(scene::Change{scene::ChangeKind::PropertyChanged, id, {}, world.atoms().intern("Text")});
        markScreenDirty(world, id);
    }
    if (field->selectAllOnFocus)
        selectAll(g_state.session);
    field->caret = g_state.session.caret;
    field->anchor = g_state.session.anchor;
    fire(world, id, "Focused");
    serviceEvent(world, uiService, "TextInputFocused", id);
}

// The field after (or before) `from` on its screen, in document order, round
// the end: where Tab goes.
[[nodiscard]] core::InstanceId neighbourField(const scene::World& world, core::InstanceId from, bool backwards)
{
    core::InstanceId screen = from;
    while (screen.valid() && world.screenGuis().find(screen) == nullptr)
        screen = world.parentOf(screen);
    if (!screen.valid())
        return {};
    std::vector<core::InstanceId> all;
    world.collectDescendants(screen, all);
    std::vector<core::InstanceId> fields;
    for (const core::InstanceId id : all) {
        const scene::UIObjectComponent* object = world.uiObjects().find(id);
        if (world.textInputs().find(id) != nullptr && object != nullptr && object->visible)
            fields.push_back(id);
    }
    const auto here = std::find(fields.begin(), fields.end(), from);
    if (fields.size() < 2 || here == fields.end())
        return {};
    const usize index = static_cast<usize>(here - fields.begin());
    const usize next = backwards ? (index + fields.size() - 1) % fields.size() : (index + 1) % fields.size();
    return fields[next];
}

// The text offset under the pointer in the focused field.
[[nodiscard]] u32 offsetUnder(const scene::World& world, core::InstanceId id, Vec2 pointer)
{
    const FieldView view = fieldView(world, id);
    return textOffsetOf(view, displayOffsetAt(view, pointer));
}

// The flags a host set before the commands existed, as commands.
void legacyCommands(const InteractionInput& input, std::vector<TextCommand>& out)
{
    const auto add = [&](TextCommand::Kind kind, TextMove move = TextMove::Left) {
        TextCommand command;
        command.kind = kind;
        command.move = static_cast<core::u8>(move);
        out.push_back(command);
    };
    // Delete, then insert, then move: the order the flags always meant.
    if (input.backspace)
        add(TextCommand::Kind::DeleteBackward);
    if (input.forwardDelete)
        add(TextCommand::Kind::DeleteForward);
    if (!input.text.empty()) {
        TextCommand typed;
        typed.kind = TextCommand::Kind::Insert;
        typed.text = input.text;
        out.push_back(typed);
    }
    if (input.caretHome)
        add(TextCommand::Kind::Move, TextMove::TextStart);
    if (input.caretEnd)
        add(TextCommand::Kind::Move, TextMove::TextEnd);
    if (input.caretLeft)
        add(TextCommand::Kind::Move, TextMove::Left);
    if (input.caretRight)
        add(TextCommand::Kind::Move, TextMove::Right);
    if (input.submit)
        add(TextCommand::Kind::Return);
}

// --- Selection (ADR 0128) -----------------------------------------------------

void pointEvent(scene::World& world, core::InstanceId subject, const char* event, Vec2 point)
{
    if (!subject.valid() || !world.alive(subject))
        return;
    world.changes().push(scene::Change{scene::ChangeKind::InstanceEventVector2, subject, scene::eventPoint(point),
                                       world.atoms().intern(event)});
}

// Shown all the way up: an element inside a hidden one is not there.
[[nodiscard]] bool shown(const scene::World& world, core::InstanceId id)
{
    for (core::InstanceId current = id; current.valid(); current = world.parentOf(current)) {
        if (const scene::UIObjectComponent* object = world.uiObjects().find(current);
            object != nullptr && !object->visible)
            return false;
        if (const scene::ScreenGuiComponent* screen = world.screenGuis().find(current); screen != nullptr)
            return screen->enabled;
    }
    return false;
}

[[nodiscard]] Vec2 centreOf(const scene::UIObjectComponent& object) noexcept
{
    return object.absolutePosition + object.absoluteSize * 0.5f;
}

// Every object a selection could go to: selectable, shown, on an enabled
// screen. In `DisplayOrder` then document order, which is what "the first"
// means and what breaks a tie (R10).
void collectSelectable(const scene::World& world, core::InstanceId uiService, std::vector<core::InstanceId>& out)
{
    out.clear();
    std::vector<std::pair<f32, core::InstanceId>> screens;
    for (core::InstanceId child = world.firstChild(uiService); child.valid(); child = world.nextSibling(child)) {
        if (const scene::ScreenGuiComponent* screen = world.screenGuis().find(child);
            screen != nullptr && screen->enabled)
            screens.emplace_back(screen->displayOrder, child);
    }
    // The topmost screen first: a menu over a HUD is where a selection starts.
    std::stable_sort(screens.begin(), screens.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<core::InstanceId> all;
    for (const auto& [order, screen] : screens) {
        all.clear();
        world.collectDescendants(screen, all);
        for (const core::InstanceId id : all) {
            if (isSelectable(world, id) && shown(world, id))
                out.push_back(id);
        }
    }
}

// **The nearest selectable object in a direction, from where they are.** Ahead
// of `from` along the direction, and the one that is least far: its distance
// along the direction plus twice its distance off it, so that the button
// straight below wins over a nearer one that is mostly to the side -- which is
// what a hand on a d-pad means by "down".
[[nodiscard]] core::InstanceId nearestToward(const scene::World& world, core::InstanceId uiService,
                                             core::InstanceId from, Vec2 direction)
{
    const scene::UIObjectComponent* origin = world.uiObjects().find(from);
    if (origin == nullptr)
        return {};
    const Vec2 here = centreOf(*origin);
    std::vector<core::InstanceId> candidates;
    collectSelectable(world, uiService, candidates);
    core::InstanceId best;
    f32 bestCost = 1.0e30f;
    for (const core::InstanceId id : candidates) {
        if (id == from)
            continue;
        const Vec2 offset = centreOf(*world.uiObjects().find(id)) - here;
        const f32 along = offset.x * direction.x + offset.y * direction.y;
        if (along <= 0.5f)
            continue;
        const f32 off = std::fabs(offset.x * direction.y - offset.y * direction.x);
        const f32 cost = along + 2.0f * off;
        if (cost < bestCost) {
            bestCost = cost;
            best = id;
        }
    }
    return best;
}

// The ancestor of `id`, or `id` itself, that has something of `Pool` as a
// child -- a drag detector, a page layout -- and which child it is.
template <typename Accept>
[[nodiscard]] std::pair<core::InstanceId, core::InstanceId> ownerOf(const scene::World& world, core::InstanceId id,
                                                                    Accept accept)
{
    for (core::InstanceId current = id; current.valid(); current = world.parentOf(current)) {
        if (world.uiObjects().find(current) == nullptr)
            break;
        for (core::InstanceId child = world.firstChild(current); child.valid(); child = world.nextSibling(child)) {
            if (accept(child))
                return {current, child};
        }
    }
    return {};
}

[[nodiscard]] Rect boxOf(const scene::UIObjectComponent& object) noexcept
{
    return Rect{object.absolutePosition, object.absolutePosition + object.absoluteSize};
}

// The size of what `id`'s `Position` is a fraction of, in pixels.
[[nodiscard]] Vec2 parentExtent(const scene::World& world, core::InstanceId id)
{
    const core::InstanceId parent = world.parentOf(id);
    if (const scene::UIObjectComponent* object = world.uiObjects().find(parent); object != nullptr)
        return object->absoluteSize;
    return world.engineState().viewportSize;
}

// How far past a press the pointer may wander and still be a press.
constexpr f32 DragSlop = 4.0f;

// One frame of a drag: the pointer is at `pointer`, and the element goes where
// the detector's style and limits put it.
void continueDrag(scene::World& world, Vec2 pointer)
{
    scene::UIDragDetectorComponent* detector = world.uiDragDetectors().find(g_state.dragDetector);
    scene::UIObjectComponent* target = world.uiObjects().find(g_state.dragTarget);
    if (detector == nullptr || target == nullptr)
        return;

    // In the element's units: its `Position`'s offsets are, and a scaled
    // screen's pixels are not.
    const f32 unit = target->unitScale > 0.0f ? target->unitScale : 1.0f;
    const Vec2 parentSize = parentExtent(world, g_state.dragTarget);
    Vec2 moved = (pointer - g_state.dragPointer) * (1.0f / unit);

    constexpr i32 TranslateLine = 1;
    constexpr i32 Rotate = 2;
    constexpr i32 Scriptable = 3;
    constexpr i32 ResponseScale = 1;
    constexpr i32 ResponseCustomOffset = 2;
    constexpr i32 ResponseCustomScale = 3;

    if (detector->dragStyle == Scriptable)
        return;

    if (detector->dragStyle == Rotate) {
        const Vec2 centre = centreOf(*target);
        const f32 angle = std::atan2(pointer.y - centre.y, pointer.x - centre.x);
        f32 turned = (angle - g_state.dragAngle) * 57.29577951f;
        // The short way round, so a drag across the angle's seam does not spin
        // the dial a whole turn back.
        while (turned > 180.0f)
            turned -= 360.0f;
        while (turned < -180.0f)
            turned += 360.0f;
        f32 total = detector->dragRotation + turned;
        g_state.dragAngle = angle;
        if (detector->maxDragAngle > detector->minDragAngle)
            total = std::clamp(total, detector->minDragAngle, detector->maxDragAngle);
        detector->dragRotation = total;
        if (detector->responseStyle != ResponseCustomOffset && detector->responseStyle != ResponseCustomScale) {
            target->rotation = g_state.dragRotation + total;
            world.changes().push(scene::Change{
                scene::ChangeKind::PropertyChanged, g_state.dragTarget, {}, world.atoms().intern("Rotation")});
        }
        return;
    }

    if (detector->dragStyle == TranslateLine) {
        const f32 length =
            std::sqrt(detector->dragAxis.x * detector->dragAxis.x + detector->dragAxis.y * detector->dragAxis.y);
        if (length <= 0.0f)
            return;
        const Vec2 axis = detector->dragAxis * (1.0f / length);
        const f32 along = moved.x * axis.x + moved.y * axis.y;
        moved = axis * along;
    }

    // The limits, in the same units, measured from where the drag began.
    const Vec2 parentUnits = parentSize * (1.0f / unit);
    const auto resolved = [&](const core::UDim2& value) {
        return Vec2{value.x.scale * parentUnits.x + value.x.offset, value.y.scale * parentUnits.y + value.y.offset};
    };
    const Vec2 least = resolved(detector->minDragTranslation);
    const Vec2 most = resolved(detector->maxDragTranslation);
    if (most.x > least.x)
        moved.x = std::clamp(moved.x, least.x, most.x);
    if (most.y > least.y)
        moved.y = std::clamp(moved.y, least.y, most.y);

    // Inside another element: where the box would be, pushed back in.
    if (const scene::UIObjectComponent* bound = world.uiObjects().find(detector->boundingUI); bound != nullptr) {
        const Rect room = boxOf(*bound);
        const Vec2 began = g_state.dragOrigin;
        const Vec2 size = target->absoluteSize;
        Vec2 wanted = began + moved * unit;
        // A box larger than its bound stays at the bound's near edge.
        wanted.x = std::fmax(room.min.x, std::fmin(wanted.x, room.max.x - size.x));
        wanted.y = std::fmax(room.min.y, std::fmin(wanted.y, room.max.y - size.y));
        moved = (wanted - began) * (1.0f / unit);
    }

    const bool asScale = detector->responseStyle == ResponseScale || detector->responseStyle == ResponseCustomScale;
    core::UDim2 total;
    if (asScale) {
        total.x.scale = parentUnits.x > 0.0f ? moved.x / parentUnits.x : 0.0f;
        total.y.scale = parentUnits.y > 0.0f ? moved.y / parentUnits.y : 0.0f;
    }
    else {
        total.x.offset = moved.x;
        total.y.offset = moved.y;
    }
    detector->dragUDim2 = total;
    if (detector->responseStyle == ResponseCustomOffset || detector->responseStyle == ResponseCustomScale)
        return;

    core::UDim2 position = g_state.dragPosition;
    position.x.scale += total.x.scale;
    position.x.offset += total.x.offset;
    position.y.scale += total.y.scale;
    position.y.offset += total.y.offset;
    target->position = position;
    world.changes().push(
        scene::Change{scene::ChangeKind::PropertyChanged, g_state.dragTarget, {}, world.atoms().intern("Position")});
    markScreenDirty(world, g_state.dragTarget);
}

void endDrag(scene::World& world, Vec2 pointer)
{
    if (g_state.dragDetector.valid() && g_state.dragMoved)
        pointEvent(world, g_state.dragDetector, "DragEnd", pointer);
    g_state.dragDetector = {};
    g_state.dragTarget = {};
    g_state.dragMoved = false;
}

// Moves a `ScrollFrame`'s canvas to `position`, in its units; the layout
// clamps it to the canvas. True when it moved.
bool scrollTo(scene::World& world, core::InstanceId id, Vec2 position)
{
    scene::ScrollFrameComponent* scroll = world.scrollFrames().find(id);
    const scene::UIObjectComponent* object = world.uiObjects().find(id);
    if (scroll == nullptr || object == nullptr)
        return false;
    const f32 unit = object->unitScale > 0.0f ? object->unitScale : 1.0f;
    const Vec2 view = object->absoluteSize * (1.0f / unit);
    const Vec2 canvas{scroll->canvasSize.x.scale * view.x + scroll->canvasSize.x.offset,
                      scroll->canvasSize.y.scale * view.y + scroll->canvasSize.y.offset};
    const Vec2 room{std::fmax(0.0f, canvas.x - view.x), std::fmax(0.0f, canvas.y - view.y)};
    const Vec2 clamped{std::clamp(position.x, 0.0f, room.x), std::clamp(position.y, 0.0f, room.y)};
    if (clamped == scroll->canvasPosition)
        return false;
    scroll->canvasPosition = clamped;
    world.changes().push(
        scene::Change{scene::ChangeKind::PropertyChanged, id, {}, world.atoms().intern("CanvasPosition")});
    markScreenDirty(world, id);
    return true;
}

// The `ScrollFrame` that `id` is, or is inside.
[[nodiscard]] core::InstanceId scrollFrameOver(const scene::World& world, core::InstanceId id)
{
    for (core::InstanceId current = id; current.valid(); current = world.parentOf(current)) {
        if (world.scrollFrames().find(current) != nullptr)
            return current;
        if (world.uiObjects().find(current) == nullptr)
            break;
    }
    return {};
}

// Says what `UIService.SelectedObject` is now, to whoever is listening, when
// it is not what it was: the object that lost it, the one that gained it, and
// the service.
void announceSelection(scene::World& world, core::InstanceId uiService)
{
    core::InstanceId& selected = world.engineState().uiSelected;
    // Something destroyed or hidden is not selected, whoever selected it.
    if (selected.valid() && (!world.alive(selected) || world.destroyed(selected) || !isSelectable(world, selected) ||
                             !shown(world, selected)))
        selected = {};
    if (selected == g_state.selected)
        return;
    fire(world, g_state.selected, "SelectionLost");
    fire(world, selected, "SelectionGained");
    g_state.selected = selected;
    if (uiService.valid() && world.alive(uiService)) {
        world.changes().push(scene::Change{scene::ChangeKind::InstanceEvent, uiService, selected,
                                           world.atoms().intern("SelectionChanged")});
        world.changes().push(
            scene::Change{scene::ChangeKind::PropertyChanged, uiService, {}, world.atoms().intern("SelectedObject")});
    }
}

} // namespace

void resetInteraction() noexcept
{
    g_state = InteractionState{};
}

bool isSelectable(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* object = world.uiObjects().find(id);
    if (object == nullptr)
        return false;
    if (object->selectable >= 0)
        return object->selectable != 0;
    // Told nothing: as what it is. A button, by its class or one it extends,
    // and a field to type into.
    if (world.textInputs().find(id) != nullptr)
        return true;
    for (const scene::ClassDescriptor* current = world.classes().find(world.classOf(id)); current != nullptr;
         current = world.classes().find(current->super)) {
        const std::string_view name = world.atoms().text(current->name);
        if (name == "TextButton" || name == "ImageButton")
            return true;
    }
    return false;
}

bool takesPointer(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* object = world.uiObjects().find(id);
    if (object == nullptr)
        return false;
    if (object->active >= 0)
        return object->active != 0;
    // Told nothing: as what it is does. A button, by its class or one it
    // extends -- every `UIObject` has `Activated`, so the event says nothing.
    for (const scene::ClassDescriptor* current = world.classes().find(world.classOf(id)); current != nullptr;
         current = world.classes().find(current->super)) {
        const std::string_view name = world.atoms().text(current->name);
        if (name == "TextButton" || name == "ImageButton")
            return true;
    }
    if (world.textInputs().find(id) != nullptr || world.scrollFrames().find(id) != nullptr)
        return true;
    if (world.textLabels().find(id) != nullptr || world.imageLabels().find(id) != nullptr)
        return false;
    // A frame, and anything else that is a rectangle: there when it is drawn.
    return object->backgroundTransparency < 1.0f;
}

namespace {

[[nodiscard]] core::InstanceId hitTestScreens(const scene::World& world, core::InstanceId uiService, core::Vec2 point,
                                              bool everything)
{
    if (!uiService.valid())
        return {};

    // Screens in `DisplayOrder`, so the topmost tree wins outright: a modal over
    // a HUD takes the click even where the HUD has a button.
    std::vector<std::pair<f32, core::InstanceId>> screens;
    for (core::InstanceId child = world.firstChild(uiService); child.valid(); child = world.nextSibling(child)) {
        if (const scene::ScreenGuiComponent* screen = world.screenGuis().find(child);
            screen != nullptr && screen->enabled) {
            screens.emplace_back(screen->displayOrder, child);
        }
    }
    std::stable_sort(screens.begin(), screens.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    const Rect whole{Vec2{-1.0e9f, -1.0e9f}, Vec2{1.0e9f, 1.0e9f}};
    core::InstanceId best;
    for (const auto& [order, screen] : screens) {
        core::InstanceId hit;
        f32 bestZ = -1.0e30f;
        for (core::InstanceId element = world.firstChild(screen); element.valid();
             element = world.nextSibling(element)) {
            probe(world, element, point, whole, hit, bestZ, everything);
        }
        if (hit.valid())
            best = hit;
    }
    return best;
}

} // namespace

core::InstanceId hitTest(const scene::World& world, core::InstanceId uiService, core::Vec2 point)
{
    return hitTestScreens(world, uiService, point, false);
}

core::InstanceId elementAt(const scene::World& world, core::InstanceId uiService, core::Vec2 point)
{
    return hitTestScreens(world, uiService, point, true);
}

core::InstanceId hitTestCanvas(const scene::World& world, core::InstanceId root, core::Vec2 point)
{
    const Rect whole{Vec2{-1.0e9f, -1.0e9f}, Vec2{1.0e9f, 1.0e9f}};
    core::InstanceId hit;
    f32 bestZ = -1.0e30f;
    for (core::InstanceId element = world.firstChild(root); element.valid(); element = world.nextSibling(element))
        probe(world, element, point, whole, hit, bestZ);
    return hit;
}

InteractionResult updateInteraction(scene::World& world, core::InstanceId uiService, const InteractionInput& input)
{
    InteractionResult result;
    if (!uiService.valid())
        return result;

    // The screen first, because it is drawn over the world; a button printed on
    // a wall answers only where no screen element covers it.
    // A locked pointer is over nothing of it (G17).
    const core::InstanceId onScreen =
        input.pointerLocked ? core::InstanceId{} : hitTest(world, uiService, input.pointer);
    const core::InstanceId over = onScreen.valid() || input.pointerLocked ? onScreen : input.worldOver;
    result.pointerOverUi = over.valid();

    // Hover, as a pair of edges rather than a state a handler has to compare
    // against its own memory.
    if (over != g_state.hovered) {
        fire(world, g_state.hovered, "PointerExited");
        fire(world, over, "PointerEntered");
        g_state.hovered = over;
    }

    // **A script's `CaptureFocus` and `ReleaseFocus`** (ADR 0139), taken here,
    // where focus is moved and nowhere else.
    {
        std::vector<std::pair<core::InstanceId, core::i8>> requests;
        world.textInputs().forEach([&requests](core::InstanceId id, scene::TextInputComponent& field) {
            if (field.focusRequest != 0) {
                requests.emplace_back(id, field.focusRequest);
                field.focusRequest = 0;
            }
        });
        for (const auto& [id, request] : requests) {
            if (request > 0) {
                takeFocus(world, uiService, id, LostScript);
            }
            else if (id == g_state.focused) {
                const scene::TextInputComponent* field = world.textInputs().find(id);
                releaseFocus(world, uiService, field != nullptr && field->releaseSubmitted, LostScript);
            }
        }
    }

    // A release ends the click it belongs to. Both ends on the same element,
    // which is what makes a press cancellable by sliding off it.
    const auto release = [&] {
        // **A press that became a drag is not a press** (ADR 0128): a window
        // dragged by a button in its title bar does not also press the button,
        // a list scrolled by a finger does not press the row under it, and a
        // page swiped does not press what is on it.
        const bool moved = g_state.dragMoved || g_state.scrollMoved;
        bool swiped = false;
        if (g_state.swiping.valid()) {
            if (const scene::UIPageLayoutComponent* pages = world.uiPageLayouts().find(g_state.swiping);
                pages != nullptr && pages->touchInputEnabled) {
                const Vec2 travelled = input.pointer - g_state.swipePointer;
                const f32 along = pages->fillDirection == 0 ? travelled.x : travelled.y;
                // A sixth of the pages' own extent, and never under a thumb's
                // width: a tap that wandered is not a swipe.
                const scene::UIObjectComponent* holder = world.uiObjects().find(world.parentOf(g_state.swiping));
                const f32 extent = holder != nullptr
                                       ? (pages->fillDirection == 0 ? holder->absoluteSize.x : holder->absoluteSize.y)
                                       : 0.0f;
                const f32 enough = std::fmax(24.0f, extent / 6.0f);
                if (std::fabs(along) >= enough && !moved) {
                    swiped = true;
                    (void)scene::stepPage(world, g_state.swiping, along < 0.0f ? 1 : -1);
                }
            }
        }
        if (over.valid() && over == g_state.pressedOn && !moved && !swiped)
            fire(world, over, "Activated");
        endDrag(world, input.pointer);
        g_state.pressedOn = {};
        g_state.dragging = false;
        g_state.scrolling = {};
        g_state.scrollMoved = false;
        g_state.swiping = {};
    };
    // **The end of one click and the start of the next in one frame** (D362):
    // the release is the earlier of the two, so it ends the first click.
    const bool releaseBeforePress = input.pressed && input.released && input.releasedFirst;
    if (releaseBeforePress)
        release();

    if (input.pressed) {
        g_state.pressedOn = over;

        // **What the press could become** (ADR 0128): a drag, when what it
        // landed on or something it is inside has a detector; otherwise a
        // scroll, inside a `ScrollFrame`; and a page turn, over a
        // `UIPageLayout`'s pages.
        endDrag(world, input.pointer);
        g_state.scrolling = {};
        g_state.scrollMoved = false;
        g_state.swiping = {};
        const auto [dragTarget, dragDetector] = ownerOf(world, over, [&world](core::InstanceId child) {
            const scene::UIDragDetectorComponent* detector = world.uiDragDetectors().find(child);
            return detector != nullptr && detector->enabled;
        });
        if (dragDetector.valid()) {
            const scene::UIObjectComponent* target = world.uiObjects().find(dragTarget);
            scene::UIDragDetectorComponent* detector = world.uiDragDetectors().find(dragDetector);
            g_state.dragDetector = dragDetector;
            g_state.dragTarget = dragTarget;
            g_state.dragPointer = input.pointer;
            g_state.dragOrigin = target->absolutePosition;
            g_state.dragPosition = target->position;
            g_state.dragRotation = target->rotation;
            const Vec2 centre = centreOf(*target);
            g_state.dragAngle = std::atan2(input.pointer.y - centre.y, input.pointer.x - centre.x);
            g_state.dragMoved = false;
            detector->dragUDim2 = {};
            detector->dragRotation = 0.0f;
        }
        else {
            if (const core::InstanceId frame = scrollFrameOver(world, over); frame.valid()) {
                g_state.scrolling = frame;
                g_state.scrollPointer = input.pointer;
                g_state.scrollCanvas = world.scrollFrames().find(frame)->canvasPosition;
            }
            const auto [holder, pages] = ownerOf(world, over, [&world](core::InstanceId child) {
                const scene::UIPageLayoutComponent* layout = world.uiPageLayouts().find(child);
                return layout != nullptr && layout->touchInputEnabled;
            });
            if (pages.valid() && !g_state.scrolling.valid()) {
                g_state.swiping = pages;
                g_state.swipePointer = input.pointer;
            }
        }

        // The hand is on the pointer: a selection the engine manages goes.
        if (world.engineState().uiAutoSelect)
            world.engineState().uiSelected = {};

        // Focus moves on the press, and moves AWAY on a press that lands
        // anywhere else -- including on nothing. A field that kept focus after
        // the player clicked the world would go on eating their keystrokes.
        const core::InstanceId wanted = world.textInputs().find(over) != nullptr ? over : core::InstanceId{};
        if (!wanted.valid()) {
            releaseFocus(world, uiService, false, LostMoved);
        }
        else {
            const bool arriving = wanted != g_state.focused;
            takeFocus(world, uiService, wanted, LostMoved);
            const scene::TextInputComponent* field = world.textInputs().find(wanted);
            // **Under the pointer**: a press places the caret, Shift+press
            // extends, a double press takes the word and a triple the line --
            // unless focusing selected everything, which is what that asked for.
            if (field != nullptr && !(arriving && field->selectAllOnFocus)) {
                TextEditState& session = g_state.session;
                const u32 at = offsetUnder(world, wanted, input.pointer);
                if (input.clicks >= 3) {
                    if (field->multiLine) {
                        const auto [from, to] = lineAt(session.text, at);
                        select(session, from, to);
                    }
                    else {
                        selectAll(session);
                    }
                }
                else if (input.clicks == 2) {
                    const auto [from, to] = wordAt(session.text, at);
                    select(session, from, to);
                }
                else {
                    setCaret(session, at, input.shiftPress && !arriving);
                    g_state.dragging = true;
                }
                // The field shows the session's caret from here on.
                if (scene::TextInputComponent* placed = world.textInputs().find(wanted); placed != nullptr) {
                    placed->caret = session.caret;
                    placed->anchor = session.anchor;
                }
                g_state.lastActivity = input.time;
            }
        }
    }

    // A drag in the focused field selects from where it started.
    if (g_state.dragging && g_state.focused.valid() && input.pointerHeld && !input.pressed) {
        setCaret(g_state.session, offsetUnder(world, g_state.focused, input.pointer), true);
        if (scene::TextInputComponent* dragged = world.textInputs().find(g_state.focused); dragged != nullptr) {
            dragged->caret = g_state.session.caret;
            dragged->anchor = g_state.session.anchor;
        }
        g_state.lastActivity = input.time;
    }

    // A drag, while the button is held: past the slop it begins, and from then
    // on the element follows.
    if (g_state.dragDetector.valid() && input.pointerHeld && !input.pressed) {
        const scene::UIDragDetectorComponent* detector = world.uiDragDetectors().find(g_state.dragDetector);
        if (detector == nullptr || !detector->enabled || !world.alive(g_state.dragTarget)) {
            endDrag(world, input.pointer);
        }
        else {
            const Vec2 travelled = input.pointer - g_state.dragPointer;
            if (!g_state.dragMoved && std::fabs(travelled.x) + std::fabs(travelled.y) > DragSlop) {
                g_state.dragMoved = true;
                pointEvent(world, g_state.dragDetector, "DragStart", g_state.dragPointer);
            }
            if (g_state.dragMoved) {
                continueDrag(world, input.pointer);
                pointEvent(world, g_state.dragDetector, "DragContinue", input.pointer);
            }
        }
    }

    // **A list dragged by a finger scrolls** (D477): the content follows the
    // pointer, so the canvas goes the other way.
    if (g_state.scrolling.valid() && input.pointerHeld && !input.pressed && !g_state.dragging) {
        const Vec2 travelled = input.pointer - g_state.scrollPointer;
        if (g_state.scrollMoved || std::fabs(travelled.x) + std::fabs(travelled.y) > DragSlop) {
            const scene::UIObjectComponent* frame = world.uiObjects().find(g_state.scrolling);
            const f32 unit = frame != nullptr && frame->unitScale > 0.0f ? frame->unitScale : 1.0f;
            // A press only stops being one when the list actually went
            // somewhere: a frame with nowhere to scroll still presses its rows.
            if (scrollTo(world, g_state.scrolling, g_state.scrollCanvas - travelled * (1.0f / unit)))
                g_state.scrollMoved = true;
        }
    }

    // **The wheel** (D477, ADR 0128): the scroll frame under the pointer moves
    // its canvas, three lines a notch; failing one, the pages under it turn.
    if (input.wheel.x != 0.0f || input.wheel.y != 0.0f) {
        constexpr f32 NotchUnits = 48.0f;
        bool taken = false;
        for (core::InstanceId frame = scrollFrameOver(world, over); frame.valid() && !taken;
             frame = scrollFrameOver(world, world.parentOf(frame))) {
            const scene::ScrollFrameComponent* scroll = world.scrollFrames().find(frame);
            // A frame that cannot move that way hands the wheel to the one it
            // is inside -- a list at its end scrolls the page it is on.
            taken = scrollTo(world, frame,
                             Vec2{scroll->canvasPosition.x + input.wheel.x * NotchUnits,
                                  scroll->canvasPosition.y - input.wheel.y * NotchUnits});
        }
        if (!taken) {
            const auto [holder, pages] = ownerOf(world, over, [&world](core::InstanceId child) {
                const scene::UIPageLayoutComponent* layout = world.uiPageLayouts().find(child);
                return layout != nullptr && layout->scrollWheelInputEnabled;
            });
            if (pages.valid())
                taken = scene::stepPage(world, pages, input.wheel.y < 0.0f || input.wheel.x > 0.0f ? 1 : -1);
        }
        result.wheelTaken = taken;
    }

    if (input.released && !releaseBeforePress)
        release();

    // --- A gamepad or the arrow keys (ADR 0128) --------------------------------
    {
        scene::EngineState& state = world.engineState();
        announceSelection(world, uiService);
        const bool typing = g_state.focused.valid();
        if ((input.navigateX != 0 || input.navigateY != 0) && !typing) {
            if (!state.uiSelected.valid()) {
                // Nothing selected and a hand on the d-pad: the first.
                if (state.uiAutoSelect) {
                    std::vector<core::InstanceId> candidates;
                    collectSelectable(world, uiService, candidates);
                    if (!candidates.empty())
                        state.uiSelected = candidates.front();
                }
            }
            else if (const scene::UIObjectComponent* object = world.uiObjects().find(state.uiSelected);
                     object != nullptr) {
                // One axis a step: across first, as a row of buttons is read.
                const bool across = input.navigateX != 0;
                const Vec2 direction = across ? Vec2{static_cast<f32>(input.navigateX), 0.0f}
                                              : Vec2{0.0f, static_cast<f32>(input.navigateY)};
                core::InstanceId next =
                    across ? (input.navigateX < 0 ? object->nextSelectionLeft : object->nextSelectionRight)
                           : (input.navigateY < 0 ? object->nextSelectionUp : object->nextSelectionDown);
                // Where it was told to go, when that is somewhere a selection
                // can be; the nearest that way otherwise.
                if (!next.valid() || !world.alive(next) || !isSelectable(world, next) || !shown(world, next))
                    next = nearestToward(world, uiService, state.uiSelected, direction);
                if (next.valid())
                    state.uiSelected = next;
            }
        }
        if (input.navigateActivate && state.uiSelected.valid() && !typing) {
            fire(world, state.uiSelected, "Activated");
            // A field to type into takes the keyboard, as a press on it would.
            if (world.textInputs().find(state.uiSelected) != nullptr)
                takeFocus(world, uiService, state.uiSelected, LostMoved);
        }
        if (input.pageStep != 0) {
            // The pages the selection is on; with none selected, the first
            // page layout that is shown.
            const auto accepts = [&world](core::InstanceId child) {
                const scene::UIPageLayoutComponent* layout = world.uiPageLayouts().find(child);
                return layout != nullptr && layout->gamepadInputEnabled;
            };
            core::InstanceId pages = ownerOf(world, state.uiSelected, accepts).second;
            if (!pages.valid()) {
                world.uiPageLayouts().forEach([&](core::InstanceId id, const scene::UIPageLayoutComponent& layout) {
                    if (!pages.valid() && layout.gamepadInputEnabled && shown(world, world.parentOf(id)))
                        pages = id;
                });
            }
            if (pages.valid())
                (void)scene::stepPage(world, pages, input.pageStep);
        }
        announceSelection(world, uiService);
        result.selectionActive = state.uiSelected.valid();
    }

    if (g_state.focused.valid()) {
        const core::InstanceId id = g_state.focused;
        scene::TextLabelComponent* label = world.textLabels().find(id);
        scene::TextInputComponent* field = world.textInputs().find(id);
        const scene::UIObjectComponent* object = world.uiObjects().find(id);
        // A field that stopped being one, or stopped being seen, lets go.
        if (label == nullptr || field == nullptr || object == nullptr || !object->visible) {
            releaseFocus(world, uiService, false, LostScript);
        }
        else {
            TextEditState& session = g_state.session;
            // A script wrote `Text`: the session starts again (its history is
            // not the player's), and a script's caret write is taken as is.
            if (field->textReplaced || session.text != label->text) {
                setText(session, label->text);
                field->textReplaced = false;
            }
            session.caret = characterBoundary(session.text, field->caret);
            session.anchor = characterBoundary(session.text, field->anchor);

            if (input.compositionChanged) {
                field->composition.assign(input.composition);
                // The platform's cursor is in characters; the drawing wants
                // bytes of the composition.
                u32 cursor = 0;
                for (i32 step = 0; step < input.compositionCursor && cursor < field->composition.size(); ++step)
                    cursor = nextCharacter(field->composition, cursor);
                field->compositionCursor = cursor;
                g_state.lastActivity = input.time;
            }

            std::vector<TextCommand> legacy;
            std::span<const TextCommand> commands = input.commands;
            if (commands.empty()) {
                legacyCommands(input, legacy);
                commands = legacy;
            }

            const TextEditRules rules = rulesOf(*field);
            std::string rejected;
            // How the field was left this frame, if it was.
            bool leave = false;
            bool leaveSubmitted = false;
            u32 leaveReason = LostScript;
            core::InstanceId tabTo;
            bool submitted = false;
            for (const TextCommand& command : commands) {
                if (leave || tabTo.valid())
                    break;
                // **While composing, the keys are the input method's**: Return
                // commits, Escape cancels and Backspace edits the composition.
                const bool composing = !field->composition.empty();
                g_state.lastActivity = input.time;
                switch (command.kind) {
                case TextCommand::Kind::Insert: {
                    const TextEditResult done = insert(session, command.text, rules, true);
                    rejected += done.rejected;
                    field->composition.clear();
                    break;
                }
                case TextCommand::Kind::Move:
                    if (!composing)
                        move(session, static_cast<TextMove>(command.move), command.extend);
                    break;
                case TextCommand::Kind::LineUp:
                case TextCommand::Kind::LineDown: {
                    if (composing)
                        break;
                    const bool up = command.kind == TextCommand::Kind::LineUp;
                    if (!field->multiLine) {
                        move(session, up ? TextMove::TextStart : TextMove::TextEnd, command.extend);
                        break;
                    }
                    field->caret = session.caret;
                    const FieldView view = fieldView(world, id);
                    const Vec2 at = caretPoint(view, displayOffset(view, session.caret));
                    const Vec2 target{at.x + 0.5f, at.y + (up ? -0.5f : 1.5f) * view.lineHeight};
                    setCaret(session, textOffsetOf(view, displayOffsetAt(view, target)), command.extend);
                    break;
                }
                case TextCommand::Kind::DeleteBackward:
                    if (!composing)
                        (void)deleteBackward(session, command.word, rules);
                    break;
                case TextCommand::Kind::DeleteForward:
                    if (!composing)
                        (void)deleteForward(session, command.word, rules);
                    break;
                case TextCommand::Kind::SelectAll:
                    selectAll(session);
                    break;
                case TextCommand::Kind::Copy:
                    if (const std::string copied = copy(session, rules); !copied.empty() && input.writeClipboard)
                        input.writeClipboard(copied);
                    break;
                case TextCommand::Kind::Cut: {
                    std::string taken;
                    if (cut(session, rules, taken).changed && input.writeClipboard)
                        input.writeClipboard(taken);
                    break;
                }
                case TextCommand::Kind::Paste:
                    if (input.readClipboard) {
                        if (const std::optional<std::string> pasted = input.readClipboard(); pasted.has_value())
                            rejected += insert(session, *pasted, rules, false).rejected;
                    }
                    break;
                case TextCommand::Kind::Undo:
                    if (rules.editable)
                        (void)undo(session);
                    break;
                case TextCommand::Kind::Redo:
                    if (rules.editable)
                        (void)redo(session);
                    break;
                case TextCommand::Kind::Return:
                    if (composing)
                        break;
                    // A multi-line field takes a new line; Ctrl+Return submits it.
                    if (field->multiLine && !command.ctrl) {
                        rejected += insert(session, "\n", rules, false).rejected;
                        break;
                    }
                    submitted = true;
                    if (field->releaseFocusOnSubmit) {
                        leave = true;
                        leaveSubmitted = true;
                        leaveReason = LostSubmitted;
                    }
                    break;
                case TextCommand::Kind::Escape:
                    if (composing)
                        break;
                    if (field->revertOnEscape)
                        setText(session, g_state.focusText);
                    leave = true;
                    leaveReason = LostCancelled;
                    break;
                case TextCommand::Kind::Tab:
                    tabTo = neighbourField(world, id, command.extend);
                    break;
                }
            }

            // The text, as the player left it.
            if (session.text != label->text) {
                label->text = session.text;
                // The same fact a script's own write produces, so a handler on
                // `GetPropertyChangedSignal("Text")` sees typing exactly as it
                // sees an assignment -- and `TextChanged`, which is the
                // player's alone.
                world.changes().push(
                    scene::Change{scene::ChangeKind::PropertyChanged, id, {}, world.atoms().intern("Text")});
                textEvent(world, id, "TextChanged", label->text);
                markScreenDirty(world, id);
            }
            if (!rejected.empty())
                textEvent(world, id, "InputRejected", std::move(rejected));
            field->caret = session.caret;
            field->anchor = session.anchor;
            if (submitted)
                textEvent(world, id, "Submitted", label->text);

            // **The caret blinks**, and is solid while the player is busy.
            const core::f64 since = input.time - g_state.lastActivity;
            field->caretVisible = since < 0.5 || std::fmod(since, 1.06) < 0.53;

            // **The caret stays in view**: a line longer than the field scrolls
            // along with it, and a multi-line field scrolls by lines.
            {
                const FieldView view = fieldView(world, id);
                const u32 shown = field->composition.empty() ? displayOffset(view, session.caret)
                                                             : view.compositionBegin + field->compositionCursor;
                const Vec2 point = caretPoint(view, shown);
                const f32 width = view.box.max.x - view.box.min.x;
                const f32 height = view.box.max.y - view.box.min.y;
                constexpr f32 Margin = 2.0f;
                if (!field->multiLine) {
                    const f32 x = point.x - view.box.min.x;
                    if (x < 0.0f)
                        field->scroll.x += x;
                    else if (x > width - Margin)
                        field->scroll.x += x - (width - Margin);
                    const f32 widest = view.lines.empty() ? 0.0f : view.lines.front().width;
                    field->scroll.x = std::clamp(field->scroll.x, 0.0f, std::fmax(0.0f, widest - width + Margin));
                    field->scroll.y = 0.0f;
                }
                else {
                    const f32 y = point.y - view.box.min.y;
                    if (y < 0.0f)
                        field->scroll.y += y;
                    else if (y + view.lineHeight > height)
                        field->scroll.y += y + view.lineHeight - height;
                    const f32 total = static_cast<f32>(view.lines.size()) * view.lineHeight;
                    field->scroll.y = std::clamp(field->scroll.y, 0.0f, std::fmax(0.0f, total - height));
                    field->scroll.x = 0.0f;
                }
                const FieldView scrolled = fieldView(world, id);
                const Vec2 caretAt = caretPoint(scrolled, shown);
                result.caret = core::Rect{caretAt, Vec2{caretAt.x + 1.5f, caretAt.y + scrolled.lineHeight}};
            }

            if (tabTo.valid()) {
                takeFocus(world, uiService, tabTo, LostMoved);
            }
            else if (leave) {
                releaseFocus(world, uiService, leaveSubmitted, leaveReason);
                if (leaveReason == LostCancelled && label->text != session.text) {
                    label->text = session.text;
                    world.changes().push(
                        scene::Change{scene::ChangeKind::PropertyChanged, id, {}, world.atoms().intern("Text")});
                    markScreenDirty(world, id);
                }
            }
        }
    }

    // A destroyed element stops being hovered, pressed or focused rather than
    // leaving a stale id that would fire an event at a corpse.
    if (!world.alive(g_state.hovered))
        g_state.hovered = {};
    if (!world.alive(g_state.pressedOn))
        g_state.pressedOn = {};
    if (!world.alive(g_state.focused))
        g_state.focused = {};
    if (!world.alive(g_state.scrolling))
        g_state.scrolling = {};
    if (!world.alive(g_state.swiping))
        g_state.swiping = {};
    if (g_state.dragDetector.valid() && (!world.alive(g_state.dragDetector) || !world.alive(g_state.dragTarget)))
        endDrag(world, input.pointer);

    result.textInputFocused = g_state.focused.valid();
    result.focusedInput = g_state.focused;
    if (const scene::TextInputComponent* field = world.textInputs().find(g_state.focused); field != nullptr) {
        result.keyboardType = field->keyboardType;
        result.masked = field->masked;
        result.multiLine = field->multiLine;
    }
    return result;
}

} // namespace engine::ui
