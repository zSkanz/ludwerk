# 0209 — Reflections and bounce from the screen

- Status: accepted; built as the head of category 4
- Date: 2026-10-12
- Builds on: ADR 0202 (reflection probes), ADR 0203 (irradiance volumes), ADR
  0090 and 0091 (materials and the surface a user writes), ADR 0146 (water)

## The question

A `ReflectionProbe` is a room seen from one point. A polished floor reflects
the room and not the character standing on it, nor anything that moved since
the picture was taken. An `IrradianceVolume` holds bounced light at points
metres apart: the red of a wall on the floor at its foot is below its grain.
A lake reflects the sky and never the far bank.

All three answers are on the screen already, where the screen has them.

## The decision

### 1. What is read instead of a G-buffer

The renderer is forward, with a depth prepass that has no fragment work, and
it stays that.

- **Depth**: the prepass's, as the occlusion pass and the contact shadow
  already read it.
- **Colour**: the lit picture of the *last* frame -- as the lit and blended
  passes left it, before bloom, tone and the interface -- kept one frame at
  the picture's size and found again by the camera that drew it. Not the
  temporal pass's history: that pass is one of five anti-aliasing modes, the
  upscaler keeps its history at another size, and three modes keep none. One
  picture for every mode is one thing to test.
- **Normals**: rebuilt from depth, as the occlusion pass rebuilds them.
- **Roughness**: in no target, which is the real cost of having no G-buffer.
  A pass before shading cannot know how rough a pixel is. So the pass marches
  the *mirror* direction, and the lit shader -- which does know -- reads the
  result a level further down its chain the rougher its surface is.

Refused: a thin prepass target for normal and roughness. It would give
normal-mapped rays and let the march skip rough pixels, and it would cost
every prepass pipeline a fragment shader that samples two textures -- plain,
skinned, morphed, instanced, the ground, the blocks, the grass, and every
surface shader a user wrote, run twice a frame. That is the prepass roughly
doubled to make one optional effect a little better. What is lost without it
is two things, and the second is section 6's: a normal map does not bend the
ray, and **a normal rebuilt from depth is the mesh's facet, not the normal a
smooth surface is shaded by**.

### 2. Reflections

Before the lit pass, at half the picture's size, each pixel's mirror ray is
marched through the depth (`screen_trace.hlsl`): steps that grow, then the
landing halved a few times, and a landing kept only when it is ON a surface
-- the ray and the depth under it meeting, not the ray passing behind
something. Where it lands is looked up in the last frame's picture and
written as that colour *times how sure the landing is*, with how sure in
alpha -- so the chain made of this picture blurs a landing into a miss
without darkening it.

How sure falls to nought at the screen's edge in either frame, at the end of
the ray's reach, for a ray that turns back to the camera, and on the sky.

**It is gathered over frames by its own answer of the frame before.** A ray's
start differs every frame, and each answer is a mean with the answers before
it: from its first frame at full strength, for about ten frames under a
camera that stands still, and for one or two under one that moves -- under a
moving camera a reflection moves as nothing else on its surface does, and an
old answer is a smear. Without it the far end of a reflection was a ragged
fringe that never settled; with it the fringe is gone at rest under every
anti-aliasing mode, none included.

The lit shader reads it at its own pixel, as it reads the occlusion texture,
and puts it in place of that much of what the probe or the sky said:
**the screen first, the probe where the screen has no answer, never black.**
Past a roughness of 0.6 the screen has nothing sharp to say and it is the
probe's alone.

### 3. Bounce

The same inputs (`screen_bounce.hlsl`): a few cosine-weighted rays a pixel,
three metres long, at half the picture's size. Where one lands, the last
frame's picture says what light comes from there. Written as the mean of what
the landed rays saw times the share that landed, and that share in alpha.

Gathered as the reflection is, and for longer -- ten frames under a camera
that moves, thirty-odd under one that stands -- **with its four
neighbours on the same surface**, which turn and stand nearer or further by
pixel and by frame. Four neighbours at four fixed places leave one pattern
exactly as they found it (one that repeats every three texels both ways), and
the grain settled into that lattice: a field of dots round every lamp, however
long it was gathered.

In the lit shader the screen's share **replaces** that share of what a volume
or the ambient light gave: the same light is never counted twice. **Nor is it
darkened twice**: the directions the screen's occlusion counts as shut are
the ones a ray landed in, and what it landed on is already as dark as it is.

