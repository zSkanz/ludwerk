# 0196 — Morph targets: a mesh in other shapes, weighted an instance

- Status: accepted; built in stages, and this says which are done
- Date: 2026-10-10
- Decided by: the owner, on a video of faces animated with shape keys ("the
  engine has to be able to do this with models"); shaped by the orchestrator;
  detailed by the agent from the code
- Builds on: the glTF importer and the compiled mesh format (ADR 0036, M6),
  skinned instancing (H2), ADR 0194 (the pose a frame is drawn with)

## The question

A face is not bones. A blink, a smile, a jaw, a brow are the same mesh in
another shape, and an artist makes them as shape keys and animates their
weights. The importer said of them: "v1 has no morph targets, so a channel
driving them is dropped rather than half-read."

## Decision

A **morph target** is the mesh in another shape, kept as how far each vertex
is from where it is at rest. A weight of one is that shape; between nought
and one is part of the way; several add. Weights are an instance's: two
heroes of one model wear two expressions.

### What is imported

- A mesh's targets: the displacement of each vertex's place and normal. A
  target's tangents are not read.
- Their names, from the mesh's `extras.targetNames`, where every exporter
  writes them; `Target<n>` for a file that names none. Two meshes of one model
  that both have a `Blink` are one target.
- The mesh's default `weights`.
- A clip's `weights` channels. A file's one sampler for all of a mesh's
  targets comes apart into a channel a target.
- For a mesh with no skeleton too. Until now such a mesh had no clips at all:
  the importer stopped at "no joints".

**Sparse.** A target moves a patch of a face, so what is kept is the vertices
it moves, ascending, and nothing for the rest. That holds in the imported
model, in the compiled file and -- when it is built -- on the card.

A displacement goes through what its vertex goes through: the node's
transform (its turn and scale, none of its translation), the optimizer's
renumbering, and the bind pose where a skeleton is baked away. A normal's
displacement is brought to the scale the transform leaves its normal at; it
is the first order of a turn, which is what a morph's normal is everywhere.

### The compiled file

Four sections, every one optional: `MRPH` (the targets), `MRPD` (their
deltas), `WCHN` (the clips' weight channels) and `WCLP` (which of those each
clip has). **The mesh format's version does not move**: a mesh with no target
is byte for byte the file it was, and a reader from before the sections skips
what it does not know.

A clip's weight channels are in a list of their own (`AnimationClip::weights`,
`Target::Weight`, `joint` the target's place in `morphs`) and never among the
joints' channels, so the pose walk did not learn a fourth kind of channel.

This corrects the summary this decision was approved on, which said animated
weights needed format version 4. They do not.

**The compiler's rules number does move** (7 to 8), and that recompiles every
source of every project once, on its next open: a model with targets that was
compiled before has none in its cached form, and the cache cannot tell which
models those are. After D597 that is seconds for a small project and under
half a minute for one of six hundred sources.

All of a mesh's levels of detail draw from one vertex stream, so a target's
vertices are every level's. **The simplifier does not know a vertex moves
with a target**: a coarser level may drop an edge loop a smile needs. Until it
does, a face should keep its vertices (one level, or a high error bound), and
the manual says so where an artist will read it.

## Stages

1. **Import and the compiled file.** Done: `engine/asset/src/gltf.cpp`,
   `mesh_format.cpp`, `tests/data/morph_quad.gltf`.
2. **Drawing.** The deltas in a storage buffer the vertex stage reads, the
   weights an instance in another, at most eight active targets a mesh with
   the largest weights winning. Morph first, then skin. A skinned instance
   has three spare floats already in its vertex layout, so the place of its
   weights rides there with no new pipeline family; a body whose weights are
   all nought says "none" and stays in its run, so a horde pays nothing. A
   mesh with targets and no skeleton is drawn singly. The shadow and prepass
   shaders read the same. Not built.
3. **The API and the clips.** `MeshPart:GetMorphTargets()`,
   `SetMorphWeight(name, weight)`, `GetMorphWeight(name)`; a playing clip's
   weight channels; a script's value over the clip's for that target until
   released. Presentation only, outside the world's hash: a weight set by a
   client's script is that machine's. Not built.
4. **The editor and the proof.** A slider a target in Properties, previewing
   live; an example head; the cost for one face, six heroes, and four hundred
   bodies whose targets are all at nought, which must be today's. Not built.

## Memory, stated

As imported and compiled, a delta is 28 bytes (a vertex, a place, a normal),
kept only for the vertices a target moves. On the card it will be 12 bytes a
vertex a target as 16-bit displacements; a head of five thousand vertices
with twenty targets that each move a third of it is 0.4 MB.

## Proof, stage 1

`gltf_tests.cpp`: a mesh's targets import by name, sparse, and where the file
says -- at rest, at each target whole, at a half, both together, through a
node that scales; they follow their vertices through the optimizer; the
host's pass gets the names and none of the vertices; a clip's weights import
as a channel a target, for a mesh with no skeleton; a model with no targets
has none.

`mesh_format_tests.cpp`: targets and clips' weights round-trip, and encode to
the same bytes twice; a mesh with no target has none of the four sections; a
delta that names a vertex the mesh lacks, deltas out of order, and a weight
channel for a target that is not there are each refused.

The determinism replays reproduce unmoved.
