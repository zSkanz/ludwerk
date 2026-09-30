# Script lifecycle and sides: the kickoff and the ledger

**An emergency, by the owner's order of 2026-09-29**: *"vamos corrigir esses
problemas, tudo ... e implementar essa feature ... depois ... rodar uma
auditoria também ... isso aí é emergência"*. It goes **before** everything else
in the queue: F2 and the rest of the game-ready plan wait until this ledger is
closed. Finish the work in hand to a green commit first (ADR 0136, built and not
yet committed on 2026-09-29), then start here.

| Stage | Decision |
|---|---|
| S0 — defects that are wrong today | this ledger, and ADR 0137's table |
| S1 — a script runs while it is live | [ADR 0137](../decisions/0137-a-script-runs-while-it-is-in-the-running-world.md) |
| S2 — a script carries its side | [ADR 0138](../decisions/0138-a-script-carries-the-side-it-runs-on.md) |
| S3 — the audit of the script system | this ledger |
| S4 — the close | this ledger |

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- **A failing test first**, for every defect and every rule: a conformance spec
  (`tests/conformance/`), a C++ test, or a packaging test. It must fail before
  the change and pass after it.
- **No determinism trace moves** unless a scenario itself clones, stamps or
  reparents a script. A trace that moves is re-recorded once, with the reason
  named, as `tests/determinism/README.md` requires.
- A protocol bump (S2 §6) is recorded as every earlier one was.
- R7: no name of the reference platform in code, in keys or in comments. A
  client-only script is a `Script` with `RunContext = Client`, never a class of
  its own.
- The full `scripts/localgate.ps1` runs before each push, the Linux stage
  included.
- Stage only what this work wrote. The tree carries other sessions' files
  (branding, art); never `git add -A`.
- Where the survey's citations and the code disagree, the code wins. Record
  what the survey got wrong in this ledger's *Findings*.

## Stage S0 — defects (wrong today, whatever S1 and S2 decide)

Security first:

- [x] **S0.1 `.engine/trash` ships.** The editor moves orphaned script files to
  `.engine/trash/` (`engine/app/src/script_files.cpp`). The export's
  `EditorOnly` list in `tools/cli/export/game.luau` does not name it, so every
  package, the players' included, carries those files as source, server code
  included. Leave it out of every package. The packaging test puts a sentinel
  there. Done: `trash` is editor-only (D331).
- [x] **S0.2 `Source` as `loadstring`.** A script can write another script's
  `Source` and enable it (`class_descriptors.gen.cpp`: `Source` is not
  read-only), which runs text the sandbox never loaded. Refuse the write to
  scripts, with a keyed error. The editor, the scene reader, the mounts and the
  dev server keep their own path (ADR 0137 §4). Done: the `ScriptReadOnly` IDL mark (D332).
- [x] **S0.3 Scene scripts ship as plain text** in every package: the
  bytecode option covers `src/` only. Compile scene and stamp `Source` in a
  package unless `ship_source = true` (ADR 0138 §5). The sentinel test
  (`tests/packaging/sides.test.luau`) today replaces the starter scene with one
  that has no script. Make it cover scene scripts. Done: `--compile-content-scripts`
  on the staged content (D333).

Then correctness:

- [x] **S0.4 A script under a replicated parent runs half on a replica.** It
  runs its file scope, then `clearForReplica` destroys it (run on 2026-09-29:
  a part's script printed once on a joined client and its `Heartbeat` never
  fired). Run `clearForReplica` before the first script starts on a `--join`
  boot, and on a run-time Join stop those scripts cleanly. Until S2 §6, such a
  script does not run on a replica at all. Done for the boot (D334); a run-time Join destroys them, which stops them.
- [x] **S0.5 A double start on a scene change.** Scripts that survive
  `clearScene` (a `KeepOnSceneLoad` screen, a player, a `generated` instance) are
  started again by `startScriptsExcept` while their first run goes on. Keep a
  record of running scripts and never start one twice. Done: the run record (D335).
- [x] **S0.6 Old handlers come back on re-enable.** Suppression is keyed on the
  script instance (`modules.cpp` `suppressionFor`, `signals.cpp`), so a
  disabled-then-enabled script's old connected handlers fire beside the new
  run's. Key it on the run (ADR 0137 §2). Done: `SuppressReason::Ended` (D335).
- [x] **S0.7 Hot reload uses the boot topology and scene.** `worldOptions` is
  built once (`engine.cpp`). Re-boot with the current ones. Done: `currentOptions` (D337).
- [x] **S0.8 `ludwerk check`'s i18n lint** is announced in
  `tools/cli/commands/check.luau`'s header and never called by `run()`. Call it,
  or correct the header. Done: the header corrected (D338).

S0.5 and S0.6 are the first half of S1's run record. Build them so that S1
extends them, not replaces them.

## Stage S1 — a script runs while it is live (ADR 0137)

- [x] The run record and "live" (§1, §2): a start creates a run. Threads,
  connections, render steps and close handlers belong to it, and a stop ends
  all of them.
