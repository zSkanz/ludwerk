# 0229 — Each player is sent what matters most to them, within a budget

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0076 (interest, interpolation), ADR 0099 (ownership),
  ADR 0162 (`ReplicationFocus`, a swarm's own budget), ADR 0228

## The question

Three hundred players in one 80 m square were sent 23 MB/s: about 78 KB/s
each, because every one of them is near every other and every change near a
player is sent on the next snapshot. A player on a phone network has a tenth
of that. Interest by distance answers *what* a player may be sent; nothing
answers *how much*, or what comes first when there is too much.

## Decision

### 1. A budget per player

Each player has a budget of bytes a second for the world's state -- 48
kilobytes (bytes, not bits) a second by default,
`[network] client_budget_kb_per_second` in `project.toml` for every player,
`Player.ReplicationBudget` (in kilobytes a second) for one. Messages a script sends
(`RemoteEvent`), the player's own input answers and the join's ground are
outside it; they have their own limits already. So is a part sent whole -- one
new to the player, as everything is when they join: it goes at once, and
counting it made a join overdraw the budget for half a second, during which
nothing else moved.

It is a token bucket: the budget is added each snapshot by the ticks since
the last, at most half a second of it is held, and what may not wait (below)
may overdraw it by that much, repaid from the snapshots after.

### 2. Priority that accumulates

Every part in a player's interest carries a priority for that player that
grows each tick it is not sent, by a rate that is:

- higher the nearer it is to the player's focus (full inside 15 m, falling
  to a tenth at the edge of the load radius);
- times `BasePart.ReplicationPriority` (1 by default);
- times what the game said for this player (`Player:SetReplicationPriority`)
  -- the party, the target, whoever is fighting them.

What may not wait goes first, whatever it costs: the player's own
characters (a machine's guests' too), what they own, a part sent whole, and
**what is within eight metres of one of their characters** -- what the
prediction of that character can collide with next has to be where the
authority has it. Then a change that has waited three quarters of a second
(45 ticks), oldest first, with at most half of what is left; then the rest,
those old ones that did not fit among them, by priority, until the budget for
that snapshot is spent. A part sent goes back to nothing. A far part is sent
less often, never never. A part that did not change costs nothing and is not
in the list at all.

The share for the old ones is what keeps priority meaning something. Without
the cap, a budget that barely carries every part once a second has nearly
everything waiting past the threshold, and the snapshot is spent in order of
age: in the test below, a part at `ReplicationPriority` 10 was sent 8 times in
four seconds against 5 for a plain one. With it, 54 against 6.

### 3. A part sent late is drawn right

A replica interpolates each part between the last two states it was sent of
that part, at the ticks they were taken, rather than between the last two
snapshots. A part updated every fourth snapshot moves smoothly at the speed it
had, a little behind; a part whose next state is late holds its last place
rather than guessing.

As built: a replica already kept a sample of a part only when its transform
changed, so the interpolation was per part. What it lacked was a reason to
draw a part further back, and that is now said on the wire: a record of a
part that waited for its turn carries a flag (protocol 55). Its samples carry
a lag, eased towards how much later than a snapshot interval its states come
(at most a second), and eased back at twice the speed once it is sent on time;
the part is drawn that many ticks further in the past. A part sent because it
may not wait is never flagged: within reach of the player's character, it is
drawn on time, or the prediction collides with where it was a second ago --
which the first measurement found, as one character corrected every other
snapshot.

### 4. Said where it can be read

`NetworkService:GetStats()` gains `Deferred` (updates held back by the budget
in the last second) and `BudgetUsed` (0 to 1); `GET /metrics` gains
`deferred_per_second`.

## Script surface

```toml
# project.toml
[network]
client_budget_kb_per_second = 48   # kilobytes (not kilobits) of the world's state a second, each player
```

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

-- A boss everybody should see moving, before the scenery.
boss.ReplicationPriority = 3
-- Grass that sways is the last thing to spend a phone's bytes on.
banner.ReplicationPriority = 0.25

