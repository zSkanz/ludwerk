# 0220 — A manager runs a game's worlds from `servers.toml`

- Status: accepted; built, with the states, the dashboard and the service of "As built"
- Date: 2026-10-14
- Builds on: ADR 0218 (the console), ADR 0219 (remote administration),
  ADR 0106 (a scene changed at run time, a match joined from a script),
  ADR 0188 (an optional provider outside the core)

## The question

An online game is several processes: a town, a field, a dungeon started for
each group that enters it. Today each is an `engine-host --serve` somebody
starts by hand, that nobody restarts when it falls, whose logs grow without
end, and whose update means everybody on it is dropped. A player joins one by
its port, so a world that moves is a world nobody finds.

## Decision

### 1. The manager is the engine's, not the tools'

`engine-host --manager servers.toml` -- and `ludwerk server run`, which finds
the engine and calls it -- is a process that starts the worlds, watches them,
restarts them, keeps their logs, and answers for them. **Native, in the
server's own binary**: a production machine runs what the game's package
carries, and Lute and the CLI are development tools (ADR 0003). The player
profile does not carry it.

### 2. It speaks to its worlds over their pipes

A world started by the manager (`--managed`) keeps its console on its
standard input (ADR 0218) and adds one more kind of line: a JSON message
prefixed so a console command can never be mistaken for one. Both ways:
`ready`, `heartbeat` (each second: tick time, players, memory),
`player-joined`, `player-left`, `travel` (ADR 0222), `publish` (ADR 0223)
from the world; `allocate`, `drain`, `deliver`, `stop` to it. **No port is
opened for this**: the pipes are the manager's and its children's alone, so
there is nothing for another program on the machine to reach, which is the
reason ADR 0035 keeps the engine from listening for its tools.

### 3. `servers.toml`

```toml
[manager]
public_address = "play.example.com"   # what players are sent to
port = 7777                           # the front door (§4)
world_ports = "7800-7899"             # the worlds' own, open to players
logs = "logs"                         # one folder a world, rotated
log_megabytes = 10                    # a file this large starts the next
log_files = 5                         # and this many are kept
restarts = 5                          # at most this many
restart_minutes = 10                  #   in this long, then the world is Failed

[[world]]
name = "town"                         # what Travel names (ADR 0222)
scene = "scenes/town.scene.json"
max_players = 200
entry = true                          # where the front door sends a new player

[[world]]
name = "fields"
scene = "scenes/fields.scene.json"

[[instance]]                          # ADR 0221
template = "crypt"
scene = "scenes/crypt.scene.json"
max_players = 5
max_instances = 40
warm = 2
idle_close_seconds = 60
```

Ports are given out of `world_ports` by the manager, so a world's port is the
manager's to choose and can change; a player never types one.

### 4. The front door

The manager answers players itself at `[manager] port`: a hello there is
answered with **`Redirect`** -- a new message, the address and port of a
world and a pass for it (ADR 0222) -- and the client joins that world as it
would any. The front door carries no world and no game; it is a fixed place
a game ships in its build (`[network] server`), that stays where it is while
the worlds behind it start, stop, restart and update. It is also where a
login queue and a choice of channel go later, with the work on scale.

### 5. A world's life

`Starting` (the process is up, the scene loading) → `Ready` (listening, its
port known) → `Allocated` (a fixed world as soon as it is ready; an instance
when it is given to a group, ADR 0221) → `Draining` (no new player: those who
come are redirected; those on it are moved or finish) → `Stopped`.

- **Health** is the heartbeat. None for ten seconds and the world is ended
  and started again, its players sent back through the front door, which is
  where a reconnecting client goes first.
- **A crash** is restarted the same way, up to `restarts` in
  `restart_minutes`; past that the world is `Failed`, said in the log and the
  dashboard, and not started again until an operator says so.
- The state is reported through one interface, so an optional provider
  outside the core (ADR 0188) can tell a cloud fleet's scheduler the same
  -- Agones has these five states by other names. None ships here.

**When the manager itself ends**, its worlds end with it: they are its
children, and a world without its manager stops on its own when its pipe
closes. The players are sent back through the front door when the manager
is started again -- by the service (§9), which restarts it -- and a
reconnecting player's instance is gone with the rest. Simple and honest: a
world that outlived its manager and had to be found again is a second way
for things to go wrong, kept for a later record if restarts ever need it.

### 6. Logs

Each world's output goes to `logs/<world>/<world>.log`, started anew at
`log_megabytes`, `log_files` kept. The manager's own is `logs/manager.log`.

