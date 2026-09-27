# 0105 — Code lives in three script services, and each side's package carries only its own

- Status: accepted
- Date: 2026-09-26
- Decided by: the owner, on 2026-09-26, in conversation. He asked how code that
  runs on the server is kept apart from code that runs on the client, and
  settled it in two passes the same day. The first, committed as this ADR's
  first text, had one `ServerScriptService` for the rules and `ScriptService`
  for everybody else. The second, once scenes that change at run time were on
  the table ([0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md)),
  is this one: a server and a client script service **in each scene**, a
  **`GlobalScriptService`** that outlives every scene, and an export that gives
  each side only its own code. Nothing of the first text had been built, so it
  is replaced rather than amended.
- Amends: [0104](0104-a-game-is-exported-from-one-window-for-windows-linux-and-android.md)
  (the export gains a multiplayer mode and server targets) and
  [0092](0092-a-script-lives-in-the-instance-it-is-put-in.md)
  (`src/server/` and `src/client/` stop being reserved; `ScriptService` is
  retired).
- Relates to: [0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md)
  (scenes at run time, which is why there is a global service at all),
  [0099](0099-teams-and-network-ownership.md) and the multiplayer guide
  (`NetworkService.Authority`, `ServerStorage`, `ReplicatedStorage`).

## Context

A script is an instance (ADRs 0050, 0092): it runs wherever it is put in the
world. In a match **every enabled script runs on every machine**, and a script
splits itself with `if NetworkService.Authority then ... end`. Three things
follow that the owner did not want:

- **The server's code ships to every player.** A packaged game carries all of
  its scripts, so the rules, the spawn logic and anything a server keeps to
  itself are in the folder a player downloaded, readable, even though they never
  run there. `ServerStorage` has the same leak: a replica empties it when it
  joins, but its contents are in the pack.
- **A dedicated server runs, and carries, client code for nothing**: cameras,
  HUDs, input handling, all loaded and started on a machine with no window.
- **Code ends up scattered through the world.** The owner does not want to put
  scripts inside parts and models; he wants code in known places. Others do
  like a script inside the door it opens, so that stays possible -- it is just
  not where the engine sends anybody.

And once a game has more than one scene (a menu, a lobby, a match -- ADR 0106),
some code belongs to one scene and some to the whole game: the arena's rules
end with the arena, but the connection, the lobby's state and the character a
player picked must live across every scene change.

`src/server/`, `src/client/` and `Enum.RunContext` were reserved for this split
and never given a meaning. They were reserved when scripts were files; scripts
are instances now, so the split is a place in the tree, the way `ServerStorage`
already is.

The engines surveyed split the same two ways. One by side: a place's server
script service and its client-side containers, where the server's never reach
a player. One by lifetime: a singleton registered with the project that sits
above whichever scene is loaded, or a game-instance object that outlives every
level load. This decision takes both, in the tree, where a person can see them.

## Decision

### 1. Three script services: two in each scene, one for the whole game

| Service | Belongs to | Its code is | Lives |
|---|---|---|---|
| **`ServerScriptService`** | the scene | that scene's server code: the arena's rules, the lobby's countdown | from when the scene loads to when it unloads |
| **`ClientScriptService`** | the scene | that scene's player code: the arena's HUD, the menu's buttons, a camera | the same |
| **`GlobalScriptService`** | the game | code for every scene, in three fixed folders: **`Server`**, **`Client`** and **`Shared`** | from boot to exit |

- **`GlobalScriptService` is the game's own service**, not a scene's. It is
  never unloaded: a script in it keeps its variables, its connections and its
  threads across every scene change, which is where the character a player
  picked, the connection and the lobby's state are kept.
- **Its three folders are fixed**: made by the engine, not renamed, not
  deleted. `Server` and `Client` follow the same rule as the two scene services
  below; `Shared` holds `ModuleScript`s both sides require.
- **Any scene's scripts can reach it**: `game:GetService("GlobalScriptService")`
  answers the same instance in every scene, and its properties, attributes and
  children can be read and written from anywhere a script may (the network
  rules in §2 still apply). It is a place to keep something for the whole game,
  not the main way scripts talk -- that stays signals, `RemoteEvent`s and
  modules.
