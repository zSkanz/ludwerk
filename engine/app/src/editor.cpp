#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/app/brush_overlay.h>
#include <engine/app/editor.h>
#include <engine/app/scene_definitions.h>
#include <engine/asset/image.h>
#include <engine/asset/terrain_layers.h>
#include <engine/core/content_path.h>
#include <engine/core/i18n.h>
#include <engine/core/json.h>
#include <engine/core/json_writer.h>
#include <engine/core/text_key.h>
#include <engine/platform/file.h>
#include <engine/platform/platform.h>
#include <engine/render/debug_draw.h>
#include <engine/render/lighting.h>
#include <engine/rhi/device.h>
#include <engine/scene/class_registry.h>
#include <engine/scene/pivot.h>
#include <engine/scene/scene_file.h>
#include <engine/scene/voxel_fluid.h>
#include <engine/scene/water.h>
#include <engine/scene/world.h>
#include <engine/script/modules.h>
#include <engine/ui/ui.h>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::app {
using core::Vec3;
namespace {

// **What the editor says when an edit reaches more streamed ground than loads
// at once** (terrain audit TA16): the words a script's refusal carries, so the
// two say the same thing.
[[nodiscard]] std::string groundTooWide()
{
    const core::I18nArg args[] = {{"limit", static_cast<core::i64>(scene::World::MaxGroundCells)}};
    return core::engineCatalog().format(ENG_TR("scene.err.terrain_ground_too_wide"), args);
}

// Every cell of ground the world streams, read now: what an action over a
// whole terrain needs before it looks at the terrain's extent.
[[nodiscard]] bool loadAllGround(const scene::World& world)
{
    constexpr double Everywhere = 1.0e12;
    return world.loadGround(core::DVec3{-Everywhere, 0.0, -Everywhere}, core::DVec3{Everywhere, 0.0, Everywhere});
}

// **An action over a whole terrain, a chunk at a time** (the terrain audit's
// editor list): the whole field's box is past what one edit may touch
// (`MaxEditVoxels`) on any large terrain -- the owner's place is four times it
// -- so Apply Rules and Replace Material were refused whole on it. Both decide
// voxel by voxel, and only where there is ground, which is only in the chunks
// the field holds: chunk by chunk is the same answer, and walks no air.
// `edit(low, high)` is one chunk's box in field metres; the reports are summed.
template <typename Edit>
[[nodiscard]] asset::EditReport overChunks(const asset::TerrainField& field, Edit edit)
{
    const double chunk = static_cast<double>(asset::ChunkEdge) * static_cast<double>(field.settings().voxelSize);
    // Inside the chunk by a hair on every side, so no box reaches a neighbour's
    // first voxel.
    const double inset = 1.0e-3 * static_cast<double>(field.settings().voxelSize);
    asset::EditReport total;
    for (const asset::ChunkKey& key : field.chunkKeys()) {
        const asset::EditReport report =
            edit(core::DVec3{key.x * chunk + inset, key.y * chunk + inset, key.z * chunk + inset},
                 core::DVec3{(key.x + 1) * chunk - inset, (key.y + 1) * chunk - inset, (key.z + 1) * chunk - inset});
        total.touched += report.touched;
        total.refused = total.refused || report.refused;
    }
    return total;
}

} // namespace

namespace {
// **A script moved into a service takes the service's side** (ADR 0138 §2):
// every `Script` in the subtree at `root` that now sits inside a script
// service has its `RunContext` written to that service's side. Taken back out,
// it keeps it -- server code stays server code, and never becomes `Shared` by
// accident, which would ship it to every player. Inside the caller's undo
// step, which every verb records before it moves anything.
void adoptServiceSide(scene::World& world, core::InstanceId root)
{
    const scene::ClassId scriptClass = world.classes().findId(world.atoms().lookup("Script"));
    const core::NameAtom runContext = world.atoms().intern("RunContext");
    std::vector<core::InstanceId> subtree{root};
    world.collectDescendants(root, subtree);
    for (const core::InstanceId id : subtree) {
        if (world.classOf(id) != scriptClass)
            continue;
        const std::optional<script::ScriptSide> side = script::serviceSideOf(world, id);
        if (!side.has_value())
            continue;
        const core::i32 value =
            *side == script::ScriptSide::Server ? script::RunContextServer : script::RunContextClient;
        (void)world.setProperty(
            id, runContext,
            scene::Value{scene::EnumValue{world.enums().findId(world.atoms().lookup("RunContext")), value}});
    }
}

// The seed a stage's world is built with. A constant, because nothing in a
// stage is simulated and nothing in it reads the generator -- and a seed drawn
// from anywhere else would make a stamp's bytes depend on when it was opened.
constexpr core::u64 kStageSeed = 0x5741'4D50u;

// The same format the headless path renders into. An editor's viewport is the
// headless render with somebody watching, so a second format here would be a
// second thing to keep in step for no gain.
constexpr rhi::TextureFormat kViewportFormat = rhi::TextureFormat::Rgba8Unorm;

// A panel dragged to nothing is a normal thing for a person to do, and a
// zero-sized texture is not a thing a device will make.
constexpr core::u32 kMinimumViewport = 1;
} // namespace

ViewportTarget::~ViewportTarget()
{
    destroy();
}

bool ViewportTarget::resize(rhi::IDevice& device, core::u32 width, core::u32 height)
{
    const core::u32 wantWidth = width > kMinimumViewport ? width : kMinimumViewport;
    const core::u32 wantHeight = height > kMinimumViewport ? height : kMinimumViewport;

    if (m_texture.valid() && m_width == wantWidth && m_height == wantHeight)
        return true;

    // The texture being replaced may still be in flight from the frame that was
    // just submitted, so it is put aside rather than freed. See the header: the
    // alternative was a full device stall on every frame of a splitter drag.
    if (m_texture.valid()) {
        m_retired.push_back(Retired{m_texture, RetirementFrames});
        m_texture = {};
    }

    m_device = &device;
    m_texture = device.createTexture({
        .format = kViewportFormat,
        // Sampled as well as drawn into: ImGui shows it, which means reading it
        // in a shader.
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = wantWidth,
        .height = wantHeight,
        .debugName = "editor-viewport",
    });

    if (!m_texture.valid()) {
        m_width = 0;
        m_height = 0;
        return false;
    }

    m_width = wantWidth;
    m_height = wantHeight;
    return true;
}

void ViewportTarget::retire(rhi::IDevice& device)
{
    // Walked backwards so an erase cannot move an entry past the cursor. The
    // list is at most three long, so this is not where the frame goes.
    for (core::usize index = m_retired.size(); index > 0; --index) {
        Retired& entry = m_retired[index - 1];
        if (entry.framesLeft > 0) {
            --entry.framesLeft;
            continue;
        }
        if (entry.texture.valid())
            device.destroy(entry.texture);
        m_retired.erase(m_retired.begin() + static_cast<std::ptrdiff_t>(index - 1));
    }
}

void ViewportTarget::destroy()
{
    // **The one place the stall belongs.** Shutting down is not a frame, there
    // is no next one for a retirement queue to be drained by, and every handle
    // here has to be gone before the device is -- so this waits for the GPU to
    // finish with all of them and frees them together.
    if (m_device != nullptr && (m_texture.valid() || !m_retired.empty())) {
        m_device->waitIdle();
        if (m_texture.valid())
            m_device->destroy(m_texture);
        for (const Retired& entry : m_retired) {
            if (entry.texture.valid())
                m_device->destroy(entry.texture);
        }
    }
    m_retired.clear();
    m_texture = {};
    m_width = 0;
    m_height = 0;
    m_device = nullptr;
}

void Editor::setCamera(const core::Mat4& projection, const core::Mat4& view, core::DVec3 origin) noexcept
{
    m_projection = projection;
    m_view = view;
    m_cameraOrigin = origin;
    m_hasCamera = true;
}

PickRay Editor::rayThrough(core::Vec2 pixelInViewport) const noexcept
{
    // The viewport's own space, not the window's: the offscreen target IS the
    // viewport, so its rectangle starts at its own origin. Carrying the panel's
    // window offset in here as well would be two corrections for one
    // displacement, and the second one is always the wrong sign.
    const ViewportRect local{0.0f, 0.0f, m_viewport.width, m_viewport.height};
    return rayThroughPixel(m_projection, m_view, m_cameraOrigin, local, pixelInViewport);
}

void Editor::play(scene::World& world)
{
    // Already in play mode -- running or paused. Taking a second snapshot here
    // would move the point stop returns to into the middle of a play session.
    if (m_run != RunState::Editing)
        return;
    // **Not over an import** (ADR 0149 §2): the world is half laid.
    if (m_import != nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_running")), true};
        return;
    }

    // Taken every time play is pressed rather than kept from the first, because
    // stop means "back to where I pressed play", not "back to where I opened
    // the editor".
    m_playSnapshot = std::make_unique<scene::WorldSnapshot>(world.snapshot());
    m_run = RunState::Playing;
    // **Attached, every time play is pressed** (S5.8). Detaching is a thing
    // somebody does DURING a run to look at something; carrying it into the next
    // one would mean pressing play and finding the view somewhere they left it a
    // session ago, with nothing on screen saying why.
    m_cameraDetached = false;
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.playing")), false};
}

void Editor::setPaused(bool paused) noexcept
{
    // Pause is a thing that happens INSIDE play mode. Asking for it while
    // editing is asking for a state that does not exist, and silently entering
    // play mode to provide it would be worse than doing nothing.
    if (m_run == RunState::Editing)
        return;
    m_run = paused ? RunState::Paused : RunState::Playing;
}

void Editor::stop(scene::World& world, Inspector& inspector)
{
    m_run = RunState::Editing;
    m_cameraDetached = false;
    if (m_playSnapshot == nullptr)
        return;

    world.restore(*m_playSnapshot);
    m_playSnapshot.reset();
    ++m_worldRestores;
    // A play session's changes were never edits, and the edits before it belong
    // to a world this restore has just replaced.
    m_history.clear();

    // A selection made DURING play can name something the restore removed. The
    // id would resolve to whatever the slot holds now, which is either nothing
    // or somebody else -- and a properties grid pointed at somebody else is how
    // an edit lands on the wrong object.
    inspector.pruneDead(world);
    inspector.onWorldRestored();

    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.stopped_the_world_is_back")), false};
}

bool Editor::save(scene::World& world, const std::filesystem::path& path)
{
    // **Never the running game.** What is on screen during play is what the
    // game did -- parts its scripts made, parts it destroyed -- and saving it
    // made every one of those the scene's: duplicated at the next run, or gone.
    if (m_run != RunState::Editing) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.stop_the_game_first_what")), true};
        return false;
    }
    // Nor a world half laid (ADR 0149 §2).
    if (m_import != nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_running")), true};
        return false;
    }

    // The terrain's cells first: the scene names where they are, so a scene
    // written before them would name cells that are not there yet.
    std::string terrainNote;
    if (m_terrainSaver && !m_terrainSaver(world, path, terrainNote)) {
        m_status = EditorStatus{
            core::tr(ENG_TR("engine.editor.status.could_not_write_the_terrain"), {{"terrain_note", terrainNote}}),
            true};
        return false;
    }
    // The scripts' files next, for the reason the cells come first: the scene
    // leaves out whatever a file holds, so the files have to be there.
    const std::string scriptNote = m_scriptFileSaver ? m_scriptFileSaver(world, path) : std::string{};

    scene::SceneIoReport report;
    // **The stamps this scene names, read once each**, so a stamped instance
    // is written as a mark plus what differs rather than as a copy of the
    // subtree (ADR 0051). Without this every one of them would be written in
    // full and unlinked -- which loses nothing and is exactly what a save with
    // no content root does.
    scene::StampLibrary stamps(world, stampSource());
    const std::string text = scene::writeScene(world, &report, &stamps);

    if (!platform::createDirectories(path.parent_path()) || !platform::writeTextFileDurable(path, text)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write"), {{"path", path.string()}}), true};
        return false;
    }

    // **The game's own file beside it** (ADR 0105): what is authored under
    // `GlobalScriptService` belongs to no scene, so it is written to the
    // content root's `global.json` on every save -- and the file goes when there
    // is nothing left to keep in it.
    if (!m_content.root().empty() && m_globalUnreadable) {
        // **Left alone.** It could not be read when the project opened, so what
        // this world holds of it is not what it holds, and writing -- or
        // deleting it for being empty -- would lose the difference.
        const char* const unreadable = core::tr(ENG_TR("engine.editor.status.global_unreadable_note"));
        terrainNote = terrainNote.empty() ? std::string(unreadable)
                                          : core::tr(ENG_TR("engine.editor.status.with_note"),
                                                     {{"status", terrainNote}, {"note", unreadable}});
    }
    else if (!m_content.root().empty()) {
        const std::filesystem::path global = m_content.root() / "global.json";
        if (const std::string globalText = scene::writeGlobal(world, nullptr, &stamps); !globalText.empty()) {
            if (!platform::writeTextFileDurable(global, globalText)) {
                m_status = EditorStatus{
                    core::tr(ENG_TR("engine.editor.status.could_not_write_2"), {{"global", global.string()}}), true};
                return false;
            }
        }
        else if (platform::fileExists(global)) {
            std::error_code ec;
            std::filesystem::remove(global, ec);
        }
    }

    // **Here rather than at each caller**, because every scene write goes
    // through this one function -- Save, Save As and the save half of a
    // confirmation all land here, and a flag cleared at three call sites is a
    // flag one of them will forget.
    m_sceneDirty = false;

    // **What was written is the package now** (terrain audit R5): a match
    // started from here loads it on every machine, so the ground's
    // replication diffs against it rather than against the scene as it was
    // opened -- which sent nothing for an edit saved and then undone.
    world.terrains().forEach(
        [](core::InstanceId, scene::TerrainComponent& terrain) { terrain.shipped = terrain.field; });
    world.voxels().forEach([](core::InstanceId, scene::VoxelComponent& voxels) { voxels.shipped = voxels.grid; });

    // The tree as types, beside the scene it came from (ADR 0078): the project
    // is the folder `content/` is in. A failure here is not the save's, which
    // has already succeeded.
    if (!m_content.root().empty())
        (void)writeSceneDefinitions(world, m_content.root().parent_path());

    const auto noted = [](const std::string& status, const std::string& note) {
        return core::tr(ENG_TR("engine.editor.status.with_note"), {{"status", status}, {"note", note}});
    };
    std::string message = core::tr(ENG_TR("engine.editor.status.saved_instances_to"),
                                   {{"count", static_cast<core::i64>(report.instances)}, {"path", path.string()}});
    if (!terrainNote.empty())
        message = noted(message, terrainNote);
    if (!scriptNote.empty())
        message = noted(message, scriptNote);
    // Counted rather than swallowed. A reference that pointed outside the scene
    // is a thing the person authored and the file cannot hold, and finding that
    // out when you reopen is finding it out too late.
    if (report.droppedReferences > 0) {
        message = core::tr(ENG_TR("engine.editor.status.with_aside"),
                           {{"status", message},
                            {"note", core::tr(ENG_TR("engine.editor.status.scene_references_dropped"),
                                              {{"count", static_cast<core::i64>(report.droppedReferences)}})}});
    }
    // **Said out loud, because the alternative is finding out days later.** A
    // stamped instance whose contents no longer match its file is written in
    // full and unlinked -- the scene keeps everything, but the instance stops
    // following the stamp, and nothing about looking at it says so. Adding a
    // child to one is the ordinary way to arrive here.
    if (report.unlinkedStamps > 0) {
        message = noted(message, core::tr(ENG_TR("engine.editor.status.stamps_unlinked"),
                                          {{"count", static_cast<core::i64>(report.unlinkedStamps)}}));
    }
    m_status = EditorStatus{message, report.unlinkedStamps > 0};
    return true;
}

bool Editor::load(scene::World& world, const std::filesystem::path& path, Inspector& inspector)
{
    std::string text;
    if (!platform::readTextFile(path, text)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_read"), {{"path", path.string()}}), true};
        return false;
    }

    scene::SceneIoReport report;
    // A world built afresh: what it is built from is what is read now.
    m_stampTexts->clear();
    // **The stamps the scene names, read through this editor's content root.**
    // `scene` is L3 and has no filesystem; a scene loaded without this opens
    // with its stamped instances missing and a count saying so.
    if (const std::optional<core::EngineError> error = scene::readScene(world, text, &report, stampSource());
        error.has_value()) {
        m_status = EditorStatus{error->message, true};
        return false;
    }

    // **The scene this replaced has to be RETIRED, not only destroyed.**
    // `readScene` clears the world with `destroy`, which unlinks and marks --
    // and the record stops resolving in `retireDestroyed`, which runs at the end
    // of a signal drain. A paused world runs no drains, so without this every
    // instance of the previous scene stays in the component pools for ever:
    // unparented, drawn by nothing, and accumulating one whole scene per load.
    //
    // The same argument `deleteInstance` makes, and the same place to make it:
    // nothing else will while the editor is editing.
    world.retireDestroyed();

    // Everything the file named was created just now, so nothing selected
    // before it still means what it meant.
    inspector.select(core::InstanceId{});
    inspector.onWorldChanged();

    std::string message = core::tr(ENG_TR("engine.editor.status.loaded_instances_from"),
                                   {{"count", static_cast<core::i64>(report.instances)}, {"path", path.string()}});
    if (report.unknownClasses > 0) {
        message = core::tr(ENG_TR("engine.editor.status.with_aside"),
                           {{"status", message},
                            {"note", core::tr(ENG_TR("engine.editor.status.unknown_classes_skipped"),
                                              {{"count", static_cast<core::i64>(report.unknownClasses)}})}});
    }
    // **A stamp the scene names and the content no longer has** (B4): its
    // instances are not in the world, and saving now would take them out of
    // the file too -- said loudly, rather than found out later.
    if (report.missingStamps > 0) {
        message = core::tr(ENG_TR("engine.editor.status.with_note"),
                           {{"status", message},
                            {"note", core::tr(ENG_TR("engine.editor.status.stamps_not_loaded"),
                                              {{"count", static_cast<core::i64>(report.missingStamps)}})}});
    }
    m_status = EditorStatus{message, report.missingStamps > 0};
    return true;
}

namespace {
// Ten, spread round the hue circle rather than picked by eye, so that two
// folders coloured a minute apart are actually distinguishable -- which is the
// entire job. Values rather than a generator, because a palette is a decision
// and a decision should be readable.
constexpr core::Color3 kFolderPalette[] = {
    core::Color3{0.85f, 0.33f, 0.31f}, // red
    core::Color3{0.88f, 0.55f, 0.24f}, // orange
    core::Color3{0.87f, 0.76f, 0.28f}, // yellow
    core::Color3{0.53f, 0.76f, 0.35f}, // green
    core::Color3{0.29f, 0.71f, 0.60f}, // teal
    core::Color3{0.30f, 0.62f, 0.85f}, // blue
    core::Color3{0.44f, 0.47f, 0.83f}, // indigo
    core::Color3{0.65f, 0.44f, 0.82f}, // violet
    core::Color3{0.85f, 0.45f, 0.66f}, // pink
    core::Color3{0.60f, 0.62f, 0.66f}, // slate
};

// `#rrggbb`, which is what a person editing `editor.json` by hand expects to
// see. Written from the same 8-bit rounding both panels draw with, so a colour
// that survives the file is the colour that was chosen.
[[nodiscard]] std::string writeHexColor(core::Color3 color)
{
    const auto channel = [](core::f32 value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    char buffer[8]{};
    (void)std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", channel(color.r), channel(color.g), channel(color.b));
    return std::string(buffer);
}

[[nodiscard]] std::optional<core::Color3> parseHexColor(std::string_view text)
{
    if (text.size() != 7 || text[0] != '#')
        return std::nullopt;
    core::u32 packed = 0;
    for (std::size_t index = 1; index < text.size(); ++index) {
        const char c = text[index];
        const core::u32 digit = c >= '0' && c <= '9'   ? static_cast<core::u32>(c - '0')
                                : c >= 'a' && c <= 'f' ? static_cast<core::u32>(c - 'a' + 10)
                                : c >= 'A' && c <= 'F' ? static_cast<core::u32>(c - 'A' + 10)
                                                       : 16u;
        if (digit > 15u)
            return std::nullopt;
        packed = (packed << 4) | digit;
    }
    return core::Color3{static_cast<core::f32>((packed >> 16) & 0xFFu) / 255.0f,
                        static_cast<core::f32>((packed >> 8) & 0xFFu) / 255.0f,
                        static_cast<core::f32>(packed & 0xFFu) / 255.0f};
}
} // namespace

std::span<const core::Color3> Editor::folderPalette() noexcept
{
    return std::span<const core::Color3>(kFolderPalette, std::size(kFolderPalette));
}

std::optional<core::Color3> Editor::folderColor(const scene::World& world, core::InstanceId id)
{
    if (!world.alive(id))
        return std::nullopt;
    const core::NameAtom atom = world.atoms().lookup(FolderColorAttribute);
    if (!atom.valid())
        return std::nullopt;
    // `getAttribute` answers with a value rather than an optional: an absent
    // attribute is `monostate`, which `get_if` reports as "not a colour".
    const scene::Value value = world.getAttribute(id, atom);
    if (const core::Color3* color = std::get_if<core::Color3>(&value); color != nullptr)
        return *color;
    return std::nullopt;
}

void Editor::setFolderColor(scene::World& world, core::InstanceId id, std::optional<core::Color3> color)
{
    if (!world.alive(id))
        return;

    m_history.record(world, color.has_value() ? core::tr(ENG_TR("engine.editor.history.colour"))
                                              : core::tr(ENG_TR("engine.editor.history.clear_colour")));
    const core::NameAtom atom = world.atoms().intern(FolderColorAttribute);
    // A `Nil` value removes it, which the world's own setter documents -- so
    // clearing a colour is the same call as setting one and there is no second
    // path to keep in step.
    (void)world.setAttribute(id, atom, color.has_value() ? scene::Value{*color} : scene::Value{});
}

std::optional<core::Color3> Editor::contentColor(std::string_view path) const
{
    const auto found = m_contentColors.find(std::string(path));
    return found != m_contentColors.end() ? std::optional<core::Color3>(found->second) : std::nullopt;
}

void Editor::setContentColor(std::string_view path, std::optional<core::Color3> color)
{
    if (color.has_value())
        m_contentColors[std::string(path)] = *color;
    else
        m_contentColors.erase(std::string(path));
}

namespace {
// The manipulator's mode and the browser's layout, as WORDS.
//
// `.engine/editor.json` is a file a person opens when something is wrong with
// it, and `"gizmo": 2` tells them nothing while `"gizmo": "scale"` tells them
// everything. An unknown word falls back rather than failing: a project written
// by a newer build should still open here, minus what this one cannot say --
// which is the rule the scene format already follows.
// The tool's name in the preferences file, and back. Unknown reads as `Select`,
// which is the tool that cannot lose work: a file written by a newer editor
// naming a tool this one does not have should open in the safe one.
[[nodiscard]] std::string_view paintModeName(asset::PaintMode mode) noexcept
{
    switch (mode) {
    case asset::PaintMode::Replace:
        return "replace";
    case asset::PaintMode::Blend:
        return "blend";
    case asset::PaintMode::Under:
        return "under";
    case asset::PaintMode::Erase:
        return "erase";
    }
    return "blend";
}

[[nodiscard]] asset::PaintMode paintModeFrom(std::string_view name) noexcept
{
    if (name == "replace")
        return asset::PaintMode::Replace;
    if (name == "under")
        return asset::PaintMode::Under;
    if (name == "erase")
        return asset::PaintMode::Erase;
    return asset::PaintMode::Blend;
}

[[nodiscard]] std::string_view brushOpName(Editor::BrushOp op) noexcept
{
    switch (op) {
    case Editor::BrushOp::Subtract:
        return "subtract";
    case Editor::BrushOp::Grow:
        return "grow";
    case Editor::BrushOp::Erode:
        return "erode";
    case Editor::BrushOp::Smooth:
        return "smooth";
    case Editor::BrushOp::Flatten:
        return "flatten";
    case Editor::BrushOp::Add:
        break;
    }
    return "add";
}

[[nodiscard]] Editor::BrushOp brushOpFrom(std::string_view name) noexcept
{
    if (name == "subtract")
        return Editor::BrushOp::Subtract;
    if (name == "grow")
        return Editor::BrushOp::Grow;
    if (name == "erode")
        return Editor::BrushOp::Erode;
    if (name == "smooth")
        return Editor::BrushOp::Smooth;
    if (name == "flatten")
        return Editor::BrushOp::Flatten;
    return Editor::BrushOp::Add;
}

[[nodiscard]] std::string_view toolName(Editor::Tool tool) noexcept
{
    switch (tool) {
    case Editor::Tool::Sculpt:
        return "sculpt";
    case Editor::Tool::Paint:
        return "paint";
    case Editor::Tool::Blocks:
        return "blocks";
    case Editor::Tool::Tiles:
        return "tiles";
    case Editor::Tool::Foliage:
        return "foliage";
    case Editor::Tool::Water:
        return "water";
    case Editor::Tool::Select:
        break;
    }
    return "select";
}

// What one stroke is called in the undo menu. A person reading "Undo Smooth"
// knows what is about to come back; "Undo Sculpt" for six different tools does
// not.
[[nodiscard]] const char* strokeLabel(Editor::Tool tool, Editor::BrushOp op, bool thin) noexcept
{
    if (tool == Editor::Tool::Paint) {
        return core::tr(ENG_TR("engine.editor.history.paint_terrain"));
    }
    if (tool == Editor::Tool::Foliage)
        return thin ? core::tr(ENG_TR("engine.editor.history.thin_foliage"))
                    : core::tr(ENG_TR("engine.editor.history.restore_foliage"));
    // The words the Terrain panel shows, so the undo menu names what the
    // person picked.
    switch (op) {
    case Editor::BrushOp::Subtract:
        return core::tr(ENG_TR("engine.editor.terrain.sculpt.dig"));
    case Editor::BrushOp::Grow:
        return core::tr(ENG_TR("engine.editor.terrain.sculpt.raise"));
    case Editor::BrushOp::Erode:
        return core::tr(ENG_TR("engine.editor.terrain.sculpt.lower"));
    case Editor::BrushOp::Smooth:
        return core::tr(ENG_TR("engine.editor.terrain.sculpt.smooth"));
    case Editor::BrushOp::Flatten:
        return core::tr(ENG_TR("engine.editor.terrain.sculpt.flatten"));
    case Editor::BrushOp::Add:
        break;
    }
    return core::tr(ENG_TR("engine.editor.terrain.sculpt.add"));
}

[[nodiscard]] Editor::Tool toolFrom(std::string_view name) noexcept
{
    if (name == "sculpt")
        return Editor::Tool::Sculpt;
    if (name == "paint")
        return Editor::Tool::Paint;
    if (name == "blocks")
        return Editor::Tool::Blocks;
    if (name == "tiles")
        return Editor::Tool::Tiles;
    if (name == "foliage")
        return Editor::Tool::Foliage;
    if (name == "water")
        return Editor::Tool::Water;
    return Editor::Tool::Select;
}

[[nodiscard]] std::string_view gizmoModeName(GizmoMode mode) noexcept
{
    switch (mode) {
    case GizmoMode::Rotate:
        return "rotate";
    case GizmoMode::Scale:
        return "scale";
    case GizmoMode::Translate:
        break;
    }
    return "translate";
}

[[nodiscard]] GizmoMode gizmoModeFrom(std::string_view name) noexcept
{
    if (name == "rotate")
        return GizmoMode::Rotate;
    if (name == "scale")
        return GizmoMode::Scale;
    return GizmoMode::Translate;
}

[[nodiscard]] std::string_view contentViewName(EditorPanels::ContentView view) noexcept
{
    switch (view) {
    case EditorPanels::ContentView::Tiles:
        return "tiles";
    case EditorPanels::ContentView::Icons:
        return "icons";
    case EditorPanels::ContentView::List:
        break;
    }
    return "list";
}

[[nodiscard]] EditorPanels::ContentView contentViewFrom(std::string_view name) noexcept
{
    if (name == "tiles")
        return EditorPanels::ContentView::Tiles;
    if (name == "icons")
        return EditorPanels::ContentView::Icons;
    return EditorPanels::ContentView::List;
}
// The window block, or nothing. Shared by `recallState` and the static reader,
// because the window is asked about twice -- once before there is an editor and
// once after -- and two spellings of "what does this file say" is how the two
// answers end up different.
[[nodiscard]] std::optional<platform::WindowPlacement> readWindow(const core::JsonValue& root)
{
    const core::JsonValue window = root["window"];
    if (window.type() != core::JsonType::Object)
        return std::nullopt;

    platform::WindowPlacement placement;
    placement.x = static_cast<core::i32>(window["x"].asInteger());
    placement.y = static_cast<core::i32>(window["y"].asInteger());
    placement.width = static_cast<core::i32>(window["width"].asInteger());
    placement.height = static_cast<core::i32>(window["height"].asInteger());
    placement.maximized = window["maximized"].asBool();
    // A size of nothing is not a window somebody left; it is a truncated file.
    if (placement.width <= 0 || placement.height <= 0)
        return std::nullopt;
    return placement;
}
} // namespace

void Editor::rememberState(const std::filesystem::path& stateDirectory) const
{
    // JSON of one field rather than the bare path, because the second thing an
    // editor wants to remember arrives sooner than anybody expects and a file
    // that is only a string has nowhere to put it. It arrived: folder colours.
    core::JsonWriter writer;
    writer.beginObject();
    writer.field("openScene", m_openScene);
    // Which default arrangement this project has already been shown. See
    // `Editor::CurrentLayoutRevision`.
    writer.field("layoutRevision", static_cast<core::f64>(m_layoutRevision));

    // **What a person set, rather than what they did.** The manipulator's mode
    // and space, snapping and its steps, and how the browser lays entries out:
    // none of it is about the world, all of it is somebody's answer to "how do I
    // work", and an editor that asks the question again every launch is one they
    // answer by hand every morning. The docking, the panel sizes and which tab
    // is open are ImGui's `.engine/layout.ini` -- two files because two writers,
    // not because two ideas.
    writer.key("tools");
    writer.beginObject();
    writer.field("gizmo", gizmoModeName(m_gizmoMode));
    // The word rather than a flag, for the same reason: `"space": false` names
    // nothing, and `local` and `world` are what the toolbar itself says.
    writer.field("space", m_gizmoLocal ? "local" : "world");
    // The same shape and the same argument: `"origin": true` names nothing, and
    // `pivot` and `centre` are what the toolbar itself says.
    writer.field("origin", m_gizmoOrigin == GizmoOrigin::Centre ? "centre" : "pivot");
    writer.field("snap", m_snap);
    writer.key("snapSteps");
    writer.beginArray();
    for (const f32 step : m_snapStep)
        writer.value(static_cast<core::f64>(step));
    writer.endArray();
    // The word again, for the third time and the same argument: `"tool": 1`
    // names nothing and `sculpt` is what the toolbar itself says.
    writer.field("tool", toolName(m_tool));
    writer.key("brush");
    writer.beginObject();
    writer.field("radius", static_cast<core::f64>(m_brush.radius));
    writer.field("spacing", static_cast<core::f64>(m_brush.spacing));
    writer.field("strength", static_cast<core::f64>(m_brush.strength));
    writer.field("material", static_cast<core::f64>(m_brush.material));
    writer.field("op", brushOpName(m_brush.op));
    writer.field("shape", m_brush.shape == BrushShape::Box ? "box" : "sphere");
    writer.field("falloff", static_cast<core::f64>(m_brush.falloff));
    writer.field("paint", paintModeName(m_brush.paintMode));
    writer.endObject();
    writer.key("blocks");
    writer.beginObject();
    writer.field("op", m_blockOp == BlockOp::Break ? "break" : m_blockOp == BlockOp::Replace ? "replace" : "place");
    writer.field("type", static_cast<core::f64>(m_blockType));
    writer.endObject();
    writer.key("water");
    writer.beginObject();
    writer.field("op", m_waterOp == WaterOp::Lake    ? "lake"
                       : m_waterOp == WaterOp::Ocean ? "ocean"
                       : m_waterOp == WaterOp::Pool  ? "pool"
                                                     : "river");
    writer.field("width", static_cast<core::f64>(m_waterWidth));
    writer.field("depth", static_cast<core::f64>(m_waterDepth));
    writer.endObject();
    writer.endObject();

    writer.key("panels");
    writer.beginObject();
    writer.field("contentView", contentViewName(m_contentView));
    writer.endObject();

    // **The OS window, which ImGui's layout file cannot hold**: that one knows
    // about the panels INSIDE a window and nothing about the window. Two halves
    // of one expectation -- "it opens the way I left it" -- and a person cannot
    // tell which file answers which half.
    if (m_window.has_value()) {
        writer.key("window");
        writer.beginObject();
        writer.field("x", static_cast<core::i64>(m_window->x));
        writer.field("y", static_cast<core::i64>(m_window->y));
        writer.field("width", static_cast<core::i64>(m_window->width));
        writer.field("height", static_cast<core::i64>(m_window->height));
        writer.field("maximized", m_window->maximized);
        writer.endObject();
    }

    if (!m_contentColors.empty()) {
        writer.key("folderColors");
        writer.beginObject();
        // `std::map`, so this is the same bytes for the same state without
        // sorting here -- the property every other format in this repository
        // has and the reason the container is ordered.
        for (const auto& entry : m_contentColors)
            writer.field(entry.first, writeHexColor(entry.second));
        writer.endObject();
    }
    writer.endObject();

    (void)platform::createDirectories(stateDirectory);
    (void)platform::writeTextFile(stateDirectory / "editor.json", writer.text());
}

void Editor::recallState(const std::filesystem::path& stateDirectory)
{
    m_contentColors.clear();

    std::string text;
    if (!platform::readTextFile(stateDirectory / "editor.json", text))
        return;

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(text); !parsed.ok)
        return;

    const core::JsonValue root = document.root();

    // Every block is optional and each is read on its own. A file written before
    // one of these existed is not a broken file -- it is a file from last week,
    // and the missing block means "whatever this build's default is".
    //
    // Absent means zero here, and zero is the answer that matters: a project
    // arranged before this field existed is exactly the one whose default tab
    // was never applied.
    if (const core::JsonValue revision = root["layoutRevision"]; revision.type() == core::JsonType::Number)
        m_layoutRevision = static_cast<core::i64>(revision.asNumber());

