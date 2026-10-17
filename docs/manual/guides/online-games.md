# Online games

A match is one world: a server and the players in it. An **online game** is
several worlds at once -- a town, the fields beyond it, a dungeon made for
each party that asks -- each its own process, one game. Players move between
them, worlds tell each other what happened, and one program on the server
machine keeps them all running: the **manager**.

Everything on this page runs on the server's own binary, the one a
`windows-server` or `linux-server` export makes. Nothing else is installed.

## The manager and `servers.toml`

`servers.toml`, beside the game's `project.toml`, names the worlds:

```toml
[manager]
port = 7777                 # the front door: where every player connects
world_ports = "7800-7899"   # the worlds' own ports, taken from this range
public_address = "203.0.113.10"   # what players are told to connect to

[[world]]
name = "town"
scene = "scenes/town.scene.json"
max_players = 64
entry = true                # the front door sends every newcomer here

[[world]]
name = "fields"
scene = "scenes/fields.scene.json"

[[instance]]                # a scene copied for one group at a time
template = "dungeon"
scene = "scenes/dungeon.scene.json"
max_players = 10
max_instances = 20
warm = 1                    # started ahead, so a party waits for nothing
idle_close_seconds = 60
```

Start it with `ludwerk server run` in that folder, or the binary itself:

```
engine-host --manager=servers.toml
```

It starts every world without a window, watches each by a heartbeat, starts a
fallen one again -- `restarts` times in `restart_minutes`, five in ten by
default, after which it is *failed* until somebody says otherwise -- and keeps
each world's output in `logs/<world>/<world>.log`, a new file past
`log_megabytes`, `log_files` of them kept.

**The front door** is the one address players know. It answers a player's
first packet with where to go -- the entry world -- and a signed pass for it;
their machine goes there behind a loading screen, and the game sees a player
who arrived. The front door holds nobody: it only answers and sends.

### Its console

The manager reads lines from the terminal it runs in:

| Command | Does |
|---|---|
| `worlds [--watch]` | Each world and instance: state, port, players, tick time, restarts. `--watch` draws the table again each second until Enter. |
| `instances` | Each instance, its template, state and players. |
| `restart <world>` | Ends a world and starts it again, a failed one too. |
| `end <world>` | Ends a world's process as a crash would, to try what the game does about one. |
| `stop [minutes]` | Every world counts down, saves and lets its players go; then the manager ends. |
| `update <folder>` | The build in that folder takes over, a world at a time. See below. |
| `<world>: <line>` | A line for one world's own console: `town: status`, `town: kick Player3`. |

### Updating without dropping anybody

`update <folder>` -- or `ludwerk server update <folder> --key=ana.key` from
another terminal, through the manager's administration (below) -- points the
manager at a new build. Each world in turn: the new
build's copy starts on another port and becomes ready, the old one stops
taking players, and its players are moved to the new one with the same
loading screen a trip between worlds has; then the old one stops. New
instances start from the new build at once; running ones finish on the old.

A build that speaks another protocol is refused by its first world, and the
update stops there with the old build still running.

### Running it as a service

```
ludwerk server install [folder] [--name=realms-servers]
ludwerk server uninstall [--name=realms-servers]
```

On Linux this writes a systemd unit -- run it with `sudo` -- that starts the
manager with the machine and again after a fall, as the user who installed
it; `journalctl -u <name>` shows its output. On Windows it registers a task
that runs at startup as the system; run it from a terminal opened as
administrator. Nothing is installed without the command.

## Sending a player to another world

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

