# 0207 — A volume kept: baked to a file, and its mean for a machine that works none out

- Status: accepted; built (protocol 50)
- Date: 2026-10-11
- Builds on: ADR 0203 (the rays, the points, what a surface reads), ADR 0206
  (a box as tiles)

## The question

An `IrradianceVolume` is worked out on the machine that draws it: rays
against what stands still, lit, gathered. That is right for a room whose
light changes and for an editor, and wrong twice over for a shipped game.

**A room that never changes is worked out every time it is opened**, on
every machine, to the same answer. Forty milliseconds of rays for a level of
4,096 points is a hitch on a desktop and is not offered on a phone at all.

**A machine that works no volume out has nothing.** Below
`GlobalIllumination = High` a volume is not there: the room is lit by the
flat ambient, which is the sky's -- the one light a closed room does not have.

## The decision

### 1. `IrradianceVolume.Baked` names a file

`Baked: Content`, an `asset://...irradiance` under `content/`. The file holds
what the volume worked out -- every point's twelve numbers, a piece a tile --
and what it was worked out AS: where its box stood and how large, its
`Spacing`, `Bounces` and `Mode`. Binary, low byte first, about forty-eight
bytes a point: a room of 512 points is 25 KB, a hall of sixteen tiles 3 MB.

It is written by:

- **the editor**: `Bake` on the volume, in Properties. A volume that names
  no file is given one, `asset://light/<its part's name>.irradiance`; it is
  worked out by rays whatever it was showing, and written when it is whole;
- **a command**: `engine-host <project> --bake-irradiance` opens the project
  as a game is opened, with no window, works out every volume that names a
  file -- whatever that machine's settings -- writes each, and exits. It
  needs a device: the light is a frame's, and a run with none draws no
  frame. A project where no volume names a file is an error that says so.

Not for a volume with `FollowsCamera`: it is another place every frame.

### 2. A machine that works volumes out reads the file and casts no ray

The frame the volume is first asked for -- two ticks in, as before -- its
light is the file's: there, whole, `IsReady` true, and said once in the log.

- **`Dynamic`**, the file is its first light, and it is worked out all the
  same in the frames that follow, to be lit again as the light moves: the
  rays are what a relight needs.
- **`Refresh`** is an author asking for rays, and gets them.
- **The file is the light of THAT volume.** With another box, `Spacing`,
  `Bounces` or `Mode` -- or a number of pieces or of points that is not the
  file's -- it is refused, said once, and the volume is worked out as one
  with no file is. So is a file that is not there, or is not one.

### 3. A machine that works none out reads one colour

The mean of every point's light from every side: one colour for the whole
box, drawn where the flat ambient would have been (`Full`) or added to it
(`BounceOnly`), with the volume's own `Intensity`, `Tint` and `Blend`. Not
the light of each corner -- the room's own tone, where there was the sky's.

It costs nothing a frame that a volume's rows did not already cost: the
volume is its seven rows with no points, the shader returns the colour before
it would have fetched, and no texture layer is kept for it. So it is drawn
on a machine that keeps no layers for volumes at all.

### 4. The room it was baked in

With the file goes one number for **what stood in the box**: every part
that does not move and is in or against the box -- where it stands in the
box's own space, how it is turned and how large, to the millimetre -- summed,
so the same room is the same number in any order.

Where somebody is there to be told -- an editor, a development build -- a
volume whose room is no longer that number says so, once: *the room is not
the room this was baked in; bake it again.* The file's light is drawn all the
same; it is the nearest thing to the room's light there is. A shipped game
does not look.

**What the number does not see**, and the manual says in plain words: a wall
repainted, a lamp moved or recoloured, the sun, the ground of a `Terrain`.
The light in the file is the light there was when it was baked. *Bake again
after changing a room* is the rule; the number catches the commonest way of
forgetting it.

### 5. In a match, and in a replay

`Baked` travels with the volume, as a content name (a field more on the
wire: **protocol 50**). Each machine reads its own copy of the file. What a
volume shows is picture, each machine's own, as it always was: nothing of
this is in the world's hash but the property.

## Considered and not done

- **Lightmaps.** ADR 0203 says why: a second set of coordinates on every
  mesh, and nothing that moves is lit by them.
- **The file for a volume that follows the camera.** It would be the whole
  map at the finest level's spacing. A map that wants its open light kept
  draws boxes round its places.
- **Compressing the file.** The pack compresses what is put in it.
- **Baking with no device.** The light a volume is lit by is the frame's: the
  sky as the renderer evaluates it, the lamps as the extract hands them. A
  second path that worked a sky out without one would be a second answer.

## What it is measured by

`baked_room_gate`, over the white furnace of ADR 0203: the room is baked by
the command; opened again it takes its 512 points from the file, casts no
ray, and **is the picture the rays made**, pixel for pixel; at a setting
that works no volume out it is the picture of the same room with no volume
and an ambient of the file's mean (0.152, where radiosity puts the middle of
a wall at 0.158); with no file it is the room with no ambient at all; a
volume of another spacing is refused; a pillar that was not there is said.
