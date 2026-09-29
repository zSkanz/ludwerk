# 0137 — A script runs while it is in the running world, and stops when it leaves

- Status: accepted, built 2026-09-29 (`docs/briefs/script-sides-kickoff.md`, S0 and S1; see the amendment)
- Date: 2026-09-29
- Decided by: the owner, on 2026-09-29, as an emergency: *"vamos corrigir esses
  problemas, tudo ... isso aí é emergência"*, after a survey of the script
  lifecycle, run against the packaged engine, found scripts that never start,
  scripts that start twice, and scripts that run half their file on a client
  and die.
- Builds on: [0050](0050-a-script-is-an-ordinary-instance-and-its-source-is-a-property.md)
  and [0092](0092-a-script-lives-in-the-instance-it-is-put-in.md) (a script is
  an instance, and lives where it is put),
  [0059](0059-enabled-is-about-resumption-and-nothing-else.md) (disabling),
  [0107](0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md)
  (sub-worlds), [0105](0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md)
  (sides by service), [0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md)
  (scene changes), [0124](0124-a-scene-closes-as-a-game-does-and-a-handler-dies-with-its-script.md)
  (a handler dies with its script).
- Followed by: [0138](0138-a-script-carries-the-side-it-runs-on.md), which
  adds a side to each script and relies on the rules below.

## Context

What the survey of 2026-09-29 found. Each item was either run against the
packaged engine (headless, and a local server with a client joined on port
7778) or read in the code:

| Case | Today | How it was found |
|---|---|---|
| A model with a script, cloned at run time | the clone's script never starts | run |
| `Instance.new("Script")`, `Source` set, parented to Workspace | never starts | run |
| The same, created disabled and then enabled | starts | run |
| A script inside a model kept in `ReplicatedStorage` | runs: the template runs itself | run |
| A script in `ServerStorage` | runs, on a joined client too (non-dedicated package) | run |
| A script inside a replicated part, on a joined client | runs its file scope once, then is destroyed with the part by `clearForReplica` and never comes back | run |
| A stamp placed at run time (`Instance.stamp`) | its scripts never start | code: `instance_binding.cpp` `stamp` starts nothing |
| A running script moved into another container | keeps running wherever it lands; no ancestry hook | code: `modules.cpp` `suppressionFor` ignores location |
| A script that survives a scene change (a `KeepOnSceneLoad` screen, a player) | started **again** on every change while its first run goes on | code: `world_host.cpp` `loadScene` then `startScriptsExcept`; nothing records a running script |
| A script disabled and enabled again | the old run's connected handlers fire again beside the new run's | code: suppression is keyed on the instance, not the run |
| Join, Disconnect, JoinFailed | only file-mounted server scripts are restarted | code: `restartServerCode` |
| Hot reload after a run-time Join or scene change | re-boots with the boot topology and boot scene | code: `worldOptions` built once in `engine.cpp` |
| A script writes another script's `Source`, then enables it | the text runs: a `loadstring` the sandbox removed on purpose | code: `Source` is script-writable |
| A `SubWorld` on a dedicated server | runs its client scripts | code: a sub-world always boots `Solo` |

The one rule under all of it: **a script's side and place are checked once, when
it starts, and a start happens only at boot, at a scene change, or on an
`Enabled` write.** Everything that enters the world any other way is dead code;
everything that leaves keeps running.

## Decision

### 1. Live

A `Script` is **live** when all of these hold:

- it is `Enabled` and its `Source` is not empty;
- it is a descendant of the `DataModel`;
- no ancestor is an **inert container** (§3);
- its side runs on this machine (ADR 0105's table, and ADR 0138's `RunContext`).

**A script runs exactly while it is live.** It starts when it becomes live and
stops when it stops being live, whatever made it change: boot, a scene load, a
clone, a stamp, `Instance.new`, a `Parent` write on it or on any ancestor,
`Enabled`, `Destroy`, a topology change (§5).

