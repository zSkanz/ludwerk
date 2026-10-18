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

### Its keys: back up the front door's

**A player's game remembers the front door's key**, the first time it joins
the game's address (see [A match is encrypted](manual:guides/multiplayer#a-match-is-encrypted)),
and refuses a front door with another key afterward: somebody may be in the
way. The worlds behind it are not remembered: the front door, or the world a
player leaves, names the next world's key as it sends them there, so a world
that restarts, or is moved to a new build by `update`, may have a new key and
nobody notices.

So the key to keep is the front door's: `keys/front-door.key`, beside
`servers.toml`. It is made the first time the manager starts and kept there
across restarts and updates. **Back it up, and carry it when the game moves
to another machine.** Lose it, and every player who joined before is refused
with *this server's key is not the one this machine saw* until they choose to trust the server again --
which the game offers them with `NetworkService:ForgetServerKey`, never the
engine on its own -- or until a client with the new `server_key` ships. The
same holds for a lone dedicated server's `keys` folder (see
[A dedicated server's key](manual:guides/shipping#a-dedicated-servers-key)).

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

**A player who left is not sent, and is no error.** A party member can leave
while the server waits -- for `CreateInstanceAsync`, for a backend -- and
their `Player` goes with them. `TravelGroup` skips them and sends the rest
together, and returns who it sent; `Travel`, `SwitchChannel` and
`TravelToPlayer` return whether they sent anybody. Only what is not a player
at all is an error.

```luau
local id = InstanceService:CreateInstanceAsync("dungeon", { Players = party })
local sent = NetworkService:TravelGroup(party, id)   -- whoever is still here
```

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

## Channels: a full world opens another of itself

A town holds as many players as one server can tell about each other. On a
launch day, more come. A `[[world]]` with `channels = true` is copied into
**channels** -- each a server of its own, from the same scene -- as it fills:

```toml
# servers.toml
[[world]]
name = "town"
scene = "scenes/town.scene.json"
entry = true
max_players = 150
channels = true
max_channels = 8
channel_open_at = 0.9        # of max_players, in every open channel
channel_close_seconds = 120  # empty that long, and a channel closes
merge_minutes = 10           # under a quarter full that long, and it is merged; 0 never
```

- **Opened as they fill.** When every open channel holds nine tenths of its
  players, the manager starts the next: `town:2`, `town:3`, up to
  `max_channels`, and no more than the machine has memory for.
- **The fullest with room.** A player sent to `town` -- by the front door or
  by `Travel` -- goes to the fullest channel with room for them, so crowds
  feel alive and fewer channels stay open. `TravelGroup` keeps a group in one
  channel.
- **To somebody.** `NetworkService:TravelToPlayer(player, friend.PublicKey)`
  sends a player to the world and channel where the other is, and
  `SwitchChannel(player, 3)` to channel 3 of this world, for a menu that
  lists `GetChannelsAsync()`.
- **Fewer when the crowd goes.** A channel other than the first, empty for
  `channel_close_seconds`, closes. One under a quarter full for
  `merge_minutes` is merged: its server hears `NetworkService.ChannelMerging`
  with the seconds left, and then its players are moved, all together, to
  the fullest channel with room.

`NetworkService.Channel` says which channel a server is (1 without channels).
Channels share what a world shares across the game: the same saves, the same
`CrossWorldService` topics, the same bans.

**Two channels saving one slot** take turns: each save is written whole and
renamed into place while it holds that slot's lock, so the file is never
torn and the backup is always the save before it. **The last save to finish
is the slot** -- a slot is never merged -- so a channel that saves a slot
another one also writes replaces what that one saved. Keep what is each
player's in a slot of their own, which only the channel they are in writes;
keep what the whole world shares in one channel's slots, and tell the others
with `CrossWorldService`. The `worlds` command lists each
channel as a world of its own, `town:2` and on.

## A line at the door when the game is full

When nobody has room for a newcomer -- every channel of the entry world full
and no more to open, or the whole game at its `max_players` -- the front door
keeps them **in line** instead of turning them away:

```toml
# servers.toml
[manager]
max_players = 2000          # the whole game, every world and channel; 0 for no limit
queue_max = 10000           # past this many waiting, the door says the game is full
queue_grace_seconds = 60
```

- **First in, first out**, as places free. Each player waiting is told their
  place when they join the line and every five seconds after, with an
  estimate measured by how fast the line moved in the last five minutes --
  none before anybody has left it.
- **A player who drops while waiting keeps their place** for
  `queue_grace_seconds`; one who drops out of a world may come back to it in
  that while without waiting at all.
- **On the player's machine**, `NetworkService.Queued(position,
  estimateSeconds)` fires each time; `Connected` fires as usual when they are
  let in, and waiting is never a join that failed. `@engine/travelscreen`
  shows the line, with no change to a game that mounted it.

```luau
NetworkService.Queued:Connect(function(position: number, estimateSeconds: number?)
    -- "You are number {position}", and the minutes when there are some
end)
```

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

## A player's record

`DataService` keeps what a player has -- a level, an inventory, money -- in
the game's database, which every world of the game shares. The manager keeps
it in `data/<project id>.db` beside `servers.toml`; a server run with no
manager keeps its own, `data.db` in its saves folder, and the code is the same.

```luau
--!strict
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")

