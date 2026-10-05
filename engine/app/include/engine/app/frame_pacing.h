// How fast frames are made (ADR 0147, stage G0).
//
// Nothing paced a frame: no vertical sync was asked for by name, no cap
// existed, and a window with no backbuffer -- minimised, or covered -- had
// nothing to wait on at all. The owner's editor ran at two thousand frames a
// second (2026-10-01). As the major engines do it: the display's sync by
// default, a cap a game or a player may set, and a low rate while nobody is
// looking.
//
// **Presentation only.** None of this is read by the simulation: a tick is
// `fixedDt` however often a frame is drawn, and a frame that waits owes the
// same ticks it would have owed.
#pragma once

#include <array>

#include "engine/core/types.h"

namespace engine::app {

using core::f32;
using core::f64;
using core::u32;
using core::u64;

// `[display]` in `project.toml`, the command line over it; `GraphicsService`
// and the player's saved choices over both when ADR 0147's later stages land.
struct FramePacing
{
    // Present on the display's refresh. On by default; always on a phone.
    bool vsync = true;
    // Frames a second at most; 0 is no cap. With `vsync` off it is the only
    // limit, and with it on it caps below the refresh.
    u32 maxFrameRate = 0;
    // The cap while the window is unfocused or minimised; 0 is no throttle.
    u32 backgroundFrameRate = 10;
    // **Paced at a rate the frames can hold** (ADR 0173, `RateGovernor`): a
    // display's refresh over one, two, three or four, stepped down while the
    // frames do not fit and up again when they do with room. A handheld's,
    // unless `[display] adaptive_frame_rate = false`; never a desk's.
    bool adaptive = false;
};

// What a handheld is capped at when its game names no cap (ADR 0173): a
// phone's display refreshes a hundred and twenty times a second, and a game
// drawn that often is a phone too hot to hold in ten minutes.
inline constexpr u32 HandheldFrameRate = 60;

// What the loop knows about its window this frame.
struct FrameWindowState
{
    bool focused = true;
    bool minimized = false;
    // The display's refresh in hertz, 0 when it will not say.
    f32 refreshRate = 0.0f;
    // Whether this frame had a backbuffer to present to. A minimised or
    // covered window has none, and then the display's sync holds nothing.
    bool presented = true;
    // Whether the sync is holding frames to the refresh (`SyncWatch`).
    bool syncHeld = true;
    // Whether this machine is in a networked session -- hosting, serving or
    // joined (NA4). Such a window in the background still runs at the
    // simulation's rate: its snapshots and its intents are other players'
    // game.
    bool networked = false;
};

// **The cap for this frame**, in frames a second; 0 is none.
//
// In the background: the background rate, or the game's own cap if that is
// lower. In front: the game's cap; and where `vsync` is asked for and is not
// holding -- no backbuffer, or a driver set to ignore it -- the display's
// refresh (60 when it will not say), so "match the monitor" is true whatever
// the driver does.
[[nodiscard]] u32 frameCapFor(const FramePacing& pacing, const FrameWindowState& window) noexcept;

// The lower of two caps, where zero is no cap.
[[nodiscard]] u32 lowerRate(u32 a, u32 b) noexcept;

// **How many ticks a capped frame may run to catch up**: `base`, or as many
// as one frame at `capHz` owes and one more. A window throttled to ten frames
// a second owes six ticks a frame at sixty hertz; clamped at the usual four,
// the game behind it would run slow -- and a match would drift from its
// server.
[[nodiscard]] u32 catchUpTicksFor(u32 base, f64 fixedDt, u32 capHz) noexcept;

// **Waits out the rest of a frame's share of a second.**
//
// A deadline that advances by the period, not `now + period`: a frame that
// took 5 ms and one that took 9 both end on the same grid, so the rate is the
// cap and not the cap less the scheduler's rounding. A frame that overran by
// more than a period starts a new grid -- time lost is not owed back, or one
// hitch would be followed by a burst.
class FrameLimiter
{
public:
    // How long to wait now, in nanoseconds, for a cap of `capHz`; 0 for none.
    // Called once a frame, after the frame's work.
    [[nodiscard]] u64 waitNs(u64 nowNs, u32 capHz) noexcept;

private:
    u64 m_deadline = 0;
    u32 m_hz = 0;
};

// **The rates a display shows evenly**: its refresh over one, two, three and
// four, so every frame is on the screen for the same number of refreshes --
// none above `ceilingHz` (0: none above the refresh), none under 24, the
// highest first. A display that will not say its refresh is taken for sixty.
// Where the ceiling is under every one of them, the ceiling alone.
struct EvenRates
{
    std::array<u32, 4> hz{};
    u32 count = 0;

