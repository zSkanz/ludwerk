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
- the contents and settings of every service, such as `Lighting`, `UIService`
  and the storages;
- its scripts, whose threads stop.

The new scene opens from the engine's settings, so nothing the old scene set
leaks into it. It reads its file, mounts its own code from
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

## When it happens

`LoadScene` returns at once. The change happens at the next safe point between
two ticks:

1. `SceneLoading(path)` fires while the old scene is still there. This is the
   moment to put a loading screen up.
2. The old scene is torn down and the new one is read.
3. The new scene's scripts start.
4. `SceneLoaded(path)` fires after their first resumption.

`SceneService.CurrentScene` says which scene is loaded now.

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

- **Loading is not free.** `SceneLoading` gives a game a frame to put a loading
  screen up. Loading the next scene in the background while the old one plays
  is later, not now.
- **A scene loaded at run time is read whole.** The streaming grid a boot scene
  is partitioned into is not rebuilt for a scene loaded afterwards, so a scene
  that needs streaming should be the one the game starts in, for now.

`examples/24-scenes` is a menu and an arena, with a loading screen that stays
across.