It feeds itself -- the last frame's picture holds the last frame's bounce --
and that is a second bounce and is right. What is read is held to what a lamp
is and taken six parts in a hundred short, so a rounding upward cannot
compound.

### 4. Water, and any blended surface that asks

A blended surface is not in the depth the screen's own pass marches, but its
own shader knows where it is and which way it faces. The surface contract
gains (1.2):

- `sceneReflectionAt(inputs, normal)`: marches the scene behind the surface
  from the surface itself, through the scene's depth as it was copied for the
  blended pass, and reads the scene's colour of *this* frame -- which is
  there, since everything opaque is drawn. A lake's reach, not a room's.
- `SurfaceOutput.Reflection`: a colour and how much of what the surface would
  reflect of the sky and the probes it takes the place of.

**The ray is stepped along the screen, not along itself.** A step of so many
metres is, far out, a step of many pixels: the ray strode over a post on the
far bank or happened to stop behind it, by which pixel it began at, and the
post stood in the water as a stack of chips. Walked across the picture in
even steps, with one over the depth running even beside them, each step is
the same few pixels wherever it is.

Two rays have no answer on the screen, and are a miss rather than a guess: one
that a wave's slope turns down into the water (marched, it lands on the
lake's own bed, which the scene behind the water holds), and one whose
landing is not ON anything -- a ray that only passed behind the bank takes
the bank's colour to water that does not reflect it, and a lake striped with
its own shore is worse than a lake that shows the sky.

The engine's water writes one into the other, less where there is foam. Glass
and particles ask for nothing and keep the probes. The march is the setting's
to switch, as the pass is, and answers nothing for a material that did not
ask for the scene's colour.

### 5. A setting each, and who turns them on

