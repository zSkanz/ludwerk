# The terrain system's audit -- 2026-09-29

The owner, the night the terrain editor was remade: "after the terrain editor,
run a whole audit of our terrain system, the editor with it, for quality,
performance, security and function -- everything must work". Three read-only
passes, one each over the data and its script API and replication, the
runtime (streaming, meshing, physics, foliage), and the editor's tools. Every
finding below was traced through the code; each fix lands with a test that
failed first and a row in [`defects.md`](../defects.md).

Severity as the 2026-09-28 audit had it: **P0** crash, hang, data loss or a
security hole; **P1** wrong behaviour a person will meet, or a cost cliff; **P2**
minor. Grouped by root, since one fix closes several rows.

## G -- ground that comes back

One root: an edit that empties a chunk drops it, and every load path merged a
cell's file into the field wherever the field held nothing -- so "dug away"
and "not loaded" were the same fact.

- [x] **G1 (P0)** A chunk dug empty returns when its cell streams out and back,
      on the editor's camera and in a game alike -- a digging game on a
      streamed world loses its holes.
- [x] **G2 (P1)** An undo, a redo or a Stop (`reconcile`) brings a dug chunk
      back in a resident cell.
- [x] **G3 (P1)** A save merges the file back into a kept-then-evicted cell,
      and a cell dug to nothing and evicted keeps its file.
- [x] **G4 (P1)** A replica told a chunk is gone loses the removal when the
      cell streams in afterwards.
