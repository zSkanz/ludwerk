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

u32 lowerRate(u32 a, u32 b) noexcept
{
    return lowerCap(a, b);
}

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

EvenRates evenRatesFor(f32 refreshRate, u32 ceilingHz) noexcept
{
    const u32 refresh = refreshRate >= 1.0f ? static_cast<u32>(std::lround(refreshRate)) : AssumedRefresh;
    EvenRates rates;
    for (u32 divisor = 1; divisor <= 4; ++divisor) {
        const u32 rate = static_cast<u32>(std::lround(static_cast<f64>(refresh) / static_cast<f64>(divisor)));
        if (rate < 24 || (ceilingHz != 0 && rate > ceilingHz))
            continue;
        rates.hz[rates.count++] = rate;
    }
    if (rates.count == 0)
        rates.hz[rates.count++] = ceilingHz != 0 ? std::min(ceilingHz, refresh) : refresh;
    return rates;
}

void RateGovernor::reset() noexcept
{
    *this = RateGovernor{};
}

void RateGovernor::restartWindow(u64 nowNs) noexcept
{
    m_windowStartNs = nowNs;
    m_frames = 0;
    m_late = 0;
    m_fits = {};
}

u32 RateGovernor::sample(u64 nowNs, u64 workNs, f32 refreshRate, u32 ceilingHz) noexcept
{
    // A second of frames is a judgement; fewer than these is not one.
    constexpr u64 WindowNs = NanosPerSecond;
    constexpr u32 FewestFrames = 8;
    // A step up taken back sooner than this was a step too far.
    constexpr u64 TakenBackNs = 10 * NanosPerSecond;
    // And one that has stood this long has earned the first wait back.
    constexpr u64 SettledNs = 60 * NanosPerSecond;

    const EvenRates ladder = evenRatesFor(refreshRate, ceilingHz);
    if (ladder != m_ladder) {
        // Another display mode, or another cap: every judgement so far was of
        // rates that are no longer the choices.
        reset();
        m_ladder = ladder;
        restartWindow(nowNs);
    }
    if (m_windowStartNs == 0)
        restartWindow(nowNs);

    const auto periodOf = [this](u32 rung) { return NanosPerSecond / m_ladder.hz[rung]; };
    ++m_frames;
    // Late by more than a twentieth of the period: a frame that took 16.9 ms
    // of 16.7 was shown on time by any display.
    const u64 period = periodOf(m_rung);
    if (workNs > period + period / 20)
        ++m_late;
    // With a fifth to spare: a rate held with nothing over is a rate lost at
    // the first explosion.
    for (u32 rung = 0; rung < m_rung; ++rung) {
        if (workNs * 5 <= periodOf(rung) * 4)
            ++m_fits[rung];
    }

    if (nowNs - m_windowStartNs < WindowNs || m_frames < FewestFrames)
        return rate();

    const bool late = m_late * 5 > m_frames;
    if (late) {
        m_quiet = 0;
        if (m_rung + 1 < m_ladder.count) {
            ++m_rung;
            if (m_lastUpNs != 0 && nowNs - m_lastUpNs < TakenBackNs)
                m_upDelay = std::min(m_upDelay * 2, MostUpDelay);
            m_lastUpNs = 0;
        }
    }
    else {
        // The highest rate nineteen frames in twenty of this window fitted.
        u32 candidate = m_rung;
        for (u32 rung = 0; rung < m_rung; ++rung) {
            if (m_fits[rung] * 20 >= m_frames * 19) {
                candidate = rung;
                break;
            }
        }
        if (candidate < m_rung) {
            m_quietRung = m_quiet == 0 ? candidate : std::max(m_quietRung, candidate);
            if (++m_quiet >= m_upDelay) {
                m_rung = m_quietRung;
                m_lastUpNs = nowNs;
                m_quiet = 0;
            }
        }
        else {
            m_quiet = 0;
        }
        if (m_lastUpNs != 0 && nowNs - m_lastUpNs > SettledNs) {
            m_upDelay = FirstUpDelay;
            m_lastUpNs = 0;
        }
    }
    restartWindow(nowNs);
    return rate();
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

void LoadingCurtain::raise() noexcept
{
    m_up = true;
    m_sinceNs = 0;
    m_settled = 0;
}

LoadingCurtain::Lift LoadingCurtain::update(const Frame& frame) noexcept
{
    if (!m_up)
        return Lift::Kept;
    if (m_sinceNs == 0)
        m_sinceNs = frame.nowNs;
    const bool ready = frame.loadersIdle && frame.groundMeshed && frame.holds == 0;
    m_settled = ready ? m_settled + 1 : 0;
    if (m_settled >= SettleFrames) {
        m_up = false;
        return Lift::Ready;
    }
    if (frame.nowNs - m_sinceNs > (frame.holds > 0 ? HeldTimeoutNs : TimeoutNs)) {
        m_up = false;
        return Lift::TimedOut;
    }
    return Lift::Kept;
}

} // namespace engine::app
