#include "engine/app/node_canvas.h"

#include <algorithm>
#include <cmath>

namespace engine::app {

using core::f32;
using core::u32;
using core::usize;

namespace {

constexpr f32 kLeastZoom = 0.25f;
constexpr f32 kMostZoom = 2.0f;
// How far the pointer travels with the button down before a press is a drag:
// a click on a node that slips a pixel selects it and moves nothing.
constexpr f32 kDragSlack = 3.0f;
// How far a copy stands from what it was copied from.
constexpr f32 kCopyStep = 32.0f;

[[nodiscard]] f32 distanceToSegment(CanvasPoint p, CanvasPoint a, CanvasPoint b) noexcept
{
    const f32 dx = b.x - a.x;
    const f32 dy = b.y - a.y;
    const f32 length = dx * dx + dy * dy;
    f32 t = length > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const f32 x = a.x + dx * t - p.x;
    const f32 y = a.y + dy * t - p.y;
    return std::sqrt(x * x + y * y);
}

} // namespace

bool pinAccepts(PinType to, PinType from) noexcept
{
    if (to == PinType::Flow || from == PinType::Flow)
        return to == from;
    // A width not yet said is let through: the graph it is in says whether
    // it came to fit, on the node.
    if (to == PinType::Any || from == PinType::Any)
        return true;
    return to == from || from == PinType::Number;
}

const std::string* CanvasNode::field(std::string_view name) const noexcept
{
    for (const CanvasField& held : fields) {
        if (held.name == name)
            return &held.value;
    }
    return nullptr;
}

usize shownFields(const CanvasNode& node) noexcept
{
    return static_cast<usize>(std::count_if(node.fields.begin(), node.fields.end(),
                                            [](const CanvasField& held) { return !held.value.empty(); }));
}

CanvasRect nodeRect(const CanvasNode& node, const CanvasMetrics& metrics) noexcept
{
    const usize rows = std::max(node.inputs.size(), node.outputs.size()) + shownFields(node);
    return {node.x, node.y, metrics.nodeWidth, metrics.titleHeight + static_cast<f32>(rows) * metrics.rowHeight};
}

CanvasPoint inputPlace(const CanvasNode& node, u32 pin, const CanvasMetrics& metrics) noexcept
{
    return {node.x, node.y + metrics.titleHeight + (static_cast<f32>(pin) + 0.5f) * metrics.rowHeight};
}

CanvasPoint outputPlace(const CanvasNode& node, u32 pin, const CanvasMetrics& metrics) noexcept
{
    return {node.x + metrics.nodeWidth,
            node.y + metrics.titleHeight + (static_cast<f32>(pin) + 0.5f) * metrics.rowHeight};
}

CanvasLine arrowLine(const CanvasNode& from, const CanvasNode& to, const CanvasMetrics& metrics) noexcept
{
    const CanvasRect a = nodeRect(from, metrics);
    const CanvasRect b = nodeRect(to, metrics);
    CanvasPoint start{a.x + a.width * 0.5f, a.y + a.height * 0.5f};
    CanvasPoint end{b.x + b.width * 0.5f, b.y + b.height * 0.5f};
    const f32 dx = end.x - start.x;
    const f32 dy = end.y - start.y;
    const f32 length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0f)
        return {start, end};
    // To the right of the way it points.
    const f32 aside = metrics.pinReach * 0.7f;
    const f32 nx = -dy / length * aside;
    const f32 ny = dx / length * aside;
    start = {start.x + nx, start.y + ny};
    end = {end.x + nx, end.y + ny};
    // Each end is brought to its box's rim along the line.
    const auto rim = [](const CanvasRect& rect, CanvasPoint inside, CanvasPoint outside) {
        const f32 x = outside.x - inside.x;
        const f32 y = outside.y - inside.y;
        f32 least = 1.0f;
        if (x > 0.0f)
            least = std::min(least, (rect.x + rect.width - inside.x) / x);
        if (x < 0.0f)
            least = std::min(least, (rect.x - inside.x) / x);
        if (y > 0.0f)
            least = std::min(least, (rect.y + rect.height - inside.y) / y);
        if (y < 0.0f)
            least = std::min(least, (rect.y - inside.y) / y);
        least = std::clamp(least, 0.0f, 1.0f);
        return CanvasPoint{inside.x + x * least, inside.y + y * least};
    };
    return {rim(a, start, end), rim(b, end, start)};
}

