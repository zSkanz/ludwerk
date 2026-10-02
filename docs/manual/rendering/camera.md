# The camera

A `Camera` is an ordinary instance a script owns and moves. The engine never
takes it over, which is why there is no camera-type state machine and no hidden
controller to fight with.

## Making one and using it

```luau
--!strict
local camera = Instance.new("Camera")
camera.FieldOfView = 55
camera.NearPlane = 0.1
camera.FarPlane = 400
camera.Parent = workspace

workspace.CurrentCamera = camera
```

`Workspace.CurrentCamera` is the whole of "make this the view". It is nullable,
and **`nil` renders nothing** rather than falling back to a camera the engine
invented — a view nobody asked for is harder to debug than a black frame that
says why.

## Moving it

Write `Camera.CFrame`. That is the only mechanism, and `CFrame.lookAt` is
usually the shortest way to say what you mean. **A camera that follows
something moves every frame, in a render step, following where it is drawn:**

```luau
--!strict
local RunService = game:GetService("RunService")

RunService:BindToRenderStep("follow", Enum.RenderPriority.Camera.Value, function(dt: number)
    local at = subject:GetRenderCFrame().Position
    camera.CFrame = CFrame.lookAt(at + vector.create(0, 6, 11), at + vector.create(0, 2, 0))
end)
```

The camera looks along its `CFrame.LookVector`, which is **−Z**.

`Camera` is a `PVInstance`, so `PVInstance.PivotTo` works on it too.

## Which phase to move it in

A frame runs its **render phase** -- the input a frame reads at
`Rate = Render`, then the render steps by priority, then `RunService.PreRender`
-- and then draws. The simulation runs on its own clock, sixty ticks a second,
whatever the display does. What a camera reads and writes depends on which of
the two it runs in:

| | In a render phase | In a simulation phase (`Heartbeat` …) |
|---|---|---|
| `part:GetRenderCFrame()` | where it is **drawn** this frame | its `CFrame` |
| `camera.CFrame = …` | **presented**: drawn exactly as written, and the simulation's camera from the next tick | the simulation's camera now, drawn between ticks |
| `camera.CFrame` read | what was presented | the simulation's |

So:

- **A camera that follows a moving thing: a render step, and
  `GetRenderCFrame`.** It moves every frame with what it follows as the picture
  shows it, and turns with the mouse in the frame. Following the *simulated*
  place every frame is what shakes: that place moves once a tick, and the
  camera every frame.
- **A camera the gameplay moves once a tick** -- a cut to a new shot, a
  camera on rails -- can be written on `Heartbeat`; it is drawn between ticks.
- **Nothing in a render phase runs without a window.** A test, a gate or a
  server runs no render step and no `PreRender`: a camera that matters there
  also needs a tick's path -- which is what `rig:Update` below is.

The simulation reads the camera the player last saw -- movement relative to
the camera included -- and only at a tick's start, so what a tick does never
depends on when a frame happened.

## The three numbers

| Property | Unit | Default |
|---|---|---|
| `Camera.FieldOfView` | Degrees, **vertical** | 70 |
| `Camera.NearPlane` | Metres | 0.1 |
| `Camera.FarPlane` | Metres | 5000 |

`FieldOfView` accepts more than zero and less than 180, open at both ends.

`Camera.ViewportSize` is the size of what the world is drawn into, in pixels:
the window in a game, the Viewport panel in the editor. It is read-only, changes
when the window is resized or a phone is turned, and is what a script divides by
to turn a screen position into a fraction of the screen.

**Raise the near plane rather than lowering it.** Small values buy very little
and cost depth precision across the whole scene — a near plane of 0.01 makes
distant geometry fight with itself.

## Camera rigs

`@engine/camera` ships two, so that a third-person game does not start by writing
spherical coordinates:

```luau
--!strict
local RunService = game:GetService("RunService")
local camera = require("@engine/camera")

local rig = camera.thirdPerson({
    Subject = character,
    Distance = 11,
    Height = 6,
    Focus = 2,
    -- Read every frame, from an InputContext at Rate = Render.
    TurnAction = turnAction,
    LookAction = lookAction,
})

RunService.Heartbeat:Connect(function(dt: number)
    local forward, right = rig:Basis()
    -- ... move the character in that basis ...
    rig:Update(dt) -- moves the camera only where no frame is drawn
end)
```

