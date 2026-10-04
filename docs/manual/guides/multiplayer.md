# Multiplayer

One project runs alone, hosts a match, serves one with no window, or joins one.
A script moves between them with `NetworkService` (ADR 0106):

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

NetworkService.Connected:Connect(function()
    print("in the match")
end)
NetworkService.JoinFailed:Connect(function(reason: string)
    print("could not join:", reason)
end)
NetworkService.Disconnected:Connect(function(reason: string)
    print("back to solo:", reason)
end)

NetworkService:Join("play.example.com:7777") -- or Join() for [network] server
```

| Call | What it does |
|---|---|
| `Join(address?)` | Connects to a server. With no address, `[network] server` from `project.toml`. |
| `Host(port?)` | Makes this machine the authority others join, on port 7777 by default. `HostFailed` says why when the port cannot be opened. |
| `Disconnect()` | Leaves the match, or stops hosting, and goes back to solo. What was sent before it still arrives. |
| `State` | `Offline`, `Connecting`, `Connected`, `Reconnecting`, `Hosting` or `Serving`. |

- **A join changes nothing until the server takes it.** While `State` is
  `Connecting` the machine is what it was: its scene, its menu and its own
  server code go on. If the server never answers, is full, or runs another
  version of the game, `JoinFailed` says which -- to the very code that called
  `Join`, which is still there to hear it.
- **Taken, this machine's scene is replaced with the server's.** What the
  server replicates arrives, and this machine's server code stops: it no longer
  decides the world. The scene's client code -- `ClientScriptService` -- starts
  fresh in the world that arrived. **So `Connected` is heard by code that lives
  across worlds** -- `GlobalScriptService` -- and by the new scene's client
  code from its first line on; the scene that called `Join` has gone by then.
- **Leaving goes back to solo in the scene the join was made from**, not the
  server's: a client never runs the server's scene alone, with its server code,
  as if it were the authority. The other players go, server code starts again,
  fresh, and the player at this machine is player 1 again.
- **`Authority` can change during a run**: false from a join, true again from a
  leave. A scene's scripts start after the change, so code there can read it
  once; code in `GlobalScriptService` that lives across it listens to `State`.
- A dedicated server cannot `Join`, `Host` or `Disconnect`.

The command line is the shortcut: the same calls, made before the first tick.

```
run.bat                        solo: nobody else, and this machine decides
run.bat --host                 a window, and the authority other machines join
run.bat --serve                the authority with no window: a dedicated server
run.bat --join=127.0.0.1       a window showing a world another machine decides
```

A host and a server listen on port 7777; `--join=address:port` names another.
`--serve` is the only way to be a dedicated server. A client started with
`--join=` dials again if the connection drops; one that joined from a script
hears `Disconnected` and decides for itself.

**Joining the same server again is being the same player.** A machine keeps,
for as long as it runs, who it was on each server it joined, and a script's
`Join` to that address presents it: the server welcomes the player back with
the `UserId` it had, so a score or an inventory keyed on it is still theirs. A
game that wants to come back on its own calls `Join` from `Disconnected`.

**A connection that drops is dialled again, for as long as the timeout.** While
it is, `State` is `Reconnecting`: the world stands as it was, and a game shows
it and stops predicting. What happens next depends on who answers:

| Who answers | What this machine gets |
|---|---|
| the same run of the server | the match it was in, as the player it was: `Connected` fires and nothing else changes |
| a server that was restarted | a new join: the world is replaced by the new server's, the scene's client code starts fresh, and `Connected` fires. The old run's `UserId` meant nothing to the new one |
| nobody, for `[network] timeout` | `Disconnected`, and the machine is solo |

So a game needs one rule for all three: draw "reconnecting" while `State` is
`Reconnecting`, and treat `Connected` as "the world is here" every time it fires
-- it fires on every return, not only the first. The first dial of a client
started with `--join=` has no timeout: it waits for a server that is not up
yet.

**A server that goes silent is gone after ten seconds**, whether it closed,
a cable was pulled or its game froze, and the same holds for a player a server
stops hearing. A full server says so at once, and so does one of another
version.
`[network] timeout` in `project.toml` sets it, from 1 to 120 seconds:

```toml
[network]
timeout = 5
```

## How the connection is doing

`NetworkService:GetStats()` is what a connection-quality mark is built from:
`Ping`, `Jitter` and `Loss` as the transport measures them -- on a thread of
its own, so a window in the background reads the same ping as one in front --
how many times this machine's own character was corrected (`Corrections`, and
`CorrectionsPerSecond`), how far in the past the others are drawn
(`InterpolationDelay`, in milliseconds: it grows with a jittery link and comes
back when the link does), the authority's queue of this player's input, and
the unreliable messages this machine sent, took in and dropped.

**A worse network, on purpose.** To see a game the way a player far away does:

```
run.bat --join=127.0.0.1 --net-delay=75 --net-jitter=20 --net-loss=2
```

holds each packet 75 ms each way, give or take 20, and loses 2% of them --
below the transport, so its own round trip, resends and timeouts see it as they
would see the real thing. For measuring; a shipping build refuses it. The
engine's own acceptance gate walks a predicted character this way at 0, 50, 150
and 300 ms round trip and with frames of 150 and 400 ms, and holds every
condition to no correction on a straight walk.

**A server and a player of one game on one machine** keep separate saves: a
dedicated server keeps `saves-server` beside the player's `saves`, and
`--saves=DIR` names any folder.

## Playing a match from the editor

At the top of **Run and Debug** (`Ctrl+Shift+D`), and under the **Run** menu,
are a player count, from 1 to 4, and a **Dedicated server** box. With one
player and no server, Start (`F5`) is the ordinary Play in the viewport.
With more, Play saves the scene and starts a match of separate windows (ADR
0106):

- without a dedicated server, a **Host** -- the first player -- and a
  **Client** for each other player;
- with one, a **Server** with no window and a **Client** for every player.

The windows are tiled across the screen. What each one prints appears in the
Output panel under its name (`[Host]`, `[Client 1]`...), and each writes its
own log under `.engine/match/`. **Stop** closes them all.

## Where code runs

**Where a script is decides which machine runs it** (ADR 0105). The rules go
where only the machine deciding the world runs them; the HUD and the camera go
where only a machine a player sits at runs them:

```text
src/server/       GlobalScriptService.Server   the rules, for every scene
src/client/       GlobalScriptService.Client   the camera and the controls
src/shared/       GlobalScriptService.Shared   modules both require
```

A scene has its own two as well, `ServerScriptService` and
`ClientScriptService`, from `src/scenes/<scene>/server/` and `client/`.

| Where a `Script` is | Dedicated server | Client that joined | Solo and host |
|---|---|---|---|
| `ServerScriptService`, `GlobalScriptService.Server` | runs | **absent** | runs |
| `ClientScriptService`, `GlobalScriptService.Client` | **absent** | runs | runs |
| `GlobalScriptService.Shared` | required by server code | required by client code | both |
| anywhere else (`Workspace`, a part, a stamp...) | its `RunContext`: `Server`, `Shared` | `Client`, `Shared` | every side, once |

**Solo and a host run each script once**, so a game written for a match runs
alone with no configuration branch, and **the server's code never reaches a
player**: it is not replicated, a client that joins empties its own copy, and a
dedicated export leaves it out of the player's package -- per script, so a
stamp's `Server` half is not in a player's package either. A client runs the
`Client` and `Shared` scripts of its own package under what the server sends:
see [Where my code runs](manual:concepts/scripts) for the whole rule and its
price. **Such a script arrives with its instance as it is now**: a door that
opened before the player joined arrives open, so read the state when the
script starts as well as on each change.

`NetworkService.Authority` still answers "does this machine decide the world?"
for code that runs everywhere, such as a script inside a part left `Shared`:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

if NetworkService.Authority then
    -- solo, a host or a server
end
```

