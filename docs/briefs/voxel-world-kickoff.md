# The block world, made a place to build a game in: the kickoff and the ledger

Decided on 2026-10-02 under the owner's standing rule (choose as professional
engines do, then tell him), from a block-world game made with the engine as it
is -- procedural world built chunk by chunk from Luau, a sea of fluid blocks,
breaking and placing validated by the server, Windows and Android -- and from
the owner playing it on his phone: *"dá muita lagada quando o jogo tá
construindo as chunks... depois que constrói fica fluidinho, perfeito"*, and
*"a gente já poderia implementar algo com level of details pra chunks longe"*.

The measurements, the reproductions and the eleven things the game had to write
for itself are in the game's own report
(`Documents\MundoDeBlocos\FINDINGS-voxel-game-2026-10-02.md`, outside the
repository). This ledger is the order they are done in. The ADR that records
the decisions below is the builder's to write, next number, before B2.

**Place in the queue:** B1 goes first, ahead of what is queued, because it is
the owner's complaint. B2 onwards after the batch in hand.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- A failing test or a failing measurement first.
- The sprint measurement (B0) is run on the desktop at every stage and on the
  phone at B1, B3 and B6. Nothing lands that makes it worse.
- No determinism trace moves except a block-world scenario, once, with the reason.
- Every string through i18n keys; every class and member has a reference page.
- Full localgate, Linux included, before every push. Stage only your own files.

## B0 — the eyes

- [x] A block gallery scene and a scripted sprint: a world generated from a
  seed, a player moved at sprint speed in a straight line for forty seconds
  while column chunks are built ahead and let go behind, one every four ticks.
  The run reports frames over 33 ms, the 95th percentile, and the simulation
  and CPU-drawing parts apart. **The sprint is `tests/perf/blocksprint`**
  (`engine-host tests/perf/blocksprint --headless --frames=2400 --exit
  --frame-stats`), a world from a formula so two runs build the same blocks;
  the gallery is `examples/14-voxels`, which shows every look a block has.
- [ ] As measured on 2026-10-02 (desktop, package `5b8a24fa`, fluid sea):
  127 of 2389 frames over 33 ms, p95 33 ms; CPU drawing p95 19 ms, simulation
  p95 9 ms. With the game building in large boxes and resting after each chunk:
  49 over 33 ms, p95 20 ms. These are the numbers to beat.

## B1 — a chunk that changes does not cost the frame that draws it

What block engines do, and the decision: meshing is work for other threads.

**What the probe found first** (D449): meshing was a tenth of it. Of the 26 ms
a batch of 32 chunks cost its frame, 18 were making two GPU buffers for every
mesh, up to 7 were finding what had changed, and 1 to 3 were the meshing. So
B1 is three things, in the order they paid: chunk meshes share buffers, what
changed is found in one walk of the chunks' digests, and the mesh is made on
a worker.

- [x] A changed chunk's render mesh is built on a worker from its blocks and
  the 26 chunks around it, shared and never copied; the frame goes on drawing
  the old mesh until the new one is ready, then swaps. Chunks asked for
  together -- up to 32, two batches at the workers at once -- are swapped in
  the same frame, so the two sides of a broken block never disagree. A change
  of four chunks or fewer (a block broken or placed) is waited for where it is
  asked. A frame a picture is taken from waits for everything.
- [x] Uploads cost a copy: a chunk's meshes are slices of buffers the mesh
  cache already has (`MeshUsage::Pooled`), given back and taken again.
- [ ] The collision mesh the same way, with one rule on top: a chunk a
  character stands in or beside is finished before the step that needs it --
  nobody falls through ground that is being built. **Not moved, and why**:
  measured in both sprints at 1.4 ms a tick at most, because it is already
  bounded -- two chunks a tick, only within 24 m of something that moves, and
  the chunk in a mover's way at once (D417). And what the simulation reads
  must be the same on every machine at the same tick, so a collider cannot
  arrive when a worker happens to finish: moving it means building at tick T
  and applying at T + N whatever the worker did, which is N ticks of ground
  that is not there yet for everything not in a mover's way. To do if the
  phone's measurement asks for it; the mesher would then make the collider
  alone, which today it makes with the faces.
