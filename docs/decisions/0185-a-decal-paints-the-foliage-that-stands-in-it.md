# 0185 — A decal paints the foliage that stands in it: `FoliageLayer.ReceivesDecals`

- Status: accepted
- Date: 2026-10-06
- Decided by: the agent, on the owner's word relayed by the coordinator ("if
  it is an engine limit it cannot stay; it has to be possible, as in any game
  engine"), under the standing rule to decide as professional engines do and
  record it
- Builds on: F2 (decals), ADR 0116 (foliage), ADR 0160 (a decal's blend modes
  and `BasePart.ReceivesDecals`)

## Context

A game lays a warning ring on the ground under a boss. On bare ground it is a
ring. On a meadow it is a ring with every blade of grass in it left green: a
mark drawn on the dirt between the blades, and hidden by them.

Nothing excluded foliage from a decal. A decal is a box drawn over the
picture: each pixel it covers reads the depth that is there, turns it back
into a position, and paints if the position is in the box -- and foliage is in
that depth like everything else. What left it clean is the fade a decal has
for surfaces that turn away from it: the pixel's normal is taken from the
depth round it, and where it is more than about eighty degrees from the way
the decal points, the decal is nothing. That is what stops a mark on a floor
from smearing down the side of every step it crosses. A blade of grass is a
card standing on end; a mark on the ground points straight down at it,
edge-on. Every blade faded to nothing, but for a line of pixels where one
card's edge met the next and the normal came out as nonsense.

So the limit was real, and a game could not reach round it: nothing in a
material or a decal said otherwise.

## How mature engines do it

A decal in both of the engines this one follows is laid over whatever is in
its volume and says it receives; grass under a decal projector is tinted by
it, blade and ground alike, and fading by angle is something a decal is
*given*, not something that silently takes foliage out. Foliage says for
itself whether it receives -- a flag on the foliage type or on its renderer --
and it receives unless it says not.

## Decision

1. **`FoliageLayer.ReceivesDecals`**, a boolean, **true by default**. On, a
   decal paints what the layer grows. Off, a decal paints round it and under
   it and leaves it as it was, as it does a part with
   `BasePart.ReceivesDecals` off.

2. **Foliage that receives takes the decal whichever way it faces.** The
   facing fade is skipped on a pixel that is foliage. A blade takes the
   picture as the ground under it does: its position is in the box, and it is
   not the side of a step. Everything else keeps the fade.

3. **Which pixels are foliage is a depth drawn for the purpose**
   (`decalFoliage_`): on a frame with a decal in the picture, the receiving
   layers are drawn once more, depth only, with the holes the picture gave
   them -- the card's image and the dither that thins an instance at its draw
   distance -- through the shadow pass's own shader and the camera. Where
   that depth and the picture's are the same surface, the pixel is foliage.
   The frozen RHI has no stencil to mark it with, and this is the device
   `BasePart.ReceivesDecals` already uses.

4. **A layer that does not receive is drawn into the mask the parts that do
   not are drawn into** (`decalMask_`), and is left alone by the same test.

5. **All three blend modes**: it is one shader and one pass, so `Multiply`,
   `Alpha` and `Additive` land on foliage as they land on the ground.

6. **It costs only where it is used, and no more than it must.** Nothing on a
   frame with no decal in the picture. With one: the mask is drawn only inside
   the rectangle of the picture the frame's decals cover (`decalCoverage`),
   and only the instances no further from the camera than the decals reach
   (`decalReach`) -- a mark is usually at a hero's feet, with grass behind it
   to the horizon.

7. **Not on the wire, not in the world hash** -- a `FoliageLayer` is neither
   (ADR 0116) -- and saved with the scene like the rest of a layer. Changing
   it regrows nothing: it is how a layer is drawn, not where it grows.

## What it costs

Measured on the meadow example with three marks in view, 1280 by 720, a dev
build on a desktop GPU (`--gpu-pass-times`, the floor of a timed stop taken
off): the mask is **0.12 ms**, where the same foliage costs 0.23 ms in the
picture's own pass and the frame is 3.2 ms without decals. About half of what
the foliage costs to draw, once more, on a frame that has a decal in view.

On a phone it has not been measured. What to expect: the mask shades nothing
-- a depth write and one image read for the card's holes -- so it is the
foliage's vertex work again and little of its fragment work, which on a phone
is the smaller part of what foliage costs. It is independent of how the ground
is shaded: the fast terrain path (ADR 0179) takes it unchanged, since a decal
reads depth and paints over the lit picture whichever path lit it.

A layer with `ReceivesDecals` off costs the same -- it is drawn into the other
mask instead. What costs nothing is a frame with no decal in it.

## What it does not do

- A blade is painted where it is *in the box*, like anything else. A decal a
  metre deep on the ground paints the first half metre of a tall blade and
  not its tip; a game that wants the whole blade makes the box as deep as the
  grass is tall.
- It does not light the decal by the blade: an `Alpha` decal on grass is lit
  as one on the ground is, by the sun and the ambient (ADR 0160).
- Instances are not culled against the decals one by one. An instance nearer
  than the furthest decal and off to the side of all of them is still drawn
  into the mask and cut by the rectangle.

## Consequences

- A ring, a scorch mark, a painted zone reads on a meadow as it reads on
  bare ground.
- The decal shader binds a fourth image; a frame with decals and no foliage
  binds the scene's depth there and does not read it.
- Tests: `foliage_decal_gate` (two fields of upright cards, a mark over both:
  the receiving field's cards are darker inside the mark than outside it, and
  the refusing field's are the same pixels as before the mark);
  `decalCoverage` and `decalReach` by arithmetic; a bucket takes its layer's
  flag and regrows nothing for it.