    if (const core::JsonValue tools = root["tools"]; tools.type() == core::JsonType::Object) {
        m_gizmoMode = gizmoModeFrom(tools["gizmo"].asString());
        m_gizmoLocal = tools["space"].asString() == "local";
        m_gizmoOrigin = tools["origin"].asString() == "centre" ? GizmoOrigin::Centre : GizmoOrigin::Pivot;
        if (const core::JsonValue snap = tools["snap"]; snap.type() == core::JsonType::Boolean)
            m_snap = snap.asBool();
        if (const core::JsonValue steps = tools["snapSteps"]; steps.type() == core::JsonType::Array) {
            for (core::usize index = 0; index < steps.size() && index < std::size(m_snapStep); ++index) {
                // Through the setter's rule rather than around it: a hand-edited
                // zero or a negative is a snap that divides by nothing.
                const auto step = static_cast<f32>(steps.at(index).asNumber());
                m_snapStep[index] = step > 0.0f ? step : m_snapStep[index];
            }
        }
        m_tool = toolFrom(tools["tool"].asString());
        if (const core::JsonValue brush = tools["brush"]; brush.type() == core::JsonType::Object) {
            // **Through the setters, so a hand-edited file cannot make a brush
            // the UI could not have made.** A radius of zero stamps nothing and
            // a material of zero would erase the world on the first click.
            if (const core::JsonValue radius = brush["radius"]; radius.type() == core::JsonType::Number)
                setBrushRadius(static_cast<f32>(radius.asNumber()));
            if (const core::JsonValue spacing = brush["spacing"]; spacing.type() == core::JsonType::Number)
                setBrushSpacing(static_cast<f32>(spacing.asNumber()));
            if (const core::JsonValue material = brush["material"]; material.type() == core::JsonType::Number)
                setBrushMaterial(static_cast<core::u8>(std::clamp(material.asNumber(), 0.0, 255.0)));
            if (const core::JsonValue strength = brush["strength"]; strength.type() == core::JsonType::Number)
                setBrushStrength(static_cast<f32>(strength.asNumber()));
            m_brush.op = brushOpFrom(brush["op"].asString());
            m_brush.shape = brush["shape"].asString() == "box" ? BrushShape::Box : BrushShape::Sphere;
            if (const core::JsonValue falloff = brush["falloff"]; falloff.type() == core::JsonType::Number)
                setBrushFalloff(static_cast<f32>(falloff.asNumber()));
            if (const core::JsonValue paint = brush["paint"]; paint.type() == core::JsonType::String)
                m_brush.paintMode = paintModeFrom(paint.asString());
        }
        if (const core::JsonValue blocks = tools["blocks"]; blocks.type() == core::JsonType::Object) {
            const std::string_view op = blocks["op"].asString();
            m_blockOp = op == "break" ? BlockOp::Break : op == "replace" ? BlockOp::Replace : BlockOp::Place;
            if (const core::JsonValue type = blocks["type"]; type.type() == core::JsonType::Number)
                setBlockType(static_cast<asset::BlockId>(std::clamp(type.asNumber(), 1.0, 65535.0)));
        }
        if (const core::JsonValue water = tools["water"]; water.type() == core::JsonType::Object) {
            const std::string_view op = water["op"].asString();
            m_waterOp = op == "lake"    ? WaterOp::Lake
                        : op == "ocean" ? WaterOp::Ocean
                        : op == "pool"  ? WaterOp::Pool
                                        : WaterOp::River;
            if (const core::JsonValue width = water["width"]; width.type() == core::JsonType::Number)
                setWaterWidth(static_cast<f32>(width.asNumber()));
            if (const core::JsonValue depth = water["depth"]; depth.type() == core::JsonType::Number)
                setWaterDepth(static_cast<f32>(depth.asNumber()));
        }
    }

    if (const core::JsonValue panels = root["panels"]; panels.type() == core::JsonType::Object)
        m_contentView = contentViewFrom(panels["contentView"].asString());

    m_window = readWindow(root);

    // Read last and not returned from early: a file with no colours still has
    // everything above it.
    if (const core::JsonValue colors = root["folderColors"]; colors.type() == core::JsonType::Object) {
        for (core::usize index = 0; index < colors.size(); ++index) {
            const std::string_view path = colors.keyAt(index);
            if (const std::optional<core::Color3> color = parseHexColor(colors[path].asString()); color.has_value())
                m_contentColors.emplace(std::string(path), *color);
        }
    }

    // Recalling is not a change somebody made.
    m_preferencesDirty = false;
}

std::optional<platform::WindowPlacement> Editor::recallWindow(const std::filesystem::path& stateDirectory)
{
    std::string text;
    if (!platform::readTextFile(stateDirectory / "editor.json", text))
        return std::nullopt;

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(text); !parsed.ok)
        return std::nullopt;

    return readWindow(document.root());
}

void Editor::rememberWindow(const platform::WindowPlacement& placement) noexcept
{
    // **A maximised window's geometry is not the geometry to keep.** SDL reports
    // the screen it fills, so storing that and restoring it would give somebody
    // who un-maximises a window the size of their display with a title bar. The
    // geometry stays whatever it was when the window was last normal, and the
    // flag carries the rest.
    if (placement.maximized) {
        if (m_window.has_value() && m_window->maximized)
            return;
        platform::WindowPlacement kept = m_window.value_or(placement);
        kept.maximized = true;
        m_window = kept;
        m_preferencesDirty = true;
        return;
    }

    if (m_window.has_value() && m_window->x == placement.x && m_window->y == placement.y &&
        m_window->width == placement.width && m_window->height == placement.height && !m_window->maximized) {
        return;
    }
    m_window = placement;
    m_preferencesDirty = true;
}

std::string Editor::recallOpenScene(const std::filesystem::path& stateDirectory)
{
    std::string text;
    if (!platform::readTextFile(stateDirectory / "editor.json", text))
        return {};

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(text); !parsed.ok)
        return {};

    return std::string(document.root()["openScene"].asString());
}

void UndoStack::record(const scene::World& world, std::string label, core::u64 coalesceKey)
{
    // Consecutive work on the same thing is one step. A drag on a colour writes
    // a value every frame, and without this a two-second drag buries everything
    // before it under a hundred and twenty steps of the same colour.
    if (coalesceKey != 0 && !m_undo.empty() && m_undo.back().key == coalesceKey)
        return;
    record(world.snapshot(), std::move(label), coalesceKey);
}

void UndoStack::record(scene::WorldSnapshot state, std::string label, core::u64 coalesceKey)
{
    if (coalesceKey != 0 && !m_undo.empty() && m_undo.back().key == coalesceKey)
        return;

    // Anything ahead of here is a future that no longer happens. Keeping it
    // would let a redo after a new edit apply a change to a world that has
    // moved on -- which is the one way an undo stack can destroy work rather
    // than restore it.
    m_redo.clear();

    m_undo.push_back(Step{std::move(state), std::move(label), coalesceKey});
    while (m_undo.size() > Depth)
        m_undo.pop_front();
}

bool UndoStack::undo(scene::World& world)
{
    if (m_undo.empty())
        return false;

    // The world as it is now becomes the redo, carrying the label of the step
    // being undone -- so "Redo Delete" names the thing it will do again rather
    // than the thing before it.
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back(Step{world.snapshot(), step.label, 0});
    world.restore(step.state);
    return true;
}

bool UndoStack::redo(scene::World& world)
{
    if (m_redo.empty())
        return false;

    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back(Step{world.snapshot(), step.label, 0});
    world.restore(step.state);
    return true;
}

std::string_view UndoStack::undoLabel() const noexcept
{
    return m_undo.empty() ? std::string_view{} : std::string_view{m_undo.back().label};
}

std::string_view UndoStack::redoLabel() const noexcept
{
    return m_redo.empty() ? std::string_view{} : std::string_view{m_redo.back().label};
}

void UndoStack::clear() noexcept
{
    m_undo.clear();
    m_redo.clear();
}

bool Editor::undo(scene::World& world, Inspector& inspector)
{
    // **Not in the middle of a stroke** (terrain audit E1): it popped the
    // stroke's own step with the button still down, and the stamps after it
    // belonged to no step at all.
    if (m_stroke.has_value() || m_blockStroke.has_value() || m_tileStroke.has_value() || m_import != nullptr)
        return false;
    const std::string label(m_history.undoLabel());
    if (!m_history.undo(world))
        return false;
    ++m_worldRestores;

    inspector.pruneDead(world);
    inspector.onWorldRestored();

    // "Undone: Edit CFrame" -- what the step was, as the history names it (the
    // owner: "undid Edit" read strangely).
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.undone"), {{"label", label}}), false};
    return true;
}

bool Editor::redo(scene::World& world, Inspector& inspector)
{
    if (m_stroke.has_value() || m_blockStroke.has_value() || m_tileStroke.has_value() || m_import != nullptr)
        return false;
    const std::string label(m_history.redoLabel());
    if (!m_history.redo(world))
        return false;
    ++m_worldRestores;

    inspector.pruneDead(world);
    inspector.onWorldRestored();

    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.redone"), {{"label", label}}), false};
    return true;
}

namespace {

[[nodiscard]] bool isClass(const scene::World& world, core::InstanceId id, std::string_view className) noexcept
{
    const scene::ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    return descriptor != nullptr && world.atoms().text(descriptor->name) == className;
}

} // namespace

bool Editor::fileBacked(const scene::World& world, core::InstanceId id)
{
    return world.alive(id) && world.mounted(id);
}

bool Editor::isEngineOwned(const scene::World& world, core::InstanceId id, core::InstanceId root) noexcept
{
    if (!world.alive(id))
        return false;
    if (id == root)
        return true;
    // `GlobalScriptService`'s three folders are the engine's too (ADR 0105).
    if (world.fixed(id))
        return true;

    const scene::ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    return descriptor != nullptr && scene::hasFlag(descriptor->flags, scene::ClassFlags::Service);
}

bool Editor::deleteInstance(scene::World& world, core::InstanceId id, core::InstanceId root, Inspector& inspector)
{
    if (!world.alive(id))
        return false;

    if (isEngineOwned(world, id, root)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_one_belongs_to_the")), true};
        return false;
    }

    const std::string name(world.atoms().text(world.name(id)));
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.delete"), {{"name", name}}));
    if (!world.destroy(id))
        return false;

    // **Retired here, because nothing else will while the editor is editing.**
    // `destroy` marks and unlinks; the record stops resolving in
    // `retireDestroyed`, which runs at the end of a signal drain -- and a paused
    // world runs no drains, so a deleted instance would keep answering `alive`
    // until somebody pressed play.
    //
    // The `Destroying` signal is therefore not fired for an editor's delete, and
    // that is the honest reading rather than an oversight: while editing, no
    // script is running to hear it. When a scene's scripts start running again
    // they do so against a world where the instance was never there.
    world.retireDestroyed();

    // The selection cannot outlive what it names. `destroy` leaves the handle
    // resolving until the end of the drain that carries `Destroying`, so this
    // is asked as a question about the tree rather than about the id.
    // A sweep rather than a comparison. `destroy` took the whole subtree, so
    // testing the selection against `id` alone would leave a selected CHILD of
    // what was deleted pointing at an instance the world has retired.
    inspector.pruneDead(world);

    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.deleted_there_is_no_undo"), {{"name", name}}), false};
    return true;
}

bool Editor::duplicateInstance(scene::World& world, core::InstanceId id, core::InstanceId root, Inspector& inspector)
{
    if (!world.alive(id))
        return false;

    const core::InstanceId parent = world.parentOf(id);
    if (!parent.valid())
        return false;

    if (isEngineOwned(world, id, root)) {
        // "One per world" is what a service IS. A second one would make every
        // `GetService` a question with two answers.
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.a_service_is_one_per")), true};
        return false;
    }

    m_history.record(
        world, core::tr(ENG_TR("engine.editor.history.duplicate"), {{"name", world.atoms().text(world.name(id))}}));
    const core::InstanceId copy = world.clone(id);
    if (!copy.valid())
        return false;

    (void)world.setParent(copy, parent);
    // Selected, because the reason to duplicate a thing is to change the copy
    // and not to admire it.
    inspector.select(copy);
    inspector.reveal(copy);

    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.status.duplicated"), {{"name", world.atoms().text(world.name(id))}}), false};
    return true;
}

bool Editor::createInstance(scene::World& world, scene::ClassId classId, core::InstanceId parent, core::InstanceId root,
                            Inspector& inspector, std::optional<core::i32> runContext)
{
    if (!world.alive(parent))
        return false;

    const scene::ClassDescriptor* descriptor = world.classes().find(classId);
    if (descriptor == nullptr || !creatable(*descriptor)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_class_cannot_be_created")), true};
        return false;
    }

    // **A service is a legal parent and the world itself is too.** What
    // `isEngineOwned` refuses is deleting, duplicating or renaming one -- but
    // `Lighting` holding a `PointLight` and `Workspace` holding a `Part` is
    // what those services are FOR, so the guard the other three verbs share
    // does not belong here. What does belong is the one it never covered:
    // something a system made is not somewhere a person authors into -- and
    // that has to be asked of the ANCESTRY, because a chunk marks its folder
    // and not the ground inside it.
    if (!canParentInto(world, parent, root)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_was_made_by_the")), true};
        return false;
    }

    m_history.record(
        world, core::tr(ENG_TR("engine.editor.history.create"), {{"name", world.atoms().text(descriptor->name)}}));

    const core::InstanceId made = world.create(classId);
    if (!made.valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_create_that_class")), true};
        return false;
    }
    if (world.setParent(made, parent).has_value()) {
        (void)world.destroy(made);
        world.retireDestroyed();
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_cannot_be_parented_there")), true};
        return false;
    }
    if (runContext.has_value()) {
        (void)world.setProperty(
            made, world.atoms().intern("RunContext"),
            scene::Value{scene::EnumValue{world.enums().findId(world.atoms().lookup("RunContext")), *runContext}});
    }
    adoptServiceSide(world, made);

    // **An interface element starts 50 by 50 pixels** (the owner's call), not
    // at the zero size a script's `Instance.new` gives it -- a Frame made in
    // the editor with no size is a Frame nobody can see to drag. Through
    // `setProperty`, so a class with no `Size` of type UDim2 simply refuses.
    if (const scene::ClassId uiObject = world.classes().findId(world.atoms().lookup("UIObject"));
        uiObject != scene::InvalidClass && world.classes().isA(classId, uiObject)) {
        (void)world.setProperty(made, world.atoms().intern("Size"),
                                scene::Value{core::UDim2{core::UDim{0.0f, 50.0f}, core::UDim{0.0f, 50.0f}}});
    }

    // **A script made in the editor starts with something in it** (the
    // owner's call, and the reference editor's): a `Script` says hello, and a
    // `ModuleScript` is the table it returns, with nothing else. A script's
    // own `Instance.new` still gives an empty `Source` -- that is the class's
    // default, and code asking for a script asked for exactly that.
    if (isClass(world, made, "ModuleScript")) {
        (void)world.setProperty(made, world.atoms().intern("Source"),
                                scene::Value{std::string("local module = {}\n\nreturn module\n")});
    }
    else if (const scene::ClassId baseScript = world.classes().findId(world.atoms().lookup("BaseScript"));
             baseScript != scene::InvalidClass && world.classes().isA(classId, baseScript)) {
        (void)world.setProperty(made, world.atoms().intern("Source"),
                                scene::Value{std::string("print(\"Hello World!\")\n")});
    }

    // **Inside a script service, a script is a file** (ADR 0105). The walk up
    // collects the folders between it and the service; a script inside
    // another script, or a scene that has no name yet, stays in the scene.
    if (const scene::ClassId baseScript = world.classes().findId(world.atoms().lookup("BaseScript"));
        baseScript != scene::InvalidClass && world.classes().isA(classId, baseScript)) {
        std::vector<std::string> folders;
        std::string container;
        std::string folderRoot;
        bool module = isClass(world, made, "ModuleScript");
        std::string sceneName = std::filesystem::path(m_openScene).filename().string();
        if (constexpr std::string_view Suffix = ".scene.json"; sceneName.ends_with(Suffix))
            sceneName.resize(sceneName.size() - Suffix.size());
        for (core::InstanceId walk = parent; walk.valid(); walk = world.parentOf(walk)) {
            if (isClass(world, walk, "ServerScriptService") || isClass(world, walk, "ClientScriptService")) {
                if (sceneName.empty())
                    break;
                const bool server = isClass(world, walk, "ServerScriptService");
                container = server ? "ServerScriptService" : "ClientScriptService";
                folderRoot = "src/scenes/" + sceneName + (server ? "/server" : "/client");
                break;
            }
            if (world.fixed(walk) && isClass(world, world.parentOf(walk), "GlobalScriptService")) {
                const std::string_view folder = world.atoms().text(world.name(walk));
                container = "GlobalScriptService/" + std::string(folder);
                folderRoot = folder == "Server" ? "src/server" : folder == "Shared" ? "src/shared" : "src/client";
                // The class made is the class kept: a `Script` in `Shared` is
                // written as `.script.luau` rather than turned into a module.
                break;
            }
            if (!isClass(world, walk, "Folder"))
                break;
            folders.emplace_back(world.atoms().text(world.name(walk)));
        }
        if (!container.empty()) {
            std::string directory = folderRoot;
            for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder)
                directory += "/" + *folder;
            const std::optional<scene::Value> source = world.getProperty(made, world.atoms().intern("Source"));
            const auto* text = source.has_value() ? std::get_if<std::string>(&source.value()) : nullptr;
            m_scriptFileRequest = ScriptFileRequest{
                .placeholder = made,
                .directory = std::move(directory),
                .name = std::string(world.atoms().text(world.name(made))),
                .container = std::move(container),
                .root = std::move(folderRoot),
                .module = module,
                .source = text != nullptr ? *text : std::string{},
            };
        }
    }

    // **In front of the camera rather than at the origin.** In a streamed world
    // the origin is not where anybody is standing, and a part created four
    // kilometres from the view is one nobody finds. Through `setProperty`
    // rather than into the component, so a class with no `CFrame` needs no
    // special case here -- it simply refuses and nothing is placed.
    //
    // Not for something whose `CFrame` is RELATIVE to the part or attachment it
    // was made in -- a light, an attachment, a decal -- where the world place
    // in front of the camera would be read as an offset from the holder.
    const bool heldRelative = world.parts().find(made) == nullptr &&
                              (world.parts().find(parent) != nullptr || world.attachments().find(parent) != nullptr);
    if (m_cameraAdopted && !heldRelative) {
        const core::Mat3& basis = m_cameraCFrame.rotation;
        const core::Vec3 forward{-basis.m[2][0], -basis.m[2][1], -basis.m[2][2]};
        // Far enough to be whole in the view and near enough to be reachable.
        constexpr f32 kSpawnDistance = 8.0f;
        core::CFrameD placed;
        placed.position = m_cameraCFrame.position + core::toDVec3(forward * kSpawnDistance);
        (void)world.setProperty(made, world.atoms().intern("CFrame"), scene::Value{placed});
    }

    // Selected, for the reason a duplicate is: the point of making a thing is
    // to change it. **And revealed**, because a parent that has never been
    // opened is not opened by gaining a child -- and an empty one had no
    // chevron to open it with, so a `Part` made inside a fresh `Folder` was
    // invisible every single time.
    inspector.select(made);
    inspector.reveal(made);

    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.status.added_a"), {{"name", world.atoms().text(descriptor->name)}}), false};
    return true;
}

bool Editor::canParentInto(const scene::World& world, core::InstanceId id, core::InstanceId root)
{
    if (!world.alive(id))
        return false;

    // Up the whole chain. Streaming marks a chunk's FOLDER and not the parts
    // inside it -- `streaming_glue.cpp` says so where it sets the flag, and the
    // scene serializer relies on exactly that economy -- so an instance that is
    // not itself generated may still be sitting inside something that is.
    //
    // **Anything else takes a child** -- the owner: "I should be able to put an
    // instance inside any other; whether it does anything is another story" --
    // and the scene saves what is inside every service (`scene_file.cpp`,
    // `FirstServices`) -- the script services and a script included: a script is an
    // instance, and one made from a file keeps what is put inside it too (ADR
    // 0092).
    for (core::InstanceId walk = id; walk.valid(); walk = world.parentOf(walk)) {
        if (world.generated(walk))
            return false;
        // **A `Player` is the engine's, made for whoever is taking part**, and
        // a scene never writes one or anything inside it -- so putting
        // something there was losing it at the next save (the audit of
        // 2026-09-28). The same rule `scene_file.cpp`'s `engineMade` follows.
        if (isClass(world, walk, "Player"))
            return false;
        if (walk == root) {
            if (isClass(world, walk, "DataModel"))
                return walk != id;
            break;
        }
    }
    return true;
}

bool Editor::authorable(const scene::World& world, core::InstanceId id, core::InstanceId root)
{
    // The same walk, plus the one thing a PARENT is allowed to be and a moved
    // or deleted instance is not: one of the engine's own.
    return canParentInto(world, id, root) && !isEngineOwned(world, id, root);
}

Editor::ReparentPlan Editor::planReparent(const scene::World& world, std::span<const core::InstanceId> ids,
                                          core::InstanceId newParent, core::InstanceId root)
{
    ReparentPlan plan;
    if (!world.alive(newParent)) {
        plan.targetRefuses = true;
        return plan;
    }
    if (!canParentInto(world, newParent, root)) {
        plan.targetRefuses = true;
        return plan;
    }

    // Document order rather than click order, so the result is a function of
    // the SET. It is also the order that keeps a parent ahead of its own child,
    // which stops a move of both depending on which was reached first.
    std::vector<core::InstanceId> ordered;
    orderByTree(world, root, ids, ordered);

    for (const core::InstanceId id : ordered) {
        if (isEngineOwned(world, id, root)) {
            ++plan.refused;
            continue;
        }
        // **A script made from a file moves, and its file follows at the save**
        // (`script_files.h`): refusing the drag is what sent people to copy
        // and paste instead, and a pasted copy beside a file that still
        // mounted was two scripts at the next open.
        // A cycle: onto itself, or into its own subtree. `World::setParent`
        // refuses both and this asks the SAME function rather than carrying a
        // second copy of the rule.
        if (id == newParent || world.isAncestorOf(id, newParent)) {
            ++plan.refused;
            continue;
        }
        // Already there. Not a refusal -- a parent earlier in the walk has
        // taken its children with it, and re-parenting a child to where it
        // already is would only move it to the end of the sibling list.
        if (world.parentOf(id) == newParent)
            continue;
        plan.movable.push_back(id);
    }
    return plan;
}

namespace {

// Where `id` is under `root`, child index by child index.
[[nodiscard]] std::vector<core::usize> childPosition(const scene::World& world, core::InstanceId root,
                                                     core::InstanceId id)
{
    std::vector<core::usize> path;
    for (core::InstanceId walk = id; walk.valid() && walk != root; walk = world.parentOf(walk)) {
        core::usize index = 0;
        for (core::InstanceId sibling = world.firstChild(world.parentOf(walk)); sibling.valid() && sibling != walk;
             sibling = world.nextSibling(sibling))
            ++index;
        path.insert(path.begin(), index);
    }
    return path;
}

[[nodiscard]] core::InstanceId atChildPosition(const scene::World& world, core::InstanceId root,
                                               const std::vector<core::usize>& path)
{
    core::InstanceId at = root;
    for (const core::usize index : path) {
        core::InstanceId child = world.firstChild(at);
        for (core::usize step = 0; step < index && child.valid(); ++step)
            child = world.nextSibling(child);
        if (!child.valid())
            return {};
        at = child;
    }
    return at;
}

} // namespace

void Editor::copySelection(const scene::World& world, std::span<const core::InstanceId> ids, core::InstanceId root)
{
    m_clipboard.clear();
    m_clipboardMarks.clear();
    if (ids.empty())
        return;

    // Document order, so pasting four things back reproduces the order they
    // were in rather than the order somebody happened to ctrl-click. It is the
    // same reason every batch verb sorts, and R10's discipline applied to a
    // clipboard.
    std::vector<core::InstanceId> ordered;
    orderByTree(world, root, ids, ordered);

    for (const core::InstanceId id : ordered) {
        if (isEngineOwned(world, id, root))
            continue;
        // **Anything inside another thing already copied is skipped.** Copying
        // a parent and its child and pasting would otherwise produce the child
        // twice: once inside the parent, where it belongs, and once beside it.
        bool insideAnother = false;
        for (const core::InstanceId other : ordered) {
            if (other != id && world.isAncestorOf(other, id)) {
                insideAnother = true;
                break;
            }
        }
        if (insideAnother)
            continue;
        m_clipboard.push_back(scene::writeCopy(world, id));
        // **The stamp marks in it, by where they are** (B11): the text holds
        // every stamped instance in full, and a paste that dropped their marks
        // made unlinked copies the next change to the stamp left behind.
        std::vector<ClipboardMark> marks;
        std::vector<core::InstanceId> subtree{id};
        world.collectDescendants(id, subtree);
        for (const core::InstanceId each : subtree) {
            if (const core::NameAtom mark = world.stampOf(each); mark.valid())
                marks.push_back(ClipboardMark{childPosition(world, id, each), std::string(world.atoms().text(mark))});
        }
        m_clipboardMarks.push_back(std::move(marks));
    }

    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.copied_instance_s"),
                                     {{"count", static_cast<core::i64>(m_clipboard.size())}}),
                            false};
}

bool Editor::paste(scene::World& world, core::InstanceId parent, core::InstanceId root, Inspector& inspector)
{
    if (m_clipboard.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.there_is_nothing_to_paste")), true};
        return false;
    }
    if (!canParentInto(world, parent, root)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_authored_can_live_in")), true};
        return false;
    }
    // **Not a stamp inside itself**, as placing one refuses: an instance of
    // the stamp being edited, pasted into it, is a file that contains itself.
    if (m_stamp.open()) {
        for (const std::vector<ClipboardMark>& marks : m_clipboardMarks) {
            for (const ClipboardMark& mark : marks) {
                if (mark.stamp == m_stamp.path) {
                    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_an_instance_of")), true};
                    return false;
                }
            }
        }
    }

    // Recorded before the first one, so a paste of four is one press of ctrl-Z.
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.paste_count"),
                                     {{"count", static_cast<core::i64>(m_clipboard.size())}}));

    std::vector<core::InstanceId> pasted;
    for (core::usize index = 0; index < m_clipboard.size(); ++index) {
        scene::SceneIoReport report;
        const core::InstanceId placed = scene::readStamp(world, m_clipboard[index], parent, "<clipboard>", &report);
        if (!placed.valid())
            continue;
        // **Not the `<clipboard>` mark reading gives it**, which names no file;
        // the marks the copied instances had, which do.
        world.setStamp(placed, core::NameAtom{});
        if (index < m_clipboardMarks.size()) {
            for (const ClipboardMark& mark : m_clipboardMarks[index]) {
                if (const core::InstanceId at = atChildPosition(world, placed, mark.position); at.valid())
                    world.setStamp(at, world.atoms().intern(mark.stamp));
            }
        }
        adoptServiceSide(world, placed);
        pasted.push_back(placed);
    }

    if (pasted.empty()) {
        // Nothing was built, so the step is taken back rather than left: a step
        // that undoes nothing eats a press of ctrl-Z.
        (void)m_history.undo(world);
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_in_the_clipboard_could")), true};
        return false;
    }

    inspector.select(pasted);
    inspector.reveal(pasted.front());
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.status.pasted_instance_s"), {{"count", static_cast<core::i64>(pasted.size())}}),
        false};
    return true;
}

bool Editor::placeMesh(scene::World& world, std::string_view path, core::InstanceId parent, core::InstanceId root,
                       Inspector& inspector, std::optional<core::DVec3> restOn)
{
    const scene::ClassId meshPart = world.classes().findId(world.atoms().lookup("MeshPart"));
    if (meshPart == scene::InvalidClass) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.this_build_has_no_meshpart")), true};
        return false;
    }
    if (!createInstance(world, meshPart, parent, root, inspector))
        return false;
    const core::InstanceId made = inspector.selection();
    if (!world.alive(made) || world.classOf(made) != meshPart)
        return false;
    // The file's own name, minus its folders and its extension: what somebody
    // will look for in the tree.
    std::string stem = std::filesystem::path(std::string(path)).stem().string();
    if (!stem.empty())
        world.setName(made, world.atoms().intern(stem));
    (void)world.setProperty(made, world.atoms().intern("MeshContent"),
                            scene::Value{std::string(asset::AssetScheme) + std::string(path)});
    if (restOn.has_value()) {
        core::CFrameD frame;
        frame.position = *restOn;
        (void)world.setProperty(made, world.atoms().intern("CFrame"), scene::Value{frame});
    }
    m_meshFits.push_back(MeshFit{made, restOn, 0});
    touch();
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.placed"), {{"value", stem}}), false};
    return true;
}

bool Editor::canReparent(const scene::World& world, std::span<const core::InstanceId> ids, core::InstanceId newParent,
                         core::InstanceId root)
{
    return !ids.empty() && !planReparent(world, ids, newParent, root).movable.empty();
}

namespace {

// `text` with `from` at its start replaced by `to` -- the whole of it, or a
// path inside it -- or nothing when it does not name `from`.
[[nodiscard]] std::optional<std::string> movedPath(std::string_view text, std::string_view from, std::string_view to)
{
    if (text == from)
        return std::string(to);
    if (text.size() > from.size() && text.starts_with(from) && text[from.size()] == '/')
        return std::string(to) + std::string(text.substr(from.size()));
    return std::nullopt;
}

// Every instance of `world` that names `from` -- a content property, a
// material, a stamp mark -- pointed at `to`. How many it changed.
std::size_t retargetWorld(scene::World& world, std::string_view from, std::string_view to)
{
    const std::string oldUrn = std::string(asset::AssetScheme) + std::string(from);
    const std::string newUrn = std::string(asset::AssetScheme) + std::string(to);
    std::size_t changed = 0;
    std::vector<core::InstanceId> pending;
    for (core::InstanceId root = world.firstChild(core::InstanceId{}); root.valid(); root = world.nextSibling(root))
        pending.push_back(root);
    while (!pending.empty()) {
        const core::InstanceId id = pending.back();
        pending.pop_back();
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child))
            pending.push_back(child);

        if (const core::NameAtom mark = world.stampOf(id); mark.valid()) {
            if (const std::optional<std::string> moved = movedPath(world.atoms().text(mark), from, to)) {
                world.setStamp(id, world.atoms().intern(*moved));
                ++changed;
            }
        }
        for (scene::ClassId cls = world.classOf(id); cls != scene::InvalidClass;) {
            const scene::ClassDescriptor* descriptor = world.classes().find(cls);
            if (descriptor == nullptr)
                break;
            for (const scene::PropertyDesc& property : descriptor->properties) {
                if (property.readOnly ||
                    (property.type != scene::ValueType::String && property.type != scene::ValueType::Material))
                    continue;
                const std::optional<scene::Value> value = world.getProperty(id, property.name);
                if (!value.has_value())
                    continue;
                if (const auto* text = std::get_if<std::string>(&*value)) {
                    if (const std::optional<std::string> moved = movedPath(*text, oldUrn, newUrn)) {
                        (void)world.setProperty(id, property.name, scene::Value{*moved});
                        ++changed;
                    }
                }
                else if (const auto* material = std::get_if<scene::MaterialRef>(&*value)) {
                    if (const std::optional<std::string> moved = movedPath(material->source, oldUrn, newUrn)) {
                        (void)world.setProperty(id, property.name,
                                                scene::Value{scene::MaterialRef{*moved, material->clone}});
                        ++changed;
                    }
                }
            }
            cls = descriptor->super;
        }
    }
    return changed;
}

} // namespace

std::string Editor::moveContent(scene::World& world, std::string_view from, std::string_view intoFolder)
{
    // **Not during play**: the stop puts back the world as it was before play,
    // with every path the move just rewrote pointing at where the file was.
    if (inPlayMode()) {
        report(core::tr(ENG_TR("engine.editor.status.stop_the_game_first_a")), true);
        return {};
    }
    std::string why;
    const std::string moved = m_content.move(from, intoFolder, &why);
    if (moved.empty()) {
        report(core::tr(ENG_TR("engine.editor.status.could_not_move"), {{"from", from}, {"why", why}}), true);
        return {};
    }
    if (moved == from)
        return moved;

    m_undoClearedByMove = false;
    const std::optional<std::size_t> followed = followContent(world, from, moved);
    if (!followed.has_value())
        return {};
    const std::size_t references = *followed;
    std::string said = core::tr(ENG_TR("engine.editor.status.moved_to"), {{"path", moved}});
    if (references > 0) {
        said = core::tr(ENG_TR("engine.editor.status.with_note"),
                        {{"status", said},
                         {"note", core::tr(ENG_TR("engine.editor.status.references_follow"),
                                           {{"count", static_cast<core::i64>(references)}})}});
    }
    if (m_undoClearedByMove) {
        said = core::tr(ENG_TR("engine.editor.status.with_clause"),
                        {{"status", said}, {"note", core::tr(ENG_TR("engine.editor.status.undo_cleared_by_move"))}});
    }
    report(std::move(said), false);
    return moved;
}

std::optional<std::size_t> Editor::followContent(scene::World& world, std::string_view from, std::string_view to)
{
    if (from == to)
        return 0;
    const RetargetOutcome rewritten =
        retargetContentReferencesAll(m_content.root(), m_content.root().parent_path() / "project.toml", from, to);
    if (!rewritten.failed.empty()) {
        // **All or none** (audit A6): the files are as they were, so the move
        // goes back too -- a file where nothing names it is the break this
        // exists to prevent.
        const bool undone = platform::renameFile(m_content.root() / std::filesystem::path(std::string(to)),
                                                 m_content.root() / std::filesystem::path(std::string(from)));
        (void)m_content.refresh();
        report(core::tr(undone ? ENG_TR("engine.editor.status.could_not_rewrite_stayed")
                               : ENG_TR("engine.editor.status.could_not_rewrite_stranded"),
                        {{"file", rewritten.failed.front().filename().string()}, {"from", from}}),
               true);
        return std::nullopt;
    }
    const std::size_t files = rewritten.changed;
    // The scene's world, and the stamp open over it, which is a world of its own.
    std::size_t instances = retargetWorld(world, from, to);
    if (m_stage != nullptr)
        instances += retargetWorld(m_stage->world(), from, to);
    // **A terrain's cells follow their folder** (the audit of 2026-09-28): the
    // index is not a property, so nothing above moved it, and the next save
    // wrote the resident cells back where they had been and pointed the scene
    // there -- the rest left behind in the folder that moved. With the index
    // followed, the save copies every cell into the scene's own folder.
    // **And its layers' materials** (audit A5): URNs in the component, not in
    // a property, so the walk above never saw them, and a moved material left
    // a terrain painted with nothing.
    const std::string oldUrn = std::string(asset::AssetScheme) + std::string(from);
    const std::string newUrn = std::string(asset::AssetScheme) + std::string(to);
    world.terrains().forEach([&](core::InstanceId, scene::TerrainComponent& terrain) {
        if (const std::optional<std::string> followed = movedPath(terrain.cellIndex, from, to); followed.has_value())
            terrain.cellIndex = *followed;
        for (std::string& layer : terrain.layers) {
            if (const std::optional<std::string> followed = movedPath(layer, oldUrn, newUrn); followed.has_value()) {
                layer = *followed;
                instances += 1;
            }
        }
    });
    // What is open here follows it, so the next save writes where it now is
    // rather than bringing the old path back.
    for (std::string* path : {&m_openScene, &m_stamp.path, &m_material.path}) {
        if (const std::optional<std::string> followed = movedPath(*path, from, to))
            *path = *followed;
    }
    // **Undo stops here.** Every step before this move holds the old paths,
    // and undoing one would point the world back at files that are not there
    // any more -- for the next save to write down. The steps go; the status
    // says so.
    if (files + instances > 0 && m_history.canUndo()) {
        m_history.clear();
        m_undoClearedByMove = true;
    }
    return files + instances;
}

