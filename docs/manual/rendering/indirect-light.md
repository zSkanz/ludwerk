# Indirect light: what a room reflects and gives back

Without it, everything reflects one thing and is lit from one place: the sky. A
metal door at the end of a corridor reflects blue; a room with one window is as
bright in its far corner as at the window; a red carpet does not redden the
wall beside it.

Two classes change that, and both are placed the same way — **under a part
whose box is their box**:

| | What it gives | Worked out by |
|---|---|---|
| `ReflectionProbe` | What shiny surfaces reflect, and — where there is no volume — the light matte ones get | Six pictures taken from inside the box |
| `IrradianceVolume` | The light every surface gets from everything but the lamps and the sun themselves: light that has bounced | Rays, on the processor, from points a `Spacing` apart |

`examples/39-interior` is two rooms with one of each.

## A room is an invisible part

```luau
--!strict
local function room(center: vector, size: vector)
    local box = Instance.new("Part")
    box.Size = size
    box.Position = center
    box:SetMaterialParameter("Transparency", 1)
    box.CanCollide = false
    box.CanQuery = false
    box.Anchored = true
    box.Parent = workspace

    local probe = Instance.new("ReflectionProbe")
    probe.Parent = box

    local volume = Instance.new("IrradianceVolume")
    volume.Spacing = 1
    volume.Parent = box
end

room(vector.create(-4, 1.6, 0), vector.create(8, 3.2, 8))
room(vector.create(4, 1.6, 0), vector.create(8, 3.2, 8))
```

The part is sized, moved and turned like any part, in the editor and from a
script. It is wholly transparent, so light passes it; **turn `CanQuery` off as
well**, so the game's own rays do not meet a wall that is not there.

Size the box **to the room**: its faces on the inside of the room's walls, or
anywhere within their thickness. Two rooms that share a wall get two boxes that
meet inside that wall.

## The thin wall

A surface does not read the box it is in. It reads the box that **the point a
quarter of a meter in front of it, along its normal,** is in. That one rule
does three things:

- A room's own floor and walls lie *on* its box, and are the room's.
- The dark side of a wall between a bright room and a dark one steps into the
  dark room and reads the dark room's volume, **however thin the wall is**. A
  wall of ten centimeters leaks nothing; so does a wall of none.
- Where two boxes meet in the open — a doorway — one gives way to the other
  across five centimeters either side of the meeting, so a character walking
  through is lit by one room and then the other with no line across the floor.

What it cannot fix is **one volume stretched over two rooms**. Inside one
volume, light is read between its points, and a point in the bright room and a
point in the dark room on either side of a wall are blended across it. A
volume a room.

## `ReflectionProbe`

| Property | Default | |
|---|---|---|
| `Enabled` | true | Off, its box is the sky's again. |
| `Offset` | 0, 0, 0 | Where the pictures are taken from, from the middle of the box, in the box's space. Move it when the middle is inside a pillar. |
| `Blend` | 1 | Meters over which it fades out past the box's faces, where no other probe is. |
| `Intensity` | 1 | What its light is multiplied by. |
| `Priority` | 0 | A whole number. Where two boxes overlap the higher takes the overlap: a small probe round a mirror, inside the room's own. |
| `BoxProjection` | true | Reflections are corrected for the box's walls, so a floor reflects the wall where the wall is. Off for a probe in the open. |
| `Dynamic` | false | Taken again when the light changes. |

`probe:Refresh()` takes it again: after a wall falls, a door opens, a room is
built.

A probe's pictures are of **what is anchored**. A character, a crate that can
be pushed and anything with a skeleton are left out, or they would stand in
every reflection where they stood when it was taken. What a probe cannot show
— a character in a wet floor — is what the screen shows instead: see *From
the screen*, below.

**A probe is not a mirror.** Its picture is 128 texels a side for the whole
sphere, a texel about a degree and a half: a polished floor reflects the room
from it softly, a shape with a smooth edge and no detail in it. What makes a
true mirror is the screen (see *From the screen*, below).

