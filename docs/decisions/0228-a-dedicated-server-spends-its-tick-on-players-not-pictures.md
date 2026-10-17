# 0228 — A dedicated server spends its tick on its players, not on pictures

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0076 (interest by distance), ADR 0099 (network ownership),
  ADR 0162 (`ReplicationFocus`), ADR 0220 (a world's numbers)

## The question

A load test of a dedicated server with headless players (four to a bot, 60
Hz) measured where its tick goes:

| Run | Players | Server characters | Tick (ms) | Out | Largest share |
|---|---|---|---|---|---|
| spread over 4 km, `LoadRadius` 150 | 500 | 0 | 2.7–3.0 (worst 5.3) | 2.0 MB/s | `net.peers` ~11 ms median in a profile window |
| spread, with NPCs | 500 | 1,000 | 9.9–10.1 (worst 15.6) | 5.0 MB/s | `net.peers` ~55%, `physics.apply` ~20%, `physics.solve` ~10%, `net.capture` ~6% |
| one 80 m square | 300 | 0 | 3–4 (worst 20–63) | 23 MB/s | `net.peers` ~85% |

And on a server with no window, `draw.submit` (0.31 ms) and `extract.lights`
(0.22 ms) ran every frame: the renderer's extraction and the debug wire box
of every part, for a picture nobody sees.

`net.peers` is the work of telling each player what changed, and it grows as
players times things: for each peer the session walks every replicated part
to measure its distance (`interestOf`), and walks every part again to find
what that peer owns. Five hundred peers and fifteen hundred parts is 750,000
distance checks a send, twice.

## Decision

### 1. No drawing on a dedicated server

A process whose topology is `Dedicated` builds no render snapshot, extracts
nothing, submits nothing and lays out no world UI. Its frame is the tick, the
network and the console. A host (a player who hosts) draws as it does now.

### 2. Interest from a grid, not from every part

Each send, the session sorts the replicated parts into a uniform grid in the
X–Z plane, cells the size of the load radius, once. A peer's interest is the
cells within its radius of each of its foci (its character, its
`ReplicationFocus`, its machine's other players), and only the parts in those
cells are measured exactly. A part larger than a cell is in every cell it
covers. The answer is the same set as today -- the same edge-to-edge
distance, the same hysteresis -- found from tens of parts instead of all.

### 3. What a peer owns, from the parts that have an owner

The parts that have an owner at all (ADR 0099) are listed once a send, and
each peer's are found in that list, rather than by walking every part for
every peer. The same pass sends each peer the parts it looks for by looking
each up, rather than walking the whole world's state for every peer.

### 4. The physics step's cost, named

`physics.apply` was thought to be the scene's state written into the physics
backend for every body every tick. Measured, it is not: with 1,000 server
characters and 1,600 parts, the 1,600 parts cost 0.12 ms a tick and the
characters 5.75 ms -- each character's controller moving it, sweeping the
world as it goes. Writing only what changed would save almost nothing. The
characters' share is now its own scope, `physics.characters`, and what cuts
it is moving fewer of them: those no player is near sleep (ADR 0234).

### 5. Measured before and after, in the order of the shares

Each step is measured with the same three runs before it lands, with
encryption on (Phase A), and the numbers go in the "As built" section. The
order is the profile's: `net.peers` (2, 3), `physics.apply` (4), then 1.

### 6. The profile says what its numbers are

`CaptureProfile`'s report gave `meanMs` of about 11 for a light run and
457–1,111 for an overloaded one. Both are a frame's time, and correct: the
overloaded server ran about 34 ticks in each frame to keep up, so a tick's
scope held 34 ticks of time. The report adds `calls` (how many times a scope
ran in a frame), `perCallMs` (one run of it) and `ticksPerFrame`, and `GET
/metrics` adds `ticks_per_frame`.

### 7. A server catching up sends at the snapshot rate of real time

A server runs ticks back to back to catch up a frame that ran long, up to
half a second of them, and sent a snapshot every `ticksPerSnapshot` of those
ticks: a burst of states stale before they arrive, paid for in the per-peer
work exactly while the server is behind. In a frame that runs more than one
tick, a snapshot is now sent only when an interval of real time has passed
since the last one was; a snapshot due meanwhile is owed and goes with the
next tick that may send. A frame of one tick sends as it always has.

The interval is counted from the last send, not kept as a schedule: a server
short of time then gives up sends before it gives up ticks, which is what lets
it catch up. **Catch-up is for spikes. A world that stays behind is time
dilation's (ADR 0233).**

What a held snapshot changes is when a peer is told, never what: the next one
is against the baseline the peer acknowledged and answers every input the
held ticks consumed.

## Measured

Development builds, each pair the same build with and without the change,
two runs each; headless players four to a bot.

- **Interest from the grid, and a peer's entities looked up**: 128 players
  and 1,000 server characters over 4 km, `net.peers` 131.5 and 134.9 ms a
  frame before, 89.0 and 89.4 with the grid, about 86 with the look-up.
- **§7 under spikes** -- 64 players, 300 server characters, a 300 ms tick
  every five seconds: frames unchanged (55 a second), snapshots 29.1 and 29.0
  a second a player against 29.2 and 29.3, corrections 41.8 and 33.8 a player
  against 59.2 and 60.4.
- **§7 under sustained overload** -- 128 players and 1,000 server
  characters, a server exactly at its limit: without it, 1.8 frames a second
  (555 ms each, about 34 ticks), `net.peers` 19 ms a frame, 27 snapshots a
  second a player, 137 and 154 corrections a player. With it, the server comes
  out of the spiral: 38 to 40 frames a second (25 ms), `net.peers` about 1 ms,
  4.6 and 5.6 corrections a player -- and 17 to 19 snapshots a second.
- **Rejected: a fixed schedule** (each snapshot due an interval after the
  last was due), tried to keep the rate under that overload: the server stayed
  in the spiral (2 frames a second, 510 ms), 40 corrections a player, and still
  19 to 20 snapshots a second.
- **The table in The question understates the network.** Its players were
  four to a connection, so the server sent a quarter of the streams and
  received a quarter of the packets that as many real players make: with one
  player a connection (ADR 0236's bots), 128 players are twice the bytes out
  and three times the bytes in. The tick shares above are of the same runs
  and stand; what ADR 0229 budgets is measured with the bots.
- **ADR 0229's budget cuts bytes, not this tick.** Three hundred players in
  one square ticked 16.6 ms (development build) with no budget, 48 KB/s and
  16 KB/s alike: every changed record is still encoded before the budget
  chooses among them. What cuts the tick is fewer records to make -- ADR
  0234's dormancy and ADR 0230's channels.

## Script surface

None new. What a script sees is the same world, sent cheaper. The numbers
are read where they are now:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

local stats = NetworkService:GetStats()
print(stats.BytesSent, stats.SnapshotBytes)
-- and, on a dedicated server, `GET /metrics` (`tick_ms_mean`, `wire_bytes_sent`)
```

## Proof

- 1,500 server characters and 500 players spread over 4 km: the tick has
  headroom on one core (target: under 8 ms, worst under 16).
- 500 players: `net.peers` under 2 ms a send.
- A dedicated server's profile has no `draw.*`, `extract.*` or `ui.world`.
- Every replication test and the netcode suite unchanged: the grid finds the
  same sets the walk did.

## Consequences

- The per-peer cost grows with what is near each peer, not with the world.
- A dedicated server cannot be asked for a screenshot; nothing needs one.

## Not decided here

How much each peer is sent (ADR 0229); which world a crowd is in (ADR 0230).

**A parallel character step**, a later option: the characters' controllers
moved on several workers at once. Measured at 5.7 µs a character a tick in a
development build -- 1,000 characters, 5.75 ms -- it is the largest share of a
crowded server's tick that dormancy (ADR 0234) does not remove. **In a
release build**, a dedicated server with characters walking and none asleep
(`physics.characters`, then the whole tick): 500 characters, 2.21 ms (4.4 µs
each) and 3.17 ms; 1,000, 5.17 ms (5.2 µs each) and 7.94 ms, the solver
growing faster than the count (0.61 ms at 500, 2.03 at 1,000); 2,000, past
what one core holds -- 13.3 ms of characters and 22.5 ms in all a tick, and
the server catching up thirty ticks a frame. Characters
push each other, so it needs a determinism proof of its own before it is
built.