std::string Editor::normalizeStampPath(std::string_view typed)
{
    return scene::normalizeStampPath(typed);
}

bool Editor::stampNameIsUsable(std::string_view typed)
{
    return sceneNameIsUsable(normalizeStampPath(typed));
}

scene::StampSource Editor::stampSource() const
{
    // Captured by value: the source outlives the call that made it, and a
    // reference into an editor that has been destroyed is the kind of thing
    // that works until somebody loads a scene during shutdown.
    const std::filesystem::path root = m_content.root();
    const std::shared_ptr<std::unordered_map<std::string, std::string>> built = m_stampTexts;
    return [root, built](std::string_view stamp) -> std::optional<std::string> {
        // Under `content/` or nothing (audit F5): the name is the scene's word.
        const std::optional<std::filesystem::path> file = core::resolveUnder(root, stamp);
        std::string text;
        if (!file.has_value() || !platform::readTextFile(*file, text))
            return std::nullopt;
        // The first reading is what the world was built from; a later one of
        // the same stamp is the same file unless something changed it, and
        // then `stampChangedOnDisk` is what moves the record on.
        (void)built->try_emplace(std::string(stamp), text);
        return text;
    };
}

core::u32 Editor::stampChangedOnDisk(scene::World& world, core::InstanceId gameRoot, std::string_view path)
{
    const auto found = m_stampTexts->find(std::string(path));
    if (found == m_stampTexts->end() || !world.alive(gameRoot))
        return 0;
    std::string now;
    if (!platform::readTextFile(m_content.root() / std::filesystem::path(path), now) || now == found->second)
        return 0;
    const std::string before = found->second;
    found->second = now;
    scene::SceneIoReport moved;
    const core::u32 followed = scene::restamp(world, gameRoot, path, before, now, &moved);
    if (followed > 0) {
        world.retireDestroyed();
        m_sceneDirty = true;
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.changed_on_disk_instance_s"),
                                         {{"path", path}, {"count", static_cast<core::i64>(followed)}}),
                                false};
    }
    return followed;
}

bool Editor::breakStamp(scene::World& world, core::InstanceId id)
{
    const core::InstanceId stampRoot = world.stampRootOf(id);
    if (!stampRoot.valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_not_a_stamped")), true};
        return false;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.break_stamp")));
    world.setStamp(stampRoot, core::NameAtom{});
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.broken_it_is_its_own")), false};
    return true;
}

Editor::Stage::Stage(scene::ClassRegistry& classes, scene::EnumRegistry& enums, core::AtomTable& atoms, core::u64 seed)
    : m_world(classes, enums, atoms, seed)
{
    // Exactly what drawing a subtree needs and nothing else: somewhere to put
    // the instances, and something to see them by. No DataModel, no services, no
    // scripts -- a stage is a place to arrange things and look at them, and
    // every row this does not create is a row that would have appeared in an
    // Explorer somebody opened to look at ONE prefab.
    //
    // `Service` and `NotCreatable` are not checked by `World::create` -- they
    // are rules about `Instance.new`, enforced in `script` -- which is what
    // lets the engine build its own furniture here as it does at boot.
    const auto make = [this, &classes, &atoms](std::string_view className) {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        const core::InstanceId instance = m_world.create(id);
        if (instance.valid())
            m_world.setName(instance, atoms.intern(className));
        return instance;
    };

    m_workspace = make("Workspace");
    m_lighting = make("Lighting");
}

bool Editor::openStamp(std::string_view path, scene::ClassRegistry& classes, scene::EnumRegistry& enums,
                       core::AtomTable& atoms, Inspector& inspector)
{
    if (m_run != RunState::Editing) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.stop_the_world_before_opening")), true};
        return false;
    }
    if (m_stamp.open()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.a_stamp_is_already_open")), true};
        return false;
    }

    const std::string relative = normalizeStampPath(path);
    std::string text;
    if (!platform::readTextFile(m_content.root() / std::filesystem::path(relative), text)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_stamp_is_not_there")), true};
        return false;
    }

    // **Built before anything is committed to.** A stage that fails to read its
    // stamp is a stage nobody wanted, and dropping it costs nothing -- while a
    // game world half-cleared for a stamp that would not load is the mess the
    // first cut of this had to unwind with a snapshot.
    auto stage = std::make_unique<Stage>(classes, enums, atoms, kStageSeed);
    // The stage resolves materials where the game does (ADR 0090).
    stage->world().setMaterialLibrary(m_materials);
    if (!stage->workspace().valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_build_a_stage")), true};
        return false;
    }

    scene::SceneIoReport report;
    const core::InstanceId root = scene::readStamp(stage->world(), text, stage->workspace(), relative, &report);
    if (!root.valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_stamp_could_not_be")), true};
        return false;
    }

    m_stage = std::move(stage);
    m_stamp = StampSession{relative, root, false, text};

    // A different world in the plainest sense -- a different `scene::World`
    // object -- so everything a panel keyed by id has to go. That is what
    // `onWorldChanged` is for, and it is exactly true here rather than
    // approximately true as it was when this cleared the game's scene instead.
    m_history.clear();
    inspector.onWorldChanged();
    inspector.select(root);
    inspector.reveal(root);

    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.editing"), {{"relative", relative}}), false};
    return true;
}

// --- Materials (ADR 0090) ---------------------------------------------------

std::string Editor::normalizeMaterialPath(std::string_view typed)
{
    std::string path(typed);
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    constexpr std::string_view ContentPrefix = "content/";
    while (path.compare(0, ContentPrefix.size(), ContentPrefix) == 0)
        path.erase(0, ContentPrefix.size());
    if (path.empty())
        return path;

    // A bare name lands in `content/materials/`, the convention and not a rule
    // -- the kind is in the compound suffix, as a stamp's is (ADR 0049).
    if (path.find('/') == std::string::npos)
        path = "materials/" + path;
    if (!asset::isMaterialPath(path))
        path += asset::MaterialSuffix;
    return path;
}

std::string Editor::contentUrn(std::string_view relative)
{
    return std::string(asset::AssetScheme) + normalizeMaterialPath(relative);
}

namespace {

// Writes one material file under the content root. The text is ADR 0090's:
// fixed key order, one field per line, a pure function of what it holds.
[[nodiscard]] bool writeMaterialFile(const std::filesystem::path& absolute, const asset::MaterialAsset& material)
{
    return platform::createDirectories(absolute.parent_path()) &&
           platform::writeTextFileDurable(absolute, asset::writeMaterialAsset(material));
}

} // namespace

std::string Editor::createMaterial(std::string_view name)
{
    const std::string relative = normalizeMaterialPath(name);
    if (relative.empty() || !sceneNameIsUsable(relative)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_not_a_name")), true};
        return {};
    }
    const std::filesystem::path absolute = m_content.root() / std::filesystem::path(relative);
    std::error_code ec;
    if (std::filesystem::exists(absolute, ec)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.something_is_already_called_that")), true};
        return {};
    }
    // **The engine default's values, declaring nothing** (ADR 0090): a new
    // material looks like a plain part, and it is exactly what its author makes
    // it until they opt a parameter in.
    asset::MaterialAsset material;
    material.written = asset::AllMaterialFields;
    if (!writeMaterialFile(absolute, material)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write_3"), {{"relative", relative}}), true};
        return {};
    }
    (void)m_content.refresh();
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.created"), {{"relative", relative}}), false};
    return relative;
}

std::string Editor::normalizeShaderPath(std::string_view typed)
{
    std::string path(typed);
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    constexpr std::string_view ContentPrefix = "content/";
    while (path.compare(0, ContentPrefix.size(), ContentPrefix) == 0)
        path.erase(0, ContentPrefix.size());
    if (path.empty())
        return path;
    if (path.find('/') == std::string::npos)
        path = "shaders/" + path;
    if (contentKindOf(path) != ContentKind::Shader)
        path += kShaderExtension;
    return path;
}

namespace {

// **What "New Surface Shader" writes.** It compiles as it stands and draws
// what the built-in surface draws with a tint, so a new file is never a broken
// one; everything else is comments, because the contract is best learnt in
// the file somebody is about to change.
constexpr std::string_view SurfaceShaderTemplate =
    R"hlsl(// A surface shader (ADR 0091). A material names this file in its `shader`
// field, and every part wearing that material is drawn with it -- lit, shadowed
// and fogged exactly as the built-in surface is. The contract, with every
// member documented, is `engine/surface.hlsli`.
#include "engine/surface.hlsli"

// **Parameters** are what a material sets, by name, and what the material panel
// shows as fields: `ENG_PARAM(type, Name, default, annotation)`, where the
// annotation is `range(min, max)`, `colour`, `toggle` or `none`.
ENG_PARAM(float3, Tint, float3(1.0, 1.0, 1.0), colour)
ENG_PARAM(float, Wobble, 0.0, range(0, 1))

// **Textures** are named the same way, and read white when a material sets
// none: `ENG_TEXTURE(Name)`, or `ENG_TEXTURE(Name, black)` / `normal`.
// Sample one with `ENG_SAMPLE(Name, inputs.Uv0)`.

// Moves a vertex, in the object's space, before it is drawn and before it
// casts its shadow. `inputs.Time` is the simulation's clock -- the one a script
// reads as `RunService.SimTime` -- so a script can know where a vertex went.
void surfaceVertex(inout SurfaceVertex vertex, SurfaceInputs inputs)
{
    vertex.Position += vertex.Normal * sin(inputs.Time * 3.0 + vertex.Position.y * 4.0) * Wobble * 0.05;
}

// Decides what the surface IS -- colour, alpha, metalness, roughness, normal,
// emission -- and never how it is lit. `surface` starts as the engine's
// defaults: white, opaque, roughness 0.7, the mesh's normal.
void surfaceFragment(SurfaceInputs inputs, inout SurfaceOutput surface)
{
    surface.BaseColor = Tint * inputs.VertexColor.rgb;
}
)hlsl";

} // namespace

std::string Editor::createSurfaceShader(std::string_view name)
{
    const std::string relative = normalizeShaderPath(name);
    if (relative.empty() || !sceneNameIsUsable(relative)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_not_a_name_2")), true};
        return {};
    }
    const std::filesystem::path absolute = m_content.root() / std::filesystem::path(relative);
    std::error_code ec;
    if (std::filesystem::exists(absolute, ec)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.something_is_already_called_that")), true};
        return {};
    }
    std::filesystem::create_directories(absolute.parent_path(), ec);
    if (!platform::writeTextFile(absolute, SurfaceShaderTemplate)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write_3"), {{"relative", relative}}), true};
        return {};
    }
    (void)m_content.refresh();
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.created"), {{"relative", relative}}), false};
    return relative;
}

std::string Editor::createMaterialVariant(std::string_view parent, std::string_view name)
{
    const std::string parentPath = normalizeMaterialPath(parent);
    std::string text;
    if (parentPath.empty() || !platform::readTextFile(m_content.root() / std::filesystem::path(parentPath), text) ||
        !asset::readMaterialAsset(text).has_value()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.there_is_no_material_to")), true};
        return {};
    }
    const std::string relative = normalizeMaterialPath(name);
    if (relative.empty() || !sceneNameIsUsable(relative) || relative == parentPath) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_not_a_name")), true};
        return {};
    }
    const std::filesystem::path absolute = m_content.root() / std::filesystem::path(relative);
    std::error_code ec;
    if (std::filesystem::exists(absolute, ec)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.something_is_already_called_that")), true};
        return {};
    }
    // A parent and nothing of its own: it looks exactly like the parent until
    // one field is changed, and follows every field it has not changed.
    asset::MaterialAsset variant;
    variant.parent = contentUrn(parentPath);
    if (!writeMaterialFile(absolute, variant)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write_3"), {{"relative", relative}}), true};
        return {};
    }
    (void)m_content.refresh();
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.created_a_variant_of"),
                                     {{"relative", relative}, {"parent_path", parentPath}}),
                            false};
    return relative;
}

std::string_view skyFaceOfName(std::string_view fileName) noexcept
{
    // The stem: no folders, no extension.
    if (const auto slash = fileName.find_last_of("/\\"); slash != std::string_view::npos)
        fileName.remove_prefix(slash + 1);
    if (const auto dot = fileName.find('.'); dot != std::string_view::npos)
        fileName = fileName.substr(0, dot);
    // Its last word, lower-cased: `Sunset_BK` and `sunset-back` say the same.
    const std::size_t end = fileName.size();
    std::size_t begin = end;
    while (begin > 0 && std::isalnum(static_cast<unsigned char>(fileName[begin - 1])) != 0)
        --begin;
    if (begin == end)
        return {};
    std::string word(fileName.substr(begin, end - begin));
    for (char& c : word)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    struct Spelling
    {
        std::string_view word;
        std::string_view property;
    };
    static constexpr std::array<Spelling, 22> kSpellings{{
        {"back", "SkyboxBack"},     {"bk", "SkyboxBack"},     {"pz", "SkyboxBack"},   {"down", "SkyboxDown"},
        {"dn", "SkyboxDown"},       {"bottom", "SkyboxDown"}, {"ny", "SkyboxDown"},   {"front", "SkyboxFront"},
        {"ft", "SkyboxFront"},      {"nz", "SkyboxFront"},    {"left", "SkyboxLeft"}, {"lf", "SkyboxLeft"},
        {"nx", "SkyboxLeft"},       {"right", "SkyboxRight"}, {"rt", "SkyboxRight"},  {"px", "SkyboxRight"},
        {"up", "SkyboxUp"},         {"top", "SkyboxUp"},      {"py", "SkyboxUp"},     {"bot", "SkyboxDown"},
        {"forward", "SkyboxFront"}, {"behind", "SkyboxBack"},
    }};
    for (const Spelling& spelling : kSpellings) {
        if (word == spelling.word)
            return spelling.property;
    }
    return {};
}

bool Editor::assignSkybox(scene::World& world, std::string_view path, core::InstanceId sky)
{
    if (!world.alive(sky) || world.destroyed(sky) || world.skies().find(sky) == nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.drop_sky_pictures_on_a")), true};
        return false;
    }

    // One picture, or every picture directly in the folder.
    std::vector<std::pair<std::string_view, std::string>> faces;
    const std::string folder(path);
    for (const ContentEntry& entry : m_content.entries()) {
        if (entry.kind != ContentKind::Texture)
            continue;
        const bool itself = entry.path == folder;
        const bool inside = entry.path.size() > folder.size() + 1 && entry.path.starts_with(folder) &&
                            entry.path[folder.size()] == '/' &&
                            entry.path.find('/', folder.size() + 1) == std::string::npos;
        if (!itself && !inside)
            continue;
        if (const std::string_view property = skyFaceOfName(entry.path); !property.empty())
            faces.emplace_back(property, std::string(asset::AssetScheme) + entry.path);
    }
    if (faces.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.no_picture_there_is_named")), true};
        return false;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.assign_sky_pictures")));
    for (const auto& [property, urn] : faces)
        (void)world.setProperty(sky, world.atoms().intern(property), scene::Value{urn});
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.status.faces_set"), {{"count", static_cast<core::i64>(faces.size())}}), false};
    return true;
}

bool Editor::assignMaterialTo(scene::World& world, std::string_view path, std::span<const core::InstanceId> targets)
{
    const std::string relative = normalizeMaterialPath(path);
    if (targets.empty() || relative.empty())
        return false;
    const scene::Value worn{scene::MaterialRef{contentUrn(relative), 0}};
    const core::NameAtom property = world.atoms().intern("Material");

    // **Nothing recorded when there is nothing to do** (D134): a drop onto
    // parts that all already wear this is a success with no step, because a
    // step that undoes nothing eats a press of ctrl-Z.
    core::usize live = 0;
    bool everyTargetAlready = true;
    for (const core::InstanceId target : targets) {
        if (!world.alive(target) || world.destroyed(target))
            continue;
        ++live;
        const std::optional<scene::Value> held = world.getProperty(target, property);
        if (!held.has_value() || !(*held == worn))
            everyTargetAlready = false;
    }
    if (live > 0 && everyTargetAlready) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.already"), {{"relative", relative}}), false};
        return true;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.assign_material")));
    core::usize written = 0;
    for (const core::InstanceId target : targets) {
        if (!world.alive(target) || world.destroyed(target))
            continue;
        const scene::World::SetResult wrote = world.setProperty(target, property, worn);
        if (wrote == scene::World::SetResult::Changed || wrote == scene::World::SetResult::Unchanged)
            ++written;
    }
    if (written == 0) {
        // Nothing selected is a part, so nothing changed and nothing is left
        // on the stack.
        (void)m_history.undo(world);
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_selected_takes_that")), true};
        return false;
    }
    touch();
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.assigned"), {{"relative", relative}}), false};
    return true;
}

bool Editor::openMaterial(std::string_view path)
{
    const std::string relative = normalizeMaterialPath(path);
    // **The one being edited is never thrown away by opening another**: its
    // edits are saved first, and if they cannot be it stays open. The same one
    // asked for again keeps its edits rather than re-reading the file.
    if (m_material.open() && m_material.dirty()) {
        if (m_material.path == relative)
            return true;
        const std::string previous = m_material.path;
        if (!saveMaterial()) {
            m_status = EditorStatus{
                core::tr(ENG_TR("engine.editor.status.could_not_save_so_it"), {{"previous", previous}}), true};
            return false;
        }
    }
    std::string text;
    if (relative.empty() || !platform::readTextFile(m_content.root() / std::filesystem::path(relative), text)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_read_2"), {{"relative", relative}}), true};
        return false;
    }
    std::string error;
    std::optional<asset::MaterialAsset> read = asset::readMaterialAsset(text, nullptr, &error);
    if (!read.has_value()) {
        m_status = EditorStatus{
            core::tr(ENG_TR("engine.editor.status.is_not_a_material"), {{"relative", relative}, {"error", error}}),
            true};
        return false;
    }
    closeMaterial();
    m_material.path = relative;
    m_material.asset = *read;
    m_material.saved = *read;
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.editing"), {{"relative", relative}}), false};
    return true;
}

void Editor::closeMaterial()
{
    if (!m_material.open())
        return;
    // Whatever the panel put in the library goes, so the next frame reads the
    // file as it stands -- which, for a session that was never saved, is the
    // material as it was before it was opened.
    if (m_materials != nullptr)
        m_materials->forget(contentUrn(m_material.path));
    m_material = MaterialSession{};
}

void Editor::editMaterial(const asset::MaterialAsset& next, bool continuing)
{
    if (!m_material.open() || next == m_material.asset)
        return;
    if (!continuing || m_material.undo.empty())
        m_material.undo.push_back(m_material.asset);
    m_material.redo.clear();
    m_material.asset = next;
    if (m_materials != nullptr)
        m_materials->put(contentUrn(m_material.path), m_material.asset);
}

bool Editor::undoMaterial()
{
    if (!m_material.open() || m_material.undo.empty())
        return false;
    m_material.redo.push_back(m_material.asset);
    m_material.asset = m_material.undo.back();
    m_material.undo.pop_back();
    if (m_materials != nullptr)
        m_materials->put(contentUrn(m_material.path), m_material.asset);
    return true;
}

bool Editor::redoMaterial()
{
    if (!m_material.open() || m_material.redo.empty())
        return false;
    m_material.undo.push_back(m_material.asset);
    m_material.asset = m_material.redo.back();
    m_material.redo.pop_back();
    if (m_materials != nullptr)
        m_materials->put(contentUrn(m_material.path), m_material.asset);
    return true;
}

bool Editor::saveMaterial()
{
    if (!m_material.open())
        return false;
    const std::filesystem::path absolute = m_content.root() / std::filesystem::path(m_material.path);
    if (!writeMaterialFile(absolute, m_material.asset)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write"), {{"path", m_material.path}}), true};
        return false;
    }
    m_material.saved = m_material.asset;
    // **Every open world updates on save** (ADR 0062): the library forgets
    // the file and reads it back, which is what a watcher's report would do a
    // moment later anyway -- and a variant of this material follows with it.
    if (m_materials != nullptr)
        m_materials->forget(contentUrn(m_material.path));
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.saved"), {{"path", m_material.path}}), false};
    return true;
}

std::vector<core::NameAtom> Editor::overridesOf(const scene::World& world, core::InstanceId id)
{
    if (!id.valid() || !world.alive(id))
        return {};

    // A library per call rather than one kept on the editor: it caches the
    // stamps it reads for as long as it lives, and a cache that outlives an
    // edit to the file it read is a panel showing yesterday's answer.
    scene::StampLibrary stamps(const_cast<scene::World&>(world), stampSource());
    return scene::stampOverrides(world, id, stamps);
}

bool Editor::revertOverride(scene::World& world, core::InstanceId id, core::NameAtom property)
{
    if (!id.valid() || !world.alive(id) || !property.valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.there_is_nothing_selected_to")), true};
        return false;
    }

    scene::StampLibrary stamps(world, stampSource());
    const std::optional<scene::Value> theirs = scene::stampReferenceValue(world, id, property, stamps);
    if (!theirs.has_value()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_not_part_of")), true};
        return false;
    }

    const std::string name(world.atoms().text(property));

    // **Asked before it is recorded.** A revert of a property that already
    // matches the file is a step that undoes nothing, and `UndoStack::record`
    // clears the redo stack -- so it would destroy a real redo future and leave
    // a junk one in its place (D134).
    const scene::PropertyDesc* descriptor = world.classes().findProperty(world.classOf(id), property);
    if (descriptor == nullptr || descriptor->get == nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.is_not_a_property_of"), {{"name", name}}), true};
        return false;
    }
    const std::optional<scene::Value> mine = descriptor->get(world, id);
    if (mine.has_value() && *mine == *theirs) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.already_matches_the_stamp"), {{"name", name}})};
        return true;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.revert"), {{"name", name}}));
    const scene::World::SetResult wrote = world.setProperty(id, property, *theirs);
    if (wrote != scene::World::SetResult::Changed && wrote != scene::World::SetResult::Unchanged) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_revert"), {{"name", name}}), true};
        return false;
    }
    touch();
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.reverted_to_the_stamp"), {{"name", name}})};
    return true;
}

bool Editor::applyOverride(scene::World& world, core::InstanceId gameRoot, core::InstanceId id, core::NameAtom property)
{
    if (!id.valid() || !world.alive(id) || !property.valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.there_is_nothing_selected_to_2")), true};
        return false;
    }

    // The stamp this instance belongs to, which is the file about to change.
    core::InstanceId stampRoot = id;
    core::NameAtom mark{};
    while (stampRoot.valid()) {
        mark = world.stampOf(stampRoot);
        if (mark.valid())
            break;
        stampRoot = world.parentOf(stampRoot);
    }
    if (!mark.valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_not_part_of_2")), true};
        return false;
    }
    const std::string path(world.atoms().text(mark));

    // **Refused while that stamp is open on the stage**, because then there are
    // two writers of one file and the one a person can see would lose. Said
    // rather than silently preferred: the stage is right there.
    if (m_stamp.open() && m_stamp.path == path) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.is_open_for_editing_apply"), {{"path", path}}), true};
        return false;
    }

    const std::string name(world.atoms().text(property));
    const scene::PropertyDesc* descriptor = world.classes().findProperty(world.classOf(id), property);
    if (descriptor == nullptr || descriptor->get == nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.is_not_a_property_of"), {{"name", name}}), true};
        return false;
    }
    const std::optional<scene::Value> mine = descriptor->get(world, id);
    if (!mine.has_value()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_read_3"), {{"name", name}}), true};
        return false;
    }
    // **A reference names an instance of THIS world**, and the file's scratch
    // copy has its own ids -- written there, it pointed at whatever held that
    // number (B14). Refused, and said.
    if (descriptor->type == scene::ValueType::Instance) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.points_at_an_instance_which"), {{"name", name}}), true};
        return false;
    }

    const std::optional<std::string> before = stampSource()(path);
    if (!before.has_value()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_read"), {{"path", path}}), true};
        return false;
    }

    // **Built into a scratch world of its own, edited there, and written back.**
    // The alternative -- editing the JSON text -- would be a second reader of
    // the stamp format, and `readSceneNode`'s own comment gives the reason that
    // is not worth having: one definition of what a stamp means.
    scene::World scratch(world.classes(), world.enums(), world.atoms(), 1u);
    const core::InstanceId scratchRoot = scene::readStamp(scratch, *before, core::InstanceId{}, path);
    if (!scratchRoot.valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_read"), {{"path", path}}), true};
        return false;
    }

    // The same walk down from each root, which is what pairs the live instance
    // with the one in the file.
    std::vector<core::u32> descent;
    for (core::InstanceId step = id; step != stampRoot; step = world.parentOf(step)) {
        const core::InstanceId parent = world.parentOf(step);
        if (!parent.valid())
            break;
        core::u32 index = 0;
        core::InstanceId child = world.firstChild(parent);
        while (child.valid() && child != step) {
            child = world.nextSibling(child);
            ++index;
        }
        descent.push_back(index);
    }
    core::InstanceId target = scratchRoot;
    for (auto step = descent.rbegin(); step != descent.rend(); ++step) {
        core::InstanceId child = scratch.firstChild(target);
        for (core::u32 skipped = 0; skipped < *step && child.valid(); ++skipped)
            child = scratch.nextSibling(child);
        if (!child.valid()) {
            m_status =
                EditorStatus{core::tr(ENG_TR("engine.editor.status.that_instance_is_not_in"), {{"path", path}}), true};
            return false;
        }
        target = child;
    }
    // The same position is the same instance only when it is the same class:
    // one whose stamp moved on structurally could otherwise write into a
    // different thing in the file (B14).
    // One registry for both worlds, so the class ids compare.
    if (scratch.classOf(target) != world.classOf(id)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.that_instance_is_not_in"), {{"path", path}}), true};
        return false;
    }

    const scene::World::SetResult intoStamp = scratch.setProperty(target, property, *mine);
    if (intoStamp != scene::World::SetResult::Changed && intoStamp != scene::World::SetResult::Unchanged) {
        m_status = EditorStatus{
            core::tr(ENG_TR("engine.editor.status.could_not_write_into"), {{"name", name}, {"path", path}}), true};
        return false;
    }

    scene::SceneIoReport wrote;
    const std::string after = scene::writeStamp(scratch, scratchRoot, &wrote);
    const std::filesystem::path absolute = m_content.root() / std::filesystem::path(path);
    if (!platform::createDirectories(absolute.parent_path()) || !platform::writeTextFileDurable(absolute, after)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write"), {{"path", path}}), true};
        return false;
    }

    // **Every linked instance follows, measured against the text they were
    // built from** -- the same arithmetic `saveStamp` does, and for the same
    // reason: an instance that overrode this property with some other value
    // differs from `before` and keeps what it has.
    scene::SceneIoReport moved;
    const core::u32 followed =
        gameRoot.valid() && world.alive(gameRoot) ? scene::restamp(world, gameRoot, path, *before, after, &moved) : 0u;
    // The children it replaced leave the pools now: a paused world runs no
    // drain to retire them, as `load` says (B5).
    if (followed > 0)
        world.retireDestroyed();
    if (m_stamp.open() && m_stamp.path == path)
        m_stamp.baseline = after;
    // What the instances are now built from, so the watcher seeing this very
    // write does not take it for somebody else's.
    (*m_stampTexts)[path] = after;
    if (followed > 0 || moved.unlinkedStamps > 0)
        m_sceneDirty = true;

    const auto item = [](const std::string& status, const std::string& note) {
        return core::tr(ENG_TR("engine.editor.status.with_item"), {{"status", status}, {"note", note}});
    };
    std::string message = core::tr(ENG_TR("engine.editor.status.applied_to"), {{"name", name}, {"path", path}});
    if (followed > 0) {
        message = item(message, core::tr(ENG_TR("engine.editor.status.in_the_world"),
                                         {{"count", static_cast<core::i64>(followed)}}));
    }
    if (moved.unlinkedStamps > 0) {
        message = item(message, core::tr(ENG_TR("engine.editor.status.left_alone"),
                                         {{"count", static_cast<core::i64>(moved.unlinkedStamps)}}));
    }
    m_status = EditorStatus{message};
    return true;
}

bool Editor::saveStamp(scene::World& game, core::InstanceId gameRoot)
{
    if (!m_stamp.open() || m_stage == nullptr || !m_stage->world().alive(m_stamp.root)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.there_is_no_stamp_open")), true};
        return false;
    }

    scene::SceneIoReport report;
    // **The stamps this stamp names, for the reason `save` gives about a scene**
    // (D133). A stamp can contain a stamped instance -- a lamp post inside a
    // street -- and without the library every one of them is written in full and
    // unlinked, so editing the lamp post stops reaching the street. `save` has
    // done this since ADR 0051 landed and this path never learned it.
    scene::StampLibrary stamps(m_stage->world(), stampSource());
    const std::string text = scene::writeStamp(m_stage->world(), m_stamp.root, &report, &stamps);
    const std::filesystem::path absolute = m_content.root() / std::filesystem::path(m_stamp.path);
    if (!platform::createDirectories(absolute.parent_path()) || !platform::writeTextFileDurable(absolute, text)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write"), {{"path", m_stamp.path}}), true};
        return false;
    }

    // **Every linked instance in the game's world follows the file** (ADR
    // 0051), measured against the text they were built from -- which is why the
    // session carries it. Done AFTER the write, so a save that could not reach
    // the disk does not move the world to match a file that is not there.
    //
    // Not an undo step, and that is deliberate: this is a change to a FILE, and
    // the history a person can undo here belongs to the stage. What the world
    // now holds is what the file says, which is the one thing an undo could not
    // put back.
    scene::SceneIoReport moved;
    const core::u32 followed =
        game.alive(gameRoot) ? scene::restamp(game, gameRoot, m_stamp.path, m_stamp.baseline, text, &moved) : 0u;
    if (followed > 0)
        game.retireDestroyed();
    m_stamp.baseline = text;
    (*m_stampTexts)[m_stamp.path] = text;

    // **Every instance it moved is a change to the SCENE, and the scene has to
    // know.** `restamp` rebuilds live instances in the game's world -- that is
    // the whole point of a linked stamp -- and nothing else marks it, because
    // the frame's own `touch()` attributes a mutation to whatever is open and
    // what is open is this stamp.
    //
    // Without this the sequence is silent and it loses work: edit a stamp, watch
    // every instance in the scene change, close the editor or start a new scene,
    // and be asked nothing -- because the scene believes it is clean.
    if (followed > 0 || moved.unlinkedStamps > 0)
        m_sceneDirty = true;

    m_stamp.dirty = false;
    const auto item = [](const std::string& status, const std::string& note) {
        return core::tr(ENG_TR("engine.editor.status.with_item"), {{"status", status}, {"note", note}});
    };
    std::string message = core::tr(ENG_TR("engine.editor.status.saved_stamp"),
                                   {{"path", m_stamp.path}, {"count", static_cast<core::i64>(report.instances)}});
    if (followed > 0) {
        message = item(message, core::tr(ENG_TR("engine.editor.status.in_the_world"),
                                         {{"count", static_cast<core::i64>(followed)}}));
    }
    // Said out loud rather than counted quietly: an instance somebody changed
    // structurally stops following its stamp, and finding that out by noticing
    // one lamp post did not move is how a person stops trusting the link.
    if (moved.unlinkedStamps > 0) {
        message = item(message, core::tr(ENG_TR("engine.editor.status.left_alone"),
                                         {{"count", static_cast<core::i64>(moved.unlinkedStamps)}}));
    }
    // **Counted rather than swallowed**, exactly as `save` counts a scene's
    // (D133). A stamp is written from its root DOWN, so a reference pointing at
    // anything outside that subtree cannot be carried: it came back as `null`
    // on the next open, silently, and `restamp` then pushed that null into
    // every instance in the world. A material is a URN now (ADR 0090) and never
    // arrives here; a constraint's attachment still can.
    if (report.droppedReferences > 0) {
        message = item(message, core::tr(ENG_TR("engine.editor.status.stamp_references_dropped"),
                                         {{"count", static_cast<core::i64>(report.droppedReferences)}}));
    }
    m_status = EditorStatus{message, report.droppedReferences > 0};
    return true;
}

bool Editor::closeStamp(scene::World& game, core::InstanceId gameRoot, Inspector& inspector, bool save)
{
    if (!m_stamp.open())
        return false;

    const std::string closed = m_stamp.path;
    const bool wrote = save && saveStamp(game, gameRoot);
    // **A save that failed keeps it open** (B9): closing anyway threw the edits
    // away behind a message that said it had closed -- on a read-only or locked
    // file, the one case where the edits exist nowhere else.
    if (save && !wrote) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_save_so_it_2"),
                                         {{"closed", closed}, {"message", m_status.value().message}}),
                                true};
        return false;
    }

    // The stage goes, and with it every instance in it. **The game's world was
    // never touched**, so there is nothing to restore and no snapshot to keep --
    // which is the whole reason a stage is a world of its own.
    m_stage.reset();
    m_stamp = StampSession{};

    m_history.clear();
    inspector.onWorldChanged();

    m_status = EditorStatus{
        core::tr(wrote ? ENG_TR("engine.editor.status.saved_and_closed") : ENG_TR("engine.editor.status.closed"),
                 {{"name", closed}}),
        false};
    return true;
}