A room's pictures are taken twice the first time, and twice whenever they are
taken again: the first set has only the sky for the room's own shiny surfaces
to reflect, and the second has the first.

## `IrradianceVolume`

| Property | Default | |
|---|---|---|
| `Enabled` | true | |
| `Spacing` | 2 | Meters between its points along each side of the box; no finer than 0.5. |
| `Blend` | 1 | Meters over which it fades out past the box's faces, where no other volume is. |
| `Intensity` | 1 | What its light is multiplied by. One is what the arithmetic says. |
| `Tint` | white | A color its light is multiplied by. Read every frame: nothing is worked out again for it. |
| `Mode` | `Full` | `Full`, or `BounceOnly`: see below. |
| `Bounces` | 1 | How many times light is let bounce, 1 to 3. |
| `Dynamic` | false | Lit again when the light changes. |
| `FollowsCamera` | false | Carried with the camera: light that bounces in the open. See below. |
| `Levels` | 1 | With `FollowsCamera`, 1 to 3: each twice the last. Each keeps its rays: 13 MB a level of 3,200 points. |

`volume:Refresh()` works it out again. `volume:GetProgress()` is how much of it
is done, 0 to 1. `volume:IsReady()` is whether it is worked out, and
`volume.Completed` fires when it becomes so:

```luau
if not volume:IsReady() then
    volume.Completed:Wait()
end
```

Ready means *as it was last asked for*: false again from `Refresh` -- or from
its box, `Spacing`, `Bounces` or `Mode` changing -- until that is whole. Being
lit again because the light moved is upkeep, and it stays ready through it. On
a machine that works none out it is ready at once. A `ReflectionProbe` has the
same two.

One volume holds **4,096 points, and 32 along a side**. A box that `Spacing`
would give more is worked out as **tiles** -- as few as hold it, up to sixteen,
each giving way to the next where they meet, with nothing to set and no line to
see. The nearest eight volumes to the camera are drawn, a tile being one. A box
that would take more than sixteen is sampled more coarsely, and says so once in
the log. A meter is right for a room; two for a hall.

### `Full`, or what bounces alone

`Mode = Full`: inside the volume, a surface's diffuse light is the volume's --
the sky through every opening and everything that bounces -- **in place of the
sky's and of `Lighting.Ambient`**, which were standing in for it. A room with
no window and no lamp is dark, as it should be.

`Mode = BounceOnly`: the volume works out only what bounces -- off what the sun
and the lamps light, and off what glows -- and it is **added**. The sky and
`Lighting.Ambient` light the surface exactly as they do outside the volume.
For a place whose ambient light is authored, by hand or by the hour, and is to
stay as authored: the volume then only ever adds the red of the red wall.

### How bright a room comes out

**A volume gives a room no more light than was put into it.** That is tested:
a closed room of one gray with one lamp comes out of the engine within a
level of 255 of what the arithmetic says it must. So when a room lit through
a volume is brighter than it was without one, the volume is telling the truth
about one of these:

- **The walls.** A surface gives back its color's share of what lands on it,
  each bounce. Walls at 0.5 give back three quarters of the lamps' own light
  after two bounces; white walls at 0.9 give back 1.7 times it. A room
  authored against a flat ambient was authored with none of that.
- **The sky.** Under `Full` a ray that meets nothing sees the sky, and the
  volume's light takes the place of `Lighting.Ambient`. **A scene exposed for
  a flat ambient somebody typed goes brighter under `Full`**, because the
  sky's real light is more than that number was -- and a set with no roof,
  for a camera that looks down into it, is open to all of it.
  **`BounceOnly` is the mode for a place whose tone lives in its authored
  ambient**: the ambient stays exactly as typed, and the volume adds only
  what bounces.
- **What glows.** A surface's `Emissive` lights the room it is in. Without a
  volume it lit nothing, so a panel made bright for the bloom, beside the lamp
  that was standing in for it, is now two lamps.