NetworkService.PlayerAdded:Connect(function(player: Player)
    local record = DataService:LoadAsync(player)
    record:Set("Level", ((record:Get("Level") :: number?) or 0) + 1)
end)
```

A record holds what a save slot holds, and it is held by one world at a time:
the world the player is in. It is written behind -- every 30 seconds while it
changes, when the player leaves or travels, and when the world stops -- so a
world that crashes loses at most the last 30 seconds of it. A world that asks
for a record another world still holds waits up to ten seconds for that world
to let it go, and then raises an error that begins with `DataBusy`.
`record:Save()` writes it at once, and `record:Release()` writes it and lets
it go.

A record belongs to the player's account where they joined with one (see
[Who may join, and accounts](manual:guides/multiplayer)), and to the key
their copy of the game joined with where they did not.

### Items and money

What changes hands is not in a record. An item is a row of its own, with an
id that is never given twice, and money is a balance per owner and currency.
Both move only in a transaction, which makes all of its moves or none:

```luau
--!strict
local DataService = game:GetService("DataService")

local function sell(seller: Player, buyer: Player, swordId: number)
    local ok, reason = DataService:TransactAsync({
        { Move = swordId, From = seller, To = buyer },
        { Pay = 250, Currency = "Gold", From = buyer, To = seller },
    })
    if not ok then
        print(reason) -- ItemNotThere, NotEnough
    end
end
```

A transaction is on the disk before `TransactAsync` returns, so a server that
crashes a moment later has done it, and one that crashes before has not:
nothing is duplicated and nothing is lost. Every move is logged with its
transaction and its world. An owner is a `Player`, or a name the game gives
what no player owns, such as a guild's bank. For money, nil is the game
itself: that is how gold enters the game, as a reward, and leaves it, as a
purchase from the shop. `CreateItemAsync`, `DestroyItemAsync`,
`GetItemsAsync` and `GetBalanceAsync` complete it.

### Values no player owns

An auction's listings, a guild's bank, a world event's progress: a shared
value belongs to the game, and any world may change it.
`DataService:UpdateAsync` reads it, calls your function with it, and writes
what the function returns. If another world wrote first, it calls the
function again with what that world wrote. No world waits on another's
script:

```luau
--!strict
local DataService = game:GetService("DataService")

local total = DataService:UpdateAsync("event", "wolves", function(old: any): any
    return ((old :: number?) or 0) + 1
end)
```

`GetAsync` returns a value and its version, and `SetAsync` writes only at
the version it is given, for a game that wants that loop in its own hands.

### When a record's format changes

Set `DataService.Version` when the game's records change shape, and
`OnMigrate` brings an older record up to date the first time it is loaded:

```luau
--!strict
local DataService = game:GetService("DataService")

DataService.Version = 2
DataService.OnMigrate = function(record: DataRecord, fromVersion: number)
    if fromVersion < 2 then
        record:Set("Purse", { Gold = record:Get("Gold") })
        record:Remove("Gold")
    end