std::string Editor::createStampOfClass(scene::World& world, core::InstanceId root, scene::ClassId classId,
                                       std::string_view name)
{
    if (!stampNameIsUsable(name))
        return {};

    const scene::ClassDescriptor* descriptor = world.classes().find(classId);
    if (descriptor == nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.no_such_class")), true};
        return {};
    }

    // Refused before anything is made, so a name that is taken does not leave an
    // orphan instance in the world with no file behind it.
    const std::string relative = normalizeStampPath(name);
    std::error_code ec;
    if (std::filesystem::exists(m_content.root() / std::filesystem::path(relative), ec)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.something_is_already_called_that")), true};
        return {};
    }

    // Recorded BEFORE the instance exists, so one undo takes both back.
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.new_stamp")));

    const core::InstanceId made = world.create(classId);
    // Named after the file, minus its folders and its suffix -- which is what
    // somebody typed and what they will look for in the Explorer.
    std::string stem = relative;
    if (const std::string::size_type slash = stem.rfind('/'); slash != std::string::npos)
        stem = stem.substr(slash + 1);
    if (const std::string::size_type dot = stem.find('.'); dot != std::string::npos)
        stem = stem.substr(0, dot);
    world.setName(made, world.atoms().intern(stem));
    if (world.setParent(made, root).has_value()) {
        (void)m_history.undo(world);
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_authored_can_live_in")), true};
        return {};
    }

    // One step to undo, the one recorded above (B8): the mark is part of it.
    m_stampRecorded = true;
    const bool stamped = createStamp(world, made, root, name);
    m_stampRecorded = false;
    if (!stamped) {
        // `createStamp` said why. Taking the instance back with it, because an
        // instance in the world that nothing wrote is not what was asked for.
        (void)m_history.undo(world);
        return {};
    }

    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.created"), {{"relative", relative}}), false};
    return relative;
}

bool Editor::createStamp(scene::World& world, core::InstanceId id, core::InstanceId root, std::string_view name)
{
    if (!world.alive(id)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_to_make_a_stamp")), true};
        return false;
    }
    if (!stampNameIsUsable(name)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_not_a_usable")), true};
        return false;
    }
    // Engine-owned, or inside something a system made: neither is somebody's
    // authored work, and a stamp of a streamed chunk is a recording of where
    // streaming happened to be.
    if (isEngineOwned(world, id, root) || !canParentInto(world, id, root)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_not_something_a")), true};
        return false;
    }

    // **A stamp of a stamp is refused rather than half-answered** (ADR 0049).
    // Does the outer file record the inner link? Does breaking the outer break
    // the inner? Those are real questions with no answer yet, and a format that
    // silently picked one would be a format somebody depends on before anybody
    // decides.
    for (core::InstanceId child = world.firstChild(id); child.valid();) {
        if (world.stampOf(child).valid()) {
            m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_already_contains_a_stamped")), true};
            return false;
        }
        if (const core::InstanceId inner = world.firstChild(child); inner.valid()) {
            child = inner;
            continue;
        }
        while (child.valid() && child != id && !world.nextSibling(child).valid())
            child = world.parentOf(child);
        child = child == id ? core::InstanceId{} : world.nextSibling(child);
    }

    // **Nor from code that is files** (the audit of 2026-09-28): the stamp
    // would carry the scripts and `src/` would mount them again beside it --
    // two of each at the next open.
    {
        std::vector<core::InstanceId> subtree{id};
        world.collectDescendants(id, subtree);
        for (const core::InstanceId each : subtree) {
            if (world.mounted(each)) {
                m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_holds_scripts_that_are")), true};
                return false;
            }
        }
    }

    // Nor inside one: the outer file owns what is under it, and a mark in
    // there is lost the next time it is saved (B6).
    for (core::InstanceId above = world.parentOf(id); above.valid(); above = world.parentOf(above)) {
        if (world.stampOf(above).valid()) {
            m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_is_inside_a_stamped")), true};
            return false;
        }
    }

    const std::string relative = normalizeStampPath(name);
    const std::filesystem::path absolute = m_content.root() / std::filesystem::path(relative);
    // **Never over a stamp that exists** (B7): the instances of that one would
    // be measured against a file they were never made from.
    std::error_code taken;
    if (std::filesystem::exists(absolute, taken)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.a_stamp_is_already_called")), true};
        return false;
    }
    if (!platform::createDirectories(absolute.parent_path())) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_make_the_folder")), true};
        return false;
    }

    scene::SceneIoReport report;
    // In full: a stamp is a file of its own, and a script from `src/` in it is
    // code the stamp has to carry.
    const std::string text = scene::writeCopy(world, id, &report);
    if (!platform::writeTextFileDurable(absolute, text)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write_that_stamp")), true};
        return false;
    }

    // **And the thing it was made from becomes an instance of it.** A file plus
    // a copy of it that nothing connects is two things that drift apart by
    // tomorrow, which is the state this whole model exists to avoid.
    if (!m_stampRecorded)
        m_history.record(world, core::tr(ENG_TR("engine.editor.history.create_stamp")));
    world.setStamp(id, world.atoms().intern(relative));
    // The world changed: the scene has something to save (B8).
    touch();

    (void)m_content.refresh();
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.stamped_instance_s_into"),
                                     {{"count", static_cast<core::i64>(report.instances)}, {"relative", relative}}),
                            false};
    return true;
}

namespace {
// **A `Model` is moved by its PIVOT, because a pivot is the only handle it
// has.** `Model` declares no `CFrame` property at all, so a placement that
// wrote one wrote nothing: the class refused it, the refusal was a return value
// nobody read, and the subtree stayed at the coordinates its file records --
// which for anything authored near where it was built is the world origin.
// `Part` does have a `CFrame`, and a `Part` root is the case that got tried.
//
// This is `PivotTo` (`instance_binding.cpp`) reached without a VM, off the same
// `scene::pivotOf`. `pivot.h` was lifted out of the binding precisely so that
// "where is the middle of this model" has one answer below `script`, and it
// names an editor gizmo as a caller; nothing in `engine/app` had asked it yet.
// The rule about what travels comes with it: a model moves every part under it,
// which is what keeps the layout somebody built, and a part moves alone,
// because what hangs off a part is welds and constraints rather than geometry.
void pivotTo(scene::World& world, core::InstanceId id, const core::CFrameD& target)
{
    // `delta` puts the pivot on the target, and everything the object owns moves
    // by that same transform -- which is what preserves relative layout.
    const core::CFrameD delta = target * core::inverse(scene::pivotOf(world, id));
    const core::NameAtom cframeProperty = world.atoms().intern("CFrame");

    if (world.models().find(id) != nullptr) {
        std::vector<core::InstanceId> descendants;
        world.collectDescendants(id, descendants);
        for (const core::InstanceId descendant : descendants) {
            const scene::PartComponent* part = world.parts().find(descendant);
            if (part == nullptr)
                continue;
            // Through `setProperty` rather than into the component, so the
            // renderer and anything watching `CFrame` see it by the path they
            // already have. The component is what says the write can land, so
            // the result answers nothing this has not already asked.
            world.setProperty(descendant, cframeProperty, scene::Value{delta * part->cframe});
        }
        return;
    }

    if (const scene::PartComponent* part = world.parts().find(id); part != nullptr) {
        world.setProperty(id, cframeProperty, scene::Value{delta * part->cframe});
        return;
    }

    if (const scene::CameraComponent* camera = world.cameras().find(id); camera != nullptr)
        world.setProperty(id, cframeProperty, scene::Value{delta * camera->cframe});
}
} // namespace

bool Editor::instantiateStamp(scene::World& world, std::string_view name, core::InstanceId parent,
                              core::InstanceId root, Inspector& inspector, bool linked)
{
    if (!canParentInto(world, parent, root)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_authored_can_live_in")), true};
        return false;
    }
    // **Not into a stamp being edited** (B6): a stamp inside a stamp is a
    // question the format has not answered, and the inner one was lost on the
    // next save of the outer.
    if (linked && m_stage != nullptr && &world == &m_stage->world()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.a_stamp_cannot_hold_another")), true};
        return false;
    }

    const std::string relative = normalizeStampPath(name);
    std::string text;
    if (!platform::readTextFile(m_content.root() / std::filesystem::path(relative), text)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_stamp_is_not_there")), true};
        return false;
    }

    // Recorded BEFORE the instance exists, so one undo takes the whole subtree
    // back -- which is what `WorldSnapshot` makes cheap and what a
    // reversible-command design would have made hard (`editor.h` says why).
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.stamp")));

    scene::SceneIoReport report;
    const core::InstanceId placed = scene::readStamp(world, text, parent, relative, &report);
    if (!placed.valid()) {
        // Nothing usable was built, so the step is taken back rather than
        // left: a step that undoes nothing eats a press of ctrl-Z, and undoing
        // it also removes whatever partial subtree the read managed before it
        // gave up.
        (void)m_history.undo(world);
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_stamp_could_not_be")), true};
        return false;
    }
    adoptServiceSide(world, placed);

    // In front of the camera rather than at the origin, for the reason
    // `createInstance` places a new part there: something four kilometres from
    // the view is something nobody finds.
    //
    // **Through the pivot**, because the root of a stamp is a `Model` as often
    // as not -- grouping parts is what produces one -- and a `Model` has no
    // `CFrame` to write. See `pivotTo` above for what that cost.
    if (m_cameraAdopted) {
        const core::Mat3& basis = m_cameraCFrame.rotation;
        const Vec3 forward{-basis.m[2][0], -basis.m[2][1], -basis.m[2][2]};
        constexpr f32 kSpawnDistance = 8.0f;
        core::CFrameD spawn;
        spawn.position = m_cameraCFrame.position + core::toDVec3(forward * kSpawnDistance);
        pivotTo(world, placed, spawn);
    }

    // **A copy is a placement that forgets where it came from.** Same subtree,
    // no mark -- so it is written in full, and nothing that happens to the
    // stamp reaches it again.
    if (!linked)
        world.setStamp(placed, core::NameAtom{});

    inspector.select(placed);
    inspector.reveal(placed);
    m_status = EditorStatus{(linked ? "stamped " : "copied ") + relative, false};
    return true;
}

bool Editor::assignStampTo(scene::World& world, core::InstanceId root, core::InstanceId parent, std::string_view path,
                           std::string_view property, std::span<const core::InstanceId> targets)
{
    if (targets.empty() || property.empty())
        return false;

    const std::string relative = normalizeStampPath(path);
    const core::NameAtom mark = world.atoms().intern(relative);
    const core::NameAtom field = world.atoms().intern(property);

    // The one already in the world wins. Document order, so which one is found
    // is a fact about the tree rather than about pool layout -- and so two runs
    // of the same gesture on the same scene agree.
    //
    // **Searched BEFORE anything is recorded** (D134), because the search does
    // not mutate and because of what comes next: dropping the same material on
    // a part that already wears it must not record a step. `UndoStack::record`
    // clears the redo stack, so a step taken back afterwards has already
    // destroyed a real redo future and leaves a junk one behind -- the file's
    // own invariant is "a step that undoes nothing eats a press of ctrl-Z", and
    // this was the hole left in it.
    core::InstanceId subject;
    static thread_local std::vector<TreeRow> rows;
    collectTree(world, root, rows);
    for (const TreeRow& row : rows) {
        if (world.stampOf(row.id) == mark && !world.destroyed(row.id)) {
            subject = row.id;
            break;
        }
    }

    if (subject.valid()) {
        bool everyTargetAlready = true;
        core::usize live = 0;
        for (const core::InstanceId target : targets) {
            if (!world.alive(target) || world.destroyed(target))
                continue;
            ++live;
            const std::optional<scene::Value> held = world.getProperty(target, field);
            if (!held.has_value() || !std::holds_alternative<core::InstanceId>(*held) ||
                std::get<core::InstanceId>(*held) != subject) {
                everyTargetAlready = false;
                break;
            }
        }
        if (live > 0 && everyTargetAlready) {
            // Nothing to do, and saying so is the honest answer: the part is
            // wearing what was dropped on it.
            m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.already"), {{"relative", relative}}), false};
            return true;
        }
    }

    // **Recorded before the world is touched**, so the placement and every write
    // it enables are one press of ctrl-Z. Two steps would mean undoing a drop
    // left a material in the world that nothing points at.
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.assign")));

    if (!subject.valid()) {
        std::string text;
        if (!platform::readTextFile(m_content.root() / std::filesystem::path(relative), text)) {
            (void)m_history.undo(world);
            m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_stamp_is_not_there")), true};
            return false;
        }

        scene::SceneIoReport report;
        subject = scene::readStamp(world, text, parent, relative, &report);
        if (!subject.valid()) {
            (void)m_history.undo(world);
            m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_stamp_could_not_be")), true};
            return false;
        }
        adoptServiceSide(world, subject);
    }

    // **Not selected and not revealed.** Somebody dropping a material on a part
    // is looking at the part; replacing their selection with the thing they
    // dragged would take away what they were working on. Placing a stamp INTO
    // the world is a different gesture and does select, which is why this does
    // not share `instantiateStamp`.
    core::usize written = 0;
    for (const core::InstanceId target : targets) {
        if (!world.alive(target) || world.destroyed(target))
            continue;
        // `Unchanged` counts: a part that already pointed at this material was
        // asked for the same thing and got it. Treating it as a failure would
        // make dropping one material on two parts report a refusal because one
        // of them was already right.
        const scene::World::SetResult wrote = world.setProperty(target, field, scene::Value{subject});
        if (wrote == scene::World::SetResult::Changed || wrote == scene::World::SetResult::Unchanged)
            ++written;
    }

    if (written == 0) {
        // The class does not take it, or the setter refused the class of what
        // was dropped. Nothing changed, so nothing is left on the stack.
        (void)m_history.undo(world);
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_selected_takes_that")), true};
        return false;
    }

    m_sceneDirty = true;
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.assigned"), {{"relative", relative}}), false};
    return true;
}

bool Editor::reparent(scene::World& world, std::span<const core::InstanceId> ids, core::InstanceId newParent,
                      core::InstanceId root, Inspector& inspector, std::optional<core::u32> at)
{
    if (ids.empty())
        return false;

    // **Decided before anything is recorded**, so a drag that cannot move
    // anything leaves no undo step behind. A step that undoes nothing is worse
    // than no step: it eats a press of ctrl-Z and the second press takes back
    // something the person had stopped thinking about.
    const ReparentPlan plan = planReparent(world, ids, newParent, root);
    if (plan.targetRefuses) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_authored_can_live_in_2")), true};
        return false;
    }
    if (plan.movable.empty()) {
        m_status =
            EditorStatus{core::tr(plan.refused > 0 ? ENG_TR("engine.editor.status.nothing_can_be_moved_into_that")
                                                   : ENG_TR("engine.editor.status.already_there")),
                         plan.refused > 0};
        return false;
    }

    core::usize refused = plan.refused;
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.reparent_count"),
                                     {{"count", static_cast<core::i64>(plan.movable.size())}}));
    for (const core::InstanceId id : plan.movable) {
        if (world.setParent(id, newParent).has_value())
            ++refused;
        else
            adoptServiceSide(world, id);
    }
    if (at.has_value() && plan.movable.size() == 1 && world.parentOf(plan.movable.front()) == newParent)
        (void)world.moveChild(newParent, plan.movable.front(), *at);

    inspector.pruneDead(world);
    inspector.onWorldRestored();
    // **Where it went, opened.** Dropping something into a collapsed folder and
    // watching it disappear is the same defect creating one inside an empty one
    // was, arriving through the other verb.
    inspector.reveal(plan.movable.front());

    std::string message = "moved " + std::to_string(plan.movable.size());
    if (refused > 0)
        message += ", refused " + std::to_string(refused);
    m_status = EditorStatus{message, false};
    return true;
}

bool Editor::reorder(scene::World& world, core::InstanceId child, core::u32 index, Inspector& inspector)
{
    const core::InstanceId parent = world.parentOf(child);
    if (!parent.valid())
        return false;

    // **Decided before anything is recorded**, the same rule `reparent` above
    // states: a drag that moves nothing must leave no undo step behind, because
    // a step that undoes nothing eats a press of ctrl-Z and clears the redo
    // stack on its way past (D134). `moveChild` would answer `Unchanged` and
    // there is no way to un-record afterwards, so the question is asked first.
    //
    // One walk answers both halves of it -- where the child stands now, and how
    // many places there are to stand -- and it is the same walk `moveChild`
    // makes, on a gesture nobody performs in a loop.
    core::u32 at = 0;
    core::u32 count = 0;
    bool found = false;
    for (core::InstanceId sibling = world.firstChild(parent); sibling.valid(); sibling = world.nextSibling(sibling)) {
        if (sibling == child) {
            at = count;
            found = true;
        }
        ++count;
    }
    if (!found)
        return false;

    if (index >= count) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.cannot_move_there")), true};
        return false;
    }
    if (index == at) {
        // Not an error. Dropping a row back where it started is a person
        // changing their mind, and a red status line for it would be the tool
        // scolding somebody for a gesture it invited.
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.already_there")), false};
        return false;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.reorder")));
    if (world.moveChild(parent, child, index) != scene::World::MoveResult::Moved)
        return false;

    // The stamp's, when a stamp is open: reordering inside it is an edit to
    // the stamp and not to the scene behind it (B15).
    touch();
    inspector.reveal(child);
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.reordered")), false};
    return true;
}

bool Editor::deleteInstances(scene::World& world, std::span<const core::InstanceId> ids, core::InstanceId root,
                             Inspector& inspector)
{
    if (ids.empty())
        return false;
    if (ids.size() == 1)
        return deleteInstance(world, ids[0], root, inspector);

    std::vector<core::InstanceId> ordered;
    orderByTree(world, root, ids, ordered);

    std::vector<core::InstanceId> removable;
    for (const core::InstanceId id : ordered) {
        if (!isEngineOwned(world, id, root))
            removable.push_back(id);
    }

    if (removable.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.that_one_belongs_to_the")), true};
        return false;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.delete_instances"),
                                     {{"count", static_cast<core::i64>(removable.size())}}));

    core::usize removed = 0;
    for (const core::InstanceId id : removable) {
        // A parent destroyed earlier in the walk took its children with it, so
        // a child named separately is already gone -- and that is not an error.
        // Selecting a parent and its child and pressing delete means both, and
        // both is what happened.
        if (world.alive(id) && world.destroy(id))
            ++removed;
    }

    // A paused world runs no signal drain, so nothing else retires these and
    // they would go on answering `alive` -- the same reason `deleteInstance`
    // calls it.
    world.retireDestroyed();

    inspector.pruneDead(world);
    inspector.onWorldRestored();
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.status.deleted_instance_s"), {{"count", static_cast<core::i64>(removed)}}),
        false};
    return true;
}

namespace {

// The shallowest instance that is an ancestor of, or equal to, every id.
//
// **Where a group goes.** Grouping four things from two branches has to put the
// container somewhere both of them can reach, and that is their common ancestor
// -- picking the first one's parent would silently move the other three into a
// branch nobody asked about.
[[nodiscard]] core::InstanceId commonParent(const scene::World& world, std::span<const core::InstanceId> ids,
                                            core::InstanceId root)
{
    core::InstanceId shared = core::InstanceId{};
    for (const core::InstanceId id : ids) {
        const core::InstanceId parent = world.parentOf(id);
        if (!parent.valid())
            continue;
        if (!shared.valid()) {
            shared = parent;
            continue;
        }
        if (shared == parent)
            continue;

        // Walk `shared` up until it covers `parent` too. Bounded by the tree's
        // depth, and `root` is the backstop for two branches that meet nowhere
        // -- which a world with more than one top-level tree can produce.
        core::InstanceId walk = shared;
        while (walk.valid() && !world.isAncestorOf(walk, parent))
            walk = world.parentOf(walk);
        shared = walk.valid() ? walk : root;
    }
    return shared;
}

} // namespace

bool Editor::groupSelection(scene::World& world, std::span<const core::InstanceId> ids, core::InstanceId root,
                            Inspector& inspector, bool asFolder)
{
    if (ids.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.select_something_to_group")), true};
        return false;
    }

    std::vector<core::InstanceId> ordered;
    orderByTree(world, root, ids, ordered);

    std::vector<core::InstanceId> movable;
    bool wantsModel = false;
    for (const core::InstanceId id : ordered) {
        if (!world.alive(id) || id == root || isEngineOwned(world, id, root))
            continue;
        // **A `Model` when anything has a transform.** A model has a pivot,
        // extents and a scale, all meaningless around four scripts -- and a
        // folder around four parts throws away the one thing grouping parts is
        // for.
        wantsModel = wantsModel || world.parts().find(id) != nullptr || world.models().find(id) != nullptr;
        movable.push_back(id);
    }
    // Asked for by name, a folder is what it is: somebody tidying four parts
    // into a folder is organising the tree, not making something to move.
    if (asFolder)
        wantsModel = false;

    if (movable.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_there_can_be_grouped")), true};
        return false;
    }

    const scene::ClassId containerClass = world.classes().findId(world.atoms().intern(wantsModel ? "Model" : "Folder"));
    if (containerClass == scene::InvalidClass) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.this_build_has_no_class")), true};
        return false;
    }

    const core::InstanceId parent = commonParent(world, movable, root);

    // **Recorded before the create**, so one ctrl-Z takes the whole group back
    // rather than leaving an empty container behind.
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.group_count"),
                                     {{"count", static_cast<core::i64>(movable.size())}}));

    const core::InstanceId container = world.create(containerClass);
    if (!container.valid()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.this_build_has_no_class")), true};
        return false;
    }
    world.setName(container, world.atoms().intern(wantsModel ? "Model" : "Folder"));
    (void)world.setParent(container, parent.valid() ? parent : root);

    core::usize moved = 0;
    for (const core::InstanceId id : movable) {
        // The container cannot be moved into itself, and neither can anything
        // ABOVE it -- which `commonParent` makes impossible by construction and
        // `setParent` refuses anyway. Counted rather than assumed.
        if (!world.setParent(id, container).has_value())
            ++moved;
    }

    inspector.pruneDead(world);
    inspector.onWorldRestored();
    // **The container, selected.** Grouping is a thing you do in order to then
    // move the group, so leaving the children selected would mean the next drag
    // undoes the reason you grouped them.
    inspector.select(container);
    inspector.reveal(container);
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.status.grouped_instance_s"), {{"count", static_cast<core::i64>(moved)}}), false};
    return true;
}

bool Editor::ungroupSelection(scene::World& world, std::span<const core::InstanceId> ids, core::InstanceId root,
                              Inspector& inspector)
{
    if (ids.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.select_a_group_to_take")), true};
        return false;
    }

    std::vector<core::InstanceId> containers;
    for (const core::InstanceId id : ids) {
        // **Only something with children.** Ungrouping a part is not a thing,
        // and a verb that silently destroyed one would be the worst possible
        // reading of a key nobody meant to press.
        if (world.alive(id) && !isEngineOwned(world, id, root) && world.firstChild(id).valid())
            containers.push_back(id);
    }

    if (containers.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_selected_has_anything_in")), true};
        return false;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.ungroup_count"),
                                     {{"count", static_cast<core::i64>(containers.size())}}));

    std::vector<core::InstanceId> freed;
    for (const core::InstanceId container : containers) {
        const core::InstanceId parent = world.parentOf(container);

        // **Collected before any of them moves.** `firstChild`/`nextSibling` is
        // a live list, and reparenting while walking it drops every child after
        // the first -- which is the shape of bug that leaves three of five in a
        // container the editor then destroys.
        std::vector<core::InstanceId> children;
        for (core::InstanceId child = world.firstChild(container); child.valid(); child = world.nextSibling(child)) {
            children.push_back(child);
        }

        for (const core::InstanceId child : children) {
            if (!world.setParent(child, parent.valid() ? parent : root).has_value())
                freed.push_back(child);
        }

        // Only once it is empty. A container that kept a child the tree refused
        // to move is a container that still holds something, and destroying it
        // would take that something with it.
        if (!world.firstChild(container).valid())
            (void)world.destroy(container);
    }

    world.retireDestroyed();
    inspector.pruneDead(world);
    inspector.onWorldRestored();

    // What came out, selected -- because that is what somebody is now looking at
    // and what they are about to move.
    inspector.select(freed);
    if (!freed.empty())
        inspector.reveal(freed.front());

    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.status.took_out_instance_s"), {{"count", static_cast<core::i64>(freed.size())}}),
        false};
    return true;
}

bool Editor::duplicateInstances(scene::World& world, std::span<const core::InstanceId> ids, core::InstanceId root,
                                Inspector& inspector)
{
    if (ids.empty())
        return false;
    if (ids.size() == 1)
        return duplicateInstance(world, ids[0], root, inspector);

    std::vector<core::InstanceId> ordered;
    orderByTree(world, root, ids, ordered);

    std::vector<core::InstanceId> copyable;
    for (const core::InstanceId id : ordered) {
        if (!isEngineOwned(world, id, root) && world.parentOf(id).valid())
            copyable.push_back(id);
    }

    if (copyable.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.nothing_there_can_be_duplicated")), true};
        return false;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.duplicate_instances"),
                                     {{"count", static_cast<core::i64>(copyable.size())}}));

    std::vector<core::InstanceId> copies;
    for (const core::InstanceId id : copyable) {
        const core::InstanceId copy = world.clone(id);
        if (!copy.valid())
            continue;
        (void)world.setParent(copy, world.parentOf(id));
        copies.push_back(copy);
    }

    // The copies, not the originals: the point of duplicating is to change what
    // came out, and a selection left on the source is a second click before
    // anything can be done to it.
    inspector.select(copies);
    if (!copies.empty())
        inspector.reveal(copies.front());
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.duplicated_instance_s"),
                                     {{"count", static_cast<core::i64>(copies.size())}}),
                            false};
    return true;
}

bool Editor::renameInstance(scene::World& world, core::InstanceId id, core::InstanceId root, std::string_view name)
{
    if (!world.alive(id) || name.empty())
        return false;

    if (isEngineOwned(world, id, root)) {
        // **A script reaches a service by NAME** -- `game.Workspace` is a
        // lookup, not a keyword -- so renaming one breaks every line that does
        // it, in files nothing here can see.
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.a_service_s_name_is")), true};
        return false;
    }

    m_history.record(world,
                     core::tr(ENG_TR("engine.editor.history.rename"), {{"name", world.atoms().text(world.name(id))}}));
    world.setName(id, world.atoms().intern(name));
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.renamed_to"), {{"name", name}}), false};
    return true;
}

void Editor::newScene(scene::World& world, Inspector& inspector)
{
    scene::clearScene(world);
    // See `load`: `clearScene` destroys, and a paused world never drains, so
    // without this the old scene lives on in the pools as unparented husks.
    world.retireDestroyed();

    // Undoing into a world that no longer exists is not undoing.
    m_history.clear();
    m_openScene.clear();
    // A scene nobody has touched yet. Whoever asked for this was asked about the
    // old one first, if there was anything to ask about.
    m_sceneDirty = false;
    inspector.select(core::InstanceId{});
    inspector.onWorldChanged();
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.new_scene_untitled_until_you")), false};
}

std::string Editor::normalizeScenePath(std::string_view typed)
{
    std::string path(typed);

    // Typed by a person into a box the dialog has already labelled `content/`,
    // so the prefix is the natural thing to type and the wrong thing to keep:
    // it resolves against the content root and makes `content/content/`. This
    // engine met that on its first real use (D068) and it left a scene nobody
    // could open beside one nobody meant to save.
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    constexpr std::string_view kContentPrefix = "content/";
    while (path.compare(0, kContentPrefix.size(), kContentPrefix) == 0)
        path.erase(0, kContentPrefix.size());

    if (path.size() < kSceneExtension.size() ||
        path.compare(path.size() - kSceneExtension.size(), kSceneExtension.size(), kSceneExtension) != 0) {
        path += kSceneExtension;
    }
    return path;
}

