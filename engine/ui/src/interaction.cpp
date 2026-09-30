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
void probe(const scene::World& world, core::InstanceId id, Vec2 point, Rect clip, core::InstanceId& best, f32& bestZ)
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
    if (contains(box, point) && contains(clip, point) && self->zIndex >= bestZ) {
        best = id;
        bestZ = self->zIndex;
    }

    for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child))
        probe(world, child, point, childClip, best, bestZ);
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

} // namespace

void resetInteraction() noexcept
{
    g_state = InteractionState{};
}

core::InstanceId hitTest(const scene::World& world, core::InstanceId uiService, core::Vec2 point)
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
            probe(world, element, point, whole, hit, bestZ);
        }
        if (hit.valid())
            best = hit;
    }
    return best;
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
    const core::InstanceId onScreen = hitTest(world, uiService, input.pointer);
    const core::InstanceId over = onScreen.valid() ? onScreen : input.worldOver;
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
        if (over.valid() && over == g_state.pressedOn)
            fire(world, over, "Activated");
        g_state.pressedOn = {};
        g_state.dragging = false;
    };
    // **The end of one click and the start of the next in one frame** (D362):
    // the release is the earlier of the two, so it ends the first click.
    const bool releaseBeforePress = input.pressed && input.released && input.releasedFirst;
    if (releaseBeforePress)
        release();

    if (input.pressed) {
        g_state.pressedOn = over;

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

    if (input.released && !releaseBeforePress)
        release();

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
