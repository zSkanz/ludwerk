# CharacterBody

A capsule that walks. The player, or anything that should climb a ramp and step
over a kerb instead of tumbling.

`CharacterBody` extends `BasePart`, so it has a `CFrame`, a `Size`, a `Material`, a
`CollisionGroup` and everything else a part has. What it adds is that it is a
**controller rather than a rigid body**: it sweeps its own shape and moves at
the velocity you give it, which is why it does not tip over.

There is no `Humanoid` and no `HumanoidRootPart`. This is one instance.

## Making one

```luau
--!strict
local character = Instance.new("CharacterBody")
character.Size = vector.create(2, 5, 2)   -- diameter 2 m, full height 5 m
character.WalkSpeed = 7                   -- metres per second
character.JumpSpeed = 5.5
character.AutoStepHeight = 0.6
character.MaxSlopeAngle = 46
character.Position = vector.create(0, 6, 0)
character.Parent = workspace
```

`Size` becomes a capsule: **full height from `Size.y`, diameter from the larger
of `Size.x` and `Size.z`** — authored the way `BasePart.Size` is, so a 2 × 5 × 2
part and a character of the same size occupy the same volume.

`Position` is the capsule's **centre**, like any other part. Not its feet.

## Walking

```luau
--!strict
local RunService = game:GetService("RunService")

RunService.Heartbeat:Connect(function(dt: number)
    character:Move(moveDirection)
end)
```

`CharacterBody.Move` sets the direction to walk in **for the next simulation
tick**, in world space. Four things about it:

- **Only the horizontal part is used.** Vertical movement is gravity's and
  `Jump`'s.
- **The vector is a direction and a throttle, and never more than all of it.**
  Its length is clamped to 1: `Move(1, 0, 1)` walks at `WalkSpeed`, not at 1.41
  times it, and a vector five long is no faster than one. Shorter than 1 walks
  slower, which is exactly what a half-deflected thumbstick should do -- so
  there is nothing to normalise, and a server can hand a client's vector
  straight over without it being a speed hack.
- **`WalkSpeed` is the speed across the ground, up a slope and down it
  alike.** On ground the character can walk, the horizontal speed is
  `WalkSpeed` whatever the slope; the climb or the descent comes on top.
- **Call it every tick while moving.** A character told nothing stops.
- It is intent, not a teleport: the controller still sweeps, still collides, and
  still refuses to walk through a wall.

**Gravity is applied for you**, from `Workspace.Gravity`, integrated into the
character's own vertical velocity every tick. You do not add it to `Move`.

**`LinearVelocity` is what the character did, not what it was asked.** It is
the distance the last tick moved it, over the tick: a character walking into a
wall reads zero, and one sliding along it reads the slide. Pick the walk or the
idle animation from it.

## Jumping

```luau
if character.Grounded then
    character:Jump()
end
```

`CharacterBody.Jump` launches the character upward at `CharacterBody.JumpSpeed`
at the next simulation tick, **wherever it is**. It does not check
`CharacterBody.Grounded`, and that is deliberate: a check inside `Jump` would
make a double jump, a wall jump and a triple jump impossible to write as a jump
at all.

The line above is the old behaviour, in one line, in the game — where a jump
policy belongs. Coyote time and jump buffering become counters beside it.

Calling `Jump` every frame is flying. That is the game's bug to fix, not the
engine's to prevent.

How high a jump reaches depends on `Workspace.Gravity`. That relationship is
what a game tunes, rather than tuning a height.

## Standing on things

| Member | Type | Means |
|---|---|---|
| `CharacterBody.Grounded` | `boolean`, read-only | Standing on something walkable as of the last tick. |
| `CharacterBody.State` | `Enum.CharacterState`, read-only | `Grounded` or `Airborne` on foot; `Swimming`; `Flying`. |
| `CharacterBody.FloorMaterial` | `Material?`, read-only | What it stands on: the part's material, or the layer the terrain is drawn as under its feet. Nil in the air and on a part wearing nothing. |
| `CharacterBody.Landed` | `Signal<BasePart?>` | Fired on becoming grounded after being airborne. |

