// The Water tool (ADR 0146 section 7; the water ledger's W5 and W1).
//
// **Water is drawn where it goes.** A river is clicked along its course and
// runs down the ground it was clicked on, a lake is clicked round its shore,
// a pool is dragged as a rectangle, a sea is clicked at the height it comes up
// to. Until this a river was a `Water` and a child a point, made in the
// Explorer and typed into Properties a coordinate at a time -- which is how
// nobody finds out what a river looks like.
//
// It is `driveTiles`' shape: the tool has the pointer while its panel is open,
// a gesture is one undo step, and what it is aiming at is published for the
// frame to draw (`WaterGuide`).

#include <algorithm>
#include <array>
#include <cmath>
#include <engine/app/editor.h>
#include <engine/app/inspector.h>
#include <engine/app/picking.h>
#include <engine/core/i18n.h>
#include <engine/core/text_key.h>
#include <engine/render/debug_draw.h>
#include <engine/scene/class_registry.h>
#include <engine/scene/water.h>
#include <engine/scene/world.h>
#include <optional>
#include <string_view>
#include <vector>

namespace engine::app {

namespace {

namespace shape = scene::water_shape;

// How near the pointer has to be to a handle to have it, in pixels.
constexpr core::f32 HandleReachPixels = 12.0f;
// How far the pointer moves before a press on a handle is a drag: under it the
// press was a click, and a click on a point selects the point.
constexpr core::f32 DragStartPixels = 3.0f;
// The smallest pool a drag makes, a side, in metres: under it the drag was a
// click.
constexpr core::f64 SmallestPool = 1.0;
// How far above the ground it was clicked on a river's surface lies there: on
// the ground exactly it would be under it wherever the ground is not flat.
constexpr core::f64 RiverRise = 0.25;
// With nothing solid under the pointer and the world's zero not ahead of it,
// water is drawn on a level this far under the eye, in metres.
constexpr core::f64 EyeDrop = 10.0;
// Past this a ray has met nothing a person aimed at.
constexpr core::f64 Reach = 4096.0;

[[nodiscard]] std::optional<core::DVec3> onPlane(const PickRay& ray, core::f64 height) noexcept
{
    const auto down = static_cast<core::f64>(ray.direction.y);
    if (std::abs(down) < 1e-6)
        return std::nullopt;
    const core::f64 along = (height - ray.origin.y) / down;
    if (!(along > 0.0) || along > Reach)
        return std::nullopt;
    return core::DVec3{ray.origin.x + static_cast<core::f64>(ray.direction.x) * along, height,
                       ray.origin.z + static_cast<core::f64>(ray.direction.z) * along};
}

// Where the pointer meets something solid: the ground or a part, whichever is
// nearer. Not water -- a river is drawn across the ground under another's
// surface, not on it.
[[nodiscard]] std::optional<core::DVec3> onSolid(const scene::World& world, core::InstanceId root, const PickRay& ray)
{
    std::optional<PickHit> hit = pickNearest(world, root, ray);
    if (const std::optional<PickHit> ground = pickGround(world, root, ray, Reach);
        ground.has_value() && (!hit.has_value() || ground->distance < hit->distance)) {
        hit = ground;
    }
    if (!hit.has_value())
        return std::nullopt;
    const auto along = static_cast<core::f64>(hit->distance);
    return core::DVec3{ray.origin.x + static_cast<core::f64>(ray.direction.x) * along,
                       ray.origin.y + static_cast<core::f64>(ray.direction.y) * along,
                       ray.origin.z + static_cast<core::f64>(ray.direction.z) * along};
}

[[nodiscard]] std::vector<core::InstanceId> pointsOf(const scene::World& world, core::InstanceId water)
{
    std::vector<core::InstanceId> points;
    for (core::InstanceId child = world.firstChild(water); child.valid(); child = world.nextSibling(child)) {
        if (world.waterPoints().find(child) != nullptr)
            points.push_back(child);
    }
    return points;
}

void write(scene::World& world, core::InstanceId id, std::string_view property, const scene::Value& value)
{
    (void)world.setProperty(id, world.atoms().intern(property), value);
}

void place(scene::World& world, core::InstanceId point, const core::DVec3& at)
{
    write(world, point, "Position",
          scene::Value{
              core::Vec3{static_cast<core::f32>(at.x), static_cast<core::f32>(at.y), static_cast<core::f32>(at.z)}});
}

// A new instance of `className` under `parent`, named for what it is; invalid
// when the class is not there or the parent refuses it.
[[nodiscard]] core::InstanceId make(scene::World& world, std::string_view className, std::string_view name,
                                    core::InstanceId parent)
{
    const scene::ClassId classId = world.classes().findId(world.atoms().intern(className));
    if (classId == scene::InvalidClass)
        return {};
    const core::InstanceId id = world.create(classId);
    if (!id.valid())
        return {};
    world.setName(id, world.atoms().intern(name));
    if (world.setParent(id, parent).has_value()) {
        (void)world.destroy(id);
        return {};
    }
    return id;
}

[[nodiscard]] scene::Value shapeValue(const scene::World& world, core::i32 value)
{
    return scene::Value{scene::EnumValue{world.enums().findId(world.atoms().lookup("WaterShape")), value}};
}

[[nodiscard]] core::InstanceId seaIn(const scene::World& world, core::InstanceId root)
{
    core::InstanceId found;
    world.waters().forEach([&](core::InstanceId id, const scene::WaterComponent& water) {
        if (!found.valid() && water.shape == shape::Ocean && world.isAncestorOf(root, id))
            found = id;
    });
    return found;
}

// A pool's `Position` and `Size` from two opposite corners of it.
void layRectangle(scene::World& world, core::InstanceId pool, const core::DVec3& from, const core::DVec3& to)
{
    const scene::WaterComponent* water = world.waters().find(pool);
    if (water == nullptr)
        return;
    const core::f64 width = std::max(std::abs(to.x - from.x), SmallestPool);
    const core::f64 length = std::max(std::abs(to.z - from.z), SmallestPool);
    const core::Vec3 position{static_cast<core::f32>((from.x + to.x) * 0.5), water->position.y,
                              static_cast<core::f32>((from.z + to.z) * 0.5)};
    const core::Vec3 size{static_cast<core::f32>(width), water->size.y, static_cast<core::f32>(length)};
    write(world, pool, "Position", scene::Value{position});
    write(world, pool, "Size", scene::Value{size});
}

} // namespace

void Editor::setWaterOp(WaterOp op) noexcept
{
    // Refused mid-gesture, as `setTool` is mid-stroke.
    if (m_waterGesture.has_value())
        return;
    m_waterOp = op;
    m_preferencesDirty = true;
}

void Editor::setWaterWidth(f32 metres) noexcept
{
    m_waterWidth = std::clamp(metres, 0.5f, 512.0f);
    m_preferencesDirty = true;
}

void Editor::setWaterDepth(f32 metres) noexcept
{
    m_waterDepth = std::clamp(metres, 0.25f, 512.0f);
    m_preferencesDirty = true;
}

core::InstanceId Editor::waterInHand(const scene::World& world, const Inspector& inspector) noexcept
{
    const core::InstanceId selected = inspector.selection();
    if (!selected.valid() || !world.alive(selected))
        return {};
    if (world.waters().find(selected) != nullptr)
        return selected;
    if (world.waterPoints().find(selected) != nullptr) {
        const core::InstanceId parent = world.parentOf(selected);
        if (parent.valid() && world.waters().find(parent) != nullptr)
            return parent;
    }
    return {};
}

void Editor::finishRiver(Inspector& inspector) noexcept
{
    if (m_waterGesture.has_value())
        return;
    inspector.select(core::InstanceId{});
}

core::u64 Editor::carveWater(scene::World& world, core::InstanceId root, Inspector& inspector)
{
    const core::InstanceId inHand = waterInHand(world, inspector);
    if (!inHand.valid() || m_waterGesture.has_value())
        return 0;
    std::vector<core::InstanceId> terrains;
    world.terrains().forEach([&](core::InstanceId id, const scene::TerrainComponent&) {
        if (!world.destroyed(id) && world.isAncestorOf(root, id))
            terrains.push_back(id);
    });
    // The world as it was, kept only if something is cut: a carve that finds
    // the ground already lower leaves nothing to undo.
    scene::WorldSnapshot before = world.snapshot();
    core::u64 lowered = 0;
    for (const core::InstanceId terrain : terrains)
        lowered += scene::carveWaterBed(world, inHand, terrain);
    if (lowered == 0) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.water_nothing_to_carve")), false};
        return 0;
    }
    m_history.record(std::move(before), core::tr(ENG_TR("engine.editor.history.carve_water")));
    m_sceneDirty = true;
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.status.water_carved"), {{"count", static_cast<core::i64>(lowered)}}), false};
    return lowered;
}