- [x] Start on becoming live, stop on leaving, for every path: clone,
  `Instance.stamp`, `Instance.new` plus `Parent`, a `Parent` write on the script
  or any ancestor, `Enabled`, `Destroy`. Conformance specs for each path.
  `instance/script_enabled.spec.luau` today asserts that a script created
  enabled does not start. That assertion is reversed, with the ADR named.
- [x] Inert storage (§3): `ServerStorage`, `ReplicatedStorage`. One keyed
  warning per scene that has a `Script` there. Fix the examples, templates and
  conformance scenes that rely on it (search them all).
- [x] `Source` refused to scripts (§4), if S0.2 did not already finish it.
- [x] Topology changes (§5): every script re-checked on Join, Host,
  Disconnect, JoinFailed and a lost server. Back in solo, the machine runs what
  a fresh solo boot of the current scene would run. State the mechanism in
  ADR 0137's amendment.
- [x] Sub-worlds (§6): client-side scripts only where there is a display.
- [x] Cost (§7): a subtree with no scripts costs nothing extra.
  `docs/perf-baselines.md` gets a clone of 10 000 parts with no scripts and one
  with 1 000 scripts, before and after.
- [x] ADR 0137's amendment, as built.

S1 done 2026-09-29 (D339 to D343). `instance/script_live.spec.luau` holds every
path in and out and the order of the starts; `script_enabled.spec.luau` no
longer asserted the old rule by the time S1 came, because S0.2 had already
rewritten it around stamps. `network_session_tests.cpp` holds back to solo and
`sub_world_tests.cpp` the dedicated sub-world.

## Stage S2 — a script carries its side (ADR 0138)

- [x] `Enum.RunContext.Shared = 2`. `Script.RunContext`, default `Shared`, with
  a new IDL mark: editable and saved, refused to scripts (§2). Regenerate the
  API dump, the definitions and the docs, as `gen_cpp` and the gate require.
- [x] The service decides (§2): the effective side, and the property written
  when a script is moved into a service. Editor undo covers the write.
- [x] Where it runs (§3), wired into the side check that `scriptSideOf` and
  `scriptSideRunsHere` make today.
- [x] The Properties panel greys it inside a service, with *"set by …"*
  (i18n'd). The Explorer mark outside the services (§4): generalise
  `drawIconBadge`, draw three overlay SVGs in the theme's style. The *Insert*
  menu's three entries.
- [x] Packages (§5): the per-script strip across scenes, `global.json`, stamps
  and the partition cache, with the instance kept and `Source` emptied. The
  sentinel test covers all five places §5 lists.
- [x] A replica runs its own copy (§6):
  - the scene's scripts, re-attached by identity;
  - the stamp reference on the wire (a protocol bump) and the replica's
    attachment from its own package;
  - a clone of a stamp behaves the same.
- [x] Hash and file (§7): hashed and written only when not `Shared`. Prove no
  trace moved.
- [x] Help (§8): the language service's three warnings, the `ludwerk check`
  pass over scene and stamp scripts, and the manual's *"Where my code runs"*.
- [x] ADR 0105 gets a line under its status pointing to 0138. The decisions
  index lists 0137 and 0138.
- [x] ADR 0138's amendment, as built.

S2 done 2026-09-29, protocol 29. `instance/script_run_context.spec.luau`,
`world_host_tests.cpp` (where each side runs, once; the file and the hash; a
move into a service and its undo), `network_session_tests.cpp` (a joined
client's own copy of a door's scripts and of a stamp placed at run time),
`script_sides_tests.cpp`, `export.test.luau` and the sentinel test. The
*Insert* entries are on the Explorer's row menu (the amendment says why).

## Stage S3 — the audit of the script system

Run once S1 and S2 are in, by the method of the audit of 2026-09-28
(`docs/briefs/audit-2026-09-28.md`): findings ranked P0, P1 and P2; a failing
test for each before its fix; defects that share a root fixed together.

- [x] **The matrix.** Every way a script enters or leaves the world (boot, scene
  load, clone, stamp, `Instance.new`, reparent in, reparent out, `Enabled`,
  `Destroy`, scene change, hot reload) × every topology (solo, host, dedicated,
  replica, sub-world) × every container (each service, each storage, the world,
  a stamp, a screen) × each `RunContext`. Each cell is either a test or a line
  saying why it cannot happen.
- [x] **Leaks.** Unpack every target's package (windows, linux, android and
  both servers; `none`, `host` and `dedicated`). Search it for every sentinel,
  as source and as bytecode string constants.
- [x] **Network.** Join, leave, rejoin, a scene change on a replica, and a stamp
  placed and cloned at run time. Each with scripts of each side, checked on the
  authority and on two replicas.
- [x] **Security.** Nothing a peer sends can start, stop or change a script on
  another machine. Nothing a script does can run text it did not load.
- [x] **Determinism.** The traces, across worker counts.
- [x] **Performance.** The S1 baselines. Plus a scene change with 1 000 scripts,
  and 200 stamps with scripts placed in one tick.
