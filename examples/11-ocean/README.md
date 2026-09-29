# examples/11-ocean — a sea, a boat that floats on it, and nothing faked

Sail a boat over an open ocean, with cargo floating around it. The sea is a
[`Water`](../../docs/manual/world/water.md) (ADR 0118) with three `WaterWave`s
under it, and **there is no wave maths in the example**: the engine draws the
waves and floats things on them from one definition, so the hull rides the
water it is drawn on.

```
examples\11-ocean\run.bat
```

**Controls** — W/S throttle · A/D steer · arrow keys or the right stick orbit the
camera · Space drops a crate over the side.

## The boat is a body

The hull is an unanchored `Part` of density 0.4. The water holds it up at 27
points across its volume, each by the weight of the water it displaces there,
so it settles with 40% of itself below the surface, a wave under one side rolls
it, and the water rights it again. A box that wide for that draft is stable:
the centre of what it displaces moves outboard faster than its weight does.
Measured over a minute of sailing, turning both ways, it never heels past 21
degrees and mostly stays under ten.

The script only sails it, in [`src/client/init.luau`](src/client/init.luau):

- **the screw** is `ApplyImpulseAtPosition` at the stern, below the waterline,
  sized so that the water's own drag caps the speed at `BoatTopSpeed`;
- **the rudder** is `ApplyAngularImpulse` about the vertical, chasing a turn
  rate that grows with speed -- a boat dead in the water does not turn;
- **the keel** takes out most of the sideways slide, which is the difference
  between a hull and a hovercraft.

The deck, the cabin, the mast and the sail are welded to the hull. A welded
part is placed by its weld rather than simulated, so it neither floats nor
sinks on its own -- the hull carries it -- and they do not collide, so the
pieces that overlap the hull do not shove it.

## The cargo is only parts

Crates and barrels are parts with a `Density` and nothing else: 0.35 rides high
like balsa, 0.85 sits waterlogged, past 1.0 it sinks. The sea's `Current`
carries them apart; the water's `Viscosity` is what makes one stop bobbing.

## Numbers worth turning

| | |
|---|---|
| `WaveDefinitions` | the sea state -- wavelength, amplitude, direction, how sharp the crests are. Speed is not in the table: deep-water waves travel at `sqrt(g·k)`, so a long swell outruns a short chop on its own |
| `Ocean.Current` | how fast the sea carries what floats in it |
| `Ocean.Viscosity` | how hard the water drags on what is in it. The drag grows with speed and with how much of a body is under water, not with its shape, so a fast hull wants it light: at the default of 1, seven metres a second is held back by nearly the hull's own weight, low down, and the bow noses under. 0.4 here |
| `HullDensity` | 0.4 rides high and stiff; towards 0.9 it wallows; past 1 it goes down |
| `BoatTopSpeed`, `BoatTurnRate`, `KeelGrip` | how the boat handles |

## Determinism

The waves are a function of `RunService.SimTime`, evaluated in the engine's own
maths, and the floating is part of the physics step -- so the same inputs sail
the same boat on every machine.
