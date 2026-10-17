# 0236 — A load test is a command

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0220 (`GET /metrics`), ADR 0228

## The question

Every number the scale work is judged by came from a hand-made script that
starts bot processes, each a full engine with four local players. It is not
in the repository, it needs a person to read its logs, and a full engine per
four bots limits a machine to a few hundred players. A game's maker should be
able to ask "how many players does my server hold" with one command.

## Decision

### 1. One command

`ludwerk server loadtest <address> --bots=N --seconds=S [--behavior=...]`
starts `N` light bots against a running server (its own, or one a manager
runs), for `S` seconds, and writes a report.

### 2. Light bots

A bot is a connection, not a game: `engine-host <project> --join=host:port
--bots=N` joins `N` times, each bot with a world of its own that keeps the
snapshots it is sent and a key of its own for its whole run, answers with
input, and runs no renderer, no physics and no game scripts -- only the
behavior. One process holds up to 64 bots, so 500 is eight processes. The
project is read for its id and its Luau flags, which the handshake binds.

Joins are spread out -- a bot every tenth of a second, and a refused one
again after one to three seconds, eight times at most -- because a server
takes twenty connections a second and sixteen handshakes at once from one
address. A server takes 64 players from one address by default
(`max_connections_per_address`), so a test from one machine runs the server
with `--max-per-address=0`.

Each process prints one line a second, a JSON object after a record
separator: bots in, failed, dropped, retries, bytes in, snapshots, worlds
travelled to, and for a game's behavior how many failed and ended.

### 3. Behaviors

`wander` (walk to random points near where it started), `crowd` (walk to the
same few metres round the world's middle), `travel` (walk to the parts tagged
`Portal`, which a game tags on the pads its server sends players on from;
with none, wander), `idle`. Every behavior follows wherever the server sends
the bot: a front door, `Travel`, a party's instance.

A game writes its own as a Luau file that the bot runs with a small `Bot`
API, `require("@engine/bot")`: `WalkTo(position, seconds?)` (waits until
there, within two metres, or the time is out, and says which), `Wait`,
`GetPosition`, `FindTagged(tag)` (in the bot's copy of the world), `Say`
(a line in the process's output), `Spawn` and `Index`. It runs in a VM of
the bot process's own, sandboxed, one thread a bot, from its beginning in
each world; it has the Luau libraries and `@engine/bot` and nothing else. A
behavior that runs a quarter of a second without waiting is stopped, since
every bot of the process shares one thread, and so is one that requires
anything else; each failure is said once and counted. The type side is
`runtime/engine/bot/init.luau`.

### 4. The report

Per second, from the server's `GET /metrics` (ADR 0220): the tick's mean,
from two counters `/metrics` gains for it -- `ticks_total` and
`tick_ms_total`, since the world began, as the byte counts are; its
`tick_ms_mean` is the last minute's, which a person reads and a per-second
sample cannot -- players, memory, bytes out and in. The summary is of the
seconds every bot was in; joining and leaving cost more (a newcomer is sent
the whole of what is near it) and are reported apart as the peak. From the
bots: joins that failed and why, drops, retries, bytes in, snapshots,
travels, behaviors that failed and why. A summary and a JSON file with every
second; the exit code is 0 when every bot got in and stayed, every behavior
ran, and no second of the plateau took longer than sixty ticks may.

A light bot does not predict, so the corrections a player's machine makes
are not in the report; what the server receives and sends is what it would
with players (see Proof).

### 5. Bounded

Bots go only to an address given on the command line; a test against
somebody else's server is theirs to answer for. The command refuses more
than 1,000 bots from one machine.

## Script surface

```
ludwerk server loadtest 127.0.0.1:7777 --bots=200 --seconds=120 --behavior=crowd
ludwerk server loadtest play.example.com --bots=500 --behavior=bots/shopper.luau
```

```luau
--!strict
-- bots/shopper.luau: a bot that walks to the market, waits, and walks home.
local Bot = require("@engine/bot")

while true do
    Bot:WalkTo(vector.create(40, 0, 12))
    Bot:Wait(math.random(5, 15))
    Bot:WalkTo(Bot.Spawn)
    Bot:Say("back")
end
```

## Proof

- The runs ADR 0228 starts from, reproduced by the command, with the same
  numbers within 10%.

## Measured

Development build, one machine, the same server binary and project for each
pair; the hand-made runs are the script ADR 0228's numbers came from, a full
engine per process.

| Run | Server tick (ms) | Out (KiB/s) | In (KiB/s) |
|---|---|---|---|
| 32 players, one per process, by hand | 0.60 | 81.4 | 154.4 |
| 32 light bots | 0.46 | 81.5 | 154.5 |
| 128 players, four per process, by hand | 5.06 | 333.4 | 268.8 |
| 128 light bots | 5.44 | 648.6 | 826.2 |

- **What the server sends and receives is a player's**: with one player a
  connection on both sides, the bytes agree to 0.1%.
- **The tick agrees within 10% at 128** (7.5%). At 32 the light run is 0.14
  ms faster; 32 full engines beside it took 0.94 of a core, the bots 0.1.
- **Four players a connection was not 128 players.** The hand-made runs
  shared a connection among a process's four local players, so the server
  sent a quarter of the streams and received a quarter of the packets: half
  the bytes out and a third of the bytes in of 128 real players. The numbers
  ADR 0228 starts from understated the network; the command's do not.
- 64 bots are 0.27 of a core and 62 MB; 128 joined from one address in
  about fourteen seconds, none refused, with `--max-per-address=0`.
- `travel` against `examples/47-realms`' manager, eight bots, forty seconds:
  154 journeys between the town, the fields and the dungeons the manager
  opened and closed, none failed.

## What one machine proves, and what needs another

With 64 bots a process, 500 connections are eight processes, which one
development machine runs beside its server. So **on this machine**: 500 real
connections; 300 players in one square inside their budgets (ADR 0229);
channels opening and closing by themselves (ADR 0230); a dilated battle with
no spurious corrections (ADR 0233); the line (ADR 0232). Every one of them
over the loopback interface: real sockets, real encryption, no real network.

**Another machine** -- a VM, which costs money and so is not this ADR's to
decide -- is needed only for what the network itself does: latency, loss and
the server's uplink filling under 500 players. The bots' own `--net-delay`,
`--net-jitter` and `--net-loss` stand in for those meanwhile.

## Consequences

- The scale proofs of this phase are a command anyone can run again.

## Not decided here

Bots on several machines driven from one (the report can be merged by hand).
A bot that predicts as a player's machine does, to measure corrections:
that is a full engine per player, which the hand-made script still is.