- **Every lamp in reach** of the box is counted; the picture has a budget of
  lamps (`GraphicsService.LightBudget`).

To calibrate: set the walls' colors to what the surfaces are, not to what
looked right under a flat ambient; leave `Intensity` at 1; and use `Tint` and
`Intensity` after that, for a look.

## In the open: a volume that follows the camera

A street, a square, a clearing: nowhere to draw a box round. Set
`FollowsCamera` and the volume stands round the camera instead -- the part
gives its **size**, and where the part is no longer matters.

```luau
local size = Instance.new("Part")
size.Size = vector.create(40, 16, 40)
size.Transparency = 1
size.CanCollide = false
size.CanQuery = false
size.Anchored = true
size.Parent = workspace

local open = Instance.new("IrradianceVolume")
open.FollowsCamera = true
open.Levels = 3
open.Spacing = 2
open.Dynamic = true
open.Parent = size
```

Each level after the first is twice as large round the same middle, with its
points twice as far apart: these three are 40, 80 and 160 meters wide at 2, 4
and 8 meters, 3,200 points each. A surface is lit by the finest level it is
inside, and each level gives way to the wider one over its last two cells.

A level stands on a grid of its own cells, so its points do not move as the
camera turns or steps; when the camera has gone four cells the level is
carried after it. The points it had in common with where it stood keep their
rays, so a step costs the fifth that is new -- about a quarter of a second at
the default budget, during which the wider levels cover.

**A room's own volume has the room.** The levels light what no other volume
covers, and a point every two meters knows nothing of a wall ten centimeters
thick: a house in the open wants a volume of its own, as it did.

It is on where volumes are on. Three levels are 39 MB of kept rays.
`examples/42-nightfall` is a courtyard lit this way as the sun goes down.

### What a ray meets

**What is drawn and does not move blocks light and gives it back; what is half
transparent or more lets it by.** Whichever class it is, and whatever its
collision is set to: a wall with `CanCollide` and `CanQuery` off still casts a
shadow in the picture, and so it does here.

| What | To a ray |
|---|---|
| An anchored `MeshPart` | Its own triangles: a doorway and a window modeled into one mesh let the light in, whatever its `CollisionFidelity`. |
| An anchored `Part` -- block, ball, cylinder, wedge | Its exact shape. |
| Terrain | Its surface, in one earth color. |
| Anything unanchored, and a `CharacterBody` | Nothing: it will not be there next frame. |
| A part half transparent or more, and a section of a mesh whose material blends and is mostly clear | Nothing: glass lets light through. |

A surface bounces **its color times the mean of its picture**: a tiled floor
gives back terracotta, a brick wall brick. The mean is read off the texture
when it is loaded; nothing is asked of the model. A surface also gives off its
`Emissive`, so a glowing panel lights the room it is in.

A building that arrives as **one mesh** is lit through the openings it has:
`examples/39-interior`'s annex is a single file -- walls, ceiling, a tiled
floor, a window and a door -- with a volume over it like any other room.

## Light that changes

A `Dynamic` probe or volume follows the light it is under: the sun moving by
about a degree, the sky changing color, a lamp that reaches its box switched
on, moved or recolored.

- A **volume** keeps the rays it cast and lights what they met again — a share
  of it each frame — and when the new light is whole, what is drawn goes from
  the old to the new over half a second. A room of two hundred points follows
  a lamp within a few frames; a volume at its 4,096 is lit again in about
  twenty, and then goes from old to new. Nothing pops.
  **One volume is lit again at a time, by the light there was when its turn
  began.** Under a light that never stands still -- a torch that flickers, a
  sun going down -- each volume in view takes its turn, the one that has
  waited longest first, and what is drawn is always a whole volume under one
  moment's light. The work never piles up: what is not done this frame is the
  same work next frame.
  `GraphicsService.GlobalIlluminationBudget` is the milliseconds of a frame
  all of this may take, a quarter to sixteen, a millisecond and a half unless
  the game says: less, and the light follows more slowly.