- **A `Script` elsewhere in the world keeps running everywhere**, as today: a
  door with its own script, a stamp that carries its behaviour. It is allowed,
  and documented as the second choice.
- **`ScriptService` is retired.** Its role -- code of the machine a player sits
  at -- is `ClientScriptService` for one scene's and `GlobalScriptService.Client`
  for the game's. A scene written before carries what somebody put in its
  `ScriptService` into its `ClientScriptService` when read; a script asking for
  `ScriptService` gets a keyed error naming the two. `Enum.RunContext` stays
  reserved: where code runs is a place in the tree, not a property a script
  carries.

### 2. Where each one runs

| Where a `Script` is | Dedicated server (`--serve`) | Client that joined | Solo and host |
|---|---|---|---|
| `ServerScriptService`, `GlobalScriptService.Server` | runs | **absent** -- never sent, not in its package | runs |
| `ClientScriptService`, `GlobalScriptService.Client` | **absent** -- not in its package | runs | runs |
| `GlobalScriptService.Shared` | required by server code | required by client code | both |
| anywhere else in the world (`Workspace`, a part...) | runs | runs | runs |

- **Solo and host run both sides**, because they are the authority and a player
  at once. A single-player game, and a game whose players host, have one
  machine doing both, and that is the only difference.
- **Server code runs while the machine is the authority.** A game that starts
  solo in its menu and then joins a match (ADR 0106) stops its server code when
  it joins -- it is not the authority any more -- and starts it again, fresh,
  if it drops back to solo.
- **Never replicated**: `ServerScriptService` and `GlobalScriptService.Server`
  are not sent to a replica, and a replica started from a folder that has them
  empties its own, as it empties `ServerStorage`. `ClientScriptService` and
  `GlobalScriptService.Client` are not replicated either: each client runs its
  own, from its own package.
- **What the authority writes to `GlobalScriptService` itself, and to what is
  under `Shared`**, replicates to every client -- an attribute on the service is
  the simplest thing the whole game can see. A client's write to them is its
  own, and the next write from the authority replaces it.
- **In the editor, Play runs as solo**: every service runs.

### 3. Code lives in files, in known places

| Folder | Becomes |
|---|---|
| `src/server/` | `Script`s under `GlobalScriptService.Server` |
| `src/client/` | `Script`s under `GlobalScriptService.Client` |
| `src/shared/` | `ModuleScript`s under `GlobalScriptService.Shared` |
| `src/scenes/<scene>/server/` | `Script`s under that scene's `ServerScriptService` |
| `src/scenes/<scene>/client/` | `Script`s under that scene's `ClientScriptService` |

- `<scene>` is the scene file's name without `.scene.json`: `arena` for
  `scenes/arena.scene.json`. Each subdirectory becomes a `Folder`, as
  `src/scripts/` does today. Hot reload covers them all.
- **A script made in the editor inside one of the three services is written as
  a file** in the folder above, not into the scene: code is in `src/`, where a
  person, a diff and a text editor find it. What is authored there that is not
  code -- a `Folder` of values, a `Sound` -- is saved with the scene for the two
  scene services, and in `content/global.json` for `GlobalScriptService`.
- `src/scripts/` is read as `src/client/` for one release, with a warning that
  names the move; the examples and templates move in the same work.

### 4. The export gives each side only its own code

What a package may leave out depends on who runs the rules, so the project says
it once:

```toml
[export]
multiplayer = "none"                # none | host | dedicated

[network]
server = "play.example.com:7777"   # where NetworkService:Join() goes with no address
```

| Mode | Packages | A player's package carries | The server's package carries |
|---|---|---|---|
| `none` (default) | one per target | everything -- it runs solo, as the authority | -- |
| `host` | one per target | everything -- any player may be the host | -- |
| `dedicated` | a **client** per target, and a **server** for Windows and Linux | everything **except** `ServerScriptService`, `GlobalScriptService.Server`, `ServerStorage`, `src/server/` and `src/scenes/*/server/` | everything **except** `ClientScriptService`, `GlobalScriptService.Client`, `src/client/` and `src/scenes/*/client/` |

