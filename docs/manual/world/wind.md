# Wind

A world's wind is three properties of `Workspace`:

```luau
--!strict
workspace.GlobalWind = vector.create(6, 0, 2) -- direction and speed, metres a second
workspace.WindGusts = 0.4 -- how far the speed rises and falls, 0 to 1
workspace.WindTurbulence = 0.3 -- how far the direction wanders, 0 to 1
```

The default is no wind at all, so a world that never sets it looks as it did.

**The wind is visual.** It moves what is drawn and pushes no body:

- **Particles** drift with it when their emitter's `WindAffectsDrift` is on
  (off by default, so an effect made before the wind does not change).
- **Surface shaders** read it as `SurfaceInputs.Wind` — the wind at the vertex
  or pixel, gusts included — in both `surfaceVertex` and `surfaceFragment`
  ([Surface shaders](manual:rendering/surface-shaders)). A flag, a sail or
  grass bends with it.

```hlsl
void surfaceVertex(inout SurfaceVertex vertex, SurfaceInputs inputs)
{
    // Lean with the wind, more towards the top.
    vertex.Position.xz += inputs.Wind.xz * 0.02 * vertex.Position.y;
}
```

## Where the wind is

`workspace:GetWindAt(position)` is the wind at a point now, in metres a second.
It is the same function the renderer draws with, at `RunService.SimTime`, so a
sound or a script-moved sail placed where it says the wind is agrees with the
picture.

- **Gusts travel downwind** at the wind's own speed, so a gust is seen crossing
  a field rather than every blade bending at once.
- **Turbulence turns the direction** a little about the vertical, here and
  there, and keeps the speed.

The wind settings replicate like any other `Workspace` property, so every
player sees the same wind. They are not part of the world's determinism hash:
a world that never sets them hashes as it did before they existed.
