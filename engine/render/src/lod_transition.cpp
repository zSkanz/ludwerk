#include "engine/render/lod_transition.h"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

using core::f32;
using core::f64;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

// A draw's key: its instance and its section, so a mesh of several sections
// keeps one state each (they agree, being one mesh at one distance).
[[nodiscard]] u64 keyOf(const DrawItem& draw) noexcept
{
    return draw.motionKey * 64u + (draw.section & 63u);
}

} // namespace

LodTransitionStats LodTransitions::apply(RenderWorld& world, const MeshCache& meshes, f32 pixelsPerUnit,
                                         const GraphicsSettings& settings, bool crossfade, f64 nowSeconds)
{
    LodTransitionStats stats;
    ++frame_;
    if (pixelsPerUnit <= 0.0f)
        return stats;
    const f32 viewDistance = settings.viewDistance > 0.0f ? settings.viewDistance : 1.0f;
    const f32 smallest = SmallestPixels / viewDistance;

    const auto stateOf = [&](u64 key) -> State* {
        const auto at = std::lower_bound(states_.begin(), states_.end(), key,
                                         [](const State& held, u64 probe) { return held.key < probe; });
        return at != states_.end() && at->key == key ? &*at : nullptr;
    };
    std::vector<State> added;

    // The outgoing levels' draws, each after the draw it fades into.
    std::vector<std::pair<usize, DrawItem>> outgoings;
    for (usize index = 0; index < world.draws.size(); ++index) {
        DrawItem& draw = world.draws[index];
        // Only what a part draws: the ground, blocks and anything without an
        // instance choose their own levels, or have none.
        if (draw.motionKey == 0 || draw.terrain || draw.voxelBlock || draw.transparent)
            continue;
        const MeshCache::Resolved* resolved = meshes.resolve(draw.mesh);

        // **Too small to see**: left out. A cut, not a fade: what is two
        // pixels across has nothing to pop, and a fading draw is drawn alone,
        // out of its instanced run -- a forest's far trees, one call each.
        if (draw.inCameraFrustum && draw.boundsRadius > 0.0f) {
            const f32 distance = std::sqrt(core::dot(draw.boundsCenter, draw.boundsCenter));
            if (distance > draw.boundsRadius && 2.0f * draw.boundsRadius * pixelsPerUnit / distance < smallest) {
                draw.inCameraFrustum = false;
                ++stats.tooSmall;
            }
        }

        u32 target = 0;
        if (resolved != nullptr && resolved->lods.size() > 1 && !draw.finestLod)
            target = selectDrawLod(*resolved, draw, pixelsPerUnit, settings);
        const u64 key = keyOf(draw);
        State* state = stateOf(key);
        if (state == nullptr) {
            added.push_back(State{key, target, target, -1.0, frame_});
            draw.lodFixed = static_cast<u8>(std::min<u32>(target, 254u));
        }
        else {
            // **Hysteresis**: the level changes only past a band either side.
            u32 wanted = state->lod;
            if (resolved != nullptr && resolved->lods.size() > 1 && !draw.finestLod) {
                if (target > state->lod) {
                    const u32 nearer = selectDrawLod(*resolved, draw, pixelsPerUnit * (1.0f + Hysteresis), settings);
                    if (nearer > state->lod)
                        wanted = nearer;
                }
                else if (target < state->lod) {
                    const u32 further = selectDrawLod(*resolved, draw, pixelsPerUnit / (1.0f + Hysteresis), settings);
                    if (further < state->lod)
                        wanted = further;
                }
            }
            else {
                wanted = target;
            }
            if (wanted != state->lod) {
                state->from = state->lod;
                state->lod = wanted;
                state->since = crossfade ? nowSeconds : -1.0;
                ++stats.changed;
            }
            state->seen = frame_;
            draw.lodFixed = static_cast<u8>(std::min<u32>(state->lod, 254u));
        }
        ++stats.tracked;

        // **The crossfade**: both levels, each through the other's holes.
        const State* now = state;
        const bool fadingLevel =
            now != nullptr && now->since >= 0.0 && nowSeconds - now->since < FadeSeconds && now->from != now->lod &&
            draw.inCameraFrustum && !draw.cutout &&
            (draw.material >= world.materials.size() || world.materials[draw.material].surface.empty());
        if (fadingLevel) {
            const f32 share = static_cast<f32>((nowSeconds - now->since) / FadeSeconds);
            draw.lodFade = encodeLodFade(share, true);
            DrawItem outgoing = draw;
            outgoing.lodFixed = static_cast<u8>(std::min<u32>(now->from, 254u));
            outgoing.lodFade = encodeLodFade(1.0f - share, false);
            outgoing.castShadow = false;
            outgoings.emplace_back(index, outgoing);
            ++stats.fading;
            continue;
        }
    }
    if (!outgoings.empty()) {
        scratch_.clear();
        scratch_.reserve(world.draws.size() + outgoings.size());
        usize next = 0;
        for (usize index = 0; index < world.draws.size(); ++index) {
            scratch_.push_back(world.draws[index]);
            while (next < outgoings.size() && outgoings[next].first == index)
                scratch_.push_back(outgoings[next++].second);
        }
        world.draws.swap(scratch_);
    }

    // New ones in, and the long unseen out, in key order (R10).
    if (!added.empty()) {
        states_.insert(states_.end(), added.begin(), added.end());
        std::sort(states_.begin(), states_.end(), [](const State& a, const State& b) { return a.key < b.key; });
        states_.erase(
            std::unique(states_.begin(), states_.end(), [](const State& a, const State& b) { return a.key == b.key; }),
            states_.end());
    }
    if (frame_ % ForgetFrames == 0)
        std::erase_if(states_, [&](const State& held) { return frame_ - held.seen > ForgetFrames; });
    return stats;
}

} // namespace engine::render
