# 0138 — A script carries the side it runs on, and a service decides it for the scripts inside

- Status: accepted, built 2026-09-29 (`docs/briefs/script-sides-kickoff.md`, S2, protocol 29; see the amendment)
- Date: 2026-09-29
- Decided by: the owner, on 2026-09-29: *"na classe do script em si deveria ter
  um run context ... client ou server ... se eu exportar só para o servidor, o
  script no client não vai, e vice-versa"*; in a service the property is *"meio
  escuro, como se não tivesse como alterar"*; in the world it is lit, and a
  mark shows it; *"se eu instanciar uma estampa ... client, ele vai rodar no
  client ... server ... no server ... shared, nos dois"*. Confirmed on the same
  day that a `Shared` script runs **once** on a solo machine, not twice.
- Amends: [0105](0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md):
  §1 (*"`Enum.RunContext` stays reserved"*), the "anywhere else in the world"
  row of §2's table, and the rejected alternative *"`RunContext` as a property
  on every script"*, whose objection (*"it hides where code runs in a property
  somebody has to open"*) §4 answers with a mark in the Explorer.
- Builds on: [0137](0137-a-script-runs-while-it-is-in-the-running-world.md)
  (when a script is live), [0077](0077-game-messages-cross-the-wire-through-a-remote-event.md)
  (one script runs solo, hosting and networked),
  [0083](0083-the-simulation-is-deterministic-across-platforms.md) (the hash).

## Context

ADR 0105 put server code in `ServerScriptService` and client code in
`ClientScriptService`, and each package leaves out the other side's. That solved
the problem for code kept in the services. It left out the code the owner wants
next: **a stamp that carries its own behaviour**. A door needs a server half
(does it open, does the player hold the key, move it) and a client half (a
sound, a particle, the "press E" text). Today the stamp can only:

- hold one script that branches on `NetworkService.Authority`. The server's
  rules then ship to every player, and the dedicated server loads the sounds'
  code; or
- move its halves into the services. The stamp is then no longer
  self-contained, and whoever places it must remember to copy them.