bool Editor::driveWater(scene::World& world, core::InstanceId root, Inspector& inspector)
{
    m_waterGuide = WaterGuide{};
    if (m_tool != Tool::Water || !m_waterPanelShown || !root.valid() || !world.alive(root)) {
        m_waterGesture.reset();
        return false;
    }
    // **A gesture is on the water it began on.** That one gone under it -- an
    // undo, a delete in the Explorer -- the gesture is over.
    if (m_waterGesture.has_value() && m_waterGesture->water.valid() && !world.alive(m_waterGesture->water))
        m_waterGesture.reset();

    const core::InstanceId inHand = waterInHand(world, inspector);
    const scene::WaterComponent* held = inHand.valid() ? world.waters().find(inHand) : nullptr;
    const core::i32 heldShape = held != nullptr ? held->shape : -1;
    const bool river = m_waterOp == WaterOp::River && scene::waterIsRiver(heldShape);
    const bool lake = m_waterOp == WaterOp::Lake && heldShape == shape::Lake;
    const bool pool = m_waterOp == WaterOp::Pool && scene::waterIsPool(heldShape);
    // Whether its points have heights of their own: a `River`'s do.
    const bool rises = river && heldShape == shape::River;
    const core::f64 heldLevel = held != nullptr ? held->surfaceLevel : 0.0;

    // **The handles**, pointer or no pointer: they are what says which water
    // is in hand. A river's points in their order, a lake's round its shore,
    // a pool's four corners. On the surface, which is where the water is seen.
    std::vector<core::InstanceId> points;
    if (river || lake) {
        points = pointsOf(world, inHand);
        for (const core::InstanceId point : points) {
            const core::Vec3 at = world.waterPoints().find(point)->position;
            m_waterGuide.handles.push_back(core::DVec3{static_cast<core::f64>(at.x),
                                                       rises ? static_cast<core::f64>(at.y) : heldLevel,
                                                       static_cast<core::f64>(at.z)});
        }
        m_waterGuide.closed = lake && points.size() >= 3;
        if (river)
            m_waterGuide.width = held->size.x;
    }
    else if (pool) {
        const auto x = static_cast<core::f64>(held->position.x);
        const auto z = static_cast<core::f64>(held->position.z);
        const core::f64 halfX = static_cast<core::f64>(held->size.x) * 0.5;
        const core::f64 halfZ = static_cast<core::f64>(held->size.z) * 0.5;
        m_waterGuide.handles = {
            core::DVec3{x - halfX, heldLevel, z - halfZ}, core::DVec3{x + halfX, heldLevel, z - halfZ},
            core::DVec3{x + halfX, heldLevel, z + halfZ}, core::DVec3{x - halfX, heldLevel, z + halfZ}};
        m_waterGuide.closed = true;
    }
    if (m_waterOp == WaterOp::River && !river)
        m_waterGuide.width = m_waterWidth;

    const PickRay ray = rayThrough(m_pointer);

    // --- A gesture under way ------------------------------------------------
    if (m_waterGesture.has_value()) {
        if (!m_pointerDown) {
            const WaterGesture ended = *m_waterGesture;
            m_waterGesture.reset();
            m_pending.reset();
            if (ended.kind == WaterGesture::Kind::Rectangle) {
                const std::optional<core::DVec3> to = onPlane(ray, ended.level);
                if (to.has_value() && std::abs(to->x - ended.anchor.x) >= SmallestPool &&
                    std::abs(to->z - ended.anchor.z) >= SmallestPool) {
                    m_history.record(world, core::tr(ENG_TR("engine.editor.history.draw_pool")));
                    const core::InstanceId made = make(world, "Water", "Pool", root);
                    if (made.valid()) {
                        write(world, made, "Shape", shapeValue(world, shape::Pool));
                        write(world, made, "SurfaceLevel", scene::Value{ended.level});
                        write(world, made, "Size", scene::Value{core::Vec3{1.0f, m_waterDepth, 1.0f}});
                        layRectangle(world, made, ended.anchor, *to);
                        inspector.select(made);
                        inspector.reveal(made);
                        m_sceneDirty = true;
                    }
                }
                // **A drag that went nowhere was a click**, and a click on
                // water picks it up: its corners come out to be dragged.
                else if (const std::optional<PickHit> hit = pickWater(world, root, ray); hit.has_value()) {
                    inspector.select(hit->instance);
                    inspector.reveal(hit->instance);
                }
            }
            // **A click on a point selects it**: Delete then removes it, and
            // the next click still adds to its water (`waterInHand`).
            else if (ended.kind == WaterGesture::Kind::Point && !ended.moved && world.alive(ended.point)) {
                inspector.select(ended.point);
                inspector.reveal(ended.point);
            }
            return true;
        }

        WaterGesture& gesture = *m_waterGesture;
        const core::Vec2 travelled{m_pointer.x - gesture.pressedAt.x, m_pointer.y - gesture.pressedAt.y};
        const bool dragging =
            gesture.moved || std::sqrt(travelled.x * travelled.x + travelled.y * travelled.y) >= DragStartPixels;
        switch (gesture.kind) {
        case WaterGesture::Kind::Click:
            break;
        case WaterGesture::Kind::Rectangle:
            if (const std::optional<core::DVec3> to = onPlane(ray, gesture.level); to.has_value())
                m_waterGuide.rectangle = std::array<core::DVec3, 2>{gesture.anchor, *to};
            break;
        case WaterGesture::Kind::Point: {
            m_waterGuide.hot = gesture.index;
            if (!dragging || !world.alive(gesture.point))
                break;
            // Across the ground where there is ground under the pointer -- and
            // a river's point takes the ground's height there -- and across
            // the point's own level where there is none.
            core::DVec3 to;
            if (const std::optional<core::DVec3> solid = onSolid(world, root, ray); solid.has_value()) {
                to = core::DVec3{solid->x, gesture.rises ? solid->y + RiverRise : gesture.level, solid->z};
            }
            else if (const std::optional<core::DVec3> level = onPlane(ray, gesture.level); level.has_value()) {
                to = *level;
            }
            else {
                break;
            }
            if (!gesture.moved) {
                // Recorded with the first movement and not with the press: a
                // press that was a click leaves nothing to undo.
                m_history.record(world, core::tr(gesture.outline ? ENG_TR("engine.editor.history.move_lake_point")
                                                                 : ENG_TR("engine.editor.history.move_river_point")));
                gesture.moved = true;
            }
            place(world, gesture.point, to);
            if (gesture.index >= 0 && static_cast<core::usize>(gesture.index) < m_waterGuide.handles.size())
                m_waterGuide.handles[static_cast<core::usize>(gesture.index)] = to;
            m_sceneDirty = true;
            break;
        }
        case WaterGesture::Kind::Corner: {
            m_waterGuide.hot = gesture.index;
            if (!dragging)
                break;
            const std::optional<core::DVec3> to = onPlane(ray, gesture.level);
            if (!to.has_value())
                break;
            if (!gesture.moved) {
                m_history.record(world, core::tr(ENG_TR("engine.editor.history.resize_pool")));
                gesture.moved = true;
            }
            layRectangle(world, gesture.water, gesture.anchor, *to);
            m_sceneDirty = true;
            break;
        }
        }
        return true;
    }

    if (!m_pointerOverViewport)
        return false;

    // --- The handle under the pointer ---------------------------------------
    const ViewportRect local{0.0f, 0.0f, m_viewport.width, m_viewport.height};
    core::f32 nearest = HandleReachPixels;
    for (core::usize index = 0; index < m_waterGuide.handles.size(); ++index) {
        const std::optional<core::Vec2> pixel =
            worldToViewport(m_projection, m_view, m_cameraOrigin, local, m_waterGuide.handles[index]);
        if (!pixel.has_value())
            continue;
        const core::f32 dx = pixel->x - m_pointer.x;
        const core::f32 dy = pixel->y - m_pointer.y;
        const core::f32 distance = std::sqrt(dx * dx + dy * dy);
        if (distance < nearest) {
            nearest = distance;
            m_waterGuide.hot = static_cast<core::i32>(index);
        }
    }

    // --- Where a click would land -------------------------------------------
    //
    // On something solid; where there is nothing solid, on the level of the
    // water in hand -- a river's last point's -- or the world's zero with
    // none: an empty scene has to be somewhere a first river can go. **And
    // where zero is not ahead**, a level under the eye: a new project's camera
    // stands AT zero, looking at the horizon, and the first click of a first
    // river landed nowhere.
    const core::f64 lastLevel =
        rises && !m_waterGuide.handles.empty() ? m_waterGuide.handles.back().y : (held != nullptr ? heldLevel : 0.0);
    std::optional<core::DVec3> aim = onSolid(world, root, ray);
    const bool onGround = aim.has_value();
    if (!aim.has_value())
        aim = onPlane(ray, lastLevel);
    if (!aim.has_value() && held == nullptr)
        aim = onPlane(ray, std::floor(ray.origin.y) - EyeDrop);
    // Where the point a click lays would be: a river's a little over the
    // ground it was clicked on, a lake's on its level.
    const auto pointFor = [&](const core::DVec3& at) {
        // A lake's first point is where it was clicked: that is its level.
        if (m_waterOp == WaterOp::Lake)
            return core::DVec3{at.x, lake ? heldLevel : at.y, at.z};
        if (river && !rises)
            return core::DVec3{at.x, heldLevel, at.z};
        return core::DVec3{at.x, onGround ? at.y + RiverRise : at.y, at.z};
    };
    if (m_waterGuide.hot < 0 && aim.has_value()) {
        const bool lays = (m_waterOp == WaterOp::River) || (m_waterOp == WaterOp::Lake);
        m_waterGuide.aim = lays ? pointFor(*aim) : *aim;
        if ((river || lake) && !m_waterGuide.handles.empty()) {
            m_waterGuide.from = m_waterGuide.handles.back();
            // A lake's outline comes back round to where it began.
            if (lake && m_waterGuide.handles.size() >= 2)
                m_waterGuide.closing = m_waterGuide.handles.front();
        }
    }

    if (!m_pointerPressed)
        return false;

    // --- A press ------------------------------------------------------------
    if (m_waterGuide.hot >= 0) {
        const auto index = static_cast<core::usize>(m_waterGuide.hot);
        WaterGesture gesture;
        gesture.water = inHand;
        gesture.index = m_waterGuide.hot;
        gesture.pressedAt = m_pointer;
        gesture.level = m_waterGuide.handles[index].y;
        if (river || lake) {
            gesture.kind = WaterGesture::Kind::Point;
            gesture.point = points[index];
            gesture.rises = rises;
            gesture.outline = lake;
        }
        else {
            gesture.kind = WaterGesture::Kind::Corner;
            // The corner across from the one in hand stays where it is.
            gesture.anchor = m_waterGuide.handles[(index + 2) % 4];
        }
        m_waterGesture = gesture;
        m_pending.reset();
        return true;
    }

    // **A tool with nowhere to act does not eat the click**, for the reason
    // `driveSculpt` gives: a ray into the sky is a click on nothing.
    if (!aim.has_value())
        return false;

    WaterGesture gesture;
    gesture.kind = WaterGesture::Kind::Click;
    gesture.pressedAt = m_pointer;

    // No water of the kind in hand, and the click is on one: that one is
    // picked up, and the next click adds to it. Starting a second river in
    // the first one's bed is what nobody means.
    const auto pickedUp = [&](bool wanted(core::i32)) {
        const std::optional<PickHit> hit = pickWater(world, root, ray);
        if (!hit.has_value())
            return false;
        const scene::WaterComponent* under = world.waters().find(hit->instance);
        if (under == nullptr || !wanted(under->shape))
            return false;
        inspector.select(hit->instance);
        inspector.reveal(hit->instance);
        return true;
    };

    switch (m_waterOp) {
    case WaterOp::River: {
        if (river) {
            m_history.record(world, core::tr(ENG_TR("engine.editor.history.add_river_point")));
            const core::InstanceId point = make(world, "WaterPoint", "WaterPoint", inHand);
            if (point.valid()) {
                place(world, point, pointFor(*aim));
                // The river stays in hand: with a point selected a second
                // Delete would take the point just laid.
                inspector.select(inHand);
                m_sceneDirty = true;
            }
            break;
        }
        if (pickedUp(scene::waterIsRiver))
            break;
        m_history.record(world, core::tr(ENG_TR("engine.editor.history.draw_river")));
        const core::InstanceId made = make(world, "Water", "River", root);
        if (!made.valid())
            break;
        const core::DVec3 first = pointFor(*aim);
        write(world, made, "Shape", shapeValue(world, shape::River));
        write(world, made, "SurfaceLevel", scene::Value{first.y});
        write(world, made, "Size", scene::Value{core::Vec3{m_waterWidth, m_waterDepth, m_waterWidth}});
        if (const core::InstanceId point = make(world, "WaterPoint", "WaterPoint", made); point.valid())
            place(world, point, first);
        inspector.select(made);
        inspector.reveal(made);
        m_sceneDirty = true;
        break;
    }
    case WaterOp::Lake: {
        if (lake) {
            m_history.record(world, core::tr(ENG_TR("engine.editor.history.add_lake_point")));
            const core::InstanceId point = make(world, "WaterPoint", "WaterPoint", inHand);
            if (point.valid()) {
                place(world, point, pointFor(*aim));
                inspector.select(inHand);
                m_sceneDirty = true;
            }
            break;
        }
        if (pickedUp([](core::i32 found) { return found == scene::water_shape::Lake; }))
            break;
        // Level at the height of its first click: click the shore, at the
        // height the water should come to, and on round it.
        m_history.record(world, core::tr(ENG_TR("engine.editor.history.draw_lake")));
        const core::InstanceId made = make(world, "Water", "Lake", root);
        if (!made.valid())
            break;
        write(world, made, "Shape", shapeValue(world, shape::Lake));
        write(world, made, "SurfaceLevel", scene::Value{aim->y});
        write(world, made, "Size", scene::Value{core::Vec3{1.0f, m_waterDepth, 1.0f}});
        if (const core::InstanceId point = make(world, "WaterPoint", "WaterPoint", made); point.valid())
            place(world, point, *aim);
        inspector.select(made);
        inspector.reveal(made);
        m_sceneDirty = true;
        break;
    }
    case WaterOp::Pool:
        // Level at the height the drag began at: press on the edge, at the
        // height the water should come to, and drag across.
        gesture.kind = WaterGesture::Kind::Rectangle;
        gesture.anchor = *aim;
        gesture.level = aim->y;
        break;
    case WaterOp::Ocean: {
        // **A world has one sea.** A second click moves it: two oceans at two
        // heights is a surface inside a surface.
        if (const core::InstanceId sea = seaIn(world, root); sea.valid()) {
            m_history.record(world, core::tr(ENG_TR("engine.editor.history.set_sea_level")));
            write(world, sea, "SurfaceLevel", scene::Value{aim->y});
            inspector.select(sea);
            inspector.reveal(sea);
            m_sceneDirty = true;
            break;
        }
        m_history.record(world, core::tr(ENG_TR("engine.editor.history.make_ocean")));
        const core::InstanceId made = make(world, "Water", "Ocean", root);
        if (!made.valid())
            break;
        write(world, made, "Shape", shapeValue(world, shape::Ocean));
        write(world, made, "SurfaceLevel", scene::Value{aim->y});
        inspector.select(made);
        inspector.reveal(made);
        m_sceneDirty = true;
        break;
    }
    }

    m_waterGesture = gesture;
    m_pending.reset();
    return true;
}

