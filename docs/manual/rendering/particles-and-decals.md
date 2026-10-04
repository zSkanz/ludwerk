# Particles and decals

Fire, smoke, sparks, dust and magic are particles. A scorch mark, a footprint,
a bullet hole, a painted line and a glowing circle are decals. Both are part
of the picture rather than the world: nothing in the simulation collides with
them, and a script sets them up and lets them run.

## Particles

A `ParticleEmitter` parented to a part or an attachment throws particles out
along its parent's up direction. From a part, they are born anywhere inside
it. Everything it does follows from its properties:

```luau
--!strict
local fire = Instance.new("ParticleEmitter")
fire.Rate = 160                         -- particles a second
fire.Lifetime = 0.8                     -- seconds each one lives
fire.Speed = 1.4                        -- metres a second, out along the parent's up
fire.SpreadAngle = 8                    -- degrees either side of it
fire.Acceleration = vector.create(0, 4, 0)
fire.Color = Color3.new(1, 0.6, 0.15)   -- at birth...
fire.ColorEnd = Color3.new(0.8, 0.1, 0.02) -- ...and at death
fire.Size = 0.7
fire.SizeEnd = 0.1
fire.LightEmission = 1                  -- added light rather than paint
fire.Brightness = 3
fire.Parent = logs
```

| Property | What it does |
|---|---|
| `Rate`, `Lifetime` | How many are born a second, and for how long each lives. |
| `Speed`, `SpreadAngle` | How fast they leave, and how widely around the parent's up. |
| `Acceleration`, `Drag` | What pulls on them -- gravity, wind, buoyant heat -- and how fast they slow. |
| `Color` → `ColorEnd`, `Size` → `SizeEnd`, `Transparency` → `TransparencyEnd` | How each one changes from birth to death. |
| `LightEmission` | 0 blends like smoke, 1 adds light like fire, and between is both. |
| `Brightness` | Multiplies the colour; above 1 glows and blooms. |
| `Shape` | `Soft`, a round falloff, or `Disc`, a crisp dot for sparks and water. Drawn when there is no `Texture`. |
| `Enabled` | Whether it keeps emitting. |

`Emit(count)` throws a burst at once, whatever `Rate` says: an explosion, a
splash, a puff of dust where a foot landed.

**Particles fade where they meet a surface**, instead of showing a hard line
along it, so smoke rolling over the ground looks like smoke. **They are cheap
because they are not the world.** They are simulated on the frame, not the
tick, a script cannot find one, and a fire of hundreds costs one draw.

### A picture, its frames, and a turn

`Texture` is the picture each particle is drawn as, facing the camera. Its
colour is multiplied by the particle's, and its alpha is the particle's
outline.

A picture that is a grid of frames is a flipbook:

```luau
local smoke = Instance.new("ParticleEmitter")
smoke.Texture = "asset://textures/smoke_sheet.png"   -- 8 frames across, 8 down
smoke.FlipbookColumns = 8
smoke.FlipbookRows = 8
smoke.FlipbookMode = Enum.ParticleFlipbookMode.OverLife
smoke.RotationSpread = 180       -- born any way up
smoke.RotationSpeedSpread = 30   -- each turning its own way, slowly
```

| Property | What it does |
|---|---|
| `FlipbookColumns`, `FlipbookRows` | How many frames the picture holds, 1 to 16 each way, read left to right and then top to bottom. |
| `FlipbookMode` | `Loop` plays them at `FlipbookFramerate`. `OverLife` plays every frame once across the particle's life. `Random` gives each particle one frame for good: debris, leaves. |
| `Rotation`, `RotationSpread` | Degrees a particle is turned at birth, and how far either way of that at random. |
| `RotationSpeed`, `RotationSpeedSpread` | Degrees a second it turns, and how far either way of that at random. |

### Curves over a life

`Color` to `ColorEnd` is a straight line. A puff that swells and then thins,
or a spark that flashes white and cools to red, is a curve:

```luau
smoke.SizeOverLife = NumberSequence.new({
    NumberSequenceKeypoint.new(0, 0.2),
    NumberSequenceKeypoint.new(0.3, 1),
    NumberSequenceKeypoint.new(1, 1.6),
})
smoke.TransparencyOverLife = NumberSequence.new({
    NumberSequenceKeypoint.new(0, 1),
    NumberSequenceKeypoint.new(0.15, 0),
    NumberSequenceKeypoint.new(1, 1),
})
```