And on a joined client a script inside a replicated part does not survive the
join at all (ADR 0137's table). Scripts never cross the wire, and that stays so.

## Decision

### 1. The property

- `Enum.RunContext` keeps `Client = 0` and `Server = 1` and gains **`Shared = 2`**.
- `Script.RunContext: Enum.RunContext`, default **`Shared`**, which is exactly
  today's behaviour for a script outside the services.
- `ModuleScript` has no `RunContext`: a module runs where it is required.

### 2. The service decides for the scripts inside it

| Where the script is | Its side | The property in Properties |
|---|---|---|
| `ServerScriptService`, `GlobalScriptService.Server` | `Server` | greyed, with *"set by ServerScriptService"* |
| `ClientScriptService`, `GlobalScriptService.Client` | `Client` | greyed, likewise |
| anywhere else in the running world (`Workspace`, a part, a model, a stamp, a screen, `GlobalScriptService.Shared`) | the property | lit, editable |
| `ServerStorage`, `ReplicatedStorage` | never runs (ADR 0137 §3) | lit: it is what the script will be when it is placed |

- **Moving a script into a service writes the property** to the service's side.
  Taken back out, the script keeps it: server code stays server code, and never
  becomes `Shared` by accident, which would ship it to players.
- The property is **written by the editor, the scene reader and tooling, never
  by a running script**: a script may read it, and a write raises. On a match
  the side was fixed when the package was made. This needs a new property mark
  in the IDL (editable in the editor and saved, refused to scripts). `ReadOnly`
  cannot be used: it is neither saved nor cloned.

### 3. Where it runs

| `RunContext` (or the service's side) | Dedicated server | Client that joined | Solo | Host |
|---|---|---|---|---|
| `Server` | runs | absent | runs | runs |
| `Client` | absent | runs | runs | runs |
| `Shared` | runs | runs | **runs once** | **runs once** |

- **Once per machine.** A solo or host machine is the server and a player at
  once, and it has one world. A `Shared` script that ran twice there would do
  everything twice in the same world.
- `Server` and `Client` both run on a solo or host machine, once each, as the
  services already do.

### 4. The mark in the Explorer

- A script **outside** the services shows a small mark on its icon: server,
  client or both. A script inside a service shows none, because the service
  already says it.
- The Explorer's badge drawing, today hard-wired to the stamp overlay, takes the
  overlay to draw. The three marks are drawn in the theme's style, as SVGs
  beside the stamp overlay.
- The editor's *Insert* menu offers three entries: a server script, a client
  script, and a script for both. They set the property as they insert.
  Inserted into a service, the service decides.

### 5. What each package carries

- **The side left out is removed per script, not per service**: in every scene
  file (the whole tree, `Workspace` included), `global.json`, every stamp file
  and the partition cache.
  - The dedicated server's package drops the `Source` of every `Client` script.
  - A joined client's package drops the `Source` of every `Server` script.
  - The **instance stays**, with an empty `Source`, so the tree is the same on
    every machine for replication and the hash. It is never live, because its
    side does not run there.
- A solo or host package keeps everything, as today.
- **A script's `Source` inside a scene or a stamp is compiled to bytecode** in a
  package, as `src/` files already are, unless `ship_source = true`.
- `tests/packaging/sides.test.luau` places a sentinel in each of these and
  proves it absent from the unpacked package of the other side:
  - a `Workspace` script,
  - a script inside a part,
  - a stamp's script,
  - a `ServerStorage` module,
  - a file in `.engine/trash/`.

### 6. A client runs its own copy

Scripts still never cross the wire (`api/wire/state.wire.luau` keeps them
excluded). A replica runs the `Client` and `Shared` scripts **from its own
package**, and attaches them to the instances the authority sends:

- **In the scene.** Before `clearForReplica` destroys a replicated subtree, the
  replica takes out the live-able scripts under it, keyed by their parent's
  identity in the scene. When the authority's instance with that identity
  arrives, the script is put back under it and becomes live. If the scene and
  the wire share no stable identity for an authored instance today, the builder
  adds one, and states it in this ADR's amendment.
- **A stamp placed at run time.** The authority's stamp root carries **which
  stamp it is** (the stamp asset's reference) on the wire. This is a protocol
  bump. The replica reads that stamp from its own package and attaches its
  `Client` and `Shared` scripts under the matching instances of the arriving
  subtree.
- **A clone of a stamp** keeps the reference, and behaves the same.
- **Anything else a script makes at run time never reaches a replica's code**:
  a clone of a plain model, or a script created with `Instance.new`. This is
  the price of never sending code, and the manual says so. Code a client needs
  must be in the client's package: a service, a stamp, or the scene.

### 7. The hash and the file

- The world hash counts `RunContext` **only when it is not `Shared`**, following
  the voxel fluid fields' precedent, so no determinism trace moves.
- A scene writes `RunContext` only when it is not `Shared`. A file without it
  reads `Shared`, so no scene needs a migration and no version bump.
- A file-mounted script (from `src/`) is always in a service, so its side comes
  from there. No file suffix is added.

### 8. Help before it runs

- The editor's language service warns:
  - when a client-side script (by service or property) asks for
    `ServerStorage` or `ServerScriptService`: it works solo and finds nothing
    on a client that joined;
  - when a server-side script touches the camera, `UIService` or
    `InputService`;
  - when, in a project whose `[export] multiplayer` is not `none`, a `Shared`
    script in the world never branches on `NetworkService.Authority`: it runs on
    the server and on every client.
- `ludwerk check` gains a pass over the scripts inside scenes and stamps, with
  the same warnings. Today it sees only `.luau` files.
- The manual gains *"Where my code runs"*: the table of §3, the solo rule, §6's
  price, and the pitfall of a module's state being shared solo (one VM) and not
  in a match.

## Consequences

- A stamp carries its server half and its client half, and each package gets
  its own.
- Where code runs is visible in the tree: by the service, or by the mark.
- Code a player never runs is not in the player's package, wherever it is
  written.
- Two ways to say where code runs (the place, the property) resolve by one
  rule: the service decides inside it, and the property decides outside.

## Not decided here

- Two VMs on a solo machine (the module-state pitfall of §8): a later decision.
- Sending code to a replica: never. It would let a server run code on a client.

## Amendment -- 2026-09-29, as built (S2, protocol 29)

- **The property** is `ScriptComponent::runContext`, `Shared` (2) by default,
  with S0's `ScriptReadOnly` mark -- written by the editor, the scene reader and
  a clone, refused to a script with `script.err.property_editor_only`. The one
  mark does for `Source` and for this what §2 asked a new one for.
- **The side** is `script::scriptSideOf`: `serviceSideOf` -- the walk ADR 0105
  already made -- and, when no service answers, the script's `RunContext`. The
  one gate that decides whether a script runs here (`scriptLive`, ADR 0137) is
  unchanged, so boot, a move, a clone and a join all follow it.
- **A service writes its side** (§2) in every editor verb that puts a script
  somewhere: reparent, create, paste and both ways of placing a stamp. Inside
  the verb's own undo step, which records the world before it moves anything.
  Properties greys the row inside a service, and hovering it names the service
  (`engine.overlay.properties.side_set_by`).
- **The marks** (§4) are `overlay.SideServer` (a rack's two slots),
  `overlay.SideClient` (a screen on a stand) and `overlay.SideShared` (the disc
  split in two), cut into the stamp mark's disc in `tools/repo/draw_icons.py`
  and tinted `system`, `ui` and `script`. `drawIconBadge` takes the face to
  draw; a script outside the services shows its side rather than a stamp mark.
  **The three *Insert* entries are on the Explorer's row menu**, beside Insert
  Object, not in the class picker or the command palette: the picker returns a
  class, and a side is not one.
- **The package** (§5): `tools/cli/export/game.luau`'s `stripScripts` removes
  the `Source` of every `Script` whose `RunContext` is the other side's, in
  every scene tree the package keeps (the world and each storage), every stamp
  and the whole of `global.json`. A file without `Source` reads back empty.
  **The partition cache follows** without its own step: a package partitions its
  scenes from the staged, stripped content. The sentinel test decodes the
  compiled scripts' base64 bytecode (S0.3) to find a side's code in it, and
  covers a world script, a script in a part, a stamp's two halves, a script in
  `GlobalScriptService.Shared`, a `ServerStorage` module and the trash.
- **An instance's origin** (§6): the world's record carries where an instance
  was authored and its place there (`World::Origin`), neither hashed nor saved.
  A scene read numbers **what it made** -- not what the world held already --
  per tree, in preorder: `scene:Workspace`, `scene:<storage>`, each storage
  counted alone so a package that leaves one out numbers the rest alike.
  `Instance.stamp` numbers what it placed as `stamp:<name>`, root 0, linked or
  not. **A clone keeps it**, so a clone of an authored door is the door again to
  a replica -- wider than §6 promised, which named a stamp's clone only; a clone
  of a scene's model now brings its client code too.
- **The wire** (protocol 29): `Spawn` carries `origin` and `originIndex`. No
  scene name is in it: spawns flow only while authority and replica hold the
  same scene, because the replica follows a scene change before the new
  scene's spawns.
- **The replica's own copy** is `replication::ScriptTemplates`, held by the
  `WorldHost`. `clearForReplica` takes the topmost scripts under each subtree
  it destroys -- `ModuleScript`s too, so a module in a replicated folder of
  `ReplicatedStorage` no longer dies with it -- detached and keyed by their
  parent's origin; one whose parent was made at run time goes. **The first
  clear that destroys anything replaces the scene's templates**, and a clear
  that finds nothing keeps them: a `--join` boot clears, and the socket
  opening clears again. Each spawn clones its origin's templates under the new
  instance, and S1 starts them once it is in the world. A stamp's templates are
  read from the replica's package the first time one of its instances arrives,
  placed and numbered as `Instance.stamp` does. Back in solo, they are dropped.
- **The hash and the file** (§7) share one rule, `scene::quietAtDefault`: a
  property left out of both at its default, today `RunContext` at `Shared`.
  No determinism trace moved and no scene changed.
- **Help** (§8) is one C++ lint, `app::lintScriptSide`, over words outside
  comments -- strings count, since `GetService("UIService")` names a service in
  one. The script editor underlines its findings when the text comes to rest;
  `ludwerk check` runs it through `engine-host --check-sides` over scenes,
  stamps, `global.json` and `src/`, so the two say the same thing. **All three
  warnings need a project whose `[export] multiplayer` is not `none`**, where §8
  asked it of the third only: a solo game has one side, and the first run over
  the examples warned `examples/28-arcade`, a solo game, for setting its camera
  from a scene's server script. `ludwerk check` still types only `.luau`
  files; the scene and stamp pass is the side pass.
