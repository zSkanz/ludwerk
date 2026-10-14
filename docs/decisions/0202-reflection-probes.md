# 0202 — Reflection probes: what a room reflects is the room

- Status: accepted and built; amended while built, see the end
- Date: 2026-10-10
- Builds on: ADR 0038 and 0043 (the split-sum environment, octahedral), ADR
  0084 (how much sky a surface sees), ADR 0107 (views), ADR 0116 (compute),
  ADR 0147 (settings), R10, R17, R18

## The question

Everything in the engine reflects one thing: the sky. A corridor's metal
door reflects blue. A cave floor is lit from above by a sky it cannot see.
What does a game place so that a room reflects the room?

## What is there

Read before this was written.

- **One environment, and it is the sky.** `environment.h` bakes the sky --
  the analytic one, or a `Sky`'s pictures -- on the CPU into one octahedral
  `Rgba16Float` texture of 128 texels with six roughness levels, nine
  spherical-harmonic coefficients of irradiance, and a split-sum table. It is
  rebaked as the sun moves. Every surface samples it.
- **Screen-space ambient occlusion and the sun's contact shadows exist**
  (`ssao.hlsl`, `contact_shadow.hlsl`), and occlusion multiplies the
  environment and the ambient, never direct light.
- **How much sky a surface sees** (ADR 0084) blends `Lighting.Ambient` and
  `OutdoorAmbient` and scales the sky's light -- and only terrain supplies it.
  A part or a mesh passes one: indoors it takes the whole sky.
- **Nothing reflects the scene.** No screen-space reflections, no probes, no
  planar reflection but `@engine/views`' mirror, which is a camera.
- **The interface below has grown since ADR 0043 was written**: texture
  arrays with mips, mip generation, drawing into one layer of an array,
  compute on every backend. It has no cube texture and no 3D texture.
- **A view is a thing the renderer already draws** (ADR 0107): the scene from
  another camera into a texture, with a history of its own.

## The decision

### 1. `ReflectionProbe`, under a part whose box is its box

A `ReflectionProbe` is parented to a `BasePart`, as a light is. **The part's
box is the probe's box**: where it applies, and the walls its reflections
are corrected against. A room gets an invisible part the size of the room
(`Transparency` 1, `CanCollide` off) with a probe under it -- sized, moved
and turned with the tools a part already has, in the editor and from a
script. Under anything else it does nothing.

| Property | Default | |
|---|---|---|
| `Enabled` | true | |
| `Offset` | 0, 0, 0 | Where the picture is taken from, from the box's middle, in its own space. |
| `Blend` | 1 | Metres over which it fades in from the box's faces, inwards. |
| `Intensity` | 1 | What its light is multiplied by. |
| `Priority` | 0 | Where boxes overlap, the higher wins the overlap's share. |
| `BoxProjection` | true | Reflections are corrected for the box's walls. Off for a probe in the open. |
| `Dynamic` | false | Taken again whenever the light changes, and on `Refresh`. |

`Refresh()` asks for it to be taken again. Nothing else: no resolution a
probe, no layers, no per-probe exposure -- the machine's settings say how
fine and how many.

### 2. What a probe holds

- **Its surroundings, as the sky's are held**: an octahedral map with six
  roughness levels, prefiltered -- a LAYER of one array texture for all the
  probes in view, so a pixel picks a probe by a number and the shader is one
  shader. The sky's own map becomes layer 0 of it.
- **Its diffuse light**: a small octahedral map of irradiance (8 texels a
  side), a layer of a second array. One bilinear tap, where nine
  coefficients a probe would be nine loads.

### 3. How it is taken

Six pictures from its place -- the scene as the main view draws it, at the
probe's resolution, without the passes that are the camera's own (ambient
occlusion, anti-aliasing, bloom, exposure, depth of field) and without what
moves by itself (characters, particles, anything unanchored): what is left is
what a room is. **One picture a frame**, so a probe arrives over six frames
and a scene of them over a few seconds, the sky standing in until each does.
The six are gathered into the octahedral level 0 and the rougher levels are
filtered from it in compute, as the sky's are on the CPU; the irradiance map
is filtered from the last.

