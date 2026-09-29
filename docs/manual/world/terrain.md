# Terrain

A `Terrain` is ground you shape rather than a floor made of parts. Raise hills,
dig pits, bore a tunnel through a mountain, paint the slopes rock. It collides,
it is saved with the scene, and a large one streams from disk as the player
moves.

**Underneath, it is a grid of voxels.** Space is cut into cubes `VoxelSize` on
a side, and each cube holds two things:

- a material;
- how full of that material it is, from 0 to 1.

The surface is wherever that fullness crosses one half. It is found by blending
between neighbouring cubes, so a ball carved out of a hillside lands where you
put it rather than on a grid line. Caves, arches, overhangs and flat ground are
all the same data, so nothing is special about any of them.

## Making one

```luau
--!strict
local terrain = Instance.new("Terrain")
terrain.VoxelSize = 1      -- metres per voxel: 1 resolves a path, 2 a hillside
terrain.MinHeight = -32    -- the world's floor: nothing is written below it
terrain.MaxHeight = 64     -- and its ceiling
terrain.Parent = workspace

-- Ground from below the floor up to y = 2, sixty-four metres square.
terrain:FillBlock(vector.create(0, -15, 0), vector.create(64, 34, 64), 1)
```

`workspace.Terrain` names the world's terrain once there is one.

**Ground laid where there was none is a slab 32 metres deep**, under the
lowest surface it lays and rounded down to a whole chunk. That covers
`WriteHeights`, Generate Flat Ground and a first raise on empty terrain.
Its sides and bottom are drawn and collide like its top, so the edge of a
terrain looks the same everywhere, however it was made. Nothing is written
below `MinHeight`. `VoxelSize` is 0.1 to 64 metres, and can only change while
the terrain is empty.

## Shaping it

| Call | What it does |
|---|---|
| `FillBall(center, radius, material)` | Adds a ball, or removes one with material 0. Near its rim a ball overhangs, which makes it the verb for a boulder or a tunnel. |
| `FillBlock(center, size, material)` | The same, as a box. |
| `FillCylinder(center, height, radius, material)` | The same, as an upright cylinder: a pillar, a well. |
| `RaiseBall(center, radius, amount, material?)` | Pushes the surface up under a disc, falling smoothly to nothing at the rim, or down for a negative amount. It never makes an overhang, which makes it the verb for a hill. Given a material, it lays new ground of it where there was none. |
| `GrowBall(center, radius, amount, material?)` | Moves the surface out along its own slope, or in for a negative amount: the editor's Raise and Lower. A field rises, a cliff comes forward. |
| `SmoothBall(center, radius, strength?)` | Softens the ground in a ball: bumps go, pits fill, flat ground stays. |
| `FlattenBall(center, radius, height, strength?)` | Pulls the ground in a ball towards a level plane. |
| `PaintBall(center, radius, material, options?)` | Changes what the ground is made of and leaves it where it is: outright, or blended over it a little at a time (see *Painting over*). |
| `ReplaceMaterial(minCorner, maxCorner, from, to)` | Every voxel of one material in a box becomes another. |
| `WriteHeights(corner, columns, heights, material)` | Writes a heightmap in one call, one height per column of voxels, row after row along +Z. It is the verb for ground from a generator or an image. `material` is one id, or a table of ids beside `heights` (0 leaves a column alone). |
| `ReadVoxels(minCorner, maxCorner)` | Materials and occupancies of a box of voxels, and how many there are on each axis. |
| `WriteVoxels(corner, size, materials, occupancies)` | Puts them back, or writes new ones. |
| `WorldToCell(position)` / `CellCenterToWorld(cell)` | Between a world position and a voxel's index. |
| `HeightAt(x, z)` | The top of the ground in a column, or nil where there is none. |
| `Clear()` | Removes everything. |

**Adding never takes ground away, and removing never adds it.** A voxel ends
up:

- when adding, as full as the fuller of it and the brush;
- when removing, as empty as the brush leaves it.

So a ball of sand pressed into grass leaves the grass under the surface as
grass.

**A tunnel is a line of balls with material 0**, and the ground above it stays
standing:

```luau
for along = -40, 40 do
    terrain:FillBall(vector.create(130 + along, 3, -90), 3, 0)
end
```

