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
2. **Drawing.** Done, and not as first written here: see *Drawing, as
   built* below. A model is drawn in the shape its file's own weights give
   it; nothing sets a weight yet.
3. **The API and the clips.** `MeshPart:GetMorphTargets()`,
   `SetMorphWeight(name, weight)`, `GetMorphWeight(name)`; a playing clip's
   weight channels; a script's value over the clip's for that target until
   released. Presentation only, outside the world's hash: a weight set by a
   client's script is that machine's. Not built.
4. **The editor and the proof.** A slider a target in Properties, previewing
   live; an example head; the cost for one face, six heroes, and four hundred
   bodies whose targets are all at nought, which must be today's. Not built.

## Drawing, as built

**A dense table on the card.** A vertex shader cannot walk a sparse list, so
the targets are laid out a row a target and a delta a vertex, sixteen bytes
each (a place as three floats, and the normal's delta as three signed ten-bit
numbers), built from the sparse deltas when the mesh is loaded
(`render::buildMorphTable`). Dense over the vertices from the first to the
last any target moves, and not over the mesh: a body whose targets are on its
face pays for the face. Past 128 MB a mesh is drawn without its targets and
the log says so.

**A body with any weight above nought is drawn on its own**, through four
shaders of its own -- lit and depth-only, each with and without a skeleton
(`pbr_morph`, `pbr_morph_skinned`, `shadow_morph`, `shadow_morph_skinned`) --
which are the built-in ones with the vertex moved by its targets first and by
its joints after. Ten pipelines stand in for the lit pass, the lit pass over
a prepass, the blended pass, the shadow maps and the camera's prepass. They
are made the first frame a body has a target above nought
(`DefaultRenderer::ensureMorph`), so a world with none builds nothing and no
capture golden moves.

The first design carried the weights in the skinned instance's spare floats,
to keep a morphing body in its run. It was dropped for this one: it reaches
only skinned meshes, it puts a table read and a loop in the shader of every
skinned instance whether or not anything morphs, and the bodies that morph at
once are a few faces, not a horde.

**A body whose weights are all at nought is not touched.** Which targets a
body is drawn with is decided each frame from that frame's weights and
nothing kept (`render::selectMorphs`): those further from nought than a
thousandth, the eight largest of them. None, and the draw has no row
(`DrawItem::morph`), is batched like any other, and nothing after can tell it
from a mesh that never had a target -- so it is back in its run the frame its
weights return. The floor is what keeps a weight that a blend of clips leaves
at a ten-thousandth from costing a draw.

**Only the sections a target reaches.** A model is drawn a material at a
time; a section none of whose vertices is in the table keeps its run while
the face beside it is drawn alone. Hence the manual's advice: give the face
its own material.

**A vertex finds its deltas by its own number**, and that number is not one
thing: with a base vertex, Direct3D counts from the mesh and Vulkan and Metal
from the buffer. So a table is given only to a mesh whose vertices start at
nought in their buffer -- a static mesh, which every model is
(`MeshCache::attachMorphs` refuses a pooled or dynamic one) -- and the place
the pipeline is chosen says the same again. The test that a morphed vertex
lands where the file says runs on both APIs.

**The mesh's bounds are grown to where each target alone can take it**, a
weight of one and of minus one (`render::growBoundsForMorphs`), for the cull
and the fit of a shadow map. Weights past one, or several targets pushing one
way, can leave them.

What a morphing body does not get: the outline and highlight masks draw it at
rest; the motion vectors carry its body's and its skeleton's motion and not
its targets'; a surface shader on its material is not used (a mesh with
targets keeps the built-in surface whatever its weights, as a skinned mesh
does, so that its look does not change the moment it speaks); a target's
tangents are not kept.

**Known next step: a crowd of faces.** Every morphing body is a draw in
every pass -- measured, about eight microseconds a body a frame. Six heroes
talking are nothing. Two hundred faces talking at once would be eight
hundred draws a frame the runs exist to avoid, and the answer to that is an
instanced morph path: the weights of every instance in one shared buffer,
each instance naming where its own begin, as the palettes of a skinned run
already are. It is not built because nothing asks for it yet, and it is
written here so that the day something does, the design is not rediscovered.

## Memory, stated

As imported and compiled, a delta is 28 bytes (a vertex, a place, a normal),
kept only for the vertices a target moves. On the card it is 16 bytes a
vertex a target, over the vertices from the first to the last any target
moves: a face of three thousand vertices with fifty targets is 2.4 MB. (The
first estimate here was 12 bytes as 16-bit displacements; the place is kept
as floats, since a quantised place would need a scale a target and the saving
is a quarter.)

## Cost, measured

The development build, no window, four hundred skinned bodies on a floor with
shadows on:

| The four hundred | Draws a frame | Frame, median |
| --- | --- | --- |
| a model with no targets | 11 | 1.22 ms |
| three targets, every weight at nought | 11 | 1.21 ms |
| three targets, two of them above nought, on every body | 1,712 | 4.23 ms |

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

## Proof, stage 2

`engine/render/tests/morph_tests.cpp`: the table covers the vertices its
targets touch and no others; a vertex moved by the shader's own steps, from
the table and the block, lands where its targets and their weights say; a
body whose weights are all at nought, or under the floor, has no morph draw;
of more targets than a draw can move by, the eight largest are kept; targets
that move nothing, or more than a mesh may hold, make no table; and only a
mesh whose vertices start at nought in their buffer is given one.

`morph_landing`, and on Windows `morph_landing_vulkan`
(`tests/screenshots/run_morph_landing.cmake`): three pillars of one mesh and
a skinned one whose joint a clip holds moved and turned, drawn from models
with targets -- two above nought, one of them negative and turning normals,
and one at nought that would throw every vertex three units out -- and drawn
again from the same pillars made in that shape with no targets. The two
pictures are the same to a level in 255, in the lit faces, the depth and the
shadows; the first run counts its four bodies as `morph` draws and the second
counts none. On the package before this stage the first run draws the pillars
at rest: no morph draw, and 11,045 pixels of 230,400 differ.
