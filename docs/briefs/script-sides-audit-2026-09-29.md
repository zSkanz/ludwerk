# The script-sides audit, 2026-09-29

Stage S3 of [`script-sides-kickoff.md`](script-sides-kickoff.md): the script
system audited once S1 (ADR 0137) and S2 (ADR 0138) were in, by the method of
the audit of 2026-09-28 -- findings ranked, a failing test before each fix,
defects of one root fixed together. A read-only review of the three commits
(S0, S1, S2) ran beside the tests below; every finding it made was reproduced
by a test that failed before its fix, or by the editor driven with real input.

**Sixteen defects, D344 to D359, all fixed**: seven P1, nine P2, no P0. No
peer can make another machine run code that machine did not ship.

## The matrix

`engine/app/tests/script_matrix_tests.cpp`, checked against an oracle written
from the ADRs' tables rather than from the code:

- **Authored**: eleven containers -- the world, a part, a model, a screen,
  `ServerScriptService`, `ClientScriptService`, `GlobalScriptService`'s three
  folders, `ServerStorage`, `ReplicatedStorage` -- times three `RunContext`s
  times four topologies booted (solo, host, dedicated, replica). Each cell is
  0 or 1 runs, never 2.
- **At run time**: a stamp placed, a clone, a move out of storage, `Enabled`
  written -- times three `RunContext`s times the same four topologies.

The rows the matrix does not hold, and where they are held instead:

| Cell | Held by |
|---|---|
| Leaving the world: parent to nil, into storage, with its model, `Destroy` | `script_live.spec.luau`, `script_enabled.spec.luau` |
| Scene change | `network_session_tests.cpp` (the audit's network run), `world_host_tests.cpp` (D346) |
| Hot reload | `reload_tests.cpp` (S0.7, D358) |
| A sub-world, and a sub-world on a dedicated server | `sub_world_tests.cpp` |
| A replica with an authority, not booted alone | `network_session_tests.cpp` (a door's scripts, a stamp placed and cloned; two clients, one leaving and rejoining, a scene change) |
| `Instance.new("Script")` and a parent | cannot run: a script cannot write `Source` (ADR 0137 §4), so it is never live |
| A replica booted with no authority, a script in a replicated part | never runs until the authority sends the part: the matrix's oracle says 0, and the network tests the rest |

## Leaks

`tests/packaging/sides.test.luau` builds a dedicated game's client and server
packages and, now, the same game as `host` and as `none`, and searches every
file -- the pack included, and every compiled script's bytecode decoded -- for
every sentinel. **D345 was found here**: the non-dedicated packages lost
`global.json`'s `Client` folder. The Linux stage runs the same test on Linux.
Android was built once for the audit (a dedicated game's client APK, unpacked):
no server sentinel, no source, every client sentinel present. A test that
builds an APK in the gate would add a Gradle run to every push, which the
Android stage already pays once.

## Network

Host and two clients over the memory transport
(`network_session_tests.cpp`): both clients run their own copy of a door's
client and shared scripts and of two lamps' (a stamp placed and cloned at run
time), and none of its server code; one leaves (its scene read again, solo),
rejoins (its own copy again); the server changes scene and both follow.

## Security

- A peer cannot start, stop or change a script on another machine: scripts
  never cross the wire; a spawn's origin chooses among the client's own
  scripts only. **D344** bounded what a hostile server's origins cost.
- A script cannot run text it did not load: `Source` and `RunContext` refuse
  a script's write; `loadstring`, `getfenv` and `setfenv` are not in the
  sandbox; a client's scripts come from its own package. **D356** closed a
  script's way out of its own stop.

## Determinism

The engine's job pool takes its worker count from the machine (19 here, 3 on
CI's runners) and has no override to sweep it in one process; the committed
traces reproduce on both, and the script lifecycle runs on the main thread
only. No trace moved in S0, S1, S2 or S3.

## Performance

`docs/perf-baselines.md`: ADR 0137's clone (unchanged), a scene change with
1 000 scripts (unchanged), 200 stamps with two scripts each placed in a tick
(+2.5 ms, the 400 starts that did not happen before).

## Editor

Driven with real input in the editor build: the Explorer's marks, the
Properties row greyed inside a service with its "Set by" text, the row menu's
three script entries (a client script made in the world, its `RunContext` lit
and `Client`). **D359**: the mark for both sides did not show. Undo of a move
into a service, and a stamp saved with a script of each side, are
`world_host_tests.cpp`.

## Docs

The manual, the API reference and the ADRs were read against what was built;
one sentence of the multiplayer guide was sharpened. The ADR amendments carry
this audit's changes.