const CanvasNode* NodeCanvas::node(u32 id) const noexcept
{
    for (const CanvasNode& held : m_nodes) {
        if (held.id == id)
            return &held;
    }
    return nullptr;
}

CanvasNode* NodeCanvas::find(u32 id) noexcept
{
    for (CanvasNode& held : m_nodes) {
        if (held.id == id)
            return &held;
    }
    return nullptr;
}

u32 NodeCanvas::add(CanvasNode added, u32 wanted)
{
    if (wanted != 0 && node(wanted) == nullptr) {
        added.id = wanted;
        m_nextId = std::max(m_nextId, wanted + 1);
    }
    else {
        added.id = m_nextId++;
    }
    m_nodes.push_back(std::move(added));
    return m_nodes.back().id;
}

bool NodeCanvas::remove(u32 id)
{
    const auto found =
        std::find_if(m_nodes.begin(), m_nodes.end(), [id](const CanvasNode& held) { return held.id == id; });
    if (found == m_nodes.end())
        return false;
    m_nodes.erase(found);
    std::erase_if(m_links, [id](const CanvasLink& held) { return held.from == id || held.to == id; });
    std::erase(m_selection, id);
    if (m_selectedLink.has_value() && (m_selectedLink->from == id || m_selectedLink->to == id))
        m_selectedLink.reset();
    return true;
}

bool NodeCanvas::move(u32 id, f32 x, f32 y)
{
    CanvasNode* const held = find(id);
    if (held == nullptr || !std::isfinite(x) || !std::isfinite(y))
        return false;
    held->x = x;
    held->y = y;
    return true;
}

bool NodeCanvas::rename(u32 id, std::string_view title)
{
    CanvasNode* const held = find(id);
    if (held == nullptr)
        return false;
    held->title = std::string(title);
    return true;
}

bool NodeCanvas::setField(u32 id, std::string_view name, std::string_view value)
{
    CanvasNode* const held = find(id);
    if (held == nullptr)
        return false;
    for (CanvasField& field : held->fields) {
        if (field.name == name) {
            field.value = std::string(value);
            return true;
        }
    }
    held->fields.push_back({std::string(name), std::string(value)});
    return true;
}

bool NodeCanvas::setPinType(u32 id, bool output, u32 pin, PinType type)
{
    CanvasNode* const held = find(id);
    if (held == nullptr)
        return false;
    std::vector<CanvasPin>& pins = output ? held->outputs : held->inputs;
    if (pin >= pins.size())
        return false;
    pins[pin].type = type;
    return true;
}

// Whether following links out of `from` comes to `to`.
bool NodeCanvas::reaches(u32 from, u32 to) const noexcept
{
    std::vector<u32> open{from};
    std::vector<u32> seen;
    while (!open.empty()) {
        const u32 at = open.back();
        open.pop_back();
        if (at == to)
            return true;
        if (std::find(seen.begin(), seen.end(), at) != seen.end())
            continue;
        seen.push_back(at);
        for (const CanvasLink& held : m_links) {
            if (held.from == at)
                open.push_back(held.to);
        }
    }
    return false;
}

LinkRefusal NodeCanvas::canLink(const CanvasLink& asked) const noexcept
{
    const CanvasNode* const from = node(asked.from);
    const CanvasNode* const to = node(asked.to);
    if (from == nullptr || to == nullptr || asked.output >= from->outputs.size() || asked.input >= to->inputs.size())
        return LinkRefusal::NoSuchPin;
    if (asked.from == asked.to && !m_rules.toItself)
        return LinkRefusal::ToItself;
    if (!pinAccepts(to->inputs[asked.input].type, from->outputs[asked.output].type))
        return LinkRefusal::TypesDoNotFit;
    if (std::find(m_links.begin(), m_links.end(), asked) != m_links.end())
        return LinkRefusal::AlreadyMade;
    // A value that came to depend on itself: what `to` feeds already reaches
    // `from`.
    if (!m_rules.cycles && asked.from != asked.to && reaches(asked.to, asked.from))
        return LinkRefusal::Cycle;
    return LinkRefusal::None;
}

