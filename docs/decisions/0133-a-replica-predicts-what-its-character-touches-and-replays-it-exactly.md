# 0133 — A replica predicts what its character touches, and replays it exactly

- Status: accepted (to be built; see `docs/briefs/multiplayer-smoothness-2026-09-28.md`, stage 2)
- Date: 2026-09-28
- Decided by: the owner, on 2026-09-28, after the second match over a VPN. The
  flick was gone, but pushing a crate put the character **inside** it, and
  jumping onto the corner of a block **rolled the character back** again and
  again. Shown how other engines solve both, the owner chose predicted physics:
  *"aprovo a física prevista como é nas engines"*, and *"temos que fazer como
  grandes engines fazem"*.
- Builds on: [0076](0076-replicas-predict-their-own-and-draw-the-rest-between-snapshots.md)
  (a replica predicts its own character; the rest is drawn between snapshots),
  [0083](0083-the-simulation-is-deterministic-across-platforms.md) (the
  simulation is bit-identical across machines),
  [0099](0099-teams-and-network-ownership.md) (network ownership),
  [0101](0101-rollback-saves-restores-and-steps-the-simulation.md) (save,
  restore and step the simulation).
- Amends: 0076 §3 (correction by re-simulation of the character alone) and §5
  (only the own character is simulated on a replica).

## Context

After the smoothness fix (D242 to D246: input buffer, redundant intents, sliding
corrections, the F3 Network panel), two things still separate the match from the
engines the owner compares it to:

- **Pushing a loose part.** The replica simulates only its own character; the
  crate is drawn from snapshots, about four ticks plus the latency in the past.
  To stop the 30 corrections a second that colliding with that stale crate
  caused, loose parts a replica only follows no longer block its character from
  the side (D244) — so the character now walks *into* a crate that is drawn
  behind where it really is.
- **A jump onto a block's corner.** A corner is where a hair's difference
  decides between "landed on top" and "slid off", and the two outcomes are tens
  of centimetres apart a few ticks later. The replica's replay steps the
  character alone through `ICharacterReplay::stepController`, from the
  position, vertical velocity and grounded flag the snapshot carries — a
  shorter path than the live physics step, from a smaller state. At a corner
  the difference shows as repeated rollbacks.

How other engines do it:

- **Predicted physics** — Unity Netcode for Entities (predicted ghosts with
  physics), Rocket League (the ball), Unreal's networked Chaos physics in 5.x:
  the client simulates the objects near its player as well, and when the
  authority's state arrives it restores those objects AND its character to it
  and re-simulates to the present with its unanswered inputs.
- **Automatic ownership** (Roblox): the nearby player's machine simulates the
  object and the server trusts it. Simpler, but it hands authority to a client;
  the owner chose to keep the server authoritative.

This engine already has the hard parts: a simulation that is bit-identical
across machines (ADR 0083) and save/restore/step of the 3D simulation
(ADR 0101).

## Decision

### 1. What a replica predicts

- **Its own character**, as today.
- **Loose parts near it**: every simulated (unanchored, not network-owned by
  someone else) part within `[network] predict_radius` (default 8 m) of the own
  character, plus any part the predicted set is touching (so a stack of crates
  is predicted as one island). Capped at `[network] predict_max_bodies`
  (default 32), nearest first.
- A part **enters** the set at its latest authoritative state (position,
  rotation, both velocities, sleep state) and **leaves** it after it has been
  out of range for `predict_linger_ticks` (default 30), going back to being
  drawn between snapshots — blended, not snapped.
- Other players' characters stay interpolated: they are other people's inputs,
  which a replica does not have. They collide with the predicted set as
  kinematic bodies at their interpolated pose (the standard compromise); the
  side-pass rule of D244 stays for them and is removed for predicted parts.

### 2. How a correction works

- Each snapshot already acknowledges the intent tick whose step it holds
  (D242). For the predicted set it now also carries the full motion state of
  every predicted body at that tick (the protocol version rises; bodies the
  peer predicts are sent in full, not delta-quantised, so the replay starts from
  the authority's exact bits).
- On a snapshot the replica compares its saved prediction of the character
  AND of the predicted bodies at that tick. If any differs past the threshold:
  - it restores the whole predicted island — character and bodies — to the
    authority's state at that tick;
  - it re-simulates **with the real physics step** (the ADR 0101 machinery,
    restricted to the island), replaying its unanswered intents, back to the
    present tick.
- **The replay is the live step.** `stepController`'s separate path is retired
  for replicas that predict: the character is stepped by the same function, in
  the same order, with the same contact state as the authority's tick, so a
  prediction with the same inputs from the same state is bit-identical
  (ADR 0083). What the snapshot must carry for that is part of the work: every
  field the controller reads (grounded state, ground normal, contacts, step-up
  state, a pending jump), not only position, vertical velocity and grounded.
- The visible result still slides (D244) for residual corrections; a correction
  that remains after this should be a rare event, not a steady state.

### 3. Cost and budget

- A re-simulation costs (latency in ticks) × (island step). At 150 ms that is
  about 9 steps of a small island per snapshot. It runs only when a comparison
  fails, and `predict_max_bodies` bounds it.
- F3's Network panel and `NetworkService:GetStats()` gain: predicted bodies,
  re-simulations per second, ticks re-simulated, re-simulation time.

### 4. Around it

- **Server-authoritative stays.** A replica predicts; it never decides. Parts
  with an explicit network owner (ADR 0099) are the owner's and are not
  predicted by others.
- Determinism (R10): the authority's trace does not change. A replica's
  prediction is local and never replicated.
- Rollback games (ADR 0101) keep working: the island re-simulation is the same
  machinery, scoped.

## How it is proved

- A replication test on the jitter/loss transport (D242's): a replica walks into
  a line of crates and pushes one for 10 s — corrections above 1 cm: zero; the
  character never overlaps a crate in the replica's own world.
- A replica jumping onto a block's edge 100 times at varied offsets, over the
  same transport: zero corrections above 1 cm.
- The owner's test game, windowed client and dedicated server, with the F3
  Network panel: pushing a crate and jumping on a corner show no rollback, and
  the numbers are recorded in `docs/perf-baselines.md` with the re-simulation
  cost.
- Then the owner's match over the VPN.

## Not decided here

- Lag compensation for hit detection (rewinding other players for a shot).
- Predicting other players' characters (extrapolation beyond interpolation).