end
```

The database is SQLite's. Copy it, with the front door's key, to keep the
game's players: they are what a server's operator must not lose.

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
  `state`, `uptime_seconds`, `tick`, `tick_ms_mean` and `tick_ms_worst` (over
  the last minute), `ticks_total` and `tick_ms_total` (since the world began:
  the mean between two samples is the one's difference over the other's),
  `ticks_per_frame` (1 for a world keeping up), `players`, `max_players`,
  `memory_bytes`, `scene`, `encrypted`, `wire_bytes_sent`,
  `wire_bytes_received`, `deferred_per_second` (updates the players'
  budgets held back in the last second), and `time_dilation` (1 at full
  speed).

Turn it on with `[server] health = true` or `--health` for a dedicated server
-- on the game's own port number over TCP, or `health_port` / `--health=PORT`
-- and with `health = true` under `[manager]` for every world a manager runs,
each on its own port number. It answers the machine's own address only,
unless `health_allow` names others as `admin_allow` does. It says numbers and
nothing else: no command reaches a world this way.

## An overloaded world slows down evenly

A battle can need more than a tick's time to simulate. Without help the
server falls behind: ticks arrive late and in bursts, and every player's
character is corrected at once. With **time dilation** a world slows its
clock instead -- everything moving at, say, half speed, smoothly -- and says
so:

```toml
# project.toml
[server]
time_dilation = true
time_dilation_floor = 0.25   # never slower than a quarter of full speed
```

or `time_dilation = true` on one `[[world]]` in `servers.toml`, or
`--time-dilation[=floor]` for a dedicated server.

- **The tick is the same; fewer of them run a second.** `dt` and what a tick
  does do not change, so the world is the one it would have been, only
  later.
- **Down at once to what fits, back up slowly.** A world that spent more
  than nine tenths of a second simulating goes straight to the rate at which
  its ticks take seven tenths; once three seconds in a row took less than
  half, it speeds up by a twentieth.
- **Every machine runs at the rate.** It travels in every snapshot, and a
  player's machine ticks at it too, so nothing is corrected because the
  server slowed down. `task.wait`, and what the simulation drives, slow with
  it; the interface, the camera and `os.time` do not.

`RunService.TimeDilation` is the rate, 1 at full speed, and
`RunService.TimeDilationChanged` fires when it changes, so a game can say
"the battle is slowed to 60%". A dedicated server's `/metrics` has
`time_dilation`.

## What no player is near sleeps

A world of an online game is mostly empty of players at any moment, and its
cost should follow them. On a dedicated server, a `CharacterBody` that no
player's focus -- their `ReplicationFocus`, or their character -- is within
`dormant_radius` of does not walk: it stays where it was, and what it was
asked to do (`Move`) waits until a player comes near. A player sees only as
far as `StreamingService.LoadRadius`, and the radius is twice that unless the
game says otherwise, so a character wakes before anybody can see it.

```toml
# project.toml
[server]
dormant_radius = 300   # metres from every player's focus; 0 is never
```

- `BasePart.Dormancy = Enum.Dormancy.Never` keeps a character awake
  wherever it is: a train, a boss that roams, a courier others wait for.
- `BasePart.Dormant` says whether a part is asleep this tick.
- A host and a single-player game sleep nothing, and neither does a world
  any of whose players has no character or focus: that player is sent the
  whole world, and sees all of it.

It is decided where each tick's physics step begins, from where the players
are then, so a world runs the same every time it is run (R10). A sleeping
character costs the server nearly nothing: it is not stepped, and what is
sent to players is not read from it again until something changes it.

**An NPC's own code** can follow the same rule with `@engine/npclod`, which
calls it as often as its distance from the nearest player deserves -- every
tick within 30 meters, every fourth within 120, every sixteenth beyond, and
not at all while it sleeps -- with the time since the last call:

```luau
--!strict
local npclod = require("@engine/npclod")

for _, guard in workspace.Guards:GetChildren() do
    npclod.think(guard, function(dt: number)
        -- look around, choose a target, walk: `dt` is the time since the last call
    end)
end

