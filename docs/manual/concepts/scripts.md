# Scripts, modules and requires

A script is an **instance**. Its code is a property, `Source`, and it lives
wherever it is put in the tree, the way a part does (ADRs 0050, 0092). There are
two kinds:

- **`Script` runs.** An enabled `Script` in the world runs on its own
  coroutine, from the moment it is there until it leaves (see
  [Starting and stopping](#starting-and-stopping)). **Where it is decides on
  which machine it runs**: see [Server code, client code](#server-code-client-code)
  below.
- **`ModuleScript` is required.** It never starts by itself. `require(module)`
  evaluates it once, and every later require of the same instance gives back the
  same value.

Both are created the way every other class is: from the editor's insert menu,
or with `Instance.new("Script")` and `Instance.new("ModuleScript")`. A script
made in the editor is saved in the scene with its `Source`, like every other
property.

## Where a script can live

Anywhere. A script inside a part, a model, a folder or a service is saved with
that thing and copied with it. `Clone` copies its `Source`, and a stamp carries
the scripts inside it.

**A script reads `Source` and never writes it** (ADR 0137): the editor writes
it. A script that could write another's code and enable it would run text the
sandbox never loaded. So a door with its own logic, made at run time, comes from
a copy -- a stamp authored in the editor, or a model already in the scene:

```luau
--!strict
-- `content/stamps/door.stamp.json`: a part with a `Script` inside it.
local door = Instance.stamp("door")
door.Parent = workspace
```

**`script` is the instance that is running.** `script.Parent` is what it was put
in, which is the idiomatic way for a script to find the thing it drives. A
`ModuleScript` is reached the same way, by the tree:

```luau
--!strict
local holder = script.Parent :: Instance
local Inventory = require(holder:WaitForChild("Inventory") :: ModuleScript)
```

In a project, the scene's tree is typed from the scene itself
([ADR 0078](manual:why/reaching-children)), so `require(script.Parent.Inventory)`
type-checks there as well.

## Server code, client code

A game that is played by more than one machine has code that only the machine
deciding the world should run -- the rules -- and code that only a machine a
player sits at should run -- the HUD, the camera, the buttons. **Where a script
is decides which** (ADR 0105), and there are three services for it:

| Service | Belongs to | Its code is |
|---|---|---|
| `ServerScriptService` | the scene | that scene's server code: the arena's rules, the lobby's countdown |
| `ClientScriptService` | the scene | that scene's player code: the arena's HUD, the menu's buttons |
| `GlobalScriptService` | the whole game | code for every scene, in three fixed folders: `Server`, `Client` and `Shared` |

`GlobalScriptService` is never unloaded: a script in it keeps its variables and
connections for as long as the game runs. Its three folders are made by the
engine and cannot be renamed, moved or destroyed. `Server` and `Client` follow
the same rules as the two scene services, and `Shared` holds the `ModuleScript`s
both sides require.

### Where my code runs

**Inside a script service, the service decides. Anywhere else, the script's
own `RunContext` does** (ADR 0138): `Server`, `Client`, or `Shared` for both,
which is what a new script is.

| Where a `Script` is | Its side | Dedicated server | Client that joined | Solo and host |
|---|---|---|---|---|
| `ServerScriptService`, `GlobalScriptService.Server` | `Server` | runs | **absent** | runs |
| `ClientScriptService`, `GlobalScriptService.Client` | `Client` | **absent** | runs | runs |
| anywhere else (`Workspace`, a part, a stamp, a screen, `GlobalScriptService.Shared`) | its `RunContext` | `Server` and `Shared` | `Client` and `Shared` | every side, **once** |
| `ServerStorage`, `ReplicatedStorage` | never runs | -- | -- | -- |

- **Solo and a host run each script once.** They decide the world and a player
  sits at them, and they hold one world: a `Shared` script that ran twice there
  would do everything twice. A single-player game needs nothing changed.
- **A service writes the side it decides.** Move a script into
  `ServerScriptService` and its `RunContext` becomes `Server`; take it back out
  and it stays `Server`, so server code never becomes `Shared` by accident. In
  the Properties panel `RunContext` is greyed inside a service, and says which
  one decides.
- **The Explorer marks where a script outside the services runs**: a server
  rack, a screen, or a disc split in two for both. The *Insert* menu has a
  server script, a client script and a script for both.
- **A script writes neither its own side nor another's.** The side was fixed
  when the package was made.
- **In the editor, Play is solo**, so every script runs, once.

**A stamp can carry both halves.** A door stamp holds a `Server` script -- does
it open, does the player hold the key -- and a `Client` script -- the creak,
the "press E" -- side by side. Each package carries only its own half's code:
the dedicated server has no sound code and a player has no rules (the instance
stays in both, with no code in it).

**A client runs its own copy.** Scripts never cross the network; a server that
could send code could run code on every player. So a client that joined runs
the `Client` and `Shared` scripts of **its own package**, put back under the
instances the server sends:

- a script in the scene, under the part it is authored in;
- a stamp placed with `Instance.stamp` while the game runs, and any clone of it:
  the client reads the same stamp from its own package.

**The price**: anything else a script makes at run time -- a clone of a plain
model, a script made with `Instance.new` -- never reaches a client's code. Code
a player needs has to be in the player's package: a script service, a stamp, or
the scene.

**A module is shared solo and not in a match.** Solo, the server's scripts and
the client's require the same `ModuleScript` and get the same table; on a
dedicated server and a client they are two machines and two tables. A value one
side stores in a module is not the other side's.

A client script that reaches for something under a server service finds it
while you test solo and does not find it on a client that joined. Keep what
both sides need in `ReplicatedStorage` or `GlobalScriptService.Shared`. In a
project with multiplayer, the script editor and `ludwerk check` warn about this,
about a server script asking for a camera, the interface or input, and about a
`Shared` script that never asks `NetworkService.Authority` which side it is on.

## Scripts from files

A project written in an outside editor, kept in git and hot-reloaded by
`ludwerk dev`, keeps its code in files. At boot every `.luau` file under five
folders becomes a script in the service its code runs from, and each
subdirectory becomes a `Folder`:

| Folder | Becomes |
|---|---|
| `src/client/` | `Script`s under `GlobalScriptService.Client` |
| `src/server/` | `Script`s under `GlobalScriptService.Server` |
| `src/shared/` | `ModuleScript`s under `GlobalScriptService.Shared` |
| `src/scenes/<scene>/client/` | `Script`s under that scene's `ClientScriptService` |
| `src/scenes/<scene>/server/` | `Script`s under that scene's `ServerScriptService` |

`<scene>` is the scene file's name without `.scene.json`: `arena` for
`scenes/arena.scene.json`. So `src/client/ui/health.luau` is a `Script` named
`health` inside a `Folder` named `ui`, under `GlobalScriptService.Client`.

A module in `src/shared/` is one module however it is reached:
`require("../shared/Racing")` from a file and `require(Shared.Racing)` from the
tree evaluate it once.

**The file is that script's source, and the scene does not duplicate it.** A
script mounted from a file is marked as mounted, and the scene does not write
it. What the file cannot hold rides with the scene instead: a script turned off
(`Enabled`), its attributes and its tags. Anything you put **inside** a mounted
script in the editor is authored work, and it is saved and kept. If the file
disappears later, those children are kept in a `Folder` of the same name rather
than lost.

**Under the three services, a script is a file, whatever put it there.** Made,
pasted, duplicated or dragged in from another service, renamed, or moved into
another folder: when you save, `src/` is made to match the tree.

- A script that moved has its file moved; one renamed, its file renamed; a
  folder renamed, every file in it.
- A script deleted, or moved out of the three services -- into `Workspace`,
  say -- gives its file up, and a script moved out is saved in the scene.
- **Nothing is deleted.** A file a script gives up is moved to
  `.engine/trash/<date>-<time>/` with its path, where you can get it back.
- **Nothing is written over.** If a file is already where a script wants to go,
  and it is not that script's, the script takes the next free name (`Tool2`)
  and is renamed in the tree to match.
- A name a file system refuses (`What?Now`) is made one it takes (`What_Now`),
  in the tree too.
- A `ModuleScript` outside `src/shared/` is written as `Name.module.luau`, and a
  `Script` inside it as `Name.script.luau`, so each opens again as what it was.
  `require("./Name")` finds either.
- **Until you save, the disk does not change**, so undo is always safe, and
  closing without saving leaves `src/` as it was.
- **Save As** gives the new scene copies of the scene's own scripts, in
  `src/scenes/<new>/`; the old scene keeps its own.

A script you make in the editor inside one of the three services opens in a
tab: code is in `src/`, where a diff and a text editor find it. What you put under `GlobalScriptService` that is
not code -- a `Folder` of values, an attribute -- is saved in
`content/global.json`, because no scene owns it.

`src/scripts/`, the folder before these, is read as `src/client/` for one
release, with a warning that says to move it.

## Starting and stopping

**A script runs exactly while it is live** (ADR 0137). Live is all of:

- `Enabled`, with code in `Source`;
- in the world -- a descendant of the game;
- under neither `ServerStorage` nor `ReplicatedStorage`, which hold templates
  and modules. A template must not run itself, so a `Script` there never runs,
  and a scene that has one says so in the log when it loads;
- its side running on this machine (see
  [Server code, client code](#server-code-client-code)).

It starts when it becomes live and stops when it stops being live, whatever
made it change. A clone parented into the world starts, and so does a stamp
placed with `Instance.stamp` and a model carrying scripts moved out of storage.
A script moved out of the world, into storage, or destroyed with what holds it
stops. So does one on a side a Join or a Disconnect took off this machine.
Moving a running script from one place in the world to another is not a stop:
it keeps its run. Coming back after a stop is a fresh start, and the old run's
handlers never come back beside the new one.

A stop takes everything the run started with it: its threads, its render steps
and its close handlers stop being called, and its connections are
disconnected -- `Connected` reads false -- once the frame's handlers have run.
What it built stays where it is. (`Enabled = false` is not a stop: see below.)

Each script starts on **its own coroutine**, deferred, in the tree's document
order. `DataModel.Loaded` fires once every script has had its first resumption,
so a `game.Loaded:Connect` written at file scope does run, and it observes a
fully booted world.

An error in one script's coroutine kills **only that coroutine**. The traceback
goes to the console and to `DebugService.MessageOut`, and every other script
carries on.

**`Enabled` decides whether a script's threads are resumed, and nothing else**
(ADR 0059):

- **false to true, while the game runs, starts that script** on a new coroutine,
  and its file scope runs against the world as it is now. Enabling a script that
  already ran runs its file scope again: a re-enable is a start, not a resume;
- **true to false stops resumption.** A thread already running continues to its
  next yield. Nothing it connected is disconnected, and nothing it built is
  undone. Its queued `task.defer`, `task.delay` and signal entries are dropped
  when they come up;
- both take effect at the next deferred drain, in document order, so a replay
  reproduces them. So does a start or a stop that a move caused;
- **while the editor is stopped**, `Enabled` is a scene edit. It decides what
  the next play starts.

Attributes are the idiomatic way to give one script a knob:

```luau
--!strict
local speed = script:GetAttribute("Speed") :: number? or 12
```

## Requiring a module

`require` takes either **a `ModuleScript`** or **a path to a file**.

```luau
--!strict
local Greeting = require("@shared/greeting")

print(Greeting.forPlayer("world"))
```

- **A `ModuleScript`** is found by the tree, as above. Only a `ModuleScript` can
  be required: a `Script` runs when the world does, and requiring one would run
  it a second time somewhere else.
- **A path** is resolved as a real file. `@shared` is an alias declared in the
  project's `.luaurc`, pointing at `src/shared`.

Both follow the standard Luau rules:

- one evaluation per module per VM;
- a cyclic require behaves as the language specifies;
- a module that errors propagates the error to its requirer, and **the failure
  is cached**: a second require of a broken module raises the same error
  without running it again.

A module returns a value, and the idiomatic value is a table of functions:

```luau
--!strict
local Greeting = {}

function Greeting.forPlayer(name: string): string
    return `Hello, {name}`
end

return Greeting
```

Note the casing: `Greeting.forPlayer` is camelCase because it is reached through
a **module**, not through an object. That rule is uniform across the whole API
and has [a page of its own](manual:why/casing).

## Every script is strict

Every script begins with `--!strict`, and the templates, the CLI and the editor's
script editor all assume it. It is not a style preference. A fully typed API
only pays off if the code using it is checked. The script editor checks as you
type, with Luau's own analyzer and the engine's definitions, and `ludwerk check`
does the same for files, so a wrong property name is an error before the engine
ever runs.

## Reserved names

`src/client/` and `src/server/` are the folder names the file mounts read
(ADR 0105). `Enum.RunContext` is no longer reserved: `Script.RunContext` reads
it (ADR 0138).

## Where to look next

- [Anatomy of a project](manual:get-started/project-anatomy): the whole tree
- [What a script may do](manual:concepts/sandbox): the global environment, in
  full
- [Signals](manual:concepts/signals): events, `Collector` and `Promise`
- [Hot reload](manual:guides/hot-reload): what happens to all this when a file
  is saved
