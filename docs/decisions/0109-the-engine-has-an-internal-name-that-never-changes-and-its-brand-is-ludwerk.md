# 0109 — The engine has an internal name that never changes, and its brand is Ludwerk

- Status: accepted
- Date: 2026-09-26

## Context

A product's name is in more places than anybody counts: namespaces, include
paths, macros, build targets, environment variables, a project's files, file
formats, the command a person types. A name that is in all of them cannot be
changed without touching all of them, and a name is exactly the thing that
sometimes has to change.

**Why the brand is Ludwerk.** It combines Latin *ludus* (game) and German
*werk* (work, workshop): "the game workshop".

## Decision

### 1. Three layers, and only the outer one says the brand

| Layer | Carries | Example |
|---|---|---|
| **The code** | a plain internal name, **`engine`**, that never changes | `namespace engine::scene`, `<engine/scene/world.h>`, `ENG_TR` |
| **A game project** | neutral names: nothing in a project says the brand | `project.toml`, `.engine/`, `@engine/camera`, `"format": "scene"` |
| **The shop window** | the brand, read from **one file** | the `ludwerk` command, the window title, `%LOCALAPPDATA%\Ludwerk`, the icon, the README, the site |

### 2. The code: `engine`, and `ENG_` for macros

- Namespaces are `engine::<module>`; a module's headers are under
  `engine/<module>/include/engine/<module>/` and are included as
  `"engine/..."`.
- Macros, CMake options and variables are `ENG_*` (`ENG_TR`,
  `ENG_ENABLE_REPLICATION`, `ENG_BUILD_ROOT`); CMake targets and functions are
  `engine_*` (`engine_app`, `engine_add_module`).
- The modules a script requires live in `runtime/engine/`.
- **`ENG_` rather than `ENGINE_`.** `ENGINE_*` is OpenSSL's engine API, and a
  TLS stack (ADR 0063) may bring it in one day.
- **i18n keys do not carry the brand.** Message *text* that names the product
  takes it from the brand file (§4).

### 3. A game project: neutral names

- The project file is `project.toml`; a project's cache is `.engine/`.
- A script requires `@engine/<module>`; built-in content is `engine://...`.
- File formats are tagged by what they are: `"scene"`, `"material"`.
- Environment variables are `ENG_*`.
- A surface shader includes `"engine/surface.hlsli"` and declares its
  parameters with `ENG_PARAM`.
- A run writes `engine.log` and `test-report.json`.
- The player template's Java package is `engine.player`.

### 4. The brand: one file

`branding/brand.toml` holds everything that says the product's name:

```toml
name = "Ludwerk"                 # what a person reads
short = "ludwerk"                # the command, file and folder names
tagline = "the game workshop"
domain = "ludwerk.dev"
```

- **CMake reads it** (`configure_file`) for the executables' names, the
  Windows VERSIONINFO, the `.rc` resources and the package folder
  (`Ludwerk-0.0.1-win64`).
- **The engine reads it** for the window title, the About box and the user
  folder (`%LOCALAPPDATA%\Ludwerk`, and the equivalent elsewhere).
- **Messages take it as a placeholder**: `"engine.boot.hello": "{brand}
  {version} -- engine initialized."`.
- **The CLI reads it**: the command is `ludwerk`.
- **The artwork** lives beside it, in `branding/`. A new brand is a new
  `brand.toml`, new artwork, and the prose in the documents and the README.

### 5. What the brand is not in

- **Anything the simulation hashes.** No class, property or enum name contains
  the brand, and `DataModel.EngineVersion` is a host fact (not hashed).
- **The wire protocol's bytes**: nothing on the wire carries the brand.

### 6. The Luau attribution

It goes in the README, the manual's first page and the About box: *"Luau is a
trademark of Roblox Corporation. Ludwerk is an independent project, not
affiliated with or endorsed by Roblox or the Luau team."*

## Consequences

- **A rename is an afternoon**: `brand.toml`, the artwork, the prose.
- **The code reads as what it is**: `engine::scene`, `ENG_TR`,
  `@engine/camera`.
- "A game engine scripted in Luau" is what the README says: a description of
  the language the engine is scripted in, not a name.

### Rejected

- **The brand as the internal name.** It puts a brand in every identifier, and
  pays the whole cost again at the next rename.
- **`ENGINE_` for macros**: OpenSSL's namespace.
- **Neutral names for the shop window too** (a command called `engine`). A
  person installs a product, and the product has a name.
