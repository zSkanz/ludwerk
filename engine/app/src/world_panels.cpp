#include "engine/app/world_panels.h"

#if ENG_DEBUG_UI

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/content_tree.h"
#include "engine/app/editor.h"
#include "engine/app/inspector.h"
#include "engine/asset/terrain_palette.h"
#include "engine/core/i18n.h"
#include "engine/scene/components.h"
#include "engine/scene/value.h"
#include "engine/scene/world.h"

namespace engine::app {

using core::f32;

namespace {

// One gesture per drag of a widget in these panels, opened on the first edit
// and closed when nothing is held -- the Properties panel's rule, so a slider
// dragged across a second is one undo step and not sixty.
struct PanelGesture
{
    core::u64 id = 0;

    void edited(Inspector& inspector)
    {
        if (id == 0)
            id = inspector.beginGesture();
    }
    void settle(Inspector& inspector)
    {
        if (id != 0 && !ImGui::IsAnyItemActive()) {
            if (inspector.gesture() == id)
                inspector.endGesture();
            id = 0;
        }
    }
};

PanelGesture g_settingsGesture;
PanelGesture g_lookGesture;

// A texture slot: "(none)" or one of the project's images, as the URN a block
// type stores. The list is read when the combo OPENS, for the reason the
// property picker reads it then: it is a directory walk.
bool textureSlot(const char* label, ContentTree& content, scene::World& world, core::NameAtom& slot)
{
    static std::vector<std::string> candidates;
    const std::string_view current = slot.valid() ? world.atoms().text(slot) : std::string_view{};
    const std::string preview = current.empty() ? std::string("(none)") : std::string(current);
    bool changed = false;
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    if (ImGui::BeginCombo("##texture", preview.c_str())) {
        if (ImGui::IsWindowAppearing())
            candidates = content.filesOfKind(ContentKind::Texture);
        if (ImGui::Selectable(core::tr(ENG_TR("engine.editor.texture_slot.none")), current.empty())) {
            slot = core::NameAtom{};
            changed = true;
        }
        for (const std::string& candidate : candidates) {
            const std::string urn = "asset://" + candidate;
            if (ImGui::Selectable(candidate.c_str(), urn == current)) {
                slot = world.atoms().intern(urn);
                changed = true;
            }
        }
        if (candidates.empty())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.texture_slot.no_images_under_content_import")));
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

} // namespace

void drawTerrainHeightmap(Editor& editor, scene::World& world, core::InstanceId root, Inspector& inspector,
                          EditorCommands& commands)
{
    const core::InstanceId terrainId = editor.terrainIn(world, root);
    const scene::TerrainComponent* terrain = terrainId.valid() ? world.terrains().find(terrainId) : nullptr;
    const float labelWidth = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.terrain.heightmap.white"))).x +
                             ImGui::GetStyle().ItemSpacing.x * 2.0f;
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.heightmap.intro")));
        ImGui::PopTextWrapPos();

        const std::filesystem::path& source = editor.heightmapSource();
        const std::string chosen = source.empty()
                                       ? std::string(core::tr(ENG_TR("engine.editor.terrain.heightmap.none")))
                                       : source.filename().string();
        ImGui::TextUnformatted(chosen.c_str());
        if (!source.empty())
            ImGui::SetItemTooltip("%s", source.string().c_str());
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.terrain.heightmap.choose")), ImVec2(-FLT_MIN, 0.0f)))
            commands.pickHeightmap = true;

        static f32 size = 256.0f;
        static f32 low = 0.0f;
        static f32 high = 64.0f;
        // An image this editor exported comes with the size and heights it was
        // taken at, taken up once as it is chosen: the same ground back.
        static std::filesystem::path adopted;
        if (source != adopted) {
            adopted = source;
            if (const std::optional<Editor::HeightmapHint>& hint = editor.heightmapHint(); hint.has_value()) {
                size = hint->size;
                low = hint->low;
                high = hint->high;
            }
        }
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.create.size")));
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##heightmap-size", &size, 1.0f, 8.0f, 32768.0f,
                         core::tr(ENG_TR("engine.editor.unit.metres_0")));
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.heightmap.size_tip")));
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.heightmap.black")));
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##heightmap-low", &low, 0.25f, -1024.0f, 1024.0f,
                         core::tr(ENG_TR("engine.editor.unit.metres_1")));
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.heightmap.white")));
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##heightmap-high", &high, 0.25f, -1024.0f, 1024.0f,
                         core::tr(ENG_TR("engine.editor.unit.metres_1")));

