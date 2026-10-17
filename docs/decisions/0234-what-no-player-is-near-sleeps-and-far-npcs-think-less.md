# 0234 — What no player is near sleeps, and far NPCs think less often

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0228 (the grid of who is near), ADR 0162 (a swarm's far
  agents), ADR 0117 (`CharacterBody`)

## The question

A world of 1,000 server characters spends a fifth of its tick in
`physics.apply` and a tenth in the solver, for characters most of whom no
player can see. An MMO world is mostly empty of players at any moment: the
work should follow them.

## Decision

### 1. Dormant regions

The world is divided into the grid of ADR 0228. A cell with no player's
focus within `dormant_radius` (twice `StreamingService.LoadRadius` by
default) is dormant:

- its bodies are put to sleep in the physics backend and are not stepped;
  a body that is pushed, moved by a script or touched by an awake one wakes
  at once, with the cell;
- its `CharacterBody`s do not walk; what they were asked (`Move`) waits;
- a part a game marks `Dormancy = Never` stays awake wherever it is -- a
  train, a projectile, a boss that roams.

A cell wakes the tick a player's focus comes within the radius, before the
player can see into it (the load radius is half the dormant radius).

### 2. NPCs that think by distance

`@engine/npclod` calls a game's NPC function at a rate by the nearest
player's distance: every tick near (under 30 m), every fourth tick at middle
distance (under 120 m), every sixteenth far, and not at all when its cell is
dormant -- passing the time since the last call, so the code is written
once. The rates and distances are the game's to change.

### 3. Asked, not guessed

`Workspace:GetNearestPlayerDistance(position)` answers from the same grid
in constant time, for a game's own decisions; `BasePart.Dormant` says
whether a part is asleep.

All of it is deterministic: dormancy is decided at the start of a tick from
the positions the tick starts with (R10), and is part of that tick's
recorded state. A client that re-simulates ticks after a correction uses each
tick's own dormancy, never the one it has now, so a rollback across a cell
waking up gives the same world as no rollback.

## Script surface

```toml
# project.toml
[server]
dormant_radius = 300          # metres from any player's focus; 0 is never
```

```luau
--!strict
local npclod = require("@engine/npclod")

-- One function per NPC, called as often as its distance deserves.
for _, guard in workspace.Guards:GetChildren() do
    npclod.think(guard, function(dt: number)
        -- look around, pick a target, walk: `dt` is the time since the last call
    end)
end

-- The rates, if the game wants others.
npclod.configure({ Near = 30, NearEvery = 1, Middle = 120, MiddleEvery = 4, FarEvery = 16 })

-- A roaming boss stays awake wherever it is.
boss.Dormancy = Enum.Dormancy.Never

if workspace:GetNearestPlayerDistance(camp.Position) > 500 then
    -- nobody is near: skip the camp's own bookkeeping
end
```

## Proof

- 1,500 server characters over 4 km and 50 players: the tick under a third
  of what it is with nothing dormant; a player walking into a dormant camp
  finds every NPC where it was, moving the tick they arrive.
- A body thrown into a dormant cell lands and comes to rest.
- A client corrected across the tick a cell woke re-simulates to the same
  world hash as one that was not corrected.
- Determinism traces unchanged for scenarios with no dormant cells.

## As built

- **By distance, not by cell.** A character sleeps when no player's focus is
  within the radius of it, measured where the physics step begins; the grid of
  ADR 0228 is the replication session's, and a world's step has the players
  and their foci in hand. Fifty players and 1,500 characters are 75,000
  distances a tick, which the measurement below does not see.
- **Characters, not bodies.** ADR 0228 §4 measured the characters as the
  cost (5.75 ms per 1,000 against 0.12 for as many plain parts), and the
  physics backend already sleeps a body at rest; a moving body is left to it.
  So a body thrown into a dormant place lands and comes to rest as anywhere
  else, and nothing here forces one asleep.
- **Only a dedicated server sleeps anything**, and not while any of its
  players has no focus: such a player is sent the whole world. A host and a
  single-player game are what they were. A replica never sleeps anything, so
  a re-simulation after a correction meets no dormancy to differ on.
- **What a capture reads**: a character asleep now and at the capture before,
  that nothing wrote, is kept from that capture without its components being
  looked at (the periodic re-reading still reaches it). It was what was left
  of the capture's cost once the step stopped moving them.
- `BasePart.Dormancy` and `Dormant`, `Workspace:GetNearestPlayerDistance`,
  `@engine/npclod` (`think`, `configure`; options in PascalCase, as ADR 0034's
  options tables are), `[server] dormant_radius`. At their defaults, neither
  property is in a world's hash.

## Measured

1,500 server characters walking over a 3.8 km square, 50 light bots (ADR
0236), development build, the same binary:

| Players | `dormant_radius` | Tick (ms) | Ticks a second |
|---|---|---|---|
| spread over the whole map | 0 (never) | 16.9 | 54.6 |
| spread over the whole map | 300 | 15.0 | 60.5 |
| within 500 m of each other | 0 (never) | 16.8 | 53.5 |
| within 500 m of each other | 300 | 8.1 | 60.6 |
| within 500 m, the capture keeping sleepers | 300 | 6.7 | 60.6 |

- **Where the players are is the whole of it.** Fifty players spread evenly
  over the map leave almost no character 300 m from all of them, and almost
  nothing sleeps. Gathered -- a town, a battle -- most of the world sleeps.
- **Gathered, the tick is 40% of what it was, not the third the Proof asks**
  (5.6 ms). What is left follows the players: telling fifty of them what
  changed (`net.peers`, 1.8 ms a frame), the characters near them that are
  awake (1.3), the solver (0.8), the capture (0.9). The server's memory fell
  from 334 MB to 116: a capture that keeps an entity keeps its field set.
- **The one without dormancy was not keeping up** (54 ticks a second where 60
  are due), so the drop measured is if anything smaller than the real one.
- **Judged again with a release build** (the package of the commit that built
  it, the load test of ADR 0236: 1,500 server characters, 50 bots,
  `wander`, 85 seconds with all of them in):

  | Players | `dormant_radius` | Tick (ms) | Ticks a second | Memory, most |
  |---|---|---|---|---|
  | spread over the whole map | 0 (never) | 16.5 | 56.2 | 332 MB |
  | spread over the whole map | 300 | 15.5 | 60.5 | 238 MB |
  | within 500 m of each other | 0 (never) | 13.6 | 58.8 | 332 MB |
  | within 500 m of each other | 300 | 7.2 | 60.6 | 112 MB |

  Gathered, the tick is **53%** of what it was in the release build, against
  40% in the development one: the release build does the awake world faster
  (13.6 ms against 16.8), and what sleeping removes is a smaller share of it.
  The third the Proof asks is not met in either. Spread, the drop is small in
  both, and the server keeps its sixty ticks only with dormancy.
- A player walking into a sleeping camp finds every character where it was,
  walking the tick they come within the radius (`engine_scene_tests`); the
  determinism traces, of worlds with no dedicated server, are unchanged.

## Consequences

- A world's cost follows where its players are, not its size.
- An NPC that must keep living far away is marked so, explicitly.

## Not decided here

Saving and unloading whole dormant regions to disk.
