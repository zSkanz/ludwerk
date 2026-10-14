# 0217 — Luau's flags are a project setting

- Status: accepted; built
- Date: 2026-10-14
- Amends: ADR 0093 (the editor's checker, and the flags it turns on)
- Builds on: ADR 0002 (the Luau pin), ADR 0216 (the upgrade's checklist),
  ADR 0085 (a joiner is refused by name), ADR 0083 (replays and their traces)

## The question

Luau ships each change to its language, its compiler and its VM behind a flag,
and leaves the flag off until the change has gone through Luau's own release
process. Some of those changes are features people ask for, such as
`if local`. A project could not turn one on. Worse, the places that read a
script did not agree about them:

- The **runtime** ran with Luau's defaults, where `if local` is off.
- **luau-lsp** turns on every flag it is not told about. So VS Code and
  `ludwerk check` accepted `if local` in a script the game then refused to run.
  Checked against the vendored luau-lsp 1.70.1: `luau-lsp analyze` passed the
  script, and the engine's compiler refused it.
- The **editor's checker** turned on Luau's checker flags and nothing else
  (ADR 0093, amended 2026-10-06).

## Decision

### One set of values, every reader

`project.toml` holds them:

```toml
[luau.flags]
LuauExperimentalIfLocalSyntax = true
LuauTypeInferIterationLimit = 4000
```

Each flag is `true`, `false`, or a whole number, as Luau defines it. The
engine applies them once per process, **before the first script is compiled**:
in every run of a project, in `--check-scripts`, in `--check-sides`, and in
the export's compile steps (`ludwerk build` now passes the project root to
`--compile-scripts` and `--compile-content-scripts`). One process holds one
project (ADR 0055). From there the values reach:

- **the runtime's** parser, compiler and VM, and hot reload, which compiles
  with the same flags;
- **the editor's checker**, which applies the engine's defaults first, then the
  project's values;
- **`ludwerk check`**, which asks the engine with `--print-luau-flags` and
  passes each value to luau-lsp as `--flag:Name=value`;
- **VS Code**, through `luau-lsp.fflags.override` in the project's
  `.vscode/settings.json`. The editor and `ludwerk dev` write that key, and
  add `luau-lsp.fflags.enableByDefault` if it is missing. The file is edited
  in place, so its comments, order and spacing stay as they were. A file that
  is missing is not created.

**What a tool is told** is every parser flag at the value the engine runs it
with, plus every other flag the project changed. It must include the parser
flags even when the project changed none of them, because luau-lsp turns on
anything it is not told about.

**The engine's default** is Luau's own default for each flag, except the
checker's flags. Those are on, as Luau's own analyser runs them, apart from
the few Luau marks as experimental (ADR 0093, unchanged).

### Which library reads a flag

`cmake/engine_luau_flags.cmake` reads every flag definition from the vendored
sources and files it under the library that defines it:

| Library | Area |
|---|---|
| `Ast` | Parser |
| `Compiler`, `Bytecode` | Compiler |
| `VM` | VM |
| `CodeGen`, `Inliner` | Native code |
| `Analysis`, `EqSat` | Checker |
| `Config`, `Require`, `Common` | Other |

The library that **defines** a flag is the only one that can read it: a lower
library does not link a higher one, and a shipped game links no checker at all
(ADR 0002). The lists are generated from the sources at the pin, so they move
with the pin and nobody has to maintain them.

### What a project may not set, and why

- **`Debug*` flags are hidden and refused.** They are Luau's instrumentation for
  working on Luau itself: time tracing, logging, and paths that assert or crash
  on purpose. They are never a feature a game wants. One left on by accident
  in a shipped game slows it down, or ends it.
- **`LuauSolverV2` is hidden and refused.** It chooses the type solver, and R2
  says every script is checked by the new one. It is defined beside the
  parser's flags, so without this rule it would go to luau-lsp as `false` and
  switch the new solver off there.
- **Flags that change compiled code are shown and marked, not hidden.** These
  are the parser's, the compiler's, the VM's, native code's and `require`'s.
  They are the flags a project has a reason to set, and they are also the ones
  every machine must agree on (below). Hiding them would hide the features.

### Matches and replays

**Fingerprint.** The values that change compiled code reduce to one number: an
FNV-1a hash of `name=value;` for each flag that differs from the engine's
default, in name order. The number is 0 when nothing differs. Changes to the
checker's flags never move it, because they do not change what a game runs.

- **A match.** The fingerprint is part of the hello, protocol 53. When a
  joiner's fingerprint differs from the authority's, the authority refuses the
  joiner with `Refused` reason 5 (`RefusedLuauFlags`), which `JoinFailed` puts
  into words. A world compiled another way drifts apart from the first tick, so
  letting the joiner in to drift would be worse than saying no.
- **A replay.** A trace records the fingerprint in a `luau-flags` line, but
  only when it is not 0. A trace recorded with the engine's defaults is the
  same bytes it always was. A run with a different fingerprint is refused, not
  compared.

### Project Settings > Luau

- **Features by name.** `if local` (`LuauExperimentalIfLocalSyntax` and
  `LuauExperimentalIfLocalAnalysis`) and exported values
  (`LuauExportValueSyntax`). Each is a checkbox with a description, and it is
  on when all its flags are on. A feature this Luau does not carry is not
  shown. **Exact tables have no flag at 0.742**, so the list does not offer
  them. Looking for new features is step 6 of every upgrade's checklist
  (ADR 0216).
- **Advanced: every Luau flag.** A searchable table, with a warning above it.
  Each row has the flag, its value, the engine's default, the area that reads
  it, and a tooltip that says whether a change reaches compiled code. A
  changed value is drawn in the accent colour and has a Reset button.
- **Apply** writes each value that differs from the default, and removes each
  value the file had that no longer differs. A value equal to the default is
  never written, because pinning it would stop it following future defaults.
  A flag name this Luau does not have stays in the file untouched: it belongs
  to the file, and the engine reports it at every start.
- **Then the editor loop** stops the checker's thread before Luau's flags
  change under it, applies the new values, updates VS Code's settings, and
  checks every open tab again. The game runs with the new values from the next
  Play.

## Consequences

- One rule now holds: a script VS Code calls clean is a script the game runs.
- An upgrade that graduates a flag removes it from Luau. A project that still
  names it gets a warning at every start that says to take it out of
  `[luau.flags]`. Nothing else breaks.
- Protocol 53: a build before this one cannot join a server from this one, or
  the other way round, as with any protocol change.
- Tests: the same `if local` script in both states, through the runtime's
  compiler, the editor's tab and the checker. Reading `[luau.flags]`, and the
  values the engine refuses. Writing the values back as Project Settings does.
  The fingerprint, and what moves it. VS Code's settings edited in place. A
  trace with and without the line. A joiner refused for a different
  fingerprint. `ludwerk check` was also run end to end with luau-lsp 1.70.1 on
  an `if local` project, once with the flags off (refused) and once with them
  on (clean).
