# 0124 — A scene closes as a game does, and a close handler dies with its script

- Status: accepted, built 2026-09-28 (`docs/briefs/foundation-kickoff.md`, A2b). As built: `IsOpen` is a
  method, `IsOpen()` (§9 gives an `Is` prefix to methods), and `BindToClose` returns nothing -- the open
  question below stays open.
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, in conversation, after a save written
  in a `BindToClose` handler never ran under the editor's Stop. The owner
  proposed the split — *"devemos manter o game:BindToClose para o jogo em si
  ... e acho que devemos ter algum tipo o game por exemplo scene ... teríamos
  algo como scene:BindToClose"* — and chose a global `scene` over reaching it
  only through the service, and a warning in the editor when a handler is
  dropped. The same day, asking how a global script and a scene's scripts
  talk, the owner chose messages *"como um actor se comunica"* on `game` and
  `scene` (§6) and the values they carry (§7).
- Amends: [0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md)
  (`SceneService.CurrentScene` stops being a string) and
  [0111](0111-a-game-saves-through-saveservice-into-the-players-own-folder.md)
  (§5, when a save is written at a close).

## Context

What the code did on 2026-09-27 (read, not yet run):

- `game:BindToClose(fn)` appends to **one list for the whole VM**
  (`dataModelBindToClose` in `engine/script/src/services.cpp`). Every call adds
  a handler; nothing is overwritten; there is no way to remove one. At a close,
  each runs in its own coroutine, in registration order, sharing one grace
  period; an error in one does not stop the others.
- **The game closing** (`WorldHost::close`) runs the list, waits out the grace
  period, and flushes saves.
- **The editor's Stop** (`WorldHost::restartRuntime`) flushes saves and drops
  the VM — **the list never runs**. A game tested from the editor never sees its
  close handlers, which is not what a person coming from the platform whose API
  shape this engine follows expects: there, stopping a test closes the game.
- **A scene change** (`LoadScene`, ADR 0106) stops the old scene's scripts and
  destroys its world, **but its scripts' handlers stay in the list** and run when
  the game later closes, against a scene that is gone.
- A scene has no close of its own to bind to. `SceneService.SceneLoading` fires
  before the teardown, but a signal does not hold the teardown for a handler
  that yields.

## Decision

### 1. Two closes, two names

| Call | Runs when |
|---|---|
| `game:BindToClose(fn)` | **the game closes**, from any script |
| `scene:BindToClose(fn)` | **the scene closes**: a `LoadScene` away from it, or the game closing while it is open |

### 2. A global `scene`

- `scene` is a global in every script's environment, as `game`, `workspace`
  and `script` are. It is the **scene open when it is read**.
- It is an object of a new class, `Scene` (not creatable, not in the tree):
  `Name`, `Path`, `IsOpen`, `BindToClose(fn)`. Later members (what `LoadScene`
  passed, for one) may move onto it; this record does not move them.
- **`SceneService.CurrentScene` becomes that object** (it was the path, a
  string). Its path is `SceneService.CurrentScene.Path`. The engine is at 0.0.1
  and ADR 0109 promised no compatibility across the rename, so this changes in
  place; every use in the repository moves in the same commit.
- A `Scene` object kept past its scene's close is **closed**: `IsOpen` is false
  and `BindToClose` on it is a keyed error that names the scene, instead of a
  registration that would never run.
- Inside a `SubWorld` (ADR 0107), `scene` is the sub-world's scene.

### 3. A handler belongs to the script that registered it

- A handler is owned by **the script whose thread called `BindToClose`**, not
  by where the function's code lives: a `ModuleScript` in a scene's
  `ReplicatedStorage`, required by a global script, registers for the global
  script.
- **When a scene closes, every handler owned by one of its scripts leaves both
  lists.** Its `scene:BindToClose` handlers run first (§4); its
  `game:BindToClose` handlers are **dropped without running** — they asked for
  the game's close, and their scene did not live to see it.
- **In the editor only**, the first drop per script logs a warning
  (`script.warn.close_handler_dropped`): the handler belonged to scene X, which
  closed; `scene:BindToClose` runs when a scene closes. An exported game does
  not log it.
- `GlobalScriptService`'s scripts own their handlers until the game closes.

### 4. Order and time

- **The game closing**: the open scene's `scene:BindToClose` handlers, then
  every `game:BindToClose` handler — inside to outside — under the one grace
  period that exists today. Then the saves are flushed.
