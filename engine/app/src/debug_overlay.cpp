#include "engine/app/debug_overlay.h"

#include "engine/app/editor_drive.h"
#include "engine/app/streaming_host.h"
#include "engine/app/view_host.h"
#include "engine/app/world_panels.h"
#include "engine/asset/terrain_layers.h"
#include "engine/asset/terrain_palette.h"
#include "engine/audio/audio.h"
#include "engine/core/brand.h"

#if ENG_DEBUG_UI

#include <SDL3/SDL_misc.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imgui_internal.h>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "engine/app/backends.h"
#include "engine/app/class_favorites.h"
#include "engine/app/command_palette.h"
#include "engine/app/export_runner.h"
#include "engine/app/icons.h"
#include "engine/app/number_expression.h"
#include "engine/app/project_config.h"
#include "engine/app/script_editor.h"
#include "engine/app/script_editor_settings.h"
#include "engine/app/surface_compiler.h"
#include "engine/app/thumbnails.h"
#include "engine/app/ui_theme.h"
#include "engine/asset/surface_shader.h"
#include "engine/core/build_info.h"
#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/math.h"
#include "engine/core/text_key.h"
#include "engine/core/toml.h"
#include "engine/core/toml_edit.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/platform/process.h"
#include "engine/platform/sdl_interop.h"
#include "engine/platform/window.h"
#include "engine/render/look.h"
#include "engine/rhi/device.h"
#include "engine/rhi/sdlgpu_interop.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/skeleton_host.h"
#include "engine/scene/value.h"
#include "engine/scene/water.h"
#include "engine/scene/world.h"
#include "engine/script/modules.h"
#include "engine/script/save_store.h"
#include "icon_ids.gen.h"

#endif

namespace engine::app {

#if ENG_DEBUG_UI

// --- What this shell decides, as opposed to what it draws -------------------
//
// Two answers that are arithmetic rather than pixels, and they sit OUT of the
// anonymous namespace below on purpose. The only case in `debug_overlay_tests`
// that builds this shell needs a display and a GPU device, and where either is
// missing it returns before it asserts -- so a check written inside it reports
// a pass it never ran. A decision that is a reachable function is a decision
// every machine can check, which is the same move `collectTree` made in
// `inspector.h`.

// **Ask, or act: the one gate in front of every verb that empties the scene.**
//
// Five doors lead to the same loss -- File > New Scene, opening another scene,
// making a project, leaving for one, and quitting -- and the toolbar's New was
// a sixth that acted on the spot. A person learns the rule from the doors that
// knock, so the one that stays silent does not read as an exception: it reads
// as "there was nothing to lose".
//
// `unsavedWork` is passed rather than read off the `Editor` so this stays a
// decision over values. `scene` is what `Pending::OpenScene` was going to open
// and is ignored by the other four; it has to survive the question, because the
// answer arrives frames after the double-click and by then the browser is
// looking somewhere else.
void issueOrAsk(EditorDialogs::Pending what, bool unsavedWork, std::string_view scene, EditorDialogs& dialogs,
                EditorCommands& commands)
{
    if (what == EditorDialogs::Pending::None)
        return;

    // Nothing to lose, so nothing to ask about: a dialog raised over a clean
    // scene is one people learn to dismiss without reading, and then dismiss
    // over a dirty one too.
    if (unsavedWork) {
        dialogs.pending = what;
        dialogs.pendingScene = std::string(scene);
        return;
    }

    switch (what) {
    case EditorDialogs::Pending::Quit:
        commands.quit = true;
        break;
    case EditorDialogs::Pending::NewScene:
        commands.newScene = true;
        break;
    case EditorDialogs::Pending::OpenScene:
        commands.openScene = std::string(scene);
        break;
    case EditorDialogs::Pending::NewProject:
        commands.newProject = true;
        break;
    case EditorDialogs::Pending::OpenProject:
        commands.openProject = true;
        break;
    case EditorDialogs::Pending::None:
        break;
    }
}

// **How tall the Console's log is: everything the panel has left, less the room
// the REPL line under it needs.**
//
// It was a fixed 160 px inside a window somebody can drag taller, so enlarging
// the Console added empty space BELOW the log rather than showing more of it --
// which is the only thing making a console bigger is ever for.
//
// `minimum` is a floor rather than tidiness. The same console is drawn at the
// foot of the F3 overlay's scrolling window, where what is left can be a few
// pixels or none, and ImGui reads a non-positive child height as "fill the
// rest, less this much" -- so an unclamped subtraction would make the log grow
// as the room for it shrank.
// **Where a row dropped against another row lands**, as an index into the
// parent's child list -- the place it will OCCUPY, which is what
// `World::moveChild` takes.
//
// `from` is where the dragged row stands now, `target` where the row it was
// dropped against stands, and `below` says which edge the pointer was nearest.
//
// **The whole subtlety is that the dragged row comes out of the list first.**
// `moveChild` removes and re-inserts, so every sibling after `from` shifts down
// by one before the insertion happens -- which means the naive answers are wrong
// in exactly one direction each:
//
//   dragging DOWN and dropping below a row: the row's own index is already the
//   place after it, because the drag's removal pulled it up by one.
//
//   dragging UP and dropping above a row: the row's index is one too far,
//   because nothing below the drag moved.
//
// Read as "before whatever stands at `target` now" instead, and a downward drag
// lands one place short every time. `moveChild`'s own doc says so; this is the
// caller that would have got it wrong.
[[nodiscard]] core::u32 dropLanding(core::u32 from, core::u32 target, bool below) noexcept
{
    if (below)
        return from > target ? target + 1 : target;
    return from < target ? (target > 0 ? target - 1 : 0) : target;
}

// A count with thousands separators, because a triangle count is the one number
// on this panel that routinely has seven digits and `1483920` is unreadable at a
// glance in a way `1,483,920` is not.
//
// Grouped by hand rather than through a locale: `std::locale` on a number this
// panel draws every frame would be a per-frame allocation and a dependency on
// what the machine is set to, and this panel is English (R1).
[[nodiscard]] std::string formatCount(core::u32 value)
{
    std::string digits = std::to_string(value);
    for (core::usize at = digits.size(); at > 3;) {
        at -= 3;
        digits.insert(at, 1, ',');
    }
    return digits;
}

[[nodiscard]] f32 consoleLogHeight(f32 available, f32 reservedBelow, f32 minimum) noexcept
{
    const f32 height = available - reservedBelow;
    return height < minimum ? minimum : height;
}

struct UiDragResult
{
    core::UDim2 position;
    core::UDim2 size;
};

// **Where a dragged interface box puts its properties.** `min`/`max` are the
// box as the drag found it, in pixels; the four flags say which edges the grip
// moves (all four for the body); `delta` is the pointer's travel. `Position`
// places the ANCHOR point, so an edge that stays put stays put only when the
// anchor moves by its share of the change. Offsets change, scales do not.
[[nodiscard]] UiDragResult uiDragResult(core::Vec2 min, core::Vec2 max, core::Vec2 delta, bool left, bool right,
                                        bool top, bool bottom, core::Vec2 anchor, core::UDim2 position,
                                        core::UDim2 size) noexcept
{
    const bool body = left && right && top && bottom;
    float x0 = min.x;
    float y0 = min.y;
    float x1 = max.x;
    float y1 = max.y;
    if (body) {
        x0 += delta.x;
        x1 += delta.x;
        y0 += delta.y;
        y1 += delta.y;
    }
    else {
        if (left)
            x0 = std::min(x0 + delta.x, x1 - 1.0f);
        if (right)
            x1 = std::max(x1 + delta.x, x0 + 1.0f);
        if (top)
            y0 = std::min(y0 + delta.y, y1 - 1.0f);
        if (bottom)
            y1 = std::max(y1 + delta.y, y0 + 1.0f);
    }
    const float grownX = (x1 - x0) - (max.x - min.x);
    const float grownY = (y1 - y0) - (max.y - min.y);
    size.x.offset += grownX;
    size.y.offset += grownY;
    position.x.offset += (x0 - min.x) + grownX * anchor.x;
    position.y.offset += (y0 - min.y) + grownY * anchor.y;
    return UiDragResult{position, size};
}

// **The REPL's up and down arrows walk what was typed before**, the way every
// shell does. `at` is -1 on the line being typed, and otherwise an index into
// `count` entries, oldest first: up goes back to the newest and stops at the
// oldest, down comes forward and past the newest returns to the line.
[[nodiscard]] core::i32 consoleHistoryStep(core::usize count, core::i32 at, bool up) noexcept
{
    if (count == 0)
        return -1;
    if (up)
        return at < 0 ? static_cast<core::i32>(count) - 1 : (at > 0 ? at - 1 : 0);
    if (at < 0 || static_cast<core::usize>(at) + 1 >= count)
        return -1;
    return at + 1;
}

// **What a selection across console lines copies**: from one (line, byte)
// place to another, in either order, whole lines between them joined by
// newlines. Offsets past a line's end are its end.
[[nodiscard]] std::string consoleSelectionText(std::span<const std::string_view> lines, core::usize fromLine,
                                               core::usize fromOffset, core::usize toLine, core::usize toOffset)
{
    if (lines.empty())
        return {};
    if (toLine < fromLine || (toLine == fromLine && toOffset < fromOffset)) {
        std::swap(fromLine, toLine);
        std::swap(fromOffset, toOffset);
    }
    fromLine = std::min(fromLine, lines.size() - 1);
    toLine = std::min(toLine, lines.size() - 1);
    std::string out;
    for (core::usize line = fromLine; line <= toLine; ++line) {
        const std::string_view text = lines[line];
        const core::usize begin = line == fromLine ? std::min(fromOffset, text.size()) : 0;
        const core::usize end = line == toLine ? std::min(toOffset, text.size()) : text.size();
        if (line != fromLine)
            out.push_back('\n');
        if (end > begin)
            out.append(text.substr(begin, end - begin));
    }
    return out;
}

namespace {

// **The Water tool's icons, by name and not from the generated list** (D418).
// `icons::` holds an id for every picture the icon set HAS, and the set had no
// picture of water when the tool was written: the two constants compiled on
// the machine that was drawing them and nowhere else. An id is only a name --
// a theme that draws these is used, and the atlas falls back where none does.
constexpr std::string_view WaterIcon = "class.Water";
// What each of the tool's four draws, on the same terms: pictures the icon set
// is still being given.
constexpr std::string_view WaterRiverIcon = "action.WaterRiver";
constexpr std::string_view WaterLakeIcon = "action.WaterLake";
constexpr std::string_view WaterPoolIcon = "action.WaterPool";
constexpr std::string_view WaterOceanIcon = "action.WaterOcean";

// **A translated label with an id of its own** (ADR 0145): the words are the
// catalog's, and the `##id` after them -- never shown -- keeps the widget the
// same widget whatever language its words are in.
[[nodiscard]] std::string labelled(core::TextKey key, std::string_view id)
{
    std::string out = core::tr(key);
    out += id;
    return out;
}

// **A combo's choices, each from the catalog**: ImGui takes them as one
// string with a NUL after each and one more at the end, which `c_str()`
// gives.
[[nodiscard]] std::string choices(std::initializer_list<core::TextKey> keys)
{
    std::string out;
    for (const core::TextKey key : keys) {
        out += core::tr(key);
        out.push_back('\0');
    }
    return out;
}

// **A Properties heading's words** (`propertyCategory`, inspector.cpp): the
// table there names a heading in English, as an id; this is what is shown.
[[nodiscard]] const char* categoryWords(std::string_view category)
{
    struct Named
    {
        std::string_view id;
        core::TextKey words;
    };
    static constexpr Named Categories[] = {
        {"Appearance", ENG_TR("engine.editor.properties.category.appearance")},
        {"Data", ENG_TR("engine.editor.properties.category.data")},
        {"Transform", ENG_TR("engine.editor.properties.category.transform")},
        {"Behavior", ENG_TR("engine.editor.properties.category.behavior")},
        {"Collision", ENG_TR("engine.editor.properties.category.collision")},
        {"Physics", ENG_TR("engine.editor.properties.category.physics")},
        {"Text", ENG_TR("engine.editor.properties.category.text")},
        {"Image", ENG_TR("engine.editor.properties.category.image")},
        {"Layout", ENG_TR("engine.editor.properties.category.layout")},
        {"Camera", ENG_TR("engine.editor.properties.category.camera")},
        {"Light", ENG_TR("engine.editor.properties.category.light")},
        {"Audio", ENG_TR("engine.editor.properties.category.audio")},
        {"Emission", ENG_TR("engine.editor.properties.category.emission")},
        {"Constraint", ENG_TR("engine.editor.properties.category.constraint")},
        {"Character", ENG_TR("engine.editor.properties.category.character")},
        {"Streaming", ENG_TR("engine.editor.properties.category.streaming")},
        {"Navigation", ENG_TR("engine.editor.properties.category.navigation")},
        {"Default Agent", ENG_TR("engine.editor.properties.category.default_agent")},
    };
    for (const Named& each : Categories) {
        if (each.id == category)
            return core::tr(each.words);
    }
    // One the table gained and this did not: its id, which is English.
    static std::string unknown;
    unknown = std::string(category);
    return unknown.c_str();
}

// **A number with a fixed count of decimals**, for a catalog slot: a slot
// takes text, and a measurement reads as it did when it was a printf.
[[nodiscard]] std::string fixed(double value, int decimals)
{
    char text[64]{};
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

// What the log falls back to where there is nothing to fill: the height it was
// fixed at before it could fill anything, so the F3 overlay's console keeps the
// shape it has always had.
constexpr f32 kConsoleLogMinHeight = 160.0f;

// Bound at construction, read while drawing. These sit beside ImGui's own
// process-wide context rather than inside the class for two reasons: that
// context already makes a second live overlay meaningless, and keeping them out
// of the header is what lets the header stay free of SDL and of a layout that
// changes with the build profile.
//
// Main-thread only, like everything else that touches SDL's event queue.
platform::Window* g_window = nullptr;
const rhi::IDevice* g_device = nullptr;

// The content browser pictures of pictures, owned by the frame loop -- which is
// where a command list lives and therefore where the decoding has to happen --
// and pointed at from here for the same reason `g_device` is: the row that wants
// one is drawn several call frames below anything holding an overlay.
ThumbnailCache* g_thumbnails = nullptr;
// The game's saves (ADR 0111), for the Saves panel. The host's.
script::SaveStore* g_saves = nullptr;

// The surface shader compiler, for the material panel's status line (ADR
// 0091). Owned by the frame loop, like the thumbnails.
SurfaceCompiler* g_surfaceCompiler = nullptr;

// The rig, for the one property that names a joint (`Bone.JointName`).
//
// Beside `g_thumbnails` and for the same reason it is there rather than in a
// signature: the row that needs it is drawn five call frames below anything
// holding a world host, and the alternative is threading a pointer through
// `drawProperties`, `drawPropertyRow` and `drawPropertyEditor` so that one
// branch of one switch can read it. Null is legal -- a build with no renderer
// has no skeletons, and the field is then what it was before, a text box.
const scene::SkeletonHost* g_skeleton = nullptr;

// What this person chose to look at the engine through (ADR 0056), and what the
// display said when the window opened.
//
// Beside `g_window` and for the same reason: the Preferences dialog is a free
// function several call frames below anything holding an overlay, and the one
// live ImGui context already makes a second set of these meaningless.
Appearance g_appearance;
f32 g_displayScale = 1.0f;

// Re-styles ImGui and writes the choice back, which is one action rather than
// two: an appearance somebody changed and did not get back next launch is a
// setting they conclude does not work.
void applyAppearance()
{
    applyTheme(themeById(g_appearance.themeId), resolveUiScale(g_appearance.scale, g_displayScale));
    (void)saveAppearance(appearanceFile(), g_appearance);
}

// The theme currently drawing. Every call site that used to write a colour out
// by hand asks this instead.
[[nodiscard]] const ThemePalette& palette() noexcept
{
    return themeById(g_appearance.themeId).palette;
}

[[nodiscard]] ImVec4 themeColor(core::Color3 color) noexcept
{
    return ImVec4(color.r, color.g, color.b, 1.0f);
}

// A step between two tokens, for the handful of states that are "that colour,
// nearer this one" rather than a decision of their own -- a hovered primary
// button, a pressed one.
[[nodiscard]] ImVec4 themeBlend(core::Color3 from, core::Color3 to, f32 amount) noexcept
{
    return ImVec4(from.r + (to.r - from.r) * amount, from.g + (to.g - from.g) * amount,
                  from.b + (to.b - from.b) * amount, 1.0f);
}

// Frame time, sampled and held; everything else read directly.
//
// The rule this panel started with -- "nothing here is sampled or estimated" --
// is right for the three static facts below and was wrong for the one value
// that changes every frame. Printed raw at 60 Hz it cannot be read at all: the
// human reported it twice, and could only read the panel by pausing a frame.
//
// A held mean is also MORE honest about what the engine costs than a number
// that trembles, because windowed frames present through the swapchain and the
// last digits are VSync and the compositor rather than engine work. The worst
// frame in the window is printed beside it, since a hitch is what a developer
// is actually looking for and a mean is precisely the statistic that hides one.
//
// `frame.index` is gone. A bare counter at 60 Hz is unreadable by construction
// and answers nothing the frame time does not -- a stalled engine stops drawing
// this panel at all. The number still exists where it is used: the capture
// stream names its frames, and the baseline collector counts them.
struct FrameTimeMeter
{
    // Four hertz, the slow end of a readable range rather than the fast one: a
    // four-digit number that changes faster than this is legible only in
    // principle, which is the defect being fixed.
    static constexpr double kWindowSeconds = 0.25;

    double elapsed = 0.0;
    double sum = 0.0;
    double worst = 0.0;
    unsigned frames = 0;

    // What is displayed, replaced only when a window closes.
    double meanMs = 0.0;
    double worstMs = 0.0;
    double perSecond = 0.0;
    bool primed = false;

    void accumulate(double renderDt) noexcept
    {
        // The first frame has no previous one to measure against. Its zero is
        // kept out of the mean rather than divided by, which is the same guard
        // the raw print needed and for the same reason.
        if (renderDt > 0.0) {
            sum += renderDt;
            worst = renderDt > worst ? renderDt : worst;
            ++frames;
        }

        elapsed += renderDt;
        if (elapsed < kWindowSeconds || frames == 0)
            return;

        const double mean = sum / static_cast<double>(frames);
        meanMs = mean * 1000.0;
        worstMs = worst * 1000.0;
        perSecond = 1.0 / mean;
        primed = true;

        elapsed = 0.0;
        sum = 0.0;
        worst = 0.0;
        frames = 0;
    }
};

// Accumulates only while the panel is drawing, which is what makes the window
// it reports the window it displayed.
FrameTimeMeter g_frameTime;

// The camera textures, for the stats (ADR 0107): set once by the frame loop.
const ViewHost* g_views = nullptr;

// Three facts the host already knows, plus the sampled frame time above.
// The Properties panel's grid and headings, used by the panels drawn before
// it in this file (the Stats panel: the owner asked for it to look the same).
bool propertiesSection(const char* label);
bool beginSectionGrid(const char* id);
void endSectionGrid();
void sectionName(std::string_view text, bool muted = false);

// One row of a stats grid: the name, and a value made by `format`.
void statRow(const char* name, const char* format, ...) IM_FMTARGS(2);
void statRow(const char* name, const char* format, ...)
{
    sectionName(name);
    ImGui::AlignTextToFramePadding();
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
}

// **The same grid and headings as Properties** (the owner, 2026-09-27): a
// name on the left, its number on the right, under a heading that folds.
void drawStats(const Frame& frame, const RenderCounters& counters)
{
    g_frameTime.accumulate(frame.renderDt);

    if (propertiesSection(core::tr(ENG_TR("engine.editor.stats.frame"))) && beginSectionGrid("frame")) {
        // Dashes rather than a made-up 0.00 before the first window closes: a
        // quarter second of "no measurement yet" is honest and 0.00 ms is not.
        if (g_frameTime.primed) {
            statRow(core::tr(ENG_TR("engine.editor.stats.time")), "%s",
                    core::tr(ENG_TR("engine.editor.stats.value.time"),
                             {{"ms", fixed(g_frameTime.meanMs, 2)}, {"fps", fixed(g_frameTime.perSecond, 0)}})
                        .c_str());
            statRow(core::tr(ENG_TR("engine.editor.stats.worst")), "%s",
                    core::tr(ENG_TR("engine.editor.stats.value.milliseconds"), {{"ms", fixed(g_frameTime.worstMs, 2)}})
                        .c_str());
        }
        else {
            statRow(core::tr(ENG_TR("engine.editor.stats.time")), "%s",
                    core::tr(ENG_TR("engine.editor.stats.value.no_time")));
        }
        const std::string_view backend = backendName(g_device->backend());
        statRow(core::tr(ENG_TR("engine.editor.stats.backend")), "%.*s", static_cast<int>(backend.size()),
                backend.data());
        const platform::WindowSize size = platform::windowPixelSize(*g_window);
        statRow(core::tr(ENG_TR("engine.editor.stats.drawable")), "%d x %d", size.width, size.height);
        endSectionGrid();
    }

    // **What the frame actually cost the GPU**, which neither shell showed for
    // nine milestones. Both draw through here, so the person authoring a world
    // and the person playing it see the same numbers -- and it is the author who
    // needs them, because a scene that costs four thousand draw calls is a
    // scene somebody built that way.
    //
    // Draws beside objects, because one call can cover a run of objects since
    // M7.5 and the gap between them is what the instanced path saved; LOD
    // draws beside triangles, because a scene of distant meshes reporting zero
    // coarse draws is a selector that is not selecting.
    if (propertiesSection(core::tr(ENG_TR("engine.editor.stats.rendering"))) && beginSectionGrid("rendering")) {
        statRow(core::tr(ENG_TR("engine.editor.stats.draw_calls")), "%s",
                core::tr(ENG_TR("engine.editor.stats.value.draw_calls"),
                         {{"count", static_cast<core::i64>(counters.drawCalls)},
                          {"instanced", static_cast<core::i64>(counters.instancedDraws)}})
                    .c_str());
        statRow(core::tr(ENG_TR("engine.editor.stats.objects")), "%u", counters.visibleObjects);
        statRow(core::tr(ENG_TR("engine.editor.stats.lod_draws")), "%u", counters.lodDraws);
        statRow(core::tr(ENG_TR("engine.editor.stats.triangles")), "%s", formatCount(counters.triangles).c_str());
        endSectionGrid();
    }

    // **How long a terrain edit takes to be seen**: what "the brush feels
    // late" is, as a number. Shown once there has been an edit to measure.
    if (counters.terrainEdits > 0 && propertiesSection(core::tr(ENG_TR("engine.editor.stats.terrain"))) &&
        beginSectionGrid("terrain")) {
        statRow(core::tr(ENG_TR("engine.editor.stats.edit_to_picture")), "%s",
                core::tr(ENG_TR("engine.editor.stats.value.edit_to_picture"),
                         {{"ms", fixed(counters.terrainEditMs, 1)},
                          {"count", static_cast<core::i64>(counters.terrainEditFrames)}})
                    .c_str());
        statRow(core::tr(ENG_TR("engine.editor.stats.edit_to_picture_worst")), "%s",
                core::tr(ENG_TR("engine.editor.stats.value.edit_to_picture"),
                         {{"ms", fixed(counters.terrainEditWorstMs, 1)},
                          {"count", static_cast<core::i64>(counters.terrainEditWorstFrames)}})
                    .c_str());
        endSectionGrid();
    }

    // **Foliage** (ADR 0116): what is grown around the camera. The instances
    // DRAWN are the GPU cull's answer and never come back to the CPU; what is
    // here is what the cull reads.
    if (counters.foliageTiles > 0 && propertiesSection(core::tr(ENG_TR("engine.editor.stats.foliage"))) &&
        beginSectionGrid("foliage")) {
        statRow(core::tr(ENG_TR("engine.editor.stats.tiles")), "%s",
                core::tr(ENG_TR("engine.editor.stats.value.tiles"),
                         {{"resident", static_cast<core::i64>(counters.foliageTiles)},
                          {"grown", static_cast<core::i64>(counters.foliageGrown)}})
                    .c_str());
        statRow(core::tr(ENG_TR("engine.editor.stats.instances")), "%s",
                formatCount(counters.foliageInstances).c_str());
        endSectionGrid();
    }

    // **Every view, with what it costs** (ADR 0107): a camera texture is the
    // world drawn again, and a wall of them is where a frame's time goes.
    if (g_views != nullptr && !g_views->views().empty() &&
        propertiesSection(core::tr(ENG_TR("engine.editor.stats.views"))) && beginSectionGrid("views")) {
        statRow(core::tr(ENG_TR("engine.editor.stats.budget")), "%s",
                core::tr(ENG_TR("engine.editor.stats.value.budget"),
                         {{"count", static_cast<core::i64>(g_views->perFrame())}})
                    .c_str());
        for (const ViewHost::View& view : g_views->views()) {
            const std::string name = "view://" + view.name;
            if (!view.drawn) {
                statRow(name.c_str(), "%s",
                        core::tr(ENG_TR("engine.editor.stats.value.view_waiting"),
                                 {{"width", static_cast<core::i64>(view.width)},
                                  {"height", static_cast<core::i64>(view.height)}})
                            .c_str());
                continue;
            }
            const core::u64 ago = frame.index >= view.lastDrawn ? frame.index - view.lastDrawn : 0;
            statRow(name.c_str(), "%s",
                    core::tr(ENG_TR("engine.editor.stats.value.view_drawn"),
                             {{"width", static_cast<core::i64>(view.width)},
                              {"height", static_cast<core::i64>(view.height)},
                              {"ms", fixed(view.milliseconds, 2)},
                              {"count", static_cast<core::i64>(ago)}})
                        .c_str());
        }
        endSectionGrid();
    }
}

// Which panel the editor is drawing on, from the panel's own background.
//
// **Decided rather than configured**, which is what makes it follow an ImGui
// style change for free and leaves nothing to keep in sync. Relative luminance
// with the usual coefficients, and the threshold is the middle: a background
// under it is a dark panel and the palette's `dark` value is the one that clears
// 3:1 against it.
[[nodiscard]] IconAtlas::Panel currentPanel() noexcept
{
    const ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const float luminance = 0.2126f * background.x + 0.7152f * background.y + 0.0722f * background.z;
    return luminance < 0.5f ? IconAtlas::Panel::Dark : IconAtlas::Panel::Light;
}

// The colour an icon is drawn in: its ROLE's, or the panel's own foreground.
//
// The fallback is not a degraded path -- it is what every icon did before there
// was a palette, and it is what all of them do when tinting is off. The set was
// drawn and collision-checked in a single ink, so an uncoloured editor is not a
// worse one.
// **An icon on an accent fill** -- a toolbar toggle that is on -- is drawn in
// the colour made to read on that fill, whatever its role's colour is: a blue
// move handle on a blue button is a button with no picture.
bool g_iconOnAccent = false;

[[nodiscard]] ImVec4 iconTint(const IconAtlas* icons, std::string_view id) noexcept
{
    if (g_iconOnAccent) {
        const core::Color3 on = palette().onAccent;
        return ImVec4(on.r, on.g, on.b, ImGui::GetStyleColorVec4(ImGuiCol_Text).w);
    }
    if (icons != nullptr) {
        if (const std::optional<core::Color3> role = icons->tintFor(id, currentPanel()); role.has_value()) {
            // Alpha from the panel's own text colour, so a disabled row's icon
            // still dims with its label -- the role says WHICH colour and the
            // style says how present it is.
            const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            return ImVec4(role->r, role->g, role->b, text.w);
        }
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// One icon, inline, at the current cursor. Returns false when there is no atlas
// or no cell, so a caller can fall back to text rather than leaving a hole.
//
// **Tinted with the current text colour**, which is the whole reason the source
// images are white masks: one drawing serves a light panel and a dark one, and
// a disabled row's icon dims with its label for free. `ImageWithBg` rather than
// `Image` because the tint parameter moved there in ImGui 1.91.9.
bool drawIcon(const IconAtlas* icons, std::string_view id, float size,
              std::optional<core::Color3> override = std::nullopt)
{
    if (icons == nullptr || !icons->ready())
        return false;

    const IconSprite sprite = icons->find(id, static_cast<core::u32>(size + 0.5f));
    if (!sprite.valid)
        return false;

    SDL_GPUTexture* native = g_device != nullptr ? rhi::nativeTexture(*g_device, icons->texture()) : nullptr;
    if (native == nullptr)
        return false;

    // **A colour a PERSON put on this folder beats the role's**, and it takes
    // the panel's own alpha the way a role does, so a disabled row's coloured
    // icon still dims with its label. The role says what KIND of thing an icon
    // is; this says which one, and only a person can say that.
    ImVec4 tint = iconTint(icons, id);
    if (override.has_value())
        tint = ImVec4(override->r, override->g, override->b, tint.w);

    ImGui::ImageWithBg(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)), ImVec2(size, size),
                       ImVec2(sprite.u0, sprite.v0), ImVec2(sprite.u1, sprite.v1), ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
                       tint);
    return true;
}

// The badge over an icon already drawn at `size`, with its top-left at `origin`
// in SCREEN space (ADR 0049's mark, `icons/README.md`'s `overlay.` namespace).
//
// **Two draws, in this order, every time.** First `overlay.StampBase` -- the
// solid silhouette -- in the panel's own BACKGROUND colour and slightly larger,
// which punches a clean hole in whatever is underneath. Then `overlay.Stamp` --
// the same silhouette with the mark cut out -- in the foreground, centred in
// that hole.
//
// **The knockout is what makes the badge exist.** Measured across the class set
// at 16 px, 37 of 42 icons already have ink where the badge goes -- 51% under
// `class.Workspace` and 49% under `class.Folder` -- so a bare badge lands on a
// folder's body and is not there.
//
// Drawn through the window's draw list rather than as an ImGui item, because it
// is not one: it sits ON a row that has already been laid out, it takes no
// space and no clicks, and an `Image` here would take both.
//
// **Its colour is its own.** A badge means the same thing on every icon, so it
// takes its own role rather than the subject's -- tinting it `spatial` on a
// `Part` and `ui` on a `Frame` would make one mark's colour mean two things.
// `face` is the mark: the stamp's by default, or a script's side (ADR 0138 §4).
// Every mark shares the stamp's halo, which is its outer silhouette.
void drawIconBadge(const IconAtlas* icons, ImVec2 origin, float size, std::string_view faceId = icons::OverlayStamp)
{
    if (icons == nullptr || !icons->ready() || size <= 0.0f)
        return;

    SDL_GPUTexture* native = g_device != nullptr ? rhi::nativeTexture(*g_device, icons->texture()) : nullptr;
    if (native == nullptr)
        return;

    const IconAtlas::Overlay& overlay = icons->overlay();
    const float mark = size * overlay.scale;
    const float halo = mark * overlay.haloScale;
    if (!(mark > 0.0f))
        return;

    const IconSprite base = icons->find(icons::OverlayStampBase, static_cast<core::u32>(halo + 0.5f));
    const IconSprite face = icons->find(faceId, static_cast<core::u32>(mark + 0.5f));
    if (!base.valid || !face.valid)
        return;

    // The corner the theme asked for, measured from the icon's own box.
    const bool right = overlay.corner == IconAtlas::Overlay::Corner::BottomRight ||
                       overlay.corner == IconAtlas::Overlay::Corner::TopRight;
    const bool bottom = overlay.corner == IconAtlas::Overlay::Corner::BottomRight ||
                        overlay.corner == IconAtlas::Overlay::Corner::BottomLeft;

    // **Both are centred on one point**, which is what keeps the rim even. The
    // two files share an outer silhouette exactly, so a badge positioned by its
    // corner instead would put the whole difference on one side.
    const ImVec2 centre(origin.x + (right ? size - halo * 0.5f : halo * 0.5f),
                        origin.y + (bottom ? size - halo * 0.5f : halo * 0.5f));

    const auto quad = [&centre](float edge) {
        const float half = edge * 0.5f;
        return std::pair<ImVec2, ImVec2>{ImVec2(centre.x - half, centre.y - half),
                                         ImVec2(centre.x + half, centre.y + half)};
    };

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto [haloMin, haloMax] = quad(halo);
    const auto [markMin, markMax] = quad(mark);

    // The panel's own background, so the hole matches whatever the row is
    // painted on -- including a selected row, which is a different colour from
    // the window behind it.
    const ImVec4 behind = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const ImVec4 front = iconTint(icons, faceId);

    const auto texture = static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native));
    draw->AddImage(texture, haloMin, haloMax, ImVec2(base.u0, base.v0), ImVec2(base.u1, base.v1),
                   ImGui::ColorConvertFloat4ToU32(ImVec4(behind.x, behind.y, behind.z, 1.0f)));
    draw->AddImage(texture, markMin, markMax, ImVec2(face.u0, face.v0), ImVec2(face.u1, face.v1),
                   ImGui::ColorConvertFloat4ToU32(front));
}

// **Why an instance of the look's classes changes nothing** (ADR 0096), as the
// sentence the editor shows -- or empty when it counts, or when it is not one of
// them. Asked of `render::lookStanding`, which walks exactly what the renderer
// resolves, so the marker and the picture cannot disagree. Found twice with
// lights before this (ADR 0095): a silent no-op is the worst answer an editor
// can give.
std::string lookInactiveReason(const scene::World& world, core::InstanceId id)
{
    // The cheap reject first: this runs for every visible Explorer row.
    const bool effect = world.postEffects().find(id) != nullptr;
    const bool environment = world.atmospheres().find(id) != nullptr || world.skies().find(id) != nullptr;
    if (!effect && !environment)
        return {};

    // The tree this instance is in, and its `Lighting` and current camera --
    // found from the instance rather than handed in, so a panel that shows a
    // stage's world and one that shows the scene ask the same question.
    core::InstanceId top = id;
    for (core::InstanceId up = world.parentOf(top); up.valid(); up = world.parentOf(top))
        top = up;
    const auto service = [&](std::string_view name) {
        const scene::ClassId classId = world.classes().findId(world.atoms().lookup(name));
        return classId == scene::InvalidClass ? core::InstanceId{} : world.findFirstChildOfClass(top, classId);
    };
    const core::InstanceId lighting = service("Lighting");
    core::InstanceId camera;
    if (const scene::WorkspaceComponent* workspace = world.workspaces().find(service("Workspace")))
        camera = workspace->currentCamera;

    core::TextKey key;
    switch (render::lookStanding(world, id, lighting, camera)) {
    case render::LookStanding::Counts:
    case render::LookStanding::NotALook:
        return {};
    case render::LookStanding::Disabled:
        key = world.bloomEffects().find(id) != nullptr ? ENG_TR("engine.overlay.look.bloom_disabled")
                                                       : ENG_TR("engine.overlay.look.disabled");
        break;
    case render::LookStanding::WrongParent:
        key = effect ? ENG_TR("engine.overlay.look.wrong_parent_effect")
                     : ENG_TR("engine.overlay.look.wrong_parent_environment");
        break;
    case render::LookStanding::NotFirst:
        key = ENG_TR("engine.overlay.look.not_first");
        break;
    case render::LookStanding::Outranked:
        key = ENG_TR("engine.overlay.look.outranked");
        break;
    }
    return core::engineCatalog().format(key);
}

// A button whose face is an icon, with the word as its fallback and its tooltip.
//
// Six drawn words in a row is a sentence somebody reads; six pictures is a
// control panel they aim at. Falls back to the word when there is no atlas or no
// cell for the id, because a button with nothing on it is not a smaller button
// -- and six `action.` ids are drawn tomorrow, so that path is live rather than
// theoretical.
//
// `strId` rather than the label, because the label changes with state (play
// becomes stop) and an ImGui id that changes with state is a button that loses
// its press half way through.
// `frameless` drops the button's background, its BORDER and its padding, leaving
// the picture and its hit box. All three, because a frame is not one thing: a
// transparent fill with the theme's outline still on it is a grey square around
// a sixteen-pixel glyph, which is what a chevron column looked like the day the
// shell learned to draw borders.
//
// It also removes a trap worth naming, because this file fell into it: a FRAMED
// button occupies its face plus the theme's padding, so a caller centring one by
// the size of the picture puts the picture in the middle of its row and the
// frame half a padding outside it, top and bottom. Frameless, the button IS the
// picture, and centring by the face is the same number as centring by the box. That is what a tree's chevron and its
// plus want: a filled rounded rectangle behind a sixteen-pixel glyph is a button drawn around a control that did not
// need one, and a column of them is a column of boxes. The affordance is the icon and the tooltip; the toolbar keeps
// its frames, because a toolbar button IS a button.
bool iconButton(const IconAtlas* icons, std::string_view id, float size, const char* strId, const char* word,
                const char* tip, bool frameless = false, bool fillRowHeight = false)
{
    if (frameless) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        // **`fillRowHeight` is what puts a toolbar's icons on the same line as
        // its text field.** Frameless normally means the button IS the picture,
        // which is what a tree row wants -- it places its chevron by hand at an
        // exact pitch, and a taller button would overflow the row. A TOOLBAR
        // wants the opposite: the tallest thing on the line is an input at frame
        // height, and an item shorter than it sits at the top of the line rather
        // than in the middle of it. Padding the button out to frame height
        // centres the picture inside it, which is one number rather than a
        // cursor nudge before every item -- and a nudge makes the line taller,
        // which is the gap that shows up under the tab bar.
        const float pad = fillRowHeight ? (ImGui::GetFrameHeight() - size) * 0.5f : 0.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, pad > 0.0f ? pad : 0.0f));
        // **A frame is a fill AND a border, and the border was surviving.**
        // `ImageButton` renders both, so a transparent `ImGuiCol_Button` leaves
        // the outline behind -- which is invisible while the theme draws no
        // borders and is a grey square around every chevron the moment it does.
        // Frameless has to mean both halves or it means neither.
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    }
    const auto unstyle = [frameless]() {
        if (frameless) {
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
        }
    };

    bool pressed = false;
    if (icons != nullptr && icons->ready() && icons->has(id)) {
        const IconSprite sprite = icons->find(id, static_cast<core::u32>(size + 0.5f));
        SDL_GPUTexture* native = g_device != nullptr ? rhi::nativeTexture(*g_device, icons->texture()) : nullptr;
        if (sprite.valid && native != nullptr) {
            pressed = ImGui::ImageButton(strId, static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)),
                                         ImVec2(size, size), ImVec2(sprite.u0, sprite.v0), ImVec2(sprite.u1, sprite.v1),
                                         ImVec4(0.0f, 0.0f, 0.0f, 0.0f), iconTint(icons, id));
            unstyle();
            if (tip != nullptr)
                ImGui::SetItemTooltip("%s", tip);
            return pressed;
        }
    }
    pressed = ImGui::Button(word);
    unstyle();
    if (tip != nullptr)
        ImGui::SetItemTooltip("%s", tip);
    return pressed;
}

// Paint without submitting another ImGui item: keyboard navigation, tooltips
// and disabled state must still belong to the labeled control underneath.
//
// `into` is the draw list to paint on, when it is not the current window's.
void paintActionIcon(const IconAtlas* atlas, std::string_view id, ImVec2 origin, float size, ImDrawList* into = nullptr)
{
    if (atlas == nullptr || !atlas->ready() || !atlas->has(id) || g_device == nullptr)
        return;
    const IconSprite sprite = atlas->find(id, static_cast<core::u32>(size + 0.5f));
    SDL_GPUTexture* texture = rhi::nativeTexture(*g_device, atlas->texture());
    if (!sprite.valid || texture == nullptr)
        return;
    ImDrawList* list = into != nullptr ? into : ImGui::GetWindowDrawList();
    list->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)), origin,
                   ImVec2(origin.x + size, origin.y + size), ImVec2(sprite.u0, sprite.v0), ImVec2(sprite.u1, sprite.v1),
                   ImGui::GetColorU32(ImGuiCol_Text));
}

bool labeledIconButton(const IconAtlas* atlas, std::string_view id, const char* label, ImVec2 size = ImVec2())
{
    if (atlas == nullptr || !atlas->ready() || !atlas->has(id))
        return ImGui::Button(label, size);
    const std::string face = tabIconPad() + label;
    const std::string stableLabel = face + "###" + label;
    const bool pressed = ImGui::Button(stableLabel.c_str(), size);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 extent = ImGui::GetItemRectSize();
    const float glyph = ImGui::GetFontSize();
    paintActionIcon(atlas, id,
                    ImVec2(min.x + std::max(ImGui::GetStyle().FramePadding.x,
                                            (extent.x - ImGui::CalcTextSize(face.c_str()).x) * 0.5f),
                           min.y + (extent.y - glyph) * 0.5f),
                    glyph);
    return pressed;
}

bool iconMenuItem(const IconAtlas* atlas, std::string_view id, const char* label, const char* shortcut = nullptr,
                  bool selected = false, bool enabled = true)
{
    if (atlas == nullptr || !atlas->ready() || !atlas->has(id))
        return ImGui::MenuItem(label, shortcut, selected, enabled);
    const std::string padded = tabIconPad() + label + "###" + label;
    const bool pressed = ImGui::MenuItem(padded.c_str(), shortcut, selected, enabled);
    const ImVec2 min = ImGui::GetItemRectMin();
    const float glyph = ImGui::GetFontSize();
    ImGui::BeginDisabled(!enabled);
    paintActionIcon(
        atlas, id,
        ImVec2(min.x + ImGui::GetStyle().ItemSpacing.x * 0.5f, min.y + (ImGui::GetItemRectSize().y - glyph) * 0.5f),
        glyph);
    ImGui::EndDisabled();
    return pressed;
}

// A submenu with an icon, lined up with the `iconMenuItem`s around it.
bool iconBeginMenu(const IconAtlas* atlas, std::string_view id, const char* label, bool enabled = true)
{
    if (atlas == nullptr || !atlas->ready() || !atlas->has(id))
        return ImGui::BeginMenu(label, enabled);
    const std::string padded = tabIconPad() + label + "###" + label;
    // **Where the row is, taken before the menu opens** (the owner: the icon
    // vanished while its submenu was open). An open `BeginMenu` has already
    // begun the submenu's window, so the item rect and the draw list read
    // after it are the submenu's -- and the icon was painted there, out of
    // sight. The row's text starts at the cursor, a line high.
    const ImVec2 row = ImGui::GetCursorScreenPos();
    ImDrawList* parent = ImGui::GetWindowDrawList();
    const bool open = ImGui::BeginMenu(padded.c_str(), enabled);
    const float glyph = ImGui::GetFontSize();
    ImGui::BeginDisabled(!enabled);
    paintActionIcon(atlas, id, row, glyph, parent);
    ImGui::EndDisabled();
    return open;
}

bool searchField(const IconAtlas* icons, const char* id, const char* hint, char* buffer, std::size_t capacity)
{
    if (icons == nullptr || !icons->ready())
        return ImGui::InputTextWithHint(id, hint, buffer, capacity);
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    const float glyph = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding.x + glyph + 4.0f, padding.y));
    const bool edited = ImGui::InputTextWithHint(id, hint, buffer, capacity);
    ImGui::PopStyleVar();
    const ImVec2 min = ImGui::GetItemRectMin();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    paintActionIcon(icons, icons::ActionSearch, ImVec2(min.x + padding.x, min.y + padding.y), glyph);
    ImGui::PopStyleColor();
    return edited;
}

// Shared World Core geometry from branding/mark.svg, tinted by the theme.
void drawBrandMark(ImVec2 origin, float size)
{
    const float unit = size / 64.0f;
    const ImU32 color = ImGui::GetColorU32(themeColor(palette().accent));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // ImGui requires clockwise winding for filled polygons and their AA fringe.
    const ImVec2 pieces[4][6] = {
        {ImVec2(30, 21), ImVec2(20, 27), ImVec2(20, 36), ImVec2(8, 43), ImVec2(8, 18), ImVec2(30, 6)},
        {ImVec2(34, 6), ImVec2(56, 18), ImVec2(56, 43), ImVec2(44, 36), ImVec2(44, 27), ImVec2(34, 21)},
        {ImVec2(22, 41), ImVec2(32, 46), ImVec2(42, 41), ImVec2(53, 47), ImVec2(32, 59), ImVec2(11, 47)},
        {ImVec2(32, 32), ImVec2(40, 36.5f), ImVec2(32, 41), ImVec2(24, 36.5f)},
    };
    for (int piece = 0; piece < 4; ++piece) {
        const int count = piece == 3 ? 4 : 6;
        for (int index = 0; index < count; ++index) {
            const ImVec2 point = pieces[piece][index];
            draw->PathLineTo(ImVec2(origin.x + point.x * unit, origin.y + point.y * unit));
        }
        draw->PathFillConcave(color);
    }
}

// One step of the content browser's path, as a control that looks like the text
// it is.
//
// **A breadcrumb is not a row of buttons.** `SmallButton` draws a filled,
// bordered box, which was invisible while the shell drew neither and became a
// rectangle around every folder name the moment a theme did. What a path wants
// is words you can click, so the frame goes and the affordance is what every
// file manager uses instead: the cursor changes and the word underlines.
[[nodiscard]] bool crumbButton(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    const bool pressed = ImGui::SmallButton(label);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(lo.x, hi.y), ImVec2(hi.x, hi.y), ImGui::GetColorU32(ImGuiCol_Text));
    }
    return pressed;
}

// Case-insensitive substring, for the add menu's filter box. ASCII, because
// every class name in this engine is (R1) and a full Unicode fold would be a
// dependency for a filter over thirty identifiers.
[[nodiscard]] bool containsFold(std::string_view haystack, std::string_view needle) noexcept
{
    if (needle.size() > haystack.size())
        return false;
    const auto lower = [](char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        std::size_t index = 0;
        while (index < needle.size() && lower(haystack[start + index]) == lower(needle[index]))
            ++index;
        if (index == needle.size())
            return true;
    }
    return false;
}

// Reused across frames rather than rebuilt: the panel fills these once per
// frame for as long as it is open, and a debug overlay that allocates a whole
// tree every frame is a profile artefact somebody eventually has to explain.
// The rows a person could actually see if they scrolled: everything not under a
// closed ancestor. Filled before anything is drawn, because a clipper needs to
// know how many rows exist before it decides which to draw -- and filled by a
// walk that never enters a closed subtree at all, so this is the only list the
// panel builds and its length is what is open rather than what exists.
std::vector<TreeRow> g_visible;
// The whole tree and what a search kept of it. Only filled while something is
// typed into the Explorer's search box -- see the block that uses them.
std::vector<TreeRow> g_searchRows;
std::unordered_set<core::u32> g_searchHits;
// The way down to something just created, moved or copied. See the reveal below.
std::vector<core::InstanceId> g_ancestors;
// And the row itself, so the list can be scrolled to it once it is laid out.
// Opening the way down is half the answer: a row four hundred deep in a tree is
// on screen only in the sense that nothing is hiding it.
core::InstanceId g_scrollTo;
// Which instances are expanded, and which have been seen at all. Held here
// rather than in ImGui's own tree state, because a clipped row's widget never
// runs and therefore has no state to hold.
std::unordered_set<core::u32> g_open;
// Set by `preserveExplorerOnNextWorld`, consumed by the next identity change.
bool g_keepExpansionOnce = false;
std::unordered_set<core::u32> g_openKnown;
// Which world the two sets above describe. They are keyed by instance INDEX and
// indices are recycled, so carrying them across a world would let a new instance
// inherit whether a dead one was expanded.
core::u64 g_explorerWorld = 0;
// The classes the add menu offers, and the world generation they were collected
// for. The registry does not change inside a world, so this is filled once and
// re-read; a fresh world refills it because a reload can register a different
// set (a project's own classes, a shipping build with no `DevOnly`).
std::vector<scene::ClassId> g_creatable;
core::u64 g_creatableWorld = 0;
// What the add menu is filtering by, and which row opened it. The filter is
// per-popup rather than global: it is a way through a list of thirty, not a
// setting anybody wants remembered.
std::array<char, 64> g_addFilter{};
// The instance index whose add popup is open, or zero. Slot zero is never an
// instance, which is what makes it usable as "none".
core::u32 g_addOpenRow = 0;
// Whether something nearer than the shell has already answered this frame's
// Escape. Set by the popup that closes itself with it and consumed by the
// shell's handler at the end of the frame, which is the only order the two can
// run in -- panels draw, then shortcuts. See both ends for why the popup being
// open is not a question that can be asked afterwards.
bool g_escapeTaken = false;
// Whether the scene is being authored this frame (`Editor::authoring`), for
// the panels that are drawn with no editor in hand. What they would ask for
// is refused at the drain whatever this says; this is what greys the asking.
bool g_authoring = true;
// Whether the game had the keyboard when the viewport was last drawn
// (`Editor::gameHasKeyboard`): the keys that open the editor's views are the
// game's then.
bool g_gameHasKeyboard = false;
// Last frame's tool and selection, so the shell can tell a brush being picked
// up from one already in hand, and a new selection from the same one.
Editor::Tool g_lastTool = Editor::Tool::Select;
core::InstanceId g_lastSelection;
core::usize g_lastSelectionCount = 0;
// Which row of the add menu the keyboard is on, and the filter that list was
// built from. See the menu itself for why a highlight of our own rather than
// ImGui's navigation: the keyboard belongs to the search box, and a list you
// arrow through without leaving the box is what a search box is for.
// The status toast's fade: which write of the status is on screen, and when it
// arrived. Wall clock through `ImGui::GetTime`, which is the right clock for
// chrome and reaches nothing the simulation can read.
core::u64 g_statusSerial = 0;
double g_statusAt = 0.0;

int g_addHighlight = 0;
std::string g_addHighlightFilter;
// The matches this frame, rebuilt each frame because the filter can change each
// frame. A member of the file rather than of the loop so the allocation is paid
// once rather than once a frame.
std::vector<ClassPick> g_addMatches;
std::vector<ClassPick> g_addPicks;
// Whether the right-drag in progress BEGAN over the viewport image. Latched on
// the press, because that is the only moment the question can be answered: once
// the pointer is held it stops reporting a position, and asking afterwards lets
// a right-click in any panel become a camera turn.
bool g_lookLatched = false;
std::vector<const scene::PropertyDesc*> g_properties;

// The drag-and-drop type the Explorer's rows publish and accept. ImGui matches
// payloads by this string, so it is one constant rather than a literal at two
// call sites: a typo in either is a drop that silently never happens, with
// nothing anywhere saying why.
// The About box's title: the product's name, from branding/brand.toml (ADR 0109),
// in the reader's language (ADR 0145); `###About` is the popup's identity.
[[nodiscard]] std::string aboutLabel()
{
    return core::tr(ENG_TR("engine.editor.dialog.about"), {{"brand", std::string_view(core::kBrandName)}});
}
[[nodiscard]] std::string aboutTitle()
{
    return aboutLabel() + "###About";
}

constexpr const char* kInstanceDragPayload = "engine.instances";

// What a dragged Explorer row carries.
struct InstanceDrag
{
    core::InstanceId id;
};

// The drag-and-drop type an entry of the content browser publishes, and what it
// carries: a content-relative path.
//
// **A path and not an index**, for the reason the content actions already
// resolve by path: the browser may have re-read its folder between the drag
// starting and the drop landing, and an index into a list that moved is how a
// drop lands on the row below the one somebody grabbed.
//
// Fixed-size, because an ImGui payload is bytes it copies -- a pointer into a
// string the browser owns would dangle the moment that folder is re-read.
constexpr const char* kContentDragPayload = "engine.content";
struct ContentDrag
{
    // Long enough for any path a person makes; a longer one is not dragged at
    // all rather than dragged cut short (`beginContentDrag`).
    char path[512]{};
    // The class of the instance the stamp is a file OF, so a drop target can
    // decide whether it wants this one BEFORE it lights up. A field that
    // highlighted for any stamp and then refused a `Part` where a `Material`
    // belongs is the broken promise the Explorer's own drop rule exists to
    // avoid -- and the browser already knows the answer, because it draws the
    // row with that class's icon.
    char rootClass[48]{};
    // A folder: said as a flag, not as a class name nothing else can take.
    bool folder = false;
};

// Whether a browser drag is a material file (ADR 0090), by the compound suffix
// every material carries wherever it sits.
[[nodiscard]] bool isMaterialDrag(const ContentDrag& drag) noexcept
{
    return asset::isMaterialPath(std::string_view(drag.path));
}

// And a stamp, which is the one kind a drop PLACES.
[[nodiscard]] bool isStampDrag(const ContentDrag& drag) noexcept
{
    return contentKindOf(std::filesystem::path(drag.path).filename().string()) == ContentKind::Stamp;
}

// A folder, and a picture: what a `Sky` row takes (ADR 0096).
[[nodiscard]] bool isFolderDrag(const ContentDrag& drag) noexcept
{
    return drag.folder;
}

// **A drop into a folder of the content** (the owner: things are dragged
// between folders): a file or folder from the browser moves there, with every
// reference to it (`Editor::moveContent`). Not into where it already is, and a
// folder not into itself or anything inside it -- refused before the target
// lights up, so it never promises what it will not do. Call inside a
// `BeginDragDropTarget`.
void acceptContentMove(EditorCommands& commands, std::string_view folder)
{
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    if (peek == nullptr || !peek->IsDataType(kContentDragPayload))
        return;
    const auto* drag = static_cast<const ContentDrag*>(peek->Data);
    const std::string_view path(drag->path);
    const std::size_t slash = path.rfind('/');
    const std::string_view parent = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
    if (parent == folder || path == folder ||
        (folder.size() > path.size() && folder.starts_with(path) && folder[path.size()] == '/'))
        return;
    if (const ImGuiPayload* took = ImGui::AcceptDragDropPayload(kContentDragPayload); took != nullptr) {
        commands.moveContent = static_cast<const ContentDrag*>(took->Data)->path;
        commands.moveContentInto = std::string(folder);
    }
}

[[nodiscard]] bool isTextureDrag(const ContentDrag& drag) noexcept
{
    return contentKindOf(std::filesystem::path(drag.path).filename().string()) == ContentKind::Texture;
}

// A mesh, which a drop into the world or the tree makes a `MeshPart` of.
[[nodiscard]] bool isMeshDrag(const ContentDrag& drag) noexcept
{
    return !drag.folder && contentKindOf(std::filesystem::path(drag.path).filename().string()) == ContentKind::Mesh;
}

// The instance tree, virtualised.
//
// **Every row used to be drawn every frame**, which on the flagship is 4,300
// `TreeNodeEx` calls sixty times a second for a panel showing thirty of them.
// The content browser beside this one clips, and applying that standard to one
// panel and not the other was an inconsistency rather than a decision.
//
// Clipping a TREE is not clipping a list, and the difference is the reason this
// is longer than it was. A clipper skips rows, and `TreeNodeEx` keeps its own
// open/closed state -- so a row nobody drew has no state, and its children would
// not know whether to appear. So the open set is held HERE, keyed by instance,
// the visible rows are computed before anything is drawn, and the clipper runs
// over that flat list. Which is what a virtualised tree view is.
// `commands` and `dialogs` are null in the F3 overlay, and that is the
// distinction rather than a convenience: **the overlay inspects and the editor
// edits.** Offering Delete over a running game would be offering an edit whose
// only result is a world nobody can put back.
// The colour menu both panels open on a folder.
//
// **Presets first and a picker underneath**, which is the order of how often
// each is used: ten swatches cover almost every case in one click, and the
// picker is there so the palette is a shortcut rather than a limit. "None"
// comes last because taking a colour off is the rarest of the three and
// putting it where a preset would be is how somebody clears one by accident.
//
// Returns true when `color` changed, so the caller can record one undo step for
// it rather than one per frame the picker is open.
[[nodiscard]] bool colorMenu(std::optional<core::Color3>& color)
{
    bool changed = false;

    const std::span<const core::Color3> palette = Editor::folderPalette();
    const float swatch = ImGui::GetFrameHeight();
    for (std::size_t index = 0; index < palette.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        const core::Color3 candidate = palette[index];
        const ImVec4 rgba(candidate.r, candidate.g, candidate.b, 1.0f);
        if (ImGui::ColorButton("##swatch", rgba, ImGuiColorEditFlags_NoTooltip, ImVec2(swatch, swatch))) {
            color = candidate;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        // Five and five rather than one row of ten, because a menu as wide as
        // ten swatches is wider than every other item in it.
        if (index % 5 != 4)
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    }

    ImGui::Separator();

    // Seeded with what is already there, so opening the picker on a coloured
    // folder starts from its colour rather than from black.
    float components[3]{0.6f, 0.62f, 0.66f};
    if (color.has_value()) {
        components[0] = color->r;
        components[1] = color->g;
        components[2] = color->b;
    }
    ImGui::SetNextItemWidth(swatch * 5.0f + ImGui::GetStyle().ItemInnerSpacing.x * 4.0f);
    if (ImGui::ColorEdit3("##pick", components, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        color = core::Color3{components[0], components[1], components[2]};
        changed = true;
    }

    if (color.has_value() && ImGui::MenuItem(core::tr(ENG_TR("engine.editor.color_menu.none")))) {
        color.reset();
        changed = true;
    }
    return changed;
}

// **One class picker, for every place that asks "which class".** The Explorer's
// add-a-child menu grew it -- a search box, a wrapping highlight, Enter to take
// the highlighted one, Escape to leave, and the IDL's own prose on hover -- and
// the content browser needs the same thing to ask which kind of stamp to make. A
// second copy would be a second set of keyboard rules to keep in step, and they
// would stop being the same set the first time one of them was improved.
//
// Draws INSIDE a popup the caller has already begun. Returns the class chosen
// this frame, or `InvalidClass`, and closes the popup when it returns one.
//
// `spacing` is the caller's item spacing: this window is a menu and not a row,
// and the two callers draw rows at different pitches.
// A five-pointed star centred on `centre`: filled for a favourite, drawn as an
// outline for one that could be. Drawn rather than taken from the font, which
// carries no star glyph.
void drawStar(ImDrawList* draw, ImVec2 centre, float radius, bool filled, ImU32 colour)
{
    ImVec2 points[10];
    for (int index = 0; index < 10; ++index) {
        const float reach = index % 2 == 0 ? radius : radius * 0.45f;
        const float angle = -1.5707964f + static_cast<float>(index) * 0.62831855f;
        points[index] = ImVec2(centre.x + std::cos(angle) * reach, centre.y + std::sin(angle) * reach);
    }
    if (filled) {
        for (int index = 0; index < 10; ++index)
            draw->AddTriangleFilled(centre, points[index], points[(index + 1) % 10], colour);
    }
    else {
        draw->AddPolyline(points, 10, colour, ImDrawFlags_Closed, 1.2f);
    }
}

// Where a class does its job, as a sentence for the dimmed rows' tooltip.
[[nodiscard]] std::string placesOf(const scene::World& world, scene::ClassId id)
{
    const scene::ClassDescriptor* descriptor = world.classes().find(id);
    while (descriptor != nullptr && descriptor->parents.empty() && descriptor->super != scene::InvalidClass)
        descriptor = world.classes().find(descriptor->super);
    std::string places;
    if (descriptor == nullptr)
        return places;
    for (const std::string_view place : descriptor->parents) {
        if (!places.empty())
            places += ", ";
        places += place;
    }
    return places;
}

[[nodiscard]] scene::ClassId drawClassPicker(const scene::World& world, const Inspector& inspector,
                                             core::InstanceId parent, const IconAtlas* icons, ImVec2 spacing)
{
    scene::ClassId picked = scene::InvalidClass;
    // Same as the row menu above: this window is not a row.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
    if (g_creatableWorld != inspector.worldIdentity() || g_creatable.empty()) {
        g_creatableWorld = inspector.worldIdentity();
        collectCreatableClasses(world, g_creatable);
    }

    // Focused on open, because a list of thirty is a list you type
    // at rather than scroll -- and the first keystroke landing in
    // the box is what makes that true.
    if (ImGui::IsWindowAppearing()) {
        g_addFilter.fill(0);
        g_addHighlight = 0;
        g_addHighlightFilter.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(210.0f * ImGui::GetStyle().FontScaleMain);
    // "search" rather than "filter": one is what a person is doing
    // and the other is what the code is doing, and a hint is written
    // for the first of those. It is also the word on every other box
    // in this shell now, and three names for one gesture is three
    // things to learn.
    searchField(icons, "##add-filter", core::tr(ENG_TR("engine.editor.class_picker.search")), g_addFilter.data(),
                g_addFilter.size());

    // **Escape closes it**, which is what Escape does to every
    // transient thing on a screen. The shell's own Escape handler
    // deliberately refuses while a popup is open -- it would take
    // the key from the dialogs that need it -- so the popup that
    // wants it has to ask, and this is the one that does.
    //
    // And it SAYS it took the key, because closing a popup takes
    // effect immediately: by the time the shell's handler runs there
    // is no popup left for its guard to see, and the one press
    // closed the menu and dropped the selection the menu had been
    // opened for. One Escape, one thing.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        g_escapeTaken = true;
    }

    const std::string_view filter(g_addFilter.data());

    // **The matches are collected before anything is drawn**, and
    // that is what makes the keyboard work at all: Up and Down have
    // to know how many rows there are, and Enter has to know which
    // class it is about, both before the first `Selectable` decides
    // whether it is the highlighted one.
    //
    // **In three groups for a parent** (see `orderClassPicks`): what is
    // starred for this kind of parent, what does its job here, and the rest,
    // dimmed. The content browser's picker has no parent and one group.
    std::string parentClass;
    if (parent.valid() && world.alive(parent)) {
        if (const scene::ClassDescriptor* kind = world.classes().find(world.classOf(parent)); kind != nullptr)
            parentClass = std::string(world.atoms().text(kind->name));
    }
    ClassFavorites& favorites = classFavorites();
    orderClassPicks(world, parent, g_creatable,
                    parentClass.empty() ? std::span<const std::string>{} : favorites.of(parentClass), g_addPicks);
    g_addMatches.clear();
    for (const ClassPick& pick : g_addPicks) {
        const scene::ClassDescriptor* candidate = world.classes().find(pick.id);
        if (candidate == nullptr)
            continue;
        if (filter.empty() || containsFold(world.atoms().text(candidate->name), filter))
            g_addMatches.push_back(pick);
    }

    // **Typing puts you back on the first match**, which is the
    // whole of "it always comes with the first thing selected and I
    // control from there": a highlight left three rows down while
    // the list under it changed is a highlight pointing at whatever
    // happens to be there now.
    if (g_addHighlightFilter != filter) {
        g_addHighlightFilter = filter;
        g_addHighlight = 0;
    }

    // **A highlight of our own rather than ImGui's navigation.** The
    // search box has the keyboard -- it has to, or the first
    // keystroke would go nowhere -- and handing the arrows to nav
    // would mean tabbing out of the box to use them. Every command
    // palette in every editor works this way for the same reason:
    // you type and you arrow, and neither interrupts the other.
    //
    // Single-line `InputText` uses neither arrow, so nothing is
    // being taken from it.
    const int matchCount = static_cast<int>(g_addMatches.size());
    bool followHighlight = false;
    if (matchCount > 0) {
        // Wrapped, because a list this short is a ring: pressing Up
        // on the first row to reach the last is faster than
        // twenty-nine presses of Down, and there is no scrollbar
        // position to be confused about at either end.
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
            g_addHighlight = (g_addHighlight + 1) % matchCount;
            followHighlight = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
            g_addHighlight = (g_addHighlight + matchCount - 1) % matchCount;
            followHighlight = true;
        }
    }
    g_addHighlight = matchCount == 0 ? 0 : std::clamp(g_addHighlight, 0, matchCount - 1);

    // Enter makes the highlighted one, which is the other half of
    // never touching the mouse. Both Return keys, because a numpad
    // Enter is an Enter.
    scene::ClassId chosenByKey = scene::InvalidClass;
    if (matchCount > 0 &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        chosenByKey = g_addMatches[static_cast<core::usize>(g_addHighlight)].id;

    if (ImGui::BeginChild("add-list", ImVec2(210.0f, 260.0f))) {
        // **Whether the pointer has actually moved.** Hovering a row
        // moves the highlight, so the mouse and the keyboard never
        // point at two different things -- but a pointer resting
        // over the list while somebody arrows past it would drag the
        // highlight back under the cursor on every frame, and the
        // arrows would appear not to work at all.
        const ImGuiIO& io = ImGui::GetIO();
        const bool pointerMoved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;

        for (int match = 0; match < matchCount; ++match) {
            const ClassPick& pick = g_addMatches[static_cast<core::usize>(match)];
            const scene::ClassId classId = pick.id;
            const scene::ClassDescriptor* candidate = world.classes().find(classId);
            if (candidate == nullptr)
                continue;
            // A line between the groups, so where the favourites end and the
            // dimmed rest begins is seen rather than inferred.
            if (match > 0 && g_addMatches[static_cast<core::usize>(match - 1)].group != pick.group)
                ImGui::Separator();
            const bool dimmed = pick.group == ClassPickGroup::Elsewhere;
            const std::string_view candidateName = world.atoms().text(candidate->name);

            // **The icon a row of this class would wear**, drawn
            // beside the name here for the reason it is drawn in the
            // tree at all: a list of thirty identifiers is read by
            // shape before it is read by word, and a menu that names
            // what it will make without showing it is a menu you have
            // to read twice.
            //
            // The name is still what the item IS -- the selectable
            // spans the row and the icon is drawn over it -- so
            // filtering, keyboard focus and the click target are
            // exactly what they were.
            char item[96];
            (void)std::snprintf(item, sizeof(item), "##%.*s", static_cast<int>(candidateName.size()),
                                candidateName.data());
            const ImVec2 itemOrigin = ImGui::GetCursorPos();
            const float rowWidth = ImGui::GetContentRegionAvail().x;
            const bool highlighted = match == g_addHighlight;
            // Overlap allowed, so the star drawn over its right end takes its
            // own click instead of choosing the class.
            const bool chosen = ImGui::Selectable(item, highlighted, ImGuiSelectableFlags_AllowOverlap);
            // Taken here, because the icon and the name are drawn
            // over the selectable afterwards and `IsItemHovered`
            // answers about the LAST item -- which would make the
            // description below appear only over the word.
            const bool itemHovered = ImGui::IsItemHovered();
            if (itemHovered && pointerMoved)
                g_addHighlight = match;
            // Followed only when the KEYBOARD moved it. Scrolling to
            // the highlight every frame would fight the scrollbar
            // the moment somebody dragged it.
            if (highlighted && followHighlight)
                ImGui::SetScrollHereY(0.5f);

            const float itemIcon = ImGui::GetTextLineHeight();
            ImGui::SetCursorPos(itemOrigin);
            // The ATLAS falls back for a class no theme has heard
            // of -- `debug_overlay_tests.cpp` holds it to that -- so
            // there is nothing to do here for a project's own class.
            // False means there is no atlas at all, which is a build
            // with no icons rather than a class with none, and then
            // the name simply stands where it always did.
            if (dimmed)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
            if (drawIcon(icons, classIconFor(icons, &world.classes(), &world.atoms(), candidateName), itemIcon))
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            else
                ImGui::SetCursorPos(itemOrigin);
            ImGui::TextUnformatted(candidateName.data(), candidateName.data() + candidateName.size());
            if (dimmed)
                ImGui::PopStyleVar();

            // **The star**, on the right: filled on a favourite, an outline on
            // the row under the pointer, nothing elsewhere -- thirty outlines
            // would be a column of noise. Only where there is a parent to be a
            // favourite FOR.
            bool starHovered = false;
            if (!parentClass.empty()) {
                const float starSize = ImGui::GetTextLineHeight();
                const bool favourite = pick.group == ClassPickGroup::Favorite;
                ImGui::SetCursorPos(ImVec2(itemOrigin.x + rowWidth - starSize - 2.0f, itemOrigin.y));
                char starId[104];
                (void)std::snprintf(starId, sizeof(starId), "##star%.*s", static_cast<int>(candidateName.size()),
                                    candidateName.data());
                if (ImGui::InvisibleButton(starId, ImVec2(starSize, starSize))) {
                    (void)favorites.toggle(parentClass, candidateName);
                    (void)saveClassFavorites(classFavoritesFile(), favorites);
                }
                starHovered = ImGui::IsItemHovered();
                if (starHovered)
                    ImGui::SetTooltip("%s", core::tr(favourite ? ENG_TR("engine.editor.class_picker.unstar_tip")
                                                               : ENG_TR("engine.editor.class_picker.star_tip"),
                                                     {{"class", parentClass}})
                                                .c_str());
                if (favourite || itemHovered || starHovered) {
                    const ImVec2 box = ImGui::GetItemRectMin();
                    const ImU32 colour = favourite || starHovered ? ImGui::GetColorU32(themeColor(palette().warning))
                                                                  : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    drawStar(ImGui::GetWindowDrawList(), ImVec2(box.x + starSize * 0.5f, box.y + starSize * 0.5f),
                             starSize * 0.42f, favourite, colour);
                }
            }

            if (chosen || classId == chosenByKey) {
                picked = classId;
                ImGui::CloseCurrentPopup();
            }
            // The IDL's own prose, which the properties grid already
            // shows for a property and which is the only description
            // of a class anywhere at runtime.
            if ((candidate->doc[0] != 0 || dimmed) && itemHovered && !starHovered) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                // Why it is dimmed, before what it is.
                if (dimmed) {
                    const std::string places = placesOf(world, classId);
                    ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s",
                                       core::tr(ENG_TR("engine.editor.explorer.inert_under"),
                                                {{"parent", parentClass}, {"places", places}})
                                           .c_str());
                }
                ImGui::TextUnformatted(candidate->doc);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return picked;
}

// The tree, from `root` down.
//
// **`root` is not drawn when it is the world's**, and IS drawn when it is a
// stamp's. Those are two different questions with the same shape. `game` has no
// properties worth a row, cannot be renamed, deleted, duplicated or reparented,
// and every useful thing is under it -- so a row for it is a line of chrome and
// an indent charged to every row beneath. A STAMP's root is the opposite: it is
// the thing being edited, it is the one instance whose name and transform are
// its own, and hiding it would leave somebody editing a set of children with
// nothing saying what they are children OF.
void drawExplorer(scene::World& world, core::InstanceId root, Inspector& inspector, EditorCommands* commands,
                  EditorDialogs* dialogs, const IconAtlas* icons, bool showGenerated, bool drawRoot = false,
                  bool hasClipboard = false)
{
    // How much of the depth is above the first row that gets drawn. One when
    // the root is hidden, zero when it is not -- and every position on a row is
    // measured from it.
    const u32 depthBase = drawRoot ? 0u : 1u;

    // **The search box, above the tree it searches.**
    //
    // Held per WORLD, like the expanded set below and for the same reason: a
    // filter that survived a scene load would be hiding most of whatever
    // somebody just opened, with nothing on screen saying why.
    static std::array<char, 96> explorerFilter{};
    ImGui::SetNextItemWidth(-1.0f);
    searchField(icons, "##explorer-filter", core::tr(ENG_TR("engine.editor.explorer.search")), explorerFilter.data(),
                explorerFilter.size());
    const std::string_view explorerNeedle{explorerFilter.data()};

    // **A DIFFERENT world opens collapsed; a restored one does not.** Loading a
    // scene or starting a new one arrives here as a world nobody has expanded
    // anything in, and a tree that remembered would be showing somebody the
    // shape of the scene they just closed -- worse, slot indices restart, so a
    // new instance would inherit whether a dead one was expanded.
    //
    // A stop, an undo or a redo is not that (D071). `World::restore` carries
    // generations precisely so an id keeps its meaning, so every row this set
    // names is still the row it named -- and an undo that collapsed the whole
    // tree took back more than the edit it was asked to.
    if (g_explorerWorld != inspector.worldIdentity()) {
        g_explorerWorld = inspector.worldIdentity();
        // Unless the world that arrived is the same one rebuilt -- see
        // `DebugOverlay::preserveExplorerOnNextWorld`.
        if (!g_keepExpansionOnce) {
            g_open.clear();
            g_openKnown.clear();
            explorerFilter.fill(0);
        }
        g_keepExpansionOnce = false;
    }

    // **Something was just made, moved or copied: open the way to it.** Before
    // visibility is decided rather than after, or the row would appear one
    // frame late -- and a person who has just pressed a plus is looking at the
    // tree on this frame.
    //
    // A parent that has never been opened is not opened by gaining a child, and
    // an EMPTY one has no chevron to open it with -- so a `Part` created inside
    // a fresh `Folder` was invisible every time, which reads as "I cannot add a
    // child to a folder". The request is one-shot, so a row deliberately closed
    // afterwards stays closed.
    if (const core::InstanceId wanted = inspector.takeReveal(); wanted.valid()) {
        collectAncestors(world, wanted, root, g_ancestors);
        for (const core::InstanceId ancestor : g_ancestors) {
            g_open.insert(ancestor.index);
            g_openKnown.insert(ancestor.index);
        }
        g_scrollTo = wanted;
    }

    // **One walk, and it does not enter what nobody can see** (ADR 0054). This
    // was a full preorder over every instance in the world followed by a second
    // pass that dropped the ones under something closed -- so an editor on a
    // world of thirty thousand instances paid for thirty thousand of them every
    // frame to draw the four rows on screen. The drawing was already clipped,
    // which is exactly why nothing ever showed it in a profile: the cost was in
    // deciding what to draw, not in drawing it.
    //
    // Parent before child, so a closed or hidden ancestor is answered once and
    // its whole subtree is never touched.
    // **Searching a TREE is not filtering a list**, and the difference is what
    // this block is. A row that matches is no use on its own: everything above
    // it has to be on screen or the match is unreachable, and everything under a
    // matching container is what somebody was looking FOR when they typed the
    // container's name. So the set is the matches, their ancestors, and their
    // descendants -- and while it is non-empty the open/closed state is ignored,
    // because a person searching has already said what they want to see.
    //
    // **And this one walk IS the whole world**, which is the cost the panel
    // otherwise refuses to pay (ADR 0054). It is paid only while something is
    // typed: a search is a thing somebody is doing, and the frame after they
    // clear the box the panel is back to costing what is open.
    if (!explorerNeedle.empty()) {
        collectTree(world, root, g_searchRows);

        g_searchHits.clear();
        // Preorder, so `depth` alone says which run of rows is under a match:
        // everything deeper than a hit, until a row at the hit's depth or above.
        u32 keepBelow = std::numeric_limits<u32>::max();
        for (const TreeRow& row : g_searchRows) {
            if (row.depth <= keepBelow)
                keepBelow = std::numeric_limits<u32>::max();
            if (keepBelow != std::numeric_limits<u32>::max()) {
                g_searchHits.insert(row.id.index);
                continue;
            }
            if (!showGenerated && world.generated(row.id))
                continue;
            if (containsFold(world.atoms().text(world.name(row.id)), explorerNeedle)) {
                g_searchHits.insert(row.id.index);
                keepBelow = row.depth;
            }
        }

        // The way up from every hit, so nothing on screen is an orphan.
        for (const TreeRow& row : g_searchRows) {
            if (!g_searchHits.contains(row.id.index))
                continue;
            for (core::InstanceId up = world.parentOf(row.id); up.valid() && up != root; up = world.parentOf(up))
                g_searchHits.insert(up.index);
        }

        g_visible.clear();
        for (const TreeRow& row : g_searchRows) {
            if (!showGenerated && world.generated(row.id))
                continue;
            if ((row.depth > 0 || drawRoot) && g_searchHits.contains(row.id.index))
                g_visible.push_back(row);
        }
    }
    else
        collectVisibleTree(
            world, root, drawRoot,
            [&](const TreeRow& row) {
                // **What streaming made is not the scene.** It pumps in edit mode as
                // well as in play -- deliberately, because a world you cannot see is
                // a world you cannot edit -- but the serializer skips a generated
                // subtree whole and nothing authored may live in one, so sixty
                // `Chunk_x_y_z` folders standing between a person and the four
                // things they wrote is the root's own complaint again: scrolling
                // past a world to find the thing you came for. Window > Streamed
                // Content brings them back.
                if (!showGenerated && world.generated(row.id))
                    return TreeVisit::Skip;

                const bool hasChildren = world.childCount(row.id) > 0;
                if (!hasChildren)
                    return TreeVisit::Collapsed;

                // The services under `game` are what anyone opening this wants to
                // see; deeper than that is a project's own tree and is its business.
                // Seeded once per instance rather than every frame, so collapsing
                // one stays collapsed.
                if (!g_openKnown.contains(row.id.index)) {
                    g_openKnown.insert(row.id.index);
                    // **The root only**, and when it is the world's it is not drawn
                    // -- opening it is what puts the services on screen at all. What
                    // is INSIDE them is the scene, and showing all of that means
                    // scrolling past a world to find the thing you came for.
                    //
                    // A stamp's root opens for the opposite reason: it IS what
                    // somebody opened, and a stage that starts collapsed shows one
                    // row.
                    if (row.depth == 0)
                        g_open.insert(row.id.index);
                }
                return g_open.contains(row.id.index) ? TreeVisit::Expanded : TreeVisit::Collapsed;
            },
            g_visible);

    // **One height for a row, decided once and told to everybody.**
    //
    // A tree row held three things of three different heights -- a framed
    // arrow, a square icon and a line of text -- and each of the three decided
    // where it sat. Worse, `ImGuiListClipper` was left to assume a row pitch of
    // its own, which is `GetTextLineHeightWithSpacing()` and is not what a
    // framed arrow makes a row: the selection highlight came out taller than
    // the pitch the clipper had reserved, so it covered the row above and the
    // row below, and nothing on the row lined up with anything else on it.
    //
    // So the height is a number: the frame height, which is the font's line
    // plus the theme's padding and therefore scales with both -- responsive
    // without being a magic constant. The clipper is told it, the selectable is
    // built to it, everything drawn on the row is centred against it, and the
    // cursor is placed at exactly the next multiple of it. Four agreements
    // instead of four guesses.
    const float indentSpacing = ImGui::GetStyle().IndentSpacing;
    const float rowHeight = ImGui::GetFrameHeight();

    // **And no vertical item spacing while the rows are drawn.** A `Selectable`
    // pads its highlight with HALF of `ItemSpacing.y` above and below -- which
    // is what makes a column of them read as one continuous list, and which is
    // exactly wrong once the rows are laid out at an exact pitch: the padding
    // has nowhere to go but into the row above and the row below. Zero here
    // makes the highlight the row, and every position on the row is explicit
    // anyway, so nothing else in this loop notices.
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing.x, 0.0f));

    // Where row zero starts, in screen space. **The exact pitch pays for itself
    // here**: with it, row `i` is at `listTop + i * rowHeight` and the guide
    // lines below can be drawn for rows the clipper never visited -- which is
    // what makes a line reaching a child scrolled half off the bottom still
    // reach it.
    const ImVec2 listTop = ImGui::GetCursorScreenPos();

    // **Scrolled to, and only when it is not already on screen.** The rows sit
    // at an exact pitch, so the row a reveal asked for can be found and scrolled
    // to without the clipper having drawn it -- which matters, because the whole
    // case is a row the clipper is skipping.
    //
    // The "already visible" test is what stops this being a jerk rather than a
    // help: clicking a part in the viewport whose row is right there should move
    // nothing, and a `SetScrollHereY` every time would yank the list under
    // somebody every click.
    if (g_scrollTo.valid()) {
        const core::InstanceId wanted = g_scrollTo;
        g_scrollTo = core::InstanceId{};
        for (std::size_t index = 0; index < g_visible.size(); ++index) {
            if (g_visible[index].id != wanted)
                continue;
            const float rowTop = ImGui::GetCursorPosY() + static_cast<float>(index) * rowHeight;
            const float viewTop = ImGui::GetScrollY();
            const float viewHeight = ImGui::GetContentRegionAvail().y;
            if (rowTop < viewTop || rowTop + rowHeight > viewTop + viewHeight)
                ImGui::SetScrollY(rowTop - (viewHeight - rowHeight) * 0.5f);
            break;
        }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_visible.size()), rowHeight);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const TreeRow& row = g_visible[static_cast<std::size_t>(index)];
            const bool hasChildren = world.childCount(row.id) > 0;

            ImGui::PushID(static_cast<int>(row.id.index));

            const ImVec2 rowOrigin = ImGui::GetCursorPos();
            const float iconSize = ImGui::GetTextLineHeight();
            // Never above the row's top, whatever it is handed. An element
            // taller than the row is a mistake, and starting it half a
            // padding above the row is that mistake spread over two rows.
            const auto centred = [&](float height) {
                const float slack = rowHeight - height;
                return rowOrigin.y + (slack > 0.0f ? slack * 0.5f : 0.0f);
            };

            const std::string_view instanceName = world.atoms().text(world.name(row.id));
            const scene::ClassDescriptor* classDescriptor = world.classes().find(world.classOf(row.id));
            const std::string_view className =
                classDescriptor != nullptr ? world.atoms().text(classDescriptor->name) : std::string_view("?");

            // **The game's service set apart from the scene's** (ADR 0105): a
            // rule above `Workspace`, the first service after
            // `GlobalScriptService` and where what a scene change replaces
            // begins.
            if (className == "Workspace" && world.parentOf(row.id) == root) {
                const ImVec2 top = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(top, ImVec2(top.x + ImGui::GetContentRegionAvail().x, top.y),
                                                    ImGui::GetColorU32(ImGuiCol_Separator));
            }

            char label[192];
            const bool haveIcon = icons != nullptr && icons->ready();
            if (haveIcon) {
                // **The class is not in the text, because the icon IS the
                // class** -- that is what a `class.<ClassName>` id means, and
                // saying it twice costs width the name needs. Without an atlas
                // it comes back, because a row showing neither has lost the
                // answer rather than moved it.
                (void)std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(instanceName.size()),
                                    instanceName.data());
            }
            else {
                (void)std::snprintf(label, sizeof(label), "%.*s  (%.*s)", static_cast<int>(instanceName.size()),
                                    instanceName.data(), static_cast<int>(className.size()), className.data());
            }

            // **The row itself is drawn first and spans the full width**,
            // arrow included and indent ignored. It owns the hit test, the
            // highlight and the height; the arrow, the icon, the name and the
            // plus are placed on top of it afterwards, which is also what puts
            // them OVER the highlight rather than under it.
            if (ImGui::Selectable("##row", inspector.isSelected(row.id),
                                  ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0.0f, rowHeight))) {
                // Ctrl adds and removes, shift takes the run from the primary
                // to here, a plain click replaces. The range is taken over
                // `g_visible` rather than over the whole tree, because a range
                // somebody drew with the mouse across a collapsed branch would
                // pick up rows they cannot see.
                const ImGuiIO& io = ImGui::GetIO();
                if (io.KeyCtrl) {
                    inspector.toggle(row.id);
                }
                else if (io.KeyShift && inspector.selection().valid()) {
                    selectVisibleRange(inspector, g_visible, inspector.selection(), row.id);
                }
                else {
                    inspector.select(row.id);
                }

                // **Double-clicking a script opens it** (ADR 0057), which is the
                // door somebody looks for first. Recorded rather than acted on:
                // the tab needs the instance's `Source` and where it came from,
                // and both are the loop's to look up at the safe point.
                if (commands != nullptr && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    world.classes().isA(world.classOf(row.id),
                                        world.classes().findId(world.atoms().lookup("BaseScript")))) {
                    commands->openScript = row.id;
                }
            }
            // Asked while the selectable is still the last item, which is the
            // only moment it answers about the ROW rather than about the button
            // drawn on top of it.
            const bool rowHovered = ImGui::IsItemHovered();
            // The popup keeps the plus drawn while it is open, or the button
            // vanishes the moment the pointer leaves the row to reach the list
            // and takes its own popup with it.
            const bool addOpen = g_addOpenRow == row.id.index;

            // --- Moving something by dragging it ---------------------------
            //
            // **Both halves live on the SAME item as the selection**, and they
            // have to: ImGui reads the last item for the source's id and for
            // the target's rectangle, so a drag source attached to anything
            // other than the row's own selectable drags the wrong thing.
            //
            // The drop target is asked first. `BeginDragDropSource` returns in
            // three lines unless this row is the one being held, but on the row
            // that IS held it opens a tooltip window, and a target asked
            // afterwards would be reading that window's last item rather than
            // the row's.
            if (commands != nullptr && g_authoring && ImGui::BeginDragDropTarget()) {
                // **Two drops, told apart by what is being dragged.**
                //
                // A row of this tree is a reparent, and the row lights only
                // where the move would do something -- a row that highlights and
                // then refuses is the same broken promise a field that takes a
                // drag the world rejects makes, so the rule is asked here, a
                // frame early, through the one function the verb itself uses.
                //
                // An entry of the content browser is a PREFAB, and dropping one
                // places it under this row. Anywhere authorable takes one.
                const ImGuiPayload* peek = ImGui::GetDragDropPayload();
                const bool fromTree = peek != nullptr && peek->IsDataType(kInstanceDragPayload);

                // **Where in the row's HEIGHT the pointer is decides which of
                // two moves this is** (S5.18). The middle is a reparent, which
                // is what this tree has always done; the top and bottom bands
                // are a reorder among siblings, which `World::moveChild` has
                // been able to do since the verb was written and nothing could
                // ask for.
                //
                // Bands rather than a separate item between rows, for two
                // reasons. The clipper draws only the rows a person can see, so
                // an interleaved zero-height item would have to be clipped in
                // step with them; and a gap thin enough not to disturb the
                // layout is a gap too thin to hit. A quarter of the row at each
                // end is what Unity, Unreal and Godot all land on, and it leaves
                // half the row still meaning "into this".
                const ImVec2 rowMin = ImGui::GetItemRectMin();
                const ImVec2 rowMax = ImGui::GetItemRectMax();
                const float dropHeight = rowMax.y - rowMin.y;
                const float band = dropHeight * 0.25f;
                const float pointerY = ImGui::GetMousePos().y;
                const bool above = fromTree && dropHeight > 0.0f && pointerY < rowMin.y + band;
                const bool below = fromTree && dropHeight > 0.0f && pointerY > rowMax.y - band;

                // **The edge of a row under ANOTHER parent is still a place.**
                // It used to accept the drop and do nothing, because a reorder
                // only moves among siblings -- so a script dragged to between
                // two rows of a different folder simply stayed where it was.
                // Now it moves in and lands at the line.
                const core::InstanceId edgeParent = world.parentOf(row.id);
                const core::InstanceId edgeDragged =
                    fromTree ? (inspector.selectionCount() == 1 ? inspector.selectionSet().front()
                                                                : static_cast<const InstanceDrag*>(peek->Data)->id)
                             : core::InstanceId{};
                const bool edgeElsewhere = (above || below) && edgeDragged.valid() && edgeParent.valid() &&
                                           world.parentOf(edgeDragged) != edgeParent;
                if (edgeElsewhere) {
                    const bool lands = inspector.selectionCount() == 1 &&
                                       Editor::canReparent(world, inspector.selectionSet(), edgeParent, root);
                    if (lands && ImGui::AcceptDragDropPayload(kInstanceDragPayload) != nullptr) {
                        core::u32 target = 0;
                        for (core::InstanceId sibling = world.firstChild(edgeParent);
                             sibling.valid() && sibling != row.id; sibling = world.nextSibling(sibling)) {
                            ++target;
                        }
                        commands->reparentTo = edgeParent;
                        // Appended last by the move, then put at the line.
                        commands->reparentIndex = below ? target + 1 : target;
                    }
                    if (lands) {
                        const float y = below ? rowMax.y : rowMin.y;
                        ImGui::GetForegroundDrawList()->AddLine(ImVec2(rowMin.x, y), ImVec2(rowMax.x, y),
                                                                ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
                    }
                }
                else if (above || below) {
                    // **The place it will OCCUPY**, which is what `moveChild`
                    // takes -- reading it as "before whatever stands here now"
                    // lands a downward drag one place short, every time. The
                    // engine-side verb's own comment says so; this is the caller
                    // that would have got it wrong.
                    //
                    // Computed at the DROP rather than carried on every row: the
                    // walk is O(the parent's children) and a drag hovers many
                    // rows on its way to one.
                    if (ImGui::AcceptDragDropPayload(kInstanceDragPayload) != nullptr) {
                        const core::InstanceId dropped = inspector.selectionCount() == 1
                                                             ? inspector.selectionSet().front()
                                                             : static_cast<const InstanceDrag*>(peek->Data)->id;
                        const core::InstanceId parent = world.parentOf(row.id);
                        if (dropped.valid() && parent.valid() && world.parentOf(dropped) == parent &&
                            dropped != row.id) {
                            core::u32 target = 0;
                            core::u32 from = 0;
                            core::u32 seen = 0;
                            for (core::InstanceId sibling = world.firstChild(parent); sibling.valid();
                                 sibling = world.nextSibling(sibling)) {
                                if (sibling == row.id)
                                    target = seen;
                                if (sibling == dropped)
                                    from = seen;
                                ++seen;
                            }
                            // Dropping BELOW a row means the place after it --
                            // unless the dragged row is currently above that
                            // row, in which case removing it first shifts
                            // everything down by one and the place after is the
                            // target's own index.
                            commands->reorderChild = dropped;
                            commands->reorderIndex = dropLanding(from, target, below);
                        }
                    }

                    // The line a person aims at. Drawn on the FOREGROUND list
                    // because a drag is over the whole window and a line on this
                    // window's own list would be under the next row's
                    // background.
                    const float y = below ? rowMax.y : rowMin.y;
                    ImGui::GetForegroundDrawList()->AddLine(ImVec2(rowMin.x, y), ImVec2(rowMax.x, y),
                                                            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
                }
                else if (fromTree && Editor::canReparent(world, inspector.selectionSet(), row.id, root) &&
                         ImGui::AcceptDragDropPayload(kInstanceDragPayload) != nullptr) {
                    commands->reparentTo = row.id;
                }

                // **A prefab dropped from the browser lands HERE**, under the
                // row it was dropped on rather than wherever the selection
                // happens to be -- which is the whole reason a drop is worth
                // having beside the menu item that does the same thing.
                const bool fromBrowser = peek != nullptr && peek->IsDataType(kContentDragPayload);
                // **A material dropped on a part is worn by it** (ADR 0090),
                // and lights only rows that are parts.
                const bool materialOnPart = fromBrowser &&
                                            isMaterialDrag(*static_cast<const ContentDrag*>(peek->Data)) &&
                                            world.parts().find(row.id) != nullptr;
                if (materialOnPart) {
                    if (const ImGuiPayload* took = ImGui::AcceptDragDropPayload(kContentDragPayload); took != nullptr) {
                        commands->assignMaterialPath = static_cast<const ContentDrag*>(took->Data)->path;
                        commands->assignMaterialTarget = row.id;
                    }
                }
                // **Sky pictures dropped on a `Sky`** (ADR 0096): a folder of
                // six, or one, each face read off its name.
                else if (fromBrowser && world.skies().find(row.id) != nullptr &&
                         (isFolderDrag(*static_cast<const ContentDrag*>(peek->Data)) ||
                          isTextureDrag(*static_cast<const ContentDrag*>(peek->Data)))) {
                    if (const ImGuiPayload* took = ImGui::AcceptDragDropPayload(kContentDragPayload); took != nullptr) {
                        commands->assignSkyboxPath = static_cast<const ContentDrag*>(took->Data)->path;
                        commands->assignSkyboxTarget = row.id;
                    }
                }
                else if (fromBrowser && isStampDrag(*static_cast<const ContentDrag*>(peek->Data)) &&
                         Editor::canParentInto(world, row.id, root)) {
                    if (const ImGuiPayload* took = ImGui::AcceptDragDropPayload(kContentDragPayload); took != nullptr) {
                        commands->placeStamp = static_cast<const ContentDrag*>(took->Data)->path;
                        commands->placeStampLinked = true;
                        commands->placeStampParent = row.id;
                    }
                }
                // A mesh becomes a `MeshPart` wearing it, under the row.
                else if (fromBrowser && isMeshDrag(*static_cast<const ContentDrag*>(peek->Data)) &&
                         Editor::canParentInto(world, row.id, root)) {
                    if (const ImGuiPayload* took = ImGui::AcceptDragDropPayload(kContentDragPayload); took != nullptr) {
                        commands->placeMesh = static_cast<const ContentDrag*>(took->Data)->path;
                        commands->placeMeshParent = row.id;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (commands != nullptr && g_authoring &&
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                // The same rule the right-click follows, and for the same
                // reason: a drag that started on a row nobody had selected acts
                // on that row, and one that started on a member of the
                // selection takes all of it. Anything else throws away four
                // things somebody had just picked in order to move one.
                if (!inspector.isSelected(row.id))
                    inspector.select(row.id);

                // **The payload says WHERE FROM, and that is all it has to
                // say.** Within one tree what moves is the selection, read at
                // the drain like the delete and the duplicate; across two it is
                // this one instance, because a selection cannot span worlds.
                const InstanceDrag dragged{row.id};
                ImGui::SetDragDropPayload(kInstanceDragPayload, &dragged, sizeof(dragged));

                const core::usize dragging = inspector.selectionCount();
                if (dragging > 1)
                    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.explorer.dragging"),
                                                    {{"count", static_cast<core::i64>(dragging)}})
                                               .c_str());
                else
                    ImGui::TextUnformatted(label);
                ImGui::EndDragDropSource();
            }

            // **Right-clicking selects first, unless this row is already part
            // of the selection.** A menu that acted on whatever was selected
            // before would delete the wrong thing the first time somebody
            // right-clicked without looking -- and one that replaced the
            // selection would throw away the four things they had just picked
            // in order to act on one of them.
            // **While the game runs the menu says why it is empty**: every
            // item in it changes the scene, which a running game refuses.
            if (commands != nullptr && dialogs != nullptr && !g_authoring) {
                if (ImGui::BeginPopupContextItem("row-menu")) {
                    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.explorer.stop_the_game_to_change")));
                    ImGui::EndPopup();
                }
            }
            else if (commands != nullptr && dialogs != nullptr && ImGui::BeginPopupContextItem("row-menu")) {
                // **The row's spacing is the ROW's, not this menu's.** Style
                // vars are a global stack, so the zero the rows are drawn with
                // reaches every window opened while they are -- and a menu whose
                // items have no space between them is the row fix leaking into
                // something that never asked for it.
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
                if (!inspector.isSelected(row.id))
                    inspector.select(row.id);

                const bool engineOwned = Editor::isEngineOwned(world, row.id, root);

                // **Making something is the first thing on the menu**, as New
                // File is on the one it follows: the plus on the row, for the
                // hand that went to the right button.
                if (iconBeginMenu(icons, icons::ActionAdd, core::tr(ENG_TR("engine.editor.explorer.insert_object")))) {
                    if (const scene::ClassId picked = drawClassPicker(world, inspector, row.id, icons, spacing);
                        picked != scene::InvalidClass) {
                        commands->createClass = picked;
                        commands->createParent = row.id;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndMenu();
                }
                // **A script for each side** (ADR 0138 §4): what the door's
                // server half, its client half and a script for both are
                // made with. Inside a script service, the service decides.
                const auto sideEntry = [&](std::string_view mark, core::TextKey label, core::i32 runContext) {
                    const std::string text = core::engineCatalog().format(label);
                    if (iconMenuItem(icons, mark, text.c_str())) {
                        commands->createClass = world.classes().findId(world.atoms().lookup("Script"));
                        commands->createParent = row.id;
                        commands->createRunContext = runContext;
                    }
                };
                sideEntry(icons::OverlaySideServer, ENG_TR("engine.overlay.explorer.insert_server_script"),
                          script::RunContextServer);
                sideEntry(icons::OverlaySideClient, ENG_TR("engine.overlay.explorer.insert_client_script"),
                          script::RunContextClient);
                sideEntry(icons::OverlaySideShared, ENG_TR("engine.overlay.explorer.insert_shared_script"),
                          script::RunContextShared);
                // **Bringing a file in from the machine, and getting the
                // instance for it.** The file still lands in `content/` --
                // that is where a project's files live and there is nowhere
                // else for them to go -- and what the Explorer adds is the
                // thing in the world that names it. A file the world has no
                // class for is imported and nothing more, which is honest and
                // is what the status line then says.
                if (iconMenuItem(icons, icons::ActionImport, core::tr(ENG_TR("engine.editor.explorer.import")), nullptr,
                                 false, platform::canPickFolder())) {
                    commands->importAssets = true;
                    commands->importParent = row.id;
                }
                ImGui::Separator();

                // **Offered on anything that has children**, rather than on the
                // `Folder` class alone. A `Model` full of parts is a folder in
                // every way that matters to somebody scanning a tree, and a
                // rule that asked the class would colour one and not the other
                // for no reason a person could see.
                if (world.childCount(row.id) > 0 || Editor::folderColor(world, row.id).has_value()) {
                    if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.explorer.colour")))) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
                        std::optional<core::Color3> chosen = Editor::folderColor(world, row.id);
                        if (colorMenu(chosen)) {
                            commands->colorAsked = true;
                            commands->colorTarget = row.id;
                            commands->colorContentPath.clear();
                            commands->color = chosen;
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                }

                if (iconMenuItem(icons, icons::ActionRename, core::tr(ENG_TR("engine.editor.explorer.rename")), "F2",
                                 false, !engineOwned)) {
                    dialogs->renameTarget = row.id;
                    dialogs->renameContentPath.clear();
                    dialogs->renameSeed = std::string(instanceName);
                    dialogs->renameInstance = true;
                }
                // Greyed rather than refused afterwards. The rule lives in
                // `Editor` because it is a rule about the world; the menu
                // reflects it so nobody presses a thing that cannot happen.
                // **On the SELECTION**, which the right-click above has just
                // made sure this row is part of. Labelled with the count when
                // there is more than one, because "Delete" over four things
                // should say four.
                const core::usize count = inspector.selectionCount();
                const core::I18nArg counted[] = {{"count", static_cast<core::i64>(count)}};
                const std::string duplicateLabel =
                    count > 1 ? core::tr(ENG_TR("engine.editor.explorer.duplicate_count"), {counted[0]})
                              : std::string(core::tr(ENG_TR("engine.editor.explorer.duplicate")));
                const std::string deleteLabel =
                    count > 1 ? core::tr(ENG_TR("engine.editor.explorer.delete_count"), {counted[0]})
                              : std::string(core::tr(ENG_TR("engine.editor.explorer.delete")));
                if (iconMenuItem(icons, icons::ActionDuplicate, duplicateLabel.c_str(), "Ctrl+D", false, !engineOwned))
                    commands->duplicateSelection = true;
                if (iconMenuItem(icons, icons::ClassModel, core::tr(ENG_TR("engine.editor.explorer.group")), "Ctrl+G",
                                 false, !engineOwned))
                    commands->groupSelection = true;
                if (iconMenuItem(icons, icons::ClassFolder, core::tr(ENG_TR("engine.editor.explorer.group_as_folder")),
                                 "Ctrl+Alt+G", false, !engineOwned))
                    commands->groupAsFolder = true;
                // Offered only on something that HAS children, because
                // ungrouping a part is not a thing and a greyed row says that
                // better than a refusal after the click does.
                if (iconMenuItem(icons, icons::ActionExpand, core::tr(ENG_TR("engine.editor.explorer.ungroup")),
                                 "Ctrl+Shift+G", false, !engineOwned && world.firstChild(row.id).valid()))
                    commands->ungroupSelection = true;

                // **Copy, cut and the two pastes**, greyed rather than refused
                // afterwards -- which is the rule every other item in this menu
                // follows, and the reason `hasClipboard` is passed in at all.
                ImGui::Separator();
                if (iconMenuItem(icons, icons::ActionCopy, core::tr(ENG_TR("engine.editor.explorer.copy")), "Ctrl+C",
                                 false, !engineOwned))
                    commands->copySelection = true;
                if (iconMenuItem(icons, icons::ActionCut, core::tr(ENG_TR("engine.editor.explorer.cut")), "Ctrl+X",
                                 false, !engineOwned))
                    commands->cutSelection = true;
                // Beside this one, and inside it. That is the whole difference,
                // and it is why they are two items rather than one.
                if (iconMenuItem(icons, icons::ActionPaste, core::tr(ENG_TR("engine.editor.explorer.paste")), "Ctrl+V",
                                 false, hasClipboard))
                    commands->paste = true;
                if (iconMenuItem(icons, icons::ActionPaste, core::tr(ENG_TR("engine.editor.explorer.paste_into")),
                                 "Ctrl+Shift+V", false, hasClipboard))
                    commands->pasteInto = true;

                // --- Stamps (ADR 0049) --------------------------------------
                //
                // On the row rather than in a menu bar, because "a stamp of
                // WHAT" is the whole question and the row is the answer.
                ImGui::Separator();
                const core::InstanceId stampRoot = world.stampRootOf(row.id);
                if (stampRoot.valid()) {
                    const std::string_view stampName = world.atoms().text(world.stampOf(stampRoot));
                    const std::string stampLabel =
                        core::tr(ENG_TR("engine.editor.explorer.break_stamp"), {{"name", stampName}});
                    if (ImGui::MenuItem(stampLabel.c_str()))
                        commands->breakStamp = stampRoot;
                }
                else if (iconMenuItem(icons, icons::ClassModel,
                                      core::tr(ENG_TR("engine.editor.explorer.convert_to_stamp")), nullptr, false,
                                      !engineOwned)) {
                    dialogs->stampSubject = row.id;
                    dialogs->newStamp = true;
                }
                ImGui::Separator();
                if (iconMenuItem(icons, icons::ActionDelete, deleteLabel.c_str(), "Del", false, !engineOwned))
                    commands->deleteSelection = true;
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            // --- What the row shows, placed on it and centred against it ----
            //
            // Every one of these is positioned outright rather than stacked
            // after the last: they are four different heights, and stacking
            // would put them at four different places on a row that has one.

            // Less whatever is above the first drawn row: with the world's
            // root hidden its children are this tree's top level, and with a
            // stamp's root drawn the root itself is.
            float penX = rowOrigin.x + static_cast<float>(row.depth - depthBase) * indentSpacing;

            // The arrow is its own control rather than part of the row, because
            // opening a thing and selecting it are different intentions and a
            // tree that conflates them makes browsing destructive.
            //
            // **The set's own chevrons rather than ImGui's**, and the pairing is
            // what the row can DO rather than what it is: an open row offers to
            // collapse, a closed one offers to expand.
            if (hasChildren) {
                const bool open = g_open.contains(row.id.index);
                // Frameless, so the button IS the picture and centring by the
                // face cannot be wrong -- `buttonFace` exists for the framed
                // case and this one no longer has a frame.
                ImGui::SetCursorPos(ImVec2(penX, centred(iconSize)));
                const bool toggled = haveIcon ? iconButton(icons, open ? icons::ActionCollapse : icons::ActionExpand,
                                                           iconSize, "toggle", open ? "-" : "+", nullptr, true)
                                              : ImGui::ArrowButton("toggle", open ? ImGuiDir_Down : ImGuiDir_Right);
                if (toggled) {
                    if (!g_open.insert(row.id.index).second)
                        g_open.erase(row.id.index);
                }
            }
            // The space is reserved either way, so a leaf's icon lines up with
            // its siblings' rather than sliding left to where their arrow is --
            // and it is reserved at the button's WIDTH, which is the face plus
            // the horizontal padding and is not the row height.
            penX += iconSize + ImGui::GetStyle().ItemInnerSpacing.x;

            // **The plus, at the end of the row's text.** Making a child of the
            // thing you are looking at is the commonest authoring act there is,
            // and until this the only way to add an instance to this engine at
            // all was to write `Instance.new` in a script. It sits on the ROW
            // rather than in the toolbar because which parent is the whole of
            // the question, and after the name rather than before it because
            // before it is where the name goes.
            // **Switched off, it looks switched off** (the owner): an instance
            // whose `Enabled` is false -- a script, a light, an emitter, a
            // screen -- has its icon and its name dimmed, so a tree of forty
            // scripts says which of them will not run without opening one.
            bool switchedOff = false;
            if (const core::NameAtom enabledName = world.atoms().lookup("Enabled"); enabledName.valid()) {
                const std::optional<scene::Value> enabled = world.getProperty(row.id, enabledName);
                const bool* flag = enabled.has_value() ? std::get_if<bool>(&*enabled) : nullptr;
                switchedOff = flag != nullptr && !*flag;
            }
            if (switchedOff)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);

            if (haveIcon) {
                ImGui::SetCursorPos(ImVec2(penX, centred(iconSize)));
                // Screen space, taken before the draw: the badge below is not
                // an ImGui item and has to be placed against the icon's own box
                // rather than against whatever the cursor is after it.
                const ImVec2 iconOrigin = ImGui::GetCursorScreenPos();
                // A `Folder` with something in it is drawn filled, as the
                // content browser's are (the owner).
                std::string rowIcon = classIconFor(icons, world.classes(), world.atoms(), world.classOf(row.id));
                if (rowIcon == icons::ClassFolder && world.firstChild(row.id).valid())
                    rowIcon = std::string(icons::ContentFolderFilled);
                if (drawIcon(icons, rowIcon, iconSize, Editor::folderColor(world, row.id))) {
                    // **The badge goes on the stamp's ROOT and nowhere else.**
                    // Every part of a stamped house is inside a stamped
                    // subtree, and badging all forty of them is noise -- the
                    // root is where the mark lives (ADR 0049) and where "is
                    // this linked?" is a question worth answering.
                    const core::NameAtom stamp = world.stampOf(row.id);
                    // **Where a script outside the services runs** (ADR 0138
                    // §4): inside one, the service already says it.
                    const std::optional<script::ScriptSide> side =
                        world.classOf(row.id) == world.classes().findId(world.atoms().lookup("Script")) &&
                                !script::serviceSideOf(world, row.id).has_value()
                            ? std::optional<script::ScriptSide>(script::scriptSideOf(world, row.id))
                            : std::nullopt;
                    if (side.has_value()) {
                        drawIconBadge(icons, iconOrigin, iconSize,
                                      *side == script::ScriptSide::Server   ? icons::OverlaySideServer
                                      : *side == script::ScriptSide::Client ? icons::OverlaySideClient
                                                                            : icons::OverlaySideShared);
                    }
                    else if (stamp.valid()) {
                        drawIconBadge(icons, iconOrigin, iconSize);
                    }

                    if (ImGui::IsItemHovered()) {
                        if (side.has_value()) {
                            const core::I18nArg args[] = {{"class", className}};
                            const core::TextKey key =
                                *side == script::ScriptSide::Server   ? ENG_TR("engine.overlay.explorer.runs_on_server")
                                : *side == script::ScriptSide::Client ? ENG_TR("engine.overlay.explorer.runs_on_client")
                                                                      : ENG_TR("engine.overlay.explorer.runs_on_both");
                            ImGui::SetTooltip("%s", core::engineCatalog().format(key, args).c_str());
                        }
                        else if (stamp.valid()) {
                            const std::string_view file = world.atoms().text(stamp);
                            ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.explorer.stamped_from"),
                                                             {{"class", className}, {"file", file}})
                                                        .c_str());
                        }
                        else {
                            ImGui::SetTooltip("%.*s", static_cast<int>(className.size()), className.data());
                        }
                    }
                    penX += iconSize + ImGui::GetStyle().ItemInnerSpacing.x;
                }
            }

            ImGui::SetCursorPos(ImVec2(penX, centred(ImGui::GetTextLineHeight())));
            // **An effect that does nothing where it is says so** (ADR 0096):
            // its name is dimmed, and hovering it gives the reason.
            const std::string inactive = lookInactiveReason(world, row.id);
            if (!inactive.empty())
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextUnformatted(label);
            if (!inactive.empty()) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", inactive.c_str());
            }
            if (switchedOff) {
                ImGui::PopStyleVar();
                if (inactive.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.explorer.disabled_enabled_is_off_tip")));
            }
            penX += ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x;

            // Only where an instance can actually go: nothing authored lives
            // inside something streaming materialised, and offering a plus that
            // refuses is worse than not offering one.
            //
            // **Asked of the ANCESTRY**, through the same function the verb
            // uses. Asking `generated` of the row alone got the chunk right and
            // everything inside the chunk wrong -- `Chunk_-3_0_0/Ground` is not
            // itself generated, so it wore a plus, took the create, and lost it
            // at the next eviction.
            // The script services and their folders included: a Script made
            // there becomes a file (ADR 0105), and anything else is made like
            // anywhere else.
            if (commands != nullptr && Editor::canParentInto(world, row.id, root)) {
                // **Shown on the row under the pointer and on the selected
                // ones -- and EXISTING on all of them.** The two are different
                // questions and conflating them is what broke the first click.
                //
                // ImGui resolves an overlap in favour of the LATER item, but
                // only if the later item was there when the overlap happened. A
                // button that comes into being on the same frame the row becomes
                // hovered is a button whose press the selectable underneath has
                // already taken -- so the first click selected the row and only
                // the second opened the menu, which is what somebody trying to
                // add a child to `Workspace` runs into.
                //
                // So the item is emitted every frame and drawn at zero alpha
                // when it is not wanted. Alpha is a rendering property and not
                // a hit-testing one, so the press lands the first time; and the
                // only rows where it is invisible are rows the pointer is not
                // on, which are the rows nobody can click it on anyway. A
                // thousand-row tree shows no plus signs at all.
                ImGui::SetCursorPos(ImVec2(penX, centred(iconSize)));

                // **Under the pointer, and nowhere else.** It was drawn on the
                // selected rows too, which meant four selected rows carried
                // four plus signs while somebody was reading them -- and the
                // one they can press is the one their pointer is already on.
                // The popup keeps it drawn while it is open, or the button
                // vanishes the moment the pointer leaves the row to reach the
                // list and takes its own popup with it.
                const bool lit = rowHovered || addOpen;
                if (!lit)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
                ImGui::BeginDisabled(!g_authoring);
                const bool add =
                    haveIcon ? iconButton(icons, icons::ActionAdd, iconSize, "add", "+",
                                          core::tr(ENG_TR("engine.editor.explorer.add_a_child_instance_tip")), true)
                             : ImGui::SmallButton("+");
                ImGui::EndDisabled();
                if (!lit)
                    ImGui::PopStyleVar();
                if (add)
                    ImGui::OpenPopup("add-child");
            }

            // **The next row starts exactly one row height down**, whatever the
            // last thing drawn on this one happened to be. Without this the
            // pitch is whatever ImGui's line tracking made of four items placed
            // by hand, which is neither `rowHeight` nor what the clipper was
            // told -- and a pitch that disagrees with the clipper is a tree
            // that scrolls to the wrong place.
            ImGui::SetCursorPos(ImVec2(rowOrigin.x, rowOrigin.y + rowHeight));

            if (commands != nullptr && ImGui::BeginPopup("add-child")) {
                g_addOpenRow = row.id.index;
                if (const scene::ClassId picked = drawClassPicker(world, inspector, row.id, icons, spacing);
                    picked != scene::InvalidClass) {
                    commands->createClass = picked;
                    commands->createParent = row.id;
                }
                ImGui::EndPopup();
            }
            else if (g_addOpenRow == row.id.index) {
                g_addOpenRow = 0;
            }

            ImGui::PopID();
        }
    }

    // --- The guide lines --------------------------------------------------
    //
    // A vertical line under every open parent, running down past its children,
    // with a stub reaching each of them. Depth alone tells you a row is nested;
    // it does not tell you WHICH row it is nested under, and on a tree four
    // levels deep with twenty siblings that is the question somebody actually
    // has.
    //
    // Drawn after the rows so the lines sit over the selection highlight rather
    // than under it -- a highlighted row is exactly the row whose parentage is
    // being read. Drawn from `g_visible` rather than from what the clipper
    // showed, because a parent above the fold still owns the children below it.
    //
    // The last child ends the line at its own middle instead of its bottom,
    // which is what turns a bar into an elbow and says "and no more after this".
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 guide = ImGui::GetColorU32(ImGuiCol_Text, 0.28f);
        const float half = rowHeight * 0.5f;
        // The same face the rows' chevrons are drawn at, so the line hangs from
        // the middle of the arrow rather than near it.
        const float chevron = ImGui::GetTextLineHeight();

        for (std::size_t i = 0; i < g_visible.size(); ++i) {
            const core::u32 depth = g_visible[i].depth;

            // The last visible row still under this one. A run rather than a
            // search: `g_visible` is preorder, so the descendants of a row are
            // exactly the rows after it until the depth stops being greater.
            std::size_t last = i;
            for (std::size_t j = i + 1; j < g_visible.size() && g_visible[j].depth > depth; ++j)
                last = j;
            if (last == i)
                continue;

            // The column: the middle of this row's own chevron, which is where
            // a child's line should hang from.
            const float x =
                std::floor(listTop.x + static_cast<float>(depth - depthBase) * indentSpacing + chevron * 0.5f);
            const float top = listTop.y + static_cast<float>(i) * rowHeight + rowHeight;
            const float bottom = listTop.y + static_cast<float>(last) * rowHeight + half;
            draw->AddLine(ImVec2(x, top), ImVec2(x, bottom), guide);

            // A stub to each DIRECT child, at the child's own middle. Only the
            // direct ones: a line to a grandchild would cross the rows between
            // them and say something that is not true.
            for (std::size_t j = i + 1; j <= last; ++j) {
                if (g_visible[j].depth != depth + 1)
                    continue;
                const float y = std::floor(listTop.y + static_cast<float>(j) * rowHeight + half);
                draw->AddLine(ImVec2(x, y), ImVec2(x + indentSpacing * 0.5f, y), guide);
            }
        }
    }

    ImGui::PopStyleVar();
}

// One widget per `ValueType` and no code per class (Decision 16): everything
// this needs arrives in the `PropertyDesc` it is handed.
//
// Nothing here writes. Every branch that accepts an edit enqueues it, and the
// queue drains at the next FrameStart (Decision 15) through
// `World::setProperty` and nothing else (Decision 14).
// A property whose value is SEVERAL numbers that have names of their own.
//
// **These get a row each, and that is the whole fix.** A `UDim` is a scale and
// an offset, a `UDim2` is two of those, and the panel drew them as two or four
// unlabelled boxes in one cell: nothing on the screen said which number was
// which, and there is no convention to fall back on the way `x y z` has one.
// Putting the word inside the box was worse -- a box is for a value, and
// `scale 0.000` is a box with a caption in it.
//
// So the name goes in the name column, where every other name in this panel
// already is, and the box holds a number and nothing else. That is what every
// properties grid does with a composite value, and it costs no width: the rows
// were always there, they were just all crammed into one.
[[nodiscard]] bool compositeKind(EditorKind kind) noexcept
{
    return kind == EditorKind::UDim || kind == EditorKind::UDim2;
}

// --- Arithmetic in a number field --------------------------------------------
//
// **`0+2` is 2 and `1/2` is 0.5** (the owner). ImGui once read operators in a
// typed number and no longer does: it scans the leading number and drops the
// rest, so `0+2` became 0. What it still keeps is the text somebody typed, for
// the frame the field lets go of the keyboard -- and that is read here, worked
// out, and written over what ImGui made of it. Nothing else changes: a drag is
// a drag, and a field that was never typed into is never re-read.

// The field being typed into as text, while it is.
ImGuiID g_typedNumber = 0;

// What was typed into field `id`, worked out -- once, on the frame it lets go
// of the keyboard. Called every frame the field is drawn, which is how it sees
// the typing start.
[[nodiscard]] std::optional<double> typedArithmetic(ImGuiID id)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (id != 0 && g.TempInputId == id && g.ActiveId == id) {
        g_typedNumber = id;
        return std::nullopt;
    }
    if (id == 0 || g_typedNumber != id)
        return std::nullopt;
    g_typedNumber = 0;
    const char* text = nullptr;
    if (g.InputTextDeactivatedState.ID == id && g.InputTextDeactivatedState.TextA.Size > 0)
        text = g.InputTextDeactivatedState.TextA.Data;
    else if (g.InputTextState.ID == id)
        text = g.InputTextState.GetText();
    if (text == nullptr)
        return std::nullopt;
    return evaluateNumberExpression(text);
}

// `DragScalar`, or `DragScalarN` for more than one component, that also takes
// arithmetic typed into any of its fields.
bool dragNumber(const char* label, ImGuiDataType type, void* data, int components, float speed, const char* format,
                const void* minimum = nullptr, const void* maximum = nullptr)
{
    bool changed = components == 1 ? ImGui::DragScalar(label, type, data, speed, minimum, maximum, format)
                                   : ImGui::DragScalarN(label, type, data, components, speed, minimum, maximum, format);
    const ImGuiID single = components == 1 ? ImGui::GetItemID() : 0;
    for (int index = 0; index < components; ++index) {
        ImGuiID id = single;
        if (components > 1) {
            // The id `DragScalarN` gave the component: its label's scope, the
            // index's, and an empty label.
            ImGui::PushID(label);
            ImGui::PushID(index);
            id = ImGui::GetID("");
            ImGui::PopID();
            ImGui::PopID();
        }
        const std::optional<double> typed = typedArithmetic(id);
        if (!typed.has_value())
            continue;
        switch (type) {
        case ImGuiDataType_Float: {
            auto* slot = static_cast<float*>(data) + index;
            const auto next = static_cast<float>(*typed);
            changed = changed || *slot != next;
            *slot = next;
            break;
        }
        case ImGuiDataType_Double: {
            auto* slot = static_cast<double*>(data) + index;
            changed = changed || *slot != *typed;
            *slot = *typed;
            break;
        }
        case ImGuiDataType_S32: {
            auto* slot = static_cast<int*>(data) + index;
            const auto next = static_cast<int>(std::lround(*typed));
            changed = changed || *slot != next;
            *slot = next;
            break;
        }
        default:
            break;
        }
    }
    return changed;
}

// One sub-row: an indented name on the left, one number on the right.
[[nodiscard]] bool numberRow(const char* label, float& value, float step, bool mixed, const char* format)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Indent();
    ImGui::TextUnformatted(label);
    ImGui::Unindent();

    if (ImGui::TableGetColumnCount() == 1)
        ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(ImGui::TableGetColumnCount() == 1 ? 0 : 1);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool changed = dragNumber("##value", ImGuiDataType_Float, &value, 1, step, mixed ? "--" : format);
    ImGui::PopID();
    return changed;
}

// The sub-rows of one composite, and the value they add up to.
//
// The offset is drawn whole because it IS whole: it is a count of pixels, and
// `formatValue` has printed it with no decimals since M6 -- the two agree on
// purpose, so a value read in the summary row and the same value read in its
// own row are the same string.
[[nodiscard]] bool drawCompositeRows(EditorKind kind, const SharedValue& shared, scene::Value& out)
{
    const bool mixed = shared.state == SharedState::Mixed;

    if (kind == EditorKind::UDim) {
        // The same guard `drawEditor` makes one function up: the declared type
        // says which rows exist and the variant says what is in it, and those
        // disagree whenever a value is absent. `std::get` would throw.
        const core::UDim* held = std::get_if<core::UDim>(&shared.value);
        if (held == nullptr)
            return false;
        core::UDim value = *held;
        bool changed =
            numberRow(core::tr(ENG_TR("engine.editor.composite_rows.scale")), value.scale, 0.01f, mixed, "%.3f");
        changed =
            numberRow(core::tr(ENG_TR("engine.editor.composite_rows.offset")), value.offset, 1.0f, mixed, "%.0f") ||
            changed;
        if (changed)
            out = scene::Value{value};
        return changed;
    }

    const core::UDim2* held = std::get_if<core::UDim2>(&shared.value);
    if (held == nullptr)
        return false;
    core::UDim2 value = *held;
    bool changed =
        numberRow(core::tr(ENG_TR("engine.editor.composite_rows.x_scale")), value.x.scale, 0.01f, mixed, "%.3f");
    changed =
        numberRow(core::tr(ENG_TR("engine.editor.composite_rows.x_offset")), value.x.offset, 1.0f, mixed, "%.0f") ||
        changed;
    changed =
        numberRow(core::tr(ENG_TR("engine.editor.composite_rows.y_scale")), value.y.scale, 0.01f, mixed, "%.3f") ||
        changed;
    changed =
        numberRow(core::tr(ENG_TR("engine.editor.composite_rows.y_offset")), value.y.offset, 1.0f, mixed, "%.0f") ||
        changed;
    if (changed)
        out = scene::Value{value};
    return changed;
}

// The kind name the IDL wrote into a descriptor, as the browser's own enum.
// Anything unrecognised is `Other`, which lists nothing rather than everything:
// a picker that fell back to the whole project would be a file dialog with extra
// steps, which is the thing `contentKind` exists to avoid.
[[nodiscard]] ContentKind contentKindNamed(std::string_view name) noexcept
{
    if (name == "Mesh")
        return ContentKind::Mesh;
    if (name == "Texture")
        return ContentKind::Texture;
    if (name == "Audio")
        return ContentKind::Audio;
    if (name == "Font")
        return ContentKind::Font;
    return ContentKind::Other;
}

// Whether `id` is something `descriptor` may point at.
//
// An empty `instanceClass` means the property named no class -- `Weld.Part0` is
// declared `Instance?` on purpose -- and then anything alive will do.
[[nodiscard]] bool acceptsInstance(const scene::World& world, const scene::PropertyDesc& descriptor,
                                   core::InstanceId id)
{
    if (!id.valid() || !world.alive(id) || world.destroyed(id))
        return false;
    if (!descriptor.instanceClass.valid())
        return true;
    const scene::ClassId want = world.classes().findId(descriptor.instanceClass);
    return want != scene::InvalidClass && world.classes().isA(world.classOf(id), want);
}

// The same question about a stamp, answered from the class its FILE is rooted
// at -- which the browser put in the drag payload precisely so a target could
// decide before it lights up.
[[nodiscard]] bool acceptsStampClass(scene::World& world, const scene::PropertyDesc& descriptor,
                                     std::string_view rootClass)
{
    if (rootClass.empty())
        return false;
    if (!descriptor.instanceClass.valid())
        return true;
    const scene::ClassId dropped = world.classes().findId(world.atoms().intern(rootClass));
    const scene::ClassId want = world.classes().findId(descriptor.instanceClass);
    return dropped != scene::InvalidClass && want != scene::InvalidClass && world.classes().isA(dropped, want);
}

// What the reference picker is offering. Held here rather than rebuilt per
// frame for the reason the add menu class list is: the popup stays open for as
// long as somebody is reading it, and filling it is a walk of the whole tree.
std::vector<core::InstanceId> g_refCandidates;
// And the stamps of that class the PROJECT holds, which is the other half of
// the same question: a material somebody made is a file until something points
// at it, and a picker offering only what is already in the world would make
// importing one and using it two unrelated gestures.
std::vector<ContentEntry> g_refStamps;
std::array<char, 64> g_refFilter{};

// **A reference is a value somebody sets, not a fact they read.**
//
// This drew the name and a `go` button and returned, so every instance-valued
// property in the engine was read-only in the property grid -- `BasePart.Material`
// among them, which is a property whose entire purpose is to be set. Nothing
// could have offered a list before `PropertyDesc::instanceClass` existed:
// without it the only honest options were every instance in the world, or none.
//
// Three ways in, because they are three situations and each is the obvious one
// from where somebody is standing:
//
//   * the picker, for "it is in this scene somewhere";
//   * a drag from the Explorer, for "it is that one, right there";
//   * a drag from the content browser, for "it is that FILE" -- which places the
//     instance when the world has none and reuses the one it has when it does.
//
// Drawn before the panel disabled block and before the absent-value guard: a nil
// reference is the state this is most often used FROM, and a guard returning
// early on `monostate` is exactly why an unset `Material` showed the word "nil"
// and nothing to click.
void drawInstanceRef(scene::World& world, core::InstanceId root, Inspector& inspector,
                     std::span<const core::InstanceId> targets, const scene::PropertyDesc& descriptor,
                     const SharedValue& shared, bool mixed, ContentTree* tree, const IconAtlas* icons,
                     EditorCommands* commands)
{
    core::InstanceId reference;
    if (!mixed && std::holds_alternative<core::InstanceId>(shared.value))
        reference = std::get<core::InstanceId>(shared.value);
    const bool live = reference.valid() && world.alive(reference);

    const std::string text = mixed  ? std::string(core::tr(ENG_TR("engine.editor.instance_ref.mixed")))
                             : live ? std::string(world.atoms().text(world.name(reference)))
                                    : std::string(core::tr(ENG_TR("engine.editor.instance_ref.none")));

    const ImGuiStyle& style = ImGui::GetStyle();
    const bool locked = !editable(descriptor);
    // Room for `go` when there is somewhere to go and none when there is not: a
    // field reserving it always would be a field with a permanent gap.
    const float goWidth = live ? ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.instance_ref.go"))).x +
                                     style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x
                               : 0.0f;

    ImGui::BeginDisabled(locked);
    if (ImGui::Button(text.c_str(), ImVec2(-(goWidth + 1.0f), 0.0f)))
        ImGui::OpenPopup("pick-instance");
    ImGui::EndDisabled();

    // A disabled item is not hovered and therefore is not a drop target at all,
    // which is the behaviour wanted: a read-only field that lit up for a drag
    // would promise a write it cannot make.
    if (!locked && ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* peek = ImGui::GetDragDropPayload();
        if (peek != nullptr && peek->IsDataType(kInstanceDragPayload)) {
            const core::InstanceId dragged = static_cast<const InstanceDrag*>(peek->Data)->id;
            // Asked BEFORE accepting, so the field highlights only where the
            // drop would do something. The Explorer row uses the same rule and
            // it is the same promise: a target that lights and then refuses is
            // worse than one that never lit.
            if (acceptsInstance(world, descriptor, dragged) &&
                ImGui::AcceptDragDropPayload(kInstanceDragPayload) != nullptr) {
                for (const core::InstanceId target : targets) {
                    if (world.alive(target))
                        inspector.enqueue(target, descriptor.name, scene::Value{dragged});
                }
            }
        }
        if (commands != nullptr && peek != nullptr && peek->IsDataType(kContentDragPayload)) {
            const auto* drag = static_cast<const ContentDrag*>(peek->Data);
            if (acceptsStampClass(world, descriptor, drag->rootClass) &&
                ImGui::AcceptDragDropPayload(kContentDragPayload) != nullptr) {
                commands->assignStampPath = drag->path;
                commands->assignStampProperty = std::string(world.atoms().text(descriptor.name));
            }
        }
        ImGui::EndDragDropTarget();
    }

    // No `go` on a mixed reference: there is no one instance to go to, and
    // jumping to whichever member happened to be first would replace the
    // selection with something nobody pointed at.
    if (live) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.instance_ref.go"))))
            inspector.select(reference);
    }

    if (!ImGui::BeginPopup("pick-instance"))
        return;

    // Collected on the frame the popup opens and read from then on. Filling it
    // walks the whole tree, and doing that every frame a list is being read is
    // thirty thousand instances paid to draw twenty rows -- the cost ADR 0054
    // took out of the Explorer.
    if (ImGui::IsWindowAppearing()) {
        g_refFilter.fill(0);
        g_refCandidates.clear();
        static thread_local std::vector<TreeRow> rows;
        collectTree(world, root, rows);
        for (const TreeRow& row : rows) {
            if (acceptsInstance(world, descriptor, row.id))
                g_refCandidates.push_back(row.id);
        }

        // **Only for a reference that named a class.** Choosing a file here
        // PLACES an instance, which is a large side effect to offer for a
        // property whose answer is "any instance at all" -- `Weld.Part0` takes a
        // part or an attachment and the IDL has no way to say so, so its list
        // would be every stamp in the project and not one of them a good guess.
        g_refStamps.clear();
        if (tree != nullptr && descriptor.instanceClass.valid()) {
            for (ContentEntry& entry : tree->collectStamps()) {
                if (acceptsStampClass(world, descriptor, entry.rootClass))
                    g_refStamps.push_back(std::move(entry));
            }
        }
        ImGui::SetKeyboardFocusHere();
    }

    ImGui::SetNextItemWidth(240.0f * ImGui::GetStyle().FontScaleMain);
    (void)ImGui::InputTextWithHint("##filter", core::tr(ENG_TR("engine.editor.instance_ref.search")),
                                   g_refFilter.data(), g_refFilter.size());
    const std::string_view needle(g_refFilter.data());

    // **Clearing is always on the list, and first.** Every reference in the IDL
    // is optional, and a picker with no way back to nothing would make setting
    // one a decision nobody could take back except by undoing.
    if (ImGui::Selectable(core::tr(ENG_TR("engine.editor.instance_ref.none")))) {
        for (const core::InstanceId target : targets) {
            if (world.alive(target))
                inspector.enqueue(target, descriptor.name, scene::Value{core::InstanceId{}});
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();

    if (g_refCandidates.empty() && g_refStamps.empty()) {
        // A real answer rather than an empty box: a project with no `Material`
        // anywhere has nothing to point a `Material` at, and saying which class
        // is missing is what tells somebody to go and make one.
        const std::string_view wanted = descriptor.instanceClass.valid() ? world.atoms().text(descriptor.instanceClass)
                                                                         : std::string_view("Instance");
        ImGui::TextDisabled(
            "%s", core::tr(ENG_TR("engine.editor.instance_ref.none_in_project"), {{"class", wanted}}).c_str());
        ImGui::EndPopup();
        return;
    }

    ImGui::BeginChild("candidates", ImVec2(240.0f, 200.0f));
    for (const core::InstanceId candidate : g_refCandidates) {
        if (!world.alive(candidate) || world.destroyed(candidate))
            continue;
        const std::string_view name = world.atoms().text(world.name(candidate));
        if (!needle.empty() && !containsFold(name, needle))
            continue;

        ImGui::PushID(static_cast<int>(candidate.index));
        const ImVec2 origin = ImGui::GetCursorPos();
        const bool chosen = ImGui::Selectable("##candidate", candidate == reference);

        const float rowIcon = ImGui::GetTextLineHeight();
        ImGui::SetCursorPos(origin);
        if (drawIcon(icons, classIconFor(icons, world.classes(), world.atoms(), world.classOf(candidate)), rowIcon))
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        else
            ImGui::SetCursorPos(origin);
        ImGui::TextUnformatted(name.data(), name.data() + name.size());

        if (chosen) {
            for (const core::InstanceId target : targets) {
                if (world.alive(target))
                    inspector.enqueue(target, descriptor.name, scene::Value{candidate});
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
    }

    // **The files, under the instances, and said to be files.** They are a
    // different thing to choose -- picking one puts an instance in the world and
    // points at THAT -- so they are separated rather than mixed into one list
    // that would quietly do two different things depending on the row.
    if (!g_refStamps.empty() && commands != nullptr) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.instance_ref.from_the_project")));
        for (const ContentEntry& entry : g_refStamps) {
            const std::string shown = ContentTree::displayNameOf(entry);
            if (!needle.empty() && !containsFold(shown, needle))
                continue;

            ImGui::PushID(entry.path.c_str());
            const ImVec2 origin = ImGui::GetCursorPos();
            const bool chosen = ImGui::Selectable("##stamp");

            const float rowIcon = ImGui::GetTextLineHeight();
            ImGui::SetCursorPos(origin);
            const ImVec2 iconOrigin = ImGui::GetCursorScreenPos();
            if (drawIcon(icons, classIconFor(icons, &world.classes(), &world.atoms(), entry.rootClass), rowIcon)) {
                // The same badge the browser and the Explorer wear, for the same
                // one idea: this row is a file one comes from.
                drawIconBadge(icons, iconOrigin, rowIcon);
                ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            }
            else {
                ImGui::SetCursorPos(origin);
            }
            ImGui::TextUnformatted(shown.c_str());

            if (chosen) {
                // The same command the drop issues, so the two ways in cannot
                // drift: place one if the world has none, reuse the one it has.
                commands->assignStampPath = entry.path;
                commands->assignStampProperty = std::string(world.atoms().text(descriptor.name));
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

// --- A part's material (ADR 0090) ----------------------------------------------

// **What a part wears**: a material file, picked from the ones the project has
// or dragged onto the field from the browser, and `open` to edit it. Nothing is
// placed in the world -- a material is worn by name, so there is no instance
// for a boundary to lose (D133, D142).
void drawMaterialField(scene::World& world, Inspector& inspector, std::span<const core::InstanceId> targets,
                       const scene::PropertyDesc& descriptor, const SharedValue& shared, bool mixed, ContentTree* tree,
                       EditorCommands* commands)
{
    std::string source;
    core::u32 clone = 0;
    if (!mixed) {
        if (const auto* worn = std::get_if<scene::MaterialRef>(&shared.value); worn != nullptr) {
            source = worn->source;
            clone = worn->clone;
        }
    }
    const std::string_view scheme = asset::AssetScheme;
    const std::string relative = source.compare(0, scheme.size(), scheme) == 0 ? source.substr(scheme.size()) : source;
    std::string shown = mixed ? std::string(core::tr(ENG_TR("engine.editor.material_field.mixed")))
                        : relative.empty()
                            ? std::string(core::tr(ENG_TR("engine.editor.material_field.default_material")))
                            : relative;
    if (const std::size_t slash = shown.rfind('/'); slash != std::string::npos && !mixed && !relative.empty())
        shown = shown.substr(slash + 1);
    if (asset::isMaterialPath(shown))
        shown.resize(shown.size() - asset::MaterialSuffix.size());
    // A clone is the same asset changed at runtime, and saying so is what stops
    // somebody opening the file to find the colour the part is drawn in.
    if (clone != 0)
        shown += core::tr(ENG_TR("engine.editor.material_field.runtime_copy"));

    const ImGuiStyle& style = ImGui::GetStyle();
    const bool locked = !editable(descriptor) || commands == nullptr;
    const bool openable = !mixed && !relative.empty() && commands != nullptr;
    const float openWidth = openable ? ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.material_field.open"))).x +
                                           style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x
                                     : 0.0f;

    ImGui::BeginDisabled(locked);
    if (ImGui::Button(shown.c_str(), ImVec2(-(openWidth + 1.0f), 0.0f)))
        ImGui::OpenPopup("pick-material");
    ImGui::EndDisabled();
    if (!relative.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", source.c_str());

    // The field takes a material row from the browser, and lights only for
    // one: a field that lit for a texture and then refused it is the broken
    // promise every drop target here avoids.
    if (!locked && ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* peek = ImGui::GetDragDropPayload();
        if (peek != nullptr && peek->IsDataType(kContentDragPayload) &&
            isMaterialDrag(*static_cast<const ContentDrag*>(peek->Data))) {
            if (const ImGuiPayload* took = ImGui::AcceptDragDropPayload(kContentDragPayload); took != nullptr)
                commands->assignMaterialPath = static_cast<const ContentDrag*>(took->Data)->path;
        }
        ImGui::EndDragDropTarget();
    }

    if (openable) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.material_field.open"))))
            commands->openMaterial = relative;
    }

    if (!ImGui::BeginPopup("pick-material"))
        return;
    static std::vector<std::string> candidates;
    static std::array<char, 96> search{};
    if (ImGui::IsWindowAppearing()) {
        candidates = tree != nullptr ? tree->filesOfKind(ContentKind::Material) : std::vector<std::string>{};
        search.fill(0);
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    ImGui::InputTextWithHint("##material-search", core::tr(ENG_TR("engine.editor.material_field.search")),
                             search.data(), search.size());
    const std::string_view needle{search.data()};
    ImGui::Separator();
    if (ImGui::BeginChild("material-list", ImVec2(ImGui::GetFontSize() * 16.0f, ImGui::GetFontSize() * 12.0f))) {
        // Wearing nothing is a real answer and the first one offered: it is
        // the engine default, which is what a plain part is.
        if (ImGui::Selectable(core::tr(ENG_TR("engine.editor.material_field.default")))) {
            for (const core::InstanceId target : targets) {
                if (world.alive(target))
                    inspector.enqueue(target, descriptor.name, scene::Value{});
            }
            ImGui::CloseCurrentPopup();
        }
        std::size_t listed = 0;
        for (const std::string& candidate : candidates) {
            if (!needle.empty() && !containsFold(candidate, needle))
                continue;
            ++listed;
            if (ImGui::Selectable(candidate.c_str())) {
                commands->assignMaterialPath = candidate;
                ImGui::CloseCurrentPopup();
            }
        }
        if (candidates.empty())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.material_field.no_materials_under_content_yet")));
        else if (listed == 0)
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.material_field.nothing_matches"),
                                               {{"search", std::string_view(search.data())}})
                                          .c_str());
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

// One parameter's widget: a colour for `Color` and `Emissive`, a drag for the
// rest. True when it changed `values`.
[[nodiscard]] bool drawParameterWidget(asset::MaterialField field, asset::MaterialProperties& values)
{
    switch (field) {
    case asset::MaterialField::Color:
        return ImGui::ColorEdit3("##value", &values.color.r, ImGuiColorEditFlags_Float);
    case asset::MaterialField::Emissive:
        return ImGui::ColorEdit3("##value", &values.emissive.r, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    case asset::MaterialField::Transparency:
        return ImGui::SliderFloat("##value", &values.transparency, 0.0f, 1.0f, "%.3f");
    case asset::MaterialField::Metalness:
        return ImGui::SliderFloat("##value", &values.metalness, 0.0f, 1.0f, "%.3f");
    case asset::MaterialField::Roughness:
        return ImGui::SliderFloat("##value", &values.roughness, 0.0f, 1.0f, "%.3f");
    case asset::MaterialField::NormalScale:
        return ImGui::DragFloat("##value", &values.normalScale, 0.01f, 0.0f, 4.0f, "%.3f");
    case asset::MaterialField::AlphaCutoff:
        return ImGui::SliderFloat("##value", &values.alphaCutoff, 0.0f, 1.0f, "%.3f");
    default:
        return false;
    }
}

// Draws `text` struck through: an override the part keeps and its material
// ignores (ADR 0090). Kept rather than dropped, so switching back to a material
// that declares it restores the look -- and shown, so nobody wonders where it
// went.
void strikeText(const char* text)
{
    ImGui::TextDisabled("%s", text);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float y = (min.y + max.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), ImGui::GetColorU32(ImGuiCol_TextDisabled));
}

// Defined with the material panel below; the Properties panel draws a part's
// own surface shader parameters with the same widgets (ADR 0091).
[[nodiscard]] const asset::SurfaceReflection* surfaceReflectionOf(const ContentTree& tree, std::string_view urn);
[[nodiscard]] bool drawSurfaceParam(const asset::SurfaceParam& param, std::array<core::f32, 4>& value);
[[nodiscard]] core::u8 surfaceComponents(asset::SurfaceParamType type) noexcept;

// **What a part may change about its material**: the parameters the material
// declares, each with its value and a revert when this part overrides it, and
// below them any override the part keeps that this material does not declare.
void drawMaterialParameters(scene::World& world, Inspector& inspector, std::span<const core::InstanceId> targets,
                            const scene::PropertyDesc& descriptor, bool mixed, const IconAtlas* icons,
                            const ContentTree* tree)
{
    if (mixed || targets.size() != 1) {
        ImGui::TextDisabled("%s", core::tr(mixed ? ENG_TR("engine.editor.material_parameters.mixed")
                                                 : ENG_TR("engine.editor.material_parameters.select_one_part")));
        return;
    }
    const scene::PartComponent* part = world.parts().find(targets.front());
    if (part == nullptr) {
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.material_parameters.none")));
        return;
    }
    const asset::ResolvedMaterial material = world.resolveMaterial(part->material, part->materialClone);
    const asset::MaterialOverrides overrides = part->materialParameters;
    asset::MaterialProperties shown = material.properties;
    asset::applyOverrides(overrides, material.instanceParameters, shown);

    const auto write = [&](const asset::MaterialOverrides& next) {
        inspector.enqueue(targets.front(), descriptor.name, scene::Value{next});
    };

    // Name order, which is the order a scene file writes them in.
    std::array<asset::MaterialField, 7> fields{asset::MaterialField::AlphaCutoff, asset::MaterialField::Color,
                                               asset::MaterialField::Emissive,    asset::MaterialField::Metalness,
                                               asset::MaterialField::NormalScale, asset::MaterialField::Roughness,
                                               asset::MaterialField::Transparency};
    const bool locked = !editable(descriptor);
    ImGui::BeginDisabled(locked);
    bool any = false;
    for (const asset::MaterialField field : fields) {
        const bool declared = (material.instanceParameters & asset::fieldBit(field)) != 0;
        const bool overridden = overrides.has(field);
        if (!declared && !overridden)
            continue;
        any = true;
        ImGui::PushID(static_cast<int>(field));
        const std::string name(asset::materialFieldName(field));
        if (!declared) {
            strikeText(name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s", core::tr(ENG_TR("engine.editor.material_parameters.kept_ignored"), {{"name", name}}).c_str());
        }
        else {
            ImGui::TextUnformatted(name.c_str());
            if (overridden) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "*");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-(overridden ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 1.0f));
            asset::MaterialProperties values = shown;
            if (drawParameterWidget(field, values)) {
                asset::MaterialOverrides next = overrides;
                (void)asset::setOverride(next, field, values);
                write(next);
            }
        }
        if (overridden) {
            ImGui::SameLine();
            if (iconButton(icons, icons::ActionRevert, ImGui::GetFontSize(), "revert",
                           core::tr(ENG_TR("engine.editor.material_parameters.revert")),
                           core::tr(ENG_TR("engine.editor.material_parameters.revert_to_material_value_tip")))) {
                asset::MaterialOverrides next = overrides;
                asset::clearOverride(next, field);
                write(next);
            }
        }
        ImGui::PopID();
    }

    // **The surface shader's parameters this material lets a part change**
    // (ADR 0091), drawn as the material panel draws them -- by what the
    // shader's source declares -- and written through the same queue, so undo
    // and the safe point see them as they see a property.
    const core::InstanceId id = targets.front();
    const std::vector<asset::ShaderParameter>* own = world.partShaderParameters(id);
    const asset::SurfaceReflection* reflection = tree != nullptr && !material.properties.shader.empty()
                                                     ? surfaceReflectionOf(*tree, material.properties.shader)
                                                     : nullptr;
    const auto mineOf = [&](std::string_view name) -> const asset::ShaderParameter* {
        if (own == nullptr)
            return nullptr;
        for (const asset::ShaderParameter& parameter : *own) {
            if (parameter.name == name)
                return &parameter;
        }
        return nullptr;
    };
    for (const std::string& name : material.instanceShaderParameters) {
        any = true;
        ImGui::PushID(name.c_str());
        const asset::ShaderParameter* mine = mineOf(name);
        const asset::ShaderParameter* base = material.properties.shaderParameter(name);
        const asset::SurfaceParam* param = reflection != nullptr ? reflection->param(name) : nullptr;
        ImGui::TextUnformatted(name.c_str());
        if (mine != nullptr) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "*");
        }
        ImGui::SameLine();
        if (param == nullptr) {
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.material_parameters.not_a_parameter_of_its")));
        }
        else {
            ImGui::SetNextItemWidth(
                -(mine != nullptr ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 1.0f));
            std::array<core::f32, 4> value = mine != nullptr   ? mine->value
                                             : base != nullptr ? base->value
                                                               : param->value;
            if (drawSurfaceParam(*param, value)) {
                asset::ShaderParameter written;
                written.name = name;
                written.value = value;
                written.components = surfaceComponents(param->type);
                inspector.enqueueShaderParameter(id, world.atoms().intern(name), std::move(written));
            }
        }
        if (mine != nullptr) {
            ImGui::SameLine();
            if (iconButton(icons, icons::ActionRevert, ImGui::GetFontSize(), "revert",
                           core::tr(ENG_TR("engine.editor.material_parameters.revert")),
                           core::tr(ENG_TR("engine.editor.material_parameters.revert_to_material_value_tip"))))
                inspector.enqueueShaderParameterClear(id, world.atoms().intern(name));
        }
        ImGui::PopID();
    }
    // Kept on the part and ignored, as a built-in override is.
    if (own != nullptr) {
        for (const asset::ShaderParameter& parameter : *own) {
            if (material.declaresShaderParameter(parameter.name))
                continue;
            any = true;
            ImGui::PushID(parameter.name.c_str());
            strikeText(parameter.name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s", core::tr(ENG_TR("engine.editor.material_parameters.kept_ignored"), {{"name", parameter.name}})
                              .c_str());
            ImGui::SameLine();
            if (iconButton(icons, icons::ActionRevert, ImGui::GetFontSize(), "revert",
                           core::tr(ENG_TR("engine.editor.material_parameters.revert")),
                           core::tr(ENG_TR("engine.editor.material_parameters.remove_it_tip"))))
                inspector.enqueueShaderParameterClear(id, world.atoms().intern(parameter.name));
            ImGui::PopID();
        }
    }
    if (!any)
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.material_parameters.this_material_lets_a_part")));
    ImGui::EndDisabled();
}

// --- The material panel (ADR 0090) ----------------------------------------------

// Puts the cursor at the right end of the current table cell, for one small
// button there -- a row's inherit or reset, beside its name.
void moveToCellEnd()
{
    const ImVec2 cell = ImGui::GetCursorScreenPos();
    const float button = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(cell.x + std::max(ImGui::GetContentRegionAvail().x - button, 0.0f), cell.y));
}

// One field of the open material. A variant shows its parent's value for what it
// does not write, and editing one makes it the variant's own; `inherit` takes it
// back. A base writes every field and has nothing to inherit.
// **A material field's name as the editor shows it** (R3): through the
// catalog, in `MaterialField` order. In English it is the name the file and a
// script spell it by.
[[nodiscard]] const char* materialFieldLabel(asset::MaterialField field) noexcept
{
    static constexpr std::array<core::TextKey, asset::MaterialFieldCount> Labels{
        ENG_TR("engine.editor.material.field.color"),
        ENG_TR("engine.editor.material.field.transparency"),
        ENG_TR("engine.editor.material.field.color_map"),
        ENG_TR("engine.editor.material.field.normal_map"),
        ENG_TR("engine.editor.material.field.metallic_roughness_map"),
        ENG_TR("engine.editor.material.field.emissive"),
        ENG_TR("engine.editor.material.field.emissive_map"),
        ENG_TR("engine.editor.material.field.metalness"),
        ENG_TR("engine.editor.material.field.roughness"),
        ENG_TR("engine.editor.material.field.normal_scale"),
        ENG_TR("engine.editor.material.field.alpha_mode"),
        ENG_TR("engine.editor.material.field.alpha_cutoff"),
        ENG_TR("engine.editor.material.field.double_sided"),
        ENG_TR("engine.editor.material.field.tile_size"),
        ENG_TR("engine.editor.material.field.height_map"),
        ENG_TR("engine.editor.material.field.triplanar"),
        ENG_TR("engine.editor.material.field.blend_sharpness"),
        ENG_TR("engine.editor.material.field.tiling_variation"),
        ENG_TR("engine.editor.material.field.tiling_far_scale"),
        ENG_TR("engine.editor.material.field.hex_tiling"),
    };
    return core::tr(Labels[static_cast<std::size_t>(field)]);
}

struct MaterialFieldRow
{
    asset::MaterialAsset& next;
    const asset::MaterialProperties& inherited;
    bool variant = false;
    bool changed = false;
    bool continuing = false;
    const IconAtlas* icons = nullptr;

    // Starts the field's row of the grid: its name on the left -- dimmed when a
    // variant inherits it, with an inherit button when the variant has its own
    // -- and returns the values the widget on the right should edit.
    asset::MaterialProperties& begin(asset::MaterialField field)
    {
        ImGui::PushID(static_cast<int>(field));
        const bool own = !variant || (next.written & asset::fieldBit(field)) != 0;
        if (!own)
            asset::copyMaterialField(field, inherited, next.properties);
        sectionName(materialFieldLabel(field), variant && !own);
        if (variant && !own)
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.material.inherited_tip")));
        if (variant && own) {
            ImGui::TableSetColumnIndex(0);
            moveToCellEnd();
            if (iconButton(icons, icons::ActionInherit, ImGui::GetFontSize(), "inherit",
                           core::tr(ENG_TR("engine.editor.material_field_label.inherit")),
                           core::tr(ENG_TR("engine.editor.material_field_label.back_to_the_parent_material_tip")))) {
                next.written = static_cast<asset::MaterialFieldMask>(next.written & ~asset::fieldBit(field));
                asset::copyMaterialField(field, inherited, next.properties);
                changed = true;
            }
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        return next.properties;
    }

    void end(asset::MaterialField field, bool edited)
    {
        if (edited) {
            next.written |= asset::fieldBit(field);
            changed = true;
            // A drag that is still held is the same edit as last frame's: one
            // step to undo for one gesture.
            continuing = continuing || (ImGui::IsItemActive() && !ImGui::IsItemActivated());
        }
        ImGui::PopID();
    }
};

// A texture slot: the picture it holds, the texture's URN, a picker of the
// project's textures, a clear, and a drop target for a texture from the browser
// over all of it (the owner: dragging is how a texture gets somewhere).
[[nodiscard]] bool drawMapField(std::string& urn, ContentTree& tree)
{
    char buffer[256]{};
    if (urn.size() + 1 <= sizeof(buffer))
        std::snprintf(buffer, sizeof(buffer), "%s", urn.c_str());
    bool changed = false;
    const float pick = ImGui::GetFrameHeight();
    const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::BeginGroup();
    // The texture itself, a frame high, so a slot says what it holds at a
    // glance; an empty square when it holds nothing.
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 end(at.x + pick, at.y + pick);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
        if (g_thumbnails != nullptr && g_device != nullptr && urn.starts_with(asset::AssetScheme)) {
            const ThumbnailCache::Thumbnail picture =
                g_thumbnails->request(tree.root() / std::filesystem::path(urn.substr(asset::AssetScheme.size())));
            if (SDL_GPUTexture* native = picture.valid() ? rhi::nativeTexture(*g_device, picture.texture) : nullptr;
                native != nullptr) {
                draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)), ImVec2(at.x + 1, at.y + 1),
                               ImVec2(end.x - 1, end.y - 1));
            }
        }
        ImGui::Dummy(ImVec2(pick, pick));
        if (!urn.empty())
            ImGui::SetItemTooltip("%s", urn.c_str());
        ImGui::SameLine(0.0f, inner);
    }
    const float clear = urn.empty() ? 0.0f : pick + inner;
    ImGui::SetNextItemWidth(-(pick + inner + clear));
    if (ImGui::InputTextWithHint("##map", core::tr(ENG_TR("engine.editor.map_field.none_drop_a_texture_here")), buffer,
                                 sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
        urn = buffer;
        changed = true;
    }
    ImGui::SameLine(0.0f, inner);
    if (ImGui::Button("...", ImVec2(pick, 0.0f)))
        ImGui::OpenPopup("map-pick");
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.map_field.the_project_s_textures_tip")));
    if (!urn.empty()) {
        ImGui::SameLine(0.0f, inner);
        if (ImGui::Button("x", ImVec2(pick, 0.0f))) {
            urn.clear();
            changed = true;
        }
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.map_field.no_texture_tip")));
    }
    ImGui::EndGroup();
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* peek = ImGui::GetDragDropPayload();
        if (peek != nullptr && peek->IsDataType(kContentDragPayload) &&
            contentKindOf(
                std::filesystem::path(static_cast<const ContentDrag*>(peek->Data)->path).filename().string()) ==
                ContentKind::Texture) {
            if (const ImGuiPayload* took = ImGui::AcceptDragDropPayload(kContentDragPayload); took != nullptr) {
                urn = std::string(asset::AssetScheme) + static_cast<const ContentDrag*>(took->Data)->path;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("map-pick")) {
        if (ImGui::Selectable(core::tr(ENG_TR("engine.editor.map_field.none")))) {
            urn.clear();
            changed = true;
        }
        for (const std::string& texture : tree.filesOfKind(ContentKind::Texture)) {
            if (ImGui::Selectable(texture.c_str())) {
                urn = std::string(asset::AssetScheme) + texture;
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    return changed;
}

// **What a surface shader declares**, read from its file and kept until the
// file changes: the panel asks every frame, and parsing the source every frame
// to answer the same question would be the only cost this panel has.
[[nodiscard]] const asset::SurfaceReflection* surfaceReflectionOf(const ContentTree& tree, std::string_view urn)
{
    struct Cached
    {
        std::filesystem::file_time_type time{};
        asset::SurfaceReflection reflection;
        bool readable = false;
        bool read = false;
    };
    // By URN: the material panel and the Properties panel ask about different
    // shaders in the same frame.
    static std::map<std::string, Cached, std::less<>> cache;
    if (!urn.starts_with(asset::AssetScheme))
        return nullptr;
    const std::filesystem::path file = tree.root() / std::filesystem::path(urn.substr(asset::AssetScheme.size()));
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, error);
    if (error)
        return nullptr;
    auto found = cache.find(urn);
    if (found == cache.end())
        found = cache.emplace(std::string(urn), Cached{}).first;
    Cached& cached = found->second;
    if (!cached.read || cached.time != time) {
        cached.read = true;
        cached.time = time;
        std::string text;
        cached.readable = platform::readTextFile(file, text);
        cached.reflection = cached.readable ? asset::reflectSurface(text) : asset::SurfaceReflection{};
    }
    return cached.readable ? &cached.reflection : nullptr;
}

// One reflected parameter as a field of the kind its declaration asks for: a
// slider for a `range`, a colour for a `colour`, a checkbox for a `toggle`.
[[nodiscard]] bool drawSurfaceParam(const asset::SurfaceParam& param, std::array<core::f32, 4>& value)
{
    using T = asset::SurfaceParamType;
    using A = asset::SurfaceAnnotation;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (param.type == T::Bool || param.annotation == A::Toggle) {
        bool on = value[0] != 0.0f;
        if (!ImGui::Checkbox("##value", &on))
            return false;
        value[0] = on ? 1.0f : 0.0f;
        return true;
    }
    if (param.type == T::Int) {
        int number = static_cast<int>(value[0]);
        const bool edited =
            param.annotation == A::Range
                ? ImGui::SliderInt("##value", &number, static_cast<int>(param.minimum), static_cast<int>(param.maximum))
                : ImGui::DragInt("##value", &number);
        value[0] = static_cast<core::f32>(number);
        return edited;
    }
    if (param.annotation == A::Colour && param.type == T::Float3)
        return ImGui::ColorEdit3("##value", value.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    if (param.annotation == A::Colour && param.type == T::Float4)
        return ImGui::ColorEdit4("##value", value.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    const int components = param.type == T::Float2 ? 2 : param.type == T::Float3 ? 3 : param.type == T::Float4 ? 4 : 1;
    if (param.annotation == A::Range)
        return ImGui::SliderScalarN("##value", ImGuiDataType_Float, value.data(), components, &param.minimum,
                                    &param.maximum, "%.3f");
    return ImGui::DragScalarN("##value", ImGuiDataType_Float, value.data(), components, 0.01f, nullptr, nullptr,
                              "%.3f");
}

[[nodiscard]] core::u8 surfaceComponents(asset::SurfaceParamType type) noexcept
{
    switch (type) {
    case asset::SurfaceParamType::Float2:
        return 2;
    case asset::SurfaceParamType::Float3:
        return 3;
    case asset::SurfaceParamType::Float4:
        return 4;
    case asset::SurfaceParamType::Float:
    case asset::SurfaceParamType::Int:
    case asset::SurfaceParamType::Bool:
        break;
    }
    return 1;
}

// **The surface shader a material names** (ADR 0091): which one, whether it
// reads the scene behind, its parameters as the fields its source declares,
// and whether it compiles. A variant that has not chosen its own shows its
// parent's, and choosing one makes it the variant's.
void drawSurfaceShaderFields(Editor& editor, MaterialFieldRow& row, const asset::MaterialProperties& inherited,
                             bool variant, EditorCommands& commands)
{
    asset::MaterialAsset& next = row.next;
    asset::MaterialProperties& p = next.properties;
    ImGui::PushID("surface");

    const bool own = !variant || next.shaderWritten;
    if (!own) {
        p.shader = inherited.shader;
        p.readsSceneColor = inherited.readsSceneColor;
    }
    sectionName(core::tr(ENG_TR("engine.editor.surface_shader_fields.shader")), variant && !own);
    if (variant && own) {
        ImGui::TableSetColumnIndex(0);
        moveToCellEnd();
        if (iconButton(row.icons, icons::ActionInherit, ImGui::GetFontSize(), "inherit",
                       core::tr(ENG_TR("engine.editor.surface_shader_fields.inherit")),
                       core::tr(ENG_TR("engine.editor.surface_shader_fields.back_to_the_parent_material_tip")))) {
            next.shaderWritten = false;
            p.shader = inherited.shader;
            p.readsSceneColor = inherited.readsSceneColor;
            row.changed = true;
        }
        ImGui::TableSetColumnIndex(1);
    }
    // The file's own name in the box; the whole URN is in the tooltip.
    const bool canEdit = p.shader.starts_with(asset::AssetScheme);
    const float editWidth = canEdit
                                ? ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.surface_shader_fields.edit"))).x +
                                      ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x
                                : 0.0f;
    ImGui::SetNextItemWidth(-FLT_MIN - editWidth);
    const std::string shownShader = p.shader.empty() ? std::string("(built-in)") : p.shader;
    if (ImGui::BeginCombo("##shader", shownShader.c_str())) {
        if (ImGui::Selectable(core::tr(ENG_TR("engine.editor.surface_shader_fields.built_in")), p.shader.empty())) {
            p.shader.clear();
            next.shaderWritten = true;
            row.changed = true;
        }
        for (const std::string& file : editor.content().filesOfKind(ContentKind::Shader)) {
            const std::string urn = std::string(asset::AssetScheme) + file;
            if (ImGui::Selectable(file.c_str(), urn == p.shader)) {
                p.shader = urn;
                next.shaderWritten = true;
                row.changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (canEdit) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.surface_shader_fields.edit"))))
            commands.openFile = p.shader.substr(asset::AssetScheme.size());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.surface_shader_fields.open_the_shader_in_the_tip")));
    }
    sectionName(core::tr(ENG_TR("engine.editor.surface_shader_fields.readsscenebehind")));
    if (ImGui::Checkbox("##reads-scene", &p.readsSceneColor)) {
        next.shaderWritten = true;
        row.changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          core::tr(ENG_TR("engine.editor.surface_shader_fields.blended_surfaces_only_the_shader_tip")));

    if (p.shader.empty()) {
        ImGui::PopID();
        return;
    }

    // Whether it compiles, and what the compiler said if it does not.
    if (g_surfaceCompiler != nullptr) {
        sectionName(core::tr(ENG_TR("engine.editor.surface_shader_fields.status")));
        ImGui::AlignTextToFramePadding();
        const std::optional<render::SurfaceStatus> status = g_surfaceCompiler->status(p.shader);
        if (!g_surfaceCompiler->available()) {
            ImGui::TextDisabled("%s",
                                core::tr(ENG_TR("engine.editor.surface_shader_fields.no_shader_compiler_in_this")));
        }
        else if (!status.has_value()) {
            ImGui::TextDisabled("%s",
                                core::tr(ENG_TR("engine.editor.surface_shader_fields.not_compiled_yet_nothing_drawn")));
        }
        else if (*status == render::SurfaceStatus::Pending) {
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.surface_shader_fields.compiling")));
        }
        else if (*status == render::SurfaceStatus::Ready) {
            ImGui::TextColored(themeColor(palette().success), "%s",
                               core::tr(ENG_TR("engine.editor.surface_shader_fields.compiled")));
        }
        else {
            ImGui::TextColored(themeColor(palette().danger), "%s",
                               core::tr(ENG_TR("engine.editor.surface_shader_fields.does_not_compile")));
            ImGui::PushTextWrapPos(0.0f);
            for (const SurfaceError& error : g_surfaceCompiler->errors(p.shader)) {
                const std::string where = std::filesystem::path(error.file).filename().string();
                ImGui::TextWrapped("%s:%u  %s", where.c_str(), error.line, error.message.c_str());
            }
            ImGui::PopTextWrapPos();
        }
    }

    const asset::SurfaceReflection* reflection = surfaceReflectionOf(editor.content(), p.shader);
    if (reflection == nullptr) {
        sectionName(core::tr(ENG_TR("engine.editor.surface_shader_fields.source")));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.surface_shader_fields.the_shader_file_cannot_be")));
        ImGui::PopID();
        return;
    }

    // **A parameter the material does not set reads the shader's default**, and
    // is shown as that; setting one writes it into the material, and the reset
    // button takes it back out. A variant's parent's value is its default.
    const auto effective = [&](std::string_view name) -> const asset::ShaderParameter* {
        if (const asset::ShaderParameter* mine = p.shaderParameter(name); mine != nullptr)
            return mine;
        return variant ? inherited.shaderParameter(name) : nullptr;
    };
    const auto drop = [&](std::string_view name) {
        std::erase_if(p.shaderParameters, [&](const asset::ShaderParameter& one) { return one.name == name; });
        row.changed = true;
    };
    for (const asset::SurfaceParam& param : reflection->params) {
        ImGui::PushID(param.name.c_str());
        const asset::ShaderParameter* current = effective(param.name);
        const bool set = p.shaderParameter(param.name) != nullptr;
        std::array<core::f32, 4> value = current != nullptr ? current->value : param.value;
        sectionName(param.name, !set);
        if (set) {
            ImGui::TableSetColumnIndex(0);
            moveToCellEnd();
            if (iconButton(row.icons, icons::ActionInherit, ImGui::GetFontSize(), "reset",
                           core::tr(ENG_TR("engine.editor.surface_shader_fields.reset")),
                           core::tr(ENG_TR("engine.editor.surface_shader_fields.back_to_the_shader_s_tip"))))
                drop(param.name);
            ImGui::TableSetColumnIndex(1);
        }
        if (drawSurfaceParam(param, value)) {
            asset::ShaderParameter written;
            written.name = param.name;
            written.value = value;
            written.components = surfaceComponents(param.type);
            p.setShaderParameter(std::move(written));
            row.changed = true;
            row.continuing = row.continuing || (ImGui::IsItemActive() && !ImGui::IsItemActivated());
        }
        ImGui::PopID();
    }
    for (const asset::SurfaceTexture& texture : reflection->textures) {
        ImGui::PushID(texture.name.c_str());
        const asset::ShaderParameter* current = effective(texture.name);
        std::string urn = current != nullptr ? current->texture : std::string{};
        sectionName(texture.name);
        if (drawMapField(urn, editor.content())) {
            if (urn.empty()) {
                drop(texture.name);
            }
            else {
                asset::ShaderParameter written;
                written.name = texture.name;
                written.texture = urn;
                written.linear =
                    texture.fallback == asset::SurfaceTextureDefault::Normal || (current != nullptr && current->linear);
                p.setShaderParameter(std::move(written));
                row.changed = true;
            }
        }
        ImGui::PopID();
    }
    for (const asset::SurfaceDiagnostic& problem : reflection->errors) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"subject", problem.subject}};
        const std::string text = core::engineCatalog().format(core::TextKey{core::hashTextKey(problem.key)}, args);
        sectionName(core::tr(ENG_TR("engine.editor.surface_shader_fields.problem")));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(themeColor(palette().danger), "%s",
                           core::tr(ENG_TR("engine.editor.surface_shader_fields.problem_line"),
                                    {{"line", static_cast<core::i64>(problem.line)}, {"problem", text}})
                               .c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopID();
}

// **The material open in the editor**: a ball wearing it, every field of its
// file, which parameters a part wearing it may change, and Save. Edits are shown
// in every world as they are made and undone with the panel's own history -- an
// edit to a file is not an edit to the scene. Closing without saving puts the
// file's look back.
void drawMaterialPanel(Editor& editor, const IconAtlas* icons, EditorCommands& commands)
{
    const Editor::MaterialSession& session = editor.materialSession();
    if (!session.open())
        return;

    // **Where the Viewport is, the size it is** (the owner: as the big engines
    // open theirs), the first time -- and wherever somebody moves it after.
    if (const ImGuiWindow* world3d = ImGui::FindWindowByName("###Viewport");
        world3d != nullptr && world3d->DockNode != nullptr)
        ImGui::SetNextWindowDockID(world3d->DockNode->ID, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 60.0f, ImGui::GetFontSize() * 36.0f),
                             ImGuiCond_FirstUseEver);

    // What the window's own X asks when there is something unsaved.
    static bool s_askClose = false;
    bool keepOpen = true;
    const std::string fileName = std::filesystem::path(session.path).filename().string();
    const std::string title = tabIconPad() + fileName + (session.dirty() ? " *" : "") + "###Material";
    const bool visible = ImGui::Begin(title.c_str(), &keepOpen);
    if (!keepOpen) {
        if (session.dirty())
            s_askClose = true;
        else
            editor.closeMaterial();
        keepOpen = true;
    }
    if (!visible) {
        ImGui::End();
        return;
    }

    asset::MaterialProperties inherited;
    const bool variant = !session.asset.parent.empty();
    asset::MaterialFieldMask parentDeclares = 0;
    std::vector<std::string> parentDeclaresShader;
    if (variant) {
        if (asset::MaterialLibrary* library = editor.materialLibrary(); library != nullptr) {
            const asset::ResolvedMaterial& parent = library->resolve(session.asset.parent);
            inherited = parent.properties;
            parentDeclares = parent.instanceParameters;
            parentDeclaresShader = parent.instanceShaderParameters;
        }
    }
    const std::filesystem::path absolute = editor.content().root() / std::filesystem::path(session.path);

    // --- The bar: what is open, and what can be done to it ---------------------
    {
        const float glyph = ImGui::GetFontSize();
        ImGui::AlignTextToFramePadding();
        if (drawIcon(icons, variant ? icons::ActionMaterialVariant : icons::ContentMaterial, glyph))
            ImGui::SameLine();
        ImGui::TextUnformatted(session.path.c_str());
        if (variant) {
            ImGui::SameLine();
            ImGui::TextDisabled(
                "%s", core::tr(ENG_TR("engine.editor.material_panel.variant_of"), {{"parent", session.asset.parent}})
                          .c_str());
        }
        // Right-aligned by what they measure: an icon button is its picture and
        // its padding, and Save is its picture, a gap and its word as well.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float button = glyph + style.FramePadding.x * 2.0f;
        const float spacing = style.ItemSpacing.x;
        const float saveWidth = glyph + style.ItemInnerSpacing.x +
                                ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.material_panel.save"))).x +
                                style.FramePadding.x * 2.0f;
        const float actions = saveWidth + (button + spacing) * 2.0f + style.WindowPadding.x;
        ImGui::SameLine(std::max(ImGui::GetWindowWidth() - actions, ImGui::GetCursorPosX() + spacing));
        ImGui::BeginDisabled(session.undo.empty());
        if (iconButton(icons, icons::ActionUndo, glyph, "##undo", core::tr(ENG_TR("engine.editor.material_panel.undo")),
                       core::tr(ENG_TR("engine.editor.material_panel.undo_the_last_change_to_tip"))) &&
            editor.undoMaterial() && g_thumbnails != nullptr)
            g_thumbnails->refresh(absolute);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(session.redo.empty());
        if (iconButton(icons, icons::ActionRedo, glyph, "##redo", core::tr(ENG_TR("engine.editor.material_panel.redo")),
                       core::tr(ENG_TR("engine.editor.material_panel.redo_tip"))) &&
            editor.redoMaterial() && g_thumbnails != nullptr)
            g_thumbnails->refresh(absolute);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!session.dirty());
        if (session.dirty()) {
            ImGui::PushStyleColor(ImGuiCol_Button, themeColor(palette().accentFill));
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().onAccent));
        }
        g_iconOnAccent = session.dirty();
        const bool save =
            labeledIconButton(icons, icons::ActionSave, core::tr(ENG_TR("engine.editor.material_panel.save")));
        g_iconOnAccent = false;
        if (session.dirty())
            ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        if (save)
            (void)editor.saveMaterial();
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.material_panel.write_it_to_its_file_tip")));
    }
    ImGui::Separator();

    asset::MaterialAsset next = session.asset;
    MaterialFieldRow row{next, inherited, variant};
    row.icons = icons;

    // --- The preview and the fields, side by side, the split movable -----------
    const ImGuiTableFlags split = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("##material-split", 2, split, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("##preview", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("##fields", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableNextRow();

        // The preview: as large as its column allows, turned by dragging, on
        // the shape somebody picks under it.
        ImGui::TableSetColumnIndex(0);
        {
            static ThumbnailCache::ShowcaseView s_view;
            const float room = ImGui::GetContentRegionAvail().x;
            const float below = ImGui::GetFrameHeightWithSpacing() * 2.0f;
            const float height = ImGui::GetContentRegionAvail().y > 0.0f
                                     ? ImGui::GetContentRegionAvail().y
                                     : ImGui::GetWindowHeight() - ImGui::GetCursorPosY();
            const float edge = std::max(64.0f, std::min(room, height - below));
            // Rendered at the pixels it is shown at, in steps, so a resize does
            // not redraw it every frame of the drag.
            const float pixels = edge * ImGui::GetIO().DisplayFramebufferScale.x;
            s_view.edge = std::clamp<core::u32>(static_cast<core::u32>(std::ceil(pixels / 64.0f) * 64.0f), 128u, 1024u);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float offset = std::max(0.0f, (room - edge) * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(at.x + offset, at.y));
            const ImVec2 corner = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##preview", ImVec2(edge, edge));
            const bool turning = ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
            if (turning) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                s_view.yaw -= delta.x * 0.01f;
                s_view.pitch = std::clamp(s_view.pitch + delta.y * 0.01f, -1.2f, 1.2f);
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                s_view.yaw = 0.0f;
                s_view.pitch = 0.0f;
            }
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.material_panel.drag_to_turn_it_double_tip")));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(corner, ImVec2(corner.x + edge, corner.y + edge), ImGui::GetColorU32(ImGuiCol_FrameBg),
                                ImGui::GetStyle().FrameRounding);
            if (g_thumbnails != nullptr && g_device != nullptr) {
                const ThumbnailCache::Thumbnail picture = g_thumbnails->requestShowcase(absolute, s_view);
                if (SDL_GPUTexture* native = picture.valid() ? rhi::nativeTexture(*g_device, picture.texture) : nullptr;
                    native != nullptr) {
                    draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)), corner,
                                          ImVec2(corner.x + edge, corner.y + edge), ImVec2(0, 0), ImVec2(1, 1),
                                          IM_COL32_WHITE, ImGui::GetStyle().FrameRounding);
                }
            }
            // What wears it, as a row of pictures under it.
            ImGui::SetCursorScreenPos(ImVec2(at.x + offset, corner.y + edge + ImGui::GetStyle().ItemSpacing.y));
            struct Shape
            {
                std::string_view icon;
                const char* word;
                core::i32 value;
            };
            constexpr Shape Shapes[] = {{icons::ActionShapeBall, "ball", 1},
                                        {icons::ActionShapeBlock, "block", 0},
                                        {icons::ActionShapeCylinder, "cylinder", 2}};
            for (const Shape& shape : Shapes) {
                const bool chosen = s_view.shape == shape.value;
                if (chosen)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                const std::string id = std::string("##shape-") + shape.word;
                const std::string tip = std::string("on a ") + shape.word;
                if (iconButton(icons, shape.icon, ImGui::GetFontSize(), id.c_str(), shape.word, tip.c_str()))
                    s_view.shape = shape.value;
                if (chosen)
                    ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }

        // The fields, in the Properties panel's grid and headings.
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginChild("##material-fields", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
            using F = asset::MaterialField;
            const auto section = [](const char* name, const char* id) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                return propertiesSection(name) && beginSectionGrid(id);
            };
            if (section(core::tr(ENG_TR("engine.editor.material_panel.surface")), "surface")) {
                {
                    asset::MaterialProperties& p = row.begin(F::Color);
                    row.end(F::Color, ImGui::ColorEdit3("##value", &p.color.r, ImGuiColorEditFlags_Float));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::Transparency);
                    row.end(F::Transparency, ImGui::SliderFloat("##value", &p.transparency, 0.0f, 1.0f, "%.3f"));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::AlphaMode);
                    const std::array<const char*, 3> Modes{
                        core::tr(ENG_TR("engine.editor.material_panel.alpha.opaque")),
                        core::tr(ENG_TR("engine.editor.material_panel.alpha.mask")),
                        core::tr(ENG_TR("engine.editor.material_panel.alpha.blend"))};
                    int mode = std::clamp(p.alphaMode, 0, 2);
                    const bool picked = ImGui::Combo("##value", &mode, Modes.data(), static_cast<int>(Modes.size()));
                    if (picked)
                        p.alphaMode = mode;
                    row.end(F::AlphaMode, picked);
                }
                {
                    asset::MaterialProperties& p = row.begin(F::AlphaCutoff);
                    row.end(F::AlphaCutoff, ImGui::SliderFloat("##value", &p.alphaCutoff, 0.0f, 1.0f, "%.3f"));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::DoubleSided);
                    row.end(F::DoubleSided, ImGui::Checkbox("##value", &p.doubleSided));
                }
                endSectionGrid();
            }
            if (section(core::tr(ENG_TR("engine.editor.material_panel.lighting")), "lighting")) {
                {
                    asset::MaterialProperties& p = row.begin(F::Metalness);
                    row.end(F::Metalness, ImGui::SliderFloat("##value", &p.metalness, 0.0f, 1.0f, "%.3f"));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::Roughness);
                    row.end(F::Roughness, ImGui::SliderFloat("##value", &p.roughness, 0.0f, 1.0f, "%.3f"));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::Emissive);
                    row.end(F::Emissive, ImGui::ColorEdit3("##value", &p.emissive.r,
                                                           ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR));
                }
                endSectionGrid();
            }
            if (section(core::tr(ENG_TR("engine.editor.material_panel.textures")), "textures")) {
                {
                    asset::MaterialProperties& p = row.begin(F::ColorMap);
                    row.end(F::ColorMap, drawMapField(p.colorMap, editor.content()));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::NormalMap);
                    row.end(F::NormalMap, drawMapField(p.normalMap, editor.content()));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::NormalScale);
                    row.end(F::NormalScale,
                            dragNumber("##value", ImGuiDataType_Float, &p.normalScale, 1, 0.01f, "%.3f"));
                    p.normalScale = std::clamp(p.normalScale, 0.0f, 4.0f);
                }
                {
                    asset::MaterialProperties& p = row.begin(F::MetallicRoughnessMap);
                    row.end(F::MetallicRoughnessMap, drawMapField(p.metallicRoughnessMap, editor.content()));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::EmissiveMap);
                    row.end(F::EmissiveMap, drawMapField(p.emissiveMap, editor.content()));
                }
                {
                    // One repeat of the textures on a part's faces, in metres;
                    // zero stretches each over the whole face.
                    asset::MaterialProperties& p = row.begin(F::TileSize);
                    const bool edited = dragNumber("##value", ImGuiDataType_Float, &p.tileSize, 1, 0.05f, "%.2f m");
                    p.tileSize = std::max(p.tileSize, 0.0f);
                    ImGui::SetItemTooltip("%s",
                                          core::tr(ENG_TR("engine.editor.material_panel.how_big_one_repeat_of_tip")));
                    row.end(F::TileSize, edited);
                }
                endSectionGrid();
            }
            // What only a terrain reads (ADR 0113), so a material worn by
            // parts alone can leave the section closed.
            if (section(core::tr(ENG_TR("engine.editor.material_panel.terrain")), "terrain")) {
                {
                    asset::MaterialProperties& p = row.begin(F::HeightMap);
                    row.end(F::HeightMap, drawMapField(p.heightMap, editor.content()));
                }
                {
                    asset::MaterialProperties& p = row.begin(F::Triplanar);
                    const bool edited = ImGui::Checkbox("##value", &p.triplanar);
                    ImGui::SetItemTooltip(
                        "%s", core::tr(ENG_TR("engine.editor.material_panel.projected_from_three_axes_so_tip")));
                    row.end(F::Triplanar, edited);
                }
                {
                    asset::MaterialProperties& p = row.begin(F::BlendSharpness);
                    const bool edited = ImGui::SliderFloat("##value", &p.blendSharpness, 0.0f, 1.0f, "%.2f");
                    ImGui::SetItemTooltip(
                        "%s", core::tr(ENG_TR("engine.editor.material_panel.where_this_layer_meets_another_tip")));
                    row.end(F::BlendSharpness, edited);
                }
                // **How the layer's repeat is broken up** (ADR 0113's
                // amendment).
                {
                    asset::MaterialProperties& p = row.begin(F::TilingVariation);
                    const bool edited = ImGui::SliderFloat("##value", &p.tilingVariation, 0.0f, 1.0f, "%.2f");
                    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.material.tiling_variation_tip")));
                    row.end(F::TilingVariation, edited);
                }
                {
                    asset::MaterialProperties& p = row.begin(F::TilingFarScale);
                    const bool edited = ImGui::SliderFloat("##value", &p.tilingFarScale, 1.0f, 32.0f, "%.1f x",
                                                           ImGuiSliderFlags_Logarithmic);
                    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.material.tiling_far_scale_tip")));
                    row.end(F::TilingFarScale, edited);
                }
                {
                    asset::MaterialProperties& p = row.begin(F::HexTiling);
                    const bool edited = ImGui::Checkbox("##value", &p.hexTiling);
                    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.material.hex_tiling_tip")));
                    row.end(F::HexTiling, edited);
                }
                endSectionGrid();
            }
            if (section(core::tr(ENG_TR("engine.editor.material_panel.surface_shader")), "surface-shader")) {
                drawSurfaceShaderFields(editor, row, inherited, variant, commands);
                endSectionGrid();
            }

            // **What a part wearing this may change about it** (ADR 0090).
            // Nothing by default, so an authored material is what its author
            // made; a variant inherits its parent's list and may add to it.
            if (section(core::tr(ENG_TR("engine.editor.material_panel.part_overrides")), "part-overrides")) {
                for (const F field : {F::Color, F::Transparency, F::Emissive, F::Metalness, F::Roughness,
                                      F::NormalScale, F::AlphaCutoff}) {
                    ImGui::PushID(100 + static_cast<int>(field));
                    const bool fromParent = (parentDeclares & asset::fieldBit(field)) != 0;
                    bool declared = fromParent || (next.instanceParameters & asset::fieldBit(field)) != 0;
                    sectionName(materialFieldLabel(field));
                    ImGui::BeginDisabled(fromParent);
                    if (ImGui::Checkbox("##declared", &declared)) {
                        if (declared)
                            next.instanceParameters |= asset::fieldBit(field);
                        else
                            next.instanceParameters = static_cast<asset::MaterialFieldMask>(next.instanceParameters &
                                                                                            ~asset::fieldBit(field));
                        row.changed = true;
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(
                            "%s", core::tr(fromParent ? ENG_TR("engine.editor.material_panel.declared_by_parent_tip")
                                                      : ENG_TR("engine.editor.material_panel.part_may_set_tip")));
                    ImGui::PopID();
                }
                // And the surface shader's own parameters (ADR 0091), by name:
                // every value one declares, and none of its textures -- a part
                // with a texture of its own is a material of its own.
                if (const asset::SurfaceReflection* reflection =
                        next.properties.shader.empty() ? nullptr
                                                       : surfaceReflectionOf(editor.content(), next.properties.shader);
                    reflection != nullptr) {
                    for (const asset::SurfaceParam& param : reflection->params) {
                        ImGui::PushID(param.name.c_str());
                        const bool fromParent =
                            std::binary_search(parentDeclaresShader.begin(), parentDeclaresShader.end(), param.name);
                        const auto mine = std::lower_bound(next.instanceShaderParameters.begin(),
                                                           next.instanceShaderParameters.end(), param.name);
                        const bool listed = mine != next.instanceShaderParameters.end() && *mine == param.name;
                        bool declared = fromParent || listed;
                        sectionName(param.name);
                        ImGui::BeginDisabled(fromParent);
                        if (ImGui::Checkbox("##declared", &declared)) {
                            if (declared)
                                asset::addShaderParameterName(next.instanceShaderParameters, param.name);
                            else if (listed)
                                next.instanceShaderParameters.erase(mine);
                            row.changed = true;
                        }
                        ImGui::EndDisabled();
                        if (fromParent && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip(
                                "%s",
                                core::tr(ENG_TR("engine.editor.material_panel.declared_by_the_parent_material_tip")));
                        ImGui::PopID();
                    }
                }
                endSectionGrid();
            }
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }

    if (row.changed) {
        editor.editMaterial(next, row.continuing);
        if (g_thumbnails != nullptr)
            g_thumbnails->refresh(absolute);
    }

    // **Closing with something unsaved asks** -- the window's X used to put the
    // file's look back without a word, which is the edit a person loses once.
    if (s_askClose) {
        ImGui::OpenPopup("###Unsaved material");
        s_askClose = false;
    }
    if (ImGui::BeginPopupModal(
            labelled(ENG_TR("engine.editor.material_panel.unsaved_material"), "###Unsaved material").c_str(), nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            core::tr(ENG_TR("engine.editor.material_panel.save_changes_to"), {{"file", fileName}}).c_str());
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.material_panel.not_saving_puts_the_file")));
        ImGui::Spacing();
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.material_panel.save")))) {
            if (editor.saveMaterial())
                editor.closeMaterial();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.material_panel.don_t_save")))) {
            editor.closeMaterial();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.material_panel.cancel"))) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_escapeTaken = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

// The skinned mesh at or above `id`, or an invalid id.
//
// The same walk `PhysicsSync::rigAbove` does and for the same reason: a `Bone`
// is parented to a `MeshPart` but may sit under an `Attachment` under another
// `Bone`, so "which rig is this joint name against" is a question about
// ancestry rather than about the parent. Duplicated rather than exposed because
// the physics one answers with the pool it is mirroring and this one answers
// with the pool the panel is looking at; the walk is four lines.
[[nodiscard]] core::InstanceId rigAbove(const scene::World& world, core::InstanceId id)
{
    for (core::InstanceId walk = id; walk.valid(); walk = world.parentOf(walk)) {
        if (g_skeleton != nullptr && g_skeleton->jointCount(walk) > 0)
            return walk;
    }
    return {};
}

// **A sequence, edited the way a gradient is** (ADR 0110): the whole sequence
// drawn as a bar, a handle under it for every stop, and below it the selected
// stop's time and value with a button to add a stop and one to take the
// selected one away. The ends are pinned at 0 and 1 -- a sequence must start
// and end there -- so their handles change colour or value and never move.
//
// Which stop is selected lives in ImGui's own storage under the widget's id,
// so two sequence rows keep two selections and nothing here outlives the panel.
// True when `value` was changed, and then it is still a valid sequence.
template <class Keypoint>
[[nodiscard]] bool drawKeypoints(std::vector<Keypoint>& stops, const std::function<ImU32(core::f32)>& colourAt,
                                 const std::function<bool(Keypoint&)>& editValue)
{
    bool changed = false;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID selectedKey = ImGui::GetID("##selected-stop");
    int selected = std::clamp(storage->GetInt(selectedKey, 0), 0, static_cast<int>(stops.size()) - 1);

    const float width = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
    const float barHeight = ImGui::GetFrameHeight() * 0.8f;
    const float handle = ImGui::GetFontSize() * 0.45f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // A checkerboard under it, so a see-through stop reads as see-through.
    const float cell = barHeight * 0.5f;
    for (float x = 0.0f; x < width; x += cell) {
        for (int row = 0; row < 2; ++row) {
            const bool dark = (static_cast<int>(x / cell) + row) % 2 == 0;
            draw->AddRectFilled(
                ImVec2(origin.x + x, origin.y + static_cast<float>(row) * cell),
                ImVec2(origin.x + std::min(x + cell, width), origin.y + static_cast<float>(row + 1) * cell),
                dark ? IM_COL32(90, 90, 96, 255) : IM_COL32(150, 150, 156, 255));
        }
    }
    constexpr int Segments = 64;
    for (int segment = 0; segment < Segments; ++segment) {
        const float from = static_cast<float>(segment) / Segments;
        const float to = static_cast<float>(segment + 1) / Segments;
        draw->AddRectFilledMultiColor(ImVec2(origin.x + from * width, origin.y),
                                      ImVec2(origin.x + to * width, origin.y + barHeight), colourAt(from), colourAt(to),
                                      colourAt(to), colourAt(from));
    }
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + barHeight), IM_COL32(0, 0, 0, 160));

    // The handles: a triangle under the bar at each stop, the selected one
    // filled. Dragging moves an inner stop between its neighbours.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + barHeight));
    ImGui::InvisibleButton("##stops", ImVec2(width, handle * 2.2f));
    const bool pressed = ImGui::IsItemActivated();
    const bool dragging = ImGui::IsItemActive();
    const float mouseTime = std::clamp((ImGui::GetIO().MousePos.x - origin.x) / width, 0.0f, 1.0f);
    if (pressed) {
        float best = FLT_MAX;
        for (int index = 0; index < static_cast<int>(stops.size()); ++index) {
            const float distance = std::fabs(stops[static_cast<core::usize>(index)].time - mouseTime) * width;
            if (distance < best) {
                best = distance;
                selected = index;
            }
        }
    }
    const bool inner = selected > 0 && selected + 1 < static_cast<int>(stops.size());
    if (dragging && inner && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        const auto at = static_cast<core::usize>(selected);
        const float low = stops[at - 1].time;
        const float high = stops[at + 1].time;
        const float time = std::clamp(mouseTime, low, high);
        if (time != stops[at].time) {
            stops[at].time = time;
            changed = true;
        }
    }
    for (int index = 0; index < static_cast<int>(stops.size()); ++index) {
        const float x = origin.x + stops[static_cast<core::usize>(index)].time * width;
        const float top = origin.y + barHeight + 1.0f;
        const ImU32 fill = index == selected ? IM_COL32(255, 196, 70, 255) : IM_COL32(210, 214, 222, 255);
        draw->AddTriangleFilled(ImVec2(x, top), ImVec2(x - handle, top + handle * 1.8f),
                                ImVec2(x + handle, top + handle * 1.8f), fill);
        draw->AddTriangle(ImVec2(x, top), ImVec2(x - handle, top + handle * 1.8f),
                          ImVec2(x + handle, top + handle * 1.8f), IM_COL32(0, 0, 0, 200));
    }

    // The selected stop: its time (the ends' is fixed), its value, and the
    // two buttons.
    auto& stop = stops[static_cast<core::usize>(selected)];
    ImGui::BeginDisabled(!inner);
    float time = stop.time;
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("0.000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    if (ImGui::DragFloat("##time", &time, 0.002f, 0.0f, 1.0f, "%.3f") && inner) {
        const auto at = static_cast<core::usize>(selected);
        stop.time = std::clamp(time, stops[at - 1].time, stops[at + 1].time);
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.keypoints.where_this_stop_is_from_tip")));
    ImGui::SameLine();
    const float buttons = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - buttons, 40.0f));
    if (editValue(stop))
        changed = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(stops.size() >= core::MaxSequenceKeypoints);
    if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
        // Into the widest gap, halfway, carrying the colour already there so
        // adding a stop changes nothing until it is moved.
        core::usize widest = 0;
        for (core::usize index = 1; index < stops.size(); ++index) {
            if (stops[index].time - stops[index - 1].time > stops[widest + 1].time - stops[widest].time)
                widest = index - 1;
        }
        Keypoint added = stops[widest];
        added.time = (stops[widest].time + stops[widest + 1].time) * 0.5f;
        stops.insert(stops.begin() + static_cast<std::ptrdiff_t>(widest + 1), added);
        selected = static_cast<int>(widest + 1);
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.keypoints.add_a_stop_in_the_tip")));
    ImGui::SameLine();
    ImGui::BeginDisabled(!inner);
    if (ImGui::Button("-", ImVec2(ImGui::GetFrameHeight(), 0.0f)) && inner) {
        stops.erase(stops.begin() + selected);
        selected = std::max(selected - 1, 0);
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.keypoints.remove_the_selected_stop_the_tip")));

    storage->SetInt(selectedKey, selected);
    return changed && core::validSequence(stops);
}

[[nodiscard]] bool drawSequenceEditor(scene::Value& value)
{
    const auto toByte = [](core::f32 channel) {
        return static_cast<int>(std::clamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    if (core::ColorSequence* colours = std::get_if<core::ColorSequence>(&value)) {
        const core::ColorSequence shown = *colours;
        return drawKeypoints<core::ColorKeypoint>(
            colours->keypoints,
            [&](core::f32 time) {
                const core::Color3 colour = core::evaluate(shown, time);
                return IM_COL32(toByte(colour.r), toByte(colour.g), toByte(colour.b), 255);
            },
            [](core::ColorKeypoint& stop) {
                float rgb[3]{stop.value.r, stop.value.g, stop.value.b};
                if (!ImGui::ColorEdit3("##stop", rgb, ImGuiColorEditFlags_Float))
                    return false;
                stop.value = core::Color3{rgb[0], rgb[1], rgb[2]};
                return true;
            });
    }
    if (core::NumberSequence* numbers = std::get_if<core::NumberSequence>(&value)) {
        const core::NumberSequence shown = *numbers;
        // A number is drawn as white over the checkerboard, as opaque as one
        // minus it: every number sequence the UI takes is a transparency.
        return drawKeypoints<core::NumberKeypoint>(
            numbers->keypoints,
            [&](core::f32 time) {
                const core::f32 opacity = 1.0f - core::evaluate(shown, time);
                return IM_COL32(245, 245, 245, toByte(opacity));
            },
            [](core::NumberKeypoint& stop) {
                return ImGui::DragFloat("##stop", &stop.value, 0.01f, 0.0f, 0.0f, "%.3f");
            });
    }
    return false;
}

// **The service that decides a script's side, named**, or empty when none does
// (ADR 0138 §2): `ServerScriptService`, `ClientScriptService`, or
// `GlobalScriptService.Server` / `.Client`. What `RunContext` is greyed for.
std::string sideDecidedBy(const scene::World& world, core::InstanceId id)
{
    const scene::ClassId global = world.classes().findId(world.atoms().lookup("GlobalScriptService"));
    for (core::InstanceId walk = world.parentOf(id); walk.valid(); walk = world.parentOf(walk)) {
        const std::string_view name = world.atoms().text(world.classes().find(world.classOf(walk))->name);
        if (name == "ServerScriptService" || name == "ClientScriptService")
            return std::string(name);
        if (world.fixed(walk) && world.classOf(world.parentOf(walk)) == global) {
            const std::string_view folder = world.atoms().text(world.name(walk));
            if (folder == "Server" || folder == "Client")
                return "GlobalScriptService." + std::string(folder);
        }
    }
    return {};
}

// `tree` is the content browser's, and null wherever there is none -- the F3
// overlay over a running game has an inspector and no project. The `Content`
// editor then keeps its text field and its drop target and offers an empty list,
// which is the honest state rather than a hidden control.
void drawEditor(scene::World& world, core::InstanceId root, Inspector& inspector,
                std::span<const core::InstanceId> targets, const scene::PropertyDesc& descriptor,
                const SharedValue& shared, ContentTree* tree, const IconAtlas* icons, audio::AudioSystem* audio,
                EditorCommands* commands)
{
    if (shared.state == SharedState::Unreadable) {
        // The class declares the property and the world cannot read it: a null
        // getter. Shown rather than skipped, because a complete view of the
        // descriptor tables is the entire claim this panel makes.
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.editor.unreadable")));
        return;
    }

    // **Every widget below writes to the WHOLE selection**, and this is the only
    // place that knows how many that is. One instance is the same path with one
    // member, which is what keeps the single-selection behaviour from being a
    // second implementation nobody exercises.
    const auto commit = [&](const scene::Value& value) {
        for (const core::InstanceId target : targets) {
            if (world.alive(target))
                inspector.enqueue(target, descriptor.name, value);
        }
    };

    // The members disagree. The widget still shows something -- the first live
    // one's value, so a drag has somewhere to start -- and each branch below
    // says so in whatever way its widget can: a checkbox has ImGui's own mixed
    // state, a drag hides its number, a text field starts empty. The row's
    // label carries a `(mixed)` tag as well, because two of the twelve widgets
    // (a colour and a matrix) have no honest way to express it themselves.
    //
    // **Editing a mixed field flattens it**, which is what every engine does and
    // the only thing it can mean: a value typed into a field that stands for
    // forty instances is a value for all forty.
    const bool mixed = shared.state == SharedState::Mixed;

    // **The reference is drawn before the absent-value guard below**, and that
    // ordering is the whole of what made an unset one uneditable: a nil
    // reference IS `monostate`, so the guard returned with the word "nil" on
    // screen and no control -- on exactly the field somebody had come to set.
    if (editorFor(descriptor) == EditorKind::InstanceRef) {
        ImGui::PushID(static_cast<int>(descriptor.name.id));
        ImGui::SetNextItemWidth(-FLT_MIN);
        drawInstanceRef(world, root, inspector, targets, descriptor, shared, mixed, tree, icons, commands);
        ImGui::PopID();
        return;
    }
    // A part's material and what it may change about it (ADR 0090), before the
    // absent-value guard for the reason a reference is: wearing nothing IS the
    // absent value, and it is the state somebody opens the field to change.
    if (editorFor(descriptor) == EditorKind::Material) {
        ImGui::PushID(static_cast<int>(descriptor.name.id));
        drawMaterialField(world, inspector, targets, descriptor, shared, mixed, tree, commands);
        ImGui::PopID();
        return;
    }
    if (editorFor(descriptor) == EditorKind::MaterialParameters) {
        ImGui::PushID(static_cast<int>(descriptor.name.id));
        drawMaterialParameters(world, inspector, targets, descriptor, mixed, icons, tree);
        ImGui::PopID();
        return;
    }

    // `editorFor` answers from the DECLARED type, and the variant holds what the
    // property actually has right now. Those disagree whenever a value is absent
    // -- `scene::Value`'s own comment names the two cases, an unset attribute and
    // a nil Instance reference -- and every branch below reaches for its
    // alternative with `std::get`, which throws rather than returning.
    //
    // Guarded here rather than in each branch: there are eight of them, they all
    // have this shape, and an absent value has no editor whatever its type says.
    // Found by a human clicking `go` on `RunService.Parent`, which selects the
    // DataModel -- the one instance in the world whose own Parent is nil -- and
    // took the host down with an uncaught `std::bad_variant_access`.
    if (std::holds_alternative<std::monostate>(shared.value)) {
        ImGui::TextUnformatted(
            core::tr(mixed ? ENG_TR("engine.editor.properties.mixed_value") : ENG_TR("engine.editor.properties.nil")));
        return;
    }

    const EditorKind kind = editorFor(descriptor);
    const std::string_view kindName =
        descriptor.contentKind.valid() ? world.atoms().text(descriptor.contentKind) : std::string_view{};
    ImGui::PushID(static_cast<int>(descriptor.name.id));
    ImGui::SetNextItemWidth(-FLT_MIN);

    // `readOnly` is honoured HERE and not only by the setter: a field that
    // takes a drag the world then refuses is a UI making a claim it cannot
    // keep, and the refusal arrives a frame later with nothing attaching it to
    // the gesture that caused it.
    //
    // Over a selection, `collectCommonProperties` has already answered with the
    // most restrictive descriptor of the set, so read-only for any member is
    // read-only here.
    //
    // **`RunContext` inside a script service is the service's** (ADR 0138 §2):
    // greyed, and hovering it says which service decides.
    std::string decidedBy;
    if (descriptor.name == world.atoms().lookup("RunContext")) {
        for (const core::InstanceId target : targets) {
            if (decidedBy = sideDecidedBy(world, target); !decidedBy.empty())
                break;
        }
    }
    const bool locked = !editable(descriptor) || !decidedBy.empty();
    if (locked)
        ImGui::BeginDisabled();

    switch (kind) {
    case EditorKind::Checkbox: {
        bool value = std::get<bool>(shared.value);
        // The one widget ImGui expresses this natively for, and it draws a
        // filled square rather than a tick or a gap.
        if (mixed)
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        if (ImGui::Checkbox("##value", &value))
            commit(scene::Value{value});
        if (mixed)
            ImGui::PopItemFlag();
        break;
    }
    case EditorKind::Number: {
        f64 value = std::get<f64>(shared.value);
        // A format with no conversion in it is ImGui's own way of saying "do
        // not show the number": the drag still works and a ctrl-click still
        // opens an empty field that parses. Which is exactly right for a value
        // that is nobody's -- the alternative shows one member's number as if
        // it were everyone's.
        if (dragNumber("##value", ImGuiDataType_Double, &value, 1, 0.01f, mixed ? "--" : "%.4f"))
            commit(scene::Value{value});
        break;
    }
    case EditorKind::Code: {
        // **A row is one line high and a script is not.** The grid drew every
        // line of `Source`, so an instance with a fifty-line script in it made
        // the Properties panel fifty rows tall and buried whatever came after.
        //
        // Nothing at all when there is no code, which is what "nothing to see"
        // looks like -- and an empty field inviting somebody to type a program
        // into a property grid is not the offer to make when double-clicking
        // the instance opens a code editor two panels over.
        const std::string& text = mixed ? std::string() : std::get<std::string>(shared.value);
        if (mixed) {
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor.mixed")));
            break;
        }
        if (text.empty())
            break;

        // The first line, and an ellipsis for everything below it. The first
        // line of a Luau file is nearly always `--!strict` or the top of the
        // comment that says what the file is, which is exactly the glance a
        // property grid is for.
        const std::size_t breakAt = text.find('\n');
        std::string preview = text.substr(0, std::min(breakAt, static_cast<std::size_t>(120)));
        while (!preview.empty() && (preview.back() == '\r' || preview.back() == ' '))
            preview.pop_back();
        if (breakAt != std::string::npos || preview.size() < text.size())
            preview += "  ...";
        ImGui::TextDisabled("%s", preview.c_str());
        break;
    }
    case EditorKind::Text: {
        const std::string& text = std::get<std::string>(shared.value);
        char buffer[256]{};
        if (!mixed) {
            if (text.size() + 1 > sizeof(buffer)) {
                // Editing through a buffer that cannot hold the value would
                // write a truncated string back on the first Enter. Shown, not
                // offered.
                ImGui::TextUnformatted(text.c_str());
                break;
            }
            std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
        }
        // A mixed field starts EMPTY behind a hint rather than pre-filled with
        // one member's string. Pre-filling would make replacing forty names
        // with one look like a correction rather than an overwrite -- and
        // Enter on a field nobody edited would do it.
        // **A joint name is a string the rig has to agree with**, and until
        // this picker existed the only way to find out whether it did was to
        // type a guess and watch `JointIndex` for a -1. A rig exported from
        // Blender calls the same bone `mixamorig:LeftHand` or `hand.L` or
        // `Bip01 L Hand` depending on who exported it, and nobody remembers
        // which (E9 step 9).
        //
        // Keyed on the PROPERTY's name rather than on the class, which keeps
        // this file's rule -- no switch on a class name -- and is narrower
        // besides: `JointName` is the property this is about, and a second
        // class declaring one would get the picker for free. There is no
        // descriptor field to hang it off the way the audio button hangs off
        // `ContentKind`, because a joint is not a file.
        const bool namesJoint =
            world.atoms().text(descriptor.name) == "JointName" && g_skeleton != nullptr && targets.size() == 1;
        const core::InstanceId rig = namesJoint ? rigAbove(world, targets[0]) : core::InstanceId{};
        const core::u32 jointCount = rig.valid() ? g_skeleton->jointCount(rig) : 0;

        const float pickWidth = ImGui::GetFrameHeight();
        const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
        if (jointCount > 0)
            ImGui::SetNextItemWidth(-(pickWidth + inner));

        const bool entered =
            mixed ? ImGui::InputTextWithHint("##value", core::tr(ENG_TR("engine.editor.editor.mixed")), buffer,
                                             sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)
                  : ImGui::InputText("##value", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered)
            commit(scene::Value{std::string(buffer)});

        if (jointCount > 0) {
            ImGui::SameLine(0.0f, inner);
            if (ImGui::Button("...", ImVec2(pickWidth, 0.0f)))
                ImGui::OpenPopup("joint-pick");

            if (ImGui::BeginPopup("joint-pick")) {
                // **Filtered, because 677 is a real number.** The horse that
                // opened this milestone has that many joints, and a flat list
                // of them is a scroll bar rather than a picker.
                static std::array<char, 64> filter{};
                // Cleared on the frame the popup opens rather than every frame,
                // which would overwrite what somebody is typing with what they
                // started from. `dialogOpening()` says the same thing and is
                // declared below this, for the modals.
                if (ImGui::IsWindowAppearing())
                    filter.fill(0);
                ImGui::SetNextItemWidth(220.0f * ImGui::GetStyle().FontScaleMain);
                ImGui::InputTextWithHint("##joint-filter", core::tr(ENG_TR("engine.editor.editor.filter")),
                                         filter.data(), filter.size());

                ImGui::BeginChild("joint-list", ImVec2(220.0f, 260.0f));
                const std::string_view needle(filter.data());
                for (core::u32 joint = 0; joint < jointCount; ++joint) {
                    const std::string_view name = g_skeleton->jointName(rig, joint);
                    if (!needle.empty() && !containsFold(name, needle))
                        continue;
                    // Indented by depth, so the list reads as the tree it is --
                    // which is what tells a hand from the forearm above it when
                    // a rig has named neither of them well.
                    core::u32 depth = 0;
                    for (core::i32 walk = g_skeleton->jointParent(rig, joint); walk >= 0 && depth < 24;
                         walk = g_skeleton->jointParent(rig, static_cast<core::u32>(walk))) {
                        depth += 1;
                    }
                    const std::string label = std::string(depth * 2, ' ') + std::string(name);
                    if (ImGui::Selectable(label.c_str(), name == std::string_view(buffer))) {
                        commit(scene::Value{std::string(name)});
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }
        break;
    }
    case EditorKind::Content: {
        // **A URI is a string, and a person is not a URI.** The engine resolves
        // `asset://…` through the mounts and could not care which file a person
        // meant; what a person has is a name they half remember and a browser
        // full of files. So this is three ways in and they are all the same
        // property: type the path, pick it from the ones this property ACCEPTS,
        // or drag the file onto the field.
        const std::string& current = std::get<std::string>(shared.value);
        char buffer[256]{};
        if (!mixed && current.size() + 1 <= sizeof(buffer))
            std::snprintf(buffer, sizeof(buffer), "%s", current.c_str());

        // **A speaker on the rows that name a sound**, which is what makes a
        // `Content` something you can check rather than something you hope
        // about. It hangs off the property's declared `ContentKind` and not off
        // the class, so it is on every Audio content in the IDL and on nothing
        // else -- this file still has no switch on a class name in it.
        //
        // Nothing to audition on a mixed selection or an empty path: one button
        // cannot preview four different files, and there is no file in the
        // second case.
        const bool audible =
            audio != nullptr && contentKindNamed(kindName) == ContentKind::Audio && !mixed && !current.empty();

        // **Every widget on a property row is one frame high**, and the
        // audition button was not: `iconButton` sizes the PICTURE and the button
        // then adds frame padding around it, so asking for `GetFrameHeight()`
        // worth of icon produced a button taller and wider than the field beside
        // it -- which pushed past the column, made this row taller than every
        // other row, and is what "meio zuado" was pointing at.
        //
        // So the glyph is what is left of a frame after its padding, which is
        // the font size, and the width the row reserves is what the button will
        // actually measure. One row, one height.
        const ImVec2 padding = ImGui::GetStyle().FramePadding;
        const float pickWidth = ImGui::GetFrameHeight();
        const float glyph = ImGui::GetFontSize();
        const float playWidth = glyph + padding.x * 2.0f;
        const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(-(audible ? pickWidth + playWidth + inner * 2.0f : pickWidth + inner));
        // The scheme a path starts with, as a hint: not a word, and not the
        // catalog's.
        const bool typed = ImGui::InputTextWithHint(
            "##value", mixed ? core::tr(ENG_TR("engine.editor.properties.mixed_value")) : asset::AssetScheme.data(),
            buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue);
        if (typed)
            commit(scene::Value{std::string(buffer)});

        // **The field itself takes a drop from the browser.** Dragging a file
        // onto the property it belongs to is the gesture every engine has, and
        // it is the one that does not require knowing what the path is called.
        //
        // **Only a file of the kind it takes** lights it up: a folder or a
        // material dropped on a texture was written in as a path to nothing.
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            const ContentKind wanted = contentKindNamed(kindName);
            const bool fits =
                peek != nullptr && peek->IsDataType(kContentDragPayload) &&
                !static_cast<const ContentDrag*>(peek->Data)->folder &&
                (wanted == ContentKind::Other ||
                 contentKindOf(
                     std::filesystem::path(static_cast<const ContentDrag*>(peek->Data)->path).filename().string()) ==
                     wanted);
            if (const ImGuiPayload* took = fits ? ImGui::AcceptDragDropPayload(kContentDragPayload) : nullptr;
                took != nullptr) {
                const auto* dragged = static_cast<const ContentDrag*>(took->Data);
                commit(scene::Value{std::string(asset::AssetScheme) + dragged->path});
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine(0.0f, inner);
        if (ImGui::Button("...", ImVec2(pickWidth, 0.0f)))
            ImGui::OpenPopup("content-pick");

        if (audible) {
            ImGui::SameLine(0.0f, inner);
            // **Play and pause are the same button**, because they are the same
            // question asked twice -- and a person who has just started a
            // two-minute track needs the way to stop it in the place they
            // started it, not somewhere else on the row.
            const bool playing = audio->auditioning(current);
            if (iconButton(icons, playing ? icons::ActionPause : icons::ActionPlay, glyph, "audition",
                           playing ? "||" : ">",
                           core::tr(playing ? ENG_TR("engine.editor.properties.stop_the_preview_tip")
                                            : ENG_TR("engine.editor.properties.hear_this_file_tip")))) {
                if (playing) {
                    audio->stopAudition();
                }
                else {
                    // **At the volume and speed this instance would play it
                    // at**, read the way every other row reads a value: by name
                    // off the selection. A class with no `Volume` simply
                    // auditions at half, which is the same default a `Sound`
                    // carries.
                    //
                    // The GROUP, the distance and the master volume are left
                    // out on purpose. Those are the mix, and an audition is the
                    // file -- a preview that went silent because the listener
                    // is far from the part would be a preview nobody can use.
                    const auto number = [&](const char* name, f32 fallback) {
                        const SharedValue read = sharedValue(world, targets, world.atoms().intern(name));
                        if (read.state == SharedState::Unreadable || !std::holds_alternative<f64>(read.value))
                            return fallback;
                        return static_cast<f32>(std::get<f64>(read.value));
                    };
                    audio->audition(current, number("Volume", 0.5f), number("PlaybackSpeed", 1.0f));
                }
            }
        }

        if (ImGui::BeginPopup("content-pick")) {
            // Read when the popup OPENS rather than every frame: a project's
            // assets are a directory walk, and a list that is rebuilt sixty
            // times a second is one the filter is fighting.
            static std::vector<std::string> candidates;
            static std::array<char, 96> search{};
            if (ImGui::IsWindowAppearing()) {
                candidates =
                    tree != nullptr ? tree->filesOfKind(contentKindNamed(kindName)) : std::vector<std::string>{};
                search.fill(0);
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            ImGui::InputTextWithHint("##content-search", core::tr(ENG_TR("engine.editor.editor.search")), search.data(),
                                     search.size());
            const std::string_view needle{search.data()};

            ImGui::Separator();
            if (ImGui::BeginChild("content-list", ImVec2(ImGui::GetFontSize() * 16.0f, ImGui::GetFontSize() * 12.0f))) {
                // Clearing is a real answer and the first one offered: a
                // property that names nothing is a legal property, and hunting
                // for the way to say so is worse than an extra row.
                if (ImGui::Selectable(core::tr(ENG_TR("engine.editor.editor.none")))) {
                    commit(scene::Value{std::string{}});
                    ImGui::CloseCurrentPopup();
                }

                std::size_t shown = 0;
                for (const std::string& candidate : candidates) {
                    if (!needle.empty() && !containsFold(candidate, needle))
                        continue;
                    ++shown;
                    if (ImGui::Selectable(candidate.c_str())) {
                        commit(scene::Value{std::string("asset://") + candidate});
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (candidates.empty()) {
                    ImGui::TextDisabled("%s", (kindName.empty()
                                                   ? std::string(core::tr(ENG_TR("engine.editor.properties.no_files")))
                                                   : core::tr(ENG_TR("engine.editor.properties.no_files_of_kind"),
                                                              {{"kind", kindName}}))
                                                  .c_str());
                }
                else if (shown == 0) {
                    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.properties.nothing_matches"),
                                                       {{"search", std::string_view(search.data())}})
                                                  .c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        break;
    }
    case EditorKind::Vector3: {
        const core::Vec3 value = std::get<core::Vec3>(shared.value);
        float components[3]{value.x, value.y, value.z};
        if (dragNumber("##value", ImGuiDataType_Float, components, 3, 0.01f, mixed ? "--" : "%.3f"))
            commit(scene::Value{core::Vec3{components[0], components[1], components[2]}});
        break;
    }
    case EditorKind::CFrame: {
        core::CFrameD value = std::get<core::CFrameD>(shared.value);
        f64 position[3]{value.position.x, value.position.y, value.position.z};
        if (dragNumber("##value", ImGuiDataType_Double, position, 3, 0.01f, mixed ? "--" : "%.3f m")) {
            value.position = core::DVec3{position[0], position[1], position[2]};
            commit(scene::Value{value});
        }
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.editor.position_in_metres_tip")));
        // **The rotation, as the three angles `Orientation` uses** -- YXZ, in
        // degrees -- because the owner could move a part here and not turn it:
        // the basis was drawn as nine read-only numbers. Converted for SHOWING
        // every frame and written only when somebody drags or types, so a
        // panel left open never rewrites the matrix it is displaying. Each
        // selected instance keeps its own position; only the turn is shared.
        constexpr f32 kDegrees = 180.0f / 3.14159265f;
        const core::Vec3 radians = core::toEulerYxz(value.rotation);
        // `+ 0.0f` turns a negative zero into a zero, so a square part reads 0.0 and not -0.0.
        float degrees[3]{radians.x * kDegrees + 0.0f, radians.y * kDegrees + 0.0f, radians.z * kDegrees + 0.0f};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (dragNumber("##orientation", ImGuiDataType_Float, degrees, 3, 0.5f, mixed ? "--" : "%.1f\xC2\xB0")) {
            const core::Mat3 turned =
                core::fromEulerYxz(core::Vec3{degrees[0] / kDegrees, degrees[1] / kDegrees, degrees[2] / kDegrees});
            for (const core::InstanceId target : targets) {
                if (!world.alive(target))
                    continue;
                const std::optional<scene::Value> own = world.getProperty(target, descriptor.name);
                const core::CFrameD* current = own.has_value() ? std::get_if<core::CFrameD>(&*own) : nullptr;
                core::CFrameD next = current != nullptr ? *current : value;
                next.rotation = turned;
                inspector.enqueue(target, descriptor.name, scene::Value{next});
            }
        }
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.editor.orientation_in_degrees_about_x_tip")));
        break;
    }
    case EditorKind::Color: {
        const core::Color3 value = std::get<core::Color3>(shared.value);
        float components[3]{value.r, value.g, value.b};
        // Float and HDR because api-design.md 2.3 leaves the range open: a
        // picker that clamped to [0, 1] would silently rewrite a light's
        // intensity the first time anyone looked at it.
        //
        // A mixed colour shows the first member's swatch, which is the one
        // widget here that cannot say otherwise -- a picker with no colour in
        // it is not a picker. The row's `(mixed)` tag is what carries it.
        if (ImGui::ColorEdit3("##value", components, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
            commit(scene::Value{core::Color3{components[0], components[1], components[2]}});
        break;
    }
    case EditorKind::Vector2: {
        const core::Vec2 value = std::get<core::Vec2>(shared.value);
        float components[2]{value.x, value.y};
        if (dragNumber("##value", ImGuiDataType_Float, components, 2, 0.5f, mixed ? "--" : "%.3f"))
            commit(scene::Value{core::Vec2{components[0], components[1]}});
        break;
    }
    case EditorKind::Rect: {
        const core::Rect value = std::get<core::Rect>(shared.value);
        float components[4]{value.min.x, value.min.y, value.max.x, value.max.y};
        if (dragNumber("##value", ImGuiDataType_Float, components, 4, 1.0f, mixed ? "--" : "%.3f")) {
            commit(scene::Value{
                core::Rect{core::Vec2{components[0], components[1]}, core::Vec2{components[2], components[3]}}});
        }
        break;
    }
    case EditorKind::EnumCombo: {
        const scene::EnumValue value = std::get<scene::EnumValue>(shared.value);

        // The domain comes from the DESCRIPTOR, not from the value in the field.
        // Reading it off the value made the combo depend on the instance being
        // there and holding something -- an unset enum property offered no items
        // at all, and there was no way to ask what a property accepts without
        // creating one first. `enumDomainOf` answers from the class.
        //
        // The value's own enum is the fallback and not the source: a hand-built
        // registry (the fixtures) may declare a property without naming its
        // enum, and a field that then rendered nothing would be a regression
        // dressed as a refactor.
        scene::EnumId domain = enumDomainOf(world.enums(), descriptor);
        if (domain == scene::InvalidEnum)
            domain = value.enumId;

        const scene::EnumDescriptor* enumDescriptor = world.enums().find(domain);
        const std::string preview = mixed ? std::string(core::tr(ENG_TR("engine.editor.properties.mixed_value")))
                                          : formatValue(world, shared.value);
        if (enumDescriptor == nullptr) {
            ImGui::TextUnformatted(preview.c_str());
            break;
        }
        // **Closed, the item; open, the whole name** (the owner): a row reads
        // `Landscape`, which is what somebody scanning the grid wants, and the
        // list says `Enum.ScreenOrientation.Landscape`, which is what they type
        // in a script. The full name is under the pointer on the closed box.
        const std::size_t lastDot = preview.rfind('.');
        const std::string shortPreview = mixed || lastDot == std::string::npos ? preview : preview.substr(lastDot + 1);
        const std::string enumName(world.atoms().text(enumDescriptor->name));
        if (ImGui::BeginCombo("##value", shortPreview.c_str())) {
            // Declaration order, which is `GetEnumItems`'s documented order and
            // therefore not something a panel gets to re-sort either.
            for (const scene::EnumItemDesc& item : enumDescriptor->items) {
                const std::string itemName = "Enum." + enumName + "." + std::string(world.atoms().text(item.name));
                // Nothing is ticked while the members disagree: a tick would
                // name one of them as the selection's answer.
                const bool selected = !mixed && domain == value.enumId && item.value == value.value;
                if (ImGui::Selectable(itemName.c_str(), selected))
                    commit(scene::Value{scene::EnumValue{domain, item.value}});
            }
            ImGui::EndCombo();
        }
        else if (!mixed) {
            ImGui::SetItemTooltip("%s", preview.c_str());
        }
        break;
    }
    // **The two composites are SUMMARISED here and edited in their own rows**
    // (`drawCompositeRows`). This is the collapsed line -- `{1.000, -24}, {0.000,
    // 20}`, the same string `formatValue` has printed since M6 -- and the rows
    // underneath are where the numbers are. A box is for a value; the name of
    // the value belongs in the column that holds every other name in this panel.
    case EditorKind::Sequence: {
        // A mixed selection shows its first member's sequence, and an edit
        // writes the edited one to every member -- the same flattening every
        // other widget here does.
        scene::Value edited = shared.value;
        if (drawSequenceEditor(edited))
            commit(edited);
        break;
    }
    case EditorKind::UDim:
    case EditorKind::UDim2:
    // `InstanceRef` never reaches here -- it returns above, before the guard
    // that an absent value would have tripped -- and it is named so this switch
    // stays exhaustive under `-Werror`.
    case EditorKind::InstanceRef:
    case EditorKind::Material:
    case EditorKind::MaterialParameters:
    case EditorKind::ReadOnlyText: {
        // The floor every `ValueType` falls back to, so that one with no editor
        // of its own is still inspectable rather than absent (M4 brief,
        // entering risk 6).
        const std::string text = mixed ? std::string(core::tr(ENG_TR("engine.editor.properties.mixed_value")))
                                       : formatValue(world, shared.value);
        ImGui::TextUnformatted(text.c_str());
        break;
    }
    }

    if (locked)
        ImGui::EndDisabled();
    if (!decidedBy.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const core::I18nArg args[] = {{"service", std::string_view{decidedBy}}};
        ImGui::SetTooltip("%s",
                          core::engineCatalog().format(ENG_TR("engine.overlay.properties.side_set_by"), args).c_str());
    }

    ImGui::PopID();
}

// The gesture this panel currently owns, or zero. File-static for the same
// reason the row buffers above are: the panel is one function called once a
// frame, and its state between frames has nowhere else to live.
core::u64 g_propertyGesture = 0;

// --- Attributes (S5.15) -------------------------------------------------------
//
// **A property is declared by a class and an attribute is not**, which is the
// whole difference and the whole reason this is a separate section rather than
// more rows in the grid above. A class's properties are a fixed list the IDL
// names; an attribute is whatever this instance was given, so the panel cannot
// know what to show until it asks.
//
// **One heading style in the Properties panel.** The property categories are
// rows of the grid, on the header ground with the arrow at the cell's edge;
// Attributes and Tags were framed collapsing headers with their arrow further
// in -- two kinds of heading one above the other, which the survey counted
// against the panel. This draws the categories' kind outside the grid.
//
// **A heading has to look like one** (the owner: "Data", "Transform" and the
// rest were the size and colour of the properties under them, one step to the
// right). The band is the panel's ground tinted toward the accent, and the
// name -- with its arrow -- is in the accent, which is how an inspector tells
// a group from its members at a glance without a second font.
[[nodiscard]] ImU32 propertyHeadingBand()
{
    const ImVec4 ground = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const ImVec4 tint = themeColor(palette().accentFill);
    constexpr float mix = 0.16f;
    return ImGui::GetColorU32(ImVec4(ground.x + (tint.x - ground.x) * mix, ground.y + (tint.y - ground.y) * mix,
                                     ground.z + (tint.z - ground.z) * mix, 1.0f));
}

[[nodiscard]] ImVec4 propertyHeadingInk()
{
    return themeColor(palette().accent);
}

bool propertiesSection(const char* label)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float left = ImGui::GetWindowPos().x;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(left, at.y),
                                              ImVec2(left + ImGui::GetWindowWidth(), at.y + ImGui::GetFrameHeight()),
                                              propertyHeadingBand());
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, propertyHeadingInk());
    const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    ImGui::PopStyleColor();
    return open;
}

// **The property grid's own shape** (the owner: the boxes were not like the
// rest of the panel): two columns at the grid's split, the name on the left in
// from the heading, the value on the right, one frame high. Each section in an
// id scope of its own -- both have an "Add", and two buttons with one id are
// one button to ImGui, which is how pressing Add under Tags added under
// Attributes and set off ImGui's conflicting-id warning.
bool beginSectionGrid(const char* id)
{
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x * 1.5f, 2.0f));
    const ImGuiTableFlags flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("grid", 2, flags)) {
        ImGui::PopStyleVar();
        ImGui::PopID();
        return false;
    }
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

void endSectionGrid()
{
    ImGui::EndTable();
    ImGui::PopStyleVar();
    ImGui::PopID();
}

// The name cell of a row: in from the heading's arrow, level with the widget.
void sectionName(std::string_view text, bool muted)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.5f);
    ImGui::AlignTextToFramePadding();
    if (muted)
        ImGui::TextDisabled("%.*s", static_cast<int>(text.size()), text.data());
    else
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.5f);
    ImGui::TableSetColumnIndex(1);
}

// The primary's, not the selection's. Over four instances "the attributes" is
// four different lists, and merging them would invent a row for something three
// of them do not have -- so the section says whose it is and edits reach the
// whole selection only when somebody presses the button that says so.
void drawAttributes(scene::World& world, Inspector& inspector, core::InstanceId primary,
                    std::span<const core::InstanceId> targets)
{
    if (!propertiesSection(core::tr(ENG_TR("engine.editor.attributes.attributes"))))
        return;
    if (!beginSectionGrid("attributes"))
        return;

    static scene::AttributeMap rows;
    rows.clear();
    world.collectAttributes(primary, rows);

    if (rows.empty())
        sectionName(core::tr(ENG_TR("engine.editor.attributes.none")), true);

    const float trash = ImGui::GetFrameHeight();
    const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
    for (const auto& [name, value] : rows) {
        ImGui::PushID(static_cast<int>(name.id));
        sectionName(world.atoms().text(name));
        ImGui::SetNextItemWidth(-(trash + inner));

        // **Typed by what it HOLDS.** An attribute has no declared type, so the
        // widget follows the value -- which also means changing the type is
        // deleting it and adding it again, and that is the honest shape rather
        // than a type dropdown that would silently reinterpret a number as a
        // string.
        scene::Value edited = value;
        bool changed = false;
        if (const auto* flag = std::get_if<bool>(&value)) {
            bool held = *flag;
            changed = ImGui::Checkbox("##value", &held);
            edited = scene::Value{held};
        }
        else if (const auto* number = std::get_if<f64>(&value)) {
            f64 held = *number;
            changed = dragNumber("##value", ImGuiDataType_Double, &held, 1, 0.01f, "%.3f");
            edited = scene::Value{held};
        }
        else if (const auto* str = std::get_if<std::string>(&value)) {
            std::array<char, 192> buffer{};
            if (str->size() + 1 <= buffer.size())
                std::snprintf(buffer.data(), buffer.size(), "%s", str->c_str());
            changed = ImGui::InputText("##value", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            edited = scene::Value{std::string(buffer.data())};
        }
        else if (const auto* offset = std::get_if<core::Vec3>(&value)) {
            std::array<f32, 3> held{offset->x, offset->y, offset->z};
            changed = dragNumber("##value", ImGuiDataType_Float, held.data(), 3, 0.01f, "%.3f");
            edited = scene::Value{core::Vec3{held[0], held[1], held[2]}};
        }
        else if (const auto* tint = std::get_if<core::Color3>(&value)) {
            std::array<f32, 3> held{tint->r, tint->g, tint->b};
            changed = ImGui::ColorEdit3("##value", held.data(), ImGuiColorEditFlags_NoInputs);
            edited = scene::Value{core::Color3{held[0], held[1], held[2]}};
        }
        else {
            // A `CFrame`, a `UDim2` and a `Rect` are legal attributes too and
            // have no one-line widget -- a matrix is twelve numbers, and this
            // section is a list rather than a second property grid. Named
            // rather than hidden, because a person needs to know it is there
            // even where the panel cannot edit it, and the `x` beside it still
            // removes it.
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("<%s>", scene::valueTypeName(scene::valueType(value)));
        }
        if (changed)
            inspector.enqueueAttribute(primary, name, edited);

        ImGui::SameLine(0.0f, inner);
        // A `Nil` REMOVES it, which is `World::setAttribute`'s own rule rather
        // than something this panel invented.
        if (ImGui::Button("x", ImVec2(trash, 0.0f)))
            inspector.enqueueAttribute(primary, name, scene::Value{});
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.attributes.remove_this_attribute_tip")));
        ImGui::PopID();
    }

    // **A new one: its name where names are, its kind and Add where values
    // are.**
    static std::array<char, 96> newName{};
    static int newType = 1;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##attr-name", core::tr(ENG_TR("engine.editor.attributes.new_attribute")), newName.data(),
                             newName.size());
    ImGui::TableSetColumnIndex(1);
    const float addWidth = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.attributes.add"))).x +
                           ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(addWidth + inner));
    ImGui::Combo(
        "##attr-type", &newType,
        choices({ENG_TR("engine.editor.attributes.kind.boolean"), ENG_TR("engine.editor.attributes.kind.number"),
                 ENG_TR("engine.editor.attributes.kind.text"), ENG_TR("engine.editor.attributes.kind.vector"),
                 ENG_TR("engine.editor.attributes.kind.colour")})
            .c_str());
    ImGui::SameLine(0.0f, inner);
    const bool named = newName[0] != '\0';
    ImGui::BeginDisabled(!named);
    if (ImGui::Button(labelled(ENG_TR("engine.editor.attributes.add"), "##attribute").c_str(),
                      ImVec2(addWidth, 0.0f))) {
        const core::NameAtom atom = world.atoms().intern(std::string_view(newName.data()));
        // The four the section can EDIT, plus text. Offering a `CFrame` here
        // would put a row on screen the panel then refuses to change, which is
        // worse than not offering it.
        scene::Value seed{std::string()};
        if (newType == 0)
            seed = scene::Value{false};
        else if (newType == 1)
            seed = scene::Value{f64{0.0}};
        else if (newType == 3)
            seed = scene::Value{core::Vec3{}};
        else if (newType == 4)
            seed = scene::Value{core::Color3{1.0f, 1.0f, 1.0f}};
        // **Added to the whole selection**, unlike the edits above. "Give these
        // four a `Difficulty`" is the thing somebody wants an attribute for,
        // and doing it one instance at a time is the thing they would give up
        // on.
        for (const core::InstanceId target : targets) {
            if (world.alive(target))
                inspector.enqueueAttribute(target, atom, seed);
        }
        newName.fill(0);
    }
    ImGui::EndDisabled();
    endSectionGrid();
}

// --- Tags (S5.5) --------------------------------------------------------------
//
// **The documented PRIMARY addressing path, and it had no editor surface at
// all.** `docs/manual/assets/streaming.md` names the tag path as the way a
// script finds what a streamed world brought in, `TagService:GetTagged` is the
// call, and until now the only way to put a tag on anything was to write a line
// of Luau -- in a world whose whole point is that it is authored.
void drawTags(scene::World& world, Inspector& inspector, core::InstanceId primary,
              std::span<const core::InstanceId> targets)
{
    if (!propertiesSection(core::tr(ENG_TR("engine.editor.tags.tags"))))
        return;
    if (!beginSectionGrid("tags"))
        return;

    static scene::TagSet held;
    held.clear();
    world.collectTags(primary, held);

    if (held.empty())
        sectionName(core::tr(ENG_TR("engine.editor.tags.none")), true);

    // A row a tag, as a property is: the name, and removing it where a value
    // would be.
    const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
    for (const core::NameAtom tag : held) {
        ImGui::PushID(static_cast<int>(tag.id));
        sectionName(world.atoms().text(tag));
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.tags.remove")), ImVec2(-FLT_MIN, 0.0f))) {
            // Removed from the WHOLE selection. The rows are drawn from the
            // primary's list, and somebody removing one with four things
            // selected means all four.
            for (const core::InstanceId target : targets) {
                if (world.alive(target))
                    inspector.enqueueTag(target, tag, false);
            }
        }
        ImGui::PopID();
    }

    // **What this world already uses.** A tag is free text and a typo is a tag
    // nothing will ever find -- which is the single worst failure this feature
    // has, because it looks exactly like a working one. Offering the names
    // already in use is what turns that from a silent bug into a click.
    static scene::TagSet known;
    known.clear();
    world.collectAllTags(known);

    static std::array<char, 96> pending{};
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool entered = ImGui::InputTextWithHint("##tag-name", core::tr(ENG_TR("engine.editor.tags.new_tag")),
                                                  pending.data(), pending.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TableSetColumnIndex(1);
    const float pickWidth = known.empty() ? 0.0f : ImGui::GetFrameHeight() + inner;
    ImGui::BeginDisabled(pending[0] == '\0');
    const bool pressed = ImGui::Button(labelled(ENG_TR("engine.editor.tags.add"), "##tag").c_str(),
                                       ImVec2(-(pickWidth > 0.0f ? pickWidth : FLT_MIN), 0.0f));
    ImGui::EndDisabled();
    if ((entered || pressed) && pending[0] != '\0') {
        const core::NameAtom atom = world.atoms().intern(std::string_view(pending.data()));
        for (const core::InstanceId target : targets) {
            if (world.alive(target))
                inspector.enqueueTag(target, atom, true);
        }
        pending.fill(0);
    }
    if (!known.empty()) {
        ImGui::SameLine(0.0f, inner);
        if (ImGui::Button("...##tag-pick", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
            ImGui::OpenPopup("tag-pick");
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.tags.the_tags_this_world_already_tip")));
        if (ImGui::BeginPopup("tag-pick")) {
            for (const core::NameAtom tag : known) {
                const std::string_view text = world.atoms().text(tag);
                if (ImGui::Selectable(std::string(text).c_str())) {
                    for (const core::InstanceId target : targets) {
                        if (world.alive(target))
                            inspector.enqueueTag(target, tag, true);
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }
    endSectionGrid();
}

void drawProperties(scene::World& world, core::InstanceId root, Inspector& inspector, ContentTree* tree = nullptr,
                    const IconAtlas* icons = nullptr, audio::AudioSystem* audio = nullptr,
                    EditorCommands* commands = nullptr, Editor* editor = nullptr)
{
    // **The whole selection, not the primary.** A grid pointed at one instance
    // while three are highlighted is the editor disagreeing with itself, and it
    // is how somebody ends up moving one part and believing they moved three.
    const std::span<const core::InstanceId> targets = inspector.selectionSet();

    core::usize live = 0;
    for (const core::InstanceId id : targets)
        live += world.alive(id) ? 1u : 0u;

    if (live == 0) {
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.properties.select_an_object_in_the")));
        return;
    }

    // Said out loud, because a grid that looks identical for one instance and
    // for forty is one somebody edits forty instances with by accident.
    if (live > 1)
        ImGui::TextUnformatted(
            core::tr(ENG_TR("engine.editor.properties.selected_many"), {{"count", static_cast<core::i64>(live)}})
                .c_str());

    // The same sentence the Explorer row gives on hover, where the numbers are
    // being edited: a person tuning a blur that is under the wrong parent is
    // the person who most needs to be told.
    if (live == 1) {
        for (const core::InstanceId id : targets) {
            if (!world.alive(id))
                continue;
            if (const std::string inactive = lookInactiveReason(world, id); !inactive.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.3f, 1.0f));
                ImGui::TextWrapped("%s", inactive.c_str());
                ImGui::PopStyleColor();
            }
            // **`Lighting`'s linear fog, while an `Atmosphere` replaces it**
            // (ADR 0096): the three properties are kept -- remove the air and
            // they apply again -- and not used, which a person tuning
            // `FogEnd` and watching nothing happen needs to be told.
            if (world.lighting().find(id) != nullptr) {
                for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
                    if (world.atmospheres().find(child) == nullptr || world.destroyed(child))
                        continue;
                    const std::string note = core::engineCatalog().format(ENG_TR("engine.overlay.look.fog_replaced"));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.3f, 1.0f));
                    ImGui::TextWrapped("%s", note.c_str());
                    ImGui::PopStyleColor();
                    break;
                }
            }
        }
    }

    // **Which of these properties are this instance's own** (S5.6). Asked once
    // for the whole grid rather than once per row, and answered from a cache the
    // editor drops whenever anything changes -- the question reads the stamp
    // FILE, and a panel that asked it per row per frame would be D118 again.
    //
    // Only for a single selection. With several selected the answer differs per
    // instance, and one mark that meant "some of these" would be worse than no
    // mark: a person would revert what they could not see.
    //
    // Cached against the selection and a frame budget, in that order: a new
    // selection is answered at once, and a selection somebody is sitting on is
    // re-asked about four times a second so an edit made by a gizmo drag or a
    // script -- neither of which the editor hears about -- shows up without
    // anybody clicking away and back.
    static core::InstanceId s_overridesFor;
    static std::vector<core::NameAtom> s_overrides;
    static int s_overridesAge = 0;

    std::span<const core::NameAtom> overridden;
    if (editor != nullptr && live == 1 && targets.size() == 1) {
        constexpr int kRefreshFrames = 15;
        if (targets.front() != s_overridesFor || s_overridesAge >= kRefreshFrames) {
            s_overrides = editor->overridesOf(world, targets.front());
            s_overridesFor = targets.front();
            s_overridesAge = 0;
        }
        else {
            ++s_overridesAge;
        }
        overridden = s_overrides;
    }
    else {
        s_overridesFor = core::InstanceId{};
    }

    // One loop over the descriptor tables. There is no switch on a class name
    // anywhere below this line, which is Decision 16's whole claim.
    collectCommonProperties(world, targets, g_properties);

    // **A search box, because the grid grew** (S5.14). A `Part` declares
    // twenty-odd properties across four classes and a `TextLabel` more than
    // that; "which row is `CanCollide` again" is a scroll, and it is the single
    // most common thing anybody does in a properties panel. Every word typed
    // must appear somewhere in the name or its heading (`propertyMatches`).
    static std::array<char, 64> filter{};
    ImGui::SetNextItemWidth(-FLT_MIN);
    searchField(icons, "##property-filter", core::tr(ENG_TR("engine.editor.properties.filter")), filter.data(),
                filter.size());
    const std::string_view needle(filter.data());

    if (g_properties.empty()) {
        // Two classes with nothing in common is a legal selection and an empty
        // grid is the honest answer -- but an empty panel with no sentence in
        // it reads as a bug in the panel.
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.properties.these_classes_share_no_properties")));
        return;
    }

    // **Rows grouped by the task they serve** (`propertyCategory`), the
    // headings and the rows under each in alphabetical order. A heading was the class that declared the row, which put
    // a part's colour, size and collision under three headings nobody reads.
    struct PropertyRow
    {
        const scene::PropertyDesc* descriptor = nullptr;
        PropertyCategory category;
    };
    static std::vector<PropertyRow> s_rows;
    s_rows.clear();
    for (const scene::PropertyDesc* descriptor : g_properties) {
        const std::string_view name = world.atoms().text(descriptor->name);
        // **A script's code is not a row** (the owner: "the Properties is too
        // cluttered"): the reference editor leaves `Source` out of its grid,
        // because the code is read and written in the script editor that a
        // double-click opens, and a one-line preview of it only pushed the
        // rows that matter further down.
        if (editorFor(*descriptor) == EditorKind::Code)
            continue;
        if (propertyMatches(name, needle))
            s_rows.push_back(PropertyRow{descriptor, propertyCategory(name)});
    }
    // **A to Z: the headings, and the rows under each** (the owner, 2026-09-27):
    // a grid somebody scans for a name is one sorted by name. Case folded, so
    // `CFrame` sits between `CanCollide` and `CastShadow` rather than before
    // every lower-case name.
    const auto folded = [](std::string_view a, std::string_view b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    };
    std::stable_sort(s_rows.begin(), s_rows.end(), [&](const PropertyRow& a, const PropertyRow& b) {
        if (a.category.name != b.category.name)
            return folded(a.category.name, b.category.name);
        return folded(world.atoms().text(a.descriptor->name), world.atoms().text(b.descriptor->name));
    });
    if (s_rows.empty()) {
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.properties.no_property_matches"),
                                           {{"filter", std::string_view(filter.data())}})
                                      .c_str());
        return;
    }

    // A grid: one divider between the columns that drags, quiet alternate rows,
    // and no box around every cell -- the lines that say where a value starts
    // and nothing more.
    //
    // **Always two columns, however narrow the panel** (the owner, with a
    // screenshot: "too cluttered, hard to find anything"). A narrow panel used
    // to stack each name over its value, which doubled every row's height and
    // made names and values look alike; a name that does not fit is clipped
    // and says itself in full under the pointer, which is what a property grid
    // in every editor does.
    const ImGuiTableFlags gridFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x * 1.5f, 2.0f));
    if (ImGui::BeginTable("properties-grid", 2, gridFlags)) {
        ImGui::TableSetupColumn("##property", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

        std::string_view heading;
        bool headingOpen = true;
        for (const PropertyRow& row : s_rows) {
            const scene::PropertyDesc* descriptor = row.descriptor;
            const std::string_view propertyName = world.atoms().text(descriptor->name);

            // **The heading, as a row of its own across both columns**, on the
            // raised surface so it reads as a heading and not as a property.
            // Collapsing one is remembered per heading; a filter opens every
            // heading it matched in, because a match hidden under a closed
            // heading is a filter that found nothing.
            if (row.category.name != heading) {
                // A little air above every heading but the first, so a group
                // ends before the next one starts.
                if (!heading.empty()) {
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetStyle().ItemSpacing.y * 1.5f);
                    ImGui::TableSetColumnIndex(0);
                }
                heading = row.category.name;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, propertyHeadingBand());
                // The heading's words are the catalog's; what the table calls it
                // -- the name the tests and the fold's memory know -- is the id.
                const std::string headingText =
                    std::string(categoryWords(heading)) + "###" + std::string(heading) + "-heading";
                const char* const headingLabel = headingText.c_str();
                ImGui::PushStyleColor(ImGuiCol_Text, propertyHeadingInk());
                const bool expanded = ImGui::TreeNodeEx(
                    headingLabel, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                      ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns);
                ImGui::PopStyleColor();
                headingOpen = expanded || !needle.empty();
            }
            if (!headingOpen)
                continue;

            ImGui::PushID(static_cast<int>(descriptor->name.id));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // In from the heading's arrow, and level with the widget beside it.
            ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.5f);
            ImGui::AlignTextToFramePadding();

            const EditorKind kind = editorFor(*descriptor);

            // **A composite's name is a disclosure**, open by default: the rows
            // under it are where its numbers are edited, and starting closed
            // would put an extra click between a person and the one property
            // they opened the panel for. Collapsing it is theirs to do, and
            // ImGui remembers it per row.
            char nameLabel[128];
            (void)std::snprintf(nameLabel, sizeof(nameLabel), "%.*s", static_cast<int>(propertyName.size()),
                                propertyName.data());
            bool expanded = false;
            if (compositeKind(kind)) {
                expanded =
                    ImGui::TreeNodeEx(nameLabel, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_SpanAvailWidth);
            }
            else {
                ImGui::TextUnformatted(nameLabel);
            }

            // **The mark, and the two things a person can do about it** (S5.6).
            // A dot rather than a colour on the label: the label is already
            // coloured by state elsewhere, and a second meaning on one channel
            // is how a panel becomes unreadable. The tooltip says what the dot
            // means, because a dot on its own is a puzzle.
            const bool isOverride =
                std::find(overridden.begin(), overridden.end(), descriptor->name) != overridden.end();
            if (isOverride) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "*");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.properties.this_instance_s_own_value_tip")));
            }
            // Offered on the row whether or not it is overridden, so the menu is
            // in one place; the items themselves are disabled when there is
            // nothing to do, which says WHY rather than hiding the answer.
            if (commands != nullptr && targets.size() == 1 && ImGui::BeginPopupContextItem("override")) {
                if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.properties.revert_to_stamp")), nullptr, false,
                                    isOverride)) {
                    commands->overrideSubject = targets.front();
                    commands->overrideProperty = descriptor->name;
                    commands->overrideApply = false;
                }
                if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.properties.apply_to_stamp")), nullptr, false,
                                    isOverride)) {
                    commands->overrideSubject = targets.front();
                    commands->overrideProperty = descriptor->name;
                    commands->overrideApply = true;
                }
                ImGui::EndPopup();
            }

            // The IDL's own prose for this property, which now rides on the
            // descriptor rather than staying in a file nothing at runtime reads
            // (`class_registry.h` says why it is prose and not a catalog key).
            // Wrapped, because these are paragraphs and an unwrapped tooltip is
            // one line as wide as the sentence.
            //
            // **The name in full comes first**, since a narrow column clips it;
            // then what the tag used to say beside the name -- read-only, or
            // stored and not yet acted on -- which cost every such row a word
            // of its width and is a fact somebody hovering asks about. A
            // read-only value is already greyed, which is the part a glance
            // needs.
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                ImGui::TextUnformatted(nameLabel);
                if (const char* tag = propertyTag(*descriptor); tag != nullptr) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", core::tr(descriptor->readOnly
                                                           ? ENG_TR("engine.editor.properties.read_only")
                                                           : ENG_TR("engine.editor.properties.stored_not_acted_on")));
                }
                // A value the running game sets, which a scene does not keep:
                // said, so an edit here is not mistaken for one that saves.
                if (descriptor->transient && !descriptor->readOnly)
                    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.properties.not_saved_with_the_scene")));
                if (descriptor->doc[0] != 0) {
                    ImGui::Separator();
                    ImGui::TextUnformatted(descriptor->doc);
                }
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            // Read once and handed to the editor. Asking twice would be two
            // sweeps of the selection per row per frame, and the tag and the
            // widget could then disagree.
            const SharedValue shared = sharedValue(world, targets, descriptor->name);
            if (shared.state == SharedState::Mixed) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.properties.mixed")));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "%s", core::tr(ENG_TR("engine.editor.properties.the_selected_instances_hold_different_tip")));
            }

            ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.5f);
            ImGui::TableSetColumnIndex(1);
            drawEditor(world, root, inspector, targets, *descriptor, shared, tree, icons, audio, commands);

            // The rows a composite is actually edited in. Disabled with the
            // same rule the widget above uses, because they are the same
            // property: a read-only `UDim` must not offer four live drags.
            if (expanded && shared.state != SharedState::Unreadable) {
                const bool locked = !editable(*descriptor);
                if (locked)
                    ImGui::BeginDisabled();
                scene::Value edited;
                if (drawCompositeRows(kind, shared, edited)) {
                    for (const core::InstanceId target : targets) {
                        if (world.alive(target))
                            inspector.enqueue(target, descriptor->name, edited);
                    }
                }
                if (locked)
                    ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    // **Below the grid, and collapsed by default.** A class's properties are the
    // answer to "what is this"; attributes and tags are the answer to "what did
    // somebody decide about it", which is a question asked far less often and by
    // somebody who came looking.
    //
    // **Their headers are the grid's headings**, not ImGui's filled accent bar:
    // two kinds of heading in one panel read as two different panels.
    if (const core::InstanceId primary = inspector.selection(); world.alive(primary)) {
        ImGui::Spacing();
        const ImU32 headingBg = propertyHeadingBand();
        ImGui::PushStyleColor(ImGuiCol_Header, headingBg);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, headingBg);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        drawAttributes(world, inspector, primary, targets);
        drawTags(world, inspector, primary, targets);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }

    // **A drag in this panel is ONE edit**, and this is what tells the undo
    // stack so. ImGui keeps exactly one item active at a time, so "something in
    // this window is being held" is the whole of the question -- which is why
    // this is four lines here rather than a pair of calls repeated through
    // twelve widget branches, three of which draw more than one widget and
    // would each have got it subtly wrong.
    //
    // It matters more over a selection than it ever did over one instance: a
    // drag that writes to forty parts enqueues forty writes a frame, and
    // without a gesture every one of those frames is a world snapshot.
    //
    // The panel closes only the gesture it opened. The manipulators open their
    // own, and a properties panel that ended somebody else's drag because
    // nothing in it was focused would be worse than not tracking one at all.
    const bool holding = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsAnyItemActive();
    if (holding && g_propertyGesture == 0) {
        g_propertyGesture = inspector.beginGesture();
    }
    else if (!holding && g_propertyGesture != 0) {
        if (inspector.gesture() == g_propertyGesture)
            inspector.endGesture();
        g_propertyGesture = 0;
    }
}

// The one frame of latency Decision 15 buys determinism with, said out loud,
// plus what the last few writes actually did. A refusal that is not reported is
// a value that snaps back with no explanation.
// A write the world refused, said out loud once.
//
// **Only the tail is looked at**, because `outcomes()` is a rolling history and
// not a per-frame list: re-reporting the whole of it every frame would pin the
// toast open forever on a refusal from a minute ago.
void reportRefusedWrites(const scene::World& world, const Inspector& inspector, Editor* editor)
{
    static core::usize seen = 0;
    const std::span<const WriteOutcome> outcomes = inspector.outcomes();
    // The list is bounded and drops the oldest, so it shrinks as well as grows;
    // a count that outran it would skip every later refusal in silence.
    if (seen > outcomes.size())
        seen = 0;

    for (core::usize index = seen; index < outcomes.size(); ++index) {
        const WriteOutcome& outcome = outcomes[index];
        if (outcome.result == scene::World::SetResult::Changed ||
            outcome.result == scene::World::SetResult::Unchanged) {
            continue;
        }
        if (editor == nullptr)
            continue;
        const std::string_view name = world.atoms().text(outcome.property);
        editor->report(std::string(name) + ": " + setResultLabel(outcome.result), true);
    }
    seen = outcomes.size();
}

void drawWriteLog(scene::World& world, const Inspector& inspector)
{
    if (inspector.pendingCount() > 0)
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.properties.writes_queued"),
                                        {{"count", static_cast<core::i64>(inspector.pendingCount())}})
                                   .c_str());

    const std::span<const WriteOutcome> outcomes = inspector.outcomes();
    if (outcomes.empty())
        return;

    if (!ImGui::CollapsingHeader(core::tr(ENG_TR("engine.editor.write_log.recent_writes"))))
        return;

    for (const WriteOutcome& outcome : outcomes) {
        const std::string_view propertyName = world.atoms().text(outcome.property);
        ImGui::Text("%.*s: %s", static_cast<int>(propertyName.size()), propertyName.data(),
                    setResultLabel(outcome.result));
    }
}

// --- The console and the memory table (D017) ---------------------------------
//
// `architecture.md` §app names five panes for the DebugShell -- explorer,
// properties, profiler with the memcat table, log/REPL, streaming map, physics
// wireframe -- and two of them had never been written. The audit that found that
// is what D017 is; this is the pane.

// The last few hundred log lines, and the sink that fills them.
//
// Bounded and dropping the oldest, because a shell that grew with the log would
// be a memory leak with a scrollbar. Process-global like the sink it installs:
// `core::setLogSink` takes one function and there is one console.
struct ConsoleLog
{
    static constexpr core::usize kMaxLines = 400;

    struct Line
    {
        core::LogLevel level = core::LogLevel::Info;
        std::string text;
        // Counts up for as long as the process runs, so a selection names the
        // lines it spans and survives the oldest ones dropping off the front.
        core::u64 seq = 0;
        // A printed table's rows (`print_tree.h`), folded under the line.
        std::string tree;
        // The script and line that printed it, when a script did.
        std::optional<SourceLocation> source;
        // **The same message from the same place, again and again, is one line
        // with a count** (the owner): a print in a loop is not four hundred
        // lines that push everything else out.
        core::u32 repeats = 1;
    };

    std::mutex mutex;
    std::deque<Line> lines;
    core::u64 nextSeq = 1;
    bool installed = false;
    // The sink that was there first. Chained rather than replaced, so the
    // console pane and the log FILE both get every line -- a shell that ate the
    // log would be the last place anybody looked for it.
    core::LogSink previous;
};

ConsoleLog& console()
{
    static ConsoleLog instance;
    return instance;
}

void drawMemory(script::ScriptRuntime& runtime)
{
    const std::vector<script::ScriptRuntime::MemoryCategory> rows = runtime.memoryByCategory();
    if (!propertiesSection(core::tr(ENG_TR("engine.editor.memory.script_memory"))) ||
        !beginSectionGrid("script-memory"))
        return;

    core::usize total = 0;
    for (const auto& row : rows)
        total += row.bytes;
    statRow(core::tr(ENG_TR("engine.editor.memory.heap")), "%s",
            core::tr(ENG_TR("engine.editor.memory.across_categories"),
                     {{"size", fixed(static_cast<double>(total) / 1024.0, 1)},
                      {"count", static_cast<core::i64>(rows.size())}})
                .c_str());
    for (const auto& row : rows) {
        // A category with no name is one the pool assigned and whose script has
        // since been replaced -- worth showing as a number rather than hiding,
        // because that is exactly the leak the table is for.
        const std::string name =
            row.name.empty() ? "(recycled) #" + std::to_string(row.category) : std::string(row.name);
        statRow(name.c_str(), "%s",
                core::tr(ENG_TR("engine.editor.unit.kilobytes"),
                         {{"size", fixed(static_cast<double>(row.bytes) / 1024.0, 1)}})
                    .c_str());
    }
    endSectionGrid();
}

// Whether `line` matches what is typed in the filter, case-insensitively.
//
// Case-insensitive because a person hunting a warning types what they remember
// and not what was printed, and a filter that misses `Chunk` for `chunk` is a
// filter that reads as broken.
[[nodiscard]] bool consoleMatches(std::string_view line, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > line.size())
        return false;
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (std::string_view::size_type at = 0; at + needle.size() <= line.size(); ++at) {
        std::string_view::size_type i = 0;
        while (i < needle.size() && lower(line[at + i]) == lower(needle[i]))
            ++i;
        if (i == needle.size())
            return true;
    }
    return false;
}

// **Text in the console can be selected and copied** -- the owner: "I should be
// able to select its text". Across lines, by dragging, shift-clicking or a
// double click for a whole line, and copied with ctrl+C or the right-click menu.
// Held by line NUMBER (`ConsoleLog::Line::seq`) rather than by index, because
// the index of every line changes whenever the oldest one drops off the front.
struct ConsoleSelection
{
    core::u64 anchorSeq = 0;
    core::usize anchorOffset = 0;
    core::u64 headSeq = 0;
    core::usize headOffset = 0;
    bool dragging = false;
    // The drag reached another place, so its release is not a click on a link.
    bool moved = false;
    // The link a press landed on, followed on a release that did not move.
    std::optional<SourceLocation> pressedLink;
};

// Which printed tables are open: a line's number, then the index of each row
// opened under it. **Closed until opened** (the owner): a table printed every
// frame must not unfold into a wall.
std::unordered_set<std::string> g_consoleOpen;

bool toggleConsoleFold(const std::string& key)
{
    if (g_consoleOpen.erase(key) > 0)
        return false;
    g_consoleOpen.insert(key);
    return true;
}

// The rows of a printed table under its line, from `print_tree.h`'s form: one
// row a field, `depth 0x1F key 0x1F value`, a table's fields right after it.
// Each table row folds, closed until opened.
void drawConsoleTree(std::string_view tree, const std::string& lineKey, ImVec2 origin, float lineHeight, float rowStep)
{
    struct TreeRow
    {
        int depth = 0;
        std::string_view key;
        std::string_view value;
    };
    std::vector<TreeRow> parsed;
    for (std::size_t at = 0; at < tree.size();) {
        std::size_t end = tree.find('\n', at);
        if (end == std::string_view::npos)
            end = tree.size();
        const std::string_view row = tree.substr(at, end - at);
        at = end + 1;
        const std::size_t first = row.find('\x1F');
        const std::size_t second = first == std::string_view::npos ? first : row.find('\x1F', first + 1);
        if (second == std::string_view::npos)
            continue;
        int depth = 0;
        for (const char digit : row.substr(0, first))
            depth = digit >= '0' && digit <= '9' ? depth * 10 + (digit - '0') : depth;
        parsed.push_back(TreeRow{depth, row.substr(first + 1, second - first - 1), row.substr(second + 1)});
    }

    const ThemePalette& p = palette();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float indent = lineHeight;
    ImVec2 cursor = origin;
    // Rows deeper than this are under a closed table.
    int hiddenBelow = std::numeric_limits<int>::max();
    for (std::size_t index = 0; index < parsed.size(); ++index) {
        const TreeRow& row = parsed[index];
        if (row.depth > hiddenBelow)
            continue;
        hiddenBelow = std::numeric_limits<int>::max();
        const bool parent = index + 1 < parsed.size() && parsed[index + 1].depth > row.depth;
        const std::string key = lineKey + "/" + std::to_string(index);
        bool open = parent && g_consoleOpen.contains(key);
        const float x = cursor.x + static_cast<float>(row.depth) * indent;
        if (parent) {
            ImGui::SetCursorScreenPos(ImVec2(x, cursor.y));
            ImGui::PushID(key.c_str());
            if (ImGui::InvisibleButton("##fold", ImVec2(lineHeight, lineHeight)))
                open = toggleConsoleFold(key);
            ImGui::PopID();
            ImGui::RenderArrow(draw, ImVec2(x + lineHeight * 0.2f, cursor.y + lineHeight * 0.15f),
                               ImGui::GetColorU32(ImGuiCol_TextDisabled), open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
        }
        const float textX = x + lineHeight;
        const ImU32 keyInk = ImGui::ColorConvertFloat4ToU32(themeColor(p.accent));
        draw->AddText(ImVec2(textX, cursor.y), keyInk, row.key.data(), row.key.data() + row.key.size());
        const float keyWidth = ImGui::CalcTextSize(row.key.data(), row.key.data() + row.key.size()).x;
        constexpr std::string_view Equals = " = ";
        draw->AddText(ImVec2(textX + keyWidth, cursor.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), Equals.data(),
                      Equals.data() + Equals.size());
        const float valueX = textX + keyWidth + ImGui::CalcTextSize(Equals.data(), Equals.data() + Equals.size()).x;
        draw->AddText(ImVec2(valueX, cursor.y), ImGui::GetColorU32(ImGuiCol_Text), row.value.data(),
                      row.value.data() + row.value.size());
        // Its width, so the log's horizontal scroll reaches the end of it.
        ImGui::SetCursorScreenPos(ImVec2(x, cursor.y));
        ImGui::Dummy(ImVec2(valueX - x + ImGui::CalcTextSize(row.value.data(), row.value.data() + row.value.size()).x,
                            lineHeight));
        cursor.y += rowStep;
        if (parent && !open)
            hiddenBelow = row.depth;
    }
}

// What was typed at the REPL, oldest first -- see `consoleHistoryStep`.
struct ConsoleHistory
{
    static constexpr core::usize kMaxEntries = 100;
    std::vector<std::string> entries;
    core::i32 at = -1;
    // The line being typed when the walk started, given back at its end.
    std::string draft;
};

void drawConsole(script::ScriptRuntime* runtime, ScriptEditorCommands* scriptCommands = nullptr,
                 const IconAtlas* icons = nullptr, bool docked = false)
{
    ConsoleLog& log = console();

    static std::array<char, 128> filter{};
    static ConsoleSelection selection;
    // **Which levels show**, as the problems and output views of the editor
    // this follows filter theirs: a toggle per level with its count on it, so
    // "are there any errors" is answered without reading.
    static bool showErrors = true;
    static bool showWarnings = true;
    static bool showInfo = true;
    int errors = 0;
    int warnings = 0;
    int infos = 0;
    {
        std::lock_guard<std::mutex> lock(log.mutex);
        for (const ConsoleLog::Line& line : log.lines) {
            errors += line.level == core::LogLevel::Error ? 1 : 0;
            warnings += line.level == core::LogLevel::Warn ? 1 : 0;
            infos += line.level != core::LogLevel::Error && line.level != core::LogLevel::Warn ? 1 : 0;
        }
    }
    const auto levelToggle = [](const char* id, const char* word, int count, bool& on, core::Color3 tint) {
        char label[64];
        (void)std::snprintf(label, sizeof(label), "%d %s%s", count, word, id);
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor(tint));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        if (ImGui::Button(label))
            on = !on;
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", core::tr(on ? ENG_TR("engine.editor.console.showing_tip")
                                                : ENG_TR("engine.editor.console.hidden_tip")));
        ImGui::SameLine();
    };
    const ThemePalette& tones = palette();
    // In the level's colour only when there is one of it: a red "0 errors" is
    // an alarm about nothing.
    levelToggle("##errors", errors == 1 ? "error" : "errors", errors, showErrors,
                errors > 0 ? tones.danger : tones.textMuted);
    levelToggle("##warnings", warnings == 1 ? "warning" : "warnings", warnings, showWarnings,
                warnings > 0 ? tones.warning : tones.textMuted);
    levelToggle("##info", "info", infos, showInfo, tones.text);

    // The search takes the room; the actions sit at the right end, icons
    // only, as the editor this follows puts a panel's actions in its title.
    const float action = ImGui::GetFrameHeight();
    const float actions = action * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::SetNextItemWidth(-actions);
    searchField(icons, "##filter", core::tr(ENG_TR("engine.editor.console.filter")), filter.data(), filter.size());
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        filter.fill(0);
        g_escapeTaken = true;
    }
    ImGui::SameLine();
    const bool copyAll = iconButton(icons, icons::ActionCopy, ImGui::GetFontSize(), "copy-all",
                                    core::tr(ENG_TR("engine.editor.console.copy")),
                                    core::tr(ENG_TR("engine.editor.console.copy_every_line_shown_here_tip")));
    ImGui::SameLine();
    bool cleared = iconButton(icons, icons::ActionDelete, ImGui::GetFontSize(), "clear",
                              core::tr(ENG_TR("engine.editor.console.clear")),
                              core::tr(ENG_TR("engine.editor.console.clear_the_console_tip")));

    const std::string_view needle{filter.data()};
    core::usize shown = 0;

    // **The log is the part that grows.** One REPL line and its spacing sit
    // under it and everything else the panel has is the log's -- see
    // `consoleLogHeight`, which is where the arithmetic is asserted.
    //
    // **Docked, the panel is as tall as its dock**, and a fixed 160-pixel floor
    // pushed the input line out of a short one -- reported as the console's
    // layout leaving its window. There the floor is three lines of text; the
    // fixed height stays for the F3 overlay, whose console sits at the end of a
    // scrolling window with nothing to fill.
    const f32 logFloor = docked ? ImGui::GetTextLineHeightWithSpacing() * 3.0f : kConsoleLogMinHeight;
    const f32 logHeight =
        consoleLogHeight(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeightWithSpacing(), logFloor);

    // No box around the log: the panel is the box, and a frame inside it is
    // the "box inside a box" the survey found.
    if (ImGui::BeginChild("log", ImVec2(0.0f, logHeight), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        std::lock_guard<std::mutex> lock(log.mutex);
        // **Cleared under the same lock the sink writes under.** A clear that
        // raced a line from another thread would drop one that arrived after the
        // button and before the erase, which is the one kind of missing message
        // nobody would ever explain.
        if (cleared) {
            log.lines.clear();
            selection = ConsoleSelection{};
        }

        // Where each shown line was drawn, for the pointer to find it.
        struct Row
        {
            core::u64 seq = 0;
            std::string_view text;
            ImVec2 at;
            bool link = false;
        };
        std::vector<Row> rows;
        rows.reserve(log.lines.size());
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float lineHeight = ImGui::GetTextLineHeight();
        const float rowStep = ImGui::GetTextLineHeightWithSpacing();
        const auto widthOf = [](std::string_view text, core::usize bytes) {
            return ImGui::CalcTextSize(text.data(), text.data() + std::min(bytes, text.size())).x;
        };
        // The selection's ends in document order.
        const bool anchorFirst =
            selection.anchorSeq < selection.headSeq ||
            (selection.anchorSeq == selection.headSeq && selection.anchorOffset <= selection.headOffset);
        const core::u64 firstSeq = anchorFirst ? selection.anchorSeq : selection.headSeq;
        const core::usize firstOffset = anchorFirst ? selection.anchorOffset : selection.headOffset;
        const core::u64 lastSeq = anchorFirst ? selection.headSeq : selection.anchorSeq;
        const core::usize lastOffset = anchorFirst ? selection.headOffset : selection.anchorOffset;
        const bool selecting = firstSeq != 0 && (firstSeq != lastSeq || firstOffset != lastOffset);

        for (const ConsoleLog::Line& line : log.lines) {
            if (!consoleMatches(line.text, needle))
                continue;
            const bool levelShown = line.level == core::LogLevel::Error  ? showErrors
                                    : line.level == core::LogLevel::Warn ? showWarnings
                                                                         : showInfo;
            if (!levelShown)
                continue;
            ++shown;
            const ThemePalette& p = palette();
            const ImVec4 colour = themeColor(line.level == core::LogLevel::Error   ? p.danger
                                             : line.level == core::LogLevel::Warn  ? p.warning
                                             : line.level == core::LogLevel::Debug ? p.textMuted
                                                                                   : p.text);
            // A line with a table under it has its fold arrow in front, and its
            // text starts after it -- which is where the selection measures from.
            const ImVec2 lineStart = ImGui::GetCursorScreenPos();
            const bool folds = !line.tree.empty();
            const ImVec2 at(lineStart.x + (folds ? lineHeight : 0.0f), lineStart.y);
            rows.push_back(Row{line.seq, line.text, at, false});
            const std::string foldKey = std::to_string(line.seq);
            bool unfolded = false;
            if (folds) {
                unfolded = g_consoleOpen.contains(foldKey);
                ImGui::PushID(foldKey.c_str());
                if (ImGui::InvisibleButton("##fold", ImVec2(lineHeight, lineHeight)))
                    unfolded = toggleConsoleFold(foldKey);
                ImGui::PopID();
                ImGui::RenderArrow(draw, ImVec2(lineStart.x + lineHeight * 0.2f, lineStart.y + lineHeight * 0.15f),
                                   ImGui::ColorConvertFloat4ToU32(colour), unfolded ? ImGuiDir_Down : ImGuiDir_Right,
                                   0.7f);
                ImGui::SetCursorScreenPos(at);
            }

            if (selecting && line.seq >= firstSeq && line.seq <= lastSeq) {
                const core::usize from = line.seq == firstSeq ? firstOffset : 0;
                const core::usize to = line.seq == lastSeq ? lastOffset : line.text.size();
                // A line the selection runs past ends in a sliver, as an editor
                // shows the newline it takes with it.
                const float tail = line.seq != lastSeq ? ImGui::CalcTextSize(" ").x : 0.0f;
                draw->AddRectFilled(ImVec2(at.x + widthOf(line.text, from), at.y),
                                    ImVec2(at.x + widthOf(line.text, to) + tail, at.y + lineHeight),
                                    ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
            }

            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
            const float textEnd = ImGui::GetItemRectMax().x;
            // The link below reads the text's own rect, so the count and the
            // source are drawn after it has.
            const auto afterText = [&, textEnd, lineStart]() {
                float x = textEnd + ImGui::GetStyle().ItemSpacing.x;
                if (line.repeats > 1) {
                    char count[24];
                    (void)std::snprintf(count, sizeof(count), "x%u", static_cast<unsigned>(line.repeats));
                    const ImVec2 size = ImGui::CalcTextSize(count);
                    const float pad = ImGui::GetStyle().FramePadding.x * 0.5f;
                    draw->AddRectFilled(ImVec2(x, lineStart.y),
                                        ImVec2(x + size.x + pad * 2.0f, lineStart.y + lineHeight),
                                        ImGui::GetColorU32(ImGuiCol_FrameBg), lineHeight * 0.3f);
                    draw->AddText(ImVec2(x + pad, lineStart.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), count);
                    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.console.repeats"),
                                                         {{"count", static_cast<core::i64>(line.repeats)}})
                                                    .c_str());
                    x += size.x + pad * 2.0f + ImGui::GetStyle().ItemSpacing.x;
                }
                // **Where it was printed, at the right edge**, and a click opens
                // it -- the question after "what did it say" is "who said it".
                if (line.source.has_value()) {
                    const std::string where = line.source->chunk + ":" + std::to_string(line.source->line);
                    const ImVec2 size = ImGui::CalcTextSize(where.c_str());
                    const float right = ImGui::GetCurrentWindow()->InnerRect.Max.x + ImGui::GetScrollX() -
                                        ImGui::GetStyle().ItemSpacing.x - size.x;
                    const float left = std::max(x, right);
                    ImGui::SetCursorScreenPos(ImVec2(left, lineStart.y));
                    ImGui::PushID(foldKey.c_str());
                    if (ImGui::InvisibleButton("##source", size) && scriptCommands != nullptr)
                        scriptCommands->jumpTo = line.source;
                    ImGui::PopID();
                    const bool over = ImGui::IsItemHovered();
                    draw->AddText(ImVec2(left, lineStart.y),
                                  ImGui::GetColorU32(over ? ImGuiCol_Text : ImGuiCol_TextDisabled), where.c_str());
                    if (over) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.console.open_at_line"),
                                                         {{"chunk", line.source->chunk},
                                                          {"line", static_cast<core::i64>(line.source->line)}})
                                                    .c_str());
                    }
                }
                // The cursor is on the next line already: the text, or the
                // source after it, put it there.
                if (unfolded)
                    drawConsoleTree(line.tree, foldKey, ImVec2(lineStart.x + lineHeight, lineStart.y + rowStep),
                                    lineHeight, rowStep);
            };

            // **A line that names a source location is a link** (S5.11). An
            // error in the console names a file and a line and nothing takes you
            // to it, which is the difference between a console and a log file.
            //
            // Only the lines that name one: turning ordinary output into
            // something that looks clickable and goes nowhere is worse than not
            // linking any of it. A click that does not drag follows it; a drag
            // across it selects, like anywhere else.
            if (const std::optional<SourceLocation> link = parseSourceLocation(line.text); link.has_value()) {
                rows.back().link = true;
                if (ImGui::IsItemHovered() && !selection.dragging) {
                    const ImVec2 min = ImGui::GetItemRectMin();
                    const ImVec2 max = ImGui::GetItemRectMax();
                    draw->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), ImGui::ColorConvertFloat4ToU32(colour));
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s",
                                      core::tr(ENG_TR("engine.editor.console.chunk_line"),
                                               {{"chunk", link->chunk}, {"line", static_cast<core::i64>(link->line)}})
                                          .c_str());
                }
            }
            afterText();
        }
        // **A filter that hides everything says so.** An empty pane and a pane
        // filtered down to nothing look identical, and one of them means the
        // engine has said nothing while the other means you are not looking at
        // what it said.
        if (shown == 0 && !log.lines.empty())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.console.hidden_by_filter"),
                                               {{"count", static_cast<core::i64>(log.lines.size())}})
                                          .c_str());

        using Place = std::pair<core::usize, core::usize>;
        // The place under the pointer: the nearest row by height, and the
        // nearest character boundary along it.
        const auto placeAt = [&](ImVec2 mouse) -> Place {
            if (mouse.y < rows.front().at.y)
                return {0, 0};
            if (mouse.y >= rows.back().at.y + rowStep)
                return {rows.size() - 1, rows.back().text.size()};
            core::usize row = 0;
            while (row + 1 < rows.size() && mouse.y >= rows[row].at.y + rowStep)
                ++row;
            const std::string_view text = rows[row].text;
            core::usize best = 0;
            float before = 0.0f;
            for (core::usize at = 1; at <= text.size(); ++at) {
                // Only between whole UTF-8 characters.
                if (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0u) == 0x80u)
                    continue;
                const float width = widthOf(text, at);
                if (rows[row].at.x + (before + width) * 0.5f > mouse.x)
                    break;
                best = at;
                before = width;
            }
            return {row, best};
        };
        // A selection end's row among those shown -- the first one after it
        // when its own line is filtered out or has dropped off the front.
        const auto rowOf = [&](core::u64 seq, core::usize offset) -> Place {
            const auto found = std::lower_bound(rows.begin(), rows.end(), seq,
                                                [](const Row& row, core::u64 value) { return row.seq < value; });
            if (found == rows.end())
                return {rows.size() - 1, rows.back().text.size()};
            return {static_cast<core::usize>(found - rows.begin()), found->seq == seq ? offset : 0};
        };
        const auto textOf = [&](Place from, Place to) {
            std::vector<std::string_view> texts;
            texts.reserve(rows.size());
            for (const Row& row : rows)
                texts.push_back(row.text);
            return consoleSelectionText(texts, from.first, from.second, to.first, to.second);
        };
        const auto selectAll = [&]() {
            selection.anchorSeq = rows.front().seq;
            selection.anchorOffset = 0;
            selection.headSeq = rows.back().seq;
            selection.headOffset = rows.back().text.size();
        };
        const auto copyEverything = [&]() {
            ImGui::SetClipboardText(textOf({0, 0}, {rows.size() - 1, rows.back().text.size()}).c_str());
        };

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        // Not over the scrollbars, whose drag is theirs.
        const bool overText = ImGui::IsWindowHovered() && ImGui::GetCurrentWindow()->InnerRect.Contains(mouse);
        if (!rows.empty()) {
            if (overText && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const auto [row, offset] = placeAt(mouse);
                selection.headSeq = rows[row].seq;
                selection.headOffset = offset;
                if (!ImGui::GetIO().KeyShift || selection.anchorSeq == 0) {
                    selection.anchorSeq = rows[row].seq;
                    selection.anchorOffset = offset;
                }
                selection.dragging = true;
                selection.moved = ImGui::GetIO().KeyShift;
                selection.pressedLink.reset();
                if (rows[row].link && mouse.x < rows[row].at.x + widthOf(rows[row].text, rows[row].text.size()))
                    selection.pressedLink = parseSourceLocation(rows[row].text);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    selection.anchorOffset = 0;
                    selection.headOffset = rows[row].text.size();
                    selection.moved = true;
                }
            }
            else if (selection.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const auto [row, offset] = placeAt(mouse);
                if (rows[row].seq != selection.headSeq || offset != selection.headOffset)
                    selection.moved = true;
                selection.headSeq = rows[row].seq;
                selection.headOffset = offset;
                // Past the edge scrolls, so a selection can run beyond the view.
                const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
                if (mouse.y < inner.Min.y)
                    ImGui::SetScrollY(ImGui::GetScrollY() - rowStep);
                else if (mouse.y > inner.Max.y)
                    ImGui::SetScrollY(ImGui::GetScrollY() + rowStep);
            }
            if (selection.dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                selection.dragging = false;
                if (!selection.moved && selection.pressedLink.has_value() && scriptCommands != nullptr)
                    scriptCommands->jumpTo = selection.pressedLink;
            }

            const bool focused = ImGui::IsWindowFocused();
            const bool ctrl = ImGui::GetIO().KeyCtrl;
            // Not with Shift: Ctrl+Shift+A shows Content.
            if (focused && ctrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false))
                selectAll();
            const bool hasSelection = selection.anchorSeq != 0 && (selection.anchorSeq != selection.headSeq ||
                                                                   selection.anchorOffset != selection.headOffset);
            const auto copySelection = [&]() {
                ImGui::SetClipboardText(textOf(rowOf(selection.anchorSeq, selection.anchorOffset),
                                               rowOf(selection.headSeq, selection.headOffset))
                                            .c_str());
            };
            if (focused && ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && hasSelection)
                copySelection();

            if (ImGui::BeginPopupContextWindow("##console-menu")) {
                if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.console.copy_2")), "Ctrl+C", false, hasSelection))
                    copySelection();
                if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.console.select_all")), "Ctrl+A"))
                    selectAll();
                if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.console.copy_all"))))
                    copyEverything();
                ImGui::EndPopup();
            }
            if (copyAll)
                copyEverything();
        }

        // Only while already at the bottom, so scrolling back to read something
        // is not undone by the next log line -- nor by a selection being dragged.
        if (!selection.dragging && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    static std::array<char, 512> input{};
    static ConsoleHistory history;
    const auto recall = [](ImGuiInputTextCallbackData* data) -> int {
        if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory)
            return 0;
        ConsoleHistory& state = *static_cast<ConsoleHistory*>(data->UserData);
        const core::i32 next = consoleHistoryStep(state.entries.size(), state.at, data->EventKey == ImGuiKey_UpArrow);
        if (next == state.at)
            return 0;
        if (state.at < 0)
            state.draft.assign(data->Buf, static_cast<core::usize>(data->BufTextLen));
        state.at = next;
        const std::string& text = next < 0 ? state.draft : state.entries[static_cast<core::usize>(next)];
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, text.c_str());
        return 0;
    };
    // The prompt says what the line is for: the debug console of the editor
    // this follows marks its input with a chevron and a hint.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(themeColor(palette().accent), ">");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    const bool submitted = ImGui::InputTextWithHint(
        "##repl",
        core::tr(runtime != nullptr ? ENG_TR("engine.editor.console.repl_hint")
                                    : ENG_TR("engine.editor.console.no_game")),
        input.data(), input.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, recall,
        &history);
    if (!submitted)
        return;

    const std::string_view source{input.data()};
    // Kept for the arrows, once per run of the same line, as a shell does.
    if (!source.empty() && (history.entries.empty() || history.entries.back() != source)) {
        history.entries.emplace_back(source);
        if (history.entries.size() > ConsoleHistory::kMaxEntries)
            history.entries.erase(history.entries.begin());
    }
    history.at = -1;
    history.draft.clear();
    if (!source.empty() && runtime != nullptr) {
        // Echoed first, so the log reads as a session rather than as a list of
        // answers with no questions.
        core::logText(core::LogLevel::Info, std::string("> ").append(source));
        if (const std::optional<core::EngineError> error = runtime->evaluate(source); error.has_value())
            core::logText(core::LogLevel::Error, error->message);
    }
    input.fill(0);
    ImGui::SetKeyboardFocusHere(-1);
}

// Play, pause and step, above the image they act on.
//
// **There IS a Stop, and what makes it honest is `World::snapshot` and
// `World::restore`** (D058, closed 2026-08-23). Stop means "put the world back
// the way it was before I pressed play" -- back to the EDITED state, not to the
// scripted one -- and while nothing could remember an edited world that was a
// button this engine could not ship. The pair arrived with the transport, so it
// can: pressing play remembers the world, pressing stop puts it back, and the
// status line under this toolbar says so in the same words.
// The transport, in the order and the shape Unity and Unreal both use.
//
// **Play and stop are one button because they are opposites**; pause is a
// different question and gets its own. A single toggle cannot answer both --
// pressing play asks "run my game", pressing stop asks "give me my world back",
// and a button that means one of them while showing the other is the first
// **Play with players** (ADR 0106 §5): how many, and whether a dedicated
// server runs them. One player with no server is the ordinary Play.
void openExportWindow(Editor& editor);

void drawMatchControls(Editor& editor, bool locked)
{
    Editor::MatchSettings& match = editor.matchSettings();
    ImGui::BeginDisabled(locked);
    // "1 player" to "4 players", in the reader's language and as wide as the
    // widest of them.
    std::array<std::string, 4> counts;
    std::array<const char*, 4> shownCounts{};
    float widest = 0.0f;
    for (std::size_t index = 0; index < counts.size(); ++index) {
        counts[index] =
            core::tr(ENG_TR("engine.editor.match_controls.players"), {{"count", static_cast<core::i64>(index + 1)}});
        shownCounts[index] = counts[index].c_str();
        widest = std::max(widest, ImGui::CalcTextSize(shownCounts[index]).x);
    }
    int chosen = std::clamp(match.players, 1, 4) - 1;
    ImGui::SetNextItemWidth(widest + ImGui::GetFrameHeight() * 1.5f);
    if (ImGui::Combo("##players", &chosen, shownCounts.data(), static_cast<int>(shownCounts.size())))
        match.players = chosen + 1;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.match_controls.more_than_one_plays_a_tip")));
    ImGui::SameLine();
    ImGui::Checkbox(core::tr(ENG_TR("engine.editor.match_controls.dedicated_server")), &match.dedicated);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.match_controls.run_the_match_on_a_tip")));
    ImGui::EndDisabled();
}

// thing a person notices.
// **The top of Run and Debug**: the button that starts the game and the
// match it starts, as the editor this follows puts a play button beside its
// launch configuration there. The match's shape is a testing question and
// lives here and in the Run menu, not on the toolbar (the owner, 2026-09-27).
void drawRunHeader(Editor& editor, EditorCommands& commands, const IconAtlas* icons)
{
    const bool running = editor.inPlayMode() || editor.matchRunning();
    const char* label = core::tr(running                            ? ENG_TR("engine.editor.transport.stop_word")
                                 : editor.matchSettings().isMatch() ? ENG_TR("engine.editor.transport.start_match")
                                                                    : ENG_TR("engine.editor.transport.start"));
    ImGui::PushStyleColor(ImGuiCol_Button, themeColor(palette().accentFill));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, themeColor(palette().accent));
    ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().onAccent));
    const bool pressed = labeledIconButton(icons, running ? icons::ActionStop : icons::ActionPlay, label);
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip("%s", core::tr(running ? ENG_TR("engine.editor.transport.stop_the_game_keys_tip")
                                                 : ENG_TR("engine.editor.transport.run_the_game_keys_tip")));
    if (pressed && !editor.stampSession().open()) {
        if (editor.matchRunning())
            commands.match = false;
        else if (editor.inPlayMode())
            commands.play = false;
        else if (editor.matchSettings().isMatch())
            commands.match = true;
        else
            commands.play = true;
    }
    ImGui::SameLine();
    drawMatchControls(editor, running);
    ImGui::Separator();
}

void drawTransport(Editor& editor, EditorCommands& commands, EditorPanels& panels, const IconAtlas* icons)
{
    const RunState run = editor.runState();
    const bool inPlay = editor.inPlayMode();

    // **A toolbar button is its icon, and its label is the tooltip.** Six drawn
    // words in a row is a sentence somebody reads; six pictures is a control
    // panel they aim at. Falls back to the word when there is no atlas, because
    // a button with nothing on it is not a smaller button.
    const float glyph = ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.y * 2.0f;
    const auto fitControl = [](float width) {
        if (ImGui::GetContentRegionAvail().x < width && ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x)
            ImGui::NewLine();
    };
    const auto toolButton = [&](std::string_view id, const char* word, const char* tip) {
        const float width = icons != nullptr && icons->ready() ? glyph : ImGui::CalcTextSize(word).x;
        fitControl(width + ImGui::GetStyle().FramePadding.x * 2.0f);
        return iconButton(icons, id, glyph, word, word, tip);
    };

    // **A toggle that is on says so in the accent** (the owner: what is
    // selected up here was barely visible). A lighter grey was the whole of the
    // difference between the tool in hand and the others; the fill the active
    // tab and a primary button wear is one nobody has to look twice for.
    const auto beginOn = [](bool on) {
        if (!on)
            return;
        const ThemePalette& accentPalette = palette();
        const ImVec4 fill = themeColor(accentPalette.accentFill);
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(std::min(1.0f, fill.x * 1.15f), std::min(1.0f, fill.y * 1.15f),
                                     std::min(1.0f, fill.z * 1.15f), fill.w));
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor(accentPalette.onAccent));
        g_iconOnAccent = true;
    };
    const auto endOn = [](bool on) {
        if (!on)
            return;
        g_iconOnAccent = false;
        ImGui::PopStyleColor(3);
    };

    // **A stamp is open, so the transport is the STAMP's.** Play, pause and
    // step have nothing to mean on a stage -- nothing there ticks -- and what a
    // person wants in their place is exactly what a session offers: keep it,
    // keep it and leave, or leave without keeping it. Same corner of the screen
    // as the thing it replaces, because that is where a hand already goes.
    if (editor.stampSession().open()) {
        const Editor::StampSession& session = editor.stampSession();
        if (toolButton(icons::ActionSave, core::tr(ENG_TR("engine.editor.transport.save")),
                       core::tr(ENG_TR("engine.editor.transport.save_this_stamp_and_keep_tip"))))
            commands.saveStamp = true;

        ImGui::SameLine();
        if (toolButton(icons::ActionClose, core::tr(ENG_TR("engine.editor.transport.close")),
                       core::tr(ENG_TR("engine.editor.transport.save_and_go_back_to_tip")))) {
            commands.closeStamp = true;
            commands.closeStampSaving = true;
        }

        // Only while there is something to discard: a control that would do
        // nothing is worse than no control, and one that throws work away
        // should not sit there on a session with nothing to throw.
        if (session.dirty) {
            ImGui::SameLine();
            if (toolButton(icons::ActionDelete, core::tr(ENG_TR("engine.editor.transport.discard")),
                           core::tr(ENG_TR("engine.editor.transport.go_back_without_saving_the_tip")))) {
                commands.closeStamp = true;
                commands.closeStampSaving = false;
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        (void)drawIcon(icons, icons::ClassModel, glyph);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().warning));
        ImGui::Text("%s%s", session.path.c_str(), session.dirty ? " *" : "");
        ImGui::PopStyleColor();
        return;
    }

    // **Play, pause and step, in the middle of the bar** -- where every game
    // engine keeps them (the owner, 2026-09-27): the one control somebody
    // reaches for without looking is in the one place that does not move when
    // the tools beside it change. Drawn after the tools, and placed.
    const auto runControls = [&]() {
        const bool matching = editor.matchRunning();
        if (toolButton(inPlay || matching ? icons::ActionStop : icons::ActionPlay,
                       core::tr(inPlay || matching ? ENG_TR("engine.editor.transport.stop")
                                                   : ENG_TR("engine.editor.transport.play")),
                       core::tr(inPlay || matching ? ENG_TR("engine.editor.transport.stop_the_game_tip")
                                                   : ENG_TR("engine.editor.transport.run_the_game_tip")))) {
            if (matching)
                commands.match = false;
            else if (!inPlay && editor.matchSettings().isMatch())
                commands.match = true;
            else
                commands.play = !inPlay;
        }
        // How many players, and whether a server runs them, live on the Test tab
        // alone (the owner, 2026-09-27); Play here still plays what Test says.
        // Only inside play mode, because pausing is a thing that happens to a
        // running game. Disabled rather than hidden: a control that appears and
        // disappears moves the ones beside it.
        ImGui::SameLine();
        ImGui::BeginDisabled(!inPlay);
        if (toolButton(run == RunState::Paused ? icons::ActionPlay : icons::ActionPause,
                       core::tr(run == RunState::Paused ? ENG_TR("engine.editor.transport.resume")
                                                        : ENG_TR("engine.editor.transport.pause")),
                       core::tr(ENG_TR("engine.editor.transport.hold_the_running_world_still_tip")))) {
            commands.pause = run != RunState::Paused;
        }
        ImGui::EndDisabled();

        // **Hidden while editing, and only usable while paused.** A step is one
        // tick of the simulation; an edited world is one whose simulation is
        // deliberately stopped, so the control has nothing to mean there. Inside
        // play mode it stays PRESENT and greys while running, because a control
        // that appears and disappears moves the ones beside it -- which is the same
        // rule pause is drawn by, applied one state further in.
        if (inPlay) {
            ImGui::SameLine();
            ImGui::BeginDisabled(run != RunState::Paused);
            if (toolButton(icons::ActionForward, core::tr(ENG_TR("engine.editor.transport.step")),
                           core::tr(ENG_TR("engine.editor.transport.advance_exactly_one_simulation_tick_tip"))))
                editor.requestStep();
            ImGui::EndDisabled();

            // **Look somewhere else while it runs** (S5.8). Only in play mode,
            // because while editing the view is already the editor's -- a button
            // that did nothing in the state somebody spends most of their time in
            // would be furniture.
            ImGui::SameLine();
            const bool detached = editor.cameraDetached();
            beginOn(detached);
            if (toolButton(icons::ActionVisible, core::tr(ENG_TR("engine.editor.transport.eye")),
                           core::tr(detached ? ENG_TR("engine.editor.transport.back_to_the_games_camera_tip")
                                             : ENG_TR("engine.editor.transport.fly_the_editors_camera_tip")))) {
                editor.setCameraDetached(!detached);
            }
            endOn(detached);
        }
    };

    // --- The manipulators -----------------------------------------------
    //
    // Selection and transforms stay here; specialized operations live in their panels.
    const bool selecting = editor.tool() == Editor::Tool::Select && !editor.handlesShown();
    beginOn(selecting);
    if (toolButton(icons::ActionSelect, core::tr(ENG_TR("engine.editor.transport.select")),
                   core::tr(ENG_TR("engine.editor.transport.select_with_no_handles_in_tip")))) {
        editor.setTool(Editor::Tool::Select);
        editor.setHandlesShown(false);
    }
    endOn(selecting);

    const auto modeButton = [&](GizmoMode mode, std::string_view id, const char* word, const char* tip) {
        ImGui::SameLine();
        const bool on = editor.tool() == Editor::Tool::Select && editor.handlesShown() && editor.gizmoMode() == mode;
        beginOn(on);
        if (toolButton(id, word, tip)) {
            editor.setTool(Editor::Tool::Select);
            editor.setGizmoMode(mode);
        }
        endOn(on);
    };
    modeButton(GizmoMode::Translate, icons::ActionMove, core::tr(ENG_TR("engine.editor.transport.move")),
               core::tr(ENG_TR("engine.editor.transport.move_the_selection_tip")));
    modeButton(GizmoMode::Scale, icons::ActionScale, core::tr(ENG_TR("engine.editor.transport.size")),
               core::tr(ENG_TR("engine.editor.transport.resize_the_selection_tip")));
    modeButton(GizmoMode::Rotate, icons::ActionRotate, core::tr(ENG_TR("engine.editor.transport.turn")),
               core::tr(ENG_TR("engine.editor.transport.turn_the_selection_tip")));

    // **Greyed while resizing, because a size has no world space to be in.**
    // Three numbers in the part's own frame is what a `Size` is, so the scale
    // handles are always the part's own -- and a toggle that stayed live while
    // changing nothing is a control that lies twice: once by looking usable,
    // and once by reading "world" over local handles.
    ImGui::SameLine();
    const bool sizing = editor.gizmoMode() == GizmoMode::Scale;
    ImGui::BeginDisabled(sizing);
    // **A picture beside the word** (the owner: intuitive, with icons): the
    // world's axes are the globe, the selection's own are the part.
    const bool localAxes = sizing || editor.gizmoLocal();
    // As wide as the wider of its two words, so the bar does not move when
    // the word does.
    const char* const worldWord = core::tr(ENG_TR("engine.editor.transport.world"));
    const char* const localWord = core::tr(ENG_TR("engine.editor.transport.local"));
    fitControl(std::max(ImGui::CalcTextSize((tabIconPad() + worldWord).c_str()).x,
                        ImGui::CalcTextSize((tabIconPad() + localWord).c_str()).x) +
               ImGui::GetStyle().FramePadding.x * 2.0f);
    if (labeledIconButton(icons, localAxes ? icons::ClassPart : icons::ClassWorkspace,
                          localAxes ? localWord : worldWord))
        editor.setGizmoLocal(!editor.gizmoLocal());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", core::tr(sizing                ? ENG_TR("engine.editor.transport.a_size_is_local_tip")
                                         : editor.gizmoLocal() ? ENG_TR("engine.editor.transport.own_axes_tip")
                                                               : ENG_TR("engine.editor.transport.world_axes_tip")));

    // **Where the gizmo sits over a selection** (S5.17), beside the axis-space
    // toggle because they are the same kind of question: one is which way the
    // handles point and this is where they are.
    ImGui::SameLine();
    {
        const bool centred = editor.gizmoOrigin() == Editor::GizmoOrigin::Centre;
        const char* const centreWord = core::tr(ENG_TR("engine.editor.transport.centre"));
        const char* const pivotWord = core::tr(ENG_TR("engine.editor.transport.pivot"));
        fitControl(std::max(ImGui::CalcTextSize((tabIconPad() + centreWord).c_str()).x,
                            ImGui::CalcTextSize((tabIconPad() + pivotWord).c_str()).x) +
                   ImGui::GetStyle().FramePadding.x * 2.0f);
        // The point a thing turns about, or the middle of what is selected.
        if (labeledIconButton(icons, centred ? icons::ClassModel : icons::ClassAttachment,
                              centred ? centreWord : pivotWord))
            editor.setGizmoOrigin(centred ? Editor::GizmoOrigin::Pivot : Editor::GizmoOrigin::Centre);
        ImGui::SetItemTooltip("%s", core::tr(centred ? ENG_TR("engine.editor.transport.the_middle_tip")
                                                     : ENG_TR("engine.editor.transport.the_last_clicked_tip")));
    }

    ImGui::SameLine();
    const bool snapping = editor.snapping();
    {
        beginOn(snapping);
        if (toolButton(icons::ActionGrid, core::tr(ENG_TR("engine.editor.transport.snap")),
                       core::tr(ENG_TR("engine.editor.transport.snap_to_the_grid_hold_tip")))) {
            editor.setSnap(!editor.snapping());
        }
        endOn(snapping);

        // **The step, behind a right-click on the button it belongs to** (S5.13).
        // `setSnapStep` has existed since the manipulator did and nothing could
        // call it, so every scene in this editor was built on a quarter of a
        // metre and fifteen degrees whether or not those were the numbers -- and
        // a snap you cannot change is a snap you turn off.
        //
        // Not a Preferences page: a step is a thing somebody changes while
        // placing something, and a dialog two menus away is one they change once
        // and then work around.
        if (ImGui::BeginPopupContextItem("snap-step")) {
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.transport.snap_step")));
            ImGui::Separator();

            // One row per mode rather than one for whichever is active, because
            // the useful gesture is "set them all up the way I work" and doing
            // that through three tool switches is three times the clicks.
            struct Row
            {
                GizmoMode mode;
                const char* id;
                core::TextKey label;
                core::TextKey format;
                f32 slowest;
                f32 fastest;
            };
            const Row Rows[] = {
                {GizmoMode::Translate, "move", ENG_TR("engine.editor.transport.move"),
                 ENG_TR("engine.editor.unit.metres_3"), 0.001f, 64.0f},
                {GizmoMode::Rotate, "turn", ENG_TR("engine.editor.transport.turn"),
                 ENG_TR("engine.editor.unit.degrees_1"), 0.1f, 90.0f},
                {GizmoMode::Scale, "size", ENG_TR("engine.editor.transport.size"),
                 ENG_TR("engine.editor.unit.metres_3"), 0.001f, 64.0f},
            };
            for (const Row& row : Rows) {
                ImGui::PushID(row.id);
                ImGui::TextUnformatted(core::tr(row.label));
                ImGui::SameLine(60.0f * ImGui::GetStyle().FontScaleMain);
                ImGui::SetNextItemWidth(140.0f * ImGui::GetStyle().FontScaleMain);
                f32 step = editor.snapStep(row.mode);
                if (ImGui::DragFloat("##step", &step, step * 0.05f + 0.001f, row.slowest, row.fastest,
                                     core::tr(row.format)))
                    editor.setSnapStep(row.mode, step);
                ImGui::PopID();
            }

            ImGui::Separator();
            // **The grid follows the move step**, which is what makes it a
            // reference rather than a decoration: lines you can see and a snap
            // you cannot are two grids, and the one that catches is invisible.
            ImGui::Checkbox(core::tr(ENG_TR("engine.editor.transport.show_a_grid_in_the")), &panels.showGrid);
            ImGui::EndPopup();
        }
    }

    // **The run controls, in the middle of the bar**, when the tools on the
    // left leave room for them there -- and straight after the tools when they
    // do not.
    const ImGuiStyle& barStyle = ImGui::GetStyle();
    {
        const float button = (icons != nullptr && icons->ready()
                                  ? glyph
                                  : ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.transport.resume"))).x) +
                             barStyle.FramePadding.x * 2.0f;
        const int count = inPlay ? 4 : 2;
        const float group = button * static_cast<float>(count) + barStyle.ItemSpacing.x * static_cast<float>(count - 1);
        ImGui::SameLine();
        const float centred = (ImGui::GetWindowWidth() - group) * 0.5f;
        if (centred > ImGui::GetCursorPosX() + barStyle.ItemSpacing.x * 2.0f)
            ImGui::SetCursorPosX(centred);
        runControls();
    }

    // **And at the far end, what sets the tools up and what ships the game**:
    // the snap steps, the tool panels, the viewport's settings and Export.
    // Right-aligned by the width the group took last frame, which is exact
    // where an estimate of four widgets' widths would drift with the theme.
    // Measured as the sum of its items' own widths, which is right wherever
    // they landed -- a width taken from positions is wrong the frame the group
    // wraps, and then keeps it wrapped.
    static float s_endGroupWidth = 0.0f;
    float endGroupWidth = 0.0f;
    const auto measured = [&]() { endGroupWidth += ImGui::GetItemRectSize().x + barStyle.ItemSpacing.x; };
    ImGui::SameLine();
    const float atEnd = ImGui::GetWindowWidth() - barStyle.WindowPadding.x - s_endGroupWidth;
    if (s_endGroupWidth > 0.0f && atEnd > ImGui::GetCursorPosX() + barStyle.ItemSpacing.x * 2.0f)
        ImGui::SetCursorPosX(atEnd);
    {
        // **The steps themselves, on the toolbar beside the switch** (the
        // owner's feedback): how far a drag moves, turns and resizes is read
        // and changed while placing something, so it lives where the placing
        // is and not in a settings panel. Dimmed with the switch off, and still
        // editable, so the numbers can be set up before snapping is turned on.
        //
        // **Each wears its tool's picture** rather than its name (the owner:
        // intuitive, with icons) -- the move, turn and resize handles the
        // buttons on the left already show -- and falls back to the word when
        // there is no atlas.
        const bool pictured = icons != nullptr && icons->ready();
        const auto stepField = [&](GizmoMode mode, std::string_view icon, const char* id, const char* format,
                                   const char* worded, f32 slowest, f32 fastest, const char* tip) {
            // As wide as a step reads in this field's own format: a sample put
            // where the conversion is. Not printed through it -- a format that
            // arrived through a variable is one `-Wformat-nonliteral` refuses.
            const std::string_view shownFormat = pictured ? format : worded;
            std::string sample(shownFormat);
            if (const std::size_t at = shownFormat.find('%'); at != std::string_view::npos) {
                std::size_t end = at + 1;
                while (end < shownFormat.size() && std::isalpha(static_cast<unsigned char>(shownFormat[end])) == 0)
                    ++end;
                sample = std::string(shownFormat.substr(0, at)) + "0.25" +
                         std::string(shownFormat.substr(std::min(end + 1, shownFormat.size())));
            }
            const float width = ImGui::CalcTextSize(sample.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            fitControl(width + (pictured ? glyph + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f));
            if (!snapping)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);
            if (pictured) {
                const float lift = (ImGui::GetFrameHeight() - glyph) * 0.5f;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
                (void)drawIcon(icons, icon, glyph);
                ImGui::SetItemTooltip("%s", tip);
                measured();
                // Back on the row: `SameLine` returns to the LINE's height, not
                // the lifted icon's, so the field needs no correction -- one
                // put it a lift above every other button on the bar.
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            }
            ImGui::SetNextItemWidth(width);
            f32 step = editor.snapStep(mode);
            if (ImGui::DragFloat(id, &step, step * 0.05f + 0.001f, slowest, fastest, pictured ? format : worded))
                editor.setSnapStep(mode, step);
            if (!snapping)
                ImGui::PopStyleVar();
            ImGui::SetItemTooltip("%s", tip);
            measured();
        };
        stepField(GizmoMode::Translate, icons::ActionMove, "##move-step",
                  core::tr(ENG_TR("engine.editor.unit.metres_2")),
                  core::tr(ENG_TR("engine.editor.transport.step_move")), 0.001f, 64.0f,
                  core::tr(ENG_TR("engine.editor.transport.step_move_tip")));
        ImGui::SameLine();
        stepField(GizmoMode::Rotate, icons::ActionRotate, "##turn-step",
                  core::tr(ENG_TR("engine.editor.unit.degrees_0")),
                  core::tr(ENG_TR("engine.editor.transport.step_turn")), 0.1f, 90.0f,
                  core::tr(ENG_TR("engine.editor.transport.step_turn_tip")));
        ImGui::SameLine();
        stepField(GizmoMode::Scale, icons::ActionScale, "##size-step", core::tr(ENG_TR("engine.editor.unit.metres_2")),
                  core::tr(ENG_TR("engine.editor.transport.step_size")), 0.001f, 64.0f,
                  core::tr(ENG_TR("engine.editor.transport.step_size_tip")));
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    measured();

    ImGui::SameLine();
    fitControl(ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.transport.tools"))).x + glyph +
               ImGui::GetStyle().FramePadding.x * 3.0f);
    if (labeledIconButton(icons, icons::ActionTools, core::tr(ENG_TR("engine.editor.transport.tools"))))
        ImGui::OpenPopup("viewport-tools");
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.transport.open_a_tool_panel_without_tip")));
    measured();
    if (ImGui::BeginPopup("viewport-tools")) {
        if (iconMenuItem(icons, icons::ClassTerrain, core::tr(ENG_TR("engine.editor.transport.terrain")))) {
            panels.terrain = true;
            ImGui::SetWindowFocus("Terrain");
        }
        if (iconMenuItem(icons, icons::ClassVoxelService, core::tr(ENG_TR("engine.editor.transport.blocks")))) {
            panels.blocks = true;
            ImGui::SetWindowFocus("Blocks");
        }
        if (iconMenuItem(icons, icons::ClassTilemap2D, core::tr(ENG_TR("engine.editor.transport.tiles")))) {
            panels.tiles = true;
            ImGui::SetWindowFocus("Tiles###Tiles");
        }
        if (iconMenuItem(icons, WaterIcon, core::tr(ENG_TR("engine.editor.transport.water")))) {
            panels.water = true;
            editor.setTool(Editor::Tool::Water);
            ImGui::SetWindowFocus("###Water");
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (toolButton(icons::ActionSettings, core::tr(ENG_TR("engine.editor.transport.viewport_settings")),
                   core::tr(ENG_TR("engine.editor.transport.camera_snapping_and_visualization_overlays_tip"))))
        panels.viewportSettings = !panels.viewportSettings;
    measured();

    // **Export** (ADR 0104 §4): the way from a scene to a game on somebody's
    // phone, at the end of the bar, where a finished thing leaves.
    ImGui::SameLine();
    if (toolButton(icons::ActionExport, core::tr(ENG_TR("engine.editor.transport.export")),
                   core::tr(ENG_TR("engine.editor.transport.export_the_game_for_windows_tip"))))
        openExportWindow(editor);
    measured();
    s_endGroupWidth = endGroupWidth - barStyle.ItemSpacing.x;

    // Whether the game runs, which tool is in hand and how to fly are the
    // status bar's to say now (the owner's queue, Q2).
    (void)run;

    // The status used to be a second line here, and moving it is not cosmetic:
    // a line that exists when there is something to say and does not when there
    // is nothing RESIZES the viewport under it, twice per save. Everything
    // measured against that image moves with it -- the aspect ratio, the picking
    // ray, the manipulator under the pointer -- so a confirmation was making the
    // world jump. It is a toast over the picture now; see `drawViewportStatus`.
}

// What the mouse and keyboard are asking the viewport for.
//
// **It reports and does not act.** The camera is driven by the frame loop,
// because turning it puts the pointer into relative mode and in relative mode
// ImGui has no absolute position to difference -- so `io.MouseDelta` is zero
// exactly when the camera needs it. The motion comes from the platform's own
// events; what a UI callback can still answer is whether somebody is asking.
//
// RIGHT button, not left: left is select, and an editor where looking around
// changes what you have selected is unusable. Held rather than toggled, because
// a mode you can forget you are in is how a minute goes missing.
void reportLookInput(Editor& editor, bool overViewport)
{
    const ImGuiIO& io = ImGui::GetIO();

    // The wheel changes SPEED rather than dollying. A dolly duplicates what W
    // and S already do; a speed control is the thing a four-kilometre world and
    // a four-metre room need different values of.
    //
    // **In the 2D view it zooms instead**, about the pointer: there is no
    // flying to be fast at, and a wheel that zooms is what a 2D editor is.
    // **Not over the game's own view** (`Editor::viewportIsGames`): the wheel
    // there is the game's, and it was also changing the editor's speed.
    if (overViewport && io.MouseWheel != 0.0f && !editor.viewportIsGames()) {
        if (editor.view2D()) {
            const ViewportRect& rect = editor.viewport();
            editor.zoom2D(io.MouseWheel, core::Vec2{io.MousePos.x - rect.x, io.MousePos.y - rect.y});
        }
        else {
            editor.setCameraSpeed(editor.cameraSpeed() * (io.MouseWheel > 0.0f ? 1.25f : 0.8f));
        }
    }

    // **Latched on the press, and only there.** The first version asked "over
    // the viewport OR dragging", and `IsMouseDragging` becomes true a pixel
    // after a press ANYWHERE -- so a right-click in the explorer turned into a
    // camera turn: the pointer locked, motion stopped reaching ImGui, and the
    // context menu that click was asking for never appeared.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        g_lookLatched = overViewport;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
        g_lookLatched = false;

    Editor::LookInput look;
    look.active = g_lookLatched;

    // **The keys fly without the button** while the viewport has the keyboard,
    // which is where somebody's hands already are after clicking into the world.
    // Never with Ctrl or Alt held: those make a key a SHORTCUT, and Ctrl+D both
    // duplicating and sliding the camera right is a key doing two things.
    // Never while a text field has the keyboard, either, which a detached view
    // in play mode would otherwise steal from a game's chat box.
    const bool viewportHasKeys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
                                 (editor.cameraDetached() && !io.WantCaptureKeyboard);
    const bool shortcutHeld = io.KeyCtrl || io.KeyAlt || io.KeySuper;
    if (look.active || (viewportHasKeys && !shortcutHeld && !io.WantTextInput && !ImGui::IsAnyItemActive())) {
        const auto axis = [](ImGuiKey positive, ImGuiKey negative) -> f32 {
            return (ImGui::IsKeyDown(positive) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(negative) ? 1.0f : 0.0f);
        };
        // **Shift is for precision, not speed** (the owner: "Left Shift should
        // make the camera slower"): a quarter of the speed, for lining up on
        // something small. The wheel is what changes the speed itself.
        const f32 pace = ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 0.25f : 1.0f;
        // Not even with the button held: a shortcut pressed mid-turn is still a
        // shortcut, and the keys it shares with flying must not also fly.
        if (!shortcutHeld) {
            look.move =
                core::Vec3{axis(ImGuiKey_D, ImGuiKey_A), axis(ImGuiKey_E, ImGuiKey_Q), axis(ImGuiKey_W, ImGuiKey_S)} *
                pace;
        }
    }

    editor.setLookInput(look);
}

// The 3D view.
//
// The panel IS the image: no padding, because a margin of window background
// around a rendered world reads as a bug rather than as a frame. Its rectangle
// is handed to the editor every frame because that rectangle is the only thing
// that maps a mouse position onto a ray.
// The image and everything that reads a pointer over it, without the window
// around it.
//
// Split out so that F3 can show the WORLD with none of the furniture -- see
// `drawViewportFullscreen`. It sets the editor's viewport rect from whatever
// window it is called inside, which is what keeps the aspect ratio, the picking
// ray and the image somebody is looking at agreeing with each other whichever
// of the two is drawing.
// **The status, over the world, fading.**
//
// Drawn straight into the draw list rather than as an item, which is the whole
// point: an item occupies layout, and layout that appears and disappears is a
// viewport that changes size when the editor has something to say.
//
// `region` is the image's rectangle in screen space.
// What a terrain brush in hand does, in the Terrain panel's words -- or
// nothing, for a tool that is not one.
[[nodiscard]] std::string terrainBrushWords(const Editor& editor)
{
    // No panel, no brush (`Editor::setTerrainPanelShown`): the chip said a
    // brush was in hand that a click could not use (terrain audit E6).
    // Nor with no terrain in the world, where a click selects (the editor list).
    const Editor::Tool tool = editor.tool();
    if ((tool == Editor::Tool::Sculpt || tool == Editor::Tool::Paint || tool == Editor::Tool::Foliage) &&
        (!editor.terrainPanelShown() || !editor.hasTerrain()))
        return {};
    switch (tool) {
    case Editor::Tool::Sculpt:
        switch (editor.effectiveBrushOp()) {
        case Editor::BrushOp::Grow:
            return core::tr(ENG_TR("engine.editor.terrain.sculpt.raise"));
        case Editor::BrushOp::Erode:
            return core::tr(ENG_TR("engine.editor.terrain.sculpt.lower"));
        case Editor::BrushOp::Smooth:
            return core::tr(ENG_TR("engine.editor.terrain.sculpt.smooth"));
        case Editor::BrushOp::Flatten:
            return core::tr(ENG_TR("engine.editor.terrain.sculpt.flatten"));
        case Editor::BrushOp::Add:
            return core::tr(ENG_TR("engine.editor.terrain.sculpt.add"));
        case Editor::BrushOp::Subtract:
            return core::tr(ENG_TR("engine.editor.terrain.sculpt.dig"));
        }
        return core::tr(ENG_TR("engine.editor.terrain.mode.sculpt"));
    case Editor::Tool::Paint:
        return core::tr(ENG_TR("engine.editor.terrain.mode.paint"));
    case Editor::Tool::Foliage:
        return editor.effectiveFoliageThin() ? core::tr(ENG_TR("engine.editor.terrain.chip.thin"))
                                             : core::tr(ENG_TR("engine.editor.terrain.chip.restore"));
    case Editor::Tool::Water:
        if (!editor.waterPanelShown())
            break;
        switch (editor.waterOp()) {
        case Editor::WaterOp::River:
            return core::tr(ENG_TR("engine.editor.water_panel.river"));
        case Editor::WaterOp::Lake:
            return core::tr(ENG_TR("engine.editor.water_panel.lake"));
        case Editor::WaterOp::Pool:
            return core::tr(ENG_TR("engine.editor.water_panel.pool"));
        case Editor::WaterOp::Ocean:
            return core::tr(ENG_TR("engine.editor.water_panel.ocean"));
        }
        break;
    case Editor::Tool::Select:
    case Editor::Tool::Blocks:
    case Editor::Tool::Tiles:
        break;
    }
    return {};
}

// **A brush in hand says so, on the viewport itself** (the terrain editor,
// remade): what it does, how big it is, and how to put it down -- the answer
// to "why is the ground changing when I click", which the owner had to find by
// accident.
void drawBrushChip(const Editor& editor, ImVec2 at, ImVec2 region)
{
    const std::string words = terrainBrushWords(editor);
    if (words.empty() || region.x < 160.0f || editor.inPlayMode())
        return;
    // The Water tool has no size to say: what it draws is as wide as its
    // panel says, and the stretch it shows.
    const bool sized = editor.tool() != Editor::Tool::Water;
    char size[32];
    (void)std::snprintf(size, sizeof(size), "%.2f", static_cast<double>(editor.brush().radius));
    std::string tail = sized ? core::tr(ENG_TR("engine.editor.terrain.chip.tail"), {{"size", std::string_view(size)}})
                             : std::string(core::tr(ENG_TR("engine.editor.water_panel.chip_tail")));
    const float scale = ImGui::GetStyle().FontScaleMain;
    const float pad = 8.0f * scale;
    // **Shortened to fit, never cut** (the editor list: at the right edge of a
    // narrow viewport): the size and the hint go first, the tool's name last.
    if (sized && ImGui::CalcTextSize((words + tail).c_str()).x + pad * 4.0f > region.x)
        tail = core::tr(ENG_TR("engine.editor.terrain.chip.tail_short"), {{"size", std::string_view(size)}});
    if (ImGui::CalcTextSize((words + tail).c_str()).x + pad * 4.0f > region.x)
        tail.clear();
    const std::string text = words + tail;
    const ImVec2 extent = ImGui::CalcTextSize(text.c_str());
    const ImVec2 min(at.x + pad, at.y + pad);
    const ImVec2 max(min.x + extent.x + pad * 2.0f, min.y + extent.y + pad);
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)), 4.0f * scale);
    list->AddText(ImVec2(min.x + pad, min.y + pad * 0.5f),
                  ImGui::ColorConvertFloat4ToU32(themeColor(palette().warning)), words.c_str());
    list->AddText(ImVec2(min.x + pad + ImGui::CalcTextSize(words.c_str()).x, min.y + pad * 0.5f),
                  ImGui::GetColorU32(ImGuiCol_Text), tail.c_str());
}

void drawViewportStatus(const Editor& editor, ImVec2 at, ImVec2 region)
{
    const EditorStatus& status = editor.status();
    if (status.message.empty() || region.x < 32.0f)
        return;

    // Restarted on the WRITE and not on the words, so saving twice shows the
    // confirmation twice. See `StatusSlot`.
    if (editor.statusSerial() != g_statusSerial) {
        g_statusSerial = editor.statusSerial();
        g_statusAt = ImGui::GetTime();
    }

    // **An error is held far longer than a confirmation**, because the person
    // reading a confirmation already knew what they did and the person reading
    // an error has just been surprised by it.
    const double age = ImGui::GetTime() - g_statusAt;
    const double hold = status.failed ? 9.0 : 3.5;
    constexpr double kFade = 0.75;
    if (age >= hold + kFade)
        return;
    const auto alpha = static_cast<float>(age < hold ? 1.0 : 1.0 - (age - hold) / kFade);

    // **A notification, as the editor this follows shows one** (the owner's
    // queue, Q2): a card in the bottom-right corner of the window, above the
    // status bar, with the kind of message as its icon -- over everything, on
    // the foreground list, so no panel covers it and none moves for it.
    (void)at;
    const float scale = ImGui::GetStyle().FontScaleMain;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float statusBar = std::round(ImGui::GetFontSize() + 8.0f * scale);
    const float pad = 12.0f * scale;
    const float glyph = ImGui::GetFontSize();
    const float width = std::min(440.0f * scale, viewport->Size.x - pad * 2.0f);
    const float wrap = width - pad * 3.0f - glyph;
    const ImVec2 extent = ImGui::CalcTextSize(status.message.c_str(), nullptr, false, wrap);
    const float height = std::max(extent.y, glyph) + pad * 2.0f;
    const ImVec2 max(viewport->Pos.x + viewport->Size.x - pad, viewport->Pos.y + viewport->Size.y - statusBar - pad);
    const ImVec2 min(max.x - width, max.y - height);
    (void)region;

    const ThemePalette& p = palette();
    const auto faded = [alpha](core::Color3 colour, float opacity = 1.0f) {
        ImVec4 value = themeColor(colour);
        value.w = opacity * alpha;
        return ImGui::ColorConvertFloat4ToU32(value);
    };
    ImDrawList* draw = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    // A soft shadow under the card, two rings of fading black.
    for (int ring = 1; ring <= 3; ++ring) {
        const float grow = static_cast<float>(ring) * 2.0f * scale;
        draw->AddRectFilled(ImVec2(min.x - grow, min.y - grow + 2.0f * scale),
                            ImVec2(max.x + grow, max.y + grow + 2.0f * scale),
                            IM_COL32(0, 0, 0, static_cast<int>(28.0f * alpha)), 6.0f * scale + grow);
    }
    draw->AddRectFilled(min, max, faded(p.surface), 6.0f * scale);
    draw->AddRect(min, max, faded(p.border), 6.0f * scale);

    // The kind: a circled cross for a failure, a circled tick otherwise.
    const ImU32 tone = faded(status.failed ? p.danger : p.accent);
    const ImVec2 centre(min.x + pad + glyph * 0.5f, min.y + pad + glyph * 0.5f);
    const float radius = glyph * 0.45f;
    const float thickness = std::max(1.0f, radius * 0.18f);
    draw->AddCircle(centre, radius, tone, 0, thickness);
    if (status.failed) {
        const float arm = radius * 0.42f;
        draw->AddLine(ImVec2(centre.x - arm, centre.y - arm), ImVec2(centre.x + arm, centre.y + arm), tone, thickness);
        draw->AddLine(ImVec2(centre.x - arm, centre.y + arm), ImVec2(centre.x + arm, centre.y - arm), tone, thickness);
    }
    else {
        draw->AddLine(ImVec2(centre.x - radius * 0.45f, centre.y),
                      ImVec2(centre.x - radius * 0.1f, centre.y + radius * 0.35f), tone, thickness);
        draw->AddLine(ImVec2(centre.x - radius * 0.1f, centre.y + radius * 0.35f),
                      ImVec2(centre.x + radius * 0.5f, centre.y - radius * 0.35f), tone, thickness);
    }
    draw->AddText(nullptr, 0.0f, ImVec2(min.x + pad * 2.0f + glyph, min.y + pad), faded(p.text), status.message.c_str(),
                  nullptr, wrap);
}

// --- Handles on a selected interface element --------------------------------
//
// **A selected Frame, Button or Label gets a box and eight handles** (the owner:
// "selecting a UI should put points around it"). A corner or an edge resizes,
// the inside moves, and the whole drag is ONE undo step -- it writes `Size`
// and `Position` through the inspector like the Properties grid does, under one
// gesture. The offsets change and the scales do not: a person dragging is
// saying "this many pixels", which is what an offset is.

// Which part of the box a drag holds: the eight handles, the body, or none.
enum class UiGrip : core::u8
{
    None,
    Body,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct UiDrag
{
    UiGrip grip = UiGrip::None;
    core::InstanceId target;
    ImVec2 startMouse{};
    // The box and the properties as the drag found them.
    ImVec2 min{};
    ImVec2 max{};
    core::UDim2 position{};
    core::UDim2 size{};
};
UiDrag g_uiDrag;

// Which grip `mouse` is on, handles before the body.
[[nodiscard]] UiGrip uiGripAt(ImVec2 mouse, ImVec2 min, ImVec2 max, float reach)
{
    const float midX = (min.x + max.x) * 0.5f;
    const float midY = (min.y + max.y) * 0.5f;
    const auto near = [&](float x, float y) {
        return std::fabs(mouse.x - x) <= reach && std::fabs(mouse.y - y) <= reach;
    };
    if (near(min.x, min.y))
        return UiGrip::TopLeft;
    if (near(max.x, min.y))
        return UiGrip::TopRight;
    if (near(min.x, max.y))
        return UiGrip::BottomLeft;
    if (near(max.x, max.y))
        return UiGrip::BottomRight;
    if (near(midX, min.y))
        return UiGrip::Top;
    if (near(midX, max.y))
        return UiGrip::Bottom;
    if (near(min.x, midY))
        return UiGrip::Left;
    if (near(max.x, midY))
        return UiGrip::Right;
    if (mouse.x > min.x && mouse.x < max.x && mouse.y > min.y && mouse.y < max.y)
        return UiGrip::Body;
    return UiGrip::None;
}

[[nodiscard]] ImGuiMouseCursor uiGripCursor(UiGrip grip)
{
    switch (grip) {
    case UiGrip::Left:
    case UiGrip::Right:
        return ImGuiMouseCursor_ResizeEW;
    case UiGrip::Top:
    case UiGrip::Bottom:
        return ImGuiMouseCursor_ResizeNS;
    case UiGrip::TopLeft:
    case UiGrip::BottomRight:
        return ImGuiMouseCursor_ResizeNWSE;
    case UiGrip::TopRight:
    case UiGrip::BottomLeft:
        return ImGuiMouseCursor_ResizeNESW;
    case UiGrip::Body:
        return ImGuiMouseCursor_ResizeAll;
    default:
        return ImGuiMouseCursor_Arrow;
    }
}

// Draws the box of the selected interface element over the viewport and
// takes a drag on it. Returns whether the pointer is on the box, so the
// world pick underneath does not also take the click.
bool drawInterfaceHandles(scene::World* world, Inspector* inspector, ImVec2 origin, bool overImage)
{
    if (world == nullptr || inspector == nullptr)
        return false;
    const core::InstanceId selected = inspector->selection();
    const scene::UIObjectComponent* ui = world->uiObjects().find(selected);
    if (ui == nullptr || !ui->visible || ui->absoluteSize.x <= 0.0f || ui->absoluteSize.y <= 0.0f) {
        g_uiDrag = UiDrag{};
        return false;
    }

    const ImVec2 min(origin.x + ui->absolutePosition.x, origin.y + ui->absolutePosition.y);
    const ImVec2 max(min.x + ui->absoluteSize.x, min.y + ui->absoluteSize.y);
    const ThemePalette& p = palette();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 edge = ImGui::GetColorU32(themeColor(p.accent));
    draw->AddRect(min, max, edge, 0.0f, 0, 1.5f);

    constexpr float kHandle = 4.0f;
    const float midX = (min.x + max.x) * 0.5f;
    const float midY = (min.y + max.y) * 0.5f;
    for (const ImVec2 at : {min, ImVec2(max.x, min.y), ImVec2(min.x, max.y), max, ImVec2(midX, min.y),
                            ImVec2(midX, max.y), ImVec2(min.x, midY), ImVec2(max.x, midY)}) {
        draw->AddRectFilled(ImVec2(at.x - kHandle, at.y - kHandle), ImVec2(at.x + kHandle, at.y + kHandle),
                            ImGui::GetColorU32(themeColor(p.surface)));
        draw->AddRect(ImVec2(at.x - kHandle, at.y - kHandle), ImVec2(at.x + kHandle, at.y + kHandle), edge);
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    const UiGrip hovered = overImage ? uiGripAt(mouse, min, max, kHandle + 3.0f) : UiGrip::None;
    if (g_uiDrag.grip == UiGrip::None && hovered != UiGrip::None) {
        ImGui::SetMouseCursor(uiGripCursor(hovered));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_uiDrag = UiDrag{hovered, selected, mouse, min, max, ui->position, ui->size};
            (void)inspector->beginGesture();
        }
    }

    if (g_uiDrag.grip != UiGrip::None) {
        ImGui::SetMouseCursor(uiGripCursor(g_uiDrag.grip));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || g_uiDrag.target != selected) {
            g_uiDrag = UiDrag{};
            inspector->endGesture();
            return true;
        }

        const UiGrip grip = g_uiDrag.grip;
        const bool body = grip == UiGrip::Body;
        const UiDragResult moved = uiDragResult(
            core::Vec2{g_uiDrag.min.x, g_uiDrag.min.y}, core::Vec2{g_uiDrag.max.x, g_uiDrag.max.y},
            core::Vec2{std::round(mouse.x - g_uiDrag.startMouse.x), std::round(mouse.y - g_uiDrag.startMouse.y)},
            body || grip == UiGrip::Left || grip == UiGrip::TopLeft || grip == UiGrip::BottomLeft,
            body || grip == UiGrip::Right || grip == UiGrip::TopRight || grip == UiGrip::BottomRight,
            body || grip == UiGrip::Top || grip == UiGrip::TopLeft || grip == UiGrip::TopRight,
            body || grip == UiGrip::Bottom || grip == UiGrip::BottomLeft || grip == UiGrip::BottomRight,
            ui->anchorPoint, g_uiDrag.position, g_uiDrag.size);
        const core::UDim2 size = moved.size;
        const core::UDim2 position = moved.position;
        if (!(size == ui->size))
            inspector->enqueue(selected, world->atoms().intern("Size"), scene::Value{size});
        if (!(position == ui->position))
            inspector->enqueue(selected, world->atoms().intern("Position"), scene::Value{position});
        return true;
    }
    return hovered != UiGrip::None;
}

void drawViewportBody(Editor& editor, rhi::TextureHandle texture, EditorCommands& commands,
                      scene::World* world = nullptr, Inspector* inspector = nullptr)
{
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    editor.setViewport(ViewportRect{origin.x, origin.y, size.x, size.y});

    SDL_GPUTexture* native = texture.valid() ? rhi::nativeTexture(*g_device, texture) : nullptr;
    if (native != nullptr && size.x >= 1.0f && size.y >= 1.0f) {
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)), size);
        drawViewportStatus(editor, origin, size);
        drawBrushChip(editor, origin, size);

        // **Dropped into the WORLD** (ADR 0052): dragging something out of
        // the project's tree and onto the viewport is the shortest way to
        // say "one of those, here". It lands under `Workspace` rather than
        // where the pointer is, because a drop point is a pixel and a
        // placement is a position -- the two only meet through a pick, and
        // a prefab that landed inside whatever happened to be behind the
        // cursor would be a surprise every time it worked.
        //
        // Only what it can place: a stamp, or a material for the part under
        // the pointer. Anything else is not offered a place it would be
        // quietly dropped from.
        const ImGuiPayload* offered = ImGui::GetDragDropPayload();
        const bool placeable = offered != nullptr && offered->IsDataType(kContentDragPayload) &&
                               (isMaterialDrag(*static_cast<const ContentDrag*>(offered->Data)) ||
                                isStampDrag(*static_cast<const ContentDrag*>(offered->Data)) ||
                                isMeshDrag(*static_cast<const ContentDrag*>(offered->Data)));
        // Nothing is dropped into a running game.
        if (placeable && editor.authoring() && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(kContentDragPayload); dropped != nullptr) {
                const auto* drag = static_cast<const ContentDrag*>(dropped->Data);
                if (isMaterialDrag(*drag)) {
                    // Onto whatever part is under the pointer, which the frame
                    // loop picks: a material lands on a thing, not in a place.
                    const ImVec2 at = ImGui::GetMousePos();
                    commands.assignMaterialPath = drag->path;
                    commands.assignMaterialPixel = core::Vec2{at.x - origin.x, at.y - origin.y};
                }
                else if (isStampDrag(*drag)) {
                    commands.placeStamp = drag->path;
                    commands.placeStampLinked = true;
                    commands.placeStampParent = {};
                }
                // **A mesh becomes a `MeshPart` standing where it was dropped**
                // (the owner): the pick at the pointer says on what.
                else if (isMeshDrag(*drag)) {
                    const ImVec2 at = ImGui::GetMousePos();
                    commands.placeMesh = drag->path;
                    commands.placeMeshPixel = core::Vec2{at.x - origin.x, at.y - origin.y};
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Hovering the IMAGE, not the window: a click on the tab, the
        // border or the space beside a letterboxed image is not a click on
        // the world, and treating it as one deselects whatever the person
        // was working on.
        const bool overImage = ImGui::IsItemHovered();
        const ImVec2 mouse = ImGui::GetMousePos();
        const core::Vec2 inViewport{mouse.x - origin.x, mouse.y - origin.y};

        // **The pointer, every frame, not just on a click.** A manipulator
        // needs where it is and whether the button is held; a click alone
        // cannot say either. `pressed` is gated on the image because a drag
        // that began in another panel is not this one's, and `down` is NOT,
        // because a drag that leaves the viewport is still a drag and one
        // that ends outside it still ends.
        editor.setPointer(inViewport, overImage && ImGui::IsMouseClicked(ImGuiMouseButton_Left),
                          ImGui::IsMouseDown(ImGuiMouseButton_Left));
        editor.setPointerOverViewport(overImage);

        // **A double-click drills in** (S5.3). ImGui reports the second press
        // as both a click and a double-click, so this is one request either
        // way -- a first click that selected the model followed by a second
        // that opened it is exactly the gesture, and treating them as two
        // requests would make the first one's selection flicker.
        //
        // **Alt picks the part itself**, whatever model it is in: the quick
        // way to one wheel of a car without opening the car first.
        // A press on the selected interface element's box is the box's, not
        // a pick of whatever is behind it in the world.
        // **No handles over a running game**: its interface is the game's to
        // press, not the editor's to drag about.
        if (!editor.authoring())
            g_uiDrag = UiDrag{};
        const bool onInterface = editor.authoring() && drawInterfaceHandles(world, inspector, origin, overImage);
        if (overImage && !onInterface && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            editor.requestPick(inViewport, ImGui::GetIO().KeyCtrl, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left),
                               ImGui::GetIO().KeyAlt);
        }

        reportLookInput(editor, overImage);

        // **Who has the keyboard** (the Play rule): the game, only while this
        // window does and no text field of the editor's is being typed in. A
        // window with nothing focused at all is the viewport's -- the picture
        // alone, with the furniture put away.
        const ImGuiContext* const context = ImGui::GetCurrentContext();
        editor.setKeyboardHolder(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
                                     context->NavWindow == nullptr,
                                 ImGui::GetIO().WantTextInput);
        g_gameHasKeyboard = editor.gameHasKeyboard();

        // **A border that says whose view this is**: the accent while the game
        // has it, the warning colour while it runs or waits and the editor is
        // the one looking (ejected, or paused).
        if (editor.inPlayMode()) {
            const ThemePalette& p = palette();
            const float thickness = std::round(2.0f * ImGui::GetStyle().FontScaleMain);
            const ImVec4 tint = themeColor(editor.viewportIsGames() ? p.accentFill : p.warning);
            const float inset = thickness * 0.5f;
            ImGui::GetWindowDrawList()->AddRect(ImVec2(origin.x + inset, origin.y + inset),
                                                ImVec2(origin.x + size.x - inset, origin.y + size.y - inset),
                                                ImGui::GetColorU32(tint), 0.0f, 0, thickness);
        }
    }
}

void drawViewport(scene::World* world, Inspector* inspector, Editor& editor, rhi::TextureHandle texture,
                  EditorCommands& commands, EditorPanels& panels, bool& open, const IconAtlas* icons)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // **Room for the camera**, painted over the gap by `drawTabIcons`. Before
    // the `###`, so this is still the window called `Viewport` to the dock
    // builder and to every `layout.ini` anybody has saved.
    const std::string label = tabIconPad() + core::tr(ENG_TR("engine.editor.panel.viewport")) + "###Viewport";
    const bool visible = ImGui::Begin(label.c_str(), &open);
    ImGui::PopStyleVar();

    // **The viewport is the world and nothing else** (the owner: "the viewport
    // is just the viewport"). The transport and the tools are the ribbon's, at
    // the top of the editor, where they are whether this panel is open or not.
    (void)panels;
    (void)icons;
    if (visible)
        drawViewportBody(editor, texture, commands, world, inspector);
    ImGui::End();
}

// **The ribbon, across the top of the editor under the menu bar** -- not inside
// the viewport, because playing, inserting and arranging are the editor's and
// not one panel's, and a closed viewport took them away. A side bar of the main
// viewport, so the dockspace is measured below it the way it is below the menu.
//
// As tall as it was last frame: one row in a wide window, two when it wraps,
// and a strip that never cuts its own buttons off.
void drawRibbonBar(Editor& editor, EditorCommands& commands, EditorPanels& panels, const IconAtlas* icons)
{
    static float s_height = 0.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float oneRow = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
    const float height = s_height > 0.0f ? s_height : oneRow;
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##ribbon", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags)) {
        drawTransport(editor, commands, panels, icons);
        s_height = std::max(oneRow, ImGui::GetCursorPosY() + style.WindowPadding.y);
    }
    ImGui::End();
}

// **What F3 shows in the editor**, and the reason it has to show anything at
// all: in the editor the world is rendered into a TEXTURE and the screen itself
// is only cleared, so hiding the panels hides the world with them -- a black
// window, reported as one. The overlay's own contract calls F3 "the cheapest
// way to look at the world without the furniture", and this is the half that
// makes that true.
//
// A window of its own rather than the docked one undocked: the layout somebody
// arranged is theirs, and a keystroke that rearranged it would cost more than
// it showed. It sets the viewport rect to the whole screen while it is up, so
// the view is the view and a click still lands where it looks.
void drawViewportFullscreen(Editor& editor, rhi::TextureHandle texture, EditorCommands& commands)
{
    const ImGuiViewport* screen = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(screen->Pos);
    ImGui::SetNextWindowSize(screen->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoBackground;
    const bool visible = ImGui::Begin("##fullscreen-viewport", nullptr, flags);
    ImGui::PopStyleVar();
    if (visible) {
        drawViewportBody(editor, texture, commands);
    }
    ImGui::End();
}

// The arrangement somebody gets the first time they open the editor.
//
// Without this every panel is placed at ImGui's default position, which is the
// same position, so the first launch is five windows in a pile with the
// viewport at the bottom of it -- which is what the first run of this shell
// actually looked like. A dockspace does not arrange anything by itself; it
// only makes arranging possible.
//
// **Which tab a dock node opens on**, which `SetWindowFocus` does not decide.
//
// That is not a guess: ImGui's `FocusWindow` carries the line that would do it
// and the line is COMMENTED OUT, with its own note -- "for #2304 we avoid
// applying focus immediately before the tabbar is visible". So D083's fix for
// "the editor opens on stats" moved the focus and never moved the tab, and the
// node went on opening on whichever window was docked last, which is what it
// had always done. Reported again, in the same words, by the same person.
//
// What ImGui does read is the node's own `SelectedTabId` when the tab bar is
// built, and `NextSelectedTabId` when one already exists. Setting both is the
// same request asked of whichever of the two states the node is in.
void selectDockTab(const char* name)
{
    ImGuiWindow* window = ImGui::FindWindowByName(name);
    if (window == nullptr || window->DockNode == nullptr)
        return;
    window->DockNode->SelectedTabId = window->TabId;
    if (window->DockNode->TabBar != nullptr)
        window->DockNode->TabBar->NextSelectedTabId = window->TabId;
}

// Whether a panel is open and behind another tab of its dock: on screen for
// nobody, which is not the same as put away.
[[nodiscard]] bool dockTabBehind(const char* name)
{
    const ImGuiWindow* window = ImGui::FindWindowByName(name);
    return window != nullptr && window->DockNode != nullptr && window->DockNode->SelectedTabId != window->TabId;
}

// **A workbench key, pressed from anywhere -- the code included.** The code
// pane claims every key while its caret is in it (`SetActiveIdUsingAllKeyboardKeys`,
// as a text field does), and a global route loses to that: Ctrl+Shift+P typed
// into a script did nothing, and the next words landed in the code. The
// palette, the panels and Run work from every text field in the editor this
// follows; only a modal dialog keeps them.
bool workbenchKey(ImGuiKeyChord keys)
{
    if (ImGui::Shortcut(keys, ImGuiInputFlags_RouteGlobal))
        return true;
    return ImGui::IsAnyItemActive() && ImGui::GetTopMostPopupModal() == nullptr &&
           ImGui::IsKeyChordPressed(keys, ImGuiInputFlags_None, ImGuiKeyOwner_Any);
}

// Built once, and only when there is no saved layout: `DockBuilderRemoveNode`
// would throw away the arrangement somebody chose.
void buildDefaultLayout(ImGuiID dockspace)
{
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);

    // **A game engine's arrangement** (the owner, 2026-09-27: the look may be
    // the code editor's, the layout has to be an engine's). The world in the
    // middle, and every open script a tab beside it; the tree on the left; the
    // selection's properties down the whole right side; and along the bottom,
    // under the tree and the world both, the project's files and what the game
    // said -- where Unity keeps its Project and Console and Unreal its content
    // drawer. Split right first, so the inspector runs the full height, and
    // bottom second, so the browser gets the width a grid of assets wants.
    ImGuiID centre = dockspace;
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, nullptr, &centre);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.30f, nullptr, &centre);
    const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, nullptr, &centre);

    ImGui::DockBuilderDockWindow("Viewport", centre);
    // The tree, alone: the panel somebody is in all day is not a tab they have
    // to find behind two others.
    ImGui::DockBuilderDockWindow("Explorer", left);
    // What describes the selection, and the tools that edit it. **The world
    // tools stay beside Properties**: selecting a terrain in the tree opens its
    // panel, and a panel that appears comes to the front of its node -- beside
    // the tree, that would put the Explorer away under the click that chose
    // the terrain.
    ImGui::DockBuilderDockWindow("Properties", right);
    // The numbers beside the selection's properties (the owner): a tab away
    // from the inspector, where a glance does not cost the bottom panel.
    ImGui::DockBuilderDockWindow("Stats", right);
    ImGui::DockBuilderDockWindow("Terrain", right);
    ImGui::DockBuilderDockWindow("Blocks", right);
    ImGui::DockBuilderDockWindow("Tiles", right);
    ImGui::DockBuilderDockWindow("Water", right);
    ImGui::DockBuilderDockWindow("Viewport Settings", right);
    // Under everything: the files, what the game said and the debugger. A tab
    // node opens on whichever window was docked last, so which one greets
    // somebody is set explicitly after the build (`selectDockTab`).
    ImGui::DockBuilderDockWindow("Content", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Debug", bottom);
    ImGui::DockBuilderDockWindow("Streaming", bottom);

    ImGui::DockBuilderFinish(dockspace);
}

// --- What every dialog in this shell does with the keyboard -----------------
//
// **The box has the keyboard the moment it opens, Enter is the accept button,
// and Escape is Cancel.** Written once because the six dialogs had different
// halves of it: Rename re-focused its field on every frame -- which re-selects
// the text under somebody's caret sixty times a second -- New Folder focused
// nothing and came back holding the last name typed, and not one of them
// answered Escape.

// True on the frame a modal opens, which is where seeding and focusing belong:
// doing either every frame overwrites what a person is typing with what they
// started from.
[[nodiscard]] bool dialogOpening()
{
    return ImGui::IsWindowAppearing();
}

// Escape means Cancel, and it SAYS it took the key.
//
// `CloseCurrentPopup` takes effect immediately, so the shell's own Escape
// handler would otherwise find no popup where this one had been and drop the
// selection on the same press -- which is D095, one dialog over.
//
// Not while a shortcut is being recorded in Preferences: there Escape cancels
// the recording, and closing the window under it would lose the page too.
bool g_escapeRecordsChord = false;

[[nodiscard]] bool dialogCancelled()
{
    if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false) || g_escapeRecordsChord)
        return false;
    g_escapeTaken = true;
    return true;
}

// --- The script editor's preferences ----------------------------------------
//
// Every colour and every command key `script_editor_settings.h` lists, written
// back the moment it changes -- the rule the appearance already follows.

void saveScriptPreferences()
{
    (void)saveScriptEditorSettings(scriptEditorSettingsFile(), scriptEditorSettings());
}

// **Each page of Preferences both counts and draws** (the owner, 2026-09-27:
// a search above a tree, as the content browser has one). Called with `draw`
// false, a page only says how many of its rows `needle` finds -- which is what
// the tree beside it shows, and how a page with nothing found is left out --
// and with it true, it draws those rows in the Properties panel's grid.
struct PreferenceQuery
{
    std::string_view needle;
    // The page's own name or its group matched: every row of it shows.
    bool whole = false;
    bool draw = false;

    [[nodiscard]] bool shows(std::string_view label) const
    {
        return needle.empty() || whole || containsFold(label, needle);
    }
};

// A setting's explanation, under its widget where the value is.
void preferenceHint(const char* text)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

// A small reset beside a value, dimmed when there is nothing to reset.
bool preferenceReset(const IconAtlas* icons, bool changed, const char* tip)
{
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(!changed);
    const bool pressed = iconButton(icons, icons::ActionRevert, ImGui::GetFontSize(), "##reset",
                                    core::tr(ENG_TR("engine.editor.preference_reset.reset")), nullptr);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

// The width a value leaves for a reset beside it.
[[nodiscard]] float besideReset()
{
    return -(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
}

std::size_t drawScriptColourPreferences(const PreferenceQuery& query, const IconAtlas* icons)
{
    ScriptEditorSettings& settings = scriptEditorSettings();
    const Theme& theme = themeById(g_appearance.themeId);
    std::size_t found = 0;
    const bool grid = query.draw && beginSectionGrid("script-colours");
    if (grid && query.needle.empty()) {
        const bool anyChanged =
            std::any_of(settings.colors.begin(), settings.colors.end(),
                        [](const std::optional<core::Color3>& colour) { return colour.has_value(); });
        sectionName(core::tr(ENG_TR("engine.editor.script_colour_preferences.all_colours")));
        ImGui::BeginDisabled(!anyChanged);
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.script_colour_preferences.reset_to_the_theme")),
                          ImVec2(-FLT_MIN, 0.0f))) {
            settings.colors = {};
            saveScriptPreferences();
        }
        ImGui::EndDisabled();
        preferenceHint(core::tr(ENG_TR("engine.editor.script_colour_preferences.colours_nobody_changed_follow_the")));
    }
    for (std::size_t index = 0; index < kScriptColorCount; ++index) {
        const auto which = static_cast<ScriptColor>(index);
        const ScriptColorInfo& info = scriptColorInfo()[index];
        if (!query.shows(core::tr(info.label)))
            continue;
        ++found;
        if (!grid)
            continue;
        ImGui::PushID(static_cast<int>(index));
        sectionName(core::tr(info.label));
        const core::Color3 current = settings.color(which, theme);
        float value[3]{current.r, current.g, current.b};
        ImGui::SetNextItemWidth(besideReset());
        if (ImGui::ColorEdit3("##colour", value, ImGuiColorEditFlags_DisplayHex))
            settings.colors[index] = core::Color3{value[0], value[1], value[2]};
        if (ImGui::IsItemDeactivatedAfterEdit())
            saveScriptPreferences();
        if (preferenceReset(
                icons, settings.colors[index].has_value(),
                core::tr(ENG_TR("engine.editor.script_colour_preferences.back_to_the_themes_colour_tip")))) {
            settings.colors[index].reset();
            saveScriptPreferences();
        }
        ImGui::PopID();
    }
    if (grid)
        endSectionGrid();
    return found;
}

// The command waiting for a chord, when somebody clicked one to rebind it.
std::optional<ScriptAction> g_capturingChord;
// Set to open Preferences on its Shortcuts page, and read once.
bool g_preferencesToShortcuts = false;

// The chord being recorded: the first key that is not a modifier, with the
// modifiers held at that moment. Every frame Preferences is open, whichever
// page is on screen -- a search typed meanwhile must not strand a capture.
void captureScriptChord()
{
    if (!g_capturingChord.has_value())
        return;
    ScriptEditorSettings& settings = scriptEditorSettings();
    const ImGuiIO& io = ImGui::GetIO();
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        const auto k = static_cast<ImGuiKey>(key);
        const bool modifier = (k >= ImGuiKey_LeftCtrl && k <= ImGuiKey_RightSuper) ||
                              (k >= ImGuiKey_ReservedForModCtrl && k <= ImGuiKey_ReservedForModSuper);
        if (modifier || !ImGui::IsKeyPressed(k, false))
            continue;
        if (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseWheelY)
            continue;
        const std::size_t index = static_cast<std::size_t>(*g_capturingChord);
        if (k == ImGuiKey_Escape && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
            // The key was the capture's, not the dialog's.
            g_escapeTaken = true;
            g_capturingChord.reset();
        }
        else if (k == ImGuiKey_Backspace && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
            settings.keys[index] = KeyChord{};
            g_capturingChord.reset();
            saveScriptPreferences();
        }
        else {
            settings.keys[index] =
                KeyChord{.key = ImGui::GetKeyName(k), .ctrl = io.KeyCtrl, .shift = io.KeyShift, .alt = io.KeyAlt};
            g_capturingChord.reset();
            saveScriptPreferences();
        }
        break;
    }
}

// Searched by the command's name or by its keys, as the keyboard shortcuts
// editor of the editor this follows searches: "ctrl+d" finds what that chord
// does.
std::size_t drawScriptShortcutPreferences(const PreferenceQuery& query, const IconAtlas* icons)
{
    ScriptEditorSettings& settings = scriptEditorSettings();
    const ThemePalette& p = palette();
    std::size_t found = 0;
    const bool grid = query.draw && beginSectionGrid("script-keys");
    if (grid && query.needle.empty()) {
        const bool anyChanged = std::any_of(settings.keys.begin(), settings.keys.end(),
                                            [](const std::optional<KeyChord>& chord) { return chord.has_value(); });
        sectionName(core::tr(ENG_TR("engine.editor.script_shortcut_preferences.all_shortcuts")));
        ImGui::BeginDisabled(!anyChanged);
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.script_shortcut_preferences.reset_to_the_defaults")),
                          ImVec2(-FLT_MIN, 0.0f))) {
            settings.keys = {};
            g_capturingChord.reset();
            saveScriptPreferences();
        }
        ImGui::EndDisabled();
        preferenceHint(core::tr(ENG_TR("engine.editor.script_shortcut_preferences.click_a_shortcut_and_press")));
    }
    for (std::size_t index = 0; index < kScriptActionCount; ++index) {
        const auto which = static_cast<ScriptAction>(index);
        const ScriptActionInfo& info = scriptActionInfo()[index];
        const KeyChord chord = settings.chord(which);
        if (!query.shows(core::tr(info.label)) &&
            (chord.key.empty() || !containsFold(formatChord(chord), query.needle)))
            continue;
        ++found;
        if (!grid)
            continue;
        const std::optional<ScriptAction> clash = settings.conflictOf(which, chord);
        ImGui::PushID(static_cast<int>(index));
        sectionName(core::tr(info.label));
        const bool capturing = g_capturingChord == which;
        const std::string shown =
            capturing           ? std::string(core::tr(ENG_TR("engine.editor.script_shortcut_preferences.press_keys")))
            : chord.key.empty() ? std::string(core::tr(ENG_TR("engine.editor.script_shortcut_preferences.none")))
                                : formatChord(chord);
        if (capturing)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(shown.c_str(), ImVec2(besideReset(), 0.0f)))
            g_capturingChord = capturing ? std::optional<ScriptAction>{} : std::optional<ScriptAction>{which};
        if (capturing)
            ImGui::PopStyleColor();
        ImGui::SetItemTooltip(
            "%s", core::tr(ENG_TR("engine.editor.script_shortcut_preferences.click_then_press_the_new_tip")));
        const std::string back =
            info.chord.empty()
                ? std::string(core::tr(ENG_TR("engine.editor.script_shortcut_preferences.back_to_no_key")))
                : core::tr(ENG_TR("engine.editor.script_shortcut_preferences.back_to"), {{"keys", info.chord}});
        if (preferenceReset(icons, settings.keys[index].has_value(), back.c_str())) {
            settings.keys[index].reset();
            saveScriptPreferences();
        }
        if (clash.has_value()) {
            ImGui::TextColored(
                ImVec4(p.danger.r, p.danger.g, p.danger.b, 1.0f), "%s",
                core::tr(ENG_TR("engine.editor.script_shortcut_preferences.also"),
                         {{"action", core::tr(scriptActionInfo()[static_cast<std::size_t>(*clash)].label)}})
                    .c_str());
        }
        ImGui::PopID();
    }
    if (grid)
        endSectionGrid();
    return found;
}

// Every modal has the same escape route. Closing is cancellation, never acceptance.
// Cancel handlers clear pending requests that would otherwise reopen on the next frame.
template <typename Cancel>
//
// A `height` gives the dialog a size of its own that does not follow what is
// on show -- Preferences, whose pages must not resize the window under the
// hand moving between them (the owner). Zero fits the contents.
bool beginEditorDialog(const char* title, float width, Cancel cancel, float height = 0.0f)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = ImGui::GetStyle().FontScaleMain;
    const float fittedWidth = std::max(1.0f, std::min(width * scale, viewport->WorkSize.x - 32.0f));
    const float fittedHeight = height > 0.0f ? std::min(height * scale, viewport->WorkSize.y * 0.9f) : 0.0f;
    ImGui::SetNextWindowSize(ImVec2(fittedWidth, fittedHeight));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(fittedWidth, viewport->WorkSize.y * 0.9f));
    bool open = true;
    const bool visible = ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_NoResize);
    if (!open) {
        cancel();
        return false;
    }
    if (visible && dialogCancelled()) {
        cancel();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return false;
    }
    return visible;
}

bool dialogButton(const char* label, ImVec2 requested)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width =
        std::max(requested.x * style.FontScaleMain, ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f);
    if (ImGui::GetContentRegionAvail().x < width && ImGui::GetCursorPosX() > style.WindowPadding.x)
        ImGui::NewLine();
    return ImGui::Button(label, ImVec2(std::min(width, ImGui::GetContentRegionAvail().x), requested.y));
}

// A short label for a kind, so a row says what it is without an icon set.
// The icon id for a content kind, straight off the enum -- the same mechanical
// mapping `class.` uses, which is what `icons/README.md` means by "content.
// maps straight off the `ContentKind` enum".
[[nodiscard]] std::string_view contentKindIcon(ContentKind kind) noexcept
{
    switch (kind) {
    case ContentKind::Folder:
        return icons::ContentFolder;
    case ContentKind::Scene:
        return icons::ContentScene;
    case ContentKind::Stamp:
        // **A `Model` wearing the stamp badge**, which is not a stand-in for a
        // drawing the set owes -- it is the same sentence the Explorer makes,
        // read the other way round. There, a badged icon says "this instance
        // comes from a file"; here it says "this file is what one comes from",
        // and one mark for one idea beats two drawings for it. The base is a
        // `Model` because a stamp is a group of instances handled as one thing.
        // The badge is drawn by the browser, over this.
        return icons::ClassModel;
    case ContentKind::Mesh:
        return icons::ContentMesh;
    case ContentKind::Texture:
        return icons::ContentTexture;
    case ContentKind::Audio:
        return icons::ContentAudio;
    case ContentKind::Font:
        return icons::ContentFont;
    case ContentKind::Chunk:
        return icons::ContentChunk;
    // The class drawing the set already has for it: a material was a class
    // until ADR 0090, and the picture of one did not change when it became a
    // file.
    case ContentKind::Material:
        return icons::ContentMaterial;
    case ContentKind::Shader:
        return icons::ContentShader;
    case ContentKind::Other:
        break;
    }
    return icons::ContentOther;
}

// The icon a content row wears.
//
// **A stamp wears the icon of the instance it is a file OF** -- a character's
// stamp draws as a character and a lamp post's as a part -- because that is the
// thing a person recognises in a folder of forty, and because a file of a
// character IS a character as far as anybody browsing is concerned. The kind's
// own icon is the fallback for a stamp whose root this build could not read or
// has no drawing for; the badge on top is what says "a file one comes from"
// rather than "the instance itself".
bool drawContentIcon(const IconAtlas* icons, const scene::World* world, const ContentEntry& entry, float size,
                     std::optional<core::Color3> tint)
{
    if (entry.kind == ContentKind::Stamp && !entry.rootClass.empty() &&
        drawIcon(icons,
                 classIconFor(icons, world != nullptr ? &world->classes() : nullptr,
                              world != nullptr ? &world->atoms() : nullptr, entry.rootClass),
                 size, tint)) {
        return true;
    }
    // A folder with something in it is drawn filled (the owner).
    if (entry.kind == ContentKind::Folder && entry.filled)
        return drawIcon(icons, icons::ContentFolderFilled, size, tint);
    return drawIcon(icons, contentKindIcon(entry.kind), size, tint);
}

// **A picture row shows the picture.** An icon can say "this is an image"; the
// file name already said that. Which image it is, is the question somebody
// scrolling a folder of textures is actually asking, and only the image answers
// it.
//
// Returns false when there is no thumbnail yet -- a first frame, a file still in
// the decode queue, a file that is not really a picture -- and the caller draws
// the kind icon, which is what the row looked like before this existed.
bool drawContentThumbnail(const ContentTree& tree, const ContentEntry& entry, float size)
{
    // **Every kind that has a picture, not only textures** (S5.16). The cache
    // answers `previewKindOf` itself and refuses what it cannot draw, so a
    // folder or a sound never reaches a read -- guarding here as well would be a
    // second opinion about which files have pictures, and the one that goes
    // stale the day a third kind gets one.
    if (g_thumbnails == nullptr || g_device == nullptr)
        return false;

    const ThumbnailCache::Thumbnail thumbnail = g_thumbnails->request(tree.absolute(entry));
    if (!thumbnail.valid())
        return false;

    SDL_GPUTexture* native = rhi::nativeTexture(*g_device, thumbnail.texture);
    if (native == nullptr)
        return false;

    // **Fitted, never stretched, and centred in the box the icon would have
    // filled.** A 16:9 texture squashed into a square is a thumbnail of a
    // different picture, and a row whose face is a different height from its
    // neighbours is a list that looks broken.
    const float longer = static_cast<float>(std::max(thumbnail.width, thumbnail.height));
    const float scale = longer > 0.0f ? size / longer : 0.0f;
    const ImVec2 drawn(static_cast<float>(thumbnail.width) * scale, static_cast<float>(thumbnail.height) * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 min(origin.x + (size - drawn.x) * 0.5f, origin.y + (size - drawn.y) * 0.5f);
    const ImVec2 max(min.x + drawn.x, min.y + drawn.y);

    // The checkerboard behind it, which is not decoration either: a texture with
    // an alpha channel drawn straight onto the panel is a texture whose cut-out
    // parts are indistinguishable from its dark parts. Every tool that shows
    // images draws this, and it is the only way "transparent" reads as anything
    // other than "black".
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float cell = std::max(4.0f, size * 0.25f);
    const ImU32 pale = ImGui::ColorConvertFloat4ToU32(ImVec4(0.55f, 0.55f, 0.57f, 1.0f));
    const ImU32 dark = ImGui::ColorConvertFloat4ToU32(ImVec4(0.38f, 0.38f, 0.40f, 1.0f));
    draw->AddRectFilled(min, max, pale);
    draw->PushClipRect(min, max, true);
    for (int row = 0; min.y + static_cast<float>(row) * cell < max.y; ++row) {
        for (int column = (row & 1); min.x + static_cast<float>(column) * cell < max.x; column += 2) {
            const ImVec2 cellMin(min.x + static_cast<float>(column) * cell, min.y + static_cast<float>(row) * cell);
            draw->AddRectFilled(cellMin, ImVec2(cellMin.x + cell, cellMin.y + cell), dark);
        }
    }
    draw->PopClipRect();
    draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)), min, max);

    // The box, not the picture: the caller laid out a square and the next thing
    // it places has to start after that square whichever shape landed in it.
    ImGui::Dummy(ImVec2(size, size));
    return true;
}

[[nodiscard]] const char* contentKindLabel(ContentKind kind) noexcept
{
    switch (kind) {
    case ContentKind::Folder:
        return "dir";
    case ContentKind::Scene:
        return "scene";
    case ContentKind::Stamp:
        return "stamp";
    case ContentKind::Mesh:
        return "mesh";
    case ContentKind::Texture:
        return "tex";
    case ContentKind::Audio:
        return "audio";
    case ContentKind::Font:
        return "font";
    case ContentKind::Chunk:
        return "chunk";
    case ContentKind::Material:
        return "material";
    case ContentKind::Shader:
        return "shader";
    case ContentKind::Other:
        break;
    }
    return "";
}

// The project's assets, and the panel from which a scene is opened.
//
// **Virtualised**, and that is a measurement rather than a flourish: the quality
// bar for this was given as Unity and Unreal, and a Content Browser is judged on
// a tree of thousands. `ImGuiListClipper` draws only the rows a person can see,
// so a folder of ten thousand meshes costs the same as a folder of ten.
// `drawExplorer` beside this one is virtualised twice over: the clipper draws
// what is on screen, and the walk that decides what could be on screen does not
// enter a closed subtree (ADR 0054).
// One entry of the browser, in whichever layout is on.
//
// **The three layouts share every decision except where things are put**, which
// is why this is one function with a switch inside rather than three: the double
// click that opens a scene, the menu that colours a folder, the green that marks
// the scene already open and the colour a person chose all belong to an ENTRY
// and not to a way of drawing one. Three copies of that would drift the first
// time any of it changed, which is exactly what happened to the Explorer's row
// before it was rebuilt around one height.
struct ContentLayout
{
    // The box one entry occupies. For a list that is the full width by a row's
    // height; for the other two it is a square somebody can aim at.
    ImVec2 cell;
    float icon = 0.0f;
    // Whether the name goes under the icon rather than beside it.
    bool nameBelow = false;
    // How many fit across. One for a list.
    int columns = 1;
};

[[nodiscard]] ContentLayout contentLayoutFor(EditorPanels::ContentView view, float available)
{
    ContentLayout layout;
    switch (view) {
    case EditorPanels::ContentView::List:
        layout.cell = ImVec2(0.0f, ImGui::GetFrameHeight());
        layout.icon = ImGui::GetTextLineHeight();
        layout.nameBelow = false;
        layout.columns = 1;
        return layout;
    case EditorPanels::ContentView::Tiles:
        layout.icon = ImGui::GetFrameHeight() * 2.0f;
        break;
    case EditorPanels::ContentView::Icons:
        layout.icon = ImGui::GetFrameHeight() * 3.0f;
        break;
    }

    const float padding = ImGui::GetStyle().ItemInnerSpacing.x * 2.0f;
    // **Wide enough for a name**, not only for the icon: a cell as wide as its
    // picture cut "materials" to "materi..." -- which is a grid of pictures
    // with the words taken away. About eleven characters, as the grids of
    // Unity's and Unreal's browsers give a name.
    const float side = std::max(layout.icon, ImGui::GetFontSize() * 5.5f) + padding;
    layout.cell = ImVec2(side, layout.icon + ImGui::GetTextLineHeight() + padding);
    layout.nameBelow = true;
    // At least one, whatever the panel has been dragged down to: a column count
    // of zero is a division by it two lines later.
    layout.columns = std::max(1, static_cast<int>(available / (side + ImGui::GetStyle().ItemSpacing.x)));
    return layout;
}

// A name that does not fit, cut where it stops fitting and marked. Only the
// grids need it -- a list row has the whole panel width and simply runs on.
[[nodiscard]] std::string elideToWidth(const std::string& text, float width)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;

    const float ellipsis = ImGui::CalcTextSize("...").x;
    std::string cut;
    cut.reserve(text.size());
    for (const char c : text) {
        cut.push_back(c);
        if (ImGui::CalcTextSize(cut.c_str()).x + ellipsis > width) {
            cut.pop_back();
            break;
        }
    }
    cut += "...";
    return cut;
}

// Opening another scene loses whatever is unsaved, so it asks the same question
// closing does -- through the same gate, which is what keeps one answer for the
// doors that reach it (ADR 0055's launcher added two of them).
void openSceneOrAsk(Editor& editor, EditorCommands& commands, EditorDialogs& dialogs, std::string_view path)
{
    issueOrAsk(EditorDialogs::Pending::OpenScene, editor.hasUnsavedWork(), path, dialogs, commands);
}

// **The folder tree beside the grid** (the owner, 2026-09-27: a game engine's
// browser). Along the bottom the browser is wide, and every engine that puts it
// there gives it the project's folders down the left -- a click on any of them
// goes straight there, where the path above only goes back up. Read from disk
// when the browser re-reads a folder, never per frame.
struct ContentFolderNode
{
    std::string name;
    std::string relative;
    std::vector<std::size_t> children;
    // Holds anything at all, a folder or a file: drawn filled (the owner).
    bool filled = false;
};

void drawContentFolders(Editor& editor, EditorCommands& commands, const IconAtlas* icons)
{
    ContentTree& tree = editor.content();
    static std::vector<ContentFolderNode> s_folders;
    static std::filesystem::path s_root;
    static core::u64 s_readAt = ~0ull;
    if (s_root != tree.root() || s_readAt != tree.refreshes()) {
        s_root = tree.root();
        s_readAt = tree.refreshes();
        s_folders.clear();
        s_folders.push_back(ContentFolderNode{"content", "", {}});
        std::vector<std::string> found;
        // The folders a file sits directly in: a folder holding only files is
        // as full as one holding folders.
        std::unordered_set<std::string> holdFiles;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator walk(s_root, ec), done; !ec && walk != done;
             walk.increment(ec)) {
            const std::string name = walk->path().filename().string();
            // Hidden folders are the tools', not the project's.
            if (!name.empty() && name.front() == '.') {
                walk.disable_recursion_pending();
                continue;
            }
            if (walk->is_directory(ec))
                found.push_back(walk->path().lexically_relative(s_root).generic_string());
            else
                holdFiles.insert(walk->path().parent_path().lexically_relative(s_root).generic_string());
            // A tree of thousands is a tree nobody reads; the path still gets there.
            if (found.size() >= 4000)
                break;
        }
        std::sort(found.begin(), found.end());
        std::unordered_map<std::string, std::size_t> index{{"", 0}};
        for (const std::string& relative : found) {
            const std::size_t slash = relative.rfind('/');
            const std::string parent = slash == std::string::npos ? std::string() : relative.substr(0, slash);
            const auto up = index.find(parent);
            if (up == index.end())
                continue;
            index.emplace(relative, s_folders.size());
            s_folders[up->second].children.push_back(s_folders.size());
            s_folders.push_back(
                ContentFolderNode{slash == std::string::npos ? relative : relative.substr(slash + 1), relative, {}});
        }
        for (ContentFolderNode& folder : s_folders) {
            // The root's own files are under "." to `lexically_relative`.
            const std::string key = folder.relative.empty() ? std::string(".") : folder.relative;
            folder.filled = !folder.children.empty() || holdFiles.contains(key);
        }
    }

    const std::string current = tree.currentFolder();
    const float glyph = ImGui::GetFontSize();
    std::optional<std::string> go;
    const std::function<void(std::size_t)> node = [&](std::size_t at) {
        const ContentFolderNode& folder = s_folders[at];
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (folder.children.empty())
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (folder.relative == current)
            flags |= ImGuiTreeNodeFlags_Selected;
        // The folder somebody is in is shown, open all the way down to it.
        if (folder.relative.empty() ||
            (!current.empty() && (current == folder.relative || current.starts_with(folder.relative + "/"))))
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        const bool open = ImGui::TreeNodeEx(folder.relative.empty() ? "##content-root" : folder.relative.c_str(), flags,
                                            "%s%s", tabIconPad().c_str(), folder.name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            go = folder.relative;
        if (ImGui::BeginDragDropTarget()) {
            acceptContentMove(commands, folder.relative);
            ImGui::EndDragDropTarget();
        }
        // The folder's own picture, in the colour somebody gave it -- filled
        // when there is something in it.
        const std::string_view folderIcon = folder.filled ? icons::ContentFolderFilled : icons::ContentFolder;
        if (icons != nullptr && icons->ready() && g_device != nullptr) {
            const IconSprite sprite = icons->find(folderIcon, static_cast<core::u32>(glyph + 0.5f));
            if (SDL_GPUTexture* texture = rhi::nativeTexture(*g_device, icons->texture());
                sprite.valid && texture != nullptr) {
                ImVec4 tint = iconTint(icons, folderIcon);
                if (const std::optional<core::Color3> own = editor.contentColor(folder.relative); own.has_value())
                    tint = ImVec4(own->r, own->g, own->b, tint.w);
                const ImVec2 corner(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                                    ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - glyph) * 0.5f);
                ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)),
                                                     corner, ImVec2(corner.x + glyph, corner.y + glyph),
                                                     ImVec2(sprite.u0, sprite.v0), ImVec2(sprite.u1, sprite.v1),
                                                     ImGui::GetColorU32(tint));
            }
        }
        if (open) {
            for (const std::size_t child : folder.children)
                node(child);
            ImGui::TreePop();
        }
    };
    node(0);
    // After the walk, which reads the tree it would change.
    if (go.has_value() && *go != current)
        (void)tree.open(tree.root(), *go);
}

void drawContent(Editor& editor, EditorCommands& commands, EditorPanels& panels, EditorDialogs& dialogs,
                 const IconAtlas* icons, const scene::World* world, const Inspector* inspector)
{
    if (!ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.content")) + "###Content").c_str(),
                      &panels.content)) {
        ImGui::End();
        return;
    }

    ContentTree& tree = editor.content();
    const float toolbarIcon = ImGui::GetTextLineHeight();

    // **One line, two heights, and the taller one decides.** Everything on this
    // row is drawn at text height -- the icons, the separators, the path's own
    // words -- and the search box at the end of it is a FRAMED widget, taller by
    // twice the theme's padding. Left alone the short things sit at the top of
    // the line and the box hangs below them, which is what a person sees as a
    // toolbar that is not lined up.
    //
    // `AlignTextToFramePadding` fixes the TEXT and only the text: it moves the
    // line's baseline, and an `ImageButton` is not placed against a baseline. So
    // the icons are nudged by hand, and `onRow` is that nudge -- called before
    // each of them rather than once, because each is its own item.
    ImGui::AlignTextToFramePadding();
    const auto onRow = [](float itemHeight) {
        const float slack = ImGui::GetFrameHeight() - itemHeight;
        if (slack > 0.0f)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + slack * 0.5f);
    };

    ImGui::BeginDisabled(tree.atRoot());
    if (iconButton(icons, icons::ActionUp, toolbarIcon, "up", core::tr(ENG_TR("engine.editor.content.up")),
                   core::tr(ENG_TR("engine.editor.content.up_one_folder_tip")), true, true))
        (void)tree.leave();
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (iconButton(icons, icons::ActionRefresh, toolbarIcon, "refresh",
                   core::tr(ENG_TR("engine.editor.content.refresh")),
                   core::tr(ENG_TR("engine.editor.content.re_read_this_folder_tip")), true, true))
        (void)tree.refresh();

    ImGui::SameLine();
    // The shell's dialog, so the toolbar button and the folder's own
    // right-click menu reach the same one. A FOLDER rather than a generic
    // "new", because what it makes is a folder and the picture can say so.
    if (iconButton(icons, icons::ActionNewFolder, toolbarIcon, "new-folder",
                   core::tr(ENG_TR("engine.editor.content.new_folder")),
                   core::tr(ENG_TR("engine.editor.content.new_folder_tip")), true, true))
        dialogs.newFolder = true;

    // **One "new stamp", and the picker says which kind.** A verb per class would
    // be a toolbar that grew a button every time the engine grew a class -- and
    // the question "which class" already has an answer somewhere: the Explorer's
    // add-a-child menu. Same picker, same keyboard, same prose on hover.
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (iconButton(icons, icons::ActionAdd, toolbarIcon, "new-stamp",
                   core::tr(ENG_TR("engine.editor.content.new_stamp")),
                   core::tr(ENG_TR("engine.editor.content.new_stamp_tip")), true, true))
        dialogs.pickStampClass = true;
    // Drained HERE, beside the `BeginPopup` it opens, so the popup lands in this
    // window's ID scope whichever control asked for it.
    if (dialogs.pickStampClass) {
        dialogs.pickStampClass = false;
        ImGui::OpenPopup("new-stamp-class");
    }
    if (ImGui::BeginPopup("new-stamp-class")) {
        // Null while nothing is being inspected -- a host with no world -- and
        // then there is no list of classes to offer.
        if (world != nullptr && inspector != nullptr) {
            if (const scene::ClassId picked =
                    drawClassPicker(*world, *inspector, core::InstanceId{}, icons, ImGui::GetStyle().ItemSpacing);
                picked != scene::InvalidClass) {
                dialogs.newStampClass = picked;
                dialogs.newStampFromClass = true;
            }
        }
        ImGui::EndPopup();
    }

    // **Bringing a file in from the machine**, which is the other half of a
    // content browser: a project's assets come from somewhere, and until now the
    // only way in was a file manager beside the editor. Dropping files on the
    // window does the same thing and lands in the same folder.
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(!platform::canPickFolder());
    if (iconButton(icons, icons::ActionImport, toolbarIcon, "import", core::tr(ENG_TR("engine.editor.content.import")),
                   core::tr(ENG_TR("engine.editor.content.import_files_into_this_folder_tip")), true, true))
        commands.importAssets = true;
    ImGui::EndDisabled();

    // --- Where you are, as a row of steps you can click back through ---------
    //
    // **A path is a chain and the chain is the navigation.** Printing it as
    // text says where you are and offers nothing; every browser worth using
    // makes each step a way back to it, and the only thing that costs is
    // splitting a string somebody is already reading.
    ImGui::SameLine();
    ImGui::TextDisabled("|");

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    // **The icon AND the word, on every step including this one.** An icon
    // button's word is only its fallback for a missing atlas, so the first
    // step wore a picture and no name -- which reads as a button nobody named
    // rather than as the root of the chain.
    onRow(toolbarIcon);
    (void)drawIcon(icons, icons::ContentFolder, toolbarIcon);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (tree.atRoot()) {
        // Where you already are is a label, not a control: the step that goes
        // nowhere is drawn the same way at the end of the chain.
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.content.content")));
    }
    else if (crumbButton(core::tr(ENG_TR("engine.editor.content.content")))) {
        while (!tree.atRoot())
            (void)tree.leave();
    }
    if (ImGui::BeginDragDropTarget()) {
        acceptContentMove(commands, "");
        ImGui::EndDragDropTarget();
    }

    {
        // A COPY, and navigation deferred to after the loop. Both are load
        // bearing: the copy because `leave` assigns to the tree's own path, and
        // the deferral because a chain that shortened halfway through would go
        // on drawing crumbs for a folder it had already left.
        const std::string relative = tree.currentFolder();
        int climb = 0;
        // Counted first, because a step's job is "go up (depth - me - 1)
        // times" and it has to know how deep the chain is to say that.
        int depth = relative.empty() ? 0 : 1;
        for (const char c : relative)
            depth += c == '/' ? 1 : 0;

        std::size_t begin = 0;
        for (int step = 0; step < depth; ++step) {
            const std::size_t slash = relative.find('/', begin);
            const std::string segment =
                relative.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
            // The path AS FAR AS THIS STEP, which is what a folder's colour is
            // keyed by -- a step is a folder, and the same folder wears the
            // same colour wherever it is drawn.
            const std::string here = slash == std::string::npos ? relative : relative.substr(0, slash);
            begin = slash == std::string::npos ? relative.size() : slash + 1;

            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

            ImGui::PushID(step);
            const bool last = step == depth - 1;
            // **A folder icon per step, not one at the front.** Every step in
            // the chain IS a folder, and a row that pictures only the first of
            // them says the rest are something else -- coloured, because a
            // folder somebody coloured is the same folder here.
            onRow(toolbarIcon);
            (void)drawIcon(icons, icons::ContentFolder, toolbarIcon, editor.contentColor(here));
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            // The one you are IN is not a button: it goes nowhere, and a
            // control that does nothing is worse than a label.
            if (last) {
                ImGui::TextUnformatted(segment.c_str());
            }
            else if (crumbButton(segment.c_str())) {
                climb = depth - step - 1;
            }
            if (!last && ImGui::BeginDragDropTarget()) {
                acceptContentMove(commands, here);
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        }

        for (int up = 0; up < climb; ++up)
            (void)tree.leave();
    }

    // **What you are looking for, in the folder you are in.**
    //
    // Per folder rather than across the tree, because that is what the panel
    // shows: a browser that answered with matches from somewhere else would be
    // a search result wearing a folder's chrome. Cleared when you leave the
    // folder, for the same reason -- a filter that survived the move would hide
    // most of wherever you arrived, and nothing on screen would say why.
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x * 2.0f);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x * 2.0f);
    static std::array<char, 96> contentFilter{};
    static std::string contentFilterFolder;
    if (contentFilterFolder != tree.currentFolder()) {
        contentFilterFolder = tree.currentFolder();
        contentFilter.fill(0);
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    searchField(icons, "##content-filter", core::tr(ENG_TR("engine.editor.content.search")), contentFilter.data(),
                contentFilter.size());
    const std::string_view contentNeedle{contentFilter.data()};

    // **The view, as one button that opens the three.**
    //
    // Three buttons in a row was what this was and it read as a control panel
    // for a choice somebody makes twice a day. One button wearing the current
    // view's own icon says what is on without being asked, and the list behind
    // it says what else there is -- which is what Unreal's browser does with
    // the same question.
    {
        const char* names[] = {core::tr(ENG_TR("engine.editor.content.view.list")),
                               core::tr(ENG_TR("engine.editor.content.view.tiles")),
                               core::tr(ENG_TR("engine.editor.content.view.icons"))};
        const std::string_view viewIcon =
            panels.contentView == EditorPanels::ContentView::List ? icons::ActionList : icons::ActionGrid;

        const float room = ImGui::GetContentRegionAvail().x;
        const float wanted = toolbarIcon + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (room > wanted) {
            ImGui::SameLine(ImGui::GetCursorPosX() + room - wanted);
            if (iconButton(icons, viewIcon, toolbarIcon, "view", names[static_cast<int>(panels.contentView)],
                           core::tr(ENG_TR("engine.editor.content.how_entries_are_laid_out_tip")), true, true)) {
                ImGui::OpenPopup("view-menu");
            }
        }
        if (ImGui::BeginPopup("view-menu")) {
            for (int index = 0; index < 3; ++index) {
                const auto view = static_cast<EditorPanels::ContentView>(index);
                if (ImGui::MenuItem(names[index], nullptr, panels.contentView == view)) {
                    panels.contentView = view;
                    // Written through to the editor, which is the one that has a
                    // file: this panel's copy dies with the run.
                    editor.setContentView(view);
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::Separator();

    // Beside the grid when there is width for both -- which is the bottom of the
    // screen, where the browser lives -- and not in a narrow column.
    const float browserScale = ImGui::GetStyle().FontScaleMain;
    if (ImGui::GetContentRegionAvail().x > 640.0f * browserScale) {
        const float folderWidth =
            std::clamp(ImGui::GetContentRegionAvail().x * 0.2f, 170.0f * browserScale, 280.0f * browserScale);
        if (ImGui::BeginChild("folders", ImVec2(folderWidth, 0.0f), ImGuiChildFlags_ResizeX))
            drawContentFolders(editor, commands, icons);
        ImGui::EndChild();
        ImGui::SameLine();
    }

    if (ImGui::BeginChild("entries")) {
        // What the filter left. Pointers into the tree's own vector, which
        // outlives this frame -- and a list rather than a test inside the loop,
        // because the grid indexes by position and a skipped entry would leave
        // a hole in it.
        static std::vector<const ContentEntry*> entryList;
        entryList.clear();
        for (const ContentEntry& candidate : tree.entries()) {
            if (contentNeedle.empty() || containsFold(candidate.name, contentNeedle))
                entryList.push_back(&candidate);
        }
        const std::vector<const ContentEntry*>& entries = entryList;
        const ContentLayout layout = contentLayoutFor(panels.contentView, ImGui::GetContentRegionAvail().x);
        const float entryHeight = layout.cell.y;

        // No vertical item spacing while the rows are drawn: a `Selectable`
        // pads its highlight with half of it on each side, which at an exact
        // pitch has nowhere to go but into the neighbours. Same rule as the
        // explorer's, same reason.
        const ImVec2 entrySpacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(entrySpacing.x, layout.nameBelow ? entrySpacing.y : 0.0f));

        const int columns = layout.columns;
        const int rows = (static_cast<int>(entries.size()) + columns - 1) / columns;
        const float pitch = entryHeight + (layout.nameBelow ? entrySpacing.y : 0.0f);
        const float strideX = layout.cell.x + entrySpacing.x;

        // **Every cell is placed outright, and `SameLine` is not used at all.**
        //
        // It was, and it produced a staircase: `SameLine` returns the cursor to
        // the previous ITEM's line, and each cell ends by putting the cursor a
        // row lower, so every cell after the first started one row down and one
        // column right of where it belonged. The Explorer learned the same
        // lesson one panel over -- a layout at an exact pitch has to compute the
        // position rather than accumulate it.
        const ImVec2 gridOrigin = ImGui::GetCursorPos();

        // **The whole grid's extent, claimed before a single cell is drawn.**
        // `SetCursorPos` moves the cursor without submitting anything, so
        // without this the child never learns how tall its contents are: it
        // cannot scroll, and ImGui says so once per row per frame. A `Dummy`
        // has no id, so it takes neither a click nor a hover from the cells
        // drawn over it.
        ImGui::Dummy(ImVec2(layout.cell.x > 0.0f ? strideX * static_cast<float>(columns) : 0.0f,
                            pitch * static_cast<float>(rows)));
        ImGui::SetCursorPos(gridOrigin);

        // **A filter that hides everything says so.** An empty folder and a
        // folder filtered down to nothing look identical, and one of them means
        // there is nothing here while the other means you are not looking at it.
        if (entries.empty() && !tree.entries().empty()) {
            ImGui::SetCursorPos(gridOrigin);
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.content.nothing_here_matches"),
                                               {{"filter", std::string_view(contentFilter.data())}})
                                          .c_str());
        }

        ImGuiListClipper clipper;
        clipper.Begin(rows, pitch);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const int index = row * columns + column;
                    if (index >= static_cast<int>(entries.size()))
                        break;
                    const ContentEntry& entry = *entries[static_cast<std::size_t>(index)];
                    ImGui::PushID(index);

                    const ImVec2 entryOrigin(gridOrigin.x + static_cast<float>(column) * strideX,
                                             gridOrigin.y + static_cast<float>(row) * pitch);
                    ImGui::SetCursorPos(entryOrigin);
                    const float entryIcon = layout.icon;

                    const bool isOpenScene = entry.kind == ContentKind::Scene && entry.path == editor.openScenePath();
                    if (isOpenScene)
                        ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().accent));

                    // Double-click, because a single click is how somebody
                    // browses and opening a scene throws away what is in the
                    // world.
                    if (ImGui::Selectable("##entry", isOpenScene,
                                          ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
                                          layout.cell) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (entry.kind == ContentKind::Folder)
                            (void)tree.enter(entry.name);
                        else if (entry.kind == ContentKind::Scene)
                            openSceneOrAsk(editor, commands, dialogs, entry.path);
                        // **Opening a stamp EDITS it**, the way opening a
                        // scene edits a scene -- which is the pairing that
                        // makes the browser one idea rather than two. Placing
                        // one in the world is the right-click, because it is
                        // the thing you do to a world rather than to a file.
                        else if (entry.kind == ContentKind::Stamp)
                            commands.openStamp = entry.path;
                        // A material opens in the material panel: a file
                        // somebody edits, with its own undo (ADR 0090).
                        else if (entry.kind == ContentKind::Material)
                            commands.openMaterial = entry.path;
                        // A surface shader, or an include one reads, opens
                        // in the text editor beside the scripts (ADR 0091).
                        else if (entry.kind != ContentKind::Folder &&
                                 scriptLanguageOf(entry.name) == ScriptLanguage::Hlsl)
                            commands.openFile = entry.path;
                    }

                    // **What KIND of thing this stamp is, on hover.** A stamp
                    // is a file of an instance, and which instance decides
                    // everything about it: what a `Material` property will
                    // accept, what dropping it in the world makes, whether it
                    // is a character or a lamp post. The icon says it in a
                    // picture and this says it in a word, because a picture is
                    // a guess until somebody has learnt the set.
                    if (ImGui::IsItemHovered()) {
                        const std::string shownName = ContentTree::displayNameOf(entry);
                        if (entry.kind == ContentKind::Stamp && !entry.rootClass.empty()) {
                            ImGui::SetTooltip("%s -- a %s", shownName.c_str(), entry.rootClass.c_str());
                        }
                        else if (const char* label = contentKindLabel(entry.kind); label[0] != '\0') {
                            ImGui::SetTooltip("%s -- %s", shownName.c_str(), label);
                        }
                    }

                    // **A prefab is dragged out of here into the world**, which
                    // is what a Project window is for: drop it on a row of the
                    // Explorer to place it there, or on the viewport to place
                    // it under `Workspace`.
                    // A material is dragged too -- onto a part's `Material`
                    // field, an Explorer row or the part in the viewport, and
                    // the part wears it.
                    // And a folder, which a `Sky` takes as its six pictures.
                    // And **everything drags onto another folder**, which
                    // moves it there (the owner) -- a mesh onto a
                    // `MeshContent`, a sound onto a `Sound` too.
                    if (entry.path.size() < sizeof(ContentDrag::path) &&
                        ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                        ContentDrag payload;
                        (void)std::snprintf(payload.path, sizeof(payload.path), "%s", entry.path.c_str());
                        (void)std::snprintf(payload.rootClass, sizeof(payload.rootClass), "%s",
                                            entry.kind == ContentKind::Folder ? "" : entry.rootClass.c_str());
                        payload.folder = entry.kind == ContentKind::Folder;
                        ImGui::SetDragDropPayload(kContentDragPayload, &payload, sizeof(payload));
                        (void)drawIcon(icons, contentKindIcon(entry.kind), ImGui::GetFontSize());
                        ImGui::SameLine();
                        ImGui::TextUnformatted(ContentTree::displayNameOf(entry).c_str());
                        ImGui::EndDragDropSource();
                    }
                    // A folder takes what is dropped on it: a file or folder
                    // moves in, and an instance from the Explorer becomes a
                    // stamp in it rather than in the folder around it.
                    if (entry.kind == ContentKind::Folder && ImGui::BeginDragDropTarget()) {
                        acceptContentMove(commands, entry.path);
                        if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(kInstanceDragPayload);
                            dropped != nullptr) {
                            commands.stampSubject = static_cast<const InstanceDrag*>(dropped->Data)->id;
                            commands.stampFolder = entry.path;
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (ImGui::BeginPopupContextItem("entry-menu")) {
                        // The rows are drawn with no vertical spacing; a menu is
                        // not a row.
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, entrySpacing);
                        if (entry.kind == ContentKind::Scene &&
                            iconMenuItem(icons, icons::ActionOpen, core::tr(ENG_TR("engine.editor.content.open"))))
                            openSceneOrAsk(editor, commands, dialogs, entry.path);
                        if (entry.kind == ContentKind::Material) {
                            if (iconMenuItem(icons, icons::ActionOpen, core::tr(ENG_TR("engine.editor.content.open"))))
                                commands.openMaterial = entry.path;
                            // **A variant**: a material that names this one and
                            // changes only what it writes, so editing this one
                            // reaches it (ADR 0090).
                            if (iconMenuItem(icons, icons::ActionMaterialVariant,
                                             core::tr(ENG_TR("engine.editor.content.new_variant")))) {
                                dialogs.newMaterial = true;
                                dialogs.newMaterialParent = entry.path;
                            }
                        }
                        if (entry.kind != ContentKind::Folder && scriptLanguageOf(entry.name) == ScriptLanguage::Hlsl &&
                            iconMenuItem(icons, icons::ActionOpen, core::tr(ENG_TR("engine.editor.content.open"))))
                            commands.openFile = entry.path;
                        if (entry.kind == ContentKind::Stamp) {
                            if (iconMenuItem(icons, icons::ActionOpen, core::tr(ENG_TR("engine.editor.content.open"))))
                                commands.openStamp = entry.path;
                            // **Two ways to place one, and both are real.** A
                            // linked instance follows the file; a copy is its
                            // own from the first frame. A lamp post you will
                            // place forty of wants the first; a starting point
                            // you are about to rebuild wants the second.
                            if (iconMenuItem(icons, icons::OverlayStamp,
                                             core::tr(ENG_TR("engine.editor.content.place_linked")))) {
                                commands.placeStamp = entry.path;
                                commands.placeStampLinked = true;
                            }
                            if (iconMenuItem(icons, icons::ActionDuplicate,
                                             core::tr(ENG_TR("engine.editor.content.place_a_copy")))) {
                                commands.placeStamp = entry.path;
                                commands.placeStampLinked = false;
                            }
                        }
                        // **A directory cannot carry a colour**, so this one is
                        // kept in `.engine/editor.json` keyed by path, while a
                        // folder in the WORLD carries its own as an attribute.
                        // Two stores, because the two things are different --
                        // see `Editor::setFolderColor`.
                        if (entry.kind == ContentKind::Folder &&
                            ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.content.colour")))) {
                            std::optional<core::Color3> chosen = editor.contentColor(entry.path);
                            if (colorMenu(chosen)) {
                                commands.colorAsked = true;
                                commands.colorTarget = {};
                                commands.colorContentPath = entry.path;
                                commands.color = chosen;
                            }
                            ImGui::EndMenu();
                        }
                        if (iconMenuItem(icons, icons::ActionRename,
                                         core::tr(ENG_TR("engine.editor.content.rename")))) {
                            dialogs.renameTarget = {};
                            dialogs.renameContentPath = entry.path;
                            dialogs.renameSeed = ContentTree::stemOf(entry);
                            dialogs.renameContent = true;
                        }
                        // **How a material is made from a material.** Start
                        // from the one that is nearly right, change what is
                        // not, and everything already pointed at the original
                        // keeps working -- which is the whole reason a material
                        // is a file rather than a property.
                        //
                        // Not special-cased to materials: a scene, a stamp and
                        // a folder duplicate the same way and for the same
                        // reason.
                        if (iconMenuItem(icons, icons::ActionDuplicate,
                                         core::tr(ENG_TR("engine.editor.content.duplicate")), "Ctrl+D"))
                            commands.duplicateContent = entry.path;
                        ImGui::Separator();
                        // Here as well as on the folder's own menu: a person who
                        // right-clicks lands on whatever was under the pointer,
                        // and which of the two menus they opened is not a
                        // distinction they made on purpose.
                        if (iconMenuItem(icons, icons::ActionImport, core::tr(ENG_TR("engine.editor.content.import_2")),
                                         nullptr, false, platform::canPickFolder()))
                            commands.importAssets = true;
                        if (iconMenuItem(icons, icons::ActionDelete, core::tr(ENG_TR("engine.editor.content.delete"))))
                            dialogs.deleteContentPath = entry.path;
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }

                    const std::optional<core::Color3> tint =
                        entry.kind == ContentKind::Folder ? editor.contentColor(entry.path) : std::nullopt;

                    // Placed on the cell rather than stacked after it, for the
                    // reason the explorer's are: the icon and the text are two
                    // heights and the cell has one shape.
                    // **`lantern-post.stamp`, not `lantern-post.stamp.json`.**
                    // `.json` is how the file is stored and this panel is the
                    // one place that has to say what it IS.
                    const std::string shown = ContentTree::displayNameOf(entry);

                    if (layout.nameBelow) {
                        const float centreX = entryOrigin.x + (layout.cell.x - entryIcon) * 0.5f;
                        ImGui::SetCursorPos(ImVec2(centreX, entryOrigin.y + ImGui::GetStyle().ItemInnerSpacing.y));
                        const ImVec2 iconOrigin = ImGui::GetCursorScreenPos();
                        if (!drawContentThumbnail(tree, entry, entryIcon) &&
                            drawContentIcon(icons, world, entry, entryIcon, tint) && entry.kind == ContentKind::Stamp) {
                            drawIconBadge(icons, iconOrigin, entryIcon);
                        }

                        const std::string label = elideToWidth(shown, layout.cell.x);
                        const float textX =
                            entryOrigin.x + (layout.cell.x - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
                        ImGui::SetCursorPos(
                            ImVec2(textX, entryOrigin.y + entryIcon + ImGui::GetStyle().ItemInnerSpacing.y));
                        ImGui::TextUnformatted(label.c_str());
                        // The kind is what the icon already says, and a grid
                        // cell has no room to say it twice.
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", entry.name.c_str());
                    }
                    else {
                        float entryX = entryOrigin.x;
                        ImGui::SetCursorPos(ImVec2(entryX, entryOrigin.y + (entryHeight - entryIcon) * 0.5f));
                        const ImVec2 iconOrigin = ImGui::GetCursorScreenPos();
                        if (drawContentThumbnail(tree, entry, entryIcon)) {
                            entryX += entryIcon + ImGui::GetStyle().ItemInnerSpacing.x;
                        }
                        else if (drawContentIcon(icons, world, entry, entryIcon, tint)) {
                            // **The same badge the Explorer puts on a stamped
                            // instance**, on the file it stamps from. One mark
                            // for one idea: a person who has learned it in the
                            // hierarchy does not learn it again here, and the
                            // set can owe a `content.Stamp` drawing for as long
                            // as it likes without this row being mute.
                            if (entry.kind == ContentKind::Stamp)
                                drawIconBadge(icons, iconOrigin, entryIcon);
                            entryX += entryIcon + ImGui::GetStyle().ItemInnerSpacing.x;
                        }

                        const float textY = entryOrigin.y + (entryHeight - ImGui::GetTextLineHeight()) * 0.5f;
                        ImGui::SetCursorPos(ImVec2(entryX, textY));
                        ImGui::TextUnformatted(shown.c_str());
                        entryX += ImGui::CalcTextSize(shown.c_str()).x + 12.0f;

                        if (const char* label = contentKindLabel(entry.kind); label[0] != '\0') {
                            ImGui::SetCursorPos(ImVec2(entryX, textY));
                            ImGui::TextDisabled("%s", label);
                        }
                    }

                    // **The colour is popped after the NAME is drawn, not after
                    // the selectable.** It is the text's colour, and the text is
                    // placed on the cell rather than carried by the selectable's
                    // label -- so popping earlier would have coloured nothing.
                    if (isOpenScene)
                        ImGui::PopStyleColor();

                    ImGui::PopID();
                }
            }
        }
        ImGui::PopStyleVar();

        // **An instance dragged in from the Explorer becomes a PREFAB here**,
        // in the folder somebody dropped it in. That is what dragging from the
        // hierarchy into the Project window means in every editor that has
        // both, and it is the shortest sentence in this whole model: a prefab
        // is a thing in the world, saved.
        //
        // On the whole panel rather than on a row, because what it means is
        // "into this FOLDER" -- there is no row it could sensibly land on.
        if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect, ImGui::GetID("content-drop"))) {
            if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(kInstanceDragPayload); dropped != nullptr) {
                const InstanceDrag* incoming = static_cast<const InstanceDrag*>(dropped->Data);
                commands.stampSubject = incoming->id;
                // **The folder, not the name.** The name comes from the
                // instance, which this panel does not know -- it has an id --
                // so the drain composes the two. A dialog would be the other
                // answer and it is the wrong one: a box that opens on a DROP
                // interrupts the gesture that opened it, and renaming a file
                // afterwards is one click.
                commands.stampFolder = tree.currentFolder().empty() ? std::string(kStampFolder) : tree.currentFolder();
            }
            ImGui::EndDragDropTarget();
        }

        // The space below the rows. Right-clicking nothing is how somebody asks
        // about the FOLDER rather than about a thing in it.
        if (ImGui::BeginPopupContextWindow("folder-menu",
                                           ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            // **Where a person looks for it.** A toolbar button is where the
            // thing lives; a right-click is where somebody goes when they have
            // just decided they want it, and an item that is only on the
            // toolbar is one people conclude does not exist.
            if (iconMenuItem(icons, icons::ActionImport, core::tr(ENG_TR("engine.editor.content.import_2")), nullptr,
                             false, platform::canPickFolder()))
                commands.importAssets = true;
            ImGui::Separator();
            if (iconMenuItem(icons, icons::ActionNewFolder, core::tr(ENG_TR("engine.editor.content.new_folder_2"))))
                dialogs.newFolder = true;
            if (iconMenuItem(icons, icons::ClassModel, core::tr(ENG_TR("engine.editor.content.new_stamp_2"))))
                dialogs.pickStampClass = true;
            if (iconMenuItem(icons, icons::ActionNewMaterial, core::tr(ENG_TR("engine.editor.content.new_material")))) {
                dialogs.newMaterial = true;
                dialogs.newMaterialParent.clear();
            }
            if (iconMenuItem(icons, icons::ActionNewShader,
                             core::tr(ENG_TR("engine.editor.content.new_surface_shader"))))
                dialogs.newShader = true;
            // The engine's eight terrain materials as the project's own files,
            // to give a terrain like any other material (D330).
            if (iconMenuItem(icons, icons::ActionNewMaterial,
                             core::tr(ENG_TR("engine.editor.content.new_terrain_starter_materials"))))
                (void)editor.writeStarterTerrainMaterials();
            if (iconMenuItem(icons, icons::ActionRefresh, core::tr(ENG_TR("engine.editor.content.refresh_2"))))
                (void)tree.refresh();
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

bool menuCommand(std::string_view id, const char* label = nullptr, bool checked = false);
void toggleSideBar(EditorPanels& panels);
void openCommandPalette();

// The application menu, which every engine has and which this one did not.
//
// **A menu bar is not decoration: it is where a person looks for a thing they
// cannot see.** A panel closed by accident, a scene saved under a new name, the
// preferences -- none of those have anywhere else to live, and an editor that
// puts them only on toolbar buttons is one where closing a panel is permanent.
//
// Drawn BEFORE the dockspace, because `DockSpaceOverViewport` measures the work
// area and a menu bar declared after it would overlap the panels by its own
// height.
void drawMenuBar(Editor& editor, EditorPanels& panels, EditorCommands& commands, EditorDialogs& dialogs,
                 const IconAtlas* icons)
{
    if (!ImGui::BeginMainMenuBar())
        return;

    // No mark before `File`: the window's own title bar already carries the
    // engine's icon a line above, and two of them read as a mistake.

    // **The menus of the editor this follows**, in its order -- File, Edit,
    // Selection, View, Go, Run, Help -- each item a palette command, so a menu
    // and the palette can never disagree about what a thing does or whether it
    // can be done now.
    (void)commands;
    (void)dialogs;
    if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.menu_bar.file")))) {
        (void)menuCommand("File: New Scene");
        ImGui::Separator();
        (void)menuCommand("File: New Project...");
        (void)menuCommand("File: Open Project...");
        (void)menuCommand("File: Go to File...", core::tr(ENG_TR("engine.editor.command.open_file")));
        ImGui::Separator();
        (void)menuCommand("file.save");
        (void)menuCommand("File: Save Scene As...");
        (void)menuCommand("File: Close Stamp");
        ImGui::Separator();
        (void)menuCommand("File: Export...");
        ImGui::Separator();
        if (iconBeginMenu(icons, icons::ActionSettings, core::tr(ENG_TR("engine.editor.menu_bar.preferences")))) {
            (void)menuCommand("Preferences: Open Settings", core::tr(ENG_TR("engine.editor.command.settings")));
            (void)menuCommand("Preferences: Project Settings",
                              core::tr(ENG_TR("engine.editor.command.project_settings")));
            if (iconBeginMenu(icons, icons::ClassLighting, core::tr(ENG_TR("engine.editor.menu_bar.color_theme")))) {
                for (const Theme& theme : themes()) {
                    (void)menuCommand("theme." + std::string(theme.id), shownName(theme).c_str(),
                                      g_appearance.themeId == theme.id);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        (void)menuCommand("File: Exit");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.menu_bar.edit")))) {
        (void)menuCommand("edit.undo");
        (void)menuCommand("edit.redo");
        ImGui::Separator();
        (void)menuCommand("Edit: Cut");
        (void)menuCommand("Edit: Copy");
        (void)menuCommand("Edit: Paste");
        (void)menuCommand("Edit: Paste Into");
        ImGui::Separator();
        (void)menuCommand("Edit: Duplicate");
        (void)menuCommand("Edit: Rename...");
        (void)menuCommand("Edit: Delete");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.menu_bar.selection")))) {
        (void)menuCommand("Selection: Clear", core::tr(ENG_TR("engine.editor.command.clear_selection")));
        (void)menuCommand("View: Frame Selection");
        ImGui::Separator();
        (void)menuCommand("Edit: Group");
        (void)menuCommand("Edit: Group as Folder");
        (void)menuCommand("Edit: Ungroup");
        ImGui::Separator();
        const bool selecting = editor.tool() == Editor::Tool::Select;
        (void)menuCommand("Tool: Select", nullptr, selecting && !editor.handlesShown());
        (void)menuCommand("Tool: Move", nullptr,
                          selecting && editor.handlesShown() && editor.gizmoMode() == GizmoMode::Translate);
        (void)menuCommand("Tool: Resize", nullptr,
                          selecting && editor.handlesShown() && editor.gizmoMode() == GizmoMode::Scale);
        (void)menuCommand("Tool: Turn", nullptr,
                          selecting && editor.handlesShown() && editor.gizmoMode() == GizmoMode::Rotate);
        ImGui::Separator();
        (void)menuCommand("tool.axes", core::tr(ENG_TR("engine.editor.command.local_axes")), editor.gizmoLocal());
        (void)menuCommand("tool.snap", core::tr(ENG_TR("engine.editor.command.snap_to_grid")), editor.snapping());
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.menu_bar.view")))) {
        if (iconMenuItem(icons, icons::ActionSearch, core::tr(ENG_TR("engine.editor.menu_bar.command_palette")),
                         "Ctrl+Shift+P"))
            openCommandPalette();
        ImGui::Separator();
        (void)menuCommand("View: Toggle Side Bar", core::tr(ENG_TR("engine.editor.command.side_bar")));
        ImGui::Separator();
        const auto panelItem = [&](const char* label, bool visible) {
            (void)menuCommand(std::string("view.") + label, label, visible);
        };
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.explorer")), panels.explorer);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.content")), panels.content);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.debug")), panels.debug);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.properties")), panels.properties);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.console")), panels.console);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.stats")), panels.stats);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.streaming")), panels.streaming);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.saves")), panels.saves);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.viewport")), panels.viewport);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.viewport_settings")), panels.viewportSettings);
        ImGui::Separator();
        if (iconMenuItem(icons, icons::ClassTerrain, core::tr(ENG_TR("engine.editor.menu_bar.terrain")), nullptr,
                         panels.terrain))
            panels.terrain = !panels.terrain;
        if (iconMenuItem(icons, icons::ClassVoxelService, core::tr(ENG_TR("engine.editor.menu_bar.blocks")), nullptr,
                         panels.blocks))
            panels.blocks = !panels.blocks;
        if (iconMenuItem(icons, icons::ClassTilemap2D, core::tr(ENG_TR("engine.editor.menu_bar.tiles")), nullptr,
                         panels.tiles))
            panels.tiles = !panels.tiles;
        if (iconMenuItem(icons, WaterIcon, core::tr(ENG_TR("engine.editor.menu_bar.water")), nullptr, panels.water))
            panels.water = !panels.water;
        ImGui::Separator();
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.grid")), panels.showGrid);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.collision_shapes")), panels.showCollision);
        panelItem(core::tr(ENG_TR("engine.editor.menu_bar.skeletons")), panels.showSkeletons);
        ImGui::Separator();
        (void)menuCommand("View: Reset Layout");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.menu_bar.go")))) {
        (void)menuCommand("File: Go to File...", core::tr(ENG_TR("engine.editor.command.go_to_file")));
        if (iconMenuItem(icons, icons::ActionSearch, core::tr(ENG_TR("engine.editor.menu_bar.go_to_command")),
                         "Ctrl+Shift+P"))
            openCommandPalette();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.menu_bar.run")))) {
        (void)menuCommand("run.start");
        (void)menuCommand("Run: Stop");
        (void)menuCommand("run.pause");
        (void)menuCommand("Run: Step One Tick");
        ImGui::Separator();
        (void)menuCommand("run.camera", core::tr(ENG_TR("engine.editor.command.free_camera")), editor.cameraDetached());
        ImGui::Separator();
        // **The match's shape** (ADR 0106 §5): how many windows play, and
        // whether a server with none runs them. Fixed while one runs.
        const bool locked = editor.inPlayMode() || editor.matchRunning();
        Editor::MatchSettings& match = editor.matchSettings();
        if (iconBeginMenu(icons, icons::ClassPlayer, core::tr(ENG_TR("engine.editor.menu_bar.players")), !locked)) {
            for (int count = 1; count <= 4; ++count) {
                const std::string label = core::tr(ENG_TR("engine.editor.menu_bar.players_count"),
                                                   {{"count", static_cast<core::i64>(count)}});
                if (ImGui::MenuItem(label.c_str(), nullptr, match.players == count))
                    match.players = count;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.menu_bar.dedicated_server")), nullptr, match.dedicated,
                            !locked))
            match.dedicated = !match.dedicated;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.menu_bar.help")))) {
        (void)menuCommand("Help: Welcome");
        if (iconMenuItem(icons, icons::ActionSearch, core::tr(ENG_TR("engine.editor.menu_bar.show_all_commands")),
                         "Ctrl+Shift+P"))
            openCommandPalette();
        ImGui::Separator();
        (void)menuCommand("Help: About", aboutLabel().c_str());
        ImGui::EndMenu();
    }

    // Which scene is open is the status bar's to say now, with whether it is
    // saved.
    ImGui::EndMainMenuBar();
}

// The dialogs, at the shell's level rather than inside a panel.
//
// A popup belongs to the window that opened it, so one living inside the
// viewport could not be opened from the menu -- which is exactly what a Save As
// has to be. Modal, because each of these is a question with an answer, and one
// left half-answered behind a panel is one somebody loses track of.
// --- Project settings (S5.7) --------------------------------------------------
//
// **The PROJECT's settings, which are a different thing from Preferences.** A
// preference is about this person and this machine -- which theme, which panels;
// a project setting is about the game and is committed with it.
//
// Every field here writes ONE key, in place, leaving the rest of `project.toml`
// byte for byte -- comments included. That is not politeness: every project file
// in this repository opens with a paragraph explaining why its settings are what
// they are, and a dialog that serialised the config back would delete all of it
// the first time somebody changed a window title.
//
// Written on Apply rather than per keystroke, because each write is a file write
// and a title typed one letter at a time would be twelve of them -- and because
// a half-typed value briefly in the file is a project the engine briefly
// refuses to open.
// Every PNG in the project a person might mean as its icon: the root and the
// folders an icon is kept in, one level deep -- not the whole content tree,
// which is textures.
std::vector<std::string> projectPictures(const std::filesystem::path& root)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const char* folder : {"", "branding", "assets", "icons", "content"}) {
        const std::filesystem::path directory = root / folder;
        if (!std::filesystem::is_directory(directory, ec))
            continue;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".png")
                out.push_back(std::filesystem::relative(entry.path(), root, ec).generic_string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The icon as each platform shows it (ADR 0104 §1): a desktop tile with its
// label, a taskbar button, and a phone's home screen with the name under it.
void drawIconPreviews(const std::filesystem::path& picture, const char* label)
{
    SDL_GPUTexture* native = nullptr;
    if (!picture.empty() && g_thumbnails != nullptr && g_device != nullptr) {
        const ThumbnailCache::Thumbnail thumb = g_thumbnails->request(picture);
        native = thumb.valid() ? rhi::nativeTexture(*g_device, thumb.texture) : nullptr;
    }
    const float unit = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto picture_ = [&](ImVec2 min, float edge, float rounding) {
        const ImVec2 max(min.x + edge, min.y + edge);
        if (native != nullptr)
            draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)), min, max, ImVec2(0, 0),
                                  ImVec2(1, 1), IM_COL32_WHITE, rounding);
        else
            draw->AddRectFilled(min, max, ImGui::GetColorU32(themeColor(palette().accent)), rounding);
    };
    const auto centred = [&](float x, float width, float y, const char* text, ImU32 colour) {
        const float w = ImGui::CalcTextSize(text).x;
        draw->AddText(ImVec2(x + (width - w) * 0.5f, y), colour, text);
    };

    // Desktop: a 48 px tile on the wallpaper, the name under it.
    const float boxH = unit * 7.0f;
    const float desktopW = unit * 7.0f;
    draw->AddRectFilled(origin, ImVec2(origin.x + desktopW, origin.y + boxH), IM_COL32(40, 70, 120, 255), 4.0f);
    picture_(ImVec2(origin.x + (desktopW - 48.0f) * 0.5f, origin.y + unit), 48.0f, 0.0f);
    centred(origin.x, desktopW, origin.y + unit + 52.0f, label, IM_COL32_WHITE);

    // Taskbar: a strip with a 24 px button.
    const float barX = origin.x + desktopW + unit;
    const float barW = unit * 7.0f;
    draw->AddRectFilled(ImVec2(barX, origin.y + boxH - unit * 2.0f), ImVec2(barX + barW, origin.y + boxH),
                        IM_COL32(28, 28, 32, 255), 4.0f);
    picture_(ImVec2(barX + unit * 2.8f, origin.y + boxH - unit * 2.0f + (unit * 2.0f - 24.0f) * 0.5f), 24.0f, 0.0f);
    centred(barX, barW, origin.y + unit * 1.5f, "taskbar", ImGui::GetColorU32(ImGuiCol_TextDisabled));

    // Phone: a rounded 56 px launcher icon, the label under it.
    const float phoneX = barX + barW + unit;
    const float phoneW = unit * 6.0f;
    draw->AddRectFilled(ImVec2(phoneX, origin.y), ImVec2(phoneX + phoneW, origin.y + boxH), IM_COL32(18, 18, 22, 255),
                        unit);
    picture_(ImVec2(phoneX + (phoneW - 56.0f) * 0.5f, origin.y + unit), 56.0f, 14.0f);
    centred(phoneX, phoneW, origin.y + unit + 60.0f, label, IM_COL32(230, 230, 230, 255));

    ImGui::Dummy(ImVec2(phoneX + phoneW - origin.x, boxH));
}

void drawProjectSettings(Editor& editor)
{
    if (!beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.project_settings"), "###Project Settings").c_str(),
                           520.0f, []() {}))
        return;

    const std::filesystem::path root = editor.content().root().parent_path();

    static std::array<char, 128> name{};
    static std::array<char, 128> title{};
    static std::array<char, 96> identity{};
    static std::array<int, 2> size{};
    static int quality = 2;
    // ADR 0096's two machine switches: whether this project's worlds draw their
    // depth of field and sun rays by default. Blur and colour correction have
    // none -- a game uses them to say something.
    static bool depthOfField = true;
    static bool sunRays = true;
    // The App page (ADR 0104 §1): what every export stamps.
    static std::array<char, 32> version{};
    static std::array<char, 96> company{};
    static std::array<char, 200> icon{};
    static bool fullscreen = false;
    static bool resizable = true;
    static std::vector<std::string> pictures;
    static std::string problem;

    // Seeded on the frame it opens, and only then: re-reading every frame would
    // overwrite what somebody is typing with what is still on disk.
    if (dialogOpening()) {
        problem.clear();
        name.fill(0);
        title.fill(0);
        identity.fill(0);

        const ProjectConfig config = loadProjectConfig(root, GraphicsOverrides{});
        (void)std::snprintf(name.data(), name.size(), "%s", config.name.c_str());
        (void)std::snprintf(title.data(), title.size(), "%s", config.windowTitle.c_str());
        (void)std::snprintf(identity.data(), identity.size(), "%s", config.id.c_str());
        size[0] = config.windowWidth > 0 ? config.windowWidth : 1280;
        size[1] = config.windowHeight > 0 ? config.windowHeight : 720;
        quality = static_cast<int>(config.graphics.quality);
        depthOfField = config.graphics.depthOfField;
        sunRays = config.graphics.sunRays;
        version.fill(0);
        company.fill(0);
        icon.fill(0);
        (void)std::snprintf(version.data(), version.size(), "%s", config.version.c_str());
        (void)std::snprintf(company.data(), company.size(), "%s", config.company.c_str());
        (void)std::snprintf(icon.data(), icon.size(), "%s", config.icon.c_str());
        fullscreen = config.fullscreen;
        resizable = config.resizable;
        pictures = projectPictures(root);
    }

    ImGui::TextWrapped("%s", root.filename().string().c_str());
    ImGui::Spacing();

    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.project_settings.project")));
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.project_settings.project_name")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##name", core::tr(ENG_TR("engine.editor.project_settings.what_the_project_calls_itself")),
                             name.data(), name.size());
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.project_settings.application_id")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##id", core::tr(ENG_TR("engine.editor.project_settings.reverse_dns_identity_e_g")),
                             identity.data(), identity.size());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.project_settings.on_windows_this_is_what_tip")));
    }

    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.project_settings.app")));
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.project_settings.version")));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    ImGui::InputTextWithHint("##version", "1.0.0", version.data(), version.size());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.project_settings.x_y_z_the_windows_tip")));
    ImGui::SameLine();
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.project_settings.company")));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##company", core::tr(ENG_TR("engine.editor.project_settings.who_made_it")),
                             company.data(), company.size());
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.project_settings.icon")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##icon", icon[0] != '\0'
                                        ? icon.data()
                                        : core::tr(ENG_TR("engine.editor.project_settings.the_engine_s")))) {
        if (ImGui::Selectable(core::tr(ENG_TR("engine.editor.project_settings.the_engine_s")), icon[0] == '\0'))
            icon.fill(0);
        for (const std::string& picture : pictures) {
            if (ImGui::Selectable(picture.c_str(), picture == icon.data()))
                (void)std::snprintf(icon.data(), icon.size(), "%s", picture.c_str());
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.project_settings.one_square_png_1024_px_tip")));
    drawIconPreviews(icon[0] != '\0' ? root / std::filesystem::path(icon.data()) : std::filesystem::path{},
                     name.data());

    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.project_settings.window")));
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.project_settings.window_title")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##title", core::tr(ENG_TR("engine.editor.project_settings.the_window_s_title_bar")),
                             title.data(), title.size());
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.project_settings.resolution")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt2("##size", size.data());
    ImGui::Checkbox(core::tr(ENG_TR("engine.editor.project_settings.fullscreen")), &fullscreen);
    ImGui::SameLine();
    ImGui::Checkbox(core::tr(ENG_TR("engine.editor.project_settings.resizable")), &resizable);

    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.project_settings.graphics")));
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.project_settings.quality")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##quality", &quality,
                 choices({ENG_TR("engine.editor.project_settings.quality.low"),
                          ENG_TR("engine.editor.project_settings.quality.medium"),
                          ENG_TR("engine.editor.project_settings.quality.high"),
                          ENG_TR("engine.editor.project_settings.quality.ultra")})
                     .c_str());
    ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.project_settings.default_quality_for_this_project")));
    ImGui::Checkbox(core::tr(ENG_TR("engine.editor.project_settings.depth_of_field")), &depthOfField);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          core::tr(ENG_TR("engine.editor.project_settings.whether_a_depthoffieldeffect_in_the_tip")));
    ImGui::Checkbox(core::tr(ENG_TR("engine.editor.project_settings.sun_rays")), &sunRays);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.project_settings.whether_a_sunrayseffect_in_the_tip")));

    if (!problem.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().danger));
        ImGui::TextWrapped("%s", problem.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (dialogButton(core::tr(ENG_TR("engine.editor.project_settings.apply")), ImVec2(120.0f, 0.0f))) {
        problem.clear();
        const std::array<core::f64, 2> extent{static_cast<core::f64>(size[0]), static_cast<core::f64>(size[1])};
        static constexpr std::array<const char*, 4> Presets{"low", "medium", "high", "ultra"};
        const int chosen = quality >= 0 && quality < static_cast<int>(Presets.size()) ? quality : 2;

        // **One key at a time, and it stops at the first refusal.** A dialog
        // that pressed on after a failed write would leave the file half
        // changed, which is the one state worse than not saving.
        const std::array<std::pair<const char*, std::string>, 12> writes{{
            {"project.name", core::tomlString(std::string_view(name.data()))},
            {"project.id", core::tomlString(std::string_view(identity.data()))},
            {"project.version", core::tomlString(std::string_view(version.data()))},
            {"project.company", core::tomlString(std::string_view(company.data()))},
            {"project.icon", core::tomlString(std::string_view(icon.data()))},
            {"window.title", core::tomlString(std::string_view(title.data()))},
            {"window.size", core::tomlNumberArray(extent)},
            {"window.fullscreen", core::tomlBoolean(fullscreen)},
            {"window.resizable", core::tomlBoolean(resizable)},
            {"graphics.quality", core::tomlString(Presets[static_cast<std::size_t>(chosen)])},
            {"graphics.depth_of_field", core::tomlBoolean(depthOfField)},
            {"graphics.sun_rays", core::tomlBoolean(sunRays)},
        }};

        bool ok = true;
        for (const auto& [key, rendered] : writes) {
            // A version that is not there is not written: an empty one is a
            // value the loader refuses, where no key means "unversioned".
            if (std::string_view(key) == "project.version" && version[0] == '\0')
                continue;
            if (!writeProjectSetting(root, key, rendered, &problem)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            // **Not applied to the running window**, and the dialog says so. A
            // project file names what a RUN starts with; changing it while one
            // is open would make the editor's window disagree with the file
            // that describes it, and re-reading it mid-session is a different
            // feature.
            editor.report(core::tr(ENG_TR("engine.editor.project_settings.saved_window_follows")), false);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (dialogButton(core::tr(ENG_TR("engine.editor.project_settings.cancel")), ImVec2(120.0f, 0.0f)) ||
        dialogCancelled())
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

// --- Export (ADR 0104 §4) -----------------------------------------------------
//
// **The editor runs the exporter; it does not re-implement it.** Every button
// here is `ludwerk build` with flags, started as a worker process, and every step
// on screen is a line it wrote -- so the Export window and a CI script make the
// same bytes. A window rather than a modal, because an export takes minutes and
// the editor stays usable while it runs.
//
// The settings on the right are the project's (`[export.*]` in `project.toml`),
// each written in place the moment it is changed, as Project Settings does; the
// identity above them is shown read-only, with a link to where it is set.
struct ExportUi
{
    bool open = false;
    std::filesystem::path root;
    std::optional<CliCommand> cli;

    // `ludwerk build --status`, asked when the window opens and after an install.
    std::unique_ptr<platform::ChildProcess> statusProcess;
    std::string statusOutput;
    std::optional<ExportStatus> status;

    // A phone on adb, asked every few seconds while the window is open.
    std::unique_ptr<platform::ChildProcess> adbProcess;
    std::string adbOutput;
    std::string phone;
    double adbCheckedAt = -100.0;

    std::string selected = "windows";

    // The project's `[export]` settings, as last read or written.
    int multiplayer = 0; // none, host, dedicated
    // `[export] ship_source` (ADR 0112): scripts as source rather than bytecode.
    bool shipSource = false;
    std::array<char, 128> server{};
    std::array<char, 96> windowsExecutable{};
    std::array<char, 96> linuxExecutable{};
    std::array<char, 128> package{};
    int versionCode = 1;
    // The start scene's `UIService.ScreenOrientation`, which is what the APK
    // is held at from its first frame -- shown, not set here (`android.luau`).
    std::string orientation = "LandscapeSensor";
    std::array<char, 16> background{};
    bool release = false;
    std::array<char, 200> keystore{};
    std::array<char, 64> keyAlias{};
    std::array<char, 128> password{};
    bool rememberPassword = false;
    bool bumpVersionCode = false;
    std::array<char, 260> output{};
    std::string problem;

    // The export running, and the ones waiting behind it (Export all).
    std::unique_ptr<platform::ChildProcess> run;
    std::string running;
    ExportRun progress;
    double startedAt = 0.0;
    std::vector<std::string> queue;

    struct Finished
    {
        std::string target;
        bool ok = false;
        ExportResult result;
        core::u64 bytes = 0;
        double seconds = 0.0;
        std::string failure;
        std::string log;
    };
    std::vector<Finished> finished;
    double celebrateAt = -100.0;
    bool showLog = false;
    std::string logText;

    // Install on phone: `adb install -r`, then a launch.
    std::unique_ptr<platform::ChildProcess> install;
    std::string installOutput;
    std::string installPackage;
    int installStage = 0; // 0 idle, 1 installing, 2 launching
    std::string installMessage;

    // Install Android tools.
    std::unique_ptr<platform::ChildProcess> tools;
    std::string toolsOutput;
    std::string toolsMessage;

    // Create keystore...
    bool keystoreDialog = false;
    std::array<char, 64> newAlias{};
    std::array<char, 96> newName{};
    std::array<char, 128> newPassword{};
    std::array<char, 128> newConfirm{};
    int newYears = 25;
    bool newRemember = true;
    std::unique_ptr<platform::ChildProcess> keystoreProcess;
    std::string keystoreOutput;
    std::string keystoreProblem;

    std::vector<RecentExport> recent;
    // What Run it started; killed with the editor, as a match's windows are.
    std::vector<std::unique_ptr<platform::ChildProcess>> launched;
};

ExportUi& exportUi()
{
    static ExportUi ui;
    return ui;
}

#if defined(_WIN32)
constexpr bool kWindowsHost = true;
#else
constexpr bool kWindowsHost = false;
#endif

constexpr std::array<const char*, 3> kMultiplayerModes{"none", "host", "dedicated"};

template <std::size_t N>
void copyInto(std::array<char, N>& buffer, std::string_view text)
{
    buffer.fill(0);
    const std::size_t count = std::min(text.size(), N - 1);
    std::memcpy(buffer.data(), text.data(), count);
}

[[nodiscard]] std::string humanSize(core::u64 bytes)
{
    if (bytes >= 1024ull * 1024ull)
        return core::tr(ENG_TR("engine.editor.unit.megabytes"),
                        {{"size", fixed(static_cast<double>(bytes) / (1024.0 * 1024.0), 1)}});
    return core::tr(ENG_TR("engine.editor.unit.kilobytes"), {{"size", fixed(static_cast<double>(bytes) / 1024.0, 0)}});
}

[[nodiscard]] std::string nowText()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    (void)localtime_s(&local, &now);
#else
    (void)localtime_r(&now, &local);
#endif
    char text[32];
    (void)std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

void openFolder(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
    const std::string url = "file:///" + folder.generic_string();
    (void)SDL_OpenURL(url.c_str());
}

// The project's `[export]` settings, read into the window.
void readExportSettings(ExportUi& ui)
{
    std::string text;
    core::TomlDocument document;
    if (!platform::readTextFile(ui.root / "project.toml", text) || !document.parse(text).ok)
        return;
    const auto stringOf = [&](std::string_view key, std::string_view fallback = {}) {
        return std::string(document.string(key).value_or(fallback));
    };
    const std::string mode = stringOf("export.multiplayer", "none");
    ui.multiplayer = mode == "dedicated" ? 2 : mode == "host" ? 1 : 0;
    ui.shipSource = document.boolean("export.ship_source").value_or(false);
    copyInto(ui.server, stringOf("network.server"));
    copyInto(ui.windowsExecutable, stringOf("export.windows.executable"));
    copyInto(ui.linuxExecutable, stringOf("export.linux.executable"));
    copyInto(ui.package, stringOf("export.android.package"));
    ui.versionCode = static_cast<int>(document.number("export.android.version_code").value_or(1.0));
    ui.orientation = "LandscapeSensor";
    if (const std::string scene = stringOf("project.scene"); !scene.empty()) {
        std::string sceneText;
        core::JsonDocument sceneDocument;
        if (platform::readTextFile(ui.root / "content" / std::filesystem::path(scene), sceneText) &&
            sceneDocument.parse(sceneText).ok) {
            const core::JsonValue value =
                sceneDocument.root()["storage"]["UIService"]["properties"]["ScreenOrientation"];
            if (value.type() == core::JsonType::String)
                ui.orientation = std::string(value.asString());
        }
    }
    copyInto(ui.background, stringOf("export.android.icon_background"));
    ui.release = document.boolean("export.android.release").value_or(false);
    copyInto(ui.keystore, stringOf("export.android.keystore"));
    copyInto(ui.keyAlias, stringOf("export.android.key_alias"));
}

// One setting, into `project.toml`, in place; what went wrong is shown in the window.
void writeExportSetting(ExportUi& ui, const char* key, const std::string& rendered)
{
    ui.problem.clear();
    (void)writeProjectSetting(ui.root, key, rendered, &ui.problem);
}

void startStatus(ExportUi& ui)
{
    if (!ui.cli || ui.statusProcess)
        return;
    platform::ChildProcess::Options options;
    options.workingDirectory = ui.cli->workingDirectory;
    ui.statusOutput.clear();
    ui.statusProcess = platform::ChildProcess::start(ui.cli->command({"build", "--status"}), options);
}

void startNextExport(ExportUi& ui)
{
    if (ui.run || ui.queue.empty() || !ui.cli)
        return;
    const std::string target = ui.queue.front();
    ui.queue.erase(ui.queue.begin());

    std::vector<std::string> arguments{"build", ui.root.string(), "--target=" + target, "--progress=json"};
    // A folder picked for one target is that target's; Export all keeps each
    // in its own `dist/<target>`.
    if (ui.output[0] != '\0' && ui.queue.empty() && ui.finished.empty())
        arguments.push_back("--output=" + std::string(ui.output.data()));
    if (target == "android" && ui.bumpVersionCode)
        arguments.emplace_back("--bump-version-code");

    platform::ChildProcess::Options options;
    options.workingDirectory = ui.cli->workingDirectory;
    // **The password goes in the child's environment**, never its command line
    // (ADR 0104 §3), and only for the export that signs with it.
    if (target == "android" && ui.release && ui.password[0] != '\0')
        options.environment.emplace_back("ENG_ANDROID_KEYSTORE_PASSWORD", std::string(ui.password.data()));

    ui.progress = ExportRun{};
    ui.running = target;
    ui.startedAt = ImGui::GetTime();
    ui.run = platform::ChildProcess::start(ui.cli->command(arguments), options);
    if (!ui.run) {
        ui.finished.push_back(
            {target, false, {}, 0, 0.0, core::tr(ENG_TR("engine.editor.export_window.cli_could_not_start")), {}});
        ui.running.clear();
    }
}

void finishExport(ExportUi& ui)
{
    ui.progress.feed(ui.run->readAvailable());
    ui.progress.feed("\n");
    ExportUi::Finished done;
    done.target = ui.running;
    done.ok = ui.run->exitCode() == 0 && ui.progress.result().has_value();
    done.seconds = ImGui::GetTime() - ui.startedAt;
    done.log = ui.progress.log();
    if (ui.progress.result())
        done.result = *ui.progress.result();
    if (done.ok) {
        const std::string made = !done.result.apk.empty() ? done.result.apk : done.result.folder;
        done.bytes = sizeOnDisk(made);
        std::string version;
        {
            const ProjectConfig config = loadProjectConfig(ui.root, GraphicsOverrides{});
            version = config.version;
        }
        (void)rememberExport(recentExportsFile(), ui.root,
                             RecentExport{done.target, version, nowText(), done.bytes, made});
        ui.recent = loadRecentExports(recentExportsFile(), ui.root);
        ui.celebrateAt = ImGui::GetTime();
        // The version code went up in the file, so the field follows it.
        if (done.target == "android" && ui.bumpVersionCode) {
            readExportSettings(ui);
            ui.bumpVersionCode = false;
        }
        // Remembered only once it has signed something: a password that did
        // not open the keystore is not worth keeping.
        if (done.target == "android" && ui.release && ui.rememberPassword && ui.password[0] != '\0' && ui.cli) {
            platform::ChildProcess::Options options;
            options.workingDirectory = ui.cli->workingDirectory;
            options.environment.emplace_back("ENG_ANDROID_KEYSTORE_PASSWORD", std::string(ui.password.data()));
            (void)platform::runProcess(
                ui.cli->command({"keystore", "remember", "--out", (ui.root / ui.keystore.data()).string(), "--project",
                                 ui.root.string()}),
                options);
        }
    }
    else if (const ExportStep* failed = ui.progress.failure()) {
        done.failure = failed->message.empty()
                           ? core::tr(ENG_TR("engine.editor.export_window.step_failed"), {{"step", failed->name}})
                           : failed->message;
    }
    else {
        done.failure = ui.progress.notes().empty()
                           ? std::string(core::tr(ENG_TR("engine.editor.export_window.build_stopped")))
                           : ui.progress.notes().back();
    }
    ui.finished.push_back(std::move(done));
    ui.run.reset();
    ui.running.clear();
}

// Every frame, open or not: an export keeps running behind a closed window.
void pollExport(ExportUi& ui)
{
    if (ui.statusProcess) {
        ui.statusOutput += ui.statusProcess->readAvailable();
        if (!ui.statusProcess->running()) {
            ui.statusOutput += ui.statusProcess->readAvailable();
            ui.status = parseExportStatus(ui.statusOutput);
            ui.statusProcess.reset();
        }
    }
    if (ui.open && ui.status && !ui.status->adb.empty() && !ui.adbProcess && ImGui::GetTime() - ui.adbCheckedAt > 3.0) {
        ui.adbCheckedAt = ImGui::GetTime();
        ui.adbOutput.clear();
        ui.adbProcess = platform::ChildProcess::start({ui.status->adb, "devices", "-l"});
    }
    if (ui.adbProcess) {
        ui.adbOutput += ui.adbProcess->readAvailable();
        if (!ui.adbProcess->running()) {
            ui.adbOutput += ui.adbProcess->readAvailable();
            ui.phone = parseAdbDevice(ui.adbOutput);
            ui.adbProcess.reset();
        }
    }
    if (ui.run) {
        ui.progress.feed(ui.run->readAvailable());
        if (!ui.run->running())
            finishExport(ui);
    }
    startNextExport(ui);

    if (ui.install) {
        ui.installOutput += ui.install->readAvailable();
        if (!ui.install->running()) {
            ui.installOutput += ui.install->readAvailable();
            const bool ok = ui.install->exitCode() == 0;
            ui.install.reset();
            if (ui.installStage == 1 && ok && ui.status) {
                // Launched by package, through the launcher's own intent: no
                // activity name to know.
                ui.installStage = 2;
                ui.install = platform::ChildProcess::start({ui.status->adb, "shell", "monkey", "-p", ui.installPackage,
                                                            "-c", "android.intent.category.LAUNCHER", "1"});
            }
            else {
                ui.installMessage =
                    ok ? core::tr(ENG_TR("engine.editor.export_window.installed_on"), {{"phone", ui.phone}})
                       : core::tr(ENG_TR("engine.editor.export_window.adb_could_not_install"),
                                  {{"output", ui.installOutput}});
                ui.installStage = 0;
            }
        }
    }
    if (ui.tools) {
        ui.toolsOutput += ui.tools->readAvailable();
        if (!ui.tools->running()) {
            ui.toolsOutput += ui.tools->readAvailable();
            ui.toolsMessage = ui.tools->exitCode() == 0
                                  ? std::string(core::tr(ENG_TR("engine.editor.export_window.tools_installed")))
                                  : ui.toolsOutput;
            ui.tools.reset();
            startStatus(ui);
        }
    }
    if (ui.keystoreProcess) {
        ui.keystoreOutput += ui.keystoreProcess->readAvailable();
        if (!ui.keystoreProcess->running()) {
            ui.keystoreOutput += ui.keystoreProcess->readAvailable();
            if (ui.keystoreProcess->exitCode() == 0) {
                // The project now signs with it.
                const std::string relative = "keys/" + std::string(ui.newAlias.data()) + ".keystore";
                writeExportSetting(ui, "export.android.release", core::tomlBoolean(true));
                writeExportSetting(ui, "export.android.keystore", core::tomlString(relative));
                writeExportSetting(ui, "export.android.key_alias", core::tomlString(ui.newAlias.data()));
                readExportSettings(ui);
                ui.keystoreDialog = false;
                ui.keystoreProblem.clear();
            }
            else {
                ui.keystoreProblem = ui.keystoreOutput;
            }
            ui.keystoreProcess.reset();
        }
    }
}

void openExportWindow(Editor& editor)
{
    ExportUi& ui = exportUi();
    ui.open = true;
    const std::filesystem::path root = editor.content().root().parent_path();
    if (ui.root != root) {
        ui.root = root;
        ui.finished.clear();
        // The repository this build was compiled from, and never the
        // project's own folders (audit T1).
        ui.cli = locateCli(platform::paths().executableDir, std::filesystem::path(ENG_SOURCE_DIR));
    }
    readExportSettings(ui);
    ui.recent = loadRecentExports(recentExportsFile(), root);
    startStatus(ui);
}

[[nodiscard]] const TargetStatus* statusOf(const ExportUi& ui, std::string_view name)
{
    if (!ui.status)
        return nullptr;
    for (const TargetStatus& target : ui.status->targets) {
        if (target.name == name)
            return &target;
    }
    return nullptr;
}

[[nodiscard]] const char* targetTitle(std::string_view name)
{
    if (name == "windows")
        return core::tr(ENG_TR("engine.editor.export_window.target.windows"));
    if (name == "linux")
        return core::tr(ENG_TR("engine.editor.export_window.target.linux"));
    if (name == "android")
        return core::tr(ENG_TR("engine.editor.export_window.target.android"));
    if (name == "windows-server")
        return core::tr(ENG_TR("engine.editor.export_window.target.windows_server"));
    return core::tr(ENG_TR("engine.editor.export_window.target.linux_server"));
}

// A target's card: its name, what it will make, and whether this machine can.
void drawTargetCard(ExportUi& ui, std::string_view name, const IconAtlas* icons)
{
    const TargetStatus* status = statusOf(ui, name);
    const bool selected = ui.selected == name;
    const bool dedicated = ui.multiplayer == 2;
    const bool server = name.ends_with("-server");

    ImGui::PushID(std::string(name).c_str());
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeightWithSpacing() * 2.6f);
    if (ImGui::Selectable("##card", selected, 0, size))
        ui.selected = std::string(name);
    const ImVec2 min = ImGui::GetItemRectMin();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float pad = ImGui::GetStyle().FramePadding.x;
    // The platform's mark: a tile in its colour, with its initial.
    const ImU32 mark = name.starts_with("windows") ? IM_COL32(0, 120, 215, 255)
                       : name.starts_with("linux") ? IM_COL32(233, 84, 32, 255)
                                                   : IM_COL32(61, 220, 132, 255);
    const float tile = size.y - pad * 2.0f;
    draw->AddRectFilled(ImVec2(min.x + pad, min.y + pad), ImVec2(min.x + pad + tile, min.y + pad + tile), mark, 6.0f);
    const std::string_view mark_ = name.starts_with("windows") ? icons::ActionTargetWindows
                                   : name.starts_with("linux") ? icons::ActionTargetLinux
                                                               : icons::ActionTargetAndroid;
    // Painted, not placed: moving the cursor into the card and back left the
    // last card's cursor past everything the list had drawn, which ImGui
    // reports as an error box over the Export window -- the one the owner saw.
    const float inset = tile * 0.18f;
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
    paintActionIcon(icons, mark_, ImVec2(min.x + pad + inset, min.y + pad + inset), tile - inset * 2.0f);
    ImGui::PopStyleColor();

    const float textX = min.x + pad * 2.0f + tile;
    draw->AddText(ImVec2(textX, min.y + pad), ImGui::GetColorU32(ImGuiCol_Text), targetTitle(name));
    std::string state;
    ImU32 colour = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    if (status == nullptr) {
        state = core::tr(ui.cli ? ENG_TR("engine.editor.export_window.state.checking")
                                : ENG_TR("engine.editor.export_window.state.no_cli"));
    }
    else if (status->ready && dedicated && !server && ui.server[0] == 0) {
        // A client of a dedicated server has to be told where the server is;
        // "joins ?" left somebody guessing what the question mark wanted.
        state = core::tr(ENG_TR("engine.editor.export_window.state.no_server_address"));
        colour = ImGui::GetColorU32(themeColor(palette().warning));
    }
    else if (status->ready) {
        state = name == "android" && !ui.phone.empty()
                    ? core::tr(ENG_TR("engine.editor.export_window.state.phone_connected"), {{"phone", ui.phone}})
                : dedicated && !server ? core::tr(ENG_TR("engine.editor.export_window.state.client_joins"),
                                                  {{"server", std::string_view(ui.server.data())}})
                                       : std::string(core::tr(ENG_TR("engine.editor.export_window.state.ready")));
        colour = ImGui::GetColorU32(themeColor(palette().success));
    }
    else if (!status->tools) {
        state = core::tr(ENG_TR("engine.editor.export_window.state.tools_missing"));
        colour = ImGui::GetColorU32(themeColor(palette().warning));
    }
    else {
        state = core::tr(ENG_TR("engine.editor.export_window.state.no_player"));
        colour = ImGui::GetColorU32(themeColor(palette().warning));
    }
    draw->AddText(ImVec2(textX, min.y + pad + ImGui::GetTextLineHeightWithSpacing()), colour, state.c_str());
    ImGui::PopID();
}

void drawExportSettings(Editor& editor, ExportUi& ui, EditorDialogs& dialogs)
{
    const ProjectConfig config = loadProjectConfig(ui.root, GraphicsOverrides{});
    ImGui::SeparatorText(targetTitle(ui.selected));
    ImGui::TextDisabled(
        "%s", core::tr(ENG_TR("engine.editor.export_settings.name_version_company"),
                       {{"name", config.name},
                        {"version", config.version.empty()
                                        ? std::string_view(core::tr(ENG_TR("engine.editor.export_settings.no_version")))
                                        : std::string_view(config.version)},
                        {"company", config.company.empty()
                                        ? std::string_view(core::tr(ENG_TR("engine.editor.export_settings.no_company")))
                                        : std::string_view(config.company)}})
                  .c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.export_settings.project_settings"))))
        dialogs.projectSettings = true;

    if (ui.selected == "windows" || ui.selected == "windows-server") {
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_settings.executable")));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##winexe", config.name.c_str(), ui.windowsExecutable.data(),
                                 ui.windowsExecutable.size());
        if (ImGui::IsItemDeactivatedAfterEdit())
            writeExportSetting(ui, "export.windows.executable", core::tomlString(ui.windowsExecutable.data()));
    }
    else if (ui.selected == "linux" || ui.selected == "linux-server") {
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_settings.executable")));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##linuxexe", core::tr(ENG_TR("engine.editor.export_settings.the_name_in_lower_case")),
                                 ui.linuxExecutable.data(), ui.linuxExecutable.size());
        if (ImGui::IsItemDeactivatedAfterEdit())
            writeExportSetting(ui, "export.linux.executable", core::tomlString(ui.linuxExecutable.data()));
    }
    else if (ui.selected == "android") {
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_settings.package")));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##package", config.id.c_str(), ui.package.data(), ui.package.size());
        if (ImGui::IsItemDeactivatedAfterEdit())
            writeExportSetting(ui, "export.android.package", core::tomlString(ui.package.data()));
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_settings.version_code")));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::InputInt("##code", &ui.versionCode) && ui.versionCode < 1)
            ui.versionCode = 1;
        if (ImGui::IsItemDeactivatedAfterEdit())
            writeExportSetting(ui, "export.android.version_code",
                               core::tomlNumber(static_cast<core::f64>(ui.versionCode)));
        ImGui::SameLine();
        ImGui::Checkbox(core::tr(ENG_TR("engine.editor.export_settings.bump_on_export")), &ui.bumpVersionCode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.export_settings.a_store_refuses_an_update_tip")));
        // **The start scene's, not a setting of its own** (the owner): two
        // answers to how the phone is held turned the screen the moment the
        // game started whenever they differed.
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_settings.orientation")));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.export_settings.orientation_from_scene"),
                                           {{"orientation", std::string_view(ui.orientation.c_str())}})
                                      .c_str());
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_settings.icon_background")));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::InputTextWithHint("##background", core::tr(ENG_TR("engine.editor.export_settings.ffffff")),
                                 ui.background.data(), ui.background.size());
        if (ImGui::IsItemDeactivatedAfterEdit())
            writeExportSetting(ui, "export.android.icon_background", core::tomlString(ui.background.data()));

        // Signing: the machine's debug key, or the project's release key.
        ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.export_settings.signing")));
        if (ui.release && ui.keystore[0] != '\0')
            ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_settings.release_key"),
                                            {{"keystore", std::string_view(ui.keystore.data())}})
                                       .c_str());
        else
            ImGui::TextUnformatted(
                core::tr(ENG_TR("engine.editor.export_settings.debug_key_installs_anywhere_publishes")));
        if (ImGui::Checkbox(core::tr(ENG_TR("engine.editor.export_settings.release")), &ui.release))
            writeExportSetting(ui, "export.android.release", core::tomlBoolean(ui.release));
        ImGui::SameLine();
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.export_settings.create_keystore")))) {
            ui.keystoreDialog = true;
            copyInto(ui.newAlias, config.name.empty() ? std::string_view("release") : std::string_view(config.name));
            copyInto(ui.newName,
                     config.company.empty() ? std::string_view(config.name) : std::string_view(config.company));
            ui.newPassword.fill(0);
            ui.newConfirm.fill(0);
            ui.keystoreProblem.clear();
        }
        if (ui.release) {
            ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_settings.password")));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            ImGui::InputTextWithHint("##password",
                                     core::tr(ENG_TR("engine.editor.export_settings.remembered_on_this_machine_or")),
                                     ui.password.data(), ui.password.size(), ImGuiInputTextFlags_Password);
            ImGui::SameLine();
            ImGui::Checkbox(core::tr(ENG_TR("engine.editor.export_settings.remember_on_this_machine")),
                            &ui.rememberPassword);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.export_settings.kept_in_your_user_folder_tip")));
        }
    }

    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.export_settings.output")));
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6.0f);
    const std::string placeholder = "dist/" + ui.selected;
    ImGui::InputTextWithHint("##output", placeholder.c_str(), ui.output.data(), ui.output.size());
    ImGui::SameLine();
    if (ImGui::Button(core::tr(ENG_TR("engine.editor.export_settings.open")))) {
        const std::filesystem::path chosen =
            ui.output[0] != '\0' ? std::filesystem::path(ui.output.data()) : ui.root / "dist" / ui.selected;
        openFolder(chosen);
    }
    (void)editor;
}

void drawKeystoreDialog(ExportUi& ui)
{
    if (ui.keystoreDialog) {
        ImGui::OpenPopup("###Create keystore");
        ui.keystoreDialog = false;
    }
    if (!beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.create_keystore"), "###Create keystore").c_str(),
                           460.0f, []() {}))
        return;
    ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.keystore_dialog.the_key_an_android_game")));
    ImGui::Spacing();
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.keystore_dialog.alias")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##alias", ui.newAlias.data(), ui.newAlias.size());
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.keystore_dialog.name_on_the_certificate")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##who", ui.newName.data(), ui.newName.size());
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.keystore_dialog.valid_for_years")));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::InputInt("##years", &ui.newYears))
        ui.newYears = std::clamp(ui.newYears, 1, 100);
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.keystore_dialog.password")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##new", ui.newPassword.data(), ui.newPassword.size(), ImGuiInputTextFlags_Password);
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.keystore_dialog.password_again")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##again", ui.newConfirm.data(), ui.newConfirm.size(), ImGuiInputTextFlags_Password);
    ImGui::Checkbox(core::tr(ENG_TR("engine.editor.keystore_dialog.remember_the_password_on_this")), &ui.newRemember);

    if (!ui.keystoreProblem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().danger));
        ImGui::TextWrapped("%s", ui.keystoreProblem.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::BeginDisabled(ui.keystoreProcess != nullptr);
    if (dialogButton(core::tr(ui.keystoreProcess ? ENG_TR("engine.editor.keystore_dialog.creating")
                                                 : ENG_TR("engine.editor.keystore_dialog.create")),
                     ImVec2(120.0f, 0.0f))) {
        if (std::string_view(ui.newPassword.data()) != std::string_view(ui.newConfirm.data()))
            ui.keystoreProblem = core::tr(ENG_TR("engine.editor.keystore_dialog.passwords_differ"));
        else if (std::strlen(ui.newPassword.data()) < 6)
            ui.keystoreProblem = core::tr(ENG_TR("engine.editor.keystore_dialog.password_too_short"));
        else if (ui.newAlias[0] == '\0')
            ui.keystoreProblem = core::tr(ENG_TR("engine.editor.keystore_dialog.needs_an_alias"));
        else if (ui.cli) {
            platform::ChildProcess::Options options;
            options.workingDirectory = ui.cli->workingDirectory;
            options.environment.emplace_back("ENG_ANDROID_KEYSTORE_PASSWORD", std::string(ui.newPassword.data()));
            std::vector<std::string> arguments{
                "keystore",   "new",
                "--alias",    ui.newAlias.data(),
                "--out",      (ui.root / "keys" / (std::string(ui.newAlias.data()) + ".keystore")).string(),
                "--name",     ui.newName.data(),
                "--validity", std::to_string(ui.newYears),
                "--project",  ui.root.string()};
            if (ui.newRemember)
                arguments.emplace_back("--remember");
            ui.keystoreOutput.clear();
            ui.keystoreProcess = platform::ChildProcess::start(ui.cli->command(arguments), options);
            copyInto(ui.password, ui.newPassword.data());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (dialogButton(core::tr(ENG_TR("engine.editor.keystore_dialog.cancel")), ImVec2(120.0f, 0.0f)) ||
        dialogCancelled())
        ImGui::CloseCurrentPopup();
    // Closed by the poll once the key exists.
    if (!ui.keystoreProcess && ui.keystoreProblem.empty() && ui.keystoreOutput.find("Created") != std::string::npos) {
        ui.keystoreOutput.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The live step list of the export running, or the last one's.
void drawExportSteps(const ExportRun& run, bool active)
{
    for (const ExportStep& step : run.steps()) {
        const bool failed = step.state == ExportStep::State::Failed;
        const bool done = step.state == ExportStep::State::Done;
        const ImVec4 colour = failed ? themeColor(palette().danger)
                              : done ? themeColor(palette().success)
                                     : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        const char* mark = failed ? "x" : done ? "v" : (active ? "..." : "-");
        ImGui::TextColored(colour, "%-3s %s", mark, step.name.c_str());
        if (done) {
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f s", step.ms / 1000.0);
        }
        if (failed && !step.message.empty()) {
            ImGui::Indent();
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextWrapped("%s", step.message.c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent();
        }
    }
}

// A check mark that draws itself in, for a moment after a success.
void drawCelebration(double since)
{
    if (since < 0.0 || since > 1.6)
        return;
    const float t = static_cast<float>(std::min(since / 0.45, 1.0));
    const float size = ImGui::GetTextLineHeight() * 1.6f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 colour = ImGui::GetColorU32(themeColor(palette().success));
    const ImVec2 a(at.x + size * 0.15f, at.y + size * 0.55f);
    const ImVec2 b(at.x + size * 0.42f, at.y + size * 0.80f);
    const ImVec2 c(at.x + size * 0.88f, at.y + size * 0.22f);
    const float first = std::min(t / 0.4f, 1.0f);
    draw->AddLine(a, ImVec2(a.x + (b.x - a.x) * first, a.y + (b.y - a.y) * first), colour, 3.0f);
    if (t > 0.4f) {
        const float second = (t - 0.4f) / 0.6f;
        draw->AddLine(b, ImVec2(b.x + (c.x - b.x) * second, b.y + (c.y - b.y) * second), colour, 3.0f);
    }
    ImGui::Dummy(ImVec2(size, size));
    ImGui::SameLine();
}

void drawExportResult(ExportUi& ui, const ExportUi::Finished& done)
{
    ImGui::PushID(&done);
    if (done.ok) {
        drawCelebration(&done == &ui.finished.back() ? ImGui::GetTime() - ui.celebrateAt : -1.0);
        ImGui::TextColored(themeColor(palette().success), "%s",
                           core::tr(ENG_TR("engine.editor.export_result.done"),
                                    {{"target", std::string_view(targetTitle(done.target))},
                                     {"size", humanSize(done.bytes)},
                                     {"seconds", static_cast<core::i64>(std::lround(done.seconds))}})
                               .c_str());
        const std::string made = !done.result.apk.empty() ? done.result.apk : done.result.folder;
        if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.export_result.open_folder"))))
            openFolder(made);
        const bool desktop = !done.result.executable.empty() && ((done.target.starts_with("windows") && kWindowsHost) ||
                                                                 (done.target.starts_with("linux") && !kWindowsHost));
        if (desktop) {
            ImGui::SameLine();
            if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.export_result.run_it")))) {
                if (auto started = platform::ChildProcess::start({done.result.executable}))
                    ui.launched.push_back(std::move(started));
            }
        }
        if (!done.result.apk.empty() && !ui.phone.empty() && ui.status) {
            ImGui::SameLine();
            ImGui::BeginDisabled(ui.install != nullptr);
            if (ImGui::SmallButton(core::tr(!ui.install ? ENG_TR("engine.editor.export_result.install_on_phone")
                                            : ui.installStage == 1 ? ENG_TR("engine.editor.export_result.installing")
                                                                   : ENG_TR("engine.editor.export_result.starting")))) {
                ui.installMessage.clear();
                ui.installOutput.clear();
                ui.installPackage = done.result.package;
                ui.installStage = 1;
                ui.install = platform::ChildProcess::start({ui.status->adb, "install", "-r", done.result.apk});
            }
            ImGui::EndDisabled();
        }
    }
    else {
        ImGui::TextColored(themeColor(palette().danger), "%s",
                           core::tr(ENG_TR("engine.editor.export_result.failed"),
                                    {{"target", std::string_view(targetTitle(done.target))}})
                               .c_str());
        ImGui::TextWrapped("%s", done.failure.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.export_result.show_log")))) {
        ui.logText = done.log;
        ui.showLog = true;
    }
    ImGui::PopID();
}

void drawExportWindow(Editor& editor, EditorDialogs& dialogs, const IconAtlas* icons)
{
    ExportUi& ui = exportUi();
    pollExport(ui);
    drawKeystoreDialog(ui);
    if (!ui.open)
        return;

    // **A tab beside the Viewport, the first time** (the owner), as a script
    // opens: in the node the world is in, wherever somebody has put it. Only
    // the first time -- once it has been dragged somewhere, the layout keeps it
    // there, as it keeps every panel where its owner left it.
    if (const ImGuiWindow* world3d = ImGui::FindWindowByName("###Viewport");
        world3d != nullptr && world3d->DockNode != nullptr)
        ImGui::SetNextWindowDockID(world3d->DockNode->ID, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_FirstUseEver);
    // Room at the front of its title for the tab's picture (`drawTabIcons`).
    if (!ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.export")) + "###Export").c_str(),
                      &ui.open)) {
        ImGui::End();
        return;
    }
    // Escape closes it while it has the keyboard, as it closes every dialog.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ui.open = false;
        g_escapeTaken = true;
    }
    if (!ui.cli) {
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.export_window.the_ludwerk_cli_was_not")));
        ImGui::End();
        return;
    }

    // Multiplayer, above the cards: it decides which cards there are.
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_window.multiplayer")));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    if (ImGui::Combo("##multiplayer", &ui.multiplayer,
                     choices({ENG_TR("engine.editor.export_window.mode.single_player"),
                              ENG_TR("engine.editor.export_window.mode.players_host"),
                              ENG_TR("engine.editor.export_window.mode.dedicated_server")})
                         .c_str()))
        writeExportSetting(ui, "export.multiplayer",
                           core::tomlString(kMultiplayerModes[static_cast<std::size_t>(ui.multiplayer)]));
    if (ui.multiplayer == 2) {
        ImGui::SameLine();
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.export_window.server")));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        ImGui::InputTextWithHint("##server", core::tr(ENG_TR("engine.editor.export_window.play_example_com_7777")),
                                 ui.server.data(), ui.server.size());
        if (ImGui::IsItemDeactivatedAfterEdit())
            writeExportSetting(ui, "network.server", core::tomlString(ui.server.data()));
    }
    // Beside the mode rather than on a card: every target of a game ships its
    // scripts the same way.
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    if (ImGui::Checkbox(core::tr(ENG_TR("engine.editor.export_window.ship_source")), &ui.shipSource))
        writeExportSetting(ui, "export.ship_source", core::tomlBoolean(ui.shipSource));
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.export_window.off_the_game_s_scripts_tip")));
    if (!ui.problem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().danger));
        ImGui::TextWrapped("%s", ui.problem.c_str());
        ImGui::PopStyleColor();
    }

    std::vector<std::string> cards{"windows", "linux", "android"};
    if (ui.multiplayer == 2) {
        cards.emplace_back("windows-server");
        cards.emplace_back("linux-server");
    }
    else if (ui.selected.ends_with("-server")) {
        ui.selected = "windows";
    }

    const float left = ImGui::GetFontSize() * 17.0f;
    ImGui::BeginChild("##targets", ImVec2(left, -ImGui::GetFrameHeightWithSpacing() * 2.5f), ImGuiChildFlags_Borders);
    for (const std::string& card : cards)
        drawTargetCard(ui, card, icons);
    if (const TargetStatus* phone = statusOf(ui, "android"); phone != nullptr && !phone->tools) {
        ImGui::Spacing();
        ImGui::BeginDisabled(ui.tools != nullptr);
        if (ImGui::Button(core::tr(ui.tools ? ENG_TR("engine.editor.export_window.installing")
                                            : ENG_TR("engine.editor.export_window.install_android_tools")),
                          ImVec2(-FLT_MIN, 0.0f))) {
            platform::ChildProcess::Options options;
            options.workingDirectory = ui.cli->workingDirectory;
            ui.toolsOutput.clear();
            ui.toolsMessage.clear();
            ui.tools = platform::ChildProcess::start(ui.cli->command({"android", "install-tools"}), options);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.export_window.a_jdk_17_the_android_tip")));
        if (!ui.toolsMessage.empty())
            ImGui::TextWrapped("%s", ui.toolsMessage.c_str());
        else if (ui.tools) {
            const std::size_t last =
                ui.toolsOutput.find_last_of('\n', ui.toolsOutput.size() >= 2 ? ui.toolsOutput.size() - 2 : 0);
            ImGui::TextDisabled("%s", ui.toolsOutput.substr(last == std::string::npos ? 0 : last + 1).c_str());
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##settings", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.5f), ImGuiChildFlags_Borders);
    drawExportSettings(editor, ui, dialogs);

    // The run: its steps while it runs, then every result of this session.
    if (ui.run || !ui.progress.steps().empty()) {
        ImGui::SeparatorText(ui.run ? core::tr(ENG_TR("engine.editor.export_window.exporting_target"),
                                               {{"target", targetTitle(ui.running)}})
                                          .c_str()
                                    : core::tr(ENG_TR("engine.editor.export_window.last_export")));
        drawExportSteps(ui.progress, ui.run != nullptr);
    }
    for (auto it = ui.finished.rbegin(); it != ui.finished.rend(); ++it)
        drawExportResult(ui, *it);
    if (!ui.installMessage.empty())
        ImGui::TextWrapped("%s", ui.installMessage.c_str());

    if (!ui.recent.empty()) {
        ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.export_window.recent_exports")));
        for (const RecentExport& recent : ui.recent) {
            const std::string label = std::string(targetTitle(recent.target)) + "  " + recent.version + "  " +
                                      recent.when + "  " + humanSize(recent.bytes) + "##" + recent.folder + recent.when;
            if (ImGui::Selectable(label.c_str()))
                openFolder(recent.folder);
        }
    }
    ImGui::EndChild();

    // Export, and Export all: every ready card, one after another.
    const TargetStatus* chosen = statusOf(ui, ui.selected);
    const bool busy = ui.run != nullptr || !ui.queue.empty();
    ImGui::BeginDisabled(busy || chosen == nullptr || !chosen->ready);
    if (ImGui::Button(core::tr(busy ? ENG_TR("engine.editor.export_window.exporting")
                                    : ENG_TR("engine.editor.export_window.export")),
                      ImVec2(ImGui::GetFontSize() * 10.0f, ImGui::GetFrameHeight() * 1.5f))) {
        ui.finished.clear();
        ui.queue = {ui.selected};
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !ui.status);
    if (ImGui::Button(core::tr(ENG_TR("engine.editor.export_window.export_all")),
                      ImVec2(ImGui::GetFontSize() * 8.0f, ImGui::GetFrameHeight() * 1.5f))) {
        ui.finished.clear();
        ui.queue.clear();
        for (const std::string& card : cards) {
            if (const TargetStatus* status = statusOf(ui, card); status != nullptr && status->ready)
                ui.queue.push_back(card);
        }
    }
    ImGui::EndDisabled();
    if (busy) {
        ImGui::SameLine();
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.export_window.cancel")))) {
            ui.queue.clear();
            if (ui.run)
                ui.run->kill();
        }
    }
    ImGui::End();

    if (ui.showLog) {
        ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(labelled(ENG_TR("engine.editor.export_window.export_log"), "###Export log").c_str(),
                         &ui.showLog)) {
            ImGui::InputTextMultiline("##log", ui.logText.data(), ui.logText.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN),
                                      ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::End();
    }
}

// --- Preferences ----------------------------------------------------------
//
// **One window of one size, a search above a tree** (the owner, 2026-09-27:
// the pages were three sizes and three layouts, and a setting was found by
// opening tabs until it turned up). The tree down the left is the content
// browser's; the settings on the right are the Properties panel's grid, under
// its headings -- one page after another in one scroll, so the tree moves the
// view rather than swapping it. Whatever is typed above filters every page at
// once, and the tree says how much each one holds.

enum class PreferencePage : core::u8
{
    Appearance,
    Viewport,
    Icons,
    ScriptColours,
    ScriptShortcuts,
};

struct PreferencePageInfo
{
    PreferencePage page;
    // The tree's branch it hangs from: what the tree knows it by, and what
    // it is called. An id is never shown, and the words are the catalog's.
    const char* groupId;
    core::TextKey group;
    const char* id;
    core::TextKey name;
    std::string_view icon;
};

const PreferencePageInfo PreferencePages[] = {
    {PreferencePage::Appearance, "general", ENG_TR("engine.editor.preferences.group.general"), "appearance",
     ENG_TR("engine.editor.preferences.page.appearance"), icons::ActionPaint},
    {PreferencePage::Viewport, "general", ENG_TR("engine.editor.preferences.group.general"), "viewport",
     ENG_TR("engine.editor.preferences.page.viewport"), icons::ClassCamera},
    {PreferencePage::Icons, "general", ENG_TR("engine.editor.preferences.group.general"), "icons",
     ENG_TR("engine.editor.preferences.page.icons"), icons::ActionVisible},
    {PreferencePage::ScriptColours, "script-editor", ENG_TR("engine.editor.preferences.group.script_editor"), "colours",
     ENG_TR("engine.editor.preferences.page.colours"), icons::ClassScript},
    {PreferencePage::ScriptShortcuts, "script-editor", ENG_TR("engine.editor.preferences.group.script_editor"),
     "shortcuts", ENG_TR("engine.editor.preferences.page.keyboard_shortcuts"), icons::ActionKeyboard},
};

std::size_t drawAppearancePreferences(const PreferenceQuery& query, const IconAtlas* icons)
{
    std::size_t found = 0;
    const bool theme = query.shows(core::tr(ENG_TR("engine.editor.appearance_preferences.theme")));
    const bool scaling = query.shows(core::tr(ENG_TR("engine.editor.appearance_preferences.interface_scale")));
    found += (theme ? 1 : 0) + (scaling ? 1 : 0);
    if (!query.draw || found == 0 || !beginSectionGrid("appearance"))
        return found;

    // **First, because it is the one setting that changes what every other
    // panel looks like** -- and because a person who came here looking for one
    // thing came looking for this.
    if (theme) {
        const Theme& current = themeById(g_appearance.themeId);
        sectionName(core::tr(ENG_TR("engine.editor.appearance_preferences.theme")));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##theme", shownName(current).c_str())) {
            for (const Theme& each : themes()) {
                const bool selected = each.id == current.id;
                if (ImGui::Selectable(shownName(each).c_str(), selected)) {
                    g_appearance.themeId = std::string(each.id);
                    // Applied on the spot rather than on Close: a theme you
                    // have to dismiss a dialog to see is a theme you choose by
                    // trial and error.
                    applyAppearance();
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        preferenceHint(core::tr(ENG_TR("engine.editor.appearance_preferences.for_every_project")));
    }

    if (scaling) {
        // Shown resolved rather than as the stored zero, so the slider says
        // what the shell is actually drawn at.
        //
        // **Applied when the slider is let go, not while it is dragged**
        // (reported as the scale "not respecting" the hand on it): every step
        // of a live drag resized the dialog and the slider itself, so the value
        // under the pointer moved while the pointer did not, and the drag ran
        // away. The number follows the drag; the interface follows the release.
        static f32 s_draggedScale = 0.0f;
        f32 scale = s_draggedScale > 0.0f ? s_draggedScale : resolveUiScale(g_appearance.scale, g_displayScale);
        sectionName(core::tr(ENG_TR("engine.editor.appearance_preferences.interface_scale")));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##interface-scale", &scale, kMinimumUiScale, kMaximumUiScale, "%.2fx"))
            s_draggedScale = scale;
        if (ImGui::IsItemDeactivatedAfterEdit() && s_draggedScale > 0.0f) {
            g_appearance.scale = s_draggedScale;
            s_draggedScale = 0.0f;
            applyAppearance();
        }
        // The sizes people actually pick, one click each.
        for (const f32 preset : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
            char label[16];
            (void)std::snprintf(label, sizeof(label), "%d%%", static_cast<int>(std::lround(preset * 100.0f)));
            if (preset != 1.0f)
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::Button(label)) {
                g_appearance.scale = preset;
                s_draggedScale = 0.0f;
                applyAppearance();
            }
        }
        if (labeledIconButton(icons, icons::ActionRefresh,
                              core::tr(ENG_TR("engine.editor.appearance_preferences.match_display")))) {
            // Zero is the stored spelling of "ask the display", which is what
            // this button puts back -- not the number the display happens to
            // report today, because that one is wrong the moment somebody
            // changes monitors.
            g_appearance.scale = 0.0f;
            applyAppearance();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.appearance_preferences.display_scale"),
                                           {{"scale", fixed(static_cast<double>(g_displayScale), 2)}})
                                      .c_str());
        preferenceHint(core::tr(ENG_TR("engine.editor.appearance_preferences.for_every_project")));
    }
    endSectionGrid();
    return found;
}

std::size_t drawViewportPreferences(const PreferenceQuery& query, Editor& editor)
{
    if (!query.shows(core::tr(ENG_TR("engine.editor.viewport_preferences.camera_speed"))))
        return 0;
    if (query.draw && beginSectionGrid("viewport")) {
        f32 speed = editor.cameraSpeed();
        sectionName(core::tr(ENG_TR("engine.editor.viewport_preferences.camera_speed")));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (dragNumber("##camera-speed", ImGuiDataType_Float, &speed, 1, 0.5f, "%.1f m/s"))
            editor.setCameraSpeed(std::clamp(speed, 0.1f, 2000.0f));
        preferenceHint(core::tr(ENG_TR("engine.editor.viewport_preferences.the_scroll_wheel_changes_this")));
        endSectionGrid();
    }
    return 1;
}

std::size_t drawIconPreferences(const PreferenceQuery& query, IconAtlas* icons)
{
    if (icons == nullptr || !query.shows(core::tr(ENG_TR("engine.editor.icon_preferences.colour_icons_by_role"))))
        return 0;
    if (query.draw && beginSectionGrid("icons")) {
        bool tinting = icons->tinting();
        sectionName(core::tr(ENG_TR("engine.editor.icon_preferences.colour_icons_by_role")));
        if (ImGui::Checkbox("##tinting", &tinting))
            icons->setTinting(tinting);
        preferenceHint(core::tr(ENG_TR("engine.editor.icon_preferences.category_colours_or_monochrome_with")));
        endSectionGrid();
    }
    return 1;
}

std::size_t drawPreferencePage(PreferencePage page, const PreferenceQuery& query, Editor& editor, IconAtlas* icons)
{
    switch (page) {
    case PreferencePage::Appearance:
        return drawAppearancePreferences(query, icons);
    case PreferencePage::Viewport:
        return drawViewportPreferences(query, editor);
    case PreferencePage::Icons:
        return drawIconPreferences(query, icons);
    case PreferencePage::ScriptColours:
        return drawScriptColourPreferences(query, icons);
    case PreferencePage::ScriptShortcuts:
        return drawScriptShortcutPreferences(query, icons);
    }
    return 0;
}

void drawPreferences(Editor& editor, IconAtlas* icons)
{
    g_escapeRecordsChord = g_capturingChord.has_value();
    const bool open = beginEditorDialog(
        labelled(ENG_TR("engine.editor.dialog.preferences"), "###Preferences").c_str(), 900.0f,
        []() { g_capturingChord.reset(); }, 600.0f);
    g_escapeRecordsChord = false;
    if (!open)
        return;
    captureScriptChord();

    // The page the tree was last pressed on, to be scrolled to; and the one at
    // the top of the view, which the tree marks. Both by index.
    static std::optional<std::size_t> s_scrollTo;
    static std::size_t s_atTop = 0;
    static std::array<char, 96> s_search{};
    // Back to the top: on opening, and on every change to the search.
    static bool s_toTop = false;
    if (dialogOpening()) {
        s_search.fill(0);
        s_toTop = true;
        ImGui::SetKeyboardFocusHere();
    }
    // Opened on the shortcuts by Preferences: Keyboard Shortcuts.
    if (std::exchange(g_preferencesToShortcuts, false))
        s_scrollTo = static_cast<std::size_t>(PreferencePage::ScriptShortcuts);

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (searchField(icons, "##preferences-search", core::tr(ENG_TR("engine.editor.preferences.search_settings")),
                    s_search.data(), s_search.size()))
        s_toTop = true;
    const std::string_view needle{s_search.data()};

    // How much each page holds of what is searched, for the tree and for
    // leaving out the pages that hold none of it.
    std::array<std::size_t, std::size(PreferencePages)> found{};
    const auto queryOf = [&](const PreferencePageInfo& info, bool draw) {
        const bool whole = !needle.empty() &&
                           (containsFold(core::tr(info.name), needle) || containsFold(core::tr(info.group), needle));
        return PreferenceQuery{needle, whole, draw};
    };
    for (std::size_t index = 0; index < std::size(PreferencePages); ++index)
        found[index] =
            drawPreferencePage(PreferencePages[index].page, queryOf(PreferencePages[index], false), editor, icons);

    const float scale = ImGui::GetStyle().FontScaleMain;
    const float treeWidth = std::round(220.0f * scale);
    if (ImGui::BeginChild("##preferences-tree", ImVec2(treeWidth, 0.0f), ImGuiChildFlags_Borders)) {
        const float glyph = ImGui::GetFontSize();
        const char* group = nullptr;
        bool groupOpen = false;
        for (std::size_t index = 0; index < std::size(PreferencePages); ++index) {
            const PreferencePageInfo& info = PreferencePages[index];
            if (group == nullptr || std::string_view(group) != info.groupId) {
                if (group != nullptr && groupOpen)
                    ImGui::TreePop();
                group = info.groupId;
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                groupOpen = ImGui::TreeNodeEx(group, ImGuiTreeNodeFlags_SpanAvailWidth, "%s", core::tr(info.group));
                // The group's first page, pressed on the group.
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                    s_scrollTo = index;
            }
            if (!groupOpen)
                continue;
            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (index == s_atTop && found[index] > 0)
                flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::BeginDisabled(found[index] == 0);
            ImGui::TreeNodeEx(info.id, flags, "%s%s", tabIconPad().c_str(), core::tr(info.name));
            ImGui::EndDisabled();
            if (ImGui::IsItemClicked())
                s_scrollTo = index;
            const ImVec2 corner(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                                ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - glyph) * 0.5f);
            paintActionIcon(icons, info.icon, corner, glyph);
            // How many rows of it a search found, at the row's end.
            if (!needle.empty() && found[index] > 0) {
                const std::string count = std::to_string(found[index]);
                const ImVec2 size = ImGui::CalcTextSize(count.c_str());
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(ImGui::GetItemRectMax().x - size.x - ImGui::GetStyle().FramePadding.x,
                           ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - size.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), count.c_str());
            }
        }
        if (group != nullptr && groupOpen)
            ImGui::TreePop();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##preferences-page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        if (std::exchange(s_toTop, false))
            ImGui::SetScrollY(0.0f);
        const float top = ImGui::GetWindowPos().y;
        bool any = false;
        std::size_t atTop = s_atTop;
        bool topFound = false;
        for (std::size_t index = 0; index < std::size(PreferencePages); ++index) {
            if (found[index] == 0)
                continue;
            any = true;
            const PreferencePageInfo& info = PreferencePages[index];
            ImGui::PushID(static_cast<int>(index));
            if (s_scrollTo == index) {
                ImGui::SetScrollHereY(0.0f);
                s_scrollTo.reset();
            }
            // The last heading at or above the view's top is the page on show.
            if (!topFound || ImGui::GetCursorScreenPos().y <= top + ImGui::GetFrameHeight()) {
                atTop = index;
                topFound = true;
            }
            // Open while searching, whatever was folded: a match inside a
            // closed heading is a match nobody sees.
            if (!needle.empty())
                ImGui::SetNextItemOpen(true);
            else
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (propertiesSection(core::tr(info.name)))
                (void)drawPreferencePage(info.page, queryOf(info, true), editor, icons);
            ImGui::PopID();
            ImGui::Spacing();
        }
        // A pressed page scrolled to when nothing before it had room to move.
        s_scrollTo.reset();
        s_atTop = atTop;
        if (!any)
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.preferences.no_setting_matches"),
                                               {{"search", std::string_view(s_search.data())}})
                                          .c_str());
    }
    ImGui::EndChild();

    // No Close button: the window's own X closes it, and a second way to do
    // one thing at the bottom of a settings page is clutter (the owner's
    // call). Everything here applies the moment it changes, so there is
    // nothing for a button to confirm.
    ImGui::EndPopup();
}

void drawEditorDialogs(Editor& editor, EditorCommands& commands, EditorDialogs& dialogs, IconAtlas* icons)
{
    if (dialogs.saveAs) {
        dialogs.saveAs = false;
        ImGui::OpenPopup("###Save Scene As");
    }
    if (dialogs.preferences) {
        dialogs.preferences = false;
        ImGui::OpenPopup("###Preferences");
    }
    if (dialogs.projectSettings) {
        dialogs.projectSettings = false;
        ImGui::OpenPopup("###Project Settings");
    }
    if (dialogs.about) {
        dialogs.about = false;
        ImGui::OpenPopup("###About");
    }

    drawProjectSettings(editor);

    if (dialogs.exportWindow) {
        dialogs.exportWindow = false;
        openExportWindow(editor);
    }
    drawExportWindow(editor, dialogs, icons);

    if (beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.save_scene_as"), "###Save Scene As").c_str(), 470.0f,
                          []() {})) {
        static std::array<char, 160> path{};
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.scenes_live_under_the_project")));
        ImGui::Spacing();

        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.editor_dialogs.content")));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (dialogOpening()) {
            path.fill(0);
            ImGui::SetKeyboardFocusHere();
        }
        const bool submitted =
            ImGui::InputText("##scene-path", path.data(), path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string typed(path.data());

        // The RESOLVED path, not the typed one. The label above the box already
        // says `content/`, so `content/scenes/main` is the natural thing to
        // type and used to be saved verbatim into `content/content/` -- D068,
        // found by the first person to use this dialog. Showing what will
        // actually be written is what makes the normalisation visible instead
        // of surprising.
        const std::string resolved = Editor::normalizeScenePath(typed);
        const bool usable = !typed.empty() && Editor::sceneNameIsUsable(resolved);
        if (usable)
            ImGui::TextWrapped(
                "%s", core::tr(ENG_TR("engine.editor.editor_dialogs.saves_as_path"), {{"path", resolved}}).c_str());
        else if (typed.empty())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.saves_as_content")));
        else
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.not_a_path_inside_content")));

        ImGui::Spacing();
        ImGui::BeginDisabled(!usable);
        const bool accepted = dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.save")), ImVec2(120.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(120.0f, 0.0f)) ||
            dialogCancelled()) {
            path.fill(0);
            ImGui::CloseCurrentPopup();
        }

        if ((submitted || accepted) && usable) {
            commands.saveAs = typed;
            path.fill(0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    drawPreferences(editor, icons);

    if (dialogs.renameInstance || dialogs.renameContent) {
        ImGui::OpenPopup("###Rename");
    }
    if (dialogs.newStampFromClass) {
        dialogs.newStampFromClass = false;
        ImGui::OpenPopup("###New Stamp From Class");
    }
    if (dialogs.newFolder) {
        dialogs.newFolder = false;
        ImGui::OpenPopup("###New Folder");
    }
    if (dialogs.newMaterial) {
        dialogs.newMaterial = false;
        ImGui::OpenPopup("###New Material");
    }
    if (dialogs.newShader) {
        dialogs.newShader = false;
        ImGui::OpenPopup("###New Surface Shader");
    }
    if (dialogs.newStamp) {
        dialogs.newStamp = false;
        ImGui::OpenPopup("###New Stamp");
    }
    if (!dialogs.deleteContentPath.empty())
        ImGui::OpenPopup("###Delete");
    // The window's own close button arrives as a platform event, so the frame
    // loop raises it on the editor and this is where it becomes a question.
    if (editor.closeRequested() && dialogs.pending == EditorDialogs::Pending::None)
        dialogs.pending = EditorDialogs::Pending::Quit;
    if (dialogs.pending != EditorDialogs::Pending::None)
        ImGui::OpenPopup("###Unsaved Changes");

    if (beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.rename"), "###Rename").c_str(), 400.0f, [&]() {
            dialogs.renameInstance = false;
            dialogs.renameContent = false;
            dialogs.renameTarget = {};
            dialogs.renameContentPath.clear();
        })) {
        static std::array<char, 128> name{};
        // Seeded once, on the frame the dialog opens. Copying every frame would
        // overwrite what the person is typing with what they started from.
        if (dialogs.renameInstance || dialogs.renameContent) {
            name.fill(0);
            const std::size_t count = std::min(dialogs.renameSeed.size(), name.size() - 1);
            std::memcpy(name.data(), dialogs.renameSeed.data(), count);
            dialogs.renameInstance = false;
            dialogs.renameContent = false;
        }

        ImGui::SetNextItemWidth(-1.0f);
        // Once, on the frame it opens. Every frame re-selects the whole name
        // under somebody's caret, which makes the arrow keys useless and a
        // double-click-to-pick-a-word impossible.
        if (dialogOpening())
            ImGui::SetKeyboardFocusHere();
        const bool submitted =
            ImGui::InputText("##rename", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string typed(name.data());

        ImGui::Spacing();
        ImGui::BeginDisabled(typed.empty());
        const bool accepted =
            dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.rename")), ImVec2(120.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(120.0f, 0.0f)) ||
            dialogCancelled()) {
            dialogs.renameTarget = {};
            dialogs.renameContentPath.clear();
            ImGui::CloseCurrentPopup();
        }

        if ((submitted || accepted) && !typed.empty()) {
            if (dialogs.renameTarget.valid()) {
                commands.renameInstance = dialogs.renameTarget;
                commands.renameInstanceTo = typed;
            }
            else if (!dialogs.renameContentPath.empty()) {
                commands.renameContent = dialogs.renameContentPath;
                commands.renameContentTo = typed;
            }
            dialogs.renameTarget = {};
            dialogs.renameContentPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // A material, or a variant of the one right-clicked (ADR 0090). One box:
    // what differs between the two is only what the new file names as its
    // parent, which the browser already knows.
    if (beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.new_material"), "###New Material").c_str(), 400.0f,
                          [&]() { dialogs.newMaterialParent.clear(); })) {
        static std::array<char, 128> materialName{};
        if (dialogOpening()) {
            materialName.fill(0);
            ImGui::SetKeyboardFocusHere();
        }
        if (!dialogs.newMaterialParent.empty())
            ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.variant_of"),
                                              {{"parent", dialogs.newMaterialParent}})
                                         .c_str());
        ImGui::SetNextItemWidth(-1.0f);
        const bool submitted = ImGui::InputText("##material", materialName.data(), materialName.size(),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string typed(materialName.data());
        const std::string resolved = Editor::normalizeMaterialPath(typed);
        if (!typed.empty())
            ImGui::TextDisabled(
                "%s", core::tr(ENG_TR("engine.editor.editor_dialogs.content_path"), {{"path", resolved}}).c_str());

        ImGui::Spacing();
        ImGui::BeginDisabled(typed.empty() || !ContentTree::isUsableName(typed));
        const bool accepted =
            dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.create")), ImVec2(120.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(120.0f, 0.0f)) ||
            dialogCancelled()) {
            dialogs.newMaterialParent.clear();
            ImGui::CloseCurrentPopup();
        }
        if ((submitted || accepted) && !typed.empty() && ContentTree::isUsableName(typed)) {
            if (dialogs.newMaterialParent.empty()) {
                commands.newMaterial = typed;
            }
            else {
                commands.newMaterialVariantOf = dialogs.newMaterialParent;
                commands.newMaterialVariantName = typed;
            }
            dialogs.newMaterialParent.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // A surface shader, written from the template (ADR 0091) and opened in the
    // text editor, where the template explains itself.
    if (beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.new_surface_shader"), "###New Surface Shader").c_str(),
                          400.0f, []() {})) {
        static std::array<char, 128> shaderName{};
        if (dialogOpening()) {
            shaderName.fill(0);
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool submitted =
            ImGui::InputText("##shader", shaderName.data(), shaderName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string typed(shaderName.data());
        if (!typed.empty())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.content_path"),
                                               {{"path", Editor::normalizeShaderPath(typed)}})
                                          .c_str());

        ImGui::Spacing();
        ImGui::BeginDisabled(typed.empty() || !ContentTree::isUsableName(typed));
        const bool accepted =
            dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.create")), ImVec2(120.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(120.0f, 0.0f)) ||
            dialogCancelled())
            ImGui::CloseCurrentPopup();
        if ((submitted || accepted) && !typed.empty() && ContentTree::isUsableName(typed)) {
            commands.newShader = typed;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.new_folder"), "###New Folder").c_str(), 400.0f,
                          []() {})) {
        static std::array<char, 128> folder{};
        ImGui::SetNextItemWidth(-1.0f);
        // Cleared as well as focused: dismissed by clicking outside, this box
        // used to come back holding the last folder somebody made.
        if (dialogOpening()) {
            folder.fill(0);
            ImGui::SetKeyboardFocusHere();
        }
        const bool submitted =
            ImGui::InputText("##folder", folder.data(), folder.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string typed(folder.data());

        ImGui::Spacing();
        // Greyed before the press rather than refused after it: a name a
        // filesystem cannot carry is knowable while it is being typed.
        ImGui::BeginDisabled(!ContentTree::isUsableName(typed));
        const bool accepted =
            dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.create")), ImVec2(120.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(120.0f, 0.0f)) ||
            dialogCancelled()) {
            folder.fill(0);
            ImGui::CloseCurrentPopup();
        }

        if ((submitted || accepted) && ContentTree::isUsableName(typed)) {
            commands.createFolder = typed;
            folder.fill(0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (beginEditorDialog(
            labelled(ENG_TR("engine.editor.dialog.new_stamp_from_class"), "###New Stamp From Class").c_str(), 400.0f,
            [&]() { dialogs.newStampClass = scene::InvalidClass; })) {
        static std::array<char, 128> stampName{};
        // **What it makes, in one line.** A stamp is a file of an instance, so
        // this makes both: the instance goes into the world where it can be
        // edited, and the file into the browser where it can be reused.
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.create_an_object_in_the")));
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        if (dialogOpening()) {
            stampName.fill(0);
            ImGui::SetKeyboardFocusHere();
        }
        const bool submitted =
            ImGui::InputText("##stamp-name", stampName.data(), stampName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string typed(stampName.data());

        ImGui::Spacing();
        // Greyed before the press rather than refused after it: a name a
        // filesystem cannot carry is knowable while it is being typed. What it
        // CANNOT know here is whether the name is taken -- that is a question
        // about a folder, and the answer comes back as a refusal with a message.
        ImGui::BeginDisabled(!Editor::stampNameIsUsable(typed));
        const bool accepted =
            dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.create")), ImVec2(120.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(120.0f, 0.0f)) ||
            dialogCancelled()) {
            stampName.fill(0);
            dialogs.newStampClass = scene::InvalidClass;
            ImGui::CloseCurrentPopup();
        }

        if ((submitted || accepted) && Editor::stampNameIsUsable(typed)) {
            commands.newStampClass = dialogs.newStampClass;
            commands.newStampName = typed;
            stampName.fill(0);
            dialogs.newStampClass = scene::InvalidClass;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.new_stamp"), "###New Stamp").c_str(), 440.0f,
                          [&]() { dialogs.stampSubject = {}; })) {
        static std::array<char, 128> stamp{};
        // **What this does, in one sentence, because the second half surprises
        // people**: it writes a file AND turns the thing you made it from into
        // an instance of that file. A source plus a copy of it that nothing
        // connects is two things that drift apart by tomorrow.
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.writes_the_selected_subtree_to")));
        ImGui::Spacing();
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.editor_dialogs.content")));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (dialogOpening()) {
            stamp.fill(0);
            ImGui::SetKeyboardFocusHere();
        }
        const bool submitted =
            ImGui::InputText("##stamp", stamp.data(), stamp.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string typed(stamp.data());
        const bool usable = !typed.empty() && Editor::stampNameIsUsable(typed);

        ImGui::Spacing();
        // The RESOLVED path while it is being typed, which is the half that
        // makes a rule visible rather than surprising -- D068 is what happens
        // without it.
        if (usable)
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.writes_path"),
                                               {{"path", Editor::normalizeStampPath(typed)}})
                                          .c_str());
        else if (typed.empty())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.a_bare_name_lands_in")));
        else
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.not_a_name_a_file")));

        ImGui::Spacing();
        ImGui::BeginDisabled(!usable || !dialogs.stampSubject.valid());
        const bool accepted =
            dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.create")), ImVec2(120.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(120.0f, 0.0f)) ||
            dialogCancelled()) {
            stamp.fill(0);
            dialogs.stampSubject = {};
            ImGui::CloseCurrentPopup();
        }

        if ((submitted || accepted) && usable && dialogs.stampSubject.valid()) {
            commands.stampSubject = dialogs.stampSubject;
            commands.stampName = typed;
            stamp.fill(0);
            dialogs.stampSubject = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.delete"), "###Delete").c_str(), 440.0f,
                          [&]() { dialogs.deleteContentPath.clear(); })) {
        // **Named, not counted.** "Delete 1 item?" is a question nobody can
        // answer; the path is what tells somebody whether they meant it.
        ImGui::TextWrapped(
            "%s", core::tr(ENG_TR("engine.editor.editor_dialogs.delete_path"), {{"path", dialogs.deleteContentPath}})
                      .c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.it_is_moved_to_the")));
        ImGui::Spacing();

        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.delete")), ImVec2(120.0f, 0.0f))) {
            commands.deleteContent = dialogs.deleteContentPath;
            dialogs.deleteContentPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        // **Escape is Cancel on a question too**, and on this one it is the
        // answer somebody reaches for fastest: a confirmation you cannot back
        // out of with the key every other dialog uses is a confirmation people
        // learn to click through without reading.
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(120.0f, 0.0f)) ||
            dialogCancelled()) {
            dialogs.deleteContentPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // **The question every application with a document asks**, asked in one
    // place: closing, starting over, opening something else and leaving for
    // another project all lose the same edits, so `dialogs.pending` remembers
    // which of them asked and the answer re-issues it.
    if (beginEditorDialog(labelled(ENG_TR("engine.editor.dialog.unsaved_changes"), "###Unsaved Changes").c_str(),
                          460.0f, [&]() {
                              dialogs.pending = EditorDialogs::Pending::None;
                              dialogs.pendingScene.clear();
                              editor.clearCloseRequest();
                          })) {
        const bool stampOpen = editor.stampSession().open();
        // **Everything that would be lost, named** -- the stamp, the scene, the
        // material, a shader -- and Save saves every one of them. It used to
        // save the stamp OR the scene, and a material's edits went unasked.
        const bool sceneUntitled = editor.openScenePath().empty();
        const bool sceneChanged = editor.sceneDirty();
        const bool haveSomewhereToSave = !(sceneUntitled && sceneChanged && !stampOpen);
        std::vector<std::string> unsaved;
        if (stampOpen && editor.stampSession().dirty)
            unsaved.push_back(editor.stampSession().path);
        if (sceneChanged && !sceneUntitled)
            unsaved.push_back(editor.openScenePath());
        if (editor.materialSession().dirty())
            unsaved.push_back(editor.materialSession().path);
        if (editor.fileTabsUnsaved())
            unsaved.emplace_back(core::tr(ENG_TR("engine.editor.editor_dialogs.an_open_shader")));

        if (!haveSomewhereToSave) {
            // An untitled scene has nowhere to go, so the honest question is a
            // different one and the buttons below say so.
            ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.this_scene_has_never_been")));
        }
        else {
            std::string list;
            for (const std::string& name : unsaved)
                list += (list.empty() ? "" : ", ") + name;
            ImGui::TextWrapped(
                "%s", list.empty()
                          ? core::tr(ENG_TR("engine.editor.editor_dialogs.save_this_project"))
                          : core::tr(ENG_TR("engine.editor.editor_dialogs.save_changes_to"), {{"list", list}}).c_str());
            // **Named, because Save cannot keep it** (audit A5): with a stamp
            // open, the untitled scene under it has nowhere to be written, and
            // it went without a word. Cancel, close the stamp, Save As.
            if (stampOpen && sceneUntitled && sceneChanged) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s",
                                   core::tr(ENG_TR("engine.editor.editor_dialogs.the_untitled_scene_under_this")));
            }
        }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.unsaved_edits_will_be_lost")));
        ImGui::Spacing();

        // Three answers and no more, in the order every application puts them:
        // keep the work, throw it away, or change your mind.
        const auto proceed = [&]() {
            // The answer re-issues the verb through the gate that raised the
            // question, with the work now knowingly discarded -- so there is
            // one place that turns a verb into a command and it is the same one
            // the doors call.
            issueOrAsk(dialogs.pending, /*unsavedWork=*/false, dialogs.pendingScene, dialogs, commands);
            dialogs.pending = EditorDialogs::Pending::None;
            dialogs.pendingScene.clear();
            editor.clearCloseRequest();
            ImGui::CloseCurrentPopup();
        };

        if (dialogButton(core::tr(haveSomewhereToSave ? ENG_TR("engine.editor.editor_dialogs.save")
                                                      : ENG_TR("engine.editor.editor_dialogs.save_as")),
                         ImVec2(130.0f, 0.0f))) {
            if (haveSomewhereToSave) {
                commands.saveAll = true;
                proceed();
            }
            else {
                // **The name has to be typed before anything can be written**,
                // and what was asked for is dropped rather than queued behind a
                // second dialog: a person who has just been asked where to save
                // will ask again for whatever they wanted, and an action that
                // fired after an unrelated box closed would be a surprise.
                commands.wantSaveAs = true;
                dialogs.pending = EditorDialogs::Pending::None;
                dialogs.pendingScene.clear();
                editor.clearCloseRequest();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.don_t_save")), ImVec2(130.0f, 0.0f)))
            proceed();
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel")), ImVec2(130.0f, 0.0f))) {
            dialogs.pending = EditorDialogs::Pending::None;
            dialogs.pendingScene.clear();
            editor.clearCloseRequest();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (beginEditorDialog(aboutTitle().c_str(), 430.0f, []() {})) {
        ImGui::TextUnformatted(std::string(core::kBrandName).c_str());
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.an_open_source_game_engine")));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_dialogs.about_version"),
                                           {{"version", std::string_view(ENG_VERSION_STRING)}})
                                      .c_str());
        ImGui::Spacing();
        if (dialogButton(core::tr(ENG_TR("engine.editor.editor_dialogs.close")), ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// The editor's furniture. The panels inside it are the overlay's own, which is
// the whole argument of ADR 0046: what an editor mostly is, this engine already
// had.
// **The class icons on the tabs of the central node** -- a camera on the
// Viewport, a script's own icon on each open script.
//
// Painted over the tab bar rather than submitted into it, because ImGui's tab
// bar takes a string: `BeginTabItem` measures a label and draws it, and there is
// no per-tab draw callback and no icon font in this build. So a tab that wants
// a picture reserves the room in its own label with `tabIconPad()` and this
// fills the gap in afterwards.
//
// Into the HOST WINDOW's draw list and not the foreground one, so a floating
// panel dragged over the strip covers the icons the way it covers the tabs
// underneath them. The clip rect is pushed by hand because the window has
// already been ended, and a scrolled tab bar must not paint an icon over its
// own scroll arrows.
//
// Reads the tab bar's layout from THIS frame, which is why the call site is the
// last thing `drawEditorShell` does.
void drawTabIcons(ImGuiID dockspace, const IconAtlas* icons, const scene::World* world, const ScriptEditor* scripts)
{
    if (icons == nullptr || !icons->ready() || g_device == nullptr)
        return;

    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    SDL_GPUTexture* native = rhi::nativeTexture(*g_device, icons->texture());
    if (native == nullptr)
        return;

    const auto iconForName = [&](std::string_view name) {
        std::string id;
        if (name.ends_with("###Viewport")) {
            // The camera, because that is what a viewport IS -- and it is the
            // icon the Explorer already draws on the one in the tree, which is
            // the whole reason to reuse it rather than draw a second picture of
            // the same idea.
            id = std::string(icons::ClassCamera);
        }
        else if (name.ends_with("###Explorer"))
            id = std::string(icons::ClassModel);
        else if (name.ends_with("###Properties"))
            id = std::string(icons::ActionSettings);
        else if (name.ends_with("###Content"))
            id = std::string(icons::ContentFolder);
        else if (name.ends_with("###Console"))
            id = std::string(icons::ClassScriptService);
        else if (name.ends_with("###Streaming"))
            id = std::string(icons::ClassStreamingService);
        else if (name.ends_with("###Viewport Settings"))
            id = std::string(icons::ClassCamera);
        else if (name.ends_with("###Stats"))
            id = std::string(icons::ClassDebugService);
        else if (name.ends_with("###Terrain"))
            id = std::string(icons::ClassTerrain);
        else if (name.ends_with("###Tiles"))
            id = std::string(icons::ClassTilemap2D);
        else if (name.ends_with("###Water"))
            id = std::string(WaterIcon);
        else if (name.ends_with("###Blocks"))
            id = std::string(icons::ClassVoxelService);
        else if (name.ends_with("###Debug"))
            id = std::string(icons::ClassDebugService);
        else if (name.ends_with("###Welcome"))
            id = std::string(icons::ActionInformation);
        else if (name.ends_with("###Saves"))
            id = std::string(icons::ActionSave);
        else if (name.ends_with("###Material"))
            id = std::string(icons::ContentMaterial);
        else if (name.ends_with("###Export"))
            id = std::string(icons::ActionExport);
        else if (scripts != nullptr && world != nullptr) {
            for (const OpenScript& tab : scripts->tabs()) {
                if (!name.ends_with(scriptWindowId(tab)))
                    continue;
                if (tab.origin == ScriptOrigin::File) {
                    id = std::string(icons::ContentShader);
                    break;
                }
                // Its own class, so a `ModuleScript` tab and a `Script` tab are
                // told apart at a glance -- which is the question somebody with
                // six tabs open actually has.
                if (world->alive(tab.instance))
                    id = classIconFor(icons, world->classes(), world->atoms(), world->classOf(tab.instance));
                break;
            }
        }
        return id;
    };

    const auto unsavedScript = [&](std::string_view name) {
        if (scripts == nullptr)
            return false;
        for (const OpenScript& tab : scripts->tabs()) {
            if (name.ends_with(scriptWindowId(tab)))
                return tab.dirty();
        }
        return false;
    };

    const auto paintNode = [&](auto&& self, ImGuiDockNode* current) -> void {
        if (current == nullptr)
            return;
        self(self, current->ChildNodes[0]);
        self(self, current->ChildNodes[1]);
        if (current->TabBar == nullptr || current->HostWindow == nullptr)
            return;
        const ImGuiTabBar& bar = *current->TabBar;
        const float size = ImGui::GetFontSize();
        ImDrawList* draw = current->HostWindow->DrawList;
        draw->PushClipRect(bar.BarRect.Min, bar.BarRect.Max, true);

        for (const ImGuiTabItem& item : bar.Tabs) {
            if (item.Window == nullptr)
                continue;

            const std::string id = iconForName(item.Window->Name);
            if (id.empty())
                continue;

            const IconSprite sprite = icons->find(id, static_cast<core::u32>(size + 0.5f));
            if (!sprite.valid)
                continue;

            const float x = bar.BarRect.Min.x + item.Offset - bar.ScrollingAnim + ImGui::GetStyle().FramePadding.x;
            const float y = bar.BarRect.Min.y + (bar.BarRect.GetHeight() - size) * 0.5f;
            const ImVec4 tint = iconTint(icons, id);
            draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)), ImVec2(x, y),
                           ImVec2(x + size, y + size), ImVec2(sprite.u0, sprite.v0), ImVec2(sprite.u1, sprite.v1),
                           ImGui::GetColorU32(tint));

            // **An unsaved script ends in a dot**, in the gap its label leaves
            // after the title (see `drawScriptEditor`). A floppy was tried and
            // taken back (the owner: "the dot, as it was, looks better"): a
            // dot is the mark every editor uses for this, and it reads at a
            // glance where a picture has to be looked at.
            if (unsavedScript(item.Window->Name)) {
                const std::string_view name = item.Window->Name;
                const std::string_view shown = name.substr(0, name.find("###"));
                const float gap = ImGui::CalcTextSize(tabIconPad().c_str()).x;
                const float end = x + ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x;
                const float radius = std::max(2.5f, std::floor(size * 0.2f));
                draw->AddCircleFilled(ImVec2(std::floor(end - gap * 0.5f), std::floor(y + size * 0.5f)), radius,
                                      ImGui::GetColorU32(themeColor(palette().warning)));
            }
        }

        draw->PopClipRect();
    };
    paintNode(paintNode, node);
    // Floating dock groups have their own root; ordinary floating windows have a title bar.
    ImGuiContext& context = *ImGui::GetCurrentContext();
    for (const ImGuiStoragePair& pair : context.DockContext.Nodes.Data) {
        auto* floating = static_cast<ImGuiDockNode*>(pair.val_p);
        if (floating != nullptr && floating != node && floating->ParentNode == nullptr)
            paintNode(paintNode, floating);
    }
    for (ImGuiWindow* window : context.Windows) {
        if (!window->Active || window->Hidden || window->DockIsActive ||
            (window->Flags & ImGuiWindowFlags_NoTitleBar) != 0)
            continue;
        const std::string id = iconForName(window->Name);
        if (id.empty())
            continue;
        const float size = ImGui::GetFontSize();
        const IconSprite sprite = icons->find(id, static_cast<core::u32>(size + 0.5f));
        if (!sprite.valid)
            continue;
        const ImRect title = window->TitleBarRect();
        // Shared theme keeps titles left aligned and hides the collapse button.
        const ImVec2 origin(title.Min.x + window->WindowBorderSize + ImGui::GetStyle().FramePadding.x,
                            title.Min.y + (title.GetHeight() - size) * 0.5f);
        window->DrawList->PushClipRect(title.Min, title.Max, false);
        window->DrawList->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)), origin,
                                   ImVec2(origin.x + size, origin.y + size), ImVec2(sprite.u0, sprite.v0),
                                   ImVec2(sprite.u1, sprite.v1), ImGui::GetColorU32(iconTint(icons, id)));
        window->DrawList->PopClipRect();
    }
}

// Declared here and defined beside the other overlay panels, because the editor
// shell is drawn above them in this file and the Streaming panel needs it.
void drawStreaming(const StreamingHost& streaming);

// The Terrain panel's contents (F1, remade 2026-09-29).
//
// **One job at a time.** The panel was every section at once -- create,
// sculpt, brush, paint, rules, foliage, heightmap, settings -- in one column a
// person scrolled through to find the one they wanted, with the tool that was
// acting nowhere in sight (the owner: "not practical, not easy to understand").
// It is five tabs now, in the order the work goes: shape the ground, paint it,
// grow things on it, and -- once, at the start -- make it and set it up. The
// tools are tiles named for what they do to the ground, the brush is two
// sliders, and the keys that make a brush fast are written under it.
//
// **A tab is not a tool.** Opening the panel, or a tab, puts nothing in the
// hand (the owner, 2026-09-23: a brush that followed the pointer over ground
// nobody meant to edit). A tile does, and the viewport says so until Escape or
// a click on something else puts it down.
namespace {

enum class TerrainMode : core::u8
{
    Sculpt,
    Paint,
    Foliage,
    Create,
    Settings,
};

TerrainMode g_terrainMode = TerrainMode::Sculpt;
Editor::Tool g_terrainLastTool = Editor::Tool::Select;

// The fixed height Flatten levels to when it is not the stroke's own, kept here
// so switching between the two keeps the number somebody typed. Nothing until
// somebody does: the first is the ground's (the terrain audit's editor list).
std::optional<f32> g_flattenFixed;
// The ground's world height at the terrain's position, for a fixed Flatten
// height chosen before the brush has aimed at any ground.
std::optional<f32> g_groundAtTerrain;

[[nodiscard]] std::string terrainLayerName(const std::string& urn)
{
    std::string name = urn;
    if (const std::size_t slash = name.find_last_of('/'); slash != std::string::npos)
        name = name.substr(slash + 1);
    if (name.ends_with(".material.json"))
        name.resize(name.size() - std::string_view(".material.json").size());
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

// The engine's own layers in their palette colours; a project's in its
// material's colour.
[[nodiscard]] ImVec4 terrainLayerColor(scene::World& world, const std::vector<std::string>& engineLayers,
                                       const std::string& urn)
{
    const auto at = std::find(engineLayers.begin(), engineLayers.end(), urn);
    if (at != engineLayers.end()) {
        const core::Vec3 c = asset::terrainColorOf(static_cast<core::u8>(at - engineLayers.begin() + 1));
        return ImVec4(c.x, c.y, c.z, 1.0f);
    }
    const asset::ResolvedMaterial material = world.resolveMaterial(world.atoms().intern(urn), 0);
    core::Color3 color = material.properties.color;
    // A material coloured by one of the engine's terrain textures -- a starter
    // is a variant of a built-in one -- is that texture's colour, tinted: its
    // own colour factor is white, and a row of white swatches says nothing.
    const std::string& map = material.properties.colorMap;
    if (map.starts_with(asset::EngineTerrainPrefix)) {
        const std::string layer = map.substr(0, map.find('/', asset::EngineTerrainPrefix.size()));
        const auto engine = std::find(engineLayers.begin(), engineLayers.end(), layer);
        if (engine != engineLayers.end()) {
            const core::Vec3 c = asset::terrainColorOf(static_cast<core::u8>(engine - engineLayers.begin() + 1));
            color = core::Color3{c.x * color.r, c.y * color.g, c.z * color.b};
        }
    }
    return ImVec4(color.r, color.g, color.b, 1.0f);
}

// One cell of the mode bar: an icon over a word, the chosen one lit. **A bar
// and not tabs**, because the dock beside the viewport is narrow and five tabs
// in it were cut to "Scu..." and "Cre...".
bool terrainModeCell(const IconAtlas* icons, std::string_view icon, const char* word, bool on, float width)
{
    const float glyph = ImGui::GetFontSize() * 1.15f;
    const float height = glyph + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 3.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(word);
    const bool clicked = ImGui::InvisibleButton("##mode", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + width, min.y + height);
    const ImU32 fill =
        ImGui::GetColorU32(on ? ImGuiCol_ButtonActive : (hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button));
    list->AddRectFilled(min, max, fill, ImGui::GetStyle().FrameRounding);
    if (on)
        list->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 2.0f), ImVec2(max.x - 4.0f, max.y),
                            ImGui::GetColorU32(ImGuiCol_NavHighlight));
    paintActionIcon(icons, icon, ImVec2(min.x + (width - glyph) * 0.5f, min.y + ImGui::GetStyle().FramePadding.y),
                    glyph, list);
    const ImVec2 text = ImGui::CalcTextSize(word);
    list->AddText(
        ImVec2(min.x + std::max(0.0f, (width - text.x) * 0.5f), max.y - text.y - ImGui::GetStyle().FramePadding.y),
        ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), word);
    return clicked;
}

// A tile: an icon and a word, highlighted while it is the tool in hand, and
// with a quieter highlight when the held keys have turned the brush into it.
bool terrainTile(const IconAtlas* icons, std::string_view icon, const char* word, bool on, bool borrowed, float width)
{
    if (on)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    else if (borrowed)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    const bool clicked = labeledIconButton(icons, icon, word, ImVec2(width, ImGui::GetFrameHeight() * 1.6f));
    if (on || borrowed)
        ImGui::PopStyleColor();
    return clicked;
}

// Size and strength, which every brush has, and what only some have.
// **Where Paint may paint** (B4): a slope band, a height band, and the
// materials under it -- a road that keeps off the cliffs, snow above a line,
// moss over the rock and nothing else.
void drawPaintMask(Editor& editor, const scene::TerrainComponent& terrain)
{
    if (!ImGui::CollapsingHeader(core::tr(ENG_TR("engine.editor.terrain.mask.title"))))
        return;
    asset::PaintMask mask = editor.paintMask();
    bool changed = false;
    changed |= ImGui::Checkbox(core::tr(ENG_TR("engine.editor.terrain.mask.slope")), &mask.bySlope);
    if (mask.bySlope) {
        float range[2] = {mask.slopeMin, mask.slopeMax};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat2("##mask-slope", range, 0.5f, 0.0f, 90.0f,
                              core::tr(ENG_TR("engine.editor.unit.degrees_0")))) {
            mask.slopeMin = std::min(range[0], range[1]);
            mask.slopeMax = std::max(range[0], range[1]);
            changed = true;
        }
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.mask.slope_tip")));
    }
    changed |= ImGui::Checkbox(core::tr(ENG_TR("engine.editor.terrain.mask.height")), &mask.byHeight);
    if (mask.byHeight) {
        float range[2] = {mask.heightMin, mask.heightMax};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat2("##mask-height", range, 0.25f, -4096.0f, 4096.0f,
                              core::tr(ENG_TR("engine.editor.unit.metres_1")))) {
            mask.heightMin = std::min(range[0], range[1]);
            mask.heightMax = std::max(range[0], range[1]);
            changed = true;
        }
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.mask.height_tip")));
    }
    changed |= ImGui::Checkbox(core::tr(ENG_TR("engine.editor.terrain.mask.material")), &mask.byMaterial);
    if (mask.byMaterial) {
        for (std::size_t at = 0; at < terrain.layers.size() && at < 255; ++at) {
            const auto id = static_cast<core::u8>(at + 1);
            bool on = mask.allows(id);
            if (ImGui::Checkbox((terrainLayerName(terrain.layers[at]) + "##mask-" + std::to_string(at)).c_str(), &on)) {
                if (on)
                    mask.allow(id);
                else
                    mask.materials[id / 64u] &= ~(core::u64{1} << (id % 64u));
                changed = true;
            }
        }
    }
    if (changed)
        editor.setPaintMask(mask);
}

// Every voxel of one material made another, across the terrain (B4).
void drawReplaceMaterial(Editor& editor, scene::World& world, core::InstanceId root,
                         const scene::TerrainComponent& terrain)
{
    if (terrain.layers.size() < 2 || !ImGui::CollapsingHeader(core::tr(ENG_TR("engine.editor.terrain.replace.title"))))
        return;
    static int from = 0;
    static int to = 1;
    const auto count = static_cast<int>(std::min<std::size_t>(terrain.layers.size(), 255));
    from = std::clamp(from, 0, count - 1);
    to = std::clamp(to, 0, count - 1);
    const auto pick = [&](const char* id, int& which) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, terrainLayerName(terrain.layers[static_cast<std::size_t>(which)]).c_str())) {
            for (int at = 0; at < count; ++at) {
                if (ImGui::Selectable(terrainLayerName(terrain.layers[static_cast<std::size_t>(at)]).c_str(),
                                      at == which))
                    which = at;
            }
            ImGui::EndCombo();
        }
    };
    pick("##replace-from", from);
    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.replace.becomes")));
    pick("##replace-to", to);
    ImGui::BeginDisabled(from == to);
    if (ImGui::Button(core::tr(ENG_TR("engine.editor.terrain.replace.button")), ImVec2(-FLT_MIN, 0.0f)))
        (void)editor.replaceMaterialEverywhere(world, root, static_cast<core::u8>(from + 1),
                                               static_cast<core::u8>(to + 1));
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.replace.tip")));
}

// **How Paint goes on** (ADR 0114): the four modes, and how soft its rim is.
void drawPaintMode(Editor& editor)
{
    const Editor::Brush& brush = editor.brush();
    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.paint.mode_title")));
    struct Mode
    {
        asset::PaintMode mode;
        core::TextKey label;
        core::TextKey tip;
    };
    static constexpr std::array<Mode, 4> Modes{{
        {asset::PaintMode::Blend, ENG_TR("engine.editor.terrain.paint.blend"),
         ENG_TR("engine.editor.terrain.paint.blend_tip")},
        {asset::PaintMode::Replace, ENG_TR("engine.editor.terrain.paint.replace"),
         ENG_TR("engine.editor.terrain.paint.replace_tip")},
        {asset::PaintMode::Under, ENG_TR("engine.editor.terrain.paint.under"),
         ENG_TR("engine.editor.terrain.paint.under_tip")},
        {asset::PaintMode::Erase, ENG_TR("engine.editor.terrain.paint.erase"),
         ENG_TR("engine.editor.terrain.paint.erase_tip")},
    }};
    for (std::size_t at = 0; at < Modes.size(); ++at) {
        if (at > 0)
            ImGui::SameLine();
        if (ImGui::RadioButton(core::tr(Modes[at].label), brush.paintMode == Modes[at].mode))
            editor.setBrushPaintMode(Modes[at].mode);
        ImGui::SetItemTooltip("%s", core::tr(Modes[at].tip));
    }
    const float label = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.terrain.brush.strength"))).x +
                        ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.paint.softness")));
    ImGui::SameLine(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    int percent = static_cast<int>(std::lround(brush.falloff * 100.0f));
    if (ImGui::SliderInt("##paint-softness", &percent, 0, 100, core::tr(ENG_TR("engine.editor.unit.percent"))))
        editor.setBrushFalloff(static_cast<f32>(percent) / 100.0f);
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.paint.softness_tip")));
}

void drawBrushControls(Editor& editor, bool strength, bool shape, bool flatten)
{
    const Editor::Brush brush = editor.brush();
    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.brush.title")));
    const float label = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.terrain.brush.strength"))).x +
                        ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.brush.size")));
    ImGui::SameLine(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    f32 radius = brush.radius;
    if (ImGui::SliderFloat("##brush-size", &radius, 0.25f, 64.0f, core::tr(ENG_TR("engine.editor.unit.metres_2")),
                           ImGuiSliderFlags_Logarithmic))
        editor.setBrushRadius(radius);
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.brush.size_tip")));
    if (strength) {
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.brush.strength")));
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        int percent = static_cast<int>(std::lround(brush.strength * 100.0f));
        if (ImGui::SliderInt("##brush-strength", &percent, 2, 100, core::tr(ENG_TR("engine.editor.unit.percent"))))
            editor.setBrushStrength(static_cast<f32>(percent) / 100.0f);
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.brush.strength_tip")));
    }
    if (shape) {
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.brush.shape")));
        ImGui::SameLine(label);
        int square = brush.shape == Editor::BrushShape::Box ? 1 : 0;
        if (ImGui::RadioButton(core::tr(ENG_TR("engine.editor.terrain.brush.round")), &square, 0))
            editor.setBrushShape(Editor::BrushShape::Sphere);
        ImGui::SameLine();
        if (ImGui::RadioButton(core::tr(ENG_TR("engine.editor.terrain.brush.square")), &square, 1))
            editor.setBrushShape(Editor::BrushShape::Box);
    }
    if (flatten) {
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.brush.level_to")));
        const std::optional<f32> fixed = editor.flattenHeight();
        int which = fixed.has_value() ? 1 : 0;
        if (ImGui::RadioButton(core::tr(ENG_TR("engine.editor.terrain.brush.level_stroke")), &which, 0))
            editor.setFlattenHeight(std::nullopt);
        if (ImGui::RadioButton(core::tr(ENG_TR("engine.editor.terrain.brush.level_fixed")), &which, 1))
            editor.setFlattenHeight(
                g_flattenFixed.value_or(editor.lastGroundHeight().value_or(g_groundAtTerrain.value_or(0.0f))));
        if (fixed.has_value()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            f32 height = *fixed;
            if (ImGui::DragFloat("##flatten-height", &height, 0.1f, -4096.0f, 4096.0f,
                                 core::tr(ENG_TR("engine.editor.unit.metres_1")))) {
                g_flattenFixed = height;
                editor.setFlattenHeight(height);
            }
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.brush.level_fixed_tip")));
        }
    }
    if (ImGui::TreeNode(core::tr(ENG_TR("engine.editor.brush_controls.brush_more")), "%s",
                        core::tr(ENG_TR("engine.editor.terrain.brush.more")))) {
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.brush.spacing")));
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        int spacing = static_cast<int>(std::lround(brush.spacing * 100.0f));
        if (ImGui::SliderInt("##brush-spacing", &spacing, 10, 100,
                             core::tr(ENG_TR("engine.editor.unit.percent_of_size"))))
            editor.setBrushSpacing(static_cast<f32>(spacing) / 100.0f);
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.brush.spacing_tip")));
        bool plane = editor.brushPlaneLock();
        if (ImGui::Checkbox(core::tr(ENG_TR("engine.editor.terrain.brush.plane")), &plane))
            editor.setBrushPlaneLock(plane);
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.brush.plane_tip")));
        ImGui::TreePop();
    }
}

void drawTerrainRules(Editor& editor, scene::World& world, core::InstanceId root,
                      const scene::TerrainComponent& terrain, const IconAtlas* icons)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.rules.intro")));
    ImGui::PopTextWrapPos();

    std::vector<asset::TerrainRule> rules = terrain.rules;
    const std::vector<std::string>& layers = terrain.layers;
    const auto nameOf = [&](core::u8 id) {
        const std::string name = id >= 1 && id <= layers.size() ? terrainLayerName(layers[id - 1]) : std::string("?");
        return std::to_string(id) + "  " + name;
    };
    bool changed = false;
    const char* label = core::tr(ENG_TR("engine.editor.history.edit_terrain_rule"));
    core::u64 coalesce = 0;
    std::optional<std::size_t> remove;
    std::optional<std::pair<std::size_t, std::size_t>> swap;

    for (std::size_t index = 0; index < rules.size(); ++index) {
        asset::TerrainRule& rule = rules[index];
        ImGui::PushID(static_cast<int>(index));
        const std::string title = std::to_string(index + 1) + ". " + nameOf(rule.material) + "###rule";
        if (ImGui::Checkbox("##on", &rule.enabled)) {
            changed = true;
            label = rule.enabled ? core::tr(ENG_TR("engine.editor.history.enable_terrain_rule"))
                                 : core::tr(ENG_TR("engine.editor.history.disable_terrain_rule"));
        }
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.rules.on_tip")));
        ImGui::SameLine();
        if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            const auto row = [&](const char* text) {
                ImGui::TextUnformatted(text);
                ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
                ImGui::SetNextItemWidth(-FLT_MIN);
            };
            // A drag's frames are one step: the key is the rule and field.
            const auto edited = [&](bool moved, core::u64 field) {
                if (moved) {
                    changed = true;
                    coalesce = 0x52554C45000000ull | (static_cast<core::u64>(index) << 8) | field;
                }
            };
            row(core::tr(ENG_TR("engine.editor.terrain.rules.material")));
            if (ImGui::BeginCombo("##material", nameOf(rule.material).c_str())) {
                for (std::size_t id = 1; id <= layers.size(); ++id) {
                    if (ImGui::Selectable(nameOf(static_cast<core::u8>(id)).c_str(), rule.material == id)) {
                        rule.material = static_cast<core::u8>(id);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            row(core::tr(ENG_TR("engine.editor.terrain.rules.slope")));
            float slope[2] = {rule.slopeMin, rule.slopeMax};
            edited(ImGui::DragFloat2("##slope", slope, 0.25f, 0.0f, 90.0f,
                                     core::tr(ENG_TR("engine.editor.unit.degrees_1"))),
                   1);
            // In order, as the paint mask keeps its own (the editor list).
            rule.slopeMin = std::clamp(std::min(slope[0], slope[1]), 0.0f, 90.0f);
            rule.slopeMax = std::clamp(std::max(slope[0], slope[1]), 0.0f, 90.0f);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.rules.slope_tip")));
            row(core::tr(ENG_TR("engine.editor.terrain.rules.height")));
            float height[2] = {rule.heightMin, rule.heightMax};
            edited(ImGui::DragFloat2("##height", height, 0.25f, -100000.0f, 100000.0f,
                                     core::tr(ENG_TR("engine.editor.unit.metres_1"))),
                   2);
            rule.heightMin = std::min(height[0], height[1]);
            rule.heightMax = std::max(height[0], height[1]);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.rules.height_tip")));
            row(core::tr(ENG_TR("engine.editor.terrain.rules.blend")));
            edited(ImGui::DragFloat("##blend", &rule.blend, 0.1f, 0.0f, 1000.0f, "%.1f"), 3);
            rule.blend = std::max(rule.blend, 0.0f);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.rules.blend_tip")));
            row(core::tr(ENG_TR("engine.editor.terrain.rules.noise")));
            edited(ImGui::DragFloat("##noise", &rule.noise, 0.005f, 0.0f, 2.0f, "%.3f"), 4);
            rule.noise = std::max(rule.noise, 0.0f);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.rules.noise_tip")));
            row(core::tr(ENG_TR("engine.editor.terrain.rules.covers")));
            const std::string covers = rule.appliesTo.empty()
                                           ? std::string(core::tr(ENG_TR("engine.editor.terrain.rules.covers_all")))
                                           : core::tr(ENG_TR("engine.editor.terrain.rules.covers_some"),
                                                      {{"count", static_cast<core::i64>(rule.appliesTo.size())}});
            if (ImGui::BeginCombo("##covers", covers.c_str())) {
                for (std::size_t id = 1; id <= layers.size(); ++id) {
                    const auto layer = static_cast<core::u8>(id);
                    const bool listed =
                        std::find(rule.appliesTo.begin(), rule.appliesTo.end(), layer) != rule.appliesTo.end();
                    bool on = rule.appliesTo.empty() ? layer != rule.material : listed;
                    if (ImGui::Checkbox(nameOf(layer).c_str(), &on)) {
                        // From "every layer but its own" to an explicit list the
                        // first time one is unticked.
                        if (rule.appliesTo.empty()) {
                            for (std::size_t other = 1; other <= layers.size(); ++other) {
                                if (other != rule.material)
                                    rule.appliesTo.push_back(static_cast<core::u8>(other));
                            }
                        }
                        std::erase(rule.appliesTo, layer);
                        if (on)
                            rule.appliesTo.push_back(layer);
                        std::sort(rule.appliesTo.begin(), rule.appliesTo.end());
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.rules.covers_tip")));
            if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.terrain.rules.up"))) && index > 0)
                swap = std::pair{index, index - 1};
            ImGui::SameLine();
            if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.terrain.rules.down"))) && index + 1 < rules.size())
                swap = std::pair{index, index + 1};
            ImGui::SameLine();
            if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.terrain.rules.remove"))))
                remove = index;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (swap.has_value()) {
        std::swap(rules[swap->first], rules[swap->second]);
        changed = true;
        label = core::tr(ENG_TR("engine.editor.history.reorder_terrain_rules"));
        coalesce = 0;
    }
    if (remove.has_value()) {
        rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(*remove));
        changed = true;
        label = core::tr(ENG_TR("engine.editor.history.remove_terrain_rule"));
        coalesce = 0;
    }
    ImGui::BeginDisabled(rules.size() >= asset::MaxTerrainRules);
    if (labeledIconButton(icons, icons::ActionAdd, core::tr(ENG_TR("engine.editor.terrain.rules.add")),
                          ImVec2(-FLT_MIN, 0.0f))) {
        // A height rule, which is the one people reach for after the slope
        // rock: snow above a line. **Of the layer called snow**, where there is
        // one, and the last layer where not (the editor list: it took layer 4,
        // whatever that was).
        asset::TerrainRule rule;
        rule.material = static_cast<core::u8>(std::max<std::size_t>(layers.size(), 1));
        for (std::size_t id = 1; id <= layers.size(); ++id) {
            std::string name = terrainLayerName(layers[id - 1]);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            if (name.find("snow") != std::string::npos) {
                rule.material = static_cast<core::u8>(id);
                break;
            }
        }
        rule.slopeMin = 0.0f;
        rule.slopeMax = 90.0f;
        rule.heightMin = 40.0f;
        rule.blend = 4.0f;
        rule.noise = 0.25f;
        rules.push_back(std::move(rule));
        changed = true;
        label = core::tr(ENG_TR("engine.editor.history.add_terrain_rule"));
        coalesce = 0;
    }
    ImGui::EndDisabled();
    if (changed)
        (void)editor.setTerrainRules(world, root, std::move(rules), label, coalesce);

    ImGui::BeginDisabled(terrain.rules.empty() || terrain.field.empty());
    if (labeledIconButton(icons, icons::ActionPaint, core::tr(ENG_TR("engine.editor.terrain.rules.apply")),
                          ImVec2(-FLT_MIN, 0.0f)))
        (void)editor.applyTerrainRules(world, root);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.rules.apply_tip")));
}

// The terrain's layers as swatches, the brush's material among them, and the
// list's own edits.
//
// **A terrain's materials come from Content** (D330, the owner: "tem que ser
// feito como é feito em outras game engines"): a `+` after the swatches picks
// one of the project's materials, a material dragged from the browser onto the
// `+` adds it and onto a swatch puts it in that layer's place, and a swatch's
// right-click replaces, opens or removes it. The panel never makes a material:
// that is Content's, and starter terrain materials are one of its New items.
void drawTerrainLayers(Editor& editor, scene::World& world, core::InstanceId root,
                       const scene::TerrainComponent* terrain)
{
    if (terrain == nullptr)
        return;
    const std::vector<std::string> engineLayers = asset::defaultTerrainLayers();
    const std::vector<std::string>& layers = terrain->layers;
    const core::u8 selected = editor.brush().material;

    // Which layer the picker puts its choice in: zero adds one. Opened after
    // the swatches, at this level, so a swatch's menu can ask for it without
    // it being that menu's child.
    static core::u8 picking = 0;
    bool openPicker = false;
    const auto takesMaterial = []() -> std::optional<std::string> {
        if (!ImGui::BeginDragDropTarget())
            return std::nullopt;
        std::optional<std::string> taken;
        const ImGuiPayload* peek = ImGui::GetDragDropPayload();
        if (peek != nullptr && peek->IsDataType(kContentDragPayload) &&
            isMaterialDrag(*static_cast<const ContentDrag*>(peek->Data))) {
            if (const ImGuiPayload* took = ImGui::AcceptDragDropPayload(kContentDragPayload); took != nullptr)
                taken = std::string(static_cast<const ContentDrag*>(took->Data)->path);
        }
        ImGui::EndDragDropTarget();
        return taken;
    };

    if (layers.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.layers.none")));
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.layers.none_how")));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.terrain.layers.starters")), ImVec2(-FLT_MIN, 0.0f)))
            (void)editor.useStarterTerrainMaterials(world, root);
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.layers.starters_tip")));
        ImGui::Spacing();
    }

    const f32 swatch = ImGui::GetFrameHeight() * 1.6f;
    const int columns =
        std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) /
                                     (swatch + ImGui::GetStyle().ItemSpacing.x)));
    for (std::size_t index = 0; index <= layers.size(); ++index) {
        if (index > 0 && index % static_cast<std::size_t>(columns) != 0)
            ImGui::SameLine();
        const auto id = static_cast<core::u8>(index + 1);
        ImGui::PushID(static_cast<int>(id));
        if (index == layers.size()) {
            // The `+`: pick a project material, or drop one.
            ImGui::BeginDisabled(layers.size() >= asset::MaxTerrainLayers);
            if (ImGui::Button("+", ImVec2(swatch, swatch))) {
                picking = 0;
                openPicker = true;
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.layers.add_tip")));
            if (const std::optional<std::string> dropped = takesMaterial(); dropped.has_value())
                (void)editor.addTerrainLayer(world, root, *dropped);
            ImGui::PopID();
            break;
        }
        const bool on = selected == id;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
        }
        if (ImGui::ColorButton("##swatch", terrainLayerColor(world, engineLayers, layers[index]),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(swatch, swatch)))
            editor.setBrushMaterial(id);
        if (on) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        const bool builtIn = asset::isEngineMaterial(layers[index]);
        std::string relative = layers[index];
        if (relative.starts_with(asset::AssetScheme))
            relative = relative.substr(asset::AssetScheme.size());
        // Double-click opens it, as an asset field's does.
        if (!builtIn && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            (void)editor.openMaterial(relative);
        ImGui::SetItemTooltip("%s\n%s\n%s", terrainLayerName(layers[index]).c_str(), layers[index].c_str(),
                              core::tr(ENG_TR("engine.editor.terrain.layers.swatch_tip")));
        if (const std::optional<std::string> dropped = takesMaterial(); dropped.has_value())
            (void)editor.replaceTerrainLayer(world, root, id, *dropped);
        if (ImGui::BeginPopupContextItem("layer-menu")) {
            ImGui::TextDisabled(
                "%s", core::tr(ENG_TR("engine.editor.terrain.layers.menu_title"),
                               {{"id", static_cast<core::i64>(id)}, {"name", terrainLayerName(layers[index])}})
                          .c_str());
            ImGui::Separator();
            if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.terrain.layers.replace_with")))) {
                picking = id;
                openPicker = true;
            }
            if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.terrain.layers.open")), nullptr, false, !builtIn))
                (void)editor.openMaterial(relative);
            // Only the last: removing another would renumber the voxels after it.
            if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.terrain.layers.remove")), nullptr, false,
                                index + 1 == layers.size()))
                (void)editor.removeLastTerrainLayer(world, root);
            if (index + 1 != layers.size())
                ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.layers.remove_last_tip")));
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (selected >= 1 && selected <= layers.size()) {
        ImGui::Text("%s", terrainLayerName(layers[selected - 1]).c_str());
        // A surface shader is not read on terrain (ADR 0113): said here, where
        // somebody picked the material.
        const asset::ResolvedMaterial wearing = world.resolveMaterial(world.atoms().intern(layers[selected - 1]), 0);
        if (!wearing.properties.shader.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().warning));
            ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.terrain.layers.no_shader")));
            ImGui::PopStyleColor();
        }
    }

    // **The picker**: the project's materials, searchable -- the asset field's
    // picker, for a terrain.
    if (openPicker)
        ImGui::OpenPopup("terrain-material-picker");
    if (ImGui::BeginPopup("terrain-material-picker")) {
        static std::vector<std::string> candidates;
        static std::array<char, 96> search{};
        if (ImGui::IsWindowAppearing()) {
            candidates = editor.content().filesOfKind(ContentKind::Material);
            search.fill(0);
            ImGui::SetKeyboardFocusHere();
        }
        if (picking >= 1 && picking <= layers.size())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.layers.pick_in_place"),
                                               {{"name", terrainLayerName(layers[picking - 1])}})
                                          .c_str());
        else
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.layers.pick_add")));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        ImGui::InputTextWithHint("##terrain-material-search", core::tr(ENG_TR("engine.editor.terrain.layers.search")),
                                 search.data(), search.size());
        const std::string_view needle{search.data()};
        ImGui::Separator();
        if (ImGui::BeginChild("terrain-material-list",
                              ImVec2(ImGui::GetFontSize() * 16.0f, ImGui::GetFontSize() * 12.0f))) {
            std::size_t listed = 0;
            for (const std::string& candidate : candidates) {
                if (!needle.empty() && !containsFold(candidate, needle))
                    continue;
                ++listed;
                const std::string urn = std::string(asset::AssetScheme) + candidate;
                const bool has = std::find(layers.begin(), layers.end(), urn) != layers.end();
                ImGui::PushID(candidate.c_str());
                ImGui::ColorButton("##chip", terrainLayerColor(world, engineLayers, urn),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                                   ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
                ImGui::SameLine();
                ImGui::BeginDisabled(has);
                if (ImGui::Selectable(candidate.c_str())) {
                    if (picking >= 1)
                        (void)editor.replaceTerrainLayer(world, root, picking, candidate);
                    else
                        (void)editor.addTerrainLayer(world, root, candidate);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                if (has)
                    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.layers.has_it")));
                ImGui::PopID();
            }
            if (candidates.empty())
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.layers.pick_none")));
            else if (listed == 0)
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.layers.pick_no_match"),
                                                   {{"search", std::string_view(search.data())}})
                                              .c_str());
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
}

// What each sculpt tile does, in the words its tooltip and the line under the
// tiles use.
struct SculptTool
{
    Editor::BrushOp op;
    std::string_view icon;
    core::TextKey word;
    core::TextKey what;
};

} // namespace

// **Ground that is there is asked about before it goes** (the terrain audit's
// editor list): Clear All Ground and the two Replace buttons acted on a click,
// and one of them sat under Export Heightmap. The undo brings it back, but a
// terrain of a thousand cells is a long ctrl-Z to find out about. True on the
// frame the person says yes; `id` is the popup's, opened by the button.
[[nodiscard]] bool askBeforeGround(const char* id, const char* question, const char* detail, const char* verb)
{
    bool yes = false;
    if (beginEditorDialog(id, 440.0f, []() {})) {
        ImGui::TextWrapped("%s", question);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", detail);
        ImGui::Spacing();
        if (dialogButton(verb, ImVec2(160.0f, 0.0f))) {
            yes = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (dialogButton(core::tr(ENG_TR("engine.editor.terrain.ask.cancel")), ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return yes;
}

void drawTerrainPanel(Editor& editor, scene::World& world, core::InstanceId root, Inspector& inspector,
                      const IconAtlas* icons, EditorCommands& commands)
{
    const core::InstanceId terrainId = editor.terrainIn(world, root);
    const scene::TerrainComponent* terrain = terrainId.valid() ? world.terrains().find(terrainId) : nullptr;
    const Editor::Tool tool = editor.heldTool();
    const bool brushInHand =
        tool == Editor::Tool::Sculpt || tool == Editor::Tool::Paint || tool == Editor::Tool::Foliage;

    // **A tool picked from outside picks its tab** -- T, Y, a status-bar click
    // -- and a world with no ground opens on Create, the only thing to do.
    if (tool != g_terrainLastTool) {
        if (tool == Editor::Tool::Sculpt)
            g_terrainMode = TerrainMode::Sculpt;
        else if (tool == Editor::Tool::Paint)
            g_terrainMode = TerrainMode::Paint;
        else if (tool == Editor::Tool::Foliage)
            g_terrainMode = TerrainMode::Foliage;
        g_terrainLastTool = tool;
    }
    if (terrain == nullptr && g_terrainMode != TerrainMode::Create && g_terrainMode != TerrainMode::Settings &&
        !brushInHand)
        g_terrainMode = TerrainMode::Create;

    g_groundAtTerrain.reset();
    if (terrain != nullptr) {
        if (const std::optional<float> ground = asset::heightAt(terrain->field, 0.0, 0.0); ground.has_value())
            g_groundAtTerrain = *ground + static_cast<f32>(terrain->origin.y);
    }

    // The ground in one line, above the tabs.
    if (terrain != nullptr) {
        const asset::FieldSettings& settings = terrain->field.settings();
        char voxel[32];
        char lowest[32];
        char highest[32];
        (void)std::snprintf(voxel, sizeof(voxel), "%.2f", static_cast<double>(settings.voxelSize));
        (void)std::snprintf(lowest, sizeof(lowest), "%.0f", static_cast<double>(settings.minHeight));
        (void)std::snprintf(highest, sizeof(highest), "%.0f", static_cast<double>(settings.maxHeight));
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.panel.summary"),
                                           {{"count", static_cast<core::i64>(terrain->field.chunkCount())},
                                            {"voxel", std::string_view(voxel)},
                                            {"low", std::string_view(lowest)},
                                            {"high", std::string_view(highest)}})
                                      .c_str());
    }
    else {
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.panel.no_terrain")));
    }
    // The far ground being made in the background (ADR 0150): said while it
    // is, and gone once it is all there.
    // Not over an import: the bar under it is what is under way, and two
    // percentages a line apart read as one job said twice.
    if (terrain != nullptr && editor.farGroundProgress() < 1.0f && !editor.terrainImportRunning()) {
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.panel.far_ground"),
                                           {{"percent", static_cast<core::i64>(editor.farGroundProgress() * 100.0f)}})
                                      .c_str());
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.panel.far_ground_tip")));
    }

    // **An import under way is the panel** (ADR 0149 §2): how far it is, and
    // the way out. Nothing else here is offered while the world is half laid
    // -- and cancelling puts the world back, so nothing below may go on
    // reading the terrain it held.
    if (editor.terrainImportRunning()) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.import.laying")));
        ImGui::PopTextWrapPos();
        const float done = editor.terrainImportProgress();
        char percent[16];
        (void)std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(done * 100.0f));
        ImGui::ProgressBar(done, ImVec2(-FLT_MIN, 0.0f), percent);
        const bool cancel =
            ImGui::Button(core::tr(ENG_TR("engine.editor.terrain.import.cancel")), ImVec2(-FLT_MIN, 0.0f));
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.import.cancel_tip")));
        if (cancel)
            editor.cancelTerrainImport(world, inspector);
        return;
    }

    // The mode bar.
    {
        struct Mode
        {
            TerrainMode mode;
            std::string_view icon;
            core::TextKey word;
            core::TextKey tip;
        };
        static constexpr std::array<Mode, 5> Modes{{
            {TerrainMode::Sculpt, icons::ActionRaise, ENG_TR("engine.editor.terrain.mode.sculpt"),
             ENG_TR("engine.editor.terrain.mode.sculpt_tip")},
            {TerrainMode::Paint, icons::ActionPaint, ENG_TR("engine.editor.terrain.mode.paint"),
             ENG_TR("engine.editor.terrain.mode.paint_tip")},
            {TerrainMode::Foliage, icons::ActionGrid, ENG_TR("engine.editor.terrain.mode.foliage"),
             ENG_TR("engine.editor.terrain.mode.foliage_tip")},
            {TerrainMode::Create, icons::ActionAdd, ENG_TR("engine.editor.terrain.mode.create"),
             ENG_TR("engine.editor.terrain.mode.create_tip")},
            {TerrainMode::Settings, icons::ActionSettings, ENG_TR("engine.editor.terrain.mode.setup"),
             ENG_TR("engine.editor.terrain.mode.setup_tip")},
        }};
        const float spacing = 2.0f;
        const float cell = std::floor((ImGui::GetContentRegionAvail().x - spacing * 4.0f) / 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, ImGui::GetStyle().ItemSpacing.y));
        for (std::size_t index = 0; index < Modes.size(); ++index) {
            if (index > 0)
                ImGui::SameLine();
            const Mode& entry = Modes[index];
            if (terrainModeCell(icons, entry.icon, core::tr(entry.word), g_terrainMode == entry.mode, cell) &&
                g_terrainMode != entry.mode) {
                g_terrainMode = entry.mode;
                // **Changing mode changes the brush in hand, never picks one
                // up**: a sculpt brush becomes a paint brush on Paint, and
                // Create and Setup put it down.
                if (brushInHand) {
                    if (entry.mode == TerrainMode::Sculpt)
                        editor.setTool(Editor::Tool::Sculpt);
                    else if (entry.mode == TerrainMode::Paint)
                        editor.setTool(Editor::Tool::Paint);
                    else if (entry.mode == TerrainMode::Foliage)
                        editor.setTool(Editor::Tool::Foliage);
                    else
                        editor.setTool(Editor::Tool::Select);
                    g_terrainLastTool = editor.heldTool();
                }
            }
            ImGui::SetItemTooltip("%s", core::tr(entry.tip));
        }
        ImGui::PopStyleVar();
        ImGui::Spacing();
    }
    const auto tab = [&](TerrainMode mode) { return g_terrainMode == mode; };

    const float avail = ImGui::GetContentRegionAvail().x;
    const auto needTerrain = [&] {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.terrain.panel.no_ground")));
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.terrain.panel.create_ellipsis")), ImVec2(-FLT_MIN, 0.0f)))
            g_terrainMode = TerrainMode::Create;
    };
    const auto brushDown = [&](const char* what) {
        if (brushInHand)
            return;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", what);
        ImGui::PopStyleColor();
    };

    // --- Sculpt ---------------------------------------------------------------
    if (tab(TerrainMode::Sculpt)) {
        if (terrain == nullptr) {
            needTerrain();
        }
        else {
            static constexpr std::array<SculptTool, 6> Tools{{
                {Editor::BrushOp::Grow, icons::ActionRaise, ENG_TR("engine.editor.terrain.sculpt.raise"),
                 ENG_TR("engine.editor.terrain.sculpt.raise_tip")},
                {Editor::BrushOp::Erode, icons::ActionDig, ENG_TR("engine.editor.terrain.sculpt.lower"),
                 ENG_TR("engine.editor.terrain.sculpt.lower_tip")},
                {Editor::BrushOp::Smooth, icons::ActionSmooth, ENG_TR("engine.editor.terrain.sculpt.smooth"),
                 ENG_TR("engine.editor.terrain.sculpt.smooth_tip")},
                {Editor::BrushOp::Flatten, icons::ActionFlatten, ENG_TR("engine.editor.terrain.sculpt.flatten"),
                 ENG_TR("engine.editor.terrain.sculpt.flatten_tip")},
                {Editor::BrushOp::Add, icons::ActionAdd, ENG_TR("engine.editor.terrain.sculpt.add"),
                 ENG_TR("engine.editor.terrain.sculpt.add_tip")},
                {Editor::BrushOp::Subtract, icons::ActionErase, ENG_TR("engine.editor.terrain.sculpt.dig"),
                 ENG_TR("engine.editor.terrain.sculpt.dig_tip")},
            }};
            const Editor::BrushOp chosen = editor.brush().op;
            const Editor::BrushOp acting = editor.effectiveBrushOp();
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float width = std::floor((avail - spacing * 2.0f) / 3.0f);
            for (std::size_t index = 0; index < Tools.size(); ++index) {
                if (index % 3 != 0)
                    ImGui::SameLine();
                const SculptTool& entry = Tools[index];
                const bool on = tool == Editor::Tool::Sculpt && chosen == entry.op;
                const bool borrowed = tool == Editor::Tool::Sculpt && acting == entry.op && acting != chosen;
                if (terrainTile(icons, entry.icon, core::tr(entry.word), on, borrowed, width)) {
                    editor.setBrushOp(entry.op);
                    editor.setTool(Editor::Tool::Sculpt);
                    g_terrainLastTool = Editor::Tool::Sculpt;
                }
                ImGui::SetItemTooltip("%s (%zu)\n%s", core::tr(entry.word), index + 1, core::tr(entry.what));
            }
            const auto described = std::find_if(Tools.begin(), Tools.end(), [&](const SculptTool& entry) {
                return entry.op == (tool == Editor::Tool::Sculpt ? acting : chosen);
            });
            if (described != Tools.end()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(core::tr(described->word));
                ImGui::SameLine();
                ImGui::TextDisabled("%s", core::tr(described->what));
                ImGui::PopTextWrapPos();
            }
            brushDown(core::tr(ENG_TR("engine.editor.terrain.sculpt.brush_down")));
            const Editor::BrushOp shown = tool == Editor::Tool::Sculpt ? acting : chosen;
            drawBrushControls(editor, true, shown == Editor::BrushOp::Add || shown == Editor::BrushOp::Subtract,
                              shown == Editor::BrushOp::Flatten);
            // The material new ground is made of, where the tools that make
            // ground are.
            if (shown == Editor::BrushOp::Add || shown == Editor::BrushOp::Grow) {
                const core::u8 fill = editor.brush().material;
                if (fill >= 1 && fill <= terrain->layers.size()) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.sculpt.new_ground"),
                                                       {{"name", terrainLayerName(terrain->layers[fill - 1])}})
                                                  .c_str());
                    ImGui::PopTextWrapPos();
                }
            }
        }
    }

    // --- Paint ----------------------------------------------------------------
    if (tab(TerrainMode::Paint)) {
        if (terrain == nullptr) {
            needTerrain();
        }
        else {
            ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.paint.material")));
            drawTerrainLayers(editor, world, root, terrain);
        }
        // Nothing to paint with until there is a material.
        if (terrain != nullptr && !terrain->layers.empty()) {
            ImGui::Spacing();
            const bool on = tool == Editor::Tool::Paint;
            if (terrainTile(icons, icons::ActionPaint,
                            on ? core::tr(ENG_TR("engine.editor.terrain.paint.painting"))
                               : core::tr(ENG_TR("engine.editor.terrain.mode.paint")),
                            on, false, -FLT_MIN)) {
                editor.setTool(on ? Editor::Tool::Select : Editor::Tool::Paint);
                g_terrainLastTool = editor.heldTool();
            }
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.paint.tip")));
            brushDown(core::tr(ENG_TR("engine.editor.terrain.paint.brush_down")));
            drawPaintMode(editor);
            drawBrushControls(editor, true, false, false);
            drawPaintMask(editor, *terrain);
            drawReplaceMaterial(editor, world, root, *terrain);
            ImGui::Spacing();
            if (ImGui::CollapsingHeader(core::tr(ENG_TR("engine.editor.terrain.rules.title"))))
                drawTerrainRules(editor, world, root, *terrain, icons);
        }
    }

    // --- Foliage (ADR 0116) -----------------------------------------------------
    //
    // The layers growing on this terrain, and a brush that paints a layer's
    // density by hand. A layer and a mesh are instances, so what they are is
    // edited in Properties; this makes them and chooses which the brush paints.
    if (tab(TerrainMode::Foliage)) {
        if (terrain == nullptr) {
            needTerrain();
        }
        else {
            std::vector<core::InstanceId> layers;
            for (core::InstanceId child = world.firstChild(terrainId); child.valid();
                 child = world.nextSibling(child)) {
                if (world.foliageLayers().find(child) != nullptr)
                    layers.push_back(child);
            }
            if (layers.empty())
                ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.terrain.foliage.none")));
            if (!layers.empty() && std::find(layers.begin(), layers.end(), editor.foliageLayer()) == layers.end())
                editor.setFoliageLayer(layers.front());
            for (core::usize index = 0; index < layers.size(); ++index) {
                const core::InstanceId layer = layers[index];
                core::usize meshes = 0;
                for (core::InstanceId child = world.firstChild(layer); child.valid(); child = world.nextSibling(child))
                    meshes += world.foliageMeshes().find(child) != nullptr ? 1 : 0;
                const std::string label = core::tr(ENG_TR("engine.editor.terrain.foliage.layer_row"),
                                                   {{"name", world.atoms().text(world.name(layer))},
                                                    {"count", static_cast<core::i64>(meshes)}}) +
                                          "###foliage-layer-" + std::to_string(index);
                if (ImGui::Selectable(label.c_str(), layer == editor.foliageLayer())) {
                    editor.setFoliageLayer(layer);
                    inspector.select(layer);
                }
            }
            const float half = std::floor((avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
            if (labeledIconButton(icons, icons::ActionAdd, core::tr(ENG_TR("engine.editor.terrain.foliage.add_layer")),
                                  ImVec2(half, 0.0f)))
                (void)editor.createFoliageLayer(world, root, inspector);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.foliage.add_layer_tip")));
            ImGui::SameLine();
            const scene::FoliageLayerComponent* chosen = world.foliageLayers().find(editor.foliageLayer());
            ImGui::BeginDisabled(chosen == nullptr);
            if (labeledIconButton(icons, icons::ActionAdd, core::tr(ENG_TR("engine.editor.terrain.foliage.add_mesh")),
                                  ImVec2(-FLT_MIN, 0.0f)))
                (void)editor.addFoliageMesh(world, editor.foliageLayer(), inspector);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.foliage.add_mesh_tip")));

            // **The chosen layer's meshes, each edited here** (the owner,
            // 2026-09-30: a layer said "5 meshes" and nothing more, and which
            // mesh grew was found only by opening the layer in the Explorer).
            // Each row is named by its mesh file and opens on the fields a
            // person tunes, drawn by the Properties panel's own editors -- the
            // same picker, the same drop target, the same undo.
            if (chosen != nullptr) {
                std::vector<core::InstanceId> meshes;
                for (core::InstanceId child = world.firstChild(editor.foliageLayer()); child.valid();
                     child = world.nextSibling(child)) {
                    if (world.foliageMeshes().find(child) != nullptr)
                        meshes.push_back(child);
                }
                ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.foliage.meshes")));
                if (meshes.empty())
                    ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.terrain.foliage.no_meshes")));
                struct FoliageField
                {
                    const char* name;
                    core::TextKey label;
                };
                static const std::array<FoliageField, 9> Fields{{
                    FoliageField{"Mesh", ENG_TR("engine.editor.terrain.foliage.field.mesh")},
                    FoliageField{"Weight", ENG_TR("engine.editor.terrain.foliage.field.weight")},
                    FoliageField{"ScaleMin", ENG_TR("engine.editor.terrain.foliage.field.scale_min")},
                    FoliageField{"ScaleMax", ENG_TR("engine.editor.terrain.foliage.field.scale_max")},
                    FoliageField{"RandomRotation", ENG_TR("engine.editor.terrain.foliage.field.random_rotation")},
                    FoliageField{"AlignToNormal", ENG_TR("engine.editor.terrain.foliage.field.align_to_normal")},
                    FoliageField{"Sink", ENG_TR("engine.editor.terrain.foliage.field.sink")},
                    FoliageField{"WindResponse", ENG_TR("engine.editor.terrain.foliage.field.wind_response")},
                    FoliageField{"Stiffness", ENG_TR("engine.editor.terrain.foliage.field.stiffness")},
                }};
                const scene::ClassId meshClass = world.classes().findId(world.atoms().intern("FoliageMesh"));
                core::InstanceId removed;
                for (core::usize index = 0; index < meshes.size(); ++index) {
                    const core::InstanceId mesh = meshes[index];
                    const std::span<const core::InstanceId> target(&meshes[index], 1);
                    ImGui::PushID(static_cast<int>(index));
                    const SharedValue file = sharedValue(world, target, world.atoms().intern("Mesh"));
                    const std::string* path = std::get_if<std::string>(&file.value);
                    const std::string title =
                        path != nullptr && !path->empty()
                            ? std::filesystem::path(*path).stem().string()
                            : std::string(core::tr(ENG_TR("engine.editor.terrain.foliage.no_mesh")));
                    const float removeWidth = ImGui::GetFrameHeight();
                    // Open where there is something to decide -- a mesh not
                    // chosen yet -- or where the whole layer fits; a long list
                    // reads as its names.
                    const bool unchosen = path == nullptr || path->empty();
                    const bool open =
                        ImGui::TreeNodeEx("##mesh",
                                          (unchosen || meshes.size() <= 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0) |
                                              ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth,
                                          "%s", title.c_str());
                    if (ImGui::IsItemClicked())
                        inspector.select(mesh);
                    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.foliage.mesh_row_tip")));
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - removeWidth);
                    if (iconButton(icons, icons::ActionDelete, ImGui::GetFontSize(), "remove", "x",
                                   core::tr(ENG_TR("engine.editor.terrain.foliage.remove_mesh_tip"))))
                        removed = mesh;
                    if (open) {
                        if (meshClass != scene::InvalidClass &&
                            ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingStretchProp)) {
                            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                                    ImGui::GetFontSize() * 7.5f);
                            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                            for (const FoliageField& field : Fields) {
                                const scene::PropertyDesc* descriptor =
                                    world.classes().findProperty(meshClass, world.atoms().intern(field.name));
                                if (descriptor == nullptr)
                                    continue;
                                ImGui::PushID(field.name);
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(core::tr(field.label));
                                if (descriptor->doc[0] != 0)
                                    ImGui::SetItemTooltip("%s", descriptor->doc);
                                ImGui::TableSetColumnIndex(1);
                                drawEditor(world, root, inspector, target, *descriptor,
                                           sharedValue(world, target, descriptor->name), &editor.content(), icons,
                                           nullptr, &commands);
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                if (removed.valid())
                    (void)editor.deleteInstance(world, removed, root, inspector);
            }

            ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.foliage.density")));
            const bool painting = tool == Editor::Tool::Foliage;
            const bool thin = painting ? editor.effectiveFoliageThin() : editor.foliageThin();
            if (terrainTile(icons, icons::ActionPaint, core::tr(ENG_TR("engine.editor.terrain.foliage.restore")),
                            painting && !thin, false, half)) {
                editor.setFoliageThin(false);
                editor.setTool(Editor::Tool::Foliage);
                g_terrainLastTool = Editor::Tool::Foliage;
            }
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.foliage.restore_tip")));
            ImGui::SameLine();
            if (terrainTile(icons, icons::ActionErase, core::tr(ENG_TR("engine.editor.terrain.foliage.thin")),
                            painting && thin, false, -FLT_MIN)) {
                editor.setFoliageThin(true);
                editor.setTool(Editor::Tool::Foliage);
                g_terrainLastTool = Editor::Tool::Foliage;
            }
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.foliage.thin_tip")));
            if (chosen != nullptr)
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.foliage.painted_tiles"),
                                                   {{"count", static_cast<core::i64>(chosen->mask.size())}})
                                              .c_str());
            ImGui::EndDisabled();
            brushDown(core::tr(ENG_TR("engine.editor.terrain.foliage.brush_down")));
            drawBrushControls(editor, true, false, false);
        }
    }

    // --- Create -----------------------------------------------------------------
    if (tab(TerrainMode::Create)) {
        ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.create.flat")));
        // **256 m, which is 256 columns square at the default voxel**: matched
        // to the reference engines on sample count rather than metres, and
        // four whole 64 m streaming cells on a side.
        static f32 groundSize = 256.0f;
        static f32 groundHeight = 0.0f;
        const float label = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.terrain.create.height"))).x +
                            ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.create.size")));
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##ground-size", &groundSize, 1.0f, 8.0f, 2048.0f,
                         core::tr(ENG_TR("engine.editor.unit.metres_square")));
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.create.height")));
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##ground-height", &groundHeight, 0.25f, -256.0f, 256.0f,
                         core::tr(ENG_TR("engine.editor.unit.metres_1")));
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.create.height_tip")));
        {
            // **The cost before the click**: a size field with no cost readout
            // only reports its mistake afterwards.
            const f32 voxel =
                terrain != nullptr ? terrain->field.settings().voxelSize : asset::FieldSettings{}.voxelSize;
            const auto columns = static_cast<int>(groundSize / std::max(voxel, 0.01f));
            const auto side = static_cast<long long>(columns / static_cast<int>(asset::ChunkEdge) + 1);
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.create.cost"),
                                               {{"columns", static_cast<core::i64>(columns)},
                                                {"kb", static_cast<core::i64>(side * side * 4)}})
                                          .c_str());
        }
        if (const core::u8 fill = editor.brush().material;
            terrain != nullptr && fill >= 1 && fill <= terrain->layers.size())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.create.made_of"),
                                               {{"name", terrainLayerName(terrain->layers[fill - 1])}})
                                          .c_str());
        bool layFlat = false;
        const char* replaceFlat = core::tr(ENG_TR("engine.editor.terrain.create.replace_flat"));
        if (labeledIconButton(icons, icons::ActionAdd,
                              terrain == nullptr ? core::tr(ENG_TR("engine.editor.terrain.create.create"))
                                                 : replaceFlat,
                              ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.4f))) {
            if (terrain == nullptr)
                layFlat = true;
            else
                ImGui::OpenPopup("###ask-flat");
        }
        ImGui::SetItemTooltip("%s", terrain == nullptr
                                        ? core::tr(ENG_TR("engine.editor.terrain.create.create_tip"))
                                        : core::tr(ENG_TR("engine.editor.terrain.create.replace_flat_tip")));
        layFlat = askBeforeGround((std::string(replaceFlat) + "###ask-flat").c_str(),
                                  core::tr(ENG_TR("engine.editor.terrain.create.ask_flat")),
                                  core::tr(ENG_TR("engine.editor.terrain.create.ask_flat_detail")),
                                  core::tr(ENG_TR("engine.editor.terrain.create.replace"))) ||
                  layFlat;
        if (layFlat &&
            editor.generateGround(world, root, inspector, groundSize, groundHeight, editor.brush().material)) {
            // Straight on to shaping it, which is what somebody who just made
            // ground wants next.
            g_terrainMode = TerrainMode::Sculpt;
        }

        ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.create.hills")));
        {
            // **Hills from noise** (B4): a seed, how big the largest are and
            // how many sizes of smaller ones ride on them, between two heights.
            static Editor::HillSpec hills;
            const float wide = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.terrain.create.octaves"))).x +
                               ImGui::GetStyle().ItemSpacing.x * 2.0f;
            ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.create.size")));
            ImGui::SameLine(wide);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##hills-size", &hills.size, 1.0f, 16.0f, 32768.0f,
                             core::tr(ENG_TR("engine.editor.unit.metres_square")));
            {
                const f32 voxelNow =
                    terrain != nullptr ? terrain->field.settings().voxelSize : asset::FieldSettings{}.voxelSize;
                if (hills.size / std::max(voxelNow, 0.01f) + 1.0f > static_cast<f32>(Editor::MaxTableColumns)) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.import.tiled_note")));
                    ImGui::PopTextWrapPos();
                }
            }
            ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.create.heights")));
            ImGui::SameLine(wide);
            ImGui::SetNextItemWidth(-FLT_MIN);
            float range[2] = {hills.low, hills.high};
            if (ImGui::DragFloat2("##hills-heights", range, 0.25f, -256.0f, 256.0f,
                                  core::tr(ENG_TR("engine.editor.unit.metres_1")))) {
                hills.low = std::min(range[0], range[1]);
                hills.high = std::max(range[0], range[1]);
            }
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.create.heights_tip")));
            ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.create.scale")));
            ImGui::SameLine(wide);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##hills-scale", &hills.scale, 1.0f, 8.0f, 2048.0f,
                             core::tr(ENG_TR("engine.editor.unit.metres_0")));
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.create.scale_tip")));
            ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.create.octaves")));
            ImGui::SameLine(wide);
            ImGui::SetNextItemWidth(-FLT_MIN);
            int octaves = static_cast<int>(hills.octaves);
            if (ImGui::SliderInt("##hills-octaves", &octaves, 1, 8))
                hills.octaves = static_cast<core::u32>(octaves);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.create.octaves_tip")));
            ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.create.seed")));
            ImGui::SameLine(wide);
            ImGui::SetNextItemWidth(-FLT_MIN);
            int seed = static_cast<int>(hills.seed);
            if (ImGui::InputInt("##hills-seed", &seed))
                hills.seed = static_cast<core::u32>(std::max(seed, 0));
            bool layHills = false;
            const char* replaceHills = core::tr(ENG_TR("engine.editor.terrain.create.replace_hills"));
            if (labeledIconButton(icons, icons::ActionAdd,
                                  terrain == nullptr ? core::tr(ENG_TR("engine.editor.terrain.create.create_hills"))
                                                     : replaceHills,
                                  ImVec2(-FLT_MIN, 0.0f))) {
                if (terrain == nullptr)
                    layHills = true;
                else
                    ImGui::OpenPopup("###ask-hills");
            }
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.create.hills_tip")));
            layHills = askBeforeGround((std::string(replaceHills) + "###ask-hills").c_str(),
                                       core::tr(ENG_TR("engine.editor.terrain.create.ask_hills")),
                                       core::tr(ENG_TR("engine.editor.terrain.create.ask_hills_detail")),
                                       core::tr(ENG_TR("engine.editor.terrain.create.replace"))) ||
                       layHills;
            if (layHills) {
                hills.material = editor.brush().material;
                if (editor.generateHills(world, root, inspector, hills))
                    g_terrainMode = TerrainMode::Sculpt;
            }
        }

        ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.create.heightmap")));
        drawTerrainHeightmap(editor, world, root, inspector, commands);
        if (terrain != nullptr) {
            static f32 exportSize = 256.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            ImGui::DragFloat("##export-size", &exportSize, 1.0f, 16.0f, 4096.0f,
                             core::tr(ENG_TR("engine.editor.unit.metres_square")));
            ImGui::SameLine();
            if (ImGui::Button(core::tr(ENG_TR("engine.editor.terrain.create.export")), ImVec2(-FLT_MIN, 0.0f)))
                (void)editor.exportHeightmap(world, root, exportSize);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.create.export_tip")));
        }

        ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.create.function")));
        {
            // **Ground from a Luau function of a place** (ADR 0149 §2): a
            // file that returns `function(x, z)`, laid a tile at a time.
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.create.function_intro")));
            ImGui::PopTextWrapPos();
            const std::filesystem::path& file = editor.heightFunctionSource();
            const std::string chosen = file.empty()
                                           ? std::string(core::tr(ENG_TR("engine.editor.terrain.create.function_none")))
                                           : file.filename().string();
            ImGui::TextUnformatted(chosen.c_str());
            if (!file.empty())
                ImGui::SetItemTooltip("%s", file.string().c_str());
            if (ImGui::Button(core::tr(ENG_TR("engine.editor.terrain.create.function_choose")), ImVec2(-FLT_MIN, 0.0f)))
                commands.pickHeightFunction = true;
            static f32 functionSize = 1024.0f;
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##function-size", &functionSize, 1.0f, 16.0f, 32768.0f,
                             core::tr(ENG_TR("engine.editor.unit.metres_square")));
            ImGui::BeginDisabled(file.empty());
            if (ImGui::Button(core::tr(ENG_TR("engine.editor.terrain.create.function_lay")), ImVec2(-FLT_MIN, 0.0f)))
                (void)editor.importFunction(world, root, inspector,
                                            Editor::FunctionImport{file, functionSize, editor.brush().material});
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.import.tiled_note")));
        }

        if (terrain != nullptr) {
            // Apart from the export above it, with room between: the two sat
            // one under the other (the editor list).
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
            ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.terrain.create.start_over")));
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor(palette().warning));
            const char* clearAll = core::tr(ENG_TR("engine.editor.terrain.create.clear"));
            if (labeledIconButton(icons, icons::ActionDelete, clearAll, ImVec2(-FLT_MIN, 0.0f)))
                ImGui::OpenPopup("###ask-clear");
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.create.clear_tip")));
            if (askBeforeGround((std::string(clearAll) + "###ask-clear").c_str(),
                                core::tr(ENG_TR("engine.editor.terrain.create.ask_clear")),
                                core::tr(ENG_TR("engine.editor.terrain.create.ask_clear_detail")),
                                core::tr(ENG_TR("engine.editor.terrain.create.clear_verb"))))
                editor.clearTerrain(world, root, inspector);
        }
    }

    // --- Setup ------------------------------------------------------------------
    if (tab(TerrainMode::Settings)) {
        drawTerrainSettings(editor, world, root, inspector);
    }

    // **The keys that make a brush quick**, written where the brush is.
    // Not on Paint with nothing to paint with, where no brush can be held.
    if (terrain != nullptr &&
        (g_terrainMode == TerrainMode::Sculpt || (g_terrainMode == TerrainMode::Paint && !terrain->layers.empty()) ||
         g_terrainMode == TerrainMode::Foliage)) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        // Each mode's own keys: Paint has no strength and no opposite
        // (terrain audit E6), and Foliage's opposite thins.
        ImGui::TextDisabled(
            "%s", g_terrainMode == TerrainMode::Sculpt  ? core::tr(ENG_TR("engine.editor.terrain.keys.sculpt"))
                  : g_terrainMode == TerrainMode::Paint ? core::tr(ENG_TR("engine.editor.terrain.keys.paint"))
                                                        : core::tr(ENG_TR("engine.editor.terrain.keys.foliage")));
        ImGui::PopTextWrapPos();
    }
}

// Inspectable settings stay available while working in the viewport.
void drawViewportSettings(Editor* editor, EditorPanels& panels, const IconAtlas* icons, bool streams)
{
    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.viewport_settings.visualization")));
    const auto toggle = [&](std::string_view icon, const char* label, bool& value, const char* tip) {
        const std::string padded = tabIconPad() + label;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const bool changed = ImGui::Checkbox(padded.c_str(), &value);
        paintActionIcon(icons, icon,
                        ImVec2(origin.x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x,
                               origin.y + ImGui::GetStyle().FramePadding.y),
                        ImGui::GetFontSize());
        ImGui::SetItemTooltip("%s", tip);
        return changed;
    };
    toggle(icons::ActionGrid, core::tr(ENG_TR("engine.editor.viewport_settings.reference_grid")), panels.showGrid,
           core::tr(ENG_TR("engine.editor.viewport_settings.reference_grid_tip")));
    toggle(icons::ClassBone, core::tr(ENG_TR("engine.editor.viewport_settings.skeletons")), panels.showSkeletons,
           core::tr(ENG_TR("engine.editor.viewport_settings.skeletons_tip")));
    toggle(icons::ClassPhysicsService, core::tr(ENG_TR("engine.editor.viewport_settings.collision_shapes")),
           panels.showCollision, core::tr(ENG_TR("engine.editor.viewport_settings.collision_shapes_tip")));
    toggle(icons::ClassTerrain, core::tr(ENG_TR("engine.editor.viewport_settings.terrain_wireframe")),
           panels.showTerrainWireframe, core::tr(ENG_TR("engine.editor.viewport_settings.terrain_wireframe_tip")));
    toggle(icons::ClassTerrain, core::tr(ENG_TR("engine.editor.viewport_settings.terrain_normals")),
           panels.showTerrainNormals, core::tr(ENG_TR("engine.editor.viewport_settings.terrain_normals_tip")));
    // **Only a world that streams has a grid to show**, so the switch says so
    // rather than being a checkbox that does nothing (the owner's report).
    ImGui::BeginDisabled(!streams);
    toggle(icons::ClassStreamingService, core::tr(ENG_TR("engine.editor.viewport_settings.streaming_grid")),
           panels.showChunkGrid,
           core::tr(streams ? ENG_TR("engine.editor.viewport_settings.streaming_grid_tip")
                            : ENG_TR("engine.editor.viewport_settings.no_streaming_grid_tip")));
    ImGui::EndDisabled();
    if (editor == nullptr)
        return;
    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.viewport_settings.camera")));
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.viewport_settings.fly_speed_m_s")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    f32 speed = editor->cameraSpeed();
    if (ImGui::DragFloat("##fly-speed", &speed, 0.5f, 0.1f, 1000.0f, "%.1f"))
        editor->setCameraSpeed(speed);
    ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.viewport_settings.wasd_qe_fly_while_the")));
    // The snap switch and its steps are on the viewport's own toolbar, where a
    // drag is; a second copy here was a second place to look for one number.
    ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.viewport_settings.snapping")));
    ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.viewport_settings.snapping_and_how_far_it")));
}

// The Blocks panel's contents (V1, `VoxelService`).
//
// **Types first, then what a click does.** A block world is a palette before
// it is anything else -- nothing can be placed until a type exists -- so the
// panel opens on the palette, and making a type is one row under it rather
// than a dialog. The colours are the three faces a type has: top, sides and
// bottom, because a grass block is green on top and earth everywhere else.
void drawBlocksPanel(Editor& editor, scene::World& world, Inspector& inspector, const IconAtlas* icons)
{
    scene::VoxelComponent* voxels = Editor::voxelsIn(world);
    if (voxels == nullptr) {
        // A stamp stage: no services, so no block world. Said rather than
        // shown as an empty palette nobody could fill.
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.blocks_panel.this_world_has_no_block")));
        return;
    }

    // --- What a click does -------------------------------------------
    {
        const auto opButton = [&](Editor::BlockOp op, std::string_view icon, const char* word, const char* tip) {
            const bool on = editor.blockOp() == op && editor.heldTool() == Editor::Tool::Blocks;
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            const float width = std::max(76.0f * ImGui::GetStyle().FontScaleMain,
                                         ImGui::CalcTextSize(word).x + ImGui::CalcTextSize(tabIconPad().c_str()).x +
                                             ImGui::GetStyle().FramePadding.x * 2.0f);
            if (ImGui::GetContentRegionAvail().x < width && ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x)
                ImGui::NewLine();
            if (labeledIconButton(icons, icon, word, ImVec2(width, 0.0f))) {
                editor.setBlockOp(op);
                editor.setTool(Editor::Tool::Blocks);
            }
            if (on)
                ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", tip);
        };
        opButton(Editor::BlockOp::Place, icons::ActionPlaceBlock, core::tr(ENG_TR("engine.editor.blocks_panel.place")),
                 core::tr(ENG_TR("engine.editor.blocks_panel.place_tip")));
        ImGui::SameLine();
        opButton(Editor::BlockOp::Break, icons::ActionBreakBlock, core::tr(ENG_TR("engine.editor.blocks_panel.break")),
                 core::tr(ENG_TR("engine.editor.blocks_panel.break_tip")));
        ImGui::SameLine();
        opButton(Editor::BlockOp::Replace, icons::ActionReplaceBlock,
                 core::tr(ENG_TR("engine.editor.blocks_panel.replace")),
                 core::tr(ENG_TR("engine.editor.blocks_panel.replace_tip")));
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.blocks_panel.chunks_blocks"),
                                           {{"count", static_cast<core::i64>(voxels->grid.chunkCount())},
                                            {"size", fixed(static_cast<double>(voxels->blockSize), 2)}})
                                      .c_str());
    }

    // --- Types ------------------------------------------------------
    if (ImGui::CollapsingHeader(core::tr(ENG_TR("engine.editor.blocks_panel.types")), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (voxels->types.empty())
            ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.blocks_panel.no_block_types_yet_add")));

        const f32 swatch = ImGui::GetFrameHeight() * 1.4f;
        const auto columns = static_cast<core::usize>(
            std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) /
                                         (swatch + ImGui::GetStyle().ItemSpacing.x))));
        for (core::usize at = 0; at < voxels->types.size(); ++at) {
            const scene::VoxelBlockType& type = voxels->types[at];
            const auto id = static_cast<asset::BlockId>(at + 1);
            if (at % columns != 0)
                ImGui::SameLine();
            ImGui::PushID(static_cast<int>(id));
            const bool on = editor.blockType() == id;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            }
            if (ImGui::ColorButton("##type", ImVec4(type.color.r, type.color.g, type.color.b, 1.0f),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                                   ImVec2(swatch, swatch))) {
                editor.setBlockType(id);
            }
            if (on) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            const std::string_view name = world.atoms().text(type.name);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.blocks_panel.type_id"),
                                                 {{"name", name}, {"id", static_cast<core::i64>(id)}})
                                            .c_str());
            ImGui::PopID();
        }

        // The selected type's three colours, edited in place. One undo step
        // per drag of a picker, through the inspector's gesture.
        if (const asset::BlockId selected = editor.blockType();
            selected != asset::AirBlock && selected <= voxels->types.size()) {
            const scene::VoxelBlockType type = voxels->types[selected - 1u];
            const std::string_view name = world.atoms().text(type.name);
            ImGui::Separator();
            ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
            float top[3] = {type.color.r, type.color.g, type.color.b};
            float side[3] = {type.side.r, type.side.g, type.side.b};
            float bottom[3] = {type.bottom.r, type.bottom.g, type.bottom.b};
            bool changed = ImGui::ColorEdit3(core::tr(ENG_TR("engine.editor.blocks_panel.top")), top,
                                             ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            changed |= ImGui::ColorEdit3(core::tr(ENG_TR("engine.editor.blocks_panel.sides")), side,
                                         ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            changed |= ImGui::ColorEdit3(core::tr(ENG_TR("engine.editor.blocks_panel.bottom")), bottom,
                                         ImGuiColorEditFlags_NoInputs);
            static core::u64 recolour = 0;
            if (changed) {
                if (recolour == 0)
                    recolour = inspector.beginGesture();
                (void)editor.setBlockTypeColors(world, inspector, selected, core::Color3{top[0], top[1], top[2]},
                                                core::Color3{side[0], side[1], side[2]},
                                                core::Color3{bottom[0], bottom[1], bottom[2]}, recolour);
            }
            if (recolour != 0 && !ImGui::IsAnyItemActive()) {
                inspector.endGesture();
                recolour = 0;
            }
            drawBlockLook(editor, world, inspector);
        }

        ImGui::Separator();
        static char newName[64] = "";
        static float newTop[3] = {0.36f, 0.62f, 0.24f};
        static float newSide[3] = {0.47f, 0.33f, 0.2f};
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##newName", core::tr(ENG_TR("engine.editor.blocks_panel.new_type_name")), newName,
                                 sizeof(newName));
        ImGui::ColorEdit3(labelled(ENG_TR("engine.editor.blocks_panel.top"), "##new").c_str(), newTop,
                          ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3(labelled(ENG_TR("engine.editor.blocks_panel.sides_and_bottom"), "##new").c_str(), newSide,
                          ImGuiColorEditFlags_NoInputs);
        if (labeledIconButton(icons, icons::ActionAdd, core::tr(ENG_TR("engine.editor.blocks_panel.add_type")),
                              ImVec2(-FLT_MIN, 0.0f))) {
            const core::Color3 side{newSide[0], newSide[1], newSide[2]};
            if (editor.addBlockType(world, inspector, newName, core::Color3{newTop[0], newTop[1], newTop[2]}, side,
                                    side) != asset::AirBlock) {
                newName[0] = '\0';
                editor.setTool(Editor::Tool::Blocks);
            }
        }
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.blocks_panel.registers_a_type_and_selects_tip")));
    }

    // --- World --------------------------------------------------------
    if (ImGui::CollapsingHeader(core::tr(ENG_TR("engine.editor.blocks_panel.world")))) {
        if (labeledIconButton(icons, icons::ActionDelete, core::tr(ENG_TR("engine.editor.blocks_panel.clear_blocks")),
                              ImVec2(-FLT_MIN, 0.0f)))
            (void)editor.clearBlocks(world, inspector);
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.blocks_panel.removes_every_block_and_keeps_tip")));
    }
}

// The Tiles tool's panel (the 2D layer): the 2D view it is used from, what a
// click does, and the tile it lays -- picked from the tileset's own picture
// when the renderer has loaded it, and by number when it has not.
void drawTilesPanel(Editor& editor, scene::World& world, core::InstanceId root, Inspector& inspector,
                    const IconAtlas* icons)
{
    bool flat = editor.view2D();
    const ImVec2 viewOrigin = ImGui::GetCursorScreenPos();
    if (ImGui::Checkbox((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.2d_view")) + "###2D view").c_str(), &flat))
        editor.setView2D(flat);
    paintActionIcon(icons, icons::ActionView2D,
                    ImVec2(viewOrigin.x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x,
                           viewOrigin.y + ImGui::GetStyle().FramePadding.y),
                    ImGui::GetFontSize());
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.tiles_panel.look_straight_at_the_2d_tip")));

    const core::InstanceId target = Editor::tilemapFor(world, inspector, root);
    const scene::Tilemap2DComponent* tilemap = target.valid() ? world.tilemaps2d().find(target) : nullptr;
    if (tilemap == nullptr) {
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.tiles_panel.this_world_has_no_tilemap2d")));
        return;
    }
    const std::string_view name = world.atoms().text(world.name(target));
    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.tiles_panel.painting"), {{"name", name}}).c_str());

    const auto opButton = [&](Editor::TileOp op, std::string_view icon, const char* word, const char* tip) {
        const bool on = editor.tileOp() == op && editor.heldTool() == Editor::Tool::Tiles;
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const float width = ImGui::CalcTextSize(word).x + ImGui::CalcTextSize(tabIconPad().c_str()).x +
                            ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetContentRegionAvail().x < width && ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x)
            ImGui::NewLine();
        if (labeledIconButton(icons, icon, word, ImVec2(width, 0.0f))) {
            editor.setTileOp(op);
            editor.setTool(Editor::Tool::Tiles);
        }
        if (on)
            ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tip);
    };
    opButton(Editor::TileOp::Paint, icons::ActionPaint, core::tr(ENG_TR("engine.editor.tiles_panel.paint")),
             core::tr(ENG_TR("engine.editor.tiles_panel.paint_tip")));
    ImGui::SameLine();
    opButton(Editor::TileOp::Erase, icons::ActionErase, core::tr(ENG_TR("engine.editor.tiles_panel.erase")),
             core::tr(ENG_TR("engine.editor.tiles_panel.erase_tip")));

    const Editor::TilesetPreview& preview = editor.tilesetPreview();
    const int columns = preview.tileSize.x > 0.0f ? static_cast<int>(preview.pixels.x / preview.tileSize.x) : 0;
    const int rows = preview.tileSize.y > 0.0f ? static_cast<int>(preview.pixels.y / preview.tileSize.y) : 0;
    SDL_GPUTexture* native =
        g_device != nullptr && preview.texture.valid() ? rhi::nativeTexture(*g_device, preview.texture) : nullptr;
    ImGui::Separator();
    if (native != nullptr && columns > 0 && rows > 0) {
        // The tileset as a palette, a row of it at a time, wrapped to the
        // panel. Capped, because a tileset of a hundred thousand tiles is a
        // texture somebody pointed at by mistake.
        const float side = 32.0f * ImGui::GetStyle().FontScaleMain;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int across =
            std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (side + 8.0f + spacing)));
        const int count = std::min(columns * rows, 4096);
        for (int index = 0; index < count; ++index) {
            const int column = index % columns;
            const int row = index / columns;
            const ImVec2 uv0(static_cast<float>(column) * preview.tileSize.x / preview.pixels.x,
                             static_cast<float>(row) * preview.tileSize.y / preview.pixels.y);
            const ImVec2 uv1(uv0.x + preview.tileSize.x / preview.pixels.x,
                             uv0.y + preview.tileSize.y / preview.pixels.y);
            const auto tile = static_cast<core::u16>(index + 1);
            if (index % across != 0)
                ImGui::SameLine();
            ImGui::PushID(index);
            const bool on = editor.tile() == tile;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            }
            if (ImGui::ImageButton("##tile", static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native)),
                                   ImVec2(side, side), uv0, uv1)) {
                editor.setTile(tile);
                editor.setTileOp(Editor::TileOp::Paint);
                editor.setTool(Editor::Tool::Tiles);
            }
            if (on) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            ImGui::SetItemTooltip(
                "%s", core::tr(ENG_TR("engine.editor.tiles_panel.tile"), {{"index", static_cast<core::i64>(index + 1)}})
                          .c_str());
            ImGui::PopID();
        }
    }
    else {
        int value = editor.tile();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt(core::tr(ENG_TR("engine.editor.tiles_panel.tile")), &value))
            editor.setTile(static_cast<core::u16>(std::clamp(value, 1, 65535)));
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.tiles_panel.set_the_tilemap_s_tileset")));
    }
    if (editor.lastTileEdits() > 0)
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.tiles_panel.last_stroke"),
                                           {{"count", static_cast<core::i64>(editor.lastTileEdits())}})
                                      .c_str());
}

// --- The Water panel (ADR 0146 section 7; the water ledger's W5) -------------
//
// Three things to draw, what a click does with the one chosen, and the few
// numbers somebody changes between two clicks: how wide, how deep, how high,
// how fast. Everything else a `Water` has is in Properties, where drawing it
// has already put it -- the tool selects what it makes.
void drawWaterPanel(Editor& editor, scene::World& world, Inspector& inspector, const IconAtlas* icons,
                    core::InstanceId root)
{
    const auto opButton = [&](Editor::WaterOp op, std::string_view icon, const char* word, const char* tip) {
        const bool on = editor.waterOp() == op && editor.heldTool() == Editor::Tool::Water;
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const float width = ImGui::CalcTextSize(word).x + ImGui::CalcTextSize(tabIconPad().c_str()).x +
                            ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetContentRegionAvail().x < width && ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x)
            ImGui::NewLine();
        if (labeledIconButton(icons, icon, word, ImVec2(width, 0.0f))) {
            editor.setWaterOp(op);
            editor.setTool(Editor::Tool::Water);
        }
        if (on)
            ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tip);
    };
    opButton(Editor::WaterOp::River, WaterRiverIcon, core::tr(ENG_TR("engine.editor.water_panel.river")),
             core::tr(ENG_TR("engine.editor.water_panel.river_tip")));
    ImGui::SameLine();
    opButton(Editor::WaterOp::Lake, WaterLakeIcon, core::tr(ENG_TR("engine.editor.water_panel.lake")),
             core::tr(ENG_TR("engine.editor.water_panel.lake_tip")));
    ImGui::SameLine();
    opButton(Editor::WaterOp::Pool, WaterPoolIcon, core::tr(ENG_TR("engine.editor.water_panel.pool")),
             core::tr(ENG_TR("engine.editor.water_panel.pool_tip")));
    ImGui::SameLine();
    opButton(Editor::WaterOp::Ocean, WaterOceanIcon, core::tr(ENG_TR("engine.editor.water_panel.ocean")),
             core::tr(ENG_TR("engine.editor.water_panel.ocean_tip")));
    ImGui::Separator();

    if (editor.heldTool() != Editor::Tool::Water) {
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.water_panel.pick_one")));
        return;
    }

    const Editor::WaterOp op = editor.waterOp();
    const char* hint = core::tr(ENG_TR("engine.editor.water_panel.ocean_hint"));
    switch (op) {
    case Editor::WaterOp::River:
        hint = core::tr(ENG_TR("engine.editor.water_panel.river_hint"));
        break;
    case Editor::WaterOp::Lake:
        hint = core::tr(ENG_TR("engine.editor.water_panel.lake_hint"));
        break;
    case Editor::WaterOp::Pool:
        hint = core::tr(ENG_TR("engine.editor.water_panel.pool_hint"));
        break;
    case Editor::WaterOp::Ocean:
        break;
    }
    ImGui::TextWrapped("%s", hint);
    ImGui::Spacing();

    // A number dragged is one undo step, however many frames the drag lasts:
    // the gesture opens when the field is taken and closes when it is let go.
    const float fieldWidth = 120.0f * ImGui::GetStyle().FontScaleMain;
    const auto number = [&](core::TextKey key, const char* id, float value, float speed, float low,
                            float high) -> std::optional<float> {
        float edited = value;
        ImGui::SetNextItemWidth(fieldWidth);
        const bool changed = ImGui::DragFloat(labelled(key, id).c_str(), &edited, speed, low, high, "%.2f",
                                              ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated())
            (void)inspector.beginGesture();
        if (ImGui::IsItemDeactivated())
            inspector.endGesture();
        return changed ? std::optional<float>(edited) : std::nullopt;
    };

    const core::InstanceId inHand = Editor::waterInHand(world, inspector);
    const scene::WaterComponent* water = inHand.valid() ? world.waters().find(inHand) : nullptr;
    const core::i32 held = water != nullptr ? water->shape : -1;
    const bool river = op == Editor::WaterOp::River && scene::waterIsRiver(held);
    const bool lake = op == Editor::WaterOp::Lake && held == scene::water_shape::Lake;
    const bool pool = op == Editor::WaterOp::Pool && scene::waterIsPool(held);
    const bool sea = op == Editor::WaterOp::Ocean && held == scene::water_shape::Ocean;

    if (river || lake || pool || sea) {
        // **The water in hand**, and its own numbers.
        const std::string_view name = world.atoms().text(world.name(inHand));
        if (river || lake) {
            core::i64 count = 0;
            for (core::InstanceId child = world.firstChild(inHand); child.valid(); child = world.nextSibling(child))
                count += world.waterPoints().find(child) != nullptr ? 1 : 0;
            ImGui::TextDisabled(
                "%s",
                core::tr(ENG_TR("engine.editor.water_panel.drawing"), {{"name", name}, {"count", count}}).c_str());
            if (lake && count < 3)
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.water_panel.lake_needs_three")));
        }
        else {
            ImGui::TextDisabled("%s", std::string(name).c_str());
        }
        const core::Vec3 size = water->size;
        // A river that descends has no one height: its points have theirs.
        if (held != scene::water_shape::River) {
            if (const std::optional<float> level =
                    number(ENG_TR("engine.editor.water_panel.surface"), "###water-surface",
                           static_cast<float>(water->surfaceLevel), 0.05f, -100000.0f, 100000.0f)) {
                inspector.enqueue(inHand, world.atoms().intern("SurfaceLevel"),
                                  scene::Value{static_cast<core::f64>(*level)});
            }
        }
        if (river) {
            if (const std::optional<float> width =
                    number(ENG_TR("engine.editor.water_panel.width"), "###water-width", size.x, 0.1f, 0.5f, 512.0f)) {
                inspector.enqueue(inHand, world.atoms().intern("Size"),
                                  scene::Value{core::Vec3{*width, size.y, size.z}});
                editor.setWaterWidth(*width);
            }
        }
        if (!sea) {
            if (const std::optional<float> depth =
                    number(ENG_TR("engine.editor.water_panel.depth"), "###water-depth", size.y, 0.1f, 0.25f, 512.0f)) {
                inspector.enqueue(inHand, world.atoms().intern("Size"),
                                  scene::Value{core::Vec3{size.x, *depth, size.z}});
                editor.setWaterDepth(*depth);
            }
        }
        if (river) {
            if (const std::optional<float> flow = number(ENG_TR("engine.editor.water_panel.flow"), "###water-flow",
                                                         static_cast<float>(water->flowSpeed), 0.05f, -64.0f, 64.0f)) {
                inspector.enqueue(inHand, world.atoms().intern("FlowSpeed"),
                                  scene::Value{static_cast<core::f64>(*flow)});
            }
        }
        if (!sea) {
            // **The bed, cut into the ground**: the water lies over the
            // ground until it is, and a river over a hillside shows why.
            if (const std::optional<float> bank = number(ENG_TR("engine.editor.water_panel.bank"), "###water-bank",
                                                         static_cast<float>(water->bankWidth), 0.05f, 0.0f, 64.0f)) {
                inspector.enqueue(inHand, world.atoms().intern("BankWidth"),
                                  scene::Value{static_cast<core::f64>(*bank)});
            }
            if (labeledIconButton(icons, icons::ActionDig, core::tr(ENG_TR("engine.editor.water_panel.carve"))))
                (void)editor.carveWater(world, root, inspector);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.water_panel.carve_tip")));
        }
        if (river || lake) {
            ImGui::Spacing();
            if (labeledIconButton(icons, icons::ActionAdd,
                                  core::tr(river ? ENG_TR("engine.editor.water_panel.new_river")
                                                 : ENG_TR("engine.editor.water_panel.new_lake"))))
                editor.finishRiver(inspector);
            ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.water_panel.new_river_tip")));
        }
        return;
    }

    // **Nothing in hand**: what the next one is made with.
    if (op == Editor::WaterOp::Ocean)
        return;
    if (op == Editor::WaterOp::River || op == Editor::WaterOp::Lake)
        ImGui::TextDisabled("%s", core::tr(op == Editor::WaterOp::River
                                               ? ENG_TR("engine.editor.water_panel.nothing_in_hand")
                                               : ENG_TR("engine.editor.water_panel.no_lake_in_hand")));
    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.water_panel.new_water")));
    if (op == Editor::WaterOp::River) {
        if (const std::optional<float> width = number(ENG_TR("engine.editor.water_panel.width"), "###water-width",
                                                      editor.waterWidth(), 0.1f, 0.5f, 512.0f))
            editor.setWaterWidth(*width);
    }
    if (const std::optional<float> depth = number(ENG_TR("engine.editor.water_panel.depth"), "###water-depth",
                                                  editor.waterDepth(), 0.1f, 0.25f, 512.0f))
        editor.setWaterDepth(*depth);
}

// --- The command palette and quick open (the owner's queue, Q2) -------------
//
// **Every command here is one the menus, the ribbon or a key already run**,
// named "Area: Verb" so typing either half finds it. Built each frame the
// palette is open and never otherwise: the enabled state follows the editor
// (Undo greys with nothing to undo) and a list that is not drawn costs nothing.
CommandPalette g_palette;
void toggleSideBar(EditorPanels& panels);
std::vector<PaletteItem> g_paletteCommands;
// Quick open's list, walked once when it opens: the content tree is a disk
// walk, and a file that appears while the list is up can wait for the next one.
std::vector<PaletteItem> g_paletteFiles;

void buildPaletteCommands(Editor& editor, EditorCommands& commands, EditorPanels& panels, EditorDialogs& dialogs,
                          const scene::World* world, const Inspector* inspector, core::InstanceId root,
                          const IconAtlas* icons)
{
    std::vector<PaletteItem>& out = g_paletteCommands;
    out.clear();
    // `id` names a command whose title changes with the state ("Undo Move",
    // "Show Console"); the menus find commands by it. Empty is the title.
    const auto add = [&](std::string title, std::string shortcut, std::string_view icon, bool enabled,
                         std::function<void()> run, std::string id = {}) {
        if (id.empty())
            id = title;
        out.push_back(PaletteItem{
            std::move(title), {}, std::move(shortcut), std::string(icon), enabled, std::move(run), std::move(id)});
    };

    const bool inPlay = editor.inPlayMode();
    const bool matching = editor.matchRunning();
    const bool stampOpen = editor.stampSession().open();
    const bool authoring = !inPlay;
    const bool hasSelection = inspector != nullptr && inspector->selectionCount() > 0;
    const bool editable =
        authoring && hasSelection && world != nullptr && !Editor::isEngineOwned(*world, inspector->selection(), root);
    const auto ask = [&](EditorDialogs::Pending what) {
        return
            [&editor, &commands, &dialogs, what] { issueOrAsk(what, editor.hasUnsavedWork(), {}, dialogs, commands); };
    };

    // File.
    add(core::tr(ENG_TR("engine.editor.command.file_new_scene")), "", icons::ContentScene, true,
        ask(EditorDialogs::Pending::NewScene), "File: New Scene");
    add(
        stampOpen ? std::string(core::tr(ENG_TR("engine.editor.command.file_save_stamp")))
                  : std::string(core::tr(ENG_TR("engine.editor.command.file_save_scene"))),
        "Ctrl+S", icons::ActionSave, true,
        [&editor, &commands] {
            if (editor.stampSession().open())
                commands.saveStamp = true;
            else if (editor.openScenePath().empty())
                commands.wantSaveAs = true;
            else
                commands.save = true;
        },
        "file.save");
    add(
        core::tr(ENG_TR("engine.editor.command.file_save_scene_as")), "Ctrl+Shift+S", icons::ActionSave, true,
        [&dialogs] { dialogs.saveAs = true; }, "File: Save Scene As...");
    add(
        core::tr(ENG_TR("engine.editor.command.file_export")), "Ctrl+Shift+B", icons::ActionExport, true,
        [&dialogs] { dialogs.exportWindow = true; }, "File: Export...");
    add(core::tr(ENG_TR("engine.editor.command.file_new_project")), "", icons::ActionNew, true,
        ask(EditorDialogs::Pending::NewProject), "File: New Project...");
    add(core::tr(ENG_TR("engine.editor.command.file_open_project")), "", icons::ActionOpen, true,
        ask(EditorDialogs::Pending::OpenProject), "File: Open Project...");
    add(
        core::tr(ENG_TR("engine.editor.command.file_go_to_file")), "Ctrl+P", icons::ActionOpen, true,
        [] { g_palette.open(CommandPalette::Mode::Files); }, "File: Go to File...");
    if (stampOpen) {
        add(
            core::tr(ENG_TR("engine.editor.command.file_close_stamp")), "", icons::ActionClose, true,
            [&commands] {
                commands.closeStamp = true;
                commands.closeStampSaving = true;
            },
            "File: Close Stamp");
    }
    add(core::tr(ENG_TR("engine.editor.command.file_exit")), "", icons::ActionClose, true,
        ask(EditorDialogs::Pending::Quit), "File: Exit");

    // Edit.
    const std::string undo =
        editor.history().canUndo()
            ? core::tr(ENG_TR("engine.editor.command.edit_undo_named"), {{"label", editor.history().undoLabel()}})
            : std::string(core::tr(ENG_TR("engine.editor.command.edit_undo")));
    const std::string redo =
        editor.history().canRedo()
            ? core::tr(ENG_TR("engine.editor.command.edit_redo_named"), {{"label", editor.history().redoLabel()}})
            : std::string(core::tr(ENG_TR("engine.editor.command.edit_redo")));
    add(
        undo, "Ctrl+Z", icons::ActionUndo, editor.history().canUndo(), [&commands] { commands.undo = true; },
        "edit.undo");
    add(
        redo, "Ctrl+Y", icons::ActionRedo, editor.history().canRedo(), [&commands] { commands.redo = true; },
        "edit.redo");
    add(
        core::tr(ENG_TR("engine.editor.command.edit_cut")), "Ctrl+X", icons::ActionCut, editable,
        [&commands] { commands.cutSelection = true; }, "Edit: Cut");
    add(
        core::tr(ENG_TR("engine.editor.command.edit_copy")), "Ctrl+C", icons::ActionCopy, editable,
        [&commands] { commands.copySelection = true; }, "Edit: Copy");
    add(
        core::tr(ENG_TR("engine.editor.command.edit_paste")), "Ctrl+V", icons::ActionPaste,
        authoring && editor.hasClipboard(), [&commands] { commands.paste = true; }, "Edit: Paste");
    add(
        core::tr(ENG_TR("engine.editor.command.edit_paste_into")), "Ctrl+Shift+V", icons::ActionPaste,
        authoring && editor.hasClipboard(), [&commands] { commands.pasteInto = true; }, "Edit: Paste Into");
    add(
        core::tr(ENG_TR("engine.editor.command.edit_duplicate")), "Ctrl+D", icons::ActionDuplicate, editable,
        [&commands] { commands.duplicateSelection = true; }, "Edit: Duplicate");
    add(
        core::tr(ENG_TR("engine.editor.command.edit_delete")), "Del", icons::ActionDelete, editable,
        [&commands] { commands.deleteSelection = true; }, "Edit: Delete");
    {
        const core::InstanceId target = editable ? inspector->selection() : core::InstanceId{};
        std::string seed = editable ? std::string(world->atoms().text(world->name(target))) : std::string();
        add(
            core::tr(ENG_TR("engine.editor.command.edit_rename")), "F2", icons::ActionRename, editable,
            [&dialogs, target, seed] {
                dialogs.renameTarget = target;
                dialogs.renameContentPath.clear();
                dialogs.renameSeed = seed;
                dialogs.renameInstance = true;
            },
            "Edit: Rename...");
    }
    add(
        core::tr(ENG_TR("engine.editor.command.edit_group")), "Ctrl+G", icons::ClassModel, editable,
        [&commands] { commands.groupSelection = true; }, "Edit: Group");
    add(
        core::tr(ENG_TR("engine.editor.command.edit_group_as_folder")), "Ctrl+Alt+G", icons::ClassFolder, editable,
        [&commands] { commands.groupAsFolder = true; }, "Edit: Group as Folder");
    add(
        core::tr(ENG_TR("engine.editor.command.edit_ungroup")), "Ctrl+Shift+G", icons::ActionExpand, editable,
        [&commands] { commands.ungroupSelection = true; }, "Edit: Ungroup");
    add(
        core::tr(ENG_TR("engine.editor.command.selection_clear")), "Esc", icons::ActionSelect, hasSelection,
        [&commands] { commands.clearSelection = true; }, "Selection: Clear");

    // The transform tools.
    const auto tool = [&editor](std::optional<GizmoMode> mode) {
        return [&editor, mode] {
            editor.setTool(Editor::Tool::Select);
            if (mode.has_value())
                editor.setGizmoMode(*mode);
            else
                editor.setHandlesShown(false);
        };
    };
    add(core::tr(ENG_TR("engine.editor.command.tool_select")), "Ctrl+1", icons::ActionSelect, true, tool(std::nullopt),
        "Tool: Select");
    add(core::tr(ENG_TR("engine.editor.command.tool_move")), "Ctrl+2", icons::ActionMove, true,
        tool(GizmoMode::Translate), "Tool: Move");
    add(core::tr(ENG_TR("engine.editor.command.tool_resize")), "Ctrl+3", icons::ActionScale, true,
        tool(GizmoMode::Scale), "Tool: Resize");
    add(core::tr(ENG_TR("engine.editor.command.tool_turn")), "Ctrl+4", icons::ActionRotate, true,
        tool(GizmoMode::Rotate), "Tool: Turn");
    add(
        editor.gizmoLocal() ? std::string(core::tr(ENG_TR("engine.editor.command.tool_use_world_axes")))
                            : std::string(core::tr(ENG_TR("engine.editor.command.tool_use_local_axes"))),
        "Ctrl+L", icons::ActionMove, true, [&editor] { editor.setGizmoLocal(!editor.gizmoLocal()); }, "tool.axes");
    add(
        editor.snapping() ? std::string(core::tr(ENG_TR("engine.editor.command.tool_turn_snapping_off")))
                          : std::string(core::tr(ENG_TR("engine.editor.command.tool_turn_snapping_on"))),
        "", icons::ActionGrid, true, [&editor] { editor.setSnap(!editor.snapping()); }, "tool.snap");
    add(
        core::tr(ENG_TR("engine.editor.command.tool_terrain")), "", icons::ClassTerrain, true,
        [&panels] {
            panels.terrain = true;
            ImGui::SetWindowFocus("Terrain");
        },
        "Tool: Terrain");
    add(
        core::tr(ENG_TR("engine.editor.command.tool_blocks")), "", icons::ClassVoxelService, true,
        [&panels] {
            panels.blocks = true;
            ImGui::SetWindowFocus("Blocks");
        },
        "Tool: Blocks");
    add(
        core::tr(ENG_TR("engine.editor.command.tool_tiles")), "", icons::ClassTilemap2D, true,
        [&panels] {
            panels.tiles = true;
            ImGui::SetWindowFocus("Tiles###Tiles");
        },
        "Tool: Tiles");
    add(
        core::tr(ENG_TR("engine.editor.command.tool_water")), "", WaterIcon, true,
        [&panels, &editor] {
            panels.water = true;
            editor.setTool(Editor::Tool::Water);
            ImGui::SetWindowFocus("###Water");
        },
        "Tool: Water");
    add(
        core::tr(ENG_TR("engine.editor.command.view_frame_selection")), "F", icons::ClassCamera,
        world != nullptr && hasSelection,
        [&editor, world, inspector] {
            core::DVec3 centre;
            core::f64 radius = 0.0;
            if (selectionBounds(*world, inspector->selectionSet(), centre, radius))
                editor.focusCamera(centre, radius);
        },
        "View: Frame Selection");

    // Run.
    const RunState run = editor.runState();
    add(
        editor.matchSettings().isMatch() ? std::string(core::tr(ENG_TR("engine.editor.command.run_start_match")))
                                         : std::string(core::tr(ENG_TR("engine.editor.command.run_start"))),
        "F5", icons::ActionPlay, !inPlay && !matching && !stampOpen,
        [&editor, &commands] {
            if (editor.matchSettings().isMatch())
                commands.match = true;
            else
                commands.play = true;
        },
        "run.start");
    add(
        core::tr(ENG_TR("engine.editor.command.run_stop")), "Shift+F5", icons::ActionStop, inPlay || matching,
        [&commands, matching] {
            if (matching)
                commands.match = false;
            else
                commands.play = false;
        },
        "Run: Stop");
    add(
        run == RunState::Paused ? std::string(core::tr(ENG_TR("engine.editor.command.run_resume")))
                                : std::string(core::tr(ENG_TR("engine.editor.command.run_pause"))),
        "", icons::ActionPause, inPlay, [&commands, run] { commands.pause = run != RunState::Paused; }, "run.pause");
    add(
        core::tr(ENG_TR("engine.editor.command.run_step_one_tick")), "", icons::ActionForward,
        inPlay && run == RunState::Paused, [&editor] { editor.requestStep(); }, "Run: Step One Tick");
    add(
        editor.cameraDetached() ? std::string(core::tr(ENG_TR("engine.editor.command.run_follow_the_game_s_camera")))
                                : std::string(core::tr(ENG_TR("engine.editor.command.run_free_camera"))),
        "Shift+P", icons::ActionVisible, inPlay, [&editor] { editor.setCameraDetached(!editor.cameraDetached()); },
        "run.camera");

    // View.
    // `title` is the command's id; `name` the panel's name as shown.
    const auto panel = [&](const char* title, core::TextKey name, const char* shortcut, std::string_view icon,
                           bool& visible) {
        add(
            core::tr(visible ? ENG_TR("engine.editor.command.view_hide") : ENG_TR("engine.editor.command.view_show"),
                     {{"panel", std::string_view(core::tr(name))}}),
            shortcut, icon, true, [&visible] { visible = !visible; }, std::string("view.") + title);
    };
    add(
        core::tr(ENG_TR("engine.editor.command.view_toggle_side_bar")), "Ctrl+B", icons::ClassModel, true,
        [&panels] { toggleSideBar(panels); }, "View: Toggle Side Bar");
    panel("Explorer", ENG_TR("engine.editor.panel.explorer"), "Ctrl+Shift+E", icons::ClassModel, panels.explorer);
    panel("Properties", ENG_TR("engine.editor.panel.properties"), "Ctrl+Alt+B", icons::ActionSettings,
          panels.properties);
    panel("Console", ENG_TR("engine.editor.panel.console"), "Ctrl+J", icons::ClassScriptService, panels.console);
    panel("Content", ENG_TR("engine.editor.panel.content"), "", icons::ContentFolder, panels.content);
    panel("Viewport", ENG_TR("engine.editor.panel.viewport"), "", icons::ClassCamera, panels.viewport);
    panel("Stats", ENG_TR("engine.editor.panel.stats"), "", icons::ClassDebugService, panels.stats);
    panel("Streaming", ENG_TR("engine.editor.panel.streaming"), "", icons::ClassStreamingService, panels.streaming);
    panel("Saves", ENG_TR("engine.editor.panel.saves"), "", icons::ActionSave, panels.saves);
    panel("Viewport Settings", ENG_TR("engine.editor.panel.viewport_settings"), "", icons::ClassCamera,
          panels.viewportSettings);
    panel("Debug", ENG_TR("engine.editor.panel.debug"), "", icons::ClassDebugService, panels.debug);
    panel("Grid", ENG_TR("engine.editor.panel.grid"), "", icons::ActionGrid, panels.showGrid);
    panel("Collision Shapes", ENG_TR("engine.editor.panel.collision_shapes"), "", icons::ClassPhysicsService,
          panels.showCollision);
    panel("Skeletons", ENG_TR("engine.editor.panel.skeletons"), "", icons::ClassBone, panels.showSkeletons);
    add(
        core::tr(ENG_TR("engine.editor.command.view_reset_layout")), "", icons::ClassUIService, true,
        [&commands] { commands.resetLayout = true; }, "View: Reset Layout");

    // Preferences, and one command per theme, as the editor this follows lists
    // them under "Color Theme".
    add(
        core::tr(ENG_TR("engine.editor.command.preferences_open_settings")), "Ctrl+,", icons::ActionSettings, true,
        [&dialogs] { dialogs.preferences = true; }, "Preferences: Open Settings");
    add(
        core::tr(ENG_TR("engine.editor.command.preferences_keyboard_shortcuts")), "", icons::ActionSettings, true,
        [&dialogs] {
            dialogs.preferences = true;
            g_preferencesToShortcuts = true;
        },
        "Preferences: Keyboard Shortcuts");
    add(
        core::tr(ENG_TR("engine.editor.command.preferences_project_settings")), "", icons::ClassWorkspace, true,
        [&dialogs] { dialogs.projectSettings = true; }, "Preferences: Project Settings");
    for (const Theme& theme : themes()) {
        const std::string id(theme.id);
        add(
            core::tr(ENG_TR("engine.editor.command.preferences_color_theme"), {{"theme", shownName(theme)}}), "",
            icons::ClassLighting, g_appearance.themeId != id,
            [id] {
                g_appearance.themeId = id;
                applyAppearance();
            },
            "theme." + id);
    }
    add(
        core::tr(ENG_TR("engine.editor.command.help_welcome")), "", icons::ActionInformation, true,
        [&panels] {
            panels.welcome = true;
            ImGui::SetWindowFocus("###Welcome");
        },
        "Help: Welcome");
    add(
        core::tr(ENG_TR("engine.editor.command.help_about")), "", icons::ActionInformation, true,
        [&dialogs] { dialogs.about = true; }, "Help: About");

    // Insert, one per class a person can make: the Explorer's plus, by name.
    if (world != nullptr && inspector != nullptr && !stampOpen) {
        if (g_creatableWorld != inspector->worldIdentity() || g_creatable.empty()) {
            g_creatableWorld = inspector->worldIdentity();
            collectCreatableClasses(*world, g_creatable);
        }
        for (const scene::ClassId id : g_creatable) {
            const scene::ClassDescriptor* descriptor = world->classes().find(id);
            if (descriptor == nullptr)
                continue;
            std::string name(world->atoms().text(descriptor->name));
            const std::string icon = classIconFor(icons, world->classes(), world->atoms(), id);
            add(
                core::tr(ENG_TR("engine.editor.command.insert_class"), {{"class", name}}), "", icon, authoring,
                [&commands, name] { commands.insertClassName = name; }, "Insert: " + name);
        }
    }
}

// **What the command list is built from this frame**, so the menus -- drawn
// before anything else -- and the palette -- drawn after everything -- ask for
// the same list, and it is built at most once a frame, and only when one of
// them is open.
struct CommandContext
{
    Editor* editor = nullptr;
    EditorCommands* commands = nullptr;
    EditorPanels* panels = nullptr;
    EditorDialogs* dialogs = nullptr;
    const scene::World* world = nullptr;
    const Inspector* inspector = nullptr;
    core::InstanceId root;
    const IconAtlas* icons = nullptr;
};
CommandContext g_commandContext;
int g_commandsFrame = -1;

const std::vector<PaletteItem>& currentCommands()
{
    if (g_commandsFrame != ImGui::GetFrameCount() && g_commandContext.editor != nullptr) {
        const CommandContext& c = g_commandContext;
        buildPaletteCommands(*c.editor, *c.commands, *c.panels, *c.dialogs, c.world, c.inspector, c.root, c.icons);
        g_commandsFrame = ImGui::GetFrameCount();
    }
    return g_paletteCommands;
}

[[nodiscard]] const PaletteItem* findCommand(std::string_view id)
{
    for (const PaletteItem& item : currentCommands()) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

// A menu item that runs a palette command: its icon, its shortcut, whether it
// can act -- one definition, two ways in. The label is the title after its
// area ("File: Save Scene" reads "Save Scene" under File) unless given.
bool menuCommand(std::string_view id, const char* label, bool checked)
{
    const PaletteItem* item = findCommand(id);
    if (item == nullptr)
        return false;
    std::string text = label != nullptr ? std::string(label) : item->title;
    if (label == nullptr) {
        if (const std::size_t colon = text.find(": "); colon != std::string::npos)
            text = text.substr(colon + 2);
    }
    const std::string shortcut = item->shortcut;
    const std::function<void()> run = item->run;
    if (!iconMenuItem(g_commandContext.icons, item->icon, text.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(),
                      checked, item->enabled))
        return false;
    if (run)
        run();
    return true;
}

// Runs a palette command by id, from somewhere that is not a menu.
void runCommand(std::string_view id)
{
    const PaletteItem* item = findCommand(id);
    if (item == nullptr || !item->enabled || !item->run)
        return;
    const std::function<void()> run = item->run;
    run();
}

// Quick open: the project's scenes, stamps, materials and shaders, and every
// script in the tree -- what a person means by "a file" here.
void buildPaletteFiles(Editor& editor, EditorCommands& commands, EditorDialogs& dialogs, const scene::World* world,
                       core::InstanceId root, const IconAtlas* icons)
{
    std::vector<PaletteItem>& out = g_paletteFiles;
    out.clear();
    const auto split = [](std::string_view path) {
        const std::size_t slash = path.rfind('/');
        if (slash == std::string_view::npos)
            return std::pair<std::string, std::string>(std::string(path), std::string());
        return std::pair<std::string, std::string>(std::string(path.substr(slash + 1)),
                                                   std::string(path.substr(0, slash)));
    };
    for (const ContentKind kind :
         {ContentKind::Scene, ContentKind::Stamp, ContentKind::Material, ContentKind::Shader}) {
        for (const std::string& path : editor.content().filesOfKind(kind)) {
            auto [name, folder] = split(path);
            PaletteItem item;
            item.title = std::move(name);
            item.detail = std::move(folder);
            item.icon = std::string(contentKindIcon(kind));
            switch (kind) {
            case ContentKind::Scene:
                item.run = [&editor, &commands, &dialogs, path] { openSceneOrAsk(editor, commands, dialogs, path); };
                break;
            case ContentKind::Stamp:
                item.run = [&commands, path] { commands.openStamp = path; };
                break;
            case ContentKind::Material:
                item.run = [&commands, path] { commands.openMaterial = path; };
                break;
            default:
                item.run = [&commands, path] { commands.openFile = path; };
                break;
            }
            out.push_back(std::move(item));
        }
    }

    // **Every script of the project can be found**, not only the ones in the
    // world (the owner found `game.luau` missing): the code of a scene other
    // than the open one -- `src/scenes/<scene>/` -- is not mounted, so the tree
    // below never lists it. It opens as a file, relative to the content root
    // the way every file tab is. The open scene's own folder and `src/client`,
    // `src/server` and `src/shared` are in the tree already.
    {
        std::error_code ec;
        const std::filesystem::path project = editor.content().root().parent_path();
        const std::filesystem::path scenes = project / "src" / "scenes";
        std::string openScene = std::filesystem::path(editor.openScenePath()).filename().string();
        if (const std::size_t dot = openScene.find('.'); dot != std::string::npos)
            openScene.resize(dot);
        std::vector<std::string> found;
        for (std::filesystem::recursive_directory_iterator walk(scenes, ec), done; !ec && walk != done;
             walk.increment(ec)) {
            if (!walk->is_regular_file(ec) || walk->path().extension() != ".luau")
                continue;
            const std::string relative = walk->path().lexically_relative(project).generic_string();
            const std::string scene = walk->path().lexically_relative(scenes).begin()->generic_string();
            if (!openScene.empty() && scene == openScene)
                continue;
            found.push_back(relative);
        }
        // The walk's order is the file system's; the list is the palette's.
        std::sort(found.begin(), found.end());
        for (const std::string& relative : found) {
            auto [name, folder] = split(relative);
            PaletteItem item;
            item.title = std::move(name);
            item.detail = std::move(folder);
            item.icon = std::string(icons::ClassScript);
            item.run = [&commands, relative] { commands.openFile = "../" + relative; };
            out.push_back(std::move(item));
        }
    }

    if (world == nullptr)
        return;
    const scene::ClassId baseScript = world->classes().findId(world->atoms().lookup("BaseScript"));
    // Depth first, in tree order; the path to each script is its detail, the
    // way a file's folder is.
    std::vector<core::InstanceId> stack{root};
    std::vector<core::InstanceId> children;
    while (!stack.empty()) {
        const core::InstanceId id = stack.back();
        stack.pop_back();
        children.clear();
        for (core::InstanceId child = world->firstChild(id); child.valid(); child = world->nextSibling(child))
            children.push_back(child);
        stack.insert(stack.end(), children.rbegin(), children.rend());
        if (id == root || !world->classes().isA(world->classOf(id), baseScript))
            continue;
        std::string path;
        for (core::InstanceId up = world->parentOf(id); up.valid() && up != root; up = world->parentOf(up))
            path = std::string(world->atoms().text(world->name(up))) + (path.empty() ? "" : ".") + path;
        PaletteItem item;
        item.title = std::string(world->atoms().text(world->name(id)));
        item.detail = std::move(path);
        item.icon = classIconFor(icons, world->classes(), world->atoms(), world->classOf(id));
        item.run = [&commands, id] { commands.openScript = id; };
        out.push_back(std::move(item));
    }
}

void openCommandPalette()
{
    g_palette.open(CommandPalette::Mode::Commands);
}

void openQuickOpen()
{
    g_palette.open(CommandPalette::Mode::Files);
}

// The palette's keys, and the other window-wide keys the editor this follows
// has taught everybody's hands. Read whatever has the keyboard: there,
// Ctrl+Shift+P works with the caret in a file.
void handleWorkbenchKeys(Editor& editor, EditorCommands& commands, EditorPanels& panels, EditorDialogs& dialogs,
                         const DebugView& debug)
{
    const auto global = workbenchKey;
    // **While the game has the keyboard, the keys are the game's** (the Play
    // rule): F1, Ctrl+P, Ctrl+B and Ctrl+J opened the editor's furniture over
    // a game that uses them. Only what controls the play itself still fires --
    // stop below, the eye, pause, and the debugger's keys.
    if (!editor.gameHasKeyboard()) {
        if (global(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P) || global(ImGuiKey_F1))
            g_palette.open(CommandPalette::Mode::Commands);
        else if (global(ImGuiMod_Ctrl | ImGuiKey_P))
            g_palette.open(CommandPalette::Mode::Files);
        if (global(ImGuiMod_Ctrl | ImGuiKey_B))
            toggleSideBar(panels);
        if (global(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_B))
            panels.properties = !panels.properties;
        if (global(ImGuiMod_Ctrl | ImGuiKey_J))
            panels.console = !panels.console;
        if (global(ImGuiMod_Ctrl | ImGuiKey_GraveAccent)) {
            panels.console = true;
            ImGui::SetWindowFocus("Console");
        }
        if (global(ImGuiMod_Ctrl | ImGuiKey_Comma))
            dialogs.preferences = true;
    }
    // F5 is the debugger's Continue while a script is stopped, so it starts the
    // game only when nothing is.
    if (!debug.parked && !editor.inPlayMode() && !editor.matchRunning() && !editor.stampSession().open() &&
        global(ImGuiKey_F5)) {
        if (editor.matchSettings().isMatch())
            commands.match = true;
        else
            commands.play = true;
    }
    if (global(ImGuiMod_Shift | ImGuiKey_F5)) {
        if (editor.matchRunning())
            commands.match = false;
        else if (editor.inPlayMode())
            commands.play = false;
    }
}

// --- The status bar (the owner's queue, Q2) ---------------------------------
//
// **One line at the bottom that answers "what state is this in"** without
// opening anything: running or not, which scene and whether it is saved, how
// many errors and warnings the console holds, what is selected, which tool is
// in hand. The editor this follows turns the whole bar the accent colour while
// a program runs, and so does this one -- the surest way to know an edit made
// now will be thrown away at stop.
namespace {

// The problem glyphs, drawn rather than taken from the icon set, which has
// neither: a circled cross and a triangle with a bang, as the bar they imitate
// draws them.
void paintErrorGlyph(ImDrawList* draw, ImVec2 centre, float radius, ImU32 colour)
{
    const float thickness = std::max(1.0f, radius * 0.18f);
    draw->AddCircle(centre, radius, colour, 0, thickness);
    const float arm = radius * 0.42f;
    draw->AddLine(ImVec2(centre.x - arm, centre.y - arm), ImVec2(centre.x + arm, centre.y + arm), colour, thickness);
    draw->AddLine(ImVec2(centre.x - arm, centre.y + arm), ImVec2(centre.x + arm, centre.y - arm), colour, thickness);
}

void paintWarningGlyph(ImDrawList* draw, ImVec2 centre, float radius, ImU32 colour)
{
    const float thickness = std::max(1.0f, radius * 0.18f);
    const ImVec2 top(centre.x, centre.y - radius);
    const ImVec2 left(centre.x - radius * 1.05f, centre.y + radius * 0.85f);
    const ImVec2 right(centre.x + radius * 1.05f, centre.y + radius * 0.85f);
    draw->AddTriangle(top, right, left, colour, thickness);
    draw->AddLine(ImVec2(centre.x, centre.y - radius * 0.35f), ImVec2(centre.x, centre.y + radius * 0.25f), colour,
                  thickness);
    draw->AddCircleFilled(ImVec2(centre.x, centre.y + radius * 0.55f), thickness * 0.6f, colour);
}

// One item: a hover ground the width of its content, and a click. `paint`
// draws the content from the left edge at the given centre line.
bool statusItem(const char* id, float width, float height, const char* tip,
                const std::function<void(ImDrawList*, ImVec2 min)>& paint)
{
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float padding = 5.0f * ImGui::GetStyle().FontScaleMain;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width + padding * 2.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered())
        draw->AddRectFilled(min, ImVec2(min.x + width + padding * 2.0f, min.y + height), IM_COL32(255, 255, 255, 30));
    paint(draw, ImVec2(min.x + padding, min.y));
    if (tip != nullptr && tip[0] != '\0')
        ImGui::SetItemTooltip("%s", tip);
    ImGui::SameLine(0.0f, 2.0f * ImGui::GetStyle().FontScaleMain);
    return pressed;
}

// A plain item: an optional icon, then words.
bool statusText(const IconAtlas* icons, const char* id, std::string_view icon, const std::string& text, ImU32 ink,
                float height, const char* tip)
{
    const float glyph = ImGui::GetFontSize();
    const float gap = 4.0f * ImGui::GetStyle().FontScaleMain;
    const float iconWidth = icon.empty() ? 0.0f : glyph + gap;
    const float width = iconWidth + ImGui::CalcTextSize(text.c_str()).x;
    return statusItem(id, width, height, tip, [&](ImDrawList* draw, ImVec2 min) {
        const float y = min.y + (height - glyph) * 0.5f;
        if (!icon.empty())
            paintActionIcon(icons, icon, ImVec2(min.x, y), glyph);
        draw->AddText(ImVec2(min.x + iconWidth, y), ink, text.c_str());
    });
}

[[nodiscard]] const char* gizmoWord(GizmoMode mode) noexcept
{
    switch (mode) {
    case GizmoMode::Translate:
        return core::tr(ENG_TR("engine.editor.status_bar.move"));
    case GizmoMode::Rotate:
        return core::tr(ENG_TR("engine.editor.status_bar.turn"));
    case GizmoMode::Scale:
        return core::tr(ENG_TR("engine.editor.status_bar.resize"));
    }
    return core::tr(ENG_TR("engine.editor.status_bar.move"));
}

} // namespace

void drawStatusBar(Editor& editor, const Inspector* inspector, EditorPanels& panels, const IconAtlas* icons)
{
    const float scale = ImGui::GetStyle().FontScaleMain;
    const float height = std::round(ImGui::GetFontSize() + 8.0f * scale);
    const bool running = editor.inPlayMode() || editor.matchRunning();
    const ThemePalette& p = palette();
    const ImVec4 ground = themeColor(running ? p.accentFill : p.background);
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(themeColor(running ? p.onAccent : p.textMuted));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    if (ImGui::BeginViewportSideBar("##status-bar", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags)) {
        const ImVec2 origin = ImGui::GetWindowPos();
        const float width = ImGui::GetWindowWidth();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (!running) {
            draw->AddLine(origin, ImVec2(origin.x + width, origin.y),
                          ImGui::ColorConvertFloat4ToU32(themeColor(p.border)));
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 4.0f * scale, origin.y));

        // Running, paused, a match, a stamp -- or editing.
        const RunState run = editor.runState();
        std::string state = core::tr(ENG_TR("engine.editor.status_bar.editing"));
        std::string_view stateIcon = icons::ActionSelect;
        if (editor.matchRunning()) {
            const Editor::MatchSettings& match = editor.matchSettings();
            state = core::tr(match.dedicated ? ENG_TR("engine.editor.status_bar.match_dedicated")
                                             : ENG_TR("engine.editor.status_bar.match"),
                             {{"count", static_cast<core::i64>(match.players)}});
            stateIcon = icons::ActionPlay;
        }
        else if (run == RunState::Playing) {
            state = core::tr(ENG_TR("engine.editor.status_bar.playing"));
            stateIcon = icons::ActionPlay;
        }
        else if (run == RunState::Paused) {
            state = core::tr(ENG_TR("engine.editor.status_bar.paused"));
            stateIcon = icons::ActionPause;
        }
        else if (editor.stampSession().open()) {
            state = core::tr(ENG_TR("engine.editor.status_bar.editing_a_stamp"));
            stateIcon = icons::OverlayStamp;
        }
        (void)statusText(icons, "##state", stateIcon, state, ink, height,
                         core::tr(running ? ENG_TR("engine.editor.status_bar.stops_the_game_tip")
                                          : ENG_TR("engine.editor.status_bar.starts_the_game_tip")));

        // Which scene, and whether it has unsaved work: the dot the editor this
        // follows puts on a tab with changes.
        const std::string scenePath = editor.openScenePath();
        std::string sceneName =
            scenePath.empty() ? std::string(core::tr(ENG_TR("engine.editor.status_bar.untitled"))) : scenePath;
        if (const std::size_t slash = sceneName.rfind('/'); slash != std::string::npos)
            sceneName = sceneName.substr(slash + 1);
        if (editor.hasUnsavedWork())
            sceneName += "  \xE2\x97\x8F";
        const std::string sceneTip = core::tr(
            editor.hasUnsavedWork() ? ENG_TR("engine.editor.status_bar.scene_unsaved_tip")
                                    : ENG_TR("engine.editor.status_bar.scene_tip"),
            {{"scene",
              scenePath.empty() ? std::string(core::tr(ENG_TR("engine.editor.status_bar.never_saved"))) : scenePath}});
        if (statusText(icons, "##scene", icons::ContentScene, sceneName, ink, height, sceneTip.c_str()))
            g_palette.open(CommandPalette::Mode::Files);

        // The console's errors and warnings, counted from what it holds now --
        // so clearing the console clears the count.
        int errors = 0;
        int warnings = 0;
        {
            ConsoleLog& log = console();
            std::lock_guard<std::mutex> lock(log.mutex);
            for (const ConsoleLog::Line& line : log.lines) {
                errors += line.level == core::LogLevel::Error ? 1 : 0;
                warnings += line.level == core::LogLevel::Warn ? 1 : 0;
            }
        }
        {
            const std::string errorText = std::to_string(errors);
            const std::string warningText = std::to_string(warnings);
            const float glyph = ImGui::GetFontSize() * 0.9f;
            const float gap = 4.0f * scale;
            const float span = glyph + gap + ImGui::CalcTextSize(errorText.c_str()).x + gap * 2.0f + glyph + gap +
                               ImGui::CalcTextSize(warningText.c_str()).x;
            const ImU32 errorInk = errors > 0 && !running ? ImGui::ColorConvertFloat4ToU32(themeColor(p.danger)) : ink;
            const ImU32 warningInk =
                warnings > 0 && !running ? ImGui::ColorConvertFloat4ToU32(themeColor(p.warning)) : ink;
            const std::string tip = core::tr(ENG_TR("engine.editor.status_bar.problems_tip"),
                                             {{"errors", core::tr(ENG_TR("engine.editor.status_bar.errors"),
                                                                  {{"count", static_cast<core::i64>(errors)}})},
                                              {"warnings", core::tr(ENG_TR("engine.editor.status_bar.warnings"),
                                                                    {{"count", static_cast<core::i64>(warnings)}})}});
            if (statusItem("##problems", span, height, tip.c_str(), [&](ImDrawList* list, ImVec2 min) {
                    const float mid = min.y + height * 0.5f;
                    const float textY = min.y + (height - ImGui::GetFontSize()) * 0.5f;
                    float x = min.x;
                    paintErrorGlyph(list, ImVec2(x + glyph * 0.5f, mid), glyph * 0.45f, errorInk);
                    x += glyph + gap;
                    list->AddText(ImVec2(x, textY), ink, errorText.c_str());
                    x += ImGui::CalcTextSize(errorText.c_str()).x + gap * 2.0f;
                    paintWarningGlyph(list, ImVec2(x + glyph * 0.5f, mid), glyph * 0.45f, warningInk);
                    x += glyph + gap;
                    list->AddText(ImVec2(x, textY), ink, warningText.c_str());
                })) {
                panels.console = true;
                ImGui::SetWindowFocus("Console");
            }
        }

        // The right-hand side, laid from the right edge: frame rate, tool,
        // selection. Each is measured first so they can be placed.
        const float padding = 5.0f * scale;
        const float spacing = 2.0f * scale;
        float right = origin.x + width - 4.0f * scale;
        const auto place = [&](const std::string& text) {
            const float itemWidth = ImGui::CalcTextSize(text.c_str()).x;
            right -= itemWidth + padding * 2.0f + spacing;
            ImGui::SetCursorScreenPos(ImVec2(right, origin.y));
            return itemWidth;
        };
        const float leftEnd = ImGui::GetCursorScreenPos().x;

        const float fps = ImGui::GetIO().Framerate;
        const std::string rateText =
            core::tr(ENG_TR("engine.editor.status_bar.fps"), {{"fps", fixed(static_cast<double>(fps), 0)}});
        const std::string rateTip =
            core::tr(ENG_TR("engine.editor.status_bar.frame_time_tip"),
                     {{"ms", fixed(static_cast<double>(fps > 0.0f ? 1000.0f / fps : 0.0f), 2)}});
        const float rateWidth = place(rateText);
        if (right > leftEnd &&
            statusItem("##rate", rateWidth, height, rateTip.c_str(), [&](ImDrawList* list, ImVec2 min) {
                list->AddText(ImVec2(min.x, min.y + (height - ImGui::GetFontSize()) * 0.5f), ink, rateText.c_str());
            })) {
            panels.stats = true;
            ImGui::SetWindowFocus("Stats");
        }

        // A brush in hand says so, and how to put it down -- a terrain brush
        // only with its panel open, which is when it is one.
        const char* brush = nullptr;
        switch (editor.tool()) {
        case Editor::Tool::Sculpt:
            brush = editor.terrainPanelShown() ? core::tr(ENG_TR("engine.editor.status_bar.brush.sculpt")) : nullptr;
            break;
        case Editor::Tool::Paint:
            brush = editor.terrainPanelShown() ? core::tr(ENG_TR("engine.editor.status_bar.brush.paint")) : nullptr;
            break;
        case Editor::Tool::Blocks:
            brush = core::tr(ENG_TR("engine.editor.status_bar.brush.blocks"));
            break;
        case Editor::Tool::Tiles:
            brush = core::tr(ENG_TR("engine.editor.status_bar.brush.tiles"));
            break;
        case Editor::Tool::Foliage:
            brush = core::tr(ENG_TR("engine.editor.status_bar.brush.foliage"));
            break;
        case Editor::Tool::Water:
            brush = editor.waterPanelShown() ? core::tr(ENG_TR("engine.editor.status_bar.brush.water")) : nullptr;
            break;
        case Editor::Tool::Select:
            break;
        }
        if (!running && brush != nullptr) {
            const std::string words = terrainBrushWords(editor);
            const std::string text =
                words.empty()
                    ? core::tr(ENG_TR("engine.editor.status_bar.brush_in_hand"), {{"brush", brush}})
                    : core::tr(ENG_TR("engine.editor.status_bar.brush_doing"), {{"brush", brush}, {"doing", words}});
            const float brushWidth = place(text);
            if (right > leftEnd &&
                statusItem("##tool", brushWidth, height,
                           core::tr(ENG_TR("engine.editor.status_bar.put_the_brush_down_tip")),
                           [&](ImDrawList* list, ImVec2 min) {
                               list->AddText(ImVec2(min.x, min.y + (height - ImGui::GetFontSize()) * 0.5f),
                                             ImGui::ColorConvertFloat4ToU32(themeColor(p.warning)), text.c_str());
                           })) {
                editor.setTool(Editor::Tool::Select);
            }
        }
        if (!running && editor.tool() == Editor::Tool::Select) {
            std::string tool = editor.handlesShown() ? gizmoWord(editor.gizmoMode())
                                                     : core::tr(ENG_TR("engine.editor.status_bar.select"));
            if (editor.handlesShown()) {
                const bool local = editor.gizmoLocal() || editor.gizmoMode() == GizmoMode::Scale;
                tool = core::tr(local ? ENG_TR("engine.editor.status_bar.tool_local")
                                      : ENG_TR("engine.editor.status_bar.tool_world"),
                                {{"tool", tool}});
            }
            if (editor.snapping()) {
                tool = core::tr(
                    ENG_TR("engine.editor.status_bar.tool_snapping"),
                    {{"tool", tool}, {"step", fixed(static_cast<double>(editor.snapStep(GizmoMode::Translate)), 2)}});
            }
            const float toolWidth = place(tool);
            const std::string toolTip = core::tr(ENG_TR("engine.editor.status_bar.tool_keys_tip"),
                                                 {{"speed", fixed(static_cast<double>(editor.cameraSpeed()), 0)}});
            if (right > leftEnd)
                (void)statusItem("##tool", toolWidth, height, toolTip.c_str(), [&](ImDrawList* list, ImVec2 min) {
                    list->AddText(ImVec2(min.x, min.y + (height - ImGui::GetFontSize()) * 0.5f), ink, tool.c_str());
                });
        }

        const core::usize selected = inspector != nullptr ? inspector->selectionCount() : 0;
        if (selected > 0) {
            const std::string text =
                core::tr(ENG_TR("engine.editor.status_bar.selected"), {{"count", static_cast<core::i64>(selected)}});
            const float selectedWidth = place(text);
            if (right > leftEnd)
                (void)statusItem(
                    "##selection", selectedWidth, height, core::tr(ENG_TR("engine.editor.status_bar.esc_clears_tip")),
                    [&](ImDrawList* list, ImVec2 min) {
                        list->AddText(ImVec2(min.x, min.y + (height - ImGui::GetFontSize()) * 0.5f), ink, text.c_str());
                    });
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// --- The activity bar (the owner's queue, Q2) -------------------------------
//
// **A column of icons down the left edge, one per side-bar view**, as the
// editor this follows has it: a click shows that view, a click on the view
// already showing hides it, and the one showing is marked with a bar in the
// accent. The icons are drawn in one ink, not in their class colours -- a
// column of coloured pictures reads as content, a column of glyphs reads as
// navigation.
namespace {

struct ActivityView
{
    // The window's `###` identity, which is what ImGui finds it by.
    const char* window;
    // Its name as shown (ADR 0145).
    core::TextKey title;
    std::string_view icon;
    bool EditorPanels::*visible;
    const char* shortcut;
};

constexpr ActivityView ActivityViews[] = {
    {"###Explorer", ENG_TR("engine.editor.panel.explorer"), icons::ClassModel, &EditorPanels::explorer, "Ctrl+Shift+E"},
    {"###Content", ENG_TR("engine.editor.panel.content"), icons::ContentFolder, &EditorPanels::content, "Ctrl+Shift+A"},
    {"###Debug", ENG_TR("engine.editor.panel.run_and_debug"), icons::ClassDebugService, &EditorPanels::debug,
     "Ctrl+Shift+D"},
    {"###Terrain", ENG_TR("engine.editor.panel.terrain"), icons::ClassTerrain, &EditorPanels::terrain, ""},
    {"###Blocks", ENG_TR("engine.editor.panel.blocks"), icons::ClassVoxelService, &EditorPanels::blocks, ""},
    {"###Tiles", ENG_TR("engine.editor.panel.tiles"), icons::ClassTilemap2D, &EditorPanels::tiles, ""},
    {"###Water", ENG_TR("engine.editor.panel.water"), WaterIcon, &EditorPanels::water, ""},
};

// Shows a panel, brings it to the front of its node and gives it the keyboard.
//
// **Only that panel** (the owner: the Content icon opened Content AND Run and
// Debug). This used to put back everything a collapse had put away with it --
// the VS Code side bar, where the views shared one column. In an engine's
// layout the panels live in different places, and a button is one panel.
void revealPanel(EditorPanels&, const char* window, bool& open)
{
    open = true;
    selectDockTab(window);
    ImGui::SetWindowFocus(window);
}

// Hides the one panel a button stands for.
void collapseSideBar(EditorPanels& panels, const char* window)
{
    for (const ActivityView& view : ActivityViews) {
        if (std::string_view(view.window) == window)
            panels.*view.visible = false;
    }
}

} // namespace

// **Ctrl+B**: the tree on the left away, or back.
void toggleSideBar(EditorPanels& panels)
{
    if (panels.explorer)
        panels.explorer = false;
    else
        revealPanel(panels, "###Explorer", panels.explorer);
}

void drawActivityBar(EditorPanels& panels, EditorDialogs& dialogs, const IconAtlas* icons)
{

    const float scale = ImGui::GetStyle().FontScaleMain;
    const float width = std::round(44.0f * scale);
    const float glyph = std::round(22.0f * scale);
    const ThemePalette& p = palette();

    ImGui::PushStyleColor(ImGuiCol_WindowBg, themeColor(p.background));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    if (ImGui::BeginViewportSideBar("##activity-bar", ImGui::GetMainViewport(), ImGuiDir_Left, width, flags)) {
        const ImVec2 origin = ImGui::GetWindowPos();
        const float height = ImGui::GetWindowHeight();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddLine(ImVec2(origin.x + width - 1.0f, origin.y), ImVec2(origin.x + width - 1.0f, origin.y + height),
                      ImGui::ColorConvertFloat4ToU32(themeColor(p.border)));

        const core::Color3 active = p.text;
        const core::Color3 idle = p.textMuted;
        const auto button = [&](const char* id, std::string_view icon, bool on, const std::string& tip) {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, width));
            const bool hovered = ImGui::IsItemHovered();
            if (on) {
                draw->AddRectFilled(min, ImVec2(min.x + 2.0f * scale, min.y + width),
                                    ImGui::ColorConvertFloat4ToU32(themeColor(p.accentFill)));
            }
            ImGui::SetCursorScreenPos(ImVec2(min.x + (width - glyph) * 0.5f, min.y + (width - glyph) * 0.5f));
            (void)drawIcon(icons, icon, glyph, on || hovered ? active : idle);
            ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + width));
            ImGui::SetItemTooltip("%s", tip.c_str());
            return pressed;
        };

        // **The mark says the panel EXISTS, not that it is in front** (the
        // owner): a click on a panel that is open anywhere -- behind another
        // tab, unfocused, on another monitor -- closes it as its own x would,
        // and a click on a closed one opens it with the keyboard in it.
        for (const ActivityView& view : ActivityViews) {
            bool& open = panels.*view.visible;
            std::string tip = core::tr(view.title);
            if (view.shortcut[0] != '\0')
                tip += std::string(" (") + view.shortcut + ")";
            if (button(view.window, view.icon, open, tip)) {
                // **Behind another tab, the icon brings it forward**: it
                // closed it, which with a tool that rests while its panel is
                // not the one on screen made the way back to the tool a press
                // that put the panel away, and a second to get it again.
                if (open && !dockTabBehind(view.window))
                    collapseSideBar(panels, view.window);
                else
                    revealPanel(panels, view.window, open);
            }
        }

        // At the foot, as the editor this follows keeps its gear: the settings,
        // and the palette for everything else.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height - width));
        if (button("##manage", icons::ActionSettings, false, core::tr(ENG_TR("engine.editor.activity_bar.manage"))))
            ImGui::OpenPopup("##manage-menu");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 6.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 6.0f * scale));
        if (ImGui::BeginPopup("##manage-menu")) {
            if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.activity_bar.command_palette")), "Ctrl+Shift+P"))
                g_palette.open(CommandPalette::Mode::Commands);
            ImGui::Separator();
            if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.activity_bar.settings")), "Ctrl+,"))
                dialogs.preferences = true;
            if (ImGui::MenuItem(core::tr(ENG_TR("engine.editor.activity_bar.project_settings"))))
                dialogs.projectSettings = true;
            if (ImGui::BeginMenu(core::tr(ENG_TR("engine.editor.activity_bar.color_theme")))) {
                for (const Theme& theme : themes()) {
                    if (ImGui::MenuItem(shownName(theme).c_str(), nullptr, g_appearance.themeId == theme.id)) {
                        g_appearance.themeId = std::string(theme.id);
                        applyAppearance();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    // The keys the editor this follows opens its views with -- from anywhere,
    // the code included (`workbenchKey`).
    // Not while the game has the keyboard (`g_gameHasKeyboard`).
    const auto global = [](ImGuiKeyChord keys) { return !g_gameHasKeyboard && workbenchKey(keys); };
    if (global(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E))
        revealPanel(panels, "###Explorer", panels.explorer);
    if (global(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_A))
        revealPanel(panels, "###Content", panels.content);
    if (global(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_D))
        revealPanel(panels, "###Debug", panels.debug);
}

// --- Saves (ADR 0111) ----------------------------------------------------------
//
// **What the game saved, where a person can see it**: the slots the project's
// Play and `ludwerk dev` wrote, each key in the Properties grid, a text, a
// number or a switch changed in place, and a key, a slot or all of them
// removed. The store is the running world's, so a change here is a change the
// game reads.

// A value, as one line: a table by what is in it.
[[nodiscard]] std::string describeSaveValue(const scene::World& world, const script::SaveValue& value)
{
    if (value.table == nullptr)
        return formatValue(world, value.scalar);
    std::string out = "{";
    bool first = true;
    for (const script::SaveValue& item : value.table->array) {
        if (out.size() > 80)
            break;
        out += first ? "" : ", ";
        out += describeSaveValue(world, item);
        first = false;
    }
    for (const auto& [key, item] : value.table->fields) {
        if (out.size() > 80)
            break;
        out += first ? "" : ", ";
        out += key + " = " + describeSaveValue(world, item);
        first = false;
    }
    return out.size() > 80 ? out + ", ...}" : out + "}";
}

void drawSaves(scene::World& world, const IconAtlas* icons)
{
    if (g_saves == nullptr) {
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.saves.this_run_keeps_no_saves")));
        return;
    }
    static std::string s_selected;
    const std::vector<std::string> slots = g_saves->list();
    const core::f64 version = world.engineState().saveVersion;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", g_saves->options().directory.empty() ? core::tr(ENG_TR("engine.editor.saves.in_memory"))
                                                                   : g_saves->options().directory.string().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(slots.empty());
    if (labeledIconButton(icons, icons::ActionDelete, core::tr(ENG_TR("engine.editor.saves.clear_all")))) {
        for (const std::string& name : slots)
            (void)g_saves->remove(name);
        s_selected.clear();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.saves.remove_every_slot_and_its_tip")));

    const ImGuiTableFlags split =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##saves-split", 2, split, ImGui::GetContentRegionAvail()))
        return;
    ImGui::TableSetupColumn("##slots", ImGuiTableColumnFlags_WidthStretch, 0.3f);
    ImGui::TableSetupColumn("##values", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (ImGui::BeginChild("##save-slots")) {
        if (slots.empty())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.saves.no_slot_saved_yet_a")));
        for (const std::string& name : slots) {
            if (ImGui::Selectable(name.c_str(), name == s_selected))
                s_selected = name;
        }
    }
    ImGui::EndChild();

    ImGui::TableSetColumnIndex(1);
    if (ImGui::BeginChild("##save-values")) {
        script::SaveSlotData* slot =
            s_selected.empty() || std::find(slots.begin(), slots.end(), s_selected) == slots.end()
                ? nullptr
                : g_saves->open(s_selected);
        if (slot == nullptr) {
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.saves.pick_a_slot_to_see")));
        }
        else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(core::tr(slot->recovered ? ENG_TR("engine.editor.saves.slot_row")
                                                            : ENG_TR("engine.editor.saves.slot_row_empty"),
                                            {{"name", slot->name},
                                             {"version", static_cast<double>(slot->version)},
                                             {"count", static_cast<core::i64>(slot->values.size())}})
                                       .c_str());
            ImGui::SameLine();
            if (labeledIconButton(icons, icons::ActionDelete, core::tr(ENG_TR("engine.editor.saves.delete_slot")))) {
                (void)g_saves->remove(slot->name);
                s_selected.clear();
                slot = nullptr;
            }
        }
        if (slot != nullptr && beginSectionGrid("save-grid")) {
            std::optional<std::string> removeKey;
            bool changed = false;
            for (auto& [key, value] : slot->values) {
                ImGui::PushID(key.c_str());
                sectionName(key);
                const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
                const float trash = ImGui::GetFrameHeight();
                // Where the cell starts and how wide it is, so the remove button
                // lands at its right end whatever the widget before it is.
                const float cellStart = ImGui::GetCursorPosX();
                const float cellWidth = ImGui::GetContentRegionAvail().x;
                ImGui::SetNextItemWidth(-(trash + inner));
                if (value.table == nullptr && std::holds_alternative<bool>(value.scalar)) {
                    bool held = std::get<bool>(value.scalar);
                    if (ImGui::Checkbox("##value", &held)) {
                        value.scalar = scene::Value{held};
                        changed = true;
                    }
                }
                else if (value.table == nullptr && std::holds_alternative<core::f64>(value.scalar)) {
                    core::f64 held = std::get<core::f64>(value.scalar);
                    if (dragNumber("##value", ImGuiDataType_Double, &held, 1, 0.1f, "%.6g")) {
                        value.scalar = scene::Value{held};
                        changed = true;
                    }
                }
                else if (value.table == nullptr && std::holds_alternative<std::string>(value.scalar)) {
                    char buffer[512]{};
                    const std::string& text = std::get<std::string>(value.scalar);
                    (void)std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
                    if (ImGui::InputText("##value", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
                        value.scalar = scene::Value{std::string(buffer)};
                        changed = true;
                    }
                }
                else {
                    // Read here, changed by the game: a field of the same width
                    // as the others, so every row's remove sits in one column.
                    std::string shown = describeSaveValue(world, value);
                    ImGui::InputText("##shown", shown.data(), shown.size() + 1, ImGuiInputTextFlags_ReadOnly);
                }
                ImGui::SameLine(0.0f, inner);
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), cellStart + cellWidth - trash));
                if (iconButton(icons, icons::ActionDelete, ImGui::GetFontSize(), "##remove", "x",
                               core::tr(ENG_TR("engine.editor.saves.remove_this_key_tip"))))
                    removeKey = key;
                ImGui::PopID();
            }
            if (removeKey.has_value()) {
                slot->values.erase(*removeKey);
                changed = true;
            }
            if (changed) {
                // Written now: a person who changed a save in a tool expects the
                // file to say so.
                ++slot->generation;
                g_saves->flush(version);
            }
            endSectionGrid();
        }
    }
    ImGui::EndChild();
    ImGui::EndTable();
}

// --- The Welcome page (the owner's queue, Q2) -------------------------------
//
// **What the editor this follows shows when nothing is open**: the name, a
// column of ways to start, the recent work, and the keys worth knowing on the
// first day. A tab beside the Viewport, opened from Help > Welcome; closing it
// is permanent until it is asked for again, because a page that comes back
// every launch is a page people learn to close without reading.
namespace {

// A line of accent-coloured text that acts, as a link does there.
bool welcomeLink(const IconAtlas* icons, std::string_view icon, const char* label, const char* detail = nullptr)
{
    const float glyph = ImGui::GetFontSize();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = glyph + 6.0f + ImGui::CalcTextSize(label).x;
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##link", ImVec2(width, ImGui::GetTextLineHeightWithSpacing()));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(themeColor(palette().accent));
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    paintActionIcon(icons, icon, at, glyph);
    ImGui::PopStyleColor();
    draw->AddText(ImVec2(at.x + glyph + 6.0f, at.y), ink, label);
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float y = at.y + ImGui::GetTextLineHeight();
        draw->AddLine(ImVec2(at.x + glyph + 6.0f, y), ImVec2(at.x + width, y), ink);
    }
    if (detail != nullptr) {
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("%s", detail);
    }
    return pressed;
}

} // namespace

void drawWelcome(Editor& editor, EditorPanels& panels, EditorCommands& commands, EditorDialogs& dialogs,
                 const IconAtlas* icons, ImGuiID centralNode)
{
    if (!panels.welcome)
        return;
    if (centralNode != 0)
        ImGui::SetNextWindowDockID(centralNode, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.welcome")) + "###Welcome").c_str(),
                      &panels.welcome)) {
        ImGui::End();
        return;
    }

    const float scale = ImGui::GetStyle().FontScaleMain;
    const float room = ImGui::GetContentRegionAvail().x;
    const float width = std::min(room, 880.0f * scale);
    const float inset = std::max(0.0f, (room - width) * 0.5f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + inset);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0.0f, 28.0f * scale));

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.4f);
    ImGui::TextUnformatted(std::string(core::kBrandName).c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextDisabled("%s", editor.content().root().parent_path().filename().string().c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 24.0f * scale));

    const bool twoColumns = width > 560.0f * scale;
    const float column = twoColumns ? (width - 40.0f * scale) * 0.5f : width;
    const auto heading = [](const char* text) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
        ImGui::Spacing();
    };

    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(column, 0.0f));
    heading(core::tr(ENG_TR("engine.editor.welcome.start")));
    if (welcomeLink(icons, icons::ContentScene, core::tr(ENG_TR("engine.editor.welcome.new_scene"))))
        runCommand("File: New Scene");
    if (welcomeLink(icons, icons::ActionOpen, core::tr(ENG_TR("engine.editor.welcome.open_file")), "Ctrl+P"))
        openQuickOpen();
    if (welcomeLink(icons, icons::ActionOpen, core::tr(ENG_TR("engine.editor.welcome.open_project"))))
        runCommand("File: Open Project...");
    if (welcomeLink(icons, icons::ActionPlay, core::tr(ENG_TR("engine.editor.welcome.start_the_game")), "F5"))
        runCommand("run.start");
    if (welcomeLink(icons, icons::ActionExport, core::tr(ENG_TR("engine.editor.welcome.export")), "Ctrl+Shift+B"))
        dialogs.exportWindow = true;
    ImGui::Dummy(ImVec2(0.0f, 20.0f * scale));

    // The project's scenes: recent work, the one a project has.
    heading(core::tr(ENG_TR("engine.editor.welcome.scenes")));
    const std::vector<std::string> scenes = editor.content().filesOfKind(ContentKind::Scene);
    if (scenes.empty())
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.welcome.no_scene_yet_new_scene")));
    for (std::size_t index = 0; index < scenes.size() && index < 8; ++index) {
        const std::string& path = scenes[index];
        const std::size_t slash = path.rfind('/');
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
        ImGui::PushID(static_cast<int>(index));
        if (welcomeLink(icons, icons::ContentScene, name.c_str(), folder.c_str()))
            openSceneOrAsk(editor, commands, dialogs, path);
        ImGui::PopID();
    }
    ImGui::EndGroup();

    if (twoColumns)
        ImGui::SameLine(0.0f, 40.0f * scale);
    else
        ImGui::Dummy(ImVec2(0.0f, 20.0f * scale));

    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(column, 0.0f));
    heading(core::tr(ENG_TR("engine.editor.welcome.keys_to_know")));
    // The keys' names are the keys'; what each does is the catalog's.
    const std::pair<core::TextKey, core::TextKey> Keys[] = {
        {ENG_TR("engine.editor.welcome.keys.commands"), ENG_TR("engine.editor.welcome.does.commands")},
        {ENG_TR("engine.editor.welcome.keys.files"), ENG_TR("engine.editor.welcome.does.files")},
        {ENG_TR("engine.editor.welcome.keys.run"), ENG_TR("engine.editor.welcome.does.run")},
        {ENG_TR("engine.editor.welcome.keys.side_bar"), ENG_TR("engine.editor.welcome.does.side_bar")},
        {ENG_TR("engine.editor.welcome.keys.tools"), ENG_TR("engine.editor.welcome.does.tools")},
        {ENG_TR("engine.editor.welcome.keys.frame"), ENG_TR("engine.editor.welcome.does.frame")},
        {ENG_TR("engine.editor.welcome.keys.fly"), ENG_TR("engine.editor.welcome.does.fly")},
        {ENG_TR("engine.editor.welcome.keys.console"), ENG_TR("engine.editor.welcome.does.console")},
    };
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& [keys, what] : Keys) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(themeColor(palette().accent), "%s", core::tr(keys));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", core::tr(what));
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0.0f, 16.0f * scale));
    if (welcomeLink(icons, icons::ActionSearch, core::tr(ENG_TR("engine.editor.welcome.show_all_commands")),
                    "Ctrl+Shift+P"))
        openCommandPalette();
    if (welcomeLink(icons, icons::ActionSettings, core::tr(ENG_TR("engine.editor.welcome.settings")), "Ctrl+,"))
        dialogs.preferences = true;
    ImGui::EndGroup();

    ImGui::EndGroup();
    (void)panels;
    ImGui::End();
}

// **Which tab each dock node shows, and which window has the keyboard**, kept
// across anything that takes every panel away and brings them back -- a launch,
// F3. Panels that come back together all "appear" in one frame, and ImGui then
// gives the focus, and the front of each node, to whichever appeared last: F3
// twice turned Properties into Stats (the owner). What was in front is put
// back for a few frames after.
std::vector<std::pair<ImGuiID, ImGuiID>> g_savedTabs;
std::string g_savedFocus;
int g_restoreTabs = -1;
constexpr int RestoreTabFrames = 12;

void rememberDockTabs()
{
    g_savedTabs.clear();
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    for (int index = 0; index < context.DockContext.Nodes.Data.Size; ++index) {
        const auto* node = static_cast<const ImGuiDockNode*>(context.DockContext.Nodes.Data[index].val_p);
        if (node != nullptr && node->SelectedTabId != 0)
            g_savedTabs.emplace_back(node->ID, node->SelectedTabId);
    }
    const ImGuiWindow* focused = context.NavWindow != nullptr ? context.NavWindow->RootWindow : nullptr;
    g_savedFocus = focused != nullptr && focused->DockNode != nullptr ? focused->Name : "###Explorer";
}

void drawEditorShell(const Frame& frame, scene::World* world, core::InstanceId root, Inspector* inspector,
                     script::ScriptRuntime* runtime, Editor* editor, rhi::TextureHandle viewport, bool& laidOut,
                     EditorCommands& commands, EditorPanels& panels, EditorDialogs& dialogs, IconAtlas* icons,
                     ScriptEditor* scripts, ScriptEditorCommands& scriptCommands, DebugView& debug,
                     audio::AudioSystem* audio, const StreamingHost* streaming, bool furniture,
                     const RenderCounters& counters)
{
    // **First, before a single window is submitted.** A code pane that owns the
    // active id makes every other item in the frame unhoverable, so this is
    // where a click that landed somewhere else takes the caret back -- see
    // `releaseScriptPaneFocus`. Ahead of the early return too: F3 hides the
    // furniture, and a pane still holding the caret behind it would swallow the
    // clicks meant for the world.
    if (scripts != nullptr)
        releaseScriptPaneFocus();

    // **The Play rule's facts for this frame.** Whether the scene is being
    // authored, for the panels drawn with no editor in hand; and the keyboard
    // is nobody's until the viewport, drawn below, says it has it -- a
    // viewport behind a script's tab is not drawn, and must not go on
    // answering that it is focused.
    g_authoring = editor == nullptr || editor->authoring();
    g_gameHasKeyboard = false;
    if (editor != nullptr)
        editor->setKeyboardHolder(false, ImGui::GetIO().WantTextInput);

    // **F3 down: the world and nothing else.** Returning before the dockspace
    // rather than hiding each panel, because a dockspace with no windows in it
    // is still a dockspace and would draw its own background over the picture.
    static bool s_furnitureShown = true;
    if (!furniture) {
        if (s_furnitureShown) {
            rememberDockTabs();
            s_furnitureShown = false;
        }
        if (editor != nullptr)
            drawViewportFullscreen(*editor, viewport, commands);
        return;
    }
    if (!s_furnitureShown) {
        s_furnitureShown = true;
        g_restoreTabs = RestoreTabFrames;
    }

    // Before the dockspace. `DockSpaceOverViewport` measures the work area, and
    // a menu bar declared after it would sit on top of the panels by its own
    // height.
    if (editor != nullptr) {
        g_commandContext = CommandContext{editor,
                                          &commands,
                                          &panels,
                                          &dialogs,
                                          world,
                                          inspector,
                                          editor->stampSession().open() ? editor->stampSession().root : root,
                                          icons};
        drawMenuBar(*editor, panels, commands, dialogs, icons);
        // And the ribbon under it, for the same reason.
        drawRibbonBar(*editor, commands, panels, icons);
        // And the status bar along the bottom.
        drawStatusBar(*editor, inspector, panels, icons);
        // And the activity bar down the left, between the two.
        drawActivityBar(panels, dialogs, icons);
    }

    // **Each node opens on the tab it was left on.** ImGui restores that only
    // when a node's tab bar is made, and selects any tab that turns up in a
    // LATER frame -- and a panel or two is only submitted a few frames in, so
    // every launch after the first opened the bottom panel on Stats. The saved
    // choices are read from the layout file itself -- `ID=` and `Selected=` on
    // each node's line, which is the whole of what ImGui kept -- and put back
    // for the first frames; a click in the first tenth of a second is the
    // price. (ImGui's own parsed copy is a type private to its source file.)
    if (g_restoreTabs < 0) {
        g_restoreTabs = 0;
        g_savedFocus = "###Explorer";
        std::string layout;
        if (const char* file = ImGui::GetIO().IniFilename; file != nullptr && platform::readTextFile(file, layout)) {
            for (std::size_t at = 0; at < layout.size();) {
                const std::size_t end = std::min(layout.find('\n', at), layout.size());
                const std::string_view line(layout.data() + at, end - at);
                at = end + 1;
                if (line.find("Dock") == std::string_view::npos)
                    continue;
                const std::size_t id = line.find(" ID=0x");
                const std::size_t selected = line.find(" Selected=0x");
                if (id == std::string_view::npos || selected == std::string_view::npos)
                    continue;
                const auto hex = [&line](std::size_t from) {
                    return static_cast<ImGuiID>(std::strtoul(std::string(line.substr(from, 8)).c_str(), nullptr, 16));
                };
                g_savedTabs.emplace_back(hex(id + 6), hex(selected + 12));
            }
        }
        if (!g_savedTabs.empty())
            g_restoreTabs = RestoreTabFrames;
    }

    // A transparent central node, so a layout that has not been built yet shows
    // the frame underneath instead of a slab of grey.
    const ImGuiID dockspace =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    // `DockBuilderGetNode` answers null until the dockspace exists, and a saved
    // layout has already put windows into it by the time it does -- so "nobody
    // has arranged this yet" is the node having no split and no window, which is
    // exactly the state a first launch is in.
    bool builtThisFrame = false;
    if (!laidOut || commands.resetLayout) {
        const bool asked = commands.resetLayout;
        commands.resetLayout = false;
        laidOut = true;
        // **The browser opens the way it was left**, which the panel struct
        // cannot answer by itself -- it is rebuilt from nothing every launch.
        // Seeded here rather than at construction because this is the first
        // frame that has an editor in hand.
        if (!asked && editor != nullptr)
            panels.contentView = editor->contentView();
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
        // Asked for, or never arranged. `DockBuilderRemoveNode` throws away an
        // arrangement somebody chose, so it only runs when they said so or when
        // there is nothing to throw away.
        // And once for a layout older than the workbench's arrangement, which moved
        // every panel (`Editor::CurrentLayoutRevision`).
        const bool beforeWorkbench = editor != nullptr && editor->layoutRevision() < Editor::LastRebuiltLayoutRevision;
        if (asked || beforeWorkbench || node == nullptr || (!node->IsSplitNode() && node->Windows.Size == 0)) {
            buildDefaultLayout(dockspace);
            builtThisFrame = true;
            // The browser moved under the world, where a list is a column of
            // names in a wide panel: an arrangement older than that one gets
            // the grid with it.
            if (beforeWorkbench && editor != nullptr) {
                panels.contentView = EditorPanels::ContentView::Tiles;
                editor->setContentView(panels.contentView);
            }
            if (asked) {
                panels = EditorPanels{};
                // Reset Layout means the arrangement, and the browser's layout
                // is part of it -- so the remembered one goes with it rather
                // than coming back on the next launch.
                if (editor != nullptr)
                    editor->setContentView(panels.contentView);
            }
        }
    }

    if (editor != nullptr) {
        if (panels.viewport)
            drawViewport(world, inspector, *editor, viewport, commands, panels, panels.viewport, icons);
        const ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspace);
        drawWelcome(*editor, panels, commands, dialogs, icons, centralNode != nullptr ? centralNode->ID : 0);
        if (panels.content)
            drawContent(*editor, commands, panels, dialogs, icons, world, inspector);
    }

    // **Siblings of the Viewport, in the central node** (ADR 0057). A window per
    // open script rather than a tab bar of our own: the dockspace already turns
    // siblings in one node into a tab strip, and it also lets somebody drag one
    // out to sit BESIDE the world rather than over it -- which is what was asked
    // for and what a hand-rolled tab bar would have refused.
    if (scripts != nullptr) {
        // Where the Viewport IS, which is not always the dockspace's central
        // node: somebody may have moved the world, and a script belongs with it.
        const ImGuiWindow* world3d = ImGui::FindWindowByName("###Viewport");
        const ImGuiDockNode* central = world3d != nullptr && world3d->DockNode != nullptr
                                           ? world3d->DockNode
                                           : ImGui::DockBuilderGetCentralNode(dockspace);
        const ScriptActionButton scriptButton = [icons](std::string_view id, const char* label, bool compact) {
            if (compact)
                return iconButton(icons, id, ImGui::GetFontSize(), label, label, label);
            return labeledIconButton(icons, id, label);
        };
        drawScriptEditor(*scripts, central != nullptr ? static_cast<core::u32>(central->ID) : 0u, debug, world, root,
                         scriptCommands, scriptButton);
        if (panels.debug)
            drawDebugPanel(*scripts, debug, scriptCommands, panels.debug, scriptButton, [&] {
                if (editor != nullptr)
                    drawRunHeader(*editor, commands, icons);
            });
    }

    // **While a stamp is open the tree is the STAMP's**, root row and all: no
    // services, no scene, nothing but what is in the file. That is the whole of
    // "a separate environment" as far as the Explorer is concerned, and the
    // viewport already shows the same thing because opening cleared the scene
    // out of the world.
    //
    // Out here rather than inside the Explorer block because the Properties
    // panel needs the same answer: a reference picker offering what is in the
    // SCENE while somebody is editing a stamp would be offering instances that
    // are not there.
    const bool editingStamp = editor != nullptr && editor->stampSession().open();
    const core::InstanceId treeRoot = editingStamp ? editor->stampSession().root : root;

    if (panels.explorer) {
        if (ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.explorer")) + "###Explorer").c_str(),
                         &panels.explorer)) {
            if (world != nullptr && inspector != nullptr) {
                // **Two trees, and the tabs are what says they are two.** The
                // scene is what this world holds; `Content` is what the PROJECT
                // holds, global to every scene in it (ADR 0052). Instance
                // inside instance in both, the same verbs in both -- and
                // nothing in `Content` runs, which is the one sentence that
                // explains the difference.
                drawExplorer(*world, treeRoot, *inspector, &commands, &dialogs, icons, panels.showGenerated,
                             editingStamp, editor != nullptr && editor->hasClipboard());
            }
        }
        ImGui::End();
    }

    // Where the right-hand column is, so a panel that has never been placed can
    // be put there rather than left floating in the middle of the screen.
    //
    // **Read off the live window rather than rebuilt.** `buildDefaultLayout`
    // only runs when there is no saved layout, and a person who has one has
    // arranged it -- so a new panel finding its home must not cost them that
    // arrangement. This is the node Properties is actually in, this launch.
    ImGuiID rightColumn = 0;
    for (const char* name : {"Properties", "Stats", "Terrain", "Blocks"}) {
        if (const ImGuiWindow* window = ImGui::FindWindowByName(name); window != nullptr && window->DockId != 0) {
            rightColumn = window->DockId;
            break;
        }
    }

    // What was selected last frame, so "the selection changed to a terrain" is a
    // transition rather than a state -- see the panel below.
    static core::InstanceId lastSelection;

    if (panels.properties) {
        if (ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.properties")) + "###Properties").c_str(),
                         &panels.properties)) {
            if (ImGui::GetWindowDockID() != 0)
                rightColumn = ImGui::GetWindowDockID();
            if (world != nullptr && inspector != nullptr) {
                drawProperties(*world, treeRoot, *inspector, editor != nullptr ? &editor->content() : nullptr, icons,
                               audio, editor != nullptr ? &commands : nullptr, editor);
                // **The write log is a DEBUG panel and the editor is not one.**
                // It stays in the F3 overlay, where showing the machinery is the
                // whole point; here it was a collapsing header that appeared
                // after every edit, pushed the grid down and went away again --
                // a panel reflowing under somebody's pointer as they drag a
                // number.
                //
                // What it was carrying that nothing else was: a write the world
                // REFUSED. That is now said in the editor's own voice, so the
                // one useful line survives the panel it was buried in.
                reportRefusedWrites(*world, *inspector, editor);
            }
        }
        ImGui::End();
    }
    if (editor != nullptr)
        drawMaterialPanel(*editor, icons, commands);

    // Draws with no VM for the same reason it does in the overlay: the LOG half
    // is what somebody wants when the VM failed to boot.
    if (panels.console) {
        if (ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.console")) + "###Console").c_str(),
                         &panels.console))
            drawConsole(runtime, &scriptCommands, icons, true);
        ImGui::End();
    }

    // --- The Terrain panel (F1) ------------------------------------------
    //
    // `drawTerrainPanel` above argues the shape. What is here is the plumbing:
    // where it docks the first time, and the three pointers it needs.
    // **Selecting the terrain opens its tools.** The editors this one is measured
    // against all do some version of this -- clicking the thing brings up what
    // edits it -- and it is the difference between a panel somebody has to know
    // about and one that introduces itself.
    //
    // Only on the frame the selection CHANGES to it, so closing the panel while
    // the terrain is still selected leaves it closed. A panel that reopened
    // every frame would be one nobody could dismiss.
    if (editor != nullptr && world != nullptr && inspector != nullptr) {
        const core::InstanceId chosen = inspector->selection();
        if (chosen != lastSelection) {
            lastSelection = chosen;
            if (chosen.valid() && world->terrains().find(chosen) != nullptr)
                panels.terrain = true;
            if (chosen.valid() && world->voxels().find(chosen) != nullptr)
                panels.blocks = true;
            if (chosen.valid() && world->tilemaps2d().find(chosen) != nullptr)
                panels.tiles = true;
            if (chosen.valid() && world->waters().find(chosen) != nullptr)
                panels.water = true;
        }
    }

    // **A tool is in hand while its panel is the one on screen** (the owner,
    // 2026-10-01; `Editor::tool`): the Terrain brush, the Blocks, Tiles and
    // Water tools, by one rule. A panel behind another tab, or closed, puts
    // its tool at rest -- no ring, no chip, no handles, and a click selects --
    // and coming back to it puts the tool in hand again. "Open, not in front"
    // had been the rule, so that Properties coming forward did not kill a
    // brush; nothing is killed now, only rested, and the brush that followed
    // the pointer over every other panel's work is what the rule was costing.
    bool terrainShown = false;
    if (panels.terrain) {
        // `FirstUseEver`, so this decides only where a panel with no remembered
        // place goes. Somebody who has moved it keeps it where they put it, and
        // an existing `layout.ini` is not rewritten for a window it predates.
        if (rightColumn != 0)
            ImGui::SetNextWindowDockID(rightColumn, ImGuiCond_FirstUseEver);
        terrainShown = ImGui::Begin(
            (tabIconPad() + core::tr(ENG_TR("engine.editor.panel.terrain")) + "###Terrain").c_str(), &panels.terrain);
        if (terrainShown) {
            // **The shell holds pointers, and every one of the three may be
            // null**: this same function draws the F3 overlay, which has a frame
            // and counters and no editor at all.
            if (editor == nullptr || world == nullptr || inspector == nullptr) {
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_shell.no_world")));
                ImGui::End();
                goto terrainPanelDone;
            }
            // **Greyed while the game runs** (the Play rule): Create, Clear,
            // a new layer and the rest are calls, not commands, so the drain
            // cannot refuse them -- the panel does, whole.
            ImGui::BeginDisabled(!editor->authoring());
            drawTerrainPanel(*editor, *world, treeRoot, *inspector, icons, commands);
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
terrainPanelDone:;
    if (editor != nullptr)
        editor->setTerrainPanelShown(terrainShown && panels.terrain);
    bool blocksShown = false;
    if (panels.blocks) {
        if (rightColumn != 0)
            ImGui::SetNextWindowDockID(rightColumn, ImGuiCond_FirstUseEver);
        blocksShown = ImGui::Begin(
            (tabIconPad() + core::tr(ENG_TR("engine.editor.panel.blocks")) + "###Blocks").c_str(), &panels.blocks);
        if (blocksShown) {
            if (editor == nullptr || world == nullptr || inspector == nullptr)
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_shell.no_world")));
            else {
                ImGui::BeginDisabled(!editor->authoring());
                drawBlocksPanel(*editor, *world, *inspector, icons);
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }
    if (editor != nullptr)
        editor->setBlocksPanelShown(blocksShown && panels.blocks);
    // **The Tiles tool follows its panel**, as the brush does.
    bool tilesShown = false;
    if (panels.tiles) {
        if (rightColumn != 0)
            ImGui::SetNextWindowDockID(rightColumn, ImGuiCond_FirstUseEver);
        tilesShown = ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.tiles")) + "###Tiles").c_str(),
                                  &panels.tiles);
        if (tilesShown) {
            if (editor == nullptr || world == nullptr || inspector == nullptr)
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_shell.no_world")));
            else {
                ImGui::BeginDisabled(!editor->authoring());
                drawTilesPanel(*editor, *world, treeRoot, *inspector, icons);
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }
    if (editor != nullptr)
        editor->setTilesPanelShown(tilesShown && panels.tiles);
    // **The Water tool follows its panel**, as the others do.
    bool waterShown = false;
    if (panels.water) {
        if (rightColumn != 0)
            ImGui::SetNextWindowDockID(rightColumn, ImGuiCond_FirstUseEver);
        waterShown = ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.water")) + "###Water").c_str(),
                                  &panels.water);
        if (waterShown) {
            if (editor == nullptr || world == nullptr || inspector == nullptr)
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.editor_shell.no_world")));
            else {
                ImGui::BeginDisabled(!editor->authoring());
                drawWaterPanel(*editor, *world, *inspector, icons, treeRoot);
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }
    if (editor != nullptr)
        editor->setWaterPanelShown(waterShown && panels.water);
    if (panels.stats) {
        if (ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.stats")) + "###Stats").c_str(),
                         &panels.stats)) {
            drawStats(frame, counters);
            if (runtime != nullptr)
                drawMemory(*runtime);
        }
        ImGui::End();
    }
    // Under the world, beside the Console: a list and a table want width.
    if (panels.saves && world != nullptr) {
        if (const ImGuiWindow* console = ImGui::FindWindowByName("###Console");
            console != nullptr && console->DockNode != nullptr)
            ImGui::SetNextWindowDockID(console->DockNode->ID, ImGuiCond_FirstUseEver);
        if (ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.saves")) + "###Saves").c_str(),
                         &panels.saves))
            drawSaves(*world, icons);
        ImGui::End();
    }
    if (panels.streaming) {
        if (rightColumn != 0)
            ImGui::SetNextWindowDockID(rightColumn, ImGuiCond_FirstUseEver);
        if (ImGui::Begin((tabIconPad() + core::tr(ENG_TR("engine.editor.panel.streaming")) + "###Streaming").c_str(),
                         &panels.streaming)) {
            ImGui::Checkbox(core::tr(ENG_TR("engine.editor.editor_shell.show_chunk_grid")), &panels.showChunkGrid);
            ImGui::Checkbox(core::tr(ENG_TR("engine.editor.editor_shell.show_streamed_objects_in_explorer")),
                            &panels.showGenerated);
            ImGui::Separator();
            if (streaming != nullptr && streaming->active())
                drawStreaming(*streaming);
            else
                ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.editor_shell.streaming_is_inactive_in_this")));
        }
        ImGui::End();
    }
    if (panels.viewportSettings) {
        if (rightColumn != 0)
            ImGui::SetNextWindowDockID(rightColumn, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(
                (tabIconPad() + core::tr(ENG_TR("engine.editor.panel.viewport_settings")) + "###Viewport Settings")
                    .c_str(),
                &panels.viewportSettings)) {
            drawViewportSettings(editor, panels, icons, streaming != nullptr && streaming->active());
        }
        ImGui::End();
    }

    // **Escape lets go of the selection, from anywhere.** Deselecting is a thing
    // a person does constantly and it needs a key that works wherever they are
    // looking -- the explorer, the viewport, the content browser.
    //
    // Only when nothing else has a claim on it, and the order matters: a modal
    // is closed by Escape and a text field cancels its edit with it, and taking
    // the key from either would make the shell's own dialogs unclosable. So it
    // is asked for last, after everything that could have wanted it.
    const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // **And only when nothing NEARER has already taken it this frame.**
    // `popupOpen` cannot answer that on its own: a popup that closed itself with
    // Escape is already off the stack when this runs, so the same press would
    // arrive here as though there had never been a menu. See the add-child popup
    // in `drawExplorer`.
    const bool escapeTaken = std::exchange(g_escapeTaken, false);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() && !popupOpen && !escapeTaken) {
        // **In the game's view Escape is the game's** (the Play rule): it
        // closes the game's own menu, and it used to stop the game as well --
        // one key doing two things, and the second one threw the session away.
        // Stop is Shift+F5 and the transport. A game that holds the pointer
        // and never gives it back (D069) is left with Shift+P, which hands the
        // pointer to the editor, and Shift+F5: both are keys, and both arrive.
        if (editor != nullptr && editor->viewportIsGames()) {
        }
        else if (editor != nullptr && world != nullptr && inspector != nullptr &&
                 editor->tool() == Editor::Tool::Water && Editor::waterInHand(*world, *inspector).valid()) {
            // **The river first, then the tool**: Escape with a river in hand
            // puts the river down, so the next click starts another; a second
            // Escape puts the tool down.
            editor->finishRiver(*inspector);
        }
        else if (editor != nullptr && editor->authoring() && editor->heldTool() != Editor::Tool::Select) {
            // **Out of a brush before letting go of anything.** Q did this
            // until it became a fly key, and a brush somebody cannot put down
            // with the key they reach for first is a brush they are stuck in.
            // The tool held, in hand or at rest: Escape puts it down for good.
            editor->setTool(Editor::Tool::Select);
        }
        else if (editor != nullptr && editor->drilled().valid()) {
            // **One level out before letting go** (S5.3). Somebody who has
            // double-clicked into a model and wants out again reaches for
            // Escape; deselecting instead would leave them inside it with
            // nothing selected, which is the state that looks like the drill
            // being permanent.
            editor->setDrilled(core::InstanceId{});
        }
        else
            commands.clearSelection = true;
    }

    // **A brush and a selection do not share the viewport.** Picking up a
    // terrain or block brush lets go of the selection, so a click is a stroke
    // and never also a handle; and something newly selected -- a row in the
    // Explorer, a part just inserted -- puts the brush down, because the person
    // has just said what they want to work on and it is not the ground. The
    // Tiles tool is left out of both: it paints the tilemap that IS selected.
    if (editor != nullptr && inspector != nullptr) {
        const auto isBrush = [](Editor::Tool tool) {
            return tool == Editor::Tool::Sculpt || tool == Editor::Tool::Paint || tool == Editor::Tool::Blocks ||
                   tool == Editor::Tool::Foliage;
        };
        const Editor::Tool tool = editor->tool();
        const core::InstanceId primary = inspector->selection();
        const core::usize count = inspector->selectionCount();
        // **The ground is what the terrain brushes work on**: selecting the
        // terrain or something growing on it -- which making ground and adding
        // a foliage layer both do -- is not a new subject, and put the brush
        // down in the middle of the work that made it.
        const auto onTheGround = [&](core::InstanceId id) {
            if (world == nullptr || !id.valid() || tool == Editor::Tool::Blocks)
                return false;
            for (core::InstanceId at = id; at.valid(); at = world->parentOf(at)) {
                if (world->terrains().find(at) != nullptr)
                    return true;
            }
            return false;
        };
        if (isBrush(tool) && !isBrush(g_lastTool)) {
            if (count > 0 && !(count == 1 && onTheGround(primary)))
                commands.clearSelection = true;
        }
        else if (isBrush(tool) && count > 0 && (primary != g_lastSelection || count != g_lastSelectionCount) &&
                 !(count == 1 && onTheGround(primary))) {
            editor->setTool(Editor::Tool::Select);
            // And what was selected is shown, in front of the brush's panel.
            selectDockTab("Properties");
        }
        g_lastTool = editor->tool();
        g_lastSelection = primary;
        g_lastSelectionCount = count;
    }

    // **Shift+P flies free while the game runs**: the view leaves the game's
    // camera and the fly keys drive the editor's, with the simulation untouched
    // -- the eye on the transport, on the key people already use for it. The
    // game sees the key too, the same arrangement Escape has.
    if (editor != nullptr && editor->inPlayMode() && ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl &&
        !ImGui::GetIO().KeyAlt && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        editor->setCameraDetached(!editor->cameraDetached());
    }

    // **Ctrl+1 to Ctrl+4 for select, move, scale and rotate, and Ctrl+L for
    // the space**, which is the set most people arriving here already have in
    // their hands -- and the reason single letters are not used: W, A, S, D, Q
    // and E fly the camera without a held button, so a tool key among them
    // would move the view and change the tool with one press.
    //
    // Alt suspends the grid for as long as it is held. That way round because
    // the number somebody wants is far more often a round one, so the modifier
    // is for the exception rather than for the rule.
    const bool ctrlOnly = ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyAlt;
    // Held Ctrl turns a terrain brush round and held Shift smooths, for the
    // stroke that starts while they are down (`Editor::setBrushModifiers`).
    if (editor != nullptr) {
        const bool typing = ImGui::GetIO().WantTextInput;
        editor->setBrushModifiers(ImGui::GetIO().KeyCtrl && !typing, ImGui::GetIO().KeyShift && !typing);
        // Alt with Paint in hand is the eyedropper (B4).
        editor->setBrushPicking(ImGui::GetIO().KeyAlt && !typing);
    }
    // **Not while the game plays** (terrain audit E6): a game's own 1 to 6, T
    // and B changed the editor's brush and opened its panels behind it.
    if (editor != nullptr && !ImGui::IsAnyItemActive() && !popupOpen && !editor->lookInput().active &&
        !editor->inPlayMode()) {
        if (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            editor->setTool(Editor::Tool::Select);
            editor->setHandlesShown(false);
        }
        if (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_2, false)) {
            editor->setTool(Editor::Tool::Select);
            editor->setGizmoMode(GizmoMode::Translate);
        }
        if (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_3, false)) {
            editor->setTool(Editor::Tool::Select);
            editor->setGizmoMode(GizmoMode::Scale);
        }
        if (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_4, false)) {
            editor->setTool(Editor::Tool::Select);
            editor->setGizmoMode(GizmoMode::Rotate);
        }
        if (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_L, false))
            editor->setGizmoLocal(!editor->gizmoLocal());

        if (editor->hasTerrain() && !ImGui::GetIO().KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
                editor->setTool(Editor::Tool::Sculpt);
                panels.terrain = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                editor->setTool(Editor::Tool::Paint);
                panels.terrain = true;
            }
        }
        if (editor->hasVoxels() && !ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
            editor->setTool(Editor::Tool::Blocks);
            panels.blocks = true;
        }

        // **The terrain brush's keys**, the set the reference editors share:
        // [ and ] size it, with Shift its strength, and 1 to 6 pick a sculpt
        // tool in the order the panel shows them. Not while text is typed.
        const Editor::Tool held = editor->tool();
        const bool terrainBrush =
            held == Editor::Tool::Sculpt || held == Editor::Tool::Paint || held == Editor::Tool::Foliage;
        if (terrainBrush && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt) {
            const Editor::Brush& brush = editor->brush();
            const bool shift = ImGui::GetIO().KeyShift;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) {
                if (shift)
                    editor->setBrushStrength(brush.strength - 0.05f);
                else
                    editor->setBrushRadius(brush.radius / 1.2f);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) {
                if (shift)
                    editor->setBrushStrength(brush.strength + 0.05f);
                else
                    editor->setBrushRadius(brush.radius * 1.2f);
            }
            if (held == Editor::Tool::Sculpt && !shift) {
                constexpr std::array<std::pair<ImGuiKey, Editor::BrushOp>, 6> Keys{{
                    {ImGuiKey_1, Editor::BrushOp::Grow},
                    {ImGuiKey_2, Editor::BrushOp::Erode},
                    {ImGuiKey_3, Editor::BrushOp::Smooth},
                    {ImGuiKey_4, Editor::BrushOp::Flatten},
                    {ImGuiKey_5, Editor::BrushOp::Add},
                    {ImGuiKey_6, Editor::BrushOp::Subtract},
                }};
                for (const auto& [key, op] : Keys) {
                    if (ImGui::IsKeyPressed(key, false))
                        editor->setBrushOp(op);
                }
            }
        }

        // Q out of the Tiles tool, whatever else the world has.
        if (editor->tool() == Editor::Tool::Tiles && ImGui::IsKeyPressed(ImGuiKey_Q, false))
            editor->setTool(Editor::Tool::Select);
        // Enter ends the river being drawn, as it ends a path in every tool
        // that draws one.
        if (editor->tool() == Editor::Tool::Water && inspector != nullptr && !ImGui::GetIO().WantTextInput &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            editor->finishRiver(*inspector);

        // **F frames the selection**, which is the one camera shortcut every
        // editor in this shape shares -- Unity, Unreal, Godot and Blender all
        // put it here, so it is what somebody's hands already reach for.
        //
        // Nothing happens with nothing selected, and nothing happens for a
        // selection with no extent: a `Folder` has a position and no size, and
        // "showing" it would move the view somewhere arbitrary.
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && world != nullptr && inspector != nullptr) {
            core::DVec3 centre;
            core::f64 radius = 0.0;
            if (selectionBounds(*world, inspector->selectionSet(), centre, radius))
                editor->focusCamera(centre, radius);
        }
    }
    if (editor != nullptr)
        editor->setSnapSuspended(ImGui::GetIO().KeyAlt);

    // Ctrl+Z and Ctrl+Y, under the same rule Escape is: not while a field has
    // the keyboard, because Ctrl+Z inside a text box is the box's own undo and
    // taking it would make typing a name unrecoverable.
    // Nor while the game has the keyboard: Ctrl+Z there is the game's.
    if (!ImGui::IsAnyItemActive() && !popupOpen && ImGui::GetIO().KeyCtrl &&
        !(editor != nullptr && editor->gameHasKeyboard())) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            // Ctrl+Shift+Z is redo everywhere except Windows, and on Windows it
            // is redo as well as Ctrl+Y -- so both work and nobody has to learn
            // which half of their habits this editor kept.
            if (ImGui::GetIO().KeyShift)
                commands.redo = true;
            else
                commands.undo = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
            commands.redo = true;
        if (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_B, false) && editor != nullptr)
            openExportWindow(*editor);
        // **Ctrl+S saves what is open**, which is a stamp while one is and the
        // scene otherwise. One key, because "save" is one intention and the
        // person pressing it is not thinking about which document it reaches.
        // **Ctrl+Shift+S is Save As, and only that.** The shift went unread,
        // so it overwrote the open scene and never asked for a name -- the
        // opposite of what the menu beside it promises.
        if (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (editor == nullptr || !editor->stampSession().open())
                commands.wantSaveAs = true;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (editor != nullptr && editor->stampSession().open())
                commands.saveStamp = true;
            else if (editor != nullptr && !editor->openScenePath().empty())
                commands.save = true;
            else
                commands.wantSaveAs = true;
        }
    }

    // --- Delete, F2 and Ctrl+D ---------------------------------------------
    //
    // **The three every editor has**, on the keys every editor puts them on, so
    // that hands already know them. Under the same guard as the rest: not while
    // a field has the keyboard, because Delete in a text box is a character and
    // F2 in one is nothing.
    //
    // **And not while playing.** A running world is one `stop` is about to put
    // back, so an edit made in it is work about to be thrown away without a
    // word -- which is worse than a key that does nothing.
    const bool authoring = editor != nullptr && world != nullptr && inspector != nullptr && !ImGui::IsAnyItemActive() &&
                           !popupOpen && !editor->inPlayMode();

    // **Paste needs no selection**, because pasting into an empty world is
    // exactly what somebody does after copying out of another one. Everything
    // else acts ON something and is guarded below.
    if (authoring && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        // Shift is the difference between beside and inside, which is the one
        // thing about a paste anybody has to remember.
        if (ImGui::GetIO().KeyShift)
            commands.pasteInto = true;
        else
            commands.paste = true;
    }

    if (authoring && inspector->selectionCount() > 0) {
        const bool engineOwned = Editor::isEngineOwned(*world, inspector->selection(), root);

        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !engineOwned)
            commands.deleteSelection = true;

        if (ImGui::GetIO().KeyCtrl && !engineOwned) {
            // Not with Shift: Ctrl+Shift+D shows Run and Debug.
            if (!ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_D, false))
                commands.duplicateSelection = true;

            // **Ctrl+G and Ctrl+Shift+G**, which is the pair every editor uses
            // and therefore the pair a hand already knows. Shift for the
            // opposite of a verb is the same rule ctrl-Z and ctrl-shift-Z
            // follow one row up.
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
                if (ImGui::GetIO().KeyShift)
                    commands.ungroupSelection = true;
                else if (ImGui::GetIO().KeyAlt)
                    commands.groupAsFolder = true;
                else
                    commands.groupSelection = true;
            }
            // Ctrl+U as well, which is the ungroup many hands already know.
            if (ImGui::IsKeyPressed(ImGuiKey_U, false))
                commands.ungroupSelection = true;
            if (ImGui::IsKeyPressed(ImGuiKey_C, false))
                commands.copySelection = true;
            if (ImGui::IsKeyPressed(ImGuiKey_X, false))
                commands.cutSelection = true;
        }

        // F2 opens the box on the PRIMARY, because renaming four things to one
        // name is not a thing anybody means -- which is the same reason the
        // menu item beside it is singular.
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && !engineOwned) {
            dialogs.renameTarget = inspector->selection();
            dialogs.renameContentPath.clear();
            dialogs.renameSeed = std::string(world->atoms().text(world->name(inspector->selection())));
            dialogs.renameInstance = true;
        }
    }

    if (commands.wantSaveAs) {
        commands.wantSaveAs = false;
        dialogs.saveAs = true;
    }
    if (editor != nullptr)
        drawEditorDialogs(*editor, commands, dialogs, icons);

    // Last, so the palette is over every panel and dialog it can open.
    if (editor != nullptr) {
        handleWorkbenchKeys(*editor, commands, panels, dialogs, debug);
        static bool s_filesListed = false;
        const bool listingFiles = g_palette.isOpen() && g_palette.mode() == CommandPalette::Mode::Files;
        if (listingFiles && !s_filesListed)
            buildPaletteFiles(*editor, commands, dialogs, world, treeRoot, icons);
        s_filesListed = listingFiles;
        if (g_palette.isOpen()) {
            g_palette.draw(currentCommands(), g_paletteFiles,
                           [icons](std::string_view id, float size) { (void)drawIcon(icons, id, size); });
        }
    }

    // After every panel has been declared, because a window ImGui has not seen
    // this frame has no dock node to select a tab in.
    //
    // **On the frame the layout was built, and once for a layout that predates
    // this working.** The first is the default and needs no explanation. The
    // second does: the tab selection above never took effect, so every project
    // arranged before now has `stats` written into its `layout.ini` -- not
    // because anybody chose it but because it was docked last. Reset Layout
    // would fix it and would also throw away the arrangement somebody built,
    // which is a bad trade for a tab. So the revision is bumped once, the tab is
    // put where it belongs, and every panel size and split stays exactly where
    // it was. A person who chooses `stats` afterwards keeps it, because the
    // revision has already moved and this never runs again.
    if (g_restoreTabs > 0) {
        // **And the keyboard back where it was, once** -- the tree, at launch.
        // A window that appears takes the focus, the last one to appear keeps
        // it, and a node always shows the tab of the focused window -- so the
        // bottom panel went on opening on Stats, the last panel drawn, whatever
        // tab had been left in front.
        if (g_restoreTabs == RestoreTabFrames && !g_savedFocus.empty())
            ImGui::SetWindowFocus(g_savedFocus.c_str());
        --g_restoreTabs;
        for (const auto& [nodeId, tabId] : g_savedTabs) {
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(nodeId);
            if (node != nullptr && node->TabBar != nullptr &&
                ImGui::TabBarFindTabByID(node->TabBar, tabId) != nullptr) {
                node->SelectedTabId = tabId;
                node->TabBar->NextSelectedTabId = tabId;
            }
        }
        if (builtThisFrame)
            g_restoreTabs = 0;
    }

    const bool migrating = editor != nullptr && editor->layoutRevision() < Editor::CurrentLayoutRevision;
    // Forgotten where they floated, so they open beside the Viewport the next
    // time -- and are then remembered wherever somebody moves them.
    if (migrating) {
        ImGui::ClearWindowSettings("###Material");
        ImGui::ClearWindowSettings("###Export");
    }
    if (builtThisFrame || migrating) {
        // Each node's tabs in the order an engine keeps them: under the world
        // the files, what the game said, then the debugger; on the right the
        // inspector, then the numbers. A tab bar sorts the tabs it adds by
        // this, and the layout file keeps it; without it the order is
        // whichever panel was drawn first.
        short order = 0;
        for (const char* name : {"###Content", "###Console", "###Debug", "###Streaming", "###Properties", "###Stats"}) {
            if (ImGuiWindow* window = ImGui::FindWindowByName(name); window != nullptr)
                window->DockOrder = order++;
        }
        selectDockTab("Properties");
        selectDockTab("Content");
        selectDockTab("Explorer");
        // Keyboard focus goes to the tree rather than to the grid: it belongs
        // to the panel somebody is about to move around in.
        ImGui::SetWindowFocus("Explorer");
        if (editor != nullptr)
            editor->setLayoutRevision(Editor::CurrentLayoutRevision);
    }

    // **Play brings the viewport to the front** (the owner's report): a script
    // tab docked over it, or the keyboard in another panel, left somebody
    // pressing play and watching code while the game ran behind it. Once, on
    // the frame play begins, and only when there is a viewport to show.
    //
    // **And Stop gives it back to the script that had it** (the owner: "when I
    // stop, the focus should go back to the script I was working on"). Which
    // one is remembered by its instance and world rather than by tab index: a
    // tab can close while the game runs.
    static bool s_wasPlaying = false;
    static std::optional<std::pair<core::InstanceId, ScriptOrigin>> s_scriptBeforePlay;
    const bool playing = editor != nullptr && editor->inPlayMode();
    if (playing && !s_wasPlaying) {
        s_scriptBeforePlay.reset();
        const ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow;
        if (scripts != nullptr && focused != nullptr &&
            std::string_view(focused->Name).find("###script-") != std::string_view::npos) {
            if (const OpenScript* tab = scripts->active(); tab != nullptr)
                s_scriptBeforePlay = std::pair{tab->instance, tab->origin};
        }
        if (panels.viewport)
            ImGui::SetWindowFocus("Viewport###Viewport");
    }
    if (!playing && s_wasPlaying && s_scriptBeforePlay.has_value() && scripts != nullptr) {
        if (const std::optional<std::size_t> index =
                scripts->indexOf(s_scriptBeforePlay->first, s_scriptBeforePlay->second);
            index.has_value())
            scripts->requestFocus(*index);
        s_scriptBeforePlay.reset();
    }
    s_wasPlaying = playing;

    // **Last, because it paints over a strip that has already been laid out.**
    // Every window in the central node has been submitted by now, so the tab bar
    // knows where each tab is; before this point it does not.
    drawTabIcons(dockspace, icons, world, scripts);
}

// --- the streaming map ------------------------------------------------------
//
// **The panel `api-design.md` has promised since M7 and nothing drew.**
// `StreamingManager::view` was written "for the overlay the deliverable owes"
// and had no caller in the tree until this; the manual page told people to open
// a panel that did not exist. It exists now, and E5's gate is a picture of it.
//
// A MAP rather than a table, because the question a person has about streaming
// is spatial: which cells are here, which are on their way, and how far out the
// ring reaches. A table of chunk ids answers none of that at a glance.

[[nodiscard]] ImU32 chunkStateColor(asset::ChunkState state)
{
    // Through the palette, so the map is legible in both themes -- the colours
    // it carried were picked against a dark ground and three of the five were
    // invisible on a light one.
    const ThemePalette& p = palette();
    switch (state) {
    case asset::ChunkState::Resident:
        return ImGui::ColorConvertFloat4ToU32(ImVec4(p.success.r, p.success.g, p.success.b, 0.90f));
    case asset::ChunkState::Decoded:
        return ImGui::ColorConvertFloat4ToU32(ImVec4(p.warning.r, p.warning.g, p.warning.b, 0.90f));
    case asset::ChunkState::Loading:
        return ImGui::ColorConvertFloat4ToU32(ImVec4(p.accent.r, p.accent.g, p.accent.b, 0.90f));
    case asset::ChunkState::Failed:
        return ImGui::ColorConvertFloat4ToU32(ImVec4(p.danger.r, p.danger.g, p.danger.b, 0.90f));
    case asset::ChunkState::Unloaded:
        break;
    }
    return ImGui::ColorConvertFloat4ToU32(ImVec4(p.border.r, p.border.g, p.border.b, 0.70f));
}

void drawStreaming(const StreamingHost& streaming)
{
    const asset::StreamingStats& stats = streaming.stats();
    ImGui::TextWrapped(
        "%s", core::tr(ENG_TR("engine.editor.streaming.counts"), {{"resident", static_cast<core::i64>(stats.resident)},
                                                                  {"loading", static_cast<core::i64>(stats.loading)},
                                                                  {"decoded", static_cast<core::i64>(stats.decoded)},
                                                                  {"failed", static_cast<core::i64>(stats.failed)}})
                  .c_str());
    ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.streaming.memory"),
                                      {{"mib", fixed(static_cast<double>(stats.bytesResident) / 1048576.0, 2)},
                                       {"loaded", static_cast<core::i64>(stats.chunksLoaded)},
                                       {"evicted", static_cast<core::i64>(stats.chunksEvicted)}})
                                 .c_str());
    ImGui::TextWrapped(
        "%s", core::tr(ENG_TR("engine.editor.streaming.pump"), {{"worst", fixed(stats.worstTickMs, 2)},
                                                                {"last", fixed(streaming.lastPumpMilliseconds(), 2)},
                                                                {"count", static_cast<core::i64>(streaming.rebases())}})
                  .c_str());

    const std::vector<asset::StreamingManager::ChunkView> cells = streaming.view();
    if (cells.empty())
        return;

    // One map per size class, because that is what a layer IS (ADR 0053) and
    // because two classes drawn on one grid would put a pebble's cell on top of
    // a hillside's. The gate's own item -- "a large object stays resident at a
    // distance that has already evicted a small one" -- is a thing you read off
    // these two pictures side by side.
    for (core::i32 layer = 0; layer < asset::ChunkLayerCount; ++layer) {
        core::i32 minX = 0;
        core::i32 maxX = 0;
        core::i32 minZ = 0;
        core::i32 maxZ = 0;
        core::u32 present = 0;
        core::u32 resident = 0;
        for (const asset::StreamingManager::ChunkView& cell : cells) {
            if (cell.id.layer != layer)
                continue;
            if (present == 0) {
                minX = maxX = cell.id.x;
                minZ = maxZ = cell.id.z;
            }
            minX = std::min(minX, cell.id.x);
            maxX = std::max(maxX, cell.id.x);
            minZ = std::min(minZ, cell.id.z);
            maxZ = std::max(maxZ, cell.id.z);
            ++present;
            if (cell.state == asset::ChunkState::Resident)
                ++resident;
        }
        if (present == 0)
            continue;

        const core::TextKey layerNames[] = {ENG_TR("engine.editor.streaming.layer.detail"),
                                            ENG_TR("engine.editor.streaming.layer.structures"),
                                            ENG_TR("engine.editor.streaming.layer.terrain")};
        ImGui::SeparatorText(core::tr(layer < 3 ? layerNames[layer] : ENG_TR("engine.editor.streaming.layer.other")));
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.streaming.resident_of"),
                                          {{"resident", static_cast<core::i64>(resident)},
                                           {"present", static_cast<core::i64>(present)}})
                                     .c_str());

        const int columns = maxX - minX + 1;
        const int rows = maxZ - minZ + 1;
        // Capped so a 17x17 world and a 200x200 one both fit a panel. A cell
        // smaller than three pixels is a colour nobody can read, and a map
        // wider than the panel is one nobody can see the edge of.
        const float side = std::clamp(220.0f / static_cast<float>(std::max(columns, rows)), 3.0f, 18.0f);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (const asset::StreamingManager::ChunkView& cell : cells) {
            if (cell.id.layer != layer)
                continue;
            const float x = origin.x + static_cast<float>(cell.id.x - minX) * side;
            // Z DOWN the screen, which is the reading a map wants: north at the
            // top is a convention, and a grid drawn with +z upward reads
            // mirrored against every other view of the same world.
            const float y = origin.y + static_cast<float>(cell.id.z - minZ) * side;
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + side - 1.0f, y + side - 1.0f), chunkStateColor(cell.state));
        }
        ImGui::Dummy(ImVec2(static_cast<float>(columns) * side, static_cast<float>(rows) * side));
    }
}

// --- The project browser (ADR 0055) ------------------------------------------
//
// **One window filling the screen, and no dockspace.** The editor's shell is
// arrangeable because somebody works in it for hours; this is a screen you look
// at once per session and leave, and a launcher whose panels can be dragged
// apart is a launcher somebody can break.
//
// Every decision it takes is written into the `LauncherView` the caller owns and
// acted on by the loop, for the reason `EditorCommands` exists: starting a
// process from inside an ImGui callback is a frame that never finishes drawing.

// What the person is typing. Held here rather than in the view, because it is
// the panel's own state in exactly the way the explorer's expanded set is: the
// loop has no use for a half-typed name.
struct LauncherForm
{
    // Sized rather than dynamic, because ImGui's text field takes a buffer. Long
    // enough for a path somebody would actually type.
    char name[64]{};
    char parent[512]{};
    char open[512]{};
    int templateIndex = 0;
    bool creating = false;
    bool seeded = false;
};

LauncherForm g_launcherForm;
std::vector<std::string> g_launcherTemplates;

void copyInto(char* buffer, std::size_t size, std::string_view text)
{
    const std::size_t count = std::min(text.size(), size - 1);
    std::memcpy(buffer, text.data(), count);
    buffer[count] = '\0';
}

// --- Small pieces the two columns share --------------------------------------

// A quiet label over a block. With no rounding and no cards, a label and the
// space under it are what make a group read as one thing -- which is the whole
// technique this screen is drawn with.
void sectionLabel(const char* text, const IconAtlas* icons, std::string_view icon)
{
    if (drawIcon(icons, icon, ImGui::GetFontSize(), palette().accent))
        ImGui::SameLine();
    ImGui::TextUnformatted(text);
    ImGui::Spacing();
}

// **The one button on a screen that wears the accent.** Everything else is a
// surface, for the reason `applyTheme` states: spend the brand colour on every
// button and it stops saying anything.
//
// **And it takes the accent off when it cannot act**, rather than relying on
// `BeginDisabled`'s alpha. A dimmed brand colour is still the brand colour, so a
// disabled primary drawn that way reads as the thing to press -- which is the
// one place on this screen where being wrong costs somebody a click into
// nothing. Refusing looks like a surface; the caller still wraps it in
// `BeginDisabled`, which is what makes it actually refuse.
bool primaryButton(const char* label, ImVec2 size, bool enabled, const IconAtlas* icons)
{
    const ThemePalette& p = palette();
    const core::Color3 face = enabled ? p.accentFill : p.surfaceRaised;
    ImGui::PushStyleColor(ImGuiCol_Button, themeColor(face));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, themeBlend(face, p.text, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, themeBlend(face, p.onAccent, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_Text, themeColor(enabled ? p.onAccent : p.textMuted));
    const bool pressed = labeledIconButton(icons, icons::ActionAdd, label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// The list, and the two things a row can do.
void drawLauncherProjects(LauncherView& view, const IconAtlas* icons, float height)
{
    ProjectList& projects = *view.projects;
    if (projects.entries().empty()) {
        // Inside the same bordered box the list would fill, rather than as a
        // bare line where the box would have been: an empty state that changes
        // the shape of the screen reads as something having gone wrong.
        if (ImGui::BeginChild("##projects", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.launcher_projects.no_projects_yet")));
            ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.launcher_projects.create_a_project_or_open")));
        }
        ImGui::EndChild();
        return;
    }

    const float rowHeight = ImGui::GetFrameHeight() * 2.0f;
    if (ImGui::BeginChild("##projects", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
        std::filesystem::path forget;
        for (const RecentProject& entry : projects.entries()) {
            ImGui::PushID(entry.path.string().c_str());

            const ImVec2 origin = ImGui::GetCursorPos();
            const float rowWidth = ImGui::GetContentRegionAvail().x;
            // The whole row is the target, so opening a project is a click
            // anywhere on it rather than a hunt for a button.
            //
            // **`AllowOverlap` is what lets the Remove button be pressed at
            // all.** Without it the selectable owns every pixel it covers, and
            // an item submitted on top of it later is drawn and never hovered --
            // so Remove looked like a button, and the click went to the row
            // underneath and reported that the project was missing. Which is
            // exactly what a person deleting a folder and then trying to tidy
            // the list gets: the same message, over and over, from a button.
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight))) {
                if (entry.missing)
                    view.message = core::tr(ENG_TR("engine.editor.launcher_projects.not_there_any_more"),
                                            {{"path", entry.path.string()}});
                else
                    view.open = entry.path;
            }

            const ImVec2 rowMin = ImGui::GetItemRectMin();
            const ImVec2 rowMax = ImGui::GetItemRectMax();
            // **Asked of the RECTANGLE and not of the selectable**, because with
            // the overlap allowed the selectable stops reporting hover the
            // moment the pointer reaches the button on top of it -- and a
            // button that vanishes when you point at it cannot be clicked by
            // anybody. The rectangle is true for the row and everything drawn
            // over it, which is what "the pointer is on this row" means.
            const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(rowMin, rowMax);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (hovered) {
                draw->AddRect(rowMin, rowMax, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
            }
            const float glyph = ImGui::GetFontSize() * 1.5f;
            const ImVec2 glyphPos(rowMin.x + ImGui::GetStyle().FramePadding.x, rowMin.y + (rowHeight - glyph) * 0.5f);
            paintActionIcon(icons, icons::ContentFolder, glyphPos, glyph);
            const float textX = origin.x + glyph + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorPos(ImVec2(textX, origin.y + ImGui::GetStyle().FramePadding.y));
            const float removeWidth =
                ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.launcher_projects.remove"))).x +
                ImGui::GetStyle().FramePadding.x * 3.0f;
            ImGui::PushClipRect(ImVec2(glyphPos.x + glyph, rowMin.y),
                                ImVec2(std::max(glyphPos.x + glyph, rowMax.x - removeWidth), rowMax.y), true);
            if (entry.missing) {
                ImGui::TextDisabled("%s", entry.name.c_str());
                ImGui::SetCursorPos(ImVec2(textX, origin.y + ImGui::GetTextLineHeightWithSpacing()));
                // Said out loud, and the row stays. A list that edits itself
                // when a drive is unplugged is a list nobody can trust.
                ImGui::TextColored(
                    themeColor(palette().warning), "%s",
                    core::tr(ENG_TR("engine.editor.launcher_projects.missing"), {{"path", entry.path.string()}})
                        .c_str());
            }
            else {
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::SetCursorPos(ImVec2(textX, origin.y + ImGui::GetTextLineHeightWithSpacing()));
                ImGui::TextDisabled("%s", entry.path.string().c_str());
            }

            ImGui::PopClipRect();

            // **Emitted every row, every frame, and drawn at nothing when the
            // pointer is elsewhere.** A button that comes into being on the
            // frame a row becomes hovered is a button whose first press the
            // thing underneath has already taken -- the same defect the
            // Explorer's plus sign had, from the other side. Alpha is a drawing
            // property and not a hit-testing one, so the press lands the first
            // time; and the only rows where it is invisible are rows the
            // pointer is not on, which are the rows nobody can press it on.
            const float buttonWidth =
                ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.launcher_projects.remove"))).x +
                ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorPos(ImVec2(origin.x + rowWidth - buttonWidth - ImGui::GetStyle().FramePadding.x,
                                       origin.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
            if (!hovered)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
            if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.launcher_projects.remove"))))
                forget = entry.path;
            if (!hovered)
                ImGui::PopStyleVar();
            // Said on the button rather than only in the row's own text: a
            // person tidying a list of seven wants to know this one leaves and
            // nothing on disk is touched.
            ImGui::SetItemTooltip("%s",
                                  core::tr(entry.missing ? ENG_TR("engine.editor.launcher_projects.forget_gone_tip")
                                                         : ENG_TR("engine.editor.launcher_projects.forget_tip")));

            ImGui::SetCursorPos(ImVec2(origin.x, origin.y + rowHeight + ImGui::GetStyle().ItemSpacing.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
        if (!forget.empty()) {
            projects.forget(forget);
            view.message =
                core::tr(ENG_TR("engine.editor.launcher_projects.removed_from_the_list"), {{"path", forget.string()}});
            // The loop writes the file. See `LauncherView::forgot`.
            view.forgot = true;
        }
    }
    ImGui::EndChild();
}

void drawLauncherNew(LauncherView& view, const IconAtlas* icons)
{
    LauncherForm& form = g_launcherForm;

    if (g_launcherTemplates.empty()) {
        // Said rather than shown as a disabled button nobody can explain.
        ImGui::TextColored(
            themeColor(palette().warning), "%s",
            core::tr(ENG_TR("engine.editor.launcher_new.no_templates"), {{"path", view.templatesDir.string()}})
                .c_str());
        return;
    }

    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.launcher_new.template")));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##template", g_launcherTemplates[static_cast<std::size_t>(form.templateIndex)].c_str())) {
        for (int i = 0; i < static_cast<int>(g_launcherTemplates.size()); ++i) {
            const bool selected = i == form.templateIndex;
            if (ImGui::Selectable(g_launcherTemplates[static_cast<std::size_t>(i)].c_str(), selected))
                form.templateIndex = i;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.launcher_new.name")));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##name", core::tr(ENG_TR("engine.editor.launcher_new.my_game")), form.name,
                             sizeof(form.name));

    ImGui::Spacing();
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.launcher_new.location")));
    const float browseWidth = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.launcher_new.browse"))).x +
                              ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputText("##parent", form.parent, sizeof(form.parent));
    ImGui::SameLine();
    ImGui::BeginDisabled(!view.canBrowse);
    if (labeledIconButton(icons, icons::ContentFolder, core::tr(ENG_TR("engine.editor.launcher_new.browse")),
                          ImVec2(browseWidth, 0.0f)))
        view.browse = true;
    ImGui::EndDisabled();
    if (!view.canBrowse && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", core::tr(ENG_TR("engine.editor.launcher_new.this_build_has_no_folder_tip")));

    ImGui::Spacing();

    // The refusal is shown BEFORE the button is pressed, which is the difference
    // between a form and a form that argues with you. The line is reserved
    // whether or not it is showing, so the Create button does not jump out from
    // under the pointer the moment somebody types a space.
    const bool nameOk = validProjectName(form.name);
    if (form.name[0] != '\0' && !nameOk)
        ImGui::TextColored(themeColor(palette().warning), "%s",
                           core::tr(ENG_TR("engine.editor.launcher_new.letters_digits_dashes_and_underscores")));
    else
        ImGui::NewLine();

    ImGui::BeginDisabled(!nameOk);
    if (primaryButton(core::tr(ENG_TR("engine.editor.launcher_new.create_project")),
                      ImVec2(-1.0f, ImGui::GetFrameHeight() * 1.4f), nameOk, icons)) {
        const NewProjectResult result =
            createProject(view.templatesDir, view.definitions,
                          {.parent = std::filesystem::path(form.parent),
                           .name = form.name,
                           .templateName = g_launcherTemplates[static_cast<std::size_t>(form.templateIndex)]});
        if (result.error.has_value()) {
            view.message = core::engineCatalog().format(result.error->key, {});
            if (!result.error->detail.empty())
                view.message += " (" + result.error->detail + ")";
        }
        else {
            // Made and opened in one press, which is what somebody pressing
            // Create is asking for.
            view.open = result.path;
        }
    }
    ImGui::EndDisabled();
}

void drawLauncherOpen(LauncherView& view, const IconAtlas* icons)
{
    LauncherForm& form = g_launcherForm;

    const float buttonWidth = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.launcher_open.open"))).x +
                              ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(buttonWidth + ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##openpath", core::tr(ENG_TR("engine.editor.launcher_open.path_to_a_project")), form.open,
                             sizeof(form.open));
    ImGui::SameLine();
    if (labeledIconButton(icons, icons::ActionOpen, core::tr(ENG_TR("engine.editor.launcher_open.open")),
                          ImVec2(buttonWidth, 0.0f))) {
        const std::filesystem::path chosen(form.open);
        if (chosen.empty())
            view.message = core::tr(ENG_TR("engine.editor.launcher_open.type_a_path"));
        else if (!isProjectDirectory(chosen))
            view.message = core::tr(ENG_TR("engine.editor.launcher_open.not_a_project"), {{"path", chosen.string()}});
        else
            view.open = chosen;
    }
    ImGui::BeginDisabled(!view.canBrowse);
    if (labeledIconButton(icons, icons::ContentFolder,
                          core::tr(ENG_TR("engine.editor.launcher_open.browse_for_a_project")), ImVec2(-1.0f, 0.0f)))
        view.browse = true;
    ImGui::EndDisabled();
}

// The band across the top: the wordmark, and which engine this is.
//
// A band rather than a line of text, because it is the only thing on this screen
// that is not a control and it should not read as one. `WindowPadding` is zero
// for this window so the band can reach both edges; the body child below puts
// the padding back.
void drawLauncherHeader(const IconAtlas* icons)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ThemePalette& p = palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = themeMetrics().windowPaddingX * style.FontScaleMain * 2.0f;
    const float height = ImGui::GetFrameHeight() * 3.0f;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + viewport->WorkSize.x, min.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(themeColor(p.background)));
    draw->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), ImGui::GetColorU32(themeColor(p.border)));
    const float markSize = ImGui::GetFrameHeight() * 1.65f;
    drawBrandMark(ImVec2(min.x + pad, min.y + (height - markSize) * 0.5f), markSize);
    const float titleX = min.x + pad + markSize + style.ItemSpacing.x;

    // **`FontSizeBase`, not `GetFontSize()`.** The second is the size AFTER the
    // global scale factors, so feeding it back into `PushFont` multiplies the
    // scale in a second time -- ImGui's own header says so in capitals. Nothing
    // saw it because the scale was one until this milestone gave it a setting.
    ImGui::PushFont(nullptr, style.FontSizeBase * 1.75f);
    const float titleHeight = ImGui::GetFontSize();
    ImGui::SetCursorScreenPos(ImVec2(titleX, min.y + (height - titleHeight) * 0.5f));
    ImGui::TextUnformatted(std::string(core::kBrandName).c_str());
    const float titleWidth = ImGui::GetItemRectSize().x;
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(
        ImVec2(titleX + titleWidth + style.ItemSpacing.x * 1.5f, min.y + (height - ImGui::GetFontSize()) * 0.5f));
    ImGui::TextDisabled("%s", ENG_VERSION_STRING);

    const char* themeLabel = core::tr(currentTheme().dark ? ENG_TR("engine.editor.launcher.light_theme")
                                                          : ENG_TR("engine.editor.launcher.dark_theme"));
    const float themeWidth = ImGui::CalcTextSize(themeLabel).x + ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f;
    if (max.x - themeWidth - pad > titleX + titleWidth + 80.0f * style.FontScaleMain) {
        ImGui::SetCursorScreenPos(ImVec2(max.x - themeWidth - pad, min.y + (height - ImGui::GetFrameHeight()) * 0.5f));
        if (labeledIconButton(icons, icons::ClassLighting, themeLabel, ImVec2(themeWidth, 0.0f))) {
            g_appearance.themeId = currentTheme().dark ? "light" : "dark";
            applyAppearance();
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(min.x, max.y));
}

// The last thing that happened, in a band of its own at the bottom.
//
// At the bottom rather than under whichever column produced it: a message that
// appears somewhere different depending on what you pressed is a message
// somebody has to hunt for, and half of these are refusals.
void drawLauncherFooter(const LauncherView& view, float height)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + ImGui::GetMainViewport()->WorkSize.x, min.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(themeColor(palette().surface)));
    draw->AddLine(min, ImVec2(max.x, min.y), ImGui::ColorConvertFloat4ToU32(themeColor(palette().border)));

    ImGui::SetCursorScreenPos(ImVec2(min.x + style.WindowPadding.x, min.y + (height - ImGui::GetFontSize()) * 0.5f));
    ImGui::TextColored(themeColor(palette().warning), "%s", view.message.c_str());
}

void drawLauncher(LauncherView* view, const IconAtlas* icons)
{
    if (view == nullptr || view->projects == nullptr)
        return;

    LauncherForm& form = g_launcherForm;
    if (!form.seeded) {
        form.seeded = true;
        copyInto(form.parent, sizeof(form.parent), view->defaultParent.string());
        g_launcherTemplates = availableTemplates(view->templatesDir);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    // Zero, so the header and the footer can be bands that reach both edges. The
    // body child puts the real padding back, which is the only way to have both
    // in one window.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleVar();

    drawLauncherHeader(icons);

    const float footerHeight = view->message.empty() ? 0.0f : ImGui::GetFrameHeight() * 1.6f;
    if (ImGui::BeginChild("##body", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        // Two columns: what you have on the left, what you can make on the
        // right. The proportion is deliberate -- the list is the thing somebody
        // came for, and on every launch after the first it is the only thing
        // they touch.
        const float scale = ImGui::GetStyle().FontScaleMain;
        const bool compact = viewport->WorkSize.x < 900.0f * scale;
        const float rightWidth = std::min(viewport->WorkSize.x * 0.38f, 420.0f * scale);
        if (ImGui::BeginTable("##launcher-columns", compact ? 1 : 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthStretch);
            if (!compact)
                ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthFixed, rightWidth);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            sectionLabel(core::tr(ENG_TR("engine.editor.launcher.your_projects")), icons, icons::ContentFolder);
            drawLauncherProjects(*view, icons, compact ? ImGui::GetFrameHeight() * 6.0f : 0.0f);

            if (compact)
                ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(compact ? 0 : 1);
            // **Making one comes before opening one**, which is the opposite of
            // the order this screen shipped with. Somebody who has projects
            // opens them from the list on the left; the right-hand column is
            // where somebody who has none goes, and for them the first thing
            // should be the thing they need.
            sectionLabel(core::tr(ENG_TR("engine.editor.launcher.create_a_project")), icons, icons::ActionNew);
            drawLauncherNew(*view, icons);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            sectionLabel(core::tr(ENG_TR("engine.editor.launcher.open_a_project")), icons, icons::ActionOpen);
            drawLauncherOpen(*view, icons);

            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    if (footerHeight > 0.0f)
        drawLauncherFooter(*view, footerHeight);

    ImGui::End();
}

// **How the connection is doing** (the multiplayer smoothness brief): what a
// player feels as lag, in the numbers that say why -- the link, the snapshots,
// how often and how far this machine's character was corrected, and the
// authority's queue of this player's input.
void drawNetwork(const scene::World& world)
{
    const scene::EngineState& state = world.engineState();
    const scene::EngineState::NetworkStats& stats = state.networkStats;
    if (ImGui::BeginTable("##network", 2, ImGuiTableFlags_SizingStretchProp)) {
        const auto row = [](const char* name, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.c_str());
        };
        const auto number = [](double value, int decimals, const char* unit) {
            char text[64];
            (void)std::snprintf(text, sizeof(text), "%.*f%s", decimals, value, unit);
            return std::string(text);
        };
        row(core::tr(ENG_TR("engine.editor.network.ping")), number(stats.pingMs, 0, " ms"));
        row(core::tr(ENG_TR("engine.editor.network.jitter")), number(stats.jitterMs, 0, " ms"));
        row(core::tr(ENG_TR("engine.editor.network.loss")), number(stats.lossPercent, 1, " %"));
        row(core::tr(ENG_TR("engine.editor.network.snapshots")), number(stats.snapshotsPerSecond, 0, " /s"));
        row(core::tr(ENG_TR("engine.editor.network.corrections")), number(stats.correctionsPerSecond, 1, " /s"));
        row(core::tr(ENG_TR("engine.editor.network.last_correction")),
            number(stats.lastCorrectionMetres * 100.0, 1, " cm"));
        row(core::tr(ENG_TR("engine.editor.network.input_buffer")),
            number(static_cast<double>(stats.inputBufferDepth), 0, " ticks"));
        row(core::tr(ENG_TR("engine.editor.network.input_ran_dry")),
            number(static_cast<double>(stats.inputStarvations), 0, ""));
        row(core::tr(ENG_TR("engine.editor.network.predicted_parts")),
            number(static_cast<double>(stats.predictedParts), 0, ""));
        row(core::tr(ENG_TR("engine.editor.network.resimulations")), number(stats.resimulationsPerSecond, 1, " /s"));
        row(core::tr(ENG_TR("engine.editor.network.resimulated_ticks")),
            number(stats.resimulatedTicksPerSecond, 0, " /s"));
        row(core::tr(ENG_TR("engine.editor.network.resimulation")), number(stats.resimulationMs, 2, " ms"));
        row(core::tr(ENG_TR("engine.editor.network.peers")),
            number(static_cast<double>(state.networkPeerCount), 0, ""));
        ImGui::EndTable();
    }
}

void drawShell(const Frame& frame, scene::World* world, core::InstanceId root, Inspector* inspector,
               script::ScriptRuntime* runtime, const StreamingHost* streaming, const RenderCounters& counters)
{
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 520.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(std::string(core::kBrandName).c_str())) {
        drawStats(frame, counters);

        // A host with no world is a normal state -- `--version`, the render
        // gates, a test with no scene -- and it gets the stats panel it has
        // always had.
        if (world != nullptr && inspector != nullptr) {
            ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.shell.explorer")));
            if (ImGui::BeginChild("explorer", ImVec2(0.0f, 200.0f), ImGuiChildFlags_Borders))
                // Everything, including what streaming made: this is the debug
                // overlay rather than the editor, and it exists to show the
                // world as it IS rather than as it was authored.
                drawExplorer(*world, root, *inspector, nullptr, nullptr, nullptr, true);
            ImGui::EndChild();

            ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.shell.properties")));
            drawProperties(*world, root, *inspector);
            drawWriteLog(*world, *inspector);
        }

        if (runtime != nullptr) {
            ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.shell.memory")));
            drawMemory(*runtime);
        }

        // Only in a match, for the reason the streaming panel below is only
        // where something streams.
        if (world != nullptr && world->engineState().networkTopology != scene::NetworkTopology::Solo) {
            ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.shell.network")));
            drawNetwork(*world);
        }

        // Only for a project that streams. A panel of zeroes on every example
        // that does not would be furniture nobody can act on.
        if (streaming != nullptr && streaming->active()) {
            ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.shell.streaming")));
            drawStreaming(*streaming);
        }

        // The console draws even with no VM: the LOG half is the half a person
        // wants when the VM failed to boot, which is the moment they most want
        // it. The input line simply does nothing.
        ImGui::SeparatorText(core::tr(ENG_TR("engine.editor.shell.console")));
        drawConsole(runtime, nullptr);
    }
    ImGui::End();
}

} // namespace

DebugOverlay::DebugOverlay(platform::Window& window, rhi::IDevice& device, Shell shell, std::string layoutPath)
    : shell_(shell), layoutPath_(std::move(layoutPath))
{
    // The editor IS the application, so it is up from the first frame. F3 still
    // works and still hides it, which is the cheapest way to look at the world
    // without the furniture.
    visible_ = shell_ == Shell::Editor;

    SDL_Window* sdlWindow = platform::nativeWindow(window);
    SDL_GPUDevice* gpuDevice = rhi::nativeDevice(device);

    // Not a failure and not worth a message: `--rhi=capture` and `--rhi=null`
    // have nothing to draw with, and answering false from active() is the
    // entire contract for that case.
    if (sdlWindow == nullptr || gpuDevice == nullptr)
        return;

    if (ImGui::GetCurrentContext() != nullptr) {
        core::log(core::LogLevel::Warn, ENG_TR("engine.overlay.warn.already_running"));
        return;
    }

    // The pipeline ImGui builds is compiled against one colour format, and the
    // only source of the right one is the device-window pair. An unclaimed
    // window answers INVALID here rather than at the first draw, so the
    // ordering requirement is checked where it can still be explained.
    const SDL_GPUTextureFormat colorFormat = SDL_GetGPUSwapchainTextureFormat(gpuDevice, sdlWindow);
    if (colorFormat == SDL_GPU_TEXTUREFORMAT_INVALID) {
        core::log(core::LogLevel::Warn, ENG_TR("engine.overlay.warn.window_not_claimed"));
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    // Docking is why ADR 0011 pins the docking tag rather than the release one.
    // No dockspace host window is created: one panel does not need one, and the
    // editor that would is not in v1 (R15).
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Otherwise ImGui writes imgui.ini into the working directory, which under
    // CTest is the source tree (R14) and for a game is wherever it happened to
    // be launched from. Remembered window positions are not state a GAME has
    // decided to keep -- but they are exactly what an editor owes somebody who
    // arranged their panels once, so `Shell::Editor` names a file inside the
    // project it opened.
    io.IniFilename = nullptr;
    if (shell_ == Shell::Editor && !layoutPath_.empty()) {
        // ImGui writes the file and never the directory above it, and a project
        // that has not been built yet has no `.engine/`. Failing here would mean
        // a layout that silently never persists, which is worse than a layout
        // that never existed.
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(layoutPath_).parent_path(), ec);
        io.IniFilename = layoutPath_.c_str();
    }
    // **The shell's own look (ADR 0056), before anything draws.** Two things
    // rather than one: `StyleColorsDark` was written for a debug window over a
    // game, and ImGui's default face is a 13 px bitmap designed for a debugger
    // -- which between them are most of the reason this editor read as an
    // overlay with panels rather than as an application.
    g_appearance = loadAppearance(appearanceFile());
    scriptEditorSettings() = loadScriptEditorSettings(scriptEditorSettingsFile());
    // Read once, here, because it is the display the window OPENED on. Following
    // a monitor change would mean re-rasterising the atlas mid-frame, and a
    // person who drags the editor to a second screen can reopen it -- which is
    // what every editor in this shape does today.
    g_displayScale = platform::windowDisplayScale(window);
    applyTheme(themeById(g_appearance.themeId), resolveUiScale(g_appearance.scale, g_displayScale));

    // Before the backend starts, so the atlas it builds is the one with Inter
    // in it. False is a normal outcome -- a build tree whose content has not
    // been staged -- and it is said once rather than drawn silently wrong.
    if (!loadUiFont())
        core::log(core::LogLevel::Warn, ENG_TR("engine.overlay.warn.font_missing"));
    // The code face, added second so the UI one stays `Fonts[0]` and therefore
    // the default. False is the same normal state and gets the same treatment:
    // the script editor falls back to the UI face, which is readable and wrongly
    // spaced, and the log says which.
    if (!loadCodeFont())
        core::log(core::LogLevel::Warn, ENG_TR("engine.overlay.warn.code_font_missing"));

    if (!ImGui_ImplSDL3_InitForSDLGPU(sdlWindow)) {
        ImGui::DestroyContext();
        core::log(core::LogLevel::Warn, ENG_TR("engine.overlay.warn.init_failed"));
        return;
    }

    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = gpuDevice;
    info.ColorTargetFormat = colorFormat;
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;

    if (!ImGui_ImplSDLGPU3_Init(&info)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        core::log(core::LogLevel::Warn, ENG_TR("engine.overlay.warn.init_failed"));
        return;
    }

    g_window = &window;
    g_device = &device;
    active_ = true;
}

DebugOverlay::~DebugOverlay()
{
    if (!active_)
        return;

    // **Written on the way out.** ImGui saves the ini by itself, but on a timer
    // -- so an arrangement made in the last few seconds before somebody quits is
    // an arrangement they made twice. `IniFilename` is null for every shell but
    // the editor, and `SaveIniSettingsToDisk` is only called when it is not.
    if (const ImGuiIO& io = ImGui::GetIO(); io.IniFilename != nullptr)
        ImGui::SaveIniSettingsToDisk(io.IniFilename);

    // Renderer first: it releases GPU objects through the device, which is
    // still alive because the constructor's contract says it must be.
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    g_window = nullptr;
    g_device = nullptr;
    g_thumbnails = nullptr;
    g_saves = nullptr;
    g_surfaceCompiler = nullptr;
}

void DebugOverlay::setThumbnails(ThumbnailCache* thumbnails) noexcept
{
    g_thumbnails = thumbnails;
}

void DebugOverlay::setSaves(script::SaveStore* saves) noexcept
{
    g_saves = saves;
}

void DebugOverlay::setSurfaceCompiler(SurfaceCompiler* compiler) noexcept
{
    g_surfaceCompiler = compiler;
}

void DebugOverlay::clearConsole()
{
    ConsoleLog& log = console();
    std::lock_guard<std::mutex> lock(log.mutex);
    log.lines.clear();
}

void DebugOverlay::setSkeleton(const scene::SkeletonHost* skeleton) noexcept
{
    g_skeleton = skeleton;
}

void DebugOverlay::handleEvents(std::span<const platform::Event> events)
{
    if (!active_)
        return;

    // ImGui models far more input than the engine does -- text, mouse capture,
    // window focus -- so it reads the untranslated stream. That stream existing
    // at all is what sdl_interop.h is for.
    // **While the pointer is held, the UI does not see the mouse.** Relative
    // mode hides the cursor and stops moving it, and SDL goes on posting motion
    // with a logical position it accumulates from the deltas -- so without this
    // the invisible cursor walks across the panels, highlighting rows in the
    // explorer and hovering buttons nobody is pointing at.
    //
    // **Two holders, and they need different things.** The EDITOR holds it
    // while a right-drag turns the fly camera (D063), and there only motion is
    // withheld: the button going UP still has to arrive, or ImGui believes it
    // is held forever and the turn never ends. The GAME holds it for as long as
    // somebody is playing (D069), and there the mouse is not the UI's at all --
    // motion, wheel and button PRESSES are all withheld, because a player
    // turning their head must not be clicking the explorer at the same time.
    //
    // A release is delivered in both cases and for the same reason: ImGui can
    // never be left believing a button it was never told about is down.
    const bool looking = editor_ != nullptr && editor_->lookInput().active;
    for (const SDL_Event& raw : platform::rawEvents()) {
        const bool motion = raw.type == SDL_EVENT_MOUSE_MOTION;
        if (looking && motion)
            continue;
        if (gameHoldsPointer_ &&
            (motion || raw.type == SDL_EVENT_MOUSE_WHEEL || raw.type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
            continue;
        }
        ImGui_ImplSDL3_ProcessEvent(&raw);
    }

    // **And the cursor is nowhere**, which withholding motion alone does not
    // say: ImGui would keep the last position it was told about, so whichever
    // row the cursor happened to be over when play was pressed would stay lit
    // for the whole session. `-FLT_MAX` is ImGui's own spelling of "there is no
    // mouse". The SDL3 backend will not overwrite it while relative mode is on
    // -- `ImGui_ImplSDL3_UpdateMouseData` guards its global-state fallback with
    // exactly that -- so this holds for the frame.
    if (gameHoldsPointer_)
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);

    for (const platform::Event& event : events) {
        // Repeats excluded: holding F3 down should not strobe the panel.
        // **The game's while it has the keyboard** (the Play rule) -- except
        // to bring the furniture BACK: with it put away the viewport is all
        // there is, and a key that only worked one way would leave no way
        // out of the picture but stopping the game.
        if (event.type == platform::EventType::KeyDown && event.key == platform::Key::F3 && !event.repeat &&
            (!g_gameHasKeyboard || !visible_))
            visible_ = !visible_;
    }
}

namespace {

// **An interface error goes to the log, once**, not only to a red box on the
// screen. ImGui reports a misuse -- a cursor moved past what a window drew, a
// push without its pop -- as a tooltip that only somebody looking at that
// window sees; the owner met one in the Export window and could not say what
// it was. Its debug log carries the same line, and this reads the new part of
// that log each frame and says each distinct error once, in the engine's own
// log, where the console, the log file and a test can all see it.
void forwardInterfaceErrors()
{
    static std::size_t seen = 0;
    static std::set<std::string> said;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiTextBuffer& log = g.DebugLogBuf;
    if (static_cast<std::size_t>(log.size()) < seen)
        seen = 0;
    const std::string_view fresh(log.begin() + seen, static_cast<std::size_t>(log.size()) - seen);
    seen = static_cast<std::size_t>(log.size());
    constexpr std::string_view Marker = "[imgui-error] In window ";
    std::size_t at = fresh.find(Marker);
    while (at != std::string_view::npos) {
        const std::size_t end = fresh.find('\n', at);
        const std::size_t from = at + Marker.size();
        std::string message(fresh.substr(from, end == std::string_view::npos ? std::string_view::npos : end - from));
        if (said.insert(message).second) {
            const core::I18nArg args[] = {{"message", message}};
            core::log(core::LogLevel::Warn, ENG_TR("engine.overlay.warn.interface_error"), args);
        }
        at = end == std::string_view::npos ? end : fresh.find(Marker, end);
    }
}

} // namespace

void DebugOverlay::render(rhi::ICmdList& cmd, rhi::TextureHandle target, const Frame& frame)
{
    // **The editor draws while hidden, and every other shell does not.** In the
    // editor the world lives in a texture that only ImGui puts on the screen,
    // so an early return here is a black window; the shell below draws the
    // viewport alone in that state. The F3 overlay is drawn OVER a finished
    // frame and has nothing to show when it is down.
    if (!active_ || !target.valid())
        return;
    if (!visible_ && shell_ != Shell::Editor && shell_ != Shell::Launcher)
        return;

    // The frame currently being recorded. Null means the caller is outside
    // beginFrame()/submitAndPresent(), where there is nothing to draw into.
    SDL_GPUCommandBuffer* buffer = rhi::nativeCommandBuffer(*g_device);
    if (buffer == nullptr)
        return;

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // After the platform's input and before the frame reads it, so a scripted
    // pointer wins over wherever the real one is.
    if (drive_ != nullptr && drive_->feed())
        driveQuit_ = true;
    ImGui::NewFrame();
    if (shell_ == Shell::Launcher)
        drawLauncher(launcher_, icons_);
    else if (shell_ == Shell::Editor)
        drawEditorShell(frame, world_, root_, inspector_, runtime_, editor_, viewportTexture_, layoutBuilt_, commands_,
                        panels_, dialogs_, icons_, scripts_, scriptCommands_, debugView_, audio_, streaming_, visible_,
                        counters_);
    else
        drawShell(frame, world_, root_, inspector_, runtime_, streaming_, counters_);
    ImGui::Render();
    forwardInterfaceErrors();

    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr)
        return;

    // Mandatory, and mandatory HERE: this is where the vertex and index buffers
    // are uploaded, and a copy cannot run inside a render pass. The backend
    // header says so in capitals for the same reason our seam does.
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, buffer);

    // Load, not Clear: the overlay is drawn on top of a finished frame.
    const std::array<rhi::ColorAttachment, 1> colors{rhi::ColorAttachment{
        .texture = target,
        .loadOp = rhi::LoadOp::Load,
        .storeOp = rhi::StoreOp::Store,
    }};

    cmd.pushDebugGroup("debug-overlay");
    cmd.beginRenderPass({.colorAttachments = colors, .debugName = "imgui"});

    // Opened through the seam a line ago, so this is the pass just begun -- the
    // device owns exactly one command list, which is the one `cmd` refers to.
    if (SDL_GPURenderPass* pass = rhi::nativeRenderPass(*g_device); pass != nullptr)
        ImGui_ImplSDLGPU3_RenderDrawData(drawData, buffer, pass);

    cmd.endRenderPass();
    cmd.popDebugGroup();
}

void DebugOverlay::setViews(const ViewHost* views) noexcept
{
    g_views = views;
}

void DebugOverlay::preserveExplorerOnNextWorld() noexcept
{
    g_keepExpansionOnce = true;
}

bool DebugOverlay::editorTyping() const noexcept
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput;
}

void DebugOverlay::captureLog()
{
    ConsoleLog& log = console();
    if (log.installed)
        return;
    log.installed = true;

    log.previous = core::setLogSink([](core::LogLevel level, std::string_view text) {
        ConsoleLog& sink = console();
        {
            // A script's line carries where it came from first, then a printed
            // table's rows (`core::logDetail`, `print_tree.h`).
            std::string_view tree = core::logDetail();
            std::optional<SourceLocation> source;
            if (!tree.empty() && tree.front() == '@') {
                const std::size_t end = tree.find('\n');
                const std::string_view head = tree.substr(1, end == std::string_view::npos ? tree.npos : end - 1);
                const std::size_t split = head.find('\x1F');
                if (split != std::string_view::npos) {
                    const std::string_view number = head.substr(split + 1);
                    core::u32 line = 0;
                    for (const char digit : number)
                        line = digit >= '0' && digit <= '9' ? line * 10 + static_cast<core::u32>(digit - '0') : line;
                    source = SourceLocation{std::string(head.substr(0, split)), line};
                }
                tree = end == std::string_view::npos ? std::string_view{} : tree.substr(end + 1);
            }
            std::lock_guard<std::mutex> lock(sink.mutex);
            ConsoleLog::Line* last = sink.lines.empty() ? nullptr : &sink.lines.back();
            const bool sameSource =
                last != nullptr && last->source.has_value() == source.has_value() &&
                (!source.has_value() || (last->source->chunk == source->chunk && last->source->line == source->line));
            if (last != nullptr && last->level == level && last->text == text && last->tree == tree && sameSource) {
                ++last->repeats;
            }
            else {
                sink.lines.push_back(
                    ConsoleLog::Line{level, std::string(text), sink.nextSeq++, std::string(tree), std::move(source)});
                while (sink.lines.size() > ConsoleLog::kMaxLines)
                    sink.lines.pop_front();
            }
        }
        // Chained rather than replaced: the console pane and the log FILE both
        // get every line. A shell that ate the log would be the last place
        // anybody looked for it.
        if (sink.previous)
            sink.previous(level, text);
    });
}

#else

// ADR 0011: a shipping build contains no ImGui, so the overlay contains no
// behaviour. The class keeps its shape and its signatures -- that is what lets
// the frame loop call it without an #ifdef -- and active() answers false, which
// is how anything that asks finds out there is nothing here.

DebugOverlay::DebugOverlay(platform::Window&, rhi::IDevice&, Shell, std::string)
{}

DebugOverlay::~DebugOverlay() = default;

void DebugOverlay::handleEvents(std::span<const platform::Event>)
{}

// A shipping build has no content browser, so there is nothing to show a
// picture in. The frame loop still owns a cache and still offers it, which is
// what keeps that loop free of an #ifdef.
void DebugOverlay::setThumbnails(ThumbnailCache*) noexcept
{}

void DebugOverlay::setSaves(script::SaveStore*) noexcept
{}

void DebugOverlay::setSurfaceCompiler(SurfaceCompiler*) noexcept
{}

void DebugOverlay::clearConsole()
{}

void DebugOverlay::setSkeleton(const scene::SkeletonHost*) noexcept
{}

void DebugOverlay::render(rhi::ICmdList&, rhi::TextureHandle, const Frame&)
{}

// Nothing to capture INTO: the ring buffer and the console pane that reads it
// live in the half of this file that ImGui compiles. Leaving the process log
// sink alone is the whole behaviour -- the log FILE keeps every line, which is
// where a shipping build's log was always going to be read from.
void DebugOverlay::setViews(const ViewHost* views) noexcept
{
    (void)views;
}

void DebugOverlay::preserveExplorerOnNextWorld() noexcept
{}

bool DebugOverlay::editorTyping() const noexcept
{
    return false;
}

void DebugOverlay::captureLog()
{}

#endif

} // namespace engine::app
