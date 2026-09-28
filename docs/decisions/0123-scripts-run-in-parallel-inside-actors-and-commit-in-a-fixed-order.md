# 0123 — Scripts run in parallel inside actors, and commit in a fixed order

- Status: accepted (to be built; see `docs/briefs/parallel-kickoff.md`)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, asking whether the engine supports
  multitasking and approving block E of `docs/briefs/game-ready-plan.md` when
  the answer was "natively yes, in scripts no".
- Builds on: `docs/architecture.md` §3 (the reserved parallel windows A and B),
  `api/schema.luau` (`ThreadSafety`), `engine/jobs/include/engine/jobs/commit.h`
  (the stable commit rule), [0083](0083-the-simulation-is-deterministic-across-platforms.md)
  (determinism), `docs/api-design.md` §3.1 (deferred signals).

## Context

The native side is parallel: a job pool of one worker per core but one meshes
terrain and voxels, prefilters the environment and decodes icons, committing
results in bucket order so a replay is bit-identical; Jolt steps on its own
pool; audio and file I/O have their own threads.

**Scripts are not.** Every script runs in one Luau VM on the main thread.
`task.spawn` is a coroutine — concurrency, never two things at once. A game with
many agents, procedural generation or its own pathfinding is bound to one core.

The engine reserved the shape from the start: windows A (after the
PreSimulation drain, inside the tick) and B (after the PreRender drain, at render
rate) exist in the `Phase` enum and are held in place by
`engine/core/tests/phase_tests.cpp`; every IDL member carries a `ThreadSafety`
level; `ConnectParallel`, `task.desynchronize` and `task.synchronize` are
reserved names. Nothing runs in the windows and nothing reads the annotations.

The one hard constraint is R10: two workers finishing in a different order must
not produce a different world.

## Decision

### 1. `Actor` and its VM

- `Actor` is a container instance, like a `Model`. The scripts under an actor
  run in **that actor's own Luau VM** (a separate `lua_State` global state,
  sandboxed exactly as the game VM, with the same globals and `@engine/*`
  modules). A script not under an actor runs in the game VM, as today.
- A module `require`d inside an actor is loaded in that actor's VM: module
  state is per actor, never shared. Bytecode is compiled once and shared.
- VMs are created when an actor's first script starts and closed when the
  actor leaves the world. `[script] max_actors` (default 256) bounds them.
- An `Actor` replicates as a container; its VMs are per machine, as any
  script's state is.

### 2. Two phases

- **Serial** — what every script does today: anything, in the game's
  deterministic order.
- **Parallel** — code running in window A or B, on the job workers, many actors
  at once. Reached three ways:
  - `task.desynchronize()`: the calling thread resumes in the next parallel
    window;
  - `task.synchronize()`: the calling thread resumes in the serial phase after
    the window;
  - `Signal:ConnectParallel(fn)`: the handler runs in the parallel window that
    follows the fire's drain (A for tick-rate signals, B for render-rate ones).
- Only a script under an `Actor` may enter the parallel phase; in the game VM
  the three calls are keyed errors.

### 3. What parallel code may touch

- The binding checks the current phase against each member's `ThreadSafety`:
  - `Safe` — anywhere;
  - `ReadParallel` — read in parallel, write only in serial;
  - `LocalSafe` — callable in parallel, touching only the caller's actor;
  - `Unsafe` — serial only.
  A violation is a keyed error naming the member and the phase
  (`script.err.not_parallel_safe`), never a race.
- **The world is read as it stood when the window opened.** No member that
  writes the world is callable in parallel; engine systems do not run during a
  window.
- The annotations are **audited** before the windows open: a member is widened
  from `Unsafe` to `ReadParallel` only when its C++ read path has been checked
  for hidden writes (lazy caches, dirty flags, ref counts) — the audit is part of
  the work, member by member, with a test per widened class.

### 4. Commit in a fixed order

- When a window closes, the threads that called `task.synchronize()` resume
  **in actor order** (the actor's stable id, assigned at creation, then the
  order the threads desynchronized), never in completion order. This is the
  stable commit rule of `commit.h`, applied to scripts.
- **Messages**: `Actor:SendMessage(topic, ...)` and
  `Actor:BindToMessage(topic, fn)` (and `BindToMessageParallel`). A message
  sent in a window is delivered after it, in the sender's actor order and send
  order. It carries the values of ADR 0124 §7, the engine's one rule for what
  a message may carry, which `game` and `scene` mailboxes share.
- **Shared data**: `SharedTable` — a table of plain values that any VM can
  read. Reads in a window see the value from before the window; writes in a
  window are buffered per actor and applied at the barrier in actor order (the
  last writer by actor order wins); writes in serial apply at once.
  `SharedTable.update(t, key, fn)` for read-modify-write in serial.
- Window A is inside the tick and deterministic by these rules. Window B is at
  render rate and, like `PreRender`, not part of the simulation: what a script
  computes there reaches the world only through serial code.

### 5. The rest of the engine

- GC: each actor VM is stepped by the budget of §3 of the architecture, in its
  own window when it has work there, in serial otherwise.
- Hot reload rebuilds the actor VMs whose scripts changed, at the FrameStart safe
  point.
- The script debugger (built in editor milestone E7) sees actor VMs: a breakpoint in parallel code
  pauses the window.
- F3 and Stats: actors, VMs, and each window's wall time against the serial
  sum of its work (the speedup, measured, not assumed).

## Consequences

- A game uses every core for its own logic: agents, generation, searches.
- A game that uses no `Actor` runs exactly as before: no VM, no window fired.
- Memory per actor: one VM and its modules. The budget keeps it bounded.
- The `ThreadSafety` annotations become load-bearing, and the direction that
  stays compatible is widening.

## Not decided here

- Parallel physics queries from scripts beyond what the audit widens (a raycast
  reads the broadphase; whether that read is safe during a window is the audit's
  finding to record).
- Native code generation for actors; R16 still holds.