### 7. An update that drops nobody

`update <folder>` (the manager's console, or `ludwerk server update`) points
the manager at a new build of the game:

- New instances start from it at once, and warm ones are replaced; running
  ones finish on the old build.
- Each fixed world in turn: the new build starts on another port and reaches
  `Ready`; the old one goes `Draining`, and its players are moved to the new
  one by Travel (ADR 0222) -- a loading screen of a second or two, the same
  party together -- then it stops. The front door sends new players to the
  new one from the moment it is ready.

A build whose protocol differs is refused by its first world (ADR 0100), and
the update stops there with the old build still running.

### 8. The dashboard and the console

In a terminal the manager draws a table of its worlds -- state, players, tick
time, restarts, port -- refreshed each second, under a prompt with its own
commands (`worlds`, `instances`, `restart <world>`, `stop [minutes]` for all,
`update <folder>`, and `<world>: <command>` to send a line to one world's
console). Without a terminal it logs instead. Remote administration (ADR
0219) reaches the manager the same way it reaches a world.

### 9. As a service

`ludwerk server install` writes a systemd unit on Linux, or registers a
Windows service, that runs the manager in the folder it was given and starts
it with the machine. `uninstall` takes it away. Nothing is installed without
the command.

## As built

- **`engine-host --manager=servers.toml`**, and `ludwerk server run`, which
  runs that in a folder with a `servers.toml` and one dedicated server in any
  other.
- **The pipes carry lines that begin with the record separator (0x1E)**,
  then one JSON object with a `type`: what a console line can never begin
  with. Each world says `hello`, `ready` (its port, key and protocol), a
  `heartbeat` each second, `player-joined` and `player-left`; travel,
  instances and publishing have their own (ADRs 0221 to 0223).
- **The states are `Starting`, `Ready`, `Draining`, `Stopped` and `Failed`.**
  `Allocated` is not a state of a world: an instance given to its group is a
  ready world with a group, and a warm one is a ready world with none.
- **The front door is a match's own transport**, encrypted, with a key of its
  own (`keys/front-door.key`); the passes are signed with the manager's
  Ed25519 key (`keys/manager.key`), kept across restarts.
- **The dashboard is `worlds --watch`**: the table again each second, the
  screen cleared where the terminal reads the sequence, until a line is
  typed. A full-screen terminal interface was not built; the table and a
  prompt cover the same ground with no terminal library.
- **`end <world>`** ends a world's process as a crash would, which the editor's
  *End* sends (ADR 0225).
- **An update to a folder with no binary of its own** runs this build on the
  game that folder holds: the editor's *Restart with this build*.
- **An update to a folder that holds no game is refused** before anything
  moves, and one whose first world will not start puts the old build back in
  charge of whatever starts next -- restarts and instances included -- as one
  of another protocol does.
- **Each world has its own engine log**, `logs/<world>/engine.log`, beside
  what the manager keeps of its output: every world runs in the game's
  folder, and one `engine.log` there was several processes writing one file.
- **As a service**: on Linux a systemd unit, written by `ludwerk server
  install` (with `sudo`) and enabled; on Windows a task that runs at startup
  as the system, since the server binary is not a Windows service program.
  `uninstall` takes either away.
- **Health over HTTP**, off by default: `[server] health` or `--health` for a
  dedicated server, `[manager] health` for every world a manager runs.
  `GET /health` answers 200 while the world takes players and 503 while it
  starts or stops; `GET /metrics` its numbers as one JSON object. On the
  game's port number over TCP -- each world its own -- on the machine's own
  address unless `health_allow` names others, by ADR 0219's rule.
- **The editor's own mode** (`--manager-play`, `--manager-project`,
  `--manager-root`) is ADR 0225's: the manager says its worlds and their
  lines on its output as messages, and runs at most eight.

## Consequences

- An online game's operator runs one program and edits one file.
- A new message (`Redirect`), in the online server's one protocol bump with the pass
  (ADR 0222) and encryption (ADR 0224).
- Worlds on one machine. Worlds on several is a later record.
- **The front door is one process at one address**: while it is down, no
  new player gets in (those already in a world stay). A second front door
  is a later record.
- Manual: "Online games > Putting a game online" (the manager, `servers.toml`,
  the front door, running it as a service) and "Worlds and instances".

## Not decided here

Instances (ADR 0221), Travel and its pass (ADR 0222), messages between worlds
(ADR 0223).
