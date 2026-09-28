# 0111 — A game saves through `SaveService`, into the player's own folder

- Status: accepted (to be built; see `docs/briefs/foundation-kickoff.md`, A2)
- Date: 2026-09-27
- Amended by: [0124](0124-a-scene-closes-as-a-game-does-and-a-handler-dies-with-its-script.md)
  (a scene closes as a game does: `scene:BindToClose`, the global `scene`,
  `SceneService.CurrentScene` as an object, and a close handler dies with its
  script)
- Decided by: the owner, on 2026-09-27, approving the plan in
  `docs/briefs/game-ready-plan.md` after a conversation that found the gap: a
  game made with the engine cannot keep a save, a setting or a best time
  between two runs.
- Relates to: ADR 0045 and 0104 (the exported game and its `[project]`
  identity), ADR 0083 (determinism), ADR 0085 (a player's token), ADR 0106 (a
  scene is a place), ADR 0120 (a player is a key).

## Context

The sandbox (R4) gives a script no file system, and nothing else gives it a
place to keep anything. `SDL_GetPrefPath` is called in
`engine/platform/src/platform.cpp`, but for the engine's own files, under the
brand's name, and no script reaches it. A game can be played; it cannot be
continued.

Every engine a person could compare this one to answers the question, and the
answers differ mainly in where the bytes live: a local file under the user's
data folder (most engines), or a service in the vendor's cloud (the platform
whose API shape this engine follows). A standalone engine has no cloud of its
own, so the local folder is the base and a remote store is a game's own
decision, made through `NetworkService:Request` (ADR 0119).

## Decision

### 1. One service, slots, values

- `SaveService`, reached with `game:GetService("SaveService")`. It is not
  replicated: every machine keeps its own saves.
- `SaveService:GetSlot(name): SaveSlot` yields until the slot is read. A slot is
  a named file; a name is 1 to 64 characters of `[A-Za-z0-9_-]`, anything else
  is a keyed error (no path ever reaches the file system from a script).
- `SaveSlot:Get(key)`, `:Set(key, value)`, `:Update(key, fn)` (the function
  receives the old value and returns the new one), `:Remove(key)`,
  `:GetKeys()`, `:Save()` (yields until the write is durable), and
  `SaveSlot.Changed` (deferred, R8).
- `SaveService:ListSlots()`, `:DeleteSlot(name)`.
- **Values** are the attribute domain (`string`, `number`, `boolean`, `vector`,
  `CFrame`, `Color3`, `Vector2`, `UDim`, `UDim2`, `Rect`, `ColorSequence`,
  `NumberSequence`) **plus tables of them**, nested, with string or array keys.
  A function, a thread, an Instance or a cycle is a keyed error at `Set`, not a
  silent drop at write time.

### 2. Where the bytes live