`HeightAt` answers the top of a column, even with a cave in it. To find the
first surface along a direction, such as the floor of a tunnel under a torch,
cast a ray with `Workspace:Raycast`. It meets terrain anywhere, near things
that move or not.

## Reading and writing voxels

`ReadVoxels` returns flat tables. The voxel at `x, y, z`, counting from 0 at
the low corner, is at index `1 + x + size.X * (y + size.Y * z)`. What
`ReadVoxels` reads, `WriteVoxels` puts back exactly, which is how to copy a
piece of terrain somewhere else:

```luau
local materials, occupancies, size = terrain:ReadVoxels(vector.create(0, -8, 0), vector.create(16, 8, 16))
terrain:WriteVoxels(vector.create(100, -8, 0), size, materials, occupancies)
```

**Occupancy fades across four voxels at a surface**, not one. Terrain laid
from heights has to keep its slopes smooth, and a one-voxel fade turns any
slope steeper than 45 degrees into steps. The four-voxel fade has two
consequences:

- a thin feature is partly full rather than full;
- a voxel inside a narrow well reads a little occupancy even though it is air
  at its centre.

The surface, where occupancy crosses one half, is where the brush put it.

## Materials are layers

A voxel's material is a number, and **the number means the terrain's layer
of that number**: a material asset, like the ones parts wear
([Materials](manual:world/materials)).

**A new terrain has no layers.** Its materials are the project's own files,
which open in Content like any other; until it has one, its ground draws a
plain grey, and the first material it is given is what that ground becomes.
In the editor, the Terrain panel's Paint mode makes one (**New Material**),
uses one the project has, or writes a starter set (**Add Starter
Materials**): eight files under `content/materials/terrain/`, each a variant
of one of the engine's built-in terrain materials, so they look finished and
change like any material:

| Starter | Built on |
|---|---|
| `materials/terrain/grass.material.json` | `engine://terrain/grass` |
| `materials/terrain/sand.material.json` | `engine://terrain/sand` |
| `materials/terrain/rock.material.json` | `engine://terrain/rock` |
| `materials/terrain/snow.material.json` | `engine://terrain/snow` |
| `materials/terrain/mud.material.json` | `engine://terrain/mud` |
| `materials/terrain/sandstone.material.json` | `engine://terrain/sandstone` |
| `materials/terrain/basalt.material.json` | `engine://terrain/basalt` |
| `materials/terrain/ice.material.json` | `engine://terrain/ice` |

Material 0 is not ground: it is what digging writes.

A scene saved before terrains started empty has no list, and reads the
engine's eight directly, with the slope rock rule; it looks as it did. A
script sets the list with `SetLayers` and reads it with `GetLayers`:

```luau
--!strict
terrain:SetLayers({
    "asset://materials/terrain/grass.material.json", -- id 1
    "asset://materials/terrain/rock.material.json", -- id 2
})
local layers = terrain:GetLayers()
layers[2] = "asset://materials/cliff.material.json" -- every rock is now cliff
table.insert(layers, "asset://materials/moss.material.json") -- id 3
terrain:SetLayers(layers)
```

- **The voxels keep their numbers.** Replacing layer 3 repaints every voxel
  of it at once; adding a layer gives you a new id to paint with.
- **The list is saved with the scene**, up to 255 layers.
- **What a layer reads from its material**: `ColorMap`, `NormalMap`,
  `MetallicRoughnessMap`, the colour, roughness and metalness factors,
  `NormalScale`, `TileSize` (metres per repeat), and `Triplanar` (projected
  from three axes, on by default, so a cliff is not a smear). `HeightMap` and
  `BlendSharpness` are for where two layers meet. A surface shader is not read
  on terrain.
- **Where layers meet**, a pixel blends the layers of its triangle's corners,
  so a painted edge is soft rather than stepped.

## Painting over

**A voxel holds two materials and how much of the one over shows** (ADR
0114): the ground's own, and one painted over it. `PaintBall`'s options say
what a stroke does with them:

```luau
--!strict
local terrain = workspace:WaitForChild("Terrain") :: Terrain
-- A little sand over the grass, softer towards the rim of the ball.
terrain:PaintBall(vector.create(0, 0, 0), 6, 2, {
    Mode = Enum.TerrainPaintMode.Blend,
    Strength = 0.3,
    Falloff = 0.8,
})
```

