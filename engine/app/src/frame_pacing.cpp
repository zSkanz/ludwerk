#include "engine/app/frame_pacing.h"

#include <algorithm>
#include <cmath>

namespace engine::app {

namespace {

constexpr u64 NanosPerSecond = 1'000'000'000ull;
// The refresh assumed of a display that will not say.
constexpr u32 AssumedRefresh = 60;

[[nodiscard]] u32 lowerCap(u32 a, u32 b) noexcept
{
    if (a == 0)
        return b;
    if (b == 0)
        return a;
    return std::min(a, b);
}

} // namespace

// What a window in the background is held to at least while it is in a
// networked session: the simulation's own sixty a second.
constexpr u32 NetworkedBackgroundRate = 60;

u32 frameCapFor(const FramePacing& pacing, const FrameWindowState& window) noexcept
{
    if ((!window.focused || window.minimized) && pacing.backgroundFrameRate != 0) {
        // **Never below the simulation's rate in a networked session** (NA4):
        // a host behind another window at ten frames a second sent its
        // snapshots in bursts a tenth of a second apart, longer than every
        // client's interpolation delay, and each saw the others freeze and
        // jump; a client there sent its intents six at a time.
        const u32 background = window.networked ? std::max(pacing.backgroundFrameRate, NetworkedBackgroundRate)
                                                : pacing.backgroundFrameRate;
        return lowerCap(background, pacing.maxFrameRate);
    }

    u32 cap = pacing.maxFrameRate;
    if (pacing.vsync && (!window.presented || !window.syncHeld)) {
        const u32 refresh =
            window.refreshRate >= 1.0f ? static_cast<u32>(std::lround(window.refreshRate)) : AssumedRefresh;
        cap = lowerCap(cap, refresh);
    }
    return cap;
}

u32 catchUpTicksFor(u32 base, f64 fixedDt, u32 capHz) noexcept
{
    if (capHz == 0 || !(fixedDt > 0.0))
        return base;
    // Less a hair: sixty ticks at ten frames is six, not seven by rounding.
    const f64 owed = std::ceil(1.0 / (static_cast<f64>(capHz) * fixedDt) - 1e-9);
    // Bounded: a cap of one frame a second is sixty ticks, and no more than
    // that is ever run in a frame whatever is asked.
    const u32 ticks = static_cast<u32>(std::clamp(owed, 1.0, 64.0)) + 1;
    return std::max(base, ticks);
}

u64 FrameLimiter::waitNs(u64 nowNs, u32 capHz) noexcept
{
    if (capHz == 0) {
        m_deadline = 0;
        m_hz = 0;
        return 0;
    }
    const u64 period = NanosPerSecond / capHz;
    // A new cap, a first frame, or a frame that overran by more than a
    // period: the grid starts here.
    if (m_hz != capHz || m_deadline == 0 || nowNs > m_deadline + period) {
        m_hz = capHz;
        m_deadline = nowNs + period;
        return 0;
    }
    if (nowNs >= m_deadline) {
        // Late, by less than a period: no wait, and the grid keeps its place.
        m_deadline += period;
        return 0;
    }
    const u64 wait = m_deadline - nowNs;
    m_deadline += period;
    return wait;
}

void SyncWatch::sample(u64 frameNs, f32 refreshRate) noexcept
{
    if (!(refreshRate >= 1.0f)) {
        m_fast = 0;
        return;
    }
    const f64 period = static_cast<f64>(NanosPerSecond) / static_cast<f64>(refreshRate);
    if (static_cast<f64>(frameNs) < period * (2.0 / 3.0))
        m_fast = std::min(m_fast + 1, FastFrames);
    else
        m_fast = 0;
}

} // namespace engine::app