- **A dedicated client carries none of the server's code or storage**, and a
  dedicated server none of the clients' code. Tests export a project with a
  sentinel string in each side's scripts and in `ServerStorage` and assert each
  string is absent from the other side's output, pack included.
- **A dedicated client starts in its first scene** -- usually a menu -- and
  joins when its scripts call `NetworkService:Join()` (ADR 0106). With no
  address, `Join` goes to `[network] server`. It can run solo only as far as a
  game with no server code can: menus, settings, a tutorial written as client
  code.
- **The server targets** are `windows-server` and `linux-server`: the player
  host and the game, started as `--serve` with no arguments (the exported
  `luaug.toml` carries the role). No window, so no icon step; trimming the
  engine content a server never reads is later, not now.
- `luaug build --target=` gains the two server targets; the Export window shows
  the multiplayer mode as a selector above the target cards, and the server
  cards only in `dedicated` mode.

### 5. What each service is, on each side

The reference the documentation carries (solo and a host have both columns).
Every service but `GlobalScriptService` belongs to the scene (ADR 0106).

| Service | Dedicated server | Client that joined |
|---|---|---|
| `GlobalScriptService` | `Server` runs, `Shared` is there, `Client` absent | `Client` runs, `Shared` is there, `Server` absent; the service's attributes arrive |
| `ServerScriptService` | runs | absent |
| `ClientScriptService` | absent | runs |
| `ServerStorage` | has it | absent from a dedicated package; emptied on join |
| `ReplicatedStorage` | has it | receives all of it |
| `Workspace` | decides | receives what is near its character |
| `Lighting` | decides | receives it |
| `NetworkService` | `Authority` | `LocalPlayer`, the players |
| `TeamService` | decides | receives the teams |
| `RunService`, `TagService`, `TweenService` | yes | yes |
| `PhysicsService` | simulates | simulates its own character; the rest arrives interpolated |
| `NavigationService` | moves the characters it drives | works, but moving a replicated character is the server's |
| `StreamingService` | decides what each client is sent | `InstanceStreamedIn` / `Out` |
| `VoxelService` | decides | to be verified in this work |
| `InputService` | no devices; a player's input arrives as intent | keyboard, touch, gamepad |
| `UIService`, `AudioService` | exist, draw and play nothing | yes |
| `DebugService` | logs | the F3 overlay |
| `HotReloadService` | development only | development only |

## Consequences

- **Breaking**: `ScriptService` goes, and `src/scripts/` moves to `src/client/`
  (read as it for one release, with a warning). A project that ran a dedicated
  server with its rules behind `if Authority` moves them to
  `ServerScriptService` or `src/server/`. The CHANGELOG says so under
  *Changed -- BREAKING*, with the moves. Solo and host play every migrated
  example exactly as before, and no determinism trace moves. Tagging the release
  that carries it, and its version, is the owner's.
- **Each side's package is smaller and says less**: no rules in a player's
  folder, no HUD code on a server.
- **The tree says where code runs.** Three services, one rule each; a person
  who never opens a property panel still knows what runs where.
- A script under a server service is invisible to a joined client, so a client
  script that reaches for it fails there and works solo -- the documentation
  names this, and the conformance suite covers both.

### Rejected

- **One `ServerScriptService` for the whole game and `ScriptService` for the
  rest** (this ADR's first text). It had no place for code that belongs to one
  scene, and the owner wants scenes to be complete, like places (ADR 0106).
- **Two global services, `GlobalServerScripts` and `GlobalClientScripts`.** Four
  script services in the tree; one global service with three folders says
  "whole game" once.
- **`ClientSceneScriptService` and the like.** The scene is where a person
  spends their time, so the scene's services get the short names; only the
  global one needs a word that says it is different.
- **`RunContext` as a property on every script.** It hides where code runs in a
  property somebody has to open; a service shows it in the tree.
- **Stripping the server's code from every export.** A single-player game and a
  hosted one ARE the server; their package cannot leave the rules out.