### 2. A run

- Each start creates a **run**. The engine keeps the set of running scripts and
  each one's current run.
- Threads, connections, render steps and `BindToClose` handlers a run creates
  **belong to the run**. When the run stops, all of them stop: threads are
  killed, connections disconnected, render steps unbound. ADR 0124 already does
  this for a scene's scripts at a scene change; this makes it the rule for every
  stop.
- A script that is already running is **never started again**. A script that
  survives a scene change keeps its run and is not restarted.
- Disabling and enabling again is a stop and a fresh run. The old run's
  handlers never come back.

### 3. Storage does not run

- `ServerStorage` and `ReplicatedStorage` are **inert**: a script under them is
  never live. They hold templates and modules, and a template must not run
  itself.
- A scene that has a `Script` under either one logs one warning when it loads,
  with a key naming the script and where to put it instead. `ModuleScript`s are
  unaffected: they run when they are required.
- The examples, templates and conformance scenes are updated so that none of
  them relies on a script running in storage.

### 4. `Source` is written by the editor, not by a script

- `Script.Source` and `ModuleScript.Source` stay readable, but **a script
  writing either one raises** a keyed error. The editor, the scene reader, the
  file mounts and the dev server keep writing it through the engine's own path.
- This closes the `Source` + `Enabled` route to running text the sandbox never
  loaded.

### 5. Changing topology

- On Join, Host, Disconnect, JoinFailed and a lost server, **every** script is
  checked against §1. Scripts that stopped being live stop; scripts that became
  live start.
- After going back to solo, the machine runs **exactly what a fresh solo boot of
  the current scene would run**. Server scripts that the join destroyed (the
  scene's `ServerScriptService`, file mounts, scripts inside parts the join
  replaced) are put back. Reloading the current scene is the simplest honest
  way to do that; the builder chooses the mechanism and states it.
- **A replica starts no script that joining will destroy.** A `--join` boot
  runs `clearForReplica` before the first script starts. Until ADR 0138 §6
  lands, a script under a replicated parent simply does not run on a replica,
  instead of running its file scope once and dying.

### 6. Hot reload and sub-worlds

- A hot reload re-boots with the **current** topology and the **current** scene.
- A `SubWorld` is its own authority, as ADR 0107 says. Its server-side scripts
  run wherever it is loaded. Its client-side scripts run only where there is a
  display, so never on a dedicated server.

### 7. What it costs

- Becoming or leaving live is checked when a subtree moves. **A subtree with no
  script in it must cost nothing extra.** The builder keeps a count of script
  descendants per subtree, or an equivalent, so that cloning or reparenting ten
  thousand parts costs what it costs today. `docs/perf-baselines.md` records a
  clone of a 10 000-part model with no scripts and one with 1 000 scripts,
  before and after.
- **No determinism trace moves** unless a scenario itself clones, stamps or
  reparents a script. If one does, it is re-recorded once, with the reason
  named, per `tests/determinism/README.md`.

## Consequences

- A clone, a stamp and a script made in code behave like a script in the scene.
- A template in storage stays a template.
- A script that leaves the world takes everything it started with it.
- A game that relied on a script running in `ServerStorage` or
  `ReplicatedStorage` moves it; the warning says where.
- A game that wrote `Source` at run time gets an error. No example does.

## Not decided here

- Where a script runs, per script: ADR 0138.
- Two VMs in a solo game, so that a module's state is not shared between the
  server's and the client's code as it is today: a later decision.

## Amendment -- 2026-09-29, as built (S0 and S1)

- **Live is one function**, `script::scriptLive`: a `Script`, enabled, with a
  non-empty `Source`, a descendant of the `DataModel`, with no `ServerStorage`
  or `ReplicatedStorage` above it, its side running here. `startScript` asks
  it, so boot, a scene load, an `Enabled` write and a move all start by the
  same rule.
