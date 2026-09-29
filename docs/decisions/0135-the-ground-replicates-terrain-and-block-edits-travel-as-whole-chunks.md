# 0135 — The ground replicates: terrain and block edits travel as whole chunks

- Status: accepted (2026-09-29)
- Date: 2026-09-29
- Decided by: the owner, on 2026-09-29: *"nosso terreno em si deve ter um
  acesso por código … editar durante a gameplay mesmo, por exemplo, fazer um
  jogo de escavação ou fazer mesmo a geração procedural em tempo de
  execução."* Scripts could already edit the ground at run time; a match could
  not see it.
- Builds on: [0069](0069-replication-reads-state-and-diffs-it.md)
  (the wire), [0082](0082-terrain-is-a-grid-of-voxels.md) (the field),
  [0103](0103-2d-on-the-wire-is-interpolated-and-a-tilemap-replicates-by-blocks.md) (whole blocks, the model this
  follows), [0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md) (a scene each end loads),
  [0113](0113-terrain-layers-are-engine-materials-and-rules-paint-by-slope-and-height.md)
  (layers and rules).

## Context

A terrain and a block world are loaded from the scene on every machine and
never replicated: "a world's ground arrives with the world, on every machine,
from its scene". Everything a script did to them afterwards -- a crater, a
tunnel, a mined block, a world generated when the match began -- stayed on the
machine that did it. A digging game had no way to be multiplayer, and a server
that generated its world gave every player a different one.

## Decision

**The authority sends the ground's chunks as whole chunks, when they change,
and every chunk that differs from the scene to a replica that joins.** A chunk
received twice is simply the truth, as a tilemap's block is (ADR 0103); nothing
is replayed, so nothing depends on two machines computing a brush the same way.

1. **What is replicated is the workspace's ground**: the terrain
   `workspace.Terrain` names and the world's `VoxelService` block world. Both
   are there on each end, loaded from the same scene, or made on the replica
   when the authority's has ground and the replica's has none (a server that
   creates its terrain in a script). They are named by what they are, not by a
   network id: there is one of each.
2. **The authority diffs by chunk identity.** Chunks are shared and copied on
   write, so a chunk an edit touched is a different chunk: the authority keeps
   the chunk pointers it last sent (the shadow) and compares pointers, which
   costs a walk of the chunk list when the ground's revision moved and nothing
   when it did not. A chunk that is gone is sent empty.
3. **A replica that joins is sent every chunk that differs from the scene** --
   the authority keeps the field as the scene loaded it (a copy of pointers)
   and sends what differs -- then every change after.
4. **Messages**, on Control (reliable, after the spawns of the same send):
   - `TerrainChunks` (type 17): the field's settings, then up to 64 chunks, each
     its key and its voxels coded as a terrain cell codes them (runs of packed
     voxels). A chunk with no runs is empty.
   - `TerrainLook` (type 18): the layer list and the rules, whole, when either
     changed and once to a replica that joins.
   - `VoxelChunks` (type 19): the block size, then up to 64 chunks of blocks, as
     a scene codes them. A chunk with no blocks is empty.
   - `VoxelTypes` (type 20): the block types and fluid reactions, whole, when
     they changed and once to a replica that joins.
5. **A replica's own edits are its own until the authority's arrive.** A
   replica's script may edit its ground -- for the feel of a dig before the
   server answers -- and the next chunk the authority sends over it wins. The
   authority never takes a replica's ground; a player digs by asking the server,
   through a remote.
6. **Bounded.** A chunk's code is refused past what a chunk can hold; a key
   past the field's reach (F12) is refused; a message names at most 64 chunks.

## Consequences

- A digging game, a building game and a world generated at run time are
  multiplayer as they are single-player: the server edits, every player sees it,
  and a late joiner sees the same ground.
- The cost is proportional to what changes: a dig sends the few chunks it
  touched, a quiet match sends nothing. A server that regenerates a whole world
  sends all of it once.
- The protocol moves to 24.
- The editor's play-with-players and an exported server need nothing new.

## Not decided here

- Predicting a dig on the replica with a rollback of the ground.
- Streaming a replica's ground from the authority instead of from its package:
  a streamed terrain's cells still come from each end's own files, and the
  chunks that differ travel.

## Amendment -- 2026-09-29, the terrain audit (protocol 25)

The audit of the terrain system (`docs/briefs/terrain-audit-2026-09-29.md`)
found what this decision left out, and each is now part of it:

- **A removal is kept as known.** A chunk the authority removed before a
  replica loaded its cell is recorded in the replica's `shipped`, so the load
  leaves it removed (G4). On the authority, a key leaving the ground and the
  package's copy in one interval is a cell streamed out, and is not sent (G5).
- **The look carries the terrain's place** (`TerrainLook` gains the origin:
  protocol 25), and a replica with no terrain makes one from the look as well
  as from chunks (R1, R3). A replica whose terrain is empty takes the
  authority's voxel size (R2).
- **A terrain replaced in a scene is the same ground to its peers**: the
  package they loaded stays the base the new one is measured against, and a
  terrain destroyed sends the removal of its ground (R4).
- **A terrain that appears after a peer was sent the ground** starts its shadow
  from the package, so its removals of package chunks are sent.
- **A save makes what it wrote the package**, and a world put back keeps the
  keys it had loaded with what the disk now holds at them (R5).