- A **probe** is taken again once the volumes in view have settled, a picture
  a frame.

Not `Dynamic`, each is worked out once, under the light there was when the
scene opened — and is then right at noon and wrong at dusk. **A game with a day
and night cycle sets `Dynamic` on the rooms it lets the player into.**

What changes the *geometry* — a wall built or destroyed — is not followed by
itself: call `Refresh`.

## Kept in a file

A room whose light does not change need not be worked out every time it is
opened, and a phone need not work it out at all. Give the volume a file:

```luau
volume.Baked = "asset://light/hall.irradiance"
```

and bake it -- **Bake** on the volume in the editor's Properties, which names
a file for a volume that has none, or for every volume of a project at once:

```
engine-host <project> --bake-irradiance
```

That opens the project as a game is opened, with no window, works out every
volume that names a file whatever that machine's settings are, writes each
under `content/`, and exits. It needs a graphics device.

**A machine that works volumes out** reads the file and casts no ray: the
room is lit the frame it is first seen, and the log says so once. `Dynamic`,
the file is its first light and it is lit again as the light moves; `Refresh`
works it out by rays.

**A file baked again is the room's light at once**: from the editor's Bake,
or written on disk while `ludwerk dev` runs, every volume that names it is
given the new light, on either kind of machine. The editor's Bake on a
machine that works none out works out the volume it was asked for, and every
other volume stays the color of its own file.

**A machine that works none out** -- `GlobalIllumination` below `High`, which
is every phone unless a player turns it up -- reads one color of the file:
the mean of the whole. The room is lit by that where it had the flat ambient.
It is not the light of each corner; it is the room's own tone, where there
was the sky's, and it costs a frame nothing.

The file is about forty-eight bytes a point: 25 KB for a room of 512 points,
200 KB for a volume at its 4,096.

**Bake again after changing a room.** The file is the light of the volume it
was baked for and the room that stood in it:

- A volume with another box, `Spacing`, `Bounces` or `Mode` does not take the
  file's light. It says so once and is worked out as a volume with no file is.
- In the editor and in a development build, a volume says once when what
  stands in its box is not what stood there: a wall moved, added, taken away
  or resized. The file's light is still drawn.
- **Nothing notices** a wall repainted, a lamp moved or recolored, the sun
  at another hour, or the ground of a `Terrain` reshaped. The light in the
  file is the light there was when it was baked.

Not for a volume with `FollowsCamera`, which is another place every frame.

## Nobody watches a room light up

When a scene opens, the pictures are taken and the rays cast **behind the
loading screen**, as much each frame as a frame has room for, and the screen
does not lift while a probe or a volume in view of the camera is still the sky
standing in. The example's two rooms are lit in about a twentieth of a second;
a volume at its 4,096 points with thirty-two probes in about a sixth, on a
desktop graphics card.

Only what a script builds *while the game is being watched* arrives as it goes:
a new probe or volume comes in over half a second.

`SceneService:HoldLoading` and `GetProgress` are there for a game that wants
its own word on it.

## From the screen

A probe is a room seen from one point, once. A volume holds bounced light at
points meters apart. What neither can hold is on the screen already: the
character standing on the polished floor, the crate that was pushed since the
probe was taken, the far bank in the lake, the red of a wall on the floor at
its foot.

Two settings read it, each an `Enum.GraphicsLevel`:

- **`GraphicsService.ScreenSpaceReflections`**: each pixel's mirror ray is
  walked through what is on the screen, and where it lands is what a smooth
  surface reflects there, in place of that much of the probe's answer. A
  rougher surface reads it blurred, and past a roughness of 0.6 it is the
  probe's alone.
- **`GraphicsService.ScreenSpaceBounce`**: each pixel casts a few short rays
  -- three meters -- and the light of what they land on is the light that
  bounces to it, in place of that share of what a volume or the ambient light
  gave. The same light is never counted twice.

**Where the screen has no answer the probe and the volume keep theirs, never
black**: a ray that leaves the picture, one that turns back to the camera, one
that would land on what is hidden behind something else.