A probe is taken when it first comes into use. A `Dynamic` one is taken again
when `Lighting`'s clock has moved the sun by what rebuilds the sky's own map,
when a light inside its box changes, and on `Refresh` -- still a face a
frame. A probe's capture sees the PREVIOUS capture's light in the surfaces it
draws, so a second `Refresh` is a second bounce.

### 4. How a pixel uses it

Probes are put into the light clusters with the lights (`clusters.h`): each
cluster lists the few whose boxes reach it. A pixel walks them: its weight
in each box (zero outside, rising over `Blend`), the direction it reflects in
corrected for the box (`BoxProjection`: where the ray from the pixel leaves
the box, seen from the probe's place), one tap of the specular array at the
roughness's level and one of the irradiance array. Weights are normalised
when they pass one, by priority first; **the sky takes whatever is left**,
scaled as before by how much sky the surface sees. Inside a probe's box the
sky's direct share is the probe's to give: the probe saw the sky through the
windows it has.

At most two probes a pixel are blended; a cluster lists up to four.

### 5. What it costs, and where it is off

`GraphicsService.ReflectionProbes` (and `--[no-]reflection-probes`): off at
Low, on from Medium. Off, no probe is taken, no array is made, and the
shader is the one that ships today. The resolution and how many probes may
be live at once are the preset's: 64 texels and 8 at Medium, 128 and 16 at
High, 128 and 32 at Ultra; a handheld's presets halve the count. A probe
past the budget is the furthest from the camera and is the sky.

Measured, before it is called built, on Direct3D 12 and on Vulkan: what a
frame costs with no probe in view (the shader's branch), with one and with
two a pixel; what one face of a capture costs; the memory of the two arrays.
The numbers go here.

### 6. Simulation, or picture

Picture. Nothing a script can read changes with it but the properties it
wrote. The class travels as authored (protocol 46), each machine taking its
own pictures.

### 7. The interface below

`ComputeTextureWrite` gains `layer`: a compute pass writes one layer of an
array, at one mip. Both backends have it underneath; the frozen interface
(ADR 0037) had no word for it.

## Not decided here

- **Screen-space reflections**: what a probe cannot show -- a character in a
  wet floor. A pass of its own, later, on top of this.
- **A probe that follows the camera**, for an open world with no rooms.
- **Baking to disk.** A probe is taken at run time, every run. Kept on disk,
  a game would start with its rooms already lit; it needs the capture read
  back, and a place in the export.

## Decided while built (2026-10-10)

What the code said when it was written. Where this section and the text above disagree, this section is what
was built.

1. **The setting is the one the model already had.** ADR 0147 reserved
   `GraphicsService.Reflections`, an `Enum.GraphicsLevel`, "kept and not
   applied". It is applied now, and no `ReflectionProbes` flag was added: `Low`
   is the sky and no probe taken, `Medium` eight probes, `High` sixteen,
   `Ultra` thirty-two. `--reflections=LEVEL` on the command line, and
   `reflections = "..."` under `[graphics]`.
2. **The level's size is the pictures', not the map's.** A probe's six pictures
   are 64 texels a side at `Medium` and 128 above; the map they are gathered
   into is always the sky's own 128, because it is a layer of the sky's
   texture.
3. **One array for every view.** The environment texture was one a view; it is
   one for the renderer. Its first layer is the sky of the view being drawn --
   each view keeps its own cache of the sky's levels, and a level is uploaded
   when what the layer holds is not what that view's cache holds, which under
   one sky is never. Then two layers a probe (its reflection by roughness; its
   diffuse light, eight texels a side, at one coarse level of the second), and
   then the irradiance volumes' slices (ADR 0203). **No texture binding was
   added to any pipeline**: the slots after the engine's thirteen are a
   surface shader's own, and the terrain's.
4. **No compute, and no read-back** (section 7 is withdrawn). Direct3D 12
   binds a whole level of an array to a compute pass and SDL one layer, so
   neither writes "one layer at one level" the same way; and reading the
   pictures back to filter them on the processor would stop the frame every
   time a `Dynamic` probe followed the sun. The six pictures are gathered and
   filtered by ordinary full-screen passes into textures of their own, and
   copied into the probe's layers by one new word of the interface,
   `ICmdList::blitTextureLevel(source, sourceLevel, destination, layer, level)`.
5. **The table of probes rides the frame's uniform block**, eight a frame,
   the nearest in view, not the light clusters (section 4): a frame has few
   probes and a box test is cheap, and the clusters' three textures stay what
   they were.
6. **A surface reads the probe that the point a quarter of a metre in front
   of it is inside** -- ADR 0203's rule for a volume, for the same wall.
   `Blend` therefore fades OUT past the box's faces, where no other probe is,
   and not in from them: a room's own floor and walls lie on its box, and
   under the first rule they were the surfaces a probe applied to least. Two
   probes whose boxes meet give way to each other across five centimetres
   either side of the meeting.
7. **Every taking is two.** The first pictures of a room have only the sky for
   the room's own shiny surfaces to reflect, and a mirror then shows a bright
   floor in a dark room. So a probe is taken twice at first, and twice when it
   is taken again -- `Refresh`, or the light changing under a `Dynamic` one.
8. **Ambient occlusion stays in a picture.** It is part of how the room is
   lit, and at 128 texels it costs nothing to leave. What is left out is what
   is a camera's own: the look, exposure, bloom, the tone curve, anti-aliasing.
9. **What moves is left out by what it is**: a draw with a skeleton, a part
   that is not anchored, a `CharacterBody`.
10. **Nobody watches a room light up.** Behind
    the loading curtain -- and in a run with no window -- pictures are drawn
    for as much of each frame as twelve milliseconds allow, and the curtain
    does not lift while a probe in view of the camera, or within six metres of
    it, has not been taken twice. One picture a frame is for a world a script
    builds while it is watched, where a new probe comes in over half a second.
11. **A phone's levels keep half as many, and none below `High`**, from the
    measurement below.

### Measured

Release build, no window, 1920 by 1080, the whole screen inside a room
(`examples/39-interior`). The desktop card is an RTX 4070 Ti SUPER through
Direct3D 12. Vulkan with no window can only be driven on this machine's
integrated graphics (Intel UHD 770) -- the desktop card's Vulkan driver has no
headless surface -- so the two columns are two graphics cards as well as two
interfaces, and the second is the nearer to a phone.

| Forward pass, GPU | Direct3D 12, desktop card | Vulkan, integrated |
|---|---|---|
| No probe in the frame | 0.23 ms | 7.1 ms |
| A probe a pixel | 0.26 ms | 8.4 ms |
| A probe and a volume a pixel | 0.29 ms | 9.5 ms |

Two probes a pixel happens only in the ten centimetres where two boxes meet.
A scene with no probe in it draws through the arithmetic it always did: the
forward pass timed the same as the release before this, within the noise of
three runs, and a picture of it is the same picture, every pixel.

One picture, all in, taken flat out: about a quarter of a millisecond on the
desktop card, about three on the integrated one.

Memory, 174,763 bytes a layer: 0.35 MB at `Low` (it was 0.17), 3.0 MB at
`Medium`, 8.6 MB at `High` and 14.2 MB at `Ultra`, volumes' layers included.

### Baking to disk: not built

The rule was that an ordinary case over about a second behind the curtain
puts a bake to disk. From scene open to every room
in view lit, flat out:

| | Direct3D 12, desktop card | Vulkan, integrated |
|---|---|---|
| The example: two probes, two volumes of 192 points | 43 ms | 51 ms |
| One volume of 4,096 points and thirty-two probes, at `Ultra` | 154 ms | 1,186 ms |

Nothing ordinary is near a second; the one case over it is the limit of both
classes at once, at a level an integrated card does not run. So nothing is
kept on disk. If it is ever wanted, what makes a bake stale is: the scene's
anchored geometry and its materials' colours, the box and the settings of the
probe or volume, the sun, the sky and the lamps that reach the box, and the
level of the setting -- a sidecar would carry a hash of those and be ignored
with one warning when it did not match.

## Amended 2026-10-11

`ReflectionProbe:IsReady()` and the event `Completed`, as ADR 0203's
amendment of the same day has them for a volume: true once its pictures are
taken -- both takes -- as it was last asked for, false from `Refresh` until
they are taken again, and true through being taken again because the light
moved. True at once on a machine that keeps no probes, and for a probe past
the number the machine keeps: nothing more will be done for either.
