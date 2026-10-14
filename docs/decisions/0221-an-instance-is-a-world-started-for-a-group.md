# 0221 — An instance is a world started for a group

- Status: accepted; not built yet
- Date: 2026-10-14
- Builds on: ADR 0220 (the manager, `[[instance]]`, a world's life),
  ADR 0222 (Travel), ADR 0111 (saves)

## The question

A dungeon, an arena, a house: a copy of a scene that exists for one group and
goes when they leave. The game must be able to ask for one and send a group
to it, the group must be the only ones who can enter, and nobody should wait
long or be lost if it falls.

## Decision

### 1. Asked for by a world, made by the manager

```luau
local WorldManager = game:GetService("WorldManager")
local instance = WorldManager:CreateInstanceAsync("crypt", {
    Players = party,                 -- who may enter; nobody else
    Data = { difficulty = 3 },       -- its scene's `GetLoadData` (ADR 0106)
})
NetworkService:TravelGroup(party, instance, { from = "town" })
```

`CreateInstanceAsync` yields until the instance is `Ready` and returns an
`InstanceHandle` (`Id`, `Template`, `State`, `PlayerCount`, `Closed` signal),
or errors with why: the template has `max_instances` running, the manager
cannot start a process, the instance failed to load. It exists on a server
under a manager; anywhere else it errors at once, saying so.

### 2. A warm pool

The manager keeps `warm` processes of each template `Ready` and unallocated,
started ahead, so an instance is a reservation and a scene load rather than a
process start. Giving one away starts the next warm one. A warm process has
loaded nothing of the game but the engine; it loads the template's scene when
it is allocated, with the `Data` it was given.

### 3. Only those it was made for

An instance takes a player only with a pass that names it (ADR 0222) and only
if the player is in its `Players`. **A player of it who drops comes back to
it**: while it lives, the front door sends a reconnecting player listed in it
there, not to the entry world. The instance's script can add or remove
players (`WorldManager:AllowPlayer`, `DisallowPlayer`), for a friend who
joins late.

### 4. It closes when it is empty

`idle_close_seconds` after its last player leaves -- 60 by default -- the
instance goes `Draining`, `WorldManager.Closing` fires so its script can save
what it keeps, and it stops. A script can close it sooner
(`WorldManager:Close()`) or keep it open longer (`KeepOpen(seconds)`).

### 5. A crash sends the group back

When an instance's process ends without `Draining`, the manager sends each of
its players to where they came from -- the world named in their pass -- with
the reason `InstanceFailed`, which the origin's `NetworkService.TravelArrived`
carries (ADR 0222), so the game can say so and give back what the run cost.
The instance is not restarted: what was in it is gone, and a new instance is
the game's to ask for.

### 6. Bounded

`max_instances` a template, and the machine's own: the manager refuses a new
one when free memory is under `[manager] min_free_megabytes` (512 by
default), and says so in the error and the log.

## Consequences

- A dungeon is a scene, a `[[instance]]` row and two calls.
- A warm process is memory spent before it is needed; `warm = 0` spends none
  and waits for a process start.
- New API: `WorldManager` (`CreateInstanceAsync`, `AllowPlayer`,
  `DisallowPlayer`, `Close`, `KeepOpen`, `Closing`, `InstanceId`), `InstanceHandle`.
- Manual: "Online games > Worlds and instances".

## Not decided here

Instances on other machines; an instance kept after its players leave (a
player's house) beyond `KeepOpen`, which is a saved world, a later record.
