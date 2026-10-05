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
    m_cpuNs = 0;
}

u32 RateGovernor::sample(u64 nowNs, const FrameCost& frame, f32 refreshRate, u32 ceilingHz) noexcept
{
    // A second of frames is a judgement; fewer than these is not one.
    constexpr u64 WindowNs = NanosPerSecond;
    constexpr u32 FewestFrames = 8;
    // How many seconds in a row must be late before a rate is given up.
    constexpr u32 LateWindows = 2;
    // A step up taken back sooner than this was a step too far.
    constexpr u64 TakenBackNs = 10 * NanosPerSecond;
    // And one that has stood this long has earned the first wait back.
    constexpr u64 SettledNs = 30 * NanosPerSecond;

    const EvenRates ladder = evenRatesFor(refreshRate, ceilingHz);
    if (!m_begun || ladder != m_ladder) {
        // A start, another display mode, or another cap: every judgement so
        // far was of rates that are no longer the choices.
        reset();
        m_begun = true;
        m_ladder = ladder;
        m_quietUntilNs = nowNs + WarmUpNs;
        m_rungSinceNs = nowNs;
        restartWindow(nowNs);
        return rate();
    }

    // **A hitch is not a rate** (D558): a scene's load, a shader made at first
    // use. It is left out, and so is the second it fell in -- the frames round
    // a hitch are its aftermath.
    if (frame.intervalNs >= HitchNs) {
        m_quietUntilNs = std::max(m_quietUntilNs, nowNs + WindowNs);
        m_lateWindows = 0;
        restartWindow(nowNs);
        return rate();
    }
    if (nowNs < m_quietUntilNs) {
        restartWindow(nowNs);
        return rate();
    }

    const auto periodOf = [this](u32 rung) { return NanosPerSecond / m_ladder.hz[rung]; };
    const u64 period = periodOf(m_rung);
    // At the display's own rate the display paces, and a late frame is one
    // that was on the screen for a refresh more: by the interval. At a rate
    // under it the frame is held to its place, and a late one is one whose
    // work overran the place: by what it took before the hold.
    const u32 refresh = refreshRate >= 1.0f ? static_cast<u32>(std::lround(refreshRate)) : AssumedRefresh;
    const bool displayPaced = m_ladder.hz[m_rung] + 1 >= refresh;
    const bool late = displayPaced ? frame.intervalNs > period + period / 4 : frame.workNs > period + period / 20;
    ++m_frames;
    m_late += late ? 1u : 0u;
    m_cpuNs += frame.cpuNs;

    if (nowNs - m_windowStartNs < WindowNs || m_frames < FewestFrames)
        return rate();

    // More than three in ten.
    const bool lateWindow = m_late * 10 > m_frames * 3;
    const u64 meanCpuNs = m_cpuNs / m_frames;
    // No more than one in ten, for a rate to be called held.
    const bool heldWindow = m_late * 10 <= m_frames;

    if (lateWindow) {
        if (++m_lateWindows >= LateWindows && m_rung + 1 < m_ladder.count) {
            m_step = Step{m_ladder.hz[m_rung], m_ladder.hz[m_rung + 1], m_late, m_frames, meanCpuNs};
            ++m_rung;
            if (m_lastUpNs != 0 && nowNs - m_lastUpNs < TakenBackNs)
                m_upDelayNs = std::min(m_upDelayNs * 2, MostUpDelayNs);
            m_lastUpNs = 0;
            m_rungSinceNs = nowNs;
            m_lateWindows = 0;
        }
    }
    else {
        m_lateWindows = 0;
        // The rate above is tried when this one has been held for the wait,
        // is being held now, and the CPU's own part of a frame fits it with a
        // tenth to spare. The rest is found out by running at it.
        if (m_rung > 0 && heldWindow && nowNs - m_rungSinceNs >= m_upDelayNs &&
            meanCpuNs * 10 <= periodOf(m_rung - 1) * 9) {
            m_step = Step{m_ladder.hz[m_rung], m_ladder.hz[m_rung - 1], m_late, m_frames, meanCpuNs};
            --m_rung;
            m_lastUpNs = nowNs;
            m_rungSinceNs = nowNs;
        }
        else if (m_lastUpNs != 0 && nowNs - m_lastUpNs > SettledNs) {
            m_upDelayNs = FirstUpDelayNs;
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