`ColorOverLife`, `SizeOverLife` and `TransparencyOverLife` **multiply what the
start and end values make**, so the defaults -- white, one and zero -- change
nothing, and an emitter can use either or both.

### Particles that land

By default a particle passes through everything. `Collision` says what it
meets, and `CollisionResponse` what it does there:

```luau
local sparks = Instance.new("ParticleEmitter")
sparks.Acceleration = vector.create(0, -9.81, 0)
sparks.Collision = Enum.ParticleCollision.Both
sparks.CollisionResponse = Enum.ParticleCollisionResponse.Bounce
sparks.Bounce = 0.4      -- how much of the speed into a surface comes back
sparks.Friction = 0.3    -- how much of the speed along it each touch takes
```

| `Collision` | What a particle meets |
|---|---|
| `None` | Nothing. |
| `Terrain` | The terrain's ground, in view or not. |
| `SceneDepth` | What is drawn: parts, meshes, the ground. |
| `Both` | Both. |

| `CollisionResponse` | What it does there |
|---|---|
| `Bounce` | Comes back off the surface, and comes to rest when it barely rises. |
| `Stick` | Stays where it landed for the rest of its life. |
| `Kill` | Dies there: rain on a roof. |

`CollisionRadius` is how far from its middle a particle touches. Left at 0 it
is half the particle's size.

### A hundred thousand: `Simulation`

An emitter simulates its particles on the CPU unless it says otherwise. That
carries a few thousand an emitter. A storm, a waterfall, or embers over a
battlefield is `Enum.ParticleSimulation.Gpu`:

```luau
local rain = Instance.new("ParticleEmitter")
rain.Simulation = Enum.ParticleSimulation.Gpu
rain.Rate = 20000
rain.Lifetime = 5
rain.Collision = Enum.ParticleCollision.Both
rain.CollisionResponse = Enum.ParticleCollisionResponse.Kill
```

A hundred thousand particles bouncing on the ground cost a little over a
millisecond a frame on a desktop GPU, and nothing on the CPU. Every property
on this page works the same on both. What differs:

| | `Cpu` | `Gpu` |
|---|---|---|
| How many | A few thousand an emitter | Up to 262 144 alive an emitter (`Rate` times `Lifetime`) |
| Drawing order | Back to front, with every other CPU particle | None among themselves, and after the CPU's |
| `SceneDepth` | A ray against the parts in the way, at most 2048 a frame across every emitter | The picture's depth: what is on the screen and not hidden |
| `Terrain` | The ground, anywhere | The ground within about 56 m of the camera |
| Needs | Nothing | A device with compute shaders |

**Choose by what the particles are:**

- **Light** -- sparks, embers, rain, magic, anything with `LightEmission` at
  1: `Gpu`. Light added to light looks the same in any order.
- **Paint** -- thick smoke, dust, anything with `LightEmission` at 0 that
  overlaps itself: `Cpu`, which sorts it.
- **Must land off the screen** -- debris that has to be lying there when the
  camera turns round: `Terrain`, or `Cpu` with `SceneDepth`. On the GPU,
  `SceneDepth` knows only the surfaces in the last frame: a spark that falls
  behind a wall falls through the floor there.

On a machine with no compute shaders a `Gpu` emitter runs on the CPU, with
the CPU's limit on how many, and the log says so once. The game looks thinner
there rather than failing, so a storm should not be what a level depends on.

### Weather: rain and snow over the player

Rain is not a jet from a point, and it does not need a shape of its own: **an
emitter under a part is born anywhere inside that part**, so a wide, flat,
invisible part over the player's head is a cloud. Keep it over the camera and
the rain is wherever the player looks:

```luau
--!strict
local RunService = game:GetService("RunService")

-- A slab of sky: sixty metres across, and the rain is born anywhere inside it.
local sky = Instance.new("Part")
sky.Size = vector.create(60, 2, 60)
sky.Anchored = true
sky.CanCollide = false
sky.CanQuery = false
sky.CanTouch = false
sky.CastShadow = false
sky:SetMaterialParameter("Transparency", 1)
sky.Parent = workspace

local rain = Instance.new("ParticleEmitter")
rain.Simulation = Enum.ParticleSimulation.Gpu
rain.Rate = 6000
rain.Lifetime = 1.6
rain.Speed = 22                               -- along the slab's up
rain.SpreadAngle = 2
rain.Acceleration = vector.create(3, 0, 0)    -- the wind
rain.Shape = Enum.ParticleShape.Disc
rain.Size = 0.05
rain.Color = Color3.new(0.7, 0.8, 1)
rain.Transparency = 0.5
rain.Collision = Enum.ParticleCollision.Both
rain.CollisionResponse = Enum.ParticleCollisionResponse.Kill
rain.Parent = sky

-- Over the camera every frame, and turned upside down: an emitter throws
-- along its parent's up, and this parent's up is the way rain falls.
RunService:BindToRenderStep("weather", Enum.RenderPriority.Camera.Value + 1, function()
    local camera = workspace.CurrentCamera
    if camera then
        sky.CFrame = CFrame.new(camera.CFrame.Position + vector.create(0, 25, 0))
            * CFrame.fromEuler(math.pi, 0, 0)
    end
end)
```

- **The part's `Size` is the storm's**: as wide as the player can see rain
  falling, a metre or two thick so drops do not start in a sheet. Its `CFrame`
  turns the whole fall: tilt the slab and the rain slants.
- **`Rate` times `Lifetime` is how many are in the air.** Rain that dies on
  what it hits (`Kill`) spends none under a roof, and `Both` stops it at the
  ground whether the ground is in view or not.
- **Snow is the same slab with a slower emitter**: a `Speed` of 2 or 3, a
  `Lifetime` long enough to reach the ground, some `Drag`, a wider
  `SpreadAngle`, and `Stick` to lie where it lands for the rest of its life.
- **A drop is a dot unless it has a picture**: give the emitter a `Texture`
  of a streak for rain seen from the side.
- **In a match, weather is each player's own.** Make the slab and its emitter
  in a client script: particles are drawn, not simulated by the server, and a
  slab the server moved would follow one player's camera for everybody.

## Decals

A `Decal` projects an image onto whatever lies inside its box: curved,
sculpted, animated, anything. It is not tied to a face of a part. By default
it darkens what it lands on and keeps that surface's own light and shadow, so
a scorch mark on a lit wall is a lit scorch mark.

**A raycast hit is a decal's placement:**

```luau
local result = workspace:Raycast(origin, direction * 100)
if result then
    local mark = Instance.new("Decal")
    mark.CFrame = CFrame.lookAt(result.Position, result.Position + result.Normal)
    mark.Size = vector.create(0.3, 0.3, 0.2)   -- width, height, and how deep it reaches
    mark.Texture = "asset://textures/bullet_hole.png"
    mark.Parent = workspace
end
```

| Property | What it does |
|---|---|
| `CFrame` | Where the box is, relative to its parent part when it has one, so a mark on a moving crate moves with it. |
| `Size` | Width, height and depth of the box, in metres. Only what is inside it is painted. |
| `Texture` | The image. Its alpha is how much of it lands. |
| `Color` | Multiplies the image. |
| `Transparency` | 0 paints the image as it is; 1 paints nothing. |
| `BlendMode` | How the image meets the surface. See below. |
| `Emissive` | How brightly the image glows of its own: 0 none, above 1 enough to bloom. Read by `Alpha` and `Additive`. |

`BlendMode` is what kind of mark it is:

| `Enum.DecalBlendMode` | What it does | For |
|---|---|---|
| `Multiply` (the default) | Multiplies what is there. It darkens and tints, and cannot brighten: a white pixel is no change. | A stain, a scorch mark, a shadow blob. |
| `Alpha` | Lays the image over the surface by its alpha, lit as the surface is. | A painted line, a sign, a warning ring that reads on dark ground. |
| `Additive` | Adds the image's light to what is there, unlit. | A glow, an aura, a magic circle. |

```luau
local ring = Instance.new("Decal")
ring.Texture = "asset://textures/warning_ring.png"
ring.BlendMode = Enum.DecalBlendMode.Additive
ring.Color = Color3.new(1, 0.2, 0.1)
ring.Emissive = 2
ring.Size = vector.create(6, 6, 1)
ring.CFrame = CFrame.lookAt(where, where + vector.create(0, 1, 0))
ring.Parent = workspace
```

Both decals and particle emitters replicate: on every machine of a match, the
same emitters run and the same marks are painted.

## Where to look next

- [World-space UI](manual:ui/world-space)
- [Lighting and the sky](manual:rendering/lighting)
- `examples/16-particles`: a campfire and its smoke, a fountain, a shower of
  sparks, a burst every two seconds, and a scorch mark
