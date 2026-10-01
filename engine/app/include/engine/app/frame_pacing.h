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
};

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
};

// **The cap for this frame**, in frames a second; 0 is none.
//
// In the background: the background rate, or the game's own cap if that is
// lower. In front: the game's cap; and where `vsync` is asked for and is not
// holding -- no backbuffer, or a driver set to ignore it -- the display's
// refresh (60 when it will not say), so "match the monitor" is true whatever
// the driver does.
[[nodiscard]] u32 frameCapFor(const FramePacing& pacing, const FrameWindowState& window) noexcept;

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

} // namespace engine::app