LinkRefusal NodeCanvas::link(const CanvasLink& asked)
{
    const LinkRefusal refusal = canLink(asked);
    if (refusal != LinkRefusal::None)
        return refusal;
    if (!m_rules.manyIntoAnInput) {
        std::erase_if(m_links,
                      [&asked](const CanvasLink& held) { return held.to == asked.to && held.input == asked.input; });
    }
    m_links.push_back(asked);
    return LinkRefusal::None;
}

bool NodeCanvas::unlink(const CanvasLink& asked)
{
    const auto found = std::find(m_links.begin(), m_links.end(), asked);
    if (found == m_links.end())
        return false;
    m_links.erase(found);
    if (m_selectedLink == asked)
        m_selectedLink.reset();
    return true;
}

std::optional<CanvasLink> NodeCanvas::linkInto(u32 nodeId, u32 input) const noexcept
{
    for (const CanvasLink& held : m_links) {
        if (held.to == nodeId && held.input == input)
            return held;
    }
    return std::nullopt;
}

void NodeCanvas::clear()
{
    m_nodes.clear();
    m_links.clear();
    m_selection.clear();
    m_selectedLink.reset();
}

bool NodeCanvas::selected(u32 id) const noexcept
{
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void NodeCanvas::select(u32 id, bool adding)
{
    if (node(id) == nullptr)
        return;
    m_selectedLink.reset();
    if (!adding) {
        m_selection.assign(1, id);
        return;
    }
    // Added to a selection it is already in, it is taken out of it.
    if (selected(id))
        std::erase(m_selection, id);
    else
        m_selection.push_back(id);
}

void NodeCanvas::selectLink(const CanvasLink& asked)
{
    if (std::find(m_links.begin(), m_links.end(), asked) == m_links.end())
        return;
    m_selection.clear();
    m_selectedLink = asked;
}

u32 NodeCanvas::removeSelected()
{
    if (m_selectedLink.has_value())
        return unlink(*m_selectedLink) ? 1u : 0u;
    const std::vector<u32> going = m_selection;
    u32 removed = 0;
    for (const u32 id : going)
        removed += remove(id) ? 1u : 0u;
    return removed;
}

u32 NodeCanvas::duplicateSelected()
{
    const std::vector<u32> from = m_selection;
    std::vector<std::pair<u32, u32>> copies;
    for (const u32 id : from) {
        const CanvasNode* const held = node(id);
        if (held == nullptr)
            continue;
        CanvasNode copy = *held;
        copy.x += kCopyStep;
        copy.y += kCopyStep;
        copies.emplace_back(id, add(std::move(copy)));
    }
    const auto copyOf = [&copies](u32 id) -> std::optional<u32> {
        for (const auto& [was, is] : copies) {
            if (was == id)
                return is;
        }
        return std::nullopt;
    };
    // Links among what was copied come with it; a link to something that was
    // not copied stays where it was.
    const std::vector<CanvasLink> links = m_links;
    for (const CanvasLink& held : links) {
        const std::optional<u32> a = copyOf(held.from);
        const std::optional<u32> b = copyOf(held.to);
        if (a.has_value() && b.has_value())
            m_links.push_back({*a, held.output, *b, held.input});
    }
    m_selection.clear();
    m_selectedLink.reset();
    for (const auto& [was, is] : copies)
        m_selection.push_back(is);
    return static_cast<u32>(copies.size());
}

void NodeCanvas::commit()
{
    // The first is what there was to go back to.
    if (m_undo.empty()) {
        m_undo.push_back({m_nodes, m_links});
        return;
    }
    m_undo.push_back({m_nodes, m_links});
    m_redo.clear();
    ++m_edits;
}

void NodeCanvas::restore(const Snapshot& snapshot)
{
    m_nodes = snapshot.nodes;
    m_links = snapshot.links;
    // What is selected may be gone.
    std::erase_if(m_selection, [this](u32 id) { return node(id) == nullptr; });
    if (m_selectedLink.has_value() && std::find(m_links.begin(), m_links.end(), *m_selectedLink) == m_links.end())
        m_selectedLink.reset();
    for (const CanvasNode& held : m_nodes)
        m_nextId = std::max(m_nextId, held.id + 1);
}

bool NodeCanvas::undo()
{
    if (m_undo.size() < 2)
        return false;
    m_redo.push_back(std::move(m_undo.back()));
    m_undo.pop_back();
    restore(m_undo.back());
    --m_edits;
    return true;
}

bool NodeCanvas::redo()
{
    if (m_redo.empty())
        return false;
    m_undo.push_back(std::move(m_redo.back()));
    m_redo.pop_back();
    restore(m_undo.back());
    ++m_edits;
    return true;
}

void NodeCanvas::setView(CanvasPoint pan, f32 zoom) noexcept
{
    if (std::isfinite(pan.x) && std::isfinite(pan.y))
        m_pan = pan;
    if (std::isfinite(zoom))
        m_zoom = std::clamp(zoom, kLeastZoom, kMostZoom);
}

CanvasPoint NodeCanvas::toCanvas(f32 panelX, f32 panelY) const noexcept
{
    return {(panelX - m_pan.x) / m_zoom, (panelY - m_pan.y) / m_zoom};
}

CanvasPoint NodeCanvas::toPanel(f32 canvasX, f32 canvasY) const noexcept
{
    return {canvasX * m_zoom + m_pan.x, canvasY * m_zoom + m_pan.y};
}

void NodeCanvas::frameAll(f32 panelWidth, f32 panelHeight, const CanvasMetrics& metrics) noexcept
{
    if (m_nodes.empty() || panelWidth <= 0.0f || panelHeight <= 0.0f)
        return;
    f32 left = 0.0f;
    f32 top = 0.0f;
    f32 right = 0.0f;
    f32 bottom = 0.0f;
    bool first = true;
    for (const CanvasNode& held : m_nodes) {
        const CanvasRect rect = nodeRect(held, metrics);
        left = first ? rect.x : std::min(left, rect.x);
        top = first ? rect.y : std::min(top, rect.y);
        right = first ? rect.x + rect.width : std::max(right, rect.x + rect.width);
        bottom = first ? rect.y + rect.height : std::max(bottom, rect.y + rect.height);
        first = false;
    }
    // A margin of a row round them, and never magnified past one.
    const f32 margin = metrics.rowHeight * 2.0f;
    const f32 width = right - left + margin * 2.0f;
    const f32 height = bottom - top + margin * 2.0f;
    m_zoom = std::clamp(std::min(panelWidth / width, panelHeight / height), kLeastZoom, 1.0f);
    m_pan = {(panelWidth - (left + right) * m_zoom) * 0.5f, (panelHeight - (top + bottom) * m_zoom) * 0.5f};
}

std::optional<u32> NodeCanvas::nodeAt(f32 x, f32 y, const CanvasMetrics& metrics) const noexcept
{
    // The last drawn is on top.
    for (auto held = m_nodes.rbegin(); held != m_nodes.rend(); ++held) {
        if (nodeRect(*held, metrics).holds(x, y))
            return held->id;
    }
    return std::nullopt;
}

std::optional<NodeCanvas::PinAt> NodeCanvas::pinAt(f32 x, f32 y, const CanvasMetrics& metrics) const noexcept
{
    const f32 reach = metrics.pinReach * metrics.pinReach;
    for (auto held = m_nodes.rbegin(); held != m_nodes.rend(); ++held) {
        for (u32 pin = 0; pin < held->outputs.size(); ++pin) {
            const CanvasPoint at = outputPlace(*held, pin, metrics);
            if ((at.x - x) * (at.x - x) + (at.y - y) * (at.y - y) <= reach)
                return PinAt{held->id, pin, true};
        }
        for (u32 pin = 0; pin < held->inputs.size(); ++pin) {
            const CanvasPoint at = inputPlace(*held, pin, metrics);
            if ((at.x - x) * (at.x - x) + (at.y - y) * (at.y - y) <= reach)
                return PinAt{held->id, pin, false};
        }
    }
    return std::nullopt;
}

std::optional<u32> NodeCanvas::rimAt(f32 x, f32 y, const CanvasMetrics& metrics) const noexcept
{
    // A band a pin's reach and a half deep, most of it inside the box: the
    // part of a box a hand finds when it means the edge. (A band half that
    // was tried first, and a press meant for the edge moved the box.)
    const f32 out = metrics.pinReach * 0.6f;
    const f32 in = metrics.pinReach * 0.9f;
    for (auto held = m_nodes.rbegin(); held != m_nodes.rend(); ++held) {
        const CanvasRect rect = nodeRect(*held, metrics);
        const CanvasRect outer{rect.x - out, rect.y - out, rect.width + out * 2.0f, rect.height + out * 2.0f};
        const CanvasRect inner{rect.x + in, rect.y + in, rect.width - in * 2.0f, rect.height - in * 2.0f};
        if (inner.holds(x, y))
            return std::nullopt;
        if (outer.holds(x, y))
            return held->id;
    }
    return std::nullopt;
}

std::optional<CanvasLink> NodeCanvas::linkAt(f32 x, f32 y, const CanvasMetrics& metrics) const noexcept
{
    std::optional<CanvasLink> nearest;
    f32 least = metrics.pinReach;
    for (const CanvasLink& held : m_links) {
        const CanvasNode* const from = node(held.from);
        const CanvasNode* const to = node(held.to);
        if (from == nullptr || to == nullptr)
            continue;
        const CanvasLine line =
            m_rules.arrows ? arrowLine(*from, *to, metrics)
                           : CanvasLine{outputPlace(*from, held.output, metrics), inputPlace(*to, held.input, metrics)};
        const f32 distance = distanceToSegment({x, y}, line.from, line.to);
        if (distance <= least) {
            least = distance;
            nearest = held;
        }
    }
    return nearest;
}

CanvasEvent NodeCanvas::pointer(const CanvasPointer& at, const CanvasMetrics& metrics)
{
    CanvasEvent event;
    const CanvasPoint here = toCanvas(at.x, at.y);

    // The wheel zooms about the pointer: what is under it stays under it.
    if (at.wheel != 0.0f) {
        const f32 zoom = std::clamp(m_zoom * std::pow(1.1f, at.wheel), kLeastZoom, kMostZoom);
        m_pan = {at.x - here.x * zoom, at.y - here.y * zoom};
        m_zoom = zoom;
    }

    // The view, dragged by its own button.
    if (at.panning) {
        if (m_dragging.kind == Dragging::Kind::View) {
            m_pan.x += at.x - m_lastPanel.x;
            m_pan.y += at.y - m_lastPanel.y;
        }
        else if (m_dragging.kind == Dragging::Kind::Nothing) {
            m_dragging = {};
            m_dragging.kind = Dragging::Kind::View;
        }
        m_lastPanel = {at.x, at.y};
        return event;
    }
    if (m_dragging.kind == Dragging::Kind::View)
        m_dragging = {};

    if (at.pressed) {
        m_dragging = {};
        m_dragging.from = here;
        m_dragging.to = here;
        m_moved = false;
        // Where arrows are, a box's rim is what a link is drawn out of; a
        // box that nothing leaves -- it has no output -- has no such rim.
        const std::optional<u32> rim = m_rules.arrows ? rimAt(here.x, here.y, metrics) : std::nullopt;
        const std::optional<PinAt> pin = m_rules.arrows ? std::nullopt : pinAt(here.x, here.y, metrics);
        if (m_locked && (rim.has_value() || pin.has_value())) {
            // A pin of a graph that is only looked at is part of its node.
            if (const std::optional<u32> hit = nodeAt(here.x, here.y, metrics); hit.has_value()) {
                select(*hit, at.adding);
                event.kind = CanvasEvent::Kind::SelectionChanged;
            }
            m_dragging = {};
        }
        else if (rim.has_value() && !node(*rim)->outputs.empty()) {
            m_dragging.kind = Dragging::Kind::Link;
            m_dragging.node = *rim;
            m_dragging.pin = 0;
            m_dragging.fromOutput = true;
        }
        else if (pin.has_value()) {
            m_dragging.kind = Dragging::Kind::Link;
            m_dragging.node = pin->node;
            m_dragging.pin = pin->pin;
            m_dragging.fromOutput = pin->output;
        }
        else if (const std::optional<u32> hit = nodeAt(here.x, here.y, metrics); hit.has_value()) {
            // A press on one of several selected keeps them, to be dragged
            // together; a press on another selects it.
            if (at.adding || !selected(*hit)) {
                select(*hit, at.adding);
                event.kind = CanvasEvent::Kind::SelectionChanged;
            }
            // The node pressed is drawn over the others.
            const auto held = std::find_if(m_nodes.begin(), m_nodes.end(),
                                           [&hit](const CanvasNode& node) { return node.id == *hit; });
            std::rotate(held, held + 1, m_nodes.end());
            m_dragging.kind = Dragging::Kind::Nodes;
            m_dragStart.clear();
            for (const u32 id : m_selection) {
                if (const CanvasNode* const dragged = node(id); dragged != nullptr)
                    m_dragStart.emplace_back(id, CanvasPoint{dragged->x, dragged->y});
            }
        }
        else if (const std::optional<CanvasLink> line = linkAt(here.x, here.y, metrics); line.has_value()) {
            selectLink(*line);
            event.kind = CanvasEvent::Kind::SelectionChanged;
        }
        else {
            m_dragging.kind = Dragging::Kind::Box;
            if (!at.adding && (!m_selection.empty() || m_selectedLink.has_value())) {
                m_selection.clear();
                m_selectedLink.reset();
                event.kind = CanvasEvent::Kind::SelectionChanged;
            }
        }
        return event;
    }

    if (at.down && m_dragging.kind != Dragging::Kind::Nothing) {
        m_dragging.to = here;
        const f32 dx = here.x - m_dragging.from.x;
        const f32 dy = here.y - m_dragging.from.y;
        if (!m_moved && (std::abs(dx) + std::abs(dy)) * m_zoom >= kDragSlack)
            m_moved = true;
        if (m_dragging.kind == Dragging::Kind::Nodes && m_moved && !m_locked) {
            for (const auto& [id, from] : m_dragStart)
                (void)move(id, from.x + dx, from.y + dy);
        }
        return event;
    }

    if (at.released) {
        const Dragging was = m_dragging;
        m_dragging = {};
        switch (was.kind) {
        case Dragging::Kind::Nodes:
            if (m_moved && !m_locked) {
                event.kind = CanvasEvent::Kind::NodesMoved;
                commit();
            }
            break;
        case Dragging::Kind::Link: {
            // An arrow lands on the box it is let go over, anywhere on it.
            std::optional<PinAt> pin = m_rules.arrows ? std::nullopt : pinAt(here.x, here.y, metrics);
            if (m_rules.arrows) {
                std::optional<u32> over = nodeAt(here.x, here.y, metrics);
                if (!over.has_value())
                    over = rimAt(here.x, here.y, metrics);
                // Let go on the box it left: a press on a rim, and nothing
                // more.
                if (over.has_value() && *over == was.node && !m_rules.toItself)
                    break;
                if (over.has_value())
                    pin = PinAt{*over, 0, false};
            }
            // Let go over nothing, or over a pin of the same side: for
            // whoever owns the graph to offer what could be put there.
            if (!pin.has_value() || pin->output == was.fromOutput) {
                if (!m_moved)
                    break;
                event.kind = CanvasEvent::Kind::LinkDropped;
                event.link = was.fromOutput ? CanvasLink{was.node, was.pin, 0, 0} : CanvasLink{0, 0, was.node, was.pin};
                event.fromOutput = was.fromOutput;
                event.at = here;
                break;
            }
            const CanvasLink made = was.fromOutput ? CanvasLink{was.node, was.pin, pin->node, pin->pin}
                                                   : CanvasLink{pin->node, pin->pin, was.node, was.pin};
            event.link = made;
            event.refusal = link(made);
            event.kind =
                event.refusal == LinkRefusal::None ? CanvasEvent::Kind::LinkMade : CanvasEvent::Kind::LinkRefused;
            if (event.refusal == LinkRefusal::None)
                commit();
            break;
        }
        case Dragging::Kind::Box: {
            const f32 left = std::min(was.from.x, here.x);
            const f32 right = std::max(was.from.x, here.x);
            const f32 top = std::min(was.from.y, here.y);
            const f32 bottom = std::max(was.from.y, here.y);
            bool changed = false;
            for (const CanvasNode& held : m_nodes) {
                const CanvasRect rect = nodeRect(held, metrics);
                const bool touched =
                    rect.x < right && rect.x + rect.width > left && rect.y < bottom && rect.y + rect.height > top;
                if (touched && !selected(held.id)) {
                    m_selection.push_back(held.id);
                    changed = true;
                }
            }
            if (changed)
                event.kind = CanvasEvent::Kind::SelectionChanged;
            break;
        }
        case Dragging::Kind::Nothing:
        case Dragging::Kind::View:
            break;
        }
    }
    return event;
}

} // namespace engine::app
