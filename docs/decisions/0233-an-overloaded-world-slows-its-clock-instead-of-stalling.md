# 0233 — An overloaded world slows its clock instead of stalling

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0025 (a fixed tick), ADR 0076 (prediction), ADR 0228

## The question

A battle with three hundred players in one place can need more than a
tick's time to simulate. Today the server falls behind: ticks arrive late
and in bursts, every player's prediction is corrected, and the fight feels
broken for everybody at once. A world that slows down evenly -- everything
moving at, say, 60% speed, smoothly, and saying so -- is playable; one that
stutters is not.

## Decision

### 1. Opt-in, per world

`[server] time_dilation = true` in `project.toml`, or `time_dilation = true`
on a `[[world]]` in `servers.toml`. Off, a world behaves as now.

### 2. The tick stays the same; fewer of them run a second

The simulation's step is unchanged, so the same inputs give the same world
(R10). What changes is how many steps run in a wall-clock second: when the
mean tick time over the last second is over 90% of the tick's budget, the
rate is lowered in steps of 5%, down to `time_dilation_floor` (0.25 by
default); when it is under 70%, raised again the same way. A rate change is
taken between ticks and never more than once a second.

### 3. Every machine runs at the rate

The rate is sent with every snapshot. A client's fixed loop runs at the same
rate, its prediction steps as often as the server's do, and its
interpolation delay is measured in ticks, not milliseconds -- so nothing is
corrected because the server slowed down. Animations and sounds driven by the
simulation slow with it; the interface and the camera do not.

### 4. A replayed tick runs at the rate it ran at

A client that re-simulates ticks after a correction (ADR 0076, ADR 0157)
replays each at the rate that tick had, which is recorded with the tick's
state, never at the rate it has now. A rollback across a change of rate gives
the same world as no rollback.

### 5. Said, not hidden

`RunService.TimeDilation` is the rate (1 when the world is at full speed),
`RunService.TimeDilationChanged` fires when it changes; `GET /metrics` has
`time_dilation`. A game can show "the battle is slowed to 60%".

## Script surface

```toml
# project.toml
[server]
time_dilation = true
time_dilation_floor = 0.25
```

```luau
--!strict
local RunService = game:GetService("RunService")

RunService.TimeDilationChanged:Connect(function()
    local rate = RunService.TimeDilation
    if rate < 1 then
        print(`The world runs at {math.floor(rate * 100)}% speed`)
    end
end)

-- Simulation code is unchanged: `dt` is the tick's, and a dilated world
-- simply runs fewer ticks a second.
RunService.Heartbeat:Connect(function(dt: number)
    -- ...
end)
```

## Proof

- A world given 2,000 characters on purpose: the rate falls until the tick
  fits, the clients' corrections stay at the rate of an unloaded world, and
  the rate returns to 1 when the characters go.
- Determinism traces unchanged: dilation decides how often, never what.
- A client corrected across a `TimeDilationChanged` re-simulates to the same
  world hash as a client that was not corrected.

## As built

- **The controller is not §2's.** It measures the share of each second the
  simulation took (the ticks, with what each sends) and, past nine tenths,
  goes at once to the rate at which ticks of the cost they had take seven
  tenths of a second, in steps of a twentieth, at least one, down to the
  floor; it comes back up a twentieth once three seconds in a row took less
  than half. Stepping down a twentieth a second, as §2 had it, a world spent
  the seconds on the way behind its ticks, and the first measurement below
  is what that cost.
- **A change of rate costs a correction a character.** The rate reaches a
  replica a round trip after its authority runs at it, and for that moment
  the two tick at different rates. Between 70% and 90% around a target of
  80%, a tick's cost -- which varies by a fifth from one second to the next
  -- moved the rate every few seconds, five corrections each time for a
  machine of four players. Hence the wide band and the three calm seconds.
- `FrameScheduler::setTimeScale` is the whole of how: the time owed to the
  simulation is the wall's times the rate, so the tick and its order are
  untouched, and a re-simulation after a correction is the same whatever the
  rate was (§4 holds with nothing recorded). The rate is in every snapshot as
  ten thousandths (protocol 55); `RunService.TimeDilation` is a host fact,
  in no world's hash.
- `time_dilation` per `[[world]]` in `servers.toml` becomes the world's
  `--time-dilation`.

## Measured

2,000 server characters walking in one world, nothing asleep, one full engine
of four players joined, development build:

| Run | The rate | The four's corrections |
|---|---|---|
| no crowd | 1 | 3 in 60 s |
| the crowd, no dilation | 1 (behind its ticks) | 655 and 659 in 60 s |
| the crowd, dilated, stepping down a twentieth a second | to 0.45 in 10 s, then 0.45 to 0.6 | 31 in 60 s, 21 of them on the way down |
| the crowd, dilated, to what fits at once | 0.45 to 0.6, moving every few seconds | 25 in 60 s, five at each change |
| the crowd, dilated, as built | 0.45, 0.5 a minute later | 4 in 60 s |
| the same, the crowd taken away after 45 s of its time | 0.4, then 1 within 30 s | 10 in 140 s |

The crowd's 45 seconds were 110 of the wall's: `task.delay` is the
simulation's time, and dilates with it.

## Consequences

- A world a crowd overwhelms degrades evenly and visibly instead of breaking.
- Timers in wall-clock seconds (`os.time`) do not dilate; `task.wait` and the
  simulation's time do.

## Not decided here

Dilating only part of a world.