    [[nodiscard]] bool operator==(const EvenRates&) const noexcept = default;
};
[[nodiscard]] EvenRates evenRatesFor(f32 refreshRate, u32 ceilingHz) noexcept;

// **The rate a handheld holds** (ADR 0173, as D558 left it).
//
// A frame that takes 18 ms on a display that refreshes every 8.3 is shown for
// three refreshes, the next -- 16 ms -- for two, and a game at "forty-five
// frames a second" is one that stutters. What every engine's frame pacer does
// about it is choose a rate the frames fit and hold every frame to it: fewer
// frames, each on the screen as long as the last.
//
// Told every frame what it cost, it answers the rate to pace at, one of
// `evenRatesFor`:
//
//   - **Nothing is judged at a start**: not for four seconds after it begins
//     or is `reset` -- a launch, a scene arriving, a curtain lifting -- and a
//     frame of a quarter of a second or more is a hitch, not a rate: it is
//     left out and the second it fell in with it.
//   - **Down on a sustained miss only**: more than three frames in ten late,
//     in each of two seconds running. A game that holds fifty-nine with one
//     frame in twenty doubled is a game at sixty.
//   - **Up by trying**: after five seconds at a rate, when the frame's own
//     work on the CPU fits the rate above, it steps up and sees. Whether the
//     GPU has the room is not something a frame held at a lower rate can say
//     -- a phone slows its GPU's clock when it idles -- so it is found out by
//     running at the rate. A step up taken back within ten seconds doubles the
//     wait before the next, to a minute; half a minute held gives the first
//     wait back.
struct FrameCost
{
    // From this frame's pacing point to the last one's.
    u64 intervalNs = 0;
    // The same, less what pacing itself made it wait: what the frame took,
    // the wait for the display's image and for the GPU included.
    u64 workNs = 0;
    // The same, less every wait -- the image, the present, the pacing: the
    // CPU's part alone.
    u64 cpuNs = 0;
};

class RateGovernor
{
public:
    [[nodiscard]] u32 sample(u64 nowNs, const FrameCost& frame, f32 refreshRate, u32 ceilingHz) noexcept;
    // The rate held now; 0 before the first sample.
    [[nodiscard]] u32 rate() const noexcept { return m_rung < m_ladder.count ? m_ladder.hz[m_rung] : 0; }
    // Forgets what it has seen and starts from the highest rate again, judging
    // nothing for a while: after a loading curtain, whose frames say nothing
    // about the game's.
    void reset() noexcept;

    // **Why the rate last changed**, in the numbers that decided it: a
    // reading from a device should need no guessing. `from` is 0 until a rate
    // has been given up or tried.
    struct Step
    {
        u32 from = 0;
        u32 to = 0;
        // The second that decided it: how many of its frames were late.
        u32 lateFrames = 0;
        u32 frames = 0;
        // The CPU's mean time in a frame over that second.
        u64 meanCpuNs = 0;
    };
    [[nodiscard]] const Step& lastStep() const noexcept { return m_step; }

    // In nanoseconds.
    static constexpr u64 WarmUpNs = 4'000'000'000ull;
    static constexpr u64 HitchNs = 250'000'000ull;
    static constexpr u64 FirstUpDelayNs = 5'000'000'000ull;
    static constexpr u64 MostUpDelayNs = 60'000'000'000ull;

private:
    void restartWindow(u64 nowNs) noexcept;

    EvenRates m_ladder{};
    u32 m_rung = 0;
    // Nothing is judged before this.
    u64 m_quietUntilNs = 0;
    bool m_begun = false;
    u64 m_windowStartNs = 0;
    u32 m_frames = 0;
    // Frames of the window that were late at the rate held, and the CPU's
    // time over all of them.
    u32 m_late = 0;
    u64 m_cpuNs = 0;
    // Seconds in a row that were late.
    u32 m_lateWindows = 0;
    // Since when the rate held has been held, the last step up that has not
    // yet stood half a minute, and how long a rate is held before the one
    // above is tried.
    u64 m_rungSinceNs = 0;
    u64 m_lastUpNs = 0;
    u64 m_upDelayNs = FirstUpDelayNs;
    Step m_step{};
};

// **Whether vertical sync is holding** -- told the length of every presented
// frame, it says no once thirty in a row came in at under two thirds of the
// refresh period, which a present that waited for the display cannot do; and
// yes again as soon as one does not. A driver's control panel can turn sync
// off for an application that asked for it, and nothing reports that it did.
class SyncWatch
{
public:
    void sample(u64 frameNs, f32 refreshRate) noexcept;
    [[nodiscard]] bool held() const noexcept { return m_fast < FastFrames; }

private:
    static constexpr u32 FastFrames = 30;
    u32 m_fast = 0;
};

// **The loading curtain** (D507, ADR 0159): the backdrop a game's world is
// shown from behind once it has arrived -- at the first scene and at every
// scene after it. It lifts when, for three frames running, the loaders have
// nothing in flight, the ground round the camera is drawn as it will be, and
// no script holds it; or when it has waited long enough -- ten seconds, a
// minute while a script holds it -- and then it says it gave up. The game's
// own interface draws over it, so its loading screen is what a player sees.
class LoadingCurtain
{
public:
    struct Frame
    {
        u64 nowNs = 0;
        // Nothing waiting to be read or put up: meshes, pictures.
        bool loadersIdle = true;
        // The ground round the camera drawn as it will be.
        bool groundMeshed = true;
        // `SceneService:HoldLoading` calls not yet released.
        u32 holds = 0;
    };
    enum class Lift
    {
        Kept,
        Ready,
        TimedOut,
    };

    void raise() noexcept;
    [[nodiscard]] bool up() const noexcept { return m_up; }
    // **Up, with nothing left to wait for** (ADR 0176): the frame before found
    // the loaders idle, the ground meshed and no script holding, and the
    // curtain is counting the frames it asks of that before it lifts. From
    // here the world is drawn behind it, so what a first frame costs -- every
    // pipeline it needs, made at first use -- is paid where nobody watches.
    [[nodiscard]] bool settling() const noexcept { return m_up && m_settled > 0; }
    // One frame's word: whether the curtain lifts, and why.
    Lift update(const Frame& frame) noexcept;

    static constexpr u32 SettleFrames = 3;
    static constexpr u64 TimeoutNs = 10'000'000'000ull;
    static constexpr u64 HeldTimeoutNs = 60'000'000'000ull;

private:
    bool m_up = false;
    u64 m_sinceNs = 0;
    u32 m_settled = 0;
};

} // namespace engine::app