`examples/15-multiplayer` is laid out this way: its rules are in `src/server/`,
the controls and camera in `src/client/`, and how a racer is driven in
`src/shared/`, which both require.

A replica builds nothing it expects the authority to send. Everything under
`Workspace` that replicates arrives from the authority: parts, models,
folders, their names, colours and motion, the `Lighting` service's time of day
and fog, decals, particle emitters, `RemoteEvent`s and `RemoteFunction`s. The
ground -- `Terrain` and the block world -- arrives with the world, from each
machine's own scene, and the authority sends what its scripts changed of it:
to a player who joins, every chunk that differs from the scene, and after that
each chunk as it changes ([Terrain in a match](manual:world/terrain)). A
digging game digs on the server, and every player sees the hole.

## Players

`NetworkService:GetPlayers()` lists everyone taking part, and `PlayerAdded` and
`PlayerRemoving` say when that changes. `NetworkService.LocalPlayer` is the
player at this machine, and a dedicated server has none. `Player.UserId` is the
same number on every machine.

**What a player did reaches the authority as intent**: the values of their input
actions, read with `player:GetIntent("Move")` exactly as `GetState` reads them
on the machine where the key was pressed. The authority decides what the key
did. A client that could say "I moved here" or "I hit" would be a client that
always wins.