- **A run is the globals table its start made** (S0.5, S0.6):
  `ModuleRegistry::runs`, one record per instance slot, holding that table.
  Every thread, handler, render step and close handler knows the globals it
  was made under. **A stop is `endRun`**: the record is cleared, and from then
  on anything of a `Script` whose globals are not its script's current run's
  is suppressed when it comes up -- a thread is not resumed, a handler, a
  render step and a close handler are not called. That is ADR 0059's
  mechanism for `Enabled = false`, applied to every stop, **not an eager kill
  or disconnect**: what a reader can tell apart is that a stopped run's
  connection still reads `Connected` true until it is disconnected or its
  signal's owner goes. A `ModuleScript`'s functions are not
  a run's, and are never suppressed by one: a module required by a stopped
  script still serves the scripts that are running. The `@engine/*` modules'
  own functions run under a globals table with no raw `script` field, and so
  belong to no run either -- without that rule, stopping the camera rig's
  caller stopped the rig.
- **Becoming or leaving live is found by the move that caused it, with no
  per-subtree count.** `World::setParent` already walks the moved subtree for
  its change fan-out; it compares each member's class with `Script`'s and
  queues the scripts (`World::takeMovedScripts`). A subtree with no script in
  it costs one class compare per instance, inside a walk that was already
  there. `ScriptRuntime`'s drain hands the queue and the `Enabled` writes to
  `script::reconcileScripts`: every queued script that is running and no longer
  live stops; then every `Enabled` write is a stop and a fresh start, in
  document order, as boot starts scripts; then every queued script that is live
  and not running starts, **in the order of the moves**, and within one moved
  subtree in its document order (the preorder `setParent` walked). That order
  is deterministic (R10) without walking the world, which a game cloning one
  scripted projectile a frame would otherwise pay every frame; only an
  `Enabled` write pays the walk, as it did before. A script queued twice starts
  once. `script_live.spec.luau` holds each path and the order.
- **Nothing moved starts before the world's scripts have**
  (`ModuleRegistry::started`, set by `startScripts`): in the editor, with the
  game stopped, a move is an edit. A stop needs no such guard -- nothing has a
  run to stop.
- **The storage warning** (`scene.warn.script_in_storage`) is logged once per
  storage service per scene load, after the scene's scripts started: how many
  `Script`s it holds and the first one's name. No example, template or
  conformance scene had one.
- **Changing topology** (§5): a Join clears what the authority replicates and
  then checks every script (`script::reconcileAllScripts`); a Host checks every
  script. **Back to solo** -- a Disconnect, a JoinFailed, a lost server -- is
  `WorldHost::returnToSolo`: the current scene loaded again from this
  machine's own package, which puts back the parts and the scripts inside them
  that the join replaced; then `restartServerCode`, which starts the
  file-mounted server code afresh; then every script checked. A script in
  something the scene keeps across a load keeps its run. **Not restored:** a
  non-code item authored in `content/global.json` under
  `GlobalScriptService.Server` that the join destroyed -- a `Folder` of values
  kept there comes back only with a restart. No example has one.
- **Hot reload** re-boots with `WorldHost::currentOptions`: the topology and
  the scene the world is in now (S0.7).
- **A sub-world** (§6) boots with the `Dedicated` topology when the world that
  holds it is dedicated, and `Solo` otherwise, so its client-side scripts run
  only where there is a display. `sub_world_tests.cpp` holds it.
- **What it cost** (§7), 2026-09-29, `win-msvc-editor`, a clone of a
  10 000-part model, parented and a frame run, best of five, three runs each:
  no scripts, 190 to 223 ms before and 190 to 210 ms after; with 1 000 scripts,
  192 to 207 ms before (none of them started) and 198 to 227 ms after (all of
  them started). Both inside the run-to-run spread. `docs/perf-baselines.md`
  has the table.
- **No determinism trace moved.**