- [x] **Editor.** Greying, the marks, *Insert*, undo of a move into a service,
  and a stamp edited and saved with scripts of each side.
- [x] **Docs.** The manual, the API reference and the ADRs agree with what was
  built.

S3 done 2026-09-29: [`script-sides-audit-2026-09-29.md`](script-sides-audit-2026-09-29.md),
D344 to D359, sixteen defects fixed (seven P1, nine P2, no P0).

## Stage S4 — the close

- [ ] Rebuild the package (`scripts/package.ps1`).
- [ ] Put a stamp door in `C:\Users\juanr\Downloads\Ludwerk-TesteMultiplayer` (the
  owner's test game, outside the repository). The door has a `Server` script
  (it opens for a player at it) and a `Client` script (a sound and a text).
  Export it `dedicated`, and prove the client package has no `Server` source
  and the server package no `Client` source. Run a server on **port 7778,
  never 7777**, which is the owner's own live server. Join it with a client
  under real input, and see the door open with its sound.
- [ ] `PROGRESS.md` records the ledger closed. The queue returns to F2.

## Findings

- **S0.2 reached the conformance suite.** Three specs wrote `Source` to make
  scripts and modules with code; they now clone templates from stamps under
  `tests/conformance/content/stamps/`, and `Instance.stamp` in a conformance
  run looks there (D336) -- it looked under the project's `content/` only.
- **A spec that errors at its file scope vanishes from the count.** Before the
  stamp lookup was fixed, `script_enabled.spec.luau` failed at its first line;
  the run reported 1,386 cases passing instead of a failure, because a spec is
  an entry script and the runner only sees what registered. For S3: the runner
  should know which spec files registered nothing.
- **S0.3 needed the one-sided package staged too.** A game that is not
  dedicated packed straight from the project's `content/`; it is staged like a
  side now, so its scene scripts can be compiled without touching the
  project's files.
- **S0.4 was the boot only.** A run-time Join already destroyed those scripts
  before any of them could run again, which stops them (D097); the boot was
  where the file scope got its one run, because the boot drain came before the
  socket opened.
- **S0.5's second start was every scene change, not only the first:** a kept
  script had run three times after two changes.
- **S0.8's lint is the repository's.** It checks the engine's `ENG_TR` keys,
  which a game has none of; the header was corrected rather than the call
  added.
- **S1 needed no per-subtree script count.** `setParent` already walks the
  moved subtree for the change fan-out, so a class compare per member finds
  the scripts; the measured clone of 10 000 parts did not move (the amendment
  and `docs/perf-baselines.md`).
- **The first build walked the whole world on every drain a script moved in**,
  to start the moved scripts in document order. A game that clones a scripted
  projectile a frame would have paid a walk of the world a frame. Moved
  scripts now start in the order of the moves, which is as deterministic;
  only an `Enabled` write walks, as before S1.
- **A stop suppresses; it does not disconnect** -- which was wrong, and S4
  corrected it (D360): the suppressed handlers stayed on their signals, and a
  game spawning scripted projectiles slowed without bound. A stop disconnects
  now, after the drain.
- **An `@engine/*` module's functions belong to no run.** S0.6's first
  version suppressed them with the script that called them, and stopped the
  camera rig; a run's globals now need a raw `script` field.
- **Back to solo could not be "restart the server code" alone**: the join had
  replaced the scene's parts, and the scripts inside them, with the
  authority's. Loading the current scene again is what a solo boot of it is.
- **S2's new IDL mark was S0's.** `ScriptReadOnly`, made for `Source`, is
  exactly "editable and saved, refused to scripts"; `RunContext` wears it with
  its own error key.
- **The sentinel test could not see a scene script's code.** S0.3 ships it as
  base64 bytecode, so the string a script prints is not in the package's text;
  the test decodes every `luauc:` blob before it searches. Without that, the
  per-script strip would have passed with nothing stripped.
- **An origin had to be what a read MADE, not a walk of the tree after it.** A
  replica loads a scene into a world already holding the authority's instances,
  and the authority's holds its players' characters; numbering the whole tree
  would number two different trees.
- **The side warnings are for multiplayer projects only.** Run over every
  example, the first version warned a solo arcade for its camera.
- **The replica now keeps modules too.** A `ModuleScript` in a replicated
  folder of `ReplicatedStorage` died with the join's clear; the templates keep
  it and put it back.
- **The audit's leak item found what S0.3 broke**: a game that is not
  dedicated lost `global.json`'s `Client` folder, because staging it as the
  side `all` stripped `global.json` as the server's (D345). Only building the
  other modes showed it.
- **A run cannot be known by a global the script owns.** The `script` global
  was the run's name, and a script could clear it (D356); a weak registry
  table keyed by the run's globals table is the name now.
- **The waits were the stop's blind spot**: every yield that resumes through
  `resumeScheduled` -- `WaitForChild`, answers, requests, saves, preloads --
  came back into a stopped run (D348).
