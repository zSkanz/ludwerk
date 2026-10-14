# 0203 — Light that bounces: irradiance volumes, not lightmaps

- Status: accepted and built; amended while built, see the end
- Date: 2026-10-10
- Builds on: ADR 0202 (reflection probes, whose capture this reuses), ADR
  0084, ADR 0116 (compute), ADR 0147 (settings), R10, R16

## The question

The side of a wall the sun does not reach is lit by one flat colour,
`Lighting.Ambient`. A red carpet does not redden the wall beside it; a room
with one window is as bright in its far corner as at the window. What is the
least the engine can do that makes light bounce?

## The two ways, and what each costs

**Baked lightmaps.** Every static surface gets a second set of texture
coordinates and a texture of the light that reaches it, worked out ahead of
time. At run time it is one texture sample a pixel -- the cheapest there is,
and the best-looking for a static scene. What it costs is everything
around that sample:

- a second UV set on every mesh, unwrapped without overlaps, which the
  engine would have to generate (parts and terrain have none, and most
  imported models' are not fit for it);
- a baker: rays against the scene, off line, minutes for a level, with a
  place in the editor and in the export;
- textures by the hundred megabyte for a large level, which is exactly what
  a phone does not have (R16), and which do not stream with a world that
  does;
- and it lights nothing that moves. A character in a baked room needs light
  probes anyway.

It is also the opposite of how this engine's worlds are made: by scripts,
at run time, out of parts that are placed, moved and destroyed while the
game runs. A lightmap is a promise that the world stays as it was baked.

**Probes.** Points in space that each hold the light arriving there from
every direction, and surfaces that read the nearest few. Coarser than a
lightmap -- no sharp bounced shadow, and light leaks through a thin wall
between two samples -- but:

- nothing is asked of a mesh: no UVs, no import step;
- what moves is lit like what does not;
- it is taken by the machinery ADR 0202 builds, at run time, and can be
  taken again when a wall falls or night comes;
- its memory is kilobytes a room.

**The recommendation is probes**, in the form of a grid: the irradiance
volume. Lightmaps are named below as what a game of large static interiors
would want on top, and not built now.

## The decision

### 1. `IrradianceVolume`, under a part whose box is its box

As a `ReflectionProbe` is (ADR 0202): parented to a `BasePart`, whose box is
the volume. Inside it, diffuse light is the volume's; outside, what it was.

| Property | Default | |
|---|---|---|
| `Enabled` | true | |
| `Spacing` | 2 | Metres between samples, along each axis of the box. No finer than 0.5. |
| `Blend` | 1 | Metres over which it fades in from the box's faces. |
| `Intensity` | 1 | |
| `Bounces` | 1 | How many times light is let bounce: 1 to 3. Each is the bake again. |
| `Dynamic` | false | Baked again when the light changes, and on `Refresh`. |

`Refresh()` bakes it again. `GetProgress()` is how much of the bake is done,
0 to 1: a loading screen's to read.

A volume has at most 4,096 samples; a box that `Spacing` would give more is
sampled more coarsely and says so once.

### 2. What a sample holds

The light arriving at that point, as four spherical-harmonic coefficients a
colour -- a constant and a direction (L1). Twelve numbers: three
`Rgba16Float` texels. It is enough to say "more light from the window's
side, and it is warm", which is what bounce light is; it cannot say "a
bright thing exactly there", which is a reflection's job.

All the volumes in view share three array textures, a slice of a volume a
layer, so the hardware interpolates across a slice and the shader between
two: six taps for a pixel inside a volume.

### 3. How it is baked

A sample is a reflection probe's capture made small: six pictures of
sixteen texels a side from that point, of what does not move, projected onto
the four coefficients in compute and written into the sample's three texels.
A dozen faces a frame, so a room of two hundred samples is baked in under
two seconds while the game runs, light arriving as it goes; the sky's
irradiance stands in for a sample not yet taken.

**A bounce is a pass.** The first pass sees surfaces lit by the sun, the
lamps and the sky: its samples hold one bounce. A second pass sees those
surfaces lit by the first pass's samples as well, and holds two.

**A sample inside a wall is not used.** A sample most of whose six pictures
are the back of something is marked, and its neighbours' weights take its
share -- the commonest leak, a sample in the thickness of a wall lighting
both rooms, closed without asking anybody to place anything.

### 4. How a pixel uses it

A volume is in the light clusters, as a probe is. A pixel inside one reads
the eight samples round it (six taps), weighted against its normal, and that
is its diffuse indirect light -- in place of the sky's irradiance and of
`Lighting.Ambient`, which were standing in for it. Where a reflection probe
and a volume both cover a pixel, the volume gives the diffuse and the probe
the specular. Screen-space occlusion multiplies it as it did the ambient.

One volume a pixel; where two overlap, the finer spacing wins.

### 5. What it costs, and where it is off

`GraphicsService.IndirectLight` (and `--[no-]indirect-light`): off at Low
and Medium, on at High and Ultra, and off in every handheld preset until it
is measured on a phone to fit. Off, nothing is baked and the diffuse is what
ships today -- or the reflection probe's own irradiance, where there is one
(ADR 0202), which is the cheap half of this already.

Measured before it is called built, on Direct3D 12 and Vulkan: a frame with
a volume covering the screen against none; what a bake's dozen faces cost
the frame they are in; the bake's whole time for a room; the three arrays'
memory. The numbers go here.

### 6. Simulation, or picture

Picture, as ADR 0202's probes are. Travels as authored (protocol 46).

## Not decided here

- **Lightmaps**, for a game whose levels are large, static and indoors: the
  sharp bounced shadow a grid cannot draw. On top of this, with a baker.
- **Baking to disk**, with ADR 0202's.
- **A volume that follows the camera** over an open world, cascaded.
- **Occlusion a sample**, beyond "inside a wall": a sample that knows how
  far it sees in each direction would close the leaks through thin walls
  that this leaves.

## Decided while built (2026-10-10)

Where this section and the text above disagree, this section is what was
built.

1. **A volume is worked out by rays on the processor, not by pictures**
   (section 3 is withdrawn). A volume of 4,096 points by pictures is 24,576
   passes of the whole renderer, each with its own shadow fit: tens of seconds
   on any machine. By rays it is about a million queries of the physics world
   spread over the job system -- 65 ms for those 4,096 points -- and nothing
   of the GPU is in it: the same light through Direct3D, Vulkan and on a
   phone, and lit again under a moving sun without costing the frame a pass.
   Sixty-four rays leave each point, the same sixty-four from every point.
2. **What a ray meets** (amended the same week:
   a game's interior is an imported building, and a building that is a shut
   box to the light is not a feature):
   - **an anchored mesh is its own triangles.** The mesh library keeps a
     mesh's indices beside its points, for this and nothing else, and the
     first volume begun with the mesh in the world makes a tree of boxes over
     them (`TriangleTree`). A ray is taken into the mesh's own space -- its
     stretch taken out -- and asked there. So a doorway and a window modelled
     into one mesh let light through, and a mesh whose collision is switched
     off still casts a shadow in the bake as it does in the picture. Its body
     in the physics world, a hull, is left out of the ray's cast there;
   - a `Part` is its exact shape: from the physics world where that holds
     it, and by the triangles of the shape it is drawn as where it does not
     -- a part with its queries off, or with no body at all. **One rule,
     whichever class an author used and whatever its collision is set to:
     what is drawn and anchored blocks light and gives it back; what is half
     transparent or more lets it by.** The instances asked by their
     triangles are found through a tree of their boxes, so a scene of
     thousands costs a ray a few of them;
   - what is unanchored, and a `CharacterBody`, is passed: it will not be
     there next frame; so is a part half transparent or more, and so is a
     section of a mesh whose material blends and is mostly clear -- a pane in
     a window frame that is one mesh with the wall;
   - terrain is its surface, in one earth colour.

   **A surface gives back its colour times the mean of its picture.** A
   texture's mean colour, as light, is read off its smallest level when it is
   decoded (`TextureAsset::mean`), kept beside the texture, and multiplied
   into what a material bounces: a tiled floor bounces terracotta, where it
   bounced the white its tint is. A section of a mesh gives back its own
   material's, and gives off its `Emissive`.

   The warning that named the meshes in a volume's box is kept for the one
   case left: a mesh whose triangles are not to be had -- it has not loaded
   -- and which is therefore still its hull.

   What it costs, 4,096 points, Release: the hall of the stress case out of
   parts, by the physics world alone, 65 ms of rays; the same hall as ONE
   mesh by its triangles, 76 ms at 192 triangles, 79 ms at 27,648 and 87 ms
   at 442,368 -- and, once a mesh, the tree: under a millisecond for a room,
   about 190 ms for those 442,368 triangles. `--skip=bake_triangles` asks a
   mesh by its hull again, to read the difference off two runs. The gate is
   `mesh_room_gate`: a room that is one mesh is black by its hull and lit
   through its window by its triangles.
3. **When the rays run**: between ticks, inside the frame's own drawing, with
   the frame's thread waiting for the workers. The physics world is read by
   many threads at once and stepped by none while they do. A volume is begun
   two ticks after it is asked for -- first seen, moved, or `Refresh` -- since
   a tick is what brings the physics world up to the scene, and a wall made
   this frame is not in it.
4. **A bounce is a pass over rays already cast** (not "the bake again"). The
   rays are kept while a volume is worked out: first each point holds what it
   sees of the sky and of what glows; then, a bounce at a time, the surfaces
   round it lit by the sun, the lamps and the light the pass before found
   there. `Bounces` 1 is therefore one bounce of everything, the sky's light
   included, and each further bounce costs a fraction of the first.
5. **A point inside something is dropped** when half its rays or more began in
   a solid or met the back of a shell, and takes its neighbours' light. Then
   each point takes a little of the points beside it -- a half of itself, a
   quarter of each neighbour, along each side in turn -- because sixty-four
   rays find a patch of sun with two of them from one point and three from
   the next, and a wall lit by the difference is blotched.
6. **The thin wall**: *a volume a room, and a
   surface reads the volume that the point a quarter of a metre in front of
   it, along its normal, is inside.* The dark side of a wall steps into the
   dark room and reads the dark room's volume whatever the wall's thickness,
   none included. Past a volume's faces its say among volumes is gone within
   five centimetres, so two only blend where both hold the point -- the plane
   of a doorway, which a character crosses with no line across the floor --
   while against the flat ambient a volume fades over its own `Blend`.
   (Five centimetres and not the whole quarter metre: at the whole step the
   floor at the foot of a thin wall was lit a hand's width into the dark room
   by the bright one, and the gate caught it.) One volume stretched over two
   rooms leaks, and the manual says so.

   It is the acceptance picture, kept as a gate (`thin_wall_gate`): a room
   shut on every side beside a room open to the sky, ten centimetres between
   them; the shared wall stays darker than a dim panel that is the measure,
   and with the flat ambient in the volume's place it is four times brighter
   than it.
7. **Light that changes**: a volume has a
   `Dynamic` of its own. It follows the sun by about a degree, the sky's
   colour, and each lamp whose range reaches its box. What it does then is
   light its kept rays again -- the rays towards the sun and the lamps are
   cast, the others are not -- for a millisecond and a half of each frame,
   into a copy; and when the copy is whole, what is drawn goes from the old
   light to the new over half a second. Nothing is drawn half lit. A room of
   two hundred points is whole in a frame or two; a volume at its 4,096,
   timed, in 25 to 39 ms of rays -- seventeen to twenty-six frames. Not `Dynamic`, a volume is right for the light the
   scene opened under and is not lit again: a game with a day and night sets
   it. Its rays are kept only by a `Dynamic` volume: 64 bytes a ray, 16.8 MB
   at 4,096 points.
8. **The setting is the one the model already had**:
   `GraphicsService.GlobalIllumination` (ADR 0147), `High` and above, and no
   `IndirectLight` flag. A phone's levels all say `Medium` for it; a player
   may still raise it.
9. **Where it is kept**: in layers of the environment array (ADR 0202), four
   slices a layer, three texels a point side by side. Sixteen layers:
   sixty-four slices among the volumes in view, four volumes a frame.
10. **Nobody watches a room light up**: as ADR 0202's. `GetProgress` is what
    a game's own loading screen reads.

### Measured

As ADR 0202's, which has the table: a volume a pixel adds 0.03 ms to a
0.23 ms forward pass on a desktop card and 1.3 ms to a 7.1 ms one on
integrated graphics. The rays for 4,096 points take 62 to 69 ms; for the
example's 192, three.

## Amended 2026-10-11: what a game in production found

A game used volumes for real -- maps of two hundred metres, a dusk that
never stands still -- and sent what it met. Each was run before it was
touched. Where this section and the text above disagree, this is what is
built.

1. **A volume gives a room no more light than was put into it, and that is
   now held.** The report was that `Intensity` 1 with two bounces blew an
   interior out, and 0.22 was needed. Run: a closed room of one grey with
   one lamp, whose bounced light is known by radiosity, comes out of the
   whole pipeline -- rays, lamp, bounces, upload, shader -- within a level
   of 255 of the arithmetic (`furnace_gate`, and the unit test of the same
   name). The engine was not adding light. What makes an interior brighter
   under a volume is that the volume is right: walls that give back nine
   tenths give back 1.7 times the lamp's own light after two bounces; a
   place with no roof is open to the sky, which the volume lets in; a
   surface that glows lights the room, where without a volume it lit
   nothing. The manual has how to calibrate, and section 4 below is the
   control that was really being asked for.
2. **A defect, found by that test.** A light that shines every way had a dark
   cone: every `PointLight` left a black disc on the ceiling straight above
   itself, and in a volume a dark cone behind itself. A cone as wide as
   there is has no edge to soften. Fixed in the shader and in the rays.
3. **One volume is lit again at a time, by the light it began with.** Under
   a light that changes every frame -- a torch that flickers, a sun going
   down -- a `Dynamic` volume began to be lit again the frame after it
   finished, was first in the queue being nearest, and took the whole
   budget every frame: a second volume was never lit at all (its progress
   read 0.00 nine hundred frames on), and the first was lit a share a frame
   by a different light each share. Now: a volume takes the light as it is
   when its lighting begins and keeps it until it is whole; one is lit
   again at a time; the next is the one in view that has waited longest,
   then the nearest (`pickRelight`); a volume with no light yet goes before
   any whose light is old. Nothing accumulates: what is not done this frame
   is the same work next frame, not more.
4. **The budget is a setting**: `GraphicsService.GlobalIlluminationBudget`,
   the milliseconds of a watched frame, a quarter to sixteen, 1.5 unless a
   game says; `--global-illumination-budget=MS`, which also makes a run
   with no window keep to it. The gathering is in pieces now, as the rays
   and the lighting were: it was done in one go whatever the budget, and
   was the nine milliseconds of a frame the report saw. Same scene, a
   budget of 1.5 ms: the worst frame's share went from 9.1 ms to 2.5.
5. **`IrradianceVolume.Mode`** (`Enum.IrradianceMode`). `Full`, as before:
   the sky through every opening and everything that bounces, in the place
   of the sky's light and of `Lighting.Ambient`. `BounceOnly`: what bounces
   alone -- off what the sun and the lamps light, and off what glows --
   ADDED to the sky and the ambient, which light the surface as they do
   outside. For a place whose ambient is authored and is to stay so. And
   **`Tint`**, a colour beside `Intensity`, read every frame.
6. **Ready is said**: `IsReady()` and the event `Completed` on a volume and
   on a `ReflectionProbe` (ADR 0202). True from the moment it is worked out
   as it was last asked for; false again from `Refresh` -- or its box, its
   spacing, its bounces or its mode changing -- until that is whole;
   **true through being lit again**, which is upkeep, and `GetProgress`
   stays 1 through it too. On a machine that works none out -- a server, a
   level below `High` -- ready at once: nothing more will be done there.
   (`Ready` would have been the name; the interface's rule is that an event
   is a fact in the past tense.)

Protocol 49 carries `Mode`, `Tint`, `FollowsCamera` and `Levels`. ADR 0206
is the volume that follows the camera, and a box as tiles.