- [x] Results are applied in a stable order (R10): what the simulation reads
  -- collision -- is the same on every machine at the same tick. Nothing the
  simulation reads changed: a render mesh is not read by it.
- [x] Acceptance on the desktop: `tests/perf/blocksprint`, 1465 of 2389 frames
  over 33 ms to none, median 38.9 ms to 5.5 ms, CPU drawing 30.8 ms to 2.7 ms;
  the game's own sprint, 20 to 26 over 33 ms to none, p95 28 ms to 7 ms.
- [ ] Acceptance on the phone: no frame over 33 ms while one chunk a second
  arrives. Not measured -- the builder has no device.

## B2 — blocks in bulk, and knowing what changed

- [ ] `ReadBlocks(from, to) -> buffer` and `WriteBlocks(from, to, buffer)`:
  one call per chunk for generation, structures, copy and paste.
- [ ] `Raycast` also returns how far the hit is.
- [ ] `BlockChanged(block, before, after)` on the server and on clients, fired
  once per changed block or once per bulk write with its box -- the ADR says
  which, with the cost of each.
- [ ] Fluids treat a place where no chunk exists as solid: a sea beside a
  chunk that is not built stays where it is. (The game builds and moves
  invisible walls for this today.)

## B3 — chunks are the engine's business

The ADR settles first whether the block world becomes an instance -- a
`VoxelWorld` under `Workspace`, placed and moved in the editor, more than one a
scene, `VoxelService` staying as the way to the scene's first -- as the terrain
is an instance. Recommended: yes; a block world is a thing in a scene, not a
property of the process.

- [ ] A generator the engine calls per chunk, on workers, given the chunk's
  coordinates and a buffer to fill. Deterministic by contract: the same seed
  and coordinates give the same blocks, and the engine may call it again.
- [ ] A view distance per player; chunks requested nearest first, built
  within a budget, released when nobody is near. On a server, per player; a
  client is sent what its player is near.
- [ ] What players changed is kept as a difference from what the generator
  made, saved with the world, and put back when the chunk is built again.
- [ ] A game can still do all of it by hand, as today: the generator is
  optional.

## B4 — a character in a block world

- [x] `ApplyImpulse` on a `CharacterBody` does what it says; its velocity can
  be written; `GravityScale` in 3D as in 2D (D466).
- [x] Movement modes -- walking, falling, swimming, flying -- with swimming
  entered by itself in a `Water` and in a fluid block.
- [x] `@engine/camera.firstPerson`.

## B5 — light

- [ ] Sky light stopped by blocks and light given by blocks, propagated
  through the grid and carried per vertex: a sealed room is dark, a cave is
  dark, a torch lights it. A block type says how much light it gives and how
  much it lets through.
- [ ] Relit on a worker with the mesh (B1), bounded per frame.

## B6 — the far world

The owner's idea, and what large block worlds do: past the radius where
blocks are kept, the world is drawn and not stored.

- [ ] Beyond the view distance, column tiles in a quadtree -- blocks of 2, 4
  and 8 on a side -- meshed on workers straight from the generator's surface:
  height and top block per column. No blocks kept, no collision, no fluid.
- [ ] A tile gives way to the real chunk as it is built, under the fog, with
  no hole and no doubled ground at the seam.
- [ ] Edits far away: a tile over chunks that hold saved differences is
  built from them too, coarsely -- a tower a player built is still on the
  horizon.
- [ ] Needs B1 and B3. The same idea ADR 0150 is for the terrain; share what
  can be shared.

## B7 — the rest of the genre's furniture

- [ ] Block flags and shapes: a type that does not collide, half blocks,
  stairs, crossed planes.
- [ ] A highlight for the block pointed at, and break stages drawn on its faces.
- [ ] Icons of block types for an interface (ADR 0107's `ViewportFrame`
  covers it; say so in the manual with an example).

## Findings

(Filled as stages close.)
