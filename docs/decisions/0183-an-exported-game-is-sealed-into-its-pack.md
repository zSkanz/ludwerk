# 0183 — An exported game is sealed into its pack

- Status: accepted
- Date: 2026-10-06
- Decided by: the owner, who approved the form on 2026-10-04 and on 2026-10-06
  put it first -- he wants to send a game to testers outside without handing
  them its scripts and assets as files -- and the agent, who wrote it down
- Amends: ADR 0045 (a packaged game is a folder), ADR 0104 (what `ludwerk
  build` makes), ADR 0112 (an exported game carries bytecode)

## Context

`ludwerk build` left a game as a folder of its parts. Counted in the game it
was asked for (a horde game, 0.8.22):

- **85 scripts**, each a file of bytecode under the name and the folder its
  source had: `game/src/server/Run/Rules/...`. The structure of the project,
  to browse.
- **`content.manifest.json`**, beside the content pack: the path of every one
  of 825 assets, in clear. The pack itself held hashes and no names -- and
  could not be read without the manifest.
- **The pack stored text as text**: every scene, stamp, material and
  `global.json`, to be read with a text editor or `strings`.
- The catalogues, `project.toml` and `.luaurc`, loose.

On a phone the same folder is in the APK, which is a zip.

What was asked for is not secrecy. The owner's words were that a game should
not be handed over "on a plate"; and three things were declined when the form
was agreed: leaving the server's modules out of a build (in this game any
player hosts), encryption called security (the key ships with the game), and
an obfuscator of the scripts.

## How mature engines do it

A shipped game is a few archives the engine mounts and reads from memory:
Unreal's `.pak` and `.utoc`/`.ucas` (an index of hashed paths, compressed
blocks, a signature that is checked); Unity's asset bundles and its
`data.unity3d`; Godot's `.pck`, which holds scripts, scenes and
`project.godot` alike. Loose files are a development convenience. None of
them claims the archive cannot be opened: tools that open each are a search
away.

## Decision

1. **A game is sealed, unless it says not** (`[export] packed`, true where
   nothing is said; a game that ships its source -- `ship_source` -- is left
   as files unless it also says `packed = true`). Development runs from loose
   files as it did.

2. **One pack holds the game** (`game/.engine/content.lpack`, format 2):
   - its content, as before;
   - **the names its content answers to, each as a hash of the name**
     (`AssetKind::Names`, `PackName`): `asset://textures/base.png` is asked
     for by a script as it always was, hashed, and looked up. No name is in
     the pack, and the manifest that listed them is not shipped;
   - **the game's own files** -- every script under `src/`, the catalogues
     under `i18n/`, `project.toml`, `.luaurc` -- under names of their own
     (`game://src/client/Main.luauc`), and the list of them (`game://`), which
     is what a walk of `src/` asked the disk for.

3. **What is text is deflated** (`BlobCodec::Deflate`): scenes, stamps,
   materials, surfaces, bytecode, catalogues, the settings, the names. Small
   entries always, so that none is left readable for not shrinking; large
   ones only where it saves a twentieth, so a font or a sound is stored as it
   is and read where it lies. With the deflate the engine already carries
   (stb's): no dependency is added. Textures and meshes are compressed in
   their own formats and are stored as they are.

4. **Every entry is filed under the hash of what it holds, and is checked
   against it**: a deflated one when it is first inflated; all of them once,
   on a worker, while a sealed game loads (`Pack::verify`). A game whose pack
   was cut short or changed says its files are damaged and stops
   (`engine.err.game_damaged`), where it would have read as far as it could.

5. **The host reads a sealed game's files out of the pack** (`SealedGame`):
   the scripts a scene's folders hold, what `require` resolves and reads,
   `.luaurc`, the catalogues, and `project.toml` before anything is mounted.
   A script's name, its place in the tree and the line an error names are
   what they were: the list of files is in the pack, deflated with the rest.

6. **`ludwerk build` seals last** (`assetc seal`, the step `seal`): after the
   scripts are compiled and the world partitioned, on the folder a player
   would have been given. Every target: on Android the APK's payload is the
   same folder.

7. **`assetc unseal`** writes a sealed game's files back out. It is how the
   tests look inside, and it is the answer to "can it be opened".

## What it does not do

- **It is not a lock.** The engine is open source and so is this format; the
  tool that opens a sealed game ships with the engine. It keeps a project
  from being handed over as a folder to read, and makes a change to it show.
  Somebody who sets out to take a game apart will. What must not be in a
  game -- a key, a token -- still belongs on a server (ADR 0112).
- It does not encrypt, obfuscate scripts, or leave either side's code out of
  a game that is not dedicated: declined, above.
- **What is streamed from its own files stays beside the pack**: a streamed
  terrain's cells and far ground, the partition cache, streamed chunks. They
  are binary, and are read a file at a time by design (D411).
- No layout that keeps a patch small: entries are in the order of their
  hashes, and a changed asset moves what follows it. A store's delta of the
  whole file is what there is.
- The Export window has no switch for it yet: the key is in `project.toml`.
- The engine's own content beside a game -- shaders, fonts, its catalogue,
  its Luau modules -- is as it was. It is the engine's, and public.

## Consequences

- The game it was asked for: 91 files in `game/` become one, 69.3 MB become
  67.6, and a search of the pack for `asset://`, a script's folder or a
  scene's name finds nothing, where the pack and its manifest had 190 lines
  with `asset://` in them. The same run, sealed and not, logs the same lines.
- A game changes nothing: no key is needed, no script reads differently, and
  saves are not in the game's folder.
- `ContentMounts::packedUrns` answers nothing for a sealed pack: a sealed
  game's content can be asked for by name and not listed.
- A build tree whose `assetc` is older than this cannot seal, and the build
  says so by name.