void submitWaterGuide(const Editor::WaterGuide& guide, core::DVec3 cameraOrigin, render::DebugDraw& draw)
{
    const render::DebugColor water = render::DebugColor::fromLinear(0.25f, 0.7f, 1.0f, 1.0f);
    const render::DebugColor idle = render::DebugColor::fromLinear(1.0f, 1.0f, 1.0f, 1.0f);
    const render::DebugColor hot = render::DebugColor::fromLinear(0.95f, 0.75f, 0.25f, 1.0f);

    // Camera-relative, like everything else the frame's overlay draws.
    const auto local = [&cameraOrigin](const core::DVec3& point) {
        return core::Vec3{static_cast<core::f32>(point.x - cameraOrigin.x),
                          static_cast<core::f32>(point.y - cameraOrigin.y),
                          static_cast<core::f32>(point.z - cameraOrigin.z)};
    };
    // A box that is the same size on screen wherever it is: a point across the
    // valley has to be as easy to see, and to take, as the one underfoot.
    const auto handle = [&](const core::DVec3& point, render::DebugColor color) {
        const core::Vec3 at = local(point);
        const core::f32 half = std::max(0.08f, core::length(at) * 0.008f);
        draw.wireBox(at, core::Vec3{half, half, half}, color);
    };

    const core::usize count = guide.handles.size();
    for (core::usize index = 0; index + 1 < count; ++index)
        draw.line(local(guide.handles[index]), local(guide.handles[index + 1]), water);
    if (guide.closed && count > 2)
        draw.line(local(guide.handles[count - 1]), local(guide.handles[0]), water);
    for (core::usize index = 0; index < count; ++index)
        handle(guide.handles[index], static_cast<core::i32>(index) == guide.hot ? hot : idle);

    if (guide.aim.has_value()) {
        if (guide.from.has_value()) {
            // The stretch a click would add, as wide as the river is.
            const core::Vec3 from = local(*guide.from);
            const core::Vec3 to = local(*guide.aim);
            draw.line(from, to, hot);
            const core::Vec3 along{to.x - from.x, 0.0f, to.z - from.z};
            const core::f32 span = std::sqrt(along.x * along.x + along.z * along.z);
            if (span > 1e-3f && guide.width > 0.0f) {
                const core::f32 half = guide.width * 0.5f / span;
                const core::Vec3 side{-along.z * half, 0.0f, along.x * half};
                draw.line(core::Vec3{from.x + side.x, from.y, from.z + side.z},
                          core::Vec3{to.x + side.x, to.y, to.z + side.z}, hot);
                draw.line(core::Vec3{from.x - side.x, from.y, from.z - side.z},
                          core::Vec3{to.x - side.x, to.y, to.z - side.z}, hot);
            }
        }
        // A lake's outline back to where it began.
        if (guide.closing.has_value())
            draw.line(local(*guide.aim), local(*guide.closing), hot);
        handle(*guide.aim, hot);
    }

    if (guide.rectangle.has_value()) {
        const core::DVec3& a = (*guide.rectangle)[0];
        const core::DVec3& b = (*guide.rectangle)[1];
        const core::Vec3 corners[4] = {local(a), local(core::DVec3{b.x, a.y, a.z}), local(core::DVec3{b.x, a.y, b.z}),
                                       local(core::DVec3{a.x, a.y, b.z})};
        for (int edge = 0; edge < 4; ++edge)
            draw.line(corners[edge], corners[(edge + 1) % 4], hot);
    }
}

} // namespace engine::app
