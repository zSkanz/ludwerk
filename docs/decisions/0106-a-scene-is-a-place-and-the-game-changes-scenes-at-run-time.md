# 0106 — A scene is a place, and a game changes scenes at run time

- Status: accepted; both amendments below are built (2026-09-28).
- Date: 2026-09-26
- Amended by: [0124](0124-a-scene-closes-as-a-game-does-and-a-handler-dies-with-its-script.md)
  (a scene closes as a game does: `scene:BindToClose`, the global `scene`,
  `SceneService.CurrentScene` as an object, and a close handler dies with its
  script)
- Amended by: [0125](0125-a-scene-is-prepared-in-the-background-and-activated-when-the-game-says.md)
  (`LoadSceneAsync`: a scene prepared in the background, with progress, and
  activated when the game says)
- Decided by: the owner, on 2026-09-26, in conversation. He asked whether a game
  could have a menu that is not multiplayer, a lobby, and the match itself as
  separate scenes. It could not: a project opens one scene and keeps it, and the
  network role is fixed on the command line. He chose scenes that are complete,
  like places -- every service part of the scene -- with code that outlives them
  in one global service ([0105](0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md)),
  a network a script can join and leave, and attributes that replicate so that a
  lobby is not a pile of messages.
- Amends: [0080](0080-replicated-storage-and-server-storage.md) (a scene carries every
  service's settings, not only what is under them).
- Relates to: [0105](0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md)
  (the three script services and what each package carries),
  [0069](0069-replication-reads-state-and-diffs-it.md) and the multiplayer guide.

## Context

What exists on 2026-09-26:

- **One scene per run.** `[project] scene` names the scene a game opens with,
  and nothing loads another. The editor opens other scenes, but a game cannot.
- **The network role is fixed at launch**: solo, `--host`, `--serve` or
  `--join=address`. `NetworkService` answers questions (`Authority`,
  `Topology`, `LocalPlayer`, `GetPlayers`, `PlayerAdded`, `PlayerRemoving`) and
  has nothing to call: a button in a menu cannot join a match.
- **Attributes do not replicate.** The protocol carries a fixed set of classes
  and properties (`docs/protocol/wire.md`); an attribute set on the authority
  stays there. Telling every client that somebody is ready takes a
  `RemoteEvent` and a table the game keeps in step by hand.
- **A multiplayer test is terminals**: a host and a client started by hand with
  flags.
- **A service's settings were lost when nothing was under it.** A scene wrote a
  service only when something was inside it, so `Lighting.ClockTime` set on an
  empty `Lighting` was gone at the next save; and opening another scene in the
  editor emptied the services but kept their settings, so a scene that said
  nothing about one opened with the previous scene's. Fixed with this ADR (D203).

The owner's picture of a game: a **menu** (no network), then a **lobby** on a
server, then the **match**, all players moving together; if the server drops,
back to the menu.

The engines surveyed agree on the shape and differ on the words. One keeps a
game's services for ever and treats a separate place as a separate server,
teleporting players between them with a table of data. One swaps the current
scene under a root that holds project-registered singletons. One swaps levels
under a game-instance object that outlives them, and moves connected clients
with the server ("seamless travel"). The owner chose the first one's model of a
scene -- a complete place, services and all -- with the second and third's
object that outlives the scene, which ADR 0105 made `GlobalScriptService`.

## Decision

### 1. A scene is a complete place

- **Every service except `GlobalScriptService` belongs to the scene**: the
  world, `Lighting`, `UIService` and its screens, `ReplicatedStorage`,
  `ServerStorage`, `ServerScriptService`, `ClientScriptService`, and the
  settings of every other service (`PhysicsService`, `StreamingService`,
  `AudioService`...). The menu has its own lighting and its own UI; the arena
  has its own.
- **A scene saves every setting somebody changed**, on a service with nothing
  under it too, and **a scene opens from the engine's settings**: nothing is
  inherited from the scene before it. A setting that is the running game's state
  and not the scene's -- the overlay being open, the pointer being locked -- is
  `Transient` in the IDL and never saved (D203, fixed with this ADR).
- **A scene with nothing changed is the byte-for-byte file it was.**

### 2. `SceneService` changes the scene at run time

```luau
local SceneService = game:GetService("SceneService")

SceneService:LoadScene("scenes/arena.scene.json", { Mode = "ctf" })
SceneService.SceneLoaded:Connect(function(path: string) ... end)
local data = SceneService:GetLoadData()   -- { Mode = "ctf" } in the new scene
```

| Member | What it is |
|---|---|
| `CurrentScene: string` (read-only) | the path of the scene loaded now |
| `LoadScene(path, data?)` | unloads this scene and loads that one; returns at once, the change happens at the next safe point between ticks |
| `GetLoadData(): any` | the `data` the last `LoadScene` passed, or `nil`; plain values only (the same encoding a `RemoteEvent` argument takes) |
| `SceneLoading: (path)` | fires before the old scene is torn down |
| `SceneLoaded: (path)` | fires once the new scene is in place and its scripts have started |

- **The old scene goes whole**: its world, its services' contents and settings,
  its scripts (their threads stop). **`GlobalScriptService` stays**, with its
  scripts, their variables and their connections -- that is what it is for.
- **A `ScreenGui` with `KeepOnSceneLoad = true` goes with it** to the new
  scene's `UIService`: a loading screen drawn before `LoadScene` stays on the
  screen while the scene changes under it, and a script removes it on
  `SceneLoaded`.
- **In a match the scene is the authority's.** A host or a server calls
  `LoadScene`, and every client loads the same scene, from its own package,
  without dropping the connection; the new world then arrives by replication as
  it does at a join. A client that calls `LoadScene` while connected gets a
  keyed error: the scene is the server's. A client that joins late loads the
  scene the server is in.
- **`Player`s outlive the change**: the connection is the same, so the player
  is the same, attributes included. Their characters were in the world and go
  with it; the server's scripts make new ones.
- **Loading is not free**: `SceneLoading` gives a game the frame to put a
  loading screen up, and a large scene streams in as it already does. Loading
  the next scene in the background while the old one plays is later, not now.

### 3. `NetworkService` joins, hosts and leaves at run time

| Member | What it is |
|---|---|
| `Join(address?)` | connects to a server; with no address, `[network] server` from `luaug.toml` |
| `Host(port?)` | this machine becomes the authority others join (7777 by default) |
| `Disconnect()` | leaves, or stops hosting |
| `State: Enum.NetworkState` (read-only) | `Offline`, `Connecting`, `Connected`, `Hosting`, `Serving` |
| `Connected: ()` | the join succeeded |
| `JoinFailed: (reason: string)` | it did not; the reason is a keyed, readable message |
| `Disconnected: (reason: string)` | the connection ended: the server left, the network dropped, or `Disconnect` was called |

- **Joining replaces this machine's scene with the server's.** The menu is
  unloaded, the server's current scene is loaded (as in §2) and its world
  arrives by replication. `GlobalScriptService.Client` keeps running and sees
  `Connected`; `GlobalScriptService.Server`, which ran while this machine was
  solo, stops, because this machine is no longer the authority (ADR 0105 §2).
- **Leaving goes back to solo** with the scene it is in, and the game's code
  decides what next -- usually `LoadScene` of its menu. Server code starts
  again, fresh, since this machine is the authority again.
- **`Authority` can change during a run** now, from true to false at a join and
  back at a leave. It changes only at the safe point where the scene changes,
  never in the middle of a tick, and `State` says why.
- **`--host`, `--serve` and `--join=address` stay**, as the same calls made
  before the first tick. `--serve` is the only way to be a dedicated server:
  `Serving` is not something a script can switch into or out of.
- **A dedicated server** cannot `Join`, `Host` or `Disconnect`; each is a keyed
  error there.

### 4. Attributes replicate

- **An attribute set on the authority reaches every replica that has the
  instance**: on a part, a model, a `Player`, a `Team`, `GlobalScriptService`
  and what is under its `Shared`, anything the protocol already sends.
  `GetAttributeChangedSignal` and `AttributeChanged` fire on the replica when it
  arrives.
- **A replica's own write is its own** until the authority writes the same
  attribute again, as with every replicated property.
- An attribute on something that does not replicate (`ServerStorage`, the
  server services) does not.
- This is a protocol change: a new message or field, the protocol version bumped,
  `docs/protocol/wire.md` regenerated, and the CHANGELOG saying so.

With it, a lobby is a server script and attributes:

```luau
-- ServerScriptService in the lobby scene
player:SetAttribute("Ready", true)          -- every client's list redraws itself

-- ClientScriptService in the lobby scene
player:GetAttributeChangedSignal("Ready"):Connect(redraw)
```

### 5. The editor plays a match

- **Play with players**: beside Play, a count (1 to 4) and a *Dedicated server*
  toggle. It starts a server (or a host) and that many clients of the project as
  separate processes, tiled on the screen, each one's log in the Output panel
  under its own name, and Stop ends them all.
- Plain Play stays solo in the editor's viewport, as today.

### 6. What a game looks like with all of it

```
scenes/menu.scene.json      ClientScriptService: the buttons; "Play" calls NetworkService:Join()
scenes/lobby.scene.json     ServerScriptService: who is ready (attributes); when all are,
                            SceneService:LoadScene("scenes/arena.scene.json") and everybody goes
scenes/arena.scene.json     ServerScriptService: the rules; ClientScriptService: the HUD
src/client/                 GlobalScriptService.Client: on Disconnected, LoadScene the menu
src/server/                 GlobalScriptService.Server: the match's state across lobby and arena
src/shared/                 GlobalScriptService.Shared: modules both sides require
```

And a game with one scene -- every example today -- is the same game: nothing
here asks it to change.

## Consequences

- **A menu, a lobby and a match are three scenes**, and a player moves through
  them without restarting the game or dropping the connection.
- **`Authority` is no longer constant for a run.** Code that read it once at the
  top of a script and branched for ever is still right inside one scene -- a
  scene's scripts start after the change -- but global code that cached it must
  listen to `State`. The documentation says so.
- **The protocol version moves** once, for attributes and for the scene change
  message.
- **The editor's scene switching and the game's are the same code**: opening a
  scene in the editor and `LoadScene` both clear to the engine's settings and
  read the file.
- **Later, not now**: loading a scene in the background, moving players between
  separate servers (a lobby server and match servers, with matchmaking),
  finding a game on the local network, unreliable messages and lag
  compensation. Each is its own decision when a game needs it.

### Rejected

- **Services that belong to the game and a scene that is only the world.** It
  was proposed first; the owner wants a scene to be a complete place, as in the
  engine he knows, and one global service for what outlives it.
- **Changing scene by relaunching the executable** with other flags. It drops
  the connection, the window and everything in memory.
- **A client that changes its own scene in a match.** Two machines in two
  scenes cannot share one world.

## Amended, 2026-10-02 (the netcode audit)

- **A join changes nothing until the server takes it.** The world becomes the
  server's at the welcome; until then the scene and its menu run on, and a join
  that fails -- nobody answered, the server is full, another version -- leaves
  them as they were and says why in `JoinFailed`. `Host` that cannot open its
  port says so in `HostFailed`.
- **Leaving returns to the scene the join was made from**, solo, the player at
  this machine player 1 again; a client never runs the server's scene alone.
- **`Reconnecting`** is the state while a dropped connection is dialled again.