-- A party sees its own members first, wherever they are in the crowd.
local function partyFirst(player: Player, party: { Player })
    for _, member in party do
        if member ~= player and member.Character then
            player:SetReplicationPriority(member.Character, 4)
        end
    end
end

-- A player on a weak network, or a spectator, gets less: 24 kilobytes a second.
player.ReplicationBudget = 24

print(NetworkService:GetStats().Deferred, NetworkService:GetStats().BudgetUsed)
```

## Proof

- 300 players in one 80 m square: each sent at most its budget, the tick
  inside its time, and every player's own character corrected no more with
  the budget than without it in the same crowd.
- A far part changing every tick is sent at least once a second.
- The netcode suite unchanged with a budget no test reaches.

## Measured

**What the authority holds of a peer** is the change under all of it. A diff
was taken against the authority's own state at the tick the peer
acknowledged, and a snapshot's checksum was of the authority's state: with a
part held back, the peer no longer holds that. So each snapshot that held
something back keeps what the peer holds at it -- that state, with the parts
it was not sent as the peer had them -- until an acknowledgement passes it,
and a diff and a checksum against an acknowledged one use it. A snapshot that
held nothing back keeps nothing, as before. Records are encoded once each, in
id order, so the budget weighs their real bytes, and the chosen ones are
copied out in order.

Tests (replication suite): two hundred parts changing every tick against 10
KB/s -- the budget kept (with the snapshots' headers), no checksum failure, the
longest a part waited 40 ticks, and the replica the authority's world once
they stopped; the priority above; and a part sent every six or seven ticks
drawn moving 0.1 m every tick with none standing, where without the lag it
stood 80 ticks of 240 and jumped 0.3 m.

**Three hundred players in one 80 m square** (development build, 296 light
bots of ADR 0236 walking between random places in it, and one full engine of
four players inside it, kept there):

| Budget | Out (MB/s) | Each player (KiB/s, wire) | Held back a second | Server tick |
|---|---|---|---|---|
| none | 14.3 | 47.5 | 0 | 16.6 ms |
| 48 KB/s | 13.4 | 44.8 | 10,297 | 16.6 ms |
| 16 KB/s | 5.5 | 18.5 | 120,470 | 16.6 ms |

- **Each player is sent at most its budget**, with the snapshots' headers and
  the transport's beside it.
- **The tick is the same with a budget and without**: every changed record is
  still encoded before the choice. 16.6 ms in a development build is about 5.5
  in a package (ADR 0228); the cost is the records, which ADR 0230's channels
  and ADR 0234's dormancy cut.
- **Corrections of the full engine's own characters, forty seconds**: alone
  in the square 3 and 4; in the crowd with no budget 7 and 22; at 16 KB/s 13
  and 15 (before the reach rule, 22 at 16 KB/s, and 170 with a waited
  neighbour drawn late). With the budget, no worse than without it in the
  same crowd -- the proof as it stands. The crowd itself is what adds them, as
  in any engine where the server decides: the others are drawn in the past and
  pushed on the authority, so a character among three hundred is corrected
  more than one alone, budget or none. The lever a game has is whether its
  characters collide with one another at all (collision groups); where they
  do, ADR 0163's stand-in rule keeps a character from being pulled back
  through another. Bringing a character in a crowd down to what it is alone
  is an open target, for ADRs 0234 and 0230, not tuned here.

## As built

- `[network] client_budget_kb_per_second` (0 to 65,536; 48), `Player.ReplicationBudget`,
  `BasePart.ReplicationPriority` and `Player:SetReplicationPriority` (at most
  256 a player; 1 forgets one; an error on a client). The two properties are
  the authority's and in no world hash.
- `GetStats()`'s `Deferred` and `BudgetUsed`, and `/metrics`'
  `deferred_per_second`.

## Consequences

- The crowded square costs the server's uplink players × budget, a number an
  operator can plan for.
- A game that wants a part always up to date says so with its priority.

## Not decided here

Splitting a crowd into copies of the world (ADR 0230).