-- The distances and the rates, if the game wants others.
npclod.configure({ Near = 30, NearEvery = 1, Middle = 120, MiddleEvery = 4, FarEvery = 16 })
```

`Workspace:GetNearestPlayerDistance(position)` answers the same question
for a game's own decisions -- `math.huge` when no player has a focus.

## Characters step on several threads

The characters awake in a tick step on several threads at once when nothing
they could change is near them: no part that is not anchored, and no other
character that could reach the same place in the same tick. Those that could
-- a crowd pressing on one spot, players running into one another, someone
pushing a crate -- step one after another, as they always have. Either way a
world runs the same every time it is run, on any number of threads, and two
characters never end up inside one another.

A dedicated server steps on four threads beside its own. A machine running
several worlds can give each fewer:

```toml
# project.toml
[server]
physics_threads = 1   # 0 steps everything on the server's own thread
```

## How many players it holds: a load test

`ludwerk server loadtest` sends bots at a running server and reports what the
server did while they were in:

```
ludwerk server loadtest 127.0.0.1:7777 --bots=200 --seconds=120 --behavior=crowd
```

The server needs its health on (`--health`), because the report is the
server's own `/metrics`, read each second; `--metrics=host:port` names it
when it answers elsewhere. Every bot comes from this machine's one address,
so for the test start the server with `--max-per-address=0`, or set
`max_connections_per_address = 0` under `[network]`: by default a server
takes 64 players from one address, and the bots after the 64th are refused
with that reason.

**A bot is a connection, not a game.** It joins as a player does -- the same
handshake, encrypted, with the same Luau flags -- and keeps what the server
sends it, and sends input back. It runs no picture, no physics and none of
the game's scripts, so one machine holds many: one process runs up to 64
bots, using about a quarter of a core and 62 MB, and the command starts as
many processes as it needs, up to 1,000 bots. Wherever the server sends a bot
-- a front door, `Travel`, a party's instance -- it follows.

What a bot does is its behavior, `--behavior=`:

- `wander` (the default): walks to places near where it started;
- `crowd`: everybody walks to the same few meters round the world's middle;
- `travel`: walks to the parts tagged `Portal` -- tag the pads your server
  sends players on from -- and, with none, wanders;
- `idle`: stands;
- a Luau file of the game's own, `--behavior=bots/shopper.luau`.

A bot walks with the intent `Move`, a `Direction2D`, as a stick would;
`--bot-intent=` on `engine-host` names another.

### A behavior of your own

```luau
--!strict
-- bots/shopper.luau: to the market, a while there, and home.
local Bot = require("@engine/bot")

while true do
    local stalls = Bot:FindTagged("Stall")
    if #stalls > 0 then
        Bot:WalkTo(stalls[math.random(1, #stalls)])
    end
    Bot:Wait(math.random(5, 15))
    Bot:WalkTo(Bot.Spawn)
    Bot:Say("back")
end
```

`Bot:WalkTo(position, seconds?)` walks there and waits until it arrives,
within two meters, or the time (30 seconds unless given) runs out, and says
which. `Bot:Wait(seconds)` waits. `Bot:GetPosition()` is where the bot's
character is. `Bot:FindTagged(tag)` is where the parts with a tag are, in
what the server sent the bot. `Bot:Say(text)` writes a line to the bot
process's output. `Bot.Spawn` is where the bot stood when its behavior
began, and `Bot.Index` which bot of its process it is.

A behavior has the Luau libraries and `@engine/bot`, and nothing else: no
`game`, no services, no other module. Every bot runs the file from its
beginning, and again in each world it is sent to. The bots of a process
share one thread, so a loop has to wait -- one that runs a quarter of a
second without `Wait` or `WalkTo` is stopped, with the reason.

### The report

```
Bots: 128 of 128 in; 0 could not join, 0 were dropped.
Server, the 45 seconds all 128 were in: a tick took 5.44 ms on average,
6.05 ms in its slowest second, at 60.5 ticks a second; 648.6 KiB a second
out and 826.2 KiB in (2003.4 at most, as players joined or left); 80 MB of
memory at most.
```

The numbers are those of the seconds every bot was in. Joining costs more
than staying -- a newcomer is sent the whole of what is near it -- so the
peak while players joined or left is said apart. A join refused, and why,
and a behavior that stopped, and why, are printed once each.

`--report=loadtest.json` (the default) keeps everything: those numbers, each
second's, and every sample of `/metrics`. The command ends with 0 when every
bot got in and stayed, every behavior ran, and no second took longer than
its sixty ticks may; else with 1, so a build pipeline can run it.

A bot sends what a player of the game sends, so what the server receives and
sends is what it would with players; what a bot does not do is predict, so
the corrections a player's machine makes are not measured here. Bots go only
to the address on the command line: a test of somebody else's server is
yours to answer for.

## What is not here

- Worlds on several machines: one manager runs every world on its own machine.
- A second front door: while the one front door is down, no new player gets
  in, and those already in a world stay.
- Accounts themselves: see [Who may join, and accounts](manual:guides/multiplayer).
- A backup taken while the game runs, and a game's own backend in place of
  the database: the rest of the data service is still to come.