NetworkService:Travel(player, "fields", { Gift = "a lantern" })
NetworkService:TravelGroup(party, "fields")   -- together, in this order
```

Their machine leaves this world and joins that one. Their character does not
travel: the world they arrive in makes them one. In that world,
`NetworkService.TravelArrived` fires after `PlayerAdded` with what was sent --
anything `SaveService` can store, at most 16 KiB -- and the name of the world
they came from.

**The pass** is what lets them in: signed by the manager, naming the player,
both worlds and when it was made, good for thirty seconds and for one use. A
copied pass, an old one or a made-up one is refused at the door of the world
it names.

**On the client**, `NetworkService.TravelStarted` fires before the machine
leaves, with the world's name, and `Connected` when the next world has taken
it. The engine's loading screen does both in one line, from a script under
`GlobalScriptService.Client`, which a trip does not stop:

```luau
require("@engine/travelscreen").mount()
```

Anything a client script made in the world it left went with that world:
make a camera again on `SceneService.SceneLoaded`, and keep input contexts
under `InputService`, which every world shares.

## Worlds and instances

An instance is a copy of a template's scene for one group -- a dungeon, an
arena, a house -- in a process of its own:

```luau
--!strict
local InstanceService = game:GetService("InstanceService")
local NetworkService = game:GetService("NetworkService")

local id = InstanceService:CreateInstanceAsync("dungeon", {
    Players = party,
    Data = { Difficulty = 3 },
})
NetworkService:TravelGroup(party, id)
```

`CreateInstanceAsync` yields until the instance is ready -- at once when a
warm one is waiting -- and raises with the reason when the template already
runs `max_instances`, the machine is short of memory, or it would not start.

- **Only the group may enter**, by their keys. `InstanceService:AllowPlayer`
  lets a latecomer in; `DisallowPlayer` stops letting one in.
- **A player who drops comes back to it** while it lives.
- **It closes when it has been empty** for `idle_close_seconds`, or when its
  script calls `InstanceService:Close`. `KeepOpen(seconds)` keeps it open
  meanwhile. Before it stops, `Closing` fires there, for its script to keep
  what it keeps, and five seconds later it is gone.
- Its scene reads what it was handed with `SceneService:GetLoadData`, and
  `InstanceService.InstanceId` is its id.

**The world that made it** hears `InstanceService.InstanceClosed(id, reason)`
when it is gone: `"Closed"`, or `"InstanceFailed"` when it stopped without
closing. Its players are then sent back where they came from, and their
`TravelArrived` carries `reason` `"InstanceFailed"`, so the game can say what
happened and give back what the run cost.

## Messages between worlds

`CrossWorldService` carries events from one world to the others -- a chat
line, a boss that fell, a market price:

```luau
--!strict
local CrossWorldService = game:GetService("CrossWorldService")

CrossWorldService:Subscribe("chat", function(message: any, origin: string)
    print(`{origin}: {message.Text}`)
end)
CrossWorldService:Publish("chat", { Text = "the gates are open" })
```

A message is at most 4 KiB, a topic at most 80 bytes, and a world publishes at
most 200 a second. Delivery is at most once: a world that is restarting misses
what was sent meanwhile, and one older than ten seconds when its turn comes is
dropped. It is for events, never for movement. A world on its own, with no
manager, hears what it publishes itself, so the same code is tried on one
machine.

For a service outside the game -- a chat server, a backend that pushes --
`require("@std/net").websocket(url)` opens a WebSocket from the server:

```luau
--!strict
local net = require("@std/net")

