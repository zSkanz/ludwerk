# 0187 — A run says how the one before it ended, and where its report is

- Status: accepted
- Date: 2026-10-06
- Decided by: the coordinator, for a game about to ship on a store and on a
  phone, in the shape "one call at start that says whether the last run ended
  by a crash or by an unhandled script error and where its report is; nothing
  uploaded and nothing shown by the engine"; shaped by the agent
- Builds on: `architecture.md` §app (the crash handler), audit A15 (the log of
  the run before is kept), D569 (a phone's log is in the game's external
  folder), ADR 0019 (errors are keys)

## Context

The engine has had a crash handler since M0: a dump, a readable note with a
stack, beside the log, their paths printed at start. All of it was built for
one reader -- the engine's developer, at the machine that crashed.

A game that ships has another: a player, on a machine nobody who can fix
anything will ever see. The game dies; the player starts it again; and the
game that just started knows nothing. The log of the run that died is there,
rotated to `engine.previous.log`, and the note is there, named after a process
number -- and nothing tells the game either exists, so it cannot ask for them.
Worse for a script error nothing caught: the game went on running, half of it
not working, and the only trace is a line in a file.

## How mature engines do it

The engines this one follows keep a crash folder and, on the next start,
offer its newest entry to a reporter: one asks "did the last run crash" of
the platform (and exposes the last report to script), the other writes a
session marker at start and clears it at a clean exit, which is how its
reporter tells a crash from an exit without one. Mobile systems keep the same
fact themselves -- why the app's last process ended -- because an app killed
in the background and an app that faulted look the same from inside the next
run. All of them separate **learning that it happened** (the engine's) from
**what to do about it** (the game's, or a service the game chose).

## Decision

1. **Every run keeps a record beside its log**, named after it (`engine.log`
   keeps `engine.run`): that it is running, its process number, when it
   began, what game and engine it is, and how many script errors nothing
   caught. Written at boot; written again as clean when `main` returns --
   whatever it returns. Written whole to a second file and renamed over, so a
   run that dies writing it leaves the record it had.

2. **The next run reads it before writing its own**, and what it found is
   `core::lastRun()`:

   - no record: `None`;
   - it says clean: `Clean`;
   - it still says running, and the crash handler's note for that process is
     there and was written after that run began: `Crashed`;
   - it still says running and there is no such note: `Unfinished` -- ended
     from outside, by the system, the player or the power.

   **`Unfinished` is not `Crashed`, and the difference is the point.** A phone
   ends a game in the background whenever it wants the memory; a player ends a
   task. A game that called those crashes would ask for a report every
   morning. The crash handler is the only witness to a fault, so its note is
   the test -- and a note under a reused process number, written before the
   run began, is somebody else's.

3. **A report is written for a run that left something to report** --
   crashed, unfinished, or with a script error -- as one text file beside the
   log, `engine.last-run.txt`: what the game, the engine and the platform
   were, how it ended, the count of script errors and the first of them, the
   crash note whole, and the last 256 KB of that run's log. One file, of text,
   small enough for any way a player has of sending anything. A run that ended
   clean with no error has none, and the one before it is removed: an old
   report beside a good run would be sent as if it were about it. The dump,
   where the platform writes one, stays beside it and is named.

   The report is in English and not the catalog's (R3 is about what a player
   reads): it is read by the game's developer, as the log is.

4. **A script error nothing caught is counted** where every such error
   already passes -- the script module's report of it, at error level -- and
   the first is kept. The record is written at the first, so a run that dies a
   moment later still has it, and no more than once a second after: a script
   failing every frame must not write a file every frame.

5. **`RunService:GetLastRun()`** returns `{ Outcome: Enum.RunOutcome,
   ScriptErrors: number, FirstScriptError: string?, Report: string?, Dump:
   string? }`. A method and not properties: it is one machine's fact, read
   once, and nothing of it is replicated, saved or hashed (R10).

6. **Nothing is uploaded and nothing is shown.** What a game does with the
   path -- show it, ask for the file, hand it to a service of its own choosing
   through `@std/net` -- is the game's, and a privacy decision the engine has
   no standing to make for it.

7. **Where the file is.** Beside the log: the folder the game runs from;
   the game's own folder under the user's application data
   (`<company>/<game>/logs`, beside its saves) when that cannot be written --
   a protected Desktop, Program Files (D595); and on a phone the game's own
   external folder (`Android/data/<package>/files`, D569), which a cable or
   the system's file browser opens with no permission asked.

8. **`--fault-on-purpose`** makes the host die of a real fault once the crash
   handler is in. It is the instrument of the gate that checks all this: a
   crash cannot be checked from inside the process that has it.

## What it does not do

- **No share sheet, no copy to a public folder.** On the newest phones a
  third-party file manager cannot open another app's folder; the system's own
  can, and so can a cable. Handing the file to the system's share sheet needs
  a content provider in the player's package and is the next step if a
  shipped game finds its players cannot reach the file. Said here so it is
  not discovered.
- **No stack for a phone's crash.** The handler's note on a phone names the
  signal and nothing else; a readable stack there is the handler's work, not
  this record's.
- **Not a count of crashes.** One run back. A game that wants a history keeps
  it in its own save.

## Consequences

- A file more beside every log, a few hundred bytes, written twice a run.
- `Enum.RunOutcome` and one method join the API; a game that does not ask
  pays the file and nothing else.
- Tests: the record's rules in `core/tests/run_record_tests.cpp` (first run,
  clean, crashed, unfinished, a stale note, script errors, the report replaced,
  a second run from one folder), and `last_run_gate`, which runs the host six
  times against one log -- none, a script error, clean with its report, a
  real fault, crashed with the handler's note in its report, clean again with
  the report gone. The second is what checks that three files written by
  three pieces of the engine are found under the names the next run looks for.
