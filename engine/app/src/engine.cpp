#include "engine/app/engine.h"

#include "engine/app/editor_drive.h"
#include "engine/app/match_launcher.h"
#include "engine/app/network_session.h"
#include "engine/app/script_editor.h"
#include "engine/core/brand.h"
#include "engine/core/content_path.h"
#if ENG_DEBUG_UI
#include "engine/app/surface_compiler.h"
#else
#include "engine/render/pack_surface_source.h"
#endif
#include <lua.h>

#include "engine/script/debugger.h"

// `std::sort` for the frame-time median, and `std::ptrdiff_t` beside it. Both
// were reached transitively for a milestone: this file compiles on Windows and
// in the Tier-2 container without either include, and fails on the CI runner's
// libstdc++, which is a different version with a different transitive graph.
// A header a translation unit uses is a header it includes.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/app/backends.h"
#include "engine/app/brush_overlay.h"
#include "engine/app/chunk_overlay.h"
#include "engine/app/content_import.h"
#include "engine/app/debug_overlay.h"
#include "engine/app/dev_control.h"
#include "engine/app/editor.h"
#include "engine/app/field_streamer.h"
#include "engine/app/frame_scheduler.h"
#include "engine/app/icons.h"
#include "engine/app/inspector.h"
#include "engine/app/launcher.h"
#include "engine/app/partition_cache.h"
#include "engine/app/picking.h"
#include "engine/app/pointer_ownership.h"
#include "engine/app/preview_renderer.h"
#include "engine/app/reference_grid.h"
#include "engine/app/reload.h"
#include "engine/app/scene_definitions.h"
#include "engine/app/screenshot.h"
#include "engine/app/script_files.h"
#include "engine/app/skeleton_overlay.h"
#include "engine/app/soak.h"
#include "engine/app/streaming_host.h"
#include "engine/app/terrain_cells.h"
#include "engine/app/terrain_overlay.h"
#include "engine/app/text_input_focus.h"
#include "engine/app/thumbnails.h"
#include "engine/app/ui_text.h"
#include "engine/app/view_host.h"
#include "engine/app/world_host.h"
#include "engine/app/world_ui.h"
#include "engine/asset/content.h"
#include "engine/asset/image.h"
#include "engine/core/build_info.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/core/text_key.h"
#include "engine/jobs/jobs.h"
#include "engine/platform/event.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/platform/stop_signal.h"
#include "engine/platform/window.h"
#include "engine/render/debug_draw.h"
#include "engine/render/debug_renderer.h"
#include "engine/render/foliage.h"
#include "engine/render/lighting.h"
#include "engine/render/mesh_loader.h"
#include "engine/render/particles.h"
#include "engine/render/render_world.h"
#include "engine/render/renderer.h"
#include "engine/render/shader_library.h"
#include "engine/render/sky_loader.h"
#include "engine/render/terrain_loader.h"
#include "engine/render/transform_history.h"
#include "engine/render/ui_renderer.h"
#include "engine/render/voxel_loader.h"
#include "engine/replication/extract.h"
#include "engine/replication/replication.h"
#include "engine/rhi/device.h"
#include "engine/scene/physics_sync.h"
#include "engine/scene/scene_file.h"
#include "engine/script/modules.h"
#include "engine/ui/ui.h"

#if ENG_RHI_CAPTURE
#include "engine/rhi/capture.h"
#endif

namespace engine::app {
namespace {

// **What was typed during play survives the stop** (the owner: a variable
// changed while playing stayed changed in the tab, and the next play ran the
// old text). The text went into the PLAYING world's `Source`, and the restore
// put back the one from before play -- so the tabs, which are what is on the
// screen, write it again. Only where it differs, so a stop with nothing typed
// marks nothing unsaved.
//
// **Every way out of play** (audit A5): the Stop button and the device-lost
// save did this, and a save-all or an open scene from play did not -- a save
// wrote the text from before play over what was on the screen.
void writeTypedSources(scene::World& world, const ScriptEditor& scripts, Editor& editor)
{
    const core::NameAtom sourceKey = world.atoms().intern("Source");
    for (std::size_t index = 0; index < scripts.count(); ++index) {
        const OpenScript* tab = scripts.at(index);
        if (tab == nullptr || tab->origin != ScriptOrigin::Scene || !world.alive(tab->instance))
            continue;
        const std::optional<scene::Value> restored = world.getProperty(tab->instance, sourceKey);
        const std::string* restoredText = restored.has_value() ? std::get_if<std::string>(&*restored) : nullptr;
        const std::string typed = tab->document.text();
        if (restoredText != nullptr && *restoredText == typed)
            continue;
        (void)world.setProperty(tab->instance, sourceKey, scene::Value{typed});
        editor.touch();
    }
}

// **An open content file follows its file** (audit A5): a shader's tab kept
// the old path, and its next save wrote the file back where it had been.
void followOpenFiles(ScriptEditor& scripts, std::string_view from, std::string_view to)
{
    for (std::size_t index = 0; index < scripts.count(); ++index) {
        OpenScript* tab = scripts.at(index);
        if (tab == nullptr || tab->origin != ScriptOrigin::File)
            continue;
        if (tab->file == from)
            tab->file = std::string(to);
        else if (tab->file.size() > from.size() && tab->file.starts_with(from) && tab->file[from.size()] == '/')
            tab->file = std::string(to) + tab->file.substr(from.size());
    }
}

// `scenes/arena.scene.json` is `arena`: the folder under `src/scenes/` whose
// code is that scene's (ADR 0105).
[[nodiscard]] std::string sceneStemOf(std::string_view path)
{
    std::string name = std::filesystem::path(std::string(path)).filename().string();
    if (constexpr std::string_view Suffix = ".scene.json"; name.ends_with(Suffix))
        name.resize(name.size() - Suffix.size());
    return name;
}

using core::f32;
using core::f64;
using core::I18nArg;
using core::LogLevel;

constexpr f64 kNanosPerSecond = 1'000'000'000.0;

// **A replica's character replay, through whichever world is live** (ADR 0076,
// as amended): by the reference to `host` rather than to its physics mirror,
// because a reload replaces the host and the mirror with it -- the same reason
// the husk probe below holds the reference.
class LiveCharacterReplay final : public scene::ICharacterReplay
{
public:
    explicit LiveCharacterReplay(std::unique_ptr<WorldHost>& host) noexcept : m_host(host) {}

    [[nodiscard]] std::optional<scene::CharacterCommand> lastCommand(core::InstanceId character) const override
    {
        const scene::PhysicsSync* physics = m_host != nullptr ? m_host->physics() : nullptr;
        return physics != nullptr ? physics->lastCommand(character) : std::nullopt;
    }

    [[nodiscard]] std::vector<core::CFrameD> replay(core::InstanceId character,
                                                    const scene::CharacterReplayStart& start,
                                                    std::span<const scene::CharacterCommand> commands) override
    {
        scene::PhysicsSync* physics = m_host != nullptr ? m_host->physics() : nullptr;
        return physics != nullptr ? physics->replay(character, start, commands) : std::vector<core::CFrameD>{};
    }

    void remember(core::u64 tick) override
    {
        if (scene::PhysicsSync* physics = m_host != nullptr ? m_host->physics() : nullptr; physics != nullptr)
            physics->remember(tick);
    }

    [[nodiscard]] std::optional<core::CFrameD> remembered(core::u64 tick, core::InstanceId id) const override
    {
        const scene::PhysicsSync* physics = m_host != nullptr ? m_host->physics() : nullptr;
        return physics != nullptr ? physics->remembered(tick, id) : std::nullopt;
    }

private:
    std::unique_ptr<WorldHost>& m_host;
};

// The format the headless target is created with, named once so the pipeline
// and the texture cannot disagree.
constexpr rhi::TextureFormat kOffscreenFormat = rhi::TextureFormat::Rgba8Unorm;

// The physics backend's wireframe, forwarded into the engine's debug draw
// (roadmap M5, "Jolt debug-draw bridge").
//
// A sink rather than a returned buffer, and the seam is the reason: the backend
// walks shapes it already holds, and copying that into a vector so this could
// walk it again would double the cost of a view whose whole job is to be cheap
// enough to leave on.
class PhysicsWireframe final : public physics::IDebugDrawSink
{
public:
    explicit PhysicsWireframe(render::DebugDraw& draw) noexcept : m_draw(draw) {}

    // Drawn where the frame draws the parts (ADR 0134), rather than where the
    // bodies are.
    PhysicsWireframe(render::DebugDraw& draw, const scene::PhysicsSync& physics, const scene::World& world,
                     const render::DrawPoses& poses) noexcept
        : m_draw(draw), m_physics(&physics), m_world(&world), m_poses(&poses)
    {}

    [[nodiscard]] std::optional<core::CFrameD> drawnPose(core::u64 userData) override
    {
        if (m_physics == nullptr)
            return std::nullopt;
        const core::InstanceId id = m_physics->instanceOf(userData);
        if (!id.valid() || m_world->parts().find(id) == nullptr)
            return std::nullopt;
        return m_poses->part(id);
    }