local socket = net.websocket("wss://chat.example.com/room", {
    headers = { Authorization = "Bearer " .. token },
})
socket.Received:Connect(function(message: string | buffer)
    print(message)
end)
socket:Send("hello")
```

It yields until the socket is open and raises when it cannot be. Messages are
text or binary, at most 1 MiB, and at most eight sockets are open at once. A
client's game opens one only with `websocket_client = true` under
`[permissions]` in `project.toml`.

## Trying it in the editor

With a `servers.toml` in the project, the Run view has **This game's worlds**.
Ticked, Start match runs the manager on this machine, every world without a
window, and every player in the match a client of its front door, as on a
real server:

- the **Run view** lists each world and instance with its state, players and
  tick time. *End* on a row ends that world's process as a crash would.
  *Restart with this build* moves every world to the scripts as they are now,
  through the same update a server does, so nobody is dropped;
- the **Console** has a tab for each world with what it writes, and a line
  typed in a world's tab goes to that world's console;
- at most eight worlds and instances run at once: an instance past that is
  refused, with the reason in the Console.

Stop stops the manager, which stops every world.

## Administering a server from another machine

A dedicated server or a manager can take its console from another machine
too. It is off until it is turned on, and nobody gets in until somebody is
named.

1. **Make an administrator's key**, on their own machine:

   ```
   ludwerk server keygen --name=ana
   ```

   This writes `ana.key`, readable by its owner only, and `ana.pub`, and
   prints the public key.

2. **Name them on the server**, in `admins.toml` -- beside the game's saves
   for a dedicated server, beside `servers.toml` for a manager:

   ```toml
   [[admin]]
   name = "ana"
   key = "f0f48dce623433e7..."   # the line keygen printed
   level = 3                      # 0 to 3, as a game's commands ask
   ```

   The server refuses to read an `admins.toml` that others than its owner can
   read. On a manager, only level 3 may connect: a line for a world runs there
   as its operator's.

3. **Turn it on**: `[server] admin = true` in `project.toml`, or `--admin`,
   for a dedicated server; `admin = true` under `[manager]` in
   `servers.toml` for a manager. It listens on the game's port plus one, or
   `admin_port`, and **only on the machine's own address** unless
   `admin_allow` names others -- `admin_allow = ["203.0.113.4",
   "10.0.0.0/8"]`. A connection from anywhere else is closed before a byte of
   it is read.

4. **Connect**:

   ```
   ludwerk server connect play.example.com --key=ana.key
   ludwerk server connect play.example.com --key=ana.key --command="players"
   ludwerk server connect play.example.com --key=ana.key --follow
   ```

   A prompt, each line answered; `--command` runs one line and exits with its
   outcome, for a script; `--follow` also prints the server's log as it is
   written.

**Both ends are a key.** The handshake is the one a match uses: the server
learns the administrator's key and finds it in `admins.toml`, and the
administrator learns the server's. The first server met at an address is
remembered, and a different one there later is refused before anything is
sent; `--server-key` names the key to expect instead. Every message after the
handshake is sealed: nothing on the way reads, changes or plays one back.

**Bounded**: four administrators at once; an address that fails five
handshakes in a minute is not answered for ten; a session with nothing for
ten minutes is closed; a line is at most 4 KiB and an answer is cut at 1 MiB.
Every session, every command with who sent it, and every refusal is in the
log.

Keep `.key` files and `admins.toml` out of version control. A `.key` is the
person: whoever has the file can do what they can.

## Health, for whatever watches

A load balancer, a container scheduler or a dashboard asks a world how it is over
plain HTTP:

- `GET /health` answers `200 {"ready":true}`, or `503 {"ready":false}` while
  the world is starting or stopping, so nobody is sent to one that would
  refuse them;
- `GET /metrics` answers the world's numbers as one JSON object: `world`,
  `state`, `uptime_seconds`, `tick`, `tick_ms_mean`, `tick_ms_worst`,
  `players`, `max_players`, `memory_bytes`, `scene`, `encrypted`,
  `wire_bytes_sent` and `wire_bytes_received`.

Turn it on with `[server] health = true` or `--health` for a dedicated server
-- on the game's own port number over TCP, or `health_port` / `--health=PORT`
-- and with `health = true` under `[manager]` for every world a manager runs,
each on its own port number. It answers the machine's own address only,
unless `health_allow` names others as `admin_allow` does. It says numbers and
nothing else: no command reaches a world this way.

## What is not here

- Worlds on several machines: one manager runs every world on its own machine.
- A second front door: while the one front door is down, no new player gets
  in, and those already in a world stay.
- Accounts themselves: see [Who may join, and accounts](manual:guides/multiplayer).