On foot `Enum.CharacterState` has two answers and not three: ground too steep
to walk on reads as `Airborne`, because the question a script asks this
property is whether it may jump.

```luau
character.Landed:Connect(function(groundPart: BasePart?)
    -- nil when what it landed on is not an instance
end)
```

`Landed` is a **transition**, not a state — airborne last tick and grounded now.
Reading the flag alone would fire it every tick a character stands still.

## The two numbers that make it a character

`CharacterBody.MaxSlopeAngle` is the steepest ground, in degrees from
horizontal, the character can stand and walk on. Anything steeper is a wall it
slides down rather than a floor it stands on. It accepts 0 up to but not
including 90.

`CharacterBody.AutoStepHeight` is the tallest ledge, in metres, the character
walks over rather than into. **This is the single number that separates a
character from a capsule**: at zero, a kerb stops it.

Changing `Size`, `MaxSlopeAngle`, `AutoStepHeight` or `CollisionGroup` rebuilds
the controller. A capsule, a slope limit and a step height are things it is
constructed with, and a character that changes size mid-stride is a rare enough
event to pay for.

## Swimming and flying

A character whose middle is under a `Water`'s surface, or inside a fluid block
of a block world, is **swimming** -- by itself, and it stops by itself when it
comes out. Swimming, it has no weight, and `Move` is read in all three
dimensions at `CharacterBody.SwimSpeed`: up is up.

```luau
--!strict
local function swimOrWalk(character: CharacterBody, forward: vector, right: vector, input: Vector2, rise: number)
    local direction = forward * input.Y + right * input.X
    if character.State == Enum.CharacterState.Swimming then
        -- The jump key held swims up; a crouch key would swim down.
        direction += vector.create(0, rise, 0)
    end
    character:Move(direction)
end
```

`Jump` in a fluid is a kick upwards that carries: what takes a swimmer up
through the surface and onto a bank.

`CharacterBody.Flying = true` is the same for the air, asked for by the game:
no weight, and `Move` in three dimensions at `FlySpeed`. A creative mode, a
ghost, a jetpack held on.

`CharacterBody.GravityScale` is how much of `Workspace.Gravity` the character
feels: `0.5` is a floaty jump, `0` hangs where it is, a negative number falls
upwards.

## Pushing a character

A character is pushed as a part is. `ApplyImpulse` changes its speed by the
impulse over its `Mass` -- a controller's is 80 -- and `LinearVelocity` can be
written, which is where it is thrown.

```luau
--!strict
local function knockBack(character: CharacterBody, from: vector, speed: number)
    local away = character.Position - from
    local flat = vector.normalize(vector.create(away.x, 0, away.z))
    character:ApplyImpulse((flat * speed + vector.create(0, speed * 0.5, 0)) * character.Mass)
end
```

What pushed it is not its walk, and it fades: quickly on the ground, as
friction would take it, slowly in the air, and as drag would in a fluid. `Move`
is added on top, so a player knocked back can still steer.

Up and down, an impulse joins what gravity already integrates: one that lifts a
standing character is a launch, and it comes down as a jump does.

## Characters block, they do not push

Two characters block each other and neither pushes the other. Walking into
somebody standing still stops you and leaves them where they were, however fast
you were going and however long you keep walking.

Shoving, knockback and crowd flow are game rules, and a game writes them with
`ApplyImpulse` on the other character.

Whether two characters collide at all is decided by `BasePart.CollisionGroup`
like any other pair.

## What is not here

No `Health`, no `Died`, no default animation set, no state machine. Those were a
game's rules living in the engine. `BasePart.Density` is inherited but does not
tune a character — the controller carries its own mass.

## Where to look next

- [Rigid bodies](manual:physics/bodies) — everything `CharacterBody` inherits
- [Actions, bindings and contexts](manual:input/actions) — where
  `moveDirection` comes from
- [`CharacterBody`](api:CharacterBody)
