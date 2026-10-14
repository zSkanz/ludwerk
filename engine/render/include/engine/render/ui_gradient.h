// The gradient table the UI samples (ADR 0110).
//
// **One texture for every gradient in a frame**: a row per distinct gradient,
// `UiGradientWidth` texels of colour and opacity along it, and a quad names
// its row. The shader turns a fragment's place in its element into a position
// along the sequence, tiles it, and reads the row there -- so a gradient of
// twenty stops costs the same fetch as one of two.
//
// Here, in `render`, because a row is pixels and pixels are this module's; the
// UI hands over sequences and never learns what a texel is.
#pragma once

#include <span>

#include "engine/core/sequence.h"
#include "engine/core/types.h"

namespace engine::render {

// 256 texels: a hard stop softens over a 256th of the span, which is the
// resolution the gradients this API comes from document as theirs.
inline constexpr core::u32 UiGradientWidth = 256;
// Distinct gradients in one frame. Past it the rest draw ungraded, and the
// frame says so once.
inline constexpr core::u32 UiGradientRows = 256;
// RGBA8, so four bytes a texel.
inline constexpr core::usize UiGradientRowBytes = static_cast<core::usize>(UiGradientWidth) * 4u;

// Writes one row: the colour sequence as it is stored (the UI's colours are
// the screen's), and one minus the transparency sequence as alpha. `out` is
// `UiGradientRowBytes` long.
void bakeUiGradientRow(const core::ColorSequence& color, const core::NumberSequence& transparency,
                       std::span<core::u8> out) noexcept;

// **What a UI vertex carries for a gradient and a stroke** (ADR 0110), the
// same block at the end of the screen's vertex and the world's, read by the
// same functions in `engine_ui.hlsli`. Four attributes: two `Float4`s of
// gradient, one of the gradient's offset and the stroke's band, and the join.
struct UiVertexAppearance
{
    // Where this vertex is from the gradient box's centre, and the box's
    // half-size -- upright pixels, before any turn, so the gradient turns
    // with its element.
    core::f32 gradientX = 0.0f;
    core::f32 gradientY = 0.0f;
    core::f32 gradientHalfX = 0.0f;
    core::f32 gradientHalfY = 0.0f;
    // The row's `v` in the table, below zero for none; the shape plus four
    // times the tiling; the angle in radians; the scale.
    core::f32 gradientRow = -1.0f;
    core::f32 gradientKind = 0.0f;
    core::f32 gradientAngle = 0.0f;
    core::f32 gradientScale = 1.0f;
    // The offset in pixels, then the stroke's band in pixels from its box's
    // edge; an outer at or below the inner is no stroke.
    core::f32 gradientOffsetX = 0.0f;
    core::f32 gradientOffsetY = 0.0f;
    core::f32 strokeInner = 0.0f;
    core::f32 strokeOuter = -1.0f;
    // `Enum.LineJoinMode`.
    core::f32 strokeJoin = 0.0f;
};

static_assert(sizeof(UiVertexAppearance) == 52, "the UI appearance block is an ABI decision the shaders share");

// Where a quad's row sits in the table, as the shader's `v`: the centre of the
// row, so bilinear sampling never reaches a neighbour.
[[nodiscard]] constexpr core::f32 uiGradientRowV(core::u32 row) noexcept
{
    return (static_cast<core::f32>(row) + 0.5f) / static_cast<core::f32>(UiGradientRows);
}

} // namespace engine::render
