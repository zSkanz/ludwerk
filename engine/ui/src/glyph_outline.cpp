#include "engine/ui/glyph_outline.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace engine::ui {
namespace {

using core::f32;
using core::i32;
using core::u32;
using core::u8;
using core::usize;

// How much of a texel `(dx, dy)` away reaches this one: one inside the stroke,
// nothing outside it, and the part between over the one texel of its edge.
[[nodiscard]] f32 reachOf(i32 dx, i32 dy, f32 radius, u32 join) noexcept
{
    const auto ax = static_cast<f32>(std::abs(dx));
    const auto ay = static_cast<f32>(std::abs(dy));
    f32 distance = std::sqrt(ax * ax + ay * ay);
    if (join == 2)
        distance = std::fmax(ax, ay);
    else if (join == 1)
        distance = std::fmax(std::fmax(ax, ay), (ax + ay) * 0.70710678f);
    return std::fmin(std::fmax(radius + 0.5f - distance, 0.0f), 1.0f);
}

// `into[x] = max(into[x], from[x])`, a row at a time.
void raiseRow(u8* into, const u8* from, u32 count) noexcept
{
    for (u32 x = 0; x < count; ++x)
        into[x] = std::max(into[x], from[x]);
}

} // namespace

// **The same bytes as trying every tap at every texel, at a fraction of the
// work** (D563). That is what this did: a stroke of ten pixels is some three
// hundred taps a texel, a title's glyph fifteen thousand texels, and the
// frame an interface first showed its outlined text was 16 and 32 ms on a
// phone -- nearly all of what `ui.drawlist` cost there.
//
// The kernel is two things. **Its inside, where a tap counts whole**, is a run
// of texels on each row of the kernel, so the largest coverage under it is the
// largest of a few row-wise running maxima -- bytes, a row at a time. **Its
// edge, where a tap counts in part**, is a ring one texel wide, and it is
// tried only where it could change the answer: where the largest coverage
// under the WHOLE kernel is more than the inside found. Inside a letter and
// across the body of its outline the two are equal; they differ along the
// outline's outer edge alone.
void dilateCoverage(const std::vector<core::u8>& coverage, core::u32 width, core::u32 height, core::f32 radius,
                    core::u32 join, core::u32 pad, std::vector<core::u8>& out)
{
    const u32 outWidth = width + pad * 2;
    const u32 outHeight = height + pad * 2;
    out.assign(static_cast<usize>(outWidth) * outHeight, 0u);
    if (width == 0 || height == 0)
        return;

    const auto reach = static_cast<i32>(std::ceil(radius)) + 1;
    const auto rows = static_cast<usize>(reach) * 2 + 1;

    // Each row of the kernel: how far its whole taps run to either side of
    // the middle, and how far any tap does; -1 where the row has none. A tap
    // that counts in part -- or whole, outside the run -- is on the list.
    struct Tap
    {
        i32 dx = 0;
        i32 dy = 0;
        f32 weight = 0.0f;
    };
    std::vector<i32> whole(rows, -1);
    std::vector<i32> any(rows, -1);
    std::vector<Tap> edge;
    for (i32 dy = -reach; dy <= reach; ++dy) {
        const auto row = static_cast<usize>(dy + reach);
        while (whole[row] < reach && reachOf(whole[row] + 1, dy, radius, join) >= 1.0f)
            ++whole[row];
        for (i32 dx = -reach; dx <= reach; ++dx) {
            const f32 weight = reachOf(dx, dy, radius, join);
            if (weight <= 0.0f)
                continue;
            any[row] = std::max(any[row], std::abs(dx));
            if (std::abs(dx) > whole[row])
                edge.push_back(Tap{dx, dy, weight});
        }
    }

    // The coverage with nothing round it for as far as a tap reaches from any
    // texel of the answer: no read below needs to ask where it is.
    const auto margin = static_cast<u32>(reach);
    const u32 wideWidth = outWidth + margin * 2;
    const u32 wideHeight = outHeight + margin * 2;
    std::vector<u8> source(static_cast<usize>(wideWidth) * wideHeight, 0u);
    for (u32 y = 0; y < height; ++y) {
        std::copy_n(coverage.data() + static_cast<usize>(y) * width, width,
                    source.data() + static_cast<usize>(y + pad + margin) * wideWidth + pad + margin);
    }

    // `spread` is each texel's largest coverage within `run` texels along its
    // row, for `run` from nothing up: a row of the kernel that is `run` wide
    // reads it, a row of texels at a time.
    std::vector<u8> spread = source;
    std::vector<u8> wider(spread.size(), 0u);
    std::vector<u8> inside(out.size(), 0u);
    std::vector<u8> under(out.size(), 0u);
    for (i32 run = 0; run <= reach; ++run) {
        for (i32 dy = -reach; dy <= reach; ++dy) {
            const auto row = static_cast<usize>(dy + reach);
            const bool isWhole = whole[row] == run;
            const bool isAny = any[row] == run;
            if (!isWhole && !isAny)
                continue;
            for (u32 y = 0; y < outHeight; ++y) {
                const u8* from =
                    spread.data() + static_cast<usize>(static_cast<i32>(y + margin) + dy) * wideWidth + margin;
                if (isWhole)
                    raiseRow(inside.data() + static_cast<usize>(y) * outWidth, from, outWidth);
                if (isAny)
                    raiseRow(under.data() + static_cast<usize>(y) * outWidth, from, outWidth);
            }
        }
        if (run == reach)
            break;
        for (u32 y = 0; y < wideHeight; ++y) {
            const u8* from = spread.data() + static_cast<usize>(y) * wideWidth;
            u8* into = wider.data() + static_cast<usize>(y) * wideWidth;
            into[0] = std::max(from[0], wideWidth > 1 ? from[1] : u8{0});
            for (u32 x = 1; x + 1 < wideWidth; ++x)
                into[x] = std::max(std::max(from[x - 1], from[x]), from[x + 1]);
            if (wideWidth > 1)
                into[wideWidth - 1] = std::max(from[wideWidth - 2], from[wideWidth - 1]);
        }
        spread.swap(wider);
    }

    for (u32 y = 0; y < outHeight; ++y) {
        for (u32 x = 0; x < outWidth; ++x) {
            const usize at = static_cast<usize>(y) * outWidth + x;
            if (inside[at] == under[at]) {
                // Nothing under the kernel is more than its inside found, and
                // a tap that counts in part counts for less than it reads.
                out[at] = inside[at];
                continue;
            }
            auto best = static_cast<f32>(inside[at]);
            const u8* middle = source.data() + static_cast<usize>(y + margin) * wideWidth + x + margin;
            for (const Tap& tap : edge) {
                const f32 value = static_cast<f32>(middle[tap.dy * static_cast<i32>(wideWidth) + tap.dx]) * tap.weight;
                best = std::fmax(best, value);
            }
            out[at] = static_cast<u8>(std::fmin(best + 0.5f, 255.0f));
        }
    }
}

} // namespace engine::ui