**What reads the screen, in one sentence: parts, meshes and characters drawn
with the built-in material or with a surface shader of your own take both;
water reflects the screen from its own shader; the ground of a `Terrain`,
blocks and grass take neither, and are lit by probes and volumes alone.** The
bounced light a volume gives the ground is exactly what it was. And a curved
surface shaded smooth -- a ball, a pillar, a character -- keeps the probe's
reflection: the ray is walked from the mesh's own facets, and a chrome ball
reflected through them is cut into flat patches.

Both are **off at every quality level but `Ultra`**, which turns both on, and
off at every level on a phone. With both off nothing is kept and nothing is
copied: the frame is the one it was before there was either. A game that
wants its floors to reflect at `High` says so:

```luau
GraphicsService.ScreenSpaceReflections = Enum.GraphicsLevel.High
```

```toml
[graphics]
screen_space_reflections = "high"
screen_space_bounce = "low"
```

`--screen-reflections=LEVEL` and `--screen-bounce=LEVEL` on the command line.
`Low` and `Medium` are off; `High` walks twenty-four steps a ray and casts
four rays a pixel, `Ultra` forty-eight and eight.

What to know of them:

- **They are a frame behind, and gathered over a few.** What a ray lands on
  is read from the last frame's picture, and each answer is gathered with the
  answers before it -- for ten frames, and for thirty or so while the
  camera stands still. After a cut, or when the setting is switched on, the
  bounce round a lamp takes that long to settle.
- **A normal map does not bend the ray.** A floor's bumps blur what it
  reflects, by its roughness; they do not ripple it.
- **Water is the exception to "a frame behind"**: it is drawn after
  everything solid, so it reflects this frame's.

In a surface shader of your own, a blended surface asks for the same thing
water does: `sceneReflectionAt(inputs, normal)` and `surface.Reflection`
(see [surface shaders](manual:rendering/surface-shaders)).

## What it costs, and where it is off

Two settings, each an `Enum.GraphicsLevel`, each set by the quality level and
changed by a player's options:

| | `Low` | `Medium` | `High` | `Ultra` |
|---|---|---|---|---|
| `GraphicsService.Reflections` | the sky only | 8 probes, pictures 64 texels a side | 16 probes, 128 | 32 probes, 128 |
| `GraphicsService.GlobalIllumination` | none | none | volumes | volumes |

On a phone a level keeps half as many probes and none below `High`, and no
level starts with volumes on. **Where volumes are off, a volume with a file
is its file's one color** (see *Kept in a file*): the row of `none` above is
none worked out.

```toml
[graphics]
reflections = "medium"
global_illumination = "high"
```

`--reflections=LEVEL` and `--global-illumination=LEVEL` on the command line,
and `--global-illumination-budget=MS` for the milliseconds a frame -- which
also makes a run with no window keep to it, where it otherwise gives every
frame all it has room for.

Measured at 1920 by 1080 with the whole screen inside a room:

| | Forward pass, desktop card (Direct3D 12) | Forward pass, integrated graphics (Vulkan) |
|---|---|---|
| Neither | 0.23 ms | 7.1 ms |
| Probes | 0.26 ms | 8.4 ms |
| Volumes | 0.26 ms | 8.4 ms |
| Both | 0.29 ms | 9.5 ms |

A scene with no probe and no volume in it costs what it did before there were
any. The pictures and the maps are 11 MB at `High` and 17 MB at `Ultra`.

In the open, at the same size, the whole frame: 0.93 ms with no volume, 1.01
with three levels round the camera, 1.04 with one box as six tiles.

## What there is not

- **The ground reading the screen.** A `Terrain`, blocks and grass reflect
  and are lit by probes and volumes alone (see *From the screen*).
- **Lightmaps**: the sharp bounced shadow a grid of points cannot draw.
- **A file for a volume that follows the camera.** A box has one
  (`Baked`); the open is worked out as the camera goes.