- **`Replace`** (the default) makes the ground the material outright.
- **`Blend`** shows the material more with every stroke, over what is there,
  by `Strength` at the middle and less towards the rim as `Falloff` says.
  Blended until it is all that shows, it is simply what the ground is made of.
  A third material over two puts the one that shows more underneath first.
- **`Under`** changes the ground under what was painted, and leaves the paint.
- **`Erase`** takes off what was painted, revealing what is under.

**Where paint meets the ground under it**, the one painted over fills the dark
cracks of the other first -- sand in the seams of rock -- and the painted
material's `BlendSharpness` says how hard that edge is: 0 a fade, 1 a line.
Sculpting keeps the paint on the ground it moves.

## Rules: painting by slope and height

A terrain also has **rules**, which draw a layer wherever the ground is steep
enough, or high enough, whatever it was painted. A new terrain has none; a
scene from before terrains started empty has the old slope rock.

```luau
--!strict
terrain:SetRules({
    -- steep ground as rock (layer 3), over every layer but rock and basalt
    { Material = 3, SlopeMin = 45, Blend = 9.7, Noise = 0.12, AppliesTo = { 1, 2, 4, 5, 6, 8 } },
    { Material = 4, HeightMin = 120, Blend = 6, Noise = 0.3 }, -- snow above 120 m
})
```

| Field | What it means |
|---|---|
| `Enabled` | Off draws nothing. |
| `Material` | The layer the rule draws. |
| `SlopeMin`, `SlopeMax` | Degrees from level, 0 to 90. |
| `HeightMin`, `HeightMax` | World metres; -100000 and 100000 are open. |
| `Blend` | How wide the edge is: degrees across a slope bound, metres across a height bound. |
| `Noise` | How ragged the edge is; 0 is a clean line. |
| `AppliesTo` | The layers it may cover; empty is every layer but its own. |

- **In order**: each rule paints over what came before it. Up to 16.
- **Drawn, not written.** The voxels keep their materials, so turning a rule
  off undoes it. `ApplyRules(minCorner, maxCorner)` writes what the rules draw
  into the voxels at the surface, after which the rule can be off with nothing
  changing on screen.
- **What the game sees is what is drawn**: the engine evaluates the rules on
  the CPU the same way the shader does, so the ground under a steep slope drawn
  as rock is rock to the game too.
- The rules are saved with the scene.

## In the editor

The **Terrain** panel (the mountain on the left-hand bar, or select the
terrain) has five modes along its top, in the order the work goes:

| Mode | What it is for |
|---|---|
| **Sculpt** | Shaping the ground with a brush. |
| **Paint** | The terrain's materials, painting them by hand, and the rules that paint by slope and height. |
| **Foliage** | Layers of grass, flowers and trees, and a brush that grows or thins them. |
| **Create** | Flat ground, ground from a heightmap, and clearing it all. |
| **Setup** | The voxel size and the height range. |

Choosing a mode puts no brush in your hand; choosing a tool does. While a
brush is in hand the viewport says so in its corner -- the tool, the size,
and that **Esc** puts it down -- and clicking anything else also puts it down.
Every stroke is one undo step, and a stroke that changed nothing leaves none.

### Sculpt

| Tool | Key | What it does |
|---|---|---|
| Raise | 1 | Lifts the ground under the brush, most at its centre: a field rises, a cliff comes forward. |
| Lower | 2 | Sinks the ground under the brush. |
| Smooth | 3 | Softens bumps, fills pits and rounds off edges. Flat ground stays exactly where it is. |
| Flatten | 4 | Levels the ground to one height: where the stroke starts, or a fixed height set under Brush. |
| Add | 5 | A ball (or box) of ground where you aim; held still, it grows towards you. |
| Dig | 6 | A ball (or box) taken away; held still, it tunnels in. |

- **Hold Ctrl** for the opposite -- Raise lowers, Add digs -- and **hold
  Shift** to smooth, whichever tool is chosen. What the keys make of the brush
  is lit on its tile.
- **[ and ]** make the brush smaller and bigger; with **Shift** they change
  its strength.
- **Dragging** stamps the brush every step of the way; **holding it still**
  keeps working the ground under it, at a rate the strength sets.
- The ring shows the brush: a circle on the ground, with a fainter one where a
  soft brush does half as much; a box for a square Add or Dig.
