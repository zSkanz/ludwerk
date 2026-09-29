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

## Shapes

| `Shape` | Where it is |
|---|---|
| `Ocean` | everywhere, drawn out to the horizon round the camera |
| `Box` | a rectangle `Size.X` by `Size.Z` centred on `Position`, `Size.Y` deep under its level -- a lake or a pool |
| `Spline` | a ribbon `Size.X` wide and `Size.Y` deep along the `WaterPoint` children, in their order -- a river |

The surface is at `SurfaceLevel`. A lake or a river is drawn only where it is:
carve the ground out under it, and the ground hides whatever the rectangle or
the ribbon covers that is not the basin (`examples/31-lake-and-river`).

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
