# 0109 — The engine has an internal name that never changes, and its brand is Ludwerk

- Status: accepted (to be carried out after the ledger of ADRs 0104 to 0106,
  and before the one of ADR 0107; see `docs/briefs/rename-kickoff.md`)
- Date: 2026-09-26
- Decided by: the owner, on 2026-09-26, in conversation. He chose the brand
  **Ludwerk** (*"Ludwerk gostei vamos nesse"*), then asked that the code carry
  a plain internal name instead of the brand, so that the brand could change
  again without touching it (*"o ideal não seria colocarmos um nome interno
  fácil tipo engine porque se precisarmos alterar no futuro fica fácil ai
  Ludwerk seria tipo uma marca sobre a engine"*). While it was being built he
  added two more: **no compatibility with the old names** (*"projetos antigos
  não precisa adicionar compatibilidade"*), and **the versions restart at
  0.0.1** (*"não faz sentido uma engine que nem lançada ainda foi estar na
  versão 3 ou 2"*).
- Amends: every ADR that names `luaug` in an identifier the user sees --
  notably 0107 (`@luaug/views` becomes `@engine/views`) and 0108
  (`luaug mcp`, `luaug ai run`, `luaug://` become the brand's command and
  `engine://`). Their text is not rewritten; this ADR is the mapping.

## Context

**Why the name changes.** Roblox published brand guidelines for Luau on
2026-08-18 (<https://luau.org/brand/>):

- *"'Luau' and its logo are trademarks of Roblox Corporation."* It is not a
  registered mark at the USPTO (checked 2026-09-26: 126 LUAU records, none
  Roblox's), but it is claimed, and the guidelines are enforced by Roblox.
- An independent project should not use *"Luau followed by a generic product
  or technology category"* ("Luau Runtime", "Luau SDK", "Luau Studio"). The
  same goes for acronyms and for domains like `luau[product].com`.
- *"Using the Luau name in a repository, package, module, or other technical
  identifier is generally fine when it is descriptive and the surrounding
  project has a clear independent identity."*
- Every project that uses the name should say: *"Luau is a trademark of Roblox
  Corporation."*

"LuauG" reads as *Luau Game* -- close to the pattern the guidelines name --
and `luaug.com` is the domain pattern they name. The cheapest moment to change
is before a community, released games and links exist.

**Why the brand is Ludwerk.** It combines Latin *ludus* (game) and German
*werk* (work, workshop): "the game workshop". On 2026-09-26 `ludwerk.com` and
`ludwerk.dev` were unregistered, GitHub had no repository by the name, and the
USPTO had no LUDWERK mark. INPI (Brazil) and EUIPO were not checked. The
names considered and why each was dropped are in the conversation's record:
the common words were all taken, and Ludex, Ludeon and Ludris were each
already someone's.

**Why the code gets its own name.** The brand is in **about 9,800 places** in
1,087 files (`luaug` 7,158, `LUAUG` 2,331, `LuauG` 356), and 289 paths
contain it: `include/luaug/` in every module, the `LUAUG_*` macros, CMake
options and environment variables, `luaug_*` targets, `luaug.toml`, `.luaug/`,
`@luaug/*`, the `"luaug-scene"` format and the branding files. A rename that
touches all of that once is a project. If the code keeps no brand, the next
rename does not touch any of it. The same split is common elsewhere: a
browser whose code is called one thing and whose product is called another.

## Decision

### 1. Three layers, and only the outer one says the brand

| Layer | Carries | Example |
|---|---|---|
| **The code** | a plain internal name, **`engine`**, that never changes | `namespace engine::scene`, `<engine/scene/world.h>`, `ENG_TR` |
| **A game project** | neutral names: nothing in a project says the brand | `project.toml`, `.engine/`, `@engine/camera`, `"format": "scene"` |
| **The shop window** | the brand, read from **one file** | the `ludwerk` command, the window title, `%LOCALAPPDATA%\Ludwerk`, the icon, the README, the site |

### 2. The code: `engine`, and `ENG_` for macros

| Now | Becomes |
|---|---|
| `namespace luaug::<module>` | `namespace engine::<module>` |
| `engine/<module>/include/luaug/<module>/` | `engine/<module>/include/engine/<module>/` |
| `#include "luaug/..."` | `#include "engine/..."` |
| `LUAUG_*` macros, CMake options and variables (`LUAUG_TR`, `LUAUG_ENABLE_REPLICATION`, `LUAUG_BUILD_ROOT`, ...) | `ENG_*` (`ENG_TR`, `ENG_ENABLE_REPLICATION`, `ENG_BUILD_ROOT`, ...) |
| `luaug_*` CMake targets and functions (`luaug_app`, `luaug_add_module`) | `engine_*` (`engine_app`, `engine_add_module`) |
| `runtime/luaug/` (the `@luaug` modules' source) | `runtime/engine/` |

- **`ENG_` rather than `ENGINE_`.** `ENGINE_*` is OpenSSL's engine API, and a
  TLS stack (ADR 0063) may bring it in one day.
- **Binary magics stay as they are** (`"LGVC"` and the like): they are bytes in
  files that already exist, not names anybody reads.
- **i18n keys do not change.** None contains the brand; message *text* that
  names the product takes it from the brand file (§4).

### 3. A game project: neutral names, and no compatibility

| Now | Becomes |
|---|---|
| `luaug.toml` | `project.toml` |
| `.luaug/` (a project's cache) | `.engine/` |
| `require("@luaug/camera")` | `require("@engine/camera")` |
| file-format tags `"luaug-scene"`, `"luaug-material"`, ... | `"scene"`, `"material"`, ... |
| built-in content `luaug://primitive/block` | `engine://primitive/block` |
| `LUAUG_*` environment variables | `ENG_*` |
| the surface-shader contract, `#include "luaug/surface.hlsli"`, `LUAUG_PARAM` | `#include "engine/surface.hlsli"`, `ENG_PARAM` |
| `luaug-test-report.json`, `luaug.log` | `test-report.json`, `engine.log` |

- **The old names are not read.** The engine is unreleased under its brand and
  its versions restart (§7), so there is no installed base to carry. Every
  project, scene, material and shader in the repository is renamed by the
  rename's script; one made elsewhere before the rename is renamed by hand,
  which the CHANGELOG's mapping table says how to do.
- **Android identifiers.** The example projects' ids (`dev.luaug.*`) become
  `dev.ludwerk.*`, which installs as a new app on a phone. The player
  template's Java package (`org.luaug.player`) becomes `engine.player`,
  internal and neutral.
- **The one old name that survives** is `luaug_dpow`, a C symbol the vendored,
  patched Luau calls (third_party/patches/luau/0001). Renaming it means
  re-vendoring Luau; the brand lint lists it as its only exception.

### 4. The brand: one file

`branding/brand.toml` holds everything that says the product's name:

```toml
name = "Ludwerk"                 # what a person reads
short = "ludwerk"                # the command, file and folder names
tagline = "the game workshop"
domain = "ludwerk.dev"
previous = ["LuauG"]             # names it had before (a record; nothing migrates from them)
```

- **CMake reads it** (`configure_file`) for the executables' names, the
  Windows VERSIONINFO, the `.rc` resources, the installer and the package folder
  (`Ludwerk-0.0.1-windows`).
- **The engine reads it** for the window title, the splash, the About box and
  the user folder (`%LOCALAPPDATA%\Ludwerk`, and the equivalent elsewhere).
  A folder under the old name is not moved: the toolchains and keys in it are
  installed again where the new one expects them.
- **Messages take it as a placeholder**: `"engine.boot.hello": "{brand}
  {version} -- engine initialized."`. There are 8 such messages in the engine
  catalog and 7 in the CLI's.
- **The CLI reads it**: the command is `ludwerk`.
- **The artwork** lives beside it, in `branding/`. A new brand is a new
  `brand.toml`, new artwork, and the prose in docs and the README. That is the
  whole of a future rename, and a test proves it: build with a throwaway
  `brand.toml` and check that nothing says Ludwerk.

### 5. What does not change

- **History**: ADRs, the CHANGELOG's released sections, `docs/progress-archive`,
  `docs/research` and `docs/defects.md`. They say what was true when they were
  written, LuauG included. Git tags stay as they are.
- **Anything the simulation hashes.** No class, property or enum name contains
  the brand, and `DataModel.EngineVersion` is a host fact (not hashed). **No
  determinism trace moves.**
- **The wire protocol's bytes**: nothing on the wire carries the brand. The
  generated `docs/protocol/wire.md` changes only in its title.

### 6. Guarding it

- **A brand lint** in `scripts/gates/docs-lint.sh`: `luaug`, `LuauG` and
  `LUAUG` must not appear in `engine/`, `runtime/`, `tools/`, `api/`,
  `templates/`, `examples/` or `scripts/`. The only exception is
  `luaug_dpow` (§3). The brand name itself is not spelled in code as data:
  code reads `brand.toml`.
- **The Luau attribution** goes in the README, the manual's first page and
  the About box: *"Luau is a trademark of Roblox Corporation. Ludwerk is an
  independent project, not affiliated with or endorsed by Roblox or the Luau
  team."*

### 7. The version restarts at 0.0.1, and what is outside the repository

- **Ludwerk's first version is 0.0.1.** The releases as LuauG (1.0.0, 1.1.0,
  2.0.0) stay in the CHANGELOG as its history, under a heading that says so,
  and their git tags stay where they are; the next release is Ludwerk 0.0.1,
  tagged `v0.0.1`. Versions below 1.0 say what is true: the engine has not
  been released to anyone yet. Tagging it is the owner's.
- **The owner does, outside the repository**:
  - register `ludwerk.com` and `ludwerk.dev`;
  - check INPI and EUIPO;
  - rename the GitHub repository `zSkanz/LuauG` to `zSkanz/ludwerk` (GitHub
    redirects the old links);
  - optionally rename the local folder.

## Consequences

- **The next rename is an afternoon**: `brand.toml`, the artwork, the prose.
- **The code reads as what it is**: `engine::scene`, `ENG_TR`, `@engine/camera`.
- **The name no longer leans on Luau**; "a game engine scripted in Luau" is
  still what the README says, which the guidelines allow as description.
- **One large mechanical change**, done once, by a script, in stages, each
  behind the full gate. It needs a quiet tree: no other session with work in
  progress while it lands.
- New artwork is needed: the current lockups spell "LuauG".

### Rejected

- **Renaming everything to `ludwerk`.** It pays the same cost again at the next
  rename, and puts a brand in every identifier.
- **Keeping `luaug` as the internal name.** The guidelines tolerate a
  descriptive technical identifier, but "luaug" is not descriptive -- it is the
  old brand -- and it would keep the Luau question open in every file.
- **`ENGINE_` for macros**: OpenSSL's namespace.
- **Neutral names for the shop window too** (a command called `engine`). A
  person installs a product, and the product has a name.
