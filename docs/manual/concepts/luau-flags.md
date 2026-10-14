# Luau language settings

Luau adds features behind **flags**: switches that stay off until a change has
been through Luau's own release process. A project can turn one on early. The
setting is in `project.toml`, and the engine reads it in every place a script
is read, so the game, the editor and your other tools agree on what a script
means.

## Turning a feature on

The editor shows the features by name in **Project Settings > Luau**:

| Feature | What it lets a script say |
|---|---|
| `if local` | `if local x = f() then ... end`: a name that holds a value only where it is not nil. |
| Exported values | `export local`, `export function` and `export const` in a ModuleScript, in place of returning a table. |

A ticked feature is written to the project file, under its flags:

```toml
[luau.flags]
LuauExperimentalIfLocalSyntax = true
LuauExperimentalIfLocalAnalysis = true
```

You can write the same lines by hand. A flag is either `true` or `false`, or a
whole number, as Luau defines it.

## Where the settings reach

The same values reach all of these:

- **The game.** Luau's compiler and VM read them in the editor's Play, under
  `ludwerk dev`, in a server and in an exported game. An export compiles its
  scripts with them.
- **The editor.** The script editor's checker reads them. When you change one in
  Project Settings, every open tab is checked again at once, and the game runs
  with it from the next Play.
- **`ludwerk check` and `--check-scripts`.** `ludwerk check` asks the engine for
  the values and passes each one to luau-lsp.
- **VS Code.** The editor and `ludwerk dev` keep `luau-lsp.fflags.override` in
  the project's `.vscode/settings.json` up to date. They change only that key,
  and they add `luau-lsp.fflags.enableByDefault` if it is not there. Comments
  and other settings in the file stay as they are.

**luau-lsp turns on every flag unless it is told not to.** Before this setting,
VS Code and `ludwerk check` accepted `if local`, and the game then refused to
run it. Now every parser flag is passed to luau-lsp with the value the game
uses, so the two give the same answer.

## Every flag

Under **Project Settings > Luau**, **Advanced: every Luau flag** lists every
flag this engine's Luau has. Each row shows its value, Luau's default and the
part of Luau that reads it:

| Read by | What a change does |
|---|---|
| Parser | Changes what a script can say. |
| Compiler, VM, Native code | Changes what a script compiles to, or how it runs. |
| Checker | Changes only what the script editor and `ludwerk check` report. |
| Other | Changes how `require` and Luau's configuration are read. |

Two kinds of flag are not offered, and the engine ignores them in
`project.toml`:

- **`Debug*` flags** are Luau's own instrumentation, for working on Luau itself.
- **`LuauSolverV2`** chooses the type solver. The engine always checks scripts
  with the new solver.

Set a flag from this list only when you have a reason you can name. A flag is a
change that Luau has not finished. It can break scripts, change what they do,
or stop the game.

## Multiplayer and replays

A flag that the parser, the compiler or the VM reads changes the program
itself: the same source can parse, compile or run another way. So:

- **A player whose game has other values cannot join.** The server refuses the
  player and says why, in `JoinFailed` and in its own log. Values that only the
  checker reads do not matter here.
- **A replay recorded with other values is refused**, not compared. A trace
  recorded with Luau's defaults stays exactly as it was.

## When Luau is upgraded

A new Luau version can remove a flag, because its change is now part of Luau.
A project that still names that flag gets a warning at every start that says to
take it out of `[luau.flags]`. The feature is still there. It is simply no
longer behind a switch.
