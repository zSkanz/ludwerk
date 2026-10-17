# 0235 — A shot is checked against the world the shooter saw

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0076 (interpolation delay), ADR 0099 (ownership), ADR 0157
  (predicted steps)

## The question

A player with 150 ms of latency aims at a moving target and fires. What they
aimed at is where the target was drawn on their screen: the target's state
from a snapshot plus the interpolation delay, well in the past. The server
checks the shot against the target *now* and misses. Every shooter with
real latency misses moving targets they hit on screen.

## Decision

### 1. The server keeps where things were

For the last second, at every tick, the server keeps the transforms of the
parts that can be hit: every player's `Character`, and any part with
`BasePart.LagCompensated = true`. Only transforms, a ring a tick deep: 1,000
parts cost about 3 MB.

### 2. A raycast as somebody saw it

`Workspace:RaycastAsSeenBy(player, origin, direction, params)` casts against
the compensated parts placed where `player`'s machine drew them -- the tick
it was showing, from the player's measured latency and its interpolation
delay -- and against everything else as it is now. `SpherecastAsSeenBy`
does the same for a sphere.

### 3. Capped

The rewind is never more than `[network] rewind_cap_ms` (200 by default).
A shooter slower than that hits what they saw 200 ms ago, not what they saw:
past the cap, a target who reached cover on the server's clock is safe.
That is the trade-off every game with lag compensation makes, and the cap is
where it is made.

### 4. Only on the server

Called anywhere but on the authority, it raises. The client's own raycast is
for its prediction and its effects; the hit is the server's.

## Script surface

```toml
# project.toml
[network]
rewind_cap_ms = 200
```

```luau
--!strict
-- On the server, when a player's "Fire" arrives.
fire.ServerReceived:Connect(function(player: Player, origin: vector, direction: vector)
    local params = RaycastParams.new({ Filter = { player.Character :: Instance } })
    local hit = workspace:RaycastAsSeenBy(player, origin, direction * 300, params)
    if hit and hit.Instance:IsDescendantOf(enemyFolder) then
        damage(hit.Instance, 25)
    end
end)

-- A moving target dummy that is not a player's character.
dummy.LagCompensated = true
```

## Proof

- A shooter at 300 ms of simulated latency, a target moving across their
  view at running speed: shots aimed at the drawn target hit, at 150 ms and
  at 300 ms (rewound to the cap: hits where the target was 200 ms ago on the
  shooter's screen, which the test aims at).
- A target that is behind cover on the server's clock by more than the cap
  is not hit.

## As built

- **The tick a machine draws** travels with its input: every `Intent` carries
  the authority tick the replica is drawing everyone else at -- its newest
  snapshot less its interpolation delay -- in its low sixteen bits (protocol
  55, two bytes; the authority knows the rest from its own tick). The
  authority keeps, for each of the machine's players, how many ticks behind
  its own that was when it heard it, and a shot in a later tick is rewound by
  that much from the newest: the shot and the input that says what was on
  the screen left the machine together. Measured, not estimated from the
  ping: it is the round trip and the interpolation delay together, whatever
  each was.
- **The history** is a ring on the authority, as deep as the cap reaches
  plus two -- fourteen ticks at sixty a second and 200 ms -- recorded at the
  end of each tick, the state that tick's snapshot carries: 72 bytes an
  entry, so 1,000 parts are about 1 MB at the default cap, not the 3 MB a
  whole second was. Only a host or a dedicated server records; a cap of 0
  records nothing.
- **Nothing in the simulation moves.** The backend casts the world with the
  rewound bodies left out, then each rewound body's own shape where it was,
  and answers the nearest: `raycastMoved` and `spherecastMoved` on the
  physics interface. Moving the bodies and back would have touched the
  broad phase, which the simulation's determinism (R10) reads.
- **A finding**: a ray never met a character. A character's inner body
  carries the character's own user data, which the queries took for a body's
  handle -- so a ray at a character answered with whichever part's body had
  the same slot number, or went through. The queries now know a character's
  inner body by its id, `CanQuery` on a character's part is honoured, and a
  ray, a sphere and an overlap meet characters as they meet parts.
- **Not rewound**: a part the player's budget held back (ADR 0229) is drawn
  later than the rest; the rewind is the interpolation tick alone.

## Measured

- `engine_app_tests`, a target running across at 6 m/s, a shooter firing at
  where their machine draws it, real physics and the engine's own
  interpolation on both machines:
  - four frames each way (the shooter's machine 11 ticks, 183 ms, behind):
    48 shots, 48 hit as seen; checked against now, 0.
  - nine frames each way (21 ticks, 350 ms, behind): capped at 200 ms, 0 of
    48 hit -- the target is placed where it was 200 ms ago, a metre past
    where the shot went; capped at 600 ms, 48 of 48.
  - **Cover**: the shooter fires while the target still runs in the open on
    their screen and is behind a wall on the authority's clock: capped at
    200 ms, 0 of 27 hit; capped at 600 ms, 27 of 27 -- the shot around the
    corner the cap bounds.
  - a client calling it is refused.
- `engine_physics_tests`: a moved cast meets a body and a character where
  they are moved to, by their own user data, with the filter applied, and
  not where they are; and nothing moved. A ray meets a character by its own
  user data; with `CanQuery` off, not at all.
- `engine_scene_tests`: the ring keeps fourteen ticks at 200 ms, the
  characters with their parts and the compensated parts and nothing else,
  nothing for a world alone, and rewinds a player to the cap and no further.

## Consequences

- Shooting works at real latencies. A target can be hit "around a corner" by
  up to the cap; the manual says so.
- Rays, spheres and overlaps meet characters. A game that cast from inside its
  own character without leaving it out of the filter now meets it first; the
  engine's own casts already leave theirs out.

## Not decided here

Rewinding physics or animation, rather than transforms.
