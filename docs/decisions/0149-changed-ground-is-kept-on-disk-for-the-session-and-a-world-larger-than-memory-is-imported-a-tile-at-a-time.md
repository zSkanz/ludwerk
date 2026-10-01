# 0149 — Changed ground is kept on disk for the session, and a world larger than memory is imported a tile at a time

- Status: accepted (built; `docs/briefs/terrain-editing-perf.md`, P5b)
- Date: 2026-10-01
- Amended by: [0150](0150-the-far-ground-is-kept-on-disk-as-it-was-gathered-and-a-node-is-named-by-its-cells.md)
  section 7 -- a game's cache is in the machine's temporary folder, and
  changed ground is held until it does not fit a budget.
- Decided by: the owner -- *"um mapa do tamanho de GTA V deveria ser
  possível"* -- through ludwerk-08, who approved the two halves below on
  2026-10-01. By the owner's standing rule of 2026-09-30 the means are what
  engines with large editable worlds do: a working copy on disk for what has
  changed, and an importer that never holds the world.
- Amends: [0053](0053-the-grid-decides-when-and-the-model-decides-what.md) and
  [0087](0087-a-large-terrain-is-a-folder-of-cells-and-the-editor-pages-it.md) (a cell
  somebody changed is never evicted), [0144](0144-ground-is-drawn-from-the-whole-terrain-and-streaming-governs-only-what-is-resident.md)
  (what the far ground is drawn from).

## Context

A terrain of 256 cells or more is saved as cells and streamed round the
camera. **A cell somebody changed was never let go**: its file no longer says
what it holds, so letting it go would lose the change. That is right for a
crater, and it capped every world that is generated or heavily edited at the
machine's memory:

- a script that writes ground held all of it -- 8 km at a 2 m voxel peaked at
  787 MiB, so 16 km at a 1 m voxel was some 12 GiB before anything was saved;
- `--save-scene` writes what one boot's drain built, under the 5 s watchdog;
- the editor's heightmap import and its hills stopped at 4 096 columns a side,
  which is one table in memory;
- the editor refuses an edit that reaches more than 4 096 cells not loaded.

Nothing could make the world the owner asked for, so nothing could measure it
(`terrain-editing-perf.md`, P5).

## Decision

### 1. A session cache

1. **A changed cell past the load radius is written to a session cache and let
   go**, and streams back from there like any saved cell. The cache is a
   folder of cell files in the format a saved terrain has, beside the project
   (`.engine/session/terrain-<run>/`) or in the machine's temporary folder for
   a world with no project. It is this run's: removed when the run ends, and a
   folder a run that died left behind is removed by the next after a day.
2. **Ground the field holds that no cell on disk accounts for** -- written by
   a script or an import where nothing was -- is a changed cell like any
   other: it gains a row in the streamer's index when it is written out, and a
   world no file describes becomes a streamed one at that moment.
3. **It applies to a terrain of 256 cells or more**, the size at which a saved
   terrain streams (ADR 0087). A smaller one is held whole, as it always was.
4. **Memory is bounded, not best-effort**: the pump writes cells out inside its
   budget, and past a high-water mark of 96 cells waiting -- a generator
   writing a quarter of a square kilometre a frame -- it writes them all
   before the frame goes on.
5. **Saving the scene commits the cache**: every cell in it is written to the
   scene's own cells, resident or not, the index names the project's files
   again, and the cache is emptied. A world closed without saving loses what
   the cache held, as it loses what memory held.
6. **Only where nothing could want the ground back as it was.** The editor's
   undo is the world as it was, and a file on disk is not in it. So:
   - **a cell changed by hand in the editor stays in memory until it is
     saved**, as it always did -- what every editor with a streamed world does
     with a cell that is dirty. Undo is untouched;
   - **Play in the editor writes to a layer of its own** over the cache, and
     Stop drops the layer: every cell written while the game ran is what it
     was before -- the scene's file, the cache's, or nothing;
   - **an import writes to a layer too** (§2), kept when it finishes and
     dropped when it is cancelled or refused;
   - a game running outside the editor has no history, and writes to the cache
     itself.
