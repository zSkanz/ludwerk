# Foliage

Grass, flowers, stones and bushes over a terrain, with no instance per blade.
A `FoliageLayer` under the `Terrain` says **where** things grow; its
`FoliageMesh` children say **what**:

```luau
--!strict
local meadow = Instance.new("FoliageLayer")
meadow.Density = 6 -- instances a square metre, where every rule allows
meadow.SlopeMax = 32 -- degrees: steeper ground stays bare
meadow.Clumping = 0.3 -- 0 even cover, 1 tufts with bare ground between
meadow.DrawDistance = 90
meadow:SetMaterials({ { Material = 1, Density = 1 } }) -- on grass only
meadow.Parent = terrain

local grass = Instance.new("FoliageMesh")
grass.Mesh = "asset://models/grass.gltf"
grass.Weight = 12 -- twelve grass to every flower below
grass.ScaleMin, grass.ScaleMax = 0.7, 1.3
grass.Parent = meadow

local flower = Instance.new("FoliageMesh")
flower.Mesh = "asset://models/flower.gltf"
flower.Weight = 1
flower.Parent = meadow
```

Adding, removing and editing foliage is creating, deleting and setting these
instances — from a script, or in the editor's Explorer and Properties.

## Where it grows

A layer grows on ground that passes every rule:

| Property | Rule |
|---|---|
| `SetMaterials` | the terrain materials it grows on, each with a multiplier of `Density`; empty is every material |
| `SlopeMin`, `SlopeMax` | degrees from level |
| `HeightMin`, `HeightMax` | metres of world height |
| `MinSpacing` | the least distance between two instances |
| `Clumping` | how much it gathers into patches |
| `Seed` | which of the arrangements the same rules give |

**Never under a roof.** Ground that cannot see the sky — a cave, the hollow
under an arch or an overhang — grows nothing, whatever the rules say.

The material is the one the ground is **drawn** as, so a terrain rule that paints
steep ground as rock ([Terrain](manual:world/terrain)) keeps grass off it too.

## What grows

| Property | |
|---|---|
| `Mesh` | any mesh of the project |
| `Material` | a material asset, or nothing for the mesh's own; its maps and values are used, its surface shader not yet |
| `Weight` | its share of the layer's instances |
| `ScaleMin`, `ScaleMax` | the range each instance's size is drawn from |
| `RandomRotation` | turned at random about its up axis |
| `AlignToNormal` | 0 upright, 1 leaning with the ground |
| `Sink` | metres into the ground, so a stem on a slope is not floating |
| `WindResponse`, `Stiffness` | how much the wind moves it, and how hard it resists |
| `CastShadow` | casts a shadow near the camera |

## Wind

Every foliage mesh sways with the workspace's wind ([Wind](manual:world/wind)):
it bends from its base, more the higher up the mesh a vertex is, with a phase of
its own so a field does not move as one sheet. `WindResponse` 0 holds it still;
a stiff bush has a `Stiffness` above 1 and a reed below. **Not yet**: a
material's surface shader running its own `surfaceVertex` on foliage — the
built-in sway is what moves it today.

## Decals

A [decal](manual:rendering/particles-and-decals) paints the foliage that stands
in its box, blade and ground alike: a warning ring on a meadow is a ring on
the grass. A layer that should stand clear of every mark says so:

```luau
reeds.ReceivesDecals = false   -- on the FoliageLayer; true by default
```

The decal then paints round the layer's instances and under them. A blade is
painted where it is inside the box, so a box as deep as the grass is tall
paints the whole blade, and a shallow one only its foot.

On a frame with a decal in view, the foliage near it is drawn once more, depth
only: about half of what the foliage costs to draw, and nothing on a frame
with no decal.

## Visual only

Foliage has no body: nothing collides with it, a raycast passes through it and
navigation ignores it. **It is not saved, not replicated and not in the world
hash**: the engine grows it from the terrain, the rules and the seed, so every
machine grows the same field from the scene it already has. A dedicated server
grows nothing.

## As fast as it can be

- The ground is grown a **tile** at a time — one column of terrain chunks —
  as the camera comes within `DrawDistance`, on the job threads, and grown
  again only where a sculpt or a paint changed the ground or the layer
  changed.
- Every frame a **compute pass on the GPU** culls each tile's instances by the
  camera's view and the draw distance and writes the draws; nothing is drawn
  per instance from the CPU, and the general instancing limit does not apply.
- The field **fades out dithered** over `FadeDistance` before `DrawDistance`,
  so its edge is not a line.
- Shadows only near the camera, and a quality setting that thins the field in a
  stable order — lowering it keeps a subset rather than reshuffling:

```toml
[render]
foliage_density = 1.0 # the fraction drawn; 0.5 on Android by default
foliage_shadow_distance = 30 # metres; 15 on Android
```

`examples/29-meadow` is a meadow of grass and flowers over rolling ground in a
gusting wind.