- A brush too big for the terrain's voxels says so rather than doing nothing:
  one stamp may touch at most 512 x 512 x 512 voxels.

### Paint

A new terrain has no materials. **New Material** writes one into
`materials/` and makes it the terrain's next layer (and opens it, to set its
colour and textures); **Add Starter Materials** writes the eight starters
under `materials/terrain/`. **Change Layers...** puts one of the project's
materials in the selected layer's place, adds one, or removes the last.
**Paint** paints the selected material under the brush without moving the
ground. **How it goes on** chooses Blend (a little with every stamp, by the
brush's Strength), Replace, Under or Erase, and **Softness** how much less it
paints towards the rim; hold Ctrl while blending to take paint off.

**Paint by slope and height** lists the terrain's rules in order, each with
its fields, a switch, and up, down and remove; every change shows at once,
and a drag is one undo step. **Apply to Voxels** writes what the rules draw
into the ground.

### Create

**Flat ground** lays a square of the size and at the world height asked for
-- on a new world it makes the terrain, and it is one undo step either way.
On a terrain that has ground, it levels the whole square, sculpting inside it
included.

**From a heightmap** lays an image over the ground:

1. Choose a file.
2. Say how wide it is laid, and which heights black and white stand for.
3. Import.

The square is centred on the terrain's `Position`, and the image's top-left
pixel is its corner with the smallest x and z. One ctrl-Z takes the import
back.

- **File formats.** A 16-bit PNG, or a RAW file of square 16-bit little-endian
  samples (`.r16`, `.raw`), keeps a slope smooth. An 8-bit image has 256 steps,
  and over a hundred metres a character walks up those steps as stairs.
- **Ground under the square.** Each column's top moves to the image's height,
  and a cave under the top stays where it is. Where there was no ground, the
  image is laid as a slab 32 m deep under its lowest point.

### Setup

The three numbers a terrain is decided at:
- `VoxelSize`, which can change only while the terrain is empty;
- `MinHeight` and `MaxHeight`, the world's floor and ceiling.

## How it is drawn and collided

- **Drawn.** The ground is drawn as meshes, one per column of 32-voxel chunks
  near the camera, and coarser further away. A far mesh is built from each
  chunk's averages, so distant hills keep their shape with a fraction of the
  triangles. An edit rebuilds only the meshes of the chunks it changed.
- **Collided.** Collision is built near things that move: bodies that are not
  anchored, and characters. Each such chunk gets a mesh of its surface, which
  a body lands on the moment it arrives.

## Large worlds

A terrain saved with a scene streams from disk once it is sixteen cells or
more, each cell 64 m on a side. The world waits for the ground around the
player before the first frame, loads cells ahead of the camera and evicts them
behind it. **A cell somebody changed is never evicted**, so a crater stays a
crater. See [Streaming a large world](manual:assets/streaming).

## In a match

**The ground replicates** ([ADR 0135](../../decisions/0135-the-ground-replicates-terrain-and-block-edits-travel-as-whole-chunks.md)).
Every machine loads the scene's ground from its own package; the server sends
what differs from it -- to a player who joins, every chunk a script or the
editor changed, and from then on each chunk as it changes, with the layers and
rules when they change. The block world travels the same way, with its block
types. So a digging game is multiplayer as it is: a player asks the server to
dig through a remote, the server digs, and every player sees the hole, a late
joiner included. A server that makes its terrain in a script sends it whole.

- **The server's ground is the truth.** A client's script may dig its own copy
  for the feel of an instant hole; the next chunk the server sends over it wins.
- **What it costs is what changes**: a crater sends the few chunks it touched,
  and a quiet match sends nothing.
- **Its place travels too**: a terrain the server moves, or makes somewhere,
  is there on every machine. One a server script destroys takes its ground
  from every player, and one it makes after is the ground they see.

**On a large world that streams**, an edit never loses ground the camera has
not reached: a script, the brush or a block placed in a cell still on disk
reads that cell first, and a hole dug in a cell that later streams out is still
a hole when it comes back.

**Worlds saved by an earlier version**, when terrain was stored as heights with
voxels only where caves were, open as they were and are saved as voxels from
then on.

## Where to look next

- [Block worlds](manual:world/blocks) -- cubes on a grid, which is a different
  thing from terrain
- [Physics queries](manual:physics/queries)
- `examples/13-terrain` and `examples/17-cave`