7. **A part of a cell is written with the rest of it**: ground put into a
   square whose cell was not loaded is what the field holds over what the
   cell's file holds, a chunk at a time, the rule a save follows. A file that
   will not read is not written over.
8. **Not from under anything that can fall**: a cell within a collider's reach
   of a body that is not anchored, or of a character, is not written out,
   however far every camera is. A body that walks off resident ground into
   ground only the cache holds has none under it -- as on any streamed world;
   it needs a streaming focus.
9. **The far ground is drawn from the cache too**: the cell source a streamed
   terrain's far nodes read (ADR 0144) is told of each cell that changes --
   written out, saved, dug to nothing -- and keeps what it had read of every
   other. Made again for one cell's sake, it read the whole horizon again.
10. **Not in a match, for now.** A host or a dedicated server keeps changed
    cells resident as before: the ground's replication sends a joiner what
    differs from the package's copy (ADR 0135), and reads it from memory. The
    condition to lift this: that replication reading a cell from the session
    cache for a late joiner.
11. **Not in a run that is compared with another** -- a replay, a conformance
    run -- whose world must be a function of its inputs and of nothing a disk
    did. Those have no cache at all.

### 2. An importer

1. **Past one table, the ground is laid a tile at a time**: 256 columns a
   side, on the chunk grid, each tile handed with one column of its
   neighbours round it (`asset::HeightWindow`) so the slope at a seam is the
   one a single table would have had. What is laid is, voxel for voxel round
   the surface, what one table of the whole lays. The ground behind the tile
   goes to the session cache as it goes; nothing holds more than a tile and
   the ground the camera's radius keeps.
2. **Three sources.** A heightmap -- a sixteen-bit PNG, a RAW, or a set of
   tiles of either named `<anything>_x<column>_y<row>`, each decoded when a
   tile first reads it and a few kept; hills from noise; and **a Luau function
   of a place**, a file that returns `function(x, z)`, world metres in and a
   world height out. One image is decoded whole, four bytes a pixel; a world
   too large for that is a set of tiles.
3. **The editor's Create tab** lays anything wider than 4 096 columns, any set
   of tiles and any function this way, a few tiles a frame, with a progress
   bar and Cancel. **It is not an undo step**: cancelled, or refused half way,
   the world is as it was before the import began; finished, the history
   starts over. Nothing else may be undone, played or saved while it runs.
4. **`ludwerk terrain import <source> [path] --size=<metres>`** runs the same
   verbs with no window -- the scene as the editor holds it, no script run --
   and saves as the editor saves: the cells, then the scene that names them.
   `--import-terrain=` is the engine's flag under it.
5. **A script generates the same way**, with no importer at all:
   `Terrain:WriteHeights` a tile a frame on a terrain large enough to stream,
   and the ground behind it goes to the cache as it goes.
6. **The limit is 32 768 columns a side**: 32 km at a metre voxel.

## Consequences

- The bench the ledger could not run runs: 16 384 by 16 384 columns at a 1 m
  voxel, laid in bounded memory, saved as cells, and flown over.
- A session's cache is as large as what was changed: a generated world's
  whole, until it is saved. It is on the project's own disk, where the saved
  cells will be.
- The streamer's count of cells kept because somebody changed them
  (`FieldStreamer::kept`) is, outside the editor's editing and a match, how
  many were written out.
- A function that never returns hangs the import: it is called outside the
  scripts' watchdog. The command line is stopped with Ctrl+C.
- Two defects of the streaming this leans on were found building it, and
  fixed: a cell whose read came back after the camera had left was held for
  ever (D405), and the far ground went on drawing a saved cell as its file was
  before the save (D406).