        // The cost before the click, as the flat-ground form shows it.
        const f32 voxel = terrain != nullptr ? terrain->field.settings().voxelSize : asset::FieldSettings{}.voxelSize;
        const auto columns = static_cast<int>(std::lround(size / std::max(voxel, 0.01f))) + 1;
        char voxelText[32];
        (void)std::snprintf(voxelText, sizeof(voxelText), "%.2f", static_cast<double>(voxel));
        ImGui::TextDisabled(
            "%s", core::tr(ENG_TR("engine.editor.terrain.heightmap.columns"),
                           {{"columns", static_cast<core::i64>(columns)}, {"voxel", std::string_view(voxelText)}})
                      .c_str());
        if (terrain != nullptr) {
            const asset::FieldSettings& settings = terrain->field.settings();
            const auto originY = static_cast<f32>(terrain->origin.y);
            if (std::min(low, high) - originY < settings.minHeight ||
                std::max(low, high) - originY > settings.maxHeight) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.heightmap.past_range")));
                ImGui::PopTextWrapPos();
            }
        }

        // A set of tiles, or more columns than one table: laid a tile at a
        // time, which is not an undo step (ADR 0149 §2).
        if (columns > static_cast<int>(Editor::MaxTableColumns) || isHeightmapPiece(source)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.terrain.import.tiled_note")));
            ImGui::PopTextWrapPos();
        }

        ImGui::BeginDisabled(source.empty());
        if (ImGui::Button(terrain == nullptr ? core::tr(ENG_TR("engine.editor.terrain.heightmap.create"))
                                             : core::tr(ENG_TR("engine.editor.terrain.heightmap.import")),
                          ImVec2(-FLT_MIN, 0.0f))) {
            (void)editor.importHeightmap(world, root, inspector,
                                         Editor::HeightmapImport{source, size, low, high, editor.brush().material});
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.heightmap.import_tip")));
    }
}

void drawTerrainSettings(Editor& editor, scene::World& world, core::InstanceId root, Inspector& inspector)
{
    const core::InstanceId terrainId = editor.terrainIn(world, root);
    const scene::TerrainComponent* terrain = terrainId.valid() ? world.terrains().find(terrainId) : nullptr;
    if (terrain == nullptr) {
        ImGui::TextWrapped("%s", core::tr(ENG_TR("engine.editor.terrain.setup.none")));
        return;
    }
    const float labelWidth = ImGui::CalcTextSize(core::tr(ENG_TR("engine.editor.terrain.setup.voxel"))).x +
                             ImGui::GetStyle().ItemSpacing.x * 2.0f;
    {
        const asset::FieldSettings& settings = terrain->field.settings();
        const bool empty = terrain->field.empty();
        const auto write = [&](std::string_view property, f32 value) {
            g_settingsGesture.edited(inspector);
            inspector.enqueue(terrainId, world.atoms().intern(property), scene::Value{static_cast<double>(value)});
        };

        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.setup.voxel")));
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        f32 voxel = settings.voxelSize;
        ImGui::BeginDisabled(!empty);
        // The whole range the property takes, 0.1 to 64 m (the editor list: it
        // stopped at 8), on a scale where a tenth and a metre are both a drag.
        if (ImGui::DragFloat("##voxel-size", &voxel, 0.01f, 0.1f, 64.0f,
                             core::tr(ENG_TR("engine.editor.unit.metres_2")), ImGuiSliderFlags_Logarithmic))
            write("VoxelSize", voxel);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", empty ? core::tr(ENG_TR("engine.editor.terrain.setup.voxel_tip"))
                                          : core::tr(ENG_TR("engine.editor.terrain.setup.voxel_locked_tip")));

        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.setup.lowest")));
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        f32 lowest = settings.minHeight;
        if (ImGui::DragFloat("##min-height", &lowest, 0.5f, -4096.0f, settings.maxHeight - 1.0f,
                             core::tr(ENG_TR("engine.editor.unit.metres_1"))))
            write("MinHeight", lowest);
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.setup.lowest_tip")));
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.terrain.setup.highest")));
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        f32 highest = settings.maxHeight;
        if (ImGui::DragFloat("##max-height", &highest, 0.5f, settings.minHeight + 1.0f, 4096.0f,
                             core::tr(ENG_TR("engine.editor.unit.metres_1"))))
            write("MaxHeight", highest);
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.terrain.setup.highest_tip")));
        g_settingsGesture.settle(inspector);
    }
}

