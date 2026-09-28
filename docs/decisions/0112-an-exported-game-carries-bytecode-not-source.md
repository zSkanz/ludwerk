# 0112 — An exported game carries bytecode, not source

- Status: accepted (to be built; see `docs/briefs/foundation-kickoff.md`, A3)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving the plan in
  `docs/briefs/game-ready-plan.md`.
- Amends: [0045](0045-a-packaged-game-is-a-folder-that-ships-source.md) (a
  packaged game ships Luau source) and
  [0104](0104-a-game-is-exported-from-one-window-for-windows-linux-and-android.md)
  (the export).

## Context

ADR 0045 shipped source on purpose: at v1 a readable error in a player's hands
was worth more than hiding code, and bytecode had a cost nobody had measured.
Since then a game is exported from one window for three targets, a dedicated
game's two sides carry none of each other's code (ADR 0105), and the owner
wants a game an author can ship without handing its code to anyone who opens
the folder. Every comparable engine compiles or packs scripts before
shipping.

Bytecode is not encryption — a determined person can decompile it — but it
removes the plain-text copy and it loads faster.

## Decision

1. **`engine build` compiles every script to Luau bytecode** with the same
   compiler and the same options the runtime uses (optimization level 2, the
   deterministic `pow` patch of ADR 0083 applied, which the compiler already
   links), and the package carries no `.luau` source.
2. **Debug information stays at level 1**: line numbers survive, so an error in
   a player's log still names the script and the line.
3. **The bytecode version is checked at load.** The player refuses bytecode
   whose version is outside `LBC_VERSION_MIN..LBC_VERSION_TARGET` with a keyed
   error naming both — which only happens when a package is run by a different
   engine build than the one that exported it.
4. **`[export] ship_source = true`** keeps ADR 0045's behaviour, for an author
   who wants it (a modding-friendly game, a tutorial). The default is `false`.
5. **The sentinel test of ADR 0105 extends**: a distinct string in a script's
   source is absent from every exported package in text form.

## Consequences

- The package is smaller and starts faster; source is no longer lying in the
  folder.
- A crash report from a player still has line numbers.
- `engine build` takes the compile time of every script, which is milliseconds.

## Not decided here

- Obfuscation or encryption of bytecode. Neither survives a determined reader,
  and both cost the player load time.