## A player's own character

Set `player.Character` to the part that is them -- or, in a 2D game,
`player.Character2D` to the `Part2D` that is. It is one thing with two names,
each typed as what it holds, and setting either replaces the other. Two things
follow from it:

- **Their machine moves it at once.** A replica runs the same movement code on
  its own character from its own keys, and the authority's snapshots correct it
  rather than overwrite it. Pressing a key does not wait a round trip.
- **Their machine is sent what is near it.** A replica is sent the parts within
  `StreamingService.LoadRadius` of its character, and everything else in the
  world stays on the authority. A part a script there still holds becomes a
  husk when it leaves, parented to nil, and `StreamingService.InstanceStreamedOut`
  fires for it.

Everybody else's parts are drawn between the last two snapshots, a few ticks
behind the authority, so they glide instead of stepping.

### A character a script moves

A `CharacterBody` is predicted for you. A character your own code moves -- a
hero on a grid, a ship, anything on the 2D plane -- is predicted by **running
the same step on both sides**: one module, required by the server and by the
client, that turns an intent into a move.

```luau
--!strict
-- GlobalScriptService.Shared.Step
local Step = {}

function Step.move(hero: Part2D, direction: Vector2, dt: number)
    hero.Position += direction * 5 * dt
end

return Step
```

```luau
--!strict
-- ServerScriptService: the authority, for every player
RunService.Heartbeat:Connect(function(dt: number)
    for _, player in NetworkService:GetPlayers() do
        local hero = player.Character2D
        if hero then
            Step.move(hero, player:GetIntent("Move") :: Vector2, dt)
        end
    end
end)
```

```luau
--!strict
-- ClientScriptService: this machine, for its own hero only
RunService.Heartbeat:Connect(function(dt: number)
    local me = NetworkService.LocalPlayer
    local hero = if me then me.Character2D else nil
    if hero and not NetworkService.Authority then
        Step.move(hero, move:GetState() :: Vector2, dt)
    end
end)
```

The client's hero answers the key on the tick it was pressed. The authority's
snapshots do not overwrite it: each one says where the authority had the hero
at the intent it last applied, the engine compares that with where THIS machine
had it at that same tick, and moves the hero by the difference. While both
sides step alike the difference is nothing and nothing moves. When the
authority stops the hero against something the client walked through, the hero
comes back.

A client that does not step its own character at all still gets the newer of
the two pictures: its hero is put where the authority's newest snapshot has
it, where everyone else's is drawn a few ticks in the past.

### Movement your code adds: the predicted step

A dash, a double jump, a dodge, a jump pad -- movement a script adds to a
`CharacterBody` -- belongs **inside the simulation step**, where the
prediction can take it again (ADR 0157). Written in `Heartbeat`, it runs once
on each machine at each machine's moment: the authority applies the player's
input a few ticks after their machine did, the two dashes start at different
ticks, and the character is pulled back at the start and the end of every one.

