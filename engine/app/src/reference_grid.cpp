#include "engine/app/reference_grid.h"

#include <algorithm>
#include <cmath>

#include "engine/render/debug_draw.h"

namespace engine::app {

using core::f32;
using core::f64;
using core::i32;
using core::i64;

namespace {

// Dim, because a grid is a reference and not a subject: it has to be readable
// under the thing being placed rather than over it.
constexpr f32 kLineColor[3] = {0.30f, 0.32f, 0.36f};
// Every tenth line, brighter, so the eye can count without following one line
// across the whole picture. Ten because the step is nearly always a round
// number and ten of a round number is another one.
constexpr f32 kMajorColor[3] = {0.46f, 0.49f, 0.55f};
constexpr i32 kMajorEvery = 10;
// Pieces a line is cut into on each side of the centre, so its alpha can fall
// off with distance: the grid thins into the distance rather than ending at an
// edge that moves with the camera.
constexpr i32 kSegments = 6;

// `major` 0 is the minor lines' colour and 1 the major lines'.
[[nodiscard]] render::DebugColor lineColor(f32 major, f32 alpha) noexcept
{
    const auto mix = [major](int channel) {
        return kLineColor[channel] + (kMajorColor[channel] - kLineColor[channel]) * major;
    };
    return render::DebugColor::fromLinear(mix(0), mix(1), mix(2), alpha);
}

// 1 under the camera, 0 at `radius` and beyond, smoothly.
[[nodiscard]] f32 fadeAt(f64 x, f64 z, core::DVec3 focus, f64 radius) noexcept
{
    const f64 distance = std::sqrt((x - focus.x) * (x - focus.x) + (z - focus.z) * (z - focus.z));
    const f64 near = std::clamp(1.0 - distance / radius, 0.0, 1.0);
    return static_cast<f32>(near * near * (3.0 - 2.0 * near));
}

// One tier of the grid: every line `spacing` apart around `focus`, fading out
// at the tier's reach. `skipMajors` leaves out the lines the next tier up
// draws, so no line is drawn twice and brightened by the blend.
void drawTier(core::DVec3 focus, f64 groundY, f64 spacing, f32 major, f32 alpha, bool skipMajors,
              render::DebugDraw& draw)
{
    // **Snapped to the spacing**, so the lines land where the snap does. A grid
    // centred on the camera would slide under the thing being placed and line
    // up with nothing.
    const f64 centreX = std::floor(focus.x / spacing) * spacing;
    const f64 centreZ = std::floor(focus.z / spacing) * spacing;
    const f64 half = spacing * static_cast<f64>(kGridHalfLines);
    const f64 piece = half / static_cast<f64>(kSegments);

    const auto segmented = [&](bool alongZ, f64 fixed, f64 from) {
        for (i32 part = 0; part < kSegments * 2; ++part) {
            const f64 a = from + piece * static_cast<f64>(part);
            const f64 b = a + piece;
            const core::DVec3 start = alongZ ? core::DVec3{fixed, groundY, a} : core::DVec3{a, groundY, fixed};
            const core::DVec3 end = alongZ ? core::DVec3{fixed, groundY, b} : core::DVec3{b, groundY, fixed};
            const f32 fadeStart = fadeAt(start.x, start.z, focus, half) * alpha;
            const f32 fadeEnd = fadeAt(end.x, end.z, focus, half) * alpha;
            draw.line(core::toVec3(start), core::toVec3(end), lineColor(major, fadeStart), lineColor(major, fadeEnd));
        }
    };

    for (i32 index = -kGridHalfLines; index <= kGridHalfLines; ++index) {
        const f64 offset = static_cast<f64>(index) * spacing;
        // Counted from the WORLD origin rather than from the camera, so the
        // lines a tier leaves to the next one stay on the same world
        // coordinates as the view moves.
        const auto worldIndexX = static_cast<i64>(std::llround((centreX + offset) / spacing));
        const auto worldIndexZ = static_cast<i64>(std::llround((centreZ + offset) / spacing));
        if (!skipMajors || worldIndexX % kMajorEvery != 0)
            segmented(true, centreX + offset, centreZ - half);
        if (!skipMajors || worldIndexZ % kMajorEvery != 0)
            segmented(false, centreZ + offset, centreX - half);
    }
}

} // namespace

f32 referenceGridStep(f32 step, f64 reach) noexcept
{
    if (!(step > 0.0f) || !(reach > 0.0))
        return step;

    // **Multiplied by ten until it fits, rather than divided into a fixed
    // count.** A grid whose spacing is the reach over sixty lines is a different
    // grid at every camera height, and one that moves as you fly is not a
    // reference. Powers of ten keep every line drawn a line the snap would also
    // have produced -- so what is on screen is always a subset of the real grid,
    // never a rounder one beside it.
    f32 spacing = step;
    const auto span = static_cast<f64>(kGridHalfLines);
    for (int decade = 0; decade < 8 && static_cast<f64>(spacing) * span < reach; ++decade)
        spacing *= 10.0f;
    return spacing;
}

void drawReferenceGrid(core::DVec3 focus, f64 groundY, f32 step, render::DebugDraw& draw)
{
    if (!(step > 0.0f))
        return;

    // How far out the grid needs to reach to be useful: proportional to how high
    // the camera is, because that is what decides how much ground is on screen.
    // Floored so a camera at ground level still has a grid around it.
    const f64 reach = std::max(std::abs(focus.y - groundY) * 3.0, 8.0);

    // **Three tiers a decade apart, blended by height rather than switched**
    // (the owner: the grid changed as the camera moved in and out). It used to
    // jump to ten times the spacing at one height, and every line on screen
    // moved at once. Now the finest tier fades out over the decade the camera
    // climbs through, the middle one dims from major to minor as it takes over
    // from it, and the coarsest stays major -- so at the top of a decade the
    // picture is exactly the bottom of the next one, and nothing pops. Every
    // line is still a multiple of `step`.
    const f64 base = static_cast<f64>(step);
    const f64 decades = std::log10(reach / (base * static_cast<f64>(kGridHalfLines)));
    const f64 clamped = std::clamp(decades, 0.0, 8.0);
    const f64 whole = std::floor(clamped);
    const auto climb = static_cast<f32>(clamped - whole);
    const f64 spacing = base * std::pow(10.0, whole);

    drawTier(focus, groundY, spacing, 0.0f, 1.0f - climb, true, draw);
    drawTier(focus, groundY, spacing * 10.0, 1.0f - climb, 1.0f, true, draw);
    drawTier(focus, groundY, spacing * 100.0, 1.0f, 1.0f, false, draw);
}

} // namespace engine::app
