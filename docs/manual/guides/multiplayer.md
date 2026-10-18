# Multiplayer

One project runs alone, hosts a match, serves one with no window, or joins one.
A script moves between them with `NetworkService`:

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
| `Join(address?)` | Connects to a server. With no address, `[network] server` from `project.toml`. With a relay known, a join code works here too: see [Playing over the internet](#playing-over-the-internet). |
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

**A connection that drops is dialed again, for as long as the timeout.** While
it is, `State` is `Reconnecting`: the world stands as it was, and a game shows
it and stops predicting. What happens next depends on who answers:

| Who answers | What this machine gets |
|---|---|
| the same run of the server | the match it was in, as the player it was: `Connected` fires and nothing else changes |
| a server that was restarted | a new join: the world is replaced by the new server's, the scene's client code starts fresh, and `Connected` fires. The old run's `UserId` meant nothing to the new one; the player's `PublicKey` is the same |
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

## Playing over the internet

A match hosted from somebody's home is behind their router, and nobody outside
can send it a first packet: by its address it is joined from the same network,
or through a port the host opened on the router by hand. **A relay** takes that
away. It is a small program you run on a machine the internet reaches
(`engine-relay`, in the engine's folder under `relay/`), and a host that names
it is given a **join code** to hand out:

```luau
local NetworkService = game:GetService("NetworkService")

-- The host. Or leave the option out and put the relay in project.toml:
--   [network]
--   relay = "relay.example.com:7789"
NetworkService.RelayStateChanged:Connect(function()
    if NetworkService.RelayState == Enum.RelayState.Ready then
        codeLabel.Text = NetworkService.JoinCode -- eight characters, like K7QW-M2ZP
    end
end)
NetworkService:Host(7777, { Relay = "relay.example.com:7789" })

-- A friend, anywhere, types the code into the same box an address goes in.
NetworkService:Join(typedText, { Relay = "relay.example.com:7789" })
```

- **A code is eight letters and digits** with no 0, 1, I or O to mistake. It
  is read in either case, with spaces or a dash anywhere. It is the match's
  for as long as the match is hosted, and another the next time.
- **The engine finds the path**, and the game does nothing for it: two
  machines on one network connect by their own addresses; across the internet
  each knocks on the other's router at once, which opens a path between most
  home connections; and where none opens -- some mobile carriers, some office
  networks -- the relay carries the match. `GetStats().Path` says which:
  `"lan"`, `"direct"` or `"relayed"`. Through the relay the round trip has the
  relay's distance in it, so a relay near your players matters.
- **Hosting does not wait for the relay, and does not need it.** `RelayState`
  goes `Connecting`, then `Ready` when `JoinCode` is there, or `Unreachable`
  while the relay does not answer -- and the match is joined by its address
  all the while. The relay is asked again by itself, and a relay whose name
  this machine could not find is looked up again every half minute: a host
  opened before its network was up gets its code when the network is.
- **`JoinFailed` says why** in words: the relay did not answer, no match has
  that code, the match is full, its host is keeping this machine away, or no
  path opened in time.
- `{ Direct = false }` on `Join` goes straight through the relay: for testing
  that path, or for a network you know will not open.
- `Join("relay://relay.example.com:7789/K7QWM2ZP")` says the relay and the
  code in one string, for an invitation link.
- A host whose *name* could be read as a code -- eight letters with no dot --
  is written with its port, `gamehost:7777`, which no code has.

**Running the relay.** One UDP port, 7789 unless `--port` says another, open
to the internet on the machine's firewall. It keeps no file and needs no
account:

```
engine-relay --port=7789
```

`--max-matches`, `--max-relayed` (carried players a match), `--rate` (bytes
a second each way for one carried player) and `--burst` (how many seconds of
that rate a player may be carried at once: a join begins with the whole world
sent at a stroke, and eight seconds of the rate is what lets it through)
bound what it will do, and
`--report=60` says every minute what it carries. A carried player costs the
relay what the match sends that player and nothing more; a player who
connected `direct` costs it a few small packets when joining and none after.
A relay that is restarted loses nothing a host cannot say again: codes resolve
again within ten seconds, and a carried player is carried again within four.

**Several relays, and choosing one.** A game played on more than one continent
runs a relay near each and lets each player use the nearest. A relay answers a
ping from anybody, with no match and no registration, and
`NetworkService:PingRelayAsync(relay)` asks it: it yields until the relay has
answered -- three asks a tenth of a second apart, the least of them kept -- or
for a second and a half, and returns the round trip in milliseconds with what
the relay is carrying, or `nil` where nothing answered.

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

local Regions = {
    { Name = "South America", Relay = "relay-sa.example.com" },
    { Name = "North America", Relay = "relay-na.example.com" },
    { Name = "Europe", Relay = "relay-eu.example.com" },
}

-- Every region asked at once: the slowest answer is all it costs.
local best: { Name: string, Relay: string }? = nil
local bestPing = math.huge
local waiting = #Regions
for _, region in Regions do
    task.spawn(function()
        local answer = NetworkService:PingRelayAsync(region.Relay)
        if answer and answer.Ping < bestPing then
            best, bestPing = region, answer.Ping
        end
        waiting -= 1
    end)
end
while waiting > 0 do
    task.wait()
end
if best then
    NetworkService:Host(7777, { Relay = best.Relay })
end
```

The answer also says `Matches`, `Relayed` (players it is carrying),
`BytesPerSecond` and `Uptime`, for a status page or to pass over a relay that
is full. The host and everybody who joins it use the same relay: a code is
one relay's, so an invitation says which (`relay://host:port/CODE`). Asking
needs no match and does not disturb one.

**What a relay sees.** Packets it cannot read: a match is encrypted from end
to end, and the relay passes on what neither it nor any router on the way can
open (see [A match is encrypted](#a-match-is-encrypted)).

## A match is encrypted

Every match is encrypted -- a dedicated server, a host, a test -- and the game
does nothing for it. Before anything else is said, the two machines agree on
keys nobody else has: what a `RemoteEvent` carries, a player's input and the
world itself cross the network sealed, and a packet somebody changed, played
back or made up is thrown away as if it had been lost.

**Each machine has a key, and a player is theirs.** The first time a game
takes part in a match it makes a key and keeps it beside its saves, in a
`keys` folder only that user can read. The same copy of the game is the same
key next week; another copy, or a player who deleted the folder, is another.
That is what `Player.PublicKey` is (see [Players](#players)).

**A host is known by its key too.** The first time a machine joins a server by
its address, it remembers that server's key, without a word to the player. If
a later join to the same address meets another key, the join fails and
`JoinFailed` says so: somebody may be between the two, or the server was
reinstalled and made a new key. A game can offer the player a way to trust it
again:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

-- After the player read why, and chose to trust this server again.
local function trustAgain(address: string)
    NetworkService:ForgetServerKey(address)
    NetworkService:Join(address)
end
```

A join by code remembers nothing: the code names a match, not a machine, and
the relay hands it to another host later.

**A game whose players always join one server can name its key**, so even the
first join is not taken on trust. A server prints its key when it starts:

```
INFO  This machine's match key: 3f9c…e1a4. …
```

and the game's `project.toml` says it beside the address:

```toml
[network]
server = "play.example.com:7777"
server_key = "3f9c…e1a4"
```

**A server's key is kept beside its own saves**, so it is the same server
after a restart. Moved to a new machine without that folder, or installed
afresh, it is a new key, and every player who joined it before is refused
until they trust it again or the game's `server_key` is updated. See
[Shipping](manual:guides/shipping#a-dedicated-servers-key).

**Two copies of a game on one machine are two players.** The second finds the
key held by the first and plays with a key of its own for that run -- which is
what trying a match on one computer needs.

**For development only, a match in the clear.** `--insecure` on the command
line, or `encrypt = false` under `[network]` in `project.toml`, turns it off,
to look at packets with a network tool. The log says so and the F3 overlay
shows *Encryption: Off*. Both ends must agree: an encrypted game that joins
one in the clear, or the other way round, fails with a reason that says
which. A shipping build is always encrypted, and refuses `--insecure`.

**What it costs**: 25 bytes a packet, and well under a microsecond to seal or
open one. `GetStats()` has `WireBytesSent` and `WireBytesReceived`, what the
network itself carried with that in it, beside the messages' own
`BytesSent` and `BytesReceived`.

## How the connection is doing

`NetworkService:GetStats()` is what a connection-quality mark is built from:
`Ping`, `Jitter` and `Loss` as the transport measures them -- on a thread of
its own, so a window in the background reads the same ping as one in front --
how many times this machine's own character was corrected (`Corrections`, and
`CorrectionsPerSecond`), how far in the past the others are drawn
(`InterpolationDelay`, in milliseconds: it grows with a jittery link and comes
back when the link does), the authority's queue of this player's input, the
unreliable messages this machine sent, took in and dropped, and everything it
has sent and received in bytes (`BytesSent`, `BytesReceived` -- totals since
the session began, so a rate is the difference over a second).

**On a host or a server, `Ping` is the worst client's link**, not anybody's
own: a host that reads 38 ms reads its slowest friend. **One player's** is
`Player:GetNetworkPing()`, in milliseconds -- on the server, the round trip
to that player's machine; on a client, its own, for its own players. It is 0
for the host's own player, in a single-player game, and for a player who has
left, so a scoreboard can ask every row without a check:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

for _, player in NetworkService:GetPlayers() do
    print(player.Name, math.round(player:GetNetworkPing()), "ms")
end
```

**Where the bytes go.** Six more say what those bytes were, each sent and
received together: `SnapshotBytes` (the world's state: what moves),
`AttributeBytes` (attributes and tags), `RemoteBytes` (your `RemoteEvent`s and
`RemoteFunction`s), `UnreliableBytes` (your `UnreliableRemoteEvent`s),
`InputBytes` (what a client says of its own input and of the parts it owns),
and `SwarmBytes` (a replicated `Swarm`'s positions). What is left of the total
is instances coming and going, the ground, and the handshake. Read them a
second apart on a client and you have the cost of a match to a relay, by
cause:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")
local Kinds = { "SnapshotBytes", "AttributeBytes", "RemoteBytes", "UnreliableBytes", "InputBytes", "SwarmBytes" }
local last: { [string]: number } = {}
while true do
    task.wait(1)
    local stats = NetworkService:GetStats() :: { [string]: number }
    local line = ""
    for _, kind in Kinds do
        line ..= `{kind} {math.round((stats[kind] - (last[kind] or 0)) / 1024 * 10) / 10} KB/s  `
        last[kind] = stats[kind]
    end
    print(line)
end
```

**What each thing costs**, so a number can be read: a part that moves is its
position each snapshot, 34 bytes, and 8 more when it turns; an attribute that
changes is about 12 bytes each time it does, whatever else its instance
carries -- its name crosses once a connection; a client's input is about 2 KB
a second. An attribute written every tick with a new value -- a timer, an
angle -- is sent thirty times a second: write it when it matters, or round it.

**A worse network, on purpose.** To see a game the way a player far away does:

```
run.bat --join=127.0.0.1 --net-delay=75 --net-jitter=20 --net-loss=2
```

holds each packet 75 ms each way, give or take 20, and loses 2% of them --
below the transport, so its own round trip, resends and timeouts see it as they
would see the real thing. For measuring; a shipping build refuses it. The
engine's own acceptance gate walks a predicted character this way at 0, 50, 150
and 300 ms round trip and with frames of 150 and 400 ms, and holds every
condition to no correction on a straight walk -- and to one at most in the
second the walk begins, where the link loses packets.

**Why a character was corrected.** `Corrections` counts how often the server
put this machine's predicted character somewhere else; what it disagreed about
is in the log when the game is started with `--net-log-corrections`:

```
Corrected at tick 2350, local player 1, by the authority's answer to intent 2346:
0.167219 m off (-0.005486, -0.013573, 0.166576); turned: no; a part it pushes:
0 m off; predicted attributes that differ: HeldY, Held.
```

`local player` is whose character it was, for a machine with a guest at its
screen (see [several players at one screen](manual:guides/local-players)): 1 is the
machine's first player.

**What a machine predicts besides its characters** is the loose parts
within a few meters of a character that is a `CharacterBody`: a crate it
walks into moves at once. A game whose character is a part its own script
moves gets that part predicted -- it answers its keys at once and is
corrected by the server -- and does not get the loose parts round it
predicted: there is no step of the simulation to take again for it, so a
crate it meets moves when the server says so, a round trip later.

In the overlay's Network panel, `Corrections, characters and parts` and
`Last correction, a character or a part` are the machine's, whatever was put
right; the rows named for a player are that player's character alone.

A correction in which no character was off and a loose part this machine
predicts was -- a crate somebody pushed -- is counted for the machine and
for no player, and its line says so:

```
Corrected at tick 273, no character of this machine's, by the authority's answer
to intent 264: a part it predicts was 0.249946 m off.
```

A correction now and then is the design working. A steady stream of them is
one of a few things, and the line says which:

- **Attributes named**: a predicted step decided something from state the
  server sets -- a pause, a stun, a buff that arrives as an attribute of
  something else. The client learns of it a few ticks after the server acted on
  it, and is put right once when it begins and once when it ends. That is the
  cost of a server-decided stop; make it rare, or keep it out of the step.
- **Only centimeters, now and then, while two players run side by side**:
  another player's character is drawn a little in the past, and yours meets
  it where the server is expected to have it -- its last known place carried
  on by its motion -- which is a guess, off by a little when the other turns.
  Two players pushing into each other are corrected more: what each does next
  is the other's input. Characters that need not block each other -- a team in
  a co-op game -- go in a collision group that does not collide with itself,
  and are never corrected for it:

  ```luau
  PhysicsService:RegisterCollisionGroup("Heroes")
  PhysicsService:CollisionGroupSetCollidable("Heroes", "Heroes", false)
  character.CollisionGroup = "Heroes"
  ```

  A part's `CollisionGroup` and the server's table of groups reach every
  client: registered and set on the server, they are the same on all of them.
- **Only meters, in a burst after a hitch**: a long frame on this machine. The
  time it lost the server went on simulating.
- **One tick's worth, once, as a key goes down or up**: the packets carrying
  that change of input were lost or late, and the server began a tick after
  this machine did. A bad link's cost; it does not repeat while the key is
  held.
- **A part it pushes**: a crate the character moved that the server has
  somewhere else.

**A server and a player of one game on one machine** keep separate saves: a
dedicated server keeps `saves-server` beside the player's `saves`, and
`--saves=DIR` names any folder.

## Playing a match from the editor

At the top of **Run and Debug** (`Ctrl+Shift+D`), and under the **Run** menu,
are a player count, from 1 to 4, and a **Dedicated server** box. With one
player and no server, Start (`F5`) is the ordinary Play in the viewport.
With more, Play saves the scene and starts a match of separate windows:

- without a dedicated server, a **Host** -- the first player -- and a
  **Client** for each other player;
- with one, a **Server** with no window and a **Client** for every player.

The windows are tiled across the screen. What each one prints appears in the
Console under its name (`[Host]`, `[Client 1]`...), and each writes its
own log under `.engine/match/`. **Stop** closes them all.

## Where code runs

**Where a script is decides which machine runs it**. The rules go
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
folders, their names, colors and motion, the `Lighting` service's time of day
and fog, decals, particle emitters, `RemoteEvent`s and `RemoteFunction`s. The
ground -- `Terrain` and the block world -- arrives with the world, from each
machine's own scene, and the authority sends what its scripts changed of it:
to a player who joins, every chunk that differs from the scene, and after that
each chunk as it changes ([Terrain in a match](manual:world/terrain)). A
digging game digs on the server, and every player sees the hole.

### Whose what a script makes is

**What a server-side script makes is everyone's; what any other script makes
is that machine's alone**. A part, a model, a stamp or a clone made
by a script under `ClientScriptService` or `GlobalScriptService.Client` -- or
by one every machine runs -- is never sent to another machine, with everything
under it, wherever it is parented: under `Workspace`, or under a part that
does travel. That holds on a machine that is **hosting** exactly as on one
that joined, so a ring under the hero or a number over an enemy made by
client code is on that screen and no other.

```luau
-- src/client/: this machine's own. A friend never sees it.
local ring = Instance.new("Part")
ring.Parent = workspace
print(ring.Local) --> true

-- src/server/: everyone's.
local crate = Instance.new("Part")
crate.Parent = workspace
print(crate.Local) --> false
```

- It follows **who is running**, not where the code is written: a function in
  a `src/shared/` module makes a local part when client code calls it and a
  traveling one when server code does. A task it spawns, and a handler it
  connects, are the caller's too.
- `Clone` follows who calls it: a client-side copy of something everyone has
  is that machine's own.
- For a friend to see something, the server side makes it. A client asks with
  a `RemoteEvent`, below.
- `Instance.Local` reads it.

**Writing is not making.** A host is one world: a client-side script on a host
that writes a property of something everyone has -- its color, its material,
an attribute -- has written it for everybody, where the same write on a
machine that joined stays there. What one machine alone should see is made by
the client side, or said with a property that is never sent, like
`BasePart.Fade`. Start the game with
`--net-log-client-writes` to be told, once a script and property, each such
write on a machine that is hosting.

## What a spawned thing carries

A character, a torch, a chest: something a server-side script makes -- with
`Instance.stamp`, as a `Clone` of a template in `ReplicatedStorage`, or with
`Instance.new` -- reaches every other machine **with what is under it that
travels, and without what does not**. The way it was made does not matter, and
neither does when a player joined: a late joiner is sent each thing as it
stands.

For everything that travels the rule is one: **the instance and the properties
authored on it are sent when it is made and when one of them changes. Nothing
is sent a tick, and whatever the class simulates is each machine's own** -- a
cape's swing, a trail's ribbon, the particles in the air, where a sound has got
to.

| Under the thing | On other machines | What of it travels |
|---|---|---|
| `Part`, `MeshPart` | Yes | Where it is, its size, shape, mesh and material; how it collides and with what; how it grips, bounces and weighs, and how soon it stops (`Friction`, `Restitution`, `Density`, the two dampings, `Buoyant`); what a ray and a touch are told (`CanQuery`, `CanTouch`, `ContactDetails`); and its `PivotOffset`. |
| `Model` | Yes | Its `Scale`, its `PrimaryPart` -- that machine's copy of the part -- and its `PivotOffset`. Not `StreamingMode`, which nothing reads while a game runs. |
| `Folder` | Yes | The instance. |
| `Attachment` | Yes | Its place on its part. |
| `Bone` | Yes | Its place, the joint it names and its `Transform`. Where it lands in the world is worked out on each machine, from the pose drawn there. |
| A joint, a weld, a mover | Yes | What was authored ([network ownership](#handing-a-part-over-network-ownership)). A joint that breaks under its `BreakForce` arrives disabled, and the pieces of a `BasePart:Fracture` arrive as parts held by joints ([Destruction](manual:physics/destruction)). |
| `VehicleController`, `VehicleWheel` | Yes | What was authored, what the driver asks, and what the authority's simulation answered. The chassis travels as any body does, and the driver's own machine predicts it ([Vehicles](manual:physics/vehicles)). |
| `PointLight`, `SpotLight` | Yes | Every property. |
| `ParticleEmitter` | Yes | Every property -- its picture and its three sequences among them -- and each `Emit`. The particles are each machine's own. |
| `Decal` | Yes | Every property. |
| `Highlight` | Yes | Every property. Its `Adornee` arrives as that machine's copy of what it names. |
| `Beam`, `Trail` | Yes | Every property, the sequences and the two attachments among them, and a trail's `Clear`. A trail's pieces are laid on each machine. |
| `SpringBone`, `SpringCollider` | Yes | Every property. The swing is each machine's own, at its own frame rate. |
| `Cloth` | Yes | Every property. Where each vertex hangs is each machine's own, and no vertex is ever sent ([Cloth](manual:animation/cloth)). |
| `Sound` | Yes | `Content`, `Playing`, `Looped`, `Volume`, `PlaybackSpeed`, `Category` and the two roll-off distances. Never `TimePosition`, and not its `Group`. Each machine finds `Content` in its own voice language ([Dubbing](manual:audio/dubbing)). |
| `Caption` | Yes | Every property: a line a server says is read on every machine, in each one's own text language. |
| `LipSync` | Yes | Every property. The mouth is moved on each machine, from what it plays. |
| A `MeshPart` worn on another | Yes | `PoseFrom` arrives as that machine's copy of the body it names, and each machine makes the piece's pose from its own copy of the body's ([Wearing and holding](manual:animation/wearing-and-holding)). |
| `ClickDetector`, `RemoteEvent`, `UnreliableRemoteEvent`, `RemoteFunction` | Yes | A detector's properties; a remote is the instance itself. |
| `AnimationPlayer` | Yes | Its `Graph` and its `Retargeting`, and nothing of what it is playing: no clip, time, weight or state, and no parameter a script set. Each machine steps its own graph, from the body's motion and the character's attributes -- which do arrive -- so every machine shows the same walk without a byte of it being sent. A track a script plays is that machine's own. |
| `IKControl`, `FootPlacement` | Yes | Every property. The bending is each machine's own, worked out from the pose it drew. |
| `Script`, `ModuleScript` | Its own copy | Code is never sent. Each machine puts its own copy of a client or shared script under a thing that came from the scene or from a stamp; a script a server script made any other way is the server's alone. |
| `BillboardGui`, `SurfaceGui`, and the interface under them | **No** | Make it in a client script: see below. |
| `ReverbSoundEffect` and the other sound effects, `AudioGroup` | **No** | A sound arrives without them, and plays through that machine's own groups. A client script adds an effect where one is wanted. |
| `CameraTexture`, `SubWorld` | **No** | Each machine sets up what it draws ([Views](manual:rendering/views)). |
| `NavigationAgent`, `Ragdoll` | No, and nothing is missing | An agent walks its part on the server, and the part is what arrives; a ragdoll's limbs are parts and joints, which arrive. |
| `Camera`, `ScreenGui`, input actions | No, by nature | A view, a screen and a player's keys are that machine's. |

- **A sound is played by each machine, from its own copy.** A machine that
  sees `Playing` become true starts the sound at its beginning; one that sees
  it become false stops it. So a torch's crackle or a boss's music, set
  `Playing` on the server, is heard everywhere -- by a late joiner from its
  start, not from where the others have got to. Two things follow from
  "when a property changes":
  - **A one-shot may be missed.** A footstep that starts and ends between two
    of the server's sends was never seen to play, and a `Play` on a sound
    that is already playing changes nothing another machine can see. For a
    sound that must be heard every time -- a shot, a hit, a step -- tell the
    clients with a `RemoteEvent` (or an `UnreliableRemoteEvent`, when one lost
    does not matter) and play it in a client script.
  - **`Ended` fires on each machine** when its own copy ends.
- **What does not arrive is made where it is seen.** A name over another
  player's head is a client script's, built from what does arrive -- the
  character and the player's name:

  ```luau
  --!strict
  -- src/client/: a tag over a character, on this machine alone.
  local function tag(character: Instance, name: string)
      local board = Instance.new("BillboardGui")
      board.Size = UDim2.new(0, 180, 0, 54)
      board.WorldOffset = vector.create(0, 2.2, 0)
      local label = Instance.new("TextLabel")
      label.Size = UDim2.new(1, 0, 1, 0)
      label.Text = name
      label.Parent = board
      board.Parent = character
  end
  ```

  Call it for each player's `Character` as it arrives
  ([UI in the world](manual:ui/world-space) has the rest of a tag). The same
  goes for anything else in the table's **No** rows.

- **A dev run says what will be missing.** Where a match is hosted from
  `ludwerk dev` or the editor, the server's log says once for each class:
  *"A BillboardGui under an instance the server sends is on no other machine:
  the class does not replicate. Make it in a client script, on each machine
  that should have it."* -- the first time such an instance is found under
  something that travels. It is not said of what is each machine's own by
  nature (a script, a camera, a screen, an input action) or of what a replica
  loses nothing by (`NavigationAgent`, `Ragdoll`), and never in a player's
  game.
- **Under a thing that does not travel, nothing travels**: a light under a
  `BillboardGui` goes nowhere, because there is nothing on the other machine to
  put it under.
- **A `PivotOffset` changed on its own may take a quarter of a second to
  arrive.** Every part has one and almost none ever moves, so the server does
  not look at each every tick; one that moves with anything else of its part
  goes at once.

## Players

`NetworkService:GetPlayers()` lists everyone taking part, and `PlayerAdded` and
`PlayerRemoving` say when that changes. `NetworkService.LocalPlayer` is the
player at this machine, and a dedicated server has none. `Player.UserId` is the
same number on every machine.

**`UserId` is this run of the server's; `PublicKey` is the player's.** A
`UserId` is given in the order players join, and a player who reconnects while
the server runs gets theirs back. A server that restarts numbers everybody
afresh. `Player.PublicKey` is the key the player's game proved when it joined:
the same on every connection from that copy of the game, after the server
restarts and on another server. So a server keeps what lasts -- progress, a
ban -- by `PublicKey`, or by its own account id where the game has accounts
(see [Talking to a backend](manual:guides/backend)). A client knows only its own
players' keys; a match in the clear has none.

**How many play, and who stays**. `NetworkService.MaxPlayers` is
the most players the match takes, the machine's own among them: set it on the
machine that hosts, before `Host` or at any time after, and somebody who
joins a full match hears `JoinFailed` say so. Lowering it removes nobody.
`player:Kick(reason)` does: the authority's to call, on a player who joined.
Their machine goes solo and its `Disconnected` fires with your words.

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

-- A room for four, and nobody holds the start for more than a minute.
NetworkService.MaxPlayers = 4
NetworkService:Host()
task.wait(60)
for _, player in NetworkService:GetPlayers() do
    if player ~= NetworkService.LocalPlayer and not player:GetAttribute("Ready") then
        player:Kick("The run began without you.")
    end
end
```

It is not a ban: a removed player may join again. A game that means one keeps
the list and removes them in `PlayerAdded`.

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

### What a player's progress lets them see: phases

A bridge is broken until a player repairs it; after that, for them, it
stands -- while for the player beside them it is still broken. That is a
**phase**: the same place, a different thing in it, for each player.

```luau
--!strict
brokenBridge.Phase = "bridge-broken"
fixedBridge.Phase = "bridge-fixed"

local function bridgeFor(player: Player, repaired: boolean)
    player:RemovePhase(if repaired then "bridge-broken" else "bridge-fixed")
    player:AddPhase(if repaired then "bridge-fixed" else "bridge-broken")
end
```

- **A thing with a `Phase`**, and everything under it, is sent only to the
  players in that phase -- absent from everybody else's machine, not hidden
  -- and leaves a player's machine within a snapshot of their leaving the
  phase. A thing with none is everybody's, as before.
- **A player's phases** are the server's to set (`AddPhase`, `RemovePhase`);
  their machine is told, and reads them with `HasPhase` and `GetPhases`.
  `Player:CanSee(instance)` answers the server's own question, for hits and
  interactions.
- **It collides only for those who see it.** A phased part never stops the
  characters, or the owned parts, of players outside its phase -- their
  machines do not have it, and a wall only the server had would correct them
  at every step -- and does stop those in it. Everything else collides with
  it as with any part. `PhaseCollision = false` leaves a part to the game's
  own `CollisionGroup` rules.
- A world names at most 64 phases; the 65th `AddPhase` raises.

### A budget for each player

When a great many things near a player change at once -- three hundred
players in one square -- every change cannot go out on the next snapshot
without each player's machine being sent more than its network carries. So
each player has a **budget**: 48 kilobytes (1,024 bytes) of the world's state
a second, unless the game says otherwise.

```toml
# project.toml
[network]
client_budget_kb_per_second = 48   # 0 sends every change at once
```

Within it, what matters most to that player goes first:

- **Never waiting**: their own character, what they own, everything within
  eight meters of their character -- what they can bump into next -- and
  anything they have just come close enough to see.
- **Then by priority**, which grows every snapshot a change waits: faster the
  nearer it is (full within 15 meters, a tenth at the edge of
  `StreamingService.LoadRadius`), times `BasePart.ReplicationPriority`, times
  what the game said for that player with `Player:SetReplicationPriority`.
- **And nothing for ever**: a change that has waited three quarters of a
  second is given up to half of each snapshot's budget, oldest first. While
  the budget can carry each changed part once a second, none waits longer.

```luau
--!strict
-- A boss everybody should see moving, before the scenery.
boss.ReplicationPriority = 3
-- A party sees its own members first, wherever they are in the crowd.
for _, member in party do
    if member ~= player and member.Character then
        player:SetReplicationPriority(member.Character, 4)
    end
end
-- A spectator, or a player on a weak network, gets less.
player.ReplicationBudget = 24
```

A part that waited is drawn moving between the states it was sent, further
in the past by about as long as it waits, so it glides a little behind rather
than standing and jumping. A part that does not change costs nothing,
whatever its priority. What a script sends (`RemoteEvent`) and what a player
is sent as they arrive are not counted.

On the authority, `GetStats()` says how much the budgets held back in the
last second (`Deferred`) and how much of them that second spent
(`BudgetUsed`, 0 to 1). A dedicated server's `/metrics` has
`deferred_per_second`. The crowd costs the server's uplink at most players
times their budget, a number to plan for.

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
prediction can take it again. Written in `Heartbeat`, it runs once
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
instance**: on a part, a model, a `Player`, and on
`GlobalScriptService`. `GetAttributeChangedSignal` and `AttributeChanged` fire
on the replica when it arrives. **Only what changed is sent**: an
instance with twenty attributes and one that changes costs that one, and so
does a tag put on or taken off. A lobby is then a server script and attributes:

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
(`Swarm:SetTargets`), with `Replicates` on: the engine sends the agents to
every machine, each draws them with bodies of its own, and nothing about it
is the game's to pack or send. [Navigation](manual:world/navigation) has the
whole of it under "A horde in a match".

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
it to a reach: a part moves at most two meters a tick on the authority, so an
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

## A shot checked as its shooter saw it

A player with 150 ms of latency aims at somebody running and fires. What they
aimed at is where that runner was drawn on their screen -- a snapshot that
crossed the network, drawn a few ticks late so the motion is smooth -- well in
the past. Checked against where the runner is on the server now, every such
shot misses. `Workspace:RaycastAsSeenBy` checks it against the world as the
shooter's machine was drawing it:

```luau
--!strict
local fire = Instance.new("RemoteEvent")
fire.Name = "Fire"
fire.Parent = workspace

fire.ServerReceived:Connect(function(player: Player, origin: vector, direction: vector)
    local params = RaycastParams.new({ Filter = { player.Character :: Instance } })
    local hit = workspace:RaycastAsSeenBy(player, origin, vector.normalize(direction) * 300, params)
    if hit and hit.Instance:IsDescendantOf(enemies) then
        damage(hit.Instance, 25)
    end
end)

-- A moving target that is not a player's character.
dummy.LagCompensated = true
```

- **What is rewound**: every player's `Character`, the parts under it, and
  every part with `LagCompensated = true` -- placed where the shooter's
  machine drew them, at the tick it was showing. Everything else is where it
  is now. The shooter's own character is too: their machine draws it ahead,
  not behind.
- **How far back**: each player's machine says, with its input, which tick it
  is drawing; the server keeps every tick's transforms of what can be rewound,
  and places them as that tick left them. Nothing in the simulation moves --
  the cast meets the rewound bodies where they were and the rest of the world
  as it is.
- **At most `rewind_cap_ms`** under `[network]` in `project.toml`, 200 by
  default. A shooter further behind than that is checked against the world
  200 ms ago, not what they saw, and misses a runner they aimed at: past the
  cap, a target who reached cover on the server's clock is safe. Within it, a
  target can be hit "around a corner" by up to the cap -- the trade every game
  with lag compensation makes, and the number is where it is made. 0 turns it
  off.
- `SpherecastAsSeenBy` is the same for a sphere: a thick projectile, a melee
  swing.
- **Only on the server.** On a client it raises: a client's own `Raycast` is
  for its prediction and its effects, and the hit is the server's.

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

## Administering a server

A dedicated server has no window, and its **console** is its standard input:
type a line in the terminal it runs in, or feed one from a script that starts
it. A line runs between two ticks, never during one, and its answer is printed
and written to the log with who ran it.

| Command | Does |
|---|---|
| `help [command]` | Every command, the game's too, or one in full. |
| `status` | Uptime, tick time over the last minute, players, memory, the scene. |
| `players` | Each player: name, `UserId`, the start of their key, address, ping, time on. |
| `kick <player> [reason]` | Removes a player, as `Player:Kick` does. They may come back. |
| `ban <player\|key> [minutes] [reason]` | Removes them and refuses their key again, across restarts. No minutes is for good. |
| `unban <key>` | Lets a key in again. |
| `mute <player\|key> [minutes] [reason]` | Mutes a player, across restarts, in every world of the game. No minutes is for good. |
| `unmute <player\|key>` | Lets them speak again. |
| `suspects` | The ten players with the most suspicions in the last hour. |
| `suspect <player\|key>` | One player's suspicions in the last hour, line by line. |
| `say <text>` | A line every player hears: `NetworkService.Announced` fires with it. |
| `save` | Writes every save that is waiting, now. |
| `stop [minutes]` | Tells every player, and each minute after; then saves, removes everybody with a reason, and ends the process cleanly. A second `stop` cancels the countdown. |

`<player>` is a name, a `UserId` or the start of a key; one that matches two
players is refused with both.

**A ban is of a key**, kept in `bans.json` beside the server's saves. It is
worth what a key is worth: a player who deletes their game's key folder comes
back as somebody new. A ban that holds is a ban of an account, which needs a
game that knows its players' accounts. There is no ban by address: a
household, a school and a mobile carrier share one.

**A game adds its own commands** with `ConsoleService`, on the server:

```luau
--!strict
local ConsoleService = game:GetService("ConsoleService")

ConsoleService:Register("give", {
    Description = "Gives a player an item.",
    Parameters = {
        { Name = "player", Type = "Player" },
        { Name = "item", Type = "string" },
        { Name = "count", Type = "number", Optional = true },
    },
    Permission = 2,
}, function(caller, player: Player, item: string, count: number?)
    giveItem(player, item, count or 1)
    return `gave {count or 1} {item} to {player.Name}`
end)
```

Typed `give Player3 sword 2`, the function gets the `Player`, `"sword"` and
`2`. A line that does not fit is answered with the command's usage, and the
function is not called. What it returns is printed; an error it raises is
printed the same way and does not stop the server; it may yield, and the
console takes the next line meanwhile. `ConsoleService:ExecuteAsync(line)` runs
a line from a script, as the console would, and returns its answer.

**Players see what the operator says** where the game shows it:
`NetworkService.Announced` fires on every machine. A game that has nothing of
its own mounts the engine's banner in one line, which shows each one across the
top of the screen for a few seconds:

```luau
require("@engine/announcements").mount()
```

The same console can be reached from another machine, by key: see
[Online games](manual:guides/online-games).

### Limits, suspicions and mutes

**Every remote is limited on the server.** Each player may call one
`RemoteEvent`, `UnreliableRemoteEvent` or `RemoteFunction` 240 times a second,
480 at once, with at most 16 KiB in one call; calls past it are dropped before
any script sees them. That is four times what a game sending once a frame on a
240 Hz display needs. A channel of your own that needs more says so:
`remote:SetRateLimit(600)`, or `remote:SetRateLimit(0)` for no limit.

**What your game knows, it says once**, and the server counts each player's:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

NetworkService:DefineAction("Fire", { PerSecond = 5, Burst = 2 })

fire.ServerReceived:Connect(function(player: Player, target: Vector3)
    if not NetworkService:CheckAction(player, "Fire") then
        return -- over the rate: dropped, and counted against the player
    end
    -- the shot
end)
```

**Each of these is a suspicion**, kept in one log with who, what and when: a
remote past its limits, an action past its rate, a part its owner put further
than it could have moved, a pass or a login ticket only a forger sends, and
what your game finds itself --
`NetworkService:ReportSuspicion(player, "impossible_score", tostring(score))`.
The console's `suspects` and `suspect` read it, and `/metrics` counts it by
kind under `suspicion_total`. Nothing is done to a player because of it: a
ban is the operator's, once the log shows what honest players do. The one
exception is `kick_over_limit` under `[network]` in `project.toml`, nought by
default: a player whose calls go past the remotes' limits that many times in a
second is removed with a reason.

**A mute is the console's**, kept in `mutes.json` beside the bans and read by
every world of the game. `NetworkService:IsMuted(player)` answers it on the
server; what a mute stops -- chat, voice, messages to other players -- is your
game's to decide.

## Who may join, and accounts

A key is a copy of a game, not a person. A game with accounts -- a login on
its own backend -- tells the server which account a joiner is with a
**ticket**, and the server checks it **with no call out**:

```luau
--!strict
-- A server script.
local NetworkService = game:GetService("NetworkService")

NetworkService.OnJoinRequest = function(joiner)
    local claims = NetworkService:VerifyTicket(joiner.Ticket)
    if claims == nil then
        return false, "Please log in again."
    end
    joiner.AccountId = claims.sub
    return true
end
```

```luau
--!strict
-- The player's game, after its own login screen got a ticket from the backend.
NetworkService:Join("play.example.com:7777", { Ticket = ticket })
```

- **`OnJoinRequest` runs before the joiner is a player**: no `Player`, no
  `PlayerAdded`, nothing sent to them but the wait. It may yield to ask the
  backend; after 10 seconds it is a refusal, and so is an error in it.
  Refused, the player hears `JoinFailed` with your words. With no
  `OnJoinRequest`, everybody is let in.
- **A ticket is a standard JWT** signed with Ed25519 (`EdDSA`), which every
  backend's JWT library issues: `sub` the account, `aud` your `[project] id`,
  `iat` and `exp` at most 90 seconds apart, and a `jti` that is new each time.
  `VerifyTicket` checks it against `ticket_public_key` under `[network]`,
  refuses one whose algorithm is anything else (`none` included), one that has
  expired or was used before, and returns its claims. Why one was refused is
  in the server's log, never told to the player.
- **One session an account**: the same account joining again drops the one
  already in, with "This account joined again from somewhere else".
  `Player.AccountId` is the account on the server.
- **A ban that holds is a ban of an account**: banning a player who has an
  account bans `account:<id>`, which a new key does not get round.

A backend issues a ticket with the library it already has:

```js
// Node, with jose
import { SignJWT, importPKCS8 } from "jose";
const key = await importPKCS8(process.env.TICKET_PRIVATE_KEY, "EdDSA");
const ticket = await new SignJWT({})
  .setProtectedHeader({ alg: "EdDSA", typ: "JWT" })
  .setSubject(account.id).setAudience("com.example.realms")
  .setIssuedAt().setExpirationTime("90s").setJti(crypto.randomUUID())
  .sign(key);
```

```python
# Python, with PyJWT and cryptography
import jwt, time, uuid
now = int(time.time())
ticket = jwt.encode(
    {"sub": account_id, "aud": "com.example.realms", "iat": now,
     "exp": now + 90, "jti": str(uuid.uuid4())},
    private_key, algorithm="EdDSA")
```

`ludwerk ticket keygen --out=ticket.key` makes the key pair: the private key
in a file both libraries read, and the public key printed for `project.toml`.
`ludwerk ticket sign --key=ticket.key --sub=<account>` prints a ticket, for a
test or a backend that cannot sign one itself.

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

**What a `RemoteEvent` carries is hidden**: a match is encrypted (see [A
match is encrypted](#a-match-is-encrypted)), so a password sent from a client
reaches the server and nobody on the way. The server it reaches is the one this
machine met before at that address, or the one the game names with
`server_key`.

## What is not here

- lag compensation for hits.

`examples/15-multiplayer` is the whole of it in three files: racers driven by
intent and predicted by their own machine, a horn that is a `RemoteEvent`, and
H asking a `RemoteFunction` how often you have honked.

## Where to look next

- [Online games](manual:guides/online-games) — several worlds, travel between
  them, instances, and administering a server from another machine
- [Talking to a backend](manual:guides/backend) — accounts, inventories and
  anything that outlives a match
- [Input actions](manual:input/actions)
- [Streaming a large world](manual:assets/streaming)
