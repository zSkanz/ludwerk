# 29-meadow

Grass and flowers over rolling terrain (ADR 0116), with no instance per blade:
a `FoliageLayer` under the `Terrain` says where things grow -- the ground's
material, slope and height, and never under a roof -- and its `FoliageMesh`
children say what, in what share, how large and how much the wind moves them.
Each tile of ground grows as the camera comes near it, from the same rules and
seed on every machine, and a compute pass culls the field every frame into
indirect draws.

Run it with `run.bat`. The camera laps the meadow low over the grass; the steep
bank on the west thins out (`SlopeMax`), and the hollow under the arch to the
north stays bare because it cannot see the sky.

The two models in `content/models/` are generated, tapered quads for the grass
and a stem with a flower head, and carry no third-party licence.