```luau
--!strict
-- GlobalScriptService.Shared.Dash: required by the server and by the client
local RunService = game:GetService("RunService")

RunService:BindToPredictedStep("dash", function(step)
    local body = step.Character :: CharacterBody
    local ends = (body:GetAttribute("DashUntil") :: number?) or 0
    if step:Pressed("Dash") and step.Tick >= ends then
        ends = step.Tick + 60 -- a second
        body:SetAttribute("DashUntil", ends)
    end
    if step.Tick < ends then
        body:Move(body.CFrame.LookVector)
    end
end)
```

The function runs before the physics solves, once a tick for every player's
character the machine steps -- all of them on the server, its own on a client
-- and on a client **again for every tick it steps over** after the server
corrects it, with the input that tick had and `step.Replaying` true.

- **Read input from `step`**: `step:GetIntent(action)` and
  `step:Pressed(action)`, the button going down at this tick.
- **Count time in `step.Tick`**, the player's own tick: the same number on the
  server and on their machine, whatever the latency between them.
- **Keep state in attributes on `step.Character`.** An attribute the step
  writes there is predicted: remembered every tick, put back before a step is
  taken again, and the server's value sent to the client beside the character.
  A client whose own code writes one is put right.
- **Draw from `step.Random`**, seeded by the player and the tick.
- **Check `step.Replaying` before a sound or a particle**, which should happen
  once.
- It may not wait: `task.wait` in it raises.

A part can do the same when a character lands on it:

```luau
--!strict
-- A Script in a jump pad's stamp, with no side: it runs on every machine.
local pad = script.Parent :: BasePart
pad:BindToPredictedTouch(function(character, step)
    (character :: CharacterBody):Jump()
end)
```

### Input your code writes

An intent is what a player did, and what the server applies for them. Input
that is not an `InputAction` -- a turn worked out from the mouse, a gesture, a
bot -- becomes one through `RunService:BindToIntent`, which runs every tick
right after the machine's input is read:

```luau
--!strict
RunService:BindToIntent("aim", function(intent)
    intent:Set("Turn", turnFromMouse())
end)
```

Every reader of `Player:GetIntent` and `step:GetIntent` sees it, on this
machine and on the server.

## Attributes: state every machine sees

**An attribute the authority sets reaches every replica that has the
instance** (ADR 0106): on a part, a model, a `Player`, a `Team`, and on
`GlobalScriptService`. `GetAttributeChangedSignal` and `AttributeChanged` fire
on the replica when it arrives. A lobby is then a server script and attributes:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

-- On the server: whoever pressed Ready is ready.
for _, player in NetworkService:GetPlayers() do
    if player:GetIntent("Ready") == true then
        player:SetAttribute("Ready", true)
    end
end
```

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

-- On every client: the list redraws itself.
for _, player in NetworkService:GetPlayers() do
    player:GetAttributeChangedSignal("Ready"):Connect(function()
        print(player.Name, player:GetAttribute("Ready"))
    end)
end
```

- **Only the authority's writes travel.** A replica's own write is its own, and
  the next write the authority makes to that attribute replaces it.
- An attribute on something that does not replicate -- `ServerStorage`, the
  server script services -- stays where it is.
- An attribute that holds an instance arrives as the replica's copy of it, or
  nil if the replica was not sent it.

**A client that joins late has them as they stand.** Whatever the attributes
were when it arrived -- of the workspace, of a service, of a part, of a player
-- is what it reads from its first tick: a round's number set before a player
came in is there for that player.

## Messages: `RemoteEvent`

Some things a game says are not state and are not a key held down: "I bought
the sword", "the round starts", "you won". A `RemoteEvent` carries them.

Create it on the authority, under `Workspace`, and find it on a replica by
name:

```luau
local honk: RemoteEvent
if NetworkService.Authority then
    honk = Instance.new("RemoteEvent")
    honk.Name = "Honk"
    honk.Parent = workspace
else
    honk = workspace:WaitForChild("Honk") :: RemoteEvent
end
```

| Call | From | Fires |
|---|---|---|
| `honk:FireServer(...)` | any machine | `honk.ServerReceived(player, ...)` on the authority |
| `honk:FireClient(player, ...)` | the authority | `honk.ClientReceived(...)` on that player's machine |
| `honk:FireAllClients(...)` | the authority | `honk.ClientReceived(...)` everywhere a player is |

**The authority learns who sent a message from the connection**, and it arrives
first in `ServerReceived`. Nothing a message says can make it someone else's.
Solo and hosting, this machine is its own server and its own client, so every
call is delivered here as it would be across a network.

A message carries values:

- nil, booleans, numbers, strings, buffers and vectors;
- `Color3`, `CFrame`, `Vector2`, `UDim`, `UDim2` and enum items (an item of an
  enum the receiver's build does not have arrives as nil);
- instances, each arriving as the receiver's own copy, or nil where the
  receiver does not have one;
- tables of all of those, eight deep.

A function, a thread, a table that contains itself, or more than 64 KiB is
refused at the call that tried to send it. Messages are reliable and arrive in
order, at the start of the receiver's next tick. An authority takes a burst of
1024 messages and a megabyte from one player, and 64 messages and 64 KiB a
tick after that; what a client sends past it is dropped.

**A message over 64 KiB is refused, not split** (`net.err.remote_too_large`,
raised at the call). The engine does not cut one into pieces behind a script's
back, because a megabyte sent reliably holds every other reliable message of
the game behind it until the last piece is acknowledged. Send it as several
messages that each mean something -- a level a row at a time, an inventory a
page at a time -- or, when it is state that is replaced many times a second,
on an `UnreliableRemoteEvent`, below. State every machine should simply have
is an attribute or an instance, not a message.

**A call that arrives before anybody listens is kept for the first who
does.** A scene's client and server scripts start in the same tick, so a client
that fires at once fires into a remote the server may not have connected yet.
The call is not lost: it waits, with the others in the order they were sent,
and the first `Connect` to `ServerReceived` -- or `ClientReceived`, the other
way -- is handed them all. Up to 256 a remote each way; past that the calls
are dropped and the remote says so once in the log. There is nothing to do
about it in game code, and no "ready" flag to keep.

## Messages that may be lost: `UnreliableRemoteEvent`

A `RemoteEvent` always arrives, and that has a price: when the network loses
one, it is sent again, and **every reliable message behind it waits** -- the
spawns, the roster and every other event of the game. For "I bought the sword"
that is right. For where a horde stands, sent fifteen times a second and
replaced each time, it is wrong: by the time the lost one is sent again, a
newer one has made it worthless, and it held everything else up for nothing.

An `UnreliableRemoteEvent` is the same thing with the other contract:

```luau
--!strict
local positions: UnreliableRemoteEvent = workspace:WaitForChild("HordePositions") :: UnreliableRemoteEvent

-- On the server, fifteen times a second:
positions:FireAllClients(packed)

-- On a client:
positions.ClientReceived:Connect(function(packed: buffer)   -- bytes the game packed itself
    apply(packed)   -- the newest there is
end)
```

| | `RemoteEvent` | `UnreliableRemoteEvent` |
|---|---|---|
| Arrives | Always | Maybe: it is sent once |
| When one is lost | Sent again; everything reliable behind it waits | Nothing waits; the next one replaces it |
| Order | The order sent | Never older than the last one heard, across every unreliable event; nothing more |
| At most | 64 KiB | 16 KiB |
| Fired before the other side has the event, or listens | Kept, and delivered when it can be | Dropped |
| Use it for | What happened: a purchase, a round starting, a chat line | What is, right now: positions, aim, a health bar |

**The larger it is, the more often it is lost.** Past about a kilobyte a
message travels in pieces, and it is lost whole when any one piece is. On a
link losing 2% of its packets, a 1 KB message arrives 98 times in 100, a 5 KB
one about 90, and a 15 KB one about 75. So send small: only what changed, or
the world in slices that each stand alone -- a slice that is lost is one
fifteenth of a second late for a part of the horde, not for all of it.

**Never for what must arrive.** A kill, a pickup, a score: those are
`RemoteEvent`s, or attributes. `NetworkService:GetStats()` counts
`UnreliableSent`, `UnreliableReceived` and `UnreliableDropped` -- what this
machine sent, took in, and dropped itself (an event the other side did not
have yet, a flood, one older than the last). What the network lost between is
`Loss`.

Solo and on a host's own machine nothing is lost, since there is no wire to
cross: the same script runs everywhere.

## Questions: `RemoteFunction`

Some things a client needs to ASK: "how many coins do I have?", "may I open
this door?". A `RemoteFunction` carries the question to the authority and
brings the answer back, and the caller waits for it:

```luau
-- On the authority: one handler, with the player who asked first.
standing.OnServerInvoke = function(player: Player): number
    return honks[player] or 0
end

-- On any machine: yields until the answer arrives.
local count = standing:InvokeServerAsync()
```

- The answer is whatever the handler returns, carried like a message's values.
- The handler runs in a thread of its own and may wait.
- If the handler raises an error, `InvokeServerAsync` raises it at the caller.
  It also raises when the authority has no handler.
- Solo and hosting, the authority asks itself, as its own player, at the start
  of the next tick.

**Only a client asks.** The authority cannot call a client and wait: a client
that never answered would hold the server's script forever. Tell a client
something with a `RemoteEvent`.

## Keeping things: `ReplicatedStorage` and `ServerStorage`

Two services hold things that are not in the world. Nothing under them is drawn,
collides or moves, and both are saved with the scene, so what the editor put
there is there when the game starts.

| Service | Who has it |
|---|---|
| `ReplicatedStorage` | Every machine. Its contents reach every replica, whatever their distance from the player. |
| `ServerStorage` | The authority only. A replica empties it when it joins. |
| `ServerScriptService`, `GlobalScriptService.Server` | The authority only, like `ServerStorage`: code, not things. |

**A crowd the server simulates** -- a horde every player sees -- is a `Swarm`
whose agents have no body (`Swarm:AddAgentAt`), each after the nearest player
(`Swarm:SetTargets`). The server sends the positions it reads from
`GetPositions`, and each machine draws them. A body under `ServerStorage`
never replicates either, but it is a part moved every tick for nobody.

A template comes into the world by being cloned into `Workspace`:

```luau
local ServerStorage = game:GetService("ServerStorage")

local enemy = ServerStorage.Enemy:Clone()
enemy.Parent = workspace
```

`RemoteEvent`s and `RemoteFunction`s can live in `ReplicatedStorage` as well as
in `Workspace`. A replica finds them with `WaitForChild` either way.

## Sides

A side is your game's own state: an attribute on the player, set on the
authority, which every machine reads (attributes replicate):

```luau
player:SetAttribute("Team", "Red") -- on the authority
print(player:GetAttribute("Team")) -- on any machine
```

## Handing a part over: network ownership

Every loose part is simulated by the authority, so a ball a player kicks moves
a round trip after the kick. Hand the ball to that player and their machine
simulates it instead: it moves at once for them, the authority follows what
they send, and everybody else sees it through the usual snapshots.

```luau
ball:SetNetworkOwner(player) -- that player's machine simulates it
ball:SetNetworkOwner(nil)    -- the authority's again
```

Only the authority hands parts over, and only parts that are not anchored.
`GetNetworkOwner()` answers the player, or `nil` for the authority; a replica
knows only about itself, so there it answers its own player or `nil`. A player
who leaves gives back everything they owned.

**What the owner sends is trusted.** Its machine could put the part anywhere,
so hand over what a player may move -- their ball, their vehicle -- and check
anything that matters in the authority's own scripts. The authority does hold
it to a reach: a part moves at most two metres a tick on the authority, so an
owner that teleports one is followed there over a few ticks, not at once. Move
a part a long way from the authority, or hand it back first.

**A vehicle is its parts and what holds them.** Attachments, every joint, the
welds and the movers travel with the parts, so the machine that owns a car's
parts solves its hinges and drives its motors as the authority would; any other
machine is sent where the parts ended up and solves nothing. Hand over every
part of the assembly -- a joint is solved where one of its ends is simulated.
A property the owner changes on its own joint, the motor's speed for instance,
stays the owner's until the authority changes it.

## The protocol

What the machines say to each other is published, byte for byte, in
`docs/protocol/wire.md` -- generated from the same schema the engine is built
against, so it is exactly what this build speaks. A peer must speak the same
protocol version; the CHANGELOG says when a release changes it.

## Rolling back

For a game where every machine simulates -- a fighting game, a lockstep
racer -- or an authority that wants to redo a few ticks with an input that
arrived late, `RunService` saves the 3D simulation and steps it again:

```luau
local RunService = game:GetService("RunService")

local frame = 0
local states = {} -- one per tick, for the last few ticks
RunService.PostSimulation:Connect(function()
    frame += 1
    states[frame] = RunService:SaveSimulation()
    states[frame - 8] = nil
end)

-- An input for tick `past` arrived late: go back, and come forward again.
local function correct(past: number, now: number)
    RunService:RestoreSimulation(states[past])
    for t = past + 1, now do
        applyInputs(t)            -- your game's inputs for that tick
        RunService:StepSimulation()
        states[t] = RunService:SaveSimulation()
    end
end
```

The re-simulation is exact: the same inputs from the same state give the same
world, bit for bit, on every machine.

**What is saved is where things are and how they move** -- every simulated
part's `CFrame` and velocities, every character's movement state, and the
solver's own. Not which instances exist, not their other properties, not the
2D layer, and not your scripts' variables: keep your game's state (health,
scores, input history) in your own tables and restore it beside the buffer.
`RestoreSimulation` answers `false` and changes nothing if a simulated part was
created, destroyed or anchored since the save. `StepSimulation` fires no
`Touched`; those ticks already did.

## Accounts, tokens and passwords

`CryptoService` is what a login is made of ([`CryptoService`](api:CryptoService)):

```luau
--!strict
local CryptoService = game:GetService("CryptoService")

-- Registering: store the hash, never the password.
local hash = CryptoService:HashPasswordAsync(password)
-- Logging in: check against the stored hash.
if CryptoService:VerifyPasswordAsync(password, hash) then
    local session = CryptoService:UniqueId() -- a token nobody can guess
end
```

- **Chance nobody can predict** comes from `RandomBytes`, `RandomInteger` and
  `UniqueId`. `Random.new()` and `math.random` are the simulation's: the same
  numbers on every run, on purpose, and the wrong thing for a token.
- **`HashPasswordAsync` is for passwords and `Sha256` is not.** The first is
  slow on purpose and salts itself; the second is a fingerprint, and a
  fingerprint of a password is guessed at billions of tries a second.
- **Compare secrets with `SecureEquals`**, and sign what a client must not
  forge with `HmacSha256` under a key only the server holds.

**What a `RemoteEvent` carries is not hidden yet.** The connection is not
encrypted until ADR 0120 is built, so a password sent from a client can be read
by whoever carries the packets. Until then, keep a login for a network you
trust, or send it to your own backend over `https://` with `net.request`
([Talking to a backend](manual:guides/backend)), which is encrypted today.

## What is not here

- unreliable messages;
- lag compensation for hits.

`examples/15-multiplayer` is the whole of it in one file: racers driven by
intent and predicted by their own machine, a horn that is a `RemoteEvent`, and
H asking a `RemoteFunction` how often you have honked.

## Where to look next

- [Talking to a backend](manual:guides/backend) — accounts, inventories and
  anything that outlives a match
- [Input actions](manual:input/actions)
- [Streaming a large world](manual:assets/streaming)
