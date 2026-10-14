# Saving the game

A game keeps what has to outlive a run -- progress, settings, a best time --
in **slots** through `SaveService`. A slot is a named file on the player's own
machine; a script never names a path.

```luau
--!strict
local SaveService = game:GetService("SaveService")

local slot = SaveService:GetSlotAsync("profile")   -- yields until it is read
local coins = slot:Get("coins") or 0
slot:Set("coins", coins + 10)
slot:Set("checkpoint", Vector3.new(12, 3, -40))
slot:Set("inventory", { "sword", { potions = 3 } })
```

That is all a game has to do: **what is set is written by itself**, at most
once a second while something changed, when the game closes, and when a phone
puts it in the background. `slot:SaveAsync()` writes now and waits, for the
moment a player presses "Save".

## Saving at a close

What is set is written when the game closes whatever you do, and when a scene
changes. For work to do at that moment -- a score to total, a position to
record -- register a handler on the close it belongs to:

- **`scene:BindToClose`** for what belongs to a level. A `LoadScene` away from
  it waits for the handler, so a `SaveAsync` in it finishes before the next
  scene opens.
- **`game:BindToClose`** for what belongs to the whole game, from a script in
  `GlobalScriptService`. One registered by a scene's script is dropped when
  that scene closes -- the editor says so -- because its scene is gone by the
  time the game closes.

The editor's Stop is the game closing, so both run when a test ends. See
[When a scene closes](manual:guides/scenes#when-a-scene-closes).

## What a slot holds

Strings, numbers, booleans, vectors, `CFrame`s, `Color3`s, `Vector2`s,
`UDim`s, `UDim2`s, `Rect`s, `ColorSequence`s, `NumberSequence`s -- and tables
of them, nested, with string keys or an array's `1..n`. A function, a thread,
an Instance, a table that holds itself or a number that is not finite is an
error at `Set`, not a loss later. Every value comes back as the type it went
in: a `Color3` is not read back as a `Vector3`.

`Get` hands back a **copy**. Changing the table it returned changes nothing
until it is `Set` again. `Update` reads, changes and writes one key in one call:

```luau
slot:Update("coins", function(old: number?): number
    return (old or 0) + 1
end)
```

`slot.Changed` fires with the key after a change, on the next resumption point.

## Where the files are

| Run | Folder |
|---|---|
| An exported game | the player's folder for the game's `[project]` company and name: `%APPDATA%/<company>/<name>/saves` on Windows, `~/.local/share/<company>/<name>/saves` on Linux, the app's storage on Android |
| The editor's Play, `ludwerk dev`, the host run on a project folder by hand | the project's `.engine/saves/` -- a test run never touches a real player's |
| A match started from the editor | `.engine/saves/<window>/`, one folder per window |

Each slot is `<name>.save`, beside `<name>.bak`, the save before it. A damaged
file is read from the backup, with a warning; when both are damaged the slot
starts empty and `slot.Recovered` is false, so the game can tell the player.

The editor's **View > Saves** shows the project's slots and what they hold,
and changes or removes them. From a terminal:

```
ludwerk saves list                 # the project's .engine/saves
ludwerk saves clear --slot=profile # one slot, and its backup
ludwerk saves list --player        # a player's folder on this machine
```

## Limits

`[save] max_slot_bytes` (4 MiB) and `max_slots` (64) in `project.toml`. A
`Set` that would take a slot past the size is refused and leaves it as it was.

## When the layout changes

Set `SaveService.Version` before the first `GetSlotAsync`, and give
`OnMigrate` the work of bringing an older slot up to it. It runs once per slot,
before the slot is handed over, and the slot is saved at the new version.

```luau
SaveService.Version = 2
SaveService.OnMigrate = function(slot: SaveSlot, fromVersion: number)
    if fromVersion < 2 then
        slot:Set("health", (slot:Get("hp") or 100) * 10)
        slot:Remove("hp")
    end
end
```

A save is not encrypted: it is on the player's own machine, and obscuring it
would stop nobody determined. A game that needs a server's word for something
keeps that on its server.
