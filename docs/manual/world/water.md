# Water

A `Water` is a body of water: a sea, a lake or a river. The engine draws it and
floats things in it from **one** description of its surface, so a boat sits on
the water it is drawn on (ADR 0118).

```luau
--!strict
local sea = Instance.new("Water")
sea.Shape = Enum.WaterShape.Ocean
sea.SurfaceLevel = 0
sea.Parent = workspace

local swell = Instance.new("WaterWave")
swell.Wavelength = 60 -- metres crest to crest
swell.Amplitude = 0.8 -- metres above and below the level
swell.Direction = 30 -- degrees about the vertical, 0 along +X
swell.Steepness = 0.3 -- 0 a sine, towards 1 sharper crests and flatter troughs
swell.Parent = sea
```

A part with a `Density` below the water's then floats, with no script: a crate
of density 0.5 rides half under.

## In the editor

The **Tools** button, then **Water** (or the Water icon in the side bar, or `Tool: Water` in the
command palette) opens the Water panel and puts the tool in hand. While it is,
a click in the viewport draws water; Escape puts the tool down.

| | |
|---|---|
| **River** | Click the ground: each click adds a point to the end of the river, and the first one starts it. The river follows a smooth curve through its points, and each point lies a little over the ground it was clicked on, so a river clicked down a hillside runs down it. The stretch the next click would add is shown before you click. Drag a point to move it. Click a point and press Delete to remove it. Enter, Escape or **New river** puts the river down, and the next click starts another; a click on a river with none in hand picks that one up. |
| **Lake** | Click on the shore, at the height the water should come to, then on round it: the lake is the inside of the smooth curve through the points, level at the first click's height. It needs three points. Drag a point to move it; Delete removes the selected one. |
| **Pool** | Press on the edge, at the height the water should come to, and drag across: a rectangle of water, level at the height the drag began at. Click a pool to pick it up, and drag a corner to resize it. |
| **Ocean** | Click anywhere: the sea comes up to the height clicked. A world has one sea, and another click moves it. |

The panel has the numbers you change between two clicks -- the surface's
height, a river's width, the depth, how fast a river flows -- and everything
else is in Properties, because the tool selects what it draws: a river's point
has a `Width` and a `Depth` of its own there, and `Sharp` for a corner. Every
click and every drag is one step of Undo.

**Carve bed** cuts the bed of the water in hand into the terrain under it:
the ground is lowered to the water's depth, with a bank **Bank width** wide
sloping up to the water's edge, and a river's bed follows it down the hill.
It is one step of Undo, and it only ever digs -- ground already lower is left
alone -- so after moving a point, carve again. Until a river is carved it
lies over the ground, and where the ground rises between two of its points
the ground covers it.

## Shapes

| `Shape` | Where it is |
|---|---|
| `Ocean` | everywhere, drawn out to the horizon round the camera |
| `Pool` | a rectangle `Size.X` by `Size.Z` centred on `Position`, `Size.Y` deep under `SurfaceLevel` |
| `River` | a ribbon along the smooth curve through its `WaterPoint` children, in their order. Each point's height is the surface's there, so a river descends; its `Width` and `Depth` are the river's there, and where they are zero the water's `Size.X` and `Size.Y` |
| `Lake` | the inside of the closed curve through its `WaterPoint` children, level at `SurfaceLevel`, `Size.Y` deep. It needs three points |
| `Box`, `Spline` | the older names: `Box` is `Pool`; `Spline` is a river straight from point to point and level at `SurfaceLevel`, as rivers were |

```luau
--!strict
local river = Instance.new("Water")
river.Shape = Enum.WaterShape.River
river.Size = vector.create(6, 2, 6) -- six metres wide, two deep
river.FlowSpeed = 2
river.Parent = workspace
for _, at in { vector.create(0, 12, 0), vector.create(30, 8, 10), vector.create(60, 2, 0) } do
    local point = Instance.new("WaterPoint")
    point.Position = at -- its height is the river's there
    point.Parent = river
end
```

The curve passes through every point and makes no loop; a point with
`Sharp = true` is a corner. A river runs at `FlowSpeed` where it is level and
faster where it drops -- twice as fast down a slope of one in four -- and what
floats in it is held up at the river's height there. A pool or a lake is at
`SurfaceLevel`.

`water:Carve()` cuts the water's bed into every terrain it lies over, as the
editor's **Carve bed** does, and answers how many columns of ground it
lowered: to the water's depth, with a bank `BankWidth` metres wide (2 unless
it says otherwise). Water is drawn only where it is, so ground it has not
been carved into hides whatever of it is under that ground.

## Waves

The `WaterWave` children are the waves, up to eight, in child order. Each is
`A (sin p - s/2 cos 2p)` where `p` travels across the water: `Steepness`
sharpens the crests and keeps the level the average. **A wave's speed is not a
property**: it follows from its length the way deep water's does, so a long
swell outruns a short chop on its own.

A water with no waves is flat and still, which is right for a pool.

## What floats

A simulated part in a water is held up by the weight of the water it pushes
aside, taken at 27 points through its volume. So:

- **it floats where that balances its weight**: `Density` less than the
  water's `Density` (1 by default, the unit parts use) floats, more sinks;
- **a hull rolls and rights itself**: a wave under one side lifts that side,
  and a wide flat body is pushed back upright;
- `Buoyant = false` on a part leaves it alone -- an anchor, a diver's weight;
- **a part a weld drives does not float by itself**: it is placed by its weld.
  Weld the mast and the cabin to the hull, and the hull floats the boat.

Characters are not floated; swimming is a later addition.

## Drag, currents and rivers

What is in a water is dragged towards the water's own motion, harder the more
of it is under:

- `Viscosity` is how hard. The drag grows with speed and with how much of a
  body is under water, not with its shape, so the default of 1 settles a crate
  within a second and holds a fast hull back hard. **A sea for boats wants it
  light**; `examples/11-ocean` uses 0.4.
- `Current` is a steady drift, in metres a second.
- A river's water flows along its points at `FlowSpeed`.
- The waves move the water too, up and down and to and fro, fading with depth
  -- which is what lets a hull rise with a wave rather than be held back by it.

**Where two waters overlap**, the one whose surface is highest there holds a
point, and of two at one level the one that moves: a river running out into a
lake carries what it carries until it is in the lake.

## Scripts

- `water:GetHeightAt(position)` is the surface's height above the position's
  column now, waves and all, or nil outside the water.
- `water:GetNormalAt(position)` is the way the surface faces there.
- `part:ApplyImpulseAtPosition(impulse, position)` pushes a part at a point in
  the world -- off its centre, that also turns it. A boat's screw at the stern.
- `part:ApplyAngularImpulse(impulse)` turns it about the axis the impulse
  points along. A rudder.

`examples/11-ocean` sails a boat with those two and nothing else.

## The look

A water is drawn with the engine's water surface: deep where there is a lot of
water behind a pixel, clear where there is little, foam where it meets a hull
or the shore, and what is under it bent by the waves. It is a surface shader
like any other ([Surface shaders](manual:rendering/surface-shaders)), handed
the waves as parameters, and it runs the same sum the simulation does.

## Determinism and multiplayer

The waves are a function of `RunService.SimTime` in the engine's own maths, and
the floating is part of the physics step: the same inputs float the same boat
on every machine. A `Water`, its waves and its points replicate; what floats is
simulated where the part is, like any other body.