- `SDL_GetPrefPath([project] company, [project] name)` — the identity ADR 0104
  already carries. On Windows `%APPDATA%\<company>\<name>\`, on Linux
  `~/.local/share/<company>/<name>/`, on Android the app's internal storage.
  Saves live in `saves/` under it.
- **In the editor's Play**, and under `engine dev`, saves go to
  `.engine/saves/` in the project instead, so a test run never touches a real
  player's data and clearing it is deleting one folder.
- A store that syncs the user data folder (Steam Cloud and the like) syncs these
  files without any work from the engine. That is a consequence, not a feature
  to maintain.

### 3. The file

- A header (magic, format version, the game's `SaveService.Version`, payload
  length, an xxh3 checksum of the payload), then the payload in the scene
  file's value encoding, so there is one serializer for values, not two.
- **Atomic**: written to `<slot>.tmp`, flushed and synced, then renamed over
  `<slot>.save`; the previous `<slot>.save` is first renamed to `<slot>.bak`.
- **Recovery**: a slot whose checksum fails is read from `.bak`, with a
  warning in the log naming the slot and the reason. If both fail, `GetSlot`
  returns an empty slot and `SaveSlot.Recovered` says `false` — a game can
  tell the player rather than pretend.
- **A limit**: `[save] max_slot_bytes` (default 4 MiB) and `max_slots`
  (default 64), refused by keyed error at `Set`/`GetSlot`.

### 4. Versions

- `SaveService.Version: number` (default 1), set by a script before the first
  `GetSlot`. `SaveService.Migrate` is a callback `(slot, fromVersion) -> ()`;
  when a slot was written by an older version, it runs once, before `GetSlot`
  returns, and the slot is saved at the new version.

### 5. When it writes

- `Set` marks the slot dirty; the engine writes dirty slots on the I/O thread
  (the async I/O of M7) **at most once a second**, on `Save()`, on
  `game:BindToClose`, and **when the application goes to the background**
  (`SDL_EVENT_WILL_ENTER_BACKGROUND`) — Android kills a backgrounded process
  without a close, and a save that waits for one is lost.
- Nothing blocks the frame: `GetSlot` and `Save` yield the calling thread only.

### 6. Determinism (R10)

Reading a save is a wall-clock fact, as `net.request` is (ADR 0030's module
states the same rule). The simulation's trace does not include what a slot
held; a game that loads a save before its first tick and writes it into the
world has made the load part of its inputs, and the replay harness records it
as one.

### 7. Multiplayer

On a client, a slot is that player's; on the authority, it is the server's. A
server that keeps data **per player** keys it by the player's identity: today
the ADR 0085 token's `UserId`, and after ADR 0120 the player's public key. The
service adds nothing of its own for this; the key is the game's choice.

### 8. Tools

- The editor gains a **Saves** panel: the project's slots, their keys and
  values in the Properties style, edit, delete, and "clear all".
- `engine saves list|clear [--slot=name]` on the command line (the brand's
  command, ADR 0109).

## Consequences

- A game can be continued. The first thing every game after a demo needs.
- The sandbox's wall stays: `SaveService` is the only path from a script to a
  disk, and it names files, never paths.
- One more folder a game writes to on the player's machine, under the name the
  game declares — which is what a player expects.

## Not decided here

- A cloud store. A game that wants one calls its own backend through ADR 0119;
  a first-party store would need a first-party server, which this engine does
  not have.
- Encryption of save files. A save on the player's own machine is the player's;
  obfuscating it stops nobody determined and is not offered as security.

## Built (2026-09-27), and what the code changed about the words above

- **`GetSlotAsync` and `SaveAsync`**, not `GetSlot` and `Save`: the IDL's rule
  is that a method that yields ends in `Async` (`api/schema.luau`), and a
  reader of a script should see the wait in the name.
- **`OnMigrate`**, not `Migrate`: a callback's name begins with `On`, as
  `OnServerInvoke` does.
- **The payload is JSON in which every value says its type** --
  `{"$":"Color3","v":[...]}`, a table as `{"$":"table","a":[...],"m":{...}}` --
  rather than the scene file's encoding. The scene's writes a `Color3` and a
  `Vector3` both as three numbers, and reading one back as the other is the one
  mistake a save cannot make. (The same flaw loses a `Color3`, `CFrame`,
  `Vector2`, `UDim`, `UDim2` or `Rect` attribute across a scene save; that is
  a defect of its own, logged against the scene format.)
- **The header** is one text line -- `ESAV <format> <game version> <length>
  <xxh3>` -- before the payload, so a damaged file is recognised before it is
  parsed.
- **Where**: `platform::preferencePath(company, name)/saves`; the project's
  `.engine/saves/` for the editor, `ludwerk dev` and a match's windows (a
  folder per window, by its label); a folder of its own for a conformance run.
- **The slots live in the host (`SaveStore`), not the VM**, so the editor's
  Stop -- which rebuilds the VM -- and the close path write them after the
  scripts are gone. A background thread does the writing; `WorldHost` flushes
  before a rebuild, at the end of `close`, and on
  `SDL_EVENT_WILL_ENTER_BACKGROUND`.
- **The tools**: the editor's Saves panel (View > Saves) and
  `ludwerk saves list|clear [path] [--slot=name] [--player]`.