void drawBlockLook(Editor& editor, scene::World& world, Inspector& inspector)
{
    scene::VoxelComponent* voxels = Editor::voxelsIn(world);
    const asset::BlockId selected = editor.blockType();
    if (voxels == nullptr || selected == asset::AirBlock || selected > voxels->types.size())
        return;
    const scene::VoxelBlockType type = voxels->types[selected - 1u];

    std::array<core::NameAtom, 3> textures{type.texture, type.sideTexture, type.bottomTexture};
    bool changed =
        textureSlot(core::tr(ENG_TR("engine.editor.block_look.top_image")), editor.content(), world, textures[0]);
    changed |=
        textureSlot(core::tr(ENG_TR("engine.editor.block_look.side_image")), editor.content(), world, textures[1]);
    changed |=
        textureSlot(core::tr(ENG_TR("engine.editor.block_look.bottom_image")), editor.content(), world, textures[2]);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.block_look.the_colours_above_tint_the")));
    ImGui::PopTextWrapPos();

    // `Enum.BlockOpacity`, in its order.
    int opacity = std::clamp(type.opacity, 0, 2);
    ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.block_look.opacity")));
    ImGui::SetNextItemWidth(-FLT_MIN);
    {
        // ImGui takes the choices as one string, a NUL after each.
        std::string choices;
        for (const core::TextKey each :
             {ENG_TR("engine.editor.block_look.opaque"), ENG_TR("engine.editor.block_look.cutout"),
              ENG_TR("engine.editor.block_look.translucent")}) {
            choices += core::tr(each);
            choices.push_back('\0');
        }
        changed |= ImGui::Combo("##block-opacity", &opacity, choices.c_str());
    }
    ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.block_look.cutout_keeps_an_image_s_tip")));
    f32 transparency = type.transparency;
    if (opacity == 2) {
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.block_look.transparency")));
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("##block-transparency", &transparency, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("%s", core::tr(ENG_TR("engine.editor.block_look.how_much_shows_through_where_tip")));
    }

    if (changed) {
        g_lookGesture.edited(inspector);
        (void)editor.setBlockTypeLook(world, inspector, selected, textures, opacity, transparency, g_lookGesture.id);
    }
    g_lookGesture.settle(inspector);
}

} // namespace engine::app

#else

namespace engine::app {

void drawTerrainHeightmap(Editor&, scene::World&, core::InstanceId, Inspector&, EditorCommands&)
{}
void drawTerrainSettings(Editor&, scene::World&, core::InstanceId, Inspector&)
{}
void drawBlockLook(Editor&, scene::World&, Inspector&)
{}

} // namespace engine::app

#endif
