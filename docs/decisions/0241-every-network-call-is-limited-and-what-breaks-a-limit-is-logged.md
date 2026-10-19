# 0241 — Every network call is limited, and what breaks a limit is logged

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0077 (`RemoteEvent`), ADR 0099 (an owner within reach),
  ADR 0219 (the administrator's console), ADR 0224 (encryption), ADR 0226
  (tickets and bans)

## The question

What an online game's server already refuses a cheat: movement is the
server's (characters are stepped there and predicted on the client, ADR
0076), an owner moves what it owns two metres a tick at most (ADR 0099), a
player is not sent what their character cannot see, every match is
encrypted and a player is their key (ADR 0224), and a ticket names the
account a ban keeps out (ADR 0226). What is missing is the cheap kind: a
client that calls a `RemoteEvent` ten thousand times a second, sends a
megabyte where the game expects a number, or does an action faster than the
game allows -- and somewhere for the operator to see who tried.

## Decision

### 1. Every `RemoteEvent` and `RemoteFunction` has a rate and a size

On the server, each player's calls of each remote are limited by a token
bucket: `Remote.RateLimit` (calls a second) and `Remote.Burst`. A call past it
is dropped before any script sees it, and counted. Only what a client sends
is limited: a server is trusted with what it sends.

**The defaults are 240 calls a second and a burst of 480**, from what games
made with the engine send. The busiest found: one call every tick (60 a
second, `examples/30-interactions` sends its pointer so), and the most a
call driven by the picture can make is one a frame, which a 240 Hz display
makes 240. Every other client call in the examples and in the maintainer's
games is an answer to a click, a press, or a timer of a second or more. So
the defaults never touch a game that sends once a frame, and still stop a
flood of thousands a second at the first fraction of a second.

A game's own heavy channel sets its own: `remote:SetRateLimit(perSecond,
burst)`, or the attributes `RateLimit` and `Burst` on the remote, which a
model saved with them keeps. Each call's payload is limited too: the `MaxBytes` attribute (default 16
KiB, encoded), and a table nested deeper than 16 or holding more than 1,024
values is refused whole. A script that sets 0 lifts the limit for that
remote -- a game's own streaming of data -- and that is said in the log once.

**Going past a limit is a suspicion first** (§3), never a kick by itself.
`[network] kick_over_limit = N` (0, the default, for never) disconnects a
player who passes the limits of all remotes together by N times in a second,
with a reason; an operator turns it on once the suspicion log shows what the
game's honest players do.

### 2. An action's rate, said by the game

What a game knows -- a weapon fires five times a second, a spell has a
cooldown -- it says once, and the server checks:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

NetworkService:DefineAction("Fire", { PerSecond = 5, Burst = 2 })

fire.OnServerEvent:Connect(function(player: Player, target: Vector3)
    if not NetworkService:CheckAction(player, "Fire") then
        return -- over the rate: dropped, and counted against the player
    end
    -- the shot
end)
```

`CheckAction` answers whether this one is within the rate, and records it
when it is not. It does not decide what to do: the game does.

### 3. What is suspicious, in one place

Every refusal above, every owner followed short of where it said a part was
(ADR 0099), every ticket refused and every action over its rate is one line
in the **suspicion log**: the account, the world, what, how many times in the
last minute. `NetworkService:ReportSuspicion(player, kind, detail)` lets the
game add its own (an impossible score, an item it never gave). The console
(ADR 0219) shows it -- `suspects` lists the accounts with the most in the
last hour, `suspect <account>` their lines -- and `/metrics` counts them by
kind. Nothing is acted on by itself except the kick in §1; a ban is the
operator's.

### 4. Mute, beside ban

The console's `mute <account> [minutes]` and `unmute` keep a list beside the
bans (ADR 0226 §5), shared by every world under the manager.
`NetworkService:IsMuted(player)` reads it on the server; what a mute means is
the game's (`@engine/chat` drops the muted player's lines, ADR 0239).

## Proof

