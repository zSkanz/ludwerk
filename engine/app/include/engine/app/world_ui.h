// World-space UI (F3): `BillboardGui` and `SurfaceGui`, placed in the world.
//
// **The UI module lays each tree out and draws it into a canvas; this places
// the canvas.** A canvas is pixels, y down, like the screen's; a placement is
// the world point its top-left corner sits at and the two world vectors one
// canvas pixel steps along. Everything the screen's UI can draw -- panels,
// rounded corners, text, rich text, images -- then lands in the world through
// one affine map, and the renderer draws it in the world pass, hidden by what
// is in front of it (`shaders/src/ui_world.hlsl`).
//
// The placements are pure functions of numbers, in the tradition of the brush
// overlay and the chunk overlay, because a sign that faces the wrong way is a
// bug that reproduces by looking and should be fixed once.
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/render/render_world.h"
#include "engine/render/ui_gradient.h"
#include "engine/rhi/types.h"
#include "engine/ui/ui.h"

namespace engine::scene {
class World;
struct BillboardGuiComponent;
struct SurfaceGuiComponent;
} // namespace engine::scene

namespace engine::app {

// Where a canvas sits: its top-left corner (camera-relative metres), the world
// step of one canvas pixel along x and along y, and the canvas's size in
// pixels. `distance` is from the camera to its centre, for the sort.
struct CanvasPlacement
{
    core::Vec3 topLeft;
    core::Vec3 right;
    core::Vec3 down;
    core::Vec2 canvas;
    core::f32 distance = 0.0f;
};

// A billboard's canvas pixels per metre of `Size.Scale`: what a child's offset
// and a `TextSize` are measured in when the billboard is sized in the world.
inline constexpr core::f32 BillboardPixelsPerMetre = 50.0f;

// A billboard over `anchor` (world metres, the adornee's centre plus its
// offset), facing `camera`. Nothing when it has no size, is behind the camera
// plane, or is past `MaxDistance`.
[[nodiscard]] std::optional<CanvasPlacement> placeBillboard(const scene::BillboardGuiComponent& gui, core::DVec3 anchor,
                                                            const render::RenderCamera& camera, core::Vec2 viewport);

// A surface on one face of a part, lifted a millimetre off it so it never
// fights the part for the same depth.
[[nodiscard]] std::optional<CanvasPlacement> placeSurface(const scene::SurfaceGuiComponent& gui,
                                                          const core::CFrameD& part, core::Vec3 size,
                                                          core::DVec3 cameraOrigin);

// **The frame's gradient table, as rows** (ADR 0110). Every draw list of a
// frame -- the screen's and each canvas in the world -- numbers its gradients
// from one; this gives each distinct one a row of the one table both passes
// read, and bakes the rows the renderer uploads.
class UiGradientRows
{
public:
    void clear();

    // The row `gradient` is drawn from, as the shader's `v`; below zero when
    // the table is full, which draws that quad ungraded and says so once.
    [[nodiscard]] core::f32 rowOf(const ui::DrawGradient& gradient);

    [[nodiscard]] std::span<const core::u8> pixels() const noexcept { return pixels_; }
    [[nodiscard]] core::u32 count() const noexcept { return static_cast<core::u32>(rows_.size()); }

private:
    std::vector<ui::DrawGradient> rows_;
    std::vector<core::u8> pixels_;
    bool warned_ = false;
};

// One corner of `quad`, at upright pixels `(x, y)`: the rounded-corner frame
// the shader measures in -- the quad's own, or for a border stroke its
// element's -- and the gradient and stroke block. `gradientRow` is `rowOf`'s
// answer for the quad's gradient.
void fillUiCorner(const ui::DrawQuad& quad, core::f32 x, core::f32 y, core::f32 gradientRow, core::f32& localX,
                  core::f32& localY, core::f32& halfX, core::f32& halfY, render::UiVertexAppearance& look) noexcept;

// Lays out, draws and places every enabled `BillboardGui` and `SurfaceGui`
// under `workspace` or `uiService`, and appends the result to `out`'s world UI
// geometry, back to front. `textures` is the screen UI's texture table --
// the glyph atlas and the images -- which world UI shares; `gradients` the
// frame's gradient rows, null to draw every gradient as none.
void buildWorldUi(scene::World& world, core::InstanceId workspace, core::InstanceId uiService, core::Vec2 viewport,
                  std::span<const rhi::TextureHandle> textures, ui::DrawList& scratch, render::RenderWorld& out,
                  UiGradientRows* gradients = nullptr);

// What the pointer's ray met in the world's UI: the element, and how far along
// the ray it is.
struct WorldUiPick
{
    core::InstanceId element;
    core::f32 distance = 0.0f;
};

// How far along a camera-relative ray (unit direction) the nearest solid thing
// is, leaving `adornee` out -- the part a canvas is printed on or floats over
// never hides it. Nothing when nothing is in the way. The host answers it from
// the physics world; a test answers it with a lambda.
using SolidAlong =
    std::function<std::optional<core::f32>(core::Vec3 origin, core::Vec3 direction, core::InstanceId adornee)>;

// **The element of a `SurfaceGui` or `BillboardGui` under the pointer** (F3).
// The pointer's ray, through `camera`, is met with every enabled canvas's
// rectangle; where it lands, the canvas is laid out and hit-tested in its own
// pixels exactly as a screen is. A canvas seen from behind is not hit, and one
// with something solid in front of it is not either, unless it is
// `AlwaysOnTop` -- which is also drawn over everything, so it wins over one
// that is not. Otherwise the nearest wins. A canvas's empty space is not a hit:
// the ray goes on to what is behind it.
[[nodiscard]] std::optional<WorldUiPick> pickWorldUi(scene::World& world, core::InstanceId workspace,
                                                     core::InstanceId uiService, core::Vec2 viewport,
                                                     const render::RenderCamera& camera, core::Vec2 pointer,
                                                     const SolidAlong& solidAlong);

} // namespace engine::app
