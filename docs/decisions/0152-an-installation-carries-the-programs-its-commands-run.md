# 0152 — An installation carries the programs its commands run

- Status: accepted (built, 2026-10-02)
- Date: 2026-10-02
- Decided by: ludwerk-08 for the owner, from a game made with `ludwerk new` on
  the packaged engine (finding L1, defect D455): *"the package carries the
  tools it needs and uses its own copies; nothing to install."* The owner, the
  same night, on the one part that was his: StyLua stays out.
- Amends: [0054](0054-the-editor-ships-as-a-folder-and-the-cli-finds-its-own-install.md) (where the CLI looks for
  what it drives), [0109](0109-the-engine-has-an-internal-name-that-never-changes-and-its-brand-is-ludwerk.md)
  (the brand is read from `branding/brand.toml`: an installation must have it).

## Context

`ludwerk check` and `ludwerk fmt` ran two programs by name -- `luau-lsp` and
`stylua` -- and found them on `PATH` or not at all. In the repository a tool
manager puts them there. In a project a person had just made with the packaged
CLI there was no manager and no manifest for one, and the first `check` said
*"luau-lsp is not on PATH. Run rokit install."*: the name of a program they had
never installed, for a file their project did not have.

The same package called itself "engine" in its own usage text: the brand is
read from `branding/brand.toml`, and the package did not carry the file.

## Decision

1. **The CLI runs the installation's own copy of a program first**
   (`tools/cli/tools.luau`): `<installation>/tools/bin/<program>`, then the
   name on `PATH`. In the repository there is no `tools/bin`, so nothing
   changes there.
2. **The package carries its analyser.** `luau-lsp`, MIT, at the version
   `rokit.toml` pins -- the one the engine's definitions were generated for --
   copied from where the tool manager keeps the real program, and run once to
   prove the copy is one. Its licence text goes with it, and Lute's, which the
   package already shipped without (`licenses/`; the texts are kept in
   `third_party/licenses/`).
3. **The package does not carry StyLua.** It is MPL-2.0, which is not one of
   the licences R6 lets the engine ship under; an exception is the owner's to
   make and he chose not to. So formatting is the one thing a person installs:
   `check` checks the types, says in one line that the formatting was not
   checked, and passes; `fmt` says where to get the formatter and where to put
   it (`PATH`, or the installation's `tools/bin`).
4. **The package carries `branding/brand.toml`**, and `ludwerk new` no longer
   tells anybody to run a tool manager.

## Consequences

- A project made a minute ago, on a machine with nothing else on it, passes its
  first `check`.
- The package is 12 MiB larger.
- A person who wants their code formatted downloads one program. If the owner
  later allows MPL-2.0 for a tool that is shipped beside the engine and never
  linked into it, carrying it is one line in `tools/repo/package.luau`.

## Not decided here

- A formatter of the engine's own.
- Packages for Linux and macOS carrying the same: the code is the same, and the
  release jobs have to be seen doing it.