- [x] **G5 (P1)** The authority evicting a cell whose edits netted out (a
      different chunk with the package's bytes) sends it as a removal: a
      permanent hole on every replica.

## U -- edits where the ground is not loaded

- [x] **U1 (P0)** A brush or a script edit over a cell not yet streamed in
      makes a chunk of only the edit, which shadows the file's chunk for good:
      a 32 m cube of hillside gone on load, and on save. A dig there is lost.
- [x] **U2 (P1)** Replace with Flat Ground and Clear All Ground act only on the
      loaded cells of a streamed terrain; the rest streams back.

## B -- a script's arguments

- [x] **B1 (P0)** `RaiseBall` with a huge `amount` allocates gigabytes per
      column (and a non-finite one is undefined behaviour).
- [x] **B2 (P0)** `VoxelService:FillBlocks` has no volume bound: one call can
      allocate gigabytes or walk 10^18 empty chunks; its coordinates are cast
      unclamped.
- [x] **B3 (P0)** `VoxelSize`, `MinHeight` and `MaxHeight` accept settings a
      save, a stream and the wire all refuse: the ground is lost on reopen and
      never reaches a replica.
- [x] **B4 (P0)** An edit far enough out makes a chunk past `MaxChunkKey`; the
      save then refuses its whole cell and the wire the whole message.
- [x] **B5 (P1)** `SmoothBall` at the largest allowed radius allocates about
      1.5 GB, even over empty sky.
- [x] **B6 (P2)** Material ids wrap through `u8` (256 erases); a NaN strength
      turns Smooth and Flatten into a dig; `WriteHeights`' corner and
      `ReadVoxels`' region cast unclamped.

## R -- the ground on the wire (ADR 0135)

- [x] **R1 (P1)** Layers and rules sent before the replica has a terrain are
      dropped, and never sent again.
- [x] **R2 (P1)** A replica refuses every chunk of a terrain whose voxel size
      the server changed, even when its own is empty.
- [x] **R3 (P1)** The terrain's `Position` does not travel.
- [x] **R4 (P1)** A terrain destroyed and made again on the server leaves the
      old scene's ground on every replica.
- [x] **R5 (P2)** A scene with no block world leaves the last one's `shipped`;
      a save leaves `shipped` as it was loaded.
- [>] **R6 (P2)** A joiner is sent every changed chunk in one tick; the look
      and types are encoded every send; a hostile server's types are not
      checked for finite colours. **The types are checked** (D317); pacing and
      encoding are left -- see below.

## P -- the runtime

- [x] **P1 (P1)** A tick with no moving bodies retires every terrain collider
      while the pass still reads as settled: a character respawned in the same
      place falls through the ground.
- [x] **P2 (P1)** A small voxel size makes every mesh and collider build a
      stall or an out-of-memory (the sky map's reach is in metres).
- [x] **P3 (P1)** The renderer visits every root in the terrain's bounding
      box every frame: two edits a million metres apart freeze a client.
- [x] **P4 (P1)** -- left, see below. Continuous digging rebuilds the 3x3
      columns of meshes, colliders and foliage round every edit, whatever it
      touched.
- [x] **P5 (P2)** A dig across more than four chunks rebuilds the same nearest
      four every tick and starves the rest.
- [>] **P6 (P2)** -- the revision and the URN fixed (D322), the rest left.
      `TerrainLoader::find` is a linear scan; a restore rolls
      `fieldRevision` back so a cached node can be trusted stale; the node URN
      ignores the slot generation; foliage meshes the whole column height; one
      layer map that never resolves leaves every layer untextured.

## E -- the editor's tools

- [x] **E1 (P1)** Ctrl+Z or Ctrl+Y during a stroke pops the stroke's own step
      mid-drag, and a stroke that then touched nothing retracts an unrelated
      step.
- [x] **E2 (P1)** A stroke that changed nothing still clears the redo stack,
      and `retract` pops whatever is on top (a Delete made mid-stroke).
- [x] **E3 (P1)** The brush material is never checked against the layers: new
      ground is an id no layer names, and stays grey after the first material
      is added.
- [x] **E4 (P2)** Aimed past the ground, Add and Flatten stamp on a plane in
      mid-air.
- [x] **E5 (P2)** A terrain replaced mid-stroke takes the rest of the stroke.
- [x] **E6 (P2)** The terrain keys fire while the game plays; the chip and the
      status bar show a brush with the panel closed; Paint's hint names keys
      it does not have; Paint arms with no layers; Replace with Flat Ground
      refused as too large says nothing.
- [x] **E7 (P2)** A big brush dragged fast can stall a frame for seconds: the
      stamps a frame are capped, their voxels are not.

Found by the tests written for these: **a terrain that appears after a peer
was sent the ground** -- one a script makes in a scene that had none -- started
its shadow empty, so a removal of a package chunk was never sent (D306).

## Left, and why

- **P4, the cost of digging continuously.** Each dig makes the 3x3 columns of
  meshes, colliders and foliage round it stale, because each keys on its
  neighbours' whole digests. Part of that is real -- a vertex's sky reads
  columns 14.5 m away -- and the fix is a dirty box each edit reports, grown by
  what each consumer reads, which is a change to three consumers and the
  edit report together. It wants a measurement first (a digging scene in
  `docs/perf-baselines.md`), and is the next terrain performance item.
- **P6's rest**: `TerrainLoader::find` sorted and searched, foliage meshed over
  the layers it covers rather than the whole column, a layer map that never
  resolves standing in with a neutral texture. Costs and a fallback, none a
  wrong result.
- **R6's pacing**: a joiner is sent every changed chunk in one tick, on the
  reliable channel. A generated world is sent whole to each joiner; pacing it
  over ticks is the change when a real one is measured.
- **F11** (2026-09-28's audit): a crafted cell file can still ask for a chunk's
  full arrays. A cell file is a project's own content, never a peer's -- chunks
  from a peer are bounded per message (ADR 0135).
- **Cells an edit read ahead of the camera** stay in memory until the camera
  has been there and gone: the streamer never counted them resident, so it
  never lets them go. A script editing all over a large world grows its ground
  in memory, as it would have with every cell loaded.

## Found sound

Chunk decoding and every per-message bound on the wire; the raycast's chunk
leap and `HeightAt` at chunk seams; revision bumps on every script verb; no
unordered iteration or clock in the edits; the collider cache key against the
mesher's reach; level-0 render and collider triangles alike; undo memory
(64 steps of shared chunks); shortcuts while typing.
