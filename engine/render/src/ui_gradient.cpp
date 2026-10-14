#include "engine/render/ui_gradient.h"

namespace engine::render {
namespace {

[[nodiscard]] core::u8 toByte(core::f32 value) noexcept
{
    const core::f32 clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<core::u8>(clamped * 255.0f + 0.5f);
}

} // namespace

void bakeUiGradientRow(const core::ColorSequence& color, const core::NumberSequence& transparency,
                       std::span<core::u8> out) noexcept
{
    if (out.size() < UiGradientRowBytes)
        return;
    for (core::u32 texel = 0; texel < UiGradientWidth; ++texel) {
        // The first texel is time 0 and the last is time 1 exactly, so a
        // clamped gradient's end colours are the sequence's own ends.
        const core::f32 time = static_cast<core::f32>(texel) / static_cast<core::f32>(UiGradientWidth - 1);
        const core::Color3 rgb = core::evaluate(color, time);
        const core::f32 alpha = 1.0f - core::evaluate(transparency, time);
        const core::usize at = static_cast<core::usize>(texel) * 4u;
        out[at + 0] = toByte(rgb.r);
        out[at + 1] = toByte(rgb.g);
        out[at + 2] = toByte(rgb.b);
        out[at + 3] = toByte(alpha);
    }
}

} // namespace engine::render