- A client calling one remote 10,000 times in a second: the server's script
  runs 480 times in that second, the suspicion log has it, and a server tick
  stays under its budget throughout; with `kick_over_limit = 10` the client
  is disconnected with the reason.
- `examples/30-interactions`, sending once a tick, and a test sending once a
  frame at 240 Hz: nothing dropped.
- A 1 MB payload and a table 100 deep are refused without reaching a script.
- `CheckAction` at 5 a second refuses the sixth shot of a second.
- Each of these, a part teleported by its owner and a forged ticket appear in `suspects`
  under the right account, and in `/metrics`.
- A muted player: `IsMuted` true in every world under the manager.

## Consequences

- New API: `RemoteEvent:SetRateLimit`, the `RateLimit`, `Burst` and
  `MaxBytes` attributes (and `RemoteFunction`'s); `NetworkService:DefineAction`, `CheckAction`,
  `ReportSuspicion`, `IsMuted`; `[network] kick_over_limit`; console `mute`,
  `unmute`, `suspects`, `suspect`; `/metrics` `suspicion_total{kind}`.
- **A behaviour change**, the same for a project made before it as for a
  new one: a client that calls one remote more than 240 times a second, or
  sends more than 16 KiB in one call, is limited where it was not. The
  changelog says it with the one line that lifts it
  (`remote:SetRateLimit(0)`).
- No client-side anti-cheat: nothing on a player's machine can be trusted, and
  the engine does not pretend otherwise.

## Not decided here

Detecting aim assistance or wall hacks from patterns (a game's own analysis
on `ReportSuspicion`), and an optional provider for a commercial client-side
anti-cheat, which would follow ADR 0188's rules if a game asks for one.

## As built (2026-10-17 to 2026-10-19)

- **§1, the remotes' limits**: a token bucket per player and remote on the
  simulation's clock, 240 a second and a burst of 480, and 16 KiB a call;
  `remote:SetRateLimit(perSecond, burst)` and the `RateLimit`, `Burst` and
  `MaxBytes` attributes set a remote's own, and nought lifts each. **The
  1,024 values** are counted as the entries of every table of a call
  together, read from its bytes before anything is decoded, and lifted with
  the size by a `MaxBytes` of nought. **The depth is eight, not sixteen**:
  the wire's own limit since ADR 0077, which the decoder refuses whatever a
  remote's limits say; a call nested deeper is dropped as malformed, never
  reaching a script. `[network] kick_over_limit` as decided.
- **§2 to §4** as decided: `DefineAction` and `CheckAction` on the
  simulation's clock; one suspicion log beside the players, written by the
  remotes, the actions, the replication (an owner put past its reach), the
  door of a managed world (a pass only a forger sends) and `VerifyTicket` (a
  refused ticket), each line naming who by account or key, as a ban does;
  the console's `suspects` and `suspect`; `/metrics` `suspicion_total{kind}`;
  `mute` and `unmute` in `mutes.json` beside the bans, read by every world
  of the game, and `IsMuted`.
- A refused ticket's line names no player, and no account: the account it
  claims is the claim of whoever forged it.
- **Proved** by the services' tests: two thousand calls in a tick are 480
  heard and 1,520 suspicions; a 20 KiB call, a megabyte, a table a hundred
  deep and a call of 1,100 entries reach no script, the over-large and the
  over-full each a suspicion of its kind, while 1,000 entries are heard;
  four calls a tick -- once a frame at 240 Hz -- for five seconds are all
  heard with nothing suspected; `SetRateLimit(0)` lets two thousand through;
  `kick_over_limit = 10` asks one removal; `CheckAction` at five a second
  with a burst of two refuses the third; a mute by key and by account. By
  the replication's: a part put across the map by the machine it was
  handed to is a suspicion under that machine's player. By the joins': a ticket used twice and one
  signed by another key are two suspicions. By the console's: `suspects`
  and `suspect`, and a mute made by one console read by another.
- **Not yet**: `examples/30-interactions` played against the limits by a
  test, and a server's tick measured under a flood (the limits drop a call
  before it is decoded, so a flood costs its bytes' arrival and a bucket's
  lookup, not a script).
