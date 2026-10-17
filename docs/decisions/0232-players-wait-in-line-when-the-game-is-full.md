# 0232 — Players wait in line, and see where they are in it, when the game is full

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0220 (the front door), ADR 0230 (channels), ADR 0222
  (`TravelStarted`, `@engine/travelscreen`)

## The question

When every channel of the entry world is full and no more can be opened, the
front door today has nowhere to send a newcomer. Refusing them -- "the server
is full, try again" -- makes them retry by hand, and the ones who retry
fastest get in. A line is fairer and calmer: a place, a number, an estimate.

## Decision

### 1. The front door keeps a line

When the entry world cannot take a player -- every channel at
`max_players`, `max_channels` open, or the game at `[manager] max_players`
-- the front door holds them in a line instead. It sends each waiting player
their place and an estimate every five seconds, and sends them on, first in
first out, as places free.

### 2. The estimate is measured

The estimate is the player's place divided by the rate places freed in the
last five minutes, rounded to a minute; with no departures yet it says
nothing rather than guess.

### 3. Back to their place

A player who drops while waiting keeps their place for
`queue_grace_seconds` (60 by default). One who drops from a world keeps the
right to come back to it without waiting for the same while, as a reconnect
does today (ADR 0221's rule for instances, now for every world).

### 4. On the player's machine

`NetworkService.Queued(position, estimateSeconds)` fires while it waits;
`Connected` fires as usual when it is let in. `@engine/travelscreen` shows the
line with no change to a game that mounted it. A wire message of its own,
in Phase B's one protocol bump.

## Script surface

```toml
# servers.toml
[manager]
max_players = 2000          # the whole game, all worlds and channels
queue_max = 10000           # past this the door refuses, with words
queue_grace_seconds = 60
```

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

NetworkService.Queued:Connect(function(position: number, estimateSeconds: number?)
    if estimateSeconds then
        print(`You are number {position}. About {math.ceil(estimateSeconds / 60)} minutes.`)
    else
        print(`You are number {position}.`)
    end
end)
```

## Proof

- `max_players = 20` and 40 bots arriving at once: 20 in, 20 in line with
  places 1 to 20; as bots leave, the line moves in order and each estimate
  falls; a bot that drops and returns inside the grace keeps its place.

## As built

- The line is the front door's: who waits, by key, with the ticket they came
  with and their connection -- or none, and until when their place is kept.
  The first of it still at the door goes when a place frees; one whose
  connection dropped keeps their number and does not hold the rest up.
- **A place is counted the moment it is taken.** The manager counted a
  world's players from its heartbeat, a second old, and the passes it gave
  since that heartbeat: between a player's arrival and the next heartbeat a
  full town looked empty, and the door let four into a town of two. A world
  now counts who is in as their arrivals and departures say, at once, and who
  is on the way by key, until they arrive or their pass lapses (thirty
  seconds). ADR 0230's routing uses the same count.
- A place in the line and its estimate are a message of their own,
  `Queued` (protocol 55): a place from 1 and seconds in whole minutes, or
  none. The replica keeps its place in its status; a join in line has its
  clock started again at each place told, so waiting never times out.
- One who came back within the grace to the world they dropped out of goes
  straight there, if it has room; one who travelled on is not owed it.

## Measured

- `engine_app_tests`: a town of two and six real clients -- two in, four in
  line at places 1 to 4 with no estimate; the fifth drops while waiting and
  comes back to place 3, ahead of the sixth who came after them; the first
  leaves, the first in line goes in, and the next is at place 1 with an
  estimate now that the line has moved.
- **One key, one place**: a key already in line that comes back takes up its
  place again -- it never adds a second entry, and never jumps ahead -- and a
  newcomer past `queue_max` is refused with words, so a flood of new keys
  stops at that bound rather than growing the line (`engine_app_tests`, a
  line of one: the third is refused, and never given a place).
- `examples/47-realms` at `[manager] max_players = 20`, 40 light bots, two
  processes: 20 in and 20 in line; when the first twenty left after 40
  seconds, the twenty waiting went in, and the town never held more than
  twenty.

## Consequences

- A launch day is a line, not a refresh button.

## Not decided here

Paying or account priority in the line (a game's backend can let a ticket
carry it later, ADR 0226).