- **A scene closing on `LoadScene`**: `SceneLoading` fires (a notice; it holds
  nothing), then the scene's `scene:BindToClose` handlers run and the change
  **waits for them**, up to `[scene] close_grace_seconds` (default 5), shorter
  than the game's; then its saves are flushed, the scene is torn down and the
  next one loads. A loading screen put up on `SceneLoading` covers the wait.
- **The editor's Stop is the game closing**: both lists, the same order, the
  same grace period, then the flush — then the VM goes.
- A `SubWorld` closing is its scene closing.
- In a match, each machine runs the handlers of its own scripts.

### 5. Many handlers

Unchanged and now written down: every call adds one; the same function added
twice runs twice; handlers run in registration order, each in its own
coroutine, and may yield; one that errors does not stop the others; one
registered during a close does not run in that close.

### 6. Messages: `game` and `scene` are mailboxes

- `game:SendMessage(topic, ...)` and `game:BindToMessage(topic, fn)`: the
  game's mailbox. `scene:SendMessage(topic, ...)` and
  `scene:BindToMessage(topic, fn)`: the open scene's. The rule is one: **send
  to the mailbox of whom you want to reach, listen on your own.** A scene's
  coin sends `game:SendMessage("CoinCollected", 1)` and the global HUD listens
  on `game`; a global script sends `scene:SendMessage("OpenGate", "north")` and
  the level listens on `scene`.
- The names are ADR 0123's `Actor` messages: the engine has one way to send a
  message, to an actor, a scene or the game.
- **Delivery is deferred** (R8), in send order, each handler on its own
  coroutine. A handler is owned by the script that bound it (§3): a scene
  script's bindings, on either mailbox, go with its scene.
- **A scene's mailbox lives and dies with the scene**: a message sent to a
  closed `Scene` is dropped (a warning in the editor only, never an error — the
  sender may not know it closed).
- **Messages to a scene that is not open yet queue until it opens**:
  `SceneLoad.Scene` (ADR 0125) is the prepared scene's `Scene` object;
  `loading.Scene:SendMessage(...)` is delivered right after its scripts start,
  before `SceneLoaded`. Right after `LoadScene`, `scene` is still the old scene
  until the switch.
- A topic with no handler when a message arrives is dropped, with a warning in
  the editor only ("nobody listens to OpenGate in scene arena").
- **Local to one machine.** A message never crosses the wire: between machines
  it is a `RemoteEvent` (ADR 0077) or a replicated attribute. On a server,
  `game` and `scene` are the server's; on a client, the client's.
- **No reply.** A message is sent and forgotten; a request that wants an answer
  uses a topic back, or a function in a module. A reply form is later, if asked
  for.
- Shared **state** stays where it is (a module in `GlobalScriptService.Shared`,
  attributes); messages are for **events**.

### 7. What a message carries

The same values as an attribute or a save (ADR 0111), plus instances — one rule
for the engine, and the rule ADR 0123's actor messages need anyway, since their
VMs cannot share a function or a table:

- **Arrive equal**: `nil`, `boolean`, `number`, `string`, `vector`, `Vector2`,
  `CFrame`, `Color3`, `ColorSequence`, `NumberSequence`, `UDim`, `UDim2`,
  `Rect`, `EnumItem`.
- **Copied**: tables of those (nested, string keys or an array's `1..n`) and
  `buffer`s — the receiver gets its own copy and cannot change the sender's
  state. A table's metatable is not carried: an "object" arrives as its data,
  and the editor warns that its methods stayed behind.
- **By reference**: instances — the same instance, not a copy.
- **Refused at `SendMessage`**, by a keyed error naming the value and the key
  path where it was found: functions and threads (they would carry the sender's
  variables past its scene's close), tables that contain themselves, and live
  objects (`Signal`, `Connection`, `Tween`, `SceneLoad` and the like) — send the
  data, not the object.

## Consequences

- A game tested in the editor closes as it will for a player.
- A level can save what belongs to it when it ends, and a handler never runs
  against a world that is gone.
- A global script and a level talk without importing each other, and nothing
  either binds outlives its owner.
- `examples/24-scenes` compares `CurrentScene` with a path; that comparison
  would become silently false against an object. It moves to `.Path` in the
  same commit, and a test reads `CurrentScene.Path`.

## Open, for the owner

- Whether `BindToClose` should **return a `Connection`**, so a script can undo a
  registration with `Disconnect()`, as it undoes a signal's. The engine needs
  per-handler removal internally for §3 either way; exposing it is the owner's
  call.

## Amended, 2026-10-03 (D481)

- **`game`, `workspace` and `script` are mutable globals too.** The compiler
  turned `workspace.Gravity` into an import resolved once at load, exactly as
  it had `scene.Name`: every property read through the three instance globals
  was the value the chunk loaded with.
