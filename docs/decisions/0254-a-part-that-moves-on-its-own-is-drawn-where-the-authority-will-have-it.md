# 0254 — A part that moves on its own is drawn where the authority will have it

- Status: accepted
- Date: 2026-10-20
- Builds on: ADR 0076 (a replica predicts its own and draws the rest between
  snapshots), ADR 0133 (what its character touches, replayed), ADR 0134 (one
  drawn position per instance), ADR 0157 (scripts in the predicted step),
  ADR 0163 (another player's character met where the authority will have it)

## The question

A replica predicts its own character a few ticks ahead of what it has heard
and draws everything else a few ticks behind, between snapshots. A platform
the authority moves -- an anchored part a script, a tween or an animation
carries -- is drawn behind, and the character standing on it is ahead. While
the character stands on it, the step carries it by the platform as drawn
here, and the replay after an answer rebases it by the platform as it was at
the answered tick: walking, standing and turning on a moving platform are
held at no correction (ADR 0133's island, amended 2026-10-18).

In the air there is nothing to rebase by. A character hopping on a platform
takes off from the platform as drawn here -- a few ticks of its travel behind
the authority's -- and lands where the authority's platform is not: measured
at 67, 167 and 300 ms, 81, 69 and 16 corrections in sixteen seconds of
hopping, up to two metres each. Jumping onto one is put right once as the
character lands; another player standing on one is drawn sliding over it,
the two drawn at different moments.

## Decision

### 1. The parts that move on their own, near a predicting character

An anchored part whose place changes, within `[network] predict_radius` of a
character this machine predicts or carrying one, is a **moving part** of that
machine's prediction, as a loose part is (ADR 0133 §1), within the same
`predict_max_bodies`. It is drawn and simulated **at the predicted tick**,
not between snapshots: where the authority will have it when it steps the
tick this machine's character is at.

The replica knows which tick that is: each snapshot says the authority's tick
and the intent tick it applied at it, so predicted intent tick `N` is the
authority's tick `N + offset`, the offset the last snapshot's.

### 2. Where it will be, best first

1. **A motion the replica can compute exactly.**
   - **A tween** (`TweenService:Create` on a moving part's `CFrame` or
     `Position`) is sent once as its curve -- the start tick, the duration in
     ticks, the easing, the repeat and reverse, the two ends -- and evaluated
     at any tick, there and back in a replay.
   - **`RunService:BindToWorldStep(name, fn)`**: `fn(step)` runs once a tick
     inside the physics step, before it solves, on the authority every tick
     and on a replica at every tick it predicts and replays, with
     `step.Tick` the authority's tick. What it sets on an anchored part's
     place there is the part's motion. Its contract is §3's.
2. **Otherwise extrapolated.** A part moved some other way -- a script
   setting `CFrame` each tick, an animation -- is carried from its last
   authoritative pose by the velocity of its last two, linear and angular,
   for at most `[network] predict_moving_lead_ms` (250 by default) ahead;
   past that it holds where that lead takes it. A jump in its place larger
   than its velocity explains -- a teleport -- is not extrapolated.

A part leaves the set as a loose part does, after `predict_linger_ticks` out
of range, and goes back to between snapshots blended over a quarter of a
second.

### 3. The world step's contract

On a replica `fn` runs **several times for one tick**: once as the tick is
predicted, and again in every replay that passes it. So, as a predicted step
(ADR 0157 §§1, 6):

- **It is a function of `step.Tick` and of replicated state alone**: no
  wall clock, no unseeded chance (`step.Random` is seeded by the tick), no
  state of its own carried from one call to the next, no side effect but
  the places of the parts it moves. Called twice for one tick, it sets the
  same places.
- **It may not wait**: `task.wait` in it raises.
- **`step.Replaying`** is true when the tick is stepped again after an
  answer, and **`step.Predicted`** when this machine steps a tick its
  authority has not -- false on the authority and in a world played alone.
  A sound or an effect that marks a moment of the motion is played once:
  on the authority at every call, where neither is ever true, and on a
  replica only where `Replaying` is false.
- A function that breaks the contract is not refused; the replica's parts
  differ from the authority's, and each answer puts them right, as a
  correction of a part predicted from wrong inputs.

### 4. In the step and in the replay

The island's step reads each moving part at the tick it steps: the
character's ground carry, its contacts and a jump's take-off are against the
part where the authority will have it. A replay places each moving part at
its pose for the replayed tick: the answered tick's from the snapshot, every
tick after it from §2. A computed motion replays exactly; an extrapolated one
replays as it was predicted, and the next answer puts it right.

### 5. Another player on a moving part

A character drawn between snapshots that stands on a moving part is drawn
**in that part's frame**: its place relative to the part, between snapshots,
put on the part where it is drawn now. It moves with the part rather than
sliding over it. The snapshot says what such a character stands on (the
part's id, when it is a moving part of the receiving machine), and the wire
protocol rises to 64.

### 6. What it costs

- **The set is bounded**: moving parts count within `predict_max_bodies`
  (32), nearest first, with the loose parts of ADR 0133; one past it is
  drawn between snapshots, as today. A computed part is evaluated, an
  extrapolated one carried: neither is solved.
- **The world step runs per tick stepped**: once a tick on the authority,
  and on a replica once a tick predicted plus once a tick of each replay
  (latency in ticks times the replays, ADR 0133 §3). A binding that moves
  nothing near a predicting character still runs; what it costs is the
  game's code.
- **Said where it can be read**: `NetworkService:GetStats()` and F3's
  Network panel gain the moving parts predicted and the world step's time
  per tick, replays included.
- **Measured** on a server with fifty platforms -- twenty-five on a world
  step, twenty-five tweened -- and a replica among them at 167 ms: the
  authority's tick with and without the bindings, the replica's frame, its
  replays, and how many of the fifty enter its set.

### 7. A game that uses neither

A game with no `BindToWorldStep` and no tween on an anchored part sees what
it sees today, but for moving parts near its characters being extrapolated
at the predicted tick rather than drawn behind. Nothing else changes: no
binding runs, nothing is sent for curves, and a moving part nobody is near
is drawn between snapshots. The netcode suite holds that, unchanged.

### 8. What stays as it is

- Server-authoritative: the replica draws and predicts; it decides nothing.
  A part with a network owner (ADR 0099) is simulated by the machine that
  owns it, as before, and is no moving part of anybody's.
- Determinism (R10): the authority's trace does not change.
  `BindToWorldStep` runs where the physics step runs, in the order the
  bindings were made, and a world played alone runs it once a tick.
- A moving part nobody predicting is near is drawn between snapshots, as
  everything else.

## Script surface

```luau
--!strict
-- shared/lift.luau, required by the server's script and the client's.
local RunService = game:GetService("RunService")

local Lift = {}

function Lift.bind(platform: BasePart, from: vector, to: vector)
    -- Up and down every eight seconds, the same on both ends, by the tick.
    RunService:BindToWorldStep("lift", function(step)
        local t = (step.Tick % 480) / 480
        local up = if t < 0.5 then t * 2 else 2 - t * 2
        platform.Position = from:Lerp(to, up)
    end)
end

return Lift
```

```luau
--!strict
-- Or a tween, sent as its curve: drawn where it will be with no code shared.
local TweenService = game:GetService("TweenService")
local info = TweenInfo.new(4, Enum.EasingStyle.Sine, Enum.EasingDirection.InOut, -1, true)
TweenService:Create(platform, info, { Position = platform.Position + vector.create(12, 0, 0) }):Play()
```

A world step's `step`: `Tick` (the authority's tick), `DeltaTime`,
`Replaying`, `Predicted`, `Random` (seeded by the tick).

```toml
# project.toml
[network]
predict_moving_lead_ms = 250   # how far ahead an extrapolated moving part is carried
```

## Proof

The rider test's five cases (walk, hop, jump on, jump off, spin) at 67, 167
and 300 ms, each with the platform moved three ways -- a world step, a tween,
a script setting its place each tick:

- **Computed** (world step, tween): hop 0 to 2 corrections in sixteen
  seconds, jump on and jump off at most one, walk and spin none.
- **Extrapolated** (a script each tick): walk, spin and jump off as now; hop
  and jump on measured and reported, and no worse than today's.
- **Another player standing on it**, seen by a third machine: drawn moving
  with the platform, never more than a centimetre from where the platform
  carries it between two frames.
- The netcode suite unchanged with no moving part near anybody (§7).
- A world step called for one tick in a prediction and again in its replays
  sets the same places each time, and a sound played where `Replaying` is
  false plays once.
- The cost of §6, measured with fifty platforms and reported here.

## As built (2026-10-20)

- **The predicted tick.** A replica's `receive` runs before the tick it is
  input to, while the world is still at the tick before: the step about to
  run is the next, the intent it sends. Placed at the tick it was on, every
  moving part was a tick of its travel behind. Checked against the log of a
  run at 167 ms: the tick predicted was the one the authority answered that
  intent at for 467 of 474 intents, and one off for the other seven.
- **Carried on along a curve, not a line** (§2.2). Through the last two
  places, a platform swinging three metres every six seconds was missed by
  six to nine centimetres at 167 ms; through the last three, by five to ten
  millimetres. A part whose newest place is older than the newest snapshot
  has stopped -- a part is sent only when it moves -- unless the authority's
  budget held it back (ADR 0229), and is held.
- **The world step** is `PhysicsSync::runWorldSteps`, at the head of the
  step, before the predicted steps and before the scene is handed to the
  solver: what it places is this step's kinematic target. A replay runs it
  again through the same step, with the replayed command's tick. A replica
  says its offset in `EngineState::worldStepOffset` at every answer. What
  the world step moves under a character this machine draws and does not
  step carries that character with it.
- **The tween as its clock** (§2.1). `TweenService`'s clock is
  `scene::advanceTweenClock`, the same steps on both ends. The authority
  sends a tween on a part's `CFrame` or `Position` as `MovingCurve` (wire
  message 41): its timing, its clock as of a tick, the traversal's start and
  goal and where the part is -- at each traversal's start and each change of
  state, and to a peer the part enters the view of while it is in force. The
  replica steps a copy to the tick it predicts, taking each traversal's
  start where the part is and blending a `Position` in 32-bit floats, as the
  authority writes it. The first version sent none: an authority sends at
  its snapshots, every other tick, and the list of curves was a tick's.
  It is now the last second's, each taken once.
- **Another player on a moving part** (§5): `CharacterBody.GroundPart` on
  the wire (field 13). Its place relative to the part between snapshots is
  put on the part as drawn now.
- **What a project sets** (`[network] predict_radius`, `predict_max_bodies`,
  `predict_linger_ticks`, `predict_moving_lead_ms`) is read from
  `project.toml` and handed to the replica; ADR 0133's three were
  documented and read by nothing until now.
- **Said where it can be read**: `NetworkService:GetStats()` gains
  `MovingParts` and `WorldStepTime`, and F3's Network panel the same two.
- **Proved** (rider test, 67, 167 and 300 ms, five courses):
  - a world step, and a tween on the server alone: walk, hop, jump on, jump
    off and spin, no correction in any of the fifteen;
  - a script setting the platform's place each tick, carried on: walk and
    spin none, jump on and jump off one at most, hop 34, 52 and 16 -- from 81,
    69 and 16 drawn in the past;
  - another player standing still on a world step's platform, watched from a
    third machine: where it stands on the platform moves 2.6 micrometres in
    thirteen seconds, as on the authority; drawn without the platform's
    frame, it slid 1.69 m over it;
  - `predict_max_bodies = 1` holds the set to one.
- **Measured** (§6): fifty platforms, twenty-five on a world step and
  twenty-five tweened, round a player at 167 ms, a frame of each machine a
  tick: the authority 0.061 ms a tick with them still and 0.154 moving; the
  replica 0.060 and 0.145, its world step 0.015 ms a tick, twelve of the
  fifty in its set -- those within eight metres -- and six replays in ten
  seconds taking 5.7 ms in all.

## Not decided here

- A part a physics constraint drives (a hinge's motor, a prismatic's): a
  loose part already predicted (ADR 0133), not a moving part.
- Animations as curves: an animation playing on an anchored model is
  extrapolated here; sent as its track and time, it could be computed. A
  later record if a game needs it.