`camera.thirdPerson` creates the `Camera` (or adopts the scene's), assigns
`Workspace.CurrentCamera`, places it before the first frame, and **binds its
own render step at `Enum.RenderPriority.Camera`**: every frame it reads its
actions, turns, and follows its subject's `GetRenderCFrame`. A subject that
jumps further than the rig ever eases -- a teleport, a respawn -- takes the
camera with it. `camera.orbit` is the same rig turning by itself.

- **Give the rig its actions from an `InputContext` at `Rate = Render`.** At
  the simulation's rate an action holds a tick's worth, and reading it every
  frame would turn it several times over, so the rig reads only render-rate
  ones. The pointer turns it while the pointer is locked. Without actions, call
  `rig:Turn` and `rig:Look` yourself.
- **Call `rig:Update(dt)` on `Heartbeat` anyway.** Where a frame is drawn it
  does nothing; where none is -- a test, the flagship's autopilot -- it moves
  the camera once a tick, as the picture a test records expects.
- **Never scale a pointer delta by `dt`.** `Turn` takes a stick deflection and a
  `dt` because a stick is a rate. `Look` takes a mouse delta and no `dt` because
  a mouse delta is already a displacement, and multiplying it by frame time
  makes the sensitivity depend on the frame rate.

`rig:Basis()` returns the rig's forward and right vectors, flattened, which is
what turns a two-axis input into a world-space movement direction.

### First person

`camera.firstPerson` puts the camera at the subject's eyes -- `Height` above
its middle -- looking where the pointer or the stick has turned it. It takes
the same actions and gives the same `Basis`; `rig.Pitch` is how far up it
looks, and it stops a hair short of straight up and straight down.

```luau
--!strict
local camera = require("@engine/camera")
local InputService = game:GetService("InputService")
local RunService = game:GetService("RunService")

local function play(character: CharacterBody, move: InputAction, look: InputAction)
    local rig = camera.firstPerson({ Subject = character, LookAction = look })
    InputService.PointerLocked = true

    RunService.Heartbeat:Connect(function(dt: number)
        local forward, right = rig:Basis()
        local input = move:GetState() :: Vector2
        character:Move(forward * input.Y + right * input.X)
        rig:Update(dt)
    end)
end
```

It does not hide the subject: a game that shows no body makes it transparent,
and one that shows arms keeps them. Nothing is eased, either -- an eye that
lags its own head is a second of seasickness.

## From the world to the screen, and back

Three calls turn a place in the world into a pixel and a pixel into a place,
all in **window pixels from the top-left** -- the unit of
`UIObject.AbsolutePosition`, `InputService:GetPointerPosition` and a touch's
`Position`:

| Call | Gives |
|---|---|
| `camera:WorldToViewportPoint(position)` | the pixel, how far in front of the camera the point is, and whether it is on the screen |
| `camera:ViewportPointToRay(pixel)` | an origin and a direction: what to hand `Workspace:Raycast` to find what is under the pointer |
| `camera:ViewportPointToWorld2D(pixel)` | the place on the 2D plane a tap points at, or nil when the camera does not look at the plane |

```luau
--!strict
-- A name that follows a monster.
local pixel, depth, onScreen = camera:WorldToViewportPoint(monster.Position)
label.Visible = onScreen and depth > 0
label.Position = UDim2.fromOffset(pixel.X, pixel.Y)

-- What did the player click?
local origin, direction = camera:ViewportPointToRay(InputService:GetPointerPosition())
local hit = workspace:Raycast(origin, direction * 500)
```

The pixel is meaningful past the edges of the window, so an arrow at the
border can point at something off screen. `UIService.ViewportSize` is the size
those pixels are measured in.

**A `ScreenGui` that places things from the world wants `ScreenInsets = false`.**
With it on, the tree is laid out inside the safe area, and a label placed at a
pixel the camera gave is off by the notch.

For a label that should simply stay over a part, a `BillboardGui` does this
without code -- and takes a `Part2D` as well as a part
([UI in the world](manual:ui/world-space)).

## Where to look next

- [Lighting and the sky](manual:rendering/lighting)
- [The frame, phase by phase](manual:concepts/frame) — why the phase matters
- [`Camera`](api:Camera)
