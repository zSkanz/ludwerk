# Scenes at run time

A game is often more than one place: a menu, a lobby, the match. Each is a
**scene**, a complete place with its own world, lighting, UI, storages and code.
The game goes from one to another with `SceneService:LoadScene`, without
restarting and, in a match, without dropping the connection (ADR 0106).

```luau
--!strict
local SceneService = game:GetService("SceneService")

SceneService:LoadScene("scenes/arena.scene.json", { Mode = "ctf" })
```

In the new scene, `GetLoadData` gives back what was passed:

```luau
--!strict
local SceneService = game:GetService("SceneService")

local data = SceneService:GetLoadData()
if typeof(data) == "table" and data.Mode == "ctf" then
    print("capture the flag")
end
```

## What changes, and what stays

**The old scene goes whole**:

- its world;
- the contents, the settings, the attributes and the tags of `Workspace` and
  of every scene-scoped service, such as `Lighting`, `UIService` and the
  storages;
- its scripts, whose threads stop.

The new scene opens from the engine's settings, so nothing the old scene set
leaks into it: each scene-scoped service goes back to its defaults, and then
to what the new scene's file says.

| Scene-scoped: back to defaults at every change | Game-scoped: untouched |
|---|---|
| `Workspace` and everything in it | `game` itself: its attributes and tags |
| `Lighting`, `UIService`, `AudioService`, `PhysicsService`, `StreamingService`, and every other service a scene's file can describe | `GlobalScriptService` and what is in it |
| `ReplicatedStorage`, `ServerStorage`, the scene's two script services | `Player`s and their attributes |
| `VoxelService`'s blocks **and its registry of block types** | a `ScreenGui` with `KeepOnSceneLoad` |
| every `RemoteEvent` and `RemoteFunction` in the storages | the connection, the saves |

**For code that is the game's** -- a module in `GlobalScriptService` that
lives across scenes -- everything in the left column is new each time. A
`ScreenGui` it made without `KeepOnSceneLoad` died with the scene; "I already
registered the blocks" is false in the next one; a remote it kept a reference
to is gone. Keep what the game owns on `game`, on a `Player`, or in the module
itself, and build the rest again on `SceneLoaded`. The engine says so when it
happens: a `ScreenGui` a script in `GlobalScriptService` put in `UIService`,
gone with the scene, is named in a warning with what to set. It reads its file, mounts its own code from
`src/scenes/<scene>/`, and starts its scripts.

**What stays** is the game's:

- **`GlobalScriptService`**, with its scripts, their variables and their
  connections. Keep there what the whole game needs: the character a player
  picked, the connection, a lobby's state. See
  [Scripts](manual:concepts/scripts#server-code-client-code).
- **A `ScreenGui` with `KeepOnSceneLoad = true`**, which goes with the game to
  the next scene's `UIService`. It is how a loading screen stays up while the
  scene changes under it.
- **`Player`s**, attributes included. Their characters were in the world and go
  with it; the new scene's server code makes new ones.

## Starting in a scene

A run starts in `[project] scene`. To work on a level without clicking through
the menu, name the scene -- and, when the menu would have handed it something,
hand it the same thing as JSON:

```bash
ludwerk dev --scene=scenes/arena.scene.json
ludwerk dev --scene=scenes/arena.scene.json --scene-data='{"round": 3, "hard": true}'
```

The path is as the project file writes one, under `content/`. The JSON is what
`SceneService:GetLoadData()` answers in that scene, exactly as if a `LoadScene`
had passed the table: an object or an array is a table, the rest are the plain
values they are. Text that is not JSON is refused before anything starts.

## When it happens

`LoadScene` returns at once. The change happens at the next safe point between
two ticks:

1. `SceneLoading(path)` fires while the old scene is still there. This is the
   moment to put a loading screen up.
2. **The old scene closes**: its `scene:BindToClose` handlers run, and the
   change waits for them — see [When a scene closes](manual:guides/scenes#when-a-scene-closes).
   The old scene goes on running meanwhile, under the loading screen.
3. What it saved is written, it is torn down, and the new one is read.
4. The new scene's scripts start.
5. `SceneLoaded(path)` fires after their first resumption.

A loading screen goes up on `SceneLoading` and comes down on `SceneLoaded`:

```luau
--!strict
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

local loading = Instance.new("ScreenGui")
loading.Name = "Loading"
loading.KeepOnSceneLoad = true
loading.Enabled = false
loading.Parent = UIService

SceneService.SceneLoading:Connect(function()
    loading.Enabled = true
end)
SceneService.SceneLoaded:Connect(function()
    loading.Enabled = false
end)
```

## The scene

`scene` is a global in every script, as `game` is: **the scene open when it is
read**. `SceneService.CurrentScene` is the same scene as an object that stays
with it — its `Name` (`arena`), its `Path` (`scenes/arena.scene.json`), and
`IsOpen()`, which turns false when it closes.

```luau
--!strict
local SceneService = game:GetService("SceneService")

print(scene.Name) --> arena
local here = SceneService.CurrentScene
-- ...later, after a LoadScene:
print(here:IsOpen()) --> false
```

Inside a `SubWorld`, `scene` is the sub-world's scene.

## When a scene closes

There are two closes, and a handler goes on the one it is about:

| Call | Runs when |
|---|---|
| `scene:BindToClose(fn)` | the scene closes: a `LoadScene` away from it, or the game closing while it is open |
| `game:BindToClose(fn)` | the game closes, whichever scene is open — and the editor's Stop is the game closing |

A level saves what belongs to it on `scene:BindToClose`. The change waits for
its handlers — each in its own thread, so it may yield on a `SaveAsync` — up to
`[scene] close_grace_seconds` in `project.toml` (5 by default); then the saves
are written and the scene goes.

```luau
--!strict
local SaveService = game:GetService("SaveService")

scene:BindToClose(function()
    local slot = SaveService:GetSlotAsync("progress")
    slot:Set("arenaBest", 42)
    slot:SaveAsync()
end)
```

**A handler belongs to the script that registered it.** When a scene closes,
what its scripts registered goes with it: a scene script's `game:BindToClose`
is dropped without running — it asked for the game's close, and its scene did
not live to see it — and the editor says so once per script. A script in
`GlobalScriptService` keeps its handlers until the game closes. What counts is
the script that called, not where the function's code lives: a module in a
scene's storage, required by a global script, registers for the global script.

When the game closes, the open scene's handlers run first, then the game's,
under one grace period; then the saves are written.

## Messages between the game and a scene

`game` and `scene` are mailboxes. **Send to the mailbox of whom you want to
reach, and listen on your own**: a scene's coin tells the game's HUD, and a
global script tells the level.

```luau
--!strict
-- In the scene: a coin picked up.
game:SendMessage("CoinCollected", 1)

-- In the scene: the level listens for what the game tells it.
scene:BindToMessage("OpenGate", function(side: string)
    print(`opening the {side} gate`)
end)
```

```luau
--!strict
-- In GlobalScriptService: the HUD, which lives across every scene.
local coins = 0
game:BindToMessage("CoinCollected", function(amount: number)
    coins += amount
end)
scene:SendMessage("OpenGate", "north")
```

- **Deferred**, in send order, each handler in its own thread.
- **A binding goes with its script's scene**, on either mailbox.
- **A message to a closed scene is dropped**, and so is one nobody listens to;
  the editor says so in both cases, and a game never raises for it.
- **The values are copied.** What an attribute or a save holds arrives equal; a
  table or a `buffer` arrives as the receiver's own copy, without its metatable;
  an instance arrives as itself. A function, a thread, a table that holds itself
  or a live object such as a `Signal` is an error at `SendMessage`, naming where
  it was found. Send the data, not the object.
- **Local to one machine.** Between machines, use a `RemoteEvent`.
- Messages are for events; shared state stays in a module in
  `GlobalScriptService.Shared`, or in attributes.

## Loading in the background

`LoadSceneAsync` prepares the next scene while this one plays — its file read
and parsed off the main thread, the meshes it names loaded — and switches when
the game says:

```luau
--!strict
local SceneService = game:GetService("SceneService")

local loading = SceneService:LoadSceneAsync("scenes/level2.scene.json", { Activate = false })
loading.Scene:SendMessage("Difficulty", "hard") -- waits until level 2 opens
while loading.Status == Enum.SceneLoadStatus.Preparing do
    print(`loading {math.floor(loading.Progress * 100)}%`) -- a bar's width
    task.wait()
end
-- ...a fade to black here...
loading:Activate() -- the switch: SceneLoading, the old scene's close, level 2
```

- `Progress` goes from 0 to 1 and only rises: half for the file, the rest for
  the meshes as they arrive. `Ready` fires when it is prepared.
- `Activate()` switches at the next safe point, exactly as `LoadScene` does,
  with the parse already done. `Activate = true` (the default) switches as soon
  as the scene is ready.
- `Cancel()` drops it, and `Finished` fires with false; so does a failure, with
  `Error` saying why. A second `LoadSceneAsync`, or a `LoadScene`, cancels the
  one in flight.
- A message sent to `loading.Scene` waits until the scene opens and is
  delivered after its scripts start, before `SceneLoaded`.

## In a match

**The scene is the authority's.** A host or a server calls `LoadScene`, and
every client loads the same scene from its own package. The new world then
arrives by replication, as it does at a join, and the connection is never
dropped.

- A client that calls `LoadScene` while connected gets an error, because two
  machines in two scenes cannot share one world. A client asks the server with
  a `RemoteEvent` instead.
- A client that joins late loads the scene the server is in.

## Limits

- **The switch itself is still one tick's work.** `LoadSceneAsync` moves the
  reading, the parsing and the meshes off it; building the new world from the
  parse and starting its scripts stay on the main thread, between two ticks.
  Textures, sounds and terrain are not warmed yet: they arrive after the
  switch, as they do today.
- **In a match, clients do not prepare ahead.** They load the scene the moment
  the authority switches, as they do for `LoadScene`.
- **A scene loaded at run time is read whole.** The streaming grid a boot scene
  is partitioned into is not rebuilt for a scene loaded afterwards, so a scene
  that needs streaming should be the one the game starts in, for now.

`examples/24-scenes` is a menu and an arena, with a loading screen that stays
across and fills a bar while the arena is prepared.
