# 0173 — The rate a handheld holds: a cap of sixty, a rate stepped to what the frames fit, and the wait at the present

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, in the mobile performance batch (ludwerk-08, on the
  owner's request that the engine be made fast on a phone), under the
  standing rule to decide as professional engines do and record it
- Builds on: ADR 0147 (settings; stage G0, how fast frames are made), the
  mobile ledger's open item "the frame rate a phone runs at"

## Context

A phone's display refreshes a hundred and twenty times a second and its
window always waits for it. Nothing else paced a phone's frames: no cap
unless the game set one, and no notion of a rate the frames fit.

Measured on the owner's phone with the game's own cap of sixty: a median
frame of 16.2 ms and a 95th percentile of 29 to 32 -- one frame in twenty
shown for twice as long as its neighbours, at fifty-nine frames a second. At
the phone's default level the same game ran between twenty-one and
thirty-five: every frame shown for three, four or five refreshes as its work
happened to end.

A frame rate is felt as its unevenness before it is felt as its number.
Thirty frames a second, each on the screen for the same four refreshes, reads
as smooth; thirty-five made of threes, fours and fives does not.

## How mature engines do it

- **Unity**: `Application.targetFrameRate` is 30 on a phone unless the game
  says; "Optimized Frame Pacing" (Android's Swappy) holds each frame to a
  whole number of refreshes.
- **Unreal**: a frame pacer on Android (`r.SetFramePace`), choosing a rate
  that divides the refresh -- 120, 60, 40, 30, 20 -- and, with it on, the
  display is asked for a matching mode.
- **Godot**: Swappy since 4.4, with a "full sync" and "pipeline" mode.
- **Android itself** (the Frame Pacing library's guide): pick the swap
  interval the game sustains, hold it, and tell the platform the intended
  rate with `Surface.setFrameRate`.

All of them: a rate that divides the refresh, held, and the display told.

## Decision

1. **A handheld with no cap of its own is capped at sixty**
   (`HandheldFrameRate`). `[display] max_frame_rate` says otherwise -- a game
   that wants a hundred and twenty asks for it. A desk's default is still no
   cap.

2. **The rate is one the display shows evenly**: its refresh over one, two,
   three or four -- 60, 40 and 30 on a display at a hundred and twenty under
   the cap of sixty; 60 and 30 on one at sixty -- never under twenty-four
   (`evenRatesFor`).

3. **It is stepped to what the frames fit** (`RateGovernor`), as amended on
   2026-10-04 by D558 -- the first writing put a phone at thirty three
   seconds after launch and kept it there:

   - **Nothing is judged at a start.** Not for four seconds after a launch,
     a curtain lifting, another display mode; and a frame of a quarter of a
     second or more is a hitch, not a rate -- it is left out, and the second
     it fell in with it.
   - **Down on a sustained miss only**: more than three frames in ten late,
     in each of two seconds running, then one rate down. At the display's own
     rate a late frame is one the display showed for a refresh more (its
     interval over the period and a quarter); at a rate under it, one whose
     work -- the frame less what pacing made it wait -- overran its place. A
     game that holds fifty-nine, or dips to fifty for ten seconds while a
     warm phone's GPU runs slower, is a game at sixty.
   - **Up by trying.** After five seconds at a rate that is being held, when
     the CPU's part of a frame -- the frame less every wait: the GPU, the
     display's image, the present, the hold -- fits the rate above with a
     tenth to spare, it steps up and sees. The simulation's part of it is
     counted as the ticks a frame runs at the rate above (D559, below): half
     as many at sixty as at thirty. What the frame took before the
     hold cannot say whether the rate above would fit: it carries the wait
     for the display, and a phone slows its GPU's clock when the GPU idles,
     so a frame held at thirty takes longer than the same frame at sixty. A
     step up taken back within ten seconds doubles the wait before the next,
     to a minute; half a minute held gives the first wait back.

4. **The wait is at the present.** A paced frame is drawn, then held until
   its place in the rate's grid, then shown: frames a steady time apart. The
   wait used to come after the present, which leaves each frame shown whenever
   its work ended. It costs the latency of the hold, which is what a frame
   pacer is.

5. **At the display's own rate nothing is held at the present**, and the
   frame is paced as one nobody governs is (`governedPaceFor`, as D560 left
   it): by the display, and by the game's cap waited out at the frame's end.
   The first writing left it to the display alone -- "a second pacer beside
   the display's would beat against it" -- and the phone said otherwise.

6. **The display is told** (`platform::requestDisplayFrameRate`, through
   `PlayerActivity.requestFrameRate` to `Surface.setFrameRate`, Android 11
   and later) the game's CAP, once, so the system may run the display at it
   or a multiple of it. Not the rate held (D558): a display told thirty may go
   to a mode that refreshes thirty times, and from there the choices are
   thirty and under -- sixty is not one of them again.

7. **Not behind the loading curtain**, whose frames are a load's; the
   governor starts over when it lifts. Not in the background, where the
   background rate applies as before.

8. `[display] adaptive_frame_rate = false` turns 2 to 6 off for a handheld's
   game, leaving the cap. A desk's game cannot turn it on.

9. The log says the rate each time it changes, and why in the numbers that
   decided it: how many of the second's frames were late, or the CPU's time
   that was found to fit. What a report from a phone is read against.

## Amended 2026-10-05: three things the phone said (D559, D560, D561)

Read from the coordinator's runs on the phone this was written for, with the
governor on and with it off, in the same two scenes:

- **Stepped down, it stayed down** (D559). Four runs on the lean ground were
  put at thirty, rightly -- that ground could not be drawn sixty times a
  second there (ADR 0179) -- and none tried sixty again in the minute and
  more each went on. The test for trying is that the CPU's part of a frame
  fits the rate above, and the CPU's part measured at thirty was 14.9 to 17.8
  ms: over the fifteen that a sixtieth of a second leaves. But a frame at
  thirty runs two of the simulation's ticks and one at sixty runs one. The
  simulation's part is now counted at the rate above -- the frame's CPU time
  less its ticks, plus the ticks scaled by the two rates -- and the log's line
  for a step up says both numbers. It is a mean: where the rates are not a
  whole number of ticks apart some frames run a tick more than others, and
  the try is what finds that out.

- **At sixty on a display at sixty, the display alone paced worse than the
  cap beside it** (D560). With the fast ground and the governor on, 127
  frames of 4189 were over 33 ms and a frame waited 5.6 ms on the GPU and the
  display; with it off, 37 and 0.6. The one difference at that rate: an
  ungoverned frame waits out the game's cap of sixty at its end, on the
  host's clock, and the governed one waited for nothing and was stopped by
  the display's queue when it filled. The reading, not taken further on the
  device: a frame that arrives at a full queue has no slack for the one that
  runs long. Decision 5 is amended to do what the ungoverned frame does.

- **The cap's wait was counted as the CPU's drawing** (D561). It slept
  inside the frame's first stretch with no scope and in none of the frame's
  waits: a phone's report said 5.2 ms of `frame.begin` and as much "drawing
  on the CPU" that was sleep. It is the scope `wait.cap` and a wait. The
  governor was never told it: the interval it is handed begins after it.

## What it does not do

- **It does not know where the display's refreshes fall.** The grid is the
  host's clock, not the display's, so a frame held to "every third refresh"
  lands on every third only while the two do not drift across a refresh; near
  that edge a frame can land one early or late. Android's answer is the
  display's own timeline (`AChoreographer`) or a present time handed to the
  driver (`VK_GOOGLE_display_timing`, which SDL's GPU API does not expose).
  Decided after measuring on the phone which refreshes the frames land on
  (`dumpsys SurfaceFlinger --latency`): the governor and the hold are needed
  either way, and aligning their grid is an addition, not a change.
- No thermal headroom yet (`AThermal_getThermalHeadroom`): a level that
  lowers itself as the phone warms is its own decision.
- Frames in flight are SDL's two; the list of three in the backend holds only
  where no window does.

## Consequences

- A phone whose game sets no cap draws half the frames it did, at most, and
  warms accordingly less.
- A game at the edge of a rate runs at the rate below it, steadily, instead of
  between the two.
- `--frame-stats` gains a scope, `wait.pace`: the hold. It is counted as a
  wait, not as the frame's work. And, since D561, `wait.cap`: the cap waited
  out at a frame's end.
- Protocol unchanged (40).