bool Editor::sceneNameIsUsable(std::string_view typed) noexcept
{
    // A drive letter or a leading slash is a path out of the project, and `..`
    // is the same thing spelled to look like a name. Every segment has to be a
    // name the browser could have shown, which is the rule the New Folder box
    // already applies -- applied here too, because a Save box that accepts what
    // a New Folder box refuses is one rule with two answers.
    if (typed.empty() || typed.find(':') != std::string_view::npos)
        return false;

    std::size_t begin = 0;
    while (begin <= typed.size()) {
        const std::size_t end = typed.find('/', begin);
        const std::string_view segment = typed.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (!ContentTree::isUsableName(segment))
            return false;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return true;
}

bool Editor::saveSceneAs(scene::World& world, std::string_view relativePath)
{
    const std::string path = normalizeScenePath(relativePath);
    if (!sceneNameIsUsable(path)) {
        m_status = EditorStatus{
            core::tr(ENG_TR("engine.editor.status.is_not_a_path_inside"), {{"relative_path", relativePath}}), true};
        return false;
    }
    // **Never over another scene.** Every other create path refuses a name
    // that is taken; this one wrote the other scene, its terrain's cells and
    // its scripts' folder over without a word.
    std::error_code taken;
    if (path != m_openScene && std::filesystem::exists(m_content.root() / std::filesystem::path(path), taken)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.a_scene_is_already_called")), true};
        return false;
    }

    if (!save(world, m_content.root() / std::filesystem::path(path)))
        return false;

    m_openScene = path;
    // The browser is showing the folder this was written into, and it does not
    // know a file appeared in it.
    (void)m_content.refresh();
    return true;
}

void Editor::adoptOpenScene(std::string_view relativePath)
{
    // The boot load already put a scene in the world; this is the editor being
    // told WHICH, so the first save writes back to it rather than refusing for
    // want of an open scene. Naming it here rather than re-loading it is the
    // difference between the editor knowing what it has and the editor
    // discarding a world to find out.
    m_openScene = std::string(relativePath);
}

void Editor::openContent(const std::filesystem::path& contentRoot)
{
    // The return value is deliberately ignored. A project with no `content/`
    // is every example before `06-scene`, and a browser that greeted those with
    // an error would be wrong about all of them.
    (void)m_content.open(contentRoot);
}

bool Editor::openScene(scene::World& world, std::string_view relativePath, Inspector& inspector)
{
    const std::filesystem::path absolute = m_content.root() / std::filesystem::path(relativePath);
    if (!load(world, absolute, inspector))
        return false;

    m_openScene = std::string(relativePath);
    m_history.clear();
    m_sceneDirty = false;
    // The status `load` set names the file; naming the scene is more useful,
    // because the browser is already showing the file.
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.opened"), {{"open_scene", m_openScene}}), false};
    return true;
}

bool Editor::saveOpenScene(scene::World& world)
{
    if (m_openScene.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.no_scene_is_open_open")), true};
        return false;
    }
    return save(world, m_content.root() / std::filesystem::path(m_openScene));
}

namespace {

// One step down a tree: a class, a name, and which of the siblings with both
// it is -- a scene full of `Ground` is legal, and the third `Ground` is the
// third in any world built from the same file.
struct PathStep
{
    scene::ClassId classId = scene::InvalidClass;
    core::NameAtom name;
    core::u32 ordinal = 0;
};

[[nodiscard]] core::InstanceId topOf(const scene::World& world, core::InstanceId id)
{
    while (world.parentOf(id).valid())
        id = world.parentOf(id);
    return id;
}

[[nodiscard]] std::vector<PathStep> pathOf(const scene::World& world, core::InstanceId id)
{
    std::vector<PathStep> path;
    for (core::InstanceId at = id; world.parentOf(at).valid(); at = world.parentOf(at)) {
        PathStep step{world.classOf(at), world.name(at), 0};
        for (core::InstanceId sibling = world.firstChild(world.parentOf(at)); sibling != at;
             sibling = world.nextSibling(sibling)) {
            if (world.classOf(sibling) == step.classId && world.name(sibling) == step.name)
                ++step.ordinal;
        }
        path.push_back(step);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

[[nodiscard]] core::InstanceId follow(const scene::World& world, core::InstanceId top,
                                      const std::vector<PathStep>& path)
{
    core::InstanceId at = top;
    for (const PathStep& step : path) {
        core::u32 seen = 0;
        core::InstanceId found;
        for (core::InstanceId child = world.firstChild(at); child.valid(); child = world.nextSibling(child)) {
            if (world.classOf(child) != step.classId || world.name(child) != step.name)
                continue;
            if (seen++ == step.ordinal) {
                found = child;
                break;
            }
        }
        if (!found.valid())
            return {};
        at = found;
    }
    return at;
}

} // namespace

Editor::ScriptSave Editor::saveSceneScript(scene::World& world, core::InstanceId script)
{
    // **The whole scene, and why**, whenever saving one script cannot be done
    // honestly. Said, because "Ctrl+S saved everything" is what was reported.
    const auto whole = [&](const std::string& why) {
        if (!saveOpenScene(world))
            return ScriptSave::Failed;
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.saved_the_whole_scene"), {{"why", why}}), false};
        return ScriptSave::Scene;
    };

    if (m_openScene.empty() || !world.alive(script))
        return saveOpenScene(world) ? ScriptSave::Scene : ScriptSave::Failed;

    // **Under a script service, a script is a file** (ADR 0105), written by the
    // save that keeps `src/` in step with the tree -- one pasted or moved there
    // has no place in the scene file to be patched into.
    for (core::InstanceId walk = world.parentOf(script); walk.valid(); walk = world.parentOf(walk)) {
        if (isClass(world, walk, "GlobalScriptService") || isClass(world, walk, "ServerScriptService") ||
            isClass(world, walk, "ClientScriptService"))
            return whole(core::tr(ENG_TR("engine.editor.status.why.file_written_with_scene")));
    }

    const core::NameAtom sourceKey = world.atoms().intern("Source");
    const std::optional<scene::Value> source = world.getProperty(script, sourceKey);
    const std::string* text = source.has_value() ? std::get_if<std::string>(&*source) : nullptr;
    if (text == nullptr)
        return whole(core::tr(ENG_TR("engine.editor.status.why.no_source")));

    const std::filesystem::path path = m_content.root() / std::filesystem::path(m_openScene);
    std::string saved;
    if (!platform::readTextFile(path, saved))
        return whole(core::tr(ENG_TR("engine.editor.status.why.never_saved")));

    // **The saved file, in a world of its own**, with the same registries and
    // the same services at the top, so every name and class means the same.
    scene::World disk(world.classes(), world.enums(), world.atoms(), 1u);
    const core::InstanceId top = topOf(world, script);
    const core::InstanceId diskTop = disk.create(world.classOf(top));
    disk.setName(diskTop, world.name(top));
    const auto mirror = [&](core::InstanceId from, core::InstanceId to) {
        // `readScene` finds the workspace by what it IS, not by its name.
        if (world.workspaces().find(from) != nullptr && disk.workspaces().find(to) == nullptr)
            disk.workspaces().add(to, scene::WorkspaceComponent{});
    };
    mirror(top, diskTop);
    for (core::InstanceId child = world.firstChild(top); child.valid(); child = world.nextSibling(child)) {
        const scene::ClassDescriptor* descriptor = world.classes().find(world.classOf(child));
        const bool service = descriptor != nullptr && scene::hasFlag(descriptor->flags, scene::ClassFlags::Service);
        if (!service && world.workspaces().find(child) == nullptr)
            continue;
        const core::InstanceId copy = disk.create(world.classOf(child));
        disk.setName(copy, world.name(child));
        (void)disk.setParent(copy, diskTop);
        mirror(child, copy);
    }

    if (scene::readScene(disk, saved, nullptr, stampSource()).has_value())
        return whole(core::tr(ENG_TR("engine.editor.status.why.unreadable")));
    scene::StampLibrary stamps(disk, stampSource());
    if (scene::writeScene(disk, nullptr, &stamps) != saved)
        return whole(core::tr(ENG_TR("engine.editor.status.why.not_round_trip")));

    const core::InstanceId target = follow(disk, diskTop, pathOf(world, script));
    if (!target.valid() || disk.classOf(target) != world.classOf(script))
        return whole(core::tr(ENG_TR("engine.editor.status.why.not_in_saved_scene")));

    (void)disk.setProperty(target, sourceKey, scene::Value{*text});
    scene::SceneIoReport report;
    const std::string patched = scene::writeScene(disk, &report, &stamps);
    if (!platform::writeTextFileDurable(path, patched)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.status.could_not_write"), {{"path", path.string()}}), true};
        return ScriptSave::Failed;
    }

    // **Nothing else unsaved is left** when the world now writes exactly this
    // file -- then the scene is clean, and closing will not ask about it.
    {
        scene::StampLibrary live(world, stampSource());
        if (scene::writeScene(world, nullptr, &live) == patched)
            m_sceneDirty = false;
    }
    m_status =
        EditorStatus{core::tr(ENG_TR("engine.editor.status.saved_into_nothing_else_in"),
                              {{"script", world.atoms().text(world.name(script))}, {"path", path.filename().string()}}),
                     false};
    return ScriptSave::Script;
}

void Editor::reportImport(const ContentTree::ImportReport& report) noexcept
{
    if (report.imported.empty() && report.skipped.empty() && report.failed.empty() && report.companions.empty() &&
        report.missing.empty()) {
        return;
    }

    // **Counted, and the refusals named.** "Imported 3 files" is a sentence
    // nobody has to act on; "2 already here" is one they do, and the names are
    // what tells them which.
    std::string message = core::tr(ENG_TR("engine.editor.status.imported_files"),
                                   {{"count", static_cast<core::i64>(report.imported.size())}});
    const auto listOf = [](const std::vector<std::string>& names) {
        std::string joined;
        for (const std::string& name : names) {
            if (!joined.empty())
                joined += ", ";
            joined += name;
        }
        return joined;
    };
    // **Counted apart**, because a person who dragged in one file and sees
    // "imported 7" would reasonably wonder what the other six are.
    if (!report.companions.empty()) {
        message = core::tr(ENG_TR("engine.editor.status.import_and_companions"),
                           {{"status", message}, {"count", static_cast<core::i64>(report.companions.size())}});
    }
    const auto clause = [&message](core::TextKey what, const std::string& names) {
        message = core::tr(ENG_TR("engine.editor.status.with_clause"),
                           {{"status", message}, {"note", core::tr(what, {{"names", names}})}});
    };
    if (!report.skipped.empty())
        clause(ENG_TR("engine.editor.status.import_skipped"), listOf(report.skipped));
    if (!report.failed.empty())
        clause(ENG_TR("engine.editor.status.import_unreadable"), listOf(report.failed));
    // **The one that decides whether the model works.** A `.gltf` whose buffer
    // was not beside it imports perfectly and loads nothing, and this is the
    // only moment anybody can be told which file to go and find.
    if (!report.missing.empty())
        clause(ENG_TR("engine.editor.status.import_missing"), listOf(report.missing));

    m_status = EditorStatus{message, !report.failed.empty() || !report.missing.empty()};
}

void Editor::adoptCamera(const core::CFrameD& cframe) noexcept
{
    m_cameraCFrame = cframe;
    // The angles come out of the matrix ONCE, here. Deriving them every frame
    // would round-trip a rotation through Euler angles sixty times a second,
    // and that is how a fly camera acquires a slow roll nobody can explain.
    const core::Vec3 angles = core::toEulerYxz(cframe.rotation);
    m_yaw = angles.y;
    m_pitch = angles.x;
    m_cameraAdopted = true;
}

// How long the camera takes to reach what F asked it to frame. Long enough to
// read as a move rather than a cut, short enough that nobody waits for it.
constexpr core::f32 kFocusSeconds = 0.18f;

bool selectionBounds(const scene::World& world, std::span<const core::InstanceId> selection, core::DVec3& outCentre,
                     core::f64& outRadius)
{
    bool any = false;
    core::DVec3 lo{};
    core::DVec3 hi{};

    // Descendants included, so F on a `Model` frames the model. The walk is per
    // selected instance rather than over the world, so it costs what is
    // selected.
    std::vector<core::InstanceId> descendants;
    for (const core::InstanceId id : selection) {
        if (!id.valid() || !world.alive(id))
            continue;

        descendants.clear();
        world.collectDescendants(id, descendants);
        descendants.push_back(id);

        for (const core::InstanceId member : descendants) {
            const scene::PartComponent* part = world.parts().find(member);
            if (part == nullptr)
                continue;

            // The rotated box's axis-aligned extent, which is the same absolute
            // -value trick the partitioner uses: a turned crate's reach along
            // each world axis is its half-size dotted with the absolute row.
            const core::Mat3& r = part->cframe.rotation;
            const core::Vec3 h{part->size.x * 0.5f, part->size.y * 0.5f, part->size.z * 0.5f};
            const auto extent = [&](int axis) {
                return static_cast<core::f64>(std::abs(r.m[0][axis]) * h.x + std::abs(r.m[1][axis]) * h.y +
                                              std::abs(r.m[2][axis]) * h.z);
            };
            const core::DVec3 c = part->cframe.position;
            const core::DVec3 e{extent(0), extent(1), extent(2)};

            if (!any) {
                lo = {c.x - e.x, c.y - e.y, c.z - e.z};
                hi = {c.x + e.x, c.y + e.y, c.z + e.z};
                any = true;
                continue;
            }
            lo = {std::min(lo.x, c.x - e.x), std::min(lo.y, c.y - e.y), std::min(lo.z, c.z - e.z)};
            hi = {std::max(hi.x, c.x + e.x), std::max(hi.y, c.y + e.y), std::max(hi.z, c.z + e.z)};
        }
    }

    if (!any)
        return false;

    outCentre = {(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5};
    const core::DVec3 half{(hi.x - lo.x) * 0.5, (hi.y - lo.y) * 0.5, (hi.z - lo.z) * 0.5};
    // The sphere around the box rather than the box, because the camera may be
    // looking at it from any angle and a half-width is only the right distance
    // from one of them.
    outRadius = std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
    return true;
}

core::CFrameD framedCamera(const core::CFrameD& current, core::DVec3 centre, core::f64 radius, f32 fieldOfViewDegrees)
{
    // Never zero: a `Part` of no size, or a selection of one point, would put
    // the camera exactly on it.
    constexpr core::f64 kSmallest = 0.5;
    // Room around the thing, so it is framed rather than filling the panel edge
    // to edge.
    constexpr core::f64 kMargin = 1.35;

    const core::f64 half = static_cast<core::f64>(fieldOfViewDegrees) * 0.5 * 3.14159265358979323846 / 180.0;
    const core::f64 sine = std::sin(half);
    const core::f64 wanted = radius < kSmallest ? kSmallest : radius;
    const core::f64 distance = sine > 1e-6 ? (wanted * kMargin) / sine : wanted * 4.0;

    // `Mat3`'s columns are right, up and BACK, so backing off is +back.
    const core::Mat3& basis = current.rotation;
    const core::DVec3 back{static_cast<core::f64>(basis.m[2][0]), static_cast<core::f64>(basis.m[2][1]),
                           static_cast<core::f64>(basis.m[2][2])};

    core::CFrameD framed;
    framed.rotation = current.rotation;
    framed.position = {centre.x + back.x * distance, centre.y + back.y * distance, centre.z + back.z * distance};
    return framed;
}

void Editor::focusCamera(core::DVec3 centre, core::f64 radius) noexcept
{
    if (!m_cameraAdopted)
        return;
    m_focusTarget = framedCamera(m_cameraCFrame, centre, radius).position;
    m_focusRemaining = kFocusSeconds;
}

void Editor::setCameraSpeed(f32 metresPerSecond) noexcept
{
    // A speed of zero is a camera that cannot move and a negative one flies
    // backwards from every key, both of which read as broken rather than as
    // configured.
    constexpr f32 kSlowest = 0.1f;
    constexpr f32 kFastest = 2000.0f;
    m_cameraSpeed = metresPerSecond < kSlowest ? kSlowest : (metresPerSecond > kFastest ? kFastest : metresPerSecond);
}

core::CFrameD Editor::driveCamera(core::Vec2 lookDelta, core::Vec3 move, f32 dt) noexcept
{
    // Playing means the game owns its camera again. Writing here would be two
    // authors for one transform, and the visible result is a camera that
    // stutters between where the script wants it and where the editor left it.
    // Only while editing. Inside play mode the game owns its camera, paused or
    // not: a person who paused to look at something did not ask for the tool's
    // view.
    //
    // **Except when the view is detached** (Shift+P): then the game keeps its
    // camera and this one is the view, so flying it is the point.
    if ((!editing(m_run) && !cameraDetached()) || !m_cameraAdopted)
        return m_cameraCFrame;

    constexpr f32 kRadiansPerPixel = 0.0032f;
    // Just short of straight up and straight down. AT the pole the yaw axis and
    // the look direction are the same line and the camera spins on its own.
    constexpr f32 kPitchLimit = 1.5533f;

    // **Taking the controls cancels the focus.** A tool that kept sliding the
    // view after somebody started flying is a tool arguing with them, and the
    // person's input is always the more recent answer.
    if (m_focusRemaining > 0.0f) {
        if (lookDelta.x != 0.0f || lookDelta.y != 0.0f || move.x != 0.0f || move.y != 0.0f || move.z != 0.0f) {
            m_focusRemaining = 0.0f;
        }
        else {
            const f32 step = dt < m_focusRemaining ? dt : m_focusRemaining;
            const core::f64 t = m_focusRemaining > 0.0f ? static_cast<core::f64>(step / m_focusRemaining) : 1.0;
            m_cameraCFrame.position = {
                m_cameraCFrame.position.x + (m_focusTarget.x - m_cameraCFrame.position.x) * t,
                m_cameraCFrame.position.y + (m_focusTarget.y - m_cameraCFrame.position.y) * t,
                m_cameraCFrame.position.z + (m_focusTarget.z - m_cameraCFrame.position.z) * t,
            };
            m_focusRemaining -= step;
            return m_cameraCFrame;
        }
    }

    // **The 2D view pans rather than turns.** A drag moves the plane with the
    // pointer -- a pixel of drag is a pixel of world -- and WASD pans at a
    // speed that scales with the zoom, so crossing the screen takes the same
    // time zoomed in or out.
    if (m_view2D) {
        const core::f64 metresPerPixel = m_viewport.height > 0.0f ? 2.0 * static_cast<core::f64>(m_orthographicSize) /
                                                                        static_cast<core::f64>(m_viewport.height)
                                                                  : 0.0;
        const core::f64 pan = static_cast<core::f64>(m_orthographicSize) * 1.5 * static_cast<core::f64>(dt);
        m_cameraCFrame.rotation = core::Mat3{};
        m_cameraCFrame.position = {
            m_cameraCFrame.position.x - static_cast<core::f64>(lookDelta.x) * metresPerPixel +
                static_cast<core::f64>(move.x) * pan,
            m_cameraCFrame.position.y + static_cast<core::f64>(lookDelta.y) * metresPerPixel +
                static_cast<core::f64>(move.z + move.y) * pan,
            m_cameraCFrame.position.z,
        };
        return m_cameraCFrame;
    }

    m_yaw -= lookDelta.x * kRadiansPerPixel;
    m_pitch -= lookDelta.y * kRadiansPerPixel;
    m_pitch = m_pitch > kPitchLimit ? kPitchLimit : (m_pitch < -kPitchLimit ? -kPitchLimit : m_pitch);

    m_cameraCFrame.rotation = core::fromEulerYxz(core::Vec3{m_pitch, m_yaw, 0.0f});

    // `Mat3`'s columns are right, up and BACK, so forward is the negated third
    // -- which `math.h` says at the type and which is the one thing to get
    // wrong here.
    const core::Mat3& basis = m_cameraCFrame.rotation;
    const core::Vec3 right{basis.m[0][0], basis.m[0][1], basis.m[0][2]};
    const core::Vec3 up{basis.m[1][0], basis.m[1][1], basis.m[1][2]};
    const core::Vec3 forward{-basis.m[2][0], -basis.m[2][1], -basis.m[2][2]};

    const core::Vec3 step = (right * move.x + up * move.y + forward * move.z) * (m_cameraSpeed * dt);
    // Accumulated in f64. The editor's camera is the one thing in an open world
    // that a person drives for minutes at a time, and adding centimetre steps
    // to a four-kilometre f32 position is exactly the drift ADR 0014 exists to
    // prevent.
    m_cameraCFrame.position = m_cameraCFrame.position + core::toDVec3(step);
    return m_cameraCFrame;
}

// --- The manipulators -------------------------------------------------------

namespace {

// How many pixels long an arm is drawn, and therefore how big the whole gizmo
// is. Constant on screen: a handle that shrank into nothing as you flew away
// would be a handle you could not grab.
constexpr f32 kGizmoPixels = 90.0f;

[[nodiscard]] f32 snapTo(f32 value, f32 step) noexcept
{
    if (step <= 0.0f)
        return value;
    return std::round(value / step) * step;
}

} // namespace

void Editor::setGizmoMode(GizmoMode mode) noexcept
{
    // Not mid-drag. Changing what a drag MEANS half way through it is not
    // something a person can have meant, and the drag's recorded start would be
    // a start for the wrong operation.
    if (m_drag.has_value())
        return;
    m_gizmoMode = mode;
    m_handlesShown = true;
    m_preferencesDirty = true;
}

void Editor::setHandlesShown(bool shown) noexcept
{
    if (m_drag.has_value())
        return;
    m_handlesShown = shown;
}

void Editor::setGizmoLocal(bool local) noexcept
{
    if (m_drag.has_value())
        return;
    m_gizmoLocal = local;
    m_preferencesDirty = true;
}

f32 Editor::snapStep(GizmoMode mode) const noexcept
{
    return m_snapStep[static_cast<core::usize>(mode)];
}

void Editor::setSnapStep(GizmoMode mode, f32 step) noexcept
{
    m_snapStep[static_cast<core::usize>(mode)] = step > 0.0f ? step : 0.0f;
    m_preferencesDirty = true;
}

namespace {

// Where the manipulator sits for an instance, in WORLD space, or nothing when
// the instance is not anywhere.
//
// **Four kinds, and each is located the way it is defined** (S5.2). A part is
// its own `CFrame`; a camera is too. An attachment's `CFrame` is relative to the
// part it is on, so the world one is the derived `WorldCFrame` the mirror keeps.
// A `Model` has no transform at all and is located by its PIVOT, which is what
// `PivotTo` moves and therefore the only point a gizmo on one could honestly be.
// Where a water is for the gizmo: on its surface, over a lake's or an ocean's
// `Position` and over the middle of a river's points (D399).
[[nodiscard]] core::CFrameD waterFrameOf(const scene::World& world, core::InstanceId id,
                                         const scene::WaterComponent& water)
{
    core::CFrameD frame;
    frame.position = core::DVec3{static_cast<core::f64>(water.position.x), water.surfaceLevel,
                                 static_cast<core::f64>(water.position.z)};
    if (!scene::waterIsRiver(water.shape) && water.shape != scene::water_shape::Lake)
        return frame;
    // Over the middle of its points -- and at the middle of their heights,
    // for a river that descends.
    core::f64 x = 0.0;
    core::f64 y = 0.0;
    core::f64 z = 0.0;
    core::usize count = 0;
    for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
        if (const scene::WaterPointComponent* point = world.waterPoints().find(child); point != nullptr) {
            x += static_cast<core::f64>(point->position.x);
            y += static_cast<core::f64>(point->position.y);
            z += static_cast<core::f64>(point->position.z);
            ++count;
        }
    }
    if (count > 0) {
        const auto n = static_cast<core::f64>(count);
        frame.position =
            core::DVec3{x / n, water.shape == scene::water_shape::River ? y / n : water.surfaceLevel, z / n};
    }
    return frame;
}

// The frame a decal's `CFrame` is relative to: the part it is on, the world
// otherwise.
[[nodiscard]] core::CFrameD decalParentOf(const scene::World& world, core::InstanceId id)
{
    if (const scene::PartComponent* part = world.parts().find(world.parentOf(id)); part != nullptr)
        return part->cframe;
    return core::CFrameD{};
}

// A point with no turn of its own.
[[nodiscard]] core::CFrameD placedAt(core::DVec3 position)
{
    core::CFrameD frame;
    frame.position = position;
    return frame;
}

[[nodiscard]] std::optional<core::CFrameD> gizmoTransformOf(const scene::World& world, core::InstanceId id)
{
    if (const scene::PartComponent* part = world.parts().find(id); part != nullptr)
        return part->cframe;
    if (const scene::CameraComponent* camera = world.cameras().find(id); camera != nullptr)
        return camera->cframe;
    if (const scene::AttachmentComponent* attachment = world.attachments().find(id); attachment != nullptr)
        return attachment->worldCFrame;
    if (world.models().find(id) != nullptr)
        return scene::pivotOf(world, id);
    // A light is wherever it shines from (ADR 0095).
    if (const std::optional<render::LightAnchor> light = render::lightAnchorOf(world, id); light.has_value())
        return light->partFrame * light->offset;
    // A sprite is on the plane and turns about Z, so its frame is that.
    if (const scene::Part2DComponent* sprite = world.parts2d().find(id); sprite != nullptr) {
        core::CFrameD frame;
        frame.position =
            core::DVec3{static_cast<core::f64>(sprite->position.x), static_cast<core::f64>(sprite->position.y), 0.0};
        frame.rotation = core::fromAxisAngle(core::Vec3{0.0f, 0.0f, 1.0f}, sprite->rotation * 3.14159265f / 180.0f);
        return frame;
    }
    // **What is placed without being a part** (D399): a friend of the owner
    // could not move a `Water` -- nothing here knew where one was.
    if (const scene::WaterComponent* water = world.waters().find(id); water != nullptr)
        return waterFrameOf(world, id, *water);
    if (const scene::WaterPointComponent* point = world.waterPoints().find(id); point != nullptr)
        return placedAt(core::toDVec3(point->position));
    if (const scene::TerrainComponent* terrain = world.terrains().find(id); terrain != nullptr)
        return placedAt(terrain->origin);
    if (const scene::DecalComponent* decal = world.decals().find(id); decal != nullptr)
        return decalParentOf(world, id) * decal->cframe;
    if (const scene::NavigationLinkComponent* link = world.navigationLinks().find(id); link != nullptr)
        return placedAt(core::DVec3{(link->from.x + link->to.x) * 0.5, (link->from.y + link->to.y) * 0.5,
                                    (link->from.z + link->to.z) * 0.5});
    if (const scene::Tilemap2DComponent* tilemap = world.tilemaps2d().find(id); tilemap != nullptr)
        return placedAt(
            core::DVec3{static_cast<core::f64>(tilemap->position.x), static_cast<core::f64>(tilemap->position.y), 0.0});
    return std::nullopt;
}

} // namespace

// Puts one dragged instance at `after`, in world space, by whatever route its
// kind is transformed through (S5.2).
//
// **Through the inspector's queue in every case**, which is what keeps a gizmo
// drag one undo step and one safe point however many kinds are in the selection
// -- and what stops a `Model` needing its own history handling.
void Editor::applyDragTransform(scene::World& world, Inspector& inspector, core::usize index,
                                const core::CFrameD& after)
{
    const GizmoDrag& drag = *m_drag;
    const core::InstanceId id = drag.targets[index];
    const core::NameAtom cframeName = world.atoms().intern("CFrame");

    switch (drag.kinds[index]) {
    case DragKind::Attachment:
        // **Divided back through the parent**, because an `Attachment.CFrame` is
        // relative to the part it is on. Writing the world frame straight in
        // would move a bone by the part's own transform on top of the drag --
        // which on a character ten metres out is a bone ten metres away.
        inspector.enqueue(id, cframeName, scene::Value{core::inverse(drag.parents[index]) * after});
        return;

    case DragKind::Model: {
        // A `Model` has no transform, so moving it is moving everything under
        // it by the same delta -- which is exactly what `PivotTo` means and the
        // only thing that keeps the parts' relative layout.
        //
        // **From where each part was when the drag began**, never from where it
        // is now: the delta is the whole drag so far, and applying it to parts
        // the previous frames already moved sums it once per frame.
        const core::CFrameD delta = after * core::inverse(drag.before[index]);
        for (const auto& [descendant, start] : drag.inside[index]) {
            if (world.alive(descendant))
                inspector.enqueue(descendant, cframeName, scene::Value{delta * start});
        }
        return;
    }

    case DragKind::Part2D: {
        // Back onto the plane: where it went, flattened, and how far it turned
        // about Z, read off the frame's right axis. A turn about any other
        // axis has no meaning for a sprite and is dropped rather than
        // approximated.
        const core::Vec2 position{static_cast<f32>(after.position.x), static_cast<f32>(after.position.y)};
        const f32 degrees = std::atan2(after.rotation.m[0][1], after.rotation.m[0][0]) * 180.0f / 3.14159265f;
        inspector.enqueue(id, world.atoms().intern("Position"), scene::Value{position});
        inspector.enqueue(id, world.atoms().intern("Rotation"), scene::Value{static_cast<core::f64>(degrees)});
        return;
    }

    case DragKind::Water: {
        // **On its surface**: up and down is `SurfaceLevel`. A river is its
        // points, each moved by the whole drag from where it was at the start
        // -- as a spline actor is -- and turned with it; a lake or an ocean is
        // its `Position`, which does not turn.
        const core::CFrameD delta = after * core::inverse(drag.before[index]);
        const core::f64 rise = after.position.y - drag.before[index].position.y;
        inspector.enqueue(id, world.atoms().intern("SurfaceLevel"), scene::Value{drag.levels[index] + rise});
        if (!drag.inside[index].empty()) {
            const core::NameAtom positionName = world.atoms().intern("Position");
            for (const auto& [point, start] : drag.inside[index]) {
                if (world.alive(point))
                    inspector.enqueue(point, positionName, scene::Value{core::toVec3((delta * start).position)});
            }
            return;
        }
        const core::Vec3 was = drag.positions[index];
        const core::Vec3 moved = core::toVec3(after.position - drag.before[index].position);
        inspector.enqueue(id, world.atoms().intern("Position"),
                          scene::Value{core::Vec3{was.x + moved.x, was.y + moved.y, was.z + moved.z}});
        return;
    }

    case DragKind::Position:
        inspector.enqueue(id, world.atoms().intern("Position"), scene::Value{core::toVec3(after.position)});
        return;

    case DragKind::Position2D:
        inspector.enqueue(
            id, world.atoms().intern("Position"),
            scene::Value{core::Vec2{static_cast<f32>(after.position.x), static_cast<f32>(after.position.y)}});
        return;

    case DragKind::NavigationLink: {
        // Both ends by the same delta, from where they were at the start.
        const core::CFrameD delta = after * core::inverse(drag.before[index]);
        const auto& ends = drag.inside[index];
        if (ends.size() == 2) {
            inspector.enqueue(id, world.atoms().intern("From"),
                              scene::Value{core::toVec3((delta * ends[0].second).position)});
            inspector.enqueue(id, world.atoms().intern("To"),
                              scene::Value{core::toVec3((delta * ends[1].second).position)});
        }
        return;
    }

    case DragKind::Part:
    case DragKind::Camera:
    default:
        // Both own a world `CFrame` outright, and both expose it as `CFrame`.
        inspector.enqueue(id, cframeName, scene::Value{after});
        return;
    }
}

std::optional<GizmoFrame> Editor::gizmoFrame(const scene::World& world, const Inspector& inspector) const
{
    if (!editing(m_run) || !m_hasCamera || !m_handlesShown)
        return std::nullopt;
    // **The Water tool's handles are the water's own**: its points, its
    // corners. The manipulator over the river in hand would be a second set
    // of handles in the middle of it, taking the clicks that lay its course.
    if (m_tool == Tool::Water && m_waterPanelShown)
        return std::nullopt;

    const core::InstanceId primary = inspector.selection();
    if (!primary.valid() || !world.alive(primary))
        return std::nullopt;

    // **Whatever the primary IS, if it is somewhere** (S5.2). The manipulator
    // read the part pool and nothing else, so selecting a `Camera`, an
    // `Attachment` or a `Model` gave no gizmo at all -- and the two verbs an
    // editor has for moving something are the gizmo and typing numbers into the
    // grid.
    const std::optional<core::CFrameD> located = gizmoTransformOf(world, primary);
    if (!located.has_value())
        return std::nullopt;

    GizmoFrame frame;
    frame.transform.position = located->position;

    // **The middle of the selection, when asked for** (S5.17). Over one instance
    // this is the instance, so the control does nothing there; over forty it is
    // the difference between rotating a row of columns about the one you clicked
    // last and rotating it about itself.
    //
    // The mean of the transforms rather than the centre of the bounding box: a
    // box's centre moves when one part is scaled, so a gizmo on it would drift
    // during a scale drag -- and a handle that moves while you hold it is a
    // handle that does not track the pointer.
    if (m_gizmoOrigin == GizmoOrigin::Centre && inspector.selectionCount() > 1) {
        core::DVec3 sum{};
        core::usize counted = 0;
        for (const core::InstanceId id : inspector.selectionSet()) {
            if (const std::optional<core::CFrameD> at = gizmoTransformOf(world, id); at.has_value()) {
                sum = sum + at->position;
                ++counted;
            }
        }
        if (counted > 0) {
            const auto divisor = static_cast<core::f64>(counted);
            frame.transform.position = core::DVec3{sum.x / divisor, sum.y / divisor, sum.z / divisor};
        }
    }
    // World axes unless somebody asked for the part's own. A rotated crate is
    // unusable in world space and a wall is unusable in local, which is why this
    // is a choice rather than a decision made here.
    //
    // **Except for SCALE, which is always the part's own, and that is not a
    // preference.** A `Size` is three numbers in the part's own space -- there
    // is no world-space size to change -- so a world-axis scale arm on a turned
    // crate would point one way and grow it another. Unity's scale tool ignores
    // the same toggle for the same reason, and the alternative is a handle that
    // lies about what it does.
    const bool local = m_gizmoLocal || m_gizmoMode == GizmoMode::Scale;
    frame.transform.rotation = local ? located->rotation : core::Mat3{};
    frame.size = metresPerPixel(m_projection, m_viewport, m_cameraOrigin, frame.transform.position) * kGizmoPixels;

    if (!(frame.size > 0.0f))
        return std::nullopt;
    return frame;
}

std::optional<GizmoHandle> Editor::gizmoHandle() const noexcept
{
    if (m_drag.has_value())
        return m_drag->handle;
    return m_hover;
}

void Editor::setPointer(core::Vec2 pixelInViewport, bool pressed, bool down) noexcept
{
    m_pointer = pixelInViewport;
    m_pointerPressed = pressed;
    m_pointerDown = down;
}

// --- The brush (F1) ----------------------------------------------------------

void Editor::setView2D(bool on) noexcept
{
    if (on == m_view2D)
        return;
    m_view2D = on;
    m_focusRemaining = 0.0f;
    if (on) {
        m_saved3DCamera = m_cameraCFrame;
        m_saved3DYaw = m_yaw;
        m_saved3DPitch = m_pitch;
        // Square on to the plane, and in front of it: a camera behind z = 0
        // looking down -Z would see nothing that lies on it.
        m_cameraCFrame.rotation = core::Mat3{};
        m_cameraCFrame.position.z = std::max(m_cameraCFrame.position.z, 100.0);
        return;
    }
    m_cameraCFrame = m_saved3DCamera;
    m_yaw = m_saved3DYaw;
    m_pitch = m_saved3DPitch;
}

void Editor::zoom2D(f32 steps, core::Vec2 pointerInViewport) noexcept
{
    if (!m_view2D || steps == 0.0f)
        return;
    const f32 before = m_orthographicSize;
    // A fixed ratio per notch, so zooming feels the same at any scale.
    m_orthographicSize = std::clamp(before * std::pow(0.85f, steps), 0.25f, 10000.0f);
    if (!(m_viewport.width > 0.0f) || !(m_viewport.height > 0.0f))
        return;
    // The point under the pointer stays under it: its offset from the middle
    // of the view scales with the zoom, and the camera moves by the difference.
    const core::f64 aspect = static_cast<core::f64>(m_viewport.width) / static_cast<core::f64>(m_viewport.height);
    const core::f64 ndcX =
        2.0 * static_cast<core::f64>(pointerInViewport.x) / static_cast<core::f64>(m_viewport.width) - 1.0;
    const core::f64 ndcY =
        1.0 - 2.0 * static_cast<core::f64>(pointerInViewport.y) / static_cast<core::f64>(m_viewport.height);
    const core::f64 change = static_cast<core::f64>(before) - static_cast<core::f64>(m_orthographicSize);
    m_cameraCFrame.position.x += ndcX * aspect * change;
    m_cameraCFrame.position.y += ndcY * change;
}

core::InstanceId Editor::tilemapFor(const scene::World& world, const Inspector& inspector,
                                    core::InstanceId root) noexcept
{
    // The selection, or the tilemap it is inside.
    for (core::InstanceId walk = inspector.selection(); walk.valid() && world.alive(walk);
         walk = world.parentOf(walk)) {
        if (world.tilemaps2d().find(walk) != nullptr)
            return walk;
    }
    // Otherwise the first in the world, in pool order.
    core::InstanceId found;
    world.tilemaps2d().forEach([&](core::InstanceId id, const scene::Tilemap2DComponent&) {
        if (!found.valid() && root.valid() && world.isAncestorOf(root, id))
            found = id;
    });
    return found;
}

bool Editor::driveTiles(scene::World& world, core::InstanceId root, Inspector& inspector)
{
    m_tileAim.reset();
    const core::InstanceId target =
        m_tileStroke.has_value() ? m_tileStroke->tilemap : tilemapFor(world, inspector, root);
    scene::Tilemap2DComponent* tilemap = target.valid() ? world.tilemaps2d().find(target) : nullptr;

    if (m_tool != Tool::Tiles || !m_tilesPanelShown || tilemap == nullptr || !(tilemap->cellSize > 0.0f)) {
        if (m_tileStroke.has_value()) {
            m_lastTileEdits = m_tileStroke->edits;
            m_tileStroke.reset();
        }
        // A tool with nothing to act on does not eat the click, for the
        // reason `driveSculpt` gives.
        return false;
    }

    // The plane the tiles lie on, z = 0, under the pointer.
    const PickRay ray = rayThrough(m_pointer);
    if (std::abs(static_cast<double>(ray.direction.z)) > 1e-6) {
        const double along = -ray.origin.z / static_cast<double>(ray.direction.z);
        if (along > 0.0) {
            const double size = static_cast<double>(tilemap->cellSize);
            const double x =
                ray.origin.x + static_cast<double>(ray.direction.x) * along - static_cast<double>(tilemap->position.x);
            const double y =
                ray.origin.y + static_cast<double>(ray.direction.y) * along - static_cast<double>(tilemap->position.y);
            const double cellX = std::floor(x / size);
            const double cellY = std::floor(y / size);
            // Past the range a cell is numbered in, there is nothing to aim at.
            constexpr double Reach = 1.0e9;
            if (std::abs(cellX) < Reach && std::abs(cellY) < Reach)
                m_tileAim = TileAim{target, {static_cast<core::i32>(cellX), static_cast<core::i32>(cellY)}};
        }
    }

    if (m_tileStroke.has_value() && !m_pointerDown) {
        m_lastTileEdits = m_tileStroke->edits;
        m_tileStroke.reset();
        m_pending.reset();
        return true;
    }
    if (!m_tileStroke.has_value()) {
        if (!m_pointerPressed || !m_tileAim.has_value())
            return false;
        m_tileStroke = TileStroke{target, std::nullopt, 0, 0};
    }

    if (m_tileAim.has_value() && m_tileAim->cell != m_tileStroke->last) {
        const std::array<core::i32, 2> to = m_tileAim->cell;
        const std::array<core::i32, 2> from = m_tileStroke->last.value_or(to);
        m_tileStroke->last = to;
        const core::u16 value = m_tileOp == TileOp::Erase ? core::u16{0} : m_tile;

        // Every cell on the line from the last one reached, so a stroke faster
        // than a cell a frame is still a line. Bresenham, both ends included.
        std::vector<std::array<core::i32, 2>> cells;
        {
            core::i64 x = from[0];
            core::i64 y = from[1];
            const core::i64 dx = std::abs(static_cast<core::i64>(to[0]) - x);
            const core::i64 dy = -std::abs(static_cast<core::i64>(to[1]) - y);
            const core::i64 sx = x < to[0] ? 1 : -1;
            const core::i64 sy = y < to[1] ? 1 : -1;
            core::i64 error = dx + dy;
            for (;;) {
                cells.push_back({static_cast<core::i32>(x), static_cast<core::i32>(y)});
                if (x == to[0] && y == to[1])
                    break;
                const core::i64 twice = 2 * error;
                if (twice >= dy) {
                    error += dy;
                    x += sx;
                }
                if (twice <= dx) {
                    error += dx;
                    y += sy;
                }
            }
        }

        for (const std::array<core::i32, 2>& cell : cells) {
            if (tilemap->cell(cell[0], cell[1]) == value)
                continue;
            // **Recorded on the first cell that changes**, not on the press,
            // for the reason the block tool gives: a step that undoes nothing
            // is a step somebody presses ctrl-Z through wondering what it was.
            if (m_tileStroke->gesture == 0) {
                m_tileStroke->gesture = inspector.beginGesture();
                m_history.record(world,
                                 m_tileOp == TileOp::Erase ? core::tr(ENG_TR("engine.editor.history.erase_tiles"))
                                                           : core::tr(ENG_TR("engine.editor.history.paint_tiles")),
                                 m_tileStroke->gesture);
                tilemap = world.tilemaps2d().find(target);
                if (tilemap == nullptr)
                    break;
            }
            (void)tilemap->setCell(cell[0], cell[1], value);
            tilemap->revision += 1;
            m_tileStroke->edits += 1;
            m_sceneDirty = true;
        }
    }
    m_pending.reset();
    return true;
}

void Editor::setTool(Tool tool) noexcept
{
    // Refused mid-stroke, exactly as `setGizmoMode` is refused mid-drag.
    if (m_stroke.has_value() || m_blockStroke.has_value() || m_tileStroke.has_value() || m_waterGesture.has_value())
        return;
    m_tool = tool;
    m_preferencesDirty = true;
}

void Editor::setBrushRadius(f32 metres) noexcept
{
    // A radius of zero stamps nothing and a huge one would ask for a million
    // voxel writes in a frame, so both ends are clamped rather than refused --
    // a slider that stops is better than one that does nothing at its end.
    m_brush.radius = std::clamp(metres, 0.25f, 64.0f);
    m_preferencesDirty = true;
}

void Editor::setBrushSpacing(f32 fraction) noexcept
{
    // Below about a tenth the stamps overlap so heavily that a drag costs
    // hundreds of edits and looks identical; above one they stop overlapping at
    // all and a stroke becomes a dotted line.
    m_brush.spacing = std::clamp(fraction, 0.1f, 1.0f);
    m_preferencesDirty = true;
}

void Editor::setBrushFalloff(f32 falloff) noexcept
{
    if (!(falloff == falloff))
        return;
    m_brush.falloff = std::clamp(falloff, 0.0f, 1.0f);
    m_preferencesDirty = true;
}

void Editor::setBrushStrength(f32 strength) noexcept
{
    // A strength of zero is a tool that does nothing and one of one is a tool
    // with no feel, so both ends are clamped inside rather than at the extremes.
    m_brush.strength = std::clamp(strength, 0.02f, 1.0f);
    m_preferencesDirty = true;
}

void Editor::setBrushMaterial(core::u8 material) noexcept
{
    // **Never zero.** Zero means erase to `fillBall`, and a material picker
    // whose first entry deleted the hillside would be the worst possible
    // reading of one shared convention -- erasing is the `erase` flag.
    m_brush.material = material == 0 ? 1 : material;
    m_preferencesDirty = true;
}

namespace {

// The terrain a click can reach: the one under the root the viewport is
// drawing.
//
// A walk of the root's children rather than the first entry of the pool,
// because a stamp stage has a world of its own and its terrain is not the
// host's -- and because a pool's first entry is an allocation order, which is
// not a fact anybody clicking on the ground has in mind.
[[nodiscard]] core::InstanceId terrainUnder(const scene::World& world, core::InstanceId root)
{
    if (!root.valid())
        return {};
    for (core::InstanceId child = world.firstChild(root); child.valid(); child = world.nextSibling(child)) {
        if (world.terrains().find(child) != nullptr)
            return child;
    }
    return {};
}

} // namespace

bool Editor::driveSculpt(scene::World& world, core::InstanceId root, Inspector& inspector, double dt)
{
    // **The aim is cleared first, every frame.** It is what the ring is drawn
    // from, and a stale one would leave a brush hanging in the air over a tool
    // that is no longer the brush.
    m_brushAim.reset();

    // **Looked for every frame, before the tool is even consulted.** The toolbar
    // shows the brush only when there is ground to use it on, and it is drawn
    // from this -- so the answer has to be current whichever tool is selected.
    const core::InstanceId terrainId = terrainUnder(world, root);
    scene::TerrainComponent* terrain = terrainId.valid() ? world.terrains().find(terrainId) : nullptr;
    m_hasTerrain = terrain != nullptr;
    m_strokeWorld = &world;

    // **A stroke ends its undo gesture when it ends** (D168). The gesture is
    // what makes a stroke's hundred stamps one undo step; left open, the next
    // stroke's `beginGesture` returned the same one, and every stroke of the
    // session coalesced into a single step that one ctrl+Z took back whole.
    const auto endStroke = [&] {
        if (m_stroke.has_value()) {
            if (inspector.gesture() == m_stroke->gesture)
                inspector.endGesture();
            // **A stroke that changed nothing leaves nothing to undo**: a brush
            // dragged over rock it cannot raise, or refused as too big, used to
            // push a step that ctrl-Z spent a press on. Its step is recorded
            // with its first change (`applyBrushAt`), so there is none.
            //
            // **And one that was refused says so** (the brush used to do
            // nothing and say nothing): a stamp may touch at most
            // `MaxEditVoxels`, which small voxels reach at a modest radius.
            if (m_stroke->refused) {
                m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.brush_too_big")), true};
            }
            // **Restoring puts back what was thinned, and only that** (the
            // editor list: "Grow" planted nothing where nothing was thinned).
            // Where a layer grows is its rules'; a stroke with nothing to put
            // back says so rather than doing nothing silently.
            else if (m_stroke->touched == 0 && m_tool == Tool::Foliage && !m_stroke->thin) {
                m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.nothing_to_restore")), false};
            }
        }
        m_stroke.reset();
    };

    // **The brush is the Terrain panel's**: with the panel closed there is no
    // brush, whatever tool was last chosen (`setTerrainPanelShown`). Only the
    // three terrain tools stroke: the block and tile tools have brushes of
    // their own, and a click with either used to sculpt the ground under it.
    const bool terrainTool = m_tool == Tool::Sculpt || m_tool == Tool::Paint || m_tool == Tool::Foliage;
    if (!terrainTool || !m_terrainPanelShown) {
        endStroke();
        return false;
    }

    // **A stroke is on the terrain it began on** (terrain audit E5): that one
    // deleted mid-stroke with another beside it, the rest of the stroke went
    // onto the other with the first one's height and aim.
    if (m_stroke.has_value() && !(terrainId == m_stroke->terrain))
        endStroke();

    if (terrain == nullptr) {
        endStroke();
        // **A tool with nothing to act on does not eat the click.** Somebody who
        // left the brush selected and clicked a part meant to select the part,
        // and a world with no terrain in it cannot have meant anything else.
        return false;
    }
    if (!m_pointerOverViewport && !m_stroke.has_value())
        return false;

    // **Against the live field, stroke or no stroke** (the owner, 2026-09-23:
    // "it does not see the changes while I hold the mouse"). A brush held down
    // works on the ground as it now is: adding piles up towards the camera,
    // raising keeps climbing, digging goes deeper. Aiming at the field as the
    // stroke began was what the height-brush era chose, to stop a held brush
    // burrowing -- which is exactly what the reference editors' brushes do,
    // and what a person holding one down means.
    const PickRay ray = rayThrough(m_pointer);
    const asset::TerrainField& aimAt = terrain->field;
    // **Cast in the FIELD's space and answered in the world's.** A terrain can
    // be moved, and the field knows nothing about that -- the origin is applied
    // by its consumers rather than baked into every tile. So the ray goes in
    // with the origin subtracted and the hit comes back with it added, which is
    // the only place in the brush that has to know a terrain has a position.
    const core::DVec3 localOrigin{ray.origin.x - terrain->origin.x, ray.origin.y - terrain->origin.y,
                                  ray.origin.z - terrain->origin.z};
    m_brushAim = asset::raycastField(aimAt, localOrigin, ray.direction, BrushReach);
    m_brushAimOnPlane = false;
    if (m_brushAim.has_value()) {
        m_brushAim->position.x += terrain->origin.x;
        m_brushAim->position.y += terrain->origin.y;
        m_brushAim->position.z += terrain->origin.z;
        m_lastGroundHeight = static_cast<f32>(m_brushAim->position.y);
    }
    else if (m_brushPlaneLock && (aimAt.empty() || m_stroke.has_value())) {
        // **The plane, when the ray met no ground** -- on a field with none,
        // where it is the only place to start, and under a stroke carried past
        // the ground's edge. Not to start one beside ground that is there: a
        // click past a hill's edge put a ball on the origin's plane in mid-air,
        // five hundred metres out (terrain audit E4). Without this a brush over
        // an empty field stamps nothing anywhere, which is a tool that does not
        // work rather than a tool with an edge case.
        //
        // Held at the stroke's own height while one is running, so extending a
        // hillside past its edge continues it instead of dropping to the
        // origin; at the terrain's origin otherwise.
        const double planeY = m_stroke.has_value() ? static_cast<double>(m_stroke->plane) : terrain->origin.y;
        // A ray parallel to the plane meets it nowhere, and one pointing away
        // meets it behind the camera. Both are "no aim" rather than a hit at a
        // negative distance.
        if (std::abs(static_cast<double>(ray.direction.y)) > 1e-6) {
            const double along = (planeY - ray.origin.y) / static_cast<double>(ray.direction.y);
            if (along > 0.0 && along <= BrushReach) {
                asset::TerrainHit hit;
                hit.position = core::DVec3{ray.origin.x + static_cast<double>(ray.direction.x) * along, planeY,
                                           ray.origin.z + static_cast<double>(ray.direction.z) * along};
                hit.normal = core::Vec3{0.0f, 1.0f, 0.0f};
                hit.distance = along;
                m_brushAim = hit;
                m_brushAimOnPlane = true;
            }
        }
    }

    // The button came up, or the stroke ran out of ground under it.
    if (m_stroke.has_value() && !m_pointerDown) {
        // **An editor says what it did.** The manipulator has no equivalent
        // because a drag's result is on screen; a stroke's is a number of edits
        // nobody can count by looking, and it is the cheapest evidence that the
        // brush did what the drag asked rather than one stamp or a thousand.
        m_lastStrokeStamps = m_stroke->stamps;
        endStroke();
        // The release belongs to the brush: without this the same click that
        // finished a stroke falls through and selects whatever is under it.
        m_pending.reset();
        return true;
    }

    if (!m_brushAim.has_value()) {
        // Over the sky. A stroke already running keeps running -- a drag that
        // crosses a gap in the ground is one stroke, not two -- but it stamps
        // nothing this frame.
        if (m_stroke.has_value())
            return true;
        return false;
    }

    if (!m_stroke.has_value()) {
        if (!m_pointerPressed)
            // Hovering. The ring is drawn from `m_brushAim`, and the pointer is
            // still the manipulator's and the pick's.
            return false;

        // **A click on a part is the part's** (TA17b): the brush drives before
        // the pick and casts at the terrain alone, so a click on a crate on the
        // ground sculpted the ground behind the crate. What is nearer than the
        // ground along the ray is what the click was for; the pick takes it.
        if (const std::optional<PickHit> part = pickNearest(world, root, ray);
            part.has_value() && static_cast<double>(part->distance) < m_brushAim->distance)
            return false;

        // **The eyedropper** (B4): Alt and a click with Paint takes what the
        // ground there shows -- what is painted over it where that shows more
        // -- as the brush's material, and paints nothing.
        if (m_tool == Tool::Paint && m_brushPicking) {
            const core::DVec3 at{
                m_brushAim->position.x - terrain->origin.x - static_cast<double>(m_brushAim->normal.x) * 0.25,
                m_brushAim->position.y - terrain->origin.y - static_cast<double>(m_brushAim->normal.y) * 0.25,
                m_brushAim->position.z - terrain->origin.z - static_cast<double>(m_brushAim->normal.z) * 0.25};
            const asset::TerrainField& field = terrain->field;
            const asset::Voxel voxel =
                field.voxel(field.voxelIndex(at.x), field.voxelIndex(at.y), field.voxelIndex(at.z));
            const core::u8 shown = voxel.cover >= 128 ? voxel.top : voxel.material;
            if (shown != 0) {
                setBrushMaterial(shown);
                const bool named = shown <= terrain->layers.size();
                m_status = EditorStatus{named ? core::tr(ENG_TR("engine.editor.terrain.status.picked"),
                                                         {{"name", terrain->layers[shown - 1u]}})
                                              : core::tr(ENG_TR("engine.editor.terrain.status.picked_id"),
                                                         {{"id", static_cast<core::i64>(shown)}}),
                                        false};
            }
            m_pending.reset();
            return true;
        }

        // **One undo step for the whole stroke**, recorded before the first
        // stamp writes anything. A terrain snapshot is a vector of shared
        // pointers to tiles nobody is about to change, so this costs a copy of
        // the index and not a copy of the ground (ADR 0067).
        Stroke stroke;
        stroke.terrain = terrainId;
        stroke.gesture = inspector.beginGesture();
        stroke.last = m_brushAim->position;
        // Where `Flatten` levels to. Captured once, here, so a drag across a
        // hillside levels it to where the stroke began rather than chasing its
        // own result downhill.
        stroke.plane = static_cast<f32>(m_brushAim->position.y);
        stroke.op = effectiveBrushOp();
        stroke.paintMode = effectivePaintMode();
        stroke.thin = effectiveFoliageThin();
        stroke.carve = carves(m_tool, stroke.op);
        if (stroke.carve)
            stroke.aimField = terrain->field;
        // Taken now, recorded by the first stamp that changes something.
        stroke.before = world.snapshot();
        stroke.label = strokeLabel(m_tool, stroke.op, stroke.thin);
        m_stroke = std::move(stroke);

        applyBrushAt(*terrain, m_brushAim->position);
        m_stroke->stamps += 1;
        m_pending.reset();
        return true;
    }

    // **Add puts ground only where it is pointed at ground** (TA17a): past the
    // ground's edge a running stroke aims at its plane, and a held Add stamped
    // a ball there in mid-air that grew, and a drag walked a bridge of them into
    // the sky -- the owner's floating ball. The stroke runs on, and stamps again
    // when the pointer is back on ground. An empty field has only the plane,
    // and is where a first Add starts.
    if (m_brushAimOnPlane && m_tool == Tool::Sculpt && m_stroke->op == BrushOp::Add && !terrain->field.empty()) {
        m_pending.reset();
        return true;
    }

    // **A drag in progress, walked in metres; a held pointer, on the clock.**
    // One path for every tool (`holdStroke`): a drag stamps every step of the
    // way from the last stamp, so it is a function of where the pointer went
    // rather than of how many frames it took, and a pointer held still keeps
    // working the ground under it, as it now is.
    holdStroke(*terrain, ray, dt);
    m_pending.reset();
    return true;
}

namespace {

// How fast an Add or Subtract held still builds out of the ground or bores into
// it, in metres per second, from the weakest brush to the strongest. A speed
// rather than a stamp count per frame, so the result does not depend on the
// framerate -- and slow enough to stop where the person meant to.
constexpr double CarveSpeedMin = 1.5;
constexpr double CarveSpeedMax = 8.0;

// At most this many held stamps in one frame, so a hitch of a second does not
// work a second's worth of ground in one go behind the person's back.
constexpr int CarveStampsPerFrame = 4;

// The voxels a dragged stroke's stamps may walk in one frame, together: about
// a few tens of milliseconds of the heavier brushes.
constexpr double StrokeVoxelsPerFrame = 24.0e6;

// How often a brush held still stamps, from the weakest brush to the
// strongest: every tool but Add and Subtract, which pace themselves by speed.
constexpr double HeldStampsMin = 4.0;
constexpr double HeldStampsMax = 20.0;

// How far one stamp of Grow or Erode moves the surface at the brush's centre,
// in metres. Scaled by the radius, so a big brush builds a hill as fast relative
// to its size as a small one builds a bump; with the default spacing a point
// under a drag is stamped about eight times, so a full-strength pass moves it
// by roughly a third of the radius.
[[nodiscard]] float growAmount(const Editor::Brush& brush) noexcept
{
    return std::clamp(brush.strength, 0.0f, 1.0f) * brush.radius * 0.12f;
}

} // namespace

core::u8 Editor::groundMaterial(const scene::TerrainComponent& terrain, core::u8 wanted) noexcept
{
    // **A layer the terrain has, or its first** (terrain audit E3): the brush's
    // material is the project's, kept across scenes, and ground laid as a
    // layer this terrain does not name drew grey -- and stayed grey when the
    // first material was added, which the panel says the ground becomes.
    const auto layers = static_cast<core::u32>(terrain.layers.size());
    if (layers == 0 || wanted == 0)
        return 1;
    return static_cast<core::u8>(std::min<core::u32>(wanted, layers));
}

bool Editor::carves(Tool tool, const Brush& brush) noexcept
{
    return carves(tool, brush.op);
}

bool Editor::carves(Tool tool, BrushOp op) noexcept
{
    return tool == Tool::Sculpt && (op == BrushOp::Add || op == BrushOp::Subtract);
}

Editor::BrushOp Editor::effectiveBrushOp() const noexcept
{
    if (m_tool != Tool::Sculpt)
        return m_brush.op;
    if (m_brushSmoothHeld)
        return BrushOp::Smooth;
    if (!m_brushInvert)
        return m_brush.op;
    switch (m_brush.op) {
    case BrushOp::Add:
        return BrushOp::Subtract;
    case BrushOp::Subtract:
        return BrushOp::Add;
    case BrushOp::Grow:
        return BrushOp::Erode;
    case BrushOp::Erode:
        return BrushOp::Grow;
    case BrushOp::Smooth:
    case BrushOp::Flatten:
        break;
    }
    return m_brush.op;
}

void Editor::holdStroke(scene::TerrainComponent& terrain, const PickRay& ray, double dt)
{
    // **Two motions, told apart by direction.** The live aim moves ALONG the
    // ray when the stamp before it opened the wall up -- that is boring, and it
    // is paced by the clock -- and ACROSS the ray when the pointer moved -- that
    // is a drag, and it is stamped by distance like any other stroke, so a
    // tunnel dragged sideways is a trench in the wall and not a row of dents.
    const auto radius = static_cast<double>(m_brush.radius);
    const auto spacing = static_cast<double>(m_brush.spacing);
    const core::DVec3 aim = m_brushAim->position;
    // Where a drag goes: for a volume stroke, the ground it began on
    // (`Stroke::aimField`); for every other, the ground as it now is.
    core::DVec3 target = aim;
    if (m_stroke->carve) {
        const core::DVec3 local{ray.origin.x - terrain.origin.x, ray.origin.y - terrain.origin.y,
                                ray.origin.z - terrain.origin.z};
        if (const std::optional<asset::TerrainHit> hit =
                asset::raycastField(m_stroke->aimField, local, ray.direction, BrushReach)) {
            target = core::DVec3{hit->position.x + terrain.origin.x, hit->position.y + terrain.origin.y,
                                 hit->position.z + terrain.origin.z};
        }
    }
    const core::DVec3 moved{target.x - m_stroke->last.x, target.y - m_stroke->last.y, target.z - m_stroke->last.z};
    const core::DVec3 along{static_cast<double>(ray.direction.x), static_cast<double>(ray.direction.y),
                            static_cast<double>(ray.direction.z)};
    const double depth = moved.x * along.x + moved.y * along.y + moved.z * along.z;
    const core::DVec3 across{moved.x - along.x * depth, moved.y - along.y * depth, moved.z - along.z * depth};
    const double lateral = std::sqrt(across.x * across.x + across.y * across.y + across.z * across.z);
    if (lateral >= radius * spacing) {
        const std::vector<core::DVec3> stamps = strokeStamps(m_stroke->last, target, radius, spacing);
        // **As many as a frame can walk** (terrain audit E7): the stamps a
        // frame were capped and the voxels each walks were not, so a big
        // brush dragged fast stalled a frame for seconds. The rest of the drag
        // is walked from where this frame stopped, next frame.
        const double side = 2.0 * radius / static_cast<double>(terrain.field.settings().voxelSize) + 8.0;
        const auto affordable = static_cast<core::usize>(
            std::clamp(StrokeVoxelsPerFrame / (side * side * side), 1.0, static_cast<double>(stamps.size())));
        core::usize applied = 0;
        for (core::usize at = 1; at < stamps.size() && applied < affordable; ++at, ++applied) {
            applyBrushAt(terrain, stamps[at]);
            m_stroke->stamps += 1;
            m_stroke->last = stamps[at];
        }
        return;
    }

    // Held still: a stamp on the clock, each where the ray now meets the ground
    // -- which the stamp before it moved. Add and Subtract build or bore a ball
    // every `radius / speed` seconds, so a held Add grows towards the camera
    // and a held Subtract tunnels away from it; every other tool stamps at a
    // rate the strength sets.
    const double strength = static_cast<double>(std::clamp(m_brush.strength, 0.0f, 1.0f));
    const double interval = m_stroke->carve
                                ? std::max(radius, 0.1) / (CarveSpeedMin + (CarveSpeedMax - CarveSpeedMin) * strength)
                                : 1.0 / (HeldStampsMin + (HeldStampsMax - HeldStampsMin) * strength);
    m_stroke->carveClock = std::min(m_stroke->carveClock + std::max(dt, 0.0), interval * CarveStampsPerFrame);
    while (m_stroke->carveClock >= interval) {
        m_stroke->carveClock -= interval;
        applyBrushAt(terrain, aim);
        m_stroke->stamps += 1;
        // A volume stroke's drag stays on the ground it began on, so the
        // bored point is not where the next drag stamp is walked from.
        if (!m_stroke->carve)
            m_stroke->last = aim;
    }
}

void Editor::applyBrushAt(scene::TerrainComponent& terrain, core::DVec3 worldAt)
{
    // Back into the field's own space, for the reason the raycast above goes the
    // other way: the field has no idea where it sits.
    const core::DVec3 at{worldAt.x - terrain.origin.x, worldAt.y - terrain.origin.y, worldAt.z - terrain.origin.z};
    const auto radius = static_cast<double>(m_brush.radius);
    const bool box = m_brush.shape == BrushShape::Box;
    // A box the brush's width, so the two shapes cover the same ground and
    // switching between them is a change of edge rather than of size.
    const auto side = static_cast<f32>(radius * 2.0);
    const core::Vec3 extent{side, side, side};

    // **The ground under the stamp, read first** where a streamed terrain has
    // not loaded it yet (terrain audit U1): a brush over a cell still on disk
    // made chunks of only the brush, and the cell's own were lost when it came
    // in -- and on the next save.
    if (m_strokeWorld != nullptr) {
        const double reach = radius + 8.0 * static_cast<double>(terrain.field.settings().voxelSize);
        if (!m_strokeWorld->loadGround(core::DVec3{worldAt.x - reach, 0.0, worldAt.z - reach},
                                       core::DVec3{worldAt.x + reach, 0.0, worldAt.z + reach})) {
            m_status = EditorStatus{groundTooWide(), true};
            return;
        }
    }

    const BrushOp op = m_stroke.has_value() ? m_stroke->op : effectiveBrushOp();
    const core::u8 material = groundMaterial(terrain, m_brush.material);
    asset::EditReport report;
    const auto noted = [this](const asset::EditReport& stamp) {
        if (!m_stroke.has_value())
            return;
        m_stroke->touched += stamp.touched;
        m_stroke->refused = m_stroke->refused || stamp.refused;
        // **The stroke's one undo step, at its first change** (terrain audit
        // E2), from the world as the stroke found it.
        if (stamp.touched > 0 && m_stroke->before.has_value()) {
            m_history.record(std::move(*m_stroke->before), std::move(m_stroke->label), m_stroke->gesture);
            m_stroke->before.reset();
        }
    };

    if (m_tool == Tool::Foliage) {
        // The layer's painted density, not the ground: a stamp brings it back
        // towards what the rules grow or thins it, by the brush's strength.
        if (m_strokeWorld != nullptr) {
            const bool thin = m_stroke.has_value() ? m_stroke->thin : effectiveFoliageThin();
            const f32 amount = std::clamp(m_brush.strength, 0.0f, 1.0f) * 0.35f * (thin ? -1.0f : 1.0f);
            if (paintFoliage(*m_strokeWorld, m_foliageLayer, at, radius, amount, terrain.field.settings().voxelSize)) {
                m_sceneDirty = true;
                report.touched = 1;
            }
        }
        noted(report);
        return;
    }
    if (m_tool == Tool::Paint) {
        // **Paint needs a material to paint** (terrain audit E6): with none, a
        // stroke rewrote ids no layer names, deciding what the ground would
        // become once somebody added one.
        if (terrain.layers.empty()) {
            m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.no_materials")), true};
            return;
        }
        // Its mode, strength and falloff (ADR 0114); Ctrl turns a Blend into
        // an Erase for the stroke.
        const asset::PaintMode mode = m_stroke.has_value() ? m_stroke->paintMode : effectivePaintMode();
        asset::PaintMask mask = m_paintMask;
        mask.heightMin = static_cast<f32>(static_cast<double>(mask.heightMin) - terrain.origin.y);
        mask.heightMax = static_cast<f32>(static_cast<double>(mask.heightMax) - terrain.origin.y);
        report = asset::paintBall(terrain.field, at, radius, material,
                                  asset::PaintOptions{mode, m_brush.strength, m_brush.falloff, mask});
    }
    else {
        switch (op) {
        // **Add and Subtract are volume, centred on the aim** -- the reference
        // editor's, and the owner's words for it: "it models the terrain in
        // circles". Clicked on a field, a ball half in the ground; clicked on
        // the side of a cliff, a ball half in the cliff, never a column down
        // from it. The ground-shaped verb is Grow.
        case BrushOp::Add:
            report = box ? asset::fillBlock(terrain.field, at, extent, material)
                         : asset::fillBall(terrain.field, at, radius, material);
            break;
        case BrushOp::Subtract:
            report =
                box ? asset::fillBlock(terrain.field, at, extent, 0) : asset::fillBall(terrain.field, at, radius, 0);
            break;
        // **Round whichever shape is selected**, as Smooth is: the surface
        // moves along its normal by a falloff, and a square falloff would
        // leave corners on it.
        case BrushOp::Grow:
            report = asset::growBall(terrain.field, at, radius, growAmount(m_brush), material);
            break;
        case BrushOp::Erode:
            report = asset::growBall(terrain.field, at, radius, -growAmount(m_brush));
            break;
        case BrushOp::Smooth:
            // **Round whichever shape is selected.** A square blur leaves
            // visible corners in ground that is supposed to be getting softer.
            report = asset::smoothBall(terrain.field, at, radius, m_brush.strength);
            break;
        case BrushOp::Flatten: {
            // A fixed height when one is set, else where the stroke began.
            const double level = m_flattenHeight.has_value() ? static_cast<double>(*m_flattenHeight)
                                 : m_stroke.has_value()      ? static_cast<double>(m_stroke->plane)
                                                             : worldAt.y;
            report = asset::flattenBall(terrain.field, at, radius, static_cast<f32>(level - terrain.origin.y),
                                        m_brush.strength);
            break;
        }
        }
    }
    noted(report);
    // A stamp that changed nothing changes no revision: the renderer, the
    // physics mirror and the navmesh all key on it.
    if (report.touched == 0)
        return;
    // **Bumped here and nowhere else**, so the renderer and the physics mirror
    // both learn about a stamp through the one path they already read.
    terrain.fieldRevision += 1;
    m_sceneDirty = true;
}

// --- The block world (V1) ------------------------------------------------------

scene::VoxelComponent* Editor::voxelsIn(scene::World& world) noexcept
{
    scene::VoxelComponent* found = nullptr;
    world.voxels().forEach([&found](core::InstanceId, scene::VoxelComponent& voxels) {
        if (found == nullptr)
            found = &voxels;
    });
    return found;
}

asset::BlockId Editor::addBlockType(scene::World& world, Inspector& inspector, std::string_view name, core::Color3 top,
                                    core::Color3 side, core::Color3 bottom)
{
    scene::VoxelComponent* voxels = voxelsIn(world);
    if (voxels == nullptr || name.empty()) {
        m_status = EditorStatus{core::tr(voxels == nullptr ? ENG_TR("engine.editor.status.no_block_world")
                                                           : ENG_TR("engine.editor.status.block_type_needs_a_name")),
                                true};
        return asset::AirBlock;
    }
    (void)inspector;
    const core::NameAtom atom = world.atoms().intern(name);
    for (core::usize at = 0; at < voxels->types.size(); ++at) {
        if (voxels->types[at].name == atom) {
            const auto id = static_cast<asset::BlockId>(at + 1);
            (void)setBlockTypeColors(world, inspector, id, top, side, bottom);
            setBlockType(id);
            return id;
        }
    }
    if (voxels->types.size() >= 65535) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.the_block_world_has_every")), true};
        return asset::AirBlock;
    }
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.add_block_type")));
    // Recorded before the write, so look the component up again: the record
    // does not move pools, but nothing here should depend on that.
    voxels = voxelsIn(world);
    voxels->types.push_back(scene::VoxelBlockType{atom, top, side, bottom, {}, {}, {}, 0, 0.5f});
    voxels->revision += 1;
    const auto id = static_cast<asset::BlockId>(voxels->types.size());
    setBlockType(id);
    m_sceneDirty = true;
    return id;
}

bool Editor::setBlockTypeColors(scene::World& world, Inspector& inspector, asset::BlockId id, core::Color3 top,
                                core::Color3 side, core::Color3 bottom, core::u64 gesture)
{
    (void)inspector;
    scene::VoxelComponent* voxels = voxelsIn(world);
    if (voxels == nullptr || id == asset::AirBlock || id > voxels->types.size())
        return false;
    scene::VoxelBlockType& type = voxels->types[id - 1u];
    if (type.color == top && type.side == side && type.bottom == bottom)
        return false;
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.recolour_block_type")), gesture);
    scene::VoxelBlockType& live = voxelsIn(world)->types[id - 1u];
    live.color = top;
    live.side = side;
    live.bottom = bottom;
    voxelsIn(world)->revision += 1;
    m_sceneDirty = true;
    return true;
}

bool Editor::setBlockTypeLook(scene::World& world, Inspector& inspector, asset::BlockId id,
                              const std::array<core::NameAtom, 3>& textures, core::i32 opacity, f32 transparency,
                              core::u64 gesture)
{
    (void)inspector;
    scene::VoxelComponent* voxels = voxelsIn(world);
    if (voxels == nullptr || id == asset::AirBlock || id > voxels->types.size())
        return false;
    // The script's own rules: an opacity outside the enum is refused there by
    // its type, and here by the range, and see-through is clamped rather than
    // refused because a slider that overshoots is a slider.
    if (opacity < 0 || opacity > 2)
        return false;
    const f32 clamped = std::isfinite(transparency) ? std::clamp(transparency, 0.0f, 1.0f) : 0.5f;
    const scene::VoxelBlockType& type = voxels->types[id - 1u];
    if (type.texture == textures[0] && type.sideTexture == textures[1] && type.bottomTexture == textures[2] &&
        type.opacity == opacity && type.transparency == clamped)
        return false;
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.change_block_look")), gesture);
    scene::VoxelBlockType& live = voxelsIn(world)->types[id - 1u];
    live.texture = textures[0];
    live.sideTexture = textures[1];
    live.bottomTexture = textures[2];
    live.opacity = opacity;
    live.transparency = clamped;
    voxelsIn(world)->revision += 1;
    m_sceneDirty = true;
    return true;
}

bool Editor::clearBlocks(scene::World& world, Inspector& inspector)
{
    (void)inspector;
    scene::VoxelComponent* voxels = voxelsIn(world);
    if (voxels == nullptr || voxels->grid.chunkCount() == 0)
        return false;
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.clear_blocks")));
    voxelsIn(world)->grid.clear();
    m_sceneDirty = true;
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.status.every_block_removed_one_ctrl")), false};
    return true;
}

std::optional<std::array<core::i32, 3>> Editor::blockTarget() const noexcept
{
    if (!m_blockAim.has_value())
        return std::nullopt;
    const BlockAim& aim = *m_blockAim;
    if (m_blockOp == BlockOp::Place) {
        // A ray that started inside a block has no face to place against.
        if (aim.face == std::array<core::i32, 3>{0, 0, 0})
            return std::nullopt;
        return std::array<core::i32, 3>{aim.block[0] + aim.face[0], aim.block[1] + aim.face[1],
                                        aim.block[2] + aim.face[2]};
    }
    // Breaking or replacing the plane is breaking nothing.
    if (aim.onPlane)
        return std::nullopt;
    return aim.block;
}

bool Editor::blockEditChanges(const scene::VoxelComponent& voxels, const std::array<core::i32, 3>& at) const noexcept
{
    const asset::BlockId there = voxels.grid.get(at[0], at[1], at[2]);
    const bool typeKnown = m_blockType != asset::AirBlock && m_blockType <= voxels.types.size();
    switch (m_blockOp) {
    case BlockOp::Place:
        return typeKnown && there != m_blockType;
    case BlockOp::Break:
        return there != asset::AirBlock;
    case BlockOp::Replace:
        return typeKnown && there != asset::AirBlock && there != m_blockType;
    }
    return false;
}

bool Editor::applyBlockAt(scene::VoxelComponent& voxels)
{
    const std::optional<std::array<core::i32, 3>> target = blockTarget();
    if (!target.has_value() || !blockEditChanges(voxels, *target))
        return false;
    const std::array<core::i32, 3>& at = *target;
    if (!voxels.grid.set(at[0], at[1], at[2], m_blockOp == BlockOp::Break ? asset::AirBlock : m_blockType))
        return false;
    // Water beside a broken wall is due to move -- when the world next plays.
    scene::wakeFluids(voxels, at[0], at[1], at[2]);
    return true;
}

bool Editor::driveBlocks(scene::World& world, Inspector& inspector)
{
    m_blockAim.reset();
    scene::VoxelComponent* voxels = voxelsIn(world);
    m_hasVoxels = voxels != nullptr;
    m_voxelBlockSize = voxels != nullptr ? voxels->blockSize : 1.0f;

    if (m_tool != Tool::Blocks || voxels == nullptr) {
        m_blockStroke.reset();
        // A tool with nothing to act on does not eat the click, for the
        // reason `driveSculpt` gives.
        return false;
    }

    const PickRay ray = rayThrough(m_pointer);
    const asset::VoxelGrid& aimAt = m_blockStroke.has_value() ? m_blockStroke->aimGrid : voxels->grid;
    // Aimed through water, as a game's pickaxe is: building a lake bed means
    // pointing at the bed.
    const core::usize typeCount = voxels->types.size() + 1;
    const std::unique_ptr<bool[]> fluids = std::make_unique<bool[]>(typeCount);
    for (core::usize type = 0; type < voxels->types.size(); ++type)
        fluids[type + 1] = voxels->types[type].fluidReach > 0;
    if (const std::optional<asset::VoxelHit> hit =
            asset::raycastVoxels(aimAt, voxels->blockSize, ray.origin, ray.direction, BrushReach,
                                 std::span<const bool>{fluids.get(), typeCount});
        hit.has_value()) {
        m_blockAim = BlockAim{hit->block, hit->face, false};
    }
    else if (std::abs(static_cast<double>(ray.direction.y)) > 1e-6) {
        // **The ground plane, when the ray met no block** -- the block world's
        // version of the brush's plane lock, and for the same reason: an empty
        // block world is one a ray misses, and a tool that cannot place the
        // first block cannot place any.
        const double along = -ray.origin.y / static_cast<double>(ray.direction.y);
        if (along > 0.0 && along <= BrushReach) {
            const double size = static_cast<double>(voxels->blockSize);
            const double x = ray.origin.x + static_cast<double>(ray.direction.x) * along;
            const double z = ray.origin.z + static_cast<double>(ray.direction.z) * along;
            // From above, the plane is the top of the layer below 0; from
            // below, the bottom of the layer at 0.
            const bool fromAbove = ray.direction.y < 0.0f;
            m_blockAim = BlockAim{{static_cast<core::i32>(std::floor(x / size)), fromAbove ? -1 : 0,
                                   static_cast<core::i32>(std::floor(z / size))},
                                  {0, fromAbove ? 1 : -1, 0},
                                  true};
        }
    }

    if (m_blockStroke.has_value() && !m_pointerDown) {
        m_lastBlockEdits = m_blockStroke->edits;
        m_blockStroke.reset();
        m_pending.reset();
        return true;
    }

    if (!m_blockStroke.has_value()) {
        if (!m_pointerPressed || !m_blockAim.has_value())
            return false;
        BlockStroke stroke;
        stroke.aimGrid = voxels->grid;
        m_blockStroke = std::move(stroke);
    }

    // A drag edits each new cell once; holding still over one edits it once.
    if (const std::optional<std::array<core::i32, 3>> target = blockTarget();
        target.has_value() && target != m_blockStroke->last) {
        m_blockStroke->last = target;
        if (blockEditChanges(*voxels, *target)) {
            // **Recorded on the first edit that changes something**, not on
            // the press: a click that breaks the empty plane changes nothing,
            // and an undo step that undoes nothing is a step somebody presses
            // ctrl-Z through wondering what it was.
            if (m_blockStroke->gesture == 0) {
                m_blockStroke->gesture = inspector.beginGesture();
                const char* label =
                    core::tr(m_blockOp == BlockOp::Break     ? ENG_TR("engine.editor.history.break_block")
                             : m_blockOp == BlockOp::Replace ? ENG_TR("engine.editor.history.replace_block")
                                                             : ENG_TR("engine.editor.history.place_block"));
                m_history.record(world, label, m_blockStroke->gesture);
                voxels = voxelsIn(world);
            }
            (void)applyBlockAt(*voxels);
            m_blockStroke->edits += 1;
            m_sceneDirty = true;
        }
    }
    m_pending.reset();
    return true;
}

// --- Making ground exist -----------------------------------------------------

core::InstanceId Editor::workspaceUnder(const scene::World& world, core::InstanceId root) const
{
    if (!root.valid()) {
        return {};
    }
    if (world.workspaces().find(root) != nullptr) {
        return root;
    }
    for (core::InstanceId child = world.firstChild(root); child.valid(); child = world.nextSibling(child)) {
        if (world.workspaces().find(child) != nullptr) {
            return child;
        }
    }
    return {};
}

core::InstanceId Editor::terrainIn(const scene::World& world, core::InstanceId root) const
{
    return terrainUnder(world, workspaceUnder(world, root));
}

core::InstanceId Editor::createTerrain(scene::World& world, core::InstanceId rootOrWorkspace, Inspector& inspector)
{
    // **Resolved here rather than trusted from the caller**, because the shell's
    // panels hold the Explorer's root and that is the `DataModel`.
    const core::InstanceId root = workspaceUnder(world, rootOrWorkspace);
    if (const core::InstanceId existing = terrainUnder(world, root); existing.valid()) {
        // Already there. Selecting it is more useful than refusing: somebody who
        // pressed the button wants to be looking at the terrain either way.
        inspector.select(existing);
        return existing;
    }
    if (!root.valid()) {
        return {};
    }

    const scene::ClassId terrainClass = world.classes().findId(world.atoms().intern("Terrain"));
    if (terrainClass == scene::InvalidClass) {
        return {};
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.create_terrain")));
    const core::InstanceId id = world.create(terrainClass);
    if (!id.valid()) {
        return {};
    }
    world.setName(id, world.atoms().intern("Terrain"));
    if (world.setParent(id, root).has_value()) {
        world.destroy(id);
        return {};
    }

    inspector.select(id);
    m_sceneDirty = true;
    return id;
}

bool Editor::generateGround(scene::World& world, core::InstanceId rootOrWorkspace, Inspector& inspector, f32 size,
                            f32 height, core::u8 material)
{
    if (!(size > 0.0f) || material == 0)
        return false;
    // **One step, whether or not the terrain was there**: a first Create used
    // to be two, Create Terrain and Generate Ground, and the first ctrl-Z left
    // an empty terrain behind.
    const bool existed = terrainIn(world, rootOrWorkspace).valid();
    const core::InstanceId id = createTerrain(world, rootOrWorkspace, inspector);
    scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    if (terrain == nullptr)
        return false;
    if (existed)
        m_history.record(world, core::tr(ENG_TR("engine.editor.history.generate_ground")));

    // **Laid with `fillFlat` rather than carved as a box.** Ground reaching the
    // world's floor is thousands of voxels a column; `fillFlat` lays the chunks
    // the ground covers entirely as one value each, and writes voxels only
    // where the surface passes. `height` is a WORLD height, as a heightmap's
    // are: the field is laid at it less the terrain's own position.
    //
    // **Every cell of the square read first** on a streamed terrain (terrain
    // audit U2): laid over cells not loaded, the old ground above the new
    // height came back with them, in blocks a chunk wide.
    {
        const double half =
            static_cast<double>(size) / 2.0 + 8.0 * static_cast<double>(terrain->field.settings().voxelSize);
        if (!world.loadGround(core::DVec3{terrain->origin.x - half, 0.0, terrain->origin.z - half},
                              core::DVec3{terrain->origin.x + half, 0.0, terrain->origin.z + half})) {
            // Refused before a voxel moved (TA16): the step recorded goes too.
            backOutGround(world, inspector, existed, id);
            m_status = EditorStatus{groundTooWide(), true};
            return false;
        }
    }
    const asset::EditReport laid = asset::fillFlat(terrain->field, core::DVec3{0.0, 0.0, 0.0}, size,
                                                   static_cast<f32>(static_cast<double>(height) - terrain->origin.y),
                                                   groundMaterial(*terrain, material));
    // **Too big for its voxels is said, and leaves no step** (terrain audit
    // E6): it recorded "Generate Ground", answered true and changed nothing.
    if (laid.refused) {
        backOutGround(world, inspector, existed, id);
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.square_too_big")), true};
        return false;
    }

    terrain->fieldRevision += 1;
    m_sceneDirty = true;
    return true;
}

bool Editor::generateHills(scene::World& world, core::InstanceId rootOrWorkspace, Inspector& inspector,
                           const HillSpec& spec)
{
    if (!(spec.size > 0.0f) || !std::isfinite(spec.low) || !std::isfinite(spec.high) || !(spec.high > spec.low) ||
        !(spec.scale > 0.0f) || spec.material == 0) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.hills_spec")), true};
        return false;
    }
    const core::InstanceId existing = terrainIn(world, rootOrWorkspace);
    const scene::TerrainComponent* before = existing.valid() ? world.terrains().find(existing) : nullptr;
    const f32 voxel = before != nullptr ? before->field.settings().voxelSize : asset::FieldSettings{}.voxelSize;
    const double across = std::round(static_cast<double>(spec.size) / static_cast<double>(voxel)) + 1.0;
    if (across > static_cast<double>(MaxImportColumns)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.too_many_columns")), true};
        return false;
    }
    // **Past one table, a tile at a time** (ADR 0149 §2).
    if (across > static_cast<double>(MaxTableColumns)) {
        const auto columns = static_cast<core::u32>(across);
        return beginTerrainImport(
            world, rootOrWorkspace, inspector, columns, columns, spec.material,
            core::tr(ENG_TR("engine.editor.terrain.create.hills")),
            [spec](const scene::TerrainComponent& terrain, const TerrainImportPlan&, std::string&) {
                const auto originY = static_cast<f32>(terrain.origin.y);
                return hillSource(
                    terrain.field.settings(),
                    asset::HillSettings{spec.seed, spec.octaves, spec.scale, spec.low - originY, spec.high - originY});
            });
    }
    // **One step, whether or not the terrain was there** (the terrain audit's
    // editor list): on a new world the terrain's making is the step.
    const bool existed = existing.valid();
    const core::InstanceId id = createTerrain(world, rootOrWorkspace, inspector);
    if (world.terrains().find(id) == nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.nowhere")), true};
        return false;
    }
    if (existed)
        m_history.record(world, core::tr(ENG_TR("engine.editor.history.generate_hills")));
    scene::TerrainComponent& terrain = *world.terrains().find(id);
    const auto columns = static_cast<core::u32>(across);
    const double half = 0.5 * static_cast<double>(columns - 1u) * static_cast<double>(voxel);
    const core::i32 first = terrain.field.voxelIndex(-half);
    // Every cell of the square read first on a streamed terrain (terrain
    // audit U2's reason).
    if (!world.loadGround(core::DVec3{terrain.origin.x - half - 8.0 * static_cast<double>(voxel), 0.0,
                                      terrain.origin.z - half - 8.0 * static_cast<double>(voxel)},
                          core::DVec3{terrain.origin.x + half + 8.0 * static_cast<double>(voxel), 0.0,
                                      terrain.origin.z + half + 8.0 * static_cast<double>(voxel)})) {
        backOutGround(world, inspector, existed, id);
        m_status = EditorStatus{groundTooWide(), true};
        return false;
    }
    const auto originY = static_cast<f32>(terrain.origin.y);
    const std::vector<float> heights = asset::hillHeights(
        terrain.field, first, first, columns, columns,
        asset::HillSettings{spec.seed, spec.octaves, spec.scale, spec.low - originY, spec.high - originY});
    // **A refusal is read** (the editor list): heights too steep to lay are
    // refused whole by `writeHeights`, and this answered true with nothing laid.
    if (asset::writeHeights(terrain.field, first, first, columns, heights, groundMaterial(terrain, spec.material))
            .refused) {
        backOutGround(world, inspector, existed, id);
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.hills_too_steep")), true};
        return false;
    }
    terrain.fieldRevision += 1;
    m_sceneDirty = true;
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.terrain.status.hills_laid"), {{"columns", static_cast<core::i64>(columns)}}),
        false};
    return true;
}

void Editor::backOutGround(scene::World& world, Inspector& inspector, bool existed, core::InstanceId terrain)
{
    if (!existed && terrain.valid()) {
        // The terrain it made, and the step that made it.
        if (inspector.selection() == terrain)
            inspector.clearSelection();
        world.destroy(terrain);
        world.retireDestroyed();
    }
    m_history.retract();
}

std::optional<std::filesystem::path> Editor::exportHeightmap(const scene::World& world, core::InstanceId root, f32 size)
{
    const core::InstanceId id = terrainIn(world, root);
    const scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    if (terrain == nullptr || terrain->field.empty() || !(size > 0.0f)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.nothing_to_export")), true};
        return std::nullopt;
    }
    if (m_content.root().empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.export_needs_project")), true};
        return std::nullopt;
    }
    const asset::FieldSettings& settings = terrain->field.settings();
    const double voxel = static_cast<double>(settings.voxelSize);
    const double across = std::round(static_cast<double>(size) / voxel) + 1.0;
    if (across > 4096.0) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.export_too_many_columns")), true};
        return std::nullopt;
    }
    const auto columns = static_cast<core::u32>(across);
    const double half = 0.5 * static_cast<double>(columns - 1u) * voxel;
    // **Every cell of the square read first** (terrain audit TA17c): the
    // export had black holes wherever a streamed cell was not loaded.
    if (!world.loadGround(core::DVec3{terrain->origin.x - half, 0.0, terrain->origin.z - half},
                          core::DVec3{terrain->origin.x + half, 0.0, terrain->origin.z + half})) {
        m_status = EditorStatus{groundTooWide(), true};
        return std::nullopt;
    }
    const core::i32 first = terrain->field.voxelIndex(-half);
    const std::vector<float> tops =
        asset::columnHeights(terrain->field, first, first, columns, columns, settings.minHeight);
    // The world's floor black and its ceiling white, in field metres.
    asset::HeightImage image;
    image.width = columns;
    image.height = columns;
    image.samples.resize(tops.size());
    const float range = settings.maxHeight - settings.minHeight;
    for (std::size_t at = 0; at < tops.size(); ++at)
        image.samples[at] = (tops[at] - settings.minHeight) / range;
    const std::filesystem::path folder = m_content.root().parent_path() / "heightmaps";
    const std::string name = world.atoms().text(world.name(id)).empty()
                                 ? std::string("terrain")
                                 : std::string(world.atoms().text(world.name(id)));
    // **Never over an earlier export** (the editor list): a second export
    // wrote over the first without a word. The next free name instead --
    // `Terrain`, `Terrain-2`, ... -- and the status says which.
    std::string stem = name;
    for (int suffix = 2;
         platform::fileExists(folder / (stem + ".png")) || platform::fileExists(folder / (stem + ".r16")) ||
         platform::fileExists(folder / (stem + ".heightmap.json"));
         ++suffix)
        stem = name + "-" + std::to_string(suffix);
    const auto floor = static_cast<double>(settings.minHeight) + terrain->origin.y;
    const auto ceiling = static_cast<double>(settings.maxHeight) + terrain->origin.y;
    // What the image's black and white stand for, and how wide it was taken:
    // what an import needs to give the same ground back.
    core::JsonWriter hint;
    hint.beginObject();
    hint.field("size", static_cast<double>(columns - 1u) * voxel);
    hint.field("black", floor);
    hint.field("white", ceiling);
    hint.endObject();
    std::vector<std::byte> png;
    std::vector<std::byte> raw;
    if (asset::encodeHeightmap(image, asset::HeightmapFormat::Png16, png).has_value() ||
        asset::encodeHeightmap(image, asset::HeightmapFormat::Raw16, raw).has_value() ||
        !platform::createDirectories(folder) || !platform::writeFile(folder / (stem + ".png"), png) ||
        !platform::writeFile(folder / (stem + ".r16"), raw) ||
        !platform::writeTextFile(folder / (stem + ".heightmap.json"), hint.text())) {
        m_status = EditorStatus{
            core::tr(ENG_TR("engine.editor.terrain.status.export_failed"), {{"folder", folder.string()}}), true};
        return std::nullopt;
    }
    char black[32];
    char white[32];
    (void)std::snprintf(black, sizeof(black), "%.1f", floor);
    (void)std::snprintf(white, sizeof(white), "%.1f", ceiling);
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.terrain.status.exported"), {{"name", stem},
                                                                   {"columns", static_cast<core::i64>(columns)},
                                                                   {"black", std::string_view(black)},
                                                                   {"white", std::string_view(white)}}),
        false};
    return folder / (stem + ".png");
}