`GraphicsService.ScreenSpaceReflections` and `ScreenSpaceBounce`, each an
`Enum.GraphicsLevel`. `Low` and `Medium` are off. Reflections take 24 steps
at `High` and 48 at `Ultra` (water 24 and 32); bounce casts 4 rays a pixel at
`High` and 8 at `Ultra`. In a project's file, `[graphics]
screen_space_reflections` and `screen_space_bounce`; on the command line,
`--screen-reflections=LEVEL` and `--screen-bounce=LEVEL`.

**Only the `Ultra` preset turns either on**, and it turns on both. Decided on
pictures of three real scenes, not on a floor and two boxes: at `High` the
courtyard of `42-nightfall` was the same picture with reflections on as off
(nothing in it is smoother than 0.6), the lake of `31-lake-and-river` from
its own camera changed by one thin line, and `39-interior` was worse in one
place until section 6's rule. A level that pays the passes in two scenes of
three and gains nothing is not a default. So nothing a game ships at `High`
changes its look or its frame time, no picture of the default moves, and a
game that wants its floors to reflect at `High` says so in one line.

**A phone starts with both off at every level**, which is a statement about
not having measured, not about phones.

**With both off, nothing is kept and nothing is copied.** The last frame's
picture and the two passes' pictures exist only while a setting reads them.

### 6. Who reads the screen

A stage has sixteen samplers. The built-in surface and a surface shader a
user wrote read both pictures. The ground (`terrain.hlsl`), the blocks and
the grass do not: the ground's and a blended surface's samplers are spent,
and the lean ones are lean on purpose. What those reflect is the probes' and
the sky's, as before. A part standing on the ground takes the ground's
bounced light from a volume, not from the screen.

**And a curved surface shaded smooth keeps the probe.** The pass rebuilds its
normal from depth, and depth is the mesh: on a ball that is the facet's
normal. A chrome ball in the first real scene came out cut into flat patches
-- each facet a little mirror facing its own way -- where the probe, read
through the shading normal, had shown a smooth one. The surface says what it
is: where its vertex normal -- as the mesh has it, before any normal map --
turns across the screen, it is curved and shaded smooth, and the screen's
reflection is weighed to nothing. As turns to a metre, so near and far give
one answer: a radius of fifty metres and more is flat, of twelve and less is
not. Floors, walls, a box's faces and water take the screen; balls, pillars
and characters keep the probe, exactly.

### 7. Under split views

A view has its own last frame and its own march, as it has its own depth.
Where one picture is parted by a line, the far side of the line is the near
plane in a view's depth: a ray that lands there has left its camera's
picture and is a miss. A view never shows another view's world in its floor.

## What it costs

Measured as the passes' own time, each less the stop the measuring carries
(`--gpu-pass-times`), at `Ultra`, over `39-interior`, `42-nightfall` and the
lake of `31-lake-and-river` from above and from its shore. The lit pass's two
more reads are inside the noise of that method and not in these numbers.

| | Desktop card (Direct3D 12) | Integrated graphics (Vulkan) |
|---|---|---|
| Reflections, 1920 by 1080 | 0.15 to 0.34 ms | 2.5 to 3.0 ms |
| Bounce, 1920 by 1080 | 0.45 to 0.74 ms | 4.8 to 11.5 ms |
| Both, 1920 by 1080 | 0.66 to 1.06 ms | 6.8 to 13.4 ms |
| Reflections, 2560 by 1440 | 0.36 to 0.67 ms | 4.5 to 5.6 ms |
| Bounce, 2560 by 1440 | 0.69 to 1.56 ms | 9.4 to 22.1 ms |
| Both, 2560 by 1440 | 1.26 to 2.07 ms | 13.1 to 26.3 ms |

The reflection's share is the march, the copy of the last frame's picture
(about 0.1 ms) and its chain; the bounce's is sixty-four reads of depth a
pixel of a picture a quarter the size. Water's own march, a lake across a
band of the screen at 1920 by 1080: 0.05 ms on the desktop card and 2.6 ms on
the integrated one.

The two columns are two machines, not one machine twice: what Vulkan picks
here is the integrated graphics. `Ultra` is not that machine's level, and the
numbers are why nothing below `Ultra` turns either on.

Memory, at 1920 by 1080: the last frame's picture (16 MB), the reflection's
two pictures with their chains (about 11 MB), the bounce's two (8 MB). None
of it with both settings off.

## How it is held

- `screen_light_gate`, and `screen_light_gate_vulkan` on Windows
  (`tests/screenshots/run_screen_light_gate.cmake`, one project,
  `tests/screenshots/screenlight`):
  - **the floor**: a polished floor under a probe, and a box moved after the
    probe took its picture. With reflections on the floor shows the box where
    it stands and the wall where the probe still has it; off, it is the
    probe's picture. Where the screen has no answer the floor is the probe's
    to the pixel, and not black;
  - **the ball**: inside a chrome ball, the picture with reflections on is the
    picture with them off. Seen to fail first: with section 6's rule off, 273
    of the 1,521 pixels asked differed;
  - **the wall**: the floor at the foot of a glowing red wall is reddened with
    the bounce on and not with it off -- by the flag, and by the quality level
    put to `Ultra` from a script while the game runs; and not when the level
    is pinned on the command line;
  - **the lake**: a flat lake held against the scene through the camera
    mirrored in the water's plane, by `imgmirror` (`tools/imgcmp`): every
    pixel the reflection wrote is one where the mirror shows a thing, and
    went that thing's way. Seven of 9,773 are wrong, all on the last row of
    water against the bank. Seen to fail first: with the test that a landing
    is ON a surface switched off, 3,248 of 13,098 -- a beam is hung over the
    water where a ray passes behind it;
  - **off**: with both settings off the frame has no pass of the screen's
    light and no copy of the last frame, by its own list of passes.
- The two graphics APIs were found to disagree before both were run: half the
  picture's size puts a pixel's centre exactly on the line between two of the
  depth's texels, which of them `floor` took was the last bit of an
  interpolated coordinate, and the answer of the frame before was found from
  that texel's place -- a quarter of a pixel off, which gathered is a creep.
  The glow at a wall's foot stood eight pixels to one side of the wall on one
  graphics card and to the other side on another. The texel is chosen a
  quarter short of the line and the answer found where the pixel was; the two
  agree to four levels of 255.
- `options_screen_gate` pins both off, as it pins one anti-aliasing: what is
  gathered over frames differs by when it was chosen, which is not that
  gate's question.
- `tests/conformance/world/indirect_light.spec.luau`: the two settings are
  applied, read back and written.

## Not decided here

- Rays against the world itself, in hardware or software.
- The ground reading the screen: it wants the ground's samplers counted
  again, or the two pictures packed into one
 .
- A target for the shading normal, which would let a ball reflect the screen.
- A reflection on a blended surface of what is *blended* behind it: the
  scene it marches is the opaque one.
- Either setting at `High`, or on a phone: when it has been looked at there.
