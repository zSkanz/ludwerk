# The asset pipeline

Source art lives in the project's content directory. `ludwerk build-assets`
compiles it into a **pack** plus a **manifest**, and the engine mounts both.

```bash
ludwerk build-assets
ludwerk build-assets --verify
```

In development you do not have to run it at all: the loose content directory is
mounted too, and a later mount wins, so an ordinary PNG dropped into
`content/textures/` is picked up with no build step. The pipeline is what a
shipped game gets.

## What is compiled, and to what

| Source | Becomes |
|---|---|
| `.gltf`, `.glb` | The engine's mesh format, with levels of detail |
| `.fbx`, `.obj`, `.dae`, `.ply`, `.stl` | The same, via an offline converter |
| `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp` | The engine's texture container |
| `*.chunk.json` | A compiled chunk, beside the pack, plus a chunk index |
| everything else | Copied through untouched |

**glTF is the canonical format** and stays on its own importer even though the
converter could read it: two importers for one format is two behaviors for one
format.

A font, a JSON file or a shader blob falls into "copied through" and is mounted
as-is.

## Where the output goes

```text
<project>/.engine/content.lpack             the pack
<project>/.engine/content.manifest.json     what is in it
<project>/.engine/content/**.lchunk         compiled chunks
<project>/.engine/content.chunks.json       the chunk index
```

`.engine/` is generated state and is gitignored. Chunks live **outside** the pack
deliberately: a pack is read whole at mount, and a world bigger than memory
cannot have its instance lists resident.

## The manifest

```json
{"format":"content-manifest","version":1,
 "assets":[{"urn":"asset://models/tree.glb","hash":"…","kind":"mesh",
            "bytes":48213,"lods":3,"vertices":1204,"meshlets":11}]}
```

Sorted by URN, with mesh statistics where they apply. It is the file to read
when you want to know why a build is the size it is.

## Determinism

The same inputs produce **byte-identical** output. Four rules make that true:

1. Inputs are sorted by path before anything is processed.
2. Every encoder parameter is pinned in the tool rather than defaulted.
3. No timestamp, absolute path or machine name reaches the output, and a URN
   uses forward slashes on every host.
4. Pictures are encoded side by side, one to a worker, and merged in the order
   of their paths; the encoder's own threading stays off, because its format
   settings are process-global. A build with `--jobs 1` is the same bytes.

`--verify` is that guarantee as a flag you can type: it builds twice and
compares. If the two differ, something in the pipeline is not deterministic and
the build fails rather than shipping a lottery.

## What it remembers, and what it does not do

**What was compiled is remembered, where the editor keeps it.** The editor
compiles into the project's `.engine/import/cache`, and an export
(`ludwerk build`) reads the same cache: a model or a picture the editor already
compiled is not compiled again, and the export says how many it was given. An
entry is keyed by the source's bytes, its name, the compiler's options and rules
and the mesh format it writes, so an answer from it is the answer a compile
gives. `ludwerk build-assets` on its own compiles everything.

There is also no import-settings file: an asset is compiled the one way the
pipeline compiles that kind of asset. Options that would need per-asset settings
— compression choices, LOD counts — are the tool's decisions today.

## Where to look next

- [Content and asset URNs](manual:assets/content)
- [Streaming a large world](manual:assets/streaming) — where chunks come in
- [Shipping a game](manual:guides/shipping)