void Editor::setHeightmapSource(std::filesystem::path source)
{
    m_heightmapSource = std::move(source);
    m_heightmapHint.reset();
    std::filesystem::path beside = m_heightmapSource;
    beside.replace_extension(".heightmap.json");
    std::string text;
    core::JsonDocument document;
    if (m_heightmapSource.empty() || !platform::readTextFile(beside, text) || !document.parse(text).ok)
        return;
    const core::JsonValue root = document.root();
    const core::JsonValue size = root["size"];
    const core::JsonValue black = root["black"];
    const core::JsonValue white = root["white"];
    if (size.type() != core::JsonType::Number || black.type() != core::JsonType::Number ||
        white.type() != core::JsonType::Number)
        return;
    const HeightmapHint hint{static_cast<f32>(size.asNumber()), static_cast<f32>(black.asNumber()),
                             static_cast<f32>(white.asNumber())};
    if (std::isfinite(hint.size) && std::isfinite(hint.low) && std::isfinite(hint.high) && hint.size > 0.0f &&
        hint.low < hint.high)
        m_heightmapHint = hint;
}

bool Editor::replaceMaterialEverywhere(scene::World& world, core::InstanceId root, core::u8 from, core::u8 to)
{
    const core::InstanceId id = terrainIn(world, root);
    scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    if (terrain == nullptr || from == 0 || to == 0 || from == to || terrain->field.empty())
        return false;
    // **All of the terrain, not what is loaded** (terrain audit TA17c): its
    // streamed cells read first, or the whole action refused untouched.
    if (!loadAllGround(world)) {
        m_status = EditorStatus{groundTooWide(), true};
        return false;
    }
    scene::WorldSnapshot before = world.snapshot();
    const asset::EditReport report = overChunks(terrain->field, [&](core::DVec3 low, core::DVec3 high) {
        return asset::replaceMaterial(terrain->field, low, high, from, to);
    });
    if (report.refused && report.touched == 0) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.replace_too_big")), true};
        return false;
    }
    if (report.touched == 0) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.replace_none")), false};
        return false;
    }
    m_history.record(std::move(before), core::tr(ENG_TR("engine.editor.history.replace_material")));
    terrain->fieldRevision += 1;
    m_sceneDirty = true;
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.terrain.status.replaced"), {{"count", static_cast<core::i64>(report.touched)}}),
        false};
    return true;
}