    void line(core::DVec3 from, core::DVec3 to, core::u32 color) override
    {
        // Rebased the same way every other debug line is: `DebugDraw` holds
        // camera-relative f32, and a world-space line drawn through a
        // camera-relative view-projection is displaced by the camera's distance
        // from the origin -- which is D011, found by looking at a screenshot.
        m_draw.line(core::toVec3(from), core::toVec3(to),
                    render::DebugColor::fromLinear(static_cast<f32>((color >> 16) & 0xff) / 255.0f,
                                                   static_cast<f32>((color >> 8) & 0xff) / 255.0f,
                                                   static_cast<f32>(color & 0xff) / 255.0f));
    }

private:
    render::DebugDraw& m_draw;
    const scene::PhysicsSync* m_physics = nullptr;
    const scene::World* m_world = nullptr;
    const render::DrawPoses* m_poses = nullptr;
};

// A fixed camera looking at the origin from slightly above. Fixed on purpose:
// M1 has no camera Instance -- that is M4 -- and a moving camera would put a
// second source of change into a golden image whose whole value is that only
// one thing moves.
[[nodiscard]] core::Mat4 orbitCamera(core::u32 width, core::u32 height)
{
    const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
    // Far enough back and high enough that all three orbiting cubes are on
    // screen at every phase, rather than one of them being behind another for
    // part of the orbit. A deliverable that says "three cubes orbit" should
    // show three cubes.
    const core::Mat4 view = core::lookAt({0.0f, 5.5f, 8.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const core::Mat4 projection = core::perspective(1.0472f, aspect, 0.1f, 100.0f);
    return projection * view;
}

// What the engine draws for the world itself: one wire box per part that is
// under `Workspace`, from the extracted snapshot rather than from the ECS
// (ADR 0027). The real renderer is M4; until then this is how 500 scripted
// instances are seen at all, and it is what `examples/01-instances` is
// visualized with.
// The UI's draw list into the renderer's vertices, plus one run per contiguous
// span sharing a clip rectangle.
//
// A copy per frame over a few hundred quads, and it is what keeps the layering
// honest: `render` is L4 and `ui` is L5, so the renderer cannot see a
// `ui::DrawQuad` and does not need to (architecture.md §2 rule 3).
//
// Six vertices a quad rather than four and an index buffer. The geometry is the
// smallest thing in the frame; an index buffer would save a third of its
// bandwidth and cost a second upload.
void buildUiGeometry(const ui::DrawList& list, core::Vec2 viewport, std::vector<render::UiVertex>& vertices,
                     std::vector<render::UiScissorRun>& runs, std::span<const rhi::TextureHandle> textures,
                     UiGradientRows& gradients)
{
    vertices.clear();
    runs.clear();

    const auto toByte = [](f32 value) {
        const f32 clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<core::u8>(clamped * 255.0f + 0.5f);
    };

    // A run breaks on EITHER a clip change or a texture change: a draw carries
    // one scissor and one texture, so either is a new draw. The draw list is
    // already ordered by the tree, so runs stay contiguous rather than becoming
    // buckets.
    core::u32 currentScissor = 0xffffffffu;
    core::u32 currentTexture = 0xffffffffu;
    for (const ui::DrawQuad& quad : list.quads) {
        if (quad.scissor != currentScissor || quad.texture != currentTexture) {
            currentScissor = quad.scissor;
            currentTexture = quad.texture;
            const core::Rect& clip = list.scissors[currentScissor];
            // Clamped to the target, because a scissor outside it is a
            // validation error on every backend rather than an empty draw.
            const f32 left = std::fmax(0.0f, clip.min.x);
            const f32 top = std::fmax(0.0f, clip.min.y);
            const f32 right = std::fmin(viewport.x, clip.max.x);
            const f32 bottom = std::fmin(viewport.y, clip.max.y);
            runs.push_back(render::UiScissorRun{
                .scissor = {static_cast<core::i32>(left), static_cast<core::i32>(top),
                            static_cast<core::i32>(std::fmax(0.0f, right - left)),
                            static_cast<core::i32>(std::fmax(0.0f, bottom - top))},
                .firstVertex = static_cast<core::u32>(vertices.size()),
                .vertexCount = 0,
                // Index zero is "no texture" and resolves to the renderer's own
                // white pixel, so an out-of-range index degrades to an untinted
                // quad rather than to an unbound read.
                .texture = currentTexture < textures.size() ? textures[currentTexture] : rhi::TextureHandle{},
            });
        }

        // The quad's own frame, so the fragment stage can measure a corner
        // without knowing where on screen the quad is (D030).
        const f32 halfX = (quad.max.x - quad.min.x) * 0.5f;
        const f32 halfY = (quad.max.y - quad.min.y) * 0.5f;
        // Its gradient's row in the frame's table (ADR 0110), if it has one.
        const f32 gradientRow = quad.gradient != 0 && quad.gradient <= list.gradients.size()
                                    ? gradients.rowOf(list.gradients[quad.gradient - 1])
                                    : -1.0f;

        // The turn, from `GuiObject.Rotation` (S7.13). Applied to the POSITION
        // and to nothing else: the two coordinates after it are the quad's own
        // frame, and leaving them unrotated is what keeps a rounded corner round
        // rather than turning it into an ellipse -- the fragment stage measures
        // a distance in the quad's space, and the quad's space is not what
        // moved.
        //
        // `lean` is rich text's italic: the top edge moves right by the quad's
        // `slant` times its height, before the turn, so a turned label's italics
        // lean with it.
        const f32 lean = quad.slant * (quad.max.y - quad.min.y);
        const auto corner = [&](f32 x, f32 y, f32 u, f32 v, f32 shift) {
            const f32 placedX = x + shift;
            const f32 turnedX = quad.turn.x * placedX - quad.turn.y * y + quad.turnOffset.x;
            const f32 turnedY = quad.turn.y * placedX + quad.turn.x * y + quad.turnOffset.y;
            render::UiVertex vertex{turnedX,
                                    turnedY,
                                    toByte(quad.color.r),
                                    toByte(quad.color.g),
                                    toByte(quad.color.b),
                                    toByte(quad.alpha),
                                    x - (quad.min.x + halfX),
                                    y - (quad.min.y + halfY),
                                    halfX,
                                    halfY,
                                    quad.cornerRadius,
                                    u,
                                    v,
                                    {}};
            fillUiCorner(quad, x, y, gradientRow, vertex.localX, vertex.localY, vertex.halfX, vertex.halfY,
                         vertex.look);
            return vertex;
        };

        const render::UiVertex a = corner(quad.min.x, quad.min.y, quad.uvMin.x, quad.uvMin.y, lean);
        const render::UiVertex b = corner(quad.max.x, quad.min.y, quad.uvMax.x, quad.uvMin.y, lean);
        const render::UiVertex c = corner(quad.max.x, quad.max.y, quad.uvMax.x, quad.uvMax.y, 0.0f);
        const render::UiVertex d = corner(quad.min.x, quad.max.y, quad.uvMin.x, quad.uvMax.y, 0.0f);
        vertices.insert(vertices.end(), {a, b, c, a, c, d});
        runs.back().vertexCount += 6;
    }
}

// The selected instances, outlined in the viewport.
//
// A selection that exists only in a tree view is not a selection in a 3D
// editor: the whole reason to click a thing in the world is to see which thing
// you clicked. Drawn from the world rather than from the render snapshot
// because the snapshot is a filtered, culled, LOD-selected view of it -- a part
// behind the camera or past the far plane is still selected, and an outline
// that vanished when you turned around would be worse than none.
//
// Orange because nothing in a PBR scene is, and because it stays legible
// against both the lit and the shadowed halves of a frame. The primary is
// brighter than the rest, because a manipulator anchors to it and which one
// that is has to be visible.
//
// **Submitted CAMERA-RELATIVE, which is why it takes an origin and why it is
// called after `rebaseTo` rather than with the rest of the debug submissions.**
// `DebugDraw::rebaseTo` subtracts in f32 and its own header says so: a box
// recorded at an absolute world position is quantised to a float BEFORE the
// camera comes off it, which is about half a millimetre four kilometres out and
// worse beyond. `toRenderMatrix` does the subtraction in f64, and an outline
// somebody is trying to drag a handle on cannot afford the other one.
void submitSelection(const scene::World& world, const render::DrawPoses& poses,
                     std::span<const core::InstanceId> selection, core::DVec3 cameraOrigin, render::DebugDraw& draw)
{
    for (usize index = 0; index < selection.size(); ++index) {
        const core::InstanceId id = selection[index];
        if (!id.valid() || !world.alive(id))
            continue;

        const scene::PartComponent* part = world.parts().find(id);
        if (part == nullptr)
            continue;

        const bool primary = index + 1 == selection.size();
        // A hair larger than the part, so the outline sits outside the surface
        // rather than fighting it for the same depth -- a box drawn exactly on
        // a face z-fights along every edge, which reads as a flicker rather
        // than as a selection.
        constexpr f32 kOutlineMargin = 1.01f;
        draw.wireBox(core::toRenderMatrix(poses.part(id), cameraOrigin),
                     core::Vec3{part->size.x * 0.5f * kOutlineMargin, part->size.y * 0.5f * kOutlineMargin,
                                part->size.z * 0.5f * kOutlineMargin},
                     primary ? render::DebugColor::fromLinear(1.0f, 0.45f, 0.05f)
                             : render::DebugColor::fromLinear(0.75f, 0.30f, 0.03f));
    }
}

// **What a selected `Camera` sees, as a wireframe**: the pyramid from its
// position out to a few metres, or the box of an orthographic one. A camera is
// a point with an orientation, and the question somebody placing one has is
// what will be in the shot -- which a marker alone cannot answer.
//
// Drawn to a fixed depth rather than to the far plane, because a far plane
// five kilometres out is a pyramid nobody can see the near end of. The aspect
// is the viewport's, which is the image the camera will be drawn into.
void submitCameraVolumes(const scene::World& world, std::span<const core::InstanceId> selection,
                         core::DVec3 cameraOrigin, f32 aspect, render::DebugDraw& draw)
{
    constexpr f32 kShownDepth = 6.0f;
    const render::DebugColor colour = render::DebugColor::fromLinear(0.95f, 0.85f, 0.25f);
    for (const core::InstanceId id : selection) {
        const scene::CameraComponent* camera = id.valid() && world.alive(id) ? world.cameras().find(id) : nullptr;
        if (camera == nullptr)
            continue;
        const core::Mat4 transform = core::toRenderMatrix(camera->cframe, cameraOrigin);
        const auto at = [&transform](f32 x, f32 y, f32 z) {
            return core::transformPoint(transform, core::Vec3{x, y, z});
        };

        const f32 depth = camera->farPlane < kShownDepth ? camera->farPlane : kShownDepth;
        const f32 nearDepth = camera->nearPlane < depth ? camera->nearPlane : depth * 0.01f;
        f32 nearHalfY = 0.0f;
        f32 farHalfY = 0.0f;
        if (camera->projection == 1) {
            nearHalfY = camera->orthographicSize;
            farHalfY = camera->orthographicSize;
        }
        else {
            const f32 slope = std::tan(camera->fieldOfView * 0.5f * 3.14159265f / 180.0f);
            nearHalfY = slope * nearDepth;
            farHalfY = slope * depth;
        }
        const f32 nearHalfX = nearHalfY * aspect;
        const f32 farHalfX = farHalfY * aspect;

        // Forward is -Z in the camera's own frame.
        const core::Vec3 nearCorners[4] = {at(-nearHalfX, -nearHalfY, -nearDepth),
                                           at(nearHalfX, -nearHalfY, -nearDepth), at(nearHalfX, nearHalfY, -nearDepth),
                                           at(-nearHalfX, nearHalfY, -nearDepth)};
        const core::Vec3 farCorners[4] = {at(-farHalfX, -farHalfY, -depth), at(farHalfX, -farHalfY, -depth),
                                          at(farHalfX, farHalfY, -depth), at(-farHalfX, farHalfY, -depth)};
        for (int corner = 0; corner < 4; ++corner) {
            const int next = (corner + 1) % 4;
            draw.line(nearCorners[corner], nearCorners[next], colour);
            draw.line(farCorners[corner], farCorners[next], colour);
            draw.line(nearCorners[corner], farCorners[corner], colour);
        }
        // Which way is up in the shot: a small tick above the far edge.
        draw.line(at(-farHalfX * 0.2f, farHalfY, -depth), at(0.0f, farHalfY * 1.2f, -depth), colour);
        draw.line(at(0.0f, farHalfY * 1.2f, -depth), at(farHalfX * 0.2f, farHalfY, -depth), colour);
    }
}

// **The engine's look for a `ProximityPrompt`** (ADR 0126): a dark box above
// where it hangs, the key on its left -- a ring filling under it for a held
// prompt -- and the two texts. The same box, at the same place, the tick
// hit-tests a tap against (`scene::PromptWidth`).
void appendPrompts(scene::World& world, const render::DrawPoses& poses, const render::RenderCamera& camera,
                   core::Vec2 viewport, ui::DrawList& out)
{
    scene::EngineState& state = world.engineState();
    if (state.shownPrompts.empty() || !camera.valid || viewport.x <= 0.0f || viewport.y <= 0.0f) {
        // Drawn nowhere this frame -- a minimised window -- so a tap is not
        // tested against where it was drawn the last time it was.
        for (scene::ShownPrompt& shown : state.shownPrompts)
            shown.drawn = false;
        return;
    }
    const ViewportRect rect{0.0f, 0.0f, viewport.x, viewport.y};
    const auto keyName = [&world](core::i32 keyCode) -> std::string {
        const scene::EnumItemDesc* item = world.enums().findValue(scene::generated::KeyCodeEnumId, keyCode);
        std::string name = item != nullptr ? std::string(world.atoms().text(item->name)) : std::string{};
        // The standard layout's face buttons by the letters printed on them.
        if (name == "ButtonSouth")
            return "A";
        if (name == "ButtonEast")
            return "B";
        if (name == "ButtonWest")
            return "X";
        if (name == "ButtonNorth")
            return "Y";
        return name;
    };
    const core::Color3 panel{0.08f, 0.09f, 0.11f};
    const core::Color3 keyFill{0.92f, 0.93f, 0.95f};
    const core::Color3 ink{0.08f, 0.09f, 0.11f};
    const core::Color3 white{1.0f, 1.0f, 1.0f};
    const core::Color3 muted{0.72f, 0.74f, 0.78f};
    for (scene::ShownPrompt& shown : state.shownPrompts) {
        const scene::ProximityPromptComponent* prompt = world.proximityPrompts().find(shown.prompt);
        if (prompt == nullptr)
            continue;
        // Where what it hangs from is drawn (ADR 0134), not where the tick
        // left it: a prompt over a moving cart otherwise shook against it.
        core::DVec3 anchor = shown.anchor;
        if (world.alive(shown.hangsFrom)) {
            anchor = world.attachments().find(shown.hangsFrom) != nullptr ? poses.attachment(shown.hangsFrom).position
                                                                          : poses.part(shown.hangsFrom).position;
        }
        const std::optional<core::Vec2> at =
            worldToViewport(camera.projection, camera.view, camera.origin, rect, anchor);
        if (!at.has_value()) {
            shown.drawn = false;
            continue;
        }
        const core::Vec2 centre{at->x + prompt->uiOffset.x, at->y + prompt->uiOffset.y - scene::PromptLift};
        // And a tap is tested here, on the box drawn.
        shown.drawnAt = centre;
        shown.drawn = true;
        const core::Vec2 min{centre.x - scene::PromptWidth * 0.5f, centre.y - scene::PromptHeight * 0.5f};
        const core::Vec2 max{centre.x + scene::PromptWidth * 0.5f, centre.y + scene::PromptHeight * 0.5f};

        ui::DrawQuad box;
        box.min = min;
        box.max = max;
        box.color = panel;
        box.alpha = 0.78f;
        box.cornerRadius = 10.0f;
        out.quads.push_back(box);

        const f32 key = scene::PromptHeight - 16.0f;
        ui::DrawQuad cap;
        cap.min = core::Vec2{min.x + 8.0f, min.y + 8.0f};
        cap.max = core::Vec2{cap.min.x + key, cap.min.y + key};
        cap.color = keyFill;
        cap.cornerRadius = shown.inputType == 2 ? key * 0.5f : 6.0f;
        out.quads.push_back(cap);
        if (shown.holdProgress > 0.0f) {
            ui::DrawQuad fill;
            fill.min = core::Vec2{cap.min.x, cap.max.y - 4.0f};
            fill.max = core::Vec2{cap.min.x + key * std::min(1.0f, shown.holdProgress), cap.max.y};
            fill.color = core::Color3{0.30f, 0.62f, 1.0f};
            fill.cornerRadius = 2.0f;
            out.quads.push_back(fill);
        }
        const std::string label = shown.inputType == 2   ? core::engineCatalog().format(ENG_TR("ui.prompt.tap"))
                                  : shown.inputType == 1 ? keyName(prompt->gamepadKeyCode)
                                                         : keyName(prompt->keyboardKeyCode);
        ui::buildTextGeometry(label, "", label.size() > 2 ? 12.0f : 20.0f, 0.0f, core::Rect{cap.min, cap.max}, 1, 1,
                              ink, 1.0f, 0, out.quads);

        const core::Rect words{core::Vec2{cap.max.x + 10.0f, min.y + 6.0f}, core::Vec2{max.x - 8.0f, max.y - 6.0f}};
        const std::string_view object = world.atoms().text(prompt->objectText);
        const std::string_view action = world.atoms().text(prompt->actionText);
        if (object.empty()) {
            ui::buildTextGeometry(action, "", 18.0f, 0.0f, words, 0, 1, white, 1.0f, 0, out.quads);
        }
        else {
            const f32 middle = (words.min.y + words.max.y) * 0.5f;
            ui::buildTextGeometry(object, "", 13.0f, 0.0f, core::Rect{words.min, core::Vec2{words.max.x, middle}}, 0, 2,
                                  muted, 1.0f, 0, out.quads);
            ui::buildTextGeometry(action, "", 18.0f, 0.0f, core::Rect{core::Vec2{words.min.x, middle}, words.max}, 0, 0,
                                  white, 1.0f, 0, out.quads);
        }
    }
}

// **How far a click or a prompt reaches** (ADR 0126), drawn round a selected
// `ClickDetector` or `ProximityPrompt` -- or round the part holding one -- at
// the part or attachment it hangs from.
void submitDetectorVolumes(const scene::World& world, std::span<const core::InstanceId> selection,
                           core::DVec3 cameraOrigin, render::DebugDraw& draw)
{
    const auto drawOne = [&](core::InstanceId id) {
        f64 reach = 0.0;
        render::DebugColor colour = render::DebugColor::fromLinear(0.30f, 0.62f, 1.0f);
        if (const scene::ClickDetectorComponent* click = world.clickDetectors().find(id)) {
            reach = click->maxActivationDistance;
        }
        else if (const scene::ProximityPromptComponent* prompt = world.proximityPrompts().find(id)) {
            reach = prompt->maxActivationDistance;
            colour = render::DebugColor::fromLinear(1.0f, 0.78f, 0.25f);
        }
        else {
            return;
        }
        const core::InstanceId parent = world.parentOf(id);
        core::DVec3 at;
        if (const scene::PartComponent* part = world.parts().find(parent))
            at = part->cframe.position;
        else if (const scene::AttachmentComponent* attachment = world.attachments().find(parent))
            at = attachment->worldCFrame.position;
        else
            return;
        draw.wireSphere(core::toVec3(at - cameraOrigin), static_cast<f32>(reach), colour, 48);
    };
    for (const core::InstanceId selected : selection) {
        drawOne(selected);
        for (core::InstanceId child = world.firstChild(selected); child.valid(); child = world.nextSibling(child))
            drawOne(child);
    }
}

// **Where a selected light reaches, as a wireframe** (the owner: "a spot
// should draw a cone like the camera's, showing where it hits, and a point
// light a circle"). A point light is three rings of its `Range`; a spot is
// its cone out to `Range`, with the rim where the cone meets that distance.
// In the light's own colour, so two lamps side by side are told apart.
//
// Shown for a light that is selected and for the lights on a selected part --
// directly or through one of its attachments -- because the part is what
// somebody clicks in the viewport; the light itself has no shape to click.
// From `lightAnchorOf`, the renderer's own answer, so the cone is where the
// light is.
void submitLightVolumes(const scene::World& world, std::span<const core::InstanceId> selection,
                        core::DVec3 cameraOrigin, render::DebugDraw& draw)
{
    constexpr int kSegments = 32;
    constexpr f32 kTau = 6.28318531f;
    const auto drawLight = [&](core::InstanceId id) {
        const scene::PointLightComponent* point = world.pointLights().find(id);
        const scene::SpotLightComponent* spot = point == nullptr ? world.spotLights().find(id) : nullptr;
        if (point == nullptr && spot == nullptr)
            return;
        const std::optional<render::LightAnchor> anchored = render::lightAnchorOf(world, id);
        if (!anchored.has_value())
            return;
        const core::CFrameD anchor = anchored->partFrame * anchored->offset;
        const core::Mat4 transform = core::toRenderMatrix(anchor, cameraOrigin);
        const auto at = [&transform](f32 x, f32 y, f32 z) {
            return core::transformPoint(transform, core::Vec3{x, y, z});
        };
        const core::Color3 tint = point != nullptr ? point->color : spot->color;
        // Lifted towards white so a dark red lamp still shows on a dark world.
        const render::DebugColor colour =
            render::DebugColor::fromLinear(0.35f + 0.65f * tint.r, 0.35f + 0.65f * tint.g, 0.35f + 0.65f * tint.b);

        // A ring of `radius` in the plane `axis` names, `depth` along -Z.
        const auto ring = [&](f32 radius, int axis, f32 depth) {
            for (int step = 0; step < kSegments; ++step) {
                const f32 a0 = kTau * static_cast<f32>(step) / kSegments;
                const f32 a1 = kTau * static_cast<f32>(step + 1) / kSegments;
                const auto onRing = [&](f32 angle) {
                    const f32 c = std::cos(angle) * radius;
                    const f32 s2 = std::sin(angle) * radius;
                    if (axis == 0)
                        return at(0.0f, c, s2);
                    if (axis == 1)
                        return at(c, 0.0f, s2);
                    return at(c, s2, -depth);
                };
                draw.line(onRing(a0), onRing(a1), colour);
            }
        };

        if (point != nullptr) {
            const f32 range = std::max(point->range, 0.0f);
            ring(range, 0, 0.0f);
            ring(range, 1, 0.0f);
            ring(range, 2, 0.0f);
            return;
        }

        // `Angle` is the full cone; the rim sits where a ray of length `Range`
        // along the cone's edge ends, which keeps a wide spot's cone finite.
        const f32 range = std::max(spot->range, 0.0f);
        const f32 half = std::clamp(spot->angle, 0.0f, 179.0f) * 0.5f * kTau / 360.0f;
        const f32 depth = range * std::cos(half);
        const f32 radius = range * std::sin(half);
        ring(radius, 2, depth);
        const core::Vec3 apex = at(0.0f, 0.0f, 0.0f);
        for (int edge = 0; edge < 8; ++edge) {
            const f32 angle = kTau * static_cast<f32>(edge) / 8.0f;
            draw.line(apex, at(std::cos(angle) * radius, std::sin(angle) * radius, -depth), colour);
        }
        // The axis, so which way it points reads from any side.
        draw.line(apex, at(0.0f, 0.0f, -depth), colour);
    };

    for (const core::InstanceId id : selection) {
        if (!id.valid() || !world.alive(id))
            continue;
        drawLight(id);
        // The lights a selected part carries, and those on its attachments.
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
            drawLight(child);
            if (world.attachments().find(child) != nullptr) {
                for (core::InstanceId inner = world.firstChild(child); inner.valid(); inner = world.nextSibling(inner))
                    drawLight(inner);
            }
        }
    }
}

void submitWorld(const render::RenderWorld& snapshot, render::DebugDraw& draw)
{
    for (const render::RenderPart& part : snapshot.parts) {
        // Fully transparent is not drawn. A debug wireframe has no blending, so
        // the alternative is a box that a script asked to be invisible and that
        // is nonetheless the most visible thing on screen.
        if (part.transparency >= 1.0f)
            continue;

        draw.wireBox(core::toRenderMatrix(part.cframe, {}),
                     core::Vec3{part.size.x * 0.5f, part.size.y * 0.5f, part.size.z * 0.5f},
                     render::DebugColor::fromLinear(part.color.r, part.color.g, part.color.b));
    }
}

// M1's stand-in for a renderer: a colour that moves, so a static frame and a
// running one are distinguishable in a screenshot. Derived from the tick count
// rather than the wall clock, which is what makes a headless capture
// reproducible -- the same frame number gives the same colour, on any machine,
// at any speed. That property is what allows a golden gate to exist at all
// (R10, architecture.md §9).
[[nodiscard]] rhi::ColorRgba pulseColor(u64 tick, f64 fixedDt) noexcept
{
    const f64 t = static_cast<f64>(tick) * fixedDt;
    const auto wave = [t](f64 phase) { return static_cast<f32>(0.5 + 0.5 * std::sin(t + phase)); };
    // Thirds of a turn apart, so the three channels never move together and a
    // channel that is stuck shows up.
    return {.r = wave(0.0), .g = wave(2.0944), .b = wave(4.1888), .a = 1.0f};
}

// Writing the recorded stream is the app's job, not the backend's: the backend
// records into memory and knows nothing about files, which is what lets a test
// read a capture without touching a disk.
[[nodiscard]] std::optional<core::EngineError> writeCapture(const std::filesystem::path& path,
                                                            const rhi::IDevice& device)
{
#if ENG_RHI_CAPTURE
    const std::string& stream = rhi::captureStream(device);
    if (stream.empty()) {
        // An empty golden would match forever. Better to fail here than to
        // check in a file that can never catch anything.
        return core::makeError(ENG_TR("engine.capture.err.empty"));
    }

    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        const std::array<I18nArg, 1> args{I18nArg{"path", path.string()}};
        return core::makeError(ENG_TR("engine.capture.err.open_failed"), args);
    }

    // Binary mode on purpose: the stream is newline-terminated JSON lines, and
    // letting Windows translate them would make a golden recorded on one
    // platform differ from the same frame recorded on another.
    file.write(stream.data(), static_cast<std::streamsize>(stream.size()));
    file.close();

    if (!file) {
        const std::array<I18nArg, 1> args{I18nArg{"path", path.string()}};
        return core::makeError(ENG_TR("engine.capture.err.write_failed"), args);
    }

    return std::nullopt;
#else
    static_cast<void>(path);
    static_cast<void>(device);
    return core::makeError(ENG_TR("engine.capture.err.empty"));
#endif
}

} // namespace

bool gpuValidationWanted(std::string_view profile, bool optIn) noexcept
{
    return optIn || profile == "debug" || profile == "dev";
}

std::optional<core::EngineError> run(const EngineOptions& options)
{
    // **The job pool, started here, and it had no caller until M7.5.** M7 built
    // it -- work stealing, dependencies, `parallelFor`, `StableCommit` -- and
    // nothing in the engine ever called `init`, so every `parallelFor` in it had
    // been taking `jobs.h`'s documented serial path. That is a legitimate mode
    // and it is not the shipping one; the environment prefilter was the first
    // caller to notice, at two and a half milliseconds a frame.
    //
    // Zero means one worker per hardware thread less this one (jobs.h). Nothing
    // sim-visible runs on it, and the render-side work that does partitions by
    // DATA -- so the answer does not depend on how many workers this machine
    // has, which is what R10 asks of a pool at all.
    jobs::init();

    if (const auto error = platform::init({.headless = options.headless}); error.has_value())
        return error;

    // **A windowless run that serves or measures asks to be scheduled ahead**
    // (D193). A windowed game already is, by owning the foreground window; a
    // headless one that nobody is waiting on -- a test, a capture -- has no frame
    // time worth protecting at the desktop's expense.
    {
        const bool serving = options.network.topology == replication::Topology::Host ||
                             options.network.topology == replication::Topology::Dedicated;
        const bool measuring = options.frameStats || !options.soakReportPath.empty();
        if (options.headless && (serving || measuring) && platform::raiseProcessPriority())
            core::log(LogLevel::Info, ENG_TR("engine.info.priority_raised"));
    }

    // Declaration order below IS the shutdown order, reversed, and it is not
    // arbitrary: SDL_GPU requires a window to be released from its device
    // before the window is destroyed. Declaring the window first means the
    // device dies first -- releasing it -- on every path out of this function,
    // including the early returns.
    struct PlatformScope
    {
        ~PlatformScope()
        {
            platform::shutdown();
            jobs::shutdown();
        }
    } platformScope;

    platform::WindowPtr window;
    core::EngineError error;

    const bool gpuDebug = gpuValidationWanted(ENG_PROFILE_NAME, options.gpuDebug);
    if (gpuDebug)
        core::log(LogLevel::Info, ENG_TR("engine.info.gpu_debug"));
    const rhi::DeviceResult device = createDevice({.backend = options.backend, .debug = gpuDebug}, &error);
    if (device == nullptr)
        return error;

    if (!options.headless) {
        const std::array<I18nArg, 1> titleArgs{I18nArg{"version", ENG_VERSION_STRING}};
        // **Where the editor was left, if it was ever left anywhere.** Read
        // before the window exists, which is why it is a static reader: the
        // window has to be CREATED at the remembered size rather than created at
        // the default and resized, or a person watches their editor open small
        // and jump every launch.
        const std::optional<platform::WindowPlacement> placement =
            options.windowPlacement.has_value()
                ? options.windowPlacement
                : (options.editor ? Editor::recallWindow(options.scriptPath / ".engine") : std::nullopt);
        // One of a match's windows says which it is (ADR 0106 §5).
        const std::string labelled =
            options.windowLabel.empty()
                ? options.windowTitle
                : (options.windowTitle.empty() ? std::string(core::kBrandName) : options.windowTitle) + " -- " +
                      options.windowLabel;
        window = platform::createWindow(
            {
                .titleKey = ENG_TR("platform.window.title"),
                .titleArgs = titleArgs,
                .title = labelled,
                .width = placement.has_value() ? placement->width : options.width,
                .height = placement.has_value() ? placement->height : options.height,
                // The editor is always a window you can size; a game says.
                .resizable = options.editor || options.resizable,
            },
            &error);
        if (window == nullptr)
            return error;
        // `[window] fullscreen` (ADR 0104 §1): the game's, never the editor's.
        if (options.fullscreen && !options.editor && !options.windowPlacement.has_value())
            platform::setWindowFullscreen(*window, true);
        if (placement.has_value())
            platform::setWindowPlacement(*window, *placement);

        // The window wears whatever icon this executable carries in its own
        // resources (roadmap M8). Nothing is configured and nothing is
        // installed: a packaged game had that resource replaced by
        // `ludwerk build`, so the same three lines dress the engine's dev host in
        // the Ludwerk mark and a shipped game in its own.
        //
        // Every failure here is silent and survivable -- a platform with no
        // per-window icon, an icon that will not decode, a build with no
        // resource at all. A window with the default icon is a cosmetic loss;
        // refusing to open one is not.
        if (const std::vector<std::byte> iconBytes = platform::applicationIconBytes(); !iconBytes.empty()) {
            asset::Image icon;
            if (!asset::decodeImage(iconBytes, icon).has_value() && icon.valid()) {
                (void)platform::setWindowIcon(*window, icon.pixels, static_cast<i32>(icon.width),
                                              static_cast<i32>(icon.height));
            }
        }

        // **The project's own icon, while it is being made** (ADR 0104): a PNG
        // named by `[project] icon` goes on the window over the engine's mark,
        // so the game looks like itself before anybody builds it. An `.ico` is
        // for the built executable, and the window keeps what that carries.
        if (!options.projectIcon.empty() && options.projectIcon.extension() == ".png") {
            std::vector<std::byte> bytes;
            if (platform::readFile(options.projectIcon, bytes)) {
                asset::Image icon;
                if (!asset::decodeImage(bytes, icon).has_value() && icon.valid()) {
                    (void)platform::setWindowIcon(*window, icon.pixels, static_cast<i32>(icon.width),
                                                  static_cast<i32>(icon.height));
                }
            }
        }

        if (!device->claimWindow(*window))
            return core::makeError(ENG_TR("rhi.err.window_claim_failed"), {}, "SDL_ClaimWindowForGPUDevice");
    }

    // Headless has no swapchain, so it owns a target of its own. Everything
    // downstream is identical, which is the point: the harness exercises the
    // same path a windowed run does rather than a simplified one.
    rhi::TextureHandle offscreen;
    if (options.headless) {
        offscreen = device->createTexture({
            .format = kOffscreenFormat,
            .usage = rhi::TextureUsage::ColorTarget,
            .width = static_cast<core::u32>(options.width),
            .height = static_cast<core::u32>(options.height),
            .debugName = "headless-color",
        });
        if (!offscreen.valid())
            return core::makeError(ENG_TR("rhi.err.target_create_failed"));
    }

    // Dev builds only, windowed only, and after the claim: the overlay's
    // pipeline is built against the swapchain's colour format, which does not
    // exist until the device owns the window. Compiled out entirely in shipping
    // (ADR 0011), where the constructor is a no-op and active() is false.
    // Declared before the overlay, which holds a pointer to it.
    EditorDrive editorDrive;
    std::optional<DebugOverlay> overlay;
    if (window != nullptr) {
        // The layout lives inside the project it belongs to, not beside the
        // executable: two projects open in turn should not fight over one
        // arrangement of panels.
        //
        // **`.v3` because E8 added a panel** (ADR 0057), and `.v2` because
        // ADR 0056 renamed every one of them.
        //
        // ImGui keys a saved layout by window NAME, so a file written before a
        // window existed has no entry for it -- and the window then appears
        // floating in the middle of the screen rather than docked where the
        // default layout puts it. Asking the Console where it lives covers the
        // panel arriving mid-session; a new file is what covers the arrangement
        // that was already written. Both are needed, and the second is one line.
        //
        // The older half: ImGui keys a saved
        // layout by window NAME, so a file written against `explorer` docks
        // nothing at all once the window is called `Explorer` -- and the shell's
        // "nobody has arranged this yet" test sees a split node with windows in
        // it and declines to rebuild. A new name is one line and costs somebody
        // one arrangement; the alternative is a first launch with every panel
        // floating and no obvious way back.
        const std::filesystem::path layout =
            options.editor ? options.scriptPath / ".engine" / "editor-layout.v3.ini" : std::filesystem::path{};
        overlay.emplace(*window, *device, options.editor ? Shell::Editor : Shell::Overlay,
                        options.editor ? layout.string() : std::string{});
    }

    // The editor's model and the texture its viewport is drawn into. Both are
    // inert unless `--edit` asked for them, and the target is not created until
    // a panel has said how big it is.
    Editor editor;
    // The editor's Play with players (ADR 0106 §5): a match of separate
    // processes, their output drained into this process's log every frame.
    MatchLauncher match;
    // The open scripts (ADR 0057). Beside the `Editor` rather than inside it
    // because they outlive a reload: a tab is keyed on an instance and a
    // breakpoint on a chunk, and `reloadWorld` replaces every instance in the
    // world while the chunk names stay what they were.
    ScriptEditor scripts;
    // What this person had last time: their content-folder colours, and the
    // scene they were looking at (the scene is read separately below, because
    // the boot has to know it before an `Editor` exists).
    if (options.editor && !options.scriptPath.empty())
        editor.recallState(options.scriptPath / ".engine");

    // What the system's file picker answered, and whether one is open. Beside
    // the editor rather than inside it because the picker is the platform's and
    // its callback arrives while events are pumped.
    std::vector<std::filesystem::path> importedPaths;
    bool importPending = false;
    // The same, for the Terrain panel's heightmap: one file, handed to the
    // editor when it arrives rather than copied anywhere.
    std::vector<std::filesystem::path> heightmapPicked;
    bool heightmapPending = false;
    // The instance an Explorer import will parent what it makes under, held
    // across the frames the dialog is open.
    core::InstanceId importParent;
    // **A script just made, opened on the next frame** with the caret on its
    // first line: somebody who inserts a script is about to write in it, and a
    // tree row they must double-click first is a step between them and that.
    core::InstanceId scriptToOpen;

    ViewportTarget viewportTarget;
    // The editor's icons, built once from `content/icons` on the first frame
    // that has a command list -- uploading a texture is one, so this cannot be
    // done before the loop.
    IconAtlas iconAtlas;
    // The content browser pictures of pictures. Owned here rather than by the
    // overlay because decoding one needs a command list, and this is the loop
    // that has one -- the browser asks while it draws and this answers between
    // frames.
    ThumbnailCache thumbnails;
    // The half of a preview that needs a device and the host's registries. Held
    // beside the cache it feeds and destroyed with it.
    std::unique_ptr<HostPreviewRenderer> previewRenderer;
    // Which meshes finished loading this frame, so the physics mirror is handed
    // those and not the whole library. Held across frames rather than declared
    // in the loop, so the allocation happens once.
    std::vector<core::NameAtom> meshCompletions;
    // What was last written to `.engine/editor.json`, so the write happens on a
    // change rather than every frame.
    std::string rememberedScene;
    // This frame's relative pointer motion, accumulated from the events.
    core::Vec2 editorLookDelta;
    // Where the pointer was when it was last free, so a hold can put it back.
    core::Vec2 editorPointerAnchor;
    // Whether the game was taking input last frame, so the release happens once
    // on the way out rather than every frame the editor is editing.
    bool gameHadInput = true;

    // The explorer's selection and the queue its edits wait in. Held by the
    // frame loop rather than by the overlay because it outlives a hot reload
    // and the world does not, and because the drain below has to happen in
    // every profile -- including the shipping one, where nothing ever fills it
    // and the call costs an empty loop (ADR 0011, and why this line carries no
    // #ifdef).
    Inspector inspector;

    // The debug pass is built on the first frame that has a target, not here,
    // because a graphics pipeline is compiled against one colour format and the
    // swapchain's is not known until it has been acquired. Headless knows its
    // own, but running both paths through the same lazy construction keeps them
    // from drifting.
    //
    // It is also optional by design rather than by accident: a device that
    // renders nothing -- capture, null -- has no pipeline to build, and a
    // machine whose content directory is missing its shaders should boot and
    // say why rather than refuse to start.
    // Empty until something loads a mesh into it. `extract` skips a `MeshPart`
    // whose content is not here, so an unpopulated library renders the debug
    // path and nothing else -- which is exactly the state a world with no
    // MeshParts is in.
    // What the last frame actually submitted. Reported beside frame time because
    // the roadmap asks for the *why* next to the *what*: a frame that got slower
    // with the same draw count is a different problem from one that got slower
    // because it drew more.
    core::u32 frameDrawCalls = 0;
    // How many objects the camera could see, which stopped being the same
    // number as `frameDrawCalls` at M7.5: a run of objects sharing a mesh and a
    // material is now one call. The roadmap's gate for the instanced path is
    // exactly these two side by side -- if they are equal, it did nothing.
    core::u32 frameVisibleObjects = 0;
    core::u32 frameInstancedDraws = 0;
    core::u64 frameTriangles = 0;
    core::u32 frameLodDraws = 0;
    std::vector<f64> frameTimesMs;
    // **Where each frame went** (audit P1): the simulation, and the time spent
    // waiting on the GPU and the display -- a command buffer, a swapchain
    // image, the present -- with what is left the CPU drawing. Measured on the
    // frame that spent it and recorded with the next frame's sample, as the
    // frame time is. Without it a slow frame could not be attributed.
    std::vector<f64> frameSimMs;
    std::vector<f64> frameWaitMs;
    f64 phaseSimMs = 0.0;
    f64 phaseWaitMs = 0.0;
    const auto msSince = [](core::u64 since) { return static_cast<f64>(platform::nowNs() - since) / 1'000'000.0; };
    // Sixty warm-up frames rather than `--frame-stats`'s ten. A soak is minutes
    // long, so a second of startup costs it nothing -- and the streamed world
    // has not finished its first ring of chunks inside ten frames, which would
    // put the whole materialisation burst in the measured window.
    SoakRecorder soak(60);
    core::u64 lastFrameNs = 0;

    // Where everything was one tick ago (D047). Owned by the frame loop rather
    // than by the world, because it is not world state: a reload replaces the
    // host and this simply starts again, which is right -- a reloaded world has
    // no previous frame to have come from.
    // What the window has been TOLD, so a change is applied once. The engine
    // state carries what the game wants; these carry what SDL was last asked
    // for, and the two are different questions.
    bool pointerLocked = false;
    bool pointerVisible = true;
    // The orientation last handed to the window; -1 so the first frame applies
    // whatever the world starts with.
    core::i32 appliedOrientation = -1;
    // And the same question for the overlay, which has a second author: F3.
    // `DebugService.OverlayVisible` and the panel's own state are synced in
    // both directions below, and this is which of the two moved (D055).
    bool overlayVisible = false;

    render::TransformHistory transformHistory;
    // What `transformHistory` was captured against. A restore, an undo, a scene
    // load or a reload all replace the world under it, and `world.h` says in so
    // many words what that costs: the history is "rebuilt from the tree rather
    // than restored ... safe order: restore, then rebuild". Nothing rebuilt it,
    // and a snapshot preserves generations precisely so an id means the same
    // thing afterwards -- which is what let a stale entry go on answering.
    //
    // The inspector's counter is the signal because it already IS one: every
    // path that replaces the world calls `onWorldChanged`, and inventing a
    // second notion of "the world is not the one it was" would be inventing
    // somewhere for the two to disagree.
    core::u64 transformHistoryWorld = 0;
    // **Where everything is drawn this frame** (ADR 0134): resolved once per
    // instance from the history and the frame's alpha, and asked by all of
    // the frame -- the world, its UI, particles, views, prompts, the pointer.
    render::DrawPoses framePoses;

    render::MeshLibrary meshLibrary;
    // The textures the scene's `Material` instances name. Beside the mesh
    // library rather than inside it: a texture is not a property of a mesh, and
    // two materials naming one image share one upload.
    render::TextureLibrary textureLibrary;
    // Every `CameraTexture`'s texture (ADR 0107), drawn before the main view.
    ViewHost viewHost;
    viewHost.setLimits(options.maxViewsPerFrame, options.maxViewResolution);
    // Reused from frame to frame, so a view's extraction allocates once.
    render::RenderWorld viewSnapshot;
    // **What the frame keeps for each sub-world it draws** (ADR 0107 §3): its
    // own meshes and textures. A cache is keyed by one world's instances and
    // atoms, which mean nothing in another's -- the two-worlds proof's lesson.
    // Keyed by the run's serial, so a world loaded again starts clean.
    struct SubWorldGpu
    {
        core::u64 serial = 0;
        render::MeshCache meshes;
        render::MeshLoader loader;
        render::MeshLibrary library;
        render::TextureLibrary textures;
    };
    std::vector<std::unique_ptr<SubWorldGpu>> subWorldGpu;
    render::MeshCache meshCache;
    render::MeshLoader meshLoader;
    core::u64 contentRefreshesSeen = 0;

    // Where `asset://` resolves from. Two mounts and the order is the rule:
    // the project's content DIRECTORY first, then its pack if one has been
    // built, so a compiled asset wins over the source it was compiled from and
    // a developer can still drop a loose file in to try something.
    //
    // A project with no pack behaves exactly as it did before M7 -- the loose
    // path is not a fallback here, it is the dev-mode path ADR 0010 keeps
    // forever.
    asset::ContentMounts contentMounts;
    // **The process's materials** (ADR 0090), read through those mounts and
    // lent to every world this process draws -- the game's, a stamp's stage,
    // and whatever replaces the game's on a reload -- so one URN is one
    // material everywhere, and an edit in the material panel shows everywhere
    // at once. Declared after the mounts it reads through, so it goes first.
    asset::MaterialLibrary materialLibrary(asset::mountedMaterials(contentMounts));

    // The streamed world, if the project has one. Inactive and free for every
    // project that does not, which is every example before this milestone.
    StreamingHost streaming;
    // Terrain and block worlds, streamed on a grid of their own (ADR 0075).
    FieldStreamer fields;
    std::unique_ptr<render::IRenderer> renderer;
#if ENG_DEBUG_UI
    // A user's surface shaders, compiled on a worker (ADR 0091). Editor and dev
    // builds only: a shipped game carries bytecode and no compiler.
    std::unique_ptr<SurfaceCompiler> surfaceCompiler;
#else
    // A built game's surface shaders, read from its pack and never compiled.
    std::unique_ptr<render::PackSurfaceSource> packSurfaces;
#endif
    render::ShaderLibrary shaders;
    render::DebugRenderer debugRenderer;
    render::UiRenderer uiRenderer;
    UiText uiText;
    // The size the UI was last laid out against, so a target that changes size
    // dirties every tree. Held here rather than derived, because "did this
    // change" is a question about the previous frame.
    core::Vec2 lastUiViewport;
    // The pointer and keyboard facts the UI needs, gathered from this frame's
    // events. Edges rather than states: a press is a frame on which the button
    // went down, and the UI needs the edge to tell a click from a hold.
    bool uiPointerDown = false;
    bool lastUiPointerDown = false;
    std::string uiTypedText;
    bool uiBackspace = false;
    // The caret's own keys (S6.7). One flag per key rather than a state, because
    // this is what happened THIS frame and the caret is what it moved to.
    bool uiForwardDelete = false;
    bool uiCaretLeft = false;
    bool uiCaretRight = false;
    bool uiCaretHome = false;
    bool uiCaretEnd = false;
    bool uiSubmit = false;
    render::DebugDraw debugDraw;
    ui::DrawList uiDrawList;
    // One tree's canvas at a time, for the world-space UI (F3).
    ui::DrawList worldUiDrawList;
    std::vector<render::UiVertex> uiVertices;
    std::vector<render::UiScissorRun> uiRuns;
    std::vector<rhi::TextureHandle> uiTextures;
    // The frame's gradient rows, shared by the screen's UI and the world's.
    UiGradientRows uiGradients;
    bool debugPassAttempted = false;

    // Mounts and the streamed world are set up HERE, outside `ensureDebugPass`,
    // and that placement is a defect this milestone had to find twice. They
    // lived inside it, so a device that could not take shaders -- `--rhi=null`,
    // or any machine whose shader load failed -- booted with NO content mounted
    // and NO chunk index, silently. The soak gate ran in 0.17 s over a world of
    // eleven instances and reported a pass.
    //
    // Nothing below reads a shader, a format or a device. Content addressing and
    // streaming residency are decisions about files and distances; the renderer
    // is downstream of them, and coupling the two made a gate vacuous rather
    // than red -- the worse of the two failures.
    // The PROJECT's content directory, not the engine's. `asset://` is a
    // URN in the game's namespace -- `asset://models/tree.glb` means the
    // file the developer put in their own `content/models/`, and resolving
    // it beside `engine-host` would mean every project shipped its meshes
    // into the engine's install. The engine's own content directory is for
    // shaders and the message catalog, which are the engine's.
    //
    // `scriptPath` is a directory when it is a project root and a file when
    // it is a bare script (engine.h), so a lone script gets the engine's
    // content directory -- which is right: it has no project to have one.
    std::error_code pathError;
    const bool isProject = !options.scriptPath.empty() && std::filesystem::is_directory(options.scriptPath, pathError);
    const std::filesystem::path contentRoot = isProject ? options.scriptPath / "content" : platform::paths().contentDir;
    meshLoader.setContentRoot(contentRoot);
    // What `asset://` completes to in the script editor.
    if (isProject)
        setCompletionAssetRoot(contentRoot);

    contentMounts.clear();
    if (!isProject)
        contentMounts.mountDirectory(contentRoot);

    // **The content directory is the asset manager, and a scene is one of the
    // assets in it** (human decision, 2026-08-22). The browser shows the same
    // root the engine mounts -- the SOURCE tree, not the packed archive, which
    // is what a person authors and what `ContentMounts` already resolves over
    // the pack for exactly this reason.
    if (options.editor)
        editor.openContent(contentRoot);

    // **Mounted, compiled and mounted again, in one call** (E9 step 14).
    // `openProjectContent` puts the source tree down, compiles whatever has no
    // compiled form, and puts the object store above it -- which is the whole of
    // assumption 3: a project cloned from git works with no command.
    //
    // **In every host mode, not only the editor.** While the loose feed existed
    // this could be an editor convenience, because a headless run fell back to
    // parsing the `.gltf`. The cut-over deleted that fallback, so this is now
    // the only thing that puts a mesh in front of a project that has not been
    // built -- and a conformance run, a replay and a capture gate all open
    // projects exactly this way.
    if (isProject) {
        const ContentImportReport compiled = openProjectContent(options.scriptPath, contentRoot, contentMounts);
        // **Only when it did something.** Every re-open of a project would
        // otherwise announce the same totals, because the counts are reported
        // identically on a cache hit -- so the line would be there every time
        // and mean nothing any time.
        if (compiled.cacheMisses > 0) {
            const std::array<core::I18nArg, 2> args{
                core::I18nArg{"meshes", static_cast<core::i64>(compiled.meshes)},
                core::I18nArg{"textures", static_cast<core::i64>(compiled.textures)}};
            core::log(LogLevel::Info, ENG_TR("app.info.import_compiled"), args);
        }
        if (!compiled.failed.empty()) {
            // Named and survivable in the sense that matters: the source file is
            // still there to be fixed, and saying which one it was and what the
            // compiler objected to is the whole value of this line. What it is
            // NOT any more is cosmetic -- a mesh the compiler refused has no
            // other way in.
            const std::array<core::I18nArg, 2> args{core::I18nArg{"name", compiled.failed.front()},
                                                    core::I18nArg{"detail", compiled.diagnostic}};
            core::log(LogLevel::Warn, ENG_TR("app.warn.import_compile"), args);
        }

        const std::filesystem::path pack = options.scriptPath / ".engine" / "content.lpack";
        if (std::filesystem::exists(pack, pathError)) {
            if (auto mountError = contentMounts.mountPack(pack); mountError.has_value()) {
                // Named and survivable: the object store below it stands, so a
                // broken build is a message and the editor's own compiled
                // content rather than a world with no meshes in it.
                core::logText(LogLevel::Warn, mountError->message);
            }
            else {
                const std::array<core::I18nArg, 1> mountArgs{core::I18nArg{"path", pack.string()}};
                core::log(LogLevel::Info, ENG_TR("app.info.pack_mounted"), mountArgs);
            }
        }
    }
    meshLoader.setContentMounts(&contentMounts);

    // **Terrain's own loader**, beside the one it shares a cache and a library
    // with. Separate because nothing about it reads a file: the geometry is
    // computed from a field that is already in memory, so none of `MeshLoader`'s
    // mounts, budgets or failure memory means anything here.
    render::TerrainLoader terrainLoader;
    // The block world's chunks (V1), meshed and uploaded the same way.
    render::VoxelLoader voxelLoader;
    // Particles (F2): simulated on the frame, because they are a picture.
    render::ParticleSystem particles;
    // Foliage over terrain (ADR 0116), grown per tile around the camera.
    render::FoliageSystem foliage;
    foliage.setSettings(
        {.density = options.foliageDensity, .shadowDistance = options.foliageShadowDistance, .growthsPerSync = 8});
    // **A `Sky`'s six pictures** (ADR 0096), read and resampled off the frame
    // thread; the previous sky draws until a new one is ready. A headless run
    // waits for it instead, for the reason the texture loader does: a capture
    // records the frame it was told to, not the frame the sky arrived by.
    render::SkyLoader skyLoader;
    skyLoader.setContentRoot(contentRoot);
    skyLoader.setContentMounts(&contentMounts);
    skyLoader.setSynchronous(options.headless);
    // **The editor reads its textures off the frame; everything else does not**
    // (D118). A decode is 14 to 36 ms for an ordinary 1024-square PNG, and the
    // synchronous path loads every missing map it finds in one frame -- so
    // giving a part a four-map material froze the frame after the write for a
    // tenth of a second, which is how a tool that measures fine comes to feel
    // like it reloads the world whenever you touch it.
    //
    // Not turned on for a headless run, a capture or a screenshot, and that is
    // the decision rather than an oversight: those record the frame they were
    // told to record, and a texture arriving two frames later is a different
    // picture. They want the loader finished before the frame is. An editor
    // wants the frame finished before the loader is.
    meshLoader.setDeferredTextures(!options.headless);
    // **And meshes, one per frame** (D125). A parse is 191 ms for the model E9
    // opened for, and `sync` had no budget at all -- five models dropped in was
    // one frame of about a second, in the editor and mid-play alike.
    //
    // `!options.headless` rather than `options.editor`, for both of these. A
    // shipping game hitches on exactly the same code, and `--headless` is what
    // every capture, screenshot and determinism driver runs under -- so no
    // golden can observe the deferred path, and the product gets the fix
    // instead of only the tool. (`options.editor && !options.headless` is
    // identically `options.editor`: `main.cpp` refuses the two together.)
    meshLoader.setDeferredMeshes(!options.headless);
    // And the UI's pictures, for the same reason and by the same predicate: an
    // `ImageLabel` names the same kind of PNG a material does, and the
    // synchronous path decoded every one a frame newly named, in that frame,
    // with no bound at all.
    uiText.setDeferredImages(!options.headless);
    // The same mounts the meshes come from, so `TextLabel.Font` can name a face
    // out of the project the same way `MeshPart.MeshContent` names a model.
    uiText.setMounts(&contentMounts);
    // An `ImageLabel` showing `view://<name>` shows what that view draws.
    uiText.setViewLookup([&viewHost](std::string_view name, UiText::ViewPicture& out) {
        const ViewHost::View* view = viewHost.find(name);
        if (view == nullptr || !view->texture.valid())
            return false;
        out = UiText::ViewPicture{view->texture, view->width, view->height};
        return true;
    });

    if (isProject) {
        // Mounted after the pack so a loose chunk overrides a built one,
        // which is the same dev-mode override rule the content directory
        // gets, and the index sits beside them both.
        const std::filesystem::path built = options.scriptPath / ".engine" / "content";
        if (std::filesystem::is_directory(built, pathError)) {
            contentMounts.mountDirectory(built);
        }
        if (!platform::initIo()) {
            core::log(LogLevel::Warn, ENG_TR("app.warn.io_unavailable"), {});
        }
        (void)streaming.load(contentMounts, options.scriptPath / ".engine" / "content.chunks.json");
    }

    const auto ensureDebugPass = [&](rhi::TextureFormat colorFormat) {
        // Gated on "can this device take shaders", not on "will pixels come
        // out". They are different questions, and conflating them made the
        // capture backend -- the blocking render gate -- record a frame with no
        // draws in it at all, which is precisely the thing it exists to notice.
        if (debugPassAttempted || device->caps().shaderFormat == rhi::ShaderFormat::Unknown)
            return;
        debugPassAttempted = true;

        if (auto error = shaders.load(platform::paths().contentDir, device->caps().shaderFormat); error.has_value()) {
            core::logText(LogLevel::Warn, error->message);
            return;
        }
        if (auto error = debugRenderer.create(*device, shaders, colorFormat); error.has_value())
            core::logText(LogLevel::Warn, error->message);

        // Built beside the debug one and, like it, not required: a machine
        // whose content directory is missing the 2D shader boots and draws the
        // world with no UI over it, which is a better failure than refusing to
        // start.
        if (auto error = uiRenderer.create(*device, shaders, colorFormat); error.has_value())
            core::logText(LogLevel::Warn, error->message);

        // The real renderer is built beside the debug one and neither is
        // required. A machine whose content directory has the debug shader and
        // not the PBR set boots and draws wire boxes, which is a far better
        // failure than refusing to start.
        if (auto error = meshCache.create(*device); error.has_value()) {
            core::logText(LogLevel::Warn, error->message);
            return;
        }
        renderer = render::createDefaultRenderer();
        // Before `create`, because the shadow atlas is sized by the settings and
        // building it twice on the first frame would be a wasted allocation the
        // size of the whole map.
        renderer->setSettings(options.graphics);
        if (auto error = renderer->create(*device, shaders, colorFormat); error.has_value()) {
            core::logText(LogLevel::Warn, error->message);
            renderer.reset();
        }
#if ENG_DEBUG_UI
        if (renderer != nullptr) {
            // Beside the editor in a package, in host-tools in a build tree.
#if defined(_WIN32)
            constexpr std::string_view compilerName = "shadercross.exe";
#else
            constexpr std::string_view compilerName = "shadercross";
#endif
            std::filesystem::path compiler = platform::paths().executableDir / compilerName;
            std::error_code missing;
            if (!std::filesystem::exists(compiler, missing))
                compiler = std::filesystem::path(ENG_DEV_SHADERCROSS);
            const std::filesystem::path user = platform::paths().userDir;
            const std::filesystem::path cache =
                !options.surfaceCache.empty()
                    ? options.surfaceCache
                    : (user.empty() ? std::filesystem::temp_directory_path(missing) : user) / "surface-cache";
            surfaceCompiler = std::make_unique<SurfaceCompiler>(
                contentMounts, compiler, platform::paths().contentDir / "shaders" / "include", cache);
            renderer->setSurfaceSource(surfaceCompiler.get());
        }
#else
        if (renderer != nullptr) {
            packSurfaces = std::make_unique<render::PackSurfaceSource>(contentMounts);
            renderer->setSurfaceSource(packSurfaces.get());
        }
#endif
    };

    FrameScheduler scheduler;

    // The world and the VM. Booted before the loop because every entry script's
    // first resumption is a deferred callback, and the first drain is inside the
    // first tick -- so a script that fails to compile says so here rather than
    // one frame later.
    //
    // Held by pointer rather than by value because a hot reload replaces it
    // wholesale (ADR 0024, `reload.h`): everything above this line -- the
    // window, the device, the renderer, the shader cache -- outlives the swap,
    // and that is what "engine-side content survives" means in C++.
    //
    // The bag outlives the host by construction: it is what a reload carries
    // across, and the host is what a reload destroys (ADR 0024).
    script::ReloadState reloadState;

    // **The scene, if the project has one** (ADR 0047), chosen here and applied
    // by `WorldHost::boot` BEFORE it starts the scripts -- which is the order
    // the ADR describes and `scene_file.h` states, and which D067 is the cost
    // of having had backwards.
    //
    // **Which scene opens, in order: what this person had open, what the
    // project declares, then nothing.** The first is per-person state in
    // `.engine/`; the second is `[project] scene` in `project.toml`, which is the
    // decision a project makes about what a RUN of it starts with. Nothing is
    // an untitled world, which is what an editor opened on an empty project
    // should be.
    //
    // A project with no scene file is not an error and never logs one. Every
    // example before `06-scene` is exactly that.
    std::string sceneRelative;
    if (options.editor)
        sceneRelative = Editor::recallOpenScene(options.scriptPath / ".engine");
    if (sceneRelative.empty() || !platform::fileExists(contentRoot / std::filesystem::path(sceneRelative)))
        sceneRelative = options.startupScene;

    // **From the content pack when there is no file**: a built game ships its
    // `content/` as a pack, and the scene -- with the project's scripts in it
    // (ADR 0092) -- is in the pack and nowhere else.
    const auto packed = [&contentMounts](const std::string& relative) -> std::optional<std::string> {
        std::string urn = "asset://" + relative;
        std::replace(urn.begin(), urn.end(), '\\', '/');
        const asset::ResolvedContent found = contentMounts.resolve(urn);
        if (found.source != asset::ResolvedContent::Source::Pack)
            return std::nullopt;
        return std::string(reinterpret_cast<const char*>(found.bytes.data()), found.bytes.size());
    };

    std::filesystem::path bootScene;
    std::string bootSceneText;
    if (!options.scriptPath.empty() && !sceneRelative.empty()) {
        const std::filesystem::path candidate = contentRoot / std::filesystem::path(sceneRelative);
        if (platform::fileExists(candidate)) {
            bootScene = candidate;
        }
        else if (std::optional<std::string> text = packed(sceneRelative); text.has_value()) {
            bootScene = candidate;
            bootSceneText = std::move(*text);
        }
        else {
            sceneRelative.clear();
        }
    }

    // **The game's own file** (ADR 0105): what is authored under
    // `GlobalScriptService` and is not code, from the content root or the pack.
    std::string bootGlobalText;
    if (!options.scriptPath.empty()) {
        if (const std::filesystem::path global = contentRoot / "global.json"; platform::fileExists(global))
            (void)platform::readTextFile(global, bootGlobalText);
        else if (std::optional<std::string> text = packed("global.json"); text.has_value())
            bootGlobalText = std::move(*text);
    }

    // **A conformance run's own content**, when the suite ships some: the specs
    // that load a material asset (ADR 0090) need a file to load, and a spec
    // tree is not a project, so nothing above mounted it.
    if (!options.conformanceRoot.empty()) {
        std::error_code specContentError;
        const std::filesystem::path specContent = options.conformanceRoot / "content";
        if (std::filesystem::is_directory(specContent, specContentError))
            contentMounts.mountDirectory(specContent);
    }

    WorldHostOptions worldOptions{
        .projectPath = options.scriptPath,
        .seed = options.worldSeed,
        .fixedTimestep = scheduler.timing().fixedDt,
        .reloadState = &reloadState,
        .isReload = false,
        .headless = options.headless,
        .preserved = nullptr,
        .conformanceRoot = options.conformanceRoot,
        // **The scene meets the grid here** (ADR 0053), which is the one moment
        // that has the registries and does not yet have a world. What comes
        // back is the scene to apply; the cells go straight into the streaming
        // host, beside whatever a generator already built.
        .partitionScene =
            [&streaming, &fields, &options, contentRoot](scene::World& registries, const std::filesystem::path& scene) {
                if (!options.editor && !options.writeTypesOnly) {
                    // Not in the editor, and that is a decision rather than an
                    // omission: the editor holds the whole world because holding it
                    // is what editing it means. Streaming while editing is a scene
                    // that is a folder of cells, and that is a wall far past this
                    // one.
                    const asset::ChunkIndex* built = streaming.active() ? &streaming.index() : nullptr;
                    const PartitionOutcome outcome =
                        partitionProject(registries, options.scriptPath, contentRoot, scene, built);
                    if (outcome.active) {
                        const auto inCache =
                            [&outcome](const asset::ChunkIndexEntry& entry) -> std::optional<std::filesystem::path> {
                            return outcome.directory / std::filesystem::path(entry.urn);
                        };
                        if (outcome.partsActive)
                            (void)streaming.addIndex(outcome.index, inCache);
                        if (!outcome.fieldIndex.chunks.empty())
                            fields.setIndex(outcome.fieldIndex, inCache);
                        return outcome.scenePath;
                    }
                }
                return std::filesystem::path{};
            },
        // A scene changed at run time lets every streamed cell go first, and
        // is then partitioned by the lambda above (audit A4).
        .resetStreaming =
            [&streaming, &fields] {
                streaming.reset();
                fields.reset();
            },
        // Where a stamp's text comes from. `contentRoot` is what the editor's
        // browser is rooted at too, so a scene names the same file whichever of
        // the two loads it.
        // A file first, and the pack when there is none -- the same rule the
        // scene is found by.
        .bootStamps = [contentRoot, packed](std::string_view stamp) -> std::optional<std::string> {
            // A stamp's name is a scene file's word, and under `content/` or
            // nothing (audit F5).
            const std::optional<std::filesystem::path> file = core::resolveUnder(contentRoot, stamp);
            if (!file.has_value())
                return std::nullopt;
            std::string text;
            if (platform::readTextFile(*file, text))
                return text;
            return packed(std::string(stamp));
        },
        .bootScene = bootScene,
        .bootSceneText = bootSceneText,
        .bootGlobalText = bootGlobalText,
        .bootScenePath = sceneRelative,
        // A scene named at run time (ADR 0106), found as the boot scene is: a
        // file first, and the pack when there is none.
        .readContent = [contentRoot, packed](std::string_view relative) -> std::optional<std::string> {
            // Under `content/` or nothing (audit F5).
            const std::optional<std::string> safe = core::safeRelativePath(relative);
            if (!safe.has_value())
                return std::nullopt;
            std::string text;
            if (platform::readTextFile(contentRoot / std::filesystem::path(std::u8string(safe->begin(), safe->end())),
                                       text))
                return text;
            return packed(*safe);
        },
        .defaultServer = options.defaultServer,
        // **The editor mounts and does not start** (ADR 0058). Every other way
        // of running this binary starts scripts at boot exactly as it always
        // did, and that asymmetry is the whole decision: a tool shows the world
        // it was given, and behaviour begins when somebody presses play.
        .startScripts = !options.editor && !options.writeTypesOnly,
        .networkTopology = static_cast<scene::NetworkTopology>(options.network.topology),
        .maxSubWorlds = options.maxSubWorlds,
        .saveDirectory = options.saveDirectory,
        .saveMaxSlotBytes = options.saveMaxSlotBytes,
        .saveMaxSlots = options.saveMaxSlots,
        .sceneCloseGrace = options.sceneCloseGrace,
        .scriptMemoryMb = options.scriptMemoryMb,
        .developer = options.developerWarnings,
        // A prepared scene's meshes (ADR 0125), through the loader that draws
        // them. Headless, nothing loads meshes, so nothing is warmed.
        .warmContent = options.headless ? std::function<void(scene::World&, const std::vector<std::string>&)>{}
                                        : [&meshLoader](scene::World& world, const std::vector<std::string>& names) {
                                              std::vector<core::NameAtom> meshes;
                                              std::vector<core::NameAtom> images;
                                              for (const std::string& name : names) {
                                                  if (name.ends_with(".gltf") || name.ends_with(".glb"))
                                                      meshes.push_back(world.atoms().intern(name));
                                                  else if (name.ends_with(".png") || name.ends_with(".jpg") ||
                                                           name.ends_with(".jpeg") || name.ends_with(".ktx2"))
                                                      images.push_back(world.atoms().intern(name));
                                              }
                                              meshLoader.warmMeshes(meshes);
                                              meshLoader.warmTextures(images);
                                          },
        .warmedContent = options.headless
                             ? std::function<std::optional<bool>(scene::World&, std::string_view)>{}
                             : [&meshLoader, &meshLibrary, &textureLibrary](scene::World& world,
                                                                            std::string_view name) {
                                   return meshLoader.warmed(world.atoms().intern(name), meshLibrary, textureLibrary);
                               },
    };

    auto host = std::make_unique<WorldHost>();
    // **Before `boot`, and that is load-bearing.** `syncSkeletons` runs at the
    // top of the FIRST tick, and a rig that arrived one tick late would be a
    // character that starts a replay in its bind pose -- which a determinism
    // trace would record as a different world.
    host->setContentMounts(&contentMounts);
    host->setMaterialLibrary(&materialLibrary);
    editor.setMaterialLibrary(&materialLibrary);
    if (std::optional<core::EngineError> bootError = host->boot(worldOptions); bootError.has_value())
        return bootError;

    // **The migration capture** (ADR 0047). The scripts have run their file
    // scope in the boot drain, so the world is exactly what they build; writing
    // it here and returning is the whole of the command. Through `writeScene`
    // and `platform::writeTextFile` because that is what the editor's Save is
    // made of -- a second writer would be a second answer to "what is in a
    // scene".
    if (!options.saveScenePath.empty()) {
        scene::SceneIoReport report;
        const std::string text = scene::writeScene(host->world(), &report);
        if (!platform::createDirectories(options.saveScenePath.parent_path()) ||
            !platform::writeTextFile(options.saveScenePath, text)) {
            return core::makeError(ENG_TR("scene.err.scene_unwritable"));
        }
        const core::I18nArg args[] = {{"count", static_cast<core::i64>(report.instances)},
                                      {"path", options.saveScenePath.string()}};
        core::log(core::LogLevel::Info, ENG_TR("scene.info.scene_written"), args);
        return std::nullopt;
    }

    // **Partitioned and nothing else** (ADR 0053). The boot above has already
    // done the work through the hook; there is no second code path, which is
    // the whole point of putting it there. This just declines to run a frame.
    if (options.partitionOnly)
        return std::nullopt;

    // **The scene's tree as types, and nothing else** (ADR 0078).
    if (options.writeTypesOnly) {
        if (!isProject || !writeSceneDefinitions(host->world(), options.scriptPath))
            return core::makeError(ENG_TR("engine.cli.err.types_unwritable"));
        const core::I18nArg args[] = {{"path", (options.scriptPath / ".engine" / "types" / "scene.d.luau").string()}};
        core::log(core::LogLevel::Info, ENG_TR("engine.cli.info.types_written"), args);
        return std::nullopt;
    }

    // **The posture's one door** (ADR 0070, clause 2): argument parsing chose a
    // topology, and this is the only line in the engine that can turn that into
    // a socket. Solo builds nothing and pays one null check a tick.
    // **One owner for the connection** (ADR 0106): the command line's posture
    // is its `start`, and a script's `Join`, `Host` and `Disconnect` are carried
    // out by its `update` at the safe point after the frame's ticks.
    NetworkSession network([&host]() { return host.get(); }, options.network);
    if (options.network.topology != replication::Topology::Solo) {
#if ENG_ENABLE_REPLICATION
        if (std::optional<core::EngineError> networkError =
                network.start(options.network.topology, options.network.address, options.network.port);
            networkError.has_value())
            return networkError;
        if (options.network.topology == replication::Topology::Replica) {
            const core::I18nArg args[] = {{"address", options.network.address},
                                          {"port", static_cast<core::i64>(options.network.port)}};
            core::log(core::LogLevel::Info, ENG_TR("net.info.joining"), args);
        }
        else {
            const core::I18nArg args[] = {{"port", static_cast<core::i64>(options.network.port)}};
            core::log(core::LogLevel::Info, ENG_TR("net.info.hosting"), args);
        }
#else
        return core::makeError(ENG_TR("engine.cli.err.no_replication"));
#endif
    }

    // **The husk contract's one question**, asked of whichever world is live
    // when something leaves (architecture.md §4): through the reference to
    // `host` rather than the object, because a reload replaces the host.
    const auto held = [&host](core::InstanceId id) { return host != nullptr && host->instanceHeld(id); };
    streaming.setReferenceProbe(held);

    // **A terrain saved as cells** (ADR 0087): adopted by the frame loop, and
    // written by the editor's save through the same object.
    TerrainCells terrainCells(fields, contentRoot);
    editor.setTerrainSaver(
        [&host, &terrainCells](scene::World& world, const std::filesystem::path& scenePath, std::string& note) {
            return terrainCells.save(world, host->workspace(), scenePath, note);
        });
    // **And the scripts' files** (ADR 0105): what a paste, a drag or a rename
    // did under a script service, written into `src/` -- only for the world
    // being edited, never a play session's and never a stamp's.
    editor.setScriptFileSaver([&host, &editor](scene::World& world, const std::filesystem::path& scenePath) {
        if (&world != &host->world() || editor.inPlayMode())
            return std::string{};
        std::string scene = scenePath.filename().string();
        if (constexpr std::string_view Suffix = ".scene.json"; scene.ends_with(Suffix))
            scene.resize(scene.size() - Suffix.size());
        return app::syncScriptFiles(*host, scene).summary();
    });

    LiveCharacterReplay characterReplay(host);
    network.setReferenceProbe(held);
    network.setCharacterReplay(&characterReplay);

    // The editor is told which scene the world holds, so its save writes back
    // to that one rather than refusing for want of an open scene.
    if (options.editor && host->bootSceneApplied())
        editor.adoptOpenScene(sceneRelative);
    editor.setGlobalUnreadable(host->globalUnreadable());
    // **The project's tree as types, from the moment it opens** (ADR 0078), so
    // a script editor pointed at this project types `workspace.Player` before
    // anybody has saved. Every Save rewrites it.
    if (options.editor && isProject)
        (void)writeSceneDefinitions(host->world(), options.scriptPath);

    // `game`, which is where the tree the explorer walks starts. Re-pointed
    // after every reload, because a reload destroys this world and builds
    // another (ADR 0024).
    if (overlay.has_value()) {
        overlay->setInspectionTarget(&host->world(), host->runtime().dataModel(), &inspector);
        overlay->setScriptTarget(&host->runtime());
        // Not re-pointed on a reload the way the world is: the streaming host
        // outlives every world this process builds, which is the whole reason
        // `setWorld` is idempotent.
        overlay->setStreamingTarget(&streaming);
        overlay->setViews(&viewHost);
        if (!options.editorDrive.empty() && editorDrive.load(options.editorDrive))
            overlay->setDrive(&editorDrive);
        // Re-pointed on every reload, unlike streaming: the mixer belongs to the
        // `WorldHost` a reload destroys. What the properties grid does with it is
        // audition a `Content`, which is the one thing that has to work
        // while the world is not ticking at all.
        overlay->setAudioTarget(&host->audio());
        // After the console sink is installed, so the shell chains to it rather
        // than replacing it (D017).
        overlay->captureLog();
    }

    // The dev-server connection, if `ludwerk dev` started this process. Nothing
    // listens here: the engine dials out (ADR 0035), and the whole path is
    // absent when the flag is.
    DevControl control;
    std::vector<DevCommand> commands;
    // `sample` answers once the world has advanced, so the request outlives the
    // frame that received it.
    struct PendingSample
    {
        u64 id = 0;
        u64 tick = 0;
    };
    std::vector<PendingSample> pendingSamples;

    if (!options.devControlUrl.empty()) {
        if (std::optional<core::EngineError> attachError = control.start({
                .url = options.devControlUrl,
                .token = options.devControlToken,
            });
            attachError.has_value())
            return attachError;

        const std::array<I18nArg, 1> attached{I18nArg{"url", options.devControlUrl}};
        core::log(LogLevel::Info, ENG_TR("engine.dev.info.attached"), attached);
    }

    const auto replyOk = [&control](std::string_view type, u64 id, const auto& fill) {
        core::JsonWriter writer;
        writer.beginObject();
        writer.field("type", type);
        writer.field("id", id);
        fill(writer);
        writer.endObject();
        control.post(writer.text());
    };
    // Every `sample` whose tick the world has reached, answered with the tick
    // it is at (D214).
    const auto answerSamples = [&] {
        if (pendingSamples.empty())
            return;
        const u64 tick = host->world().engineState().tick;
        const u64 hash = host->world().worldHash();
        std::erase_if(pendingSamples, [&](const PendingSample& sample) {
            if (sample.tick > tick)
                return false;
            replyOk("sample", sample.id, [tick, hash](core::JsonWriter& writer) {
                writer.field("tick", tick);
                writer.field("hash", hash);
            });
            return true;
        });
    };

    render::RenderWorld snapshot;

    auto headlessStepNs = static_cast<u64>(std::ceil(scheduler.timing().fixedDt * kNanosPerSecond));
    bool quit = false;
    TextInputFocus textInputFocus;
    FrameClock frameClock;

    while (!quit) {
        if (options.frames != 0 && scheduler.totalFrames() >= options.frames)
            break;

        // The FrameStart safe point for `PhysicsService.FixedTimestep`
        // (api-design.md §2.1). Here and nowhere else: the accumulator below,
        // the `task` timer wheel and the solver all read the tick, and a value
        // that changed between two of those reads inside one frame is a class of
        // bug worth designing out rather than debugging. A script's write lands
        // in `requestedFixedTimestep` and takes effect on the frame after it.
        if (const f64 requested = host->world().engineState().requestedFixedTimestep;
            requested != scheduler.timing().fixedDt) {
            scheduler.setFixedDt(requested);
            host->world().engineState().fixedTimestep = requested;
            headlessStepNs = static_cast<u64>(std::ceil(requested * kNanosPerSecond));
        }

        // A headless run drives a synthetic clock: exactly one fixed step per
        // frame, as fast as the machine goes. Real time would make the tick
        // count -- and therefore the pixels -- depend on how busy the runner
        // was, which is the whole failure mode a golden gate must not have.
        //
        // The ceil is load-bearing. 1/60 s is 16666666.67 ns, and truncating it
        // leaves each frame a fraction short of the accumulator's threshold, so
        // ticks fire on some frames and not others -- deterministically, but
        // not the one-per-frame this comment claims. Rounding up costs 0.3 ns
        // of drift per frame and makes the claim true.
        // A headless run drives the synthetic clock; a headless DEV SESSION does
        // not. The synthetic clock exists so a golden capture does not depend on
        // how busy the runner was, and a dev session has no golden -- what it
        // has is a developer, or a test, watching a world advance. Left
        // synthetic it runs tens of thousands of ticks per second, so "the hash
        // at tick 40" is a tick the world blew past before the request for it
        // finished crossing the socket.
        // A networked session is on the real clock for the dev session's
        // reason and a stronger one: a peer on another machine is ticking in
        // real time, and a server that simulated as fast as it could would be
        // an hour ahead of its players in a minute.
        const bool syntheticClock =
            frameClock.synthetic(options.headless, !options.devControlUrl.empty(), network.active());
        const u64 nowNs = syntheticClock ? scheduler.totalFrames() * headlessStepNs : platform::nowNs();
        if (frameClock.switched())
            scheduler.rebase(nowNs);

        const Frame frame = scheduler.beginFrame(nowNs);

        // The gizmo target is armed BEFORE the ticks, not with the rest of the
        // rendering. `DebugService:DrawLine` is documented as drawing "for one
        // frame", and the handler that calls it runs inside a tick -- so a
        // target armed after the ticks would collect nothing, which is exactly
        // what happened the first time this was written the other way round.
        debugDraw.clear();
        host->setGizmoTarget(&debugDraw);

        // Published between frames, before anything this frame can read one.
        // Derived from the wall clock and therefore never legal in simulation
        // code (R10) -- they exist for a human looking at an overlay.
        host->publishStats({
            .fps = frame.renderDt > 0.0 ? 1.0 / frame.renderDt : 0.0,
            .frameTimeMs = frame.renderDt * 1000.0,
            .drawCalls = static_cast<f64>(frameDrawCalls),
            // Real from M5. It read zero for four milestones because there
            // were no bodies to count; a stat that says zero when it means
            // "not implemented" is the shape of every unbacked property this
            // repository has had to find later.
            .physicsBodies = host->physics() != nullptr ? static_cast<f64>(host->physics()->bodyCount()) : 0.0,
            .luaMemoryKb = static_cast<f64>(lua_totalbytes(host->runtime().state(), 0)) / 1024.0,
            .audioUnderruns = static_cast<f64>(host->audio().stats().underruns),
            .audioVoices = static_cast<f64>(host->audio().stats().activeVoices),
            .audioClipsLoaded = static_cast<f64>(host->audio().stats().clipsLoaded),
            .audioClipsMissing = static_cast<f64>(host->audio().stats().clipsMissing),
            .audioClipsStreamed = static_cast<f64>(host->audio().stats().clipsStreamed),
            .meshLodDraws = static_cast<f64>(frameLodDraws),
            .visibleObjects = static_cast<f64>(frameVisibleObjects),
            .instancedDraws = static_cast<f64>(frameInstancedDraws),
        });

        if (options.frameStats || !options.soakReportPath.empty()) {
            // The WALL clock, not `frame.renderDt`. Headless drives the frame
            // loop from a synthetic 1/60 s step so a golden capture cannot
            // depend on how busy the machine was (M1 Finding 8) -- which makes
            // `renderDt` exactly 16.666667 ms every frame and a perf baseline
            // built on it a measurement of the constant.
            //
            // R10 is not in the way: it forbids SIMULATION reading a wall clock.
            // A profiler is the one thing that has to.
            const core::u64 sampleNs = platform::nowNs();
            if (lastFrameNs != 0) {
                const f64 frameMs = static_cast<f64>(sampleNs - lastFrameNs) / 1'000'000.0;
                frameTimesMs.push_back(frameMs);
                frameSimMs.push_back(phaseSimMs);
                frameWaitMs.push_back(phaseWaitMs);
                // Resident size is read per frame rather than sampled, because
                // the number the gate wants is a PEAK and a peak between two
                // samples is a peak nobody saw.
                // **The PRIMARY focus, in world metres.** The returning-focus
                // check (D066's successor) compares the same place with itself,
                // so it needs to know where the place was -- and it is the
                // FIRST focus rather than a centroid, because a world with two
                // of them has two paths and averaging them describes neither.
                // A world with none reports the origin, which never departs and
                // so never claims a return.
                core::Vec3 focusPosition;
                if (const std::vector<asset::StreamingFocus> foci = streaming.collectFoci(); !foci.empty())
                    focusPosition = core::toVec3(foci.front().position);

                soak.sample({.frameMs = frameMs,
                             // The PREVIOUS frame's pump, because this sample is
                             // taken before this frame's. Off by one frame and
                             // deliberately so: every pump is counted exactly
                             // once, which is the property a histogram needs.
                             .streamingMs = streaming.lastPumpMilliseconds(),
                             .streamingCpuMs = streaming.lastPumpCpuMilliseconds(),
                             .residentBytes = platform::residentBytes(),
                             .instanceCount = static_cast<core::u64>(host->world().instanceCount()),
                             .focus = focusPosition});
            }
            lastFrameNs = sampleNs;
            phaseSimMs = 0.0;
            phaseWaitMs = 0.0;
        }

        // The FrameStart safe point. Overlay edits are applied HERE and not
        // where they were typed (M4 brief, Decision 15): the panel draws at the
        // end of the frame, after the sim has ticked and after `extract`, so a
        // write applied there would land after the tick the drawn frame came
        // from -- the mid-frame mutation the reload below is forbidden for the
        // same reason. One frame of latency on a typed value; a replay that is
        // still a replay.
        //
        // Before the reload, deliberately. A write queued against the outgoing
        // world is dropped by `onWorldChanged` rather than replayed against a
        // world that never issued the ids it names.
        // **Recorded before the write, because undo restores what was there.**
        // A whole frame's queued edits are one step: they were typed in one
        // frame and a person undoing thinks of them as one thing.
        //
        // The key says what counts as ONE edit, and `coalesceKeyFor` owns that
        // question -- it used to be four lines here, which is four lines no
        // test could reach. An open gesture is one drag however many writes it
        // made; without one the old rule still applies, so a caller that never
        // learned about gestures behaves exactly as it did.
        // **What the editor is authoring, which is not always the game's
        // world** (ADR 0049). A stamp opens onto a stage -- a `scene::World` of
        // its own, with a Workspace, a Lighting and nothing else -- and while
        // one is open every panel, every verb, the picker and the renderer look
        // at that instead. The SIMULATION never does: play is refused while a
        // stamp is open, so the game's world simply sits still.
        //
        // **Functions rather than references, and that is not style.** A hot
        // reload REPLACES `host`'s world, and a reference taken before it is a
        // reference into freed memory afterwards -- which is what the first cut
        // of this was, and what `tests/hotreload` caught as a Jolt assertion
        // inside a physics update that had nothing to do with any of it. A
        // stage appearing or going does the same thing one frame later. Asked
        // at the point of use, both answers are always the current ones.
        const auto stageOf = [&]() -> Editor::Stage* { return options.editor ? editor.stage() : nullptr; };
        const auto authored = [&]() -> scene::World& {
            Editor::Stage* const open = stageOf();
            return open != nullptr ? open->world() : host->world();
        };
        const auto authoredRoot = [&]() -> core::InstanceId {
            Editor::Stage* const open = stageOf();
            return open != nullptr ? open->workspace() : host->runtime().dataModel();
        };
        // Where something goes when nothing is selected to put it in: the
        // Workspace, or while a stamp is open the stamp itself -- its stage's
        // Workspace is beside the stamp, where the tree does not show it and
        // the save does not write it (B10).
        const auto defaultParent = [&]() -> core::InstanceId {
            Editor::Stage* const open = stageOf();
            if (open == nullptr)
                return host->workspace();
            const core::InstanceId stamped = editor.stampSession().root;
            return open->world().alive(stamped) ? stamped : open->workspace();
        };

        if (options.editor && inspector.pendingCount() > 0) {
            // **Which property**, so the history -- and the toast an undo shows
            // -- says "Edit CFrame" rather than "Edit".
            const std::span<const PendingWrite> writes = inspector.pending();
            std::string label = "Edit";
            const core::NameAtom first = writes.front().property;
            const bool oneProperty = std::all_of(
                writes.begin(), writes.end(), [first](const PendingWrite& write) { return write.property == first; });
            if (oneProperty && first.valid())
                label += " " + std::string(authored().atoms().text(first));
            editor.history().record(authored(), label, coalesceKeyFor(inspector.gesture(), inspector.pending()));
        }

        // **A typed property is a change to the document**, and it does not come
        // through `EditorCommands` -- it is drained here, which is the one place
        // that knows a write actually landed.
        if (options.editor && inspector.pendingCount() > 0)
            editor.touch();

        // **A mesh given to a `MeshPart` in Properties sizes it** when the part
        // never had a size of its own (`MeshSize` still one): it draws at the
        // mesh's own size and `Size` says what that is, as a mesh dropped in
        // does (`Editor::meshFits`).
        std::vector<core::InstanceId> meshAssigned;
        if (options.editor) {
            const core::NameAtom meshContent = authored().atoms().lookup("MeshContent");
            for (const PendingWrite& write : inspector.pending()) {
                if (meshContent.valid() && write.kind == WriteKind::Property && write.property == meshContent)
                    meshAssigned.push_back(write.target);
            }
        }

        inspector.applyPending(authored());

        for (const core::InstanceId assigned : meshAssigned) {
            const scene::MeshPartComponent* mesh = authored().meshParts().find(assigned);
            if (mesh != nullptr && mesh->meshSize == core::Vec3{1.0f, 1.0f, 1.0f})
                editor.meshFits().push_back(Editor::MeshFit{assigned, std::nullopt, 0});
        }

        // A click resolves here too, and AFTER the drain rather than before:
        // whatever was typed into the old selection lands before the selection
        // becomes something else. Walking the world for a pick is only a read,
        // but doing it here rather than inside the UI callback that noticed the
        // click is what keeps what a click selects independent of where in the
        // panel tree it happened to be handled -- the same discipline the
        // writes above are under, for the same reason.
        //
        // The click was noticed at the END of the previous frame, so a
        // selection is one frame behind the mouse. That is the same frame of
        // latency a typed value already has and it is not felt at sixty hertz.
        if (options.editor) {
            // The shell's buttons, acted on HERE and not where they were
            // pressed: play snapshots the world, stop replaces it and save walks
            // all of it, and none of those may happen while a panel is drawing
            // from the same world.
            if (overlay.has_value()) {
                // Not const: a Save the unsaved-changes dialog asked for is taken off it once
                // done, before the scene change it guarded (see below).
                EditorCommands editorCommands = overlay->takeCommands();
                if (editorCommands.play.has_value()) {
                    if (*editorCommands.play) {
                        const bool wasEditing = editing(editor.runState());
                        editor.play(host->world());
                        // **Play is what starts the scripts** (ADR 0058), and
                        // only the transition does: `Editor::play` refuses a
                        // second press while playing, and starting them again
                        // would run every file scope twice.
                        //
                        // Deferred rather than resumed, exactly as boot defers
                        // them, so the first resumption lands in this frame's
                        // drain and `game.Loaded` is raised after all of them.
                        if (wasEditing && !editing(editor.runState())) {
                            meshLoader.retryMissing();
                            ui::resetInteraction();
                            // Before the scripts start, so their first line is
                            // the first line of the console.
                            if (overlay.has_value())
                                overlay->clearConsole();
                            script::startScripts(host->runtime().state());
                            // **Armed against the chunks play just loaded.**
                            // `startScripts` binds each chunk as it loads, so a
                            // breakpoint set while editing is applied there --
                            // and this covers the rest: a chunk whose breakpoint
                            // was added after it was bound, and the boundLine the
                            // VM answers with, which is the only thing that knows
                            // whether the line has code on it.
                            for (const Breakpoint& bp : scripts.breakpoints()) {
                                const core::u32 bound = host->runtime().debugger().setBreakpoint(
                                    host->runtime().state(), bp.chunk, bp.line + 1);
                                scripts.setBoundLine(bp.chunk, bp.line, bound);
                            }
                        }
                    }
                    else {
                        // **Stop is the game closing** (ADR 0124 §4), as it is for a
                        // player: the open scene's `scene:BindToClose` handlers, then the
                        // game's, the grace period, the saves -- in the world that played,
                        // before the restore replaces it. Not from a breakpoint: the VM is
                        // parked inside a call and cannot run anything else.
                        if (!editor.debuggerParked())
                            host->close();
                        editor.stop(host->world(), inspector);
                        ui::resetInteraction();
                        writeTypedSources(host->world(), scripts, editor);

                        // **And stop throws the VM away** (ADR 0058). The world
                        // is back where play was pressed, which is what the line
                        // above did; what it could not do is undo the
                        // connections, the required modules and the queued
                        // resumptions a play session made -- so a second play
                        // inherited all of them and was not the same as the
                        // first.
                        //
                        // After the restore rather than before, and the order is
                        // load-bearing: the new runtime binds to the tree as it
                        // stands, so it adopts the DataModel and finds the
                        // services the restore put back.
                        // **Before the VM goes**, because a parked coroutine
                        // cannot survive the `lua_State` it lives in and the
                        // chunk references are patched into protos that are
                        // about to be freed. The breakpoints themselves are kept
                        // -- they are keyed on chunk names, which mean the same
                        // thing in the runtime that replaces this one.
                        host->runtime().debugger().detach(host->runtime().state());
                        editor.setDebuggerParked(false);

                        if (std::optional<core::EngineError> restart = host->restartRuntime(); restart.has_value()) {
                            core::logText(core::LogLevel::Error, restart->message);
                        }
                        // Re-pointed for the same reason a reload re-points it:
                        // the VM those referred to no longer exists.
                        if (overlay.has_value())
                            overlay->setScriptTarget(&host->runtime());
                    }
                }
                if (editorCommands.pause.has_value())
                    editor.setPaused(*editorCommands.pause);

                // --- Play with players (ADR 0106 §5) ---------------------
                //
                // The scene is saved first: every window of the match loads
                // the project from disk, and a match of yesterday's scene is
                // not what somebody who pressed Play meant.
                if (editorCommands.match.has_value()) {
                    if (*editorCommands.match && !editor.matchRunning()) {
                        if (editor.sceneDirty())
                            (void)editor.saveOpenScene(host->world());
                        const Editor::MatchSettings& settings = editor.matchSettings();
                        MatchPlan plan;
                        // This very binary: the editor's host plays the match it
                        // was asked to, with the same content beside it.
#if defined(_WIN32)
                        plan.host = platform::paths().executableDir / "engine-host.exe";
#else
                        plan.host = platform::paths().executableDir / "engine-host";
#endif
                        plan.project = options.scriptPath;
                        plan.players = settings.players;
                        plan.dedicated = settings.dedicated;
                        plan.logDirectory = options.scriptPath / ".engine" / "match";
                        std::error_code ec;
                        std::filesystem::create_directories(plan.logDirectory, ec);
                        plan.area = platform::usableDisplayArea();
                        if (match.start(plan)) {
                            editor.setMatchRunning(true);
                            editor.report("started a match of " + std::to_string(match.size()) + " window(s)", false);
                        }
                        else {
                            editor.report("the match could not start: " + plan.host.string(), true);
                        }
                    }
                    else if (!*editorCommands.match && editor.matchRunning()) {
                        match.stop();
                        editor.setMatchRunning(false);
                        editor.report("stopped the match", false);
                    }
                }

                // --- A script made in a script service is a file (ADR 0105)
                //
                // Here rather than in `createInstance`, which has neither the
                // disk nor the mount: the instance it made stands in until the
                // file is written, and the mounted one then takes its place and
                // opens in a tab, which is where somebody who just made a
                // script is going next.
                if (std::optional<Editor::ScriptFileRequest> request = editor.takeScriptFileRequest();
                    request.has_value() && !editor.stampSession().open() && !host->projectRoot().empty()) {
                    scene::World& w = host->world();
                    std::error_code ec;
                    const std::filesystem::path directory = host->projectRoot() / request->directory;
                    std::filesystem::create_directories(directory, ec);
                    // A name the folder does not have yet: `Script`, then
                    // `Script2`, as the Explorer names siblings.
                    std::string stem = request->name.empty() ? std::string("Script") : request->name;
                    // A class other than the folder's own says so in the name
                    // (`Tool.module.luau`), or the next open would make it the
                    // folder's.
                    const bool folderModules = request->root == "src/shared";
                    const std::string kind =
                        request->module == folderModules ? "" : (request->module ? ".module" : ".script");
                    std::filesystem::path file = directory / (stem + kind + ".luau");
                    for (int suffix = 2; std::filesystem::exists(file, ec) && suffix < 1000; ++suffix)
                        file = directory / (stem + std::to_string(suffix) + kind + ".luau");
                    const std::string relative =
                        std::filesystem::relative(file, host->projectRoot(), ec).generic_string();
                    if (!platform::writeTextFile(file, request->source)) {
                        editor.report("could not write " + relative, true);
                    }
                    else if (const core::InstanceId mounted =
                                 host->mountScriptFile(relative, request->root, request->container, request->module);
                             mounted.valid()) {
                        if (w.alive(request->placeholder)) {
                            (void)w.destroy(request->placeholder);
                            w.retireDestroyed();
                        }
                        inspector.select(mounted);
                        inspector.reveal(mounted);
                        scriptToOpen = mounted;
                        editor.report("wrote " + relative, false);
                    }
                }

                if (editor.matchRunning()) {
                    for (const MatchLauncher::Line& line : match.poll()) {
                        const std::array<I18nArg, 2> args{I18nArg{"process", line.process}, I18nArg{"text", line.text}};
                        core::log(core::LogLevel::Info, ENG_TR("engine.info.match_line"), args);
                    }
                    if (!match.running()) {
                        match.stop();
                        editor.setMatchRunning(false);
                    }
                }

                // --- The script editor (ADR 0057) ------------------------
                //
                // Every one of these is here for the reason the block above
                // states: opening a tab reads a property, saving walks a file,
                // and a reload replaces the world -- none of which may happen
                // inside the callback that noticed the click.
                const core::InstanceId opening =
                    editorCommands.openScript.valid() ? editorCommands.openScript : scriptToOpen;
                scriptToOpen = core::InstanceId{};
                if (opening.valid()) {
                    // **`authored()` and not the scene's world.** The Explorer
                    // draws the open stamp's tree while there is one, so the id
                    // that arrives here is a handle into THAT world -- and an id
                    // read against the other one answers about whatever instance
                    // shares the slot. That is a name from the wrong instance, a
                    // `Source` from the wrong instance, and edits written into
                    // it, which is exactly what was reported.
                    scene::World& w = authored();
                    const bool inStamp = editor.stampSession().open();
                    const core::InstanceId id = opening;
                    const std::optional<scene::Value> source = w.getProperty(id, w.atoms().intern("Source"));
                    const auto* text = source.has_value() ? std::get_if<std::string>(&source.value()) : nullptr;

                    // A stamp's script is in no VM and came from no file: its
                    // chunk name is its place in the STAMP's tree, and there is
                    // no mounted path to look up. `scriptChunkName` would ask
                    // the runtime, whose world this is not.
                    const std::string chunk =
                        inStamp ? script::treePathOf(w, id) : script::scriptChunkName(host->runtime().state(), id);
                    const std::string file =
                        inStamp ? std::string{} : std::string(script::mountedPathOf(host->runtime().state(), id));

                    scripts.open(id, inStamp ? ScriptOrigin::Stamp : ScriptOrigin::Scene, chunk, file,
                                 std::string(w.atoms().text(w.name(id))), text != nullptr ? *text : std::string{});
                }

                // **A tab says what its script is called NOW** (the owner's
                // report: a script renamed in the Explorer kept its old name on
                // its tab). Read from the world the tab came from, every frame,
                // because a rename can come from the Explorer, F2, an undo or a
                // script, and the tab should not have to hear about each.
                for (std::size_t tabIndex = 0; tabIndex < scripts.count(); ++tabIndex) {
                    OpenScript* shown = scripts.at(tabIndex);
                    if (shown == nullptr)
                        continue;
                    Editor::Stage* const stage = stageOf();
                    if (shown->origin == ScriptOrigin::Stamp && stage == nullptr)
                        continue;
                    const scene::World& w = shown->origin == ScriptOrigin::Stamp ? stage->world() : host->world();
                    if (!w.alive(shown->instance))
                        continue;
                    const std::string_view now = w.atoms().text(w.name(shown->instance));
                    if (shown->title != now)
                        shown->title = std::string(now);
                    // **And which file it is NOW**: a save that moved it, or
                    // wrote a pasted one's file for the first time (ADR 0105),
                    // changed the answer, and `Ctrl+S` writes where this says.
                    if (shown->origin == ScriptOrigin::Scene) {
                        const std::string_view file = script::mountedPathOf(host->runtime().state(), shown->instance);
                        if (shown->file != file)
                            shown->file = std::string(file);
                    }
                }

                ScriptEditorCommands scriptCommands = overlay->takeScriptCommands();

                // **A console error, clicked** (S5.11). The panel found a
                // `chunk.luau:123` in the line and this is what acts on it: the
                // pane is drawn from a snapshot and cannot open a tab, because
                // opening one reads the world.
                if (scriptCommands.jumpTo.has_value()) {
                    const SourceLocation& at = *scriptCommands.jumpTo;

                    // **A tab that already has the chunk first.** Re-opening
                    // would be a second document over one file, which is two
                    // undo histories editing one thing -- and `ScriptEditor` is
                    // idempotent by INSTANCE, which does not help here because
                    // the console names a chunk and not an id.
                    std::optional<std::size_t> found;
                    for (std::size_t index = 0; index < scripts.count(); ++index) {
                        const OpenScript* tab = scripts.at(index);
                        if (tab != nullptr && tab->chunk == at.chunk)
                            found = index;
                    }

                    // Otherwise the script instance whose chunk it is, opened.
                    // **Asked of the RUNTIME rather than derived here**, because
                    // the chunk name in an error is the one the VM gave the
                    // chunk, and a second way of spelling it is a second way of
                    // being wrong.
                    if (!found.has_value()) {
                        scene::World& w = authored();
                        core::InstanceId subject;
                        w.scripts().forEach([&](core::InstanceId id, const scene::ScriptComponent&) {
                            if (subject.valid())
                                return;
                            if (script::scriptChunkName(host->runtime().state(), id) == at.chunk)
                                subject = id;
                        });

                        if (subject.valid()) {
                            const std::optional<scene::Value> source =
                                w.getProperty(subject, w.atoms().intern("Source"));
                            const auto* text = source.has_value() ? std::get_if<std::string>(&source.value()) : nullptr;
                            scripts.open(subject, ScriptOrigin::Scene, at.chunk,
                                         std::string(script::mountedPathOf(host->runtime().state(), subject)),
                                         std::string(w.atoms().text(w.name(subject))),
                                         text != nullptr ? *text : std::string{});
                            found = scripts.indexOf(subject, ScriptOrigin::Scene);
                        }
                    }

                    if (found.has_value()) {
                        scripts.setActive(*found);
                        if (OpenScript* tab = scripts.at(*found); tab != nullptr) {
                            // Zero-based inside, one-based in a message: every
                            // editor and every error in the world counts lines
                            // from one, and the document counts from zero.
                            const core::u32 line = at.line > 0 ? at.line - 1 : 0;
                            tab->caret.head = Position{line, 0};
                            tab->caret.collapse();
                            tab->caret.desiredColumn = 0;
                            tab->extraCarets.clear();
                            tab->errorLine = line;
                            // Stale on purpose, which is how the pane knows the
                            // caret moved and scrolls to it.
                            tab->shownCaret = Position{~0u, 0};
                        }
                    }
                    else {
                        // Said rather than silent: a click that does nothing is
                        // indistinguishable from a control that does not work.
                        editor.report("nothing open is " + at.chunk, true);
                    }
                }

                // **The text goes into the world as it is typed**, so pressing
                // play runs what is on the screen. `Ctrl+S` is about the FILE,
                // not about the engine -- which is what "an instance is the only
                // thing that runs" buys.
                for (const std::size_t index : scriptCommands.edited) {
                    const OpenScript* tab = scripts.at(index);
                    if (tab == nullptr)
                        continue;
                    // **The world the tab came from**, which is the whole of
                    // `ScriptOrigin`: writing a stamp's script into the scene's
                    // world would put somebody's code on an unrelated instance.
                    Editor::Stage* const stage = stageOf();
                    if (tab->origin == ScriptOrigin::Stamp && stage == nullptr)
                        continue;
                    scene::World& w = tab->origin == ScriptOrigin::Stamp ? stage->world() : host->world();
                    if (!w.alive(tab->instance))
                        continue;
                    (void)w.setProperty(tab->instance, w.atoms().intern("Source"), scene::Value{tab->document.text()});
                    // A stamp's script is the stamp's to save. The scene's are
                    // counted below from their tabs, which know whether the
                    // text is still what was saved; a script that is a file
                    // was never the scene's to save.
                    if (tab->origin == ScriptOrigin::Stamp)
                        editor.touch();
                }
                {
                    bool sceneScriptUnsaved = false;
                    for (std::size_t index = 0; index < scripts.count() && !sceneScriptUnsaved; ++index) {
                        const OpenScript* tab = scripts.at(index);
                        // A file script's tab too: a scene save writes its
                        // file now (`script_files.h`), so quitting with one
                        // changed is work to lose like any other.
                        sceneScriptUnsaved = tab != nullptr && tab->origin == ScriptOrigin::Scene && tab->dirty();
                    }
                    editor.setSceneScriptsUnsaved(sceneScriptUnsaved);
                    bool fileTabUnsaved = false;
                    for (std::size_t index = 0; index < scripts.count() && !fileTabUnsaved; ++index) {
                        const OpenScript* tab = scripts.at(index);
                        fileTabUnsaved = tab != nullptr && tab->origin == ScriptOrigin::File && tab->dirty();
                    }
                    editor.setFileTabsUnsaved(fileTabUnsaved);
                }

                // **A surface shader's compile errors, on its lines** (ADR
                // 0091): what the compiler last said about the file a tab
                // holds, set on the document only when it changes. An error
                // in another file -- an include, the generated wrapper -- is
                // not this tab's to mark.
#if ENG_DEBUG_UI
                if (surfaceCompiler != nullptr) {
                    for (std::size_t tabIndex = 0; tabIndex < scripts.count(); ++tabIndex) {
                        OpenScript* tab = scripts.at(tabIndex);
                        if (tab == nullptr || tab->origin != ScriptOrigin::File ||
                            contentKindOf(tab->file) != ContentKind::Shader)
                            continue;
                        const std::string name = std::filesystem::path(tab->file).filename().string();
                        std::vector<Diagnostic> marks;
                        for (const SurfaceError& problem :
                             surfaceCompiler->errors(std::string(asset::AssetScheme) + tab->file)) {
                            const std::string file = std::filesystem::path(problem.file).filename().string();
                            if (!problem.file.empty() && file != name)
                                continue;
                            Diagnostic mark;
                            mark.at = Position{problem.line > 0 ? problem.line - 1 : 0, 0};
                            mark.message = problem.message;
                            marks.push_back(std::move(mark));
                        }
                        const std::span<const Diagnostic> shown = tab->document.externalDiagnostics();
                        const bool same = std::equal(marks.begin(), marks.end(), shown.begin(), shown.end(),
                                                     [](const Diagnostic& a, const Diagnostic& b) {
                                                         return a.at == b.at && a.message == b.message;
                                                     });
                        if (!same)
                            tab->document.setExternalDiagnostics(std::move(marks));
                    }
                }
#endif

                if (scriptCommands.toggleBreakpointLine.has_value()) {
                    if (const OpenScript* tab = scripts.at(scripts.activeIndex()); tab != nullptr) {
                        const core::u32 line = *scriptCommands.toggleBreakpointLine;
                        script::Debugger& debugger = host->runtime().debugger();
                        // **Both sides, in one place.** The editor holds what a
                        // person asked for and the VM holds what is actually
                        // patched; letting a panel arm one and not the other is
                        // how a marker that never fires happens.
                        if (scripts.toggleBreakpoint(tab->chunk, line)) {
                            // Luau counts lines from one and a document from
                            // zero, and the answer comes back in Luau's.
                            const core::u32 bound =
                                debugger.setBreakpoint(host->runtime().state(), tab->chunk, line + 1);
                            scripts.setBoundLine(tab->chunk, line, bound);
                        }
                        else {
                            debugger.clearBreakpoint(host->runtime().state(), tab->chunk, line + 1);
                        }
                    }
                }

                // Continue, or a step. The debugger is the only thing that may
                // resume a parked coroutine, and this is the safe point.
                switch (scriptCommands.step) {
                case DebugStep::Continue:
                    host->runtime().debugger().resume(host->runtime().state());
                    break;
                case DebugStep::Over:
                    host->runtime().debugger().stepOver(host->runtime().state());
                    break;
                case DebugStep::Into:
                    host->runtime().debugger().stepInto(host->runtime().state());
                    break;
                case DebugStep::Out:
                    host->runtime().debugger().stepOut(host->runtime().state());
                    break;
                case DebugStep::None:
                    break;
                }

                if (scriptCommands.save.has_value()) {
                    const std::size_t index = *scriptCommands.save;
                    if (const OpenScript* tab = scripts.at(index); tab != nullptr) {
                        if (tab->origin == ScriptOrigin::File) {
                            // A content file is its own file, under the content
                            // root; a saved shader is recompiled when the
                            // compiler next looks at its time (ADR 0091).
                            if (platform::writeTextFile(editor.content().root() / std::filesystem::path(tab->file),
                                                        tab->document.text()))
                                scripts.markSaved(index);
                            else
                                editor.report("could not write " + tab->file, true);
                        }
                        else if (tab->origin == ScriptOrigin::Stamp) {
                            // **A stamp's script is saved by saving the STAMP**,
                            // which is the file that holds it. Falling through
                            // to the scene wrote the project's scene instead and
                            // left the script exactly where it was -- reported as
                            // "sometimes it does not save".
                            if (editor.saveStamp(host->world(), host->runtime().dataModel()))
                                scripts.markSavedWhere(ScriptOrigin::Stamp);
                        }
                        else if (tab->file.empty()) {
                            // It lives in the scene, and ONLY it is saved
                            // there (see `Editor::saveSceneScript`) -- unless
                            // that cannot be done, and then the whole scene
                            // is, and every scene tab with it.
                            switch (editor.saveSceneScript(host->world(), tab->instance)) {
                            case Editor::ScriptSave::Script:
                                scripts.markSaved(index);
                                break;
                            case Editor::ScriptSave::Scene:
                                scripts.markSavedWhere(ScriptOrigin::Scene);
                                break;
                            case Editor::ScriptSave::Failed:
                                break;
                            }
                        }
                        else {
                            // **Never over a file changed outside the editor**
                            // since it was read (a second editor, VS Code): the
                            // disk is kept and this text is put beside it, in
                            // `.engine/conflicts/`, so neither is lost.
                            lua_State* state = host->runtime().state();
                            const std::filesystem::path file = options.scriptPath / tab->file;
                            const std::string text = tab->document.text();
                            core::u64 known = 0;
                            for (const script::ModuleRegistry::Entry& entry : script::mountedEntries(state)) {
                                if (entry.path == tab->file)
                                    known = entry.diskHash;
                            }
                            std::string disk;
                            const bool read = platform::readTextFile(file, disk);
                            const core::u64 onDisk = read ? script::scriptTextHash(disk) : 0;
                            if (read && known != 0 && onDisk != known && disk != text) {
                                const std::filesystem::path aside =
                                    options.scriptPath / ".engine" / "conflicts" / std::filesystem::path(tab->file);
                                std::error_code ec;
                                std::filesystem::create_directories(aside.parent_path(), ec);
                                if (platform::writeTextFile(aside, text))
                                    editor.report(tab->file +
                                                      " changed on disk since it was opened -- kept; this "
                                                      "version is in .engine/conflicts/" +
                                                      tab->file,
                                                  true);
                                else
                                    editor.report("could not keep " + tab->file + " aside; nothing was written", true);
                            }
                            else if (platform::writeTextFile(file, text)) {
                                script::setMountedHash(state, tab->file, script::scriptTextHash(text));
                                scripts.markSaved(index);
                            }
                            else {
                                editor.report("could not write " + tab->file, true);
                            }
                        }
                        // **And nothing else happens, which is the whole of it.**
                        //
                        // The first version rebuilt the world here, and every
                        // symptom that followed was a symptom of that: the panels
                        // vanished, the tabs closed, the caret jumped to line one,
                        // the Explorer collapsed, and the screen flashed. Each was
                        // patched in turn and the patches were the tell -- saving a
                        // text file should not destroy a world.
                        //
                        // It never needed to. The pane writes `Source` on the
                        // instance as somebody types, so the world already holds
                        // the new text; and ADR 0058 makes PLAY what starts a
                        // script, compiling `Source` at that moment. There is no
                        // running chunk to refresh, because in the editor there is
                        // no running chunk. Ctrl+S is about the FILE surviving the
                        // editor being closed, and that is a `writeTextFile`.
                    }
                }

                if (scriptCommands.close.has_value()) {
                    // **Closing a changed tab loses nothing**: its text is in
                    // the script already, typed through. The scene is marked
                    // changed so a save writes it and quitting asks.
                    if (const OpenScript* closing = scripts.at(*scriptCommands.close);
                        closing != nullptr && closing->dirty() && closing->origin == ScriptOrigin::Scene)
                        editor.touchAs(false);
                    (void)scripts.close(*scriptCommands.close);
                }

                // **What the panel draws, copied out of the VM.** A view rather
                // than a pointer: the panel outlives a reload and the snapshot
                // does not, and a `const char*` from `lua_getlocal` does not
                // outlive the pause it was read in.
                {
                    const script::DebugSnapshot& vmSnapshot = host->runtime().debugger().snapshot();
                    DebugView& view = overlay->debugView();
                    const bool wasParked = view.parked;
                    view.parked = vmSnapshot.parked;
                    view.chunk = vmSnapshot.chunk;
                    view.line = vmSnapshot.line;
                    view.frames.clear();
                    for (const script::DebugFrame& stackFrame : vmSnapshot.frames) {
                        DebugFrameView out;
                        out.function = stackFrame.function;
                        out.chunk = stackFrame.chunk;
                        out.line = stackFrame.line;
                        const auto copy = [](const std::vector<script::DebugValue>& from,
                                             std::vector<DebugValueView>& to) {
                            for (const script::DebugValue& value : from)
                                to.push_back(DebugValueView{value.name, value.type, value.preview});
                        };
                        copy(stackFrame.locals, out.locals);
                        copy(stackFrame.upvalues, out.upvalues);
                        view.frames.push_back(std::move(out));
                    }
                    // A fresh stop looks at the innermost frame, which is where
                    // execution actually is.
                    if (view.parked && !wasParked)
                        view.selectedFrame = 0;
                    if (view.selectedFrame >= view.frames.size())
                        view.selectedFrame = 0;

                    // The world holds still while a script does. The rule lives
                    // in `allowedTicks` -- see the note there on why it is not a
                    // courtesy.
                    editor.setDebuggerParked(view.parked);

                    // **The tab the stop is in, brought to the front.** Stopping
                    // somewhere somebody cannot see is a debugger they have to
                    // go looking through.
                    if (view.parked && !wasParked) {
                        for (std::size_t index = 0; index < scripts.count(); ++index) {
                            if (scripts.at(index)->chunk != view.chunk)
                                continue;
                            scripts.setActive(index);
                            break;
                        }
                    }
                }

                // **What the unsaved-changes dialog said to save, saved FIRST**:
                // everything unsaved -- the stamp, the scene, the material, a
                // shader's tab. Its Save and the verb it was guarding arrive in
                // one frame, and opening or clearing the scene before the save
                // wrote the new scene over itself, or refused for want of one --
                // the work lost either way. A save that fails stops the verb,
                // so nothing is thrown away because a disk said no.
                if (editorCommands.saveAll) {
                    bool failed = false;
                    if (editor.inPlayMode()) {
                        // The game closes first, as the Stop button's does (ADR 0124).
                        if (!editor.debuggerParked())
                            host->close();
                        editor.stop(host->world(), inspector);
                        // Or the save below writes the text from before play.
                        writeTypedSources(host->world(), scripts, editor);
                        if (std::optional<core::EngineError> restart = host->restartRuntime(); restart.has_value())
                            core::logText(core::LogLevel::Error, restart->message);
                        if (overlay.has_value())
                            overlay->setScriptTarget(&host->runtime());
                    }
                    if (editor.stampSession().open() && editor.stampSession().dirty) {
                        if (editor.saveStamp(host->world(), host->runtime().dataModel()))
                            scripts.markSavedWhere(ScriptOrigin::Stamp);
                        else
                            failed = true;
                    }
                    if (!failed && editor.sceneDirty() && !editor.openScenePath().empty()) {
                        if (editor.saveOpenScene(host->world()))
                            scripts.markSavedWhere(ScriptOrigin::Scene);
                        else
                            failed = true;
                    }
                    if (!failed && editor.materialSession().dirty() && !editor.saveMaterial())
                        failed = true;
                    for (std::size_t index = 0; index < scripts.count() && !failed; ++index) {
                        const OpenScript* tab = scripts.at(index);
                        if (tab == nullptr || tab->origin != ScriptOrigin::File || !tab->dirty())
                            continue;
                        if (platform::writeTextFile(editor.content().root() / std::filesystem::path(tab->file),
                                                    tab->document.text()))
                            scripts.markSaved(index);
                        else
                            failed = true;
                    }
                    if (failed) {
                        editorCommands.openScene.clear();
                        editorCommands.newScene = false;
                        editorCommands.quit = false;
                        editorCommands.newProject = false;
                        editorCommands.openProject = false;
                        editor.report("not everything saved, so nothing was closed -- " + editor.status().message,
                                      true);
                    }
                }

                if (!editorCommands.openScene.empty()) {
                    // Out of play mode first. Loading a scene while playing
                    // would leave the snapshot describing a world that no longer
                    // exists, and stop would restore into it.
                    if (editor.inPlayMode()) {
                        // The game closes first, as the Stop button's does (ADR 0124).
                        if (!editor.debuggerParked())
                            host->close();
                        editor.stop(host->world(), inspector);
                        writeTypedSources(host->world(), scripts, editor);
                        // The same teardown the stop button performs, and for
                        // the same reason: the scene about to be loaded must not
                        // arrive under a VM still holding the last session's
                        // connections (ADR 0058).
                        if (std::optional<core::EngineError> restart = host->restartRuntime(); restart.has_value())
                            core::logText(core::LogLevel::Error, restart->message);
                        if (overlay.has_value())
                            overlay->setScriptTarget(&host->runtime());
                    }
                    // **The same path a game's `LoadScene` takes** for what the
                    // file does not hold (ADR 0106): the scene's own code from
                    // `src/scenes/<scene>/`, and which scene the world says it is.
                    if (editor.openScene(host->world(), editorCommands.openScene, inspector)) {
                        host->remountSceneScripts(editor.openScenePath());
                        host->world().engineState().currentScene = editor.openScenePath();
                    }
                }
                if (!editorCommands.createFolder.empty())
                    (void)editor.content().createFolder(editorCommands.createFolder);

                // **Files from the machine, copied into the folder the browser
                // is looking at.** The picker is shown from here rather than
                // from the panel for the reason every dialog in this editor is:
                // an ImGui callback is inside a frame that has not finished
                // drawing, and a modal opened there owns the loop.
                if (editorCommands.importAssets && !importPending && window != nullptr) {
                    importPending = true;
                    // Remembered across the frames the dialog is open: the
                    // answer arrives from a callback, and by then the menu that
                    // asked is long gone.
                    importParent = editorCommands.importParent;
                    platform::pickFiles(*window, editor.content().root().string(), true,
                                        [&importedPaths, &importPending](std::vector<std::filesystem::path> chosen) {
                                            importedPaths = std::move(chosen);
                                            importPending = false;
                                        });
                }

                if (editorCommands.pickHeightmap && !heightmapPending && window != nullptr) {
                    heightmapPending = true;
                    platform::pickFiles(
                        *window, editor.content().root().string(), false,
                        [&heightmapPicked, &heightmapPending](std::vector<std::filesystem::path> chosen) {
                            heightmapPicked = std::move(chosen);
                            heightmapPending = false;
                        });
                }
                if (!heightmapPicked.empty()) {
                    editor.setHeightmapSource(heightmapPicked.front());
                    heightmapPicked.clear();
                }

                // **Files that were missing may be there now**: the content
                // was read again -- an import, a refresh, a folder made.
                if (editor.content().refreshes() != contentRefreshesSeen) {
                    contentRefreshesSeen = editor.content().refreshes();
                    meshLoader.retryMissing();
                }

                if (!importedPaths.empty()) {
                    const ContentTree::ImportReport report = editor.content().import(importedPaths);
                    importedPaths.clear();
                    editor.reportImport(report);

                    // **And compiled, by the same call `assetc` makes**
                    // (E9 step 12). The files are in `content/` either way --
                    // that has not changed and a loose one still resolves -- and
                    // what this adds is the compiled form beside them, in the
                    // project's own object store: BC7 with mips where the loose
                    // path uploads raw RGBA8, and a `.lmesh` where it re-parses
                    // JSON on every launch.
                    //
                    // Companions are not compiled and are not meant to be: a
                    // `.bin` and the images beside a `.gltf` are read BY it, and
                    // compiling the model reads them.
                    const ContentImportReport compiled =
                        compileImported(options.scriptPath, editor.content().root(), report.imported);
                    if (!compiled.failed.empty()) {
                        // Named rather than counted, and not fatal: the loose
                        // file is still there and still resolves, so a source
                        // the compiler refused is slower rather than absent.
                        const std::array<I18nArg, 2> args{I18nArg{"name", compiled.failed.front()},
                                                          I18nArg{"detail", compiled.diagnostic}};
                        engine::core::log(LogLevel::Warn, ENG_TR("app.warn.import_compile"), args);
                    }

                    // **And the instance, when the Explorer is what asked.** A
                    // mesh is the one kind the world has a class for today; a
                    // texture is worn by a material and a sound is played by a
                    // `Sound`, and neither is a thing to put in a tree on its
                    // own. So this creates what it can and the status line has
                    // already said what came in.
                    if (importParent.valid() && authored().alive(importParent)) {
                        const std::string folder = editor.content().currentFolder();
                        const scene::ClassId meshPartClass =
                            authored().classes().findId(authored().atoms().intern("MeshPart"));
                        const scene::ClassId modelClass =
                            authored().classes().findId(authored().atoms().intern("Model"));
                        const core::NameAtom meshContent = authored().atoms().intern("MeshContent");

                        const core::NameAtom materialProperty = authored().atoms().intern("Material");
                        for (const std::string& name : report.imported) {
                            if (contentKindOf(name) != ContentKind::Mesh || meshPartClass == scene::InvalidClass)
                                continue;

                            // `asset://` plus the path under the content root,
                            // which is what a mount resolves and what a scene
                            // stores.
                            const std::string relativeModel = folder.empty() ? name : folder + "/" + name;
                            const std::string urn = "asset://" + relativeModel;
                            // **One material asset per material in the file**
                            // (ADR 0090), and the parts below wear them. One the
                            // file's maps cannot be named for stays in the mesh.
                            const ModelMaterials worn = writeModelMaterials(editor.content().root(), relativeModel);
                            if (!worn.written.empty())
                                (void)editor.content().refresh();
                            const auto wear = [&](core::InstanceId part, const std::string& material) {
                                if (!material.empty())
                                    (void)authored().setProperty(
                                        part, materialProperty,
                                        scene::Value{scene::MaterialRef{"asset://" + material, 0}});
                            };

                            // What the compiler split this file into, if it
                            // split it at all. Read off the rows it produced
                            // rather than derived again here, so the instance's
                            // name, the fragment in its `MeshContent` and the
                            // blob in the store are one answer instead of three.
                            const std::vector<std::string>* pieces = nullptr;
                            for (const auto& [piecesName, fragments] : compiled.pieces) {
                                if (piecesName == name)
                                    pieces = &fragments;
                            }

                            // **One `MeshPart` per primitive, under a `Model`**
                            // (E9 step 12). What an author has in the file is a
                            // body, a mane and a saddle; what the engine gave
                            // them was "horse" -- one part with five submeshes,
                            // nothing to select and nothing to give a material
                            // to.
                            //
                            // A file that split into one piece is still one
                            // part, because a `Model` around a single part is a
                            // container that buys nothing and costs a click.
                            if (pieces != nullptr && pieces->size() > 1 && modelClass != scene::InvalidClass &&
                                editor.createInstance(authored(), modelClass, importParent, authoredRoot(),
                                                      inspector)) {
                                const core::InstanceId model = inspector.selection();
                                // Named after the file rather than after a
                                // piece: the file is what somebody dragged in.
                                authored().setName(
                                    model, authored().atoms().intern(std::filesystem::path(name).stem().string()));

                                for (const std::string& piece : *pieces) {
                                    if (!editor.createInstance(authored(), meshPartClass, model, authoredRoot(),
                                                               inspector)) {
                                        continue;
                                    }
                                    authored().setName(inspector.selection(), authored().atoms().intern(piece));
                                    (void)authored().setProperty(inspector.selection(), meshContent,
                                                                 scene::Value{urn + "#" + piece});
                                    wear(inspector.selection(), worn.ofPiece(piece));
                                }
                                // The MODEL is what a person wants selected
                                // after dropping a model in, not whichever piece
                                // happened to be created last.
                                inspector.select(model);
                                continue;
                            }

                            if (!editor.createInstance(authored(), meshPartClass, importParent, authoredRoot(),
                                                       inspector)) {
                                continue;
                            }
                            // `createInstance` selects what it made, so the
                            // selection IS the thing to point at the file.
                            (void)authored().setProperty(inspector.selection(), meshContent, scene::Value{urn});
                            // A part draws one material; a file that uses
                            // several without splitting keeps its own.
                            wear(inspector.selection(), worn.whole());
                        }
                    }
                    importParent = core::InstanceId{};
                }
                // Before the delete and the duplicate, so a frame that somehow
                // carried both acts on a world the create has already finished
                // with rather than on one halfway through it.
                // The name of what was just made, when it is code: see
                // `scriptToOpen`.
                const auto openIfScript = [&](bool made) {
                    const core::InstanceId id = inspector.selection();
                    if (!made || !authored().alive(id))
                        return;
                    const scene::ClassDescriptor* made_ = authored().classes().find(authored().classOf(id));
                    const std::string_view name =
                        made_ != nullptr ? authored().atoms().text(made_->name) : std::string_view{};
                    if (name == "Script" || name == "ModuleScript")
                        scriptToOpen = id;
                };
                if (editorCommands.createClass != scene::InvalidClass && editorCommands.createParent.valid()) {
                    // A script too, wherever it is made: it is an instance,
                    // and its `Source` is saved with the scene (ADR 0092).
                    openIfScript(editor.createInstance(authored(), editorCommands.createClass,
                                                       editorCommands.createParent, authoredRoot(), inspector));
                }
                // The ribbon's insert: into the selection when it can hold
                // authored things, and into the Workspace the viewport draws
                // otherwise -- the same fallback a placed stamp uses.
                if (!editorCommands.insertClassName.empty()) {
                    const scene::ClassId cls =
                        authored().classes().findId(authored().atoms().lookup(editorCommands.insertClassName));
                    const core::InstanceId primary = inspector.selection();
                    const core::InstanceId parent =
                        primary.valid() && Editor::canParentInto(authored(), primary, authoredRoot()) ? primary
                                                                                                      : defaultParent();
                    if (cls != scene::InvalidClass)
                        openIfScript(editor.createInstance(authored(), cls, parent, authoredRoot(), inspector));
                }
                // **Copied before either verb runs**, because both of them
                // change the selection -- a duplicate selects the copies -- and
                // a span into the inspector would be a span into a vector that
                // has been rewritten underneath it.
                if (editorCommands.deleteSelection || editorCommands.duplicateSelection ||
                    editorCommands.groupSelection || editorCommands.groupAsFolder || editorCommands.ungroupSelection ||
                    editorCommands.reparentTo.valid() || editorCommands.reorderChild.valid()) {
                    const std::vector<core::InstanceId> acting(inspector.selectionSet().begin(),
                                                               inspector.selectionSet().end());
                    if (editorCommands.reorderChild.valid()) {
                        // **Before the reparent, and they are exclusive by
                        // construction**: a drop sets one or the other, decided
                        // by where in the row's height the pointer was. Ordered
                        // rather than left to chance so that a future command
                        // that set both would move the instance once and
                        // predictably rather than twice.
                        (void)editor.reorder(authored(), editorCommands.reorderChild, editorCommands.reorderIndex,
                                             inspector);
                    }
                    else if (editorCommands.reparentTo.valid()) {
                        // With a place when it was dropped on the edge of a row
                        // under another parent: moved in, and put where the
                        // line was drawn.
                        (void)editor.reparent(authored(), acting, editorCommands.reparentTo, authoredRoot(), inspector,
                                              editorCommands.reparentIndex);
                    }
                    if (editorCommands.deleteSelection) {
                        (void)editor.deleteInstances(authored(), acting, authoredRoot(), inspector);
                    }
                    if (editorCommands.duplicateSelection) {
                        (void)editor.duplicateInstances(authored(), acting, authoredRoot(), inspector);
                    }
                    // Group before ungroup, so a frame that somehow carried both
                    // ends with the group taken apart rather than with a group
                    // around what was just freed. Neither key can produce that
                    // pair; the order is stated so it cannot depend on which
                    // `if` came first.
                    if (editorCommands.groupSelection || editorCommands.groupAsFolder) {
                        (void)editor.groupSelection(authored(), acting, authoredRoot(), inspector,
                                                    editorCommands.groupAsFolder);
                    }
                    if (editorCommands.ungroupSelection) {
                        (void)editor.ungroupSelection(authored(), acting, authoredRoot(), inspector);
                    }
                }
                // **Colouring a folder**, from either panel. Which store it
                // lands in is decided by which of the two targets is set --
                // an instance carries its own colour and a directory cannot,
                // and `Editor::setFolderColor` says why.
                if (editorCommands.colorAsked) {
                    if (editorCommands.colorTarget.valid()) {
                        editor.setFolderColor(authored(), editorCommands.colorTarget, editorCommands.color);
                    }
                    else if (!editorCommands.colorContentPath.empty()) {
                        editor.setContentColor(editorCommands.colorContentPath, editorCommands.color);
                        // Written now rather than at exit, for the reason the
                        // open scene is: an editor that only wrote this on a
                        // clean shutdown would forget it the one time somebody
                        // most wants it.
                        editor.rememberState(options.scriptPath / ".engine");
                    }
                }
                // --- Stamps (ADR 0049) ---------------------------------
                if (editorCommands.stampSubject.valid() &&
                    (!editorCommands.stampName.empty() || !editorCommands.stampFolder.empty())) {
                    // A dialog gives the whole name; a drop into the browser
                    // gives the folder and leaves the name to the instance,
                    // because that panel has an id and not a name.
                    const std::string name =
                        !editorCommands.stampName.empty()
                            ? editorCommands.stampName
                            : editorCommands.stampFolder + "/" +
                                  std::string(authored().atoms().text(authored().name(editorCommands.stampSubject)));
                    (void)editor.createStamp(authored(), editorCommands.stampSubject, authoredRoot(), name);
                }
                // A mesh dropped into the world or onto the tree: a `MeshPart`
                // wearing it, standing where the drop's pick landed.
                if (!editorCommands.placeMesh.empty()) {
                    std::optional<core::DVec3> restOn;
                    if (editorCommands.placeMeshPixel.has_value()) {
                        const PickRay ray = editor.rayThrough(*editorCommands.placeMeshPixel);
                        if (const std::optional<PickHit> hit = pickNearest(authored(), authoredRoot(), ray);
                            hit.has_value()) {
                            const auto along = static_cast<core::f64>(hit->distance);
                            restOn = core::DVec3{ray.origin.x + static_cast<core::f64>(ray.direction.x) * along,
                                                 ray.origin.y + static_cast<core::f64>(ray.direction.y) * along,
                                                 ray.origin.z + static_cast<core::f64>(ray.direction.z) * along};
                        }
                    }
                    const core::InstanceId parent =
                        editorCommands.placeMeshParent.valid() &&
                                Editor::canParentInto(authored(), editorCommands.placeMeshParent, authoredRoot())
                            ? editorCommands.placeMeshParent
                            : defaultParent();
                    (void)editor.placeMesh(authored(), editorCommands.placeMesh, parent, authoredRoot(), inspector,
                                           restOn);
                }
                if (!editorCommands.placeStamp.empty()) {
                    // **Under the selection when there is one**, which is what
                    // a person means by placing something while looking at a
                    // folder -- and under `Workspace` otherwise.
                    // A drop names where it landed; the menu item does not,
                    // and falls back to the selection.
                    const core::InstanceId primary = editorCommands.placeStampParent.valid()
                                                         ? editorCommands.placeStampParent
                                                         : inspector.selection();
                    const core::InstanceId parent =
                        primary.valid() && Editor::canParentInto(authored(), primary, authoredRoot()) ? primary
                                                                                                      : defaultParent();
                    (void)editor.instantiateStamp(authored(), editorCommands.placeStamp, parent, authoredRoot(),
                                                  inspector, editorCommands.placeStampLinked);
                }
                if (!editorCommands.assignStampPath.empty()) {
                    // **Inside the stamp when a stamp is open** (D133). A stamp
                    // is written from its root DOWN, so an instance placed
                    // beside it is a reference the file cannot carry: the save
                    // drops it to `null`, and `restamp` pushes that null into
                    // every instance of the stamp in the world. Under the
                    // workspace otherwise. (A material never comes this way any
                    // more: it is an asset a part names by URN, ADR 0090.)
                    //
                    // The SEARCH root stays the whole authored tree either way.
                    // Narrowing it to the stamp would stop the search finding a
                    // material that is already there and place a duplicate.
                    const core::InstanceId placeUnder =
                        editor.stampSession().open() && editor.stampSession().root.valid() ? editor.stampSession().root
                                                                                           : defaultParent();
                    (void)editor.assignStampTo(authored(), authoredRoot(), placeUnder, editorCommands.assignStampPath,
                                               editorCommands.assignStampProperty, inspector.selectionSet());
                }
                if (editorCommands.breakStamp.valid())
                    (void)editor.breakStamp(authored(), editorCommands.breakStamp);
                // **A material is worn by URN** (ADR 0090): nothing is placed in
                // the world, so there is no parent to choose and no stamp
                // boundary for it to fall outside of.
                if (!editorCommands.assignMaterialPath.empty()) {
                    core::InstanceId target = editorCommands.assignMaterialTarget;
                    if (editorCommands.assignMaterialPixel.has_value()) {
                        if (const std::optional<PickHit> hit = pickNearest(
                                authored(), authoredRoot(), editor.rayThrough(*editorCommands.assignMaterialPixel));
                            hit.has_value())
                            target = hit->instance;
                    }
                    if (target.valid()) {
                        const core::InstanceId one[] = {target};
                        (void)editor.assignMaterialTo(authored(), editorCommands.assignMaterialPath, one);
                    }
                    else if (!editorCommands.assignMaterialPixel.has_value()) {
                        (void)editor.assignMaterialTo(authored(), editorCommands.assignMaterialPath,
                                                      inspector.selectionSet());
                    }
                    else {
                        // Dropped on the sky: said, rather than a drop that
                        // did nothing and looked as if it had worked.
                        editor.report("nothing under the pointer to wear " + editorCommands.assignMaterialPath, true);
                    }
                }
                if (!editorCommands.assignSkyboxPath.empty())
                    (void)editor.assignSkybox(authored(), editorCommands.assignSkyboxPath,
                                              editorCommands.assignSkyboxTarget);
                if (!editorCommands.newMaterial.empty()) {
                    if (const std::string made = editor.createMaterial(editorCommands.newMaterial); !made.empty())
                        (void)editor.openMaterial(made);
                }
                if (!editorCommands.newMaterialVariantOf.empty()) {
                    if (const std::string made = editor.createMaterialVariant(editorCommands.newMaterialVariantOf,
                                                                              editorCommands.newMaterialVariantName);
                        !made.empty())
                        (void)editor.openMaterial(made);
                }
                if (!editorCommands.openMaterial.empty())
                    (void)editor.openMaterial(editorCommands.openMaterial);

                // **A content file in the text editor** (ADR 0091): a surface
                // shader, just written from the template or double-clicked.
                // Read here rather than by the panel, which draws a snapshot.
                const auto openContentFile = [&](const std::string& relative) {
                    std::string text;
                    if (!platform::readTextFile(editor.content().root() / std::filesystem::path(relative), text)) {
                        editor.report("could not read " + relative, true);
                        return;
                    }
                    (void)scripts.openFile(relative, std::filesystem::path(relative).filename().string(), text);
                };
                if (!editorCommands.newShader.empty()) {
                    if (const std::string made = editor.createSurfaceShader(editorCommands.newShader); !made.empty())
                        openContentFile(made);
                }
                if (!editorCommands.openFile.empty())
                    openContentFile(editorCommands.openFile);

                // **Which document the frame's mutations belonged to, decided
                // BEFORE anything can swap it.** `touch()` marks the stage when
                // a stamp is open and the scene otherwise, and it runs at the
                // end of this drain -- so a frame that both edited the scene and
                // opened a stamp used to attribute the edit to the stamp, and
                // the scene stayed clean with unsaved work in it.
                const bool mutatedWhileStamped = editor.stampSession().open();

                // **Opening and closing both replace the world**, so they are
                // drained here beside play and stop rather than acted on where
                // they were clicked -- a panel behind this one is still drawing
                // from what they would replace.
                if (!editorCommands.openStamp.empty()) {
                    (void)editor.openStamp(editorCommands.openStamp, host->classes(), host->enums(), host->atoms(),
                                           inspector);
                }
                // **The GAME's world, asked for by name rather than through
                // `authored()`**, which is the stage while a stamp is open. A
                // save moves every linked instance to match the file it just
                // wrote (ADR 0051), and those live in the world the stage is
                // standing in front of.
                if (editorCommands.saveStamp && editor.saveStamp(host->world(), host->runtime().dataModel()))
                    scripts.markSavedWhere(ScriptOrigin::Stamp);
                if (editorCommands.closeStamp) {
                    (void)editor.closeStamp(host->world(), host->runtime().dataModel(), inspector,
                                            editorCommands.closeStampSaving);
                }

                // One question about every verb rather than a flag on each:
                // "did anything change since the last save". `mutatesWorld`
                // rather than `any` because clearing a selection is not a change
                // to the document, and a close confirmation that appeared after
                // pressing Escape is one people learn to dismiss without reading.
                if (editorCommands.mutatesWorld())
                    editor.touchAs(mutatedWhileStamped);

                // --- The clipboard --------------------------------------
                //
                // **Copied before anything is deleted**, because a cut is a
                // copy plus a delete and the delete takes the instances the
                // copy would have read.
                if (editorCommands.copySelection || editorCommands.cutSelection) {
                    const std::vector<core::InstanceId> acting(inspector.selectionSet().begin(),
                                                               inspector.selectionSet().end());
                    editor.copySelection(authored(), acting, authoredRoot());
                    if (editorCommands.cutSelection)
                        (void)editor.deleteInstances(authored(), acting, authoredRoot(), inspector);
                }
                if (editorCommands.paste || editorCommands.pasteInto) {
                    // **Paste puts it BESIDE what is selected and Paste Into
                    // puts it inside**, which is the distinction every editor
                    // with a tree draws and the only one a person has to be
                    // told. With nothing selected both mean the Workspace.
                    const core::InstanceId primary = inspector.selection();
                    core::InstanceId parent = defaultParent();
                    if (primary.valid() && authored().alive(primary)) {
                        parent = editorCommands.pasteInto ? primary : authored().parentOf(primary);
                        if (!parent.valid())
                            parent = defaultParent();
                    }
                    (void)editor.paste(authored(), parent, authoredRoot(), inspector);
                }

                if (editorCommands.renameInstance.valid())
                    (void)editor.renameInstance(authored(), editorCommands.renameInstance, authoredRoot(),
                                                editorCommands.renameInstanceTo);

                // Content actions resolve the entry by path, because the tree
                // may have been re-read between the click and here -- and an
                // index into a list that moved is how a delete hits the row
                // below the one somebody chose.
                // **Made rather than found**, so it is not part of the walk that
                // resolves an existing entry by path: there is nothing there yet.
                if (editorCommands.newStampClass != scene::InvalidClass) {
                    // Into the world AND into the browser: a stamp is a file of
                    // an instance, so making one makes both.
                    // `createStampOfClass` says why when it refuses.
                    (void)editor.createStampOfClass(authored(), authoredRoot(), editorCommands.newStampClass,
                                                    editorCommands.newStampName);
                }
                // Moved by a drag onto a folder, with every reference to it.
                if (!editorCommands.moveContent.empty()) {
                    if (const std::string moved = editor.moveContent(host->world(), editorCommands.moveContent,
                                                                     editorCommands.moveContentInto);
                        !moved.empty())
                        followOpenFiles(scripts, editorCommands.moveContent, moved);
                }
                if (!editorCommands.deleteContent.empty() || !editorCommands.renameContent.empty() ||
                    !editorCommands.duplicateContent.empty()) {
                    const std::string& wanted = !editorCommands.deleteContent.empty() ? editorCommands.deleteContent
                                                : !editorCommands.duplicateContent.empty()
                                                    ? editorCommands.duplicateContent
                                                    : editorCommands.renameContent;
                    for (const ContentEntry& entry : editor.content().entries()) {
                        if (entry.path != wanted)
                            continue;
                        if (!editorCommands.deleteContent.empty()) {
                            (void)editor.content().remove(entry);
                        }
                        else if (!editorCommands.duplicateContent.empty()) {
                            // Named in the status line rather than only counted:
                            // a duplicate is called something, and the next
                            // thing a person does is find it.
                            const std::string made = editor.content().duplicate(entry);
                            editor.report(made.empty() ? "could not duplicate " + entry.name : "duplicated to " + made,
                                          made.empty());
                            // A duplicated scene gets its own copy of its code.
                            if (!made.empty() && made.ends_with(".scene.json")) {
                                const app::ScriptFileSync followed = app::followSceneScripts(
                                    *host, sceneStemOf(entry.path), sceneStemOf(made), /*copy=*/true);
                                if (!followed.problems.empty())
                                    editor.report(followed.summary(), true);
                            }
                        }
                        else {
                            // With every reference to it, as a move is.
                            const std::string before = entry.path;
                            std::string after;
                            if (editor.inPlayMode()) {
                                editor.report("stop the game first -- a file moved during play is one the stop "
                                              "would point everything back away from",
                                              true);
                            }
                            else if (editor.content().rename(entry, editorCommands.renameContentTo, &after) &&
                                     editor.followContent(host->world(), before, after).has_value()) {
                                followOpenFiles(scripts, before, after);
                                // A renamed scene takes its own code with it.
                                if (after.ends_with(".scene.json") && before.ends_with(".scene.json")) {
                                    const app::ScriptFileSync followed = app::followSceneScripts(
                                        *host, sceneStemOf(before), sceneStemOf(after), /*copy=*/false);
                                    if (!followed.problems.empty())
                                        editor.report(followed.summary(), true);
                                    if (editor.openScenePath() == after)
                                        host->world().engineState().currentScene = after;
                                }
                            }
                        }
                        break;
                    }
                }
                // File > Exit. The same door the window's own close button is,
                // so a person who reached for the menu gets the same shutdown --
                // and by the time it arrives here the question about unsaved
                // work has already been asked and answered.
                if (editorCommands.quit)
                    quit = true;

                // **Leaving this project for another one** (ADR 0055). Both
                // start the browser as a new process and close this editor,
                // which is what a project being a PROCESS makes them: a project
                // decides the content mounts, the Luau VM and the layout, all
                // resolved at boot.
                if (editorCommands.newProject || editorCommands.openProject) {
                    if (platform::startDetached({hostExecutablePath().string(), "--launcher"})) {
                        quit = true;
                    }
                    else {
                        // Said rather than silently doing nothing. The editor
                        // stays open, which is the safe half of the failure.
                        core::log(core::LogLevel::Error, ENG_TR("app.err.launcher_start_failed"));
                    }
                }
                if (editorCommands.undo)
                    (void)editor.undo(authored(), inspector);
                if (editorCommands.redo)
                    (void)editor.redo(authored(), inspector);
                if (editorCommands.clearSelection)
                    inspector.select(core::InstanceId{});
                if (editorCommands.newScene) {
                    // Out of play mode first, for the reason opening a scene is:
                    // the snapshot would describe a world that no longer exists.
                    if (editor.inPlayMode())
                        editor.stop(host->world(), inspector);
                    editor.newScene(host->world(), inspector);
                    // A new scene has no code of its own yet.
                    host->remountSceneScripts({});
                    host->world().engineState().currentScene.clear();
                }
                // Every scene script's text went into the file with it.
                if (editorCommands.save && editor.saveOpenScene(host->world()))
                    scripts.markSavedWhere(ScriptOrigin::Scene);

                // **One property of one placed stamp, taken back or pushed up**
                // (S5.6). Drained here with the rest: applying rewrites a file
                // and restamps every linked instance in the world, which is a
                // structural change and belongs at the safe point rather than
                // inside the row that asked for it.
                if (editorCommands.overrideApply.has_value() && editorCommands.overrideSubject.valid() &&
                    editorCommands.overrideProperty.valid()) {
                    // Of a placed instance, which is in the scene's world: with
                    // a stamp open the id names something in the stamp's (B10).
                    if (stageOf() != nullptr) {
                        editor.report("close the stamp to revert or apply what a placed one changed", true);
                    }
                    else if (*editorCommands.overrideApply) {
                        (void)editor.applyOverride(host->world(), host->runtime().dataModel(),
                                                   editorCommands.overrideSubject, editorCommands.overrideProperty);
                    }
                    else {
                        (void)editor.revertOverride(host->world(), editorCommands.overrideSubject,
                                                    editorCommands.overrideProperty);
                    }
                }
                if (!editorCommands.saveAs.empty() && editor.saveSceneAs(host->world(), editorCommands.saveAs))
                    scripts.markSavedWhere(ScriptOrigin::Scene);

                // Remembered on CHANGE rather than at exit: an editor that only
                // wrote this on a clean shutdown would forget everything the one
                // time somebody most wants it -- after a crash.
                if (editor.openScenePath() != rememberedScene) {
                    rememberedScene = editor.openScenePath();
                    editor.rememberState(options.scriptPath / ".engine");
                }
                // **Asked rather than subscribed to.** A move and a resize are
                // both "the window is somewhere else now", SDL reports them as
                // several events each and neither is the last one -- so the
                // question is asked once a frame and answered by comparison.
                // `rememberWindow` returns without doing anything when nothing
                // moved, which is every frame but the ones that matter.
                if (window != nullptr)
                    editor.rememberWindow(platform::windowPlacement(*window));

                // The manipulator's mode and space, snapping, the browser's
                // layout. Drained once here rather than written inside each
                // setter, because those are keystrokes -- `Ctrl+L` twice would
                // otherwise be two files written from inside an input handler.
                if (editor.takePreferencesDirty())
                    editor.rememberState(options.scriptPath / ".engine");

                // **A tab whose instance is gone closes.** Deleting a script in
                // the Explorer left its editor open on a document that could no
                // longer be saved anywhere -- there is no instance to write
                // `Source` to and no file behind it either.
                //
                // Asked once a frame and after every command that can destroy
                // one, rather than wired to the delete: an instance also goes
                // when a scene is loaded, when a stamp session opens, and when
                // an undo takes a create back. One question at one place answers
                // all of them, and a list of the ways would have missed the next
                // one somebody adds.
                //
                // `ScriptEditor::forgetDestroyed` has existed and been tested
                // since this editor was built and nothing ever called it, which
                // is the shape D087 recorded: code that is right and unreachable
                // fails the moment something finally reaches for it.
                Editor::Stage* const openStage = stageOf();
                if (const std::size_t orphaned =
                        scripts.forgetDestroyed(host->world(), openStage != nullptr ? &openStage->world() : nullptr);
                    orphaned > 0) {
                    // Said out loud, because a tab vanishing is a thing that
                    // happened TO somebody -- and if it was dirty, the text went
                    // with the instance it belonged to.
                    editor.report(orphaned == 1
                                      ? "Closed a script tab; its instance was deleted"
                                      : std::to_string(orphaned) + " script tabs closed; their instances were deleted",
                                  false);
                }
            }

            // **The manipulator gets the pointer first.** A press that lands on
            // a handle is not a press asking to select whatever is behind it,
            // and losing the selection on the frame you grab its gizmo is the
            // commonest way a first manipulator is unusable.
            // **The scene's world, or the stage's -- never the project's
            // tree.** The Explorer draws what this points at, and the project's
            // tree has a panel of its own that is handed its world directly. It
            // WAS `authored()` for one commit, and the effect was an Explorer
            // that went blank the moment somebody clicked a row in the other
            // panel: it was drawing the content world rooted at the content
            // root, whose children it was told not to draw.
            //
            // Re-aimed every frame all the same, because a stamp opening or
            // closing does swap the world underneath the panels.
            if (overlay.has_value()) {
                Editor::Stage* const open = stageOf();
                scene::World& shown = open != nullptr ? open->world() : host->world();
                const core::InstanceId shownRoot = open != nullptr ? open->workspace() : host->runtime().dataModel();
                overlay->setInspectionTarget(&shown, shownRoot, &inspector);
            }

            // **The brush gets the pointer before the manipulator does**, and
            // before the pick. A tool is what a click MEANS, so while a brush is
            // selected a click on the ground is a stamp -- not a stamp AND a
            // selection change, which is what leaving the pick to run would make
            // it. `driveSculpt` returns false whenever it did not act, so
            // `Select` and a world with no terrain both cost one comparison.
            const bool brushTook =
                editor.driveSculpt(authored(), stageOf() != nullptr ? stageOf()->workspace() : host->workspace(),
                                   inspector, frame.renderDt);
            if (brushTook)
                editor.touch();
            // The block tool, on the brush's terms and after it: at most one
            // of the two is the selected tool, so at most one can take.
            const bool blockTook = !brushTook && editor.driveBlocks(authored(), inspector);
            if (blockTook)
                editor.touch();
            // The Tiles tool (the 2D layer), on the same terms again.
            const bool tileTook =
                !brushTook && !blockTook &&
                editor.driveTiles(authored(), stageOf() != nullptr ? stageOf()->workspace() : host->workspace(),
                                  inspector);
            if (tileTook)
                editor.touch();

            const bool gizmoTook = !brushTook && !blockTook && !tileTook && editor.driveGizmo(authored(), inspector);
            // A drag moves parts without ever producing a command, so the one
            // place that knows it happened is here.
            if (gizmoTook)
                editor.touch();

            const core::InstanceId wasSelected = inspector.selection();
            if (!gizmoTook && !brushTook && !blockTook && !tileTook)
                // The root the VIEWPORT is drawing, so a click can only land
                // on something that is on screen -- the stage's workspace while
                // a stamp is open, the host's otherwise.
                // The last frame's poses, which is the picture clicked -- in
                // Play only: a Stop, a reload or an undo this frame replaced
                // the world under them, and editing draws at the tick anyway.
                editor.resolvePick(authored(), stageOf() != nullptr ? stageOf()->workspace() : host->workspace(),
                                   inspector, advancing(editor.runState()) ? &framePoses : nullptr);

            // An editor says what you picked. It is the cheapest confirmation
            // that a click landed on the thing under the cursor rather than on
            // the thing behind it, and it is the only such confirmation that
            // survives into a log somebody can read afterwards -- which matters
            // here because the ImGui shell cannot render headlessly and so
            // cannot be asserted by any test that does not have a person in it.
            if (inspector.selection() != wasSelected) {
                if (inspector.selection().valid()) {
                    const scene::ClassDescriptor* descriptor =
                        authored().classes().find(authored().classOf(inspector.selection()));
                    const core::I18nArg args[] = {
                        {"name", authored().atoms().text(authored().name(inspector.selection()))},
                        {"class",
                         descriptor != nullptr ? authored().atoms().text(descriptor->name) : std::string_view{"?"}},
                    };
                    core::log(core::LogLevel::Info, ENG_TR("engine.editor.info.selected"), args);
                }
                else {
                    core::log(core::LogLevel::Info, ENG_TR("engine.editor.info.deselected"));
                }
            }

            // The editor's camera reaches the world here, at the same safe
            // point as every other write, and only while paused -- `driveCamera`
            // returns the transform unchanged once the world is playing, so the
            // game takes its camera back on the first tick without this having
            // to know it did.
            //
            // Seeded from wherever the world's camera already is, so pressing
            // pause does not teleport the view somewhere nobody asked for.
            // **The editor's camera is the editor's**, and the world never
            // learns about it (ADR 0046's rule taken the rest of the way). It is
            // seeded once from whatever the world is looking through so that
            // opening the editor does not teleport the view, and after that the
            // renderer is TOLD which view to draw -- see `render::ViewOverride`.
            //
            // Writing `Workspace.CurrentCamera` was the first design and it was
            // wrong: it made the tool and the game two authors of one transform,
            // which is a disagreement no arbitration settles (D061).
            if (!editor.cameraAdopted()) {
                const core::InstanceId cameraId = host->currentCamera();
                const scene::CameraComponent* camera =
                    cameraId.valid() ? host->world().cameras().find(cameraId) : nullptr;
                if (camera != nullptr) {
                    editor.adoptCamera(camera->cframe);
                }
                else {
                    // **A world with no camera still gets the editor's own**
                    // (D088). Adoption exists so that opening a project does not
                    // teleport the view; with nothing to adopt it was waiting
                    // forever, `useEditorView` stayed false, and `extract`
                    // produced no camera at all -- so the viewport fell through
                    // to the M1 "the engine is alive" pulse, and a person who had
                    // just made a blank project saw a rainbow cycling where their
                    // world should be.
                    //
                    // A blank project is exactly what the launcher makes, which
                    // is why this surfaced the day it shipped and not before.
                    editor.adoptCamera(editor.cameraCFrame());
                }
            }
        }

        // The streamed world advances at the SAME safe point, and for the same
        // reason: materialising instances mid-tick is the mutation the reload
        // below is forbidden for. Two milliseconds is architecture.md §10's
        // budget, and it is denominated in time rather than in chunks because
        // a chunk's cost varies with what is in it.
        //
        // **The ground first, and the two share the two milliseconds** rather
        // than each taking them: two managers are two budgets that do not know
        // about each other, and would overrun together. The ground goes first
        // because a missing collider is a fall and a missing prop is not.
        // **A terrain saved as cells is streamed wherever it is being looked
        // at** (ADR 0087): around the editor's camera while editing, around the
        // world's own foci while it plays.
        terrainCells.frame(host->world(), host->workspace(), editor.worldRestores(),
                           options.editor && editing(editor.runState())
                               ? std::optional<core::DVec3>(editor.cameraCFrame().position)
                               : std::nullopt);
        const f64 streamBudget = streaming.active() && fields.active() ? 1.0 : 2.0;
        if (fields.active()) {
            fields.setWorld(&host->world(), host->workspace());
            fields.pump(streamBudget);
        }
        if (streaming.active()) {
            streaming.setWorld(&host->world(), host->workspace());
            streaming.setPhysics(host->physics());
            streaming.pump(streamBudget);
        }

        // `@std/net` completions land here, at the same safe point, and for the
        // same reason streaming advances here: a response arrives on a worker
        // thread at a wall-clock moment, and resuming the coroutine there would
        // put game code into the frame wherever the socket happened to land it.
        host->publishNetworkResults();

        // Published UNCONDITIONALLY, and the conformance suite is what settled
        // it: a `LoadAreaAsync` in a project with no streamed world parked a
        // coroutine nothing would ever resume. With no chunks there is nothing
        // to wait for, so the honest answer is "loaded" on the next pump -- and
        // it has to be given, because a call that hangs forever is worse than
        // one that refuses.
        // A chunk evicted and an instance out of a replica's interest are one
        // event to a script (ADR 0069 decision 6), so both land in one list.
        std::vector<core::InstanceId> streamedOut = streaming.drainStreamedOut();
        if (replication::IReplication* replicating = network.replication(); replicating != nullptr) {
            for (const core::InstanceId husk : replicating->drainStreamedOut())
                streamedOut.push_back(husk);
        }
        host->publishStreamingResults(streamedOut, [&streaming](core::DVec3 position, f64 radius) {
            return streaming.areaResident(position, radius);
        });

        // **The editor's own reload** (ADR 0057), which `ludwerk edit` has never
        // had: it is started without `--dev-control`, and the only call site was
        // gated on it. Saving a script writes the file and then rebuilds the
        // world from source, which is the loop somebody editing code expects and
        // the one the dev server has given `ludwerk dev` since M3.
        // The only place a reload happens, and for the same reason: a world
        // swapped mid-tick would break within-run determinism, which is the
        // rule architecture.md §4 states (and the reason the connection hands
        // its messages over here rather than acting on them itself).
        if (!options.devControlUrl.empty()) {
            control.takeCommands(commands);
            for (const DevCommand& command : commands) {
                switch (command.kind) {
                case DevCommand::Kind::Reload: {
                    const ReloadReport reloaded = reloadWorld(host, worldOptions);
                    host->setGizmoTarget(&debugDraw);

                    // Both halves matter. The selection and anything still
                    // queued name ids the outgoing world minted, and slot
                    // indices restart from zero -- so replaying them would
                    // write to whatever moved into the same slot rather than
                    // to nothing. The overlay's pointer is re-aimed for the
                    // blunter reason: the world it held has been destroyed.
                    inspector.onWorldChanged();
                    if (overlay.has_value()) {
                        overlay->preserveExplorerOnNextWorld();
                        // **The overlay's visibility belongs to whoever is
                        // looking, not to the world.** A reload brings a fresh
                        // `EngineState` whose `overlayVisible` is false, so
                        // without this the panels vanish on every hot reload and
                        // the editor is left as a bare viewport with no menu
                        // bar. A defect the dev server has had since M3, found
                        // while E8 briefly made the editor reload too.
                        host->world().engineState().overlayVisible = overlayVisible;
                        overlay->setInspectionTarget(&host->world(), host->runtime().dataModel(), &inspector);
                        // And the VM, for the same blunt reason: the runtime the
                        // console evaluated in has been destroyed with the world.
                        overlay->setScriptTarget(&host->runtime());
                    }
                    replyOk("reloaded", command.id, [&reloaded, &host](core::JsonWriter& writer) {
                        writer.field("ok", reloaded.ok);
                        writer.field("ms", reloaded.spanMs);
                        writer.field("scripts", reloaded.mountedScripts);
                        writer.field("preserved", reloaded.preserve.restored);
                        writer.field("tick", host->world().engineState().tick);
                        writer.field("hash", host->world().worldHash());
                        if (!reloaded.ok && reloaded.error.has_value())
                            writer.field("detail", reloaded.error->message);
                    });
                    break;
                }
                case DevCommand::Kind::Sample:
                    // A tick already past is answered at once rather than never:
                    // the reply carries the tick it was actually taken at, so a
                    // caller can tell the difference.
                    pendingSamples.push_back(PendingSample{command.id, command.atTick});
                    break;
                case DevCommand::Kind::Ping:
                    replyOk("pong", command.id, [](core::JsonWriter&) {});
                    break;
                case DevCommand::Kind::Shutdown:
                    quit = true;
                    break;
                case DevCommand::Kind::AssetChanged: {
                    // **What changed on disk is forgotten, and the next frame
                    // reads it again** (S6.4). Reserved by the protocol since
                    // M3 and deferred then because "no asset pipeline exists
                    // before M4/M7"; both shipped, so the reason expired and
                    // this is what it was waiting for.
                    //
                    // The whole implementation is a forget, because the loaders
                    // already load everything MISSING -- there is no second
                    // path to write and no state machine to get wrong. It works
                    // on LOOSE content, which is what a `ludwerk dev` session
                    // runs: a loose URN resolves to a path and the mount keeps
                    // no bytes (D039), so the next load opens the file as it
                    // stands. A packed or compiled URN reloads the same bytes,
                    // which is correct -- those are artifacts, and changing one
                    // means recompiling it.
                    std::vector<core::NameAtom> urns;
                    urns.reserve(command.paths.size());
                    for (const std::string& path : command.paths) {
                        // The watcher reports project-relative paths under
                        // `content/`; a URN is `asset://` plus the rest. Anything
                        // outside that folder is not content and is skipped
                        // rather than guessed at.
                        constexpr std::string_view kContent = "content/";
                        std::string relative = path;
                        std::replace(relative.begin(), relative.end(), '\\', '/');
                        const std::size_t at = relative.rfind(kContent);
                        if (at == std::string::npos)
                            continue;
                        relative.erase(0, at + kContent.size());
                        urns.push_back(host->world().atoms().intern("asset://" + relative));
                    }

                    core::u32 dropped =
                        urns.empty() ? 0u : meshLoader.forget(*device, urns, textureLibrary, meshLibrary, meshCache);
                    // A changed material file is forgotten too, and every
                    // variant with it -- the library re-reads what anything
                    // asks for next (ADR 0062, ADR 0090).
                    for (const core::NameAtom urn : urns) {
                        const std::string_view text = host->world().atoms().text(urn);
                        if (asset::isMaterialPath(text)) {
                            materialLibrary.forget(text);
                            ++dropped;
                        }
                    }
                    // A stamp changed by something other than this editor:
                    // its linked instances follow, keeping their own (B13).
                    if (options.editor) {
                        for (const core::NameAtom urn : urns) {
                            const std::string_view text = host->world().atoms().text(urn);
                            constexpr std::string_view Suffix = ".stamp.json";
                            if (text.size() > Suffix.size() && text.ends_with(Suffix))
                                dropped += editor.stampChangedOnDisk(host->world(), host->runtime().dataModel(),
                                                                     text.substr(std::string_view("asset://").size()));
                        }
                    }
                    const core::I18nArg args[] = {{"count", static_cast<core::i64>(dropped)}};
                    core::log(core::LogLevel::Info, ENG_TR("engine.dev.info.assets_reloaded"), args);
                    replyOk("asset-changed", command.id, [dropped](core::JsonWriter& writer) {
                        writer.field("reloaded", static_cast<core::i64>(dropped));
                    });
                    break;
                }
                case DevCommand::Kind::Unsupported:
                    // Answered rather than ignored: `asset-changed` and `eval`
                    // are reserved by the protocol and a caller that gets
                    // silence cannot tell "not yet" from "lost".
                    //
                    // The key travels as its NAME rather than through
                    // `ENG_TR`, because a `TextKey` is a four-byte hash and
                    // the peer is what resolves it, in the peer's locale. That
                    // is a spelling the i18n lint could not see, and this key
                    // spent from M3 to D139 naming nothing in any catalog --
                    // so a client that resolved it got the
                    // `[i18n:missing:xxxxxxxx]` marker. The lint sweeps
                    // key-shaped literals now, whoever writes them.
                    //
                    // `of` is the verb, and it is the message's `{of}`: the
                    // field and the placeholder carry the same name so that a
                    // client can hand the reply's own fields to the catalog.
                    replyOk("error", command.id, [&command](core::JsonWriter& writer) {
                        writer.field("key", "engine.dev.err.not_implemented");
                        writer.field("of", command.type);
                    });
                    break;
                }
            }
        }

        // **The streaming pause, which is a property that had a reader waiting
        // for it (D055).** `StreamingManager::minimumRingResident()` exists,
        // its own comment says it "is what `StreamingService.PauseOutsideLoadedArea`
        // reads", and nothing had ever called it -- so a game that turned the
        // property on got no pause and a character walking into unloaded ground
        // fell through it, which is the exact failure the property names.
        //
        // **It does not weaken R10.** What this changes is WHEN a tick runs,
        // never what a tick computes: the same ticks happen in the same order
        // with the same inputs, and the world hash after n ticks is the hash
        // after n ticks. The pump below keeps running while the world is
        // paused, which is what ends the pause.
        //
        // **Streamed ground adds one wait nobody opts into**: the first. Until
        // the cells around the focus have arrived once, a character standing
        // on streamed ground would fall through it on the first tick, so the
        // simulation holds -- the initial load every streamed world has
        // (ADR 0075). After that the property decides, for ground as for parts.
        const bool pauseOutside = host->world().engineState().streamingPauseOutsideLoadedArea;
        const bool waitingForGround = (fields.active() && !fields.primed()) ||
                                      (fields.active() && pauseOutside && !fields.minimumRingResident()) ||
                                      (streaming.active() && pauseOutside && !streaming.minimumRingResident());
        u32 simTicks = waitingForGround ? 0u : frame.simTicks;

        // **The editor's transport, and it gates ticks by exactly the same
        // argument the streaming pause above makes** (D058): what this changes
        // is WHEN a tick runs, never what a tick computes. Paused takes none,
        // a step takes one, playing takes what the frame owed and this line
        // does nothing at all.
        //
        // A paused editor still shows a built world: `WorldHost::boot` runs
        // every entry script in its own drain before the first frame and
        // advances no clock, so tick zero is a world rather than an absence.
        if (options.editor)
            simTicks = editor.allowedTicks(simTicks);

        // **The history is dropped when the world it describes has been
        // replaced**, and here because here is downstream of every way that
        // happens -- a stop, an undo, a redo, a scene load, a new scene and a
        // hot reload are all behind us and the first `capture` of this frame is
        // ahead. `world.h` states the obligation in so many words: these
        // caches are "rebuilt from the tree rather than restored ... safe
        // order: restore, then rebuild". Nothing rebuilt this one (D070).
        //
        // A snapshot preserves generations precisely so that an id means the
        // same thing afterwards, which is what let a stale entry go on
        // answering `previous()` for an instance that had moved metres.
        if (transformHistoryWorld != inspector.worldGeneration()) {
            transformHistoryWorld = inspector.worldGeneration();
            transformHistory.clear();
        }

        // The simulation, before anything is drawn: rendering shows the state a
        // tick settled on, never one being written.
        const core::u64 simStartedNs = platform::nowNs();
        for (u32 step = 0; step < simTicks; ++step) {
            // What arrived is input to the tick that follows it, and what is
            // sent is the tick's result -- reversed, both directions cost a
            // tick of latency and nothing in a loopback test would show it.
            // **The last line, not the defence** (audit N1): what a peer sends
            // is decoded under protected calls, and an error that still
            // escapes the VM ends this tick, logged, rather than the process
            // -- a server that terminates on one bad message is everybody's
            // game ended by one player.
            try {
                // BEFORE the tick, so that once the loop is done the history
                // holds where everything was one tick ago and the world holds
                // where it is now -- the two ends the frame interpolates
                // between (D047). **And before the snapshot**: another
                // player's character, which the snapshot moves, captured after
                // it had no earlier place and stepped at 60 Hz (ADR 0134).
                app::runDrawnTick(*host, network, transformHistory);
            } catch (const std::exception& error) {
                const std::array<core::I18nArg, 1> args{core::I18nArg{"message", std::string_view{error.what()}}};
                core::log(core::LogLevel::Error, ENG_TR("engine.err.tick_exception"), args);
            }
            // **Answered at the tick, not at the frame** (D214): a loaded
            // machine runs several ticks a frame, and a sample asked for tick
            // 240 came back from 241 because the frame went from 239 past it.
            answerSamples();
        }
        phaseSimMs += msSince(simStartedNs);
        // A frame that ran no tick still services the connection: a paused
        // editor, or a frame that arrived early, must not look like a peer
        // that stopped answering.
        if (network.active() && simTicks == 0)
            app::receiveDrawn(*host, network, transformHistory, false);
        // What a script asked of the network, and what the connection did.
        network.update();
        // The own character drawn sliding after a correction, never popping
        // (the multiplayer smoothness brief).
        {
            const replication::VisualCorrection slide = network.visualCorrection();
            transformHistory.setVisualOffset(slide.character, slide.offset);
        }
        // A windowless server would otherwise spin a core waiting for its next
        // tick. A millisecond is far below a tick and far above a spin.
        if (network.active() && options.headless && simTicks == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // **The pointer's state, applied to the window it belongs to.** Both
        // properties were stored and read by nothing until M8 (D049): a script
        // could set `InputService.PointerLocked` and the cursor kept wandering
        // across the desktop, which is a look control that stops at the edge of
        // the screen.
        //
        // Applied on CHANGE rather than every frame, because SDL's relative mode
        // warps the cursor when it is entered and re-entering it every frame
        // would fight anything else on the machine that moves a pointer.
        // Windowed only: a headless run has no window to lock a pointer to, and
        // the property still round-trips there, which is what makes a replay of
        // a game that locks its pointer legal.
        if (window != nullptr) {
            scene::EngineState& engineState = host->world().engineState();

            // **Who owns the mouse**, decided by `decidePointer` rather than
            // here (S7.11). Four defects came out of this question -- D049,
            // D059, D063 and D069 -- and every one of them was reported by a
            // person, because the rule was arithmetic inline in this loop and
            // nothing could call it. The rules and the reasons are in
            // `pointer_ownership.cpp` and the cases that hold them are in
            // `pointer_ownership_tests.cpp`.
            //
            // The game's property is not overwritten, only overridden. A script
            // that reads `InputService.PointerLocked` sees what it wrote, which
            // keeps the property honest and keeps a replay of a game that locks
            // its pointer legal.
            const PointerOwnership pointer = decidePointer({
                .editorProfile = options.editor,
                .editing = editing(editor.runState()),
                .cameraDetached = editor.cameraDetached(),
                .lookActive = editor.lookInput().active,
                .gameWantsLocked = engineState.pointerLocked,
                .gameWantsVisible = engineState.pointerVisible,
            });
            const bool wantLocked = pointer.locked;
            const bool wantVisible = pointer.visible;

            // Told here rather than inferred in the overlay, because this is
            // where the question is already answered, and told BEFORE
            // `handleEvents` runs later in this same frame, so there is no frame
            // of lag on either edge.
            if (overlay.has_value())
                overlay->setGameHoldsPointer(pointer.gameHoldsPointer);

            if (wantLocked != pointerLocked) {
                pointerLocked = wantLocked;
                // Logged on the transition, not per frame. It separates two
                // failures that look identical from the outside: an editor that
                // never asked to hold the pointer, and a window that refused.
                if (options.editor) {
                    const core::I18nArg args[] = {
                        {"state", pointerLocked ? std::string_view{"held"} : std::string_view{"released"}}};
                    core::log(core::LogLevel::Info, ENG_TR("app.info.pointer_lock"), args);
                }
                // The result is checked rather than discarded. A pointer lock
                // that silently did not happen looks exactly like one that did
                // -- the cursor is hidden either way -- and the difference only
                // shows up as a camera that stops turning at the edge of the
                // screen, which is a thing somebody reports and nobody can
                // explain.
                if (!platform::setPointerLocked(*window, pointerLocked))
                    core::log(core::LogLevel::Warn, ENG_TR("app.warn.pointer_lock_refused"));

                // **Anchored back to where the hold began** (D063). SDL
                // accumulates a logical position from the relative motion and
                // warps the real cursor there on the way out, so without this a
                // look that turned the camera around leaves the cursor against
                // a window edge. Every editor puts it back under the hand that
                // was holding the button.
                if (!pointerLocked)
                    platform::setPointerPosition(*window, editorPointerAnchor.x, editorPointerAnchor.y);
            }
            if (wantVisible != pointerVisible) {
                pointerVisible = wantVisible;
                platform::setPointerVisible(pointerVisible);
            }

            // **What the WINDOW knows, written where a script can read it
            // (D055).** Both of these are properties of the display rather than
            // of the world, `UIService` publishes them, and until the lint was
            // widened to sweep `EngineState` nothing wrote either -- so
            // `DisplayScale` was 1 on a doubled display and `SafeAreaInsets`
            // was zero on a device with a notch, which are exactly the two
            // machines a game gets them for. Every frame rather than on change,
            // because they change with a window drag between two monitors and
            // there is no event for that worth subscribing to at this price.
            engineState.displayScale = platform::windowDisplayScale(*window);

            // **`UIService.ScreenOrientation`, applied when it changes** -- from
            // a script, from the scene that was loaded, or from the Properties
            // panel -- and once at the start, since a phone's window is made
            // before any world says which way up it wants to be.
            if (engineState.screenOrientation != appliedOrientation) {
                appliedOrientation = engineState.screenOrientation;
                platform::setScreenOrientation(*window, appliedOrientation);
            }
            const platform::WindowInsets insets = platform::windowSafeAreaInsets(*window);
            engineState.safeAreaInsets = core::Rect{
                .min = core::Vec2{static_cast<f32>(insets.left), static_cast<f32>(insets.top)},
                .max = core::Vec2{static_cast<f32>(insets.right), static_cast<f32>(insets.bottom)},
            };

            // **The overlay and its property, in both directions.** A script
            // writing `DebugService.OverlayVisible` opens the panel; F3 opening
            // the panel writes the property back, so a game that draws its own
            // hint from it does not go stale (D055). Without the write-back the
            // two would disagree the moment anybody pressed the key, which is
            // the state D030 named and this lint exists to find.
            if (overlay.has_value()) {
                if (engineState.overlayVisible != overlayVisible) {
                    overlayVisible = engineState.overlayVisible;
                    overlay->setVisible(overlayVisible);
                }
                else if (overlay->visible() != overlayVisible) {
                    overlayVisible = overlay->visible();
                    engineState.overlayVisible = overlayVisible;
                }
            }
        }

        // Where this frame sits between those two ticks.
        //
        // **Zero on the synthetic clock, by construction and not by accident.**
        // A headless run drives exactly one fixed step per frame, so its frames
        // ARE the ticks -- and every golden in this repository was recorded that
        // way. Interpolating a headless frame by the accumulator's rounding
        // residue would move every recorded uniform in its last bits for no
        // gain at all.
        //
        // **And zero whenever the world is not advancing** (D070). Interpolation
        // blends where a part was at the START of the last tick with where it
        // is now, and a frame that runs no tick is not between those two things
        // -- there is one state and the frame is on it. The accumulator does not
        // stop for a paused editor: `FrameScheduler` drains it every frame
        // whether or not the editor let a tick through, so `alpha` goes on
        // sweeping the whole of [0, 1) at render rate. Against a history that
        // stopped moving, that is a part drawn somewhere different every frame,
        // which is what "the capsule flickers after stop" is.
        const bool worldAdvancing = !options.editor || advancing(editor.runState());
        const bool drawnBetweenTicks = !syntheticClock && worldAdvancing;
        const f32 renderAlpha = drawnBetweenTicks ? frame.alpha : 0.0f;
        // A world held at its tick is drawn AT it, which is no history
        // (D253); alpha zero with one is the tick before.
        framePoses.begin(host->world(), drawnBetweenTicks ? &transformHistory : nullptr, renderAlpha);

        // What the speakers do is a consequence of the simulation and never an
        // input to it (M6 brief, Decision 9), which is why this is after the
        // ticks rather than inside one. The listener is
        // `Workspace.CurrentCamera` -- a game with two ideas about where the
        // player is hearing from is a game with a bug.
        // The same mounts the meshes and the UI read from, so `Sound.Content`
        // names a file the same way everything else does. Set every frame
        // rather than at boot because the world -- and with it the audio system
        // -- can be replaced by a reload; the call is an identity check when
        // nothing changed.
        host->audio().setContentMounts(&contentMounts);
        // Silent while the editor is paused (D060). A world that is not ticking
        // should not be audible, and hearing a game's ambience while editing it
        // is the same wrong-owner mistake as an editor whose cursor belongs to
        // the game.
        // Silent whenever the world is not advancing -- editing OR paused in
        // play mode. A paused game that kept humming would be the same
        // half-stopped state the three-state model exists to remove.
        host->audio().setSuspended(options.editor && !advancing(editor.runState()));
        // **Pressing play ends the preview.** An audition is a thing the tool is
        // doing while the world is stopped, and a file still playing over the
        // top of a running game is the same wrong-owner mistake the suspension
        // itself exists to prevent.
        if (advancing(editor.runState()))
            host->audio().stopAudition();
        // **The selected sound shows its `TimeLength` while nothing ticks**:
        // the tick is what reads it otherwise, and an editor that is not
        // playing has not ticked. One header read, once per content.
        else if (scene::SoundComponent* shown = authored().sounds().find(inspector.selection());
                 shown != nullptr && shown->timeLength == 0.0)
            shown->timeLength = host->audio().clipDuration(shown->content);
        // **The ear goes where the eye is.** A sound parented to a part
        // is positional, and what it is positional AGAINST is the listener --
        // which is `Workspace.CurrentCamera`. A game with no camera of its own
        // is rendered through the editor's (D100), and until now the listener
        // stayed at the world origin while the picture moved: every sound was
        // attenuated against a point nobody was standing at, so a sound near the
        // origin was loud wherever you flew and a distant one was silent from
        // right beside it.
        //
        // The SAME predicate the frame uses to choose the view, computed once
        // and read twice, because D100 is what a predicate spelled differently
        // in two files does.
        const core::InstanceId listener = host->currentCamera();
        const bool worldHasCamera = host->world().alive(listener) && !host->world().destroyed(listener) &&
                                    host->world().cameras().find(listener) != nullptr;
        const bool listenWithEditor = options.editor && editor.cameraAdopted() && !worldHasCamera;
        // The whole frame: the rotation is what decides left from right, so an
        // ear given only a position hears everything in the middle.
        const core::CFrameD editorEar = editor.cameraCFrame();
        host->audio().update(host->world(), listener, listenWithEditor ? &editorEar : nullptr);

        // The physics wireframe (roadmap M5, "Jolt debug-draw bridge"): what the
        // SOLVER thinks the world looks like, which is the only picture that can
        // disagree with the rendered one and therefore the only one worth
        // having. Drawn after the ticks, because it describes the state the
        // frame is about to show.
        //
        // Behind `DebugService:ShowPanel("Physics")` rather than always on: it
        // is a line per shape edge for every body in the world, which is a frame
        // cost nobody should pay without asking.
        //
        // **`View > Collision Shapes` is the second switch, and it is the one an
        // EDITOR has** (S5.10). `ShowPanel` is a script call, and edit mode runs
        // no game script -- so the one person who could not see the colliders
        // they were placing was the author placing them.
        const bool wantCollision = (overlay.has_value() && overlay->panels().showCollision) ||
                                   script::panelOpen(host->runtime().state(), "Physics");
        if (scene::PhysicsSync* physics = host->physics(); physics != nullptr && wantCollision) {
            // **The mirror, when nothing is ticking.** A paused world never
            // calls `step`, so the backend holds no bodies at all, and the
            // wireframe of a world with no bodies is an empty picture that looks
            // exactly like a working one. `mirror` creates and retires them
            // without advancing anything.
            //
            // **Editing is the other world that does not tick** (the owner's
            // report: every View switch but the grid did nothing while editing).
            // `paused` is the SCRIPT's pause; the editor holds its world still by
            // allowing no ticks, so `paused` stayed false and the wireframe of the
            // world being built was always empty.
            if (host->world().engineState().paused || (options.editor && editing(editor.runState())))
                physics->mirror();
            PhysicsWireframe sink(debugDraw, *physics, host->world(), framePoses);
            physics->backend().debugDraw(physics->worldHandle(), sink);
        }

        // The rig, behind the View menu's `Skeletons` for the same reason: a
        // line per joint for every skinned mesh in the world. Drawn after the
        // ticks, because the pose it reads is the one this frame will show.
        //
        // The rigs load at the top of a tick, and an editor that is editing runs
        // none -- so a character placed in the editor had no skeleton to draw
        // until play. Loaded here instead, which costs a set lookup per
        // `MeshPart` once they are all in.
        if (overlay.has_value() && overlay->panels().showSkeletons) {
            host->loadSkeletons();
            if (const render::AnimationSystem* animation = host->animation(); animation != nullptr)
                drawSkeletons(host->world(), *animation, debugDraw, &framePoses);
        }

        // **What is not a part, drawn** (S5.1). A `Camera`, a `PointLight`, an
        // `Attachment` and a `Ragdoll` have no geometry, so before this the
        // viewport was a picture with things missing from it -- and a thing you
        // cannot see is a thing you cannot click even once picking knows about
        // it.
        //
        // A wire sphere at the marker's own pick radius, so what is drawn is
        // exactly what is clickable. Editor only: a running game draws its own
        // world and has no business showing the author's furniture.
        if (options.editor) {
            static std::vector<PickMarker> markers;
            collectPickMarkers(host->world(), authoredRoot(), markers, &framePoses);
            for (const PickMarker& marker : markers) {
                // Not the one the eye is inside (`eyeInsideMarker`): from in
                // there it is two lines across the whole picture.
                if (snapshot.camera.valid && eyeInsideMarker(snapshot.camera.origin, marker.at))
                    continue;
                // Brighter for the selected one, because the reason to look at
                // these is to find the one you are moving.
                const bool chosen = inspector.isSelected(marker.instance);
                debugDraw.wireSphere(core::toVec3(marker.at), kPickMarkerRadius,
                                     chosen ? render::DebugColor::fromLinear(1.0f, 0.85f, 0.35f)
                                            : render::DebugColor::fromLinear(0.45f, 0.55f, 0.70f),
                                     12);
            }
        }

        // **The terrain's triangles and normals** (the owner's terrain report),
        // behind `View > Terrain Wireframe` and `Terrain Normals` in the editor
        // and `DebugService:ShowPanel("Terrain")` in a game -- which shows both.
        // Around the eye the frame is drawn from: the editor's camera, or the
        // world's own when a game is running.
        {
            const bool terrainPanel = script::panelOpen(host->runtime().state(), "Terrain");
            const bool wireframe = terrainPanel || (overlay.has_value() && overlay->panels().showTerrainWireframe);
            const bool normals = terrainPanel || (overlay.has_value() && overlay->panels().showTerrainNormals);
            if (wireframe || normals) {
                core::CFrameD eye = editor.cameraCFrame();
                if (const scene::CameraComponent* camera = host->world().cameras().find(listener);
                    camera != nullptr && !listenWithEditor)
                    eye = camera->cframe;
                // Forward is the negated third column (`math.h`).
                const core::Mat3& basis = eye.rotation;
                const core::Vec3 forward{-basis.m[2][0], -basis.m[2][1], -basis.m[2][2]};
                drawTerrainDebug(host->world(), eye.position, forward, wireframe, normals, debugDraw);
            }
        }

        // The reference grid, at the SAME step the translate snap uses -- lines
        // you can see and a snap you cannot are two grids, and the one that
        // catches is the invisible one. On the y = 0 plane, which is where a
        // world's floor is until somebody decides otherwise.
        if (overlay.has_value() && overlay->panels().showGrid) {
            drawReferenceGrid(editor.cameraCFrame().position, 0.0, editor.snapStep(GizmoMode::Translate), debugDraw);
        }

        // The streaming grid, behind `View > Streaming Grid` in the editor and
        // `DebugService:ShowPanel("Streaming")` in a game -- the same pair of
        // switches the physics wireframe has, and for the same reason: four
        // lines per cell for every cell the index knows about is a frame cost
        // nobody should pay without asking.
        //
        // Drawn at the camera's own height, so the grid follows somebody up a
        // hill instead of being buried in it, and only the band the camera is
        // in (ADR 0086): a band is taller than any hill, so within it there is
        // no height a cell belongs at, only a height it is USEFUL at.
        if (streaming.active() && ((overlay.has_value() && overlay->panels().showChunkGrid) ||
                                   script::panelOpen(host->runtime().state(), "Streaming"))) {
            drawChunkGrid(streaming, editor.cameraCFrame().position.y, debugDraw);
        }

        // A tick already past when the request arrived is answered with the
        // tick it is now, which is what the sample's `tick` field says.
        answerSamples();

        if (host->shutdownRequested())
            quit = true;
        // Asked from outside (Ctrl+C, SIGTERM): closed exactly as a script's
        // `game:Shutdown()` closes it, so `BindToClose` runs on a server too.
        if (platform::stopRequested() && !quit) {
            core::log(core::LogLevel::Info, ENG_TR("engine.info.stop_requested"));
            quit = true;
        }

        // A dev session that has lost its dev server has nobody left to tell it
        // to stop -- and headless it has no window to close either, so it would
        // run until something else killed it. That is the orphaned-process
        // failure the M3 brief lists as entering risk 4, and this is the whole
        // of the fix. A WINDOWED session keeps running on purpose: closing
        // `ludwerk dev` should not take the window with it.
        if (!options.devControlUrl.empty() && options.headless && !control.connected())
            quit = true;

        if (!options.headless) {
            const std::span<const platform::Event> events = platform::pumpEvents();
            for (const platform::Event& event : events) {
                if (event.type != platform::EventType::Quit &&
                    event.type != platform::EventType::WindowCloseRequested) {
                    continue;
                }
                // **The window's close button asks too**, and it is the door
                // most people use. Without this the editor threw away an
                // afternoon on a click that every other application makes safe;
                // the shell draws the question and the answer comes back as
                // `EditorCommands::quit`.
                if (options.editor && editor.hasUnsavedWork())
                    editor.requestClose();
                else
                    quit = true;
            }

            // The Input Action System folds this frame's events into the device
            // snapshot the ticks below read. Accumulated rather than replaced,
            // because a key stays down between the press and the release, and
            // handed over BEFORE the ticks so that every tick this frame sees
            // one snapshot.
            // **The game does not receive input while the editor is editing**
            // (D062). Same rule as the tick, the cursor, the audio and the
            // camera, and the same reason: while the tool owns the machine, WASD
            // is a fly camera and not a character. The events still reach ImGui
            // -- it takes the untranslated SDL stream of its own -- so the shell
            // is fully live while the game is not.
            //
            // `releaseAll` on the way in rather than on the way out, so a key
            // held when play stopped is not still held when play starts again.
            // That is the alt-tab case the function was written for, arriving
            // through a different door.
            // **The motion, taken relative rather than differenced.** Once the
            // pointer is locked its POSITION stops moving, so a camera driven
            // from two positions stops turning at exactly the moment it is being
            // asked to. `platform::Event` says so at the field itself.
            editorLookDelta = {};
            for (const platform::Event& event : events) {
                if (event.type == platform::EventType::MouseMoved) {
                    editorLookDelta.x += event.pointerDeltaX;
                    editorLookDelta.y += event.pointerDeltaY;
                    // Only while the pointer is free. In relative mode the
                    // position SDL reports is a logical one it accumulates from
                    // the motion, so recording it here would anchor the cursor
                    // to wherever the camera took it -- which is the defect this
                    // is the other half of.
                    if (!pointerLocked)
                        editorPointerAnchor = core::Vec2{event.pointerX, event.pointerY};
                }
            }

            // Driven HERE rather than at the frame's safe point, because this is
            // where the motion arrives: the loop pumps events after the ticks,
            // and a camera fed at the safe point would be turning on the
            // previous frame's mouse. It moves on the RENDER clock and not the
            // tick, because a world that is not ticking is exactly when somebody
            // is flying it.
            // A detached view in play mode (Shift+P) flies the same camera.
            if (options.editor && (editing(editor.runState()) || editor.cameraDetached()) && editor.cameraAdopted()) {
                const Editor::LookInput& look = editor.lookInput();
                (void)editor.driveCamera(look.active ? editorLookDelta : core::Vec2{}, look.move,
                                         static_cast<f32>(frame.renderDt));
            }

            // **Not while the view is detached**: the fly keys are the editor's
            // then, and a character walking off on the same WASD that flies
            // the camera is two things answering one key.
            const bool gameTakesInput = !options.editor || (editor.inPlayMode() && !editor.cameraDetached());
            if (gameTakesInput) {
                // In the editor, the game's pointer is the viewport's (see
                // `toViewportEvents`): scripts, the UI and the canvases in the
                // world all read it after this.
                if (options.editor) {
                    static std::vector<platform::Event> viewportEvents;
                    toViewportEvents(events, editor.viewport(), viewportEvents);
                    host->pumpInput(viewportEvents);
                }
                else {
                    host->pumpInput(events);
                }
            }
            else if (gameHadInput) {
                host->input().releaseAll(host->world());
            }
            gameHadInput = gameTakesInput;

            // The UI's own reading of the same events. Gathered here rather
            // than from the device snapshot because two of the three are
            // EVENTS with no resting state: a typed character and a Return do
            // not persist, and a snapshot cannot express them.
            uiTypedText.clear();
            uiBackspace = false;
            uiForwardDelete = false;
            uiCaretLeft = false;
            uiCaretRight = false;
            uiCaretHome = false;
            uiCaretEnd = false;
            uiSubmit = false;
            for (const platform::Event& event : events) {
                switch (event.type) {
                case platform::EventType::MouseButtonDown:
                    if (event.button == platform::MouseButton::Left)
                        uiPointerDown = true;
                    break;
                case platform::EventType::MouseButtonUp:
                    if (event.button == platform::MouseButton::Left)
                        uiPointerDown = false;
                    break;
                case platform::EventType::TextInput:
                    uiTypedText.append(event.text);
                    break;
                case platform::EventType::KeyDown:
                    if (event.key == platform::Key::Backspace)
                        uiBackspace = true;
                    else if (event.key == platform::Key::Return || event.key == platform::Key::KeypadEnter)
                        uiSubmit = true;
                    else if (event.key == platform::Key::Delete)
                        uiForwardDelete = true;
                    else if (event.key == platform::Key::Left)
                        uiCaretLeft = true;
                    else if (event.key == platform::Key::Right)
                        uiCaretRight = true;
                    else if (event.key == platform::Key::Home)
                        uiCaretHome = true;
                    else if (event.key == platform::Key::End)
                        uiCaretEnd = true;
                    break;
                default:
                    break;
                }
            }

            // After the pump and with the span it returned: the overlay reads
            // the untranslated stream behind these, which is only valid until
            // the next pump.
            if (overlay.has_value())
                overlay->handleEvents(events);

            // **What was dropped on the window goes to the content browser.**
            // Only in the editor: a running game has no folder for a file to
            // land in, and dropping one on it should do what it has always done,
            // which is nothing. Read here because the list is valid until the
            // next pump, and acted on at the safe point with everything else
            // that writes to disk.
            if (options.editor) {
                for (const std::string& dropped : platform::droppedFiles())
                    importedPaths.emplace_back(dropped);
            }
        }

        const core::u64 beginWaitNs = platform::nowNs();
        rhi::ICmdList* cmd = device->beginFrame();
        phaseWaitMs += msSince(beginWaitNs);
        if (cmd == nullptr)
            continue;

        // An invalid target is normal, not an error: a minimized window has no
        // backbuffer this frame. Submitting the empty command buffer keeps the
        // loop pumping instead of stalling on a window nobody can see.
        rhi::TextureHandle target = offscreen;
        rhi::TextureFormat targetFormat = kOffscreenFormat;
        core::u32 targetWidth = static_cast<core::u32>(options.width);
        core::u32 targetHeight = static_cast<core::u32>(options.height);

        if (!options.headless) {
            const core::u64 acquireNs = platform::nowNs();
            const rhi::Swapchain swapchain = device->acquireSwapchain(*window);
            phaseWaitMs += msSince(acquireNs);
            target = swapchain.texture;
            targetFormat = swapchain.format;
            targetWidth = swapchain.width;
            targetHeight = swapchain.height;
        }

        // In the editor the world is not drawn to the screen: it is drawn into
        // the viewport panel's own texture, and the shell is drawn to the
        // screen on top. Everything between here and the overlay's pass
        // therefore renders at the PANEL's resolution -- which is what makes
        // the aspect ratio, the UI layout and the picking ray all agree with
        // the image somebody is looking at.
        const rhi::TextureHandle present = target;
        if (options.editor && target.valid()) {
            const ViewportRect& panel = editor.viewport();
            const core::u32 wantWidth = static_cast<core::u32>(panel.width > 1.0f ? panel.width : 1.0f);
            const core::u32 wantHeight = static_cast<core::u32>(panel.height > 1.0f ? panel.height : 1.0f);
            if (viewportTarget.resize(*device, wantWidth, wantHeight)) {
                target = viewportTarget.texture();
                targetFormat = kOffscreenFormat;
                targetWidth = viewportTarget.width();
                targetHeight = viewportTarget.height();
            }
        }

        if (target.valid()) {
            ensureDebugPass(targetFormat);

            // **A paused world runs no script phases, not even the render-rate
            // one** (D061). `PreRender` fires on the render clock rather than
            // the tick, so it kept firing while the editor was paused -- and
            // `examples/10-open-world` drives its follow camera from there, so
            // the script and the editor wrote `CurrentCamera` on alternate
            // frames and the view flickered between two answers.
            //
            // Pause means the game is not running. A phase that still fires is
            // the game still running, and no amount of arbitrating who wins the
            // camera would make that untrue.
            const bool worldIsRunning = !options.editor || advancing(editor.runState());
            if (!options.headless && worldIsRunning)
                host->preRender(frame.renderDt);
            // **Again, after `PreRender`** (ADR 0134): a camera a game writes
            // on the frame's clock -- `PreRender`, a `Rate = Render` look -- is
            // drawn where it was written this frame, not where the poses
            // resolved before the scripts ran left it.
            framePoses.begin(host->world(), drawnBetweenTicks ? &transformHistory : nullptr, renderAlpha);
            // The camera as this frame draws it, for the next tick's pointer
            // -- only where a window shows it: a headless client, networked
            // or driven, points at nothing (ADR 0134, "What this touches").
            {
                scene::World& world = host->world();
                const scene::WorkspaceComponent* space = world.workspaces().find(host->workspace());
                const bool drawnCamera = !options.headless && drawnBetweenTicks && space != nullptr &&
                                         world.cameras().find(space->currentCamera) != nullptr;
                world.engineState().drawnCameraValid = drawnCamera;
                if (drawnCamera)
                    world.engineState().drawnCamera = framePoses.camera(space->currentCamera);
            }

            // Loading comes BEFORE extraction, and the order is load-bearing:
            // `extract` reads the mesh library, so a MeshPart whose file has not
            // been read yet contributes nothing. Running the loader afterwards
            // made every newly created MeshPart invisible for exactly one frame
            // -- which a golden records faithfully and a person notices as a
            // flicker they cannot reproduce.
            meshCache.beginFrame(*device);
            // Frees the viewport targets a resize replaced, once they are old
            // enough that no command list still in flight can name one. Beside
            // the mesh cache's own retirement because it is the same rule.
            viewportTarget.retire(*device);

            // **Every world that can be drawn, not just the game's** (D115).
            // The editor has two -- the game's, and the stage a stamp opens onto
            // -- and `extract` below already knows that. The loader did not: it
            // read `host->world()` and nothing else, so a `MeshPart` or a
            // `Material` that existed only on the stage had no mesh uploaded and
            // no map loaded, and the stamp being edited drew as untextured
            // geometry. The material preview is where this is most visible,
            // because a material with four maps and none of them loaded is a
            // grey ball, which looks exactly like a material that does not work.
            //
            // Both, rather than "whichever is on screen", because the libraries
            // are shared and keyed by content: loading the game's meshes while a
            // stamp is open is what makes closing the stamp instant instead of a
            // frame of missing geometry.
            const auto loadFor = [&](scene::World& world, core::InstanceId workspace) {
                if (renderer == nullptr || !renderer->valid())
                    return;
                meshLoader.syncPrimitives(*device, *cmd, world, meshCache, meshLibrary);
                (void)meshLoader.syncTextures(*device, *cmd, world, textureLibrary);
                // The camera textures of the game's own world -- not of a stamp
                // being edited, which has no game running in it.
                if (&world == &host->world() && stageOf() == nullptr) {
                    viewHost.sync(*device, *cmd, world, workspace, textureLibrary, renderer.get(), &framePoses);
                    // The pictures the UI lends from those views, remade or
                    // gone, before anything copies the table.
                    uiText.refreshViews();
                }
                // The palette's picture: the tileset of the tilemap the Tiles
                // tool would paint, as loaded (the 2D layer).
                if (options.editor && &world == &authored()) {
                    Editor::TilesetPreview preview;
                    const core::InstanceId painted = Editor::tilemapFor(world, inspector, workspace);
                    if (const scene::Tilemap2DComponent* tilemap =
                            painted.valid() ? world.tilemaps2d().find(painted) : nullptr;
                        tilemap != nullptr && tilemap->tileset.valid()) {
                        preview.texture = textureLibrary.find(tilemap->tileset);
                        preview.pixels = textureLibrary.sizeOf(tilemap->tileset);
                        preview.tileSize = tilemap->tileSize;
                    }
                    editor.setTilesetPreview(preview);
                }
                // **Only what finished, not the whole library** (D124). A mesh
                // that lands means the physics mirror can be told what it
                // collides as -- and this used to walk every entry whenever the
                // count was non-zero, copying every loaded mesh's positions and
                // freeing the previous copy, on every frame anything landed.
                // Synchronous loading hid it inside one or two frames; spreading
                // N completions over N frames makes it N(N+1)/2 copies.
                //
                // Pushed from here because this is the one place that can see
                // both the render library and the mirror -- `render` is L4 and
                // `scene` is L3, and neither is allowed to reach the other.
                meshCompletions.clear();
                (void)meshLoader.sync(*device, *cmd, world, workspace, meshCache, meshLibrary, nullptr,
                                      &meshCompletions);

                // **A `MeshPart` waiting for its mesh is sized to it** the frame
                // the mesh is known (`Editor::meshFits`): `MeshSize` and `Size`
                // become what it measures, and one dropped on a surface stands
                // on it. Not an undo step of its own -- it is the rest of the
                // placing that recorded one.
                if (options.editor) {
                    std::vector<Editor::MeshFit>& fits = editor.meshFits();
                    scene::World& authoring = authored();
                    const core::NameAtom sizeName = authoring.atoms().intern("Size");
                    const core::NameAtom meshSizeName = authoring.atoms().intern("MeshSize");
                    const core::NameAtom frameName = authoring.atoms().intern("CFrame");
                    for (auto fit = fits.begin(); fit != fits.end();) {
                        const scene::MeshPartComponent* mesh =
                            authoring.alive(fit->part) ? authoring.meshParts().find(fit->part) : nullptr;
                        const render::MeshLibrary::Entry* entry =
                            mesh != nullptr ? meshLibrary.find(mesh->meshContent) : nullptr;
                        const core::AABB bounds = entry != nullptr ? entry->bounds : core::AABB{};
                        if (entry != nullptr && bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y &&
                            bounds.min.z <= bounds.max.z) {
                            const core::Vec3 measured{std::max(bounds.max.x - bounds.min.x, 0.01f),
                                                      std::max(bounds.max.y - bounds.min.y, 0.01f),
                                                      std::max(bounds.max.z - bounds.min.z, 0.01f)};
                            (void)authoring.setProperty(fit->part, meshSizeName, scene::Value{measured});
                            (void)authoring.setProperty(fit->part, sizeName, scene::Value{measured});
                            if (fit->restOn.has_value()) {
                                // Its lowest point on the surface, its middle
                                // over the point: drawn at its own size, the
                                // mesh's bounds are where they say.
                                core::CFrameD frame;
                                frame.position = core::DVec3{
                                    fit->restOn->x - static_cast<core::f64>(bounds.min.x + bounds.max.x) * 0.5,
                                    fit->restOn->y - static_cast<core::f64>(bounds.min.y),
                                    fit->restOn->z - static_cast<core::f64>(bounds.min.z + bounds.max.z) * 0.5};
                                (void)authoring.setProperty(fit->part, frameName, scene::Value{frame});
                            }
                            fit = fits.erase(fit);
                        }
                        else if (mesh == nullptr || ++fit->frames > 600) {
                            fit = fits.erase(fit);
                        }
                        else {
                            ++fit;
                        }
                    }
                }
                if (host->physics() != nullptr) {
                    for (const core::NameAtom content : meshCompletions) {
                        const render::MeshLibrary::Entry* entry = meshLibrary.find(content);
                        if (entry != nullptr && !entry->positions.empty())
                            host->physics()->setCollisionPoints(content, entry->positions);
                    }
                }

                // **Terrain, at the mesh loader's own safe point and BEFORE
                // extraction** (F1 Part E). The command list is open and no pass
                // is, which is what an upload needs -- and the ordering is not a
                // preference: anything registered after `extract` is invisible
                // for exactly one frame, which reads as a flicker nobody can
                // reproduce.
                //
                // The atom table is the world's, because the URN a tile is filed
                // under has to be the same atom `extract` looks up.
                (void)terrainLoader.sync(*device, *cmd, world, world.atoms(), meshCache, meshLibrary);
                (void)voxelLoader.sync(*device, *cmd, world, world.atoms(), meshCache, meshLibrary);
                // Foliage over the terrain (ADR 0116), grown here for the
                // reason the terrain is: an upload, before the extract.
                foliage.sync(*device, *cmd, world);
            };
            loadFor(host->world(), host->workspace());
            if (Editor::Stage* const openStage = stageOf(); openStage != nullptr)
                loadFor(openStage->world(), openStage->workspace());

            // Extraction happens once, at a known moment, from a world that is
            // between ticks (ADR 0027). Rendering never walks the ECS.
            // The aspect comes from the target rather than from the camera:
            // a `Camera` has no ViewportSize in this release (it needs a
            // Vector2, which the UI brings at M6), and the renderer is the one
            // that knows how many pixels it is filling.
            const f32 aspect =
                targetHeight == 0 ? 1.0f : static_cast<f32>(targetWidth) / static_cast<f32>(targetHeight);
            const f32 shadowRadius = renderer != nullptr && renderer->valid() ? renderer->shadowRadius() : 0.0f;
            // Paused: the editor's own view. Playing: the game's, unchanged --
            // which is what makes the viewport show the GAME when you press play
            // rather than a tool's idea of it.
            //
            // **Unless the game has no camera at all** (D088). Pressing play on a
            // project whose scripts never made one left nothing to render
            // through, and the frame fell back to the M1 "the engine is alive"
            // pulse -- a background cycling through colours where a world should
            // be. The editor's camera is the fallback and never the winner: a
            // game with a camera is still shown through its own, which is the
            // whole of what pressing play means.
            // The 2D view is the same camera through an orthographic lens.
            const render::ViewOverride editorView{editor.cameraCFrame(),   EditorFieldOfView,        0.1f, 5000.0f,
                                                  editor.view2D() ? 1 : 0, editor.orthographicSize()};
            // **And "has a camera" has to mean the same thing here as it does
            // in `extract`, or the fallback misses exactly the case it is for.**
            // `Workspace.CurrentCamera` is a REFERENCE: deleting the camera in
            // the editor leaves the property naming a dead id, and an id stays
            // structurally valid after the instance it named is gone. So this
            // asked "is the field set" while `extract` asked "can I draw through
            // it", the two disagreed for a world whose camera had been deleted,
            // and the frame fell back to the M1 pulse -- the same rainbow D088
            // and D091 each removed from a different direction, arriving from a
            // third. The predicate is now `extract`'s, spelled out: alive, not
            // retiring, and actually a camera.
            const core::InstanceId gameCamera = host->currentCamera();
            const bool gameHasCamera = host->world().alive(gameCamera) && !host->world().destroyed(gameCamera) &&
                                       host->world().cameras().find(gameCamera) != nullptr;
            // **Or the view was DETACHED** (S5.8). Pressing play hands the
            // view to the game's camera, which is what pressing play means;
            // detaching takes the view back without touching the simulation, so
            // the world keeps ticking and you fly around and watch. It is the
            // only way to see a running game from anywhere other than where it
            // puts you -- an enemy behind a wall, a chunk that failed to stream,
            // a character stuck inside geometry the player camera is inside of
            // too.
            const bool useEditorView = options.editor && editor.cameraAdopted() &&
                                       (editing(editor.runState()) || editor.cameraDetached() || !gameHasCamera);
            // **The selection, drawn as a silhouette rather than as a box.** The
            // wire box E1 shipped says where a thing's BOUNDS are, which for
            // anything that is not a cube is a shape the object does not have --
            // and around a tree or a character it is a box floating in the air
            // near the thing you clicked. What the renderer gets is a flag per
            // draw; what it makes of it is a mask and an outline of its edge.
            //
            // Only while EDITING. In play mode the world belongs to the game and
            // a tool's mark on it would be in every screenshot somebody takes of
            // their own game.
            //
            // **And everything inside what is selected.** A `Model` is not a
            // draw, so outlining only the selection lit nothing when a model was
            // clicked -- the one selection whose extent somebody most needs to
            // see. Its descendants are what it looks like.
            static std::vector<core::InstanceId> outlinedTree;
            outlinedTree.clear();
            if (options.editor && editing(editor.runState())) {
                for (const core::InstanceId id : inspector.selectionSet()) {
                    outlinedTree.push_back(id);
                    if (authored().alive(id) && authored().firstChild(id).valid())
                        authored().collectDescendants(id, outlinedTree);
                }
            }
            const std::span<const core::InstanceId> outlined = outlinedTree;
            // **The stage, when one is open.** A prefab is looked at on its
            // own; drawing it inside the game's scene is what a person saw and
            // called wrong, and it was.
            // The terrain nodes the loader chose for this camera (ADR 0082).
            const std::vector<render::TerrainNodeDraw> terrainNodes = terrainLoader.draws(authored());
            render::extract(authored(), stageOf() != nullptr ? stageOf()->workspace() : host->workspace(),
                            stageOf() != nullptr ? stageOf()->lighting() : host->lighting(), meshLibrary, aspect,
                            shadowRadius, host->animation(), framePoses, snapshot,
                            useEditorView ? &editorView : nullptr, outlined, &textureLibrary, terrainNodes);
            // The terrains' palettes, which their shader reads, for the same
            // world and the same root.
            terrainLoader.appendRenderTerrains(authored(),
                                               stageOf() != nullptr ? stageOf()->workspace() : host->workspace(),
                                               snapshot, &textureLibrary);
            // **Particles, on the render clock** (F2): advanced by this frame's
            // own length -- which a headless run fixes at one tick, so a golden
            // with sparks in it is still one picture -- and appended for the
            // same root the extract drew.
            particles.update(authored(), stageOf() != nullptr ? stageOf()->workspace() : host->workspace(),
                             frame.renderDt, &framePoses);
            particles.append(snapshot);
            // The foliage the tiles hold, as runs and buckets for the cull.
            foliage.append(authored(), meshLibrary, snapshot, &textureLibrary);
            // The sky's pictures, for the sky the extract resolved: a bake
            // started, or a finished one uploaded -- before any render pass.
            if (renderer != nullptr && renderer->valid()) {
                skyLoader.sync(*device, *cmd, authored(), snapshot.look.sky);
                skyLoader.append(snapshot);
            }
            // The UI is laid out against the TARGET's size rather than the
            // window's: an offscreen render at 640x360 has to produce the
            // layout that resolution would, which is the whole of what the
            // two-resolution goldens check.
            // The matrices the image was drawn with, not the ones the next
            // frame will use. A pick taken against a fresher camera lands
            // wherever the camera moved to between the click and the walk,
            // which is invisible standing still and wrong while walking.
            if (options.editor && snapshot.camera.valid)
                editor.setCamera(snapshot.camera.projection, snapshot.camera.view, snapshot.camera.origin);
            // **Where terrain is levelled from next frame.** The loader runs
            // before `extract` has decided this frame's camera, so it is handed
            // the one this frame was drawn through -- a frame of lag in a
            // level-of-detail choice is invisible.
            if (snapshot.camera.valid) {
                terrainLoader.setFocus(snapshot.camera.origin);
                voxelLoader.setFocus(snapshot.camera.origin);
                foliage.setFocus(snapshot.camera.origin);
            }

            const core::Vec2 uiViewport{static_cast<f32>(targetWidth), static_cast<f32>(targetHeight)};
            host->world().engineState().viewportSize = uiViewport;
            if (uiViewport != lastUiViewport) {
                // A scale is a fraction of something that just changed, so
                // every tree is stale. Marked here rather than in the resize
                // handler because an offscreen target can change size with no
                // window event at all.
                host->world().screenGuis().forEach(
                    [](core::InstanceId, scene::ScreenGuiComponent& screen) { screen.layoutDirty = true; });
                lastUiViewport = uiViewport;
            }
            ui::layout(host->world(), host->uiService(), uiViewport);

            // Interaction reads the rectangles the layout just produced, and it
            // runs here rather than beside the event pump for that reason: a hit
            // test against last frame's rectangles is a click that lands where a
            // button used to be.
            const input::DeviceState& devices = host->input().snapshot();
            ui::InteractionInput interaction;
            interaction.pointer = devices.pointer;
            interaction.pressed = uiPointerDown && !lastUiPointerDown;
            interaction.released = !uiPointerDown && lastUiPointerDown;
            interaction.text = uiTypedText;
            interaction.backspace = uiBackspace;
            interaction.forwardDelete = uiForwardDelete;
            interaction.caretLeft = uiCaretLeft;
            interaction.caretRight = uiCaretRight;
            interaction.caretHome = uiCaretHome;
            interaction.caretEnd = uiCaretEnd;
            interaction.submit = uiSubmit;
            // **A button printed on a wall is a button** (F3): the pointer's ray
            // into the world, met with every `SurfaceGui` and `BillboardGui`,
            // and hidden by anything solid in front of the canvas -- asked of
            // the physics world, leaving out the part the canvas is on.
            if (snapshot.camera.valid) {
                const core::DVec3 cameraOrigin = snapshot.camera.origin;
                const app::SolidAlong solidAlong = [&host,
                                                    cameraOrigin](core::Vec3 origin, core::Vec3 direction,
                                                                  core::InstanceId adornee) -> std::optional<f32> {
                    const scene::PhysicsSync* physics = host->physics();
                    if (physics == nullptr)
                        return std::nullopt;
                    const std::array<u64, 1> excluded{physics->userDataOf(adornee)};
                    physics::QueryFilter filter;
                    filter.mode = physics::QueryFilter::Mode::Exclude;
                    filter.userData = std::span<const u64>{excluded.data(), adornee.valid() ? 1u : 0u};
                    constexpr f32 Reach = 2000.0f;
                    physics::RayHit hit;
                    const physics::RayD ray{core::DVec3{cameraOrigin.x + static_cast<f64>(origin.x),
                                                        cameraOrigin.y + static_cast<f64>(origin.y),
                                                        cameraOrigin.z + static_cast<f64>(origin.z)},
                                            direction * Reach};
                    if (!physics->backend().raycast(physics->worldHandle(), ray, filter, hit))
                        return std::nullopt;
                    return hit.distance;
                };
                if (const std::optional<app::WorldUiPick> picked =
                        app::pickWorldUi(host->world(), host->workspace(), host->uiService(), uiViewport,
                                         snapshot.camera, devices.pointer, solidAlong, &framePoses))
                    interaction.worldOver = picked->element;
            }
            lastUiPointerDown = uiPointerDown;
            const ui::InteractionResult uiResult = ui::updateInteraction(host->world(), host->uiService(), interaction);
            host->input().setPointerCapturedByUi(uiResult.pointerOverUi);
            // The keyboard half of the same claim (ADR 0041): a focused
            // `TextInput` eats the keys, so typing into a chat box does not also
            // drive the character.
            host->input().setKeyboardCapturedByUi(uiResult.textInputFocused);
            // And the platform asked for the characters, which nothing did
            // (D204): on a phone this is what raises the keyboard.
            if (window != nullptr) {
                const bool editorTyping = overlay.has_value() && overlay->editorTyping();
                if (const std::optional<bool> typing = textInputFocus.follow(uiResult.textInputFocused, editorTyping))
                    platform::setTextInputEnabled(platform::windowId(*window), *typing);
            }

            ui::buildDrawList(host->world(), host->uiService(), uiDrawList);
            // Prompts over the game's own screen, never under it (ADR 0126).
            appendPrompts(host->world(), framePoses, snapshot.camera, uiViewport, uiDrawList);
            // Index 0 is "no texture" and every entry after it is a texture the
            // UI can name. The glyph atlas is index 1 when a face has been
            // rasterised; images follow it.
            // Index 0 is "no texture" and resolves to the renderer's white
            // pixel; index 1 is the glyph atlas and everything after it is an
            // image. The table is rebuilt each frame because a texture handle
            // is four bytes and a stale one is a picture from a world that has
            // been unloaded.
            uiTextures.clear();
            uiTextures.push_back(rhi::TextureHandle{});
            uiTextures.push_back(uiText.atlasTexture());
            for (const rhi::TextureHandle image : uiText.images())
                uiTextures.push_back(image);
            uiGradients.clear();
            buildUiGeometry(uiDrawList, uiViewport, uiVertices, uiRuns, uiTextures, uiGradients);

            // **The world's UI, into the picture the renderer is about to
            // draw** (F3): laid out and drawn by the same code as the screen's,
            // placed on its parts and billboards, and sharing the screen's
            // glyph atlas and images.
            buildWorldUi(host->world(), host->workspace(), host->uiService(), uiViewport, uiTextures, worldUiDrawList,
                         snapshot, &uiGradients, &framePoses);

            frameVisibleObjects = 0;
            frameTriangles = 0;
            frameLodDraws = 0;
            // The SAME level the renderer will choose, from the same function.
            // Counting level zero here while the backend drew level two would
            // be a triangle count that describes a frame nobody rendered, and a
            // stat that lies is worse than one that is missing.
            const f32 lodPixelsPerUnit =
                snapshot.camera.valid && uiViewport.y > 0.0f && !core::isOrthographic(snapshot.camera.projection)
                    ? 0.5f * uiViewport.y * snapshot.camera.projection.m[1][1]
                    : 0.0f;
            for (const render::DrawItem& draw : snapshot.draws) {
                // Counted from the snapshot rather than from the backend: it is
                // the same number, it costs nothing, and it is available on a
                // device that rasterizes nothing.
                if (!draw.inCameraFrustum)
                    continue;
                const render::MeshCache::Resolved* resolved = meshCache.resolve(draw.mesh);
                if (resolved == nullptr || resolved->lods.empty())
                    continue;
                const core::u32 level = render::selectMeshLod(*resolved, draw.transform, lodPixelsPerUnit);
                const render::MeshLodRange& range = resolved->lods[level];
                if (draw.section >= range.sectionCount ||
                    range.firstSection + draw.section >= resolved->sections.size())
                    continue;
                ++frameVisibleObjects;
                if (level > 0)
                    ++frameLodDraws;
                frameTriangles += resolved->sections[range.firstSection + draw.section].indexCount / 3u;
            }

            submitWorld(snapshot, debugDraw);

            // Read AFTER submission, because that is when the renderer knows.
            // Counted by the renderer rather than derived from the snapshot: a
            // draw call is a thing a backend issues, and inferring it from the
            // draw list is exactly the assumption instancing invalidated.
            const render::RendererStats rendererStats =
                renderer != nullptr && renderer->valid() ? renderer->stats() : render::RendererStats{};
            frameDrawCalls = rendererStats.drawCalls;
            frameInstancedDraws = rendererStats.instancedDraws;

            // Everything in the buffer is in world coordinates until here: the
            // wire boxes above, and whatever `DebugService` recorded during the
            // tick, which happened before extraction had decided where the
            // camera was. The overlay is drawn with the renderer's
            // camera-relative view-projection, so the buffer has to be moved
            // into that space -- and for the whole of M4 it was not, which put
            // every debug line the camera's own distance away from where it
            // belonged. `origin` is zero when no camera resolved, which is
            // exactly the M1 path this must not disturb.
            debugDraw.rebaseTo(snapshot.camera.origin);

            // After the rebase and not before it, because these are already in
            // the space it converts to -- see `submitSelection`. Everything the
            // editor draws over the world goes here for the same reason.
            if (options.editor) {
                // **The wire box only when nothing else marks the selection.**
                // The renderer outlines a selected part along its own
                // silhouette; a box drawn over that, with a margin of 1% of the
                // part's size, stood metres outside a large part -- reported as
                // "the selection is outside the part". It stays for the debug
                // path, which draws the world as wire boxes and has no outline.
                const bool outlinedByRenderer = renderer != nullptr && renderer->valid() && snapshot.camera.valid;
                if (!outlinedByRenderer)
                    submitSelection(host->world(), framePoses, inspector.selectionSet(), snapshot.camera.origin,
                                    debugDraw);
                if (editing(editor.runState())) {
                    submitCameraVolumes(authored(), inspector.selectionSet(), snapshot.camera.origin, aspect,
                                        debugDraw);
                    submitLightVolumes(authored(), inspector.selectionSet(), snapshot.camera.origin, debugDraw);
                    submitDetectorVolumes(authored(), inspector.selectionSet(), snapshot.camera.origin, debugDraw);
                }
                // The manipulator over the outline, because the outline says
                // WHAT is selected and the manipulator is the thing being
                // aimed at.
                if (const std::optional<GizmoFrame> gizmo = editor.gizmoFrame(host->world(), inspector);
                    gizmo.has_value()) {
                    submitGizmo(*gizmo, editor.gizmoMode(), editor.gizmoHandle(), snapshot.camera.origin, debugDraw);
                }
                // **The aiming ring, in the rebased space like everything else
                // here.** Half a millimetre of f32 error at four kilometres
                // would land on the one thing in the frame being placed
                // precisely, which is why the overlay's header says to submit
                // after the rebase rather than with the world-space draws.
                if (const std::optional<asset::TerrainHit> aim = editor.brushAim(); aim.has_value()) {
                    const core::DVec3 origin = snapshot.camera.origin;
                    const core::Vec3 centre{static_cast<f32>(aim->position.x - origin.x),
                                            static_cast<f32>(aim->position.y - origin.y),
                                            static_cast<f32>(aim->position.z - origin.z)};
                    drawBrushRing(centre, aim->normal, editor.brush().radius, debugDraw);
                }
                // **The cell a block click would change**, as a box a hair
                // larger than the block so it is not buried in the faces it
                // outlines. Amber to place, red to take away, the brush's
                // colour otherwise.
                if (const std::optional<std::array<core::i32, 3>> cell = editor.blockTarget(); cell.has_value()) {
                    const double size = static_cast<double>(editor.voxelBlockSize());
                    const core::DVec3 origin = snapshot.camera.origin;
                    const core::Vec3 centre{
                        static_cast<f32>((static_cast<double>((*cell)[0]) + 0.5) * size - origin.x),
                        static_cast<f32>((static_cast<double>((*cell)[1]) + 0.5) * size - origin.y),
                        static_cast<f32>((static_cast<double>((*cell)[2]) + 0.5) * size - origin.z)};
                    const auto half = static_cast<f32>(size * 0.51);
                    const render::DebugColor color = editor.blockOp() == Editor::BlockOp::Break
                                                         ? render::DebugColor::fromLinear(0.95f, 0.25f, 0.2f, 1.0f)
                                                         : render::DebugColor::fromLinear(0.95f, 0.75f, 0.25f, 1.0f);
                    debugDraw.wireBox(centre, core::Vec3{half, half, half}, color);
                }
                // **The cell a Tiles click would change** (the 2D layer), as a
                // flat box on the plane: amber to paint, red to erase.
                if (const std::optional<Editor::TileAim> aim = editor.tileAim(); aim.has_value()) {
                    if (const scene::Tilemap2DComponent* tilemap = authored().tilemaps2d().find(aim->tilemap);
                        tilemap != nullptr) {
                        const double size = static_cast<double>(tilemap->cellSize);
                        const core::DVec3 origin = snapshot.camera.origin;
                        const core::Vec3 centre{
                            static_cast<f32>(static_cast<double>(tilemap->position.x) +
                                             (static_cast<double>(aim->cell[0]) + 0.5) * size - origin.x),
                            static_cast<f32>(static_cast<double>(tilemap->position.y) +
                                             (static_cast<double>(aim->cell[1]) + 0.5) * size - origin.y),
                            static_cast<f32>(-origin.z)};
                        const auto half = static_cast<f32>(size * 0.5);
                        const render::DebugColor color =
                            editor.tileOp() == Editor::TileOp::Erase
                                ? render::DebugColor::fromLinear(0.95f, 0.25f, 0.2f, 1.0f)
                                : render::DebugColor::fromLinear(0.95f, 0.75f, 0.25f, 1.0f);
                        debugDraw.wireBox(centre, core::Vec3{half, half, 0.01f}, color);
                    }
                }
            }

            // Uploaded before the render pass opens, because a copy cannot run
            // inside one -- the seam says so and the backend enforces it. The
            // mesh loader is here for the same reason and one more: it is the
            // FrameStart safe point, so a file read cannot land mid-tick.
            if (debugRenderer.valid())
                debugRenderer.upload(*device, *cmd, debugDraw);
            // Beside the other uploads and for the same reason: this is the
            // only place in the frame where a copy is legal. Idempotent after
            // the first success, so the cost is one branch a frame.
            if (options.editor && !iconAtlas.ready()) {
                if (iconAtlas.load(*device, *cmd, platform::paths().contentDir, options.scriptPath))
                    core::logText(core::LogLevel::Info, iconAtlas.status());
                else
                    core::logText(core::LogLevel::Warn, iconAtlas.status());
            }
            // What the browser asked for while it drew LAST frame, answered
            // here, where a copy is legal -- so a row that showed its icon on
            // the frame it appeared shows the picture on the next one. A shell
            // with no browser never asks, and this walks an empty list.
            if (options.editor)
                // **Built on the first frame that has a renderer**, not at declaration:
                // it borrows one, and `createRenderer` can fail on a machine whose
                // shaders are missing. A cache with no preview renderer refuses a mesh
                // at `request` and the row wears its icon, which is what every build did
                // before previews existed -- so this arriving late is a picture that
                // appears, never a frame that breaks.
                if (previewRenderer == nullptr && renderer != nullptr && renderer->valid()) {
                    previewRenderer = std::make_unique<HostPreviewRenderer>(
                        host->world().classes(), host->world().enums(), host->world().atoms(), contentRoot,
                        contentMounts, *renderer);
                    previewRenderer->setMaterialLibrary(&materialLibrary);
                    thumbnails.setPreviewRenderer(previewRenderer.get());
                }
            thumbnails.flush(*device, *cmd);
            if (uiRenderer.valid())
                // The atlas first: `buildUiGeometry` has already written UVs
                // into it, and uploading after the draw would show this frame's
                // new glyphs as last frame's pixels.
                uiText.sync(*device, *cmd);
            uiRenderer.upload(*device, *cmd, uiVertices, uiRuns);
            // The gradient table, before any pass, for both UIs (ADR 0110).
            uiRenderer.uploadGradients(*cmd, uiGradients.pixels(), uiGradients.count());
            snapshot.worldUiGradients = uiRenderer.gradientTable();

            // The real renderer owns the target when there is a camera to look
            // through. Without one -- an empty project, a world booting, a
            // camera nobody assigned -- the M1 debug path still draws, which is
            // what keeps every earlier example and the capture golden working.
            const bool useRenderer = renderer != nullptr && renderer->valid() && snapshot.camera.valid;
#if ENG_DEBUG_UI
            // **A screenshot is of the world as it will look** (ADR 0091): the
            // frame it is taken from waits for surface shaders still compiling,
            // which no interactive frame ever does.
            if (useRenderer && surfaceCompiler != nullptr && !options.screenshotPath.empty() && options.frames != 0 &&
                frame.index + 1 >= options.frames)
                surfaceCompiler->drain();
#endif
            // **The camera textures, before the main view** (ADR 0107), so a
            // feed on a monitor shows this frame's picture. Each is the world
            // extracted from its camera and drawn with its own renderer state;
            // a camera that sees its own texture sees the last picture.
            if (useRenderer && stageOf() == nullptr) {
                // **Each running sub-world's content, loaded as the game's
                // is** (ADR 0107 §3), whether or not its picture is drawn this
                // frame: a mesh that lands also tells its physics what it
                // collides as. What unloaded gives its GPU state back.
                std::erase_if(subWorldGpu, [&](const std::unique_ptr<SubWorldGpu>& gpu) {
                    const std::span<const WorldHost::SubWorldRun> runs = host->subWorlds();
                    if (std::any_of(runs.begin(), runs.end(),
                                    [&](const WorldHost::SubWorldRun& run) { return run.serial == gpu->serial; }))
                        return false;
                    gpu->loader.destroy(*device);
                    gpu->meshes.destroy(*device);
                    return true;
                });
                for (const WorldHost::SubWorldRun& run : host->subWorlds()) {
                    auto found = std::find_if(subWorldGpu.begin(), subWorldGpu.end(),
                                              [&](const auto& gpu) { return gpu->serial == run.serial; });
                    if (found == subWorldGpu.end()) {
                        auto made = std::make_unique<SubWorldGpu>();
                        made->serial = run.serial;
                        if (made->meshes.create(*device).has_value())
                            continue;
                        made->loader.setContentRoot(contentRoot);
                        made->loader.setContentMounts(&contentMounts);
                        made->loader.setDeferredTextures(!options.headless);
                        made->loader.setDeferredMeshes(!options.headless);
                        subWorldGpu.push_back(std::move(made));
                        found = subWorldGpu.end() - 1;
                    }
                    SubWorldGpu& gpu = **found;
                    WorldHost& inner = *run.host;
                    gpu.meshes.beginFrame(*device);
                    gpu.loader.syncPrimitives(*device, *cmd, inner.world(), gpu.meshes, gpu.library);
                    (void)gpu.loader.syncTextures(*device, *cmd, inner.world(), gpu.textures);
                    meshCompletions.clear();
                    (void)gpu.loader.sync(*device, *cmd, inner.world(), inner.workspace(), gpu.meshes, gpu.library,
                                          nullptr, &meshCompletions);
                    if (inner.physics() != nullptr) {
                        for (const core::NameAtom content : meshCompletions) {
                            const render::MeshLibrary::Entry* entry = gpu.library.find(content);
                            if (entry != nullptr && !entry->positions.empty())
                                inner.physics()->setCollisionPoints(content, entry->positions);
                        }
                    }
                }
                const auto subWorldRunning = [&](core::InstanceId owner) {
                    const scene::SubWorldComponent* self = host->world().subWorlds().find(owner);
                    return self != nullptr && self->running && host->subWorld(owner) != nullptr;
                };

                for (ViewHost::View* view : viewHost.due(host->world(), frame.index, subWorldRunning)) {
                    const scene::World& world = host->world();
                    // **A `SubWorld`** (ADR 0107 §3): its world, from its own
                    // current camera, with its own meshes -- between its ticks,
                    // which are this world's (ADR 0134).
                    if (view->subWorld) {
                        const scene::SubWorldComponent* self = world.subWorlds().find(view->owner);
                        const WorldHost::SubWorldRun* run = nullptr;
                        for (const WorldHost::SubWorldRun& candidate : host->subWorlds()) {
                            if (candidate.owner == view->owner)
                                run = &candidate;
                        }
                        const auto gpu = run == nullptr
                                             ? subWorldGpu.end()
                                             : std::find_if(subWorldGpu.begin(), subWorldGpu.end(),
                                                            [&](const auto& g) { return g->serial == run->serial; });
                        if (self == nullptr || gpu == subWorldGpu.end())
                            continue;
                        WorldHost& inner = *run->host;
                        const core::u64 started = platform::nowNs();
                        render::DrawPoses innerPoses;
                        // A paused world steps none of its sub-worlds, whose
                        // history then stands still: drawn at the tick.
                        const bool innerBetween = drawnBetweenTicks && !world.engineState().paused;
                        innerPoses.begin(inner.world(), innerBetween ? run->history.get() : nullptr, renderAlpha);
                        render::extract(inner.world(), inner.workspace(), inner.lighting(), (*gpu)->library,
                                        static_cast<f32>(view->width) / static_cast<f32>(view->height), shadowRadius,
                                        inner.animation(), innerPoses, viewSnapshot, nullptr, {}, &(*gpu)->textures,
                                        {});
                        if (!viewSnapshot.camera.valid)
                            continue;
                        if (self->quality == 1) {
                            viewSnapshot.environment.globalShadows = false;
                            viewSnapshot.look.bloomGoverned = true;
                            viewSnapshot.look.bloomEnabled = false;
                            viewSnapshot.look.graded = false;
                            viewSnapshot.look.blurSize = 0.0f;
                            viewSnapshot.look.depthOfField = false;
                            viewSnapshot.look.sunRays = false;
                        }
                        renderer->render(*device, *cmd,
                                         {.color = view->texture,
                                          .colorFormat = ViewFormat,
                                          .width = view->width,
                                          .height = view->height,
                                          .view = view->rendererView},
                                         viewSnapshot, (*gpu)->meshes);
                        ViewHost::drawn(*view, frame.index,
                                        static_cast<core::f64>(platform::nowNs() - started) / 1'000'000.0);
                        continue;
                    }
                    // **A `ViewportFrame`** (ADR 0107): the instances inside
                    // it and nothing else, by its own light, with no sky and
                    // nothing behind -- only what is inside shows. Nothing in
                    // it is simulated, so it is drawn as it stands.
                    if (view->frame) {
                        const scene::ViewportFrameComponent* self = world.viewportFrames().find(view->owner);
                        const f32 shape = static_cast<f32>(view->width) / static_cast<f32>(view->height);
                        const std::optional<render::ViewOverride> lens =
                            frameLens(world, view->owner, shape, &framePoses);
                        if (self == nullptr || !lens.has_value())
                            continue;
                        const core::u64 started = platform::nowNs();
                        render::extract(world, view->owner, core::InstanceId{}, meshLibrary, shape, shadowRadius,
                                        host->animation(), framePoses, viewSnapshot, &*lens, {}, &textureLibrary, {});
                        render::RenderEnvironment& light = viewSnapshot.environment;
                        const core::Vec3 d = self->lightDirection;
                        const f32 length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                        light.sunDirection = core::Vec3{-d.x / length, -d.y / length, -d.z / length};
                        light.ambient = self->ambient;
                        light.outdoorAmbient = self->ambient;
                        light.lightTint = self->lightColor;
                        light.fogEnd = 0.0f;
                        light.globalShadows = false;
                        light.autoExposure = false;
                        light.exposureCompensation = 0.0f;
                        light.transparentBackground = true;
                        viewSnapshot.look = render::RenderLook{};
                        viewSnapshot.look.bloomGoverned = true;
                        viewSnapshot.look.bloomEnabled = false;
                        renderer->render(*device, *cmd,
                                         {.color = view->texture,
                                          .colorFormat = ViewFormat,
                                          .width = view->width,
                                          .height = view->height,
                                          .view = view->rendererView},
                                         viewSnapshot, meshCache);
                        ViewHost::drawn(*view, frame.index,
                                        static_cast<core::f64>(platform::nowNs() - started) / 1'000'000.0);
                        continue;
                    }
                    const scene::CameraTextureComponent* source = world.cameraTextures().find(view->owner);
                    const scene::CameraComponent* camera =
                        source != nullptr ? world.cameras().find(source->camera) : nullptr;
                    if (camera == nullptr)
                        continue;
                    const core::u64 started = platform::nowNs();
                    // **A view never samples itself.** A decal or a material
                    // showing this view's own name, seen by this view's
                    // camera, would read the texture being drawn into; taken
                    // out of the library while it draws, it is simply absent
                    // from its own picture.
                    const rhi::TextureHandle own = textureLibrary.take(view->urn);
                    // Where that camera is DRAWN, as the world around it is
                    // (ADR 0134): a feed from a camera on a moving part
                    // otherwise jumped a tick at a time.
                    const render::ViewOverride lens{.cframe = framePoses.camera(source->camera),
                                                    .fieldOfView = camera->fieldOfView,
                                                    .nearPlane = camera->nearPlane,
                                                    .farPlane = camera->farPlane,
                                                    .projection = camera->projection,
                                                    .orthographicSize = camera->orthographicSize,
                                                    .clipPlane = camera->clipPlane,
                                                    .clipPlaneOn = camera->clipPlaneOn};
                    render::extract(world, host->workspace(), host->lighting(), meshLibrary,
                                    static_cast<f32>(view->width) / static_cast<f32>(view->height), shadowRadius,
                                    host->animation(), framePoses, viewSnapshot, &lens, {}, &textureLibrary,
                                    terrainNodes);
                    terrainLoader.appendRenderTerrains(world, host->workspace(), viewSnapshot, &textureLibrary);
                    particles.append(viewSnapshot);
                    skyLoader.append(viewSnapshot);
                    viewSnapshot.worldUiGradients = uiRenderer.gradientTable();
                    // `Simple`: no shadows and none of the look's effects; the
                    // sky and the air stay, because a feed is of this world.
                    if (source->quality == 1) {
                        viewSnapshot.environment.globalShadows = false;
                        viewSnapshot.look.bloomGoverned = true;
                        viewSnapshot.look.bloomEnabled = false;
                        viewSnapshot.look.graded = false;
                        viewSnapshot.look.blurSize = 0.0f;
                        viewSnapshot.look.depthOfField = false;
                        viewSnapshot.look.sunRays = false;
                    }
                    renderer->render(*device, *cmd,
                                     {.color = view->texture,
                                      .colorFormat = ViewFormat,
                                      .width = view->width,
                                      .height = view->height,
                                      .view = view->rendererView},
                                     viewSnapshot, meshCache);
                    if (own.valid())
                        textureLibrary.set(view->urn, own, view->width, view->height);
                    ViewHost::drawn(*view, frame.index,
                                    static_cast<core::f64>(platform::nowNs() - started) / 1'000'000.0);
                }
            }

            if (useRenderer) {
                renderer->render(
                    *device, *cmd,
                    {.color = target, .colorFormat = targetFormat, .width = targetWidth, .height = targetHeight},
                    snapshot, meshCache);

                // Debug geometry goes on top, in a pass that LOADS rather than
                // clears: it is an overlay on the rendered frame, not a
                // replacement for it.
                if (debugRenderer.valid() && !debugDraw.vertices().empty()) {
                    const std::array<rhi::ColorAttachment, 1> overlayColors{rhi::ColorAttachment{
                        .texture = target,
                        .loadOp = rhi::LoadOp::Load,
                        .storeOp = rhi::StoreOp::Store,
                    }};
                    cmd->pushDebugGroup("debug-draw");
                    cmd->beginRenderPass({.colorAttachments = overlayColors, .debugName = "debug-draw"});
                    cmd->setViewport({
                        .width = static_cast<f32>(targetWidth),
                        .height = static_cast<f32>(targetHeight),
                    });
                    debugRenderer.render(*cmd, snapshot.camera.viewProjection);
                    cmd->endRenderPass();
                    cmd->popDebugGroup();
                }
            }
            else {

                const std::array<rhi::ColorAttachment, 1> colors{rhi::ColorAttachment{
                    .texture = target,
                    .loadOp = rhi::LoadOp::Clear,
                    .storeOp = rhi::StoreOp::Store,
                    .clearColor = pulseColor(scheduler.totalTicks(), scheduler.timing().fixedDt),
                }};

                cmd->pushDebugGroup("frame");
                cmd->beginRenderPass({.colorAttachments = colors, .debugName = "clear"});

                if (debugRenderer.valid() && targetWidth > 0 && targetHeight > 0) {
                    cmd->setViewport({
                        .width = static_cast<f32>(targetWidth),
                        .height = static_cast<f32>(targetHeight),
                    });
                    debugRenderer.render(*cmd, orbitCamera(targetWidth, targetHeight));
                }

                cmd->endRenderPass();
                cmd->popDebugGroup();
            }

            // The UI, in its own pass that LOADS: it is drawn over the finished
            // frame whatever produced it, so a project with no camera still has
            // a menu. Before the debug overlay and after everything else, which
            // is the order api-design.md §2.2 implies -- game UI is part of the
            // game, and the ImGui overlay is on top of the game.
            if (uiRenderer.valid() && !uiVertices.empty()) {
                const std::array<rhi::ColorAttachment, 1> uiColors{rhi::ColorAttachment{
                    .texture = target,
                    .loadOp = rhi::LoadOp::Load,
                    .storeOp = rhi::StoreOp::Store,
                }};
                cmd->pushDebugGroup("ui");
                cmd->beginRenderPass({.colorAttachments = uiColors, .debugName = "ui"});
                cmd->setViewport({
                    .width = static_cast<f32>(targetWidth),
                    .height = static_cast<f32>(targetHeight),
                });
                uiRenderer.render(*cmd, uiViewport);
                cmd->endRenderPass();
                cmd->popDebugGroup();
            }

            // Its own pass, on top of the finished frame, after ours closed and
            // before submit -- the ordering the overlay's contract asks for.
            //
            // In the editor it goes to the SCREEN while everything above went
            // to the panel's texture, and the screen has had nothing written to
            // it this frame -- so it is cleared first. Without that, whatever
            // the dockspace leaves transparent shows a previous frame or worse.
            if (options.editor && present.valid() && present != target) {
                const std::array<rhi::ColorAttachment, 1> clear{rhi::ColorAttachment{
                    .texture = present,
                    .loadOp = rhi::LoadOp::Clear,
                    .storeOp = rhi::StoreOp::Store,
                    .clearColor = {0.06f, 0.06f, 0.07f, 1.0f},
                }};
                cmd->beginRenderPass({.colorAttachments = clear, .debugName = "editor-backdrop"});
                cmd->endRenderPass();
            }

            if (overlay.has_value()) {
                if (options.editor) {
                    overlay->setEditorTarget(&editor, viewportTarget.texture());
                    overlay->setIcons(&iconAtlas);
                    overlay->setThumbnails(&thumbnails);
                    overlay->setSaves(host->saves());
#if ENG_DEBUG_UI
                    overlay->setSurfaceCompiler(surfaceCompiler.get());
#endif
                    overlay->setScriptEditor(&scripts);
                    // Re-pointed every frame rather than once: a project open
                    // or a hot reload builds a new animation system, and a
                    // pointer set at boot would name the previous one.
                    overlay->setSkeleton(host->animation());
                }
                // **What the frame cost, handed over before it is drawn**
                // (S5.12). The Stats readout showed frame time, backend and
                // drawable size and nothing about the work -- in BOTH shells,
                // despite the ledger recording that the F3 one had it. Counted
                // by the renderer where it can be (`drawCalls`), and by the
                // extraction walk where only that knows (`visibleObjects`, the
                // coarse-LOD draws and the triangles at the level chosen).
                overlay->setRenderCounters({
                    .drawCalls = frameDrawCalls,
                    .visibleObjects = frameVisibleObjects,
                    .instancedDraws = frameInstancedDraws,
                    .lodDraws = frameLodDraws,
                    .triangles = static_cast<core::u32>(frameTriangles),
                    .foliageTiles = foliage.stats().tilesResident,
                    .foliageInstances = foliage.stats().instancesResident,
                    .foliageGrown = foliage.stats().tilesGrownLastSync,
                });
                overlay->render(*cmd, options.editor && present.valid() ? present : target, frame);
            }
        }

        if (overlay.has_value() && overlay->driveAskedToQuit())
            quit = true;
        // Cleared once the frame is over. A `DrawLine` from a task resumed
        // outside a frame has nowhere to go and is the silent no-op the headless
        // contract already describes.
        host->setGizmoTarget(nullptr);

        const core::u64 presentNs = platform::nowNs();
        device->submitAndPresent();
        phaseWaitMs += msSince(presentNs);

        // **A process with no window and a real clock sleeps until its next
        // tick** -- a dedicated server, above all. A window's present waits for
        // the display, and a headless run on the synthetic clock is meant to
        // run flat out; this one had neither, and with no device to wait on
        // (a server draws nothing) it spun a thousand empty frames a second on
        // a whole core.
        if (options.headless && !syntheticClock)
            platform::sleepNs(scheduler.nanosUntilNextTick(platform::nowNs()));
        // **Nor a window with nothing to present to** (audit A13): a minimized
        // window acquires no image, so nothing waited for the display, and the
        // loop spun as fast as it could on a whole core for a game nobody saw.
        else if (!options.headless && !target.valid())
            platform::sleepNs(std::max<u64>(scheduler.nanosUntilNextTick(platform::nowNs()), 1'000'000ull));

        if (options.frames != 0 && options.exitAfterFrames && frame.index + 1 >= options.frames)
            quit = true;
        // **The device is gone** (a driver reset, most often a shader that ran
        // past the driver's timeout). The frame it happened in was dropped by
        // the RHI; nothing more can be drawn, so the loop ends and what the
        // person has open is looked after below.
        if (options.simulateDeviceLossAt != 0 && frame.index + 1 == options.simulateDeviceLossAt)
            device->simulateLoss();
        if (device->lost())
            quit = true;
    }

    if (device->lost()) {
        control.stop();
#if ENG_DEBUG_UI
        // The surfaces that were on screen are held back until they change,
        // so a restarted editor does not draw the one that hung the GPU and
        // lose its device again.
        if (surfaceCompiler != nullptr)
            surfaceCompiler->quarantineShown();
#endif
        // **Asked, and only when somebody is there to answer**: a test or a
        // capture run exits with the device-lost code and no box.
        const bool attended =
            !options.headless && !(options.frames != 0 && options.exitAfterFrames) && options.screenshotPath.empty();
        if (attended) {
            const core::Catalog& text = core::engineCatalog();
            if (options.editor) {
                const int choice = platform::askChoice(
                    window.get(), text.format(ENG_TR("engine.lost.title")), text.format(ENG_TR("engine.lost.editor")),
                    {text.format(ENG_TR("engine.lost.save_restart")), text.format(ENG_TR("engine.lost.restart")),
                     text.format(ENG_TR("engine.lost.quit"))});
                bool restart = choice == 0 || choice == 1;
                if (choice == 0) {
                    // What Ctrl+S saves: the scene -- and every script it
                    // holds -- out of play mode, each script and shader that
                    // is its own file, the open material and the open stamp.
                    //
                    // **And what did not save, said before anything is
                    // restarted** (audit A5): every result was dropped, and a
                    // restart after a failed save is the unsaved work gone.
                    std::vector<std::string> failed;
                    if (editor.inPlayMode()) {
                        editor.stop(host->world(), inspector);
                        // Or the save below writes the text from before play.
                        writeTypedSources(host->world(), scripts, editor);
                    }
                    if (!editor.openScenePath().empty() && !editor.saveOpenScene(host->world()))
                        failed.push_back(editor.openScenePath());
                    for (std::size_t index = 0; index < scripts.count(); ++index) {
                        const OpenScript* tab = scripts.at(index);
                        if (tab == nullptr || !tab->dirty() || tab->file.empty())
                            continue;
                        const std::filesystem::path file =
                            tab->origin == ScriptOrigin::File
                                ? editor.content().root() / std::filesystem::path(tab->file)
                                : options.scriptPath / tab->file;
                        if (!platform::writeTextFileDurable(file, tab->document.text()))
                            failed.push_back(tab->file);
                    }
                    if (editor.materialSession().open() && editor.materialSession().dirty() && !editor.saveMaterial())
                        failed.push_back(editor.materialSession().path);
                    if (editor.stampSession().open() && !editor.saveStamp(host->world(), host->runtime().dataModel()))
                        failed.push_back(editor.stampSession().path);
                    if (!failed.empty()) {
                        std::string list;
                        for (const std::string& name : failed)
                            list += (list.empty() ? "" : ", ") + name;
                        const std::array<core::I18nArg, 1> args{core::I18nArg{"files", std::string_view{list}}};
                        restart = platform::askChoice(window.get(), text.format(ENG_TR("engine.lost.title")),
                                                      text.format(ENG_TR("engine.lost.save_failed"), args),
                                                      {text.format(ENG_TR("engine.lost.quit")),
                                                       text.format(ENG_TR("engine.lost.restart_anyway"))}) == 1;
                    }
                }
                if (restart) {
                    std::vector<std::string> command{hostExecutablePath().string()};
                    command.insert(command.end(), options.arguments.begin(), options.arguments.end());
                    (void)platform::startDetached(command);
                }
            }
            else {
                (void)platform::askChoice(window.get(), text.format(ENG_TR("engine.lost.title")),
                                          text.format(ENG_TR("engine.lost.game")),
                                          {text.format(ENG_TR("engine.lost.quit"))});
            }
        }
        host->close();
        return core::makeError(ENG_TR("engine.err.device_lost"));
    }

    control.stop();
    // The window's events while the close handlers run, so the system does
    // not call a game that is saving on its way out unresponsive (audit A1).
    // Within what the operating system allows, when a console closed under
    // it: three and a half seconds, not thirty (audit A7).
    host->close(std::min(30.0, platform::stopDeadlineSeconds()), [&] {
        if (window != nullptr)
            (void)platform::pumpEvents();
    });
    // Closed and saved: a console handler holding the process open for this
    // may let it go.
    platform::stopFinished();
    device->waitIdle();

    if (!options.screenshotPath.empty() && offscreen.valid()) {
        const auto pixelCount = static_cast<core::usize>(options.width) * static_cast<core::usize>(options.height);
        std::vector<std::byte> pixels(pixelCount * 4u);

        if (!device->readTexture(offscreen, pixels))
            return core::makeError(ENG_TR("engine.screenshot.err.readback_failed"));

        if (auto writeError = writePng(options.screenshotPath, pixels, static_cast<core::u32>(options.width),
                                       static_cast<core::u32>(options.height));
            writeError.has_value())
            return writeError;

        const std::array<I18nArg, 1> shotArgs{I18nArg{"path", options.screenshotPath.string()}};
        core::log(LogLevel::Info, ENG_TR("engine.screenshot.info.written"), shotArgs);
    }

    if (!options.capturePath.empty()) {
        if (auto captureError = writeCapture(options.capturePath, *device); captureError.has_value())
            return captureError;

        const std::array<I18nArg, 1> captureArgs{I18nArg{"path", options.capturePath.string()}};
        core::log(LogLevel::Info, ENG_TR("engine.capture.info.written"), captureArgs);
    }

    if (!options.conformanceRoot.empty()) {
        const ConformanceReport report = host->conformanceReport();
        if (!report.ran)
            return core::makeError(ENG_TR("engine.tests.err.never_ran"));

        const std::array<I18nArg, 3> args{I18nArg{"total", report.total}, I18nArg{"passed", report.passed},
                                          I18nArg{"failed", report.failed}};
        core::log(LogLevel::Info, ENG_TR("engine.tests.info.summary"), args);

        // Written before the failure check, because a run that failed is
        // exactly the one whose per-case detail somebody wants.
        if (!options.testReportPath.empty()) {
            std::error_code ec;
            if (options.testReportPath.has_parent_path())
                std::filesystem::create_directories(options.testReportPath.parent_path(), ec);

            std::ofstream file(options.testReportPath, std::ios::binary | std::ios::trunc);
            if (!file) {
                const std::array<I18nArg, 1> path{I18nArg{"path", options.testReportPath.string()}};
                return core::makeError(ENG_TR("engine.tests.err.report_failed"), path);
            }
            file << report.json;
        }

        if (report.failed != 0)
            return core::makeError(ENG_TR("engine.tests.err.failed"), args);

        // A spec that does not compile is not a spec that passed. Before this,
        // a syntax error in one file logged a line and the run reported "955
        // passed, 0 failed" over a suite that had silently lost seventeen cases
        // -- a gate that can pass while doing nothing, which is the twelfth
        // instance of that shape in six milestones and the one that had to be
        // found by noticing a number did not move.
        if (const core::u64 failures = host->scriptLoadFailures(); failures != 0) {
            const std::array<I18nArg, 1> loadArgs{I18nArg{"count", static_cast<core::i64>(failures)}};
            return core::makeError(ENG_TR("engine.tests.err.load_failed"), loadArgs);
        }
    }

    const std::array<I18nArg, 2> summary{I18nArg{"frames", static_cast<core::i64>(scheduler.totalFrames())},
                                         I18nArg{"ticks", static_cast<core::i64>(scheduler.totalTicks())}};
    core::log(LogLevel::Info, ENG_TR("engine.frame.info.summary"), summary);

    if (options.frameStats && !frameTimesMs.empty()) {
        // The first frames are warm-up -- shader creation, the first mesh load,
        // the swapchain settling -- and including them makes a median that
        // describes startup rather than the scene. Dropped rather than averaged
        // away, because averaging a spike in is exactly how a baseline stops
        // being comparable.
        constexpr core::usize kWarmupFrames = 10;
        const auto sorted = [&](const std::vector<f64>& times) {
            std::vector<f64> kept = times;
            if (kept.size() > kWarmupFrames * 2)
                kept.erase(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(kWarmupFrames));
            std::sort(kept.begin(), kept.end());
            return kept;
        };
        const auto at = [](const std::vector<f64>& ordered, f64 fraction) {
            if (ordered.empty())
                return 0.0;
            const auto index = static_cast<core::usize>(fraction * static_cast<f64>(ordered.size() - 1) + 0.5);
            return ordered[std::min(index, ordered.size() - 1)];
        };
        std::vector<f64> measured = sorted(frameTimesMs);

        const f64 median = measured[measured.size() / 2];
        const f64 worst = measured.back();
        const std::array<I18nArg, 6> stats{
            I18nArg{"frames", static_cast<core::i64>(measured.size())},
            I18nArg{"median", median},
            I18nArg{"worst", worst},
            I18nArg{"draws", static_cast<core::i64>(frameDrawCalls)},
            I18nArg{"objects", static_cast<core::i64>(frameVisibleObjects)},
            I18nArg{"triangles", static_cast<core::i64>(frameTriangles)},
        };
        core::log(LogLevel::Info, ENG_TR("engine.frame.info.stats"), stats);

        // **The tail, and where it went** (audit P1): a median says how a scene
        // usually runs and nothing about the frames a player feels. Each phase
        // is sorted on its own, so its p95 is that phase's and not the p95
        // frame's -- the question it answers is "is this phase ever slow".
        std::vector<f64> drawMs;
        drawMs.reserve(frameTimesMs.size());
        for (core::usize index = 0; index < frameTimesMs.size(); ++index)
            drawMs.push_back(std::max(0.0, frameTimesMs[index] - frameSimMs[index] - frameWaitMs[index]));
        const std::vector<f64> sim = sorted(frameSimMs);
        const std::vector<f64> wait = sorted(frameWaitMs);
        const std::vector<f64> draw = sorted(drawMs);
        const auto hitches = static_cast<core::i64>(
            std::count_if(measured.begin(), measured.end(), [](f64 frameMs) { return frameMs > 33.0; }));
        const std::array<I18nArg, 10> tail{
            I18nArg{"p95", at(measured, 0.95)}, I18nArg{"p99", at(measured, 0.99)},
            I18nArg{"hitches", hitches},        I18nArg{"sim", at(sim, 0.5)},
            I18nArg{"sim95", at(sim, 0.95)},    I18nArg{"draw", at(draw, 0.5)},
            I18nArg{"draw95", at(draw, 0.95)},  I18nArg{"wait", at(wait, 0.5)},
            I18nArg{"wait95", at(wait, 0.95)},  I18nArg{"frames", static_cast<core::i64>(measured.size())},
        };
        core::log(LogLevel::Info, ENG_TR("engine.frame.info.phases"), tail);
    }

    // The soak verdict is computed BEFORE teardown, because teardown frees the
    // very memory the peak was measured against -- and after the frame loop,
    // because a gate that could stop a run early would report on a run that did
    // not happen.
    std::optional<core::EngineError> soakFailure;
    if (!options.soakReportPath.empty()) {
        const SoakThresholds thresholds{.memoryCeilingBytes = options.soakCeilingBytes,
                                        .minimumInstances = options.soakMinimumInstances,
                                        .returnRadiusMetres = options.soakReturnRadiusMetres};
        const SoakVerdict verdict = soak.evaluate(thresholds);

        std::ofstream report(options.soakReportPath, std::ios::binary);
        report << soak.report(thresholds);
        if (!report.good()) {
            const std::array<I18nArg, 1> writeArgs{I18nArg{"path", options.soakReportPath.string()}};
            soakFailure = core::makeError(ENG_TR("engine.soak.err.report_write_failed"), writeArgs);
        }
        report.close();

        const std::array<I18nArg, 8> reportArgs{
            I18nArg{"frames", static_cast<core::i64>(verdict.frames)},
            I18nArg{"median", verdict.medianMs},
            I18nArg{"p99", verdict.p99Ms},
            I18nArg{"worst", verdict.worstMs},
            I18nArg{"peak", static_cast<core::i64>(verdict.peakResidentBytes / (1024 * 1024))},
            I18nArg{"early", static_cast<core::i64>(verdict.earlyInstances)},
            I18nArg{"late", static_cast<core::i64>(verdict.lateInstances)},
            I18nArg{"path", options.soakReportPath.string()},
        };
        core::log(LogLevel::Info, ENG_TR("engine.soak.info.report"), reportArgs);

        // Every failure is logged and then ONE error is returned. A gate that
        // reports only its first complaint makes the second one cost another
        // five minutes.
        for (const core::EngineError& failure : verdict.failures) {
            core::logText(LogLevel::Error, failure.message);
        }
        // Loud, and not fatal. A quarantined check that stopped saying anything
        // would be a deleted check with extra steps (D066, §12).
        for (const core::EngineError& quarantined : verdict.quarantined) {
            core::logText(LogLevel::Warn, quarantined.message);
        }
        if (!verdict.ok && !soakFailure.has_value()) {
            const std::array<I18nArg, 2> failArgs{I18nArg{"failures", static_cast<core::i64>(verdict.failures.size())},
                                                  I18nArg{"path", options.soakReportPath.string()}};
            soakFailure = core::makeError(ENG_TR("engine.soak.err.failed"), failArgs);
        }
    }

    skyLoader.destroy(*device);
    for (const std::unique_ptr<SubWorldGpu>& gpu : subWorldGpu) {
        gpu->loader.destroy(*device);
        gpu->meshes.destroy(*device);
    }
    subWorldGpu.clear();
    viewHost.destroy(*device, textureLibrary, renderer.get());
    uiText.destroy(*device);
    uiRenderer.destroy(*device);
    debugRenderer.destroy(*device);
    iconAtlas.destroy(*device);
    thumbnails.destroy(*device);
    if (previewRenderer != nullptr)
        previewRenderer->destroy(*device);
    if (offscreen.valid())
        device->destroy(offscreen);
    if (window != nullptr)
        device->releaseWindow(*window);

    return soakFailure;
}

} // namespace engine::app
