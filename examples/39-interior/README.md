# 39-interior

Indirect light (ADRs 0202, 0203): what a room reflects, and the light its own
walls give back.

```
run.bat
```

Two rooms that share a wall ten centimetres thick, with a door in it. The west
one has a window the sun comes through; the east one has no window at all.

- **A room is an invisible part its own size**, and what stands under it: a
  `ReflectionProbe`, so the polished floor and the chrome ball reflect the room
  they are in and not a sky they cannot see; and an `IrradianceVolume`, so the
  room is lit by what its walls bounce and not by one flat colour.
- **The dark room stays dark.** The two boxes meet inside the thin wall. A
  surface reads the probe and the volume that the point a short step in front
  of it is inside, so each face of the wall is its own room's, however thin
  the wall is. The east room is lit by what comes through its door and by
  nothing that comes through its wall.
- **A building that is one mesh is lit through its own openings.** The annex
  north of the house is a single file -- walls, ceiling, a tiled floor, a
  window and a door (`tools/make_annex.py` writes it) -- as a building from a
  modelling program is. To the physics world it is one hull, every opening
  shut; the light is worked out against its triangles, so the sun comes in
  where it can, and what it lands on is terracotta, by the mean of the
  floor's picture -- and so is what the walls get back.
- **What moves is lit like what does not.** Nothing is baked into a wall: the
  walker going from one room to the other is lit by whichever it is in, and
  crosses from one to the other in the plane of the door.
- **Nobody watches a room light up.** The pictures are taken and the bounced
  light worked out behind the loading screen, which lifts on rooms already lit.

Keys:

- **1**, **2**, **3** look at the bright room, the dark room, the annex.
- **G** bounced light off and on (`GraphicsService.GlobalIllumination`).
- **R** room reflections off and on (`GraphicsService.Reflections`).
- **S** reflections from the screen off and on
  (`GraphicsService.ScreenSpaceReflections`): the walker and the ball stand in
  the polished floor, which no probe's picture has. Off unless the quality
  level is `Ultra`.
- **B** light bounced off the screen off and on
  (`GraphicsService.ScreenSpaceBounce`): the red wall on the floor at its
  foot, below the grain of a volume.
- **L** a lamp in the dark room off and on. Both rooms follow it, because their
  probes and volumes are `Dynamic`: the volume is lit again a share a frame and
  goes from its old light to its new over half a second.

The manual's page is [Indirect light](../../docs/manual/rendering/indirect-light.md).