bool Editor::clearTerrain(scene::World& world, core::InstanceId root, Inspector& inspector)
{
    (void)inspector;
    const core::InstanceId id = terrainIn(world, root);
    scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    if (terrain == nullptr) {
        return false;
    }
    if (terrain->field.empty()) {
        // Nothing to clear. Refused rather than recorded, because a step that
        // undoes nothing eats a press of ctrl-Z.
        return false;
    }

    m_history.record(world, core::tr(ENG_TR("engine.editor.history.clear_all_ground")));
    terrain->field = asset::TerrainField(terrain->field.settings());
    // **And its cells**, on a streamed terrain (terrain audit U2): cleared
    // only in memory, every cell not loaded streamed back in, and those let
    // go before a save came back after it. With no index the terrain is one
    // the scene holds, which it now is; an undo puts the index back.
    terrain->cellIndex.clear();
    terrain->fieldRevision += 1;
    m_sceneDirty = true;
    return true;
}

core::InstanceId Editor::createFoliageLayer(scene::World& world, core::InstanceId root, Inspector& inspector)
{
    const core::InstanceId terrain = terrainIn(world, root);
    const scene::ClassId layerClass = world.classes().findId(world.atoms().intern("FoliageLayer"));
    if (!terrain.valid() || layerClass == scene::InvalidClass)
        return {};
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.add_foliage_layer")));
    const core::InstanceId id = world.create(layerClass);
    if (!id.valid())
        return {};
    world.setName(id, world.atoms().intern("FoliageLayer"));
    if (world.setParent(id, terrain).has_value()) {
        world.destroy(id);
        return {};
    }
    m_foliageLayer = id;
    inspector.select(id);
    m_sceneDirty = true;
    return id;
}

core::InstanceId Editor::addFoliageMesh(scene::World& world, core::InstanceId layer, Inspector& inspector)
{
    const scene::ClassId meshClass = world.classes().findId(world.atoms().intern("FoliageMesh"));
    if (world.foliageLayers().find(layer) == nullptr || meshClass == scene::InvalidClass)
        return {};
    m_history.record(world, core::tr(ENG_TR("engine.editor.history.add_foliage_mesh")));
    const core::InstanceId id = world.create(meshClass);
    if (!id.valid())
        return {};
    world.setName(id, world.atoms().intern("FoliageMesh"));
    if (world.setParent(id, layer).has_value()) {
        world.destroy(id);
        return {};
    }
    inspector.select(id);
    m_sceneDirty = true;
    return id;
}

bool Editor::paintFoliage(scene::World& world, core::InstanceId layerId, core::DVec3 fieldAt, double radius, f32 amount,
                          f32 voxelSize)
{
    scene::FoliageLayerComponent* layer = world.foliageLayers().find(layerId);
    if (layer == nullptr || !(radius > 0.0) || !(voxelSize > 0.0f) || amount == 0.0f)
        return false;
    constexpr auto edge = static_cast<core::i32>(asset::ChunkEdge);
    const auto voxel = static_cast<double>(voxelSize);
    const auto lowX = static_cast<core::i32>(std::floor((fieldAt.x - radius) / voxel));
    const auto highX = static_cast<core::i32>(std::floor((fieldAt.x + radius) / voxel));
    const auto lowZ = static_cast<core::i32>(std::floor((fieldAt.z - radius) / voxel));
    const auto highZ = static_cast<core::i32>(std::floor((fieldAt.z + radius) / voxel));
    bool changed = false;
    for (core::i32 z = lowZ; z <= highZ; ++z) {
        for (core::i32 x = lowX; x <= highX; ++x) {
            const double dx = (static_cast<double>(x) + 0.5) * voxel - fieldAt.x;
            const double dz = (static_cast<double>(z) + 0.5) * voxel - fieldAt.z;
            const double distance = std::sqrt(dx * dx + dz * dz);
            if (distance > radius)
                continue;
            // A smooth falloff, full at the centre and nothing at the rim.
            const double t = 1.0 - distance / radius;
            const double weight = t * t * (3.0 - 2.0 * t);
            const core::i32 tileX = asset::floorDiv(x, edge);
            const core::i32 tileZ = asset::floorDiv(z, edge);
            scene::FoliageMaskColumn* column = nullptr;
            for (scene::FoliageMaskColumn& candidate : layer->mask) {
                if (candidate.x == tileX && candidate.z == tileZ)
                    column = &candidate;
            }
            if (column == nullptr) {
                // Thinning an untouched column makes its entry; restoring one
                // that has none changes nothing.
                if (amount > 0.0f)
                    continue;
                layer->mask.push_back(scene::FoliageMaskColumn{
                    tileX, tileZ, std::vector<core::u8>(static_cast<core::usize>(edge * edge), 255)});
                column = &layer->mask.back();
            }
            const auto at = static_cast<core::usize>(asset::floorMod(z, edge) * edge + asset::floorMod(x, edge));
            const double next = std::clamp(
                static_cast<double>(column->density[at]) + weight * static_cast<double>(amount) * 255.0, 0.0, 255.0);
            const auto value = static_cast<core::u8>(std::lround(next));
            if (value != column->density[at]) {
                column->density[at] = value;
                changed = true;
            }
        }
    }
    // A column painted back to full everywhere is no column at all.
    std::erase_if(layer->mask, [](const scene::FoliageMaskColumn& column) {
        return std::all_of(column.density.begin(), column.density.end(), [](core::u8 v) { return v == 255; });
    });
    return changed;
}

bool Editor::setTerrainLayers(scene::World& world, core::InstanceId root, std::vector<std::string> layers,
                              std::string_view label)
{
    const core::InstanceId id = terrainIn(world, root);
    scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    if (terrain == nullptr || layers.size() > asset::MaxTerrainLayers || terrain->layers == layers)
        return false;
    m_history.record(world, std::string(label));
    terrain->layers = std::move(layers);
    terrain->layersRevision += 1;
    m_sceneDirty = true;
    return true;
}

core::usize Editor::writeStarterTerrainMaterials()
{
    core::usize written = 0;
    for (core::u8 index = 1;; ++index) {
        const std::string engine = asset::engineTerrainUrn(index);
        if (engine.empty())
            break;
        const std::string name = engine.substr(asset::EngineTerrainPrefix.size());
        const std::string relative = "materials/terrain/" + name + std::string(asset::MaterialSuffix);
        const std::filesystem::path absolute = m_content.root() / std::filesystem::path(relative);
        std::error_code ec;
        if (std::filesystem::exists(absolute, ec))
            continue;
        asset::MaterialAsset variant;
        variant.parent = engine;
        if (!writeMaterialFile(absolute, variant)) {
            m_status = EditorStatus{
                core::tr(ENG_TR("engine.editor.terrain.status.could_not_write"), {{"path", relative}}), true};
            return written;
        }
        ++written;
    }
    (void)m_content.refresh();
    m_status = EditorStatus{written > 0 ? core::tr(ENG_TR("engine.editor.terrain.status.starters_written"),
                                                   {{"count", static_cast<core::i64>(written)}})
                                        : std::string(core::tr(ENG_TR("engine.editor.terrain.status.starters_there"))),
                            false};
    return written;
}

namespace {

// A content-relative material path as a terrain layer names it, or empty for
// anything that is not a material file.
[[nodiscard]] std::string terrainLayerUrn(std::string_view materialPath)
{
    std::string path(materialPath);
    if (path.starts_with(asset::AssetScheme))
        path = path.substr(asset::AssetScheme.size());
    if (path.empty() || !path.ends_with(asset::MaterialSuffix))
        return {};
    return std::string(asset::AssetScheme) + path;
}

} // namespace

bool Editor::useStarterTerrainMaterials(scene::World& world, core::InstanceId root)
{
    const core::InstanceId id = terrainIn(world, root);
    const scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    if (terrain == nullptr)
        return false;
    if (!terrain->layers.empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.starters_has_materials")), true};
        return false;
    }
    if (m_content.root().empty()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.starters_need_project")), true};
        return false;
    }
    (void)writeStarterTerrainMaterials();
    if (m_status.value().failed)
        return false;
    std::vector<std::string> layers;
    for (core::u8 index = 1;; ++index) {
        const std::string engine = asset::engineTerrainUrn(index);
        if (engine.empty() || layers.size() >= asset::MaxTerrainLayers)
            break;
        layers.push_back(terrainLayerUrn("materials/terrain/" + engine.substr(asset::EngineTerrainPrefix.size()) +
                                         std::string(asset::MaterialSuffix)));
    }
    const std::size_t count = layers.size();
    if (!setTerrainLayers(world, root, std::move(layers),
                          core::tr(ENG_TR("engine.editor.history.use_starter_materials"))))
        return false;
    setBrushMaterial(1);
    m_status = EditorStatus{
        core::tr(ENG_TR("engine.editor.terrain.status.starters_given"), {{"count", static_cast<core::i64>(count)}}),
        false};
    return true;
}

bool Editor::addTerrainLayer(scene::World& world, core::InstanceId root, std::string_view materialPath)
{
    const core::InstanceId id = terrainIn(world, root);
    const scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    const std::string urn = terrainLayerUrn(materialPath);
    if (terrain == nullptr || urn.empty())
        return false;
    if (std::find(terrain->layers.begin(), terrain->layers.end(), urn) != terrain->layers.end()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.has_material")), true};
        return false;
    }
    if (terrain->layers.size() >= asset::MaxTerrainLayers) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.too_many_materials"),
                                         {{"count", static_cast<core::i64>(asset::MaxTerrainLayers)}}),
                                true};
        return false;
    }
    std::vector<std::string> layers = terrain->layers;
    layers.push_back(urn);
    const auto added = static_cast<core::u8>(layers.size());
    if (!setTerrainLayers(world, root, std::move(layers),
                          core::tr(ENG_TR("engine.editor.history.add_terrain_material"))))
        return false;
    setBrushMaterial(added);
    return true;
}

bool Editor::replaceTerrainLayer(scene::World& world, core::InstanceId root, core::u8 index,
                                 std::string_view materialPath)
{
    const core::InstanceId id = terrainIn(world, root);
    const scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    const std::string urn = terrainLayerUrn(materialPath);
    if (terrain == nullptr || urn.empty() || index < 1 || index > terrain->layers.size())
        return false;
    if (terrain->layers[index - 1] == urn)
        return false;
    if (std::find(terrain->layers.begin(), terrain->layers.end(), urn) != terrain->layers.end()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.has_material")), true};
        return false;
    }
    std::vector<std::string> layers = terrain->layers;
    layers[index - 1] = urn;
    if (!setTerrainLayers(world, root, std::move(layers),
                          core::tr(ENG_TR("engine.editor.history.replace_terrain_material"))))
        return false;
    setBrushMaterial(index);
    return true;
}

bool Editor::removeLastTerrainLayer(scene::World& world, core::InstanceId root)
{
    const core::InstanceId id = terrainIn(world, root);
    const scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    if (terrain == nullptr || terrain->layers.empty())
        return false;
    std::vector<std::string> layers = terrain->layers;
    layers.pop_back();
    const auto remaining = static_cast<core::u8>(layers.size());
    if (!setTerrainLayers(world, root, std::move(layers),
                          core::tr(ENG_TR("engine.editor.history.remove_terrain_material"))))
        return false;
    if (m_brush.material > remaining)
        setBrushMaterial(std::max<core::u8>(remaining, 1));
    return true;
}

bool Editor::setTerrainRules(scene::World& world, core::InstanceId root, std::vector<asset::TerrainRule> rules,
                             std::string_view label, core::u64 coalesceKey)
{
    const core::InstanceId id = terrainIn(world, root);
    scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    // **A rule's bounds kept in order** (the terrain audit's editor list): a
    // lowest past its highest drew nothing, and nothing said why.
    for (asset::TerrainRule& rule : rules) {
        if (rule.slopeMin > rule.slopeMax)
            std::swap(rule.slopeMin, rule.slopeMax);
        if (rule.heightMin > rule.heightMax)
            std::swap(rule.heightMin, rule.heightMax);
    }
    if (terrain == nullptr || rules.size() > asset::MaxTerrainRules || terrain->rules == rules)
        return false;
    m_history.record(world, std::string(label), coalesceKey);
    terrain->rules = std::move(rules);
    m_sceneDirty = true;
    return true;
}

bool Editor::applyTerrainRules(scene::World& world, core::InstanceId root)
{
    const core::InstanceId id = terrainIn(world, root);
    scene::TerrainComponent* terrain = id.valid() ? world.terrains().find(id) : nullptr;
    if (terrain == nullptr || terrain->field.empty() || terrain->rules.empty())
        return false;
    // All of it, streamed cells too (terrain audit TA17c).
    if (!loadAllGround(world)) {
        m_status = EditorStatus{groundTooWide(), true};
        return false;
    }
    // Recorded once something changed: a record clears what redo would bring
    // back, and a pass that changed nothing must leave it (the editor list: it
    // was backed out with an undo, which spent the redo).
    scene::WorldSnapshot before = world.snapshot();
    const asset::EditReport report = overChunks(terrain->field, [&](core::DVec3 low, core::DVec3 high) {
        return asset::applyRules(terrain->field, terrain->rules, low, high, terrain->origin.y);
    });
    if (report.touched == 0) {
        // A refusal is not "nothing to do" (the editor list).
        m_status = report.refused ? EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.rules_too_big")), true}
                                  : EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.rules_nothing")), false};
        return false;
    }
    m_history.record(std::move(before), core::tr(ENG_TR("engine.editor.history.apply_terrain_rules")));
    terrain->fieldRevision += 1;
    m_sceneDirty = true;
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.rules_applied"),
                                     {{"count", static_cast<core::i64>(report.touched)}}),
                            false};
    return true;
}

