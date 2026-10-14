# 0206 — Light that bounces in the open: a volume carried with the camera, and a box as tiles

- Status: accepted; built (protocol 49)
- Date: 2026-10-11
- Amends: ADR 0203 (a volume was one box of at most 4,096 points; four
  volumes a frame; sixteen layers)
- Builds on: ADR 0203 (the rays, the points, what a surface reads), ADR 0202
  (the array the slices are kept in)

## The question

ADR 0203 lights a room: a box drawn round it, points inside the box. Two
things a game with maps two hundred metres across met at once.

**There is no box to draw round the open.** A square, a street, a forest
clearing: the light a red wall gives the ground beside it is the same light
as in a room, and no box of 4,096 points holds a map.

**A large room is coarse.** A hall sixty metres by fourteen by sixty is
4,096 points only at 2.4 metres between them. Asked for one metre it was
given a coarser spacing and a line in the log.

## The decision

### 1. A volume that is carried with the camera

`IrradianceVolume.FollowsCamera`. The part it is under gives its SIZE and
nothing else: the volume stands round the camera. `IrradianceVolume.Levels`,
1 to 3: each level after the first is twice as large round the same middle
with points twice as far apart -- fine light near the camera, coarse light
far from it. A part 40 by 16 by 40 at a `Spacing` of 2 with three levels is
3,200 points a level, and light for 80 metres every way.

It is the same class, and everything a volume has it has: `Mode`, `Tint`,
`Intensity`, `Bounces`, `Dynamic`, `IsReady`, `Completed`. There is no
second kind of thing to learn.

**It stands on a grid, and is carried a few cells at a time.** A level's
middle is the camera's place rounded to four of that level's cells, so a
point of it is always at the same place in the world: light does not swim as
the camera turns or steps. When the camera has gone four cells the level is
carried after it (`VolumeBake::scroll`): the points it has in common with
where it stood keep their rays -- and, under the same light, what the light
put on them -- and only the new ones are cast for. Four cells of twenty is a
fifth of the points.

**What is drawn is the level where its light is the light OF.** While a
level is being carried the picture goes on reading it where it stood; when
the new one is whole, the points the two have in common go from the old
light to the new as any volume's do, and the new points are simply there.

**Carrying takes its turn.** It is upkeep, as lighting a `Dynamic` volume
again is, and queued with it: one at a time, in view first, the one that has
waited longest (ADR 0203, "One volume is lit again at a time"), inside the
frame's `GlobalIlluminationBudget`. A camera that outruns it is lit by
levels that stand a little behind -- the wider ones cover -- and never waits
for them.

**Lying down.** A level's slices are along its own z, four to a layer of
the array. It is laid with its z up, so a level that is wide and not tall
-- which the open is -- is two layers, not five.

### 2. A box too large for one volume is tiles

Nothing to set. A box that at its `Spacing` is more than 4,096 points, or 32
along a side, is worked out as tiles: as few as hold it, each a volume of
its own on the box's own grid, sharing a cell with the tile beside it. Up to
sixteen; a box that would take more is one volume at a coarser spacing and
says so once, as before.

### 3. What a surface reads

Each volume a frame carries says how it is weighed against its neighbours
(`RenderIrradianceVolume::margin`, the row's fourth number):

- **A room** (nought): whole to its faces, as ADR 0203 has it -- the dark
  side of a thin wall is the dark room's.
- **A tile** (more): by how far in from its faces the point is, over the two
  cells it shares with the next tile. Their points stand at the same places
  there, so the two agree, and each gives way to the other across them:
  there is no line where tiles meet.
- **A level** (less): listed finest first, each takes what the finer ones
  left, by how far in from its faces the point is. A level gives way to the
  wider one over its last two cells, and the widest to no volume at all.

**A room's own volume has the room.** The levels light what no room covers.
A point at two metres or eight between points has no notion of a wall ten
centimetres thick: a house in the open wants its own volume, as it did.

Eight volumes a frame, where there were four, nearest first -- three levels
and five rooms or tiles -- and thirty-two layers, where there were sixteen.

### 4. Where it is on

Where box volumes are: `GraphicsService.GlobalIllumination` at `High` and
above, off at every level of a phone until it is measured there.

## Measured

A desktop card, 1080 lines, the whole frame with the GPU waited
for, `tests/screenshots/openfield`:

| | a frame |
|---|---|
| no volume | 0.93 ms |
| three levels round the camera | 1.01 ms |
| one box as six tiles | 1.04 ms |

- A level of 3,200 points worked out for the first time: 43 ms of rays.
  Three, behind the loading screen: 180 ms.
- A level carried four cells: 26 ms of rays, which at the default budget of
  a millisecond and a half is some seventeen frames; the frames it is spread
  over spend 1.9 ms on it at the 95th percentile and 3.4 at worst.
- Kept rays: 64 bytes a ray, 64 rays a point -- 13 MB a level of 3,200
  points, 39 MB for three. A level cannot give them back as a volume that
  is not `Dynamic` does: they are what makes carrying it cheap.

## Not decided here

- **Light leaks through thin walls in the open.** By construction (section
  3): the answer is a volume for the building. A level that knew which of
  its points see each other would not need one, and costs a visibility
  term a point.
- **Baking a level to disk.** Baking box volumes is a later item; a level
  moves, and what would be kept is the world's, not a box's.
- **More than three levels, or a level per axis of its own size.** Three
  reach 80 metres from 40; past that the sky and the flat ambient are what
  they were.
- **The rays' memory.** A ray kept as 32 bytes would halve it. Not done:
  measured first, on a game that walks.
