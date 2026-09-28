# Parallel scripts: the kickoff and the ledger

Block E of [`game-ready-plan.md`](game-ready-plan.md), approved by the owner on
2026-09-27. The order across blocks is in that file; this one is the order of
work inside block E and where each piece stands.

Decision: [ADR 0123](../decisions/0123-scripts-run-in-parallel-inside-actors-and-commit-in-a-fixed-order.md).
**Read it and `docs/architecture.md` §3 before this file.**

## Where it stood on 2026-09-27

- Native work is parallel (`engine/jobs`, one worker per core but one, stable
  commit in bucket order; Jolt on its own pool; audio and I/O threads).
- Scripts run in one VM on the main thread. `ConnectParallel`,
  `task.desynchronize` and `task.synchronize` are reserved names.
- Windows A (after the PreSimulation drain) and B (after the PreRender drain)
  exist in the `Phase` enum, held by `engine/core/tests/phase_tests.cpp`; nothing
  runs in them.
- `ThreadSafety` is on IDL members (157 `Safe`, 154 `Unsafe`, 22
  `ReadParallel` on 2026-09-27), reaches `ClassRegistry` and the api-dump, and
  is read by nothing.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- The rules of [`game-ready-plan.md`](game-ready-plan.md).
- **A game with no `Actor` is unchanged**: no VM created, no window fired, every
  trace and benchmark as before.
- **Every trace that uses actors reproduces bit for bit** with the worker count
  forced to 1, 2 and the machine's maximum — the test that proves completion
  order never leaks.

## Stage E0 — actors and their VMs, still serial

- [ ] IDL: `Actor` (a container).
- [ ] A VM per actor: sandboxed as the game VM, same globals and `@engine/*`;
  shared compiled bytecode; per-actor module state; `[script] max_actors`.
- [ ] Scripts under an actor start in its VM; created and closed with the actor.
- [ ] Everything still runs in serial, on the main thread: correctness first.
- [ ] Tests: two actors requiring one module get two module states; an actor
  destroyed closes its VM; the budget refuses by keyed error.

## Stage E1 — the windows open

- [ ] The tick fires window A and the frame fires window B; `phase_tests.cpp`
  updated to the fired shape.
- [ ] `task.desynchronize`, `task.synchronize`, `Signal:ConnectParallel`; keyed
  errors outside an actor.
- [ ] Parallel execution on the job pool; the world read as it stood when the
  window opened.
- [ ] Resume in actor order (stable id, then desynchronize order).
- [ ] Tests: the worker-count trace test (1, 2, max); a thread desynchronized
  and synchronized returns on the same tick; a render-rate signal's parallel
  handler runs in B.

## Stage E2 — the thread-safety audit and its enforcement

- [ ] The binding checks phase against `ThreadSafety`; `script.err.not_parallel_safe`
  names the member.
- [ ] Audit, class by class, of what may widen from `Unsafe` to `ReadParallel`:
  reading `CFrame`, `Position`, `Size`, attributes, tags, the tree
  (`Parent`, `GetChildren`, `FindFirstChild`), `Workspace:Raycast` and the
  navigation queries — each widened member's C++ path checked for hidden
  writes, and recorded here as a finding when it had one.
- [ ] Tests: each widened class read from many actors at once under
  ThreadSanitizer (a TSan job beside the nightly ASan + UBSan one); every write refused in
  parallel.

## Stage E3 — messages and shared data

- [ ] `Actor:SendMessage`, `BindToMessage`, `BindToMessageParallel`; delivery
  after the window in sender order.
- [ ] `SharedTable`: plain values; reads in a window see the pre-window value;
  writes buffered per actor and applied at the barrier in actor order;
  `SharedTable.update` in serial.
- [ ] Tests: two actors writing one key in one window — the higher actor order
  wins on every run and every worker count.

## Stage E4 — tools, the example and the proof

- [ ] F3 and Stats: actors, VMs, each window's wall time and the speedup against
  the serial sum.
- [ ] The script debugger pauses a window at a breakpoint in parallel code.
- [ ] Hot reload of an actor's script rebuilds only its VM.
- [ ] **An example**: a crowd of a thousand agents (steering and path queries)
  split across actors, with a toggle to run them serially.
- [ ] **The benchmark**: the same crowd serial and parallel on the reference
  machine, speedup recorded in `docs/perf-baselines.md`; the Android device's
  number beside it.
- [ ] Docs: a manual page *Parallel scripts* (actors, the two phases, what may
  be touched, the commit order and why it exists); `api-design.md` §3 and
  `architecture.md` §3 rewritten from "reserved" to "built".

## Findings

(Filled as the work goes: what the ADR assumed that reality corrected.)