bool Editor::importHeightmap(scene::World& world, core::InstanceId rootOrWorkspace, Inspector& inspector,
                             const HeightmapImport& spec)
{
    if (spec.source.empty() || !(spec.size > 0.0f) || !std::isfinite(spec.low) || !std::isfinite(spec.high) ||
        spec.material == 0) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_spec")), true};
        return false;
    }
    const std::string name = spec.source.filename().string();

    // **A folder of tiles, or more columns than one table holds, is laid a
    // tile at a time** (ADR 0149 §2).
    {
        const core::InstanceId present = terrainIn(world, rootOrWorkspace);
        const scene::TerrainComponent* held = present.valid() ? world.terrains().find(present) : nullptr;
        const f32 voxelSize = held != nullptr ? held->field.settings().voxelSize : asset::FieldSettings{}.voxelSize;
        const double wide = std::round(static_cast<double>(spec.size) / static_cast<double>(voxelSize)) + 1.0;
        std::error_code ignored;
        if (std::filesystem::is_directory(spec.source, ignored) || isHeightmapPiece(spec.source) ||
            wide > static_cast<double>(MaxTableColumns)) {
            if (wide > static_cast<double>(MaxImportColumns)) {
                m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_too_many_columns")), true};
                return false;
            }
            std::string error;
            std::shared_ptr<HeightmapSource> opened = openHeightmap(spec.source, error);
            if (opened == nullptr) {
                m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.not_a_heightmap"),
                                                 {{"name", name}, {"reason", error}}),
                                        true};
                return false;
            }
            const HeightmapSize pixels = opened->size();
            const auto columns = static_cast<core::u32>(wide);
            const auto rows = std::clamp<core::u32>(
                static_cast<core::u32>(std::lround(static_cast<double>(columns) * pixels.height / pixels.width)), 1u,
                MaxImportColumns);
            // Handed to the job whole: the opener gives it away once.
            return beginTerrainImport(
                world, rootOrWorkspace, inspector, columns, rows, spec.material, name,
                [opened, spec](const scene::TerrainComponent& terrain, const TerrainImportPlan& plan, std::string&) {
                    const auto originY = static_cast<f32>(terrain.origin.y);
                    opened->map(plan, spec.low - originY, spec.high - originY);
                    // A view of it: the shared pointer keeps the source alive.
                    struct Shared final : HeightSource
                    {
                        std::shared_ptr<HeightmapSource> held;
                        bool read(core::i32 x, core::i32 z, core::u32 width, core::u32 depth, std::span<float> out,
                                  std::string& why) override
                        {
                            return held->read(x, z, width, depth, out, why);
                        }
                    };
                    auto view = std::make_unique<Shared>();
                    view->held = opened;
                    return std::unique_ptr<HeightSource>(std::move(view));
                });
        }
    }

    // **Read and decoded before anything is recorded**, so a file that is not a
    // heightmap refuses with the world untouched and no undo step to wade past.
    std::vector<std::byte> bytes;
    if (!platform::readFile(spec.source, bytes)) {
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.could_not_read"), {{"name", name}}), true};
        return false;
    }
    asset::HeightImage image;
    if (const std::optional<core::EngineError> error = asset::decodeHeightmap(bytes, name, image); error.has_value()) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.not_a_heightmap"),
                                         {{"name", name}, {"reason", error->message}}),
                                true};
        return false;
    }

    // One column per voxel across the square, corner to corner, and as many
    // rows as keep the image's proportions. Capped where `WriteHeights` caps:
    // past it the ask is a mistake in the size, not a larger world.
    const core::InstanceId existing = terrainIn(world, rootOrWorkspace);
    const scene::TerrainComponent* before = existing.valid() ? world.terrains().find(existing) : nullptr;
    const f32 voxel = before != nullptr ? before->field.settings().voxelSize : asset::FieldSettings{}.voxelSize;
    constexpr core::u32 MaxColumns = MaxTableColumns;
    const double across = std::round(static_cast<double>(spec.size) / static_cast<double>(voxel)) + 1.0;
    const auto columns = static_cast<core::u32>(across);
    const auto rows = std::max<core::u32>(
        1u, static_cast<core::u32>(std::lround(static_cast<double>(columns) * image.height / image.width)));
    if (rows > MaxColumns) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_too_tall")), true};
        return false;
    }

    const bool existed = existing.valid();
    const core::InstanceId id = createTerrain(world, rootOrWorkspace, inspector);
    if (world.terrains().find(id) == nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.nowhere")), true};
        return false;
    }
    if (existed)
        m_history.record(world, core::tr(ENG_TR("engine.editor.history.import_heightmap")));
    scene::TerrainComponent& terrain = *world.terrains().find(id);

    // Into the field's own space, which is the world's less the terrain's
    // origin, exactly as the script verb does it.
    const auto originY = static_cast<f32>(terrain.origin.y);
    const std::vector<float> heights =
        asset::resampleHeights(image, columns, rows, spec.low - originY, spec.high - originY);
    const double half = 0.5 * static_cast<double>(columns - 1u) * static_cast<double>(voxel);
    const double halfRows = 0.5 * static_cast<double>(rows - 1u) * static_cast<double>(voxel);
    const auto firstX = static_cast<core::i32>(std::floor(-half / static_cast<double>(voxel)));
    const auto firstZ = static_cast<core::i32>(std::floor(-halfRows / static_cast<double>(voxel)));
    // Every cell of the square read first on a streamed terrain, for Replace
    // with Flat Ground's reason (terrain audit U2).
    {
        const double margin = 8.0 * static_cast<double>(voxel);
        if (!world.loadGround(
                core::DVec3{terrain.origin.x - half - margin, 0.0, terrain.origin.z - halfRows - margin},
                core::DVec3{terrain.origin.x + half + margin, 0.0, terrain.origin.z + halfRows + margin})) {
            backOutGround(world, inspector, existed, id);
            m_status = EditorStatus{groundTooWide(), true};
            return false;
        }
    }
    if (asset::writeHeights(terrain.field, firstX, firstZ, columns, heights, groundMaterial(terrain, spec.material))
            .refused) {
        backOutGround(world, inspector, existed, id);
        m_status =
            EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_too_steep"), {{"name", name}}), true};
        return false;
    }
    terrain.fieldRevision += 1;
    m_sceneDirty = true;

    const asset::FieldSettings& settings = terrain.field.settings();
    const bool clamped = std::min(spec.low, spec.high) - originY < settings.minHeight ||
                         std::max(spec.low, spec.high) - originY > settings.maxHeight;
    const core::I18nArg args[] = {{"name", name},
                                  {"width", static_cast<core::i64>(image.width)},
                                  {"height", static_cast<core::i64>(image.height)},
                                  {"columns", static_cast<core::i64>(columns)},
                                  {"rows", static_cast<core::i64>(rows)}};
    m_status =
        EditorStatus{core::engineCatalog().format(clamped ? ENG_TR("engine.editor.terrain.status.imported_clamped")
                                                          : ENG_TR("engine.editor.terrain.status.imported"),
                                                  args),
                     false};
    return true;
}

bool Editor::importFunction(scene::World& world, core::InstanceId rootOrWorkspace, Inspector& inspector,
                            const FunctionImport& spec)
{
    if (spec.source.empty() || !(spec.size > 0.0f) || spec.material == 0 || !m_heightFunctionLoader) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_spec")), true};
        return false;
    }
    const core::InstanceId present = terrainIn(world, rootOrWorkspace);
    const scene::TerrainComponent* held = present.valid() ? world.terrains().find(present) : nullptr;
    const f32 voxel = held != nullptr ? held->field.settings().voxelSize : asset::FieldSettings{}.voxelSize;
    const double across = std::round(static_cast<double>(spec.size) / static_cast<double>(voxel)) + 1.0;
    if (across > static_cast<double>(MaxImportColumns)) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_too_many_columns")), true};
        return false;
    }
    const auto columns = static_cast<core::u32>(across);
    const std::string name = spec.source.filename().string();
    return beginTerrainImport(
        world, rootOrWorkspace, inspector, columns, columns, spec.material, name,
        [this, spec](const scene::TerrainComponent& terrain, const TerrainImportPlan&, std::string& error) {
            return m_heightFunctionLoader(spec.source, terrain.origin, terrain.field.settings().voxelSize, error);
        });
}

bool Editor::beginTerrainImport(scene::World& world, core::InstanceId rootOrWorkspace, Inspector& inspector,
                                core::u32 columns, core::u32 rows, core::u8 material, std::string name,
                                const ImportOpener& open)
{
    if (m_run != RunState::Editing || m_import != nullptr || m_stroke.has_value() || columns == 0 || rows == 0) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_running")), true};
        return false;
    }
    auto job = std::make_unique<ImportJob>();
    // The world as it is now, which is what a cancel puts back -- taken before
    // the terrain is made, so a cancelled import on a new world leaves none.
    job->before = std::make_unique<scene::WorldSnapshot>(world.snapshot());
    job->existed = terrainIn(world, rootOrWorkspace).valid();
    job->wasDirty = m_sceneDirty;
    job->terrain = createTerrain(world, rootOrWorkspace, inspector);
    job->name = std::move(name);
    job->startedNs = platform::nowNs();
    const scene::TerrainComponent* terrain = world.terrains().find(job->terrain);
    if (terrain == nullptr) {
        m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.nowhere")), true};
        return false;
    }
    // Round the terrain's origin, as the one-table verbs lay it.
    TerrainImportPlan plan;
    plan.columns = columns;
    plan.rows = rows;
    plan.firstX = static_cast<core::i32>(std::floor(-0.5 * static_cast<double>(columns - 1u)));
    plan.firstZ = static_cast<core::i32>(std::floor(-0.5 * static_cast<double>(rows - 1u)));
    plan.material = groundMaterial(*terrain, material);
    std::string error;
    std::unique_ptr<HeightSource> source = open(*terrain, plan, error);
    if (source == nullptr) {
        const std::string failed = job->name;
        m_import = std::move(job);
        endTerrainImport(world, inspector, false);
        // Nothing was written out: there is no layer of the cache to drop.
        m_importOutcome = ImportOutcome::None;
        m_status = EditorStatus{
            core::tr(ENG_TR("engine.editor.terrain.status.not_a_heightmap"), {{"name", failed}, {"reason", error}}),
            true};
        return false;
    }
    job->import = std::make_unique<TerrainImport>(std::move(source), plan);
    m_import = std::move(job);
    return true;
}

f32 Editor::terrainImportProgress() const noexcept
{
    return m_import != nullptr && m_import->import != nullptr ? m_import->import->progress() : 0.0f;
}

std::optional<core::DVec3> Editor::terrainImportCursor(const scene::World& world) const
{
    if (m_import == nullptr || m_import->import == nullptr)
        return std::nullopt;
    const scene::TerrainComponent* terrain = world.terrains().find(m_import->terrain);
    return terrain != nullptr ? std::optional<core::DVec3>(m_import->import->cursor(*terrain)) : std::nullopt;
}

void Editor::driveTerrainImport(scene::World& world, Inspector& inspector, double budgetMilliseconds)
{
    if (m_import == nullptr || m_import->import == nullptr)
        return;
    const core::u64 started = platform::nowNs();
    for (;;) {
        scene::TerrainComponent* terrain = world.terrains().find(m_import->terrain);
        if (terrain == nullptr) {
            endTerrainImport(world, inspector, false);
            m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.nowhere")), true};
            return;
        }
        std::string error;
        const TerrainImport::Step step = m_import->import->step(world, *terrain, error);
        if (step == TerrainImport::Step::Failed || step == TerrainImport::Step::Refused) {
            const std::string name = m_import->name;
            endTerrainImport(world, inspector, false);
            m_status =
                step == TerrainImport::Step::Failed
                    ? EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.not_a_heightmap"),
                                            {{"name", name}, {"reason", error}}),
                                   true}
                    : EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_too_steep"), {{"name", name}}),
                                   true};
            return;
        }
        if (step == TerrainImport::Step::Done) {
            const TerrainImportPlan plan = m_import->import->plan();
            const core::I18nArg args[] = {
                {"name", m_import->name},
                {"columns", static_cast<core::i64>(plan.columns)},
                {"rows", static_cast<core::i64>(plan.rows)},
                {"seconds", static_cast<core::i64>((platform::nowNs() - m_import->startedNs) / 1000000000ull)}};
            endTerrainImport(world, inspector, true);
            m_status = EditorStatus{
                core::engineCatalog().format(ENG_TR("engine.editor.terrain.status.import_laid"), args), false};
            return;
        }
        if (static_cast<double>(platform::nowNs() - started) / 1.0e6 >= budgetMilliseconds)
            return;
    }
}

void Editor::cancelTerrainImport(scene::World& world, Inspector& inspector)
{
    if (m_import == nullptr)
        return;
    endTerrainImport(world, inspector, false);
    m_status = EditorStatus{core::tr(ENG_TR("engine.editor.terrain.status.import_cancelled")), false};
}

void Editor::endTerrainImport(scene::World& world, Inspector& inspector, bool keep)
{
    const std::unique_ptr<ImportJob> job = std::move(m_import);
    m_import.reset();
    if (job == nullptr)
        return;
    if (keep) {
        // **Not a step, and nothing before it is one any more**: every
        // snapshot the history holds is of a world this ground is not in.
        m_history.clear();
        m_sceneDirty = true;
        m_importOutcome = ImportOutcome::Kept;
        return;
    }
    world.restore(*job->before);
    ++m_worldRestores;
    // The terrain's making was a step of the history, and is not one now --
    // and it marked the scene changed, which it no longer is: seen on screen,
    // "the world is as it was" beside a title that asked to be saved.
    if (!job->existed)
        m_history.retract();
    m_sceneDirty = job->wasDirty;
    inspector.pruneDead(world);
    inspector.onWorldRestored();
    m_importOutcome = ImportOutcome::Dropped;
}

bool Editor::driveGizmo(scene::World& world, Inspector& inspector)
{
    const std::optional<GizmoFrame> frame = gizmoFrame(world, inspector);

    // The button came up, or the world stopped being editable under a drag.
    if (m_drag.has_value() && (!m_pointerDown || !frame.has_value())) {
        if (inspector.gesture() == m_drag->gesture)
            inspector.endGesture();
        m_drag.reset();
        // The release belongs to the gizmo too: without this the same click
        // that finished a drag would fall through and select whatever the
        // pointer ended up over.
        m_pending.reset();
        return true;
    }

    if (!frame.has_value()) {
        m_hover.reset();
        return false;
    }

    const PickRay ray = rayThrough(m_pointer);

    if (!m_drag.has_value()) {
        m_hover = pickGizmo(ray, *frame, m_gizmoMode);
        if (!m_pointerPressed || !m_hover.has_value())
            return false;

        GizmoDrag drag;
        drag.handle = *m_hover;
        drag.frame = *frame;

        if (m_gizmoMode == GizmoMode::Rotate) {
            const std::optional<f32> angle = gizmoDragAngle(ray, *frame, drag.handle);
            if (!angle.has_value())
                return false;
            drag.startAngle = *angle;
        }
        else {
            const std::optional<core::DVec3> point = gizmoDragPoint(ray, *frame, drag.handle);
            if (!point.has_value())
                return false;
            drag.startPoint = *point;
        }

        // Every selected instance that HAS a transform, and what it was. The
        // ones that do not are simply not moved rather than being an error: a
        // selection may hold a folder and a part, and dragging the part is a
        // thing somebody meant.
        for (const core::InstanceId id : inspector.selectionSet()) {
            const std::optional<core::CFrameD> at = gizmoTransformOf(world, id);
            if (!at.has_value())
                continue;

            const scene::PartComponent* part = world.parts().find(id);
            DragKind kind = DragKind::Part;
            core::CFrameD parent;
            if (part != nullptr) {
                kind = DragKind::Part;
            }
            else if (world.cameras().find(id) != nullptr) {
                kind = DragKind::Camera;
            }
            else if (world.parts2d().find(id) != nullptr) {
                kind = DragKind::Part2D;
            }
            else if (const std::optional<render::LightAnchor> light = render::lightAnchorOf(world, id);
                     light.has_value()) {
                // **A light moves the way an attachment does** (ADR 0095): its
                // `CFrame` is relative to what holds it, the identity when
                // nothing does, so the drag is divided back through that.
                kind = DragKind::Attachment;
                core::CFrameD own;
                if (const scene::PointLightComponent* point = world.pointLights().find(id); point != nullptr)
                    own = point->cframe;
                else if (const scene::SpotLightComponent* spot = world.spotLights().find(id); spot != nullptr)
                    own = spot->cframe;
                parent = light->partFrame * light->offset * core::inverse(own);
            }
            else if (world.attachments().find(id) != nullptr) {
                kind = DragKind::Attachment;
                // What the local `CFrame` is relative to. Derived from the two
                // frames the mirror already keeps rather than looked up through
                // the tree, so a bone under a bone is right for free.
                if (const scene::AttachmentComponent* attachment = world.attachments().find(id);
                    attachment != nullptr) {
                    parent = attachment->worldCFrame * core::inverse(attachment->cframe);
                }
            }
            else if (world.decals().find(id) != nullptr) {
                // A decal's `CFrame` is relative to the part it is on, as an
                // attachment's is, and is divided back through it the same way.
                kind = DragKind::Attachment;
                parent = decalParentOf(world, id);
            }
            else if (world.waters().find(id) != nullptr) {
                kind = DragKind::Water;
            }
            else if (world.waterPoints().find(id) != nullptr || world.terrains().find(id) != nullptr) {
                kind = DragKind::Position;
            }
            else if (world.tilemaps2d().find(id) != nullptr) {
                kind = DragKind::Position2D;
            }
            else if (world.navigationLinks().find(id) != nullptr) {
                kind = DragKind::NavigationLink;
            }
            else {
                kind = DragKind::Model;
            }

            drag.targets.push_back(id);
            drag.before.push_back(*at);
            const scene::Part2DComponent* sprite = world.parts2d().find(id);
            const scene::WaterComponent* water = world.waters().find(id);
            const scene::DecalComponent* decal = world.decals().find(id);
            drag.sizes.push_back(part != nullptr     ? part->size
                                 : sprite != nullptr ? core::Vec3{sprite->size.x, sprite->size.y, 1.0f}
                                 : water != nullptr  ? water->size
                                 : decal != nullptr  ? decal->size
                                                     : core::Vec3{1.0f, 1.0f, 1.0f});
            // An ocean has no edge to size; a lake and a river do.
            const bool sized =
                part != nullptr || sprite != nullptr || decal != nullptr || (water != nullptr && water->shape != 0);
            drag.sized.push_back(sized ? 1 : 0);
            drag.positions.push_back(water != nullptr ? water->position : core::Vec3{});
            drag.levels.push_back(water != nullptr ? water->surfaceLevel : 0.0);
            drag.kinds.push_back(kind);
            drag.parents.push_back(parent);
            std::vector<std::pair<core::InstanceId, core::CFrameD>> inside;
            if (kind == DragKind::Model) {
                std::vector<core::InstanceId> descendants;
                world.collectDescendants(id, descendants);
                for (const core::InstanceId descendant : descendants) {
                    if (const scene::PartComponent* held = world.parts().find(descendant); held != nullptr)
                        inside.emplace_back(descendant, held->cframe);
                }
            }
            else if (kind == DragKind::Water &&
                     (scene::waterIsRiver(water->shape) || water->shape == scene::water_shape::Lake)) {
                for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
                    if (const scene::WaterPointComponent* point = world.waterPoints().find(child); point != nullptr)
                        inside.emplace_back(child, placedAt(core::toDVec3(point->position)));
                }
            }
            else if (kind == DragKind::NavigationLink) {
                const scene::NavigationLinkComponent* link = world.navigationLinks().find(id);
                inside.emplace_back(id, placedAt(link->from));
                inside.emplace_back(id, placedAt(link->to));
            }
            drag.inside.push_back(std::move(inside));
        }
        if (drag.targets.empty())
            return false;

        // One gesture for the whole drag, so it is one undo step however many
        // frames and however many instances it writes.
        drag.gesture = inspector.beginGesture();
        m_drag = std::move(drag);

        // The press was the gizmo's. Whatever pick the panel queued for the same
        // click is not somebody asking to select something else.
        m_pending.reset();
        return true;
    }

    // --- A drag in progress --------------------------------------------------
    GizmoDrag& drag = *m_drag;
    m_hover = drag.handle;

    // `CFrame` is interned by `applyDragTransform`, which is where every
    // transform write now goes -- four kinds write it four ways, and the one
    // place that knows which is the one that names the property.
    const core::NameAtom sizeName = world.atoms().intern("Size");

    if (m_gizmoMode == GizmoMode::Rotate) {
        const std::optional<f32> angle = gizmoDragAngle(ray, drag.frame, drag.handle);
        if (!angle.has_value())
            return true;

        f32 turned = *angle - drag.startAngle;
        // The short way round, so a ring crossing its own seam does not spin the
        // selection by a whole turn in one frame.
        while (turned > 3.14159265f)
            turned -= 6.28318531f;
        while (turned < -3.14159265f)
            turned += 6.28318531f;
        if (snapping())
            turned = snapTo(turned, snapStep(GizmoMode::Rotate) * 3.14159265f / 180.0f);

        Vec3 axes[3];
        {
            const core::Mat3& basis = drag.frame.transform.rotation;
            for (int index = 0; index < 3; ++index)
                axes[index] = core::normalize(Vec3{basis.m[index][0], basis.m[index][1], basis.m[index][2]});
        }
        const core::Mat3 turn = core::fromAxisAngle(axes[drag.handle.axis], turned);
        const core::DVec3 pivot = drag.frame.transform.position;

        for (core::usize index = 0; index < drag.targets.size(); ++index) {
            const core::CFrameD& before = drag.before[index];
            core::CFrameD after;
            after.rotation = turn * before.rotation;
            // Around the gizmo's pivot rather than each part's own, so a
            // selection turns as one body -- which is what "rotate these" means
            // and what turning each in place would not be.
            const Vec3 offset = core::toVec3(before.position - pivot);
            after.position = pivot + core::toDVec3(turn * offset);
            applyDragTransform(world, inspector, index, after);
        }
        return true;
    }

    const std::optional<core::DVec3> point = gizmoDragPoint(ray, drag.frame, drag.handle);
    if (!point.has_value())
        return true;

    core::DVec3 delta = *point - drag.startPoint;

    if (m_gizmoMode == GizmoMode::Translate) {
        if (snapping()) {
            // **Snapped in the GIZMO's frame, and this used to say so while
            // doing the opposite.** It quantised each WORLD component, which is
            // right for a world-axis drag and wrong for every other one: an arm
            // in local space points diagonally through the world, so rounding
            // x, y and z apart takes the motion OFF the arm. A person dragging
            // a turned crate saw it wander -- reported as "não segue exatamente
            // a reta, vai todo estranho para o sentido da seta".
            //
            // Expressed in the gizmo's own basis, snapped there and turned
            // back, an axis drag stays on its axis and a plane drag stays in
            // its plane, because a component that was zero rounds to zero. In
            // world mode the basis is the identity and this is exactly what it
            // was.
            const f32 step = snapStep(GizmoMode::Translate);
            Vec3 axes[3];
            const core::Mat3& basis = drag.frame.transform.rotation;
            for (int index = 0; index < 3; ++index)
                axes[index] = core::normalize(Vec3{basis.m[index][0], basis.m[index][1], basis.m[index][2]});

            const Vec3 local = core::toVec3(delta);
            const Vec3 snapped{snapTo(core::dot(local, axes[0]), step), snapTo(core::dot(local, axes[1]), step),
                               snapTo(core::dot(local, axes[2]), step)};
            delta = core::toDVec3(axes[0] * snapped.x + axes[1] * snapped.y + axes[2] * snapped.z);
        }
        for (core::usize index = 0; index < drag.targets.size(); ++index) {
            core::CFrameD after = drag.before[index];
            after.position = after.position + delta;
            applyDragTransform(world, inspector, index, after);
        }
        return true;
    }

    // Scale. The axis handle grows one dimension, the middle grows all three,
    // and the amount is the drag measured along the axis in gizmo sizes -- so a
    // drag of one arm's length doubles it whatever the part started at.
    Vec3 factor{1.0f, 1.0f, 1.0f};
    const f32 reach = drag.frame.size > 0.0f ? drag.frame.size : 1.0f;
    if (drag.handle.uniform) {
        const auto along = static_cast<f32>(delta.x + delta.y + delta.z) / reach;
        const f32 scale = 1.0f + along;
        factor = Vec3{scale, scale, scale};
    }
    else {
        Vec3 axes[3];
        const core::Mat3& basis = drag.frame.transform.rotation;
        for (int index = 0; index < 3; ++index)
            axes[index] = core::normalize(Vec3{basis.m[index][0], basis.m[index][1], basis.m[index][2]});
        const f32 along = core::dot(core::toVec3(delta), axes[drag.handle.axis]) / reach;
        const f32 scale = 1.0f + along;
        if (drag.handle.axis == 0)
            factor.x = scale;
        else if (drag.handle.axis == 1)
            factor.y = scale;
        else
            factor.z = scale;
    }

    for (core::usize index = 0; index < drag.targets.size(); ++index) {
        if (drag.sized[index] == 0)
            continue;
        const Vec3 was = drag.sizes[index];
        Vec3 now{was.x * factor.x, was.y * factor.y, was.z * factor.z};
        if (snapping()) {
            const f32 step = snapStep(GizmoMode::Scale);
            now = Vec3{snapTo(now.x, step), snapTo(now.y, step), snapTo(now.z, step)};
        }
        // A part with no thickness has no faces and cannot be picked back, so a
        // drag through zero stops at the smallest thing that is still a thing
        // rather than turning the part inside out.
        constexpr f32 kMinimum = 0.01f;
        now = Vec3{now.x < kMinimum ? kMinimum : now.x, now.y < kMinimum ? kMinimum : now.y,
                   now.z < kMinimum ? kMinimum : now.z};
        // A sprite's size is its width and height; the third axis is not one.
        if (drag.kinds[index] == DragKind::Part2D)
            inspector.enqueue(drag.targets[index], sizeName, scene::Value{core::Vec2{now.x, now.y}});
        else
            inspector.enqueue(drag.targets[index], sizeName, scene::Value{now});
    }
    return true;
}

// The manipulator, drawn where the selection is.
//
// **Lines, because `DebugDraw` is a line list and stays one.** Its own header
// says solid shapes arrive with the milestone that needs them, and a manipulator
// does not: an arrow made of an arm and four barbs reads as an arrow, and a ring
// of segments reads as a ring. What a filled cone would buy is not worth a second
// pipeline in the debug path.
//
// **Camera-relative, like the selection outline beside it and for the same
// reason**: `DebugDraw::rebaseTo` subtracts in f32, so a submission in world
// coordinates quantises the absolute metre value before the camera comes off it
// -- about half a millimetre four kilometres out, on the one thing in the frame
// somebody is trying to place precisely.
void submitGizmo(const GizmoFrame& frame, GizmoMode mode, std::optional<GizmoHandle> active, core::DVec3 cameraOrigin,
                 render::DebugDraw& draw)
{
    using core::Vec3;

    const core::Mat3& basis = frame.transform.rotation;
    Vec3 axes[3];
    for (int index = 0; index < 3; ++index)
        axes[index] = core::normalize(Vec3{basis.m[index][0], basis.m[index][1], basis.m[index][2]});

    // The gizmo's centre in the space the debug pass draws in. Every point below
    // is this plus a metre offset, so the f64 subtraction happens once.
    const Vec3 centre = core::toVec3(frame.transform.position - cameraOrigin);
    const f32 size = frame.size;

    // X red, Y green, Z blue -- the convention `DebugDraw::axes` already uses
    // and the one every editor shares. The one under the pointer goes yellow,
    // which is the only feedback a manipulator needs and the one it cannot do
    // without.
    const render::DebugColor axisColor[3] = {
        render::DebugColor::fromLinear(0.90f, 0.25f, 0.25f),
        render::DebugColor::fromLinear(0.30f, 0.85f, 0.30f),
        render::DebugColor::fromLinear(0.30f, 0.50f, 0.95f),
    };
    const render::DebugColor hot = render::DebugColor::fromLinear(1.0f, 0.85f, 0.15f);
    const render::DebugColor white = render::DebugColor::fromLinear(0.95f, 0.95f, 0.95f);

    const auto lit = [active](core::u8 axis, bool plane, bool uniform) {
        return active.has_value() && active->axis == axis && active->plane == plane && active->uniform == uniform;
    };

    if (mode == GizmoMode::Rotate) {
        constexpr int kSegments = 48;
        for (core::u8 axis = 0; axis < 3; ++axis) {
            const Vec3 u = axes[(axis + 1) % 3] * size;
            const Vec3 v = axes[(axis + 2) % 3] * size;
            const render::DebugColor colour = lit(axis, false, false) ? hot : axisColor[axis];
            Vec3 previous = centre + u;
            for (int step = 1; step <= kSegments; ++step) {
                const f32 angle = 6.28318531f * static_cast<f32>(step) / static_cast<f32>(kSegments);
                const Vec3 point = centre + u * std::cos(angle) + v * std::sin(angle);
                draw.line(previous, point, colour);
                previous = point;
            }
        }
        return;
    }

    for (core::u8 axis = 0; axis < 3; ++axis) {
        const Vec3 direction = axes[axis];
        const Vec3 tip = centre + direction * size;
        const render::DebugColor colour = lit(axis, false, false) ? hot : axisColor[axis];

        // The arm starts clear of the centre handle, so the two do not draw over
        // each other and the gap says where one ends.
        draw.line(centre + direction * (size * 0.12f), tip, colour);

        const Vec3 side = axes[(axis + 1) % 3];
        const Vec3 other = axes[(axis + 2) % 3];
        if (mode == GizmoMode::Translate) {
            // Four barbs back from the tip: an arrowhead, in lines.
            const Vec3 back = tip - direction * (size * 0.16f);
            const f32 spread = size * 0.06f;
            draw.line(tip, back + side * spread, colour);
            draw.line(tip, back - side * spread, colour);
            draw.line(tip, back + other * spread, colour);
            draw.line(tip, back - other * spread, colour);

            // The plane square, at the corner between the OTHER two axes -- so
            // the one drawn in the XY corner moves in X and Y, and its handle is
            // named by the axis it does not move along.
            const core::u8 normal = (axis + 2) % 3;
            const Vec3 a = axes[(normal + 1) % 3];
            const Vec3 b = axes[(normal + 2) % 3];
            const f32 inner = size * 0.25f;
            const f32 outer = size * 0.55f;
            const render::DebugColor planeColour = lit(normal, true, false) ? hot : axisColor[normal];
            const Vec3 corner[4] = {
                centre + a * inner + b * inner,
                centre + a * outer + b * inner,
                centre + a * outer + b * outer,
                centre + a * inner + b * outer,
            };
            for (int edge = 0; edge < 4; ++edge)
                draw.line(corner[edge], corner[(edge + 1) % 4], planeColour);
        }
        else {
            // A small open box at the tip, which is what a scale handle looks
            // like everywhere and what tells it apart from an arrow at a glance.
            const f32 box = size * 0.05f;
            const Vec3 a = side * box;
            const Vec3 b = other * box;
            const Vec3 face[4] = {tip + a + b, tip + a - b, tip - a - b, tip - a + b};
            for (int edge = 0; edge < 4; ++edge)
                draw.line(face[edge], face[(edge + 1) % 4], colour);
        }
    }

    // The centre: uniform scale, or a screen-space drag for translate. Drawn as
    // a small box so it reads as a handle rather than as the place the arms
    // happen to meet.
    const f32 middle = size * 0.09f;
    const render::DebugColor centreColour = active.has_value() && active->uniform ? hot : white;
    draw.wireBox(centre, Vec3{middle, middle, middle}, centreColour);
}

std::optional<PickHit> Editor::resolvePick(const scene::World& world, core::InstanceId root, Inspector& inspector,
                                           const render::DrawPoses* poses)
{
    if (!m_pending.has_value())
        return std::nullopt;

    const PickRequest request = *m_pending;
    m_pending.reset();

    // No camera means nothing has been rendered yet, so there is no image the
    // click could have been aimed at. Clearing the selection would be a guess;
    // doing nothing is not.
    if (!m_hasCamera)
        return std::nullopt;

    // **The UI first, while the UI is what is being worked on** (the owner:
    // with something under `UIService` selected, a click in the viewport should
    // reach the UI under it). The screen's UI is drawn over the world, so while
    // somebody arranges it, it is what they are aiming at -- and with anything
    // else selected the world is, or a menu over the scene would make every
    // part behind it unreachable. The viewport IS the target the UI is laid out
    // against, so the click's pixel is the UI's.
    core::InstanceId uiHit;
    {
        core::InstanceId top = root;
        while (world.parentOf(top).valid())
            top = world.parentOf(top);
        const scene::ClassId uiServiceClass = world.classes().findId(world.atoms().lookup("UIService"));
        core::InstanceId uiService;
        for (core::InstanceId child = world.firstChild(top); child.valid(); child = world.nextSibling(child)) {
            if (world.classOf(child) == uiServiceClass) {
                uiService = child;
                break;
            }
        }
        const core::InstanceId selected = inspector.selection();
        if (uiService.valid() && selected.valid() && (selected == uiService || world.isAncestorOf(uiService, selected)))
            uiHit = ui::hitTest(world, uiService, request.pixel);
    }

    const PickRay ray = rayThrough(request.pixel);
    std::optional<PickHit> hit =
        uiHit.valid() ? std::optional<PickHit>(PickHit{uiHit, 0.0f}) : pickNearest(world, root, ray, poses);
    if (!uiHit.valid()) {

        // **What is not a part** (S5.1). Picking walked the part pool and nothing
        // else, so a `Camera`, a `PointLight`, an `Attachment` and a `Ragdoll` could
        // be reached only through the Explorer -- and the one you want to move is
        // the one you can see.
        //
        // A marker wins over geometry when the ray passes within its radius AND it
        // is not behind the solid hit: a marker is an aiming target rather than a
        // shape, so being smaller must not make it harder to click, and being behind
        // a wall must still make it unreachable.
        // **The ground is solid** (D399): a click on a terrain's ground is the
        // terrain's, unless a part is nearer.
        if (const std::optional<PickHit> ground = pickGround(world, root, ray);
            ground.has_value() && (!hit.has_value() || ground->distance < hit->distance)) {
            hit = ground;
        }

        static std::vector<PickMarker> markers;
        collectPickMarkers(world, root, markers, poses);
        // Not the one the eye is inside (`eyeInsideMarker`).
        std::erase_if(markers, [&ray](const PickMarker& marker) { return eyeInsideMarker(ray.origin, marker.at); });
        if (const std::optional<PickHit> marker =
                pickMarker(markers, ray, kPickMarkerRadius,
                           hit.has_value() ? hit->distance : std::numeric_limits<f32>::infinity());
            marker.has_value()) {
            hit = marker;
        }
        // **Then the water's surface**, in front of what is under it -- but
        // not over a marker: a river's points sit on it, and a surface that
        // took their clicks would leave them reachable only from the tree.
        else if (const std::optional<PickHit> water = pickWater(world, root, ray);
                 water.has_value() && (!hit.has_value() || water->distance < hit->distance)) {
            hit = water;
        }

        // **A click selects the thing, not the part it is made of** (S5.3). A
        // `Model` is something somebody made in order to move it as one, so
        // selecting the wheel of a car hands back the opposite of what the grouping
        // was for. Double-clicking drills in, and a click outside what was drilled
        // comes back out.
        if (hit.has_value() && request.direct) {
            // **Alt: the part itself.** No resolution and no drill: the model stays
            // closed, so the next plain click selects it whole again.
        }
        else if (hit.has_value()) {
            const core::InstanceId resolved = resolveSelection(world, root, hit->instance, m_drilled);

            if (request.opening) {
                // **Opening what was RESOLVED, not what was hit.** Double-clicking a
                // wheel opens the car it is part of; a second double-click then
                // opens whatever is inside that, one level per gesture.
                m_drilled = resolved != hit->instance ? resolved : hit->instance;
            }
            else if (m_drilled.valid() && !world.isAncestorOf(m_drilled, resolved) && resolved != m_drilled) {
                // Clicked outside what was open, so it is closed. Otherwise a drill
                // would be permanent and the rule would be off for the rest of the
                // session.
                m_drilled = core::InstanceId{};
            }

            hit = PickHit{resolveSelection(world, root, hit->instance, m_drilled), hit->distance};
        }
        else if (!request.additive) {
            // Empty space closes it too, for the same reason it deselects.
            m_drilled = core::InstanceId{};
        }
    }

    // **Ctrl adds and removes; a plain click replaces.** The same gesture the
    // Explorer's rows use, because it is the same question asked of a different
    // surface -- and somebody who has ctrl-clicked four parts in the tree will
    // try it in the viewport within the minute.
    if (request.additive) {
        // Ctrl on empty space keeps what is selected. Deselecting everything is
        // what a plain click means, and a modifier that means "add" cannot also
        // mean "clear".
        if (hit.has_value()) {
            inspector.toggle(hit->instance);
            // Revealed whether the toggle added or removed it: either way the
            // row somebody just acted on is the one they want to see.
            inspector.reveal(hit->instance);
        }
        return hit;
    }

    // Clicking empty space deselects. See the header: leaving the last thing
    // selected is how somebody edits the object they believed they had let go
    // of.
    inspector.select(hit.has_value() ? hit->instance : core::InstanceId{});
    // **And the tree goes to it.** Clicking a part in the viewport and then
    // hunting for its row through four closed folders is the editor knowing
    // where something is and not saying. The reveal opens the way down and the
    // Explorer scrolls the row into view, which is what every tool with a
    // viewport and a tree does.
    if (hit.has_value())
        inspector.reveal(hit->instance);
    return hit;
}

} // namespace engine::app
