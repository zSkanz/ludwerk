# The rename: the kickoff and the ledger

The owner, on 2026-09-26, chose the brand **Ludwerk** and asked that the code
carry a plain internal name, so that the brand can change again without
touching it. The decision is
[ADR 0109](../decisions/0109-the-engine-has-an-internal-name-that-never-changes-and-its-brand-is-ludwerk.md).
**Read it first** -- its tables are the mapping this ledger carried out.

While it was being built, the owner settled three more things:

- **No compatibility with the old names** (2026-09-26): nothing reads
  `luaug.toml`, `.luaug/`, `@luaug/`, `"luaug-scene"` or `LUAUG_*`. Everything
  in the repository was renamed by the script; anything made elsewhere is
  renamed by hand (the CHANGELOG's *Unreleased* section says how).
- **The versions restart at 0.0.1** (2026-09-26). LuauG's 1.0.0, 1.1.0 and
  2.0.0 stay in the CHANGELOG under *Before Ludwerk*.
- **A new repository, starting from one commit** (2026-09-27): Ludwerk is
  published as `zSkanz/ludwerk`, public, whose first commit is this tree. The
  old `zSkanz/LuauG` is kept, and made private when the owner confirms.

## The artwork

The World Core identity -- the hexagon of three blocks and the Ludwerk
wordmark, in graphite, deep teal, mint and paper -- was drawn by the owner
with ChatGPT and delivered on 2026-09-26 under neutral names in `branding/`
(`mark.svg`, `icon.ico`, `lockup-*.png`, `logo-512.png`, `app-icon-512.png`,
`social-card.png`). [Review and exact prompt](../../art/branding/orbit/STYLE.md).
The rename's R4 removed the old `branding/luaug-*` copies, each byte-identical to
its new name. The editor's drawn mark (`debug_overlay.cpp`) is the same emblem.

## How it was done

**By scripts, in `tools/repo/rename/`**, each idempotent (a second run changes
nothing) and each followed by the full gate. They are kept in the tree as the
record of what changed and how:

| Stage | Script | What it does |
|---|---|---|
| R0 | `r0_brand.py` | `branding/brand.toml`; CMake reads it into a generated `engine/core/brand.h` (`kBrandName`, `kBrandShort`, `ENG_BRAND_NAME`); the engine's and the CLI's catalogs fill `{brand}`, `{brandShort}` and `{tool}` themselves |
| R1 | `rename.py --stage code` | `luaug::` to `engine::`, `include/luaug/` and `interop/luaug/` to `engine/`, the shaders' include root |
| R2 | `rename.py --stage build` | `LUAUG_*` to `ENG_*`, `luaug_*` to `engine_*`, `luaug-*` executables and containers to `engine-*`, internal CamelCase names, the surface parser's macro lengths, Gradle's property names |
| R3 | `rename.py --stage project` | `project.toml`, `.engine/`, `@engine/`, `engine://`, bare file-format tags (`"scene"`, `"material"`...), `engine/surface.hlsli` and `ENG_PARAM`, the Android player's Java package `engine.player`, every example and test project |
| R4 | `r4_brand.py` | `project(Engine VERSION 0.0.1)`; `scripts/ludwerk.*`; generic branding file names; the user folder, the window title, the About box, the package folder, the documentation site's name and the protocol's title from the brand; prose says Ludwerk |
| R5 | `r5_docs.py` | the current documents say Ludwerk; the CHANGELOG's *Before Ludwerk*; the brand lint fails on the old name in code |

## What held

- [x] **Nothing in code names the old brand** except `luaug_dpow`, the C
      symbol the vendored, patched Luau calls (third_party/patches/luau/0001);
      the brand lint in `docs-lint.sh` fails on anything else.
- [x] **A rebrand is one file**: a message, a title, a folder or a package that
      says the product's name reads `branding/brand.toml`.
- [x] **Determinism**: every scenario reproduces. Two traces were re-recorded
      -- `example01` and `ragdoll` -- because a script's source is hashed and
      theirs changed text (a comment, and `require("@engine/ragdoll")`); the
      simulation did not change.
- [x] The full `scripts/localgate.ps1` on the renamed tree, Linux and Android
      included.
- [x] History untouched: ADRs, closed briefs, the progress archive, research,
      the defect register and the CHANGELOG's released sections still say
      LuauG.

## What is left, and whose

- [>] **The owner**: register `ludwerk.com` and `ludwerk.dev`; check INPI and
      EUIPO for LUDWERK; confirm making `zSkanz/LuauG` private.
- [x] Machines set up before the rename: set `ENG_BUILD_ROOT` (bootstrap does),
      and run `scripts/install-android.ps1` again -- the toolchains live under
      the user's Ludwerk folder now.
- [>] Release 0.0.1 when the owner says: tag `v0.0.1`, release notes, package.

## Findings

- **A new file a stage creates is invisible to the next stage** when the
  script reads only what git tracks: the brand header and the CLI's brand
  reader kept the old names until the scripts read untracked files too.
- **`.gitignore` names the cache**: renaming `.luaug/` to `.engine/` there made
  every old `.luaug/` folder visible to `git add -A`, and 707 stale cache files
  were committed once before being taken out. Clear a machine's old caches, or
  stage paths explicitly, before committing a rename of an ignored name.
- **A standalone `"luaug"` in quotes is a folder, a module or a log tag** --
  `"runtime" / "luaug"` -- and becomes `engine`, not the brand.
- **Fixed offsets hide names**: the surface-shader parser skipped
  `LUAUG_PARAM` by a literal 11; renaming the macro broke parsing until it
  asked the macro's length.
- **A script's source is part of the world hash**, so renaming text in a
  replayed script moves its trace at tick 0 with nothing else changed.
